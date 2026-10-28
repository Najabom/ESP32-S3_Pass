/* ============================================================================
 * mjpeg_player.c - MJPEG 视频播放任务（SD 卡数据源 + esp_new_jpeg 软解）
 *
 * 状态机（core0 优先级4 播放任务）：
 *   WAIT_SD      等 SD 挂载 → 扫描角色 → 载 loop → 再载 intro
 *   PLAY_INTRO   intro 播一遍（字库由后台 preload 任务加载，不阻塞）
 *   PLAY_LOOP    loop 无限回绕；首帧发布后揭示 UI（leftcol 浮入 + footer 闪动）
 *   任意播放态检测到卡拔出 → 释放全部缓冲、清 UI 角色名、隐藏画面 → WAIT_SD
 *
 * 字库（sd_fonts.c 内存版解析，官方 loader 算法）：bins 整读 PSRAM 快
 * （1.9MB≈4.4s），绝不阻塞播放——medias 由 preload 任务在进入播放态
 * 后后台加载（等 s_playing），完成后 refresh_fonts 让中文立即生效；
 * 失败重试 3 次，无字库卡放弃（回退内嵌 ASCII）。
 *
 * 播放链路（每帧一次，与旧内嵌版完全一致）：
 *   内存字节流 → SOI/EOI 帧切割 → JPEG 解码（先写 SRAM 中间帧，
 *   再大块搬 PSRAM）→ 字节序转换+色温（乘性暖色）→ LVGL 加锁换源 →
 *   30fps 节奏控制（自适应补时）
 *
 * 可调整内容：
 *   - WARM_B_PCT（色温，单位 1/128）：只降 B = 暖色；
 *     数值越大画面越暖黄。R 已实测不宜增益（暗部偏橙）。
 *   - 帧率：next_frame_us += 33333 即 30fps；改 25000=40fps
 *     （解码更快才达得到，达不到时自然降速，不会积压）。
 *   - WAIT_SD 轮询：未挂载 500ms、无有效角色 2s、loop 读失败 1s、
 *     intro 读失败 500ms×3。
 *   - 自定义文本：/sdcard/<角色>/ui_text.txt（与角色绑定，见 read_ui_text；
 *     键 leftcol= / sub_text= / op_name=，未配置或留空 → 对应元素不显示）。
 *   - 单文件大小上限见 bsp_sdcard.h 的 BSP_SDCARD_LOAD_MAX（PSRAM 保护）。
 * 坑：
 *   - intro/loop 不同驻 PSRAM：loop 在 WAIT_SD 阶段先行加载（intro 开始
 *     前就常驻），intro 播完直接切换零间隙；字库（SD 卡 GB2312 bin 内存
 *     解析，常驻 ≈2.7MB）由 preload 任务在播放稳定后加载，不抢播放
 *     （加载期间视频正常播、UI 先回退内嵌 ASCII，完成即刷新为 SD 字库）；
 *   - RTS 复位后 SD 卡不丢电、状态残留：首次大读常 CRC 失败 → intro
 *     失败不立即跳过，重试 3 轮（load_file 内部另有卸载重挂恢复）；
 *   - 加载耗时数秒，进入播放态前重置帧节奏（next_frame_us），否则按
 *     累计 deadline 追帧导致开头暴冲；
 *   - 卡拔出后隐藏 bg_img（LV_OBJ_FLAG_HIDDEN），重新插卡首帧发布时
 *     随 size_set 一起恢复显示。
 * ==========================================================================*/

#include <stdio.h>
#include <string.h>
#include <stdbool.h>
#include "freertos/FreeRTOS.h"
#include "freertos/task.h"
#include "esp_log.h"
#include "esp_heap_caps.h"
#include "esp_timer.h"
#include "esp_jpeg_dec.h"
#include "lvgl.h"
#include "bsp_lvgl.h"
#include "bsp_sdcard.h"
#include "roles.h"
#include "sd_fonts.h"
#include "ui_core_screen.h"
#include "ui_boot_anim.h"
#include "ui_menu.h"
#include "nfc_profile.h"
#include <sys/stat.h>

#define MJPEG_TAG      "MJPEG"

/* ---- 开机进度条连续化（boot_terminal 在 loop 读完后仍挂屏等字库 ---- *
 * 进度窗口：loop [0,54%)、intro [54,82%)、字库 [82,100%)。
 * 累计器由 WAIT_SD 在每次读文件前设 base/span，回调把局部进度映射到窗口。 */
typedef struct {
    uint32_t base;   // 本段起始百分比（0-99）
    uint32_t span;   // 本段跨度（54% loop / 28% intro / 18% 字库）
} boot_prog_t;
static boot_prog_t s_prog = {0, 0};

static void boot_progress_set(uint32_t base, uint32_t span)
{
    s_prog.base = base;
    s_prog.span = span;
}

/* 读卡进度 → 开机终端进度条（映射到累计窗口） */
static void boot_anim_progress_cb(uint32_t done, uint32_t total, void *ctx)
{
    (void)ctx;
    if (total == 0) return;
    uint64_t frac = (uint64_t)done * 100u;   // 0-100
    if (frac > (uint64_t)total * 100u) frac = (uint64_t)total * 100u;
    uint32_t pct = (uint32_t)(frac / total); // 本段 0-100
    if (s_prog.span) pct = pct * s_prog.span / 100u;
    ui_boot_anim_progress(s_prog.base + pct, 100);
}

// ---- 色温偏暖（仅视频帧，乘性偏移，开销 ~1ms/帧）----
// 只降 B 分量（暖色温本质 = 减蓝），不升 R：R 增益会使 R>G 产生"橙色"感，
// 而 R=G 时降蓝得到的是暖黄/暖灰（保持中性色相）。暗部按比例几乎不动。
// WARM_R_PCT/WARM_B_PCT 单位 1/128。
#define WARM_B_PCT 25    // B 减 ≈ -20%

