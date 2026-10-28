/* ============================================================================
 * ui_menu.c - 触控菜单：居中面板 + 全屏子页（快速三击打开；上滑手势亦可）
 *
 *   - 三击检测（相邻间隔 <= CLICK_TIMEOUT_MS，直接读触控 IRQ 电平）打开菜单；
 *   - 打开后：全屏半透明黑遮罩（点击关闭）+ 全屏面板滑入（280ms ease_out）；
 *     面板为半透明黑底，整体向左上偏移 SHEET_X/Y=-10 抵消触控映射偏移；
 *   - 内容 4 个入口：
 *       角色选择  扫描 SD 角色目录（头像+名字）；写 current.txt -> 热重载
 *       名片簿    cards.txt 列表（时间/用户ID）-> 点开大头像/详情
 *       蓝牙      连接状态 / 广播开关 / 素材上传进度
 *       系统信息  版本 / SD 状态 / 存储 / 日志级别切换
 *   - 点按遮罩区（面板外）关闭；子页带页头"返回"；子页切换 x 向滑入/
 *     滑出（200ms）。不做下滑关闭手势（避免与列表滚动冲突）。
 * 线程与锁：
 *   - 手势检测 lv_timer(10ms) 轮询 bsp_touch_get_indev()，运行在 LVGL
 *     任务上下文（lv_timer_handler 内），页面构建/销毁天然持 LVGL 锁；
 *   - SD 读取（角色枚举/portrait/cards.txt/blob）在页面构建时同步进行
 *     （内部持 SD 锁）：单页构建 ~100-200ms 的短暂停顿可接受（菜单是
 *     瞬时交互，视频掉几帧不可见）；
 *   - 菜单打开期间 ui_core_screen 的横幅/中央提示被抑制（is_open 查询）；
 *   - 蓝牙状态由 ui_menu_notify_ble_state 通知（BLE 线程），或本模块
 *     500ms 轮询 ble_gatt_* 刷新。
 * 可调整内容（集中在文件顶部宏）：
 *   - CLICK_TIMEOUT_MS 700：三次点击的相邻点击间隔上限
 *   - SHEET_H 320 / SHEET_X/Y -10 / SHEET_OPA 90%：面板尺寸/偏移/不透明度
 *   - SHEET_OPEN_MS 280 / PAGE_SLIDE_MS 200：动效时长
 *   - 文案只能使用 tools/gen_fonts.py 的 MENU_CJK + ASCII 字符集。
 * ==========================================================================*/

// ui/ui_menu.c
#include "ui_menu.h"
#include "bsp_lvgl.h"
#include "bsp_touch.h"
#include "bsp_sdcard.h"
#include "roles.h"
#include "card_book.h"
#include "sd_fonts.h"
#include "ui_core_screen.h"
#include "ui_options.h"
#include "mjpeg_player.h"
#include "ble_gatt.h"
#include "version.h"

#include <stdio.h>
#include <string.h>
#include <stdlib.h>
#include "esp_log.h"
#include "esp_timer.h"
#include "esp_system.h"
#include "esp_spiffs.h"
#include "esp_heap_caps.h"
#include "time_sync.h"
#include "../resources/default_avatar.h"

static const char *TAG = "MENU";

LV_FONT_DECLARE(menu_20);
LV_FONT_DECLARE(menu_16);
LV_FONT_DECLARE(role_name_14);
LV_FONT_DECLARE(role_name_18);
LV_FONT_DECLARE(uid_12);

static const lv_font_t *menu_cjk_font_cn(bool large);

/* ---------- 鎵嬪娍涓庡嚑浣曞弬鏁?---------- */
/* 涓婃粦鎵嬪娍锛氳捣鐐瑰湪搴曢儴 GESTURE_START_Y 浠ヤ笅锛屼笂绉?鈮?GESTURE_DIST锛? * 姘村钩鍋忕Щ 鈮?GESTURE_DX_MAX锛堥槻璇Е锛夊嵆鎵撳紑鑿滃崟 */
#define GESTURE_START_Y  255   /* 手势起点须在屏幕下部 */
#define GESTURE_DIST     160    /* 涓婃粦璺濈锛坧x锛?*/
#define GESTURE_DX_MAX   60    /* 姘村钩婕傜Щ涓婇檺锛坧x锛?*/
#define SHEET_H         320   /* 面板高度（全屏） */
#define SHEET_X         -13     /* 闈㈡澘 x 涓嶅亸绉伙紙瑙︽帶鏄犲皠宸蹭慨姝ｏ紝鏄剧ず淇濇寔鍏ㄥ睆姝ｄ綅锛?*/

/* 闈㈡澘涓婄Щ閲忥細椤跺嚭灞?13px锛岃鍗婇€忔槑鍖哄煙瑙嗚涓婂崰婊℃暣涓睆骞?*/
#define SHEET_TOP_OFF   13
#define SHEET_Y         (-20 - SHEET_TOP_OFF)
#define SHEET_OPA       LV_OPA_90
#define SHEET_OPEN_MS   280   /* 闈㈡澘婊戝叆鏃堕暱锛坋ase_out锛?*/
#define SHEET_CLOSE_MS  250   /* 闈㈡澘婊戝嚭鏃堕暱锛坋ase_in_out锛?*/
#define PAGE_SLIDE_MS   200   /* 子页切换时长 */
#define HEADER_H        36
#define MAX_ROLES   32    /* 骞插憳鍒楄〃涓婇檺锛堥槻鏋佺鐩綍鏁伴噺鎷栧灝鏋勫缓锛?*/

/* ---------- 页面枚举 ---------- */
typedef enum {
    PAGE_MENU = 0,
    PAGE_ROLES,
    PAGE_CARDS,
    PAGE_CARD_DETAIL,
    PAGE_BLE,
    PAGE_UIOPTS,
    PAGE_INFO,
} menu_page_t;

/* ---------- 闈欐€佹帶浠?---------- */
static lv_obj_t *s_layer = NULL;      /* 鍏ㄥ睆鑿滃崟灞傦紙鍗婇€忔槑榛戦伄缃╋級 */
static lv_obj_t *s_sheet = NULL;      /* 底部抽屉面板 */
static lv_obj_t *s_header = NULL;     /* 椤靛ご锛堣繑鍥?+ 鏍囬锛?*/
static lv_obj_t *s_back_btn = NULL;
static lv_obj_t *s_title = NULL;
static lv_obj_t *s_content = NULL;    /* 椤甸潰鍐呭瀹瑰櫒锛堥殢椤甸噸寤猴級 */
static bool      s_menu_open = false;
static menu_page_t s_page = PAGE_MENU;

/* ---------- 鎵嬪娍鐘舵€侊紙涓婃粦鎵撳紑鑿滃崟锛?---------- */
static bool     s_down = false;
static bool     s_open_fired = false;
static int16_t  s_down_x = 0;
static int16_t  s_down_y = 0;

/* ---------- 鑿滃崟鍐呮墜鍔匡細鍒楄〃鎷栧姩锛堟墜鍔ㄦ粴鍔級+ 椤堕儴涓嬫粦鏀跺洖 ----------
 * LVGL 鑷甫婊氬姩渚濊禆 indev 閲囨牱锛堝疄闄?~30Hz锛変笖涓庤鐐瑰嚮闂ㄦ帶绾犵紶锛屾墜鎰? * 鐢熸订鈥斺€旀敼涓烘墜鍔垮畾鏃跺櫒鐩存帴閲囨牱瑙︽帶鎷栧姩 list 鐨?scroll_y锛堣窡鎵嬨€佸彲
 * 鎺э級銆備笅婊戞敹鍥炰粎鍦ㄨ捣鐐?y<100锛堥潰鏉块《閮ㄥ尯锛夋瑁咃紝閬垮厤涓庡垪琛ㄦ粴鍔ㄦ姠
 * 鎵嬪娍銆?*/
static lv_obj_t *s_list = NULL;       /* 褰撳墠鍒楄〃椤靛鍣紙骞插憳/鍚嶇墖锛?*/
static bool     s_drag_prev = false;  /* 涓婁竴鎷嶆寜鍘嬬姸鎬侊紙娌挎娴嬶級 */
static int      s_drag_anchor = 0;    /* 鎸変笅娌胯Е鐐?y锛堝睆骞曞潗鏍囷級 */
static int      s_drag_base = 0;      /* 鎸変笅娌?list 鐨?scroll_y */
static bool     s_drag_active = false; /* 正在拖动列表 */
static bool     s_pull_armed = false;  /* 涓嬫粦鏀跺洖宸叉瑁咃紙璧风偣鍦ㄩ《閮ㄥ尯锛?*/

/* ---------- 瑙︽帶鏍″噯寮曞灞傦紙鏈牎鍑嗘椂鏄剧ず锛?---------- */
static lv_obj_t   *s_cal_layer = NULL;
static lv_obj_t   *s_lbl_cal_cnt = NULL;
static lv_obj_t   *s_lbl_cal_hint = NULL;
static lv_timer_t *s_cal_timer = NULL;
static uint8_t     s_cal_done_ticks = 0;

/* ---------- 椤甸潰涓存椂鏁版嵁锛堝叧闂?鍒囬〉鏃堕噴鏀撅級 ---------- */
static role_entry_t s_role_entries[MAX_ROLES];
static int  s_role_count = 0;
static uint8_t **s_role_portraits = NULL;   /* MAX_ROLES 脳 8192B PSRAM */
static card_entry_t s_card_entries[CARD_BOOK_MAX_LIST];
static int  s_card_count = 0;
static card_entry_t s_cur_card;           /* 璇︽儏椤靛綋鍓嶅悕鐗囨潯鐩?*/
static uint8_t *s_card_blob = NULL;
static size_t s_card_blob_len = 0;
static bool s_fallback_blob = false;   /* 璇︽儏椤?fallback 澶达紙static锛屼笉 free锛?*/

/* ---------- RGB565 瀛楄妭搴忚浆鎹?----------
 * portrait.bin / 鍚嶇墖 avatar 鍧囦负鏍囧噯灏忕 RGB565锛汱VGL 閰嶇疆
 * LV_COLOR_16_SWAP=1 瑕佹眰姣?16bit 楂樹綆瀛楄妭瀵硅皟鍚庡杺鍏ワ紝鍚﹀垯鑺卞睆銆? * 灏卞湴浜ゆ崲锛堣皟鐢ㄦ柟椤绘寔鐙珛鍓湰锛屼笉寰楁薄鏌撳彂鍗＄敤鐨勫師濮嬫暟鎹級銆?*/
static void swap565_inplace(uint8_t *buf, size_t len) {
    for (size_t i = 0; i + 1 < len; i += 2) {
        uint8_t t = buf[i];
        buf[i] = buf[i + 1];
        buf[i + 1] = t;
    }
}

/* ---------- 淇℃伅椤甸潤鎬佹爣绛撅紙1s 瀹氭椂鍒锋柊鏂囨湰锛?---------- */
static lv_obj_t *s_lbl_uptime = NULL;
static lv_obj_t *s_lbl_sd     = NULL;
static lv_obj_t *s_lbl_psram  = NULL;
static lv_obj_t *s_lbl_sram   = NULL;
static lv_obj_t *s_lbl_fonts  = NULL;
static lv_obj_t *s_lbl_spiffs = NULL;
static lv_obj_t *s_lbl_loglvl = NULL;

/* ---------- 钃濈墮椤垫爣绛?---------- */
static lv_obj_t *s_lbl_ble_status = NULL;
static lv_obj_t *s_lbl_ble_adv    = NULL;
static lv_obj_t *s_lbl_ble_upload = NULL;

