/* ============================================================================
 * card_log.c - 收卡记录（SD 卡）
 *
 * 功能：见 card_log.h。txt 追加与 bin 备份在同一次 SD 锁持有期内完成，
 *   防止写到一半被热插拔监控任务卸载。
 * 坑：
 *   - 行号 #NNNN 靠每次追加前扫行数得到（文件有序追加、无删除，行数即
 *     下一条序号）；文件很大时扫描开销线性增长，但名片场景（几十~
 *     几百条）完全无感；
 *   - card_<mac>.bin 文件名 17 字符超 FAT 8.3，依赖 sdkconfig 开的
 *     LFN 长文件名；LFN 未生效时 bin 写入会失败（txt 不受影响），
 *     日志会明确指出；
 *   - mkdir 已存在返回 -1/EEXIST，属正常路径，不当错误处理；
 *   - 调用上下文是 nfc_task（栈 8192）：本函数栈占用 < 400B，安全。
 * ==========================================================================*/

// nfc/card_log.c
#include "card_log.h"
#include "nfc_proto.h"
#include "bsp_sdcard.h"
#include "bsp_power.h"
#include <stdio.h>
#include <string.h>
#include <dirent.h>
#include <unistd.h>
#include <sys/stat.h>
#include "esp_log.h"
#include "time_sync.h"

static const char *TAG = "CARD_LOG";

#define CARDS_TXT_PATH  BSP_SDCARD_MOUNT_POINT "/cards.txt"
#define CARDS_DIR_PATH  BSP_SDCARD_MOUNT_POINT "/cards"

/* ---------- 字段净化：| 与换行会破坏行格式，替换为空格 ---------- */
static void sanitize(char *s, const char *src, uint16_t len) {
    for (uint16_t i = 0; i < len; i++) {
        char c = src[i];
        s[i] = (c == '|' || c == '\r' || c == '\n') ? ' ' : c;
    }
    s[len] = '\0';
}

/* ---------- 统计 cards.txt 现有行数（= 下一条序号-1） ---------- */
static long count_lines(FILE *f) {
    long n = 0;
    int c;
    while ((c = fgetc(f)) != EOF) {
        if (c == '\n') n++;
    }
    return n;
}