typedef struct {
    const uint8_t *data;
    size_t size;
    size_t offset;
} mjpeg_reader_t;

typedef enum {
    ST_WAIT_SD,      // 等 SD 挂载 + 有效角色 + 预载完成
    ST_PLAY_INTRO,   // 开场动画（一遍）
    ST_PLAY_LOOP,    // 循环动画（无限）
} player_state_t;

/* ---------- 播放状态 ----------
 * 字库在开机终端显示期间同步加载（terminal 未 hide → 全局未降 WARN，
 * SD 读取的 64KB 进度日志实时滚进终端；字库就绪后才 hide+首帧，保证
 * 视频与中文 UI 同刻出现；热重载后 fully_loaded 幂等跳过）。 */
static volatile bool s_reload_requested = false; // 热重载请求（UI/BLE 写 current.txt 后触发）
static bool     s_ui_revealed = false;           // UI 揭示开关（eject 时重置：热插 intro 播完重新揭示）

/* ---------- ESP-NOW 交换门：视频播放满 3s 后才允许交换 ----------
 * 进入播放态（intro/loop 任一）起计 3s，交换窗口才打开；退出播放态
 * （拔卡/热重载/待机）立即关闭。播放任务单写，读方任意任务可查。 */
static volatile bool    s_playing    = false;
static volatile int64_t s_play_at_us = 0;

static void mark_playing(bool on)
{
    s_playing = on;
    s_play_at_us = on ? esp_timer_get_time() : 0;
}

/* ---------- SD 配置目录自检（每 boot 一次） ----------
 * WAIT_SD 首次备播前执行：/sdcard/config 与 config/fonts 缺失（旧卡/新卡）
 * → 自动 mkdir 重建；随后视字库文件缺失/目录被重建弹顶部提示横幅
 * （见 mjpeg_player_task 内调用点）。恢复/重建均为幂等操作。 */
static bool s_cfg_dirs_checked = false;          /* 本次 boot 已执行自检 */
static bool s_cfg_rebuilt_this_boot = false;     /* 本次 boot 重建过目录 */
static bool s_fonts_notice_shown = false;        /* 字库缺失横幅只弹一次 */
static bool s_cfg_notice_shown = false;          /* 目录重建横幅只弹一次 */

/* ui_text.txt 自定义文本（与角色绑定：源 = 当前角色目录；WAIT_SD 阶段
 * 读好，intro 结束时必然就绪）。键：leftcol ≤13 码点、sub_text ≤40 码点
 * （字库仅 ASCII）；op_name = 右下角色名（可中文，≤ROLE_NAME_MAX）。
 * 未配置或留空 → 对应 UI 元素不显示（无内置默认文案）。 */
static char s_custom_leftcol[13 * 4 + 1] = {0};
static char s_custom_sub[40 * 4 + 1] = {0};
static char s_custom_opname[ROLE_NAME_MAX] = {0};
static bool s_custom_ok = false;

void mjpeg_player_request_reload(void) {
    s_reload_requested = true;
}

/* ESP-NOW 交换门：视频进入播放态满 3s 才允许交换（加载/待机期间禁止）。
 * 任意任务可调（espnow 任务用），零阻塞。 */
bool mjpeg_player_exchange_ok(void)
{
    if (!s_playing) {
        return false;
    }
    return (esp_timer_get_time() - s_play_at_us) >= 3000000LL;
}

/* ---------- 字库加载（同步，调用方持 SD 锁语义由 bsp_sdcard 保证） ----------
 * 白名单 = 全部角色名 ∪ 本机 uid/motto/uname 档案 ∪ 已收卡片的全部文本
 * （uname/name/motto 引号字段）：横幅/名片簿显示的中文全部有字形，
 * 不依赖 SD 只存角色名的旧限制（旧版导致收卡中文显示不全）。只解析
 * 白名单内的字形，跳过 bin 其余 7500+ 字形。内部幂等：fully_loaded 直接返回。 */
static void fonts_wait_loaded(void)
{
    boot_progress_set(82, 18);   /* 进度条最后 18%：字库读+解析 */

    if (sd_fonts_fully_loaded()) {
        return;
    }
    char names[1024] = {0};
    mjpeg_role_names(names, sizeof(names));
    size_t off = strlen(names);
    /* 本机档案（横幅名/寄语文本也可能在字库白名单之外） */
    const char *profile_src[] = {
        (const char *)nfc_profile_get_uname(),
        (const char *)nfc_profile_get_motto(),
        (const char *)nfc_profile_get_uid(),
        NULL,
    };
    for (size_t i = 0; profile_src[i] != NULL && off + 1 < sizeof(names); i++) {
        const char *w = profile_src[i];
        if (!w) continue;
        int n = snprintf(names + off, sizeof(names) - off, "%s ", w);
        if (n < 0) break;
        off += n;
    }
    /* 已收卡片文本（中文字库白名单：横幅/名片簿详情显示字符串） */
    {
        FILE *f = fopen(BSP_SDCARD_MOUNT_POINT "/cards.txt", "r");
        if (f != NULL) {
            char line[192];
            while (fgets(line, sizeof(line), f) != NULL) {
                if (off + 2 >= sizeof(names)) break;
                /* 提取所有双引号字段（uname=".." name=".." motto=".."） */
                const char *p = line;
                while ((p = strchr(p, '"')) != NULL) {
                    p++;
                    const char *end = strchr(p, '"');
                    if (end == NULL || end == p) break;
                    int n = snprintf(names + off, sizeof(names) - off, "%.*s ",
                                     (int)(end - p), p);
                    if (n > 0) off += n;
                    p = end + 1;
                }
            }
            fclose(f);
        }
    }
    for (int i = 0; i < 3; i++) {
        bool ok = sd_fonts_load_for_ex(names, boot_anim_progress_cb, NULL);
        if (ok) break;
        if (!sd_fonts_bins_readable()) {
            ESP_LOGW(MJPEG_TAG, "卡上无字库文件：放弃加载（回退内嵌，中文将显示方块）");
            return;
        }
        ESP_LOGW(MJPEG_TAG, "字库加载失败（第 %d/3 次），2s 后重试", i + 1);
        vTaskDelay(pdMS_TO_TICKS(2000));
    }
    if (sd_fonts_fully_loaded()) {
        ui_core_screen_refresh_fonts();   /* 角色名/横幅字体切到 SD 字库 */
        ESP_LOGW(MJPEG_TAG, "字库已就绪：角色名/横幅切 SD 字库");
    } else {
        ESP_LOGW(MJPEG_TAG, "字库 3 次重试均失败（回退内嵌）");
    }
}

