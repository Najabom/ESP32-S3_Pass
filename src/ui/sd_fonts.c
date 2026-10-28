/* ============================================================================
 * sd_fonts.c - SD 卡 bin 字库加载（官方解析算法 · 内存版）
 *
 * 背景：lv_font_load 走 lv_fs（S: 盘）逐 glyph 小块 IO，在本机 GPT 偏移
 *   卡上性能崩溃（>100s 不返回）。改为：bsp_sdcard_load_file 整读 bin
 *   到 PSRAM（实测 1.9MB ≈ 4.4s），再用与 lv_font_loader.c 完全相同的
 *   官方算法在内存上解析（逐行移植，仅 IO 层换为内存游标）——渲染
 *   正确性与官方 loader 等价，失败清理直接复用 lv_font_free()。
 * 内存：常驻 = 字形池 30px ~1.85MB + 16px ~0.8MB（lv_mem_alloc 已指向
 *   PSRAM）；bin 缓冲用完即释放，峰值 ≈ loop + bin + 池 ≈ 6.3MB < 8MB。
 * 时序：mjpeg_player 在载入 loop 之后、intro 之前同步调用（幂等），
 *   完成后视频与中文 UI 同时揭示；失败仅回退内嵌（ASCII），不阻塞。
 * ==========================================================================*/

#include "sd_fonts.h"
#include "bsp_sdcard.h"
#include "esp_log.h"
#include "esp_timer.h"
#include "esp_heap_caps.h"
#include "lvgl.h"

static const char *TAG = "SD_FONTS";

#define FONT30_PATH "/sdcard/config/fonts/role_name_30.bin"
#define FONT16_PATH "/sdcard/config/fonts/role_name_16.bin"
/* 兼容旧版 SD 卡：早期固件以 op_name_*.bin 命名写入 /config/fonts，
 * 新文件名缺失时回退尝试旧文件名（见 sd_fonts_load_for_ex）。 */
#define FONT30_LEGACY_PATH "/sdcard/config/fonts/op_name_30.bin"
#define FONT16_LEGACY_PATH "/sdcard/config/fonts/op_name_16.bin"

static lv_font_t *s_font30 = NULL;
static lv_font_t *s_font16 = NULL;
static bool s_bins_readable = false;   /* 卡上读到过有效字库文件 */
/* 实际加载成功的源文件（新名或旧名回退），供字形自检按同源重读 */
static const char *s_font30_src = FONT30_PATH;
static const char *s_font16_src = FONT16_PATH;

/* ---------- 内存游标（替代 lv_fs_file_t） ---------- */
typedef struct {
    const uint8_t *buf;
    size_t size;
    size_t pos;
} mem_file_t;

static bool mem_read(mem_file_t *mf, void *dst, size_t n)
{
    if (mf->pos + n > mf->size) return false;
    memcpy(dst, mf->buf + mf->pos, n);
    mf->pos += n;
    return true;
}

static bool mem_seek(mem_file_t *mf, size_t pos)
{
    if (pos > mf->size) return false;
    mf->pos = pos;
    return true;
}

/* ---------- bin 结构体（照抄 lv_font_loader.c 内部定义） ---------- */
typedef struct {
    uint32_t version;
    uint16_t tables_count;
    uint16_t font_size;
    uint16_t ascent;
    int16_t descent;
    uint16_t typo_ascent;
    int16_t typo_descent;
    uint16_t typo_line_gap;
    int16_t min_y;
    int16_t max_y;
    uint16_t default_advance_width;
    uint16_t kerning_scale;
    uint8_t index_to_loc_format;
    uint8_t glyph_id_format;
    uint8_t advance_width_format;
    uint8_t bits_per_pixel;
    uint8_t xy_bits;
    uint8_t wh_bits;
    uint8_t advance_width_bits;
    uint8_t compression_id;
    uint8_t subpixels_mode;
    uint8_t padding;
    int16_t underline_position;
    uint16_t underline_thickness;
} font_header_bin_t;

