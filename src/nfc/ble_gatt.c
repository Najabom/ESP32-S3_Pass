/* ============================================================================
 * ble_gatt.c - BLE GATT 服务：远程配置 + SD 素材上传
 *
 * 功能（对应 ui_menu.c 蓝牙页 / 手机 App）：
 *   - 设备信息服务 0x180A：软件版本（只读，FW_VERSION）
 *   - 设置服务 0xFFE0：
 *       0xFFE1 current.txt 读写（≤47B，禁路径字符；写后自动热重载）
 *       0xFFE2 ui_text.txt 读写（≤128B）
 *       0xFFE3 ip.txt       读写（≤256B；ESP-NOW 每 10s 自动重载）
 *       0xFFE4 日志级别     读写（warn/info/debug/error/verbose）
 *       0xFFE5 reload       只写（写任意字节 → 角色热重载）
 *       0xFFE6 uid / 0xFFE7 motto / 0xFFE8 uname 读写（名片字段，NVS）
 *       0xFFE9 time       只写（4B 小端 unix 秒，手机每次连接校准墙钟）
 *       0xFFEE cardclear  只写（写任意字节 → 清空交换历史：删 cards.txt
 *                            与 cards/ 全部 bin + 清冷却 NVS，同步执行）
 *   - SD 服务 0xFFF0：
 *       0xFFF1 角色目录列表  只读（每行一个角色名，分段返回）
 *       0xFFF2 文件读取      先写 "PATH:<path>" 打开，后读（每读返回
 *                            下一段 ≤500B，读空 = EOF；路径禁 ".."）
 *       0xFFF3 素材上传      写协议：START:<角色>/<文件>:<总字节>
 *                            → 分块数据（每块 ≤500B）→ END
 *                            白名单：<角色>/loop.mjpeg | intro.mjpeg |
 *                            portrait.bin；大小 ≤4MB；30s 无数据超时中断
 *       0xFFF4 上传状态      通知（须订阅）：OK / PROG:<pct> / DONE /
 *                            ERR:<原因>
 *       0xFFF5 设备识别码    只读（4B LE "PSUP"；PC/手机端连接后凭此
 *                            确认是本族设备，广播层另有 Service Data
 *                            0xFFE0="AP" 供扫描阶段过滤）
 *       0xFFF6 删除角色      只写（角色名；异步递归删 /sdcard/<名>/，
 *                            禁删 config/cards/fonts；完成后刷新列表）
 *
 * 线程与锁：
 *   - NimBLE 回调运行在 nimble host 任务（栈 8K，见 sdkconfig）；
 *     所有 SD 文件操作持 bsp_sdcard_lock，UI 通知持 bsp_lvgl_lock；
 *   - 上传分块在回调内同步落盘（500B @ 5MHz 仅数 ms，可接受）；
 *   - 超时检测用 1s esp_timer（回调上下文短操作，安全）。
 *
 * 已知限制：
 *   - 广播开关不持久化（重启恢复开启）；
 *   - 文件读取为游标式单路会话，一个连接内同时只支持一个打开文件；
 *   - 上传中途拔卡/断电会残留半截文件（下次 START 以 "wb" 覆盖）。
 * ==========================================================================*/

// src/nfc/ble_gatt.c
#include "ble_gatt.h"
#include "bsp_sdcard.h"
#include "ui_menu.h"
#include "nfc_profile.h"
#include "card_store.h"
#include "card_log.h"
#include "espnow_nfc.h"
#include "nfc_proto.h"
#include "roles.h"
#include "mjpeg_player.h"
#include "version.h"
#include "time_sync.h"

#include <stdio.h>
#include <errno.h>
#include <string.h>
#include <stdlib.h>
#include <strings.h>
#include <stdbool.h>
#include <sys/unistd.h>
#include <sys/stat.h>
#include <dirent.h>
#include <sys/stat.h>
#include "freertos/FreeRTOS.h"
#include "freertos/task.h"
#include "freertos/queue.h"
#include "esp_log.h"
#include "esp_timer.h"
#include "esp_mac.h"
#include "esp_heap_caps.h"
#include "nimble/nimble_port.h"
#include "nimble/nimble_port_freertos.h"
#include "host/ble_hs.h"
#include "host/util/util.h"
#include "services/gap/ble_svc_gap.h"
#include "services/gatt/ble_svc_gatt.h"

static const char *TAG = "BLE";

/* ---------- UUID ---------- */
#define UUID_SVC_DEVFOUND  0x180A
#define UUID_CHR_SWREV     0x2A28
#define UUID_SVC_SETTINGS  0xFFE0
#define UUID_CHR_CURRENT   0xFFE1   /* current.txt */
#define UUID_CHR_UITEXT    0xFFE2   /* ui_text.txt */
#define UUID_CHR_IPCFG     0xFFE3   /* ip.txt */
#define UUID_CHR_LOGLEVEL  0xFFE4
#define UUID_CHR_RELOAD    0xFFE5
#define UUID_CHR_UID       0xFFE6
#define UUID_CHR_MOTTO     0xFFE7
#define UUID_CHR_UNAME     0xFFE8
#define UUID_CHR_TIME      0xFFE9   /* unix 秒（只写，手机每次连接校准墙钟） */
#define UUID_CHR_BTNAME    0xFFEA   /* 设备名（读写，≤15B；写后重写 bt.txt 并重开广播） */
#define UUID_CHR_CARDCLR   0xFFEE   /* 清空交换历史（只写，任意字节） */
#define UUID_SVC_SD        0xFFF0
#define UUID_CHR_LIST      0xFFF1   /* 角色目录列表 */
#define UUID_CHR_READFILE  0xFFF2   /* 文件分段读取 */
#define UUID_CHR_UPLOAD    0xFFF3   /* 素材上传 */
#define UUID_CHR_UPLSTATUS 0xFFF4   /* 上传状态通知 */
#define UUID_CHR_IDENT     0xFFF5   /* 设备识别码（只读，4B LE "PSUP"） */
#define UUID_CHR_DEL       0xFFF6   /* 删除角色（只写，角色名；异步删目录） */

/* 设备族识别码：PC/手机端预埋同一魔数，扫描（广播 Service Data
 * 0xFFE0="AP"）与连接后（0xFFF5 读取）双重验证，识别不依赖设备名 */
#define BLE_FEATURE_MAGIC  0x50555350UL  /* "PSUP" (LE 字节序: 50 53 55 50) */

/* 具名 UUID（静态初始化需要常量地址，不能用复合字面量） */
static const ble_uuid16_t uuid_svc_devfound  = BLE_UUID16_INIT(UUID_SVC_DEVFOUND);
static const ble_uuid16_t uuid_chr_swrev     = BLE_UUID16_INIT(UUID_CHR_SWREV);
static const ble_uuid16_t uuid_svc_settings  = BLE_UUID16_INIT(UUID_SVC_SETTINGS);
static const ble_uuid16_t uuid_chr_current   = BLE_UUID16_INIT(UUID_CHR_CURRENT);
static const ble_uuid16_t uuid_chr_uitext    = BLE_UUID16_INIT(UUID_CHR_UITEXT);
static const ble_uuid16_t uuid_chr_ipcfg     = BLE_UUID16_INIT(UUID_CHR_IPCFG);
static const ble_uuid16_t uuid_chr_loglevel  = BLE_UUID16_INIT(UUID_CHR_LOGLEVEL);
static const ble_uuid16_t uuid_chr_reload    = BLE_UUID16_INIT(UUID_CHR_RELOAD);
static const ble_uuid16_t uuid_chr_uid       = BLE_UUID16_INIT(UUID_CHR_UID);
static const ble_uuid16_t uuid_chr_motto     = BLE_UUID16_INIT(UUID_CHR_MOTTO);
static const ble_uuid16_t uuid_chr_uname     = BLE_UUID16_INIT(UUID_CHR_UNAME);
static const ble_uuid16_t uuid_chr_time      = BLE_UUID16_INIT(UUID_CHR_TIME);
static const ble_uuid16_t uuid_chr_btname    = BLE_UUID16_INIT(UUID_CHR_BTNAME);
static const ble_uuid16_t uuid_chr_cardclr   = BLE_UUID16_INIT(UUID_CHR_CARDCLR);
static const ble_uuid16_t uuid_svc_sd        = BLE_UUID16_INIT(UUID_SVC_SD);
static const ble_uuid16_t uuid_chr_list      = BLE_UUID16_INIT(UUID_CHR_LIST);
static const ble_uuid16_t uuid_chr_readfile  = BLE_UUID16_INIT(UUID_CHR_READFILE);
static const ble_uuid16_t uuid_chr_upload    = BLE_UUID16_INIT(UUID_CHR_UPLOAD);
static const ble_uuid16_t uuid_chr_uplstatus = BLE_UUID16_INIT(UUID_CHR_UPLSTATUS);
static const ble_uuid16_t uuid_chr_ident     = BLE_UUID16_INIT(UUID_CHR_IDENT);
static const ble_uuid16_t uuid_chr_del       = BLE_UUID16_INIT(UUID_CHR_DEL);

/* ---------- 尺寸 ---------- */
#define CFG_CURRENT_MAX   48
#define CFG_UITEXT_MAX    128
#define CFG_IPCFG_MAX     256
#define CFG_LVL_MAX       8
#define CFG_UID_MAX       20
#define CFG_MOTTO_MAX     64   /* 寄语上限字节数（UTF-8；20 汉字 = 60B）——与 NFC_PROFILE_MOTTO_MAX 同步 */
#define CFG_UNAME_MAX     64   /* 用户名上限字节数（UTF-8；20 汉字 = 60B）——与 NFC_PROFILE_UNAME_MAX 同步 */
#define RD_PATH_MAX       96
#define RD_CHUNK_MAX      500
#define LIST_BUF_MAX      4096
#define UPL_PATH_MAX      90
#define UPL_TOTAL_MAX     (4 * 1024 * 1024)
#define UPL_TIMEOUT_MS    30000

/* 上传文件白名单（目标文件名，不含路径分隔符） */
#define UPL_F_LOOP "loop.mjpeg"
#define UPL_F_INTRO "intro.mjpeg"
#define UPL_F_PORTRAIT "portrait.bin"
#define UPL_F_UITEXT   "ui_text.txt"
#define UPL_F_SELF     "self.bin"
#define UPL_SELF_PREFIX ":self:"   /* 路径哨兵：cardstore 分区，不落 SD */

/* ---------- 状态 ---------- */
typedef enum {
    UPL_IDLE = 0,
    UPL_DATA,
} upload_st_t;

static char      s_dev_name[16] = "PASS-II-XXXX";

static void usbident_print(void);
static void usbident_tmr_cb(void *arg);
static bool      s_adv_enabled = true;    /* 默认开（PC/手机随时可连）；bt.txt enable=0 可关 */
static volatile bool s_connected = false;
static volatile int  s_upload_progress = -1;
static uint16_t  s_conn = 0;          /* 当前连接句柄（0 = 无） */
static uint16_t  s_status_val_handle = 0;

/* 文件读取会话（单路游标） */
static FILE     *s_rd_f = NULL;
static bool      s_rd_failed = false;
static uint32_t  s_stat_size = 0;      /* STAT: 查询结果 */
static bool      s_stat_pending = false;

/* 目录列表缓存 */
static char     *s_list_buf = NULL;
static size_t    s_list_len = 0;
static size_t    s_list_off = 0;

/* 上传会话 */
static upload_st_t s_upl_st = UPL_IDLE;
static char      s_upl_path[UPL_PATH_MAX];
static uint8_t  *s_upl_buf = NULL;   /* 非空=BUF 模式（PSRAM 整块缓冲，DATA 阶段零 SD 访问）；NULL=FILE 回落 */
static size_t    s_upl_total = 0;
static size_t    s_upl_got = 0;
static int       s_upl_last_pct = -1;
static FILE     *s_upl_f = NULL;
static uint32_t  s_upl_last_ms = 0;