/* ---------- 按 UTF-8 码点截断 ----------
 * 功能：把 src 截断到 max_cp 个码点写入 dst（含 NUL）；含非 ASCII 字符
 *       时告警一次（leftcol/sub_text 用的 Bebas/montserrat 仅 ASCII 字形）。 */
static void truncate_utf8(char *dst, size_t dst_cap, const char *src, int max_cp) {
    size_t n = 0;
    int cp = 0;
    bool warned = false;
    const char *p = src;
    while (*p && cp < max_cp && n + 1 < dst_cap) {
        unsigned char c = (unsigned char)*p;
        size_t adv;
        if (c < 0x80) {
            adv = 1;
        } else if ((c & 0xE0) == 0xC0) {
            adv = 2;
        } else if ((c & 0xF0) == 0xE0) {
            adv = 3;
        } else if ((c & 0xF8) == 0xF0) {
            adv = 4;
        } else {
            p++;   // 非法字节跳过
            continue;
        }
        if (n + adv >= dst_cap) break;
        memcpy(dst + n, p, adv);
        n += adv;
        p += adv;
        cp++;
        if (c >= 0x80 && !warned) {
            warned = true;
            ESP_LOGW(MJPEG_TAG, "ui_text.txt 含非 ASCII 字符（leftcol/sub_text 字库仅 ASCII，将显示方块）");
        }
    }
    if (*p != '\0') {
        ESP_LOGW(MJPEG_TAG, "ui_text.txt 文本超长，截断到 %d 字符", max_cp);
    }
    dst[n] = '\0';
}

/* ---------- 弯引号归一化为直引号 ----------
 * 功能：把弯引号（‘ ’ “ ” U+2018/2019/201C/201D）原位替换成直引号
 *       （' "）。字库只含 ASCII 字形：弯引号要么走回退字形（有形状），
 *       要么缺字形；归一化后统一显示无形状直引号。UTF-8 三字节→一字节，
 *       原地收缩安全。 */
static void normalize_quotes_ascii(char *s) {
    char *w = s;
    const char *r = s;
    while (*r) {
        unsigned char c = (unsigned char)*r;
        if (c == 0xE2 && r[1] == (char)0x80 && (r[2] == (char)0x98 || r[2] == (char)0x99)) {
            *w++ = '\'';       /* U+2018/2019 → ' */
            r += 3;
        } else if (c == 0xE2 && r[1] == (char)0x80 && (r[2] == (char)0x9C || r[2] == (char)0x9D)) {
            *w++ = '"';        /* U+201C/201D → " */
            r += 3;
        } else {
            *w++ = *r++;
        }
    }
    *w = '\0';
}

/* ---------- 解析单个 ui_text.txt（须持 SD 锁调用） ----------
 * 行格式 "key=value"（key 识别 leftcol / sub_text / op_name）；去行首尾
 * 空白与 \r\n；首行兼容 UTF-8 BOM；'#' 开头为注释；其他行忽略。
 * 只填充尚未就绪的字段；空值视为"留空"（对应元素不显示）。 */
static void parse_ui_text_file(const char *path, bool *have_lb, bool *have_sub,
                               bool *have_op) {
    FILE *f = fopen(path, "r");
    if (f == NULL) {
        return;
    }

    char line[192];
    while (fgets(line, sizeof(line), f)) {
        char *p = line;
        while (*p == ' ' || *p == '\t' || *p == '\r' || *p == '\n') p++;
        if (*p == '\0' || *p == '#') continue;
        size_t len = strlen(p);
        while (len > 0 && (p[len-1] == ' ' || p[len-1] == '\t' ||
                           p[len-1] == '\r' || p[len-1] == '\n')) {
            p[--len] = '\0';
        }
        char *eq = strchr(p, '=');
        if (eq == NULL) continue;
        *eq = '\0';
        char *key = p;
        char *val = eq + 1;
        if ((unsigned char)key[0] == 0xEF && (unsigned char)key[1] == 0xBB &&
            (unsigned char)key[2] == 0xBF) {
            key += 3;   // UTF-8 BOM（Windows 记事本保存的 txt 首行常带）
        }
        normalize_quotes_ascii(val);   // 弯引号→直引号（字库仅 ASCII 字形）
        if (strcmp(key, "leftcol") == 0 && !*have_lb) {
            truncate_utf8(s_custom_leftcol, sizeof(s_custom_leftcol), val, 13);
            *have_lb = true;
        } else if (strcmp(key, "sub_text") == 0 && !*have_sub) {
            truncate_utf8(s_custom_sub, sizeof(s_custom_sub), val, 40);
            *have_sub = true;
        } else if (strcmp(key, "op_name") == 0 && !*have_op) {
            truncate_utf8(s_custom_opname, sizeof(s_custom_opname), val,
                          ROLE_NAME_MAX);
            *have_op = true;
        }
    }
    fclose(f);
}