typedef struct {
    uint32_t data_offset;
    uint32_t range_start;
    uint16_t range_length;
    uint16_t glyph_id_start;
    uint16_t data_entries_count;
    uint8_t format_type;
    uint8_t padding;
} cmap_table_bin_t;

/* ---------- 位流迭代器（照抄，lv_fs_read → mem_read） ---------- */
typedef struct {
    mem_file_t *mf;
    int8_t bit_pos;
    uint8_t byte_value;
    bool ok;
} bit_iterator_t;

static bit_iterator_t init_bit_iterator(mem_file_t *mf)
{
    bit_iterator_t it;
    it.mf = mf;
    it.bit_pos = -1;
    it.byte_value = 0;
    it.ok = true;
    return it;
}

static unsigned int read_bits(bit_iterator_t *it, int n_bits)
{
    unsigned int value = 0;
    while (n_bits--) {
        it->byte_value = it->byte_value << 1;
        it->bit_pos--;
        if (it->bit_pos < 0) {
            it->bit_pos = 7;
            if (!mem_read(it->mf, &it->byte_value, 1)) {
                it->ok = false;
                return 0;
            }
        }
        int8_t bit = (it->byte_value & 0x80) ? 1 : 0;
        value |= ((unsigned int)bit << n_bits);
    }
    return value;
}

static int read_bits_signed(bit_iterator_t *it, int n_bits)
{
    unsigned int value = read_bits(it, n_bits);
    if (!it->ok) return 0;
    if (value & (1u << (n_bits - 1))) {
        value |= ~0u << n_bits;
    }
    return (int)value;
}

/* [u32 长度][4B 标签]，返回块总长（含 8B 头），失败 -1 */
static int read_label(mem_file_t *mf, size_t start, const char *label)
{
    uint32_t length;
    char buf[4];
    if (!mem_seek(mf, start) ||
        !mem_read(mf, &length, 4) ||
        !mem_read(mf, buf, 4) ||
        memcmp(label, buf, 4) != 0) {
        return -1;
    }
    return (int)length;
}

/* ---------- cmap（照抄 load_cmaps + load_cmaps_tables） ---------- */
static bool load_cmaps_tables(mem_file_t *mf, lv_font_fmt_txt_dsc_t *font_dsc,
                              uint32_t cmaps_start, cmap_table_bin_t *cmap_table)
{
    if (!mem_read(mf, cmap_table, font_dsc->cmap_num * sizeof(cmap_table_bin_t))) {
        return false;
    }

    for (unsigned int i = 0; i < font_dsc->cmap_num; ++i) {
        if (!mem_seek(mf, cmaps_start + cmap_table[i].data_offset)) {
            return false;
        }

        lv_font_fmt_txt_cmap_t *cmap = (lv_font_fmt_txt_cmap_t *)&(font_dsc->cmaps[i]);

        cmap->range_start = cmap_table[i].range_start;
        cmap->range_length = cmap_table[i].range_length;
        cmap->glyph_id_start = cmap_table[i].glyph_id_start;
        cmap->type = cmap_table[i].format_type;

        switch (cmap_table[i].format_type) {
            case LV_FONT_FMT_TXT_CMAP_FORMAT0_FULL: {
                    uint8_t ids_size = sizeof(uint8_t) * cmap_table[i].data_entries_count;
                    uint8_t *glyph_id_ofs_list = lv_mem_alloc(ids_size);

                    cmap->glyph_id_ofs_list = glyph_id_ofs_list;
                    if (!glyph_id_ofs_list ||
                        !mem_read(mf, glyph_id_ofs_list, ids_size)) {
                        return false;
                    }
                    cmap->list_length = cmap_table[i].range_length;
                    break;
                }
            case LV_FONT_FMT_TXT_CMAP_FORMAT0_TINY:
                break;
            case LV_FONT_FMT_TXT_CMAP_SPARSE_FULL:
            case LV_FONT_FMT_TXT_CMAP_SPARSE_TINY: {
                    uint32_t list_size = sizeof(uint16_t) * cmap_table[i].data_entries_count;
                    uint16_t *unicode_list = (uint16_t *)lv_mem_alloc(list_size);

                    cmap->unicode_list = unicode_list;
                    cmap->list_length = cmap_table[i].data_entries_count;
                    if (!unicode_list ||
                        !mem_read(mf, unicode_list, list_size)) {
                        return false;
                    }

                    if (cmap_table[i].format_type == LV_FONT_FMT_TXT_CMAP_SPARSE_FULL) {
                        uint16_t *buf = (uint16_t *)lv_mem_alloc(
                            sizeof(uint16_t) * cmap->list_length);
                        cmap->glyph_id_ofs_list = buf;
                        if (!buf ||
                            !mem_read(mf, buf, sizeof(uint16_t) * cmap->list_length)) {
                            return false;
                        }
                    }
                    break;
                }
            default:
                return false;
        }
    }
    return true;
}

