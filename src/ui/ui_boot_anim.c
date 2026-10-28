/* ============================================================================
 * ui_boot_anim.c - 开机终端画面（LVGL 版）
 *
 * 设计：LVGL 就绪后创建"终端式"全屏画面——左上角大号像素字体标题 +
 *       内核日志滚动区 + 底部 TUI 字符条进度（[####....] 42% 同行末尾；
 *       UnSCII 字体只有 ASCII 段，块元素用 # 实心 / . 空模拟）。
 *       日志经 esp_log_set_vprintf 钩子无过滤捕获，40ms 定时器快速消费
 *       （滚动流畅，加大打印量后效果明显）。intro 就绪后 hide() 隐藏
 *       画面、全局日志降到 WARN（周期/过程性 INFO 静默，W/E 保留，
 *       nfc-verbose 命令可恢复 INFO）。
 *
 * 线程约定：
 *   - attach_log()  app_main 首行（挂 vprintf 钩子，无 LVGL 依赖）
 *   - init()        bsp_lvgl_init 之后（内部持 bsp_lvgl_lock）
 *   - progress()    任意线程（仅存 volatile 值，LVGL 定时器消费）
 *   - is_done()     任意线程（只读）
 *   - hide()        播放器线程（内部持锁；幂等）
 * ==========================================================================*/

#include "ui_boot_anim.h"

#include <stdarg.h>
#include <stdio.h>
#include <string.h>

#include "lvgl.h"
#include "bsp_lvgl.h"
#include "esp_log.h"
#include "esp_timer.h"
#include "esp_rom_serial_output.h"
#include "freertos/FreeRTOS.h"
#include "freertos/portmacro.h"

static const char *TAG = "ui_boot_anim";

#define BOOT_ANIM_MS  5000u
#define BOOT_TITLE    "INITIALIZING..."
#define LOG_TOP       26
#define LOG_LINE_H    16
#define LOG_LINES     17
#define LOG_LINE_MAX  40
#define LOG_BUF_MAX   60

static lv_obj_t *s_root = NULL;
static lv_obj_t *s_log_labels[LOG_LINES] = {0};
static lv_obj_t *s_bar_label = NULL;
static int64_t s_start_us = 0;
static volatile bool s_hidden = false;
static bool s_ready = false;

/* TUI 字符条：UnSCII 每字符 16px 宽（adv_w=256/16），屏宽 240px 只够
 * 15 字符——[ + 8 格 + ] + 百分比 = 15 字符恰满，格数取 8（每格 12.5%） */
#define BAR_CELLS 8

/* 日志环形缓冲：钩子线程（任意任务）写，LVGL 定时器读 */
static char s_log_lines[LOG_BUF_MAX][LOG_LINE_MAX];
static volatile int s_log_head = 0;
static volatile int s_log_count = 0;
static portMUX_TYPE s_log_mux = portMUX_INITIALIZER_UNLOCKED;
static bool s_log_attached = false;

/* 进度值：SD 读取线程写，LVGL 定时器消费 */
static volatile uint32_t s_pct_cur = 0;
static volatile bool s_pct_dirty = false;

/* 日志钩子：格式化一次，双路输出——
 * ① 环形缓冲（屏幕终端全量显示，40ms 定时器消费）；
 * ② UART 寄存器直写（非阻塞：FIFO 满即丢字符，绝不在驱动/锁上阻塞）。
 * 背景：曾用 vprintf→stdout→UART 驱动转发，开机日志洪峰下偶发驱动层
 * 阻塞（esp_log 锁被永久持有，全部日志静默而系统正常）——直写根治。 */
static void rom_puts(const char *s)
{
    while (*s) {
        esp_rom_output_tx_one_char((uint8_t)*s++);
    }
}

static int boot_log_vprintf(const char *fmt, va_list ap)
{
    char tmp[LOG_LINE_MAX + 48];
    int n = vsnprintf(tmp, sizeof(tmp), fmt, ap);

    while (n > 0 && (tmp[n - 1] == '\n' || tmp[n - 1] == '\r')) {
        tmp[--n] = '\0';
    }
    if (n > 0) {
        if (n >= LOG_LINE_MAX) n = LOG_LINE_MAX - 1;
        portENTER_CRITICAL(&s_log_mux);
        memcpy(s_log_lines[s_log_head], tmp, (size_t)n + 1);
        s_log_head = (s_log_head + 1) % LOG_BUF_MAX;
        if (s_log_count < LOG_BUF_MAX) s_log_count++;
        portEXIT_CRITICAL(&s_log_mux);
        rom_puts(tmp);
        rom_puts("\r\n");
    }
    return n;
}

void ui_boot_anim_attach_log(void)
{
    if (s_log_attached) return;
    s_log_attached = true;
    esp_log_set_vprintf(boot_log_vprintf);
}

/* 级别着色：E=红 W=黄 其余=白 */
static lv_color_t log_line_color(const char *line)
{
    switch (line[0]) {
    case 'E': return lv_color_hex(0xFF4040);
    case 'W': return lv_color_hex(0xFFC040);
    default:  return lv_color_hex(0xFFFFFF);
    }
}