esp_err_t card_log_save(const uint8_t mac[6], const uint8_t *blob, uint32_t total,
                        uint16_t peer_ip_code) {
    if (mac == NULL || blob == NULL || total < sizeof(card_blob_hdr_t)) {
        return ESP_ERR_INVALID_ARG;
    }
    const card_blob_hdr_t *hdr = (const card_blob_hdr_t *)blob;

    bsp_sdcard_lock();
    if (!bsp_sdcard_is_mounted()) {
        bsp_sdcard_unlock();
        ESP_LOGW(TAG, "SD 未挂载，跳过记录（横幅显示不受影响）");
        return ESP_ERR_INVALID_STATE;   // 无卡：跳过记录（横幅显示不受影响）
    }
    /* 危急低电保护：bsp_power 判定电池濒临耗尽 → 停止写卡（下次上电恢复） */
    if (!bsp_power_storage_ok()) {
        bsp_sdcard_unlock();
        ESP_LOGW(TAG, "电量危急，跳过写卡保护数据");
        return ESP_ERR_INVALID_STATE;
    }

    // 确保备份目录存在（首次收卡时）
    mkdir(CARDS_DIR_PATH, 0755);

    /* ---- 1) 完整 bin 备份（同 MAC 覆盖写，保留最新一张） ---- */
    char bin_path[64];
    snprintf(bin_path, sizeof(bin_path),
             CARDS_DIR_PATH "/card_%02x%02x%02x%02x%02x%02x.bin",
             mac[0], mac[1], mac[2], mac[3], mac[4], mac[5]);
    esp_err_t bin_ret = ESP_OK;
    FILE *fb = fopen(bin_path, "wb");
    if (fb == NULL) {
        bin_ret = ESP_FAIL;
        ESP_LOGW(TAG, "%s 打开失败（LFN 未生效？需长文件名支持）", bin_path);
    } else {
        size_t wr = fwrite(blob, 1, total, fb);
        fclose(fb);
        if (wr != total) {
            bin_ret = ESP_FAIL;
            ESP_LOGW(TAG, "%s 写入不完整（%u/%lu）", bin_path, (unsigned)wr,
                     (unsigned long)total);
        }
    }

    /* ---- 2) txt 追加一行整齐记录 ---- */
    char uname[CARD_UNAME_MAX + 1], name[CARD_NAME_MAX + 1], motto[CARD_MOTTO_MAX + 1];
    sanitize(uname, hdr->uname, hdr->uname_len <= CARD_UNAME_MAX ? hdr->uname_len : CARD_UNAME_MAX);
    sanitize(name,  hdr->name,  hdr->name_len  <= CARD_NAME_MAX  ? hdr->name_len  : CARD_NAME_MAX);
    sanitize(motto, hdr->motto, hdr->motto_len <= CARD_MOTTO_MAX ? hdr->motto_len : CARD_MOTTO_MAX);

    esp_err_t txt_ret = ESP_OK;
    FILE *ft = fopen(CARDS_TXT_PATH, "r+");   // 读行数 + 追加
    if (ft == NULL) ft = fopen(CARDS_TXT_PATH, "w+");   // 首次创建
    if (ft == NULL) {
        txt_ret = ESP_FAIL;
        ESP_LOGW(TAG, "%s 打开失败", CARDS_TXT_PATH);
    } else {
        long seq = count_lines(ft) + 1;
        unsigned int tv = (unsigned int)hdr->ts;
        fseek(ft, 0, SEEK_END);
        fprintf(ft, "#%04ld | up=%04u | mac=%02x:%02x:%02x:%02x:%02x:%02x"
                    " | ip=%04x"
                    " | uname=\"%s\" | name=\"%s\" | uid=%.*s | motto=\"%s\""
                    " | avatar=%u | portrait=%u | stk=%u\n",
                seq, tv,
                mac[0], mac[1], mac[2], mac[3], mac[4], mac[5],
                (unsigned)peer_ip_code,
                uname, name,
                (int)(hdr->uid_len <= CARD_UID_MAX ? hdr->uid_len : CARD_UID_MAX),
                (const char *)hdr->uid,
                motto,
                (unsigned)hdr->avatar_len, (unsigned)hdr->portrait_len,
                (unsigned)hdr->sticker_cnt);
        fclose(ft);
    }
    bsp_sdcard_unlock();

    if (txt_ret == ESP_OK && bin_ret == ESP_OK) {
        ESP_LOGI(TAG, "已记录名片 %02x:%02x:%02x:%02x:%02x:%02x → cards.txt + bin",
                 mac[0], mac[1], mac[2], mac[3], mac[4], mac[5]);
        return ESP_OK;
    }
    return (txt_ret != ESP_OK) ? txt_ret : bin_ret;
}

/* ---------- 收卡记录时间平移（time_sync weak hook 的强定义）----------
 * PC 绝对对齐墙钟后，cards.txt 存量行的 up=<秒> 整体平移同一变化量，
 * 使历史记录与新时基保持一致的相对关系。bin 备份内的 hdr.ts 为对方
 * 设备打包时间，不属于本地时基，不平移。
 * 实现：SD 锁内整读 → 逐行定位 "| up=" 数字段替换 → 整文件重写。 */