static int32_t load_cmaps(mem_file_t *mf, lv_font_fmt_txt_dsc_t *font_dsc,
                          uint32_t cmaps_start)
{
    int32_t cmaps_length = read_label(mf, cmaps_start, "cmap");
    if (cmaps_length < 0) {
        return -1;
    }

    uint32_t cmaps_subtables_count;
    if (!mem_read(mf, &cmaps_subtables_count, sizeof(uint32_t))) {
        return -1;
    }

    lv_font_fmt_txt_cmap_t *cmaps =
        (lv_font_fmt_txt_cmap_t *)lv_mem_alloc(
            cmaps_subtables_count * sizeof(lv_font_fmt_txt_cmap_t));
    if (!cmaps) return -1;
    memset(cmaps, 0, cmaps_subtables_count * sizeof(lv_font_fmt_txt_cmap_t));

    font_dsc->cmaps = cmaps;
    font_dsc->cmap_num = (uint16_t)cmaps_subtables_count;

    cmap_table_bin_t *cmaps_tables = (cmap_table_bin_t *)lv_mem_alloc(
        sizeof(cmap_table_bin_t) * font_dsc->cmap_num);
    if (!cmaps_tables) return -1;

    bool success = load_cmaps_tables(mf, font_dsc, cmaps_start, cmaps_tables);
    lv_mem_free(cmaps_tables);

    return success ? cmaps_length : -1;
}

/* ---------- 白名单字形挑选（只解析用到的码点，跳过其余 7500+ 字形） ----------
 * 输入 names（UTF-8 角色名串）→ 收集码点 → 遍历 cmap 表反查 gid →
 * 构造 keep_mask[gid]（loca_count 大小）。返回 NULL 表示白名单为空。 */
