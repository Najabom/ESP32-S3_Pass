/* ============================================================================
 * bsp_sdcard.c - SD 卡读写与热插拔（SPI3 + SDSPI 主机 + FATFS）
 *
 * 结构：spi_bus_initialize(SPI3, SCK=7/MOSI=5/MISO=6，仅一次) →
 *       sdspi 设备(CS=4) + esp_vfs_fat_sdspi_mount 挂载 FAT 到 /sdcard →
 *       sd_monitor_task 1s 轮询热插拔（挂载↔CMD13 探测↔卸载）。
 * 为什么占 SPI3：ESP32-S3 只有 SPI2(屏)/SPI3 两条用户总线，且无 SDMMC
 *   外设（SD 只能走 SPI）；触摸已改 GPIO 模拟（bsp_touch.c），SPI3 归 SD。
 * 可调整内容：
 *   - 时钟 5MHz（SDSPI 兜底档）：杜邦线接触不良时 20MHz 会偶发 SPI 挂死
 *     （16G 卡 FAT 表顺序读多次复现）；5MHz 牺牲加载速度换稳定性，
 *     需要速度且线材可靠时可回升 10~20MHz（改 sdcard_mount_locked 与
 *     sdcard_mount_gpt_locked 里的 host.max_freq_khz）。
 *   - format_if_mount_failed=false：故意不自动格式化，防误清空有数据的
 *     卡；新卡请先在电脑格成 FAT32（勿改 true，除非明确知道后果）。
 *   - BSP_SDCARD_RW_TEST：首次挂载成功后写-读-比对一个小文件验证链路；
 *     置 0 则完全不写卡（纯挂载）。热插重挂不再重复测试。
 *   - MONITOR_POLL_FAST/SLOW_MS：轮询节奏；连续挂载失败 MONITOR_FAIL_BACKOFF
 *     次后降频到 SLOW，避免无卡时日志/总线空转。
 * GPT 支持：FATFS 原生只认 MBR 分区表；Windows 对 16G+ 卡默认写 GPT
 *   （LBA1 "EFI PART"，FAT 卷在 LBA 2048 起）。标准挂载失败后这里会
 *   手动解析 GPT 分区表，用自定义 diskio（扇区加分区偏移）偏移挂载。
 * 坑：
 *   - FATFS 默认关长文件名（8.3 格式）；本工程在 sdkconfig.defaults 开了
 *     LFN + UTF-8 后才支持长名/中文名（loop.mjpeg 的 5 字符扩展名就需要）。
 *   - MISO 内部上拉：卡未选中时 MISO 高阻，无上拉易读进随机噪声
 *     （必须在 spi_bus_initialize 之后设置，否则会被总线初始化覆盖）。
 *   - spi_bus_initialize 只允许调一次，重复调用返回 ESP_ERR_INVALID_STATE
 *     ——热插拔反复挂载时绝不能重复初始化总线（s_bus_ready 守卫）。
 *   - 拔出检测靠 CMD13（sdmmc_get_status）：无 CD 检测脚的折中方案；
 *     响应超时/无应答即判定拔出。
 *   - 挂载/卸载/所有文件 IO 共用一把互斥锁，防止卸载瞬间踩到打开中的
 *     文件句柄（VFS 对悬空句柄的行为未定义）。
 *   - esp_vfs_fat_sdspi_mount 失败后其内部已把 SPI 设备与 VFS 注册清理
 *     干净（call_host_deinit + unregister + free），GPT 兜底可安全重建。
 * ==========================================================================*/

// bsp/bsp_sdcard.c
#include "bsp_sdcard.h"
#include <stdio.h>
#include <string.h>
#include <stdlib.h>
#include "esp_log.h"
#include "esp_heap_caps.h"
#include "esp_vfs_fat.h"
#include "sdmmc_cmd.h"
#include "driver/sdspi_host.h"
#include "driver/spi_common.h"
#include "driver/gpio.h"
#include "freertos/FreeRTOS.h"
#include "freertos/task.h"
#include "freertos/semphr.h"
#include "esp_timer.h"
#include "ff.h"
#include "diskio_impl.h"

#define SD_SPI_HOST           SPI3_HOST
#define BSP_SDCARD_RW_TEST    1

#define MONITOR_POLL_FAST_MS  200    // 常规轮询周期（拔出检测及时性：拔卡后 ~200ms 内提示切换）
#define MONITOR_POLL_SLOW_MS  5000   // 连续挂载失败后的降频周期
#define MONITOR_FAIL_BACKOFF  5      // 连续失败多少次后降频

static const char *TAG = "BSP_SDCARD";
static sdmmc_card_t *s_card = NULL;
static SemaphoreHandle_t s_lock = NULL;
static bool s_bus_ready = false;
static bool s_rw_test_done = false;
static int s_fail_streak = 0;   /* 连续挂载失败次数（monitor 维护，播放器据此分型提示） */
static bool s_probe_ok = false; /* 最近一次 probe 探测：卡在且可初始化 = true（区分"无法识别"与"请插入"） */