static void advertise(void);
static void notify_status(const char *msg);

/* ============================================================================
 * 工具
 * ==========================================================================*/

static uint32_t now_ms(void) {
    return (uint32_t)(esp_timer_get_time() / 1000);
}

/* 配置文件的 VFS 路径 */
#define SD_CFG_DIR "/sdcard/config"

static void cfg_path(char *out, size_t cap, const char *file) {
    snprintf(out, cap, "%s/%s", SD_CFG_DIR, file);
}

/* /sdcard/config/bt.txt：蓝牙配置（SD 锁内读取）
 *   enable=0          关闭开机自动广播（默认 1 = 开）
 *   name=PASS-II-XXX  自定义设备名（≤15 字节，省略 = MAC 自动名）
 */
static void ble_load_cfg(void) {
    bsp_sdcard_lock();
    FILE *f = fopen("/sdcard/config/bt.txt", "r");
    if (f != NULL) {
        char line[80];
        while (fgets(line, sizeof(line), f)) {
            char *nl = strchr(line, '\n');
            if (nl) *nl = '\0';
            if (strncmp(line, "enable=", 7) == 0) {
                s_adv_enabled = (atoi(line + 7) != 0);
            } else if (strncmp(line, "name=", 5) == 0) {
                strncpy(s_dev_name, line + 5, sizeof(s_dev_name) - 1);
                s_dev_name[sizeof(s_dev_name) - 1] = '\0';
                /* 控制字符会破坏广播/显示，直接截断 */
                for (char *p = s_dev_name; *p; p++) {
                    if ((uint8_t)*p < 0x20) { *p = '\0'; break; }
                }
            }
        }
        fclose(f);
        ESP_LOGI(TAG, "cfg: adv=%d name=%s", s_adv_enabled ? 1 : 0, s_dev_name);
    } else {
        ESP_LOGI(TAG, "no bt.txt, defaults: adv on");
    }
    bsp_sdcard_unlock();
}

/* 读整文件到缓冲（去尾部换行）。SD 锁内调用。 */
static esp_err_t read_config_file(const char *path, char *buf, size_t cap, size_t *out_len) {
    *out_len = 0;
    FILE *f = fopen(path, "r");
    if (f == NULL) return ESP_ERR_NOT_FOUND;
    size_t n = fread(buf, 1, cap - 1, f);
    fclose(f);
    while (n > 0 && (buf[n - 1] == '\n' || buf[n - 1] == '\r')) n--;
    buf[n] = '\0';
    *out_len = n;
    return ESP_OK;
}

static esp_err_t write_config_file(const char *path, const uint8_t *data, size_t len) {
    FILE *f = fopen(path, "w");
    if (f == NULL) {
        /* 卡上无 /sdcard/config 目录（旧卡拷贝/新卡）→ 自动建目录后重试一次。
         * FATFS mkdir 对已存在目录返回 EEXIST，忽略即可。 */
        mkdir("/sdcard/config", 0755);
        f = fopen(path, "w");
    }
    if (f == NULL) return ESP_ERR_NOT_FOUND;
    size_t w = fwrite(data, 1, len, f);
    int rc = fclose(f);
    if (w != len || rc != 0) return ESP_ERR_INVALID_STATE;
    return ESP_OK;
}

/* 名称合法性（黑名单制）：长度 1..max-1；禁控制字符与 FAT/Windows
 * 路径禁用符；其余全部放行 —— 含 UTF-8 多字节（中文角色名）与空格。
 * 原 ASCII 白名单制把中文角色名全拒（ERR:path / 切角失败），
 * SD 卡 FATFS UTF-8+LFN 对中文目录支持早已验证可用。 */
static bool name_ok(const char *s, size_t len, size_t max) {
    if (len == 0 || len >= max) return false;
    for (size_t i = 0; i < len; i++) {
        unsigned char c = (unsigned char)s[i];
        if (c < 0x20 || c == 0x7F) return false;
        switch (c) {
            case '\\': case '/': case ':': case '*': case '?':
            case '"':  case '<': case '>': case '|':
                return false;
            default:
                break;
        }
    }
    return true;
}

/* ============================================================================
 * GATT 特性处理
 * ==========================================================================*/

/* 0xFFE1 current.txt 读：返回内存镜像（不碰 SD）。
 * 镜像由 cfg_writer_task 启动时从 SD 读入 + 异步写成功后更新。
 * 旧实现每次 READ 持 SD 锁读文件——播放/风暴期拿不到锁返回空，
 * PC 端"当前角色"显示为空。 */
static char s_current_mirror[CFG_CURRENT_MAX + 1];   /* 空串 = 未配置 */

static int handle_current_read(uint16_t conn_handle, uint16_t attr_handle,
                               struct ble_gatt_access_ctxt *ctxt, void *arg) {
    (void)conn_handle; (void)attr_handle; (void)arg;
    ESP_LOGW(TAG, "att: %s op=%d", __func__, ctxt->op);
    size_t n = strlen(s_current_mirror);
    /* NimBLE 契约：offset==0 首读时 ctxt->om 就是响应缓冲，
     * 必须 append 数据，不能替换 om（new_om=0 分支不拷贝） */
    return os_mbuf_append(ctxt->om, s_current_mirror, (uint16_t)n) == 0 ? 0 : BLE_ATT_ERR_INSUFFICIENT_RES;
}

/* ---------- 配置异步写：GATT 回调只入队即返，SD 写在独立任务完成 ----------
 * 旧实现三个 config 写特性在 nimble host 任务里 bsp_sdcard_lock() 阻塞等锁：
 * MJPEG 播放每帧持锁、SD CRC 风暴期持锁数秒 → host 阻塞 → ATT 超时
 * （PC 端 UnlikelyError / 监督超时掉线 unconnected）。 */
#define CFGWR_CHR_CURRENT 1
#define CFGWR_CHR_UITEXT  2
#define CFGWR_CHR_IPCFG   3
#define CFGWR_CHR_BTNAME  4
#define CFGWR_CHR_DEL     5
#define CFGWR_CHR_REBUILD 6   /* 无数据：角色列表缓存重建 */
#define CFGWR_CHR_UPL_FLUSH 7 /* 无数据：上传缓冲落盘（s_upl_buf/s_upl_path/s_upl_total） */

typedef struct {
    uint8_t chr;
    uint8_t len;
    char    data[CFG_IPCFG_MAX];   /* 最大输入 = ipcfg 256B；ui_text 128 也容纳 */
} cfgwr_item_t;

static QueueHandle_t s_cfgwr_q = NULL;

/* bt.txt 替换 name= 行（保留 enable= 等其它行）；SD 锁内调用 */
static esp_err_t bt_cfg_set_name(const char *name, size_t len) {
    char buf[256] = {0};
    size_t n = 0;
    FILE *f = fopen("/sdcard/config/bt.txt", "r");
    if (f != NULL) {
        n = fread(buf, 1, sizeof(buf) - 1, f);
        fclose(f);
    }
    char out[288] = {0};
    size_t o = 0;
    bool replaced = false;
    const char *p = buf;
    while (n > 0 && *p) {
        const char *nl = strchr(p, '\n');
        size_t linelen = nl ? (size_t)(nl - p + 1) : strlen(p);
        if (strncmp(p, "name=", 5) == 0) {
            o += (size_t)snprintf(out + o, sizeof(out) - o, "name=%.*s\n",
                                  (int)len, name);
            replaced = true;
        } else if (o + linelen < sizeof(out)) {
            memcpy(out + o, p, linelen);
            o += linelen;
        }
        p += linelen;
        n = (n > linelen) ? n - linelen : 0;
    }
    if (!replaced) {
        o += (size_t)snprintf(out + o, sizeof(out) - o, "name=%.*s\n",
                              (int)len, name);
    }
    FILE *w = fopen("/sdcard/config/bt.txt", "w");
    if (w == NULL) return ESP_ERR_NOT_FOUND;
    size_t wr = fwrite(out, 1, o, w);
    int rc = fclose(w);
    return (wr == o && rc == 0) ? ESP_OK : ESP_ERR_INVALID_STATE;
}

/* 递归删除目录（内容 + 自身）。在 cfg_writer_task 内调用（持 SD 锁）。
 * FATFS 的 dirent.d_type 提供目录类型判定。 */
static esp_err_t rmrf(const char *path) {
    DIR *d = opendir(path);
    if (d == NULL) return ESP_ERR_NOT_FOUND;
    struct dirent *e;
    while ((e = readdir(d)) != NULL) {
        if (strcmp(e->d_name, ".") == 0 || strcmp(e->d_name, "..") == 0) continue;
        char sub[UPL_PATH_MAX + 288];
        snprintf(sub, sizeof(sub), "%s/%s", path, e->d_name);
        if (e->d_type == DT_DIR) {
            if (rmrf(sub) == ESP_OK) rmdir(sub);
        } else {
            unlink(sub);
        }
    }
    closedir(d);
    return rmdir(path) == 0 ? ESP_OK : ESP_FAIL;
}

/* 前向声明（定义见 0xFFF1 段）——cfg_writer_task 删除角色后刷新列表 */
static void invalidate_list(void);
static void rebuild_list_cache(void);