/* 40ms 定时器：全量重建日志区（取环形缓冲最近 LOG_LINES 行）+ 进度条 */
static void boot_anim_timer_cb(lv_timer_t *timer)
{
    (void)timer;
    if (s_root == NULL || s_hidden) return;

    int head, count;
    portENTER_CRITICAL(&s_log_mux);
    head = s_log_head;
    count = s_log_count;
    portEXIT_CRITICAL(&s_log_mux);
    if (count == 0) count = LOG_BUF_MAX;   /* 满环：head 指向最旧行的前一位 */

    int start = (head - count + LOG_BUF_MAX * 2) % LOG_BUF_MAX;
    int first = (count > LOG_LINES) ? (count - LOG_LINES) : 0;
    start = (start + first) % LOG_BUF_MAX;
    int shown = count - first;

    for (int i = 0; i < LOG_LINES; i++) {
        if (i < shown) {
            const char *line = s_log_lines[(start + i) % LOG_BUF_MAX];
            lv_label_set_text(s_log_labels[i], line);
            lv_obj_set_style_text_color(s_log_labels[i], log_line_color(line), 0);
        } else {
            lv_label_set_text(s_log_labels[i], "");
        }
    }

    if (s_pct_dirty) {
        s_pct_dirty = false;
        uint32_t p = s_pct_cur;
        if (p > 100) p = 100;
        /* UnSCII 字体只有 ASCII 段（无 █/░ 块元素字形），用 # 实心 / . 空 */
        char bar[8 + BAR_CELLS + 8];
        int bi = 0;
        bar[bi++] = '[';
        for (int i = 0; i < BAR_CELLS; i++) {
            bar[bi++] = ((uint32_t)(i * 100 / BAR_CELLS) < p) ? '#' : '.';
        }
        bar[bi++] = ']';
        snprintf(bar + bi, sizeof(bar) - bi, " %lu%%", (unsigned long)p);
        lv_label_set_text(s_bar_label, bar);
    }
}

void ui_boot_anim_init(void)
{
    if (s_ready) return;
    s_ready = true;
    s_start_us = esp_timer_get_time();

    bsp_lvgl_lock();

    s_root = lv_obj_create(lv_scr_act());
    lv_obj_remove_style_all(s_root);
    lv_obj_set_size(s_root, 240, 320);
    lv_obj_set_pos(s_root, 0, 0);
    lv_obj_set_style_bg_color(s_root, lv_color_hex(0x000000), 0);
    lv_obj_set_style_bg_opa(s_root, LV_OPA_COVER, 0);

    lv_obj_t *title = lv_label_create(s_root);
    lv_label_set_text(title, BOOT_TITLE);
    lv_obj_set_style_text_color(title, lv_color_hex(0xFFFFFF), 0);
    lv_obj_set_style_text_font(title, &lv_font_unscii_16, 0);
    lv_obj_set_pos(title, 0, 0);

    for (int i = 0; i < LOG_LINES; i++) {
        s_log_labels[i] = lv_label_create(s_root);
        lv_label_set_text(s_log_labels[i], "");
        lv_obj_set_style_text_font(s_log_labels[i], &lv_font_montserrat_12, 0);
        lv_obj_set_pos(s_log_labels[i], 2, LOG_TOP + i * LOG_LINE_H);
    }

    s_bar_label = lv_label_create(s_root);
    lv_label_set_text(s_bar_label, "[........] 0%");
    lv_obj_set_style_text_color(s_bar_label, lv_color_hex(0x80FF80), 0);
    lv_obj_set_style_text_font(s_bar_label, &lv_font_unscii_16, 0);
    lv_obj_align(s_bar_label, LV_ALIGN_BOTTOM_LEFT, 0, -2);

    lv_timer_create(boot_anim_timer_cb, 40, NULL);

    bsp_lvgl_unlock();

    ESP_LOGI(TAG, "boot animation up (%u ms window)", BOOT_ANIM_MS);
}

void ui_boot_anim_progress(uint32_t done, uint32_t total)
{
    if (!s_ready || s_hidden || total == 0) return;
    if (done > total) done = total;
    uint32_t p = (uint32_t)(((uint64_t)done * 100u) / total);
    /* 进度只升不降：intro/字库各有最多 3 次重试，重试会从段首重新读数，
     * 终端窗口内肉眼可见"到 100% 又掉回去重新加载"——钳位后重试静默进行 */
    if (p < s_pct_cur) return;
    s_pct_cur = p;
    s_pct_dirty = true;
}

bool ui_boot_anim_is_done(void)
{
    if (!s_ready || s_hidden) return true;
    return (esp_timer_get_time() - s_start_us) >= (int64_t)BOOT_ANIM_MS * 1000;
}

void ui_boot_anim_hide(void)
{
    if (s_hidden) return;
    s_hidden = true;
    bsp_lvgl_lock();
    if (s_root != NULL) lv_obj_add_flag(s_root, LV_OBJ_FLAG_HIDDEN);
    bsp_lvgl_unlock();
    esp_log_level_set("*", ESP_LOG_WARN);
    esp_log_level_set("usbident", ESP_LOG_INFO);   /* USB 身份行保持 INFO（PC 免扫描识别） */
    ESP_LOGW(TAG, "boot terminal closed: logs lowered to WARN (nfc-verbose restores INFO)");
}