/* ---------- 加载传输看门狗 ----------
 * 目的：SPI 数据块传输无超时（fread 可永久阻塞），线材接触不良时
 *       传输偶发"永远等不到完成"（DMA/中断丢失）。fread 不返回，
 *       load_file_ex 内部的 20s 硬超时检查形同虚设。
 * 机制：每次 fread 前/后刷新时间戳；monitor 每轮检查——超过 25s
 *       没有任何传输进展 = 传输挂死 → 自动重启恢复（重启后卡大概率
 *       恢复，属硬件故障兜底，比无限僵住强）。 */
static volatile int64_t s_load_activity_ts = 0;   /* 0 = 当前无加载活动 */
static portMUX_TYPE s_load_mux = portMUX_INITIALIZER_UNLOCKED;
RTC_NOINIT_ATTR static uint32_t s_wdog_reboot_mark;   /* 看门狗重启标记（RTC 不掉电保留） */

static void load_mark_activity(void) {
    portENTER_CRITICAL(&s_load_mux);
    s_load_activity_ts = esp_timer_get_time();
    portEXIT_CRITICAL(&s_load_mux);
}

static void load_mark_idle(void) {
    portENTER_CRITICAL(&s_load_mux);
    s_load_activity_ts = 0;
    portEXIT_CRITICAL(&s_load_mux);
}

static int64_t load_last_activity(void) {
    int64_t v;
    portENTER_CRITICAL(&s_load_mux);
    v = s_load_activity_ts;
    portEXIT_CRITICAL(&s_load_mux);
    return v;
}

/* ---------- 传输挂死看门狗任务 ----------
 * 必须独立成任务且永不取 s_lock：monitor 在加载期间会被 s_lock 阻塞
 * （播放器持锁 fread 卡死），锁内检查形同虚设。本任务 1s 轮询
 * s_load_activity_ts——任何加载活动停滞 25s = SPI 传输永久挂起
 * （fread 不返回）→ 标记 RTC + esp_restart 兜底恢复。
 * 重启标记不打印日志（UART 输出可能同样被堵死）。 */
static void sd_transfer_watchdog(void *arg) {
    (void)arg;
    for (;;) {
        vTaskDelay(pdMS_TO_TICKS(1000));
        int64_t la = load_last_activity();
        if (la == 0) continue;
        int64_t stuck_us = esp_timer_get_time() - la;
        if (stuck_us > 25000000LL) {
            s_wdog_reboot_mark = 0x57444731U;   /* "WDG1" */
            esp_restart();
        }
    }
}

/* ---------- GPT 分区挂载状态 ----------
 * FATFS 原生只认 MBR 分区表；Windows 对 16G+ 卡默认写 GPT（LBA1 "EFI PART"），
 * FAT 卷在 LBA 2048 起——标准 esp_vfs_fat_sdspi_mount 直接挂整卡会
 * FR_NO_FILESYSTEM。这里在标准挂载失败后手动解析 GPT 并"偏移挂载"：
 * 自定义 diskio 把所有扇区访问加上分区起始 LBA。 */
static bool     s_gpt_mounted = false;
static uint32_t s_gpt_part_lba = 0;
static uint32_t s_gpt_part_sectors = 0;
static FATFS   *s_gpt_fs = NULL;
static sdmmc_card_t *s_gpt_card = NULL;   /* GPT diskio 转发用的卡（mount 时设置） */

static bool gpt_find_fat_lba(sdmmc_card_t *card, uint32_t *out_lba, uint32_t *out_sectors);
static esp_err_t sdcard_mount_gpt_locked(void);

bool bsp_sdcard_is_mounted(void) {
    return s_card != NULL;
}

int bsp_sdcard_fail_streak(void) {
    return s_fail_streak;
}

/* 最近一次 probe 探测是否"卡在且可初始化"（false = 卡不在/没插好）。
 * 与 fail_streak 配合：fail_streak≥3 且 probe_ok → "SD卡无法识别"；
 * 否则 → "请插入SD卡"。 */
bool bsp_sdcard_probe_ok(void) {
    return s_probe_ok;
}

void bsp_sdcard_lock(void) {
    if (s_lock) xSemaphoreTake(s_lock, portMAX_DELAY);
}

void bsp_sdcard_unlock(void) {
    if (s_lock) xSemaphoreGive(s_lock);
}

/* ---------- 读写冒烟测试（仅首次挂载执行一次，可用 BSP_SDCARD_RW_TEST 关掉） ----------
 * 功能：写一个固定内容的小文件 → 读回 → 逐字节比对。验证"挂载成功"
 *       之外的真实读写链路（接触不良/写保护会在这里暴露）。 */