static uint8_t *build_keep_mask(lv_font_fmt_txt_dsc_t *dsc, uint32_t loca_count,
                                const char *names)
{
    if (names == NULL) {
        return NULL;   /* 全量加载：不构造过滤 mask */
    }

    uint32_t cps[128];
    uint32_t cp_cnt = 0;

    while (*names && cp_cnt < 128) {
        const uint8_t *p = (const uint8_t *)names;
        if (*p < 0x80) {
            cps[cp_cnt++] = *names++;
        } else if ((*p & 0xE0) == 0xC0 && p[1]) {
            cps[cp_cnt++] = ((uint32_t)(p[0] & 0x1F) << 6) | (p[1] & 0x3F);
            names += 2;
        } else if ((*p & 0xF0) == 0xE0 && p[1] && p[2]) {
            cps[cp_cnt++] = ((uint32_t)(p[0] & 0x0F) << 12) |
                            ((uint32_t)(p[1] & 0x3F) << 6) | (p[2] & 0x3F);
            names += 3;
        } else if ((*p & 0xF8) == 0xF0 && p[1] && p[2] && p[3]) {
            cps[cp_cnt++] = ((uint32_t)(p[0] & 0x07) << 18) |
                            ((uint32_t)(p[1] & 0x3F) << 12) |
                            ((uint32_t)(p[2] & 0x3F) << 6) | (p[3] & 0x3F);
            names += 4;
        } else {
            names++;               /* 非法字节跳过 */
        }
    }
    if (cp_cnt == 0) return NULL;

    uint8_t *mask = (uint8_t *)lv_mem_alloc(loca_count);
    if (!mask) return NULL;
    memset(mask, 0, loca_count);

    for (uint32_t t = 0; t < dsc->cmap_num; ++t) {
        lv_font_fmt_txt_cmap_t *cmap = (lv_font_fmt_txt_cmap_t *)&(dsc->cmaps[t]);
        for (uint32_t n = 0; n < cp_cnt; ++n) {
            uint32_t c = cps[n];
            if (c < cmap->range_start || c >= cmap->range_start + cmap->range_length) {
                continue;
            }
            uint32_t rcp = c - cmap->range_start;
            uint32_t gid;
            switch (cmap->type) {
                case LV_FONT_FMT_TXT_CMAP_FORMAT0_FULL:
                    gid = cmap->glyph_id_start +
                          ((const uint8_t *)cmap->glyph_id_ofs_list)[rcp];
                    break;
                case LV_FONT_FMT_TXT_CMAP_SPARSE_FULL:
                    /* unicode_list 存相对码点，search(rcp) 定下标 */
                    uint32_t i = 0;
                    while (i < cmap->list_length &&
                           cmap->unicode_list[i] != (uint16_t)rcp) i++;
                    if (i == cmap->list_length) continue;
                    gid = cmap->glyph_id_start +
                          ((const uint16_t *)cmap->glyph_id_ofs_list)[i];
                    break;
                case LV_FONT_FMT_TXT_CMAP_FORMAT0_TINY:
                    gid = cmap->glyph_id_start + rcp;
                    break;
                default: /* SPARSE_TINY */
                    uint32_t j = 0;
                    while (j < cmap->list_length &&
                           cmap->unicode_list[j] != (uint16_t)rcp) j++;
                    if (j == cmap->list_length) continue;
                    gid = cmap->glyph_id_start + j;
                    break;
            }
            if (gid > 0 && gid < loca_count) mask[gid] = 1;
        }
    }
    return mask;
}

