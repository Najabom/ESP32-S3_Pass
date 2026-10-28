/* ============================================================================
 * bsp_lvgl.c - 屏幕驱动 + LVGL 移植层（ST7789 240x320 @ SPI2 80MHz）
 *
 * 职责：
 *   1. 硬件：背光 GPIO、SPI2 总线、ST7789 控制器（含伽马/VCOM 调校）
 *   2. LVGL：绘制缓冲分配（三级策略）、显示驱动注册、flush 回调
 *   3. 线程：LVGL 专用调度任务（core1 优先级5）+ 全局互斥锁
 *
 * 可调整内容：
 *   - io_config.pclk_hz (80MHz)：更高可能花屏（PCB 走线差），更低变卡。
 *   - gamma/VCOM 数值：整体色彩倾向。想更暖可微调 0xE1 组里 0x15/0x1D
 *     （正伽马暗部），想更高对比调 0xC5 的 0x1C。
 *   - lvgl_task 的 10ms 周期：越小 UI 动画越顺滑但 CPU 占用越高；
 *     5ms 是流畅下限，20ms 以上肉眼可见卡顿。
 *   - 绘制缓冲三级策略（全屏 SRAM → 半屏 SRAM → 全屏 PSRAM）：
 *     改动顺序/阈值会影响视频换帧撕裂与渲染耗时，谨慎。
 * ==========================================================================*/

// bsp/bsp_lvgl.c
#include "bsp_lvgl.h"
#include "pins_pass.h"
#include "freertos/FreeRTOS.h"
#include "freertos/task.h"
#include "freertos/semphr.h"
#include "driver/gpio.h"
#include "driver/ledc.h"
#include "esp_log.h"
#include "esp_timer.h"
#include "esp_lcd_panel_io.h"
#include "esp_lcd_panel_vendor.h"
#include "esp_lcd_panel_ops.h"

#define LCD_H_RES 240
#define LCD_V_RES 320

#define LCD_HOST SPI2_HOST

/* ---------- 背光 LEDC PWM（PCB v6.2 实机：BLK=IO9） ----------
 * 10kHz 无频闪可闻噪声；10bit 占空（0~1023）。防白屏策略保留：
 * 上电占空 0（熄灭），LVGL 首帧 flush 后由 lvgl_disp_flush_cb 升到 100%。 */
#define LCD_BL_LEDC_MODE      LEDC_LOW_SPEED_MODE
#define LCD_BL_LEDC_TIMER     LEDC_TIMER_0
#define LCD_BL_LEDC_CHANNEL   LEDC_CHANNEL_0
#define LCD_BL_LEDC_RES       LEDC_TIMER_10_BIT
#define LCD_BL_LEDC_FREQ_HZ   10000

/* 设置背光亮度 0~100（>100 截到 100；0=全灭）。
 * 任何任务可调（LEDC 寄存器写，无 LVGL 依赖）。 */
void bsp_lcd_backlight(uint8_t percent)
{
    if (percent > 100) percent = 100;
    uint32_t duty = (uint32_t)percent * 1023 / 100;
    ledc_set_duty(LCD_BL_LEDC_MODE, LCD_BL_LEDC_CHANNEL, duty);
    ledc_update_duty(LCD_BL_LEDC_MODE, LCD_BL_LEDC_CHANNEL);
}

static esp_lcd_panel_handle_t panel_handle = NULL;
static lv_disp_drv_t disp_drv;
static SemaphoreHandle_t lvgl_mutex = NULL;

/* ---------- LVGL 全局互斥锁 ----------
 * 功能：LVGL 非线程安全。除 lvgl_task 自身外，任何线程（MJPEG 任务、
 *       ESP-NOW 任务回调等）操作 UI 对象前必须 bsp_lvgl_lock()，
 *       完成后 unlock()。不带锁操作 UI 会随机崩溃/花屏。
 * 可调整内容：无（超时 portMAX_DELAY 是故意阻塞等待，避免丢帧）。 */
void bsp_lvgl_lock(void) {
    if (lvgl_mutex) xSemaphoreTake(lvgl_mutex, portMAX_DELAY);
}

