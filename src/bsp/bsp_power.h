/* ============================================================================
 * bsp_power.h - 电源监测（USB_DET + VBAT_SENSE ADC）接口
 *
 * 硬件（PCB v6.2，引脚见 pins_pass.h）：
 *   USB_DET   IO1  1M/1M 分压（插 USB ≈2.5V → 读 1；禁止内部上下拉）
 *   VBAT_SENSE IO2 电池 1/2 分压（ADC1_CH1，1% 精度）
 *
 * 行为约定：
 *   - 每次采样 ≥8 次取中值；分压网络缺失/读数不可信时 hw_valid=false，
 *     不显示电量、不触发休眠（开发板/裸板安全）。
 *   - USB 供电：显示充电状态（充电中… / 即将充满），不报低电。
 *   - 电池供电：≤3400mV 显示"电量低 x%"警告；≤3200mV 显示"电量耗尽
 *     即将休眠"，并立即拉低 storage_ok（card_log 停止写卡），连续 3 次
 *     确认后进入 deep sleep（SW5 硬开关无唤醒，重新上电恢复）。
 *
 * 状态文本经显示回调交给 UI（ui_core_screen_set_power）；主程序用
 * bsp_power_set_display_cb 注册胶水。级别：0=正常 1=警告 2=危急。
 * ==========================================================================*/
#ifndef BSP_POWER_H
#define BSP_POWER_H

#include <stdbool.h>
#include <stdint.h>
#include "esp_err.h"

/* 初始化 USB_DET/RTC 备用脚 GPIO 与 VBAT ADC（含校准，失败退化线性换算）；
 * 内部先做一次采样。任何任务可调一次。 */
esp_err_t bsp_power_init(void);

/* 启动 1s 周期监测任务（core0 低优先级；内部下采样防抖/中值滤波） */
void bsp_power_start_monitor(void);

/* true = USB 5V 输入被检测到 */
bool bsp_power_usb_det(void);

/* 电池电压 mV（分压×2；硬件无效时为 0） */
uint32_t bsp_power_vbat_mv(void);

/* 电量 0~100（硬件无效时为 -1） */
int bsp_power_soc_pct(void);

/* 分压网络存在且读数可信（范围 + 噪声门限） */
bool bsp_power_hw_valid(void);

/* false = 危急低电：业务层应停止写 SD（card_log 门禁）；恢复供电/USB 后回升 */
bool bsp_power_storage_ok(void);

/* 显示回调：text=NULL 隐藏；level 0=绿 1=黄 2=红。
 * 回调运行在监测任务上下文，实现方自行加 LVGL 锁。 */
void bsp_power_set_display_cb(void (*cb)(const char *text, int level));

#endif