/* ---------- glyf（照抄 load_glyph，含 slow path 末字节左移对齐；keep 防拷贝） ---------- */
static int32_t load_glyph(mem_file_t *mf, lv_font_fmt_txt_dsc_t *font_dsc,
                          uint32_t start, uint32_t *glyph_offset, uint32_t loca_count,
                          font_header_bin_t *header, const uint8_t *keep)
{
    int32_t glyph_length = read_label(mf, start, "glyf");
    if (glyph_length < 0) {
        return -1;
    }

    lv_font_fmt_txt_glyph_dsc_t *glyph_dsc = (lv_font_fmt_txt_glyph_dsc_t *)
        lv_mem_alloc(loca_count * sizeof(lv_font_fmt_txt_glyph_dsc_t));
    if (!glyph_dsc) return -1;
    memset(glyph_dsc, 0, loca_count * sizeof(lv_font_fmt_txt_glyph_dsc_t));

    font_dsc->glyph_dsc = glyph_dsc;

    int cur_bmp_size = 0;

    for (uint32_t i = 0; i < loca_count; ++i) {
        lv_font_fmt_txt_glyph_dsc_t *gdsc = &glyph_dsc[i];

        if (!mem_seek(mf, start + glyph_offset[i])) {
            return -1;
        }

        bit_iterator_t bit_it = init_bit_iterator(mf);

        if (header->advance_width_bits == 0) {
            gdsc->adv_w = header->default_advance_width;
        } else {
            gdsc->adv_w = read_bits(&bit_it, header->advance_width_bits);
            if (!bit_it.ok) return -1;
        }

        if (header->advance_width_format == 0) {
            gdsc->adv_w *= 16;
        }

        gdsc->ofs_x = read_bits_signed(&bit_it, header->xy_bits);
        if (!bit_it.ok) return -1;
        gdsc->ofs_y = read_bits_signed(&bit_it, header->xy_bits);
        if (!bit_it.ok) return -1;
        gdsc->box_w = read_bits(&bit_it, header->wh_bits);
        if (!bit_it.ok) return -1;
        gdsc->box_h = read_bits(&bit_it, header->wh_bits);
        if (!bit_it.ok) return -1;

        int nbits = header->advance_width_bits + 2 * header->xy_bits + 2 * header->wh_bits;
        int next_offset = (i < loca_count - 1) ? (int)glyph_offset[i + 1] : glyph_length;
        int bmp_size = next_offset - (int)glyph_offset[i] - nbits / 8;

        if (i == 0) {
            gdsc->adv_w = 0;
            gdsc->box_w = 0;
            gdsc->box_h = 0;
            gdsc->ofs_x = 0;
            gdsc->ofs_y = 0;
        }

        gdsc->bitmap_index = cur_bmp_size;
        if (keep && !keep[i]) {
            /* 白名单过滤：该字形不进池，描述符清零（LVGL 永不取用） */
            gdsc->adv_w = 0;
            gdsc->box_w = 0;
            gdsc->box_h = 0;
            gdsc->ofs_x = 0;
            gdsc->ofs_y = 0;
        } else if (gdsc->box_w * gdsc->box_h != 0) {
            cur_bmp_size += bmp_size;
        }
    }

    uint8_t *glyph_bmp = (uint8_t *)lv_mem_alloc((size_t)cur_bmp_size + 1);
    if (!glyph_bmp) return -1;

    font_dsc->glyph_bitmap = glyph_bmp;

    cur_bmp_size = 0;

    for (uint32_t i = 1; i < loca_count; ++i) {
        if (!mem_seek(mf, start + glyph_offset[i])) {
            return -1;
        }
        bit_iterator_t bit_it = init_bit_iterator(mf);

        int nbits = header->advance_width_bits + 2 * header->xy_bits + 2 * header->wh_bits;

        read_bits(&bit_it, nbits);
        if (!bit_it.ok) return -1;

        int next_offset = (i < loca_count - 1) ? (int)glyph_offset[i + 1] : glyph_length;
        int bmp_size = next_offset - (int)glyph_offset[i] - nbits / 8;

        if (keep && !keep[i]) {
            /* 白名单过滤：不拷贝，seek 跳到下一字形起点（下一轮循环
             * 重建 bit_it，残余位自然废弃） */
            mem_seek(mf, start + next_offset);
            continue;
        }

        if (glyph_dsc[i].box_w * glyph_dsc[i].box_h == 0) {
            continue;
        }

        if (nbits % 8 == 0) {  /* 快路径：整字节，直接拷 */
            if (!mem_read(mf, &glyph_bmp[cur_bmp_size], (size_t)bmp_size)) {
                return -1;
            }
        } else {
            for (int k = 0; k < bmp_size - 1; ++k) {
                glyph_bmp[cur_bmp_size + k] = (uint8_t)read_bits(&bit_it, 8);
                if (!bit_it.ok) return -1;
            }
            glyph_bmp[cur_bmp_size + bmp_size - 1] =
                (uint8_t)read_bits(&bit_it, 8 - nbits % 8);
            if (!bit_it.ok) return -1;
            /* 末段碎片在 MSB，read_bits 放到了 LSB——左移归位 */
            glyph_bmp[cur_bmp_size + bmp_size - 1] <<= (nbits % 8);
        }

        cur_bmp_size += bmp_size;
    }
    return glyph_length;
}