static void cfg_writer_task(void *arg) {
    (void)arg;
    /* 任务入口：读一次 current.txt 建立镜像（此时 SD 已挂载） */
    if (bsp_sdcard_is_mounted()) {
        char path[64];
        size_t n = 0;
        cfg_path(path, sizeof(path), "current.txt");
        bsp_sdcard_lock();
        read_config_file(path, s_current_mirror, CFG_CURRENT_MAX, &n);
        bsp_sdcard_unlock();
        if (n >= CFG_CURRENT_MAX) n = CFG_CURRENT_MAX - 1;
        s_current_mirror[n] = '\0';
        ESP_LOGI(TAG, "current mirror: \"%s\"", s_current_mirror);
    }
    cfgwr_item_t it;
    for (;;) {
        if (xQueueReceive(s_cfgwr_q, &it, portMAX_DELAY) != pdTRUE) continue;
        if (it.chr == CFGWR_CHR_BTNAME) {
            /* 设备名：bt.txt 保留式改写 */
            esp_err_t err;
            bsp_sdcard_lock();
            err = bt_cfg_set_name(it.data, it.len);
            bsp_sdcard_unlock();
            if (err != ESP_OK) {
                ESP_LOGW(TAG, "async bt.txt write failed: %s（可重试）",
                         esp_err_to_name(err));
                continue;
            }
            snprintf(s_dev_name, sizeof(s_dev_name), "%.*s", (int)it.len, it.data);
            ESP_LOGI(TAG, "device name -> %s", s_dev_name);
            usbident_print();
            /* 重开广播：连接中 advertise() 自行跳过，断开后
             * keepalive（10s）会用新名恢复广播 */
            ble_gap_adv_stop();
            advertise();
            continue;
        }
        if (it.chr == CFGWR_CHR_DEL) {
            /* 删除角色目录（可能含大文件，异步执行避免阻塞 ATT） */
            char name[CFG_CURRENT_MAX + 1];
            memcpy(name, it.data, it.len);
            name[it.len] = '\0';
            char dir[UPL_PATH_MAX + 16];
            snprintf(dir, sizeof(dir), "/sdcard/%s", name);
            bsp_sdcard_lock();
            esp_err_t err = rmrf(dir);
            bsp_sdcard_unlock();
            if (err != ESP_OK) {
                ESP_LOGW(TAG, "delete %s failed: %s", dir, esp_err_to_name(err));
                /* 失败立即通知 PC（否则 PC 空等 30s 才报超时） */
                notify_status("ERR:delete");
                continue;
            }
            ESP_LOGI(TAG, "role deleted: %s", dir);
            invalidate_list();
            rebuild_list_cache();
            /* 与 REBUILD 一致：删除成功即通知 LIST:OK（PC 等此信号再拉列表；
             * 此前 DELETE 分支无通知 → 永远超时提示 LIST:OK 未到） */
            notify_status("LIST:OK");
            if (strcmp(name, s_current_mirror) == 0) {
                ESP_LOGI(TAG, "deleted active role, reloading player");
                mjpeg_player_request_reload();
            }
            continue;
        }
        if (it.chr == CFGWR_CHR_REBUILD) {
            /* 角色列表缓存重建（upload DONE 后由 ATT 线程入队，这里持锁执行） */
            invalidate_list();
            rebuild_list_cache();
            /* 重建完成通知：PC 端等此信号再拉列表（此前上传完成即刷，
             * 读到的还是旧缓存 → 新角色不出现） */
            notify_status("LIST:OK");
            continue;
        }
        if (it.chr == CFGWR_CHR_UPL_FLUSH) {
            /* 上传缓冲落盘（BUF 模式 END 入队；host 已退出，可安全持锁）。
             * fopen 重试 3×300ms；成功才 notify DONE + 重建列表缓存 */
            uint8_t *buf = s_upl_buf;
            s_upl_buf = NULL;
            if (buf == NULL) {
                notify_status("ERR:nobuf");
                continue;
            }
            if (!bsp_sdcard_is_mounted()) {
                heap_caps_free(buf);
                ESP_LOGW(TAG, "flush abort: sd not mounted");
                notify_status("ERR:nocard");
                continue;
            }
            /* self.bin：打包 self_blob_hdr_t（仅头像）写入 cardstore 分区。
             * 后续可由 ESP-NOW 发卡（build_self_blob 优先用它） */
            if (strncmp(s_upl_path, UPL_SELF_PREFIX, strlen(UPL_SELF_PREFIX)) == 0) {
                size_t blob_len = sizeof(self_blob_hdr_t) + CARD_AVATAR_SIZE;
                uint8_t *blob = heap_caps_malloc(blob_len,
                                                 MALLOC_CAP_SPIRAM | MALLOC_CAP_8BIT);
                esp_err_t se = ESP_FAIL;
                if (blob != NULL) {
                    self_blob_hdr_t *sh = (self_blob_hdr_t *)blob;
                    sh->avatar_len = CARD_AVATAR_SIZE;
                    sh->sticker_cnt = 0;
                    memset(sh->sticker_len, 0, sizeof(sh->sticker_len));
                    memcpy(blob + sizeof(self_blob_hdr_t), buf, CARD_AVATAR_SIZE);
                    se = card_store_save_self(blob, blob_len);
                    heap_caps_free(blob);
                }
                heap_caps_free(buf);
                s_upload_progress = -1;
                ESP_LOGW(TAG, "self.bin flush: %s (%u B)", esp_err_to_name(se), (unsigned)blob_len);
                notify_status(se == ESP_OK ? "DONE" : "ERR:open");
                continue;
            }
            bsp_sdcard_lock();
            FILE *f = NULL;
            for (int i = 0; i < 3 && f == NULL; i++) {
                errno = 0;
                f = fopen(s_upl_path, "wb");
                if (f == NULL) {
                    ESP_LOGW(TAG, "flush fopen try%d failed: %s errno=%d",
                             i + 1, s_upl_path, errno);
                    vTaskDelay(pdMS_TO_TICKS(300));
                }
            }
            esp_err_t werr = ESP_FAIL;
            if (f != NULL) {
                size_t w = fwrite(buf, 1, s_upl_total, f);
                bool closed = (fclose(f) == 0);
                if (w == s_upl_total && closed) werr = ESP_OK;
            }
            bsp_sdcard_unlock();
            heap_caps_free(buf);
            s_upload_progress = -1;
            if (werr == ESP_OK) {
                invalidate_list();
                /* ui_text.txt 上传 → 热重载（read_ui_text 重读角色绑定文件） */
                const char *fname = strrchr(s_upl_path, '/');
                fname = fname ? fname + 1 : s_upl_path;
                if (strcmp(fname, UPL_F_UITEXT) == 0) {
                    mjpeg_player_request_reload();
                }
                cfgwr_item_t rit = { .chr = CFGWR_CHR_REBUILD };
                xQueueSend(s_cfgwr_q, &rit, 0);
                ESP_LOGI(TAG, "upload flushed: %s (%u B)", s_upl_path, (unsigned)s_upl_total);
                notify_status("DONE");
                /* DONE 重发一次：BLE att notify 偶发丢失（用户端曾卡 100%） */
                vTaskDelay(pdMS_TO_TICKS(100));
                notify_status("DONE");
            } else {
                unlink(s_upl_path);
                ESP_LOGW(TAG, "upload flush failed: %s errno=%d", s_upl_path, errno);
                notify_status("ERR:open");
            }
            continue;
        }
        const char *fname = it.chr == CFGWR_CHR_CURRENT ? "current.txt"
                          : it.chr == CFGWR_CHR_UITEXT  ? "ui_text.txt"
                                                        : "ip.txt";
        char path[96];
        if (it.chr == CFGWR_CHR_UITEXT && s_current_mirror[0]) {
            /* ui_text 与角色绑定：写入当前角色目录（0xFFE2 语义 = 当前角色文本） */
            snprintf(path, sizeof(path), "/sdcard/%s/%s", s_current_mirror, fname);
        } else {
            cfg_path(path, sizeof(path), fname);
        }
        esp_err_t err;
        bsp_sdcard_lock();
        err = write_config_file(path, (const uint8_t *)it.data, it.len);
        bsp_sdcard_unlock();
        if (err != ESP_OK) {
            ESP_LOGW(TAG, "async %s write failed: %s（可重试）",
                     fname, esp_err_to_name(err));
            continue;
        }
        ESP_LOGI(TAG, "%s <- %d B (async)", fname, it.len);
        if (it.chr == CFGWR_CHR_CURRENT) {
            memcpy(s_current_mirror, it.data, it.len);
            s_current_mirror[it.len] = '\0';
            mjpeg_player_request_reload();
        } else if (it.chr == CFGWR_CHR_IPCFG) {
            ESP_LOGI(TAG, "ip.txt 更新（ESP-NOW 10s 内重载）");
        } else if (it.chr == CFGWR_CHR_UITEXT) {
            /* 当前角色的 ui_text.txt 已更新：热重载立即按新文本刷新显示 */
            ESP_LOGI(TAG, "ui_text.txt 已更新（当前角色文本，热重载生效）");
            mjpeg_player_request_reload();
        }
    }
}

static int handle_current_write(uint16_t conn_handle, uint16_t attr_handle,
                                struct ble_gatt_access_ctxt *ctxt, void *arg) {
    (void)conn_handle; (void)attr_handle; (void)arg;
    ESP_LOGW(TAG, "att: %s op=%d", __func__, ctxt->op);
    uint16_t len = OS_MBUF_PKTLEN(ctxt->om);
    if (len == 0 || len >= CFG_CURRENT_MAX) return BLE_ATT_ERR_INVALID_ATTR_VALUE_LEN;
    cfgwr_item_t it = { .chr = CFGWR_CHR_CURRENT, .len = (uint8_t)len };
    os_mbuf_copydata(ctxt->om, 0, len, it.data);
    if (!name_ok(it.data, len, CFG_CURRENT_MAX)) return BLE_ATT_ERR_INVALID_ATTR_VALUE_LEN;
    if (!s_cfgwr_q || xQueueSend(s_cfgwr_q, &it, 0) != pdTRUE)
        return BLE_ATT_ERR_UNLIKELY;   /* 队列未就绪/满 */
    /* 镜像先行更新：PC 写后立即读回应得新值 */
    memcpy(s_current_mirror, it.data, len);
    s_current_mirror[len] = '\0';
    return 0;   /* 落盘由 cfg_writer_task 异步完成（含热重载） */
}

static int handle_ui_text_write(uint16_t conn_handle, uint16_t attr_handle,
                                struct ble_gatt_access_ctxt *ctxt, void *arg) {
    (void)conn_handle; (void)attr_handle; (void)arg;
    ESP_LOGW(TAG, "att: %s op=%d", __func__, ctxt->op);
    uint16_t len = OS_MBUF_PKTLEN(ctxt->om);
    if (len > CFG_UITEXT_MAX) return BLE_ATT_ERR_INVALID_ATTR_VALUE_LEN;
    cfgwr_item_t it = { .chr = CFGWR_CHR_UITEXT, .len = (uint8_t)len };
    if (len > sizeof(it.data)) return BLE_ATT_ERR_INVALID_ATTR_VALUE_LEN;   /* 防缓冲溢出 */
    os_mbuf_copydata(ctxt->om, 0, len, it.data);
    if (!s_cfgwr_q || xQueueSend(s_cfgwr_q, &it, 0) != pdTRUE)
        return BLE_ATT_ERR_UNLIKELY;
    return 0;
}

static int handle_ipcfg_write(uint16_t conn_handle, uint16_t attr_handle,
                              struct ble_gatt_access_ctxt *ctxt, void *arg) {
    (void)conn_handle; (void)attr_handle; (void)arg;
    ESP_LOGW(TAG, "att: %s op=%d", __func__, ctxt->op);
    uint16_t len = OS_MBUF_PKTLEN(ctxt->om);
    if (len > CFG_IPCFG_MAX) return BLE_ATT_ERR_INVALID_ATTR_VALUE_LEN;
    cfgwr_item_t it = { .chr = CFGWR_CHR_IPCFG, .len = (uint8_t)len };
    if (len > sizeof(it.data)) return BLE_ATT_ERR_INVALID_ATTR_VALUE_LEN;   /* 防缓冲溢出 */
    os_mbuf_copydata(ctxt->om, 0, len, it.data);
    if (!s_cfgwr_q || xQueueSend(s_cfgwr_q, &it, 0) != pdTRUE)
        return BLE_ATT_ERR_UNLIKELY;
    return 0;
}

/* 0xFFE3 读回：/sdcard/config/ip.txt 原文（无文件/空 → 空响应 = 全通）。
 * SD 锁仅覆盖 fopen+fread 两瞬（GATT 回调不能长持锁，与写侧同一纪律）。 */
static int handle_ipcfg_read(uint16_t conn_handle, uint16_t attr_handle,
                             struct ble_gatt_access_ctxt *ctxt, void *arg) {
    (void)conn_handle; (void)attr_handle; (void)arg;
    ESP_LOGW(TAG, "att: %s op=%d", __func__, ctxt->op);
    char path[RD_PATH_MAX];
    cfg_path(path, sizeof(path), "ip.txt");
    bsp_sdcard_lock();
    FILE *f = fopen(path, "r");
    if (f == NULL) {
        bsp_sdcard_unlock();
        ESP_LOGI(TAG, "ipcfg read back: (no file)");
        return 0;                     /* 空响应：客户端收到 b\"\" = 全通 */
    }
    char buf[CFG_IPCFG_MAX];
    size_t got = fread(buf, 1, sizeof(buf), f);
    fclose(f);
    bsp_sdcard_unlock();
    if (got >= sizeof(buf)) got = sizeof(buf) - 1;   /* 留 \\0 余量不用 */
    ESP_LOGI(TAG, "ipcfg read back: %u B", (unsigned)got);
    return os_mbuf_append(ctxt->om, buf, (uint16_t)got) == 0
               ? 0 : BLE_ATT_ERR_INSUFFICIENT_RES;
}