/* UI 选项页：每行状态文案（开/关，绿色/灰色），行数 = UIOPT_ROWS */
static lv_obj_t *s_lbl_uiopt[5];

static uint32_t s_last_info_refr = 0;   /* 淇℃伅椤典笂娆″埛鏂帮紙esp_timer ms锛?*/
static uint32_t s_last_ble_refr = 0;    /* 钃濈墮椤典笂娆″埛鏂帮紙esp_timer ms锛?*/

/* 前向声明 */
static void page_enter(menu_page_t page);
static void page_leave(menu_page_t page);
static void close_menu(void);
static void show_cal_overlay(void);
static void sheet_anim_exec(void *var, int32_t v);
static void delay_free(void *p);
static void flush_delay_free(void);
static void content_anim_exec(void *var, int32_t v);
static void back_evt_cb(lv_event_t *e);
static void content_in_ready_cb(lv_anim_t *a);
static void content_out_ready_cb(lv_anim_t *a);

/* ============================================================================
 * 鎶藉眽寮€鍚? * ==========================================================================*/

static void sheet_close_ready_cb(lv_anim_t *a) {
    (void)a;
    if (s_layer != NULL) {
        lv_obj_add_flag(s_layer, LV_OBJ_FLAG_HIDDEN);
    }
    s_menu_open = false;
    ESP_LOGI(TAG, "menu closed");
}

/* 鍏抽棴鎶藉眽锛堜换鎰忛〉闈紱鍙殢鏃惰皟鐢紝骞傜瓑锛?*/
static void close_menu(void) {
    if (!s_menu_open) return;
    page_leave(s_page);
    /* 菜单内改过 UI 开关：关闭时同步一次主界面元素显隐（幂等） */
    ui_core_screen_apply_show_opts();
    lv_obj_clean(s_content);
    lv_anim_del(s_sheet, sheet_anim_exec);
    lv_anim_del(s_content, content_anim_exec);
    lv_anim_t a;
    lv_anim_init(&a);
    lv_anim_set_var(&a, s_sheet);
    lv_anim_set_values(&a, lv_obj_get_y(s_sheet), 320);
    lv_anim_set_time(&a, SHEET_CLOSE_MS);
    lv_anim_set_exec_cb(&a, sheet_anim_exec);
    lv_anim_set_path_cb(&a, lv_anim_path_ease_in_out);
    lv_anim_set_ready_cb(&a, sheet_close_ready_cb);
    lv_anim_start(&a);
}

/* 鎵撳紑鎶藉眽锛氶伄缃╂樉绀?+ 涓昏彍鍗曢〉 + 闈㈡澘婊戝叆 */
static void open_menu(void) {
    if (s_menu_open) return;
    s_menu_open = true;
    s_page = PAGE_MENU;
    lv_obj_clear_flag(s_layer, LV_OBJ_FLAG_HIDDEN);
    lv_obj_move_foreground(s_layer);
    lv_anim_del(s_sheet, sheet_anim_exec);
    lv_obj_set_y(s_sheet, 320);
    page_enter(PAGE_MENU);
    lv_anim_t a;
    lv_anim_init(&a);
    lv_anim_set_var(&a, s_sheet);
    lv_anim_set_values(&a, 320, -SHEET_TOP_OFF);
    lv_anim_set_time(&a, SHEET_OPEN_MS);
    lv_anim_set_exec_cb(&a, sheet_anim_exec);
    lv_anim_set_path_cb(&a, lv_anim_path_ease_out);
    lv_anim_start(&a);
    ESP_LOGI(TAG, "menu opened");
}

static void sheet_anim_exec(void *var, int32_t v) {
    lv_obj_set_y((lv_obj_t *)var, (lv_coord_t)v);
}

static void content_anim_exec(void *var, int32_t v) {
    lv_obj_set_x((lv_obj_t *)var, (lv_coord_t)v);
}

/* 瀛愰〉婊戝叆瀹屾垚鍥炶皟锛氭棤棰濆鍔ㄤ綔 */
static void content_in_ready_cb(lv_anim_t *a) { (void)a; }

/* 瀛愰〉婊戝嚭瀹屾垚鍥炶皟锛氱湡姝ｅ垏鎹㈠埌鐩爣椤靛苟婊戝叆锛堟鏃舵棫椤电紦鍐叉墠鍙畨鍏ㄩ噴鏀撅級 */
static void content_out_ready_cb(lv_anim_t *a) {
    (void)a;
    flush_delay_free();
    page_enter(s_page);
}

/* 椤甸潰鍒囨崲锛氬綋鍓嶅唴瀹规粦鍑?鈫?鏋勫缓鏂伴〉 鈫?婊戝叆 */
static void nav_to(menu_page_t page) {
    if (!s_menu_open || page == s_page) return;
    page_leave(s_page);
    s_page = page;
    lv_anim_del(s_content, content_anim_exec);
    lv_anim_t a;
    lv_anim_init(&a);
    lv_anim_set_var(&a, s_content);
    lv_anim_set_values(&a, 0, 240);
    lv_anim_set_time(&a, PAGE_SLIDE_MS);
    lv_anim_set_exec_cb(&a, content_anim_exec);
    lv_anim_set_path_cb(&a, lv_anim_path_ease_in);
    lv_anim_set_ready_cb(&a, content_out_ready_cb);
    lv_anim_start(&a);
}

/* ============================================================================
 * 椤甸潰鏁版嵁鐢熷懡鍛ㄦ湡锛堝垏椤?鍏宠彍鍗曟椂閲婃斁锛?
 * ==========================================================================*/

static void page_leave(menu_page_t page) {
    /* 骞插憳椤碉細portrait 缂撳啿锛堝欢杩熼噴鏀锯€斺€旀粦鍑轰腑鏃ч〉浠嶆覆鏌擄級 */
    if (page == PAGE_ROLES && s_role_portraits != NULL) {
        for (int i = 0; i < s_role_count; i++) {
            if (s_role_portraits[i]) delay_free(s_role_portraits[i]);
        }
        delay_free(s_role_portraits);
        s_role_portraits = NULL;
        s_role_count = 0;
    }
    /* 鍚嶇墖璇︽儏椤碉細blob锛堝欢杩熼噴鏀撅級锛沠allback 鎸囧悜 static 澶达紝涓?free */
    if (page == PAGE_CARD_DETAIL && s_card_blob != NULL) {
        if (!s_fallback_blob) {
            delay_free(s_card_blob);
            s_card_blob = NULL;
            s_card_blob_len = 0;
        }
    }
    /* 淇℃伅椤垫爣绛惧け鏁?*/
    if (page == PAGE_INFO) {
        s_lbl_uptime = s_lbl_sd = s_lbl_fonts = NULL;
        s_lbl_psram = s_lbl_sram = NULL;
        s_lbl_spiffs = s_lbl_loglvl = NULL;
    }
    if (page == PAGE_BLE) {
        s_lbl_ble_status = s_lbl_ble_adv = s_lbl_ble_upload = NULL;
    }
    if (page == PAGE_UIOPTS) {
        for (int i = 0; i < 5; i++) s_lbl_uiopt[i] = NULL;
    }
    /* 鍒楄〃椤电寮€锛氬鍣ㄩ殢椤甸潰閲嶅缓锛屾嫋鍔ㄧ姸鎬佸け鏁?*/
    if (page == PAGE_ROLES || page == PAGE_CARDS) {
        s_list = NULL;
        s_drag_active = false;
        s_pull_armed = false;
    }
}

/* ---- 寤惰繜閲婃斁闃熷垪锛氭粦鍑哄姩鐢伙紙PAGE_SLIDE_MS锛夋湡闂翠笂涓€椤典粛寮曠敤
 *     portrait/blob 绛?PSRAM 缂撳啿 鈫?use-after-free銆? *     鏀堕泦鍒?content_out_ready_cb锛堝姩鐢诲畬鎴愩€佹棫椤典笉鍙锛夊啀鐪熸 heap_caps_free銆?--- */
#define DELAY_FREE_MAX 64
static void *s_delfree[DELAY_FREE_MAX];
static int s_delfree_n = 0;

static void delay_free(void *p) {
    if (p == NULL) return;
    if (s_delfree_n < DELAY_FREE_MAX) {
        s_delfree[s_delfree_n++] = p;
    } else {
        heap_caps_free(p);   /* 闃熷垪婊★細绔嬪嵆閲婃斁锛堟瀬绔儏鍐碉紝宕╂瘮娉勬紡寮憋級 */
    }
}

static void flush_delay_free(void) {
    for (int i = 0; i < s_delfree_n; i++) heap_caps_free(s_delfree[i]);
    s_delfree_n = 0;
}

/* ============================================================================
 * 閫氱敤灏忛儴浠? * ==========================================================================*/

/* 椤靛ご锛氳繑鍥炴寜閽紙PAGE_MENU 鏃狅級+ 灞呬腑鏍囬 */
static void build_header(const char *title, bool with_back) {
    lv_obj_clean(s_header);
    if (with_back) {
        s_back_btn = lv_btn_create(s_header);
        lv_obj_set_size(s_back_btn, 64, HEADER_H);
        lv_obj_set_pos(s_back_btn, 0, 0);
        lv_obj_set_style_bg_color(s_back_btn, lv_color_hex(0x18191C), 0);
        lv_obj_set_style_bg_opa(s_back_btn, LV_OPA_0, 0);
        lv_obj_set_style_border_width(s_back_btn, 0, 0);
        lv_obj_set_style_shadow_width(s_back_btn, 0, 0);
        lv_obj_set_style_pad_all(s_back_btn, 0, 0);
        lv_obj_set_style_radius(s_back_btn, 0, 0);
        lv_obj_t *lb = lv_label_create(s_back_btn);
        lv_label_set_text(lb, "返回");
        lv_obj_set_style_text_color(lb, lv_color_hex(0x9AA0A6), 0);
        lv_obj_set_style_text_font(lb, menu_cjk_font_cn(false), 0);
        lv_obj_align(lb, LV_ALIGN_LEFT_MID, 15, 0);
        lv_obj_add_event_cb(s_back_btn, back_evt_cb, LV_EVENT_CLICKED, NULL);
    } else {
        s_back_btn = NULL;
    }
    s_title = lv_label_create(s_header);
    lv_label_set_text(s_title, title);
    lv_obj_set_style_text_color(s_title, lv_color_hex(0xFFFFFF), 0);
    lv_obj_set_style_text_font(s_title, menu_cjk_font_cn(true), 0);
    lv_obj_align(s_title, LV_ALIGN_CENTER, -15, 0);
}

static void back_evt_cb(lv_event_t *e) {
    (void)e;
    if (s_page == PAGE_CARD_DETAIL) nav_to(PAGE_CARDS);
    else nav_to(PAGE_MENU);
}

/* 涓€琛屾枃鏈爣绛撅紙鍐呭鍖虹粷瀵瑰畾浣嶏級 */
static lv_obj_t *make_label(lv_obj_t *parent, const char *text, const lv_font_t *font,
                            lv_color_t color, lv_coord_t x, lv_coord_t y, lv_coord_t w) {
    lv_obj_t *lb = lv_label_create(parent);
    lv_label_set_text(lb, text);
    lv_obj_set_style_text_color(lb, color, 0);
    lv_obj_set_style_text_font(lb, font, 0);
    lv_obj_set_pos(lb, x, y);
    if (w > 0) {
        lv_obj_set_width(lb, w);
        lv_label_set_long_mode(lb, LV_LABEL_LONG_DOT);
    }
    return lb;
}

