/* ============================================================================
 * nfc_profile.c - 自己档案（NVS 持久化）+ USB 串口控制台
 *
 * 功能块：
 *   1. NVS 读写：uid/motto 两个字符串键，启动读档（缺失生成默认）
 *   2. 默认 uid：MAC 后 4 字节十进制（0~4294967295，≤10 位，唯一性够用）
 *   3. 串口控制台：esp_console REPL + 6 条命令（本固件控制台走
 *      USB Serial JTAG，波特率无效，直接插 USB 开串口工具即可）
 *
 * 可调整内容：
 *   - NFC_PROFILE_UID_MAX/MOTTO_MAX（.h）：放宽上限需确认对端协议
 *     字段容量（nfc_field_t 的 uid[20]/motto[20] 已对齐）。
 *   - 控制台命令：新增功能只需照 cmd_nfc_show 模板加一条
 *     esp_console_cmd_t 并 register。
 * ==========================================================================*/

#include "nfc_profile.h"
#include "card_store.h"
#include "ui_core_screen.h"
#include "bsp_sdcard.h"
#include "bsp_power.h"
#include "bsp_lvgl.h"

#include <string.h>
#include <stdio.h>
#include <stdlib.h>
#include <ctype.h>

#include "esp_log.h"
#include "esp_system.h"
#include "esp_mac.h"
#include "nvs.h"
#include "nvs_flash.h"
#include "esp_console.h"

static const char *TAG = "nfc_profile";

#define NVS_NS       "nfc"
#define NVS_KEY_UID  "uid"
#define NVS_KEY_MOTTO "motto"
#define NVS_KEY_UNAME "uname"

static char s_uid[NFC_PROFILE_UID_MAX + 1];
static char s_motto[NFC_PROFILE_MOTTO_MAX + 1];
static char s_uname[NFC_PROFILE_UNAME_MAX + 1];

/* ---------- 默认 uid 生成 ----------
 * 功能：取 WiFi STA MAC 后 4 字节拼成十进制数；读 MAC 失败回退固定值
 *       "1000000000"（仅极罕见硬件异常路径，正常永远走 MAC 分支）。 */
static void generate_default_uid(char *out, size_t len)
{
    uint8_t mac[6] = {0};
    if (esp_read_mac(mac, ESP_MAC_WIFI_STA) != ESP_OK) {
        snprintf(out, len, "1000000000");
        return;
    }
    uint32_t v = ((uint32_t)mac[2] << 24) | ((uint32_t)mac[3] << 16) |
                 ((uint32_t)mac[4] << 8) | (uint32_t)mac[5];
    snprintf(out, len, "%lu", (unsigned long)v);
}

/* ---------- 启动读档 ----------
 * 功能：从 NVS 读 uid/motto 到内存缓存；首次使用自动生成并写回。
 * 可调整内容：读档失败只告警不清数据（保留 NVS 内容优先）。 */
esp_err_t nfc_profile_init(void)
{
    nvs_handle_t h;
    esp_err_t err = nvs_open(NVS_NS, NVS_READWRITE, &h);
    if (err != ESP_OK) {
        ESP_LOGE(TAG, "nvs_open failed: %s", esp_err_to_name(err));
        return err;
    }

    size_t len = sizeof(s_uid);
    err = nvs_get_str(h, NVS_KEY_UID, s_uid, &len);
    if (err == ESP_ERR_NVS_NOT_FOUND) {
        generate_default_uid(s_uid, sizeof(s_uid));
        nvs_set_str(h, NVS_KEY_UID, s_uid);
        ESP_LOGI(TAG, "generated default uid: %s", s_uid);
    } else if (err != ESP_OK) {
        ESP_LOGE(TAG, "read uid failed: %s", esp_err_to_name(err));
    }

    len = sizeof(s_motto);
    err = nvs_get_str(h, NVS_KEY_MOTTO, s_motto, &len);
    if (err == ESP_ERR_NVS_NOT_FOUND) {
        s_motto[0] = '\0';
        nvs_set_str(h, NVS_KEY_MOTTO, s_motto);
    } else if (err != ESP_OK) {
        ESP_LOGE(TAG, "read motto failed: %s", esp_err_to_name(err));
    }

    len = sizeof(s_uname);
    err = nvs_get_str(h, NVS_KEY_UNAME, s_uname, &len);
    if (err == ESP_ERR_NVS_NOT_FOUND) {
        s_uname[0] = '\0';
        nvs_set_str(h, NVS_KEY_UNAME, s_uname);
    } else if (err != ESP_OK) {
        ESP_LOGE(TAG, "read uname failed: %s", esp_err_to_name(err));
    }

    nvs_commit(h);
    nvs_close(h);
    ESP_LOGI(TAG, "profile: uid=\"%s\" motto=\"%s\" uname=\"%s\"", s_uid, s_motto, s_uname);
    return ESP_OK;
}