/* 0xFFF6 删除角色：只写角色名，异步递归删 /sdcard/<name>/ 并刷新列表缓存 */
static int handle_del_write(uint16_t conn_handle, uint16_t attr_handle,
                            struct ble_gatt_access_ctxt *ctxt, void *arg) {
    (void)conn_handle; (void)attr_handle; (void)arg;
    ESP_LOGW(TAG, "att: %s op=%d", __func__, ctxt->op);
    uint16_t len = OS_MBUF_PKTLEN(ctxt->om);
    if (len == 0 || len >= CFG_CURRENT_MAX) return BLE_ATT_ERR_INVALID_ATTR_VALUE_LEN;
    char buf[CFG_CURRENT_MAX + 1];
    os_mbuf_copydata(ctxt->om, 0, len, (uint8_t *)buf);
    buf[len] = '\0';
    if (!name_ok(buf, len, CFG_CURRENT_MAX)) return BLE_ATT_ERR_INVALID_ATTR_VALUE_LEN;
    if (strcmp(buf, ".") == 0 || strcmp(buf, "..") == 0)
        return BLE_ATT_ERR_INVALID_ATTR_VALUE_LEN;
    /* 禁删系统目录（SD 根一级） */
    static const char *sys_dirs[] = { "config", "cards", "fonts" };
    for (size_t i = 0; i < sizeof(sys_dirs) / sizeof(sys_dirs[0]); i++) {
        if (strcasecmp(buf, sys_dirs[i]) == 0)
            return BLE_ATT_ERR_INVALID_ATTR_VALUE_LEN;
    }
    cfgwr_item_t it = { .chr = CFGWR_CHR_DEL, .len = (uint8_t)len };
    memcpy(it.data, buf, len);
    if (!s_cfgwr_q || xQueueSend(s_cfgwr_q, &it, 0) != pdTRUE)
        return BLE_ATT_ERR_UNLIKELY;
    return 0;
}

/* 日志级别读写 */
static int handle_loglevel(uint16_t conn_handle, uint16_t attr_handle,
                           struct ble_gatt_access_ctxt *ctxt, void *arg) {
    (void)conn_handle; (void)attr_handle; (void)arg;
    ESP_LOGW(TAG, "att: %s op=%d", __func__, ctxt->op);
    if (ctxt->op == BLE_GATT_ACCESS_OP_READ_CHR) {
        const char *name;
        switch (esp_log_level_get("*")) {
            case ESP_LOG_ERROR:   name = "error";   break;
            case ESP_LOG_WARN:    name = "warn";    break;
            case ESP_LOG_INFO:    name = "info";    break;
            case ESP_LOG_DEBUG:   name = "debug";   break;
            default:              name = "verbose"; break;
        }
        return os_mbuf_append(ctxt->om, name, (uint16_t)strlen(name)) == 0 ? 0 : BLE_ATT_ERR_INSUFFICIENT_RES;
    }
    uint16_t len = OS_MBUF_PKTLEN(ctxt->om);
    if (len == 0 || len >= CFG_LVL_MAX) return BLE_ATT_ERR_INVALID_ATTR_VALUE_LEN;
    char buf[CFG_LVL_MAX];
    os_mbuf_copydata(ctxt->om, 0, len, buf);
    buf[len] = '\0';
    esp_log_level_t lv;
    if (strcasecmp(buf, "error") == 0) lv = ESP_LOG_ERROR;
    else if (strcasecmp(buf, "warn") == 0) lv = ESP_LOG_WARN;
    else if (strcasecmp(buf, "info") == 0) lv = ESP_LOG_INFO;
    else if (strcasecmp(buf, "debug") == 0) lv = ESP_LOG_DEBUG;
    else if (strcasecmp(buf, "verbose") == 0) lv = ESP_LOG_VERBOSE;
    else return BLE_ATT_ERR_INVALID_ATTR_VALUE_LEN;
    esp_log_level_set("*", lv);
    ESP_LOGI(TAG, "log level -> %s", buf);
    return 0;
}

/* 设置服务 0xFFE0 各特性分发 */
static int settings_access_cb(uint16_t conn_handle, uint16_t attr_handle,
                              struct ble_gatt_access_ctxt *ctxt, void *arg) {
    (void)conn_handle; (void)attr_handle; (void)arg;
    ESP_LOGW(TAG, "att: %s chr=0x%04x op=%d", __func__, ble_uuid_u16(ctxt->chr->uuid), ctxt->op);
    uint16_t chr = ble_uuid_u16(ctxt->chr->uuid);
    switch (chr) {
        case UUID_CHR_CURRENT:
            if (ctxt->op == BLE_GATT_ACCESS_OP_READ_CHR) return handle_current_read(conn_handle, attr_handle, ctxt, arg);
            if (ctxt->op == BLE_GATT_ACCESS_OP_WRITE_CHR) return handle_current_write(conn_handle, attr_handle, ctxt, arg);
            break;
        case UUID_CHR_UITEXT:
            if (ctxt->op == BLE_GATT_ACCESS_OP_WRITE_CHR) return handle_ui_text_write(conn_handle, attr_handle, ctxt, arg);
            break;
        case UUID_CHR_IPCFG:
            if (ctxt->op == BLE_GATT_ACCESS_OP_READ_CHR) return handle_ipcfg_read(conn_handle, attr_handle, ctxt, arg);
            if (ctxt->op == BLE_GATT_ACCESS_OP_WRITE_CHR) return handle_ipcfg_write(conn_handle, attr_handle, ctxt, arg);
            break;
        case UUID_CHR_LOGLEVEL:
            return handle_loglevel(conn_handle, attr_handle, ctxt, arg);
        case UUID_CHR_UID:
            if (ctxt->op == BLE_GATT_ACCESS_OP_WRITE_CHR) {
                uint16_t len = OS_MBUF_PKTLEN(ctxt->om);
                if (len > CFG_UID_MAX) return BLE_ATT_ERR_INVALID_ATTR_VALUE_LEN;
                uint8_t buf[CFG_UID_MAX + 1];
                os_mbuf_copydata(ctxt->om, 0, len, buf);
                buf[len] = '\0';
                return nfc_profile_set_uid((const char *)buf) == ESP_OK ? 0 : BLE_ATT_ERR_UNLIKELY;
            } else if (ctxt->op == BLE_GATT_ACCESS_OP_READ_CHR) {
                const char *s = nfc_profile_get_uid();
                return os_mbuf_append(ctxt->om, s, (uint16_t)strlen(s)) == 0
                           ? 0 : BLE_ATT_ERR_INSUFFICIENT_RES;
            }
            break;
        case UUID_CHR_MOTTO:
            if (ctxt->op == BLE_GATT_ACCESS_OP_WRITE_CHR) {
                uint16_t len = OS_MBUF_PKTLEN(ctxt->om);
                if (len > CFG_MOTTO_MAX) return BLE_ATT_ERR_INVALID_ATTR_VALUE_LEN;
                uint8_t buf[CFG_MOTTO_MAX + 1];
                os_mbuf_copydata(ctxt->om, 0, len, buf);
                buf[len] = '\0';
                return nfc_profile_set_motto((const char *)buf) == ESP_OK ? 0 : BLE_ATT_ERR_UNLIKELY;
            } else if (ctxt->op == BLE_GATT_ACCESS_OP_READ_CHR) {
                const char *s = nfc_profile_get_motto();
                return os_mbuf_append(ctxt->om, s, (uint16_t)strlen(s)) == 0
                           ? 0 : BLE_ATT_ERR_INSUFFICIENT_RES;
            }
            break;
        case UUID_CHR_UNAME:
            if (ctxt->op == BLE_GATT_ACCESS_OP_WRITE_CHR) {
                uint16_t len = OS_MBUF_PKTLEN(ctxt->om);
                if (len > CFG_UNAME_MAX) return BLE_ATT_ERR_INVALID_ATTR_VALUE_LEN;
                uint8_t buf[CFG_UNAME_MAX + 1];
                os_mbuf_copydata(ctxt->om, 0, len, buf);
                buf[len] = '\0';
                return nfc_profile_set_uname((const char *)buf) == ESP_OK ? 0 : BLE_ATT_ERR_UNLIKELY;
            } else if (ctxt->op == BLE_GATT_ACCESS_OP_READ_CHR) {
                const char *s = nfc_profile_get_uname();
                return os_mbuf_append(ctxt->om, s, (uint16_t)strlen(s)) == 0
                           ? 0 : BLE_ATT_ERR_INSUFFICIENT_RES;
            }
            break;
        case UUID_CHR_TIME:
            /* 只写：4B 小端 unix 秒；6B = unix(4) + tz_min(int16 LE)。
             * 绝对对齐（无防回退守卫）：设备无条件接受上位机时间，
             * 偏移量由 time_sync 内部平移存量记录。 */
            if (ctxt->op == BLE_GATT_ACCESS_OP_WRITE_CHR) {
                uint16_t len = OS_MBUF_PKTLEN(ctxt->om);
                uint8_t raw[6];
                esp_err_t err;
                if (len == 4) {
                    os_mbuf_copydata(ctxt->om, 0, 4, raw);
                    err = time_sync_set_unix(*(uint32_t *)raw);
                    return err == ESP_OK ? 0 : BLE_ATT_ERR_UNLIKELY;
                }
                if (len == 6) {
                    os_mbuf_copydata(ctxt->om, 0, 6, raw);
                    /* int16 LE：PC struct.pack("<Ih") 低字节在前 */
                    int16_t tz = (int16_t)(raw[4] | (raw[5] << 8));
                    err = time_sync_set_tz(tz);
                    if (err == ESP_OK) {
                        uint32_t t = *(uint32_t *)raw;
                        err = time_sync_set_unix(t);
                        if (err == ESP_OK)
                            ESP_LOGI(TAG, "time+tz synced: unix=%lu tz=%dmin",
                                     (unsigned long)t, tz);
                    }
                    return err == ESP_OK ? 0 : BLE_ATT_ERR_UNLIKELY;
                }
                return BLE_ATT_ERR_INVALID_ATTR_VALUE_LEN;
            }
            break;
        case UUID_CHR_CARDCLR:
            /* 只写任意字节：清空交换历史（cards.txt+cards/ 全删）并清冷却 NVS。
             * 同步执行：删文件为轻量元数据操作，可接受在 host 任务内完成 */
            if (ctxt->op == BLE_GATT_ACCESS_OP_WRITE_CHR) {
                esp_err_t cerr = card_log_clear();
                if (cerr != ESP_OK && cerr != ESP_ERR_INVALID_STATE) {
                    return BLE_ATT_ERR_UNLIKELY;
                }
                espnow_nfc_clear_cooldown();
                return 0;
            }
            break;
        case UUID_CHR_BTNAME:
            /* 设备名：READ 返回内存名（零 SD）；WRITE 入队异步改写
             * bt.txt + 更新广播名（host 任务内不碰 SD，见 cfg_writer_task） */
            if (ctxt->op == BLE_GATT_ACCESS_OP_READ_CHR) {
                size_t n = strlen(s_dev_name);
                return os_mbuf_append(ctxt->om, s_dev_name, (uint16_t)n) == 0
                           ? 0 : BLE_ATT_ERR_INSUFFICIENT_RES;
            }
            if (ctxt->op == BLE_GATT_ACCESS_OP_WRITE_CHR) {
                uint16_t len = OS_MBUF_PKTLEN(ctxt->om);
                if (len == 0 || len > 15) return BLE_ATT_ERR_INVALID_ATTR_VALUE_LEN;
                cfgwr_item_t it = { .chr = CFGWR_CHR_BTNAME, .len = (uint8_t)len };
                os_mbuf_copydata(ctxt->om, 0, len, it.data);
                for (uint16_t i = 0; i < len; i++) {
                    if ((uint8_t)it.data[i] < 0x20) /* 禁控制字符 */
                        return BLE_ATT_ERR_INVALID_ATTR_VALUE_LEN;
                }
                if (!s_cfgwr_q || xQueueSend(s_cfgwr_q, &it, 0) != pdTRUE)
                    return BLE_ATT_ERR_UNLIKELY;
                return 0;
            }
            break;
        default:
            break;
    }
    return BLE_ATT_ERR_UNLIKELY;
}

