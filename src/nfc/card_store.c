/* ============================================================================
 * card_store.c - 名片存储实现（SPIFFS 后端）
 *
 * 结构：
 *   - 启动：挂载分区（失败自动格式化）→ 扫描 card_*.bin 重建内存
 *     元数据表 s_meta[]（只读各文件头，不整卡加载）→ 重写 index.json
 *   - 写入：直写 .bin 文件 + 更新内存表 + 重写 index.json
 *   - 索引：手写 JSON writer（带转义），避免引入 cJSON 依赖；
 *     index.json 永远是内存表的镜像，可随时重建（坏了也不影响 .bin）
 *
 * 可调整内容：max_files 16（SPIFFS 同时打开文件上限）；其余见 .h 头注释。
 * ==========================================================================*/

#include "card_store.h"
#include "nfc_proto.h"

#include <string.h>
#include <stdio.h>
#include <dirent.h>
#include <sys/stat.h>
#include <unistd.h>

#include "esp_log.h"
#include "esp_spiffs.h"

static const char *TAG = "card_store";

typedef struct {
    uint8_t         mac[6];
    card_blob_hdr_t hdr;
} card_meta_t;

static card_meta_t s_meta[CARD_STORE_MAX_CARDS];
static int         s_meta_cnt = 0;

static esp_err_t write_index_json(void);
static void       mac_to_hex(const uint8_t mac[6], char out[13]);
static void       card_path(char *buf, size_t buf_len, const char *hex);
static void       write_index_into(char *out, size_t max_len);

/* ---------- 挂载分区 + 重建索引 ----------
 * 功能：见文件头。首次上电分区为空白时自动格式化（format_if_mount_failed）。 */
esp_err_t card_store_init(void)
{
    esp_vfs_spiffs_conf_t conf = {
        .base_path              = CARD_STORE_MOUNT_POINT,
        .partition_label        = CARD_STORE_PARTITION,
        .max_files              = 16,
        .format_if_mount_failed = true,
    };

    esp_err_t err = esp_vfs_spiffs_register(&conf);
    if (err != ESP_OK) {
        if (err == ESP_ERR_NOT_FOUND) {
            ESP_LOGE(TAG, "partition %s not found, check partition table", CARD_STORE_PARTITION);
        } else {
            ESP_LOGE(TAG, "spiffs mount failed: %s", esp_err_to_name(err));
        }
        return err;
    }

    size_t total = 0, used = 0;
    if (esp_spiffs_info(CARD_STORE_PARTITION, &total, &used) == ESP_OK) {
        ESP_LOGI(TAG, "mounted %s: %u / %u KB used", CARD_STORE_MOUNT_POINT, (unsigned)used / 1024, (unsigned)total / 1024);
    }

    s_meta_cnt = 0;
    DIR *dir = opendir(CARD_STORE_MOUNT_POINT);
    if (dir == NULL) {
        ESP_LOGE(TAG, "opendir failed");
        return ESP_FAIL;
    }

    struct dirent *e;
    while ((e = readdir(dir)) != NULL) {
        const char *name = e->d_name;
        if (strncmp(name, "card_", 5) != 0 || strstr(name, ".bin") == NULL) {
            continue;
        }
        if (s_meta_cnt >= CARD_STORE_MAX_CARDS) {
            ESP_LOGW(TAG, "card count exceeds %d, ignoring %s", CARD_STORE_MAX_CARDS, name);
            continue;
        }
        char path[80];
        int n = snprintf(path, sizeof(path), "%s/%.*s", CARD_STORE_MOUNT_POINT, 63, name);
        if (n < 0 || n >= (int)sizeof(path)) {
            continue;
        }
        FILE *f = fopen(path, "rb");
        if (f == NULL) {
            continue;
        }
        card_meta_t *m = &s_meta[s_meta_cnt];
        memset(m, 0, sizeof(*m));
        /* 兼容旧卡：v1 hdr=87B（无 uname），v2=102B（无 portrait_len），
         * v3=104B（uname[16]）。目前 v4=152B（uname[64]）。
         * 读得短也接受（memset 保证新字段为 0，即"未配置"，UI 走回退）；
         * 字段位置 68B（uname 前）完全一致，安全。下次覆盖保存自动升级。 */
        size_t rd = fread(&m->hdr, 1, sizeof(m->hdr), f);
        if ((rd == sizeof(m->hdr) || rd == 104 || rd == 102 || rd == 87) &&
            m->hdr.magic == CARD_BLOB_MAGIC) {
            for (int i = 0; i < 6; i++) {
                char h = name[5 + i * 2], l = name[5 + i * 2 + 1];
                m->mac[i] = (uint8_t)((h <= '9' ? h - '0' : h - 'a' + 10) << 4 |
                                     (l <= '9' ? l - '0' : l - 'a' + 10));
            }
            s_meta_cnt++;
        }
        fclose(f);
    }
    closedir(dir);

    ESP_LOGI(TAG, "loaded %d cards from %s", s_meta_cnt, CARD_STORE_MOUNT_POINT);
    return write_index_json();
}

