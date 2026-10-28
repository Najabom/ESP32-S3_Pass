/* ============================================================================
 * espnow_nfc.c - ESP-NOW 名片交换引擎（状态机 + 冷却 + 广播）
 *
 * 会话流程（两台同类设备靠近时）：
 *   ① 双方空闲时周期广播 HELLO（200ms±10%，带特征码）
 *   ② 收到对方 HELLO → 回 HELLO_ACK；MAC 较小者拿到 HELLO_ACK 后
 *      成为"主"，主动开始发自己的名片（字段 → 分片停等）
 *   ③ 主发完发 DONE；从收完后开始发自己的 → 主收到 DONE 校验完整
 *   ④ 双方 complete_session：落盘 + 回调 UI 弹横幅 + 记录冷却
 *
 * 冷却（按对 NVS 持久化，"对"=对方 MAC）：
 *   成功 → 5 分钟冷静期；当日（墙钟日）成功 ≥3 次 → 当日封禁；
 *   失败/中断不计数。目的：避免两人长时间同处时反复弹横幅写闪存。
 *   时间源：time_sync（手机 BLE 校准的墙钟秒；未校准 = RTC 域相对秒，
 *   断电清零 → 断电即"新的一天"，常供电桌面设备符合直觉）。
 *
 * 可调整内容（集中在本文件宏与 nfc_proto.h）：
 *   - SESS_TIMEOUT_S 5：无包事件多久放弃会话（擦肩场景 ~1.5s 内完成）
 *   - CD_COOL_S 300 / CD_MAX_COUNT 3：冷却两参数，
 *     口径=用户需求（5 分钟冷静、当天 3 次封禁至次日）
 *   - NFC_HELLO_PERIOD_US/JITTER、分片与重传参数：见 nfc_proto.h
 * ==========================================================================*/

#include "espnow_nfc.h"
#include "nfc_profile.h"
#include "card_store.h"
#include "card_log.h"
#include "ip_config.h"
#include "time_sync.h"
#include "../ui/roles.h"
#include "../mjpeg_player/mjpeg_player.h"   /* 交换门：播放满 3s 才允许交换 */
#include "../resources/default_avatar.h"

#include <string.h>
#include <stdio.h>

#include "freertos/FreeRTOS.h"
#include "freertos/task.h"
#include "freertos/queue.h"

#include "esp_log.h"
#include "esp_wifi.h"
#include "esp_now.h"
#include "esp_random.h"
#include "esp_timer.h"
#include "esp_mac.h"
#include "esp_netif.h"
#include "esp_event.h"
#include "nvs.h"

static const char *TAG = "espnow_nfc";

/* ============ 会话状态 ============
 * 同一时刻最多一个会话（单对单交换）；phase 表示本机当前角色/阶段。 */

typedef enum {
    PHASE_IDLE = 0,
    PHASE_MASTER_SEND,   /* 主：发自己名片中 */
    PHASE_MASTER_RECV,   /* 主：已发完，收对方名片中 */
    PHASE_SLAVE_RECV,    /* 从：收主名片中 */
    PHASE_SLAVE_SEND,    /* 从：收完后发自己名片 */
} phase_t;

typedef struct {
    bool     active;
    phase_t  phase;
    uint8_t  peer[6];
    uint32_t last_event_s;

    uint8_t *tx_blob;
    uint32_t tx_total;
    uint32_t tx_off;
    uint8_t  tx_seq;
    uint8_t  retries;
    uint64_t last_tx_us;

    nfc_field_t field;
    uint8_t *rx_blob;
    uint32_t rx_offset;
    uint8_t  rx_expected_seq;
    uint16_t matched_ip;    /* 本会话命中（对方侧）的产品 IP 码；详情落盘用 */
    uint32_t sess_ts;       /* 会话基准时间（v4）：主机=自己打包时刻，从机=借主机的 */
} session_t;

typedef struct {
    uint8_t  src[6];
    size_t   len;
    uint8_t  data[];
} rx_item_t;

static session_t          s_sess;
static QueueHandle_t      s_rx_queue = NULL;
static nfc_recv_card_cb_t s_recv_cb = NULL;
static uint8_t            s_my_mac[6];
static uint64_t           s_next_hello_us = 0;
static uint16_t           s_hello_matched = 0;  /* 最近一次握手接受的对方 IP 码（会话开始前有效） */

#define SESS_TIMEOUT_S 10

/* ---------- 冷却（按对，NVS 持久化）----------
 * 存储键 pair_<对方MAC小写hex>，blob 为 pair_cd_t（长度不符视为无记录，
 * 旧版 16B 结构自动失效）。
 * 规则（见文件头）：
 *   冷静期：last_ok 后 CD_COOL_S 内不响应对方
 *   当日封禁：day==今日 且 cnt 达 CD_MAX_COUNT → 当日不再交换
 * 冷却只挡"交换发起"，收到过名片后短时再遇仍会拒绝（防反复触发）。
 * 实现注意：NVS 写失败（分区满/坏页）不允许影响冷却功能——内存镜像
 * 兜底（static cache），同一 boot 内永远有效；NVS 仅作跨断电备份。 */

#define CD_NS        "nfccd"
/* 【测试临时】置 1 = 关闭冷静期与当日封禁，便于反复交换测试；
 * 测试完成后改回 0 即恢复正式限制（勿删除本开关）。 */
#define WL_TEST_RELAX_COOLDOWN 0
#if WL_TEST_RELAX_COOLDOWN
#define CD_COOL_S    0UL
#define CD_MAX_COUNT 0
#else
#define CD_COOL_S    300UL   /* 正式：冷静期 5 分钟 */
#define CD_MAX_COUNT 5       /* 正式：当日封禁次数上限 */
#endif
/* 未校准（无墙钟）时段：累计已达 CD_MAX_COUNT 后的"冷静下限"——
 * 不作当日封禁（day 恒 0 永不变化 → 永久锁死），只要求再等待
 * 3 分钟；断电重启后 last_ok_ms 归零，自然放行。 */
#define CD_BOOT_COOL_MS (180UL * 1000UL)

typedef struct {
    uint32_t last_ok;   /* 上次成功交换时间（time_sync 秒，墙钟校准后有效） */
    uint32_t last_ok_ms;/* 上次成功交换时刻（esp_timer 单调毫秒，断电无关） */
    uint32_t day;       /* 计数所属"日"（秒/86400） */
    uint32_t cnt;       /* 当日成功交换次数 */
} pair_cd_t;

/* 内存镜像：NVS 只读时先查镜像，NVS 写失败时镜像兜底（boot 有效） */
#define CD_MIRROR_MAX 8
static uint8_t  s_cd_keys[CD_MIRROR_MAX][25];
static pair_cd_t s_cd_mirror[CD_MIRROR_MAX];
static int      s_cd_mirror_cnt = 0;

static uint32_t now_s(void)
{
    return time_sync_now();
}

static uint32_t now_ms(void)
{
    return (uint32_t)(esp_timer_get_time() / 1000ULL);
}

static void cd_key(char *out, size_t len, const uint8_t mac[6])
{
    /* NVS 键名上限 15 字符：前缀用 "p"（无 pair_），12 hex = 13 字符 ✓ */
    static const char hx[] = "0123456789abcdef";
    int p = snprintf(out, len, "p");
    for (int i = 0; i < 6 && p + 2 < (int)len; i++) {
        out[p++] = hx[mac[i] >> 4];
        out[p++] = hx[mac[i] & 0xF];
    }
    out[p] = '\0';
}

static int cd_mirror_find(const uint8_t mac[6])
{
    char key[24];
    cd_key(key, sizeof(key), mac);
    for (int i = 0; i < s_cd_mirror_cnt; i++) {
        if (strcmp((const char *)s_cd_keys[i], key) == 0) {
            return i;
        }
    }
    return -1;
}

