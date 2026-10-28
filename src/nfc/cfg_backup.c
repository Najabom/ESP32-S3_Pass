/* ============================================================================
 * cfg_backup.c - 关键 NVS 设置 ⇄ SD 卡快照（见 cfg_backup.h）
 *
 * 文件格式（/sdcard/config/nvs_backup.txt，UTF-8 安全）：
 *   nfc.uid=3c6f68696c75746f...     # 键=值(hex，字符串含结尾 NUL)
 *   uiopt.m=1f
 * 恢复语义：目标 NVS 键缺失才写（不覆盖运行中的新配置）。
 * ==========================================================================*/
#include "cfg_backup.h"

#include <stdio.h>
#include <string.h>
#include <sys/stat.h>

#include "freertos/FreeRTOS.h"
#include "freertos/task.h"
#include "esp_log.h"
#include "esp_timer.h"
#include "nvs.h"
#include "bsp_sdcard.h"
#include "nfc_profile.h"

#define CFGB_TAG   "CFGB"
#define CFGB_PATH  BSP_SDCARD_MOUNT_POINT "/config/nvs_backup.txt"
#define CFGB_IVL_MS 20000

/* 备份键表 */
typedef struct {
    const char *ns;
    const char *key;
    char type;                 /* 's' = 字符串(hex，含 NUL)；'1' = u8 */
} cfgb_kv_t;

static const cfgb_kv_t S_KV[] = {
    { "nfc",     "uid",   's' },
    { "nfc",     "motto", 's' },
    { "nfc",     "uname", 's' },
    { "uiopt",   "m",     '1' },
};
#define CFGB_N (sizeof(S_KV) / sizeof(S_KV[0]))

static const char CFGB_HEX[] = "0123456789abcdef";

static void hex_encode(char *dst, const uint8_t *src, size_t n)
{
    for (size_t i = 0; i < n; i++) {
        dst[i * 2]     = CFGB_HEX[src[i] >> 4];
        dst[i * 2 + 1] = CFGB_HEX[src[i] & 0x0F];
    }
    dst[n * 2] = '\0';
}

static int hex_val(char c)
{
    if (c >= '0' && c <= '9') return c - '0';
    if (c >= 'a' && c <= 'f') return c - 'a' + 10;
    if (c >= 'A' && c <= 'F') return c - 'A' + 10;
    return -1;
}

/* 读单键 → 内存。返回：0 读到；1 键缺失；<0 错误。 */
static int read_one(const cfgb_kv_t *kv, uint8_t *val, size_t cap, size_t *out_len,
                    uint8_t *u8)
{
    nvs_handle_t h;
    if (nvs_open(kv->ns, NVS_READONLY, &h) != ESP_OK) {
        return 1;
    }
    int rc = 0;
    if (kv->type == '1') {
        uint8_t v = 0;
        esp_err_t e = nvs_get_u8(h, kv->key, &v);
        if (e == ESP_OK) {
            *u8 = v;
            rc = 0;
        } else {
            rc = 1;
        }
    } else {
        size_t len = 0;
        esp_err_t e = nvs_get_str(h, kv->key, NULL, &len);
        if (e == ESP_OK && len > 0 && len <= cap) {
            if (nvs_get_str(h, kv->key, (char *)val, &len) == ESP_OK) {
                *out_len = len;
                rc = 0;
            } else {
                rc = -1;
            }
        } else {
            rc = 1;
        }
    }
    nvs_close(h);
    return rc;
}

/* 写文件（调用方持 SD 锁）。全量重写。 */
static void dump_to_file(void)
{
    FILE *f = fopen(CFGB_PATH, "w");
    if (f == NULL) {
        ESP_LOGW(CFGB_TAG, "快照文件打开失败（跳过本轮）");
        return;
    }
    char hex[2 * 256 + 1];
    uint8_t val[256];
    size_t len = 0;
    uint8_t u8 = 0;
    for (size_t i = 0; i < CFGB_N; i++) {
        if (read_one(&S_KV[i], val, sizeof(val), &len, &u8) != 0) {
            continue;
        }
        if (S_KV[i].type == '1') {
            fprintf(f, "%s.%s=%02x\n", S_KV[i].ns, S_KV[i].key, u8);
        } else {
            hex_encode(hex, val, len);
            fprintf(f, "%s.%s=%s\n", S_KV[i].ns, S_KV[i].key, hex);
        }
    }
    fclose(f);
    ESP_LOGI(CFGB_TAG, "NVS 快照已刷新: %s", CFGB_PATH);
}

