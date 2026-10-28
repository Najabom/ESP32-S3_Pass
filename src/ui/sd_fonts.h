/* ============================================================================
 * sd_fonts.h - SD 卡 bin 字库加载（LVGL 官方 lv_font_load 全量版）
 *
 * 用途：播放前从 /sdcard/config/fonts/ 读 lv_font_conv 生成的 bin 字库
 *   （GB2312 全集，tools/gen_sd_fonts.py 生成），官方 loader 流式解析，
 *   常驻 ≈2.7MB（PSRAM），让角色名/名片横幅支持中文显示。
 * 时序：mjpeg_player 在载入 loop 之后、intro 之前同步调用
 *   sd_fonts_load_for(names)——此刻 PSRAM 最空，视频与中文 UI 同时出现。
 * ==========================================================================*/

#ifndef SD_FONTS_H
#define SD_FONTS_H

#include <stdbool.h>
#include "lvgl.h"
#include "bsp/bsp_sdcard.h"

/* SD 字库档位（与 tools/gen_sd_fonts.py 的 SIZES 对应） */
#define SD_FONT_ROLE_LARGE 30   // 底部角色名 / 横幅短名
#define SD_FONT_ROLE_SMALL 16   // 横幅长名

/* 加载两档字库（幂等：两档在 → 直接 true；names 参数忽略——GB2312
 * 全集与角色名无关，保留签名兼容调用方）。
 * 返回 true = 两档就绪；false = 未就绪（回退内嵌 ASCII，不阻塞播放）。 */
bool sd_fonts_load_for(const char *names);

/* 带进度回调的加载（bsp_sdcard_progress_cb_t，读 bin 时每 4KB 触发，
 * 供开机终端进度条连续化；调用方须保证回调线程安全）。 */
bool sd_fonts_load_for_ex(const char *names, bsp_sdcard_progress_cb_t cb, void *ctx);

/* 取指定档位字体；未加载返回 NULL（调用方回退内嵌字库）。 */
const lv_font_t *sd_fonts_role(int px);

/* 大档（30px）是否已加载（横幅据此决定用 SD 阶梯还是内嵌阶梯） */
bool sd_fonts_loaded(void);

/* 两档（30+16）全部就绪 */
bool sd_fonts_fully_loaded(void);

/* 卡上是否存在字库文件（至少一个 bin 曾成功读入）。
 * false = 无字库卡（回退内嵌属正常），true = 有文件、构建失败须排查 */
bool sd_fonts_bins_readable(void);

/* 卸载子集（free 全部子集内存，幂等）。调用前须先把 UI 字体引用切回
 * 内嵌（ui_core_screen_fonts_fallback），防悬空指针。 */
void sd_fonts_unload(void);

#endif /* SD_FONTS_H */