void bsp_lvgl_unlock(void) {
    if (lvgl_mutex) xSemaphoreGive(lvgl_mutex);
}

/* ---------- 刷新完成回调 ----------
 * 功能：esp_lcd 完成一块位图 DMA 传输后回调，转告 LVGL flush 已完成
 *       （双缓冲模式下 LVGL 据此回收缓冲，缺它会导致刷屏停滞）。 */
static bool notify_lvgl_flush_ready(esp_lcd_panel_io_handle_t panel_io, esp_lcd_panel_io_event_data_t *edata, void *user_ctx) {
    lv_disp_drv_t *disp_driver = (lv_disp_drv_t *)user_ctx;
    /* LVGL 未初始化（lv_disp_get_default() 为 NULL）时不能调 flush_ready */
    if (lv_disp_get_default() == NULL) return false;
    lv_disp_flush_ready(disp_driver);
    return false;
}

/* ---------- LVGL flush 回调 ----------
 * 功能：LVGL 每次渲染出脏区域后调用，把像素块交给 ST7789 驱动
 *       （esp_lcd_panel_draw_bitmap 内部排队 DMA 传输）。
 * 开机防白屏：背光在 init 里保持熄灭（ST7789 上电默认内容常是白
 * 色垃圾画面），首帧渲染完成后的第一次 flush 才点亮背光——此时
 * 屏内已是 LVGL 画好的深色背景，不会闪过白屏。 */
static bool s_first_flush_done = false;

static void lvgl_disp_flush_cb(lv_disp_drv_t *drv, const lv_area_t *area, lv_color_t *color_map) {
    if (!s_first_flush_done) {
        s_first_flush_done = true;
        bsp_lcd_backlight(100);                /* 首帧画完才点亮，防白屏 */
    }
    esp_lcd_panel_handle_t panel = (esp_lcd_panel_handle_t) drv->user_data;
    esp_lcd_panel_draw_bitmap(panel, area->x1, area->y1, area->x2 + 1, area->y2 + 1, color_map);
}

/* ---------- LCD 硬件初始化（LVGL 显示驱动的基础） ----------
 * 功能：背光+SPI2+ST7789 序列与伽马。背光保持熄灭，由 LVGL 首次
 *       flush 点亮（防 ST7789 上电白屏）。 */