/* ---------- kern（照抄 load_kern） ---------- */
static int32_t load_kern(mem_file_t *mf, lv_font_fmt_txt_dsc_t *font_dsc,
                         uint8_t format, uint32_t start)
{
    int32_t kern_length = read_label(mf, start, "kern");
    if (kern_length < 0) {
        return -1;
    }

    uint8_t kern_format_type;
    uint8_t padding[3];
    if (!mem_read(mf, &kern_format_type, sizeof(uint8_t)) ||
        !mem_read(mf, padding, 3 * sizeof(uint8_t))) {
        return -1;
    }

    if (0 == kern_format_type) { /* 排序对 */
        lv_font_fmt_txt_kern_pair_t *kern_pair =
            (lv_font_fmt_txt_kern_pair_t *)lv_mem_alloc(sizeof(lv_font_fmt_txt_kern_pair_t));
        if (!kern_pair) return -1;
        memset(kern_pair, 0, sizeof(lv_font_fmt_txt_kern_pair_t));

        font_dsc->kern_dsc = kern_pair;
        font_dsc->kern_classes = 0;

        uint32_t glyph_entries;
        if (!mem_read(mf, &glyph_entries, sizeof(uint32_t))) {
            return -1;
        }

        int ids_size;
        if (format == 0) {
            ids_size = sizeof(int8_t) * 2 * glyph_entries;
        } else {
            ids_size = sizeof(int16_t) * 2 * glyph_entries;
        }

        uint8_t *glyph_ids = (uint8_t *)lv_mem_alloc((size_t)ids_size);
        int8_t *values = (int8_t *)lv_mem_alloc(glyph_entries);
        if (!glyph_ids || !values) return -1;

        kern_pair->glyph_ids_size = format;
        kern_pair->pair_cnt = glyph_entries;
        kern_pair->glyph_ids = glyph_ids;
        kern_pair->values = values;

        if (!mem_read(mf, glyph_ids, (size_t)ids_size)) {
            return -1;
        }
        if (!mem_read(mf, values, glyph_entries)) {
            return -1;
        }
    } else if (3 == kern_format_type) { /* 类矩阵 */
        lv_font_fmt_txt_kern_classes_t *kern_classes =
            (lv_font_fmt_txt_kern_classes_t *)lv_mem_alloc(
                sizeof(lv_font_fmt_txt_kern_classes_t));
        if (!kern_classes) return -1;
        memset(kern_classes, 0, sizeof(lv_font_fmt_txt_kern_classes_t));

        font_dsc->kern_dsc = kern_classes;
        font_dsc->kern_classes = 1;

        uint16_t kern_class_mapping_length;
        uint8_t kern_table_rows;
        uint8_t kern_table_cols;

        if (!mem_read(mf, &kern_class_mapping_length, sizeof(uint16_t)) ||
            !mem_read(mf, &kern_table_rows, sizeof(uint8_t)) ||
            !mem_read(mf, &kern_table_cols, sizeof(uint8_t))) {
            return -1;
        }

        int kern_values_length = sizeof(int8_t) * kern_table_rows * kern_table_cols;

        uint8_t *kern_left = (uint8_t *)lv_mem_alloc(kern_class_mapping_length);
        uint8_t *kern_right = (uint8_t *)lv_mem_alloc(kern_class_mapping_length);
        int8_t *kern_values = (int8_t *)lv_mem_alloc((size_t)kern_values_length);
        if (!kern_left || !kern_right || !kern_values) return -1;

        kern_classes->left_class_mapping  = kern_left;
        kern_classes->right_class_mapping = kern_right;
        kern_classes->left_class_cnt = kern_table_rows;
        kern_classes->right_class_cnt = kern_table_cols;
        kern_classes->class_pair_values = kern_values;

        if (!mem_read(mf, kern_left, kern_class_mapping_length) ||
            !mem_read(mf, kern_right, kern_class_mapping_length) ||
            !mem_read(mf, kern_values, (size_t)kern_values_length)) {
            return -1;
        }
    } else {
        return -1;
    }

    return kern_length;
}