static esp_err_t sdcard_rw_test(void) {
    const char *path = BSP_SDCARD_MOUNT_POINT "/rw_test.txt";
    const char *msg = "ESP32-S3 SD card read/write OK\n";

    FILE *f = fopen(path, "w");
    if (f == NULL) return ESP_FAIL;
    fputs(msg, f);
    fclose(f);

    char buf[64] = {0};
    f = fopen(path, "r");
    if (f == NULL) return ESP_FAIL;
    fgets(buf, sizeof(buf), f);
    fclose(f);

    if (strcmp(buf, msg) != 0) return ESP_FAIL;
    ESP_LOGI(TAG, "RW test passed (%s)", path);
    return ESP_OK;
}

/* ---------- 挂载（内部，调用方须已持锁） ----------
 * 功能：sdspi 设备 + FAT 挂载。已挂载时直接返回 ESP_OK。
 * 失败语义：保证无残留状态（s_card=NULL），返回错误码。
 * 双路径：MBR（标准）→ 失败 → GPT（自解析偏移挂载）。 */
static esp_err_t sdcard_mount_at_locked(int freq_khz); /* 单档尝试 */

/* 频率阶梯：20MHz 提速档为主；线材 margin 不足时
 * 高概率 0x107/超时（历史：16G+ 新芯片卡 20MHz 隐患）→ 逐档回落
 * 10M/5M，任一档成功即挂载（boot 首挂也只多花最多一次失败探测）。 */
static esp_err_t sdcard_mount_locked(void) {
    if (s_card != NULL) return ESP_OK;
    esp_err_t last = ESP_FAIL;
    static const int ladder[] = { 20000, 10000, 5000 };
    for (unsigned i = 0; i < sizeof(ladder) / sizeof(ladder[0]); i++) {
        last = sdcard_mount_at_locked(ladder[i]);
        if (last == ESP_OK) return ESP_OK;
    }
    return last;
}

static esp_err_t sdcard_mount_at_locked(int freq_khz) {
    if (s_card != NULL) return ESP_OK;

    sdmmc_host_t host = SDSPI_HOST_DEFAULT();
    /* 频率：20MHz 提速档（流式读卡 5MHz 只够 ~450KB/s）。
     * 历史：40MHz 初始化必败；20MHz 对 1G/4G 卡稳定但部分 16G+
     * （SDHC 新芯片）在提速段会 0x107 超时；10MHz 曾偶发 SPI 传输
     * 挂死（杜邦线 margin 不足，看门狗 25s 兜底重启）。 */
    host.max_freq_khz = freq_khz;
    sdspi_device_config_t slot_config = SDSPI_DEVICE_CONFIG_DEFAULT();
    slot_config.gpio_cs = PIN_NUM_SD_CS;
    slot_config.host_id = SD_SPI_HOST;

    esp_vfs_fat_mount_config_t mount_config = {
        .format_if_mount_failed = false,   // 勿改 true：防误格式化用户卡
        .max_files = 5,
        .allocation_unit_size = 32 * 1024,
    };

    esp_err_t ret = esp_vfs_fat_sdspi_mount(BSP_SDCARD_MOUNT_POINT, &host,
                                            &slot_config, &mount_config, &s_card);
    if (ret != ESP_OK) {
        s_card = NULL;
        /* MBR 挂载失败 → 尝试 GPT（16G+ 卡 Windows 默认 GPT 分区表，
         * FATFS 原生不认）。失败路径已被 esp_vfs_fat_sdspi_mount 清理干净。 */
        if (sdcard_mount_gpt_locked() == ESP_OK) {
            return ESP_OK;
        }
        return ret;
    }

    sdmmc_card_print_info(stdout, s_card);

#if BSP_SDCARD_RW_TEST
    if (!s_rw_test_done) {
        s_rw_test_done = true;
        if (sdcard_rw_test() != ESP_OK) {
            ESP_LOGW(TAG, "RW test failed（已挂载但读写异常，检查卡写保护/接触）");
        }
    }
#endif
    return ESP_OK;
}

/* ---------- 卸载（内部，调用方须已持锁） ---------- */
static void sdcard_unmount_locked(void) {
    if (s_card == NULL) return;
    if (s_gpt_mounted) {
        /* GPT 路径：f_mount 卸载 + VFS 反注册 + 释放自定义 diskio 与卡 */
        s_gpt_mounted = false;
        f_mount(NULL, "0:", 0);
        esp_vfs_fat_unregister_path(BSP_SDCARD_MOUNT_POINT);
        ff_diskio_unregister(0);
        sdspi_host_remove_device(s_card->host.slot);
        free(s_card);
        s_card = NULL;
        s_gpt_card = NULL;
        s_gpt_fs = NULL;
        return;
    }
    esp_vfs_fat_sdcard_unmount(BSP_SDCARD_MOUNT_POINT, s_card);
    s_card = NULL;
}

/* ---------- GPT 偏移 diskio（自定义 FATFS 底层） ----------
 * 功能：把 FATFS 的扇区访问整体加 s_gpt_part_lba 偏移转发到真实卡。
 * 注意：s_card 在挂载成功后才赋值，FATFS 挂载期间 disk_read 用的是
 *       mount_gpt 内部传入的 card——故 impl 读 card 时直接引用 s_card
 *       不可行；这里用"当前 GPT 卡"的模块级指针（顶部声明），mount 时设置。 */