/* ---------- 生成角色 ui_text.txt 模板（仅当不存在） ----------
 * 首次加载该角色且无文本文件时自动生成：注释逐行说明各键用途，便于
 * 用户直接编辑。op_name 预填角色文件夹名（名称元素默认即显示文件夹名）；
 * 用户清空某键 → 对应元素不显示。 */
static void ensure_role_ui_text(const char *path, const char *role_name) {
    FILE *t = fopen(path, "r");
    if (t != NULL) {
        fclose(t);   /* 已存在（含用户清空重存）：不打扰 */
        return;
    }
    FILE *f = fopen(path, "w");
    if (f == NULL) {
        ESP_LOGW(MJPEG_TAG, "ui_text 模板生成失败: %s", path);
        return;
    }
    fprintf(f,
        "# ========== 角色文本配置（本角色 ui_text.txt） ==========\n"
        "# 格式：键=值（'#' 开头为注释；删除值 = 界面不显示该元素）。\n"
        "# leftcol：左侧旋转立牌文字（≤13 字符，字库仅 ASCII）\n"
        "# sub_text：右下副标题（≤40 字符，字库仅 ASCII）\n"
        "# op_name：右下角色名（可中文；空 = 不显示）\n"
        "leftcol=\n"
        "sub_text=\n"
        "op_name=%s\n",
        role_name);
    fclose(f);
    ESP_LOGI(MJPEG_TAG, "ui_text 模板已生成: %s", path);
}

/* ---------- 读取界面自定义文本（与角色绑定） ----------
 * 源 = 当前角色目录 /sdcard/<角色>/ui_text.txt（config/ 级回退已取消）；
 * 文件缺失 → 自动生成模板并读回；全部未配置 → s_custom_ok=false，
 * 对应 UI 元素隐藏（不再有内置默认文案）。须持 SD 锁语义调用。 */
static void read_ui_text(void) {
    s_custom_ok = false;
    s_custom_leftcol[0] = '\0';
    s_custom_sub[0] = '\0';
    s_custom_opname[0] = '\0';
    if (!bsp_sdcard_is_mounted()) {
        return;
    }

    bsp_sdcard_lock();
    const char *role_name = roles_current_name();
    char path[ROLE_PATH_MAX];
    if (role_name != NULL && role_name[0] != '\0') {
        snprintf(path, sizeof(path), BSP_SDCARD_MOUNT_POINT "/%s/ui_text.txt", role_name);
        ensure_role_ui_text(path, role_name);
        bool have_lb = false;
        bool have_sub = false;
        bool have_op = false;
        parse_ui_text_file(path, &have_lb, &have_sub, &have_op);
        s_custom_ok = have_lb || have_sub || have_op;
    }
    bsp_sdcard_unlock();

    if (s_custom_ok) {
        ESP_LOGI(MJPEG_TAG,
                 "ui_text 生效(%s): leftcol=\"%s\" sub=\"%s\" op_name=\"%s\"",
                 role_name != NULL ? role_name : "?",
                 s_custom_leftcol[0] ? s_custom_leftcol : "(留空=隐藏)",
                 s_custom_sub[0] ? s_custom_sub : "(留空=隐藏)",
                 s_custom_opname[0] ? s_custom_opname : "(留空=隐藏)");
    }
}

static inline int clamp255(int v) { return v < 0 ? 0 : (v > 255 ? 255 : v); }

/* ---------- 单像素暖色变换 ----------
 * 功能：565 主机序 → 按比例降 B（暖色）→ 交换字节为大端（屏端顺序）。
 * 优化：B 通道 5bit 增益查表（含饱和钳制），每像素 1 次 SRAM 查表 +
 * 3 个位运算，替代原乘除/饱和分支（conv 8ms → ~2.5ms，fps +4-6）。 */
static uint8_t b_gain_tab[32];
static void build_warm_gains(void) {
    for (int i = 0; i < 32; i++) {
        int v = i - ((i * WARM_B_PCT) >> 7);
        b_gain_tab[i] = (uint8_t)(v < 0 ? 0 : (v > 31 ? 31 : v));
    }
}

static inline uint16_t warm_swap(uint16_t p) {
    uint16_t c = (uint16_t)((p & 0xF800) | (p & 0x07E0) | b_gain_tab[p & 0x1F]);
    return (uint16_t)((c >> 8) | (c << 8));   // 大端值
}

// 字节序转换 + 色温：小端双像素 → 大端双像素
static inline void convert_le2be32(uint32_t *dst, const uint32_t *src, int n32) {
    for (int i = 0; i < n32; i++) {
        uint32_t v = src[i];
        uint32_t w = v & 0xFFFF;          // 像素0 小端值
        uint32_t x = v >> 16;             // 像素1 小端值
        dst[i] = (uint32_t)warm_swap((uint16_t)w) | ((uint32_t)warm_swap((uint16_t)x) << 16);
    }
}

/* ---------- 帧切割 ----------
 * 功能：从当前偏移扫 SOI(FF D8)~EOI(FF D9) 得到一帧 JPEG；读到尾
 *       返回 false 且 offset 归 0（调用方据此判断"播完/回绕"）。 */
