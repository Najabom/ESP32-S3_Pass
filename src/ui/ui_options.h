/* ============================================================================
 * ui_options.h - 界面显示开关（用户可关闭各 UI 层元素）
 *
 *   位掩码存于 NVS（ns "uiopt" / key "m"，u8）。缺省 = 全部开启。
 *   开关由菜单 "UI 选项" 页操作（ui_menu.c），消费点在 ui_core_screen.c
 *   （揭示/横幅/提示），任意线程可查（LVGL 线程内懒加载 NVS 一次）。
 *
 *   bit  含义
 *   0x01 左侧立牌竖排文字（leftcol）
 *   0x02 底部面板角色名（op_name）
 *   0x04 底部面板副标题（sub_text）
 *   0x08 收到名片横幅（banner）
 *   0x10 屏幕中央提示（hint）
 * ==========================================================================*/
#ifndef UI_OPTIONS_H
#define UI_OPTIONS_H

#include <stdbool.h>
#include <stdint.h>

#ifdef __cplusplus
extern "C" {
#endif

typedef enum {
    UIOPT_LEFTCOL  = 0x01,
    UIOPT_OP_NAME  = 0x02,
    UIOPT_SUB_TEXT = 0x04,
    UIOPT_BANNER   = 0x08,
    UIOPT_HINT     = 0x10,
} uiopt_bit_t;

#define UIOPT_ALL (UIOPT_LEFTCOL | UIOPT_OP_NAME | UIOPT_SUB_TEXT | \
                   UIOPT_BANNER | UIOPT_HINT)

/* 位查询（首次调用懒加载 NVS） */
bool ui_options_on(uiopt_bit_t bit);

/* 当前掩码 */
uint8_t ui_options_get(void);

/* 写入掩码并持久化（NVS commit；LVGL 上下文/任意线程可调） */
void ui_options_set(uint8_t mask);

#ifdef __cplusplus
}
#endif

#endif /* UI_OPTIONS_H */