/* 骞插憳鍚?鍚嶇墖鏂囨湰鐨勫瓧浣撻€夋嫨锛歋D 瀛楀簱浼樺厛锛屾湭鍔犺浇鍥為€€鍐呭祵锛堜粎 ASCII锛?*/
static const lv_font_t *name_font_16(void) {
    const lv_font_t *f = sd_fonts_role(SD_FONT_ROLE_SMALL);
    return f ? f : &role_name_14;
}

/* 菜单/页面中文统一的 SD 字库入口：GB2312 全集；未加载回退内嵌 menu_16（仅有
 * MENU_CJK 几十个字，可能显方块，但字库正常加载后即全覆盖）。大字用 LARGE。
 * 尺寸语义与设计一致：large=原 menu_20 的 20px 级别（SD 只有 16px/30px，取
 * 16px 避免超设计尺寸），false=menu_16 的 16px 级别。 */
static const lv_font_t *menu_cjk_font_cn(bool large) {
    (void)large;
    /* SD 字库只有 30px/LARGE 与 16px/SMALL 两档；30px 用于菜单会远超设计
     * 尺寸（用户原始文案 20px），统一用 16px（SMALL）保持紧凑；回退内嵌与
     * 设计一致：20px 场景回 menu_20，16px 场景回 menu_16。 */
    const lv_font_t *f = sd_fonts_role(SD_FONT_ROLE_SMALL);
    return f ? f : (large ? &menu_20 : &menu_16);
}

/* ============================================================================
 * 涓昏彍鍗曢〉锛? 涓叆鍙? * ==========================================================================*/

typedef struct {
    const char *label;
    menu_page_t page;
} menu_entry_t;

static const menu_entry_t s_menu_entries[] = {
    { "角色选择", PAGE_ROLES },
    { "名片簿",   PAGE_CARDS },
    { "蓝牙",     PAGE_BLE },
    { "UI 选项", PAGE_UIOPTS },
    { "系统信息", PAGE_INFO },
};

static void menu_btn_evt_cb(lv_event_t *e) {
    menu_page_t *pg = (menu_page_t *)lv_event_get_user_data(e);
    if (pg) nav_to(*pg);
}

/* 瑙︽帶鏍″噯鍏ュ彛锛氭竻鎺夋棫鏍″噯骞跺脊鍑哄洓瑙掔偣鍑诲紩瀵?*/
static void cal_btn_evt_cb(lv_event_t *e) {
    (void)e;
    bsp_touch_cal_reset();
    show_cal_overlay();
    ESP_LOGI(TAG, "touch recalibration: reset + guide up");
}

static void build_page_menu(void) {
    lv_obj_set_pos(s_content, 0, 14);
    lv_obj_set_size(s_content, 240, SHEET_H - HEADER_H);
    lv_obj_clean(s_content);

    /* 椤堕儴鎶撴墜鏉★細x=90 璁╄瑙変腑蹇冨榻愬睆涓績锛堢敾甯冨彸绉?~10px 淇锛?*/
    lv_obj_t *handle = lv_obj_create(s_content);
    lv_obj_set_size(handle, 40, 4);
    lv_obj_set_pos(handle, 85, 8);
    lv_obj_set_style_bg_color(handle, lv_color_hex(0xFFFFFF), 0);
    lv_obj_set_style_bg_opa(handle, LV_OPA_30, 0);
    lv_obj_set_style_radius(handle, 2, 0);
    lv_obj_set_style_border_width(handle, 0, 0);
    lv_obj_clear_flag(handle, LV_OBJ_FLAG_SCROLLABLE);

    /* 4 涓叆鍙ｆ寜閽細40px 楂橈紝闂磋窛 6锛屼粠 y=24 璧枫€?     * 瀹?208@x=3锛堝彸缂?211锛夛細鐢诲竷鍙崇Щ ~10px 鍚庡睆 13..221锛屽彸鐣?19px */
    static const int BTN_H = 40;
    static const int GAP = 6;
    int y = 24;
    for (int i = 0; i < (int)(sizeof(s_menu_entries) / sizeof(s_menu_entries[0])); i++) {
        lv_obj_t *btn = lv_btn_create(s_content);
        lv_obj_set_size(btn, 208, BTN_H);
        lv_obj_set_pos(btn, 3, y);
        lv_obj_set_style_bg_color(btn, lv_color_hex(0x23252A), 0);
        lv_obj_set_style_bg_opa(btn, LV_OPA_COVER, 0);
        lv_obj_set_style_radius(btn, 8, 0);
        lv_obj_set_style_border_width(btn, 0, 0);
        lv_obj_set_style_shadow_width(btn, 0, 0);
        lv_obj_set_style_pad_all(btn, 0, 0);
        lv_obj_t *lb = lv_label_create(btn);
        lv_label_set_text(lb, s_menu_entries[i].label);
        lv_obj_set_style_text_color(lb, lv_color_hex(0xFFFFFF), 0);
        lv_obj_set_style_text_font(lb, menu_cjk_font_cn(true), 0);
        lv_obj_center(lb);
        lv_obj_add_event_cb(btn, menu_btn_evt_cb, LV_EVENT_CLICKED,
                            (void *)&s_menu_entries[i].page);
        y += BTN_H + GAP;
    }

    /* 搴曢儴灏忔寜閽細瑙︽帶鏍″噯锛?2px 楂橈紝鍏ㄥ睆闈㈡澘搴曢儴锛?*/
    lv_obj_t *cal_btn = lv_btn_create(s_content);
    lv_obj_set_size(cal_btn, 208, 22);
    lv_obj_set_pos(cal_btn, 3, 286);
    lv_obj_set_style_bg_color(cal_btn, lv_color_hex(0x23252A), 0);
    lv_obj_set_style_bg_opa(cal_btn, LV_OPA_COVER, 0);
    lv_obj_set_style_radius(cal_btn, 6, 0);
    lv_obj_set_style_border_width(cal_btn, 0, 0);
    lv_obj_set_style_shadow_width(cal_btn, 0, 0);
    lv_obj_set_style_pad_all(cal_btn, 0, 0);
    lv_obj_t *cal_lb = lv_label_create(cal_btn);
    lv_label_set_text(cal_lb, "触控校准");
    lv_obj_set_style_text_color(cal_lb, lv_color_hex(0xF5C542), 0);
    lv_obj_set_style_text_font(cal_lb, menu_cjk_font_cn(false), 0);
    lv_obj_center(cal_lb);
    lv_obj_add_event_cb(cal_btn, cal_btn_evt_cb, LV_EVENT_CLICKED, NULL);
}

/* ============================================================================
 * 角色选择页：头像 + 名字列表
 * ==========================================================================*/

/* 鍒楄〃琛岀偣鍑?婊戝姩闂ㄦ帶锛歀VGL 婊氬姩鏈敓鏁堬紙鎴栨湭瑙﹀彂 throw锛夋椂锛屾粦鍔ㄦ澗鎵? * 涔熶細鍙?CLICKED 璇繘璇︽儏鈥斺€擯RESSED 璁板綍璧风偣锛孋LICKED 鏃朵綅绉昏秴闃堝€? * 锛?0px锛夊垽瀹氫负婊戝姩锛屽拷鐣ユ湰娆＄偣鍑?*/
static lv_coord_t s_row_press_y = 0;
static bool      s_row_press_valid = false;

static void row_press_cb(lv_event_t *e) {
    (void)e;
    lv_indev_t *indev = lv_indev_get_act();
    if (indev != NULL) {
        lv_point_t p;
        lv_indev_get_point(indev, &p);
        s_row_press_y = p.y;
        s_row_press_valid = true;
    }
}

static bool row_swipe_check(void) {
    if (!s_row_press_valid) return false;
    s_row_press_valid = false;
    lv_indev_t *indev = lv_indev_get_act();
    if (indev == NULL) return false;
    lv_point_t p;
    lv_indev_get_point(indev, &p);
    return abs(p.y - s_row_press_y) > 20;
}

static void role_row_evt_cb(lv_event_t *e) {
    int idx = (int)(intptr_t)lv_event_get_user_data(e);
    if (row_swipe_check()) return;
    if (idx < 0 || idx >= s_role_count) return;
    const char *name = s_role_entries[idx].name;
    esp_err_t err = roles_set_current(name);
    if (err == ESP_OK) {
        ESP_LOGI(TAG, "鍒囨崲骞插憳: %s，请求热重载", name);
        mjpeg_player_request_reload();
        close_menu();
    } else {
        ESP_LOGW(TAG, "切换角色失败: %s:%s", name, esp_err_to_name(err));
    }
}