static bool get_next_frame(mjpeg_reader_t *reader, const uint8_t **out_buf, size_t *out_len) {
    // 不再自动回绕：调用方决定何时回绕。intro 因此只播一遍；
    // loop 在播到末尾后由外层手动重置 offset 实现循环。
    const uint8_t *buf = reader->data + reader->offset;
    size_t rem = reader->size - reader->offset;

    size_t start = 0;
    while (start + 1 < rem && !(buf[start] == 0xFF && buf[start + 1] == 0xD8)) start++;
    if (start + 1 >= rem) { reader->offset = 0; return false; }

    size_t end = start + 2;
    while (end + 1 < rem && !(buf[end] == 0xFF && buf[end + 1] == 0xD9)) end++;
    if (end + 1 >= rem) { reader->offset = 0; return false; }

    *out_buf = buf + start;
    *out_len = (end + 2) - start;
    reader->offset += (end + 2);
    return true;
}

/* ---------- 播放任务主体 ----------
 * 功能：见文件头状态机。pvParameters = main.c 传入的全屏 lv_img 对象。
 * 关键实现：
 *   - 双缓冲 frames[2]（PSRAM 16B 对齐，esp_new_jpeg 硬性要求）
 *   - sram_tmp（SRAM 中间帧）：解码逐像素写 PSRAM 极慢，先写 SRAM
 *     再整块 memcpy；SRAM 不足时自动回退直写 PSRAM
 *   - 尺寸变化时自动重分配（换不同分辨率视频无需重启）
 *   - intro 播完即释放，loop 由预载任务在 intro 播放期间读好：切换零间隙；
 *     拔出即全量释放 */