/* ---------- self.bin 读写 ----------
 * 功能：自己头像+表情包 blob（self_blob_hdr_t + 数据），发送时经
 *       espnow_nfc 的 build_self_blob 打包。 */
esp_err_t card_store_save_self(const uint8_t *data, size_t len)
{
    char path[64];
    snprintf(path, sizeof(path), "%s/self.bin", CARD_STORE_MOUNT_POINT);
    FILE *f = fopen(path, "wb");
    if (f == NULL) {
        ESP_LOGE(TAG, "open %s failed", path);
        return ESP_FAIL;
    }
    size_t written = fwrite(data, 1, len, f);
    fclose(f);
    if (written != len) {
        ESP_LOGE(TAG, "write self.bin failed (%u/%u)", (unsigned)written, (unsigned)len);
        return ESP_FAIL;
    }
    return ESP_OK;
}

esp_err_t card_store_load_self(uint8_t *buf, size_t buf_len, size_t *out_len)
{
    char path[64];
    snprintf(path, sizeof(path), "%s/self.bin", CARD_STORE_MOUNT_POINT);
    FILE *f = fopen(path, "rb");
    if (f == NULL) {
        return ESP_ERR_NOT_FOUND;
    }
    struct stat st;
    if (stat(path, &st) != 0) {
        fclose(f);
        return ESP_FAIL;
    }
    if ((size_t)st.st_size > buf_len) {
        ESP_LOGE(TAG, "self.bin too big: %ld > %u", (long)st.st_size, (unsigned)buf_len);
        fclose(f);
        return ESP_ERR_NO_MEM;
    }
    size_t n = fread(buf, 1, (size_t)st.st_size, f);
    fclose(f);
    if (out_len) {
        *out_len = n;
    }
    return n == (size_t)st.st_size ? ESP_OK : ESP_FAIL;
}

/* ---------- 名片 blob 写入（同 MAC 覆盖更新） ---------- */
esp_err_t card_store_save_card(const uint8_t mac[6], const uint8_t *data, size_t len)
{
    char hex[13], path[64];
    mac_to_hex(mac, hex);
    card_path(path, sizeof(path), hex);

    FILE *f = fopen(path, "wb");
    if (f == NULL) {
        ESP_LOGE(TAG, "open %s failed (fs full?)", path);
        return ESP_FAIL;
    }
    size_t written = fwrite(data, 1, len, f);
    fclose(f);
    if (written != len) {
        ESP_LOGE(TAG, "write %s failed (%u/%u)", path, (unsigned)written, (unsigned)len);
        unlink(path);
        return ESP_FAIL;
    }

    card_meta_t *slot = NULL;
    for (int i = 0; i < s_meta_cnt; i++) {
        if (memcmp(s_meta[i].mac, mac, 6) == 0) {
            slot = &s_meta[i];
            break;
        }
    }
    if (slot == NULL && s_meta_cnt < CARD_STORE_MAX_CARDS) {
        slot = &s_meta[s_meta_cnt++];
    }
    if (slot != NULL) {
        memcpy(slot->mac, mac, 6);
        memcpy(&slot->hdr, data, sizeof(slot->hdr));
    }
    ESP_LOGI(TAG, "saved card %s (%u bytes)", hex, (unsigned)len);
    return write_index_json();
}

esp_err_t card_store_load_card(const uint8_t mac[6], uint8_t *buf, size_t buf_len, size_t *out_len)
{
    char hex[13], path[64];
    mac_to_hex(mac, hex);
    card_path(path, sizeof(path), hex);

    FILE *f = fopen(path, "rb");
    if (f == NULL) {
        return ESP_ERR_NOT_FOUND;
    }
    struct stat st;
    if (stat(path, &st) != 0) {
        fclose(f);
        return ESP_FAIL;
    }
    if ((size_t)st.st_size > buf_len) {
        ESP_LOGE(TAG, "card too big: %ld > %u", (long)st.st_size, (unsigned)buf_len);
        fclose(f);
        return ESP_ERR_NO_MEM;
    }
    size_t n = fread(buf, 1, (size_t)st.st_size, f);
    fclose(f);
    if (out_len) {
        *out_len = n;
    }
    return n == (size_t)st.st_size ? ESP_OK : ESP_FAIL;
}

esp_err_t card_store_delete_card(const uint8_t mac[6])
{
    char hex[13], path[64];
    mac_to_hex(mac, hex);
    card_path(path, sizeof(path), hex);
    if (unlink(path) != 0) {
        return ESP_ERR_NOT_FOUND;
    }
    for (int i = 0; i < s_meta_cnt; i++) {
        if (memcmp(s_meta[i].mac, mac, 6) == 0) {
            memmove(&s_meta[i], &s_meta[i + 1], (size_t)(s_meta_cnt - i - 1) * sizeof(s_meta[0]));
            s_meta_cnt--;
            break;
        }
    }
    return write_index_json();
}