/* 0xFFE5 reload：任意写入即触发角色热重载 */
static int reload_access_cb(uint16_t conn_handle, uint16_t attr_handle,
                            struct ble_gatt_access_ctxt *ctxt, void *arg) {
    (void)conn_handle; (void)attr_handle; (void)arg;
    ESP_LOGW(TAG, "att: %s op=%d", __func__, ctxt->op);
    if (ctxt->op == BLE_GATT_ACCESS_OP_WRITE_CHR) {
        ESP_LOGI(TAG, "BLE 触发热重载");
        mjpeg_player_request_reload();
        return 0;
    }
    return BLE_ATT_ERR_UNLIKELY;
}

/* 0xFFF1 角色目录列表：构建缓存（PSRAM），分段读取 */
static void invalidate_list(void);   /* 定义见下（upload DONE 等清缓存用） */
/* 构建角色列表缓存（开机时预构建 + access_cb 兜底）。
 * 注意：不能在 NimBLE host 任务里做长时间 SD 枚举（ATT 会超时），
 * 正常路径缓存已在开机阶段就绪，此处仅兜底。 */
static void rebuild_list_cache(void) {
    if (s_list_buf) invalidate_list();
    s_list_buf = heap_caps_malloc(LIST_BUF_MAX, MALLOC_CAP_SPIRAM | MALLOC_CAP_8BIT);
    if (s_list_buf == NULL) return;
    s_list_len = 0;
    s_list_off = 0;
    if (!bsp_sdcard_is_mounted()) return;   /* 未挂载 = 空列表 */
    /* 注意：roles_list_all 内部已持 bsp_sdcard_lock（非递归互斥，
     * 此处不得再加锁，否则自死锁——历史 list Unreachable 根因） */
    role_entry_t entries[32];
    int cnt = roles_list_all(entries, 32);
    for (int i = 0; i < cnt; i++) {
        size_t n = strlen(entries[i].name);
        if (s_list_len + n + 1 < LIST_BUF_MAX) {
            memcpy(s_list_buf + s_list_len, entries[i].name, n);
            s_list_len += n;
            s_list_buf[s_list_len++] = '\n';
        }
    }
}

static int list_access_cb(uint16_t conn_handle, uint16_t attr_handle,
                          struct ble_gatt_access_ctxt *ctxt, void *arg) {
    (void)conn_handle; (void)attr_handle; (void)arg;
    ESP_LOGW(TAG, "att: %s op=%d", __func__, ctxt->op);
    if (ctxt->op != BLE_GATT_ACCESS_OP_READ_CHR) return BLE_ATT_ERR_UNLIKELY;

    if (s_list_buf == NULL) {
        rebuild_list_cache();   /* 兜底（正常路径开机已预构建） */
        if (s_list_buf == NULL) return BLE_ATT_ERR_INSUFFICIENT_RES;
    }

    size_t left = s_list_len - s_list_off;
    size_t n = left > RD_CHUNK_MAX ? RD_CHUNK_MAX : left;
    int rc = os_mbuf_append(ctxt->om, s_list_buf + s_list_off, (uint16_t)n);
    if (rc != 0) return BLE_ATT_ERR_INSUFFICIENT_RES;
    s_list_off += n;
    return 0;
}

/* 0xFFF2 文件读取：写 "PATH:<path>" 打开（禁 ..），读 = 下一段 ≤500B */
/* :self: 哨兵 = cardstore 分区 self.bin 的裸头像（PC 连接后热载入本机头像） */
static uint8_t *s_self_rd = NULL;
static uint16_t s_self_off = 0;
static bool s_self_active = false;

static int readfile_access_cb(uint16_t conn_handle, uint16_t attr_handle,
                              struct ble_gatt_access_ctxt *ctxt, void *arg) {
    (void)conn_handle; (void)attr_handle; (void)arg;
    ESP_LOGW(TAG, "att: %s op=%d", __func__, ctxt->op);
    if (ctxt->op == BLE_GATT_ACCESS_OP_WRITE_CHR) {
        uint16_t len = OS_MBUF_PKTLEN(ctxt->om);
        if (len < 6 || len > RD_PATH_MAX + 5) return BLE_ATT_ERR_INVALID_ATTR_VALUE_LEN;
        uint8_t buf[RD_PATH_MAX + 6];
        os_mbuf_copydata(ctxt->om, 0, len, buf);
        buf[len] = '\0';
        if (memcmp(buf, "PATH:", 5) == 0) {
            const char *path = (const char *)buf + 5;
            if (strcmp(path, ":self:") == 0) {
                if (s_self_rd == NULL) {
                    s_self_rd = heap_caps_malloc(SELF_BLOB_MAX,
                                                 MALLOC_CAP_SPIRAM | MALLOC_CAP_8BIT);
                }
                if (s_self_rd == NULL) {
                    s_rd_failed = true; s_self_active = false;
                    return 0;
                }
                size_t len = 0;
                esp_err_t se = card_store_load_self(s_self_rd, SELF_BLOB_MAX, &len);
                if (se == ESP_OK && len >= sizeof(self_blob_hdr_t)) {
                    self_blob_hdr_t *sh = (self_blob_hdr_t *)s_self_rd;
                    if (sh->avatar_len == CARD_AVATAR_SIZE) {
                        s_self_off = 0;
                        s_self_active = true;
                        s_rd_failed = false;
                    } else s_rd_failed = true;
                } else s_rd_failed = true;
                if (s_rd_f) { fclose(s_rd_f); s_rd_f = NULL; }
                ESP_LOGW(TAG, "readfile open :self: -> %s",
                         s_rd_failed ? "fail" : "ok");
                return 0;
            }
            if (strncmp(path, "/sdcard/", 8) != 0 || strstr(path, "..") != NULL) {
                return BLE_ATT_ERR_INVALID_ATTR_VALUE_LEN;
            }
            bsp_sdcard_lock();
            if (s_rd_f) { fclose(s_rd_f); s_rd_f = NULL; }
            s_self_active = false;   /* 打开磁盘文件时终止 :self: 残留段 */
            s_rd_f = fopen(path, "r");
            s_rd_failed = (s_rd_f == NULL);
            bsp_sdcard_unlock();
            ESP_LOGI(TAG, "readfile open %s -> %s", path, s_rd_failed ? "fail" : "ok");
            return 0;
        }
        if (memcmp(buf, "STAT:", 5) == 0) {
            /* 文件大小查询：下一个 READ 返回 4B LE；无文件 = 0xFFFFFFFF */
            const char *path = (const char *)buf + 5;
            if (strcmp(path, ":self:") == 0) {
                s_stat_size = CARD_AVATAR_SIZE;
                s_stat_pending = true;
                return 0;
            }
            if (strncmp(path, "/sdcard/", 8) != 0 || strstr(path, "..") != NULL) {
                return BLE_ATT_ERR_INVALID_ATTR_VALUE_LEN;
            }
            struct stat st;
            int sr = stat(path, &st);
            if (sr != 0) {
                s_stat_size = (uint32_t)-1;
            } else {
                s_stat_size = (uint32_t)st.st_size;
            }
            s_stat_pending = true;
            return 0;
        }
        return BLE_ATT_ERR_INVALID_ATTR_VALUE_LEN;
    }
    if (ctxt->op == BLE_GATT_ACCESS_OP_READ_CHR) {
        if (s_stat_pending) {
            s_stat_pending = false;
            uint32_t sz = s_stat_size;
            return os_mbuf_append(ctxt->om, &sz, 4) == 0 ? 0 : BLE_ATT_ERR_INSUFFICIENT_RES;
        }
        if (s_rd_f == NULL) {
            if (s_self_active) {
                static uint8_t self_chunk[RD_CHUNK_MAX];
                size_t avail = (size_t)CARD_AVATAR_SIZE - s_self_off;
                size_t n = avail < RD_CHUNK_MAX ? avail : RD_CHUNK_MAX;
                memcpy(self_chunk, s_self_rd + sizeof(self_blob_hdr_t) + s_self_off, n);
                s_self_off += (uint16_t)n;
                if (s_self_off >= CARD_AVATAR_SIZE) s_self_active = false;
                return os_mbuf_append(ctxt->om, self_chunk, (uint16_t)n) == 0 ? 0 : BLE_ATT_ERR_INSUFFICIENT_RES;
            }
            if (s_rd_failed) {
                s_rd_failed = false;
                return os_mbuf_append(ctxt->om, "ERR:open", 8) == 0 ? 0 : BLE_ATT_ERR_INSUFFICIENT_RES;
            }
            return 0;   /* 空段 = EOF 标志 */
        }
        static uint8_t chunk[RD_CHUNK_MAX];
        size_t n;
        bsp_sdcard_lock();
        n = fread(chunk, 1, RD_CHUNK_MAX, s_rd_f);
        if (n < RD_CHUNK_MAX) {
            fclose(s_rd_f);
            s_rd_f = NULL;
        }
        bsp_sdcard_unlock();
        return os_mbuf_append(ctxt->om, chunk, (uint16_t)n) == 0 ? 0 : BLE_ATT_ERR_INSUFFICIENT_RES;
    }
    return BLE_ATT_ERR_UNLIKELY;
}

/* ============================================================================
 * 素材上传（0xFFF3 写协议）
 * ==========================================================================*/

/* 白名单校验：payload = "<rolename>/<file>:<size>"；成功输出路径与总大小，
 * 返回 NULL；失败返回错误码字符串常量 */
static const char *upl_validate(const char *payload, size_t plen,
                                char *out_path, size_t out_cap, size_t *out_total) {
    /* 末尾 ":<size>"（rolename/file 均禁 ':'，首个冒号即分隔符） */
    const char *colon = memchr(payload, ':', plen);
    if (colon == NULL) return "ERR:path";
    size_t path_len = (size_t)(colon - payload);
    size_t size_len = plen - path_len - 1;
    if (path_len == 0 || path_len >= UPL_PATH_MAX || size_len == 0 || size_len > 10) {
        return "ERR:path";
    }
    char size_buf[12];
    memcpy(size_buf, colon + 1, size_len);
    size_buf[size_len] = '\0';
    long total = strtol(size_buf, NULL, 10);
    if (total <= 0 || total > UPL_TOTAL_MAX) return "ERR:size";

    /* 路径拆 rolename/file */
    char *slash = (char *)memchr(payload, '/', path_len);
    if (slash == NULL || slash == payload + path_len - 1) return "ERR:path";
    size_t role_len = (size_t)(slash - payload);
    const char *file = slash + 1;
    size_t file_len = path_len - role_len - 1;
    if (!name_ok(payload, role_len, ROLE_NAME_MAX)) return "ERR:path";
    bool ok_file = (file_len == strlen(UPL_F_LOOP) && memcmp(file, UPL_F_LOOP, file_len) == 0) ||
                   (file_len == strlen(UPL_F_INTRO) && memcmp(file, UPL_F_INTRO, file_len) == 0) ||
                   (file_len == strlen(UPL_F_PORTRAIT) && memcmp(file, UPL_F_PORTRAIT, file_len) == 0) ||
                   (file_len == strlen(UPL_F_UITEXT) && memcmp(file, UPL_F_UITEXT, file_len) == 0) ||
                   (file_len == strlen(UPL_F_SELF) && memcmp(file, UPL_F_SELF, file_len) == 0);
    if (!ok_file) return "ERR:path";

    /* self/self.bin：本机名片头像（裸 64x64 RGB565 LE）。不走 SD：
     * 由 cfg_writer 打包 self_blob_hdr_t 写入 cardstore 分区 */
    if (file_len == strlen(UPL_F_SELF) && memcmp(file, UPL_F_SELF, file_len) == 0 &&
        role_len == 4 && memcmp(payload, "self", 4) == 0) {
        if ((size_t)total != CARD_AVATAR_SIZE) return "ERR:size";
        snprintf(out_path, out_cap, "%s", UPL_SELF_PREFIX);
        *out_total = (size_t)total;
        return NULL;
    }

    snprintf(out_path, out_cap, "/sdcard/%.*s", (int)path_len, payload);
    *out_total = (size_t)total;
    return NULL;
}