static void build_page_roles(void) {
    lv_obj_set_pos(s_content, 0, 29);
    lv_obj_set_size(s_content, 240, SHEET_H - HEADER_H);
    lv_obj_clean(s_content);

    s_role_count = roles_list_all(s_role_entries, MAX_ROLES);
    if (s_role_count <= 0) {
        lv_obj_t *lb = make_label(s_content, "无角色", menu_cjk_font_cn(true),
                                  lv_color_hex(0x9AA0A6), 0, 60, 240);
        lv_obj_set_style_text_align(lb, LV_TEXT_ALIGN_CENTER, 0);
        return;
    }

    /* 棰勮鍏ㄩ儴 portrait锛圥SRAM锛岄〉鍏抽噴鏀撅級 */
    s_role_portraits = heap_caps_malloc((size_t)s_role_count * sizeof(uint8_t *),
                                      MALLOC_CAP_SPIRAM | MALLOC_CAP_8BIT);
    if (s_role_portraits) memset(s_role_portraits, 0, (size_t)s_role_count * sizeof(uint8_t *));
    for (int i = 0; i < s_role_count && s_role_portraits; i++) {
        if (!s_role_entries[i].has_portrait) continue;
        s_role_portraits[i] = NULL;
        char path[ROLE_PATH_MAX];
        snprintf(path, sizeof(path), "%s/%.*s/portrait.bin",
                 BSP_SDCARD_MOUNT_POINT, (int)ROLE_NAME_MAX, s_role_entries[i].name);
        size_t psz = 0;
        uint8_t *pbuf = bsp_sdcard_load_file(path, &psz);
        if (pbuf && psz == 8192) {
            swap565_inplace(pbuf, psz);   /* LE 鈫?LVGL SWAP锛堢嫭绔嬪壇鏈紝瀹夊叏锛?*/
            s_role_portraits[i] = pbuf;
        } else {
            if (pbuf) heap_caps_free(pbuf);
        }
    }

    /* 鍙粴鍔ㄥ垪琛紙鎵嬪姩鎷栧姩婊氬姩锛岃 s_list 鐘舵€佽鏄庘€斺€旂 LVGL 婊氬姩锛?*/
    lv_obj_t *list = lv_obj_create(s_content);
    s_list = list;
    lv_obj_set_pos(list, -10, 0);
    lv_obj_set_size(list, 240, SHEET_H - HEADER_H);
    lv_obj_clear_flag(list, LV_OBJ_FLAG_SCROLLABLE);
    lv_obj_set_style_bg_color(list, lv_color_hex(0x000000), 0);
    lv_obj_set_style_bg_opa(list, LV_OPA_0, 0);
    lv_obj_set_style_border_width(list, 0, 0);
    lv_obj_set_style_pad_all(list, 0, 0);
    lv_obj_set_style_radius(list, 0, 0);
    lv_obj_clear_flag(list, LV_OBJ_FLAG_SCROLL_CHAIN);

    const char *cur = roles_current_name();
    for (int i = 0; i < s_role_count; i++) {
        lv_obj_t *row = lv_obj_create(list);
        lv_obj_set_size(row, 230, 64);
        lv_obj_set_pos(row, 0, i * 64);
        lv_obj_set_style_bg_color(row, lv_color_hex(0x23252A), 0);
        lv_obj_set_style_bg_opa(row, LV_OPA_60, 0);
        lv_obj_set_style_radius(row, 6, 0);
        lv_obj_set_style_border_width(row, 0, 0);
        lv_obj_clear_flag(row, LV_OBJ_FLAG_SCROLLABLE);
        lv_obj_clear_flag(row, LV_OBJ_FLAG_SCROLL_CHAIN);

        /* 澶村儚 40x40锛坧ortrait.bin 64x64 鈫?zoom 160锛?*/
        if (s_role_portraits && s_role_portraits[i]) {
            static lv_img_dsc_t dsc[MAX_ROLES];
            lv_img_dsc_t *d = &dsc[i];
            d->header.always_zero = 0;
            d->header.w = 64;
            d->header.h = 64;
            d->header.cf = LV_IMG_CF_TRUE_COLOR;
            d->data_size = 8192;
            d->data = s_role_portraits[i];
            lv_obj_t *img = lv_img_create(row);
            lv_img_set_src(img, d);
            lv_img_set_zoom(img, 160);   /* 64 鈫?40px */
            lv_obj_align(img, LV_ALIGN_LEFT_MID, -6, 0);
        } else {
            static lv_img_dsc_t dflt_dsc[MAX_ROLES];
            lv_img_dsc_t *d = &dflt_dsc[i];
            d->header.always_zero = 0;
            d->header.w = 64;
            d->header.h = 64;
            d->header.cf = LV_IMG_CF_TRUE_COLOR;
            d->data_size = sizeof(default_avatar_565);
            d->data = (const uint8_t *)default_avatar_565;
            lv_obj_t *img = lv_img_create(row);
            lv_img_set_src(img, d);
            lv_img_set_zoom(img, 160);
            lv_obj_align(img, LV_ALIGN_LEFT_MID, -6, 0);
        }

        lv_obj_t *lb = lv_label_create(row);
        char disp[ROLE_NAME_MAX * 2];
        role_text_display(s_role_entries[i].name, disp, sizeof(disp));
        lv_label_set_text(lb, disp);
        lv_obj_set_style_text_color(lb, lv_color_hex(0xFFFFFF), 0);
        lv_obj_set_style_text_font(lb, name_font_16(), 0);
        lv_obj_set_width(lb, 168);
        lv_label_set_long_mode(lb, LV_LABEL_LONG_WRAP);
        lv_obj_set_style_text_line_space(lb, 0, 0);
        lv_obj_align(lb, LV_ALIGN_LEFT_MID, 62, 0);

        if (cur && strcmp(cur, s_role_entries[i].name) == 0) {
            lv_obj_t *badge = lv_label_create(row);
            lv_label_set_text(badge, "当前");
            lv_obj_set_style_text_color(badge, lv_color_hex(0xF5C542), 0);
            lv_obj_set_style_text_font(badge, menu_cjk_font_cn(false), 0);
            lv_obj_align(badge, LV_ALIGN_RIGHT_MID, -10, 0);
        }

        lv_obj_add_event_cb(row, role_row_evt_cb, LV_EVENT_CLICKED,
                            (void *)(intptr_t)i);
        lv_obj_add_event_cb(row, row_press_cb, LV_EVENT_PRESSED, NULL);
    }
}

/* ============================================================================
 * 鍚嶇墖绨匡細鍒楄〃椤? * ==========================================================================*/

static void format_time(char *out, size_t cap, uint32_t s) {
    if (s >= 1000000000UL) {
        /* 澧欓挓 unix 绉?鈫?鏈湴鏃堕棿锛堝姞 tz 鍋忕Щ锛夛紝鏄剧ず MM-DD HH:MM */
        uint32_t loc = s + (uint32_t)time_sync_get_tz() * 60UL;
        uint32_t days = loc / 86400UL;           /* 鑷?1970-01-01 鐨勫ぉ鏁?*/
        uint32_t rem = loc % 86400UL;
        uint32_t hh = rem / 3600UL, mm = (rem / 60UL) % 60UL;
        /* unix 澶╂暟 鈫?骞存湀鏃ワ紙鏍奸浄鎴堥噷鍘嗭紝1970-2100 鑼冨洿锛屾棤 400 骞存牎姝ｇ増锛?*/
        uint32_t y = 1970;
        while (days >= 365UL) {
            bool leap = (y % 4 == 0 && (y % 100 != 0 || y % 400 == 0));
            uint32_t nd = leap ? 366 : 365;
            if (days < nd) break;
            days -= nd;
            y++;
        }
        uint32_t m = 1, d = days + 1;
        for (uint32_t i = 1; i <= 12; i++) {
            uint32_t dim = 31;
            if (i == 4 || i == 6 || i == 9 || i == 11) dim = 30;
            else if (i == 2) dim = (y % 4 == 0 && (y % 100 != 0 || y % 400 == 0)) ? 29 : 28;
            if (d <= dim) { m = i; break; }
            d -= dim;
        }
        snprintf(out, cap, "%04u-%02u-%02u %02u:%02u", (unsigned)y, (unsigned)m,
                 (unsigned)d, (unsigned)hh, (unsigned)mm);
    } else {
        /* 鏈牎鍑嗭紙RTC 鐩稿绉掞紝涓婄數璧风畻锛夛細鏄剧ず "上电后HH:MM:SS" 鏄庣ず闈炲閽?*/
        snprintf(out, cap, "上电后%02u:%02u:%02u",
                 (unsigned)((s / 3600) % 100), (unsigned)((s / 60) % 60), (unsigned)(s % 60));
    }
}

static void card_row_evt_cb(lv_event_t *e) {
    int idx = (int)(intptr_t)lv_event_get_user_data(e);
    if (row_swipe_check()) return;
    if (idx < 0 || idx >= s_card_count) return;
    s_cur_card = s_card_entries[idx];
    nav_to(PAGE_CARD_DETAIL);
}

static void build_page_cards(void) {
    lv_obj_set_pos(s_content, 0, 29);
    lv_obj_set_size(s_content, 240, SHEET_H - HEADER_H);
    lv_obj_clean(s_content);

    s_card_count = card_book_load_list(s_card_entries, CARD_BOOK_MAX_LIST);
    if (s_card_count <= 0) {
        lv_obj_t *lb = make_label(s_content, "暂无记录", menu_cjk_font_cn(true),
                                  lv_color_hex(0x9AA0A6), 0, 60, 0);
        lv_obj_align(lb, LV_ALIGN_CENTER, 0, 0);   /* 水平居中（相对内容 240 宽） */
        lv_obj_set_y(lb, 60);                       /* 垂直保持 60 */
        (void)lb;
        return;
    }

    lv_obj_t *list = lv_obj_create(s_content);
    s_list = list;
    lv_obj_set_pos(list, -15, 0);
    lv_obj_set_size(list, 240, SHEET_H - HEADER_H);
    lv_obj_clear_flag(list, LV_OBJ_FLAG_SCROLLABLE);
    lv_obj_set_style_bg_color(list, lv_color_hex(0x000000), 0);
    lv_obj_set_style_bg_opa(list, LV_OPA_0, 0);
    lv_obj_set_style_border_width(list, 0, 0);
    lv_obj_set_style_pad_all(list, 0, 0);
    lv_obj_set_style_radius(list, 0, 0);
    lv_obj_clear_flag(list, LV_OBJ_FLAG_SCROLL_CHAIN);

    char time_buf[32];
    for (int i = 0; i < s_card_count; i++) {
        lv_obj_t *row = lv_obj_create(list);
        lv_obj_set_size(row, 230, 64);
        lv_obj_set_pos(row, 0, i * 64);
        lv_obj_set_style_bg_color(row, lv_color_hex(0x23252A), 0);
        lv_obj_set_style_bg_opa(row, LV_OPA_60, 0);
        lv_obj_set_style_radius(row, 6, 0);
        lv_obj_set_style_border_width(row, 0, 0);
        lv_obj_clear_flag(row, LV_OBJ_FLAG_SCROLLABLE);
        lv_obj_clear_flag(row, LV_OBJ_FLAG_SCROLL_CHAIN);

        /* 琛?1锛氱敤鎴稩D + uid锛堟棤 uid 鏄剧ず name/uname 鍏滃簳锛?*/
        char line1[64];
        const char *disp = s_card_entries[i].uid[0] ? s_card_entries[i].uid
                        : (s_card_entries[i].uname[0] ? s_card_entries[i].uname
                                                      : s_card_entries[i].name);
        snprintf(line1, sizeof(line1), "用户ID %s", disp);
        lv_obj_t *l1 = lv_label_create(row);
        lv_label_set_text(l1, line1);
        lv_obj_set_style_text_color(l1, lv_color_hex(0xFFFFFF), 0);
        lv_obj_set_style_text_font(l1, menu_cjk_font_cn(false), 0);
        lv_obj_set_width(l1, 214);
        lv_label_set_long_mode(l1, LV_LABEL_LONG_DOT);
        lv_obj_set_pos(l1, 8, 12);

        /* 琛?2锛?搴忓彿  鏃堕棿锛堝€掑簭鏄剧ず鏈€鏂板湪鍓嶏級 */
        format_time(time_buf, sizeof(time_buf), s_card_entries[i].up_s);
        char line2[48];
        snprintf(line2, sizeof(line2), "#%04u  %s  x%u", s_card_entries[i].seq, time_buf, s_card_entries[i].cnt);
        lv_obj_t *l2 = lv_label_create(row);
        lv_label_set_text(l2, line2);
        lv_obj_set_style_text_color(l2, lv_color_hex(0x9AA0A6), 0);
        lv_obj_set_style_text_font(l2, &uid_12, 0);   /* 12px 全ASCII：避免长序号/时间超界 */
        lv_obj_set_width(l2, 214);
        lv_label_set_long_mode(l2, LV_LABEL_LONG_DOT);
        lv_obj_set_pos(l2, 8, 38);

        lv_obj_add_event_cb(row, card_row_evt_cb, LV_EVENT_CLICKED,
                            (void *)(intptr_t)i);
        lv_obj_add_event_cb(row, row_press_cb, LV_EVENT_PRESSED, NULL);
    }
}

/* ============================================================================
 * 鍚嶇墖绨匡細璇︽儏椤碉紙澶уご鍍?+ 瀛楁锛? * ==========================================================================*/

