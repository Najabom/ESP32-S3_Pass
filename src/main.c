/* ============================================================================
 * main.c - 系统入口（角色名片系统 MJPEG 播放器 + ESP-NOW 名片交换）
 *
 * 启动流程（app_main 严格按 0~7 顺序执行）：
 *   0. 存储初始化    NVS（档案/冷却） + cardstore 分区（self.bin 自资料）
 *   1. 屏幕硬件      ST7789 240x320 SPI2 + LVGL 核心/显示驱动
 *   2. 触控          XPT2046 GPIO 模拟 SPI（注册为 LVGL 指针输入设备）
 *   2.5 SD 卡        SPI3 + FATFS 挂载 /sdcard + 热插拔监控（无卡跳过）
 *   3. LVGL 调度任务  独立 FreeRTOS 任务（core1），渲染与 UI 回调都经它
 *   4. MJPEG 显示对象 全屏底层 lv_img，由 mjpeg_player_task 持续换帧
 *   5. UI 界面       左侧旋转立牌 + 底部角色面板 + 名片横幅（隐藏态）；
 *                    角色名等 SD 扫描到角色后由播放器 set_role 下发
 *   6. 播放任务      SD 卡角色文件夹 intro(可选)+loop MJPEG 软解播放
 *   6.5 电源监测     USB_DET + VBAT_SENSE ADC（充电指示/低电警告/危急休眠）
 *   7. 近场通讯      ESP-NOW 名片交换 + 收卡记录写 SD + 串口控制台
 *
 * 可调整内容：
 *   - xTaskCreatePinnedToCore 的栈大小/优先级/核绑定：栈溢出会触发
 *     stack overflow 异常（日志里能看到），优先级越高抢占其他任务越频繁。
 *   - mjpeg_player_task 优先级(4)：调低会让视频在 UI 繁忙时卡顿，调高会
 *     抢 LVGL(5) 资源导致界面刷新掉帧，建议保持低于 LVGL。
 * ==========================================================================*/

#include <stdio.h>
#include "freertos/FreeRTOS.h"
#include "freertos/task.h"
#include "driver/gpio.h"
#include "esp_log.h"
#include "esp_timer.h"
#include "esp_lcd_panel_io.h"
#include "esp_lcd_panel_vendor.h"
#include "esp_lcd_panel_ops.h"
#include "lvgl.h"
#include "bsp_lvgl.h"
#include "bsp_touch.h"
#include "bsp_sdcard.h"
#include "pins_pass.h"
#include "bsp_power.h"
#include "ui_boot_anim.h"
#include "ui_core_screen.h"
#include "ui_menu.h"
#include "mjpeg_player.h"
#include "nvs_flash.h"
#include "card_store.h"
#include "nfc_profile.h"
#include "cfg_backup.h"
#include "espnow_nfc.h"
#include "ble_gatt.h"
#include "nfc_proto.h"
#include "time_sync.h"

/* ---------- 屏幕参数（引脚统一在 pins_pass.h，PCB v6.2 实机） ----------
 * SPI2_HOST 驱动 ST7789；LCD 6 针 / SD 4 针 / 电源监测引脚见
 * src/bsp/pins_pass.h（唯一来源，改版只动那一个文件）。 */
#define LCD_H_RES 240
#define LCD_V_RES 320

#define LCD_HOST SPI2_HOST

static const char *TAG = "MAIN";

static lv_obj_t *mjpeg_img = NULL;

/* ---------- 收到名片回调 ----------
 * 功能：ESP-NOW 成功交换一张名片后由 espnow_nfc 任务调用（非 LVGL 线程）。
 *       把数据转交给 UI 的底部横幅显示（内部会加 LVGL 锁）。
 *       横幅名称优先级：对端用户名(uname) > 对端当前显示角色名(name)；
 *       头像优先级：用户头像(avatar_len) > 角色头图(portrait_len) > 占位。
 * 可调整内容：sticker_lens 拷贝是绕开 packed 结构体取地址告警；
 *       若横幅样式/字段要改，去 src/ui/ui_core_screen.c 的
 *       ui_core_screen_show_banner，而不是这里。 */
static void on_card_received(const uint8_t mac[6], const card_blob_hdr_t *hdr,
                             const uint8_t *payload, size_t payload_len)
{
    (void)mac;
    uint16_t sticker_lens[CARD_STICKER_MAX] = {0};
    for (int i = 0; i < hdr->sticker_cnt && i < CARD_STICKER_MAX; i++) {
        sticker_lens[i] = hdr->sticker_len[i];
    }
    ui_core_screen_show_banner(hdr->name, hdr->uname, hdr->uid, payload, payload_len,
                               hdr->avatar_len, hdr->portrait_len,
                               hdr->sticker_cnt, sticker_lens);
}