/* 中断上传（timeout/disconnect/错误）；keep_partial=0 时删除半截文件 */
static void upload_abort(bool keep_partial, const char *err) {
    if (s_upl_st != UPL_IDLE) {
        s_upl_st = UPL_IDLE;
        bsp_sdcard_lock();
        if (s_upl_f) { fclose(s_upl_f); s_upl_f = NULL; }
        if (!keep_partial) unlink(s_upl_path);
        bsp_sdcard_unlock();
    }
    if (s_upl_buf) { heap_caps_free(s_upl_buf); s_upl_buf = NULL; }
    s_upload_progress = -1;
    if (err) notify_status(err);
}

/* 目录列表缓存失效（上传完成后目录可能变化） */
static void invalidate_list(void) {
    if (s_list_buf) {
        heap_caps_free(s_list_buf);
        s_list_buf = NULL;
    }
    s_list_len = 0;
    s_list_off = 0;
}

static int handle_upload_write(uint16_t conn_handle, uint16_t attr_handle,
                               struct ble_gatt_access_ctxt *ctxt, void *arg) {
    (void)conn_handle; (void)attr_handle; (void)arg;
    uint16_t len = OS_MBUF_PKTLEN(ctxt->om);
    if (len == 0) return 0;

    if (s_upl_st == UPL_IDLE) {
        /* 仅接受 START: */
        if (len < 6 || memcmp(OS_MBUF_DATA(ctxt->om, uint8_t *), "START:", 6) != 0) {
            return 0;   /* 忽略杂散写 */
        }
        char payload[UPL_PATH_MAX + 24];
        size_t plen = len - 6;
        if (plen > sizeof(payload) - 1) plen = sizeof(payload) - 1;
        os_mbuf_copydata(ctxt->om, 6, plen, (uint8_t *)payload);
        payload[plen] = '\0';

        char path[UPL_PATH_MAX];
        size_t total = 0;
        const char *err = upl_validate(payload, plen, path, sizeof(path), &total);
        if (err) {
            notify_status(err);
            return 0;
        }

        /* 确保目录存在（BUF/FILE 两模式都需要——漏掉曾致新角色 fopen 必败）。
         * EEXIST=已存在属正常。self.bin 不落 SD，跳过 mkdir */
        if (strncmp(path, UPL_SELF_PREFIX, strlen(UPL_SELF_PREFIX)) != 0) {
            char dir[UPL_PATH_MAX];
            snprintf(dir, sizeof(dir), "/sdcard/%.*s", (int)(strchr(payload, '/') - payload), payload);
            bsp_sdcard_lock();
            mkdir(dir, 0);
            bsp_sdcard_unlock();
        }

        s_upl_st = UPL_DATA;
        s_upl_total = total;
        s_upl_got = 0;
        s_upl_last_pct = -1;
        s_upl_last_ms = now_ms();
        s_upload_progress = 0;
        strncpy(s_upl_path, path, sizeof(s_upl_path) - 1);
        s_upl_path[sizeof(s_upl_path) - 1] = '\0';
        ESP_LOGW(TAG, "upload start: %s (%u B)", s_upl_path, (unsigned)total);

        /* BUF 模式优先：整块 PSRAM 缓冲，DATA 阶段零 SD 访问
         * （host 内逐块 fwrite 抢锁曾致 ATT 停摆→断链→PC 清空角色列表）。
         * 失败回落 FILE 模式：fopen 重试 3×300ms（风暴期目录扇区读失败缓解） */
        s_upl_buf = heap_caps_malloc(total, MALLOC_CAP_SPIRAM | MALLOC_CAP_8BIT);
        if (s_upl_buf == NULL) {
            bsp_sdcard_lock();
            for (int i = 0; i < 3 && s_upl_f == NULL; i++) {
                s_upl_f = fopen(path, "wb");
                if (s_upl_f == NULL) vTaskDelay(pdMS_TO_TICKS(300));
            }
            bsp_sdcard_unlock();
            if (s_upl_f == NULL) {
                upload_abort(false, "ERR:open");
                return 0;
            }
            ESP_LOGI(TAG, "upload start(FILE): %s (%u B)", path, (unsigned)s_upl_total);
        } else {
            ESP_LOGI(TAG, "upload start(BUF): %s (%u B)", path, (unsigned)s_upl_total);
        }
        notify_status("OK");
        return 0;
    }

    /* UPL_DATA 态：END 或数据块 */
    if (len == 3 && memcmp(OS_MBUF_DATA(ctxt->om, uint8_t *), "END", 3) == 0) {
        if (s_upl_buf == NULL && s_upl_f) {
            bsp_sdcard_lock();
            fclose(s_upl_f); s_upl_f = NULL;
            bsp_sdcard_unlock();
        }
        if (s_upl_got != s_upl_total) {
            ESP_LOGW(TAG, "upload size mismatch: got %u want %u", (unsigned)s_upl_got, (unsigned)s_upl_total);
            upload_abort(false, "ERR:size");
            return 0;
        }
        s_upl_st = UPL_IDLE;
        s_upload_progress = -1;

        if (s_upl_buf != NULL) {
            /* BUF 模式：落盘交给 cfg_writer_task（fopen 重试 + 持锁写），
             * flush 成功后才 notify DONE——host 全程零 SD 访问 */
            cfgwr_item_t fit = { .chr = CFGWR_CHR_UPL_FLUSH };
            xQueueSend(s_cfgwr_q, &fit, 0);
            ESP_LOGI(TAG, "upload flush queued: %s (%u B)", s_upl_path, (unsigned)s_upl_total);
        } else {
            invalidate_list();
            /* 列表重建移到 cfg_writer_task（host 内持锁枚举会阻塞 ATT，
             * 曾致 PC 端 END 响应写等待超时——卡 100% 根因之一） */
            {
                cfgwr_item_t rit = { .chr = CFGWR_CHR_REBUILD };
                xQueueSend(s_cfgwr_q, &rit, 0);
            }
            ESP_LOGI(TAG, "upload done: %s (%u/%u B)", s_upl_path,
                     (unsigned)s_upl_got, (unsigned)s_upl_total);
            notify_status("DONE");
        }
        ui_menu_notify_ble_state();
        return 0;
    }

    /* 数据块 */
    static uint8_t chunk[RD_CHUNK_MAX];
    size_t n = len > sizeof(chunk) ? sizeof(chunk) : len;
    if (s_upl_buf != NULL) {
        /* BUF 模式：仅内存拷贝，防溢出截断 */
        if (s_upl_got + n > s_upl_total) n = s_upl_total - s_upl_got;
        if (n == 0) return 0;
        os_mbuf_copydata(ctxt->om, 0, n, s_upl_buf + s_upl_got);
        s_upl_got += n;
        s_upl_last_ms = now_ms();
    } else {
        if (s_upl_f == NULL) {
            upload_abort(false, "ERR:sd");
            return 0;
        }
        os_mbuf_copydata(ctxt->om, 0, n, chunk);
        bsp_sdcard_lock();
        size_t w = fwrite(chunk, 1, n, s_upl_f);
        bsp_sdcard_unlock();
        if (w != n) {
            upload_abort(false, "ERR:sd");
            return 0;
        }
        s_upl_got += n;
        s_upl_last_ms = now_ms();
    }
    int pct = (int)(s_upl_got * 100 / s_upl_total);
    if (pct > 100) pct = 100;
    if (pct != s_upl_last_pct) {
        s_upl_last_pct = pct;
        s_upload_progress = pct;
        char buf[32];
        snprintf(buf, sizeof(buf), "PROG:%d", pct);
        notify_status(buf);
    }
    return 0;
}

/* 1s 周期：上传 30s 无数据超时（独立任务——SD 锁可能被长操作持有，
 * 不能在定时器回调里阻塞） */
static void timeout_task(void *arg) {
    (void)arg;
    for (;;) {
        vTaskDelay(pdMS_TO_TICKS(1000));
        if (s_upl_st == UPL_DATA && (now_ms() - s_upl_last_ms) > UPL_TIMEOUT_MS) {
            ESP_LOGW(TAG, "upload timeout, abort");
            upload_abort(false, "ERR:timeout");
        }
    }
}

/* ============================================================================
 * GATT 服务定义
 * ==========================================================================*/

/* 0x2A28 软件版本（只读） */
static int swrev_access_cb(uint16_t conn_handle, uint16_t attr_handle,
                           struct ble_gatt_access_ctxt *ctxt, void *arg) {
    (void)conn_handle; (void)attr_handle; (void)arg;
    ESP_LOGW(TAG, "att: %s op=%d", __func__, ctxt->op);
    if (ctxt->op != BLE_GATT_ACCESS_OP_READ_CHR) return BLE_ATT_ERR_UNLIKELY;
    return os_mbuf_append(ctxt->om, FW_VERSION, (uint16_t)strlen(FW_VERSION)) == 0 ? 0 : BLE_ATT_ERR_INSUFFICIENT_RES;
}

/* 上传状态 0xFFF4：仅通知用，无读/写入口（NimBLE 要求 access_cb 非空） */
static int uplstatus_access_cb(uint16_t conn_handle, uint16_t attr_handle,
                               struct ble_gatt_access_ctxt *ctxt, void *arg) {
    (void)conn_handle; (void)attr_handle; (void)ctxt; (void)arg;
    return BLE_ATT_ERR_UNLIKELY;
}

/* 设备识别码 0xFFF5：只读，返回 4B LE "PSUP"（BLE_FEATURE_MAGIC）。
 * PC/手机端凭此确认设备族，识别码固定预埋，不随设备名变化。 */
static int ident_access_cb(uint16_t conn_handle, uint16_t attr_handle,
                           struct ble_gatt_access_ctxt *ctxt, void *arg) {
    (void)conn_handle; (void)attr_handle; (void)arg;
    ESP_LOGW(TAG, "att: %s op=%d", __func__, ctxt->op);
    if (ctxt->op != BLE_GATT_ACCESS_OP_READ_CHR) return BLE_ATT_ERR_UNLIKELY;
    uint32_t magic = BLE_FEATURE_MAGIC;
    return os_mbuf_append(ctxt->om, &magic, sizeof(magic)) == 0 ? 0 : BLE_ATT_ERR_INSUFFICIENT_RES;
}