const char *nfc_profile_get_uid(void)
{
    return s_uid;
}

const char *nfc_profile_get_motto(void)
{
    return s_motto;
}

const char *nfc_profile_get_uname(void)
{
    return s_uname;
}

static esp_err_t profile_save_str(const char *key, const char *val)
{
    nvs_handle_t h;
    esp_err_t err = nvs_open(NVS_NS, NVS_READWRITE, &h);
    if (err != ESP_OK) {
        return err;
    }
    err = nvs_set_str(h, key, val);
    if (err == ESP_OK) {
        nvs_commit(h);
    }
    nvs_close(h);
    return err;
}

/* ---------- 设置 uid：仅接受 1..20 位纯数字，写 NVS + 内存缓存 ---------- */
esp_err_t nfc_profile_set_uid(const char *uid)
{
    size_t n = strlen(uid);
    if (n == 0 || n > NFC_PROFILE_UID_MAX) {
        ESP_LOGE(TAG, "uid length must be 1..%d", NFC_PROFILE_UID_MAX);
        return ESP_ERR_INVALID_ARG;
    }
    for (size_t i = 0; i < n; i++) {
        if (!isdigit((unsigned char)uid[i])) {
            ESP_LOGE(TAG, "uid must be digits only");
            return ESP_ERR_INVALID_ARG;
        }
    }
    esp_err_t err = profile_save_str(NVS_KEY_UID, uid);
    if (err == ESP_OK) {
        strncpy(s_uid, uid, sizeof(s_uid) - 1);
        s_uid[sizeof(s_uid) - 1] = '\0';
    }
    return err;
}

/* ---------- 设置寄语：最长 20 字节（可空），写 NVS + 内存缓存 ---------- */
esp_err_t nfc_profile_set_motto(const char *motto)
{
    if (strlen(motto) > NFC_PROFILE_MOTTO_MAX) {
        ESP_LOGE(TAG, "motto length must be <= %d bytes", NFC_PROFILE_MOTTO_MAX);
        return ESP_ERR_INVALID_ARG;
    }
    esp_err_t err = profile_save_str(NVS_KEY_MOTTO, motto);
    if (err == ESP_OK) {
        strncpy(s_motto, motto, sizeof(s_motto) - 1);
        s_motto[sizeof(s_motto) - 1] = '\0';
    }
    return err;
}

/* ---------- 设置用户名：最长 64 字节（可空=未配置），写 NVS + 内存缓存 ----------
 * 64 字节容 20 个汉字（UTF-8 ×3B=60）+ NUL；不再限制 ASCII——中文
 * 显示走 SD 字库（未加载时 hint_24 fallback），超长自省由 ui 侧 UI 约束。
 * 字符集禁控制字符（换行/转义）以防注入日志/卡片字段。 */
esp_err_t nfc_profile_set_uname(const char *uname)
{
    size_t n = strlen(uname);
    if (n > NFC_PROFILE_UNAME_MAX) {
        ESP_LOGE(TAG, "uname length must be <= %d bytes", NFC_PROFILE_UNAME_MAX);
        return ESP_ERR_INVALID_ARG;
    }
    for (size_t i = 0; i < n; i++) {
        unsigned char c = (unsigned char)uname[i];
        if (c < 0x20 || c == 0x7F) {
            ESP_LOGE(TAG, "uname charset: no control chars");
            return ESP_ERR_INVALID_ARG;
        }
    }
    esp_err_t err = profile_save_str(NVS_KEY_UNAME, uname);
    if (err == ESP_OK) {
        strncpy(s_uname, uname, sizeof(s_uname) - 1);
        s_uname[sizeof(s_uname) - 1] = '\0';
    }
    return err;
}

/* ============ console 命令 ============
 * 无参数 = 查询当前值，带参数 = 设置（esp_console 的 argtable 未用，
 * 直接按 argv 风格解析，简单够用）。 */

/* nfc-uid [数字]：查询/设置 uid */
static int cmd_nfc_uid(int argc, char **argv)
{
    if (argc < 2) {
        printf("current uid: %s\n", nfc_profile_get_uid());
        return 0;
    }
    esp_err_t err = nfc_profile_set_uid(argv[1]);
    if (err == ESP_OK) {
        printf("uid set to: %s\n", nfc_profile_get_uid());
    } else {
        printf("error: %s\n", esp_err_to_name(err));
    }
    return 0;
}