/* ---------- 主解析（照抄 lvgl_load_font；keep=NULL 解析全量） ---------- */
static bool parse_bin_font(const uint8_t *bin, size_t size, lv_font_t *font,
                           const char *names)
{
    mem_file_t mfx = { .buf = bin, .size = size, .pos = 0 };
    mem_file_t *mf = &mfx;

    lv_font_fmt_txt_dsc_t *font_dsc = (lv_font_fmt_txt_dsc_t *)
        lv_mem_alloc(sizeof(lv_font_fmt_txt_dsc_t));
    if (!font_dsc) return false;
    memset(font_dsc, 0, sizeof(lv_font_fmt_txt_dsc_t));
    font->dsc = font_dsc;

    int32_t header_length = read_label(mf, 0, "head");
    if (header_length < 0) return false;

    font_header_bin_t font_header;
    if (!mem_read(mf, &font_header, sizeof(font_header_bin_t))) return false;

    font->base_line = -font_header.descent;
    font->line_height = font_header.ascent - font_header.descent;
    font->get_glyph_dsc = lv_font_get_glyph_dsc_fmt_txt;
    font->get_glyph_bitmap = lv_font_get_bitmap_fmt_txt;
    font->subpx = font_header.subpixels_mode;
    font->underline_position = font_header.underline_position;
    font->underline_thickness = font_header.underline_thickness;

    font_dsc->bpp = font_header.bits_per_pixel;
    font_dsc->kern_scale = font_header.kerning_scale;
    font_dsc->bitmap_format = font_header.compression_id;

    uint32_t cmaps_start = (uint32_t)header_length;
    int32_t cmaps_length = load_cmaps(mf, font_dsc, cmaps_start);
    if (cmaps_length < 0) return false;

    uint32_t loca_start = cmaps_start + (uint32_t)cmaps_length;
    int32_t loca_length = read_label(mf, loca_start, "loca");
    if (loca_length < 0) return false;

    uint32_t loca_count;
    if (!mem_read(mf, &loca_count, sizeof(uint32_t))) return false;

    bool failed = false;
    uint32_t *glyph_offset = (uint32_t *)lv_mem_alloc(
        sizeof(uint32_t) * (loca_count + 1));
    if (!glyph_offset) return false;

    if (font_header.index_to_loc_format == 0) {
        for (uint32_t i = 0; i < loca_count; ++i) {
            uint16_t offset;
            if (!mem_read(mf, &offset, sizeof(uint16_t))) {
                failed = true;
                break;
            }
            glyph_offset[i] = offset;
        }
    } else if (font_header.index_to_loc_format == 1) {
        if (!mem_read(mf, glyph_offset, loca_count * sizeof(uint32_t))) {
            failed = true;
        }
    } else {
        failed = true;
    }

    if (failed) {
        lv_mem_free(glyph_offset);
        return false;
    }

    uint32_t glyph_start = loca_start + (uint32_t)loca_length;
    uint8_t *keep = build_keep_mask(font_dsc, loca_count, names);
    int32_t glyph_length = load_glyph(mf, font_dsc, glyph_start,
                                      glyph_offset, loca_count, &font_header, keep);
    lv_mem_free(glyph_offset);
    lv_mem_free(keep);
    if (glyph_length < 0) return false;

    if (font_header.tables_count < 4) {
        font_dsc->kern_dsc = NULL;
        font_dsc->kern_classes = 0;
        font_dsc->kern_scale = 0;
        return true;
    }

    uint32_t kern_start = glyph_start + (uint32_t)glyph_length;
    int32_t kern_length = load_kern(mf, font_dsc,
                                    font_header.glyph_id_format, kern_start);
    return kern_length >= 0;
}

