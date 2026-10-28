/* ============================================================================
 * card_book.h - 名片簿数据层（SD 卡收卡记录）
 *
 * 数据源：
 *   /sdcard/cards.txt            可读文本行（card_log.c 追加写入）
 *   /sdcard/cards/card_<mac>.bin 完整名片 blob（card_blob_hdr_t + 图片段）
 *
 * 列表解析：行格式 "#0001 | up=0123s | mac=aa:..:ff | uname=".." | name=".."
 *   | uid=.. | motto=".." | avatar=8192 | portrait=0 | stk=2"（card_log.c
 *   写入时 '|'/换行已转义为空格，'|' 可作为可靠分隔符）。
 * 详情段布局（与 ui_core_screen.c show_banner 一致）：
 *   blob = card_blob_hdr_t + 角色头图(portrait_len) + 用户头像(avatar_len)
 *          + 表情包们(sticker_len[i])
 *
 * 线程：所有函数内部持 SD 锁；blob 缓冲由调用方 heap_caps_free。
 * ==========================================================================*/

#ifndef CARD_BOOK_H
#define CARD_BOOK_H

#include <stdint.h>
#include <stdbool.h>
#include <stddef.h>
#include "nfc_proto.h"

#ifdef __cplusplus
extern "C" {
#endif

#define CARD_BOOK_MAX_LIST 64   /* 名片列表最大条数（内存：64 × ~90B） */

typedef struct {
    uint16_t seq;       /* #N 序号（1 起） */
    uint32_t up_s;      /* 收卡时刻 = 墙钟秒（time_sync，未校准为 RTC 相对秒） */
    char     mac[13];   /* 12 位小写 hex（= cards/card_<mac>.bin 文件名） */
    char     uid[21];
    char     name[25];  /* 角色名（可能为空串） */
    char     uname[17]; /* 用户名（可能为空串） */
    uint8_t  cnt;       /* 同一人（MAC）累计交换次数，≥1 */
} card_entry_t;

/* 解析 /sdcard/cards.txt 全部行（SD 锁内，逐行 <256B），填 out（须 >= max
 * 项），返回实际条数。同 MAC 多行合并为一条（字段取最新行、cnt 累加），
 * 按 up_s 降序（最新在前）。无卡/无文件/解析失败返回 0。 */
int card_book_load_list(card_entry_t *out, int max);

/* 名片详情：解析 /sdcard/cards.txt 中该 mac 的全部收卡行，按角色名
 * （name 字段）分组计数。out_names[i]/out_counts[i] 一一对应，按最近
 * 一次交换的序号降序（最新角色在前）。名称去重（同角色合并计数，
 * 但同一角色跨段时间切换时合并行计数）——只取"角色名"分组。
 * 返回分组数（≤max）；无记录/无卡返回 0。每个 out_names[i] 至多
 * CARD_NAME_MAX+1 字节（UTF-8，含 NUL）。 */
int card_book_load_name_counts(const char *mac12,
                               char out_names[][CARD_NAME_MAX + 1],
                               uint16_t *out_counts, int max);

/* 整读 /sdcard/cards/card_<mac12>.bin 到 PSRAM（内部持 SD 锁）。
 * 成功返回缓冲（调用方 heap_caps_free）并填 *out_len；失败返回 NULL。 */
uint8_t *card_book_load_blob(const char *mac12, size_t *out_len);

/* blob 段布局视图（parse 后各指针指向 blob 内部，blob 必须保持存活）：
 * 与 show_banner 同一规则：角色头图在 avatar 之前，依次拼接。 */
typedef struct {
    const card_blob_hdr_t *hdr;          /* blob 头部（名称/ID/寄语等） */
    const uint8_t *portrait;             /* 角色头图 64x64（NULL=无） */
    const uint8_t *avatar;               /* 用户头像 64x64（NULL=无） */
    const uint8_t *stickers[3];          /* 表情包 64x64（NULL=无） */
    uint8_t        sticker_cnt;
} card_blob_view_t;

void card_book_parse_blob(const uint8_t *blob, size_t len, card_blob_view_t *view);

#ifdef __cplusplus
}
#endif

#endif /* CARD_BOOK_H */