void bsp_lcd_early_init(void)
{
    /* 背光 LEDC 初始化：占空 0（熄灭）——首帧 flush 才由 bsp_lcd_backlight(100)
     * 点亮，防 ST7789 上电白屏。PWM 取代原 GPIO 开关，支持 0~100% 调光。 */
    ledc_timer_config_t bl_timer = {
        .speed_mode      = LCD_BL_LEDC_MODE,
        .duty_resolution = LCD_BL_LEDC_RES,
        .timer_num       = LCD_BL_LEDC_TIMER,
        .freq_hz         = LCD_BL_LEDC_FREQ_HZ,
        .clk_cfg         = LEDC_AUTO_CLK,
    };
    ESP_ERROR_CHECK(ledc_timer_config(&bl_timer));
    ledc_channel_config_t bl_ch = {
        .gpio_num   = PIN_NUM_LCD_BK_LIGHT,
        .speed_mode = LCD_BL_LEDC_MODE,
        .channel    = LCD_BL_LEDC_CHANNEL,
        .timer_sel  = LCD_BL_LEDC_TIMER,
        .duty       = 0,
        .hpoint     = 0,
        .intr_type  = LEDC_INTR_DISABLE,
    };
    ESP_ERROR_CHECK(ledc_channel_config(&bl_ch));

    spi_bus_config_t buscfg = {
        .sclk_io_num = PIN_NUM_LCD_PIXEL_CLOCK,
        .mosi_io_num = PIN_NUM_LCD_DATA0,
        .miso_io_num = -1,
        .quadwp_io_num = -1,
        .quadhd_io_num = -1,
        .max_transfer_sz = LCD_H_RES * LCD_V_RES * sizeof(uint16_t),
    };
    ESP_ERROR_CHECK(spi_bus_initialize(LCD_HOST, &buscfg, SPI_DMA_CH_AUTO));

    esp_lcd_panel_io_handle_t io_handle = NULL;
    esp_lcd_panel_io_spi_config_t io_config = {
        .dc_gpio_num = PIN_NUM_LCD_DC,
        .cs_gpio_num = PIN_NUM_LCD_CS,
        .pclk_hz = 80 * 1000 * 1000,
        .lcd_cmd_bits = 8,
        .lcd_param_bits = 8,
        .spi_mode = 0,
        .trans_queue_depth = 10,
        .on_color_trans_done = notify_lvgl_flush_ready,
        .user_ctx = &disp_drv,
    };
    ESP_ERROR_CHECK(esp_lcd_new_panel_io_spi((esp_lcd_spi_bus_handle_t)LCD_HOST, &io_config, &io_handle));

    esp_lcd_panel_dev_config_t panel_config = {
        .reset_gpio_num = PIN_NUM_LCD_RST,
        .rgb_ele_order = LCD_RGB_ELEMENT_ORDER_RGB,
        .bits_per_pixel = 16,
    };
    ESP_ERROR_CHECK(esp_lcd_new_panel_st7789(io_handle, &panel_config, &panel_handle));
    ESP_ERROR_CHECK(esp_lcd_panel_reset(panel_handle));
    ESP_ERROR_CHECK(esp_lcd_panel_init(panel_handle));
    // 自定义 gamma/VCOM：修正 ST7789 默认发灰发暗（0xE0/0xE1 正负伽马，0xC5 VCOM 对比度）
    ESP_ERROR_CHECK(esp_lcd_panel_io_tx_param(io_handle, 0xE0,
        (uint8_t[]){0xD0, 0x08, 0x11, 0x08, 0x0C, 0x06, 0x2F, 0x44, 0x40, 0x07, 0x0E, 0x09, 0x15, 0x1D}, 14));
    ESP_ERROR_CHECK(esp_lcd_panel_io_tx_param(io_handle, 0xE1,
        (uint8_t[]){0xD0, 0x08, 0x11, 0x08, 0x0C, 0x06, 0x2F, 0x44, 0x40, 0x07, 0x0E, 0x09, 0x15, 0x1D}, 14));
    ESP_ERROR_CHECK(esp_lcd_panel_io_tx_param(io_handle, 0xC5, (uint8_t[]){0x1C}, 1));
    ESP_ERROR_CHECK(esp_lcd_panel_set_gap(panel_handle, 0, 0));
    ESP_ERROR_CHECK(esp_lcd_panel_invert_color(panel_handle, false));
    ESP_ERROR_CHECK(esp_lcd_panel_disp_on_off(panel_handle, true));
}

/* 面板句柄（LVGL flush 使用；未初始化返回 NULL） */
esp_lcd_panel_handle_t bsp_lcd_panel_get(void)
{
    return panel_handle;
}

/* ---------- 屏幕与 LVGL 初始化 ----------
 * 功能：三阶段——① 硬件（背光、SPI2、ST7789 序列与伽马）② LVGL 绘制
 *       缓冲 ③ 显示驱动注册+互斥锁。上电只会调用一次。
 * 可调整内容：
 *   - 背光：gpio_set_level(...,1) 是常亮；想 PWM 调亮度需加 LEDC 通道
 *     （改这里 + 初始化 ledc，占空比即亮度，别用 0/1 电平）。
 *   - 伽马数组：见文件头说明。
 *   - 绘制缓冲：三级回退策略，PSRAM 模式下视频刷新会慢（渲染写 PSRAM
 *     远慢于 SRAM），是 SRAM 不足的兜底，不是首选。 */