/* nfc-motto [文本]：查询/设置寄语 */
static int cmd_nfc_motto(int argc, char **argv)
{
    if (argc < 2) {
        printf("current motto: %s\n", nfc_profile_get_motto());
        return 0;
    }
    esp_err_t err = nfc_profile_set_motto(argv[1]);
    if (err == ESP_OK) {
        printf("motto set to: %s\n", nfc_profile_get_motto());
    } else {
        printf("error: %s\n", esp_err_to_name(err));
    }
    return 0;
}

/* nfc-uname [文本]：查询/设置用户名（空串 = 清除，横幅回退显示角色名） */
static int cmd_nfc_uname(int argc, char **argv)
{
    if (argc < 2) {
        printf("current uname: %s\n", nfc_profile_get_uname());
        return 0;
    }
    esp_err_t err = nfc_profile_set_uname(argv[1]);
    if (err == ESP_OK) {
        printf("uname set to: %s\n", nfc_profile_get_uname());
    } else {
        printf("error: %s\n", esp_err_to_name(err));
    }
    return 0;
}

/* nfc-show：打印当前档案全貌 */
static int cmd_nfc_show(int argc, char **argv)
{
    (void)argc;
    (void)argv;
    printf("== nfc profile ==\n");
    printf("uid:   %s\n", nfc_profile_get_uid());
    printf("motto: %s\n", nfc_profile_get_motto());
    printf("uname: %s\n", nfc_profile_get_uname());
    return 0;
}

/* nfc-list：打印 SD 卡上的收卡记录（/sdcard/cards.txt 全文） */
static int cmd_nfc_list(int argc, char **argv)
{
    (void)argc;
    (void)argv;
    bsp_sdcard_lock();
    FILE *f = fopen(BSP_SDCARD_MOUNT_POINT "/cards.txt", "r");
    if (f == NULL) {
        bsp_sdcard_unlock();
        printf("(no cards logged yet, or SD not mounted)\n");
        return 0;
    }
    /* static 缓冲：控制台 REPL 任务栈仅 4KB，放栈上会栈溢出 */
    static char line[256];
    while (fgets(line, sizeof(line), f) != NULL) {
        fputs(line, stdout);
    }
    fclose(f);
    bsp_sdcard_unlock();
    return 0;
}

/* nfc-banner：调试用，直接弹一次测试横幅（无需双机交换）。
 * 用于验证横幅 浮入 → 5s 静止 → 浮出 全流程与日志。 */
static int cmd_nfc_banner(int argc, char **argv)
{
    (void)argc;
    (void)argv;
    printf("showing test banner...\n");
    /* payload=NULL 时头像回退内置占位图 */
    ui_core_screen_show_banner("DEMO1", "TEST", "12345678",
                               NULL, 0, 0, 0, 0, NULL);
    return 0;
}

/* nfc-verbose：把全局日志级别恢复 INFO（intro 开始后固件自动降到
 * WARN 以静默周期噪音，调试需要时随时切回）。 */
static int cmd_nfc_verbose(int argc, char **argv)
{
    (void)argc;
    (void)argv;
    esp_log_level_set("*", ESP_LOG_INFO);
    printf("log level back to INFO\n");
    return 0;
}

/* ---------- pwr：电源状态（USB 检测 / 电池电压 / 电量 / 门禁） ---------- */
static int cmd_pwr(int argc, char **argv)
{
    (void)argc;
    (void)argv;
    printf("usb_det=%d vbat=%lumV soc=%d%% hw_valid=%d storage_ok=%d\n",
           (int)bsp_power_usb_det(), (unsigned long)bsp_power_vbat_mv(),
           bsp_power_soc_pct(), (int)bsp_power_hw_valid(),
           (int)bsp_power_storage_ok());
    return 0;
}

/* ---------- bl：背光亮度（PWM 0~100） ---------- */
static int cmd_bl(int argc, char **argv)
{
    if (argc < 2) {
        printf("usage: bl <0-100>\n");
        return 1;
    }
    char *end = NULL;
    long v = strtol(argv[1], &end, 10);
    if (end == argv[1] || *end != '\0' || v < 0 || v > 100) {
        printf("invalid value: %s (expected 0-100)\n", argv[1]);
        return 1;
    }
    bsp_lcd_backlight((uint8_t)v);
    printf("backlight = %ld%%\n", v);
    return 0;
}