static void build_page_card_detail(void) {
    lv_obj_set_pos(s_content, 0, 29);
    lv_obj_set_size(s_content, 240, SHEET_H - HEADER_H);
    lv_obj_clean(s_content);

    size_t len = 0;
    s_card_blob = card_book_load_blob(s_cur_card.mac, &len);
    /* blob 缂哄け/鎹熷潖锛堝鏃х増 0B 鍧忔枃浠?鏈啓鎴愬姛锛夆啋 涓嶆樉绀?鎵撳紑澶辫触"锛?     * 鐢ㄥ垪琛ㄨ瀛楁锛坲name/uid/name/uid锛夋瀯閫犺鍥撅紝澶村儚鐢ㄩ粯璁ゅ崰浣嶏細
     * 璇︽儏椤垫案杩滃彲鎵撳紑锛屽瓧娈垫潵鑷?cards.txt 琛岋紙s_cur_card锛?*/
    static card_blob_hdr_t s_fallback_hdr;
    uint8_t *fallback_payload = NULL;
    if (s_card_blob == NULL || len < 8) {
        if (s_card_blob != NULL) {
            heap_caps_free(s_card_blob);
            s_card_blob = NULL;
        }
        memset(&s_fallback_hdr, 0, sizeof(s_fallback_hdr));
        s_fallback_hdr.magic = CARD_BLOB_MAGIC;
        s_fallback_hdr.ver   = CARD_BLOB_VER;
        snprintf((char *)s_fallback_hdr.name, sizeof(s_fallback_hdr.name), "%.*s",
                 (int)sizeof(s_fallback_hdr.name) - 1, s_cur_card.name);
        s_fallback_hdr.name_len = (uint8_t)strnlen(s_fallback_hdr.name, CARD_NAME_MAX);
        snprintf((char *)s_fallback_hdr.uid, sizeof(s_fallback_hdr.uid), "%.*s",
                 (int)sizeof(s_fallback_hdr.uid) - 1, s_cur_card.uid);
        s_fallback_hdr.uid_len = (uint8_t)strnlen(s_fallback_hdr.uid, CARD_UID_MAX);
        snprintf((char *)s_fallback_hdr.uname, sizeof(s_fallback_hdr.uname), "%.*s",
                 (int)sizeof(s_fallback_hdr.uname) - 1, s_cur_card.uname);
        s_fallback_hdr.uname_len = (uint8_t)strnlen(s_fallback_hdr.uname, CARD_UNAME_MAX);
        s_fallback_hdr.avatar_len = CARD_AVATAR_SIZE;
        s_fallback_hdr.ts = s_cur_card.up_s;
        fallback_payload = (uint8_t *)default_avatar_565;
        s_fallback_blob = true;
        s_card_blob = (uint8_t *)&s_fallback_hdr;
        len = sizeof(s_fallback_hdr);
    } else {
        s_fallback_blob = false;
    }
    s_card_blob_len = len;

    card_blob_view_t view;
    if (fallback_payload != NULL) {
        memset(&view, 0, sizeof(view));
        view.hdr = (const card_blob_hdr_t *)s_card_blob;
        view.avatar = fallback_payload;          /* 榛樿鑹插潡澶村儚锛堜氦鎹㈡椂鍚屾锛?*/
        view.sticker_cnt = 0;
    } else {
        card_book_parse_blob(s_card_blob, len, &view);
    }

    /* 澶уご鍍忥細64x64锛?4x64 鈫?zoom 256锛夈€備紭鍏堢骇 鐢ㄦ埛澶村儚 > 骞插憳澶村浘 > 鍗犱綅
     * 鏁版嵁婧愶紙blob 缂撳瓨/榛樿澶村儚锛夊彧璇?鈫?澶嶅埗鍒扮嫭绔嬬紦鍐插啀 swap 瀛楄妭搴?*/
    static lv_img_dsc_t dsc;
    static uint8_t *s_detail_av = NULL;   /* PSRAM 甯搁┗ 8KB 鍓湰 */
    const uint8_t *av = view.avatar ? view.avatar : (view.portrait ? view.portrait : NULL);
    if (av == NULL) av = (const uint8_t *)default_avatar_565;
    if (s_detail_av == NULL) {
        s_detail_av = heap_caps_malloc(CARD_AVATAR_SIZE, MALLOC_CAP_SPIRAM | MALLOC_CAP_8BIT);
    }
    if (s_detail_av != NULL) {
        memcpy(s_detail_av, av, CARD_AVATAR_SIZE);
        swap565_inplace(s_detail_av, CARD_AVATAR_SIZE);
        av = s_detail_av;
    }
    dsc.header.always_zero = 0;
    dsc.header.w = 64;
    dsc.header.h = 64;
    dsc.header.cf = LV_IMG_CF_TRUE_COLOR;
    dsc.data_size = CARD_AVATAR_SIZE;
    dsc.data = av;
    lv_obj_t *img = lv_img_create(s_content);
    lv_img_set_src(img, &dsc);
    lv_img_set_zoom(img, 256);
    lv_obj_set_pos(img, 8, 24);

    /* 鍙冲垪锛氱敤鎴峰悕锛堝ぇ锛? 鏁板瓧ID / 骞插憳鍚嶏紙缁熶竴灏忓瓧鍙凤紝椤哄簭 uname鈫抲id鈫抧ame锛?     * SD 瀛楀簱鍔犺浇鏃剁敤 SD_FONT_ROLE_LARGE锛圙B2312 鍏ㄩ泦锛屼腑鏂囨甯革級 */
    const card_blob_hdr_t *hdr = view.hdr;
    const lv_font_t *sd_small = sd_fonts_loaded() ? sd_fonts_role(SD_FONT_ROLE_SMALL) : NULL;
    char u_buf[CARD_UNAME_MAX + 1] = "";
    if (hdr && hdr->uname_len > 0) {
        snprintf(u_buf, sizeof(u_buf), "%.*s",
                 (int)(hdr->uname_len <= CARD_UNAME_MAX ? hdr->uname_len : CARD_UNAME_MAX),
                 hdr->uname);
    }
    lv_obj_t *u_lb = lv_label_create(s_content);
    lv_label_set_text(u_lb, u_buf[0] ? u_buf : "--");
    lv_obj_set_style_text_color(u_lb, lv_color_hex(0xFFFFFF), 0);
    /* 缁熶竴灏忓瓧鍙凤紙涓?ID/骞插憳鍚嶄竴鑷达級锛歴d_small锛圙B2312 鍏ㄩ泦涓枃锛夋垨 menu_16 */
    lv_obj_set_style_text_font(u_lb, sd_small ? sd_small : &menu_16, 0);
    lv_obj_set_width(u_lb, 120);
    /* 鐢ㄦ埛鍚嶅厑璁告崲琛岋紙LONG_WRAP锛夛細闀夸腑鏂囧悕鎶樿鍚庡叾浣欓」鑷姩涓嬬Щ銆?*/
    lv_label_set_long_mode(u_lb, LV_LABEL_LONG_WRAP);
    lv_obj_set_pos(u_lb, 102, 24);
    /* 娴嬮噺瀹為檯楂樺害锛歶name 涓€琛?17px锛涙崲琛屽悗澶氳锛寀id/name 鎸?uname
     * 瀹為檯楂樺害鍚戜笅璁╀綅锛堜笌澶村儚鍚岄《瀵归綈锛屼繚鎸佽璺濆潎鍖€锛夈€?*/
    lv_obj_update_layout(u_lb);
    lv_coord_t u_h = lv_obj_get_height(u_lb);
    if (u_h < 16) u_h = 16;
    lv_coord_t row1_y = 24 + u_h + 6;          /* 鐢ㄦ埛鍗?ID 琛岋紙鍙充笂鍖哄煙锛屼繚鐣欙級 */
    lv_coord_t row2_y = row1_y + 20;           /* 鏃?name 琛岋紙涓嶅啀浣跨敤锛?*/
    (void)row2_y;

    /* 鍙充笂鐢ㄦ埛鍗?ID锛堥粍鑹插皬瀛楋紝涓庣敤鎴峰悕鍚屽垪锛夆€斺€旇鎯呬繚鐣?*/
    char uid_line[32];
    snprintf(uid_line, sizeof(uid_line), "ID %s", s_cur_card.uid[0] ? s_cur_card.uid : "-");
    make_label(s_content, uid_line, sd_small ? sd_small : &menu_16,
               lv_color_hex(0xF5C542), 102, row1_y, 120);

    /* 骞插憳鍚嶏紙浜ゆ崲淇℃伅鍖猴級鎸夎鑹插垎缁勶細澶氭浜ゆ崲鎹㈣繃瑙掕壊鍒欏垎琛?*/
    char role_names[8][CARD_NAME_MAX + 1];
    uint16_t role_cnt[8];
    int role_n = card_book_load_name_counts(s_cur_card.mac, role_names, role_cnt, 8);

    /* 绗竴琛岋紙鍏冧俊鎭級锛?搴忓彿  鍏蜂綋鏃堕棿  x浜や簰娆℃暟锛堜繚鐣欙級 */
    char m_buf[CARD_MOTTO_MAX + 1] = "";
    if (hdr && hdr->motto_len > 0) {
        snprintf(m_buf, sizeof(m_buf), "%.*s",
                 (int)(hdr->motto_len <= CARD_MOTTO_MAX ? hdr->motto_len : CARD_MOTTO_MAX),
                 hdr->motto);
    }
    make_label(s_content, m_buf[0] ? m_buf : " ", sd_small ? sd_small : &menu_16,
               lv_color_hex(0xFFFFFF), 8, 90, 224);
    char time_buf[32];
    format_time(time_buf, sizeof(time_buf), s_cur_card.up_s);
    char meta[64];
    snprintf(meta, sizeof(meta), "#%04u  %s", s_cur_card.seq, time_buf);
    make_label(s_content, meta, &menu_16, lv_color_hex(0x9AA0A6), 8, 108, 224);

    /* 绗簩琛岋細骞插憳鍚嶏紙鏈€鏂拌鑹诧級鈥斺€旂揣鎸ㄧ涓€琛岋紙y=112锛屾棤绌鸿锛夈€?     * 澶氳鑹诧紙浜ゆ崲闂村垏鎹㈣繃锛夋椂閫愯鏄剧ず锛屾渶澶?3 琛岋紙鏈€鏂板湪鍓嶏級銆?*/
    const lv_font_t *role_font = sd_small ? sd_small : &menu_16;
    char line2[128];
    if (role_n <= 1) {
        char nb[CARD_NAME_MAX + 1] = "";
        if (hdr && hdr->name_len > 0) {
            snprintf(nb, sizeof(nb), "%.*s",
                     (int)(hdr->name_len <= CARD_NAME_MAX ? hdr->name_len : CARD_NAME_MAX),
                     hdr->name);
        }
        /* 琛?锛氳鑹插悕锛堢揣鎸ㄧ涓€琛岋紝鐧借壊閱掔洰锛?*/
        snprintf(line2, sizeof(line2), "%.*s x%u", CARD_NAME_MAX,
                 role_n == 1 ? (role_names[0][0] ? role_names[0] : nb)
                             : (nb[0] ? nb : "--"), s_cur_card.cnt);
        make_label(s_content, line2, role_font, lv_color_hex(0xFFFFFF), 8, 126, 224);
    } else {
        /* 琛?锛氭渶鏂拌鑹诧紙鐧借壊锛?*/
        snprintf(line2, sizeof(line2), "%.*s x%u", CARD_NAME_MAX,
                 role_names[0][0] ? role_names[0] : "--", s_cur_card.cnt);
        make_label(s_content, line2, role_font, lv_color_hex(0xFFFFFF), 8, 126, 224);
        /* 行3+：历史角色（灰色，全部展开，无"更多角色"截断） */
        int rows = role_n - 1;
        if (rows > 3) rows = 3;
        for (int r = 0; r < rows; r++) {
            int ri = r + 1;
            snprintf(line2, sizeof(line2), "%.*s", CARD_NAME_MAX,
                     role_names[ri][0] ? role_names[ri] : "--");
            make_label(s_content, line2, role_font, lv_color_hex(0x9AA0A6), 8, 144 + r * 18, 224);
        }
    }

    /* 琛ㄦ儏鍖呰锛堚墹3 寮狅紝40x40锛?*/
    for (int i = 0; i < view.sticker_cnt && i < 3; i++) {
        static lv_img_dsc_t stk_dsc[3];
        lv_img_dsc_t *d = &stk_dsc[i];
        d->header.always_zero = 0;
        d->header.w = 64;
        d->header.h = 64;
        d->header.cf = LV_IMG_CF_TRUE_COLOR;
        d->data_size = CARD_AVATAR_SIZE;
        d->data = view.stickers[i];
        lv_obj_t *s_img = lv_img_create(s_content);
        lv_img_set_src(s_img, d);
        lv_img_set_zoom(s_img, 160);
        lv_obj_set_pos(s_img, 8 + i * 44, 190);
    }
}