static DSTATUS gpt_disk_init(BYTE pdrv)   { (void)pdrv; return 0; }
static DSTATUS gpt_disk_status(BYTE pdrv) { (void)pdrv; return (s_gpt_card != NULL) ? 0 : STA_NOINIT; }

static DRESULT gpt_disk_read(BYTE pdrv, BYTE *buff, DWORD sector, UINT count) {
    (void)pdrv;
    if (s_gpt_card == NULL) return RES_NOTRDY;
    return sdmmc_read_sectors(s_gpt_card, buff, (uint32_t)sector + s_gpt_part_lba, count)
           == ESP_OK ? RES_OK : RES_ERROR;
}

static DRESULT gpt_disk_write(BYTE pdrv, const BYTE *buff, DWORD sector, UINT count) {
    (void)pdrv;
    if (s_gpt_card == NULL) return RES_NOTRDY;
    return sdmmc_write_sectors(s_gpt_card, buff, (uint32_t)sector + s_gpt_part_lba, count)
           == ESP_OK ? RES_OK : RES_ERROR;
}

static DRESULT gpt_disk_ioctl(BYTE pdrv, BYTE cmd, void *buff) {
    (void)pdrv;
    switch (cmd) {
    case CTRL_SYNC: return RES_OK;
    case GET_SECTOR_COUNT: *(DWORD *)buff = s_gpt_part_sectors; return RES_OK;
    case GET_SECTOR_SIZE:  *(WORD *)buff = 512; return RES_OK;
    case GET_BLOCK_SIZE:   *(DWORD *)buff = 1; return RES_OK;
    default: return RES_PARERR;
    }
}

static const ff_diskio_impl_t s_gpt_impl = {
    gpt_disk_init, gpt_disk_status, gpt_disk_read, gpt_disk_write, gpt_disk_ioctl
};

/* ---------- GPT 分区表解析（内部，调用方须已持锁） ----------
 * 功能：读 LBA0/LBA1 确认 GPT，遍历分区表（LBA2 起，128B/项）找
 *       MS-FAT 类型 GUID（EBD0A0A2-B9E5-4433-87C0-68B6B72699C7）。
 * 返回：找到返回 true 并输出分区起始 LBA 与扇区数。 */
static bool gpt_find_fat_lba(sdmmc_card_t *card, uint32_t *out_lba, uint32_t *out_sectors) {
    uint8_t sec[512];
    if (sdmmc_read_sectors(card, sec, 0, 1) != ESP_OK) return false;
    if (sec[510] != 0x55 || sec[511] != 0xaa) return false;
    if (sdmmc_read_sectors(card, sec, 1, 1) != ESP_OK) return false;
    if (memcmp(sec, "EFI PART", 8) != 0) return false;

    uint64_t pt_lba = *(const uint64_t *)(sec + 72);
    uint32_t pt_cnt = *(const uint32_t *)(sec + 80);
    uint32_t pt_sz  = *(const uint32_t *)(sec + 84);
    if (pt_lba == 0 || pt_cnt == 0 || pt_cnt > 128 || pt_sz < 128) return false;

    /* MS-FAT 分区类型 GUID（小端序：EBD0A0A2-B9E5-4433-87C0-68B6B72699C7） */
    static const uint8_t ms_fat_guid[16] = {
        0xA2, 0xA0, 0xD0, 0xEB, 0xE5, 0xB9, 0x33, 0x44,
        0x87, 0xC0, 0x68, 0xB6, 0xB7, 0x26, 0x99, 0xC7
    };
    for (uint32_t i = 0; i < pt_cnt; i++) {
        uint64_t entry_lba = pt_lba + ((uint64_t)i * pt_sz) / 512;
        uint32_t off = ((uint64_t)i * pt_sz) % 512;
        if (off + 128 > 512) continue;   /* 项跨扇区：跳过（罕见，不深究） */
        if (sdmmc_read_sectors(card, sec, entry_lba, 1) != ESP_OK) return false;
        if (memcmp(sec + off, ms_fat_guid, 16) == 0) {
            uint64_t first = *(const uint64_t *)(sec + off + 32);
            uint64_t last  = *(const uint64_t *)(sec + off + 40);
            if (first == 0 || last < first) return false;
            *out_lba = (uint32_t)first;
            *out_sectors = (uint32_t)(last - first + 1);
            return true;
        }
    }
    return false;
}

/* ---------- GPT 偏移挂载（内部，调用方须已持锁） ----------
 * 功能：标准挂载失败（GPT 卡）时：重新 sdspi 设备 + 卡初始化 → 解析
 *       GPT → 注册自定义 diskio（扇区加偏移）→ f_mount。
 * 注意：esp_vfs_fat_sdspi_mount 失败后已把 SPI 设备与 VFS 注册全部清理
 *       干净，这里从零重建，不会冲突。 */
