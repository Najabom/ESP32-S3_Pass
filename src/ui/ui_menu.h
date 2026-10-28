/* ============================================================================
 * ui_menu.h - 触控菜单（底部抽屉 + 全屏子页）
 *
 *   ui_menu_init()             创建菜单层（ui_core_screen_init 之后调用一次）
 *   ui_menu_is_open()          菜单是否打开（ui_core_screen 抑制横幅/提示用）
 *   ui_menu_notify_ble_state() BLE 连接/广播状态变化通知（BLE 线程调用）
 *
 * 交互：点击三次打开居中半透明黑透面板（4 个入口：角色选择 / 名片簿 /
 *   蓝牙 / 系统信息）；点按遮罩关闭；子页带页头"返回"。所有页面构建
 *   都在 LVGL 任务上下文（lv_timer 事件）执行，天然持 LVGL 锁。
 * ==========================================================================*/

#ifndef UI_MENU_H
#define UI_MENU_H

#include <stdbool.h>

#ifdef __cplusplus
extern "C" {
#endif

/* 创建菜单层（须在 bsp_lvgl_start_task 与 ui_core_screen_init 之后） */
void ui_menu_init(void);

/* 菜单当前是否打开（ui_core_screen.c 在 show_banner/set_hint 前查，
 * 菜单打开时抑制弹层，避免 move_foreground 盖住抽屉） */
bool ui_menu_is_open(void);

/* 蓝牙状态变化通知（BLE 线程/任意线程可调，内部持 LVGL 锁）：
 * 蓝牙页可见时刷新状态文本 */
void ui_menu_notify_ble_state(void);

/* 校准引导拉起（播放器在 loop 首帧发布、UI 揭示完成后调用）：
 * 未校准或引导未完成过时弹出四角点击引导；幂等 */
void ui_menu_maybe_show_cal_guide(void);

#ifdef __cplusplus
}
#endif

#endif /* UI_MENU_H */