/* ============================================================================
 * 钃濈墮椤? * ==========================================================================*/

static void ble_adv_evt_cb(lv_event_t *e) {
    (void)e;
    ble_gatt_set_adv(!ble_gatt_adv_enabled());
}

static void build_page_ble(void) {
    lv_obj_set_pos(s_content, 0, 29);
    lv_obj_set_size(s_content, 240, SHEET_H - HEADER_H);
    lv_obj_clean(s_content);

    /* 鐘舵€?*/
    s_lbl_ble_status = make_label(s_content, "状态: 未连接", menu_cjk_font_cn(true),
                                  lv_color_hex(0xFFFFFF), 0, 16, 208);
    /* 璁惧鍚?*/
    char name_line[64];
    snprintf(name_line, sizeof(name_line), "设备名: %s", ble_gatt_dev_name());
    make_label(s_content, name_line, menu_cjk_font_cn(false), lv_color_hex(0x9AA0A6), 0, 52, 208);

    /* 骞挎挱寮€鍏?*/
    s_lbl_ble_adv = make_label(s_content,
                               ble_gatt_adv_enabled() ? "广播: 开" : "广播: 关",
                               menu_cjk_font_cn(false), lv_color_hex(0xFFFFFF), 0, 84, 120);
    lv_obj_t *adv_btn = lv_btn_create(s_content);
    lv_obj_set_size(adv_btn, 72, 32);
    lv_obj_set_pos(adv_btn, 140, 78);
    lv_obj_set_style_bg_color(adv_btn, lv_color_hex(0x23252A), 0);
    lv_obj_set_style_radius(adv_btn, 6, 0);
    lv_obj_set_style_border_width(adv_btn, 0, 0);
    lv_obj_set_style_shadow_width(adv_btn, 0, 0);
    lv_obj_set_style_pad_all(adv_btn, 0, 0);
    lv_obj_t *adv_lb = lv_label_create(adv_btn);
    lv_label_set_text(adv_lb, "切换");
    lv_obj_set_style_text_color(adv_lb, lv_color_hex(0xFFFFFF), 0);
    lv_obj_set_style_text_font(adv_lb, menu_cjk_font_cn(false), 0);
    lv_obj_center(adv_lb);
    lv_obj_add_event_cb(adv_btn, ble_adv_evt_cb, LV_EVENT_CLICKED, NULL);

    /* 涓婁紶杩涘害锛堥粯璁ら殣钘忥紝涓婁紶涓樉绀猴級 */
    s_lbl_ble_upload = make_label(s_content, "", menu_cjk_font_cn(false), lv_color_hex(0xF5C542),
                                  0, 122, 208);
    lv_obj_add_flag(s_lbl_ble_upload, LV_OBJ_FLAG_HIDDEN);

    make_label(s_content, "手机/电脑搜索 PASS-II 连接本机", menu_cjk_font_cn(false),
               lv_color_hex(0x5F6368), 0, 152, 208);
}

/* 钃濈墮椤电姸鎬佸埛鏂帮紙鎵嬪娍瀹氭椂鍣?500ms 鑺傛媿璋冪敤锛涗粎椤靛彲瑙佹椂鏈夋晥锛?*/
static void refresh_ble_page(void) {
    ESP_LOGD(TAG, "ble refr");
    if (s_lbl_ble_status == NULL) return;
    char buf[64];
    const char *st = ble_gatt_is_connected() ? "已连接" : "未连接";
    snprintf(buf, sizeof(buf), "状态: %s", st);
    lv_label_set_text(s_lbl_ble_status, buf);
    lv_obj_set_style_text_color(s_lbl_ble_status,
        ble_gatt_is_connected() ? lv_color_hex(0x00E676) : lv_color_hex(0xFFFFFF), 0);

            snprintf(buf, sizeof(buf), "广播: %s", ble_gatt_adv_enabled() ? "开" : "关");
    lv_label_set_text(s_lbl_ble_adv, buf);

    int pct = ble_gatt_upload_progress();
    if (pct >= 0) {
        snprintf(buf, sizeof(buf), "上传中: %d%%", pct);
        lv_label_set_text(s_lbl_ble_upload, buf);
        lv_obj_clear_flag(s_lbl_ble_upload, LV_OBJ_FLAG_HIDDEN);
    } else if (!lv_obj_has_flag(s_lbl_ble_upload, LV_OBJ_FLAG_HIDDEN)) {
        lv_obj_add_flag(s_lbl_ble_upload, LV_OBJ_FLAG_HIDDEN);
    }
}

/* ============================================================================
 * 绯荤粺淇℃伅椤? * ==========================================================================*/

/* ============================================================================
 * UI 选项页：逐项开关 leftcol/角色名/副标题/横幅/中央提示（NVS 持久化；
 * 切换即时生效——显隐逻辑见 ui_core_screen_apply_show_opts）
 * ==========================================================================*/

typedef struct {
    const char *name;
    uiopt_bit_t bit;
} uiopts_row_t;

static const uiopts_row_t s_uiopt_rows[] = {
    { "立牌文字", UIOPT_LEFTCOL  },
    { "角色名",   UIOPT_OP_NAME  },
    { "副标题",   UIOPT_SUB_TEXT },
    { "名片横幅", UIOPT_BANNER   },
    { "中央提示", UIOPT_HINT     },
};
#define UIOPT_ROWS (int)(sizeof(s_uiopt_rows) / sizeof(s_uiopt_rows[0]))

/* 刷新全部开关行文案（toggle 后调用） */
static void uiopts_refresh_labels(void) {
    uint8_t mask = ui_options_get();
    for (int i = 0; i < UIOPT_ROWS; i++) {
        bool on = (mask & (uint8_t)s_uiopt_rows[i].bit) != 0;
        lv_label_set_text(s_lbl_uiopt[i], on ? "开" : "关");
        lv_obj_set_style_text_color(s_lbl_uiopt[i],
            on ? lv_color_hex(0x00E676) : lv_color_hex(0x9AA0A6), 0);
    }
}

static void uiopts_evt_cb(lv_event_t *e) {
    const uiopts_row_t *row = (const uiopts_row_t *)lv_event_get_user_data(e);
    if (row == NULL) return;
    ui_options_set(ui_options_get() ^ (uint8_t)row->bit);
    uiopts_refresh_labels();
    /* 开关即时生效：关闭立即隐藏；开启且已揭示、有文本则恢复 */
    ui_core_screen_apply_show_opts();
    ESP_LOGI(TAG, "ui option %s -> %s", row->name,
             (ui_options_get() & (uint8_t)row->bit) ? "on" : "off");
}

static void build_page_uiopts(void) {
    lv_obj_set_pos(s_content, 0, 29);
    lv_obj_set_size(s_content, 240, SHEET_H - HEADER_H);
    lv_obj_clean(s_content);

    uint8_t mask = ui_options_get();
    for (int i = 0; i < UIOPT_ROWS; i++) {
        int yy = 12 + i * 38;
        make_label(s_content, s_uiopt_rows[i].name, menu_cjk_font_cn(false),
                   lv_color_hex(0xFFFFFF), 8, yy + 8, 120);
        lv_obj_t *btn = lv_btn_create(s_content);
        lv_obj_set_size(btn, 72, 32);
        lv_obj_set_pos(btn, 148, yy);
        lv_obj_set_style_bg_color(btn, lv_color_hex(0x23252A), 0);
        lv_obj_set_style_radius(btn, 6, 0);
        lv_obj_set_style_border_width(btn, 0, 0);
        lv_obj_set_style_shadow_width(btn, 0, 0);
        lv_obj_set_style_pad_all(btn, 0, 0);
        bool on = (mask & (uint8_t)s_uiopt_rows[i].bit) != 0;
        s_lbl_uiopt[i] = lv_label_create(btn);
        lv_label_set_text(s_lbl_uiopt[i], on ? "开" : "关");
        lv_obj_set_style_text_color(s_lbl_uiopt[i],
            on ? lv_color_hex(0x00E676) : lv_color_hex(0x9AA0A6), 0);
        lv_obj_set_style_text_font(s_lbl_uiopt[i], menu_cjk_font_cn(false), 0);
        lv_obj_center(s_lbl_uiopt[i]);
        lv_obj_add_event_cb(btn, uiopts_evt_cb, LV_EVENT_CLICKED,
                            (void *)&s_uiopt_rows[i]);
    }
    make_label(s_content, "关闭后即时隐藏；全部关闭 = 纯视频画面",
               menu_cjk_font_cn(false), lv_color_hex(0x5F6368), 8, 222, 224);
}

static const char *log_level_text(esp_log_level_t lv) {
    switch (lv) {
        case ESP_LOG_ERROR:   return "错误";
        case ESP_LOG_WARN:    return "警告";
        case ESP_LOG_INFO:    return "信息";
        case ESP_LOG_DEBUG:   return "调试";
        case ESP_LOG_VERBOSE: return "调试";
        default:              return "调试";
    }
}

static void loglvl_evt_cb(lv_event_t *e) {
    (void)e;
    esp_log_level_t lv = esp_log_level_get("*");
    esp_log_level_t next;
    if (lv == ESP_LOG_WARN) next = ESP_LOG_INFO;
    else if (lv == ESP_LOG_INFO) next = ESP_LOG_DEBUG;
    else next = ESP_LOG_WARN;
    esp_log_level_set("*", next);
    ESP_LOGI(TAG, "log level -> %d", (int)next);
    if (s_lbl_loglvl) {
        char buf[48];
        snprintf(buf, sizeof(buf), "日志级别 %s", log_level_text(next));
        lv_label_set_text(s_lbl_loglvl, buf);
    }
}

