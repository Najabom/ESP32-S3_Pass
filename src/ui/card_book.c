/* ============================================================================
 * card_book.c - 名片簿数据层（SD 卡收卡记录解析）
 *
 * 功能：见 card_book.h。cards.txt 为 card_log.c 追加写入的整齐行格式，
 *   '|' 是可靠分隔符（写入时已把字段内的 | 与换行转义为空格）。
 * 坑：
 *   - name/uname 可能含空格/引号外字符，必须用引号包裹解析（不能按
 *     空格分词）；motto 我们不读（列表只需 uid/时间/mac）。
 *   - 行数上限由调用方 max 决定；CARD_BOOK_MAX_LIST=64 覆盖日常场景，
 *     名片记录超出时列表截断（文件本身无损）。
 *   - blob 段校验与 ui_core_screen.c 的 show_banner 规则一致：各段长度
 *     必须 == CARD_AVATAR_SIZE(8192) 且不越界，否则视为无该段。
 * ==========================================================================*/

// ui/card_book.c
#include "card_book.h"
#include "bsp_sdcard.h"
#include <stdio.h>
#include <string.h>
#include "esp_log.h"

static const char *TAG = "CARD_BOOK";

#define CARDS_TXT_PATH BSP_SDCARD_MOUNT_POINT "/cards.txt"
#define CARDS_BIN_DIR  BSP_SDCARD_MOUNT_POINT "/cards"

/* ---------- 字段解析：key=value（value 可选引号包裹） ----------
 * 功能：field 为 '|' 切出的单字段（可能带前导空格），解析后写入 out。
 *       key 不匹配则 out 保持原样（调用方须先清零目标）。 */
static void parse_field(const char *field, const char *key, char *out, size_t cap) {
    const char *p = field;
    while (*p == ' ' || *p == '\t') p++;
    size_t klen = strlen(key);
    if (strncmp(p, key, klen) != 0 || p[klen] != '=') return;
    p += klen + 1;
    while (*p == ' ' || *p == '\t') p++;
    if (*p == '"') {
        p++;
        size_t n = 0;
        while (p[n] && p[n] != '"' && n + 1 < cap) n++;
        memcpy(out, p, n);
        out[n] = '\0';
        return;
    }
    snprintf(out, cap, "%s", p);
}

int card_book_load_list(card_entry_t *out, int max) {
    if (out == NULL || max <= 0) return 0;

    bsp_sdcard_lock();
    if (!bsp_sdcard_is_mounted()) {
        bsp_sdcard_unlock();
        return 0;
    }
    FILE *f = fopen(CARDS_TXT_PATH, "r");
    if (f == NULL) {
        bsp_sdcard_unlock();
        return 0;
    }

    int n = 0;
    char line[256];
    while (n < max && fgets(line, sizeof(line), f) != NULL) {
        size_t len = strlen(line);
        while (len > 0 && (line[len-1] == '\n' || line[len-1] == '\r')) {
            line[--len] = '\0';
        }
        if (len == 0) continue;

        card_entry_t e;
        memset(&e, 0, sizeof(e));

        /* #0001 | up=0123s | mac=... | ... 按 '|' 切分 */
        char *tok = line;
        while (tok != NULL) {
            char *next = strchr(tok, '|');
            if (next) *next = '\0';

            const char *p = tok;
            while (*p == ' ' || *p == '\t') p++;
            if (p[0] == '#') {
                e.seq = (uint16_t)atoi(p + 1);
            } else if (strncmp(p, "up=", 3) == 0) {
                e.up_s = (uint32_t)atoi(p + 3);
            } else if (strncmp(p, "mac=", 4) == 0) {
                /* aa:bb:cc:dd:ee:ff → 12 位 hex（去掉 ':'） */
                const char *m = p + 4;
                int mi = 0;
                while (*m && mi < 12) {
                    if (*m != ':') e.mac[mi++] = *m;
                    m++;
                }
                e.mac[mi] = '\0';
            } else if (strncmp(p, "uid=", 4) == 0) {
                parse_field(p, "uid", e.uid, sizeof(e.uid));
            } else if (strncmp(p, "name=", 5) == 0) {
                parse_field(p, "name", e.name, sizeof(e.name));
            } else if (strncmp(p, "uname=", 6) == 0) {
                parse_field(p, "uname", e.uname, sizeof(e.uname));
            }
            tok = next ? next + 1 : NULL;
        }

        if (e.mac[0] == '\0') continue;   /* 坏行：无 mac 不可用 */

        /* 同 MAC 合并：字段取最新行（文件追加序 = 时间升序），次数累加 */
        int found = -1;
        for (int i = 0; i < n; i++) {
            if (strcmp(out[i].mac, e.mac) == 0) { found = i; break; }
        }
        if (found >= 0) {
            card_entry_t *dst = &out[found];
            dst->seq = e.seq;
            dst->up_s = e.up_s;
            memcpy(dst->uid, e.uid, sizeof(dst->uid));
            memcpy(dst->name, e.name, sizeof(dst->name));
            memcpy(dst->uname, e.uname, sizeof(dst->uname));
            dst->cnt++;
            continue;
        }
        e.cnt = 1;
        out[n++] = e;
    }
    fclose(f);

    /* 按收卡序号降序（最新在前；seq 来自文件行序、跨重启单调递增，
     * 不依赖墙钟——RTC 相对秒模式断电重启后 up_s 会回退，不能作排序键） */
    for (int i = 1; i < n; i++) {
        card_entry_t key = out[i];
        int j = i - 1;
        while (j >= 0 && out[j].seq < key.seq) {
            out[j + 1] = out[j];
            j--;
        }
        out[j + 1] = key;
    }
    bsp_sdcard_unlock();

    ESP_LOGI(TAG, "cards.txt 解析 %d 条（同 MAC 已合并）", n);
    return n;
}