void bsp_lvgl_init(void) {
    // 1. 屏幕硬件：面板未初始化则初始化（否则复用已就绪的面板）
    if (panel_handle == NULL) {
        bsp_lcd_early_init();
    }

    // 2. 初始化 LVGL 核心及匹配大小的 Buffer
    lv_init();

    // 缓冲策略（视频播放器：LCD 全屏刷新是每帧主瓶颈，渲染缓冲必须留在 SRAM——
    // PSRAM 缓冲让渲染+flush 每帧慢 ~13ms，实测 fps 25→18 的元凶）。
    // SRAM 预算只够半屏 x2：LVGL 把一帧拆两片渲染 + 两片异步 flush，
    // DMA 与渲染重叠，每帧发布 ~15ms（旧内嵌版实测 25fps）。
    // 优先级（BLE 使能后强制 PSRAM：BT/wifi PHY 初始化需要内部 RAM，
    //   半屏 SRAM×2 会使其在预编译固件内静默卡死——见 sdkconfig 注释）：
    //   ① 全屏双缓冲 PSRAM（首选，BLE/WiFi 共存必需）
    //   ② 全屏双缓冲 SRAM（PSRAM 分配失败兜底）
    size_t full_bytes = (size_t)LCD_H_RES * LCD_V_RES * sizeof(lv_color_t);
    size_t buf_pixels = (size_t)LCD_H_RES * LCD_V_RES;
    lv_color_t *buf1 = (lv_color_t *)heap_caps_malloc(full_bytes, MALLOC_CAP_SPIRAM | MALLOC_CAP_DMA);
    lv_color_t *buf2 = (lv_color_t *)heap_caps_malloc(full_bytes, MALLOC_CAP_SPIRAM | MALLOC_CAP_DMA);
    const char *draw_strat = "PSRAM full";
    if (!buf1 || !buf2) {
        if (buf1) heap_caps_free(buf1);
        if (buf2) heap_caps_free(buf2);
        buf1 = (lv_color_t *)heap_caps_malloc(full_bytes, MALLOC_CAP_INTERNAL | MALLOC_CAP_DMA);
        buf2 = (lv_color_t *)heap_caps_malloc(full_bytes, MALLOC_CAP_INTERNAL | MALLOC_CAP_DMA);
        draw_strat = "SRAM full";
    }
    ESP_LOGI("BSP_LVGL", "draw buf: %dKB x2 %s", (int)(buf_pixels * sizeof(lv_color_t)) / 1024, draw_strat);

    static lv_disp_draw_buf_t disp_buf;
    lv_disp_draw_buf_init(&disp_buf, buf1, buf2, buf_pixels);

    lv_disp_drv_init(&disp_drv);
    disp_drv.hor_res = LCD_H_RES;
    disp_drv.ver_res = LCD_V_RES;
    disp_drv.flush_cb = lvgl_disp_flush_cb;
    disp_drv.draw_buf = &disp_buf;
    disp_drv.user_data = panel_handle;
    lv_disp_drv_register(&disp_drv);

    lvgl_mutex = xSemaphoreCreateMutex();
}

/* ---------- LVGL 调度任务 ----------
 * 功能：以 ~100Hz 节拍驱动 LVGL：喂时间戳（lv_tick_inc）→ 处理定时器
 *       与渲染（lv_timer_handler）。LVGL 所有回调都发生在本任务上下文。
 * 可调整内容：vTaskDelay(10ms) 是节拍周期（见文件头）；时间戳来源
 *       esp_timer_get_time 毫秒级足够（动画/定时器按此推进）。 */
static void lvgl_task(void *arg) {
    int64_t last_tick_ms = esp_timer_get_time() / 1000;
    while (1) {
        vTaskDelay(pdMS_TO_TICKS(10));
        bsp_lvgl_lock();
        int64_t now_ms = esp_timer_get_time() / 1000;
        lv_tick_inc((uint32_t)(now_ms - last_tick_ms));
        last_tick_ms = now_ms;
        lv_timer_handler();
        bsp_lvgl_unlock();
    }
}

/* ---------- 启动 LVGL 任务 ----------
 * 功能：把 lvgl_task 固定在 core1 优先级5（高于 MJPEG 任务4）。
 * 可调整内容：栈 8KB 够用；绑 core1 是为与 JPEG 解码(core0)并行，
 *       解开绑核后调度器可能把两个重活任务放同核，反而变慢。 */
void bsp_lvgl_start_task(void) {
    xTaskCreatePinnedToCore(lvgl_task, "lvgl_task", 4096 * 2, NULL, 5, NULL, 1);
}