/* ============================================================================
 * time_sync.c - 墙钟时间模块实现
 *
 * 见 time_sync.h。要点：
 *   - 时间源三条腿：外置 DS3231（可选，电池走时，开机最优先）、NVS 校准
 *     基准（PC/手机 BLE 写入）+ esp_rtc_get_time_us()
 *     （RTC 域 64 位微秒计数，跨软复位保留、断电清零）；
 *   - 所有时间运算在 uint64 域完成，最后截断到 uint32 秒（2106 年前安全）；
 *   - NVS 写入只在 BLE 校准时发生（频率极低，无磨损问题）；
 *   - 绝对对齐：set_unix 无条件接受上位机时间（无防回退守卫）；与旧墙钟
 *     存在偏差时通过 shift 钩子通知存量记录平移（冷却 NVS / cards.txt）；
 *   - 时区：tz_min 存 NVS（默认 UTC+8），"当日"判定用本地日。
 * ==========================================================================*/

// nfc/time_sync.c
#include "time_sync.h"
#include "../bsp/bsp_rtc.h"
#include "nvs.h"
#include "esp_rtc_time.h"
#include "esp_log.h"
#include <string.h>

static const char *TAG = "TIME_SYNC";

#define TS_NS    "timesync"
#define TS_KEY   "base"
#define TS_KEY_TZ "tz"

/* 校准基准：校准时刻的墙钟秒 + RTC 计数快照 */
typedef struct {
    uint32_t unix_s;
    uint64_t rtc_us;
} ts_base_t;

static ts_base_t s_base;
static bool      s_valid  = false;
static int16_t   s_tz_min = 480;   /* 默认 UTC+8 */

/* 宽松下界校验：> 2001-09-09，只拦明显垃圾值（0/负溢出），不做回退拒绝 */
#define TS_MIN_UNIX 1000000000UL

/* ---------- 平移通知钩子（弱符号，由 espnow_nfc / card_log 强定义） ----------
 * time_sync 不能反向 include 它们（card_log → time_sync 已有依赖，
 * 反向引用会成环），用 weak hook 解耦。 */
extern void __attribute__((weak)) espnow_nfc_shift_cooloff_hook(int64_t delta_s);
extern void __attribute__((weak)) card_log_shift_hook(int64_t delta_s);

__attribute__((weak)) void espnow_nfc_shift_cooloff_hook(int64_t delta_s) { (void)delta_s; }
__attribute__((weak)) void card_log_shift_hook(int64_t delta_s)           { (void)delta_s; }

void time_sync_init(void)
{
    nvs_handle_t h;
    if (nvs_open(TS_NS, NVS_READONLY, &h) == ESP_OK) {
        size_t len = sizeof(s_base);
        esp_err_t err = nvs_get_blob(h, TS_KEY, &s_base, &len);
        int16_t tz = s_tz_min;
        if (nvs_get_i16(h, TS_KEY_TZ, &tz) == ESP_OK && tz >= -720 && tz <= 840) {
            s_tz_min = tz;
        }
        nvs_close(h);
        if (err != ESP_OK || len != sizeof(s_base)) {
            ESP_LOGW(TAG, "no calibration base, running on internal timer");
        } else if ((uint64_t)esp_rtc_get_time_us() < s_base.rtc_us) {
            /* 断电后内部 RTC 域清零：当前计数 < 基准快照 → 基准失效 */
            ESP_LOGW(TAG, "rtc domain reset, wall base stale");
        } else {
            s_valid = true;
            ESP_LOGI(TAG, "wall clock base: unix=%lu rtc_us=%llu tz=%d",
                     (unsigned long)s_base.unix_s, (unsigned long long)s_base.rtc_us,
                     s_tz_min);
        }
    }

    /* ---- 外置 DS3231（纽扣电池跨断电走时）：开机优先信它 ---- */
    bsp_rtc_init();
    if (bsp_rtc_available()) {
        uint32_t ru = 0;
        int16_t  rtz = 0;
        if (bsp_rtc_get_time(&ru) == ESP_OK) {
            s_base.unix_s  = ru;
            s_base.rtc_us  = (uint64_t)esp_rtc_get_time_us();
            s_valid = true;
            if (bsp_rtc_get_tz_min(&rtz) == ESP_OK) {
                s_tz_min = rtz;
            }
            ESP_LOGI(TAG, "boot: 外置 RTC 墙钟 unix=%lu tz=%dmin",
                     (unsigned long)ru, s_tz_min);
        } else if (s_valid) {
            /* RTC 掉电未同步（OSF）或读写异常：用 NVS 基准反向校时 */
            uint32_t now = time_sync_now();
            if (bsp_rtc_set_time(now) == ESP_OK) {
                bsp_rtc_set_tz_min(s_tz_min);
                ESP_LOGI(TAG, "boot: 外置 RTC 未同步，已用 NVS 基准校时 unix=%lu",
                         (unsigned long)now);
            }
        }
    }
}