static void build_page_info(void) {
    lv_obj_set_pos(s_content, 0, 29);
    lv_obj_set_size(s_content, 240, SHEET_H - HEADER_H);
    lv_obj_clean(s_content);

    /* 版本 / IDF */
    char line[128];
    snprintf(line, sizeof(line), "版本 %s", FW_VERSION);
    make_label(s_content, line, menu_cjk_font_cn(false), lv_color_hex(0xFFFFFF), 0, 6, 208);

    snprintf(line, sizeof(line), "IDF %s", esp_get_idf_version());
    make_label(s_content, line, menu_cjk_font_cn(false), lv_color_hex(0x9AA0A6), 0, 30, 208);

    /* 杩愯鏃堕棿锛?s 鍒锋柊锛?*/
    s_lbl_uptime = make_label(s_content, "", menu_cjk_font_cn(false), lv_color_hex(0x9AA0A6),
                              0, 54, 208);

    /* SD 鐘舵€侊紙1s 鍒锋柊锛?*/
    s_lbl_sd = make_label(s_content, "", menu_cjk_font_cn(false), lv_color_hex(0xFFFFFF),
                          0, 78, 208);

    /* 瀛楀簱鐘舵€?*/
    s_lbl_fonts = make_label(s_content, "", menu_cjk_font_cn(false), lv_color_hex(0x9AA0A6),
                             0, 102, 208);

    /* 鍐呭瓨锛?s 鍒锋柊锛孭SRAM/SRAM 鍒嗚閬垮厤闀挎枃鏈崲琛屾尋鍘嬶級 */
    s_lbl_psram = make_label(s_content, "", menu_cjk_font_cn(false), lv_color_hex(0x9AA0A6),
                             0, 126, 208);
    s_lbl_sram = make_label(s_content, "", menu_cjk_font_cn(false), lv_color_hex(0x9AA0A6),
                            0, 150, 208);

    /* cardstore 鍒嗗尯锛?s 鍒锋柊锛?*/
    s_lbl_spiffs = make_label(s_content, "", menu_cjk_font_cn(false), lv_color_hex(0x9AA0A6),
                              0, 174, 208);

    /* 日志级别 + 切换按钮 */
    s_lbl_loglvl = make_label(s_content, "", menu_cjk_font_cn(false), lv_color_hex(0xFFFFFF),
                              0, 198, 140);
    lv_obj_t *lvl_btn = lv_btn_create(s_content);
    lv_obj_set_size(lvl_btn, 72, 32);
    lv_obj_set_pos(lvl_btn, 140, 192);
    lv_obj_set_style_bg_color(lvl_btn, lv_color_hex(0x23252A), 0);
    lv_obj_set_style_radius(lvl_btn, 6, 0);
    lv_obj_set_style_border_width(lvl_btn, 0, 0);
    lv_obj_set_style_shadow_width(lvl_btn, 0, 0);
    lv_obj_set_style_pad_all(lvl_btn, 0, 0);
    lv_obj_t *lvl_lb = lv_label_create(lvl_btn);
    lv_label_set_text(lvl_lb, "切换");
    lv_obj_set_style_text_color(lvl_lb, lv_color_hex(0xFFFFFF), 0);
    lv_obj_set_style_text_font(lvl_lb, menu_cjk_font_cn(false), 0);
    lv_obj_center(lvl_lb);
    lv_obj_add_event_cb(lvl_btn, loglvl_evt_cb, LV_EVENT_CLICKED, NULL);
}

/* 淇℃伅椤?1s 鍒锋柊 */
static void refresh_info_page(void) {
    ESP_LOGD(TAG, "info refr up=%u", (unsigned)(esp_timer_get_time() / 1000000));
    if (s_lbl_uptime == NULL) return;
    char buf[160];

    uint32_t up = (uint32_t)(esp_timer_get_time() / 1000000);
    snprintf(buf, sizeof(buf), "运行时间 %02u:%02u:%02u",
             (unsigned)(up / 3600), (unsigned)((up / 60) % 60), (unsigned)(up % 60));
    lv_label_set_text(s_lbl_uptime, buf);

    if (bsp_sdcard_is_mounted()) {
        uint64_t total = 0, free_b = 0;
        esp_err_t err = bsp_sdcard_stats(&total, &free_b);
        if (err == ESP_OK && total > 0) {
            snprintf(buf, sizeof(buf), "SD 已挂载 %lu/%luMB",
                     (unsigned long)(free_b / (1024 * 1024)),
                     (unsigned long)(total / (1024 * 1024)));
        } else {
            snprintf(buf, sizeof(buf), "SD 已挂载");
        }
    } else if (bsp_sdcard_fail_streak() >= 3 && bsp_sdcard_probe_ok()) {
        snprintf(buf, sizeof(buf), "SD 无法识别");
    } else {
        snprintf(buf, sizeof(buf), "SD 未挂载");
    }
    lv_label_set_text(s_lbl_sd, buf);

    snprintf(buf, sizeof(buf), "字库 %s", sd_fonts_loaded() ? "已加载" : "未加载");
    lv_label_set_text(s_lbl_fonts, buf);

    size_t psram = heap_caps_get_free_size(MALLOC_CAP_SPIRAM);
    size_t sram = heap_caps_get_free_size(MALLOC_CAP_INTERNAL);
    snprintf(buf, sizeof(buf), "PSRAM 剩余 %uKB",
             (unsigned)(psram / 1024));
    lv_label_set_text(s_lbl_psram, buf);
    snprintf(buf, sizeof(buf), "SRAM 剩余 %uKB",
             (unsigned)(sram / 1024));
    lv_label_set_text(s_lbl_sram, buf);

    size_t total = 0, used = 0;
    if (esp_spiffs_info("cardstore", &total, &used) == ESP_OK) {
        snprintf(buf, sizeof(buf), "分区 cardstore %u/%uKB",
                 (unsigned)(used / 1024), (unsigned)(total / 1024));
    } else {
        snprintf(buf, sizeof(buf), "分区 cardstore 不可用");
    }
    lv_label_set_text(s_lbl_spiffs, buf);

    snprintf(buf, sizeof(buf), "日志级别 %s", log_level_text(esp_log_level_get("*")));
    lv_label_set_text(s_lbl_loglvl, buf);
}

/* ============================================================================
 * 页面入口分发
 * ==========================================================================*/

static void page_enter(menu_page_t page) {
    /* 椤靛ご锛歅AGE_MENU 鏃犻〉澶达紝鍏朵綑甯﹁繑鍥?+ 鏍囬 */
    switch (page) {
        case PAGE_MENU:
            lv_obj_add_flag(s_header, LV_OBJ_FLAG_HIDDEN);
            lv_obj_set_pos(s_content, 0, 14);
            lv_obj_set_size(s_content, 240, SHEET_H - HEADER_H);
            build_page_menu();
            break;
        case PAGE_ROLES:
            build_header("角色选择", true);
            lv_obj_clear_flag(s_header, LV_OBJ_FLAG_HIDDEN);
            build_page_roles();
            break;
        case PAGE_CARDS:
            build_header("名片簿", true);
            lv_obj_clear_flag(s_header, LV_OBJ_FLAG_HIDDEN);
            build_page_cards();
            break;
        case PAGE_CARD_DETAIL:
            build_header("名片详情", true);
            lv_obj_clear_flag(s_header, LV_OBJ_FLAG_HIDDEN);
            build_page_card_detail();
            break;
        case PAGE_BLE:
            build_header("蓝牙", true);
            lv_obj_clear_flag(s_header, LV_OBJ_FLAG_HIDDEN);
            build_page_ble();
            refresh_ble_page();
            break;
        case PAGE_UIOPTS:
            build_header("UI 选项", true);
            lv_obj_clear_flag(s_header, LV_OBJ_FLAG_HIDDEN);
            build_page_uiopts();
            break;
        case PAGE_INFO:
            build_header("系统信息", true);
            lv_obj_clear_flag(s_header, LV_OBJ_FLAG_HIDDEN);
            build_page_info();
            refresh_info_page();
            break;
    }
    /* 婊戝叆鍔ㄦ晥锛堜富鑿滃崟鎵撳紑鏃跺凡鍦?open_menu 鍓嶇疆 x=0锛屾棤闇€鍐嶅姩锛?*/
    if (s_page != PAGE_MENU || lv_obj_get_x(s_content) != 0) {
        lv_anim_del(s_content, content_anim_exec);
        lv_obj_set_x(s_content, 240);
        lv_anim_t a;
        lv_anim_init(&a);
        lv_anim_set_var(&a, s_content);
        lv_anim_set_values(&a, 240, 0);
        lv_anim_set_time(&a, PAGE_SLIDE_MS);
        lv_anim_set_exec_cb(&a, content_anim_exec);
        lv_anim_set_path_cb(&a, lv_anim_path_ease_out);
        lv_anim_set_ready_cb(&a, content_in_ready_cb);
        lv_anim_start(&a);
    }
}

/* ============================================================================
 * 瑙︽帶鏍″噯寮曞锛堟湭鏍″噯鏃跺叏灞忔彁绀猴紱鐐瑰洓瑙掑悗鑷姩淇濆瓨骞舵敹璧凤級
 * ==========================================================================*/

static void cal_timer_cb(lv_timer_t *t) {
    (void)t;
    if (s_cal_layer == NULL) return;
    /* 淇濇寔鏈€椤讹細鎻ず鍔ㄦ晥/妯箙鐨?move_foreground 浼氱洊浣忓紩瀵煎眰锛?     * 姣?100ms 寮哄埗鎷夊洖锛坺 搴忔渶楂樹紭鍏堢骇锛?*/
    lv_obj_move_foreground(s_cal_layer);
    if (s_lbl_cal_cnt == NULL) return;
    /* 瀹屾垚鏉′欢锛氭湰娆″紩瀵煎疄闄呯偣婊?4 娆★紙闃叉宸叉湁鏃ф牎鍑嗘暟鎹椂
     * 寮曞灞備竴鍑虹幇灏辫"宸叉牎鍑?鑷姩鏀惰捣锛岀敤鎴锋牴鏈病鏈轰細鐐癸級 */
    if (bsp_touch_cal_valid() && bsp_touch_cal_taps() >= 4) {
        lv_label_set_text(s_lbl_cal_cnt, "校准完成");
        lv_obj_clear_flag(s_lbl_cal_hint, LV_OBJ_FLAG_HIDDEN);
        bsp_touch_cal_mark_guided();
        if (++s_cal_done_ticks >= 10) {          /* 1s 后收起引导层 */
            lv_timer_del(s_cal_timer);
            s_cal_timer = NULL;
            lv_obj_del(s_cal_layer);
            s_cal_layer = NULL;
            s_lbl_cal_cnt = NULL;
            s_lbl_cal_hint = NULL;
        }
    } else {
        char buf[32];
        snprintf(buf, sizeof(buf), "已点击%d 次", bsp_touch_cal_taps());
        lv_label_set_text(s_lbl_cal_cnt, buf);
    }
}

static void show_cal_overlay(void) {
    if (s_cal_layer != NULL) return;   /* 闃查噸鍏ワ細浜屾璋冪敤浼氳鐩栨寚閽堟硠婕忔棫寮曞灞?*/
    lv_obj_t *scr = lv_scr_act();

    s_cal_layer = lv_obj_create(scr);
    lv_obj_move_foreground(s_cal_layer);   /* 绔嬪嵆缃《锛岄槻鍔ㄦ晥鍏堝０澶轰汉 */
    lv_obj_set_size(s_cal_layer, 240, 320);
    lv_obj_set_pos(s_cal_layer, 0, 0);
    lv_obj_set_style_bg_color(s_cal_layer, lv_color_hex(0x000000), 0);
    lv_obj_set_style_bg_opa(s_cal_layer, LV_OPA_40, 0);
    lv_obj_set_style_radius(s_cal_layer, 0, 0);
    lv_obj_set_style_border_width(s_cal_layer, 0, 0);
    lv_obj_clear_flag(s_cal_layer, LV_OBJ_FLAG_SCROLLABLE);
    lv_obj_clear_flag(s_cal_layer, LV_OBJ_FLAG_SCROLL_CHAIN);

    lv_obj_t *panel = lv_obj_create(s_cal_layer);
    lv_obj_set_size(panel, 220, 150);
    lv_obj_set_pos(panel, 10, 85);
    lv_obj_set_style_bg_color(panel, lv_color_hex(0x18191C), 0);
    lv_obj_set_style_bg_opa(panel, SHEET_OPA, 0);
    lv_obj_set_style_radius(panel, 12, 0);
    lv_obj_set_style_border_width(panel, 0, 0);
    lv_obj_clear_flag(panel, LV_OBJ_FLAG_SCROLLABLE);

    make_label(panel, "触控校准", menu_cjk_font_cn(true), lv_color_hex(0xF5C542), 20, 14, 180);
    make_label(panel, "请依次点击屏幕四个角", menu_cjk_font_cn(false), lv_color_hex(0xFFFFFF), 20, 52, 180);
    bsp_touch_cal_reset_taps();   /* 浼氳瘽璁℃暟浠?0 璧凤細鐐规弧 4 娆℃墠鏀惰捣 */
    s_lbl_cal_cnt = make_label(panel, "已点击0 次", menu_cjk_font_cn(false), lv_color_hex(0x9AA0A6), 20, 84, 180);
    s_lbl_cal_hint = make_label(panel, "上滑打开菜单", menu_cjk_font_cn(false), lv_color_hex(0x9AA0A6), 20, 110, 180);
    lv_obj_add_flag(s_lbl_cal_hint, LV_OBJ_FLAG_HIDDEN);

    s_cal_timer = lv_timer_create(cal_timer_cb, 100, NULL);
    s_cal_done_ticks = 0;
}