uint8_t *card_book_load_blob(const char *mac12, size_t *out_len) {
    if (mac12 == NULL || strlen(mac12) != 12) return NULL;
    char path[64];
    snprintf(path, sizeof(path), CARDS_BIN_DIR "/card_%s.bin", mac12);
    return bsp_sdcard_load_file(path, out_len);   /* 内部持 SD 锁 */
}

/* v3 blob → 重建为 v4 布局的临时头（v3/v4 字段顺序相同，仅 motto/uname
 * 容量 20/16 vs 64/64；字段偏移不同不能直接强转）。
 * 返回重建后的 hdr 指针；调用方 view->hdr 用该临时头（static 缓冲）。 */
#define CARD_BLOB_HDR_V3_SIZE 104
#define CARD_BLOB_HDR_V4_SIZE 196   /* ver==4：与 v5 相同仅 name[24]（v5=64） */
static card_blob_hdr_t s_v3_hdr_tmp;

static const card_blob_hdr_t *hdr_from_blob(const uint8_t *blob, size_t len,
                                            uint32_t *out_hdr_size)
{
    const card_blob_hdr_t *hdr = (const card_blob_hdr_t *)blob;
    if (hdr->ver >= CARD_BLOB_VER) {
        if (len < sizeof(card_blob_hdr_t)) return NULL;
        *out_hdr_size = sizeof(card_blob_hdr_t);
        return hdr;
    }
    /* v4（name[24]，motto[64] uname[64]）：与 v5 仅 name 长度不同。
     * 按 v4 偏移解构，填进 v5 布局的临时头。 */
    if (hdr->ver == 4) {
        if (len < CARD_BLOB_HDR_V4_SIZE) return NULL;
        const uint8_t *p = blob;
        card_blob_hdr_t *h = &s_v3_hdr_tmp;
        memset(h, 0, sizeof(*h));
        h->magic = *(const uint32_t *)p;            p += 4;
        h->ver = *p++;                               /* =4 */
        h->name_len = *p++;
        memcpy(h->name, p, h->name_len);             p += 24;   /* v4 name[24] */
        h->uid_len = *p++;
        memcpy(h->uid, p, h->uid_len);               p += CARD_UID_MAX;
        h->motto_len = *p++;
        memcpy(h->motto, p, h->motto_len);           p += CARD_MOTTO_MAX;
        h->avatar_len = *(const uint16_t *)p;        p += 2;
        h->sticker_cnt = *p++;
        memcpy(h->sticker_len, p, sizeof(h->sticker_len)); p += 6;
        h->ts = *(const uint32_t *)p;                p += 4;
        h->uname_len = *p++;
        memcpy(h->uname, p, h->uname_len);           p += CARD_UNAME_MAX;
        h->portrait_len = *(const uint16_t *)p;      p += 2;
        *out_hdr_size = CARD_BLOB_HDR_V4_SIZE;
        return h;
    }
    /* v3（104B 头，motto[20] uname[16]）：手动按 v3 偏移解构，
     * 填进 v4 布局的临时头（补齐长度为当前 CARD_MOTTO/UNAME_MAX）。 */
    if (len < CARD_BLOB_HDR_V3_SIZE) return NULL;
    const uint8_t *p = blob;
    card_blob_hdr_t *h = &s_v3_hdr_tmp;
    memset(h, 0, sizeof(*h));
    h->magic = *(const uint32_t *)p;            p += 4;
    h->ver = *p++;                               /* =3 */
    h->name_len = *p++;
    memcpy(h->name, p, h->name_len);             p += CARD_NAME_MAX;
    h->uid_len = *p++;
    memcpy(h->uid, p, h->uid_len);               p += CARD_UID_MAX;
    h->motto_len = *p++;
    memcpy(h->motto, p, h->motto_len);           p += 20;   /* v3 motto[20] */
    h->avatar_len = *(const uint16_t *)p;        p += 2;
    h->sticker_cnt = *p++;
    memcpy(h->sticker_len, p, sizeof(h->sticker_len)); p += 6;
    h->ts = *(const uint32_t *)p;                p += 4;
    h->uname_len = *p++;
    memcpy(h->uname, p, h->uname_len);           p += 16;   /* v3 uname[16] */
    h->portrait_len = *(const uint16_t *)p;      p += 2;
    *out_hdr_size = CARD_BLOB_HDR_V3_SIZE;
    return h;
}

