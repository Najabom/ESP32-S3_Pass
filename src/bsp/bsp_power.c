// bsp/bsp_power.c
#include "bsp_power.h"
#include "bsp_lvgl.h"
#include "pins_pass.h"
#include "driver/gpio.h"
#include "esp_adc/adc_oneshot.h"
#include "esp_adc/adc_cali.h"
#include "esp_adc/adc_cali_scheme.h"
#include "esp_check.h"
#include "esp_log.h"
#include "esp_sleep.h"
#include "freertos/FreeRTOS.h"
#include "freertos/task.h"

#include <stdio.h>
#include <string.h>

static const char *TAG = "BSP_POWER";

#define VBAT_ADC_UNIT      ADC_UNIT_1
#define VBAT_ADC_CHANNEL   ADC_CHANNEL_1      /* GPIO2 = ADC1_CH1 */
#define VBAT_DIV_MUL       2                  /* 1/2 分压 → 电池 = 引脚 ×2 */
#define VBAT_SAMPLE_N      9                  /* 中值滤波取样数（≥8） */
#define VBAT_VALID_MIN_MV  2500               /* 低于此值/高于下值 = 分压网络缺失 */
#define VBAT_VALID_MAX_MV  4600
#define VBAT_SPREAD_MAX    160                /* 9 次 raw 极差门限（≈120mV 电池） */
#define VBAT_CRIT_MV       3200               /* 危急：停写卡 → 连续确认 → 休眠 */
#define VBAT_LOW_MV        3400               /* 低电警告 */
#define CHG_FULL_MV        4300               /* USB 下 >4.3V ≈ 充电接近完成 */
#define CRIT_CONFIRM_N     3                  /* 连续次数确认才休眠（防毛刺） */
#define MON_TICK_MS        1000

static adc_oneshot_unit_handle_t s_adc = NULL;
static adc_cali_handle_t s_cali = NULL;
static bool s_cali_ok = false;

static bool s_usb = false;
static uint32_t s_vbat_mv = 0;
static int s_soc = -1;
static bool s_hw_valid = false;
static bool s_storage_ok = true;
static int s_crit_cnt = 0;
static int s_dots = 0;

static void (*s_disp_cb)(const char *text, int level) = NULL;

/* ---------- 电量表：mV → %（线性插值，SOC 表来自电池实测） ---------- */
typedef struct { uint16_t mv; uint8_t pct; } soc_pt_t;
static const soc_pt_t SOC_TAB[] = {
    {4150, 100}, {4050, 90}, {3950, 80}, {3870, 70}, {3830, 60}, {3800, 50},
    {3770, 40},  {3740, 30}, {3700, 20}, {3600, 10}, {3450, 5},  {3300, 0},
};
#define SOC_TAB_N (sizeof(SOC_TAB) / sizeof(SOC_TAB[0]))

static int soc_from_mv(uint32_t mv)
{
    if (mv >= SOC_TAB[0].mv) return 100;
    for (size_t i = 0; i + 1 < SOC_TAB_N; i++) {
        /* 表按电压降序：找到 mv 所在区段 [lo, hi] 后线性插值 */
        if (mv > SOC_TAB[i + 1].mv) {
            uint32_t hi = SOC_TAB[i].mv, lo = SOC_TAB[i + 1].mv;
            int ph = SOC_TAB[i].pct, pl = SOC_TAB[i + 1].pct;
            return pl + (int)((uint32_t)(mv - lo) * (ph - pl) / (hi - lo));
        }
    }
    return 0;
}

/* ---------- 单次采样：9 次中值 + 极差门限 ----------
 * 返回 false = ADC 通道读取失败；中值写回 *out_raw，极差写回 *out_spread */
