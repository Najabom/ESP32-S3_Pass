/* ============================================================================
 * bsp_rtc.c - 外置实时钟 DS3231SN 驱动（I2C，寄存器级）
 *
 * 见 bsp_rtc.h。接口约定：所有读写在无器件时安全返回错误，不崩溃。
 * 时间全部按 UTC 存取（时区只做量化借用寄存器，不参与计时）。
 * ==========================================================================*/

// bsp/bsp_rtc.c
#include "bsp_rtc.h"
#include "esp_log.h"
#include "driver/i2c_master.h"
#include <time.h>
#include <string.h>

static const char *TAG = "BSP_RTC";

#define RTC_I2C_PORT   I2C_NUM_0
#define RTC_SDA_PIN    8
#define RTC_SCL_PIN    18
#define RTC_I2C_ADDR   0x68
#define RTC_SPEED_HZ   100000

/* DS3231 寄存器（00h-06h 时间 BCD；0Eh 控制；0Fh 状态） */
#define R_SEC    0x00
#define R_TZ     0x07   /* 借用闹钟1秒：A1IE=0 时为后备 RAM */
#define R_CENT   0x05   /* bit7 世纪 */
#define R_CTRL   0x0E
#define R_STAT   0x0F
#define S_OSF    0x80   /* 振荡器停振/掉电标志 */

static i2c_master_bus_handle_t s_bus;
static i2c_master_dev_handle_t s_dev;
static bool s_avail;

static esp_err_t reg_read(uint8_t reg, uint8_t *buf, size_t len)
{
    return i2c_master_transmit_receive(s_dev, &reg, 1, buf, len, 200);
}

static esp_err_t reg_write(uint8_t reg, const uint8_t *buf, size_t len)
{
    uint8_t tmp[16];
    if (len + 1 > sizeof(tmp)) return ESP_ERR_INVALID_SIZE;
    tmp[0] = reg;
    memcpy(tmp + 1, buf, len);
    return i2c_master_transmit(s_dev, tmp, len + 1, 200);
}

static uint8_t bcd2bin(uint8_t v) { return (uint8_t)((v >> 4) * 10 + (v & 0x0F)); }
static uint8_t bin2bcd(uint8_t v) { return (uint8_t)((v / 10) << 4 | (v % 10)); }

esp_err_t bsp_rtc_init(void)
{
    if (s_avail) return ESP_OK;
    i2c_master_bus_config_t bc;
    memset(&bc, 0, sizeof(bc));
    bc.i2c_port = RTC_I2C_PORT;
    bc.sda_io_num = RTC_SDA_PIN;
    bc.scl_io_num = RTC_SCL_PIN;
    bc.clk_source = I2C_CLK_SRC_DEFAULT;
    bc.glitch_ignore_cnt = 7;
    bc.flags.enable_internal_pullup = true;
    esp_err_t err = i2c_new_master_bus(&bc, &s_bus);
    if (err != ESP_OK) {
        ESP_LOGW(TAG, "I2C 总线建立失败: %s", esp_err_to_name(err));
        return err;
    }
    i2c_device_config_t dc;
    memset(&dc, 0, sizeof(dc));
    dc.dev_addr_length = I2C_ADDR_BIT_LEN_7;
    dc.device_address = RTC_I2C_ADDR;
    dc.scl_speed_hz = RTC_SPEED_HZ;
    err = i2c_master_bus_add_device(s_bus, &dc, &s_dev);
    if (err != ESP_OK) {
        ESP_LOGW(TAG, "添加 DS3231 设备失败: %s", esp_err_to_name(err));
        return err;
    }
    if (i2c_master_probe(s_bus, RTC_I2C_ADDR, 200) != ESP_OK) {
        ESP_LOGW(TAG, "DS3231 未应答（0x%02X，SDA=%d SCL=%d）", RTC_I2C_ADDR,
                 RTC_SDA_PIN, RTC_SCL_PIN);
        return ESP_ERR_NOT_FOUND;
    }
    /* 控制寄存器：~EOSC=0 振荡器运行、INTCN=1（SQW 引脚作中断且 A1/A2
     * 未使能→高阻，防 1Hz 方波占用 GPIO17）、其余关。0x1C。 */
    uint8_t ctrl = 0x1C;
    reg_write(R_CTRL, &ctrl, 1);
    s_avail = true;
    uint8_t st = 0;
    reg_read(R_STAT, &st, 1);
    ESP_LOGI(TAG, "DS3231 就绪，status=0x%02X %s", st,
             (st & S_OSF) ? "（掉电标志置位：时间不可信）" : "");
    return ESP_OK;
}

