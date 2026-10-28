/* ============================================================================
 * pins_pass.h - PASS-II 全部 GPIO 引脚定义（唯一来源）
 *
 * 依据：PCB 实机 v6.2 原理图与《08 固件调整指示》。
 *       旧的《02 接线图》（开发杜邦线版）已作废。
 * 说明：LCD / SD / 电源监测引脚统一在此定义；改版只动本文件。
 *       触摸（bsp_touch.h）与 RTC I2C（bsp_rtc.c）引脚未变，仍各留原处。
 * 变更记录（v6.2）：
 *   - LCD: SCK 14→11、MOSI 13→14、CS 10→13、RST 11→10、BLK 15→9
 *   - SD : CS 4→7、MOSI 5→6、MISO 6→4、SCK 7→5
 *   - 新增：USB_DET(IO1)、VBAT_SENSE(IO2/ADC1_CH1)
 * ==========================================================================*/
#ifndef PINS_PASS_H
#define PINS_PASS_H

/* ---------- ST7789 屏幕（SPI2） ---------- */
#define PIN_NUM_LCD_PIXEL_CLOCK 11   // LCD_SCL（旧 14）
#define PIN_NUM_LCD_DATA0       14   // LCD_SDA（旧 13）
#define PIN_NUM_LCD_CS          13   // 片选（旧 10）
#define PIN_NUM_LCD_DC          12   // 数据/命令（不变）
#define PIN_NUM_LCD_RST         10   // 复位（旧 11）
#define PIN_NUM_LCD_BK_LIGHT    9    // 背光 PWM/LEDC（旧 15；IO15 已空出）

/* ---------- microSD（SPI3） ---------- */
#define PIN_NUM_SD_CS    7    // SD_CS（旧 4）
#define PIN_NUM_SD_MOSI  6    // SD_MOSI（旧 5）
#define PIN_NUM_SD_MISO  4    // SD_MISO（旧 6）
#define PIN_NUM_SD_SCK   5    // SD_SCK（旧 7）

/* ---------- 电源监测（PCB v6.2 新增） ----------
 * USB_DET：R21/R22 1M/1M 硬分压，插 USB 时 IO1≈2.5V 读 1，拔出≈0V 读 0；
 *          禁止内部上下拉（高阻分压会被扭曲）。
 * VBAT_SENSE：电池 1M/1M 分压（1/2）+ C25 100nF，ADC1_CH1（11dB 衰减）。 */
#define PIN_NUM_USB_DET    1
#define PIN_NUM_VBAT_SENSE 2

/* ---------- DS3231 辅助引脚（可选启用；不使用时输入+下拉防耦合） ---------- */
#define PIN_NUM_RTC_32K 16   // RTC 慢速时钟输出（开漏）
#define PIN_NUM_RTC_SQW 17   // 1Hz 心跳/中断（开漏）

#endif