static esp_err_t sdcard_mount_gpt_locked(void) {
    sdspi_device_config_t dslot = SDSPI_DEVICE_CONFIG_DEFAULT();
    dslot.gpio_cs = PIN_NUM_SD_CS;
    dslot.host_id = SD_SPI_HOST;
    sdspi_dev_handle_t handle;
    if (sdspi_host_init_device(&dslot, &handle) != ESP_OK) return ESP_FAIL;

    sdmmc_card_t *card = malloc(sizeof(sdmmc_card_t));
    if (card == NULL) {
        sdspi_host_remove_device(handle);
        return ESP_ERR_NO_MEM;
    }
    sdmmc_host_t ghost = SDSPI_HOST_DEFAULT();
    ghost.max_freq_khz = 5000;
    ghost.slot = handle;
    esp_err_t err = sdmmc_card_init(&ghost, card);
    if (err != ESP_OK) {
        free(card);
        sdspi_host_remove_device(handle);
        return err;
    }

    uint32_t part_lba = 0, part_sectors = 0;
    if (!gpt_find_fat_lba(card, &part_lba, &part_sectors)) {
        free(card);
        sdspi_host_remove_device(handle);
        return ESP_FAIL;
    }

    esp_vfs_fat_conf_t conf = {
        .base_path = BSP_SDCARD_MOUNT_POINT,
        .fat_drive = "0:",
        .max_files = 5,
    };
    esp_err_t rerr = esp_vfs_fat_register(&conf, &s_gpt_fs);
    if (rerr != ESP_OK) {
        ESP_LOGW(TAG, "GPT: esp_vfs_fat_register failed %s", esp_err_to_name(rerr));
        free(card);
        sdspi_host_remove_device(handle);
        s_gpt_fs = NULL;
        return rerr;
    }
    s_gpt_part_lba = part_lba;
    s_gpt_part_sectors = part_sectors;
    /* diskio 用的卡必须挂在 f_mount 之前：FATFS 挂载时要读 0 扇区
     * （disk_read 判 NULL 会 RES_NOTRDY → FR_DISK_ERR） */
    s_gpt_card = card;
    ff_diskio_register(0, &s_gpt_impl);
    FRESULT fr = f_mount(s_gpt_fs, "0:", 1);
    if (fr != FR_OK) {
        ESP_LOGW(TAG, "GPT: f_mount failed (%d)", fr);
        f_mount(NULL, "0:", 0);
        esp_vfs_fat_unregister_path(BSP_SDCARD_MOUNT_POINT);
        ff_diskio_unregister(0);
        free(card);
        sdspi_host_remove_device(handle);
        s_gpt_fs = NULL;
        return ESP_FAIL;
    }
    s_gpt_mounted = true;
    s_card = card;
    ESP_LOGI(TAG, "GPT partition mounted: LBA %u, %u sectors (%u MB)",
             part_lba, part_sectors, part_sectors / 2048);
    return ESP_OK;
}

/* ---------- 热插拔监控任务 ----------
 * 功能：1s 轮询。未挂载→尝试挂载（连续失败降频+首条告警后静默，防刷屏）；
 *       已挂载→CMD13 探测卡是否还在，异常则卸载。
 * 为什么不做事件回调：业务层（播放器/收卡记录）本身就有自己的任务循环，
 *       轮询 bsp_sdcard_is_mounted() 即可，省一套回调注册与跨任务竞态。 */