/* 整读 bin → 内存解析 → 释放 bin。失败返回 NULL。 */
static lv_font_t *load_font_from_file(const char *path, const char *names,
                                      bsp_sdcard_progress_cb_t cb, void *ctx)
{
    size_t sz = 0;
    uint8_t *bin = bsp_sdcard_load_file_ex(path, &sz, cb, ctx);
    if (bin == NULL) {
        ESP_LOGW(TAG, "%s 读取失败（文件缺失/SD 异常）", path);
        return NULL;
    }
    s_bins_readable = true;

    lv_font_t *font = (lv_font_t *)lv_mem_alloc(sizeof(lv_font_t));
    if (font == NULL) {
        heap_caps_free(bin);
        return NULL;
    }
    memset(font, 0, sizeof(lv_font_t));

    bool ok = parse_bin_font(bin, sz, font, names);
    heap_caps_free(bin);               /* bin 用完即释放，压峰值 */

    if (!ok) {
        ESP_LOGW(TAG, "%s 解析失败（bin 版本/LVGL 版本不匹配？）", path);
        lv_font_free(font);            /* 非空指针均按 lv_mem 分配释放 */
        return NULL;
    }
    return font;
}

/* ---------- 对外 API ---------- */
bool sd_fonts_load_for(const char *names)
{
    return sd_fonts_load_for_ex(names, NULL, NULL);
}

bool sd_fonts_load_for_ex(const char *names, bsp_sdcard_progress_cb_t cb, void *ctx)
{
    if (s_font30 && s_font16) {
        return true;                   /* 已就绪：热重载不重读 SD */
    }
    if (!bsp_sdcard_is_mounted()) {
        return false;
    }

    int64_t t0 = esp_timer_get_time();
    /* 全量字形加载（不限制白名单）：GB2312 全集 ~7542 字形均保留。
     * 白名单过滤虽省内存，但交换收到的任意中文（对方 uname/motto/name）
     * 无法预知，会大面积方块。
     * 30px（角色名/横幅大字）恢复；未来如内存紧张可改 names 子集。 */
    const char *full = NULL;
    if (s_font30 == NULL) {
        s_font30 = load_font_from_file(FONT30_PATH, full, cb, ctx);
        s_font30_src = FONT30_PATH;
        if (s_font30 == NULL) {
            /* 旧固件 SD 卡：回退 op_name_30.bin（兼容旧版 /config/fonts） */
            s_font30 = load_font_from_file(FONT30_LEGACY_PATH, full, cb, ctx);
            s_font30_src = FONT30_LEGACY_PATH;
        }
    }
    if (s_font16 == NULL) {
        s_font16 = load_font_from_file(FONT16_PATH, full, cb, ctx);
        s_font16_src = FONT16_PATH;
        if (s_font16 == NULL) {
            /* 旧固件 SD 卡：回退 op_name_16.bin（兼容旧版 /config/fonts） */
            s_font16 = load_font_from_file(FONT16_LEGACY_PATH, full, cb, ctx);
            s_font16_src = FONT16_LEGACY_PATH;
        }
    }
    int64_t dt_ms = (esp_timer_get_time() - t0) / 1000;

    bool ok = (s_font30 && s_font16);
    /* WARN 级：播放开始后全局降 WARN，成败都要在屏幕日志可见 */
    ESP_LOGW(TAG, "字库%s：30px=%s 16px=%s（%lld ms，剩余 PSRAM %u KB）%s",
             ok ? "就绪" : "未就绪",
             s_font30 ? "OK" : "缺",
             s_font16 ? "OK" : "缺",
             dt_ms,
             (unsigned)(heap_caps_get_free_size(MALLOC_CAP_SPIRAM) >> 10),
             s_bins_readable ? "" : "——卡上无字库文件，中文名将显示方块");
    return ok;
}

const lv_font_t *sd_fonts_role(int px)
{
    if (px >= SD_FONT_ROLE_LARGE) return s_font30;
    return s_font16;
}

bool sd_fonts_loaded(void)
{
    return s_font30 != NULL;
}

bool sd_fonts_fully_loaded(void)
{
    return s_font16 != NULL;
}

bool sd_fonts_bins_readable(void)
{
    return s_bins_readable;
}

void sd_fonts_unload(void)
{
    if (s_font30) {
        lv_font_free(s_font30);
        s_font30 = NULL;
    }
    if (s_font16) {
        lv_font_free(s_font16);
        s_font16 = NULL;
    }
}