static void cd_mirror_set(const uint8_t mac[6], const pair_cd_t *cd)
{
    int idx = cd_mirror_find(mac);
    if (idx < 0) {
        if (s_cd_mirror_cnt >= CD_MIRROR_MAX) return; /* 容量小，仅新 MAC 才加 */
        char key[24];
        cd_key(key, sizeof(key), mac);
        idx = s_cd_mirror_cnt++;
        snprintf((char *)s_cd_keys[idx], sizeof(s_cd_keys[idx]), "%s", key);
    }
    memcpy(&s_cd_mirror[idx], cd, sizeof(pair_cd_t));
}

static esp_err_t cd_load(const uint8_t mac[6], pair_cd_t *cd)
{
    char key[24];
    cd_key(key, sizeof(key), mac);
    memset(cd, 0, sizeof(*cd));
    /* 先查内存镜像：NVS 写失败但镜像有值（本 boot 秒数），直接返回 */
    int mi = cd_mirror_find(mac);
    if (mi >= 0) {
        memcpy(cd, &s_cd_mirror[mi], sizeof(*cd));
        return ESP_OK;
    }
    nvs_handle_t h;
    if (nvs_open(CD_NS, NVS_READONLY, &h) != ESP_OK) {
        return ESP_FAIL;
    }
    size_t len = sizeof(*cd);
    esp_err_t err = nvs_get_blob(h, key, cd, &len);
    nvs_close(h);
    if (err != ESP_OK || len != sizeof(*cd)) {
        return ESP_FAIL;   /* 无记录或旧版结构：按无冷却处理 */
    }
    /* 读到后将镜像更新（下次 NVS 失败仍有值） */
    cd_mirror_set(mac, cd);
    return ESP_OK;
}

static esp_err_t cd_save(const uint8_t mac[6], const pair_cd_t *cd)
{
    cd_mirror_set(mac, cd);   /* 内存镜像兜底：NVS 写失败也不影响本 boot 冷却 */
    char key[24];
    cd_key(key, sizeof(key), mac);
    nvs_handle_t h;
    esp_err_t err = nvs_open(CD_NS, NVS_READWRITE, &h);
    if (err != ESP_OK) {
        ESP_LOGW(TAG, "nvs_open FAIL: %s（内存镜像兜底）", esp_err_to_name(err));
        return err;
    }
    err = nvs_set_blob(h, key, cd, sizeof(*cd));
    if (err == ESP_OK) {
        err = nvs_commit(h);
        if (err != ESP_OK) {
            ESP_LOGW(TAG, "nvs_commit FAIL: %s（内存镜像兜底）", esp_err_to_name(err));
        }
    }
    nvs_close(h);
    return err;
}

esp_err_t espnow_nfc_clear_cooldown(void)
{
    s_cd_mirror_cnt = 0;   /* 清内存镜像 */
    nvs_handle_t h;
    esp_err_t err = nvs_open(CD_NS, NVS_READWRITE, &h);
    if (err != ESP_OK) {
        return err;
    }
    err = nvs_erase_all(h);
    if (err == ESP_OK) {
        err = nvs_commit(h);
    }
    nvs_close(h);
    ESP_LOGW(TAG, "交换冷却 NVS 已清空");
    return err;
}

/* ---------- 检查该对设备当前是否被冷却拦截 ---------- */
static bool pair_blocked(const uint8_t mac[6])
{
    const uint32_t max_cnt = CD_MAX_COUNT;
    pair_cd_t cd;
    if (cd_load(mac, &cd) != ESP_OK) {
        /* 无记录（首遇）：镜像/NVS 都无则放行 */
        ESP_LOGI(TAG, "cool-no-cd %02x%02x%02x%02x%02x%02x (first time)",
                 mac[0], mac[1], mac[2], mac[3], mac[4], mac[5]);
        return false;
    }
    /* 冷静期：5 分钟内同一对端不重复交换。
     * 墙钟优先（time_sync_has_wall）：经 PC 校准后跨断电有效；
     * 未校准（RTC 相对秒上电归零）退用 esp_timer 单调毫秒。
     * last_ok_ms 持久化 NVS；断电后 esp_timer 归零 → 与 last_ok_ms
     * 必须用有符号差：diff_ms >= 0 才进入冷静期；diff<0（时钟回绕/
     * 重启）时若墙钟难比则放行一次（避免永久锁死）。 */
    if (time_sync_has_wall()) {
        uint32_t now = now_s();
        int64_t diff = (int64_t)now - (int64_t)cd.last_ok;
        ESP_LOGI(TAG, "cool-check %02x%02x%02x%02x%02x%02x: now=%lu last_ok=%lu"
                        " cool=%u (diff=%lld) has_wall=1",
                 mac[0], mac[1], mac[2], mac[3], mac[4], mac[5],
                 (unsigned long)now, (unsigned long)cd.last_ok,
                 (unsigned)CD_COOL_S, (long long)diff);
        if (diff >= 0 && diff < (int64_t)CD_COOL_S) {
            return true;                     /* 墙钟冷静期（跨断电） */
        }
        if (max_cnt > 0 && cd.day == time_sync_local_day() && cd.cnt >= max_cnt) {
            return true;                     /* 当日已达 5 次：封禁至次日 */
        }
    } else {
        /* 未校准：墙钟/"日"语义不可用（RTTC 相对秒断电归零，day 恒 0），
         * 禁止"当日封禁"（否则 day 永不变 → 一旦达 5 次永久锁死交换）。
         * 改用"本上电周期"计数：cnt >= CD_MAX_COUNT 时仅追加冷静期下限
         * （3 分钟），断电重启后 last_ok_ms 归零即放行。 */
        if (max_cnt > 0 && cd.cnt >= max_cnt && cd.day == 0) {
            int64_t diff_ms = (int64_t)now_ms() - (int64_t)cd.last_ok_ms;
            ESP_LOGI(TAG, "cool-boot-count %02x%02x%02x%02x%02x%02x: cnt=%u diff_ms=%lld",
                     mac[0], mac[1], mac[2], mac[3], mac[4], mac[5],
                     (unsigned)cd.cnt, (long long)diff_ms);
            if (diff_ms >= 0 && diff_ms < (int64_t)CD_BOOT_COOL_MS) {
                return true;                 /* 本上电周期已超 5 次：再冷静 3 分钟 */
            }
        }
        int64_t diff_ms = (int64_t)now_ms() - (int64_t)cd.last_ok_ms;
        if (diff_ms >= 0 && diff_ms < (int64_t)CD_COOL_S * 1000) {
            return true;                     /* 本上电周期冷静期 */
        }
    }
    return false;
}

/* ---------- 记录一次成功交换（当日计数）---------- */
static void pair_success(const uint8_t mac[6])
{
    pair_cd_t cd;
    cd_load(mac, &cd);
    uint32_t now = now_s();
    uint32_t today = time_sync_local_day();
    /* 单调毫秒与 RTC 无关，无论环境如何都要记录（冷却的硬基准） */
    cd.last_ok_ms = now_ms();
    if (now < cd.last_ok) {
        /* RTC 回退（断电重启）：旧计数作废，重新起算 */
        cd.day = 0;
        cd.cnt = 0;
    }
    cd.last_ok = now;
    if (cd.day != today) {
        cd.day = today;
        cd.cnt = 1;
    } else {
        cd.cnt++;
    }
    cd_save(mac, &cd);
    ESP_LOGI(TAG, "pair success %02x%02x%02x%02x%02x%02x: cnt=%u day=%lu last_ok=%lu ms=%lu",
             mac[0], mac[1], mac[2], mac[3], mac[4], mac[5],
             (unsigned)cd.cnt, (unsigned long)cd.day,
             (unsigned long)cd.last_ok, (unsigned long)cd.last_ok_ms);
}

/* ---------- 墙钟校准平移（time_sync weak hook 的强定义）----------
 * PC 绝对对齐时间后，存量冷却记录整体平移同一变化量，保持
 * "冷静期 / 当日封禁"的相对语义不因校时而失效或误延长。
 * day 为绝对日编号，按 delta/86400 平移（日边界误差 ≤1 天，语义宽松可接受）。 */