static void sd_monitor_task(void *arg) {
    for (;;) {
        bsp_sdcard_lock();
        if (s_card == NULL) {
            esp_err_t ret = sdcard_mount_locked();
            bsp_sdcard_unlock();
            if (ret == ESP_OK) {
                s_fail_streak = 0;
                ESP_LOGI(TAG, "SD card mounted");
                vTaskDelay(pdMS_TO_TICKS(MONITOR_POLL_FAST_MS));
            } else {
                s_fail_streak++;
                if (s_fail_streak == 1) {
                    const char *why;
                    if (ret == ESP_ERR_TIMEOUT) {
                        why = "卡无响应（未插好/接触不良/卡不兼容）";
                    } else if (ret == ESP_ERR_INVALID_RESPONSE) {
                        why = "卡响应异常（线材质量/时钟过快）";
                    } else {
                        why = esp_err_to_name(ret);
                    }
                    ESP_LOGW(TAG, "mount failed: %s，将持续重试", why);
                    /* 诊断：协议层通过但 FAT 挂载失败（FR_NO_FILESYSTEM）时，
                     * 手动初始化一次卡，打印卡信息与关键扇区特征（区分
                     * exFAT/NTFS/GPT/未格式化/假卡）。 */
                    sdspi_device_config_t dslot = SDSPI_DEVICE_CONFIG_DEFAULT();
                    dslot.gpio_cs = PIN_NUM_SD_CS;
                    dslot.host_id = SD_SPI_HOST;
                    sdspi_dev_handle_t card_handle = (sdspi_dev_handle_t)-1;
                    sdmmc_card_t dcard_s;
                    esp_err_t pret = sdspi_host_init_device(&dslot, &card_handle);
                    if (pret == ESP_OK) {
                        sdmmc_host_t dhost = SDSPI_HOST_DEFAULT();
                        dhost.max_freq_khz = 5000;
                        dhost.slot = card_handle;
                        pret = sdmmc_card_init(&dhost, &dcard_s);
                    }
                    /* 探测结果：卡在且能初始化 = 真"无法识别"；
                     * init 超时 = 卡不在/没插好 → UI 显示"请插入SD卡" */
                    s_probe_ok = (pret == ESP_OK);
                    if (pret == ESP_OK) {
                        /* 注意：全部用 WARN 级——开机终端在 5s 后会把日志
                         * 降到 WARN，INFO 级诊断会被过滤看不到 */
                        ESP_LOGW(TAG, "probe: %s cap=%uMB is_mem=%d is_mmc=%d",
                                 dcard_s.cid.name,
                                 (unsigned)(dcard_s.csd.capacity / 2048),
                                 dcard_s.is_mem, dcard_s.is_mmc);
                        /* 大缓冲放 static：probe 在 sd_mon 任务内执行，
                         * 栈 4KB 装不下 512B 扇区 + 扫描（曾栈溢出复位） */
                        static uint8_t s_boot[512];
                        static const uint32_t scan_lbas[] = {
                            1, 2048, 4096, 8192, 16384, 32768, 65536, 131072
                        };
                        for (int si = 0; si < (int)(sizeof(scan_lbas) / 4); si++) {
                            if (sdmmc_read_sectors(&dcard_s, s_boot, scan_lbas[si], 1)
                                != ESP_OK) {
                                ESP_LOGW(TAG, "LBA %u: read failed",
                                         (unsigned)scan_lbas[si]);
                                continue;
                            }
                            char hx[64]; int p = 0;
                            for (int i = 0; i < 16; i++) {
                                p += sprintf(hx + p, "%02x ", s_boot[i]);
                            }
                            ESP_LOGW(TAG, "LBA %u: %s| sig=%02x%02x oem=\"%c%c%c%c%c%c%c%c\" f32=%d exfat=%d ntfs=%d pt0=%02x",
                                     (unsigned)scan_lbas[si], hx,
                                     s_boot[510], s_boot[511],
                                     s_boot[3], s_boot[4], s_boot[5], s_boot[6],
                                     s_boot[7], s_boot[8], s_boot[9], s_boot[10],
                                     (s_boot[82] == 0x28 || s_boot[82] == 0x29),
                                     (s_boot[3] == 'E' && s_boot[4] == 'X'),
                                     (s_boot[3] == 'N' && s_boot[4] == 'T'),
                                     s_boot[446]);
                        }
                    } else {
                        ESP_LOGW(TAG, "probe: card init failed %s", esp_err_to_name(pret));
                    }
                    if (card_handle >= 0) {
                        sdspi_host_remove_device(card_handle);
                    }
                }
                vTaskDelay(pdMS_TO_TICKS(s_fail_streak < MONITOR_FAIL_BACKOFF
                                         ? MONITOR_POLL_FAST_MS : MONITOR_POLL_SLOW_MS));
            }
        } else {
            esp_err_t ret = sdmmc_get_status(s_card);
            bsp_sdcard_unlock();
            if (ret != ESP_OK) {
                ESP_LOGW(TAG, "card probe failed: %s（判定拔出，卸载）",
                         esp_err_to_name(ret));
                bsp_sdcard_lock();
                sdcard_unmount_locked();
                bsp_sdcard_unlock();
            }
            vTaskDelay(pdMS_TO_TICKS(MONITOR_POLL_FAST_MS));
        }
    }
}

/* ---------- 初始化 ----------
 * 功能：互斥锁 + SPI3 总线（仅一次）+ 首次挂载尝试。
 * 失败语义：总线级失败返回错误码（SD 功能整体不可用）；仅挂载失败
 *       （无卡）仍返回 ESP_OK，由 monitor 后续自动挂载。 */
