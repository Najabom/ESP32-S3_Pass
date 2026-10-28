/* ============================================================================
 * ui_boot_anim.h - 开机终端画面（LVGL 版）
 *
 * 设计：LVGL 就绪后创建"终端式"全屏画面——左上角标题 + 内核日志滚动区
 *       + 底部 TUI 风格进度条。日志经 esp_log_set_vprintf 钩子无过滤捕获，
 *       40ms 定时器快速消费（滚动流畅）。intro 就绪后 hide() 隐藏画面、
 *       全局日志降到 WARN（周期/过程性 INFO 静默，W/E 保留）。
 *
 * 线程约定（与 ui_core_screen 相同）：
 *   - attach_log()    app_main 首行调用（挂 vprintf 钩子，无 LVGL 依赖）
 *   - init()          必须在 bsp_lvgl_init 之后（内部持 bsp_lvgl_lock）
 *   - progress()      任意线程（仅存值，volatile 标志，LVGL 定时器消费）
 *   - is_done()       任意线程（只读）
 *   - hide()          播放器线程（内部持锁；幂等）
 * ==========================================================================*/
#ifndef UI_BOOT_ANIM_H
#define UI_BOOT_ANIM_H

#include <stdint.h>
#include <stdbool.h>

void ui_boot_anim_attach_log(void);                 /* app_main 首行：挂日志钩子 */
void ui_boot_anim_init(void);                       /* LVGL 就绪后：建画面（幂等） */
void ui_boot_anim_progress(uint32_t done, uint32_t total);  /* 读卡进度 */
bool ui_boot_anim_is_done(void);                    /* 固定时长已到（或已隐藏） */
void ui_boot_anim_hide(void);                       /* 隐藏画面 + 日志降 WARN */

#endif /* UI_BOOT_ANIM_H */