void mjpeg_player_task(void *pvParameters) {
    lv_obj_t *bg_img = (lv_obj_t *)pvParameters;
    ESP_LOGI(MJPEG_TAG, "player task started on core %d", xPortGetCoreID());

    build_warm_gains();   // 色温查表（32 项，µs 级，单任务内一次）

    // 错峰：BLE 广播启动后 RF 状态机与 SD(SPI3) 大读取并发会触发
    // libphy 静默卡死（SD 一次挂载成功时必现——预载立即开始读 loop.mjpeg）。
    // 等 BLE adv 完全稳定后再开始 SD 预载（预载一次性整读，完成后播放不读 SD）。
    vTaskDelay(pdMS_TO_TICKS(2000));
    ESP_LOGI(MJPEG_TAG, "player task: deferred 2s for BLE adv stable, starting");

    static lv_img_dsc_t mjpeg_dsc;
    mjpeg_dsc.header.always_zero = 0;
    mjpeg_dsc.header.cf = LV_IMG_CF_TRUE_COLOR;

    // esp_new_jpeg 解码器（先试显式 RGB565_LE 输出；若色块则 hex 日志对照）
    jpeg_dec_config_t dec_cfg = DEFAULT_JPEG_DEC_CONFIG();
    dec_cfg.output_type = JPEG_PIXEL_FORMAT_RGB565_LE;
    jpeg_dec_handle_t jdec = NULL;
    if (jpeg_dec_open(&dec_cfg, &jdec) != JPEG_ERR_OK) {
        ESP_LOGE(MJPEG_TAG, "jpeg_dec_open failed");
        vTaskDelete(NULL);
    }
    ESP_LOGI(MJPEG_TAG, "jpeg decoder ready");

    // 双缓冲 ping-pong：解码写入非显示缓冲，加锁后发布，避免与 LVGL 渲染竞争
    // esp_new_jpeg 要求输出缓冲 16 字节对齐（S3）
    uint16_t *frames[2] = { NULL, NULL };
    int frame_w = 0, frame_h = 0;
    int active = 0;          // 当前已发布给 LVGL 的缓冲下标
    bool size_set = false;   // 首帧是否已设置 img 对象尺寸/位置

    // SRAM 中间帧：解码回调逐像素写 PSRAM 慢，先写 SRAM、完成后再大块 memcpy 到 PSRAM
    uint8_t *sram_tmp = NULL;

    // 耗时仪表（每 150 帧清零重计）
    int64_t dec_us = 0, conv_us = 0, pub_us = 0;

    // ---- SD 数据源状态 ----
    player_state_t state = ST_WAIT_SD;
    uint8_t *intro_buf = NULL; size_t intro_size = 0;
    uint8_t *loop_buf  = NULL; size_t loop_size  = 0;
    mjpeg_reader_t reader = { 0 };
    bool need_eject = false;   // 坏文件等主动弹出请求（统一在循环顶部处理）

    const uint8_t *jpg_data = NULL;
    size_t jpg_len = 0;

    int64_t next_frame_us = 0;  // 下一帧 deadline（微秒）
    int64_t fps_last_us = esp_timer_get_time();
    int fps_count = 0;
    int64_t unmounted_since = -1;   // 卡未挂载起始时刻（-1=挂载中）

    while (1) {
        /* ============ WAIT_SD：等卡 → 扫描角色 → 先载 loop → 再载 intro ============
         * loop 必须先于 intro 常驻：intro 播完才能零间隙切换（intro 播放
         * 期间 SD 总线空闲，帧率也更稳）。字库不再同步等：先出画面，
         * 字库由预载任务在播放期间后台加载。 */
        if (state == ST_WAIT_SD) {
            if (!bsp_sdcard_is_mounted()) {
                ui_boot_anim_hide();          // 无卡待机：屏幕只留中央提示，终端日志停止
                /* 分型提示：连续失败时用探测结果区分"卡在但坏"（无法识别）
                 * 与"卡不在/没插好"（请插入SD卡） */
                if (bsp_sdcard_fail_streak() >= 3 && bsp_sdcard_probe_ok()) {
                    ui_core_screen_set_hint("SD卡无法识别");
                } else {
                    ui_core_screen_set_hint("请插入SD卡");
                }
                ESP_LOGI(MJPEG_TAG, "WAIT_SD: card not mounted (fail_streak=%d)",
                         bsp_sdcard_fail_streak());
                vTaskDelay(pdMS_TO_TICKS(500));
                continue;
            }
            if (roles_scan() != ESP_OK) {
                ui_boot_anim_hide();          // 无角色：同上，屏幕只留中央提示
                /* 扫描失败后二次确认卡状态：扫描耗时期间卡可能被拔
                 * （monitor 拔出检测 ~200ms），避免"刚拔卡却报无内容" */
                if (!bsp_sdcard_is_mounted()) {
                    ui_core_screen_set_hint("请插入SD卡");
                } else {
                    ui_core_screen_set_hint("SD卡无内容");
                }
                vTaskDelay(pdMS_TO_TICKS(2000));   // 卡上还没有有效角色，定期重扫
                continue;
            }
            ui_core_screen_set_hint(NULL);   // 有角色了：清除状态提示
            char intro_path[ROLE_PATH_MAX] = {0};
            char loop_path[ROLE_PATH_MAX] = {0};
            roles_get_paths(intro_path, sizeof(intro_path),
                                loop_path, sizeof(loop_path));
            ESP_LOGI(MJPEG_TAG, "准备加载：loop=%s intro=%s",
                     loop_path, intro_path[0] ? intro_path : "(none)");

            /* 进度窗口：loop 占 [0,54%)，intro 占 [54,82%)，字库 30px+16px 占 [82,100%)。
             * 无 intro 时 loop 直接推到 82%（见下 set）。 */
            boot_progress_set(0, 54);
            loop_buf = bsp_sdcard_load_file_ex(loop_path, &loop_size,
                                               boot_anim_progress_cb, NULL);
            if (loop_buf == NULL) {
                vTaskDelay(pdMS_TO_TICKS(1000));   // 读失败（卡刚被拔/坏文件），重试
                continue;
            }

            /* intro：失败不立即跳过——RTS 复位后卡不丢电、SPI 状态残留，
             * 首次大读常 CRC 失败（load_file 内部已卸载重挂恢复），这里
             * 再补 3 轮兜底；仍失败才直接播放 loop。
             * （字库已是 KB 级子集，无需再为其腾 PSRAM。） */
            if (intro_path[0]) {
                boot_progress_set(54, 28);
                for (int i = 0; i < 3 && intro_buf == NULL; i++) {
                    intro_buf = bsp_sdcard_load_file_ex(intro_path, &intro_size,
                                                        boot_anim_progress_cb, NULL);
                    if (intro_buf == NULL) {
                        ESP_LOGW(MJPEG_TAG, "intro 加载失败（第 %d/3 次），500ms 后重试", i + 1);
                        vTaskDelay(pdMS_TO_TICKS(500));
                    }
                }
                if (intro_buf != NULL) {
                    /* intro 就绪（字库已是子集常驻，无需再腾 PSRAM） */
                } else {
                    ESP_LOGW(MJPEG_TAG, "intro 重试 3 次仍失败，直接播放 loop");
                }
            }

            /* 自定义文本在 intro 加载前读好：intro 结束时 UI 文本必然就绪 */
            read_ui_text();

            /* 右下角色名 = 文本配置 op_name（模板默认预填文件夹名）；
             * 未配置/留空 → 隐藏该元素，不再显示文件夹名。 */
            ui_core_screen_set_role(s_custom_opname[0] ? s_custom_opname : NULL);
            char role_disp[ROLE_NAME_MAX * 2];
            role_text_display(roles_current_name(), role_disp, sizeof(role_disp));
            ESP_LOGI(MJPEG_TAG, "角色 %s：%s", role_disp,
                     intro_buf ? "intro 开场" : "直接 loop");

            reader.data   = intro_buf ? intro_buf : loop_buf;
            reader.size   = intro_buf ? intro_size : loop_size;
            reader.offset = 0;

            /* 固定 5s 开机终端：intro 就绪后等满时长（读卡早完则补齐
             * 剩余时间；进度条由 load_file_ex 回调实时驱动）。终端未
             * hide → 全局未降 WARN，下述 SD 读取进度会实时滚进终端。 */
            while (!ui_boot_anim_is_done()) {
                vTaskDelay(pdMS_TO_TICKS(50));
            }

            /* 字库同步加载（白名单子集，只解析角色名用字）：
             * 60-70KB 字形池常驻内存（30px+16px），读两 bin 约 10s。
             * 单 bin 读取内部硬超时 20s；卡上无 bin 立即回退内嵌。 */

            /* —— SD 配置目录自检（每 boot 一次）：config[/fonts] 缺失 →
             * 自动重建。此后字库加载/企划配置(ip.txt)写入才有落点 —— */
            if (!s_cfg_dirs_checked) {
                s_cfg_dirs_checked = true;
                bool rebuilt = false;
                if (mkdir(BSP_SDCARD_MOUNT_POINT "/config", 0755) == 0) {
                    rebuilt = true;
                }
                if (mkdir(BSP_SDCARD_MOUNT_POINT "/config/fonts", 0755) == 0) {
                    rebuilt = true;
                }
                if (rebuilt) {
                    ESP_LOGW(MJPEG_TAG,
                             "SD 配置目录缺失，已自动重建 /sdcard/config[/fonts]");
                    s_cfg_rebuilt_this_boot = true;
                }
            }

            fonts_wait_loaded();

            /* 顶部提示横幅（只提示一次；字库缺失优先，目录重建次之）。
             * ASCII 文本：无字库卡也能渲染（内嵌 montserrat）。 */
            if (!s_fonts_notice_shown && !sd_fonts_bins_readable()) {
                s_fonts_notice_shown = true;
                ui_core_screen_notice("FONT FILES MISSING",
                                      "ADD VIA PC APP", 8000);
            } else if (!s_cfg_notice_shown && s_cfg_rebuilt_this_boot) {
                s_cfg_notice_shown = true;
                ui_core_screen_notice("SD CONFIG REBUILT",
                                      "SET UP VIA PC APP", 6000);
            }

            ui_boot_anim_hide();

            state = intro_buf ? ST_PLAY_INTRO : ST_PLAY_LOOP;
            mark_playing(true);
            // 加载耗时数秒：重置节奏，防按累计 deadline 追帧暴冲
            next_frame_us = esp_timer_get_time();
            fps_last_us = next_frame_us;
            fps_count = 0;
            continue;
        }

        /* ============ 热重载请求（触控菜单/BLE 写 current.txt 后触发） ============
         * 播放中：与弹出同款清理（释放缓冲/重置揭示/重扫角色）后走 WAIT_SD
         * 重载；WAIT_SD 态：静默忽略——下轮扫描自然读取新 current.txt。
         * 开机终端首次 hide 后 is_done 恒真，WAIT_SD 的 5s 等待自动跳过。 */
        if (s_reload_requested) {
            s_reload_requested = false;
            if (state != ST_WAIT_SD) {
                ESP_LOGI(MJPEG_TAG, "热重载请求：清理缓冲后重扫角色");
                need_eject = true;   // 复用弹出清理路径（不拔卡）
            }
        }

        /* ============ 弹出处理：卡拔出 / 坏文件 ============
         * WAIT_SD 本身就在等卡，无需弹出判定。其余状态：拔出判定带
         * 2s 宽限——load_file 出错时会卸载重挂恢复（约 1.5s），期间
         * is_mounted 短暂为 false，不能当拔卡处理。真拔卡时
         * is_mounted 持续 false，宽限到点才弹出。 */
        if (state != ST_WAIT_SD) {
            if (bsp_sdcard_is_mounted()) {
                unmounted_since = -1;
            } else if (unmounted_since < 0) {
                unmounted_since = esp_timer_get_time();
            }
            if (unmounted_since >= 0 &&
                (esp_timer_get_time() - unmounted_since) > 2000000) {
                need_eject = true;
            }
        }
        if (need_eject) {
            need_eject = false;
            if (intro_buf) { heap_caps_free(intro_buf); intro_buf = NULL; intro_size = 0; }
            if (loop_buf)  { heap_caps_free(loop_buf);  loop_buf = NULL; loop_size = 0; }
            if (frames[0]) { heap_caps_free(frames[0]); frames[0] = NULL; }
            if (frames[1]) { heap_caps_free(frames[1]); frames[1] = NULL; }
            if (sram_tmp)  { heap_caps_free(sram_tmp);  sram_tmp = NULL; }
            frame_w = frame_h = 0;
            active = 0;
            size_set = false;
            s_ui_revealed = false;      // 热插 intro 播完要重新揭示 UI
            memset(&reader, 0, sizeof(reader));
            roles_clear();
            ui_core_screen_set_role(NULL);
            ui_core_screen_hide_ui();   // 隐藏立牌/footer/横幅，等重新揭示
            bsp_lvgl_lock();
            lv_obj_add_flag(bg_img, LV_OBJ_FLAG_HIDDEN);
            bsp_lvgl_unlock();
            state = ST_WAIT_SD;
            mark_playing(false);
            ESP_LOGI(MJPEG_TAG, "已弹出 → WAIT_SD");
            continue;
        }

        /* ============ 取帧（intro 播完切 loop；loop 坏帧弹出） ============ */
        if (!get_next_frame(&reader, &jpg_data, &jpg_len)) {
            if (state == ST_PLAY_INTRO) {
                // intro 播完：释放 intro 腾 PSRAM，直接切 loop（loop 在
                // WAIT_SD 阶段已先行加载完成，零间隙切换）
                heap_caps_free(intro_buf);
                intro_buf = NULL;
                intro_size = 0;
                if (loop_buf == NULL) {
                    ESP_LOGW(MJPEG_TAG, "loop 缓冲缺失，弹出重来");
                    need_eject = true;
                    continue;
                }
                reader.data   = loop_buf;
                reader.size   = loop_size;
                reader.offset = 0;
                state = ST_PLAY_LOOP;
                ESP_LOGI(MJPEG_TAG, "intro 播完 → loop");
                continue;
            }
            // loop 中找不到完整帧：回绕重试一次；仍失败视为坏文件
            reader.offset = 0;
            if (!get_next_frame(&reader, &jpg_data, &jpg_len)) {
                ESP_LOGW(MJPEG_TAG, "loop.mjpeg 无完整帧，弹出");
                need_eject = true;
                continue;
            }
        }

        /* ============ 解码 + 发布（与旧内嵌版一致） ============ */
        {
            jpeg_dec_io_t io = { 0 };
            io.inbuf = (uint8_t *)jpg_data;
            io.inbuf_len = (int)jpg_len;

            jpeg_dec_header_info_t hinfo = { 0 };
            if (jpeg_dec_parse_header(jdec, &io, &hinfo) != JPEG_ERR_OK) {
                ESP_LOGW(MJPEG_TAG, "parse_header failed");
            } else if ((int)hinfo.width != frame_w || (int)hinfo.height != frame_h) {
                // 尺寸变化：重分配双缓冲（PSRAM 16B 对齐）+ SRAM 中间帧（可选）
                size_t buf_size = (size_t)hinfo.width * hinfo.height * sizeof(uint16_t);
                uint16_t *new0 = (uint16_t *)heap_caps_aligned_alloc(16, buf_size, MALLOC_CAP_SPIRAM);
                uint16_t *new1 = (uint16_t *)heap_caps_aligned_alloc(16, buf_size, MALLOC_CAP_SPIRAM);
                uint8_t *new_tmp = (uint8_t *)heap_caps_aligned_alloc(16, buf_size, MALLOC_CAP_INTERNAL);
                if (new0 && new1) {
                    if (frames[0]) heap_caps_free(frames[0]);
                    if (frames[1]) heap_caps_free(frames[1]);
                    frames[0] = new0;
                    frames[1] = new1;
                    if (sram_tmp) heap_caps_free(sram_tmp);
                    sram_tmp = new_tmp;   // 可能为 NULL（SRAM 不足则回落直写 PSRAM）
                    frame_w = (int)hinfo.width;
                    frame_h = (int)hinfo.height;
                    active = 0;
                    size_set = false;
                    ESP_LOGI(MJPEG_TAG, "frame %dx%d, buf %d bytes x2%s", frame_w, frame_h, (int)buf_size, sram_tmp ? " + sram" : "");
                    if (!sram_tmp) {
                        ESP_LOGI(MJPEG_TAG, "sram_tmp 不可用：internal free=%d largest=%d",
                                 (int)heap_caps_get_free_size(MALLOC_CAP_INTERNAL),
                                 (int)heap_caps_get_largest_free_block(MALLOC_CAP_INTERNAL));
                    }
                } else {
                    if (new0) heap_caps_free(new0);
                    if (new1) heap_caps_free(new1);
                    if (new_tmp) heap_caps_free(new_tmp);
                    ESP_LOGE(MJPEG_TAG, "buf alloc failed (%dx%d)", hinfo.width, hinfo.height);
                }
            }

            if (frames[0] && frames[1] && frame_w == (int)hinfo.width && frame_h == (int)hinfo.height) {
                int target = 1 - active;

                size_t fb = (size_t)frame_w * frame_h * sizeof(uint16_t);
                uint8_t *dec_dst = sram_tmp ? sram_tmp : (uint8_t *)frames[target];
                io.outbuf = dec_dst;
                io.out_size = (int)fb;

                int64_t t_dec0 = esp_timer_get_time();
                if (jpeg_dec_process(jdec, &io) == JPEG_ERR_OK) {
                    dec_us += esp_timer_get_time() - t_dec0;
                    int npx = frame_w * frame_h;
                    int64_t t_conv0 = esp_timer_get_time();
                    convert_le2be32((uint32_t *)frames[target], (const uint32_t *)dec_dst, npx / 2);
                    conv_us += esp_timer_get_time() - t_conv0;

                    // 发布：加锁更新 LVGL
                    int64_t t_pub0 = esp_timer_get_time();
                    bsp_lvgl_lock();

                    if (!size_set) {
                        // 首帧：给 img 对象设置真实尺寸并对齐，否则 0x0 对象不渲染
                        lv_obj_set_size(bg_img, frame_w, frame_h);
                        lv_obj_align(bg_img, LV_ALIGN_CENTER, 0, 0);
                        lv_obj_clear_flag(bg_img, LV_OBJ_FLAG_HIDDEN);   // 热插恢复
                        size_set = true;
                    }

                    mjpeg_dsc.header.w = (uint16_t)frame_w;
                    mjpeg_dsc.header.h = (uint16_t)frame_h;
                    mjpeg_dsc.data_size = (uint32_t)fb;
                    mjpeg_dsc.data = (const uint8_t *)frames[target];

                    lv_img_cache_invalidate_src(&mjpeg_dsc);
                    lv_img_set_src(bg_img, &mjpeg_dsc);
                    lv_obj_move_background(bg_img);

                    bsp_lvgl_unlock();
                    pub_us += esp_timer_get_time() - t_pub0;
                    active = target;
                } else {
                    dec_us += esp_timer_get_time() - t_dec0;
                    ESP_LOGW(MJPEG_TAG, "decode failed");
                }
            }
        }

        // 首帧 loop 发布后揭示 UI（leftcol 浮入 + footer 闪动链），只做一次；
        // 无 intro 直接 loop 时同样在首帧触发。字库子集已在 WAIT_SD 阶段
        // （intro 载入前）同步就绪，揭示时中文必然有字形。
        if (!s_ui_revealed && state == ST_PLAY_LOOP && size_set) {
            s_ui_revealed = true;
            ui_core_screen_intro_done(
                (s_custom_ok && s_custom_leftcol[0]) ? s_custom_leftcol : NULL,
                (s_custom_ok && s_custom_sub[0]) ? s_custom_sub : NULL);
            // 校准引导：揭示完成后才拉起（开机终端/揭示动效已结束，
            // 不会被后续 z 序操作挤掉）
            ui_menu_maybe_show_cal_guide();
        }

        // 帧率统计（每 150 帧打印一次实际播放帧率 + 各阶段耗时均值）
        fps_count++;
        if (fps_count >= 150) {
            int64_t now = esp_timer_get_time();
            ESP_LOGI(MJPEG_TAG, "fps: %d | dec %dus conv %dus pub %dus",
                     (int)((fps_count * 1000000) / (now - fps_last_us)),
                     (int)(dec_us / fps_count), (int)(conv_us / fps_count),
                     (int)(pub_us / fps_count));
            fps_last_us = now;
            fps_count = 0;
            dec_us = conv_us = pub_us = 0;
        }

        // 自适应帧节奏：按 30fps（33.3ms/帧）deadline 累计，解码快则补齐余量、慢则不额外等待
        next_frame_us += 33333;
        int64_t remain_us = next_frame_us - esp_timer_get_time();
        if (remain_us > 0) {
            vTaskDelay(pdMS_TO_TICKS(remain_us / 1000));
        } else {
            // 解码超时：至少让出 1 个 tick，保证 IDLE0 有机会运行喂看门狗
            vTaskDelay(pdMS_TO_TICKS(1));
        }
    }
}