void espnow_nfc_shift_cooloff_hook(int64_t delta_s)
{
    nvs_iterator_t it = NULL;
    if (nvs_entry_find(NULL, CD_NS, NVS_TYPE_BLOB, &it) != ESP_OK || it == NULL) {
        return;
    }
    int32_t day_shift = (int32_t)(delta_s / 86400);
    ESP_LOGI(TAG, "shift cooloff by %+lld s (day %+d)",
             (long long)delta_s, day_shift);
    esp_err_t res = ESP_OK;
    while (res == ESP_OK) {
        nvs_entry_info_t info;
        nvs_entry_info(it, &info);
        res = nvs_entry_next(&it);
        /* 键名兼容：新 "p" + 12hex（13 字符，NVS 上限 15）；旧 "pair_" + 12hex
         * 曾超上限（17 字符）从未成功写入——旧键仅清理不再迁移 */
        if (info.key[0] != 'p' || info.key[1] == 'a') {
            continue;
        }
        nvs_handle_t h;
        if (nvs_open(CD_NS, NVS_READWRITE, &h) != ESP_OK) {
            break;
        }
        size_t len = sizeof(pair_cd_t);
        pair_cd_t cd;
        if (nvs_get_blob(h, info.key, &cd, &len) == ESP_OK && len == sizeof(cd)) {
            int64_t nl = (int64_t)cd.last_ok + delta_s;
            cd.last_ok = nl > 0 ? (uint32_t)nl : 1;
            int64_t nd = (int64_t)cd.day + day_shift;
            cd.day = nd > 0 ? (uint32_t)nd : 0;
            nvs_set_blob(h, info.key, &cd, sizeof(cd));
        }
        nvs_commit(h);
        nvs_close(h);
    }
    nvs_release_iterator(it);
}

/* ============ 自己 blob 构建 ============
 * 把「名字(roles 当前项) + uid/motto(NVS 档案) + 头像/表情包
 * (self.bin，缺省回退内置默认头像)」打包成 card_blob_hdr_t + payload
 * 的 PSRAM 缓冲。static self_buf 只读缓存 self.bin，避免每次交换都
 * 重新读文件。 */

static esp_err_t build_self_blob(uint8_t **out_blob, uint32_t *out_total)
{
    static uint8_t *self_buf = NULL;
    if (self_buf == NULL) {
        self_buf = heap_caps_malloc(SELF_BLOB_MAX, MALLOC_CAP_SPIRAM | MALLOC_CAP_8BIT);
        if (self_buf == NULL) {
            ESP_LOGE(TAG, "no PSRAM for self blob");
            return ESP_ERR_NO_MEM;
        }
    }

    card_blob_hdr_t hdr;
    memset(&hdr, 0, sizeof(hdr));
    hdr.magic = CARD_BLOB_MAGIC;
    hdr.ver   = CARD_BLOB_VER;
    hdr.ts    = now_s();

    /* 角色名 = SD 卡当前角色文件夹名（可以是中文等 UTF-8，协议仅限长
     * 24 字节）；弯引号归一化为直引号后发送（对端显示一致）；无 SD/无
     * 角色时 name 留空，接收方 UI 回退显示。 */
    const char *role_name = roles_current_name();
    if (role_name != NULL) {
        role_text_display(role_name, hdr.name, sizeof(hdr.name));
    }
    hdr.name_len = (uint8_t)strlen(hdr.name);

    const char *uid = nfc_profile_get_uid();
    strncpy(hdr.uid, uid, sizeof(hdr.uid) - 1);
    hdr.uid_len = (uint8_t)strlen(hdr.uid);

    const char *motto = nfc_profile_get_motto();
    strncpy(hdr.motto, motto, sizeof(hdr.motto) - 1);
    hdr.motto_len = (uint8_t)strlen(hdr.motto);

    const char *uname = nfc_profile_get_uname();
    strncpy(hdr.uname, uname, sizeof(hdr.uname) - 1);
    hdr.uname_len = (uint8_t)strlen(hdr.uname);

    /* 角色头图：从 SD 卡当前角色文件夹读 portrait.bin（64x64 RGB565
     * 小端 = CARD_PORTRAIT_SIZE 字节），无 SD/无文件则 portrait_len=0，
     * 接收方回退显示用户头像。static 缓冲：build_self_blob 只在
     * nfc_task 单上下文调用，无并发问题。 */
    static uint8_t s_portrait[CARD_PORTRAIT_SIZE];
    const uint8_t *portrait = NULL;
    if (roles_read_portrait(s_portrait) == ESP_OK) {
        portrait = s_portrait;
    }
    hdr.portrait_len = (portrait != NULL) ? CARD_PORTRAIT_SIZE : 0;

    const uint8_t *avatar      = (const uint8_t *)default_avatar_565;
    uint16_t       avatar_len  = DEFAULT_AVATAR_SIZE;
    const uint8_t *stickers    = NULL;
    uint8_t        sticker_cnt = 0;
    uint16_t       sticker_len[CARD_STICKER_MAX] = {0, 0, 0};

    size_t self_len = 0;
    if (card_store_load_self(self_buf, SELF_BLOB_MAX, &self_len) == ESP_OK && self_len >= sizeof(self_blob_hdr_t)) {
        self_blob_hdr_t *sh = (self_blob_hdr_t *)self_buf;
        uint8_t *p = self_buf + sizeof(self_blob_hdr_t);
        if (sh->avatar_len == CARD_AVATAR_SIZE) {
            avatar     = p;
            avatar_len = sh->avatar_len;
        }
        p += avatar_len;
        if (sh->sticker_cnt <= CARD_STICKER_MAX) {
            sticker_cnt = sh->sticker_cnt;
            memcpy(sticker_len, sh->sticker_len, sizeof(sticker_len));
            stickers = p;
        }
    }

    hdr.avatar_len  = avatar_len;
    hdr.sticker_cnt = sticker_cnt;
    memcpy(hdr.sticker_len, sticker_len, sizeof(sticker_len));

    uint32_t payload_total = (uint32_t)hdr.portrait_len + avatar_len;
    for (int i = 0; i < sticker_cnt; i++) {
        payload_total += sticker_len[i];
    }

    uint32_t blob_size = sizeof(hdr) + payload_total;
    uint8_t *blob = heap_caps_malloc(blob_size, MALLOC_CAP_SPIRAM | MALLOC_CAP_8BIT);
    if (blob == NULL) {
        ESP_LOGE(TAG, "no PSRAM for tx blob (%lu)", (unsigned long)blob_size);
        return ESP_ERR_NO_MEM;
    }
    memcpy(blob, &hdr, sizeof(hdr));
    /* payload 顺序：角色头图 → 用户头像 → 表情包（接收方按同一顺序解析） */
    uint8_t *dst = blob + sizeof(hdr);
    if (hdr.portrait_len > 0 && portrait != NULL) {
        memcpy(dst, portrait, hdr.portrait_len);
    }
    dst += hdr.portrait_len;
    memcpy(dst, avatar, avatar_len);
    dst += avatar_len;
    if (sticker_cnt > 0) {
        for (int i = 0; i < sticker_cnt; i++) {
            memcpy(dst, stickers, sticker_len[i]);
            dst += sticker_len[i];
            stickers += sticker_len[i];
        }
    }

    *out_blob  = blob;
    *out_total = blob_size;
    return ESP_OK;
}

/* ============ 发送 ============ */

static const uint8_t s_bcast[6] = {0xFF, 0xFF, 0xFF, 0xFF, 0xFF, 0xFF};

/* ---------- 通用发包：填 magic/ver/type/seq/ack_seq，走广播或单播 ---------- */
static esp_err_t send_packet(const uint8_t *dst, uint8_t type, uint8_t seq, uint8_t ack_seq,
                             const void *payload, uint16_t len)
{
    nfc_packet_t pkt;
    memset(&pkt, 0, sizeof(pkt));
    pkt.magic   = NFC_MAGIC;
    pkt.ver     = NFC_VER;
    pkt.type    = type;
    pkt.seq     = seq;
    pkt.ack_seq = ack_seq;
    pkt.len     = len;
    if (len > 0 && payload != NULL) {
        memcpy(pkt.payload, payload, len);
    }
    return esp_now_send(dst, (const uint8_t *)&pkt, NFC_PACKET_HDR_SIZE + len);
}

static uint32_t s_hello_cnt;

