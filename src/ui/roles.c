/* ============================================================================
 * roles.c - 角色动态发现（SD 卡后端）
 *
 * 功能：扫描 /sdcard 选定当前角色，提供名字与素材路径。所有 SD 访问经
 *   bsp_sdcard_lock() 持锁（与热插拔卸载互斥）。
 * 选择规则：
 *   1. /sdcard/config/current.txt（内容为文件夹名，允许首尾空白/换行）有效则用；
 *   2. 否则遍历 /sdcard 顶层目录，取字母序第一个含 loop.mjpeg 的文件夹；
 *   3. 跳过 '.' 开头的隐藏项与 "System Volume Information"。
 * 坑：
 *   - 判定"含 loop.mjpeg"用 fopen 试开，比 stat 少一次路径拼接歧义；
 *   - readdir 顺序不保证有序，"字母序第一个"必须全量遍历后取最小值；
 *   - 静态名字缓冲跨任务读（NFC 任务发卡时取名字），不做加锁——
 *     最坏情况是换角色瞬间读到半截旧名，下一次发卡即自愈。
 * ==========================================================================*/

// ui/roles.c
#include "roles.h"
#include <stdio.h>
#include <string.h>
#include <errno.h>
#include <dirent.h>
#include "esp_log.h"
#include "esp_heap_caps.h"
#include "bsp_sdcard.h"

#define PORTRAIT_SIZE 8192   // 64x64 RGB565

static const char *TAG = "ROLES";
static char s_name[ROLE_NAME_MAX] = {0};   // 空串 = 无角色

const char *roles_current_name(void) {
    return s_name[0] ? s_name : NULL;
}

void roles_clear(void) {
    s_name[0] = '\0';
}

/* ---------- 内部：拼路径并试开（调用方须已持 SD 锁） ---------- */
static bool file_exists(const char *path) {
    FILE *f = fopen(path, "rb");
    if (f) fclose(f);
    return f != NULL;
}

static void make_path(char *out, size_t cap, const char *name, const char *file) {
    snprintf(out, cap, "%s/%s/%s", BSP_SDCARD_MOUNT_POINT, name, file);
}

/* 该文件夹是否是有效角色（含 loop.mjpeg） */
static bool is_valid_role(const char *name) {
    char loop[ROLE_PATH_MAX];
    make_path(loop, sizeof(loop), name, "loop.mjpeg");
    return file_exists(loop);
}

/* ---------- 内部：读 current.txt 指定项（调用方须已持 SD 锁） ---------- */
static bool read_current_txt(char *out, size_t cap) {
    const char *path = BSP_SDCARD_MOUNT_POINT "/config/current.txt";
    FILE *f = fopen(path, "rb");
    if (f == NULL) {
        ESP_LOGW(TAG, "current.txt 读取失败: %s（已挂载=%d）", path, (int)bsp_sdcard_is_mounted());
        return false;
    }
    size_t n = fread(out, 1, cap - 1, f);
    fclose(f);
    out[n] = '\0';
    // 去首尾空白/换行
    char *start = out;
    while (*start == ' ' || *start == '\t' || *start == '\r' || *start == '\n') start++;
    size_t len = strlen(start);
    while (len > 0 && (start[len-1] == ' ' || start[len-1] == '\t' ||
                       start[len-1] == '\r' || start[len-1] == '\n')) {
        start[--len] = '\0';
    }
    if (len == 0 || len >= ROLE_NAME_MAX) return false;
    memmove(out, start, len + 1);
    return true;
}