void card_book_parse_blob(const uint8_t *blob, size_t len, card_blob_view_t *view) {
    memset(view, 0, sizeof(*view));
    if (blob == NULL || len < 8) return;   /* 至少 magic 4B + ver 1B */

    uint32_t hdr_size = 0;
    const card_blob_hdr_t *hdr = hdr_from_blob(blob, len, &hdr_size);
    if (hdr == NULL) {
        ESP_LOGW(TAG, "blob 头解析失败（len=%u）", (unsigned)len);
        return;
    }
    view->hdr = hdr;

    const uint8_t *p = blob + hdr_size;
    size_t rem = len - hdr_size;
    uint32_t total = (uint32_t)hdr->portrait_len + hdr->avatar_len;
    for (int i = 0; i < hdr->sticker_cnt && i < 3; i++) {
        total += hdr->sticker_len[i];
    }
    if (total > rem) {
        ESP_LOGW(TAG, "blob 段越界（声明 %u，剩余 %u），按可用段解析",
                 (unsigned)total, (unsigned)rem);
    }

    /* 段布局：portrait + avatar + stickers（与 show_banner 一致） */
    if (hdr->portrait_len == CARD_AVATAR_SIZE && hdr->portrait_len <= rem) {
        view->portrait = p;
        p += hdr->portrait_len;
        rem -= hdr->portrait_len;
    } else if (hdr->portrait_len > rem) {
        rem = 0;
    } else {
        p += hdr->portrait_len;
        rem -= hdr->portrait_len;
    }
    if (hdr->avatar_len == CARD_AVATAR_SIZE && hdr->avatar_len <= rem) {
        view->avatar = p;
        p += hdr->avatar_len;
        rem -= hdr->avatar_len;
    } else if (hdr->avatar_len > rem) {
        rem = 0;
    } else {
        p += hdr->avatar_len;
        rem -= hdr->avatar_len;
    }
    for (int i = 0; i < hdr->sticker_cnt && i < 3; i++) {
        if (hdr->sticker_len[i] == CARD_AVATAR_SIZE && hdr->sticker_len[i] <= rem) {
            view->stickers[view->sticker_cnt++] = p;
            p += hdr->sticker_len[i];
            rem -= hdr->sticker_len[i];
        } else if (hdr->sticker_len[i] > rem) {
            rem = 0;
        } else {
            p += hdr->sticker_len[i];
            rem -= hdr->sticker_len[i];
        }
    }
}