esp_err_t bsp_sdcard_init(void) {
    ESP_LOGI(TAG, "Initializing SD card via SPI3_HOST (CS=4 MOSI=5 MISO=6 SCK=7)");

    if (s_lock == NULL) {
        s_lock = xSemaphoreCreateMutex();
        if (s_lock == NULL) return ESP_ERR_NO_MEM;
    }

    if (!s_bus_ready) {
        spi_bus_config_t bus_cfg = {
            .mosi_io_num = PIN_NUM_SD_MOSI,
            .miso_io_num = PIN_NUM_SD_MISO,
            .sclk_io_num = PIN_NUM_SD_SCK,
            .quadwp_io_num = -1,
            .quadhd_io_num = -1,
            .max_transfer_sz = 32 * 1024,   // 32KB：FAT 簇 16KB 的整数倍，DMA 链式传输，
                                            // 比 4KB 显著减少 SD 命令/中断开销
        };
        esp_err_t ret = spi_bus_initialize(SD_SPI_HOST, &bus_cfg, SPI_DMA_CH_AUTO);
        if (ret != ESP_OK) {
            ESP_LOGE(TAG, "spi_bus_initialize failed: %s", esp_err_to_name(ret));
            return ret;
        }
        ESP_LOGI(TAG, "spi bus ready (SPI3_HOST, 32KB max transfer)");
        // 卡未选中时 MISO 高阻；内部上拉防悬空噪声（须在总线初始化后设置）
        gpio_set_pull_mode(PIN_NUM_SD_MISO, GPIO_PULLUP_ONLY);
        // 输出脚驱动强度拉满：杜邦线 + 高速下弱驱动导致边沿过缓、
        // 采样余量不足（曾偶发 data CRC failed）；S3 GPIO 矩阵输出可独立配置
        // （GPIO_DRIVE_CAP_MAX 是计数非法值，最强是 GPIO_DRIVE_CAP_3）
        gpio_set_drive_capability(PIN_NUM_SD_MOSI, GPIO_DRIVE_CAP_3);
        gpio_set_drive_capability(PIN_NUM_SD_SCK, GPIO_DRIVE_CAP_3);
        gpio_set_drive_capability(PIN_NUM_SD_CS, GPIO_DRIVE_CAP_3);
        ESP_LOGI(TAG, "gpio ready (CS=4 MOSI=5 MISO=6 SCK=7, drive max)");
        s_bus_ready = true;
    }

    bsp_sdcard_lock();
    esp_err_t ret = sdcard_mount_locked();
    bsp_sdcard_unlock();
    if (ret != ESP_OK) {
        ESP_LOGW(TAG, "首次挂载失败: %s（无卡可稍后热插，monitor 会自动识别）",
                 esp_err_to_name(ret));
    }
    return ESP_OK;
}

void bsp_sdcard_start_monitor(void) {
    static bool s_started = false;
    if (s_started) return;
    s_started = true;
    /* 上次因 SD 传输挂死自动重启的标记（RTC 域，掉电/手动复位不保留） */
    if (s_wdog_reboot_mark == 0x57444731U) {
        s_wdog_reboot_mark = 0;
        ESP_LOGW(TAG, "上次因 SD 传输挂死自动重启（线材接触不良？请检查杜邦线）");
    }
    /* 栈 4096：probe 诊断块含 512B 扇区缓冲（static）与 sdmmc_card_t（约 200B），
     * 3072 曾栈溢出复位（Rebooting: stack overflow in task sd_mon） */
    xTaskCreatePinnedToCore(sd_monitor_task, "sd_mon", 4096, NULL, 2, NULL, 1);
    /* 传输挂死看门狗：独立任务（永不取 s_lock，monitor 会被锁阻塞） */
    xTaskCreatePinnedToCore(sd_transfer_watchdog, "sd_wdog", 2048, NULL, 2, NULL, 1);
}

/* ---------- 整文件读取（内部持锁，缓冲在 PSRAM） ----------
 * 功能：fopen → 取大小（上限 BSP_SDCARD_LOAD_MAX）→ PSRAM 分配 → 整读。
 * 为什么整读进 PSRAM：播放链路是纯内存指针驱动，预载后 SD 只承担秒级
 *       的加载窗口，拔卡/总线抖动不影响播放与显示。
 * 错误恢复：整读中途失败（本卡在高速持续读取偶发 data CRC 错误，
 *       卡控制器会卡死、探针 CMD13 也会超时）→ 卸载重挂恢复控制器 →
 *       整文件重试，最多 2 次。否则 monitor 会误判拔卡，反复卸载/重挂
 *       一轮就要浪费 10 秒级时间。
 * 注意：单次尝试有 20s 硬超时（fread 返回后检查）；SPI 传输层挂死
 *       （fread 不返回）无法在此恢复，属硬件级故障（见 5MHz 说明）。 */