/* ============ 产品 IP（分 IP 隔离）============
 * 见 ip_config.h / nfc_proto.h。规则（接收方视角）：
 *   - 本机未配置（cnt==0）→ 接受所有设备（保持旧行为）；
 *   - 对方为旧版 6B payload 且组码 0（全通标记）→ 接受；
 *   - 本机与对方 IP 码列表任一相同（交集）→ 接受；
 *   - 否则忽略（不回 ACK、不入会）并给对方记 60s 失配冷却（RAM）。 */

static const uint16_t *my_ip_codes(int *out_cnt)
{
    const ip_config_t *c = ip_config_get();
    *out_cnt = c->cnt;
    return c->codes;
}

static bool code_in_list(uint16_t code, const uint16_t *codes, int cnt)
{
    for (int i = 0; i < cnt; i++) {
        if (codes[i] == code) {
            return true;
        }
    }
    return false;
}

/* ---------- 失配冷却表（RAM，60s）----------
 * 只服务 IP 失配防刷：被拒绝的对端在 60s 内不再回 ACK/入会。
 * 8 槽覆盖更新；仅在 nfc_task 上下文访问（handle_packet 链），无需锁。 */
#define REJECT_COOL_S   60
#define REJECT_COOL_MAX 8
static uint8_t   s_reject_macs[REJECT_COOL_MAX][6];
static uint32_t  s_reject_ms[REJECT_COOL_MAX];   /* esp_timer 单调 ms */
static int       s_reject_cnt;

static int reject_find(const uint8_t *mac)
{
    for (int i = 0; i < s_reject_cnt; i++) {
        if (memcmp(s_reject_macs[i], mac, 6) == 0) {
            return i;
        }
    }
    return -1;
}

static bool reject_blocked(const uint8_t *mac)
{
    int i = reject_find(mac);
    if (i < 0) {
        return false;
    }
    uint32_t now = now_ms();
    if ((int64_t)now - (int64_t)s_reject_ms[i] >= REJECT_COOL_S * 1000LL) {
        return false;   /* 惰性过期（槽留待覆盖） */
    }
    return true;
}

static void reject_mark(const uint8_t *mac)
{
    int i = reject_find(mac);
    if (i < 0) {
        if (s_reject_cnt < REJECT_COOL_MAX) {
            i = s_reject_cnt++;
        } else {
            i = 0;   /* 满：覆盖最旧槽 */
        }
    }
    memcpy(s_reject_macs[i], mac, 6);
    s_reject_ms[i] = now_ms();
}

static void reject_unmark(const uint8_t *mac)
{
    int i = reject_find(mac);
    if (i >= 0) {
        s_reject_ms[i] = 0;   /* 置 0 = 立即过期（槽保留） */
    }
}

/* 解析 HELLO payload → 对方 IP 码列表（静态小缓冲返回）。
 * 兼容旧版 6B payload {feature, group}；新形状 len 精确校验（防伪造）。
 * 返回 false = 非同类设备 / 形状非法。 */
static bool hello_peer_codes(const nfc_packet_t *pkt,
                             const uint16_t **out_codes, int *out_cnt)
{
    static uint16_t s_legacy[1];   /* 旧版单码：仅本函数同步使用 */
    if (pkt->len < sizeof(uint32_t)) {
        return false;
    }
    const uint8_t *b = pkt->payload;
    uint32_t fc;
    memcpy(&fc, b, 4);
    if (fc != NFC_FEATURE_CODE) {
        return false;
    }
    if (pkt->len == 6) {           /* 6B：v2 空列表(cnt=0) 与旧版 {feature,group} 同长 */
        if (b[4] == 0 && b[5] == NFC_HELLO_VER) {
            /* v2 cnt=0：空表=全通（交由调用方 pc==0 分支处理） */
            *out_codes = NULL;
            *out_cnt   = 0;
            return true;
        }
        uint16_t g;                /* 旧版单码：{feature_code, group_code} */
        memcpy(&g, b + 4, 2);
        s_legacy[0] = g;
        *out_codes = s_legacy;
        *out_cnt   = 1;
        return true;
    }
    uint8_t ver = b[5];
    if (ver != 2 && ver != NFC_HELLO_VER_LATEST) {
        return false;
    }
    int cnt = b[4];
    uint16_t need = (uint16_t)(NFC_HELLO_HDR_SIZE + cnt * 2 +
                               (ver == NFC_HELLO_VER_LATEST ? NFC_HELLO_TS_SIZE : 0));
    if (cnt > NFC_IP_CODES_MAX || pkt->len != need) {
        return false;
    }
    *out_codes = (const uint16_t *)(b + NFC_HELLO_HDR_SIZE);
    *out_cnt   = cnt;
    return true;
}

/* 取 HELLO/HELLO_ACK payload 尾部的发送方墙钟秒（v3 才有；v2/旧版返回 false） */
static bool hello_peer_ts(const nfc_packet_t *pkt, uint32_t *out_ts)
{
    if (pkt->len < NFC_HELLO_HDR_SIZE + NFC_HELLO_TS_SIZE) {
        return false;
    }
    const uint8_t *b = pkt->payload;
    uint32_t fc;
    memcpy(&fc, b, 4);
    if (fc != NFC_FEATURE_CODE || b[5] != NFC_HELLO_VER_LATEST) {
        return false;
    }
    int cnt = b[4];
    if (cnt > NFC_IP_CODES_MAX ||
        pkt->len != (uint16_t)(NFC_HELLO_HDR_SIZE + cnt * 2 + NFC_HELLO_TS_SIZE)) {
        return false;
    }
    memcpy(out_ts, b + NFC_HELLO_HDR_SIZE + cnt * 2, 4);
    return true;
}

/* 产品 IP 判定：false = 失配（调用方负责拒绝+冷却）。命中时回填对方
 * 匹配到的那个码（详情记录用；对方为全通时回填 0）。 */
static bool hello_ip_check(const nfc_packet_t *pkt, uint16_t *out_matched)
{
    int my_cnt;
    const uint16_t *my = my_ip_codes(&my_cnt);
    int pc;
    const uint16_t *peer;
    if (!hello_peer_codes(pkt, &peer, &pc)) {
        return false;
    }
    if (my_cnt == 0) {
        /* 本机未配置：全通；回填对方首码（无码则 0）供详情 */
        if (out_matched != NULL) {
            *out_matched = (pc > 0) ? peer[0] : 0;
        }
        return true;
    }
    if (pc == 0) {
        /* 对端未配置（v2 cnt=0，空列表）：对称全通——本机配置不隔离未配置设备 */
        if (out_matched != NULL) {
            *out_matched = 0;
        }
        return true;
    }
    for (int i = 0; i < pc; i++) {
        if (peer[i] == 0) {          /* 旧版全通标记 */
            if (out_matched != NULL) {
                *out_matched = 0;
            }
            return true;
        }
        if (code_in_list(peer[i], my, my_cnt)) {
            if (out_matched != NULL) {
                *out_matched = peer[i];
            }
            return true;
        }
    }
    return false;
}

/* 失配处理：记冷却 + 限流日志（HELLO 广播 200ms/台，异 IP 会刷屏） */
static void reject_hello(const uint8_t *src, const nfc_packet_t *pkt)
{
    reject_mark(src);
    static int64_t s_reject_log_us;
    int64_t now = esp_timer_get_time();
    if (now - s_reject_log_us < 5000000LL) {
        return;
    }
    s_reject_log_us = now;
    int pc;
    const uint16_t *peer;
    char buf[48] = "";
    int p = 0;
    if (hello_peer_codes(pkt, &peer, &pc)) {
        for (int i = 0; i < pc && i < 4 && p + 6 < (int)sizeof(buf); i++) {
            p += snprintf(buf + p, sizeof(buf) - (size_t)p, "%s%04x",
                          i ? "," : "", (unsigned)peer[i]);
        }
    } else {
        snprintf(buf, sizeof(buf), "?");
    }
    ESP_LOGI(TAG, "ignored HELLO from %02x%02x%02x%02x%02x%02x: IP no overlap"
                  " (peer codes: %s) -> %ds cooldown",
             src[0], src[1], src[2], src[3], src[4], src[5], buf,
             REJECT_COOL_S);
}