uint32_t time_sync_now(void)
{
    uint64_t rtc_us = (uint64_t)esp_rtc_get_time_us();
    if (s_valid && rtc_us >= s_base.rtc_us) {
        uint64_t wall = (uint64_t)s_base.unix_s + (rtc_us - s_base.rtc_us) / 1000000ULL;
        return (uint32_t)wall;
    }
    /* 未校准（或基准失效）：RTC 域相对秒（上电起，断电归零） */
    return (uint32_t)(rtc_us / 1000000ULL);
}

bool time_sync_has_wall(void)
{
    return s_valid;
}

int16_t time_sync_get_tz(void)
{
    return s_tz_min;
}

esp_err_t time_sync_set_tz(int16_t tz_min)
{
    if (tz_min < -720 || tz_min > 840) {
        return ESP_ERR_INVALID_ARG;
    }
    nvs_handle_t h;
    esp_err_t err = nvs_open(TS_NS, NVS_READWRITE, &h);
    if (err != ESP_OK) return err;
    err = nvs_set_i16(h, TS_KEY_TZ, tz_min);
    if (err == ESP_OK) err = nvs_commit(h);
    nvs_close(h);
    if (err == ESP_OK) s_tz_min = tz_min;
    if (err == ESP_OK && bsp_rtc_available()) {
        bsp_rtc_set_tz_min(tz_min);   /* 时区随身：掉电也不丢 */
    }
    return err;
}

/* 无墙钟时借用对端 ESP-NOW 时间戳对齐冷却（每 boot 至多一次） */
static bool s_peer_adopted;

esp_err_t time_sync_adopt_peer(uint32_t unix_s)
{
    if (s_valid || s_peer_adopted) {
        return ESP_OK;
    }
    if (unix_s < TS_MIN_UNIX) {
        return ESP_ERR_INVALID_ARG;
    }
    s_peer_adopted = true;
    ESP_LOGI(TAG, "借用对端时间戳对齐冷却: unix=%lu", (unsigned long)unix_s);
    return time_sync_set_unix(unix_s);
}

/* 本地"日"编号：冷却封禁等按日判定统一走此接口（时区感知） */
uint32_t time_sync_local_day(void)
{
    return (time_sync_now() + (uint32_t)s_tz_min * 60UL) / 86400UL;
}

esp_err_t time_sync_set_unix(uint32_t unix_s)
{
    /* 绝对对齐：仅拦明显垃圾值；允许任意方向调整（含回退） */
    if (unix_s < TS_MIN_UNIX) {
        ESP_LOGW(TAG, "reject unix=%lu out of range", (unsigned long)unix_s);
        return ESP_ERR_INVALID_ARG;
    }

    /* 与旧墙钟的偏差 → 通知平移存量记录（仅此前已有有效墙钟时才有意义；
     * 未校准状态下旧"时间"是上电相对秒，无平移语义） */
    bool had_wall = s_valid;
    int64_t delta = 0;
    if (had_wall) {
        delta = (int64_t)unix_s - (int64_t)time_sync_now();
    }

    ts_base_t b;
    b.unix_s = unix_s;
    b.rtc_us = (uint64_t)esp_rtc_get_time_us();

    nvs_handle_t h;
    esp_err_t err = nvs_open(TS_NS, NVS_READWRITE, &h);
    if (err != ESP_OK) {
        return err;
    }
    err = nvs_set_blob(h, TS_KEY, &b, sizeof(b));
    if (err == ESP_OK) {
        err = nvs_commit(h);
    }
    nvs_close(h);
    if (err != ESP_OK) {
        ESP_LOGE(TAG, "persist base failed: %s", esp_err_to_name(err));
        return err;
    }

    s_base = b;
    s_valid = true;
    ESP_LOGI(TAG, "wall clock synced: unix=%lu (delta=%lld)",
             (unsigned long)unix_s, (long long)delta);

    if (had_wall && delta != 0) {
        espnow_nfc_shift_cooloff_hook(delta);
        card_log_shift_hook(delta);
    }
    if (bsp_rtc_available()) {
        bsp_rtc_set_time(unix_s);   /* 同步写外置 RTC（电池域，跨断电保时） */
        bsp_rtc_set_tz_min(s_tz_min);
    }
    return ESP_OK;
}