bool bsp_rtc_available(void) { return s_avail; }

esp_err_t bsp_rtc_get_time(uint32_t *unix_utc)
{
    if (!s_avail || unix_utc == NULL) return ESP_ERR_INVALID_STATE;
    uint8_t st;
    if (reg_read(R_STAT, &st, 1) != ESP_OK) return ESP_ERR_INVALID_STATE;
    if (st & S_OSF) return ESP_ERR_INVALID_STATE;   /* 掉电后未重写：不可信 */
    uint8_t t[7];
    if (reg_read(R_SEC, t, 7) != ESP_OK) return ESP_ERR_INVALID_STATE;
    if ((t[2] & 0x40) != 0) return ESP_ERR_INVALID_STATE;   /* 非 24h 模式 */
    struct tm tmv;
    memset(&tmv, 0, sizeof(tmv));
    tmv.tm_sec  = bcd2bin(t[0] & 0x7F);
    tmv.tm_min  = bcd2bin(t[1] & 0x7F);
    tmv.tm_hour = bcd2bin(t[2] & 0x3F);
    tmv.tm_mday = bcd2bin(t[4] & 0x3F);
    tmv.tm_mon  = bcd2bin(t[5] & 0x1F) - 1;
    tmv.tm_year = bcd2bin(t[6]) + ((t[5] & 0x80) ? 100 : 0) + 100; /* 2000 基 */
    time_t w = timegm(&tmv);
    if (w < 946684800) return ESP_ERR_INVALID_STATE;         /* < 2000 视为坏 */
    *unix_utc = (uint32_t)w;
    return ESP_OK;
}

esp_err_t bsp_rtc_set_time(uint32_t unix_utc)
{
    if (!s_avail) return ESP_ERR_INVALID_STATE;
    time_t w = (time_t)unix_utc;
    struct tm tmv;
    gmtime_r(&w, &tmv);
    uint8_t t[7];
    t[0] = bin2bcd((uint8_t)tmv.tm_sec);
    t[1] = bin2bcd((uint8_t)tmv.tm_min);
    t[2] = bin2bcd((uint8_t)tmv.tm_hour);            /* 24h 模式 bit6=0 */
    t[3] = bin2bcd((uint8_t)((tmv.tm_wday == 0) ? 7 : tmv.tm_wday)); /* 周日=1 */
    t[4] = bin2bcd((uint8_t)tmv.tm_mday);
    int full_year = tmv.tm_year + 1900;
    t[5] = (uint8_t)(bin2bcd((uint8_t)(tmv.tm_mon + 1)) |
                     (full_year >= 2100 ? 0x80 : 0));
    t[6] = bin2bcd((uint8_t)(full_year % 100));
    if (full_year < 2000 || full_year > 2199) return ESP_ERR_INVALID_ARG;
    esp_err_t err = reg_write(R_SEC, t, 7);
    if (err != ESP_OK) {
        ESP_LOGW(TAG, "写时间失败: %s", esp_err_to_name(err));
        return err;
    }
    /* 清 OSF：置 0 表示时间已有效 */
    uint8_t st = 0;
    if (reg_read(R_STAT, &st, 1) == ESP_OK) {
        st &= (uint8_t)~S_OSF;
        reg_write(R_STAT, &st, 1);
    }
    return ESP_OK;
}

esp_err_t bsp_rtc_set_tz_min(int16_t tz_min)
{
    if (!s_avail) return ESP_ERR_INVALID_STATE;
    if (tz_min < -720 || tz_min > 840) return ESP_ERR_INVALID_ARG;
    int16_t q = tz_min / 15;   /* /15 量化：覆盖全部已用时区偏移 */
    uint8_t v = (uint8_t)(int8_t)q;
    return reg_write(R_TZ, &v, 1);
}

esp_err_t bsp_rtc_get_tz_min(int16_t *tz_min)
{
    if (!s_avail || tz_min == NULL) return ESP_ERR_INVALID_STATE;
    uint8_t v = 0;
    esp_err_t err = reg_read(R_TZ, &v, 1);
    if (err != ESP_OK) return err;
    int8_t q = (int8_t)v;
    if (q < -48 || q > 56) return ESP_ERR_INVALID_STATE;   /* 超出时区量化域 */
    *tz_min = (int16_t)(q * 15);
    return ESP_OK;
}
