/* ============================================================================
 * bsp_rtc.h - 外置实时钟 DS3231SN（I2C）
 *
 * 接线：SDA=GPIO8  SCL=GPIO18  32K=GPIO16（EN32KX 默认关，引脚高阻）  SQW=GPIO17（INTCN=1 无使能，高阻）
 * 地址 0x68。时间以 UTC 存寄存器 00h-06h（BCD），时区偏移（分钟，/15
 * 量化）借用闹钟1 秒寄存器 07h（A1IE=0 时该寄存器为纯后备 RAM）。
 * 纽扣电池保持：设备断电后继续走时，开机可直读墙钟。
 * ==========================================================================*/

#ifndef BSP_RTC_H
#define BSP_RTC_H

#include <stdbool.h>
#include <stdint.h>
#include "esp_err.h"

#ifdef __cplusplus
extern "C" {
#endif

/* 建立 I2C 总线并探测器件。探测失败不视为错误（RTC 可不装）。 */
esp_err_t bsp_rtc_init(void);

/* 器件在位且此前通信正常 */
bool bsp_rtc_available(void);

/* 读 UTC 墙钟秒。器件缺失/OSF（掉电标志未清）→ ESP_ERR_NOT_FOUND/INVALID_STATE */
esp_err_t bsp_rtc_get_time(uint32_t *unix_utc);

/* 写 UTC 墙钟秒（含清 OSF / 重算星期） */
esp_err_t bsp_rtc_set_time(uint32_t unix_utc);

/* 时区偏移（分钟）借用老化寄存器存取（/15 量化，范围 ±12h..+14h） */
esp_err_t bsp_rtc_set_tz_min(int16_t tz_min);
esp_err_t bsp_rtc_get_tz_min(int16_t *tz_min);

#ifdef __cplusplus
}
#endif

#endif /* BSP_RTC_H */