/* 恢复（调用方持 SD 锁）：键缺失才写；nfc 键顺带刷新 profile 缓存。 */
static void restore_from_file(void)
{
    FILE *f = fopen(CFGB_PATH, "r");
    if (f == NULL) {
        return;
    }
    char line[600];
    int restored = 0;
    char saved_uid[64] = { 0 }, saved_motto[64] = { 0 }, saved_uname[128] = { 0 };

    while (fgets(line, sizeof(line), f) != NULL) {
        char *p = line;
        while (*p == ' ' || *p == '\t') p++;
        char *eq = strchr(p, '=');
        if (eq == NULL) continue;
        *eq = '\0';
        const char *ns_key = p;               /* "ns.key" */
        const char *hexv = eq + 1;
        char *nl = strchr(hexv, '\n');
        if (nl) *nl = '\0';
        char *dot = strchr(ns_key, '.');
        if (dot == NULL) continue;
        *dot = '\0';
        const char *ns = ns_key;
        const char *key = dot + 1;

        /* 定位键表项 */
        const cfgb_kv_t *kv = NULL;
        for (size_t i = 0; i < CFGB_N; i++) {
            if (strcmp(S_KV[i].ns, ns) == 0 && strcmp(S_KV[i].key, key) == 0) {
                kv = &S_KV[i];
                break;
            }
        }
        if (kv == NULL) continue;

        size_t hv = strlen(hexv);
        if (hv % 2 != 0) continue;
        uint8_t raw[256];
        size_t rl = hv / 2;
        if (rl > sizeof(raw)) continue;
        bool bad = false;
        for (size_t i = 0; i < rl; i++) {
            int hi = hex_val(hexv[i * 2]);
            int lo = hex_val(hexv[i * 2 + 1]);
            if (hi < 0 || lo < 0) { bad = true; break; }
            raw[i] = (uint8_t)((hi << 4) | lo);
        }
        if (bad) continue;

        nvs_handle_t h;
        if (nvs_open(ns, NVS_READWRITE, &h) != ESP_OK) {
            continue;
        }
        esp_err_t e = ESP_OK;
        if (kv->type == '1') {
            uint8_t cur = 0;
            if (nvs_get_u8(h, key, &cur) != ESP_OK) {
                if (rl >= 1) {
                    e = nvs_set_u8(h, key, raw[0]);
                }
            }
        } else {
            size_t probe = 0;
            if (nvs_get_str(h, key, NULL, &probe) != ESP_OK) {
                /* 键缺失：blob 语义写入（含 NUL，与 nvs_get_str 兼容） */
                e = nvs_set_blob(h, key, raw, rl);
            }
        }
        if (e == ESP_OK) {
            e = nvs_commit(h);
        }
        nvs_close(h);
        if (e == ESP_OK) {
            restored++;
            if (kv->type == 's' && rl > 0) {
                if (strcmp(kv->key, "uid") == 0 && rl <= sizeof(saved_uid)) {
                    memcpy(saved_uid, raw, rl);
                } else if (strcmp(kv->key, "motto") == 0 && rl <= sizeof(saved_motto)) {
                    memcpy(saved_motto, raw, rl);
                } else if (strcmp(kv->key, "uname") == 0 && rl <= sizeof(saved_uname)) {
                    memcpy(saved_uname, raw, rl);
                }
            }
        }
    }
    fclose(f);

    /* 刷新 profile 缓存（若启动早期已生成默认 uid 则覆盖为快照值） */
    if (saved_uid[0]) {
        nfc_profile_set_uid(saved_uid);
    }
    if (saved_motto[0]) {
        nfc_profile_set_motto(saved_motto);
    }
    if (saved_uname[0]) {
        nfc_profile_set_uname(saved_uname);
    }
    if (restored > 0) {
        ESP_LOGI(CFGB_TAG, "从快照恢复 %d 项设置（刷写固件不丢配置）", restored);
    }
}

void cfg_backup_restore(void)
{
    if (!bsp_sdcard_is_mounted()) {
        return;
    }
    bsp_sdcard_lock();
    restore_from_file();
    bsp_sdcard_unlock();
}

/* 周期快照任务 */
static void cfg_backup_task(void *arg)
{
    (void)arg;
    uint32_t last_restore_check = 0;
    for (;;) {
        vTaskDelay(pdMS_TO_TICKS(CFGB_IVL_MS));
        if (!bsp_sdcard_is_mounted()) {
            continue;
        }
        /* 首次检测到卡（热插卡兜底恢复一次） */
        if (last_restore_check == 0) {
            last_restore_check = 1;
            cfg_backup_restore();
            continue;
        }
        bsp_sdcard_lock();
        /* 目录缺失（全量烧录只清 flash，SD 不动；新卡无 config）自动建 */
        mkdir(BSP_SDCARD_MOUNT_POINT "/config", 0755);
        dump_to_file();
        bsp_sdcard_unlock();
    }
}

void cfg_backup_start(void)
{
    static bool started = false;
    if (started) {
        return;
    }
    started = true;
    xTaskCreate(cfg_backup_task, "cfg_backup", 4096, NULL, 3, NULL);
    ESP_LOGI(CFGB_TAG, "快照任务已启动（%d s 周期）", CFGB_IVL_MS / 1000);
}