/* ---------- 构建 HELLO/HELLO_ACK payload（v3：IP 码列表 + 墙钟时间戳，
 * 长度 6 + cnt*2 + 4；接收方无墙钟时用 ts 对齐冷却） ---------- */
static void fill_hello_payload(uint8_t *buf)
{
    uint32_t fc = NFC_FEATURE_CODE;
    memcpy(buf, &fc, 4);
    int cnt;
    const uint16_t *codes = my_ip_codes(&cnt);
    buf[4] = (uint8_t)cnt;
    buf[5] = NFC_HELLO_VER_LATEST;
    for (int i = 0; i < cnt && i < NFC_IP_CODES_MAX; i++) {
        memcpy(buf + NFC_HELLO_HDR_SIZE + i * 2, &codes[i], 2);
    }
    uint32_t ts = time_sync_now();   /* 未校准时是相对秒，接收方按 <1e9 弃用 */
    memcpy(buf + NFC_HELLO_HDR_SIZE + cnt * 2, &ts, NFC_HELLO_TS_SIZE);
}

static uint16_t hello_payload_len(void)
{
    int cnt;
    (void)my_ip_codes(&cnt);
    return (uint16_t)(NFC_HELLO_HDR_SIZE + cnt * 2 + NFC_HELLO_TS_SIZE);
}

static void send_hello(void)
{
    if (!mjpeg_player_exchange_ok()) return;   /* 播放未满 3s / 加载中：不发邀请 */
    uint8_t pl[NFC_HELLO_HDR_SIZE + NFC_IP_CODES_MAX * 2 + NFC_HELLO_TS_SIZE];
    fill_hello_payload(pl);
    esp_err_t err = send_packet(s_bcast, NFC_TYPE_HELLO, 0, 0, pl, hello_payload_len());
    s_hello_cnt++;
    if (err != ESP_OK) {
        ESP_LOGW(TAG, "hello send err: %s", esp_err_to_name(err));
    } else if (s_hello_cnt % 25 == 0) {
        ESP_LOGI(TAG, "hello tx %u", (unsigned)s_hello_cnt);
    }
}

static void send_hello_ack(const uint8_t *dst)
{
    uint8_t pl[NFC_HELLO_HDR_SIZE + NFC_IP_CODES_MAX * 2 + NFC_HELLO_TS_SIZE];
    fill_hello_payload(pl);
    send_packet(dst, NFC_TYPE_HELLO_ACK, 0, 0, pl, hello_payload_len());
}

static void send_done(void)
{
    send_packet(s_sess.peer, NFC_TYPE_DONE, 0, 0, NULL, 0);
}

/* ---------- 发一个数据分片 ----------
 * 注意：不在这里自增 tx_seq——重传时复用同一 seq（接收方按 seq 去重），
 * ACK 匹配成功后（handle_ack）才推进 tx_off/tx_seq。 */
static void send_seg(void)
{
    uint32_t remaining = s_sess.tx_total - sizeof(card_blob_hdr_t) - s_sess.tx_off;
    if (remaining == 0) {
        return;
    }
    uint16_t seg_len = remaining > NFC_SEG_SIZE ? NFC_SEG_SIZE : (uint16_t)remaining;

    uint8_t payload[NFC_SEG_SIZE + sizeof(nfc_seg_hdr_t)];
    nfc_seg_hdr_t sh = { .offset = (uint16_t)s_sess.tx_off, .seg_len = seg_len };
    memcpy(payload, &sh, sizeof(sh));
    memcpy(payload + sizeof(sh),
           s_sess.tx_blob + sizeof(card_blob_hdr_t) + s_sess.tx_off, seg_len);

    s_sess.last_tx_us = esp_timer_get_time();
    s_sess.last_event_s = now_s();
    send_packet(s_sess.peer, NFC_TYPE_DATA_SEG, s_sess.tx_seq, 0, payload, sizeof(sh) + seg_len);
}

static void copy_str(char *dst, size_t dst_cap, const char *src, size_t src_cap)
{
    size_t n = strnlen(src, src_cap);
    if (n >= dst_cap) {
        n = dst_cap - 1;
    }
    memcpy(dst, src, n);
    dst[n] = '\0';
}

/* ---------- 发字段元信息（对方据此预分配接收缓冲） ---------- */
static void send_field(void)
{
    card_blob_hdr_t *th = (card_blob_hdr_t *)s_sess.tx_blob;
    nfc_field_t f;
    memset(&f, 0, sizeof(f));
    copy_str(f.name, sizeof(f.name), th->name, sizeof(th->name));
    copy_str(f.uid, sizeof(f.uid), th->uid, sizeof(th->uid));
    copy_str(f.motto, sizeof(f.motto), th->motto, sizeof(th->motto));
    copy_str(f.uname, sizeof(f.uname), th->uname, sizeof(th->uname));
    f.avatar_len   = th->avatar_len;
    f.portrait_len = th->portrait_len;
    f.sticker_cnt  = th->sticker_cnt;
    memcpy(f.sticker_len, th->sticker_len, sizeof(f.sticker_len));
    f.total_size = (uint16_t)(s_sess.tx_total - sizeof(card_blob_hdr_t));
    /* 会话基准时间：从机回传时借用主机的 ts（sess_ts 在建会时定死），
     * 保证两端给这次交换记同一个时间。0=旧版兼容，用本机打包时刻。 */
    f.ts = (s_sess.sess_ts != 0) ? s_sess.sess_ts : th->ts;
    send_packet(s_sess.peer, NFC_TYPE_DATA_FIELD, 0, 0, &f, sizeof(f));
    s_sess.last_event_s = now_s();
}

static uint32_t field_payload_size(const nfc_field_t *f)
{
    uint32_t n = (uint32_t)f->portrait_len + f->avatar_len;
    for (int i = 0; i < f->sticker_cnt && i < CARD_STICKER_MAX; i++) {
        n += f->sticker_len[i];
    }
    return n;
}

/* ============ 会话控制 ============ */

/* ---------- 释放会话全部资源（blob 缓冲等）并回到 IDLE ---------- */
static void free_session(void)
{
    if (s_sess.tx_blob) {
        heap_caps_free(s_sess.tx_blob);
    }
    if (s_sess.rx_blob) {
        heap_caps_free(s_sess.rx_blob);
    }
    memset(&s_sess, 0, sizeof(s_sess));
}

static void rx_hdr_from_field(card_blob_hdr_t *hdr, const nfc_field_t *f)
{
    memset(hdr, 0, sizeof(*hdr));
    hdr->magic = CARD_BLOB_MAGIC;
    hdr->ver   = CARD_BLOB_VER;
    copy_str(hdr->name, sizeof(hdr->name), f->name, sizeof(f->name));
    hdr->name_len = (uint8_t)strnlen(f->name, sizeof(f->name));
    copy_str(hdr->uid, sizeof(hdr->uid), f->uid, sizeof(f->uid));
    hdr->uid_len = (uint8_t)strnlen(f->uid, sizeof(f->uid));
    copy_str(hdr->motto, sizeof(hdr->motto), f->motto, sizeof(f->motto));
    hdr->motto_len = (uint8_t)strnlen(f->motto, sizeof(f->motto));
    copy_str(hdr->uname, sizeof(hdr->uname), f->uname, sizeof(f->uname));
    hdr->uname_len = (uint8_t)strnlen(f->uname, sizeof(f->uname));
    hdr->avatar_len  = f->avatar_len;
    hdr->portrait_len = (f->portrait_len <= CARD_PORTRAIT_SIZE) ? f->portrait_len : 0;
    hdr->sticker_cnt = f->sticker_cnt;
    memcpy(hdr->sticker_len, f->sticker_len, sizeof(hdr->sticker_len));
    hdr->ts = f->ts;   /* v4：基准时间取字段携带的（= 主机打包时刻） */
}