uint8_t *bsp_sdcard_load_file_ex(const char *path, size_t *out_size,
                                 bsp_sdcard_progress_cb_t cb, void *ctx) {
    uint8_t *buf = NULL;
    if (out_size) *out_size = 0;

    bsp_sdcard_lock();
    load_mark_activity();   /* 进入加载窗口：挂死看门狗开始计时 */
    for (int attempt = 0; attempt < 3; attempt++) {
        if (!bsp_sdcard_is_mounted()) break;

        FILE *f = fopen(path, "rb");
        if (f == NULL) {
            ESP_LOGW(TAG, "%s fopen 失败（已挂载=%d）：卡刚拔出/文件不存在/目录异常",
                     path, (int)bsp_sdcard_is_mounted());
            break;   // 无卡/文件不存在：重试无意义
        }

        fseek(f, 0, SEEK_END);
        long sz = ftell(f);
        fseek(f, 0, SEEK_SET);
        if (sz <= 0 || sz > BSP_SDCARD_LOAD_MAX) {
            ESP_LOGW(TAG, "%s 大小 %ld 超限/异常（上限 %d）", path, sz, BSP_SDCARD_LOAD_MAX);
            fclose(f);
            break;
        }

        if (buf == NULL) {
            buf = heap_caps_malloc((size_t)sz, MALLOC_CAP_SPIRAM | MALLOC_CAP_8BIT);
            if (buf == NULL) {
                ESP_LOGE(TAG, "PSRAM 分配 %ld 字节失败（剩余 %u）", sz,
                         (unsigned)heap_caps_get_free_size(MALLOC_CAP_SPIRAM));
                fclose(f);
                break;
            }
        }

        /* 分块读取：① 防止单次 fread 数 MB 饿死看门狗；② 每块间隔让 IDLE0
         * 喂 task_wdt；③ 打印实际速度，方便评估是否需要继续提频/换卡。
         * 块大小 4KB：实测 16G GPT 卡在 32KB 分块（64 扇区连续 DMA 链）
         * 下长传输偶发挂死（fread 永不返回、系统僵住），8 扇区短传输
         * 稳定——短传输换稳定性，加载速度损失有限。 */
        #define LOAD_CHUNK  (4 * 1024)
        int64_t t0 = esp_timer_get_time();
        int64_t deadline = t0 + 20000000LL;   /* 单次尝试硬超时 20s：卡 wedged
                                               * 时驱动内部静默重试会拖 30s+，
                                               * 超时放弃比无限等更稳（走调用方
                                               * 的无 intro/无 loop 回退路径） */
        size_t got = 0;
        size_t target = (size_t)sz;
        size_t mb_logged = 0;   /* 每读满 64KB 打一条进度日志（临时诊断加密） */
        while (got < target) {
            size_t chunk = target - got;
            if (chunk > LOAD_CHUNK) chunk = LOAD_CHUNK;
            size_t n = fread(buf + got, 1, chunk, f);
            load_mark_activity();   /* fread 返回（无论成败）：刷新看门狗时间戳 */
            if (n == 0) break;
            got += n;
            if (cb != NULL) cb((uint32_t)got, (uint32_t)target, ctx);
            if (got / (64 * 1024) > mb_logged) {
                mb_logged = got / (64 * 1024);
                ESP_LOGI(TAG, "reading %s: %u/%lu KB", path,
                         (unsigned)(got / 1024), (unsigned long)(target / 1024));
            }
            if (esp_timer_get_time() > deadline) {
                ESP_LOGE(TAG, "%s 读取超时（20s，%u/%u），放弃本文件", path,
                         (unsigned)got, (unsigned)target);
                fclose(f);
                goto load_failed;
            }
        }
        fclose(f);

        if (got == target) {
            int64_t dt_us = esp_timer_get_time() - t0;
            if (dt_us > 0) {
                ESP_LOGI(TAG, "loaded %s: %u bytes in %lld ms (%.1f KB/s)",
                         path, (unsigned)sz, dt_us / 1000,
                         (double)((float)sz / 1024.0f) / ((float)dt_us / 1e6f));
            }
            if (out_size) *out_size = (size_t)sz;
            bsp_sdcard_unlock();
            load_mark_idle();
            return buf;
        }

        ESP_LOGW(TAG, "%s 读取不完整（%u/%ld，第 %d 次），卸载重挂恢复中",
                 path, (unsigned)got, sz, attempt + 1);
        sdcard_unmount_locked();
        if (sdcard_mount_locked() != ESP_OK) {
            break;   // 卡真没了
        }
    }

load_failed:
    if (buf) heap_caps_free(buf);
    bsp_sdcard_unlock();
    load_mark_idle();
    return NULL;
}

/* bsp_sdcard_load_file_ex 的无进度回调包装 */
uint8_t *bsp_sdcard_load_file(const char *path, size_t *out_size) {
    return bsp_sdcard_load_file_ex(path, out_size, NULL, NULL);
}

/* ---------- SD 容量统计（系统信息页用） ----------
 * 功能：FATFS f_getfree 取卷总容量与空闲（SD 锁内，与卸载互斥）。
 * 注意：MBR 与 GPT 两条挂载路径都注册在 FATFS 盘符 "0:"。 */
esp_err_t bsp_sdcard_stats(uint64_t *total_bytes, uint64_t *free_bytes) {
    if (total_bytes) *total_bytes = 0;
    if (free_bytes) *free_bytes = 0;

    bsp_sdcard_lock();
    if (!bsp_sdcard_is_mounted()) {
        bsp_sdcard_unlock();
        return ESP_ERR_INVALID_STATE;
    }
    FATFS *fs = NULL;
    DWORD fre_clst = 0;
    FRESULT fr = f_getfree("0:", &fre_clst, &fs);
    bsp_sdcard_unlock();
    if (fr != FR_OK || fs == NULL) {
        ESP_LOGW(TAG, "f_getfree failed: %d", (int)fr);
        return ESP_FAIL;
    }
    if (total_bytes) {
        *total_bytes = (uint64_t)(fs->n_fatent - 2) * fs->csize * fs->ssize;
    }
    if (free_bytes) {
        *free_bytes = (uint64_t)fre_clst * fs->csize * fs->ssize;
    }
    return ESP_OK;
}