/* ---------- 控制台挂载 ----------
 * 功能：esp_console 初始化 + 注册 4 条命令 + 启动 REPL（USB Serial
 *       JTAG 通道，随本固件 sdkconfig 的 console 配置自动选择）。
 * 可调整内容：repl.prompt "esp> "（提示符）；max_cmdline_length 默认
 *       256 字节够用（motto 才 20 字）。 */
void nfc_profile_console_register(void)
{
    /* 注意：不要在这里调用 esp_console_init —— esp_console_new_repl_xxx
     * 内部（esp_console_common_init）会自己调 esp_console_init，重复调用
     * 会得到 ESP_ERR_INVALID_STATE 导致 repl 静默失败。 */
    const esp_console_cmd_t cmd_uid = {
        .command = "nfc-uid",
        .help = "get/set own uid (digits, <=20). usage: nfc-uid [digits]",
        .hint = NULL,
        .func = cmd_nfc_uid,
        .argtable = NULL,
    };
    const esp_console_cmd_t cmd_motto = {
        .command = "nfc-motto",
        .help = "get/set own motto (<=20 chars). usage: nfc-motto [text]",
        .hint = NULL,
        .func = cmd_nfc_motto,
        .argtable = NULL,
    };
    const esp_console_cmd_t cmd_uname = {
        .command = "nfc-uname",
        .help = "get/set own username (<=15 ASCII chars; empty=unset). usage: nfc-uname [text]",
        .hint = NULL,
        .func = cmd_nfc_uname,
        .argtable = NULL,
    };
    const esp_console_cmd_t cmd_show = {
        .command = "nfc-show",
        .help = "show current nfc profile",
        .hint = NULL,
        .func = cmd_nfc_show,
        .argtable = NULL,
    };
    const esp_console_cmd_t cmd_list = {
        .command = "nfc-list",
        .help = "list received cards (index.json)",
        .hint = NULL,
        .func = cmd_nfc_list,
        .argtable = NULL,
    };
    const esp_console_cmd_t cmd_banner = {
        .command = "nfc-banner",
        .help = "show a test banner (debug)",
        .hint = NULL,
        .func = cmd_nfc_banner,
        .argtable = NULL,
    };
    const esp_console_cmd_t cmd_verbose = {
        .command = "nfc-verbose",
        .help = "restore global log level to INFO (intro lowers it to WARN)",
        .hint = NULL,
        .func = cmd_nfc_verbose,
        .argtable = NULL,
    };
    const esp_console_cmd_t cmd_power = {
        .command = "pwr",
        .help = "show power status (usb/vbat/soc/storage_ok)",
        .hint = NULL,
        .func = cmd_pwr,
        .argtable = NULL,
    };
    const esp_console_cmd_t cmd_backlight = {
        .command = "bl",
        .help = "set backlight percent 0-100. usage: bl <0-100>",
        .hint = NULL,
        .func = cmd_bl,
        .argtable = NULL,
    };

    esp_console_cmd_register(&cmd_uid);
    esp_console_cmd_register(&cmd_motto);
    esp_console_cmd_register(&cmd_uname);
    esp_console_cmd_register(&cmd_show);
    esp_console_cmd_register(&cmd_list);
    esp_console_cmd_register(&cmd_banner);
    esp_console_cmd_register(&cmd_verbose);
    esp_console_cmd_register(&cmd_power);
    esp_console_cmd_register(&cmd_backlight);

#if CONFIG_ESP_CONSOLE_USB_SERIAL_JTAG
    esp_console_dev_usb_serial_jtag_config_t dev = ESP_CONSOLE_DEV_USB_SERIAL_JTAG_CONFIG_DEFAULT();
    esp_console_repl_config_t repl = ESP_CONSOLE_REPL_CONFIG_DEFAULT();
    repl.prompt = "esp> ";
    repl.task_stack_size = 16384;
    repl.task_priority = 10;
    esp_console_repl_t *repl_h = NULL;
    esp_err_t nerr = esp_console_new_repl_usb_serial_jtag(&dev, &repl, &repl_h);
    if (nerr == ESP_OK && repl_h != NULL) {
        esp_err_t serr = esp_console_start_repl(repl_h);
        if (serr != ESP_OK) {
            ESP_LOGE(TAG, "start_repl failed: %s", esp_err_to_name(serr));
        }
    } else {
        ESP_LOGE(TAG, "new_repl_usb_serial_jtag failed: %s", esp_err_to_name(nerr));
    }
#endif
    ESP_LOGI(TAG, "console ready: nfc-uname / nfc-uid / nfc-motto / nfc-show / nfc-list / nfc-banner / nfc-verbose / pwr / bl");
}