static const struct ble_gatt_svc_def gatt_svcs[] = {
    /* 设备信息 0x180A：软件版本 */
    {
        .type = BLE_GATT_SVC_TYPE_PRIMARY,
        .uuid = (const ble_uuid_t *)&uuid_svc_devfound,
        .characteristics = (struct ble_gatt_chr_def[]) {
            {
                .uuid = (const ble_uuid_t *)&uuid_chr_swrev,
                .access_cb = swrev_access_cb,
                .flags = BLE_GATT_CHR_F_READ,
            },
            { 0 },
        },
    },
    /* 设置服务 0xFFE0 */
    {
        .type = BLE_GATT_SVC_TYPE_PRIMARY,
        .uuid = (const ble_uuid_t *)&uuid_svc_settings,
        .characteristics = (struct ble_gatt_chr_def[]) {
            {
                .uuid = (const ble_uuid_t *)&uuid_chr_current,
                .access_cb = settings_access_cb,
                .flags = BLE_GATT_CHR_F_READ | BLE_GATT_CHR_F_WRITE,
            },
            {
                .uuid = (const ble_uuid_t *)&uuid_chr_uitext,
                .access_cb = settings_access_cb,
                .flags = BLE_GATT_CHR_F_WRITE,
            },
            {
                .uuid = (const ble_uuid_t *)&uuid_chr_ipcfg,
                .access_cb = settings_access_cb,
                .flags = BLE_GATT_CHR_F_READ | BLE_GATT_CHR_F_WRITE,
            },
            {
                .uuid = (const ble_uuid_t *)&uuid_chr_loglevel,
                .access_cb = settings_access_cb,
                .flags = BLE_GATT_CHR_F_READ | BLE_GATT_CHR_F_WRITE,
            },
            {
                .uuid = (const ble_uuid_t *)&uuid_chr_reload,
                .access_cb = reload_access_cb,
                .flags = BLE_GATT_CHR_F_WRITE,
            },
            {
                .uuid = (const ble_uuid_t *)&uuid_chr_uid,
                .access_cb = settings_access_cb,
                .flags = BLE_GATT_CHR_F_READ | BLE_GATT_CHR_F_WRITE,
            },
            {
                .uuid = (const ble_uuid_t *)&uuid_chr_motto,
                .access_cb = settings_access_cb,
                .flags = BLE_GATT_CHR_F_READ | BLE_GATT_CHR_F_WRITE,
            },
            {
                .uuid = (const ble_uuid_t *)&uuid_chr_uname,
                .access_cb = settings_access_cb,
                .flags = BLE_GATT_CHR_F_READ | BLE_GATT_CHR_F_WRITE,
            },
            {
                .uuid = (const ble_uuid_t *)&uuid_chr_time,
                .access_cb = settings_access_cb,
                .flags = BLE_GATT_CHR_F_WRITE,
            },
            {
                .uuid = (const ble_uuid_t *)&uuid_chr_btname,
                .access_cb = settings_access_cb,
                .flags = BLE_GATT_CHR_F_READ | BLE_GATT_CHR_F_WRITE,
            },
            {
                .uuid = (const ble_uuid_t *)&uuid_chr_cardclr,
                .access_cb = settings_access_cb,
                .flags = BLE_GATT_CHR_F_WRITE,
            },
            { 0 },
        },
    },
    /* SD 服务 0xFFF0 */
    {
        .type = BLE_GATT_SVC_TYPE_PRIMARY,
        .uuid = (const ble_uuid_t *)&uuid_svc_sd,
        .characteristics = (struct ble_gatt_chr_def[]) {
            {
                .uuid = (const ble_uuid_t *)&uuid_chr_list,
                .access_cb = list_access_cb,
                .flags = BLE_GATT_CHR_F_READ,
            },
            {
                .uuid = (const ble_uuid_t *)&uuid_chr_readfile,
                .access_cb = readfile_access_cb,
                .flags = BLE_GATT_CHR_F_READ | BLE_GATT_CHR_F_WRITE,
            },
            {
                .uuid = (const ble_uuid_t *)&uuid_chr_upload,
                .access_cb = handle_upload_write,
                .flags = BLE_GATT_CHR_F_WRITE | BLE_GATT_CHR_F_WRITE_NO_RSP,
            },
            {
                .uuid = (const ble_uuid_t *)&uuid_chr_uplstatus,
                .access_cb = uplstatus_access_cb,
                .flags = BLE_GATT_CHR_F_NOTIFY,
            },
            {
                .uuid = (const ble_uuid_t *)&uuid_chr_ident,
                .access_cb = ident_access_cb,
                .flags = BLE_GATT_CHR_F_READ,
            },
            {
                .uuid = (const ble_uuid_t *)&uuid_chr_del,
                .access_cb = handle_del_write,
                .flags = BLE_GATT_CHR_F_WRITE,
            },
            { 0 },
        },
    },
    { 0 },
};

/* 状态通知（0xFFF4） */
/* 本函数会被 cfg_writer_task / esp_event 任务跨线程调用，
 * NimBLE host API 必须持 hs 锁（曾致 mbuf 池竞争→堆元数据损坏→随机
 * StoreProhibited 崩溃）。ble_hs_lock/unlock 定义于 NimBLE 内部头
 * ble_hs_priv.h（esp-idf 未导出），此处手动声明，符号可链 */
extern void ble_hs_lock(void);
extern void ble_hs_unlock(void);

static void notify_status(const char *msg) {
    if (!s_connected) return;
    if (s_status_val_handle == 0) {
        if (ble_gatts_find_chr((const ble_uuid_t *)&uuid_svc_sd,
                               (const ble_uuid_t *)&uuid_chr_uplstatus,
                               NULL, &s_status_val_handle) != 0) return;
    }
    struct os_mbuf *om = ble_hs_mbuf_from_flat(msg, strlen(msg));
    if (om == NULL) return;
/* 本函数会被 cfg_writer_task / esp_event 任务跨线程调用，
     * NimBLE host API 必须持 hs 锁（曾致 mbuf 池竞争→堆元数据损坏→随机
     * StoreProhibited 崩溃） */
    ble_hs_lock();
    ble_gatts_notify_custom(s_conn, s_status_val_handle, om);
    ble_hs_unlock();
}

/* ============================================================================
 * GAP 回调：连接/断开
 * ==========================================================================*/

/* 连接参数更新（公共实现）：请求 min=max=itvl_slots（1.25ms/槽），
 * latency 0，supervision 4s。NimBLE API 自带 host 锁，可从任意任务
 * 调用（esp_timer 任务均实测安全）。
 * 设备已断开则静默跳过。 */
static esp_timer_handle_t s_conn_param_timer = NULL;  /* 收紧定时器（1s 后触发） */

static uint16_t s_desired_itvl = 12;  /* 最近一次请求的间隔（槽数） */
static bool s_upd_retried = false;    /* 当前期望值是否已失败重试一次 */

static void request_conn_itvl(uint16_t itvl_slots) {
    if (!s_connected) return;
    if (itvl_slots != s_desired_itvl) {
        s_desired_itvl = itvl_slots;
        s_upd_retried = false;   /* 换了新目标，重试额度重置 */
    }
    struct ble_gap_upd_params p = {
        .itvl_min = itvl_slots,
        .itvl_max = itvl_slots,
        .latency = 0,
        .supervision_timeout = 400,
    };
    int rc = ble_gap_update_params(s_conn, &p);
    ESP_LOGI(TAG, "conn param update -> %d (itvl %u slots)",
             rc, (unsigned)itvl_slots);
}

/* 连接稳定 1s 后请求收紧连接间隔到 15ms（12 × 1.25ms） */
static void conn_param_cb(void *arg) {
    (void)arg;
    request_conn_itvl(12);
}

static int gap_cb(struct ble_gap_event *e, void *arg) {
    (void)arg;
    switch (e->type) {
        case BLE_GAP_EVENT_CONNECT:
            if (e->connect.status == 0) {
                s_conn = e->connect.conn_handle;
                s_connected = true;
                /* 新会话 list 游标归位：缓存跨会话保留（见 DISCONNECT 注释），
                 * 但游标若停在上次读完处，下次连接首读即空段 → 主机误判目录为空 */
                s_list_off = 0;
                 /* WARN 级：启动后全局日志降 WARN，INFO 级连接日志不可见，
                  * 诊断"连接即掉线"必须能看到连接/断开/加密事件 */
                  ESP_LOGW(TAG, "connected (handle %u)", s_conn);
                /* 连接稳定 1s 后请求收紧连接间隔到 15ms（吞吐天花板由
                 * central 默认 30-50ms 决定 ~17KB/s；收紧后 ~40KB/s）。
                 * Windows 拒绝则维持默认，无害。"连接即掉线"真凶是 host
                 * 内同步抢 SD 锁（已根治），参数更新本身安全 */
                if (s_conn_param_timer == NULL) {
                    esp_timer_create_args_t args = {
                        .callback = conn_param_cb,
                        .name = "connparam",
                    };
                    if (esp_timer_create(&args, &s_conn_param_timer) != ESP_OK)
                        s_conn_param_timer = NULL;
                }
                if (s_conn_param_timer != NULL)
                    esp_timer_start_once(s_conn_param_timer, 1000000);
                ui_menu_notify_ble_state();
            } else {
                ESP_LOGW(TAG, "connect FAILED status=%d", e->connect.status);
                advertise();
            }
            break;
        case BLE_GAP_EVENT_CONN_UPDATE:
            /* 中央机拒绝参数更新（BLE4.2 无强制力）→ 对同一期望值单次
             * 重试，仍失败则维持现状（连接间隔调整非正确性依赖） */
            if (e->conn_update.status != 0 && !s_upd_retried) {
                s_upd_retried = true;
                ESP_LOGW(TAG, "conn update rejected status=%d, retry once",
                         e->conn_update.status);
                request_conn_itvl(s_desired_itvl);
            }
            break;
        case BLE_GAP_EVENT_DISCONNECT:
            ESP_LOGW(TAG, "disconnected (handle %u, reason %d)",
                     (unsigned)e->disconnect.conn.conn_handle, e->disconnect.reason);
            s_conn = 0;
            s_connected = false;
            if (s_upl_st != UPL_IDLE) upload_abort(false, "ERR:dis");
            /* 注意：不 invalidate_list —— 会话间保留缓存，重连后立即可读；
             * 目录变化（上传完成/拔卡）由 upload DONE 与 SD 事件清缓存 */
            ui_menu_notify_ble_state();
            advertise();
            break;
        case BLE_GAP_EVENT_ENC_CHANGE:
            ESP_LOGW(TAG, "enc change: status=%d", e->enc_change.status);
            break;
        case BLE_GAP_EVENT_PARING_COMPLETE:
            ESP_LOGW(TAG, "pairing complete: status=%d", e->pairing_complete.status);
            break;
        case BLE_GAP_EVENT_ADV_COMPLETE:
            if (s_adv_enabled) advertise();
            break;
        default:
            break;
    }
    return 0;
}

/* ============================================================================
 * 广播
 * ==========================================================================*/

/* 广播失败重试：esp-idf NimBLE 在 ADV_COMPLETE 回调里立即 adv_start
 * 可能被拒（内部仍在收尾）→ 广播永久停（设备周期性静默，电脑/手机
 * 发现不了）。失败后 1s 重试，直到成功或广播被关闭。 */
static esp_timer_handle_t s_adv_retry_timer = NULL;

static void adv_retry_cb(void *arg) {
    (void)arg;
    s_adv_retry_timer = NULL;
    if (s_adv_enabled && !s_connected) advertise();
}

static void schedule_adv_retry(void) {
    if (s_adv_retry_timer != NULL) return;
    esp_timer_create_args_t args = {
        .callback = adv_retry_cb,
        .name = "ble_adv_retry",
    };
    if (esp_timer_create(&args, &s_adv_retry_timer) == ESP_OK) {
        esp_timer_start_once(s_adv_retry_timer, 1000000);   /* 1s 后重试 */
    }
}

/* 广播保活：NimBLE 偶发广播漏重开（ADV_COMPLETE 竞态/事件丢失/连接残留），
 * 导致设备周期性静默——Windows"全部设备"里消失、安卓/nRF 完全搜不到。
 * 每 10s 强制重开一轮：未广播则补发；已在广播则 stop+start 无隙衔接
 * （几十 ms 间隙，扫描器感知不到）。连接期间跳过。 */
static esp_timer_handle_t s_adv_keepalive_timer = NULL;

static void adv_keepalive_cb(void *arg) {
    (void)arg;
    if (!s_adv_enabled || s_connected) return;
    ble_gap_adv_stop();       /* 幂等：未广播时返回 ENOENT，忽略 */
    advertise();
}

static void adv_keepalive_start(void) {
    if (s_adv_keepalive_timer != NULL) return;
    esp_timer_create_args_t args = {
        .callback = adv_keepalive_cb,
        .name = "ble_adv_keepalive",
    };
    if (esp_timer_create(&args, &s_adv_keepalive_timer) == ESP_OK) {
        esp_timer_start_periodic(s_adv_keepalive_timer, 10000000);  /* 10s */
    }
}

