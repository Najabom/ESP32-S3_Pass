/* ============================================================================
 * ip_config.c - 产品 IP 配置实现
 *
 * 解析 /config/ip.txt（UTF-8，# 注释，容忍 BOM/CR/LF/空白）：
 *   每行一个"产品 IP"标识（自然语言，中文/英文/日文均可）→ 本机所属
 *   IP 集合；最多 IP_CONFIG_CODE_MAX 个；空文件/全注释 = 全通。
 *   （兼容旧版 key=value：my_name=/peer= 前缀按纯标识行解析）
 * 短识别码 = FNV-1a 32 位哈希取低 16 位（ASCII 大小写归一）。
 * ==========================================================================*/

#include "ip_config.h"

#include <stdio.h>
#include <string.h>
#include <ctype.h>

#include "esp_log.h"
#include "bsp_sdcard.h"

static const char *TAG = "ip_config";

#define IP_CONFIG_PATH "/sdcard/config/ip.txt"
#define IP_CONFIG_LINE_MAX 96

static ip_config_t s_cfg;

/* ---------- 自然语言 → 短识别码 ----------
 * FNV-1a：UTF-8 逐字节参与；ASCII 字母归一为小写（"PASS"=="pass"），
 * 中文/日文等非 ASCII 字节序列原样参与（两端文本必须逐字节一致）。 */
uint16_t ip_code_from_text(const char *s)
{
    uint32_t h = 2166136261u;   /* FNV offset basis */
    const uint8_t *p = (const uint8_t *)s;
    for (; *p; p++) {
        uint8_t c = *p;
        if (c >= 'A' && c <= 'Z') {
            c = (uint8_t)(c + 32);
        }
        h ^= c;
        h *= 16777619u;         /* FNV prime */
    }
    h ^= h >> 16;
    return (uint16_t)h;
}

/* 行清理：去尾 \r\n/空白 + 去头部空白 + 跳过 BOM（EF BB BF） */
static void trim_line(char *s)
{
    size_t n = strlen(s);
    while (n > 0 && (s[n - 1] == '\r' || s[n - 1] == '\n' ||
                     s[n - 1] == ' ' || s[n - 1] == '\t')) {
        s[--n] = '\0';
    }
    if (n >= 3 && (uint8_t)s[0] == 0xEF && (uint8_t)s[1] == 0xBB &&
        (uint8_t)s[2] == 0xBF) {
        memmove(s, s + 3, n - 3 + 1);
    }
    while (*s == ' ' || *s == '\t') {
        s++;
    }
}

void ip_config_load(void)
{
    bsp_sdcard_lock();

    FILE *f = fopen(IP_CONFIG_PATH, "r");
    if (f == NULL) {
        bsp_sdcard_unlock();
        /* 无文件：保持上次配置（首次 = 全 0 全通），仅首次提示 */
        if (s_cfg.cnt == 0) {
            ESP_LOGI(TAG, "no %s: all devices exchange (no IP isolation)",
                     IP_CONFIG_PATH);
        }
        return;
    }

    ip_config_t next;
    memset(&next, 0, sizeof(next));

    char line[IP_CONFIG_LINE_MAX];
    while (fgets(line, sizeof(line), f) != NULL) {
        trim_line(line);
        if (line[0] == '\0' || line[0] == '#') {
            continue;
        }
        /* 兼容旧版 my_name=/peer= 前缀（剥掉后按纯标识行解析） */
        char *word = line;
        char *eq = strchr(line, '=');
        if (eq != NULL) {
            *eq = '\0';
            if (strcmp(line, "my_name") == 0 || strcmp(line, "peer") == 0) {
                word = eq + 1;
                while (*word == ' ' || *word == '\t') {
                    word++;
                }
            } else {
                continue;   /* 未知 key= 行：跳过 */
            }
        }
        if (word[0] == '\0') {
            continue;
        }
        if (next.cnt >= IP_CONFIG_CODE_MAX) {
            ESP_LOGW(TAG, "too many IP identifiers (max %d), ignoring '%s'",
                     IP_CONFIG_CODE_MAX, word);
            continue;
        }
        uint16_t code = ip_code_from_text(word);
        /* 去重（同码/同词不重复计，保持集合语义） */
        bool dup = false;
        for (int i = 0; i < next.cnt; i++) {
            if (next.codes[i] == code) {
                dup = true;
                break;
            }
        }
        if (!dup) {
            next.codes[next.cnt++] = code;
        }
    }
    fclose(f);
    bsp_sdcard_unlock();

    s_cfg = next;

    if (s_cfg.cnt > 0) {
        char buf[96] = "";
        int p = 0;
        for (int i = 0; i < s_cfg.cnt && p + 6 < (int)sizeof(buf); i++) {
            p += snprintf(buf + p, sizeof(buf) - (size_t)p, "%s%04x",
                          i ? "," : "", (unsigned)s_cfg.codes[i]);
        }
        ESP_LOGI(TAG, "ip config: %d code(s) [%s]", s_cfg.cnt, buf);
    } else {
        ESP_LOGW(TAG, "ip.txt exists but empty: all devices exchange (no IP isolation)");
    }
}

const ip_config_t *ip_config_get(void)
{
    return &s_cfg;
}