/* ---------- 发起会话（本机为 MAC 较小的一方 = 主） ---------- */
static void start_master(const uint8_t *peer)
{
    memset(&s_sess, 0, sizeof(s_sess));
    s_sess.active    = true;
    s_sess.phase     = PHASE_MASTER_SEND;
    s_sess.last_event_s = now_s();
    memcpy(s_sess.peer, peer, 6);
    s_sess.matched_ip = s_hello_matched;
    s_hello_matched   = 0;

    uint32_t blob_size = 0;
    if (build_self_blob(&s_sess.tx_blob, &blob_size) != ESP_OK) {
        free_session();
        return;
    }
    s_sess.tx_total = blob_size;
    /* 会话基准时间定死为主机打包时刻（UTC 秒）：本次交换两端都记它 */
    s_sess.sess_ts  = ((card_blob_hdr_t *)s_sess.tx_blob)->ts;

    send_field();
    s_sess.tx_seq = 1;
    send_seg();
    ESP_LOGI(TAG, "master session with %02x%02x%02x%02x%02x%02x",
             peer[0], peer[1], peer[2], peer[3], peer[4], peer[5]);
}

/* ---------- 以从机身份入会（收到陌生 DATA_FIELD 且对方 MAC 更小） ---------- */
static void start_slave(const uint8_t *peer, const nfc_field_t *f)
{
    uint32_t payload = field_payload_size(f);
    if (f->total_size < payload) {
        ESP_LOGW(TAG, "bad field total_size");
        return;
    }

    memset(&s_sess, 0, sizeof(s_sess));
    s_sess.active    = true;
    s_sess.phase     = PHASE_SLAVE_RECV;
    s_sess.last_event_s = now_s();
    memcpy(s_sess.peer, peer, 6);
    memcpy(&s_sess.field, f, sizeof(*f));
    s_sess.sess_ts   = f->ts;   /* 借主机基准时间：收卡记录与回传字段都用它 */
    s_sess.matched_ip = s_hello_matched;
    s_hello_matched   = 0;

    s_sess.rx_blob = heap_caps_malloc(sizeof(card_blob_hdr_t) + payload,
                                      MALLOC_CAP_SPIRAM | MALLOC_CAP_8BIT);
    if (s_sess.rx_blob == NULL) {
        ESP_LOGE(TAG, "no PSRAM for rx blob");
        free_session();
        return;
    }
    rx_hdr_from_field((card_blob_hdr_t *)s_sess.rx_blob, f);
    s_sess.rx_expected_seq = 1;
    ESP_LOGI(TAG, "slave session, expecting %lu bytes", (unsigned long)payload);
}

/* ---------- 会话收尾：SD 记录名片 + 回调 UI + 记录冷却 ---------- */
static void complete_session(void)
{
    card_blob_hdr_t *hdr = (card_blob_hdr_t *)s_sess.rx_blob;
    if (hdr == NULL) {
        free_session();
        return;
    }
    uint32_t payload_len = (uint32_t)hdr->portrait_len + hdr->avatar_len;
    for (int i = 0; i < hdr->sticker_cnt && i < CARD_STICKER_MAX; i++) {
        payload_len += hdr->sticker_len[i];
    }
    uint32_t total = sizeof(card_blob_hdr_t) + payload_len;

    /* v4：ts 统一为会话基准时间（主机打包时刻；从机经字段借用）。
     * seg 流会覆盖 blob 头区（内嵌的是发送方各自打包时刻），这里必须
     * 用 sess_ts 强制覆写，双方记录（txt 行与 bin）才会逐秒一致；
     * 基准缺失（旧版对端/未校准相对秒 <1e9）才退回本地时刻。 */
    if (s_sess.sess_ts >= 1000000000UL) {
        hdr->ts = s_sess.sess_ts;
    }
    if (hdr->ts < 1000000000UL) {
        hdr->ts = now_s();
    }
    ESP_LOGI(TAG, "exchange complete with %02x%02x%02x%02x%02x%02x: %s / %s / %s / %s"
                  " (ip=%04x)",
             s_sess.peer[0], s_sess.peer[1], s_sess.peer[2],
             s_sess.peer[3], s_sess.peer[4], s_sess.peer[5],
             hdr->name, hdr->uid, hdr->motto, hdr->uname,
             (unsigned)s_sess.matched_ip);

    if (card_log_save(s_sess.peer, s_sess.rx_blob, total,
                      s_sess.matched_ip) == ESP_OK) {
        ESP_LOGI(TAG, "card logged to SD (%lu bytes)", (unsigned long)total);
    } else {
        ESP_LOGW(TAG, "card log FAILED (SD not mounted or write error)");
    }
    if (s_recv_cb != NULL) {
        s_recv_cb(s_sess.peer, hdr, s_sess.rx_blob + sizeof(card_blob_hdr_t), payload_len);
    }

    /* 成功冷却：当日计数 + 冷静期（已恢复） */
    pair_success(s_sess.peer);
    free_session();
}

static void abort_session(const char *why)
{
    ESP_LOGW(TAG, "session aborted: %s", why);
    free_session();
}

/* ============ 包处理 ============
 * handle_packet 是唯一分发入口：校验 magic/ver、过滤自己发的包，
 * 按 type 分派。会话状态机只认当前 peer 的包（防第三人干扰）。 */

/* ---------- DATA_ACK：确认序号推进发送进度；发完即切换阶段 ---------- */
static void handle_ack(const nfc_packet_t *pkt)
{
    if (pkt->ack_seq != s_sess.tx_seq) {
        return;
    }
    s_sess.retries = 0;
    s_sess.last_event_s = now_s();

    uint32_t payload_done = s_sess.tx_total - sizeof(card_blob_hdr_t);
    uint32_t sent = (uint32_t)pkt->ack_seq * NFC_SEG_SIZE;
    s_sess.tx_off = sent < payload_done ? sent : payload_done;

    if (s_sess.tx_off >= payload_done) {
        if (s_sess.phase == PHASE_MASTER_SEND) {
            s_sess.phase = PHASE_MASTER_RECV;
            s_sess.rx_expected_seq = 1;
            send_done();
            ESP_LOGI(TAG, "master payload sent, waiting slave");
        } else if (s_sess.phase == PHASE_SLAVE_SEND) {
            send_done();
            ESP_LOGI(TAG, "slave payload sent, done");
            complete_session();
        }
    } else {
        s_sess.tx_seq++;
        send_seg();
    }
}

/* ---------- DATA_SEG：按 seq+offset 严格顺序写入（乱序/重复直接忽略，
 *            但 ACK 照回——对端按 ACK 推进，超时重发同 seq） ---------- */
static void handle_seg(const nfc_packet_t *pkt)
{
    const nfc_seg_hdr_t *sh = (const nfc_seg_hdr_t *)pkt->payload;
    if (pkt->len < sizeof(nfc_seg_hdr_t) + sh->seg_len) {
        return;
    }
    s_sess.last_event_s = now_s();

    /* 空指针防御：对端 DATA_SEG 可能先于 DATA_FIELD 到达（无线重排），
     * 此时 rx_blob 还没分配，直接解引用会崩溃（表现：白屏/卡死） */
    if (s_sess.rx_blob == NULL) {
        return;
    }

    card_blob_hdr_t *hdr = (card_blob_hdr_t *)s_sess.rx_blob;
    /* 接收容量 = 角色头图 + 用户头像 + 表情包。
     * 曾漏算 portrait_len，导致发送端带角色头图时其尾段分片被拒
     * （offset+seg_len > cap）而交换中止/头像截半花屏。 */
    uint32_t cap = hdr->portrait_len + hdr->avatar_len;
    for (int i = 0; i < hdr->sticker_cnt && i < CARD_STICKER_MAX; i++) {
        cap += hdr->sticker_len[i];
    }

    if (pkt->seq == s_sess.rx_expected_seq && sh->offset == s_sess.rx_offset &&
        (uint32_t)sh->offset + sh->seg_len <= cap) {
        memcpy(s_sess.rx_blob + sizeof(card_blob_hdr_t) + sh->offset,
               pkt->payload + sizeof(nfc_seg_hdr_t), sh->seg_len);
        s_sess.rx_offset = sh->offset + sh->seg_len;
        s_sess.rx_expected_seq = (uint8_t)(pkt->seq + 1);
    }
    send_packet(s_sess.peer, NFC_TYPE_DATA_ACK, 0, pkt->seq, NULL, 0);
}