/* ---------- 扫描并选定当前角色 ---------- */
esp_err_t roles_scan(void) {
    bsp_sdcard_lock();
    if (!bsp_sdcard_is_mounted()) {
        bsp_sdcard_unlock();
        return ESP_ERR_INVALID_STATE;
    }

    // 1) current.txt 指定优先
    char name[ROLE_NAME_MAX];
    if (read_current_txt(name, sizeof(name))) {
        if (is_valid_role(name)) {
            strncpy(s_name, name, sizeof(s_name) - 1);
            s_name[sizeof(s_name) - 1] = '\0';
            bsp_sdcard_unlock();
            ESP_LOGI(TAG, "current.txt 指定角色: %s", s_name);
            return ESP_OK;
        }
        ESP_LOGW(TAG, "current.txt 指定的 '%s' 无效（缺 loop.mjpeg？），回退自动选择", name);
    }

    // 2) 字母序第一个含 loop.mjpeg 的文件夹
    DIR *dir = opendir(BSP_SDCARD_MOUNT_POINT);
    if (dir == NULL) {
        ESP_LOGW(TAG, "opendir %s 失败: errno=%d", BSP_SDCARD_MOUNT_POINT, errno);
        bsp_sdcard_unlock();
        return ESP_FAIL;
    }
    char best[ROLE_NAME_MAX] = {0};
    struct dirent *ent;
    int found = 0;
    errno = 0;
    while ((ent = readdir(dir)) != NULL) {
        ESP_LOGI(TAG, "  dir entry: type=%d \"%s\"", (int)ent->d_type, ent->d_name);
        if (ent->d_type != DT_DIR) continue;
        if (ent->d_name[0] == '.') continue;
        if (strcmp(ent->d_name, "System Volume Information") == 0) continue;
        if (strlen(ent->d_name) >= ROLE_NAME_MAX) continue;
        if (!is_valid_role(ent->d_name)) continue;
        found++;
        if (best[0] == '\0' || strcmp(ent->d_name, best) < 0) {
            snprintf(best, sizeof(best), "%s", ent->d_name);   // 上行已保证长度 < ROLE_NAME_MAX
        }
    }
    closedir(dir);
    if (errno != 0) {
        ESP_LOGW(TAG, "readdir 中断: errno=%d（只扫到 %d 个）", errno, found);
    }
    bsp_sdcard_unlock();
    ESP_LOGI(TAG, "扫描到 %d 个有效角色文件夹，取字母序第一个", found);

    if (best[0] == '\0') {
        ESP_LOGW(TAG, "SD 卡上没有有效角色文件夹（需 <名字>/loop.mjpeg）");
        return ESP_ERR_NOT_FOUND;
    }
    strcpy(s_name, best);
    ESP_LOGI(TAG, "自动选定角色: %s", s_name);
    return ESP_OK;
}

/* ---------- 素材路径 ---------- */
esp_err_t roles_get_paths(char *intro_path, size_t intro_cap,
                              char *loop_path, size_t loop_cap) {
    if (s_name[0] == '\0') return ESP_ERR_INVALID_STATE;

    if (loop_path && loop_cap > 0) {
        make_path(loop_path, loop_cap, s_name, "loop.mjpeg");
    }
    if (intro_path && intro_cap > 0) {
        make_path(intro_path, intro_cap, s_name, "intro.mjpeg");
        bsp_sdcard_lock();
        bool has_intro = bsp_sdcard_is_mounted() && file_exists(intro_path);
        bsp_sdcard_unlock();
        if (!has_intro) intro_path[0] = '\0';
    }
    return ESP_OK;
}

/* ---------- 名片用 portrait（64x64 RGB565） ---------- */
esp_err_t roles_read_portrait(uint8_t *out) {
    if (s_name[0] == '\0') return ESP_ERR_INVALID_STATE;

    char path[ROLE_PATH_MAX];
    make_path(path, sizeof(path), s_name, "portrait.bin");

    size_t size = 0;
    uint8_t *buf = bsp_sdcard_load_file(path, &size);   // 内部已持锁
    if (buf == NULL) return ESP_ERR_NOT_FOUND;
    if (size != PORTRAIT_SIZE) {
        ESP_LOGW(TAG, "%s 尺寸 %u 不符（须 %d 字节），忽略", path,
                 (unsigned)size, PORTRAIT_SIZE);
        heap_caps_free(buf);
        return ESP_ERR_INVALID_SIZE;
    }
    memcpy(out, buf, PORTRAIT_SIZE);
    heap_caps_free(buf);
    return ESP_OK;
}

/* ---------- 触控 UI：枚举所有角色（内部持 SD 锁） ---------- */
int roles_list_all(role_entry_t *out, int max) {
    if (out == NULL || max <= 0) return 0;

    bsp_sdcard_lock();
    if (!bsp_sdcard_is_mounted()) {
        bsp_sdcard_unlock();
        return 0;
    }

    DIR *dir = opendir(BSP_SDCARD_MOUNT_POINT);
    if (dir == NULL) {
        ESP_LOGW(TAG, "opendir %s 失败: errno=%d", BSP_SDCARD_MOUNT_POINT, errno);
        bsp_sdcard_unlock();
        return 0;
    }

    int n = 0;
    struct dirent *ent;
    errno = 0;
    while ((ent = readdir(dir)) != NULL) {
        if (ent->d_type != DT_DIR) continue;
        if (ent->d_name[0] == '.') continue;
        if (strcmp(ent->d_name, "System Volume Information") == 0) continue;
        if (strlen(ent->d_name) >= ROLE_NAME_MAX) continue;
        if (!is_valid_role(ent->d_name)) continue;
        if (n < max) {
            snprintf(out[n].name, sizeof(out[n].name), "%s", ent->d_name);
            char p[ROLE_PATH_MAX];
            make_path(p, sizeof(p), ent->d_name, "portrait.bin");
            out[n].has_portrait = file_exists(p);
            n++;
        }
    }
    closedir(dir);

    /* 字母序稳定排序（选择排序，n 很小） */
    for (int i = 0; i < n - 1; i++) {
        for (int j = i + 1; j < n; j++) {
            if (strcmp(out[j].name, out[i].name) < 0) {
                role_entry_t t = out[i];
                out[i] = out[j];
                out[j] = t;
            }
        }
    }

    bsp_sdcard_unlock();
    ESP_LOGI(TAG, "枚举角色 %d 个", n);
    return n;
}

