/* ============================================================================
 * ui_core_screen.h - 主界面 + 名片横幅接口
 *
 *   ui_core_screen_init(role_name_text)   创建主界面（一次）
 *   ui_core_screen_set_role(name)   更新底部面板角色名（NULL/空串 = 隐藏，
 *                                       不显示占位，可反复调用，内部有 LVGL 锁）
 *   ui_core_screen_intro_done(...)      intro 播完后揭示 leftcol/footer（一次，
 *                                        leftcol 浮入 + role_name/sub_text 闪动链）
 *   ui_core_screen_show_banner(...)     收到名片时弹横幅（可反复调用，
 *                                       再次调用会重置 5 秒倒计时）
 *   ui_core_screen_set_hint(text)       屏幕中央状态提示（"请插入SD卡"等）
 *   ui_core_screen_hide_ui()            隐藏全部 UI 元素（拔卡/热插 intro 前）
 * ==========================================================================*/

// ui/ui_core_screen.h
#ifndef UI_CORE_SCREEN_H
#define UI_CORE_SCREEN_H

#include <stdint.h>
#include "lvgl.h"

/* 初始化主界面；role_name_text 为底部面板初始角色名（NULL/空串 = 隐藏，
 * 待 mjpeg_player 按 ui_text op_name 配置后调 set_role 揭示） */
void ui_core_screen_init(const char *role_name_text);

/* 更新底部面板角色名（文本源 = 当前角色 ui_text.txt 的 op_name，原样显示）；
 * NULL/空串 = 隐藏该元素（不显示占位）。字体优先用 SD 字库
 * （sd_fonts，支持中文），未加载时回退内嵌 role_name_30（仅 ASCII）。 */
void ui_core_screen_set_role(const char *name);

/* SD 字库后台加载完成后调用：把角色名/当前可见横幅的字体从内嵌回退
 * 刷新为 SD 字库（中文立即生效）。未加载字库/无横幅时无害空操作。 */
void ui_core_screen_refresh_fonts(void);

/* intro 播完后揭示 UI（幂等，只生效一次）：leftcol 按 Bebas 阶梯重选
 * 字体并从紧贴屏缘（右缘 4px 已在屏内）激进缓出浮入（1.5s，由快变慢）；
 * footer 显示后 role_name 闪 7 次共 1s（间隔递减=越来越快，与 leftcol
 * 同时开始）→ sub_text 闪 4 次共 0.5s → 固定。leftcol_text/sub_text 为
 * 当前角色 ui_text.txt 的自定义文本（NULL/空串 = 该元素不显示，无默认
 * 文案）。任何线程可调（内部有 LVGL 锁）。 */
void ui_core_screen_intro_done(const char *leftcol_text, const char *sub_text);

/* 底部弹名片横幅：name=对端角色名，uname=对端用户名（空串时横幅
 * 回退显示 name）；payload=角色头图+用户头像+表情包拼接（RGB565），
 * 头像显示优先级：用户头像(avatar_len) > 角色头图(portrait_len) >
 * 内置占位图；sticker_lens 为各表情包字节长。任何线程可调（内部有
 * LVGL 锁）。 */
void ui_core_screen_show_banner(const char *name, const char *uname, const char *uid,
                                const uint8_t *payload, size_t payload_len,
                                uint16_t avatar_len, uint16_t portrait_len,
                                uint8_t sticker_cnt, const uint16_t *sticker_lens);

/* 屏幕中央状态提示：text=NULL 隐藏；非 NULL 显示（内部按内容去重，
 * 重复调用零开销）。hint_24 内嵌中文字库渲染（"请插入SD卡"/"SD卡无内容"）。
 * 播放器状态机在无卡/无角色/加载成功时调用。任何线程可调（内部持锁）。 */
void ui_core_screen_set_hint(const char *text);

/* 隐藏全部 UI 元素（leftcol/footer/sub_text/横幅）并重置揭示状态：
 * 拔卡或热插卡播放 intro 前调用，"intro 播完重新显示 UI 元素加载动效"
 * 由 intro_done 重新触发。幂等；任何线程可调（内部持锁）。 */
void ui_core_screen_hide_ui(void);

/* 把 UI 字体引用从 SD 字库切回内嵌（卸载 SD 字库前必须先调，防悬空）。
 * 之后 sd_fonts 重新加载并 refresh_fonts 后自动恢复 SD 字体。 */
void ui_core_screen_fonts_fallback(void);

/* 按“UI 选项”开关即时同步元素可见性（leftcol/角色名/副标题/footer/
 * 横幅/提示；菜单切换按钮点击后调用）。须 LVGL 上下文调用。幂等。 */
void ui_core_screen_apply_show_opts(void);

/* 顶部系统提示横幅（SD 自检提示：字库缺失/配置目录重建）。
 * line1/line2 为两行纯 ASCII 文本（无字库卡也能渲染，内嵌 montserrat）；
 * ms 后自动隐藏（0=默认 8s）。任意任务可调（内部持 LVGL 锁）；
 * 再次调用会刷新文本并重置计时。 */
void ui_core_screen_notice(const char *line1, const char *line2, uint32_t ms);

/* 右上角电源状态 chip：text=NULL/空串 隐藏；level 0=绿 1=黄 2=红。
 * 由 bsp_power 监测任务经显示回调驱动（"充电中…"/"即将充满"/
 * "电量低 x%"/"电量耗尽 即将休眠"）。任何线程可调（内部持锁）。 */
void ui_core_screen_set_power(const char *text, int level);

#endif