/* ---------- DONE：主发来的 → 从开始回发；从发来的 → 主校验收尾 ---------- */
static void handle_done(const nfc_packet_t *pkt)
{
    (void)pkt;
    s_sess.last_event_s = now_s();

    if (s_sess.phase == PHASE_SLAVE_RECV) {
        uint32_t payload = field_payload_size(&s_sess.field);
        if (s_sess.rx_offset < payload) {
            abort_session("slave payload incomplete");
            return;
        }

        uint32_t blob_size = 0;
        if (build_self_blob(&s_sess.tx_blob, &blob_size) != ESP_OK) {
            abort_session("no mem for slave tx");
            return;
        }
        s_sess.tx_total = blob_size;
        s_sess.phase = PHASE_SLAVE_SEND;
        s_sess.rx_expected_seq = 0;

        send_field();
        s_sess.tx_seq = 1;
        send_seg();
        ESP_LOGI(TAG, "slave switching to send");
    } else if (s_sess.phase == PHASE_MASTER_RECV) {
        /* 空指针防御：对方 DONE 先于 DATA_FIELD 到达时 rx_blob 未分配，
         * 此时不能收尾（会存下空名片），直接放弃本会话等下次相遇 */
        if (s_sess.rx_blob == NULL) {
            abort_session("master rx not ready");
            return;
        }
        uint32_t payload = field_payload_size(&s_sess.field);
        if (s_sess.rx_offset < payload) {
            abort_session("master payload incomplete");
            return;
        }
        complete_session();
    }
}

/* ---------- DATA_FIELD：主接收阶段的对方字段 → 备接收缓冲；
 *            空闲时收到字段 → 对方 MAC 更小则以从机入会 ---------- */
static void handle_field(const uint8_t *src, const nfc_packet_t *pkt)
{
    const nfc_field_t *f = (const nfc_field_t *)pkt->payload;
    if (pkt->len < sizeof(nfc_field_t)) {
        return;
    }

    if (s_sess.active && s_sess.phase == PHASE_MASTER_RECV && memcmp(src, s_sess.peer, 6) == 0) {
        uint32_t payload = field_payload_size(f);
        if (s_sess.rx_blob != NULL) {
            /* 字段重传路径：先释放旧缓冲（防泄漏），再按新 payload 分配 */
            heap_caps_free(s_sess.rx_blob);
            s_sess.rx_blob = NULL;
        }
        s_sess.rx_blob = heap_caps_malloc(sizeof(card_blob_hdr_t) + payload,
                                          MALLOC_CAP_SPIRAM | MALLOC_CAP_8BIT);
        if (s_sess.rx_blob == NULL) {
            abort_session("no mem for master rx");
            return;
        }
        rx_hdr_from_field((card_blob_hdr_t *)s_sess.rx_blob, f);
        s_sess.rx_expected_seq = 1;
        s_sess.last_event_s = now_s();
        return;
    }

    if (!s_sess.active && !pair_blocked(src) && memcmp(src, s_my_mac, 6) < 0) {
        start_slave(src, f);
    }
}

/* ---------- 保证对端已加入 ESP-NOW 对等表 ----------
 * ESP-NOW 单播要求发送方必须先把接收方加入 peer 列表（连广播地址
 * 都需要），我们启动时只加了广播地址，任何陌生 MAC 的单播都会
 * ESP_ERR_ESPNOW_NOT_FOUND 静默失败。这里收到谁就登记谁。 */
#define PEER_CACHE_MAX 8
static uint8_t s_peers[PEER_CACHE_MAX][6];
static int s_peer_cnt;

static void ensure_peer(const uint8_t *mac)
{
    for (int i = 0; i < s_peer_cnt; i++) {
        if (memcmp(s_peers[i], mac, 6) == 0) {
            return;
        }
    }
    if (s_peer_cnt >= PEER_CACHE_MAX) {
        return;
    }
    esp_now_peer_info_t peer;
    memset(&peer, 0, sizeof(peer));
    memcpy(peer.peer_addr, mac, 6);
    peer.channel = 1;
    peer.ifidx   = WIFI_IF_STA;
    peer.encrypt = false;
    if (esp_now_add_peer(&peer) == ESP_OK) {
        memcpy(s_peers[s_peer_cnt], mac, 6);
        s_peer_cnt++;
        ESP_LOGI(TAG, "peer added: %02x%02x%02x%02x%02x%02x",
                 mac[0], mac[1], mac[2], mac[3], mac[4], mac[5]);
    }
}

static void handle_packet(const rx_item_t *item)
{
    const nfc_packet_t *pkt = (const nfc_packet_t *)item->data;
    if (item->len < NFC_PACKET_HDR_SIZE || pkt->magic != NFC_MAGIC || pkt->ver != NFC_VER) {
        return;
    }
    if (memcmp(item->src, s_my_mac, 6) == 0) {
        return;
    }
    ensure_peer(item->src);

    switch (pkt->type) {
    case NFC_TYPE_HELLO: {
        if (!mjpeg_player_exchange_ok()) {
            return;   /* 加载/播放未满 3s：忽略邀请，不进入交换 */
        }
        {   /* 冷却时间对齐：本机无墙钟时借用对方 hello 里的 UTC 时间戳 */
            uint32_t pts;
            if (!time_sync_has_wall() && hello_peer_ts(pkt, &pts) &&
                pts >= 1000000000UL) {
                time_sync_adopt_peer(pts);
            }
        }
        uint16_t matched = 0;
        if (!hello_ip_check(pkt, &matched)) {
            reject_hello(item->src, pkt);
            return;
        }
        reject_unmark(item->src);
        s_hello_matched = matched;
        if (!s_sess.active && !pair_blocked(item->src) && !reject_blocked(item->src)) {
            send_hello_ack(item->src);
        }
        break;
    }
    case NFC_TYPE_HELLO_ACK: {
        uint32_t pts;
        if (!time_sync_has_wall() && hello_peer_ts(pkt, &pts) &&
            pts >= 1000000000UL) {
            time_sync_adopt_peer(pts);
        }
        uint16_t matched = 0;
        if (!hello_ip_check(pkt, &matched)) {
            reject_hello(item->src, pkt);
            return;
        }
        reject_unmark(item->src);
        s_hello_matched = matched;
        if (!s_sess.active && !pair_blocked(item->src) && !reject_blocked(item->src) &&
            memcmp(s_my_mac, item->src, 6) < 0) {
            start_master(item->src);
        }
        break;
    }
    case NFC_TYPE_DATA_FIELD:
        handle_field(item->src, pkt);
        break;
    case NFC_TYPE_DATA_SEG:
        if (s_sess.active && memcmp(item->src, s_sess.peer, 6) == 0 &&
            (s_sess.phase == PHASE_MASTER_RECV || s_sess.phase == PHASE_SLAVE_RECV)) {
            handle_seg(pkt);
        }
        break;
    case NFC_TYPE_DATA_ACK:
        if (s_sess.active && memcmp(item->src, s_sess.peer, 6) == 0 &&
            (s_sess.phase == PHASE_MASTER_SEND || s_sess.phase == PHASE_SLAVE_SEND)) {
            handle_ack(pkt);
        }
        break;
    case NFC_TYPE_DONE:
        if (s_sess.active && memcmp(item->src, s_sess.peer, 6) == 0) {
            handle_done(pkt);
        }
        break;
    default:
        break;
    }
}

/* ---------- ESP-NOW 接收回调（WiFi 任务上下文）----------
 * 只做两件事：长度粗校验 + malloc 拷贝投递队列（快进快出，
 * 绝不在中断/回调里做解码和状态机——那在 nfc_task 里做）。 */
static uint32_t s_rx_cnt;

