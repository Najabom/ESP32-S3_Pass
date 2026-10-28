/* ============================================================================
 * bsp_touch.h - XPT2046 电阻触摸屏（GPIO 模拟 SPI）接口
 *
 * 引脚（与屏幕共用 PCB；原为独立 SPI3 总线，SPI3 已让给 SD 卡，现改
 *   GPIO bit-bang——触摸是低速轮询设备，模拟后性能无感知差异）：
 *   T_CLK 42 / T_CS 41 / T_DIN(MOSI) 40 / T_DO(MISO) 39 / T_IRQ 38
 * 可调整内容：仅接线变更时改下方宏（需同步重接线）。IRQ 是触摸中断
 *   指示脚，读回调轮询其电平判断按下/抬起。 */
#ifndef BSP_TOUCH_H
#define BSP_TOUCH_H

#include "lvgl.h"

#define PIN_NUM_TOUCH_CLK   42   // T_CLK
#define PIN_NUM_TOUCH_CS    41   // T_CS
#define PIN_NUM_TOUCH_DIN   40   // T_DIN (MOSI)
#define PIN_NUM_TOUCH_DO    39   // T_DO  (MISO)
#define PIN_NUM_TOUCH_IRQ   38   // T_IRQ

/* 初始化触控总线与 LVGL 指针输入设备（LVGL 任务启动前调用） */
void bsp_touch_init(void);

/* 返回已注册的 LVGL 指针输入设备句柄（手势检测等轮询用；未初始化返回 NULL） */
lv_indev_t *bsp_touch_get_indev(void);

/* 直接读 IRQ 电平判断当前是否按压（不依赖 LVGL indev 采样；
 * 供上滑手势等低频手势检测使用） */
bool bsp_touch_pressed(void);

/* 读取当前触点并映射到屏幕坐标（与 LVGL indev 采样相同的固定映射；
 * 调用方须保证当前处于按压状态（IRQ 低），返回 false 表示未按下。
 * 供上滑手势检测取起点/实时位移用，不经过 LVGL indev。 */
bool bsp_touch_point(int16_t *x, int16_t *y);

/* ---- 触控自动校准（raw 范围自适应 + NVS 持久化） ----
 * 校准流程：按压即采样 raw 值并扩张记录范围；跨度达到阈值（X≥2400 /
 * Y≥3000 raw 单位）即视为有效并自动保存。UI 可用 bsp_touch_cal_valid()
 * 判断是否已校准（未校准时显示引导层提示用户点击四角），用
 * bsp_touch_cal_taps() 显示本次按压次数。bsp_touch_cal_reset() 清空
 * 校准并擦除 NVS 记录（重新校准入口）。 */
bool bsp_touch_cal_valid(void);
int  bsp_touch_cal_taps(void);
void bsp_touch_cal_reset_taps(void);
void bsp_touch_cal_reset(void);

/* 校准引导"已完成一次"标记（NVS）：未标记时开机弹出四角引导，
 * 引导完成自动落盘；之后不再打扰。 */
bool bsp_touch_cal_guided(void);
void bsp_touch_cal_mark_guided(void);

#endif