/* ---------- 显示名归一化（弯引号 → 直引号） ----------
 * UTF-8 文本中的弯引号（U+2018 ' U+2019 ' U+201C " U+201D "）转成
 * ASCII 直引号（' "），其余字节原样。显示出口（UI label / 名片发送 /
 * 字库白名单收集）都过这里，保证屏幕与 PC 端字形一致（直引号在任何
 * 字库里都有字形，弯引号则由输入法产生、字形随字体而弯）。
 * out 至少 cap 字节；结果保证 '\0' 结尾、绝不超 cap。 */
void role_text_display(const char *in, char *out, size_t cap) {
    if (out == NULL || cap == 0) return;
    if (in == NULL) in = "";
    size_t o = 0;
    const unsigned char *p = (const unsigned char *)in;
    while (*p && o + 2 < cap) {
        /* UTF-8 弯引号三元组 E2 80 98 / E2 80 99 / E2 80 9C / E2 80 9D */
        if (p[0] == 0xE2 && p[1] == 0x80 && (p[2] == 0x98 || p[2] == 0x99)) {
            out[o++] = '\'';
            p += 3;
            continue;
        }
        if (p[0] == 0xE2 && p[1] == 0x80 && (p[2] == 0x9C || p[2] == 0x9D)) {
            out[o++] = '"';
            p += 3;
            continue;
        }
        out[o++] = (char)*p++;
    }
    out[o] = '\0';
}

/* ---------- 全角色名聚合（中文字库白名单用） ---------- */
void mjpeg_role_names(char *dst, size_t cap) {
    if (cap == 0 || dst == NULL) return;
    role_entry_t entries[16];
    int cnt = roles_list_all(entries, 16);
    size_t off = 0;
    char tmp[ROLE_NAME_MAX * 2];
    for (int i = 0; i < cnt && off + 1 < cap; i++) {
        role_text_display(entries[i].name, tmp, sizeof(tmp));
        int w = snprintf(dst + off, cap - off, "%s ", tmp);
        if (w < 0) break;
        off += w;
        if (off >= cap) break;
    }
    if (off > 0 && off - 1 < cap) {
        dst[off - 1] = '\0';             /* 去掉末尾空格 */
    } else {
        dst[0] = '\0';
    }
}

/* ---------- 触控 UI：写 current.txt 切换角色 ---------- */
esp_err_t roles_set_current(const char *name) {
    if (name == NULL || name[0] == '\0') return ESP_ERR_INVALID_ARG;
    if (strlen(name) >= ROLE_NAME_MAX) return ESP_ERR_INVALID_ARG;
    for (const char *p = name; *p; p++) {
        if (*p == '/' || *p == '\\' || *p == '\r' || *p == '\n') {
            return ESP_ERR_INVALID_ARG;   // 防路径注入
        }
    }

    bsp_sdcard_lock();
    if (!bsp_sdcard_is_mounted()) {
        bsp_sdcard_unlock();
        return ESP_ERR_INVALID_STATE;
    }
    if (!is_valid_role(name)) {
        bsp_sdcard_unlock();
        ESP_LOGW(TAG, "set_current: '%s' 无效（缺 loop.mjpeg）", name);
        return ESP_ERR_NOT_FOUND;
    }
    const char *path = BSP_SDCARD_MOUNT_POINT "/config/current.txt";
    FILE *f = fopen(path, "w");
    if (f == NULL) {
        bsp_sdcard_unlock();
        ESP_LOGW(TAG, "current.txt 打开失败: %s", path);
        return ESP_FAIL;
    }
    size_t wr = fwrite(name, 1, strlen(name), f);
    fputc('\n', f);   /* 与手工编辑格式一致（读取端去空白） */
    fclose(f);
    bsp_sdcard_unlock();

    if (wr != strlen(name)) {
        ESP_LOGW(TAG, "current.txt 写入不完整");
        return ESP_FAIL;
    }
    ESP_LOGI(TAG, "current.txt 已写入: %s", name);
    return ESP_OK;
}