/* ============================================================================
 * 涓婃粦鎵嬪娍妫€娴嬶紙10ms LVGL 瀹氭椂鍣紝杩愯鍦?LVGL 浠诲姟涓婁笅鏂囷級
 * 鐩存帴璇昏Е鎺?IRQ 鐢靛钩 + 灞忓箷鍧愭爣锛堜笉渚濊禆 LVGL indev 閲囨牱锛夛細
 * 璧风偣鍦ㄥ睆骞曚笅閮紙y鈮?55锛変笖涓婄Щ 鈮?0px銆佹按骞虫紓绉?鈮?0px 鈫?鎵撳紑鑿滃崟銆? * 鎸変笅鏈熼棿瀹炴椂鍒ゅ畾锛堟棤闇€绛夊緟鏉炬墜锛夛紝瑙﹀彂涓€娆″悗鏈鎸夊帇涓嶅啀閲嶅銆? * ==========================================================================*/

static void gesture_timer_cb(lv_timer_t *t) {
    (void)t;
    if (s_cal_layer != NULL) return;   /* 鏍″噯寮曞鏈熼棿绂佺敤鎵嬪娍 */

    if (!s_menu_open) {
        bool down = bsp_touch_pressed();
        if (down) {
            if (!s_down) {
                /* 按下沿：记录起点 */
                s_down = true;
                s_open_fired = false;
                int16_t px, py;
                if (bsp_touch_point(&px, &py)) {
                    s_down_x = px;
                    s_down_y = py;
                }
            } else if (!s_open_fired) {
                /* 鎸夊帇涓細瀹炴椂妫€鏌ヤ笂婊戞槸鍚﹁揪鏍?*/
                int16_t px, py;
                if (bsp_touch_point(&px, &py)) {
                    if (s_down_y >= GESTURE_START_Y &&
                        (s_down_y - py) >= GESTURE_DIST &&
                        abs(px - s_down_x) <= GESTURE_DX_MAX) {
                        s_open_fired = true;
                        open_menu();
                    }
                }
            }
        } else {
            s_down = false;
            s_open_fired = false;
        }
    }

    /* 鑿滃崟鎵撳紑鏃讹細鍒楄〃鎵嬪姩鎷栧姩 + 椤堕儴鍖轰笅婊戞敹鍥?*/
    if (s_menu_open) {
        bool down = bsp_touch_pressed();
        int16_t tx, ty;
        bool has = bsp_touch_point(&tx, &ty);
        if (down && has) {
            if (!s_drag_prev) {
                /* 鎸変笅娌匡細璧风偣 y<100锛堥潰鏉块《閮ㄥ尯锛夆啋 姝﹁涓嬫粦鏀跺洖锛?                 * 鍒楄〃椤?鈫?璁板綍鎷栧姩璧风偣锛涘叾浣欓〉闈㈡棤鎵嬪娍 */
                s_drag_anchor = ty;
                if (ty < 100) {
                    s_pull_armed = true;
                    s_drag_active = false;
                } else if ((s_page == PAGE_ROLES || s_page == PAGE_CARDS) &&
                           s_list != NULL) {
                    s_pull_armed = false;
                    s_drag_base = lv_obj_get_scroll_y(s_list);
                    s_drag_active = true;
                } else {
                    s_pull_armed = false;
                    s_drag_active = false;
                }
            } else {
                /* 鎸夊帇涓細涓嬫粦杈炬爣 鈫?鏀跺洖锛涘惁鍒欏垪琛ㄩ〉璺熸墜婊氬姩 */
                if (s_pull_armed && (ty - s_drag_anchor) >= 50) {
                    s_pull_armed = false;
                    s_drag_active = false;
                    close_menu();
                } else if (s_drag_active && s_list != NULL) {
                    lv_obj_scroll_to_y(s_list, s_drag_base + (s_drag_anchor - ty),
                                       LV_ANIM_OFF);
                }
            }
        } else {
            s_pull_armed = false;
            s_drag_active = false;
        }
        s_drag_prev = down;
    }

    /* 钃濈墮椤?500ms / 淇℃伅椤?1s 鍒锋柊锛堢湡瀹炴椂闂撮┍鍔紝涓嶄緷璧栬妭鎷嶈鏁扳€斺€?     * lv_timer_handler 瀹為檯鍛ㄦ湡 ~33ms锛?50/%100 璁℃暟浼氭參 3 鍊嶏級 */
    if (s_menu_open && s_page == PAGE_BLE) {
        uint32_t now_ms = (uint32_t)(esp_timer_get_time() / 1000);
        if (now_ms - s_last_ble_refr >= 500) {
            s_last_ble_refr = now_ms;
            refresh_ble_page();
        }
    } else if (s_menu_open && s_page == PAGE_INFO) {
        uint32_t now_ms = (uint32_t)(esp_timer_get_time() / 1000);
        if (now_ms - s_last_info_refr >= 1000) {
            s_last_info_refr = now_ms;
            refresh_info_page();
        }
    }
}

/* ============================================================================
 * 閬僵鐐瑰嚮 鈫?鍏抽棴
 * ==========================================================================*/

static void layer_evt_cb(lv_event_t *e) {
    (void)e;
    close_menu();
}

/* ============================================================================
 * 鍒濆鍖? * ==========================================================================*/

void ui_menu_init(void) {
    bsp_lvgl_lock();

    lv_obj_t *scr = lv_scr_act();

    /* 鍏ㄥ睆鑿滃崟灞傦細鍗婇€忔槑榛戦伄缃╋紝鐐瑰嚮鍏抽棴 */
    s_layer = lv_obj_create(scr);
    lv_obj_set_size(s_layer, 240, 320);
    lv_obj_set_pos(s_layer, 0, 0);
    lv_obj_set_style_bg_color(s_layer, lv_color_hex(0x000000), 0);
        lv_obj_set_style_bg_opa(s_layer, LV_OPA_30, 0);   /* 鍗婇€忔槑榛戦€忛伄缃?*/
    lv_obj_set_style_radius(s_layer, 0, 0);
    lv_obj_set_style_border_width(s_layer, 0, 0);
    lv_obj_clear_flag(s_layer, LV_OBJ_FLAG_SCROLLABLE);
    lv_obj_clear_flag(s_layer, LV_OBJ_FLAG_SCROLL_CHAIN);
    lv_obj_add_flag(s_layer, LV_OBJ_FLAG_HIDDEN);
    lv_obj_add_event_cb(s_layer, layer_evt_cb, LV_EVENT_CLICKED, NULL);

    /* 鍏ㄥ睆闈㈡澘锛堝崐閫忔槑榛戦€忥紝涓庨伄缃╁彔鍔狅紱鏁翠綋鍚戝乏涓婂亸绉绘姷娑堣Е鎺у亸绉伙級 */
    s_sheet = lv_obj_create(s_layer);
    lv_obj_set_size(s_sheet, 240, SHEET_H);
    lv_obj_set_pos(s_sheet, SHEET_X, SHEET_Y);
    lv_obj_set_style_bg_color(s_sheet, lv_color_hex(0x18191C), 0);
    lv_obj_set_style_bg_opa(s_sheet, SHEET_OPA, 0);
    lv_obj_set_style_radius(s_sheet, 0, 0);
    lv_obj_set_style_border_width(s_sheet, 0, 0);
    lv_obj_clear_flag(s_sheet, LV_OBJ_FLAG_SCROLLABLE);
    lv_obj_clear_flag(s_sheet, LV_OBJ_FLAG_SCROLL_CHAIN);

    /* 页头（默认隐藏，子页打开时显示） */
    s_header = lv_obj_create(s_sheet);
    lv_obj_set_size(s_header, 240, HEADER_H);
    lv_obj_set_pos(s_header, 0, 0);
    lv_obj_set_style_bg_color(s_header, lv_color_hex(0x18191C), 0);
    lv_obj_set_style_bg_opa(s_header, LV_OPA_0, 0);
    lv_obj_set_style_border_width(s_header, 0, 0);
    lv_obj_set_style_pad_all(s_header, 0, 0);
    lv_obj_set_style_radius(s_header, 0, 0);
    lv_obj_clear_flag(s_header, LV_OBJ_FLAG_SCROLLABLE);

    /* 鍐呭瀹瑰櫒锛氭暣浣撲笅绉?HEADER_H锛岄伩鍏嶇洊浣忛〉澶磋繑鍥炴寜閽?*/
    s_content = lv_obj_create(s_sheet);
    lv_obj_set_size(s_content, 240, SHEET_H - HEADER_H);
    lv_obj_set_pos(s_content, 0, 14);
    lv_obj_set_style_bg_color(s_content, lv_color_hex(0x000000), 0);
    lv_obj_set_style_bg_opa(s_content, LV_OPA_0, 0);
    lv_obj_set_style_border_width(s_content, 0, 0);
    lv_obj_set_style_pad_all(s_content, 0, 0);
    lv_obj_set_style_radius(s_content, 0, 0);
    lv_obj_clear_flag(s_content, LV_OBJ_FLAG_SCROLLABLE);

    /* 涓夋鐐瑰嚮鎵嬪娍瀹氭椂鍣紙10ms锛?*/
    lv_timer_create(gesture_timer_cb, 10, NULL);

    bsp_lvgl_unlock();
    ESP_LOGI(TAG, "touch menu ready (tap 3x to open)");
}

/* 鏍″噯寮曞鎷夎捣锛堢敱鎾斁鍣ㄥ湪 loop 棣栧抚鍙戝竷銆乁I 鎻ず瀹屾垚鍚庤皟鐢級锛? * 宸叉寜鐢ㄦ埛瑕佹眰绂佺敤鑷姩寮瑰嚭锛堟牎鍑嗘祦绋嬫棤鏁堟灉锛屾敼鐢ㄨ酱鍙嶈浆+鍋忕Щ淇锛?*/
void ui_menu_maybe_show_cal_guide(void) {
    (void)0;   /* 绂佺敤锛氫笉鍐嶈嚜鍔ㄦ媺璧锋牎鍑嗗紩瀵?*/
#if 0
    if (s_cal_layer != NULL) return;                       /* 已在显示 */
    if (bsp_touch_cal_valid() || bsp_touch_cal_guided()) return;
    bsp_lvgl_lock();
    show_cal_overlay();
    bsp_lvgl_unlock();
#endif
}

bool ui_menu_is_open(void) {
    return s_menu_open;
}

void ui_menu_notify_ble_state(void) {
    if (s_lbl_ble_status == NULL) return;
    bsp_lvgl_lock();
    refresh_ble_page();
    bsp_lvgl_unlock();
}