static bool sample_vbat(int *out_raw, int *out_spread)
{
    int raws[VBAT_SAMPLE_N];
    int mn = 1 << 12, mx = -1;
    for (int i = 0; i < VBAT_SAMPLE_N; i++) {
        int r = 0;
        if (adc_oneshot_read(s_adc, VBAT_ADC_CHANNEL, &r) != ESP_OK) return false;
        raws[i] = r;
        if (r < mn) mn = r;
        if (r > mx) mx = r;
    }
    *out_spread = mx - mn;
    /* 插入排序取中值（9 个元素，代价可忽略） */
    for (int i = 1; i < VBAT_SAMPLE_N; i++) {
        int key = raws[i], j = i - 1;
        while (j >= 0 && raws[j] > key) { raws[j + 1] = raws[j]; j--; }
        raws[j + 1] = key;
    }
    *out_raw = raws[VBAT_SAMPLE_N / 2];
    return true;
}

static void refresh_vbat(void)
{
    int raw = 0, spread = 0;
    if (!sample_vbat(&raw, &spread)) {
        s_hw_valid = false;
        s_vbat_mv = 0;
        return;
    }
    int pin_mv = 0;
    if (s_cali_ok) {
        adc_cali_raw_to_voltage(s_cali, raw, &pin_mv);
    } else {
        pin_mv = (int)((int64_t)raw * 3300 / 4095);   /* 无校准线性兜底 */
    }
    uint32_t vbat = (uint32_t)pin_mv * VBAT_DIV_MUL;
    s_hw_valid = (spread <= VBAT_SPREAD_MAX) &&
                 (vbat >= VBAT_VALID_MIN_MV) && (vbat <= VBAT_VALID_MAX_MV);
    s_vbat_mv = s_hw_valid ? vbat : 0;
}

/* ---------- 监测任务：采样 → 状态判定 → 显示回调 ---------- */
static void power_monitor_task(void *arg)
{
    char old_text[48] = "";
    const char *DOTS[4] = {"", "·", "··", "···"};

    for (;;) {
        s_usb = (gpio_get_level(PIN_NUM_USB_DET) != 0);
        refresh_vbat();
        s_soc = s_hw_valid ? soc_from_mv(s_vbat_mv) : -1;

        const char *text = NULL;
        char buf[48];
        int level = 0;

        if (s_usb) {
            /* USB 供电：充电状态指示（不报低电） */
            s_crit_cnt = 0;
            s_storage_ok = true;
            if (s_hw_valid && s_vbat_mv > CHG_FULL_MV) {
                snprintf(buf, sizeof(buf), "即将充满");
            } else {
                s_dots = (s_dots + 1) & 3;             /* 点动画 1Hz */
                snprintf(buf, sizeof(buf), "充电中%s", DOTS[s_dots]);
            }
            text = buf;
            level = 0;
        } else if (s_hw_valid && s_vbat_mv <= VBAT_CRIT_MV) {
            /* 危急低电：停写卡；连续 3 次确认 → deep sleep（SW5 硬开关，重上电恢复） */
            s_storage_ok = false;
            if (++s_crit_cnt >= CRIT_CONFIRM_N) {
                ESP_LOGE(TAG, "电量耗尽（%lumV），停止写卡并进入深度休眠", (unsigned long)s_vbat_mv);
                if (s_disp_cb) s_disp_cb("电量耗尽 即将休眠", 2);
                vTaskDelay(pdMS_TO_TICKS(300));       /* 留一帧给屏幕 */
                bsp_lcd_backlight(0);                 /* 休眠前关背光，避免继续耗电 */
                vTaskDelay(pdMS_TO_TICKS(50));
                esp_deep_sleep_start();
            }
            snprintf(buf, sizeof(buf), "电量耗尽 即将休眠");
            text = buf;
            level = 2;
        } else if (s_hw_valid && s_vbat_mv <= VBAT_LOW_MV) {
            s_crit_cnt = 0;
            s_storage_ok = true;
            snprintf(buf, sizeof(buf), "电量低 %d%%", s_soc);
            text = buf;
            level = 1;
        } else {
            s_crit_cnt = 0;
            s_storage_ok = true;
            text = NULL;                              /* 正常电池供电/硬件无效：不显示 */
        }

        /* 文本变化才回调（USB 充电点动画每 tick 变化 → 自动刷新） */
        bool need = false;
        if (!text) {
            if (old_text[0]) { old_text[0] = '\0'; need = true; }
        } else if (strcmp(text, old_text) != 0) {
            strlcpy(old_text, text, sizeof(old_text));
            need = true;
        }
        if (need && s_disp_cb) s_disp_cb(text, level);

        vTaskDelay(pdMS_TO_TICKS(MON_TICK_MS));
    }
}