void card_log_shift_hook(int64_t delta_s)
{
    bsp_sdcard_lock();
    do {
        if (!bsp_sdcard_is_mounted()) {
            break;
        }
        FILE *f = fopen(CARDS_TXT_PATH, "rb");
        if (f == NULL) {
            break;                       /* 无记录文件：无事可做 */
        }
        fseek(f, 0, SEEK_END);
        long sz = ftell(f);
        fseek(f, 0, SEEK_SET);
        if (sz < 8) {
            fclose(f);
            break;
        }
        char *src = malloc((size_t)sz + 2);
        if (src == NULL) {
            fclose(f);
            ESP_LOGW(TAG, "shift: OOM");
            break;
        }
        size_t rd = fread(src, 1, (size_t)sz, f);
        fclose(f);
        src[rd] = '\0';

        /* 平移后数字位数最多增 3（999→1000），给 25% 余量 */
        char *dst = malloc(rd + rd / 4 + 32);
        if (dst == NULL) {
            free(src);
            ESP_LOGW(TAG, "shift: OOM");
            break;
        }

        size_t o = 0;
        int cnt = 0;
        const char *p = src;
        while (*p) {
            const char *eol = strchr(p, '\n');
            size_t linelen = eol ? (size_t)(eol - p + 1) : strlen(p);
            const char *u = NULL;
            for (size_t i = 0; i + 5 <= linelen; i++) {
                if (memcmp(p + i, "| up=", 5) == 0) {
                    u = p + i + 5;
                    break;
                }
            }
            if (u) {
                char *endp;
                unsigned long v = strtoul(u, &endp, 10);
                /* 只平移未迁移的相对时基旧行（boot 秒）；已是绝对 unix 的行
                 * （含 v4 会话基准）永不触碰——否则每次连接校时的 ±1~2s 抖动
                 * 会把全表累计平移，双端记录将永远无法逐字相等 */
                if (v >= 1000000000UL) {
                    memcpy(dst + o, p, linelen);
                    o += linelen;
                    p += linelen;
                    continue;
                }
                long long nv = (long long)v + delta_s;
                if (nv < 1) {
                    nv = 1;
                }
                memcpy(dst + o, p, (size_t)(u - p));
                o += (size_t)(u - p);
                o += (size_t)snprintf(dst + o, 16, "%llu",
                                      (unsigned long long)nv);
                cnt++;
                size_t restlen = linelen - (size_t)(endp - p);
                memcpy(dst + o, endp, restlen);
                o += restlen;
            } else {
                memcpy(dst + o, p, linelen);
                o += linelen;
            }
            p += linelen;
        }
        dst[o] = '\0';
        free(src);

        FILE *w = fopen(CARDS_TXT_PATH, "wb");
        esp_err_t ret = ESP_OK;
        if (w == NULL || fwrite(dst, 1, o, w) != o) {
            ret = ESP_FAIL;
        }
        if (w) {
            fclose(w);
        }
        free(dst);

        if (ret == ESP_OK && cnt > 0) {
            ESP_LOGI(TAG, "cards.txt 时间平移 %+lld s（%d 行）",
                     (long long)delta_s, cnt);
        } else if (ret != ESP_OK) {
            ESP_LOGW(TAG, "cards.txt 平移写入失败");
        }
    } while (0);
    bsp_sdcard_unlock();
}

esp_err_t card_log_clear(void)
{
    if (!bsp_sdcard_is_mounted()) {
        return ESP_ERR_INVALID_STATE;
    }
    bsp_sdcard_lock();
    esp_err_t ret = ESP_OK;
    unlink(CARDS_TXT_PATH);           /* 记录不存在也算成功 */
    DIR *d = opendir(CARDS_DIR_PATH);
    if (d != NULL) {
        struct dirent *e;
        while ((e = readdir(d)) != NULL) {
            if (e->d_name[0] == '.') {
                continue;
            }
            char path[288];
            snprintf(path, sizeof(path), "%s/%s", CARDS_DIR_PATH, e->d_name);
            if (unlink(path) != 0) {
                ret = ESP_FAIL;
            }
        }
        closedir(d);
    }
    bsp_sdcard_unlock();
    ESP_LOGW(TAG, "交换历史已清空（cards.txt + cards/ 目录）");
    return ret;
}