static void advertise(void) {
    if (!s_adv_enabled || s_connected) return;
    ESP_LOGI(TAG, "adv: set fields");
    struct ble_hs_adv_fields fields = { 0 };
    fields.flags = BLE_HS_ADV_F_DISC_GEN | BLE_HS_ADV_F_BREDR_UNSUP;
    fields.name = (uint8_t *)s_dev_name;
    fields.name_len = (uint8_t)strlen(s_dev_name);
    fields.name_is_complete = 1;
    /* 设备族识别码：16-bit Service Data（0xFFE0, "AP"）。PC/手机端
     * 扫描时凭此过滤本族设备，不依赖可修改的设备名。AD 布局：
     * [uuid16 LE][payload]，NimBLE 的 svc_data_uuid16 指向完整数据。 */
    static const uint8_t svc_data_ap[] = { 0xE0, 0xFF, 'A', 'P' };
    fields.svc_data_uuid16 = svc_data_ap;
    fields.svc_data_uuid16_len = sizeof(svc_data_ap);
    /* 知名 UUID 0x180A（Device Information，GATT 里本就有此服务）：
     * 安卓 14+ 设置页对只广播自定义 Service Data 的设备默认隐藏，
     * 必须广播至少一个知名 UUID 才会显示。 */
    static const ble_uuid16_t uuid_180a = BLE_UUID16_INIT(0x180A);
    fields.uuids16 = &uuid_180a;
    fields.num_uuids16 = 1;
    fields.uuids16_is_complete = 1;
    int rc = ble_gap_adv_set_fields(&fields);
    if (rc != 0) {
        ESP_LOGW(TAG, "adv_set_fields err %d, retry in 1s", rc);
        schedule_adv_retry();
        return;
    }
    ESP_LOGI(TAG, "adv: fields ok");
    struct ble_gap_adv_params advp = { 0 };
    advp.conn_mode = BLE_GAP_CONN_MODE_UND;
    advp.disc_mode = BLE_GAP_DISC_MODE_GEN;
    advp.itvl_min = 160;      /* ~200ms */
    advp.itvl_max = 200;
    /* duration=0 在该 esp-idf NimBLE 版本会立即触发 ADV_COMPLETE，
     * 造成 advertise() 无限重开循环（日志风暴 + RF 忙死）。
     * 用 15 分钟一轮 + ADV_COMPLETE 事件自动重开；重开失败由
     * schedule_adv_retry() 兜底，杜绝设备周期性静默。 */
    rc = ble_gap_adv_start(BLE_OWN_ADDR_PUBLIC, NULL, 900000, &advp, gap_cb, NULL);
    if (rc != 0 && rc != BLE_HS_EALREADY) {
        ESP_LOGW(TAG, "adv_start err %d, retry in 1s", rc);
        schedule_adv_retry();
        return;
    }
    ESP_LOGI(TAG, "adv: start issued rc=%d", rc);
}

/* ============================================================================
 * NimBLE 同步/复位回调
 * ==========================================================================*/

static void on_sync(void) {
    ESP_LOGI(TAG, "nimble sync, device: %s", s_dev_name);
    advertise();
    adv_keepalive_start();    /* 广播保活：防漏重开导致设备静默 */
    ESP_LOGI(TAG, "sync done");
}

static void on_reset(int reason) {
    ESP_LOGW(TAG, "nimble reset: %d", reason);
}

/* ============================================================================
 * 主机任务
 * ==========================================================================*/

static void hb_task(void *param) {
    (void)param;
    while (1) {
        vTaskDelay(pdMS_TO_TICKS(2000));
        ESP_LOGI(TAG, "hb: core0 alive, sram %u", 
                 (unsigned)heap_caps_get_free_size(MALLOC_CAP_INTERNAL));
    }
}

static void host_task(void *param) {
    (void)param;
    nimble_port_run();
    nimble_port_freertos_deinit();
}

/* ============================================================================
 * 对外 API
 * ==========================================================================*/

bool ble_gatt_is_connected(void) { return s_connected; }

bool ble_gatt_adv_enabled(void) { return s_adv_enabled; }

void ble_gatt_set_adv(bool on) {
    if (on == s_adv_enabled) return;
    s_adv_enabled = on;
    if (on) {
        advertise();
    } else {
        if (s_adv_retry_timer != NULL) {
            esp_timer_stop(s_adv_retry_timer);
            esp_timer_delete(s_adv_retry_timer);
            s_adv_retry_timer = NULL;
        }
        if (!s_connected) {
            int rc = ble_gap_adv_stop();
            if (rc != 0 && rc != BLE_HS_EALREADY && rc != BLE_HS_ENOTCONN) {
                ESP_LOGW(TAG, "adv_stop err %d", rc);
            }
        }
    }
    ESP_LOGI(TAG, "advertising %s", on ? "on" : "off");
}

const char *ble_gatt_dev_name(void) { return s_dev_name; }

int ble_gatt_upload_progress(void) { return s_upload_progress; }

static void usbident_tmr_cb(void *arg);

/* USB 身份上报：向 USB-Serial/JTAG 控制台周期打印一行稳定标识，
 * PC 端不必 BLE 扫描即可识别插着线的是哪台（设备名可自定义） */
static void usbident_print(void) {
    uint8_t mac[6];
    esp_read_mac(mac, ESP_MAC_BT);
    ESP_LOGI("usbident", "name=%s mac=%02x%02x%02x%02x%02x%02x",
             s_dev_name, mac[0], mac[1], mac[2], mac[3], mac[4], mac[5]);
}

static void usbident_tmr_cb(void *arg) {
    (void)arg;
    usbident_print();
}

/* BLE 初始化在独立任务中执行：nimble_port_init + GATT 注册 + 预构建角色列表
 * 缓存（rebuild_list_cache 内部做 SD 枚举/FATFS 遍历，栈需求大）——
 * 之前在 main 任务（默认栈 3584B）里执行导致栈溢出
 * （"A stack overflow in task main has been detected"）→ 无 panic 日志的
 * 静默复位 → 开机重启循环 → 电脑连不上。任务栈 8192 与 NimBLE host task 一致。 */
static void ble_init_task(void *arg) {
    (void)arg;
    /* 设备名：默认 PASS-II-XXXX（MAC 尾 2 字节大写），bt.txt 可覆盖 */
    uint8_t mac[6];
    esp_read_mac(mac, ESP_MAC_BT);
    snprintf(s_dev_name, sizeof(s_dev_name), "PASS-II-%02X%02X", mac[4], mac[5]);
    /* 读取 /sdcard/config/bt.txt：enable（默认关）+ name 覆盖 */
    ble_load_cfg();
    ESP_LOGI(TAG, "init, name=%s adv=%d", s_dev_name, s_adv_enabled ? 1 : 0);
    usbident_print();
    /* 预构建角色列表缓存：此阶段 SD 已就绪且 MJPEG 播放器任务尚未创建
     * （main.c 顺序 BLE → 播放器），SD 空闲，枚举快速完成；
     * 避免连接后首次读在 NimBLE host 任务里阻塞（ATT 超时 Unreachable） */
    rebuild_list_cache();
    unsigned n_entries = 0;
    for (size_t i = 0; i < s_list_len; i++) if (s_list_buf[i] == '\n') n_entries++;
    ESP_LOGI(TAG, "list cache: %u B, %u entries", (unsigned)s_list_len, n_entries);

    /* 配置异步写任务：GATT 回调只入队，SD 写/镜像维护在此任务完成
     * （host 任务内抢 SD 锁会阻塞 ATT → 掉线/操作报错，见 cfg_writer_task 注释） */
    s_cfgwr_q = xQueueCreate(4, sizeof(cfgwr_item_t));
    if (s_cfgwr_q) {
        xTaskCreatePinnedToCore(cfg_writer_task, "ble_cfgwr", 8192,
                                NULL, 2, NULL, 1);
        ESP_LOGI(TAG, "cfg writer ready");
    } else {
        ESP_LOGE(TAG, "cfgwr queue create failed");
    }

    /* USB 身份周期上报：boot 与改名时即时打印；30s 低频兜底
     * （PC 晚开 COM 口时最多等一拍即可识别插着线的是哪台） */
    static bool ident_started = false;
    if (!ident_started) {
        ident_started = true;
        esp_timer_handle_t tmr = NULL;
        esp_timer_create_args_t targs = {
            .callback = usbident_tmr_cb, .name = "usbident",
        };
        if (esp_timer_create(&targs, &tmr) == ESP_OK && tmr) {
            esp_timer_start_periodic(tmr, 30000000);
        }
    }

    /* 调试心跳：core0 2s 周期——定位"全系统静默"时是 core0 中断级卡死
     * 还是任务级卡死（心跳停=core0 整体僵死） */
    static bool hb_started = false;
    if (!hb_started) {
        hb_started = true;
        xTaskCreatePinnedToCore(hb_task, "ble_dbg_hb", 2048, NULL, 2, NULL, 0);
    }

    int rc = nimble_port_init();
    if (rc != ESP_OK) {
        ESP_LOGE(TAG, "nimble_port_init err %d", rc);
        return;
    }
    ESP_LOGI(TAG, "port init ok");
    ble_svc_gap_init();
    /* GAP 服务 0x2A00 设备名与广播名一致（默认 "nimble"，不设置的话
     * Windows/手机连接后显示的名字与扫描时的广播名对不上） */
    ble_svc_gap_device_name_set(s_dev_name);
    ble_svc_gatt_init();
    rc = ble_gatts_count_cfg(gatt_svcs);
    if (rc != 0) ESP_LOGE(TAG, "count_cfg err %d", rc);
    ESP_LOGI(TAG, "count ok");
    rc = ble_gatts_add_svcs(gatt_svcs);
    if (rc != 0) ESP_LOGE(TAG, "add_svcs err %d", rc);
    ESP_LOGI(TAG, "add ok");

    ble_hs_cfg.sync_cb = on_sync;
    ble_hs_cfg.reset_cb = on_reset;

    /* SMP Just Works 配对（无输入输出，免密）——官方 bleprph 兼容配置：
     * Windows 设置页 / 安卓原生配对流程依赖 SMP。之前完全禁用导致
     * "看到但连不上"；legacy 无密钥（key_dist=0）导致 Windows 配对被拒
     * （ENOTSUP, status=8）→ 连接后立即断开。
     * 回到 bonding+SC+ENC：Windows 配对/加密成功（win_conn1 实测 status=0）
     * 后曾出现 ~5.7s 设备侧 reason 133 断开——官方 esp-idf #17951
     * （NimBLE SM 状态机加密后超时断开，In Progress）高度吻合；本版加
     * ATT 观测日志（access_cb 全打 WARN）用于确认加密后请求是否到达。 */
    ble_hs_cfg.sm_io_cap = BLE_HS_IO_NO_INPUT_OUTPUT;
    ble_hs_cfg.sm_bonding = 1;
    ble_hs_cfg.sm_mitm = 0;
    ble_hs_cfg.sm_sc = 1;
    ble_hs_cfg.sm_our_key_dist = BLE_SM_PAIR_KEY_DIST_ENC;
    ble_hs_cfg.sm_their_key_dist = BLE_SM_PAIR_KEY_DIST_ENC;

    /* 上传超时检测：独立任务（避免在定时器回调里阻塞 SD 锁） */
    xTaskCreatePinnedToCore(timeout_task, "ble_upl_to", 4096, NULL, 1, NULL, 0);

    nimble_port_freertos_init(host_task);
    ESP_LOGI(TAG, "gatt ready");
    vTaskDelete(NULL);
}

void ble_gatt_init(void) {
    if (xTaskCreatePinnedToCore(ble_init_task, "ble_init", 8192, NULL, 5, NULL, 0)
        != pdPASS) {
        ESP_LOGE(TAG, "ble init task create failed");
    }
}