/* ---------- 名片详情：该 MAC 收卡按角色名分组计数 ----------
 * 与 load_list 同源解析（'|' 分隔 + 引号字段），但保留每次交换的
 * 角色名（name 字段）——同角色合并计数，不同角色分行，按最近一次
 * 序号降序。 */
int card_book_load_name_counts(const char *mac12,
                               char out_names[][CARD_NAME_MAX + 1],
                               uint16_t *out_counts, int max) {
    if (mac12 == NULL || strlen(mac12) != 12 || out_names == NULL ||
        out_counts == NULL || max <= 0) {
        return 0;
    }

    bsp_sdcard_lock();
    if (!bsp_sdcard_is_mounted()) {
        bsp_sdcard_unlock();
        return 0;
    }
    FILE *f = fopen(CARDS_TXT_PATH, "r");
    if (f == NULL) {
        bsp_sdcard_unlock();
        return 0;
    }

    /* 命中该 MAC 的行的 (name, seq) 队列（最多 32 行单 MAC 上限；超过
     * 仅计数（栈预算：32×67 ≈ 2.1KB，lvgl_task 栈 8K 内安全）） */
    typedef struct { uint16_t seq; char name[CARD_NAME_MAX + 1]; } item_t;
    item_t items[32];
    int n_items = 0;
    /* 分组合并结果 */
    char used_names[CARD_NAME_MAX + 1][8];
    uint16_t used_cnt[8];
    int used_n = 0;

    char line[256];
    while (n_items < 32 && fgets(line, sizeof(line), f) != NULL) {
        size_t len = strlen(line);
        while (len > 0 && (line[len-1] == '\n' || line[len-1] == '\r')) {
            line[--len] = '\0';
        }
        if (len == 0) continue;

        uint16_t seq = 0;
        char mac[13] = "";
        char name[CARD_NAME_MAX + 1] = "";
        char *tok = line;
        while (tok != NULL) {
            char *next = strchr(tok, '|');
            if (next) *next = '\0';
            const char *p = tok;
            while (*p == ' ' || *p == '\t') p++;
            if (p[0] == '#') {
                seq = (uint16_t)atoi(p + 1);
            } else if (strncmp(p, "mac=", 4) == 0) {
                const char *m = p + 4;
                int mi = 0;
                while (*m && mi < 12) {
                    if (*m != ':') mac[mi++] = *m;
                    m++;
                }
                mac[mi] = '\0';
            } else if (strncmp(p, "name=", 5) == 0) {
                parse_field(p, "name", name, sizeof(name));
            }
            tok = next ? next + 1 : NULL;
        }
        if (strcmp(mac, mac12) != 0) continue;
        item_t *it = &items[n_items++];
        it->seq = seq;
        memcpy(it->name, name, sizeof(it->name));
    }
    fclose(f);
    bsp_sdcard_unlock();

    if (n_items == 0) return 0;

    /* 按 seq 降序排 items（最新角色在前） */
    for (int i = 1; i < n_items; i++) {
        item_t key = items[i];
        int j = i - 1;
        while (j >= 0 && items[j].seq < key.seq) { items[j + 1] = items[j]; j--; }
        items[j + 1] = key;
    }

    /* 相同角色名合并计数（含同名重复出现合并） */
    for (int i = 0; i < n_items; i++) {
        const char *nm = items[i].name;
        int found = -1;
        for (int k = 0; k < used_n; k++) {
            if (strcmp(used_names[k], nm) == 0) { found = k; break; }
        }
        if (found >= 0) {
            used_cnt[found]++;
        } else if (used_n < 8) {
            memcpy(used_names[used_n], nm, sizeof(used_names[0]));
            used_cnt[used_n] = 1;
            used_n++;
        }
    }

    int out_n = 0;
    for (int k = 0; k < used_n && out_n < max; k++) {
        memcpy(out_names[out_n], used_names[k], CARD_NAME_MAX + 1);
        out_counts[out_n] = used_cnt[k];
        out_n++;
    }
    ESP_LOGI(TAG, "name counts for %s: %d groups", mac12, out_n);
    return out_n;
}