/* ---------- 电源状态显示胶水 ----------
 * bsp_power 监测任务回调用：把状态文本/级别转给右上角 chip
 * （ui_core_screen_set_power 内部自行加 LVGL 锁）。 */
static void power_display_cb(const char *text, int level)
{
    ui_core_screen_set_power(text, level);
}

// ==================== 主入口程序 ====================
void app_main(void)
{
    setvbuf(stdout, NULL, _IONBF, 0);   // 日志/printf 无缓冲直出，保证串口实时性
    ui_boot_anim_attach_log();          // 首行挂日志钩子：全量日志进环形缓冲，供开机终端显示

    ESP_LOGI(TAG, "=== PASS System V1.0 ===");
    ESP_LOGI(TAG, "boot reason: power-on | cpu 240MHz x2 | SRAM 275KB / PSRAM 8MB");
    ESP_LOGI(TAG, "--- boot sequence ---");

    // ---- 0. 存储初始化：NVS（档案/冷却）+ cardstore 分区（名片与自资料）----
    // 功能：NVS 保存自己 uid/motto 与按对冷却状态；cardstore(SPIFFS) 保存
    //       self.bin（自己头像表情包）与收到的 card_<mac>.bin。
    // 可调整内容：NVS 损坏（擦除全片或换分区表）时会自动整区擦除重来，
    //       代价是 uid/motto/冷却记录全部丢失（属正常安全行为，勿删）。
    ESP_LOGI(TAG, "[0/8] probing storage: NVS + cardstore partition...");
    esp_err_t err = nvs_flash_init();
    if (err == ESP_ERR_NVS_NO_FREE_PAGES || err == ESP_ERR_NVS_NEW_VERSION_FOUND) {
        nvs_flash_erase();
        nvs_flash_init();
    }
    card_store_init();

    // ---- 0.5. 提前挂载 SD（SPI3 + FATFS → /sdcard）----
    // 目的：先于 nfc_profile_init 回灌 /sdcard/config/nvs_backup.txt 快照
    //       （全量烧录/整片擦除后 NVS 为空，profile 会生成默认 uid 并写入，
    //       晚于该点的恢复将无法覆盖默认值）。快照含 uid/motto/uname、
    //       UI 选项掩码——刷固件不再丢配置。SD 可选：无卡时
    //       cfg_backup 任务在热插卡后兜底恢复一次。
    ESP_LOGI(TAG, "[0/8] mounting SD card (SPI3, FATFS /sdcard)...");
    if (bsp_sdcard_init() == ESP_OK) {
        bsp_sdcard_start_monitor();
        ESP_LOGI(TAG, "[0/8] SD ok: FATFS mounted");
    } else {
        ESP_LOGW(TAG, "[0/8] SD unavailable (probe timeout), hot-plug will retry");
    }
    cfg_backup_restore();      // 有快照则回灌 NVS（键缺失才写，幂等）
    cfg_backup_start();        // 周期快照任务（内容未变不重写）

    nfc_profile_init();
    time_sync_init();
    ESP_LOGI(TAG, "[0/8] storage ok: NVS / SD / cardstore / self profile / time base ready");

    // ---- 1. 初始化 LVGL（屏幕硬件 + 核心/显示驱动）----
    ESP_LOGI(TAG, "[1/8] initializing screen (ST7789 240x320 @ SPI2 80MHz)...");
    bsp_lvgl_init();
    ESP_LOGI(TAG, "[1/8] screen ok: panel ready, draw buf half-SRAM x2");
    
    // ---- 2. 初始化触控（XPT2046 GPIO 模拟 SPI）----
    // 功能：XPT2046 注册为 LVGL 指针输入，为后续弹窗重构提供点击能力。
    //       原硬件 SPI3 已让给 SD 卡（触摸为低速轮询，模拟后无感知差异）。
    ESP_LOGI(TAG, "[2/8] initializing touch (XPT2046 gpio bitbang)...");
    bsp_touch_init();
    ESP_LOGI(TAG, "[2/8] touch ok: 240x320 calibration cached");


    // ---- 3. 启动 LVGL 独立调度 Task (FreeRTOS) ----
    // 功能：LVGL 所有渲染/回调都跑在该任务（core1 优先级5），其他线程
    //       操作 UI 必须先 bsp_lvgl_lock() 防竞争。
    ESP_LOGI(TAG, "[4/8] starting LVGL scheduler (core1 prio5)...");
    bsp_lvgl_start_task();
    ESP_LOGI(TAG, "[4/8] LVGL running");

    // ---- 4. 创建 MJPEG 帧显示对象 ----
    // 功能：全屏底层 lv_img（move_background 垫底），mjpeg_player_task
    //       每帧更新它的图像源，实现视频底图。
    // 可调整内容：pad_all 必须为 0——LVGL 默认图片样式带 5px 内边距，
    //       会把 240x320 的视频画面右/下各裁掉 5px（画面全出血时可见）；
    //       align 严格居中，视频尺寸恰为屏幕尺寸，偏移只会造成越界。
    // 注意：此时 LVGL 任务已在运行，操作控件必须持 LVGL 锁，
    //       否则与渲染线程并发会破坏控件链表（随机崩溃/花屏）。
    ESP_LOGI(TAG, "[5/8] creating video canvas...");
    bsp_lvgl_lock();
    mjpeg_img = lv_img_create(lv_scr_act());
    lv_obj_set_style_pad_all(mjpeg_img, 0, 0);
    lv_obj_align(mjpeg_img, LV_ALIGN_CENTER, 0, 0);
    lv_obj_move_background(mjpeg_img);
    bsp_lvgl_unlock();
    ESP_LOGI(TAG, "[5/8] video canvas ready (full-screen bg img)");

    // ---- 5. 初始化主界面 UI（角色名片系统；函数内部自行 lock/unlock）----
    // 功能：左立牌/底部角色面板/名片横幅（详见 ui_core_screen.c）。
    //       角色名先占位（"--"）：SD 尚未扫描；播放器扫到角色后调
    //       ui_core_screen_set_role() 下发真实名字（可能是中文）。
    ESP_LOGI(TAG, "[6/8] building UI (leftcol / footer / banner)...");
    ui_core_screen_init(NULL);
    ESP_LOGI(TAG, "[6/8] UI ok");

    // ---- 5.5. 开机终端画面（LVGL 渲染，日志滚动 + 读卡进度，intro 就绪后隐藏）----
    ESP_LOGI(TAG, "[7/8] boot terminal up (logs on screen, 5s window)...");
    ui_boot_anim_init();
    ESP_LOGI(TAG, "[7/8] boot terminal running");

    // ---- 5.6. 触控菜单（底部抽屉 + 全屏子页；手势轮询 LVGL 定时器）----
    ESP_LOGI(TAG, "[7.5/8] touch menu (drawer + pages)...");
    ui_menu_init();
    ESP_LOGI(TAG, "[7.5/8] touch menu ready (swipe up to open)");

    // ---- 6.5. 电源监测（PCB v6.2：USB_DET IO1 + VBAT_SENSE IO2/ADC1_CH1）----
    // 功能：1s 周期采样（9 次中值）；USB 显示充电状态、低电警告、
    //       危急（≤3.2V）停写卡并连续确认后 deep sleep；状态→右上角 chip。
    ESP_LOGI(TAG, "[7.6/8] power monitor (USB_DET/VBAT)...");
    bsp_power_init();
    bsp_power_set_display_cb(power_display_cb);
    bsp_power_start_monitor();
    ESP_LOGI(TAG, "[7.6/8] power monitor ready");

    // ---- 7.5. 近场名片交换（ESP-NOW）——必须先于 BLE ----
    // 实测：BLE 广播启动后再 esp_wifi_start 会挂死（BLE RF 活动与 WiFi
    // phy 初始化冲突，libphy 静默卡死）。WiFi 先行、BLE 后 init 可共存。
    espnow_nfc_set_recv_cb(on_card_received);
    espnow_nfc_init();

    nfc_profile_console_register();

    // ---- 8. BLE GATT（WiFi 已就绪后初始化，HW 共存）----
    // 注意：SD 大块读取与 BT PHY 初始化并发会让 libphy 静默卡死
    //       （core0 全停，见 sdkconfig 校准注释）。
    ESP_LOGI(TAG, "[8/8] starting BLE GATT (remote config + upload)...");
    ble_gatt_init();
    ESP_LOGI(TAG, "[8/8] BLE GATT ready");

    // ---- 9. MJPEG 播放任务（RF 全部就绪后创建，任务内部再错峰 2s）----
    ESP_LOGI(TAG, "[8/8] starting MJPEG player + NFC...");
    BaseType_t task_ok = xTaskCreatePinnedToCore(
        mjpeg_player_task,
        "mjpeg_player",
        8192,
        (void *)mjpeg_img,
        4,          // 优先级略低于 LVGL Task
        NULL,
        0           // 绑定在 Core 0 上解算 JPEG
    );
    if (task_ok != pdPASS) {
        ESP_LOGE("MAIN", "MJPEG task creation failed!");
    } else {
        ESP_LOGI("MAIN", "MJPEG task created!");
    }

    ESP_LOGI(TAG, "=== boot sequence complete [8/8], system ready ===");
}