static void on_recv(const esp_now_recv_info_t *info, const uint8_t *data, int len)
{
    s_rx_cnt++;
    if (s_rx_cnt % 20 == 0) {
        ESP_LOGI(TAG, "espnow rx %u (len %d from %02x%02x%02x%02x%02x%02x)",
                 (unsigned)s_rx_cnt, len,
                 info->src_addr[0], info->src_addr[1], info->src_addr[2],
                 info->src_addr[3], info->src_addr[4], info->src_addr[5]);
    }
    if (len <= 0 || len > (int)(NFC_PACKET_HDR_SIZE + NFC_MAX_PAYLOAD)) {
        return;
    }
    rx_item_t *item = heap_caps_malloc(sizeof(rx_item_t) + (size_t)len, MALLOC_CAP_DEFAULT);
    if (item == NULL) {
        return;
    }
    memcpy(item->src, info->src_addr, 6);
    item->len = (size_t)len;
    memcpy(item->data, data, (size_t)len);
    if (xQueueSend(s_rx_queue, &item, 0) != pdTRUE) {
        free(item);
    }
}

static void on_send(const esp_now_send_info_t *info, esp_now_send_status_t status)
{
    (void)info;
    if (status != ESP_NOW_SEND_SUCCESS) {
        ESP_LOGW(TAG, "send failed");
    }
}

/* ============ 任务 ============
 * nfc_task（core0 优先级2，5ms 循环）：
 *   - 消费收包队列驱动状态机
 *   - 发送阶段：ACK 50ms 超时重发（同 seq，最多 3 次）
 *   - 接收阶段：5s 无事件放弃会话
 *   - 空闲：200ms±10% 抖动周期广播 HELLO（抖动防两台同固件相位锁死） */

/* ---------- 会话超时/重传推进（每个任务循环调一次） ---------- */
static void sess_tick(void)
{
    bool sending = (s_sess.phase == PHASE_MASTER_SEND || s_sess.phase == PHASE_SLAVE_SEND);
    if (!sending) {
        if (now_s() - s_sess.last_event_s > SESS_TIMEOUT_S) {
            abort_session("timeout");
        }
        return;
    }

    if (esp_timer_get_time() - s_sess.last_tx_us > NFC_ACK_TIMEOUT_MS * 1000ULL) {
        if (s_sess.retries >= NFC_ACK_MAX_RETRY) {
            abort_session("ack timeout");
            return;
        }
        s_sess.retries++;
        send_seg();
    }
}

static void nfc_task(void *arg)
{
    (void)arg;
    rx_item_t *item = NULL;
    int64_t next_cfg_us = esp_timer_get_time() + 3000000LL;  /* 首次加载延迟 3s */
    while (1) {
        while (xQueueReceive(s_rx_queue, &item, 0) == pdTRUE) {
            handle_packet(item);
            free(item);
        }

        /* 组织隔离配置：异步周期重载（不阻塞 init）。
         * 首载延迟 3s（SD 挂载+角色扫描窗口过后）；之后每 10s 一次，
         * 幂等且失败保持上次——SD 卡热插后隔离配置自动跟上。 */
        if (esp_timer_get_time() >= next_cfg_us) {
            next_cfg_us = esp_timer_get_time() + 10000000LL;
            ip_config_load();
        }

        if (s_sess.active) {
            sess_tick();
        } else {
            uint64_t now = esp_timer_get_time();
            if (now >= s_next_hello_us) {
                send_hello();
                int64_t jitter = (int64_t)(esp_random() % (NFC_HELLO_JITTER_US * 2)) - (int64_t)NFC_HELLO_JITTER_US;
                s_next_hello_us = now + NFC_HELLO_PERIOD_US + (uint64_t)jitter;
            }
        }

        vTaskDelay(pdMS_TO_TICKS(5));
    }
}

/* ============ 初始化 ============
 * 顺序：读 MAC → 收包队列 → netif/事件循环/WiFi(STA 不连 AP、
 * RAM 存储、信道 1 固定) → ESP-NOW 初始化 + 广播 peer → 启动任务。
 * 可调整内容：信道 1 固定（两台设备必须同信道，改需同步双方固件）；
 *   任务栈 4096 已按栈高水位实测留余量，勿缩。 */

void espnow_nfc_set_recv_cb(nfc_recv_card_cb_t cb)
{
    s_recv_cb = cb;
}

esp_err_t espnow_nfc_init(void)
{
    if (esp_read_mac(s_my_mac, ESP_MAC_WIFI_STA) != ESP_OK) {
        ESP_LOGE(TAG, "read mac failed");
        return ESP_FAIL;
    }
    ESP_LOGI(TAG, "my mac: %02x%02x%02x%02x%02x%02x",
             s_my_mac[0], s_my_mac[1], s_my_mac[2], s_my_mac[3], s_my_mac[4], s_my_mac[5]);

    s_rx_queue = xQueueCreate(16, sizeof(rx_item_t *));
    if (s_rx_queue == NULL) {
        return ESP_ERR_NO_MEM;
    }

    esp_netif_init();
    esp_event_loop_create_default();
    esp_netif_create_default_wifi_sta();

    ESP_LOGI(TAG, "pre-wifi-init: SRAM %u / DMA %u", 
             (unsigned)heap_caps_get_free_size(MALLOC_CAP_INTERNAL),
             (unsigned)heap_caps_get_free_size(MALLOC_CAP_DMA));

    wifi_init_config_t cfg = WIFI_INIT_CONFIG_DEFAULT();
    esp_err_t err = esp_wifi_init(&cfg);
    if (err != ESP_OK) {
        ESP_LOGE(TAG, "wifi init failed: %s", esp_err_to_name(err));
        return err;
    }
    ESP_LOGI(TAG, "post-wifi-init: SRAM %u / DMA %u",
             (unsigned)heap_caps_get_free_size(MALLOC_CAP_INTERNAL),
             (unsigned)heap_caps_get_free_size(MALLOC_CAP_DMA));
    esp_wifi_set_mode(WIFI_MODE_STA);
    ESP_LOGI(TAG, "wifi mode sta");
    esp_wifi_set_storage(WIFI_STORAGE_RAM);
    err = esp_wifi_start();
    if (err != ESP_OK) {
        ESP_LOGE(TAG, "wifi start failed: %s", esp_err_to_name(err));
        return err;
    }
    ESP_LOGI(TAG, "wifi started");
    /* 不连 AP 时默认 modem sleep 可能漏收 ESP-NOW 包（会话中断、
     * 冷却不建立 → 反复重触发），强制关省电保接收。 */
    esp_wifi_set_ps(WIFI_PS_NONE);
    /* 官方 espnow 例程在 start 之后设信道；start 前设可能不生效，
     * 两台设备信道不一致会导致收不到对方广播 */
    esp_wifi_set_channel(1, WIFI_SECOND_CHAN_NONE);

    err = esp_now_init();
    if (err != ESP_OK) {
        ESP_LOGE(TAG, "esp_now_init failed: %s", esp_err_to_name(err));
        return err;
    }
    ESP_LOGI(TAG, "esp_now ready");
    esp_now_register_send_cb(on_send);
    esp_now_register_recv_cb(on_recv);

    esp_now_peer_info_t peer;
    memset(&peer, 0, sizeof(peer));
    memcpy(peer.peer_addr, s_bcast, 6);
    peer.channel = 1;
    peer.ifidx   = WIFI_IF_STA;
    peer.encrypt = false;
    err = esp_now_add_peer(&peer);
    if (err != ESP_OK && err != ESP_ERR_ESPNOW_EXIST) {
        ESP_LOGE(TAG, "add peer failed: %s", esp_err_to_name(err));
        return err;
    }

    memset(&s_sess, 0, sizeof(s_sess));
    /* 任务栈 6KB：完成交换时会嵌套 card_store 写 SPIFFS（其内部
     * write_index_json 用静态 2KB 缓冲）+ UI 横幅操作，4096 会栈
     * 溢出（表现：发现对方后随机白屏/卡死）。8192 时在 LVGL
     * draw buf 150KB + wifi 缓冲 + boot_term 任务的组合下会
     * 分配失败（task create failed），故折中 6144。 */
    if (xTaskCreatePinnedToCore(nfc_task, "espnow_nfc", 6144, NULL, 2, NULL, 0) != pdPASS) {
        ESP_LOGE(TAG, "task create failed");
        return ESP_FAIL;
    }
    ESP_LOGI(TAG, "esp-now nfc ready");
    return ESP_OK;
}