esp_err_t card_store_list(char *json_out, size_t max_len)
{
    if (json_out == NULL || max_len == 0) {
        return ESP_ERR_INVALID_ARG;
    }
    write_index_into(json_out, max_len);
    return ESP_OK;
}

static void mac_to_hex(const uint8_t mac[6], char out[13])
{
    static const char hx[] = "0123456789abcdef";
    for (int i = 0; i < 6; i++) {
        out[i * 2]     = hx[mac[i] >> 4];
        out[i * 2 + 1] = hx[mac[i] & 0xF];
    }
    out[12] = '\0';
}

static void card_path(char *buf, size_t buf_len, const char *hex)
{
    snprintf(buf, buf_len, "%s/card_%s.bin", CARD_STORE_MOUNT_POINT, hex);
}

/* ---------- JSON 转义追加（防止名字/寄语里的引号破坏 JSON） ---------- */
static void json_escape_append(char *out, size_t max_len, size_t *pos, const char *s, size_t n)
{
    static const char hx[] = "0123456789abcdef";
    for (size_t i = 0; i < n && *pos + 4 < max_len; i++) {
        char c = s[i];
        if (c == '"' || c == '\\') {
            out[(*pos)++] = '\\';
            out[(*pos)++] = c;
        } else if ((unsigned char)c < 0x20) {
            out[(*pos)++] = '\\';
            out[(*pos)++] = 'u';
            out[(*pos)++] = '0';
            out[(*pos)++] = '0';
            out[(*pos)++] = hx[((unsigned char)c) >> 4];
            out[(*pos)++] = hx[((unsigned char)c) & 0xF];
        } else {
            out[(*pos)++] = c;
        }
    }
}

static void write_index_into(char *out, size_t max_len)
{
    /* 所有追加都 clamp 到 max_len-1：snprintf/escape 一旦截断，
     * 返回"本应写"长度会把 pos 抬过 max_len → 后续 max_len-pos 下溢越界 */
    size_t pos = 0;
    int wrote = snprintf(out + pos, max_len - pos, "{\"cards\":[");
    pos = (wrote >= 0) ? pos + (size_t)wrote : 0;
    if (pos >= max_len - 2) pos = max_len - 2;
    for (int i = 0; i < s_meta_cnt; i++) {
        card_meta_t *m = &s_meta[i];
        char hex[13];
        mac_to_hex(m->mac, hex);
        wrote = snprintf(out + pos, max_len - pos, "%s{\"mac\":\"%s\",\"name\":\"", i ? "," : "", hex);
        pos = (wrote >= 0) ? pos + (size_t)wrote : 0;
        if (pos >= max_len - 2) break;
        json_escape_append(out, max_len, &pos, m->hdr.name, m->hdr.name_len);
        wrote = snprintf(out + pos, max_len - pos, "\",\"uid\":\"");
        pos = (wrote >= 0) ? pos + (size_t)wrote : 0;
        if (pos >= max_len - 2) break;
        json_escape_append(out, max_len, &pos, m->hdr.uid, m->hdr.uid_len);
        wrote = snprintf(out + pos, max_len - pos, "\",\"motto\":\"");
        pos = (wrote >= 0) ? pos + (size_t)wrote : 0;
        if (pos >= max_len - 2) break;
        json_escape_append(out, max_len, &pos, m->hdr.motto, m->hdr.motto_len);
        wrote = snprintf(out + pos, max_len - pos, "\",\"uname\":\"");
        pos = (wrote >= 0) ? pos + (size_t)wrote : 0;
        if (pos >= max_len - 2) break;
        json_escape_append(out, max_len, &pos, m->hdr.uname, m->hdr.uname_len);
        wrote = snprintf(out + pos, max_len - pos, "\",\"ts\":%lu}", (unsigned long)m->hdr.ts);
        pos = (wrote >= 0) ? pos + (size_t)wrote : 0;
        if (pos >= max_len - 2) break;
    }
    wrote = snprintf(out + pos, max_len - pos, "]}");
    pos = (wrote >= 0) ? pos + (size_t)wrote : 0;
    out[max_len - 1] = '\0';
}

/* ---------- 写 index.json ----------
 * 注意：buf 是 static（2KB 栈缓冲会让调用者任务栈溢出——本函数
 * 常被 nfc_task 的交换收尾链路嵌套调用）。代价是函数不可重入，
 * 目前所有写入路径都只发生在 nfc_task / 启动初始化，安全。 */
static esp_err_t write_index_json(void)
{
    static char buf[2048];
    write_index_into(buf, sizeof(buf));
    char path[64];
    snprintf(path, sizeof(path), "%s/index.json", CARD_STORE_MOUNT_POINT);
    FILE *f = fopen(path, "wb");
    if (f == NULL) {
        ESP_LOGE(TAG, "open %s failed", path);
        return ESP_FAIL;
    }
    size_t n = strlen(buf);
    size_t written = fwrite(buf, 1, n, f);
    fclose(f);
    return written == n ? ESP_OK : ESP_FAIL;
}