/* ---------- 初始化 ---------- */
esp_err_t bsp_power_init(void)
{
    /* USB_DET：输入，禁止内部上下拉（外部 1M/1M 分压，内部拉会破坏分压比）；
     * RTC 32K/SQW 未启用时输入+下拉，防悬空耦合干扰。 */
    gpio_config_t usb_conf = {
        .pin_bit_mask = (1ULL << PIN_NUM_USB_DET),
        .mode         = GPIO_MODE_INPUT,
        .pull_up_en   = GPIO_PULLUP_DISABLE,
        .pull_down_en = GPIO_PULLDOWN_DISABLE,
        .intr_type    = GPIO_INTR_DISABLE,
    };
    ESP_RETURN_ON_ERROR(gpio_config(&usb_conf), TAG, "USB_DET gpio");

    gpio_config_t rtc_aux_conf = {
        .pin_bit_mask = (1ULL << PIN_NUM_RTC_32K) | (1ULL << PIN_NUM_RTC_SQW),
        .mode         = GPIO_MODE_INPUT,
        .pull_up_en   = GPIO_PULLUP_DISABLE,
        .pull_down_en = GPIO_PULLDOWN_ENABLE,
        .intr_type    = GPIO_INTR_DISABLE,
    };
    ESP_RETURN_ON_ERROR(gpio_config(&rtc_aux_conf), TAG, "RTC aux gpio");

    adc_oneshot_unit_init_cfg_t unit_cfg = { .unit_id = VBAT_ADC_UNIT };
    ESP_RETURN_ON_ERROR(adc_oneshot_new_unit(&unit_cfg, &s_adc), TAG, "adc unit");

    adc_oneshot_chan_cfg_t chan_cfg = {
        .atten    = ADC_ATTEN_DB_12,
        .bitwidth = ADC_BITWIDTH_DEFAULT,
    };
    ESP_RETURN_ON_ERROR(adc_oneshot_config_channel(s_adc, VBAT_ADC_CHANNEL, &chan_cfg),
                        TAG, "adc channel");

    adc_cali_curve_fitting_config_t cal_cfg = {
        .unit_id  = VBAT_ADC_UNIT,
        .chan     = VBAT_ADC_CHANNEL,
        .atten    = ADC_ATTEN_DB_12,
        .bitwidth = ADC_BITWIDTH_DEFAULT,
    };
    s_cali_ok = (adc_cali_create_scheme_curve_fitting(&cal_cfg, &s_cali) == ESP_OK);
    if (!s_cali_ok) {
        ESP_LOGW(TAG, "ADC 校准不可用，使用线性换算兜底");
    }

    /* 先做一次采样，尽早让状态可用 */
    s_usb = (gpio_get_level(PIN_NUM_USB_DET) != 0);
    refresh_vbat();
    s_soc = s_hw_valid ? soc_from_mv(s_vbat_mv) : -1;
    ESP_LOGI(TAG, "init: USB_DET=IO%d VBAT=IO%d cali=%d usb=%d hw=%d vbat=%lumV soc=%d",
             PIN_NUM_USB_DET, PIN_NUM_VBAT_SENSE, (int)s_cali_ok, (int)s_usb,
             (int)s_hw_valid, (unsigned long)s_vbat_mv, s_soc);
    return ESP_OK;
}

void bsp_power_start_monitor(void)
{
    xTaskCreatePinnedToCore(power_monitor_task, "pwr_mon", 3072, NULL, 2, NULL, 0);
}

bool bsp_power_usb_det(void)     { return s_usb; }
uint32_t bsp_power_vbat_mv(void) { return s_vbat_mv; }
int bsp_power_soc_pct(void)      { return s_soc; }
bool bsp_power_hw_valid(void)    { return s_hw_valid; }
bool bsp_power_storage_ok(void)  { return s_storage_ok; }

void bsp_power_set_display_cb(void (*cb)(const char *text, int level))
{
    s_disp_cb = cb;
}
