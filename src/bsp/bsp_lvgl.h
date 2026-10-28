/* ============================================================================
 * bsp_lvgl.h - 屏幕与 LVGL 移植层接口
 *
 *   bsp_lcd_early_init()    仅屏幕硬件初始化（SPI2+ST7789+背光，无 LVGL）
 *   bsp_lcd_panel_get()     面板句柄（LVGL flush 使用）
 *   bsp_lvgl_init()         初始化 LVGL+显示驱动（LCD 已就绪则自动复用）
 *   bsp_lvgl_start_task()   启动 LVGL 调度任务（之后才可安全渲染）
 *   bsp_lvgl_lock()/unlock() 非 LVGL 线程操作 UI 前必须加锁（成对使用）
 * ==========================================================================*/

// bsp/bsp_lvgl.h
#ifndef BSP_LVGL_H
#define BSP_LVGL_H

#include "lvgl.h"
#include "esp_lcd_panel_ops.h"

/* 仅硬件：SPI2 + ST7789 + 背光（无 LVGL 依赖；背光由首帧 flush 点亮） */
void bsp_lcd_early_init(void);
/* 背光亮度 0~100（PWM 调光；任何任务可调，串口 bl 命令使用） */
void bsp_lcd_backlight(uint8_t percent);
/* 共享面板句柄（未初始化返回 NULL） */
esp_lcd_panel_handle_t bsp_lcd_panel_get(void);
/* 初始化 ST7789 屏 + LVGL 绘制缓冲与显示驱动（仅调一次） */
void bsp_lvgl_init(void);
/* 启动 core1 上的 LVGL 调度任务（init 之后调用一次） */
void bsp_lvgl_start_task(void);
/* LVGL 互斥锁：其他线程碰 UI 对象前 lock，完成后 unlock */
void bsp_lvgl_lock(void);
void bsp_lvgl_unlock(void);

#endif