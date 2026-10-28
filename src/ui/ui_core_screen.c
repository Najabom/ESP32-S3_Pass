/* ============================================================================
 * ui_core_screen.c - 主界面（角色名片系统样式）+ 名片横幅
 *
 * 布局（240x320）：
 *   - 底层：MJPEG 视频画面（main.c 创建的 lv_img，非本文件管理）
 *   - 左侧旋转立牌：竖排文字由当前角色 ui_text.txt 的 leftcol= 配置
 *     （≤13 字；未配置/留空 → 不显示），BebasNeue 字体，运行时按实测
 *     宽度自动选 56/48/40 号中能放进 300px 的最大字号
 *   - 底部角色面板：role_name（由 ui_text.txt 的 op_name= 配置，可中文；
 *     留空 → 不显示）+ 副标题（ui_text.txt 的 sub_text=，montserrat_10
 *     固定；留空 → 不显示）
 *   - 名片横幅：收到 ESP-NOW 名片时底部弹出 5 秒（头像+姓名+ID+表情包）
 *   - leftcol/footer 在 intro 播完前隐藏：ui_core_screen_intro_done()
 *     一次揭示——leftcol 从紧贴屏缘（右缘 4px 已在屏内）激进缓出浮入
 *     （1.5s，起步快后段慢），role_name 闪 7 次共 1s（间隔递减 = 越来越
 *     快，与 leftcol 同时开始）后 sub_text 闪 4 次共 0.5s 再固定（见
 *     下方 s_reveal_steps 事件表）
 *
 * 可调整内容（集中在文件顶部宏）：
 *   - BANNER_ANIM_MS 1000：从下方浮入动效时长（非线性缓出）
 *   - BANNER_OUT_MS 1000：从下方浮出动效时长（非线性缓入缓出，
 *     动画结束才真正隐藏）
 *   - BANNER_SHOW_MS 5000：横幅静止显示时长（从动效结束后起算）
 *   - BANNER_ROTATE_MS 1200：多表情包时轮换间隔
 *   - BANNER_W/H 240x90：横幅尺寸。BANNER_W 不得超过屏宽 240，
 *     内部控件（头像 64 + 名/ID 宽 160）均按 ≤240 约束布局
 *   - BANNER_BG_OPA 60%：横幅半透明底不透明度（LV_OPA_60）
 *   - 横幅角色名字号阶梯：SD 字库已加载用 30/16 两档（GB2312 全集，
 *     支持中文，见 sd_fonts.c）；未加载回退内嵌 30/24/18/14（仅 ASCII）。
 *     可用区 160px，超宽自动落小档，末档放不下由 LONG_DOT 截断
 *   - leftcol 尺寸 300x44 与 pivot(22,22)：旋转轴心在"字高中心"，
 *     改字号阶梯或文本需同步验证不超出屏幕（运行时日志会打印实测宽度）
 *   - footer 的 align(0,-15) 与 role_name 的 x=40：面板位置微调
 * ==========================================================================*/

// ui/ui_core_screen.c
#include <string.h>
#include <stdio.h>
#include "ui_core_screen.h"
#include "ui_menu.h"
#include "ui_options.h"
#include "roles.h"
#include "sd_fonts.h"
#include "bsp_lvgl.h"
#include "esp_log.h"
#include "esp_heap_caps.h"
#include "nfc_proto.h"
#include "../resources/default_avatar.h"

static const char *TAG = "UI";

/* RGB565 字节序：portrait/avatar 数据为 LE（ffmpeg rgb565le/固件打包），
 * 而 LVGL 显示要求 LV_COLOR_16_SWAP 字节序——显示前就地交换高地位。
 * 输入缓冲须为独立副本（不可改 blob 缓存/只读数组）。 */
static void swap565_inplace(void *buf, size_t len) {
    uint8_t *p = (uint8_t *)buf;
    for (size_t i = 0; i + 1 < len; i += 2) {
        uint8_t t = p[i];
        p[i] = p[i + 1];
        p[i + 1] = t;
    }
}

LV_FONT_DECLARE(bebas_56);
LV_FONT_DECLARE(bebas_48);
LV_FONT_DECLARE(bebas_40);
LV_FONT_DECLARE(role_name_30);
LV_FONT_DECLARE(role_name_24);
LV_FONT_DECLARE(role_name_18);
LV_FONT_DECLARE(role_name_14);
LV_FONT_DECLARE(uname_18);
LV_FONT_DECLARE(uname_14);
LV_FONT_DECLARE(uid_12);
LV_FONT_DECLARE(hint_24);
LV_FONT_DECLARE(pwr_16);

/* ---------- 横幅尺寸/时长参数（本文件的主要调整点）---------- */
#define BANNER_W 240
#define BANNER_H 90
#define BANNER_BG_OPA LV_OPA_60 /* 横幅半透明底；太透（如 30）文字压不住
                                  * 亮画面，太实（如 80）又遮住视频 */
#define BANNER_ANIM_MS 1000   /* 入场动效时长：从屏幕下方浮入，缓出（非线性） */
#define BANNER_OUT_MS 1000    /* 出场动效时长：向屏幕下方浮出（原 300ms 缓入过短，视觉上等于直接消失） */
#define BANNER_SHOW_MS 5000   /* 静止显示时长，动效结束后才起算 */
#define BANNER_ROTATE_MS 1200

/* 横幅静态控件与图像缓冲：
 * - 静态缓冲（不随函数返回释放）：NFC 回调里的 payload 会被回收，
 *   必须拷贝进 s_banner_avatar/s_banner_stickers 才能跨帧显示。
 * - 缓冲各 4096 u16 = 8192B = 64x64 RGB565 一张图，与协议一致。 */
static lv_obj_t   *s_role_name = NULL;   /* 底部面板角色名（set_role 更新） */
static lv_obj_t   *s_banner = NULL;
static lv_obj_t   *s_banner_img = NULL;
static lv_obj_t   *s_banner_name = NULL;
static lv_obj_t   *s_banner_uid = NULL;
static bool        s_banner_use_uname = false; /* 横幅当前显示的是用户名还是角色名（refresh_fonts 重选字体用） */
static lv_timer_t *s_banner_timer = NULL;
static lv_timer_t *s_banner_rotate = NULL;
static lv_img_dsc_t s_banner_dsc[4];
static uint16_t   s_banner_avatar[4096];
static uint16_t   s_banner_stickers[3][4096];
static uint8_t    s_banner_count = 0;
static uint8_t    s_banner_idx = 0;
static int32_t    s_banner_end_y = 0; /* 横幅落定后的 y 坐标（init 时量取） */
static int32_t    s_banner_hide_y = 0; /* 浮出动画终点（= 屏底，横幅完全出屏） */

/* ---------- 未就绪横幅缓存 ----------
 * 功能：boot 阶段屏幕归裸终端，UI 虽已提前创建但可能晚于 NFC 首交换
 *       （首交换 ≥3s，理论追不上，但防时序抖动）：UI 未就绪（s_banner
 *       尚未创建）时 show_banner 把全部参数拷贝进 PSRAM，init 完成后
 *       立即补显，不丢名片横幅。 */
typedef struct {
    bool     valid;
    char     name[CARD_NAME_MAX];
    char     uname[CARD_UNAME_MAX];
    char     uid[CARD_UID_MAX];
    uint8_t *payload;                 /* PSRAM 拷贝（portrait+avatar+stickers） */
    size_t   payload_len;
    uint16_t avatar_len;
    uint16_t portrait_len;
    uint8_t  sticker_cnt;
    uint16_t sticker_lens[CARD_STICKER_MAX];
} pending_banner_t;
static pending_banner_t s_pending_banner;

/* ---------- intro 揭示控件与闪动链参数 ---------- */
static lv_obj_t   *s_leftcol = NULL;   /* 左侧旋转立牌（intro 播完揭示） */
static lv_obj_t   *s_brand_label = NULL;    /* 立牌文字标签（文本由角色 ui_text.txt 的 leftcol 配置） */
static lv_obj_t   *s_footer = NULL;    /* 底部角色面板（intro 播完揭示） */
static lv_obj_t   *s_sub_text = NULL;  /* 面板副标题（文本由角色 ui_text.txt 的 sub_text 配置） */
static bool        s_revealed = false; /* intro_done 幂等开关（只揭示一次） */
static lv_timer_t *s_flash_timer = NULL;
static lv_obj_t   *s_hint = NULL;     /* 屏幕中央状态提示（set_hint 控制，hint_24 内嵌中文字库） */
static char        s_hint_text[32] = ""; /* 当前提示内容（去重用） */

/* 揭示闪动链：事件表（delay_ms = 距上一事件的间隔；on = 亮/灭）。
 * 30fps 帧表由用户指定，已换算为毫秒：
 *   role_name 7 次共 1000ms（与 leftcol 浮入同时开始）：
 *     暗@帧0，亮@峰值帧，灭@结束帧，第 7 次最终常亮
 *     帧/ms：1/33 亮，4/133 灭，6/200 亮，8/267 灭，10/333 亮，
 *     12/400 灭，14/467 亮，16/533 灭，18/600 亮，19/633 灭，
 *     21/700 亮，22/733 灭，30/1000 最终常亮
 *   sub_text 4 次共 500ms（role_name 常亮后接续，第 13 拍同时切 sub 灭）：
 *     暗@起始帧，亮@中帧，灭@结束帧，第 4 次最终常亮
 *     帧/ms：2/67 亮，4/133 灭，6/200 亮，8/267 灭，10/333 亮，
 *     11/367 灭，15/500 最终常亮
 * 增删拍数需同步改周期表与 REVEAL_ROLE_STEPS/REVEAL_TOTAL。 */
#define REVEAL_ROLE_STEPS  13   /* 前 13 拍 = role_name 阶段（第 13 拍=最终常亮，兼启动 sub 灭） */
#define REVEAL_TOTAL     20
typedef struct {
    uint32_t delay_ms;
    bool     on;      /* true=LV_OPA_COVER，false=LV_OPA_TRANSP */
} reveal_step_t;
static const reveal_step_t s_reveal_steps[REVEAL_TOTAL] = {
    /* ---- role_name：7 次闪烁，共 1000ms ---- */
    {  33, true  },   /* 帧1  亮 */
    { 100, false },   /* 帧4  灭 */
    {  67, true  },   /* 帧6  亮 */
    {  67, false },   /* 帧8  灭 */
    {  66, true  },   /* 帧10 亮 */
    {  67, false },   /* 帧12 灭 */
    {  67, true  },   /* 帧14 亮 */
    {  66, false },   /* 帧16 灭 */
    {  67, true  },   /* 帧18 亮 */
    {  33, false },   /* 帧19 灭 */
    {  67, true  },   /* 帧21 亮 */
    {  33, false },   /* 帧22 灭 */
    { 267, true  },   /* 帧30 最终常亮（同时 sub_text 切灭，开始 sub 阶段） */
    /* ---- sub_text：4 次闪烁，共 500ms ---- */
    {  67, true  },   /* 帧2  亮 */
    {  66, false },   /* 帧4  灭 */
    {  67, true  },   /* 帧6  亮 */
    {  67, false },   /* 帧8  灭 */
    {  66, true  },   /* 帧10 亮 */
    {  34, false },   /* 帧11 灭 */
    { 133, true  },   /* 帧15 最终常亮 */
};
static int s_flash_step = 0;

/* 前向声明：横幅隐藏/轮换回调定义在下方（banner_anim_ready_cb 需要引用） */
static void banner_hide_cb(lv_timer_t *timer);
static void banner_rotate_cb(lv_timer_t *timer);

/* ---------- 入场动效执行回调 ----------
 * 功能：lv_anim 每帧调用，把横幅 y 从屏幕下方移到落定位。
 * 注意：lv_anim 由 LVGL 任务驱动，天然与渲染同线程，无需加锁。 */
static void banner_anim_exec(void *var, int32_t v)
{
    lv_obj_set_y((lv_obj_t *)var, (lv_coord_t)v);
}

/* ---------- 立牌浮入动效执行回调 ----------
 * 功能：lv_anim 每帧调用，把 leftcol 的 x 从屏幕外左侧移到落定位。
 * 注意：lv_anim 由 LVGL 任务驱动，天然与渲染同线程，无需加锁。 */
static void leftcol_anim_exec(void *var, int32_t v)
{
    lv_obj_set_x((lv_obj_t *)var, (lv_coord_t)v);
}

/* ---------- 立牌浮入路径（激进缓出） ----------
 * 功能：u = t/T ∈ [0,1024]，v = 1-(1-u)^3 ∈ [0,1024]——起步几乎全速
 *       （半程已走 87.5%），后段急剧减速（比内置 ease_out 的 75% 更激进）。 */
static int32_t leftcol_path_aggressive(const lv_anim_t *a)
{
    int32_t u = lv_map(a->act_time, 0, a->time, 0, 1024);
    int32_t om = 1024 - u;
    int32_t v = 1024 - (((om * om) >> 10) * om >> 10);
    return a->start_value + ((a->end_value - a->start_value) * v >> 10);
}

/* ---------- 入场动效结束回调 ----------
 * 功能：动效完成后才开始 BANNER_SHOW_MS 静止显示计时（不算动效时长），
 *       并启动表情包轮换。 */
static void banner_anim_ready_cb(lv_anim_t *a)
{
    (void)a;
    ESP_LOGI(TAG, "banner in done, hide timer %d ms", BANNER_SHOW_MS);
    s_banner_timer = lv_timer_create(banner_hide_cb, BANNER_SHOW_MS, NULL);
    if (s_banner_timer != NULL) {
        lv_timer_set_repeat_count(s_banner_timer, 1); /* 只触发一次（触发后 LVGL 自删） */
    }
    if (s_banner_count > 1) {
        s_banner_rotate = lv_timer_create(banner_rotate_cb, BANNER_ROTATE_MS, NULL);
        lv_timer_set_repeat_count(s_banner_rotate, -1);
    }
}

/* ---------- 表情包轮换定时器回调 ----------
 * 功能：每 BANNER_ROTATE_MS 把横幅头像区切到下一张图（头像→表情包们）。 */
static void banner_rotate_cb(lv_timer_t *timer)
{
    (void)timer;
    if (s_banner_count == 0) {
        return;
    }
    s_banner_idx = (uint8_t)((s_banner_idx + 1) % s_banner_count);
    lv_img_set_src(s_banner_img, &s_banner_dsc[s_banner_idx]);
}

/* ---------- 浮出动画结束回调 ----------
 * 功能：横幅完全滑出屏幕后才真正隐藏。 */
static void banner_out_anim_ready_cb(lv_anim_t *a)
{
    (void)a;
    ESP_LOGI(TAG, "banner out done");
    if (s_banner != NULL) {
        lv_obj_add_flag(s_banner, LV_OBJ_FLAG_HIDDEN);
    }
}

/* ---------- 横幅隐藏定时器回调 ----------
 * 功能：静止显示时长到 → 停掉轮换定时器 → 播放浮出动画（向下滑出
 *       屏幕），动画结束（banner_out_anim_ready_cb）才真正隐藏。
 * 注意：本定时器已在创建处设为 repeat_count=1（触发后 LVGL 自删），
 *       勿在此回调里 lv_timer_del(timer)——自删后 lv_timer_handler
 *       仍会访问该 timer，造成悬垂访问。 */
static void banner_hide_cb(lv_timer_t *timer)
{
    (void)timer;
    s_banner_timer = NULL;

    if (s_banner == NULL) {
        return;
    }
    if (s_banner_rotate != NULL) {
        lv_timer_del(s_banner_rotate);
        s_banner_rotate = NULL;
    }

    /* 入场/出场动画共用 banner_anim_exec，先清掉可能未完成的动画，
     * 从当前位置开始浮出，避免 y 跳变 */
    lv_anim_del(s_banner, banner_anim_exec);

    lv_anim_t a;
    lv_anim_init(&a);
    lv_anim_set_var(&a, s_banner);
    lv_anim_set_values(&a, lv_obj_get_y(s_banner), s_banner_hide_y);
    lv_anim_set_time(&a, BANNER_OUT_MS);
    lv_anim_set_exec_cb(&a, banner_anim_exec);
    lv_anim_set_path_cb(&a, lv_anim_path_ease_in_out); /* 缓入缓出：全程可见的滑出，与入场动效对称 */
    lv_anim_set_ready_cb(&a, banner_out_anim_ready_cb);
    lv_anim_start(&a);
    ESP_LOGI(TAG, "banner out anim: y %d -> %d (%d ms)",
             (int)lv_obj_get_y(s_banner), (int)s_banner_hide_y, BANNER_OUT_MS);
}

/* ---------- 底部角色名字体选择 ----------
 * 功能：优先 SD 字库 30 档（GB2312 全集，支持中文）；未加载回退内嵌
 *       role_name_30（ASCII）。横幅阶梯同理（show_banner 里现场组）。 */
static const lv_font_t *role_font_30(void) {
    const lv_font_t *f = sd_fonts_role(SD_FONT_ROLE_LARGE);
    return f ? f : &role_name_30;
}

/* ---------- 主界面初始化 ----------
 * 功能：创建全部静态 UI（屏幕黑底、左立牌、底部角色面板、隐藏态横幅）。
 *       函数内部全程持 LVGL 锁（由 main.c 在 LVGL 任务外调用）。
 * 可调整内容：
 *   - role_name_text：角色名初始文本（main.c 传 NULL → 隐藏；播放器就绪
 *     后由 mjpeg_player 按 ui_text.txt 的 op_name 调 set_role 揭示）。
 *   - 左立牌文本：由用户 ui_text.txt 配置（无固定默认）；Bebas 字库为
 *     ASCII 全量生成，任何英文/数字组合均有字形（中文不支持）。
 *   - 字号阶梯 56/48/40：与 gen_fonts.py 的 BEBAS_SIZES 一一对应，
 *     增删字号要两处同步。 */
void ui_core_screen_init(const char *role_name_text) {
    bsp_lvgl_lock();
    ESP_LOGI(TAG,"1");
    lv_obj_t *scr = lv_scr_act();
    ESP_LOGI(TAG,"scr");
    lv_obj_set_style_bg_color(scr, lv_color_black(), 0); 
    ESP_LOGI(TAG,"scr bgc");
    lv_obj_set_style_bg_opa(scr, LV_OPA_COVER, 0);
    lv_obj_clear_flag(scr, LV_OBJ_FLAG_SCROLLABLE); // 禁止屏幕滚动（leftcol 旋转区超宽，但不应可滑动）
    ESP_LOGI(TAG,"scr bgopa");

    s_leftcol = lv_label_create(scr);
    lv_label_set_text(s_leftcol, "");
    lv_obj_set_size(s_leftcol, 300, 44);
    /* 字形行高 55 > label 高 44：必须放行溢出，否则字形下半（旋转后左侧）被裁 */
    lv_obj_add_flag(s_leftcol, LV_OBJ_FLAG_OVERFLOW_VISIBLE);
    /* 旋转几何 x' = px + py - y（px=py=22），字形 y ∈ [0,line_h] → 左缘 =
     * obj_x + 44 - line_h。x 最终按实际选中字体行高在选字后重算（贴边） */
    lv_obj_set_pos(s_leftcol, 0, -4); // 占位，选字后重定位
    lv_obj_set_style_transform_pivot_x(s_leftcol, 22, 0);
    lv_obj_set_style_transform_pivot_y(s_leftcol, 22, 0);
    lv_obj_set_style_transform_angle(s_leftcol, 900, 0);
    s_brand_label = lv_label_create(s_leftcol);
    lv_label_set_text(s_brand_label, "");   // 占位文本：intro_done 按 ui_text 配置填入
    lv_obj_set_style_text_color(s_brand_label, lv_color_hex(0xFFFFFF), 0);
    lv_obj_align(s_brand_label, LV_ALIGN_TOP_LEFT, 0, 0);

    // 旋转后纵向可用空间 = 标签宽度 300px（屏高 320，余量 20）
    // 尺寸阶梯：实测文本宽度，取能放进 300px 的最大 BebasNeue 字号。
    // 初始按空文本走最大字号定隐藏态几何；真实文本在 intro_done 重测重选
    static const lv_font_t *bebas_sizes[] = {
        &bebas_56, &bebas_48, &bebas_40
    };
    const lv_font_t *bebas = bebas_sizes[2];
    lv_coord_t fit_w = 0;
    for (int i = 0; i < 3; i++) {
        lv_point_t sz;
        lv_txt_get_size(&sz, "", bebas_sizes[i], 0, 0,
                        LV_COORD_MAX, LV_TEXT_FLAG_NONE);
        fit_w = sz.x;
        if (sz.x <= 300) { bebas = bebas_sizes[i]; break; }
    }
    lv_obj_set_style_text_font(s_brand_label, bebas, 0);
    ESP_LOGI(TAG, "leftcol line_height %d, width %d/300",
             (int)lv_font_get_line_height(bebas), (int)fit_w);
    /* 旋转 90°（LVGL: x' = px + py - y, pivot 22/22）后文字竖排：
     * 字形 y ∈ [0, line_h] → 视觉左界 = obj_x + 44 - line_h。
     * 贴边左界=0 → obj_x = line_h - 44（bebas_56→11 / 48→4 / 40→-4）。
     * 视觉横向宽 = line_h（≤55px < 屏宽240），纵向 = 文本宽（阶梯限 ≤300 < 屏高320） */
    lv_obj_set_pos(s_leftcol, (int)lv_font_get_line_height(bebas) - 52, -4);
    lv_obj_add_flag(s_leftcol, LV_OBJ_FLAG_HIDDEN);   // intro 播完才揭示（浮入动效）

    // 底部角色面板 (50px)
    s_footer = lv_obj_create(scr);
    lv_obj_set_size(s_footer, 240, 55);
    lv_obj_align(s_footer, LV_ALIGN_BOTTOM_MID, 0, 0);
    lv_obj_set_style_bg_color(s_footer, lv_color_hex(0x18191C), 0);
    lv_obj_set_style_bg_opa(s_footer, LV_OPA_0, 0);
    lv_obj_set_style_radius(s_footer, 0, 0);
    lv_obj_set_style_border_width(s_footer, 0, 0);
    lv_obj_set_style_outline_width(s_footer, 0, 0);
    lv_obj_clear_flag(s_footer, LV_OBJ_FLAG_SCROLLABLE); /* 固定横幅，禁止内部拖动/滚动 */
    lv_obj_clear_flag(s_footer, LV_OBJ_FLAG_SCROLL_CHAIN);
    lv_obj_add_flag(s_footer, LV_OBJ_FLAG_HIDDEN);    // intro 播完才揭示（闪动动效）

    s_role_name = lv_label_create(s_footer);
    if (role_name_text && role_name_text[0]) {
        char disp[ROLE_NAME_MAX * 2];
        role_text_display(role_name_text, disp, sizeof(disp));
        lv_label_set_text(s_role_name, disp);
        lv_obj_clear_flag(s_role_name, LV_OBJ_FLAG_HIDDEN);
    } else {
        /* 未配置名称：初始隐藏，由 set_role / intro_done 按配置揭示 */
        lv_label_set_text(s_role_name, "");
        lv_obj_add_flag(s_role_name, LV_OBJ_FLAG_HIDDEN);
    }
    lv_obj_set_style_text_color(s_role_name, lv_color_hex(0xFFFFFF), 0);
    lv_obj_set_style_text_font(s_role_name, role_font_30(), 0);
    // 右移避开左侧旋转立牌（带宽 44px + 8px 间隔）
    lv_obj_align(s_role_name, LV_ALIGN_BOTTOM_LEFT, -5, -7);

    s_sub_text = lv_label_create(s_footer);
    lv_label_set_text(s_sub_text, "");
    lv_obj_add_flag(s_sub_text, LV_OBJ_FLAG_HIDDEN);   // 有配置时 intro_done 揭示
    lv_obj_set_style_text_color(s_sub_text, lv_color_hex(0xFFFFFF), 0);
    lv_obj_set_style_text_font(s_sub_text, &lv_font_montserrat_10, 0);
    lv_obj_align(s_sub_text, LV_ALIGN_BOTTOM_LEFT, -3, 8);

    // 底部名片横幅（收到 ESP-NOW 名片时显示 5 秒）
    s_banner = lv_obj_create(scr);
    lv_obj_set_size(s_banner, BANNER_W, BANNER_H);
    /* 注意：不能用 lv_obj_align —— 它会以「样式」方式存下对齐关系，
     * lv_obj_refr_pos 每次刷新都会重新套用（动画每帧 set_y 后又被
     * 加回 ph-h 偏移），导致横幅越滑越偏（实测 +230px 漂移）。
     * 用 lv_obj_set_pos 写死坐标（align 样式保持默认 TOP_LEFT），
     * 刷新时原样套用坐标，动画才能自由控制 y。 */
    lv_obj_set_pos(s_banner, 0, lv_obj_get_height(scr) - BANNER_H);
    lv_obj_set_style_bg_color(s_banner, lv_color_hex(0x18191C), 0);
    lv_obj_set_style_bg_opa(s_banner, BANNER_BG_OPA, 0);
    lv_obj_set_style_radius(s_banner, 0, 0);
    lv_obj_set_style_border_width(s_banner, 0, 0);
    lv_obj_set_style_outline_width(s_banner, 0, 0);
    /* 落定/浮出 y 依据屏幕高度计算（240x320 → 落定 230、屏底 320）。
     * 不依赖 lv_obj_get_y(s_banner)：横幅此刻刚 HIDDEN，对齐坐标在某些
     * LVGL 路径下尚未落盘（实测曾量到 y=0，导致浮出终点错成 90）。 */
    s_banner_end_y = lv_obj_get_height(scr) - BANNER_H; /* 落定 y（= 屏高 - BANNER_H） */
    s_banner_hide_y = lv_obj_get_height(scr); /* 浮出终点 = 屏底（横幅完全出屏） */
    ESP_LOGI(TAG, "banner geom: align_y %d, scr %dx%d, end %d, hide %d",
             (int)lv_obj_get_y(s_banner), (int)lv_obj_get_width(scr),
             (int)lv_obj_get_height(scr), (int)s_banner_end_y, (int)s_banner_hide_y);
    lv_obj_add_flag(s_banner, LV_OBJ_FLAG_HIDDEN);
    lv_obj_clear_flag(s_banner, LV_OBJ_FLAG_SCROLLABLE); /* 固定横幅，禁止内部拖动/滚动 */
    lv_obj_clear_flag(s_banner, LV_OBJ_FLAG_SCROLL_CHAIN);

    s_banner_img = lv_img_create(s_banner);
    /* LVGL 默认图片样式带 5px 内边距，会把人像内容再往里推 5px；
       清零后 align 的 x=5 才是真正的"距左缘 5px" */
    lv_obj_set_style_pad_all(s_banner_img, 0, 0);
    lv_obj_align(s_banner_img, LV_ALIGN_LEFT_MID, -6, 0); /* 头像距左缘 5px */

    s_banner_name = lv_label_create(s_banner);
    lv_obj_set_style_text_color(s_banner_name, lv_color_hex(0xFFFFFF), 0);
    // 横幅角色名默认 14px（最小档）；实际字号在 show_banner 里按
    // 文本宽度走 30/24/18/14 阶梯（可用区 160px，至多 10 字不超界）
    lv_obj_set_style_text_font(s_banner_name, &role_name_14, 0);
    lv_obj_set_width(s_banner_name, 160);
    lv_label_set_long_mode(s_banner_name, LV_LABEL_LONG_DOT);
    lv_obj_align(s_banner_name, LV_ALIGN_TOP_LEFT, 61, 6);

    s_banner_uid = lv_label_create(s_banner);
    lv_obj_set_style_text_color(s_banner_uid, lv_color_hex(0x9AA0A6), 0);
    lv_obj_set_style_text_font(s_banner_uid, &uid_12, 0);
    lv_obj_set_width(s_banner_uid, 160);
    lv_label_set_long_mode(s_banner_uid, LV_LABEL_LONG_DOT);
    lv_obj_align(s_banner_uid, LV_ALIGN_BOTTOM_LEFT, 61, -10);

    /* 屏幕中央状态提示（默认 HIDDEN；播放器无卡/无角色时 set_hint 显示） */
    s_hint = lv_label_create(scr);
    lv_label_set_text(s_hint, "");
    lv_obj_set_style_text_color(s_hint, lv_color_hex(0xFFFFFF), 0);
    lv_obj_set_style_text_font(s_hint, &hint_24, 0);
    lv_obj_align(s_hint, LV_ALIGN_CENTER, 0, 0);
    lv_obj_add_flag(s_hint, LV_OBJ_FLAG_HIDDEN);

    bsp_lvgl_unlock();

    /* 补显未就绪期间缓存的横幅（UI 已就绪；show_banner 内部自锁） */
    if (s_pending_banner.valid) {
        pending_banner_t *p = &s_pending_banner;
        ui_core_screen_show_banner(p->name, p->uname, p->uid, p->payload, p->payload_len,
                                   p->avatar_len, p->portrait_len, p->sticker_cnt,
                                   p->sticker_lens);
        heap_caps_free(p->payload);
        p->valid = false;
    }
}

/* ---------- 更新底部面板角色名 ----------
 * 功能：播放器在角色文本（ui_text.txt op_name）就绪后调用（core0 播放器
 *       任务，非 LVGL 线程，故内部持锁）。文本原样显示（不做文件夹名
 *       变形）；name 为 NULL/空串 = 未配置 → 标签隐藏（元素不显示）。 */
void ui_core_screen_set_role(const char *name) {
    if (s_role_name == NULL) {
        return;
    }
    bsp_lvgl_lock();
    if (name == NULL || name[0] == '\0') {
        lv_obj_add_flag(s_role_name, LV_OBJ_FLAG_HIDDEN);
    } else {
        lv_label_set_text(s_role_name, name);
        lv_obj_clear_flag(s_role_name, LV_OBJ_FLAG_HIDDEN);
        lv_obj_set_style_text_font(s_role_name, role_font_30(), 0);
    }
    bsp_lvgl_unlock();
}

/* ---------- 横幅名称字体选择（show_banner 与 refresh_fonts 共用） ----------
 * 功能：实测文本宽度，取能放进 160px 可用区（240 屏宽 - 头像区 76）
 *       的最大字号。角色名：SD 字库已加载用 {30,16} 两档（GB2312 全集，
 *       支持中文）；未加载回退内嵌 {30,24,18,14} 四档（仅 ASCII）。
 *       用户名：ASCII 字库 {18,14} 两档（18px 约 8 字符，更长落 14px）。 */
static const lv_font_t *pick_banner_font(const char *text, bool use_uname_font) {
    const lv_font_t *font_list[4];
    int font_cnt;
    bool has_utf8 = false;
    for (const char *p = text; p && *p; p++) {
        if ((unsigned char)*p >= 0x80) {
            has_utf8 = true;
            break;
        }
    }
    if (has_utf8) {
        /* 文本含非 ASCII（中文用户名/角色名）：
         * 横幅直接强制 SD 16px 字库（30px RLE 个别字形解码歧义曾导致花字，
         * 16px 全字正常）；未加载时 hint_24 兜底。 */
        if (sd_fonts_loaded()) {
            return sd_fonts_role(SD_FONT_ROLE_SMALL);
        }
        return &hint_24;
    } else if (use_uname_font) {
        font_list[0] = &uname_18;
        font_list[1] = &uname_14;
        font_cnt = 2;
    } else if (sd_fonts_loaded()) {
        font_list[0] = sd_fonts_role(SD_FONT_ROLE_LARGE);
        font_list[1] = sd_fonts_role(SD_FONT_ROLE_SMALL);
        font_cnt = 2;
    } else {
        font_list[0] = &role_name_30;
        font_list[1] = &role_name_24;
        font_list[2] = &role_name_18;
        font_list[3] = &role_name_14;
        font_cnt = 4;
    }
    const lv_font_t *banner_font = font_list[font_cnt - 1];
    for (int i = 0; i < font_cnt; i++) {
        lv_point_t sz;
        lv_txt_get_size(&sz, text, font_list[i], 0, 0, LV_COORD_MAX, LV_TEXT_FLAG_NONE);
        if (sz.x <= 160) {
            banner_font = font_list[i];
            break;
        }
    }
    return banner_font;
}

/* ---------- 刷新字体（SD 字库后台加载完成后调用） ----------
 * 功能：角色名标签 + 当前可见横幅的名字标签重选字体，让中文立即生效。
 *       任何线程可调（内部有 LVGL 锁）。 */
void ui_core_screen_refresh_fonts(void) {
    if (s_role_name == NULL) {
        return;
    }
    bsp_lvgl_lock();
    lv_obj_set_style_text_font(s_role_name, role_font_30(), 0);
    if (s_banner != NULL && !lv_obj_has_flag(s_banner, LV_OBJ_FLAG_HIDDEN)) {
        const char *txt = lv_label_get_text(s_banner_name);
        lv_obj_set_style_text_font(s_banner_name,
                                   pick_banner_font(txt, s_banner_use_uname), 0);
    }
    bsp_lvgl_unlock();
}

/* ---------- 揭示闪动链回调 ----------
 * 功能：按 s_reveal_steps 事件表推进：前 REVEAL_ROLE_STEPS 拍翻 role_name
 *       透明度（7 次闪烁共 1s，第 13 拍=最终常亮），随后 7 拍翻
 *       sub_text（4 次闪烁共 0.5s，末拍=最终常亮）；第 13 拍同时把
 *       sub_text 切灭（其第 1 次闪烁的"暗"从此刻开始）。
 *       每次回调后把定时器周期改成下一拍间隔（拍间间隔不等）。
 * 注意：定时器在 intro_done 里以 repeat_count=REVEAL_TOTAL 创建，触发
 *       完 LVGL 自删，勿在此回调里 lv_timer_del(timer)（悬垂访问）。 */
static void reveal_flash_cb(lv_timer_t *timer)
{
    if (s_flash_step >= REVEAL_TOTAL) {
        return;   /* 触发完（LVGL 按 repeat_count 自删），保险兜底 */
    }
    bool role_phase = (s_flash_step < REVEAL_ROLE_STEPS);
    lv_obj_t *target = role_phase ? s_role_name : s_sub_text;
    lv_obj_set_style_text_opa(target,
        s_reveal_steps[s_flash_step].on ? LV_OPA_COVER : LV_OPA_TRANSP, 0);
    if (s_flash_step == REVEAL_ROLE_STEPS - 1) {
        /* role_name 最终常亮：sub_text 同步进入其第 1 次闪烁的"暗" */
        lv_obj_set_style_text_opa(s_sub_text, LV_OPA_TRANSP, 0);
    }
    lv_timer_set_period(timer, s_reveal_steps[s_flash_step].delay_ms);
    s_flash_step++;
    if (s_flash_step == REVEAL_TOTAL) {
        lv_obj_set_style_text_opa(s_role_name, LV_OPA_COVER, 0);
        lv_obj_set_style_text_opa(s_sub_text, LV_OPA_COVER, 0);
        s_flash_timer = NULL;
    }
}

/* ---------- intro 播完后揭示 UI（leftcol 浮入 + footer 闪动链） ----------
 * 功能：播放器在 intro 播完（或直接 loop）的首帧发布后调用一次（幂等）。
 *       文本已在播放器侧就绪（角色 ui_text.txt 于 intro 加载前读好），
 *       这里只做显示。**未配置/空串的元素不显示**（无内置默认文案）：
 *       leftcol 有文本 → Bebas 56/48/40 阶梯重选字体 → 清 HIDDEN →
 *       激进缓出浮入（1.5s）；footer 清 HIDDEN → role_name 闪 7 次共 1s
 *       （间隔递减，与 leftcol 同时开始）→ sub_text 闪 4 次共 0.5s →
 *       固定（角色名/副标题未配置时其标签保持隐藏，闪动链照常但不可见）。
 *       任何线程可调（内部有 LVGL 锁）。 */
void ui_core_screen_intro_done(const char *leftcol_text, const char *sub_text) {
    if (s_leftcol == NULL || s_revealed) {
        return;
    }
    bsp_lvgl_lock();
    s_revealed = true;

    /* leftcol：仅当配置了文本且开关开启才显示（否则保持隐藏）。 */
    bool has_lb = (leftcol_text != NULL && leftcol_text[0] != '\0');
    has_lb = has_lb && ui_options_on(UIOPT_LEFTCOL);
    if (has_lb) {
        const char *lb = leftcol_text;
        lv_label_set_text(s_brand_label, lb);
        static const lv_font_t *bebas_sizes[] = {
            &bebas_56, &bebas_48, &bebas_40
        };
        const lv_font_t *bebas = bebas_sizes[2];
        for (int i = 0; i < 3; i++) {
            lv_point_t sz;
            lv_txt_get_size(&sz, lb, bebas_sizes[i], 0, 0, LV_COORD_MAX, LV_TEXT_FLAG_NONE);
            if (sz.x <= 300) { bebas = bebas_sizes[i]; break; }
        }
        lv_obj_set_style_text_font(s_brand_label, bebas, 0);
        ESP_LOGI(TAG, "intro done: leftcol \"%s\" (font h %d)", lb,
                 (int)lv_font_get_line_height(bebas));
        lv_obj_clear_flag(s_leftcol, LV_OBJ_FLAG_HIDDEN);

        /* 浮入动效：x 由 -40 激进缓出到贴边落定位，1.5s 完成。
         * 起点几何：立牌旋转 90° 后视觉左界 = pos + 44 - line_h，
         * pos=-40 时左界 = 4 - line_h 在屏外；右界 = pos + 44 = 4px 已入屏——
         * 与 role_name 闪动同步推进；
         * 落定位 pos = line_h - 44：视觉左界 = 0px（紧贴左缘，随字号自适应）；
         * 自定义曲线 1-(1-u)^3：前段几乎全速推进，后段急剧减速 */
        lv_anim_t a;
        lv_anim_init(&a);
        lv_anim_set_var(&a, s_leftcol);
        lv_anim_set_values(&a, -40, (int)lv_font_get_line_height(bebas) - 52);
        lv_anim_set_time(&a, 1500);
        lv_anim_set_exec_cb(&a, leftcol_anim_exec);
        lv_anim_set_path_cb(&a, leftcol_path_aggressive);
        lv_anim_start(&a);
    }

    /* footer：副标题按配置显隐（未配置 → 空串且隐藏），容器揭示后由
     * 闪动链点亮角色名/副标题。role_name 第 1 次闪烁的"暗"从揭示瞬间
     * 开始——与 leftcol 浮入同时引入。
     * UI 开关参与判定：角色名/副标题开关各自控制对应标签；
     * 两者都不可见时整条底部面板保持隐藏。 */
    bool has_sub = (sub_text != NULL && sub_text[0] != '\0');
    has_sub = has_sub && ui_options_on(UIOPT_SUB_TEXT);
    lv_label_set_text(s_sub_text, has_sub ? sub_text : "");

    bool opt_name = ui_options_on(UIOPT_OP_NAME);
    bool name_txt = lv_label_get_text(s_role_name)[0] != '\0';
    bool footer_show = (opt_name && name_txt) || has_sub;
    if (!footer_show) {
        lv_obj_add_flag(s_footer, LV_OBJ_FLAG_HIDDEN);
        bsp_lvgl_unlock();
        return;
    }
    if (opt_name && name_txt) {
        lv_obj_clear_flag(s_role_name, LV_OBJ_FLAG_HIDDEN);
    } else {
        lv_obj_add_flag(s_role_name, LV_OBJ_FLAG_HIDDEN);
    }
    if (has_sub) {
        lv_obj_clear_flag(s_sub_text, LV_OBJ_FLAG_HIDDEN);
    }
    lv_obj_clear_flag(s_footer, LV_OBJ_FLAG_HIDDEN);

    s_flash_step = 0;
    lv_obj_set_style_text_opa(s_role_name, LV_OPA_TRANSP, 0);   /* 暗起手（帧0） */
    /* sub_text 同样暗起手：footer 揭示瞬间不可见，role_name 第 13 拍
     * （最终常亮）起才按事件表从暗开始闪动——否则 LVGL 默认 COVER，
     * footer 一出现副标题就亮着，观感是"从可见状态开始闪" */
    lv_obj_set_style_text_opa(s_sub_text, LV_OPA_TRANSP, 0);
    s_flash_timer = lv_timer_create(reveal_flash_cb,
                                    s_reveal_steps[0].delay_ms, NULL);
    if (s_flash_timer != NULL) {
        lv_timer_set_repeat_count(s_flash_timer, REVEAL_TOTAL);
    }

    bsp_lvgl_unlock();
}

/* ---------- 显示名片横幅 ----------
 * 功能：ESP-NOW 收到名片后由 main.c 的 on_card_received 调用（NFC 线程，
 *       非 LVGL 线程，故内部持锁）。payload = 头像(8192B) + 表情包们
 *       依次拼接；校验各段长度，非法段跳过。
 * 可调整内容：显示时长/轮换见文件头宏；头像回退优先级为用户头像 >
 *       对端角色头图(portrait_len) > 内置占位 default_avatar_565（改图需
 *       重跑 tools/gen_default_avatar.py）。 */
void ui_core_screen_show_banner(const char *name, const char *uname, const char *uid,
                                const uint8_t *payload, size_t payload_len,
                                uint16_t avatar_len, uint16_t portrait_len,
                                uint8_t sticker_cnt, const uint16_t *sticker_lens)
{
    if (ui_menu_is_open()) {
        return;   /* 菜单打开时抑制横幅（move_foreground 会盖住抽屉） */
    }
    if (!ui_options_on(UIOPT_BANNER)) {
        return;   /* “名片横幅”开关关闭：不显示也不做任何准备 */
    }
    /* 横幅文本含非 ASCII（中文名）且 SD 字库尚未加载：立即按需要加载。
     * 这样横幅永远用完整中文（GB2312）而不是随机回退 hint_24（字符少）。
     * 幂等：sd_fonts_loaded() 为真时直接返回。此调用会阻塞 ~7s
     * （读 1.9MB+解析），仅首次横幅。banner 显示前完成，保证不漏字形。 */
    {
        bool need_utf8 = false;
        const char *t = (uname && uname[0]) ? uname : name;
        for (const char *q = t; q && *q; q++) {
            if ((unsigned char)*q >= 0x80) { need_utf8 = true; break; }
        }
        if (need_utf8 && !sd_fonts_loaded()) {
            char wl[640];
            snprintf(wl, sizeof(wl), "%s %s %s",
                     name ? name : "", uname ? uname : "", uid ? uid : "");
            bool ok = sd_fonts_load_for_ex(wl, NULL, NULL);
            ESP_LOGW(TAG, "banner 触发字库加载：%s", ok ? "就绪" : "失败（将回退 hint_24）");
        }
    }
    if (s_banner == NULL) {
        /* UI 尚未初始化（屏幕仍归裸终端）：缓存全部参数，init 完成后补显 */
        if (!s_pending_banner.valid && payload_len <= CARD_BLOB_MAX && payload != NULL) {
            memset(&s_pending_banner, 0, sizeof(s_pending_banner));
            s_pending_banner.valid = true;
            if (name)  snprintf(s_pending_banner.name,  sizeof(s_pending_banner.name),  "%s", name);
            if (uname) snprintf(s_pending_banner.uname, sizeof(s_pending_banner.uname), "%s", uname);
            if (uid)   snprintf(s_pending_banner.uid,   sizeof(s_pending_banner.uid),   "%s", uid);
            s_pending_banner.payload_len  = payload_len;
            s_pending_banner.avatar_len   = avatar_len;
            s_pending_banner.portrait_len = portrait_len;
            s_pending_banner.sticker_cnt  = sticker_cnt;
            for (int i = 0; i < sticker_cnt && i < CARD_STICKER_MAX; i++) {
                s_pending_banner.sticker_lens[i] = sticker_lens ? sticker_lens[i] : 0;
            }
            s_pending_banner.payload = heap_caps_malloc(payload_len,
                                                        MALLOC_CAP_SPIRAM | MALLOC_CAP_8BIT);
            if (s_pending_banner.payload != NULL) {
                memcpy(s_pending_banner.payload, payload, payload_len);
                ESP_LOGW(TAG, "banner before UI ready: cached, replay after init");
            } else {
                s_pending_banner.valid = false;
                ESP_LOGW(TAG, "banner before UI ready: cache malloc failed, dropped");
            }
        } else {
            ESP_LOGW(TAG, "banner before UI ready: cache busy/oversize, dropped");
        }
        return;
    }
    bsp_lvgl_lock();

    /* payload 布局：角色头图(portrait_len) + 用户头像(avatar_len) + 表情包。
     * 头像显示优先级：用户头像 > 角色头图 > 内置占位（三者都没有配置）。 */
    const uint8_t *avatar_src = NULL;
    uint32_t off = (uint32_t)portrait_len + avatar_len;
    if (avatar_len == CARD_AVATAR_SIZE && (uint32_t)portrait_len + avatar_len <= payload_len) {
        avatar_src = payload + portrait_len;   /* 用户头像 */
    } else if (portrait_len == CARD_AVATAR_SIZE && portrait_len <= payload_len) {
        avatar_src = payload;                  /* 角色头图 */
    }
    if (avatar_src != NULL) {
        memcpy(s_banner_avatar, avatar_src, CARD_AVATAR_SIZE);
        swap565_inplace(s_banner_avatar, CARD_AVATAR_SIZE);   /* LE → LVGL SWAP */
    } else {
        memcpy(s_banner_avatar, default_avatar_565, sizeof(default_avatar_565));
        swap565_inplace(s_banner_avatar, sizeof(default_avatar_565));   /* LE 原值 → SWAP */
    }
    s_banner_count = 1;
    s_banner_dsc[0].header.w = 64;
    s_banner_dsc[0].header.h = 64;
    s_banner_dsc[0].header.cf = LV_IMG_CF_TRUE_COLOR;
    s_banner_dsc[0].data_size = CARD_AVATAR_SIZE;
    s_banner_dsc[0].data = (const uint8_t *)s_banner_avatar;

    if (sticker_cnt > 3) {
        sticker_cnt = 3;
    }
    for (int i = 0; i < sticker_cnt; i++) {
        uint16_t len = sticker_lens ? sticker_lens[i] : 0;
        if (len == CARD_AVATAR_SIZE && off + len <= payload_len) {
            memcpy(s_banner_stickers[i], payload + off, len);
            swap565_inplace(s_banner_stickers[i], len);   /* LE → LVGL SWAP */
            s_banner_dsc[s_banner_count].header.w = 64;
            s_banner_dsc[s_banner_count].header.h = 64;
            s_banner_dsc[s_banner_count].header.cf = LV_IMG_CF_TRUE_COLOR;
            s_banner_dsc[s_banner_count].data_size = len;
            s_banner_dsc[s_banner_count].data = (const uint8_t *)s_banner_stickers[i];
            s_banner_count++;
        }
        off += len;
    }
    s_banner_idx = 0;
    lv_img_set_src(s_banner_img, &s_banner_dsc[0]);

    /* 横幅名称优先级：对端用户名(可配置) > 对端当前显示角色名。
     * 用户名用 ASCII 字库阶梯（uname_18/14），角色名用 role_name 阶梯。 */
    const char *disp_name = (uname && uname[0] != '\0') ? uname : name;
    const bool use_uname_font = (uname && uname[0] != '\0');
    s_banner_use_uname = use_uname_font;

    lv_label_set_text(s_banner_name, disp_name ? disp_name : "");
    const lv_font_t *bd_font = pick_banner_font(lv_label_get_text(s_banner_name),
                                                use_uname_font);
    lv_obj_set_style_text_font(s_banner_name, bd_font, 0);
    ESP_LOGW(TAG, "banner font: txt=[%s] font=%s", disp_name ? disp_name : "?",
             (bd_font == sd_fonts_role(SD_FONT_ROLE_LARGE)) ? "SD_L" :
             (bd_font == sd_fonts_role(SD_FONT_ROLE_SMALL)) ? "SD_S" :
             (bd_font == &hint_24) ? "HINT24" : "OTHER");
    {
        const lv_font_t *f30 = sd_fonts_role(SD_FONT_ROLE_LARGE);
        const lv_font_t *f16 = sd_fonts_role(SD_FONT_ROLE_SMALL);
        const lv_font_fmt_txt_dsc_t *d30 = (const lv_font_fmt_txt_dsc_t *)(f30 ? f30->dsc : NULL);
        const lv_font_fmt_txt_dsc_t *d16 = (const lv_font_fmt_txt_dsc_t *)(f16 ? f16->dsc : NULL);
        if (d30) {
            ESP_LOGW(TAG, "fnt30: bl=%d lh=%d bpp=%d cmaps=%u fmt=%u kern=%u subpx=%d line=%d desc=%d",
                     f30->base_line, f30->line_height, d30->bpp,
                     d30->cmap_num, d30->bitmap_format, d30->kern_classes,
                     f30->subpx, f30->underline_position, f30->underline_thickness);
        }
        if (d16) {
            ESP_LOGW(TAG, "fnt16: bl=%d lh=%d bpp=%d cmaps=%u fmt=%u kern=%u subpx=%d line=%d desc=%d",
                     f16->base_line, f16->line_height, d16->bpp,
                     d16->cmap_num, d16->bitmap_format, d16->kern_classes,
                     f16->subpx, f16->underline_position, f16->underline_thickness);
        }
    }
lv_label_set_text(s_banner_uid, uid ? uid : "");

    lv_obj_clear_flag(s_banner, LV_OBJ_FLAG_HIDDEN);
    lv_obj_move_foreground(s_banner);

    // 清掉旧的隐藏/轮换定时器与未完成的入场/出场动效（共用
    // banner_anim_exec，一次 lv_anim_del 全部清掉），重新开始
    if (s_banner_timer != NULL) {
        lv_timer_del(s_banner_timer);
        s_banner_timer = NULL;
    }
    if (s_banner_rotate != NULL) {
        lv_timer_del(s_banner_rotate);
        s_banner_rotate = NULL;
    }
    lv_anim_del(s_banner, banner_anim_exec);

    // 从下方浮入：y 由 s_banner_hide_y（完全在屏外）缓出到落定位，
    // 动效结束后 ready 回调才开始 5 秒静止显示计时
    lv_anim_t a;
    lv_anim_init(&a);
    lv_anim_set_var(&a, s_banner);
    lv_anim_set_values(&a, s_banner_hide_y, s_banner_end_y);
    lv_anim_set_time(&a, BANNER_ANIM_MS);
    lv_anim_set_exec_cb(&a, banner_anim_exec);
    lv_anim_set_path_cb(&a, lv_anim_path_ease_out); /* 非线性缓出 */
    lv_anim_set_ready_cb(&a, banner_anim_ready_cb);
    lv_anim_start(&a);

    ESP_LOGI(TAG, "banner: %s / %s (sticker %u)", disp_name ? disp_name : "", uid ? uid : "", sticker_cnt);
    if (disp_name && disp_name[0]) {
        const uint8_t *p = (const uint8_t *)disp_name;
        ESP_LOGW(TAG, "banner name hex: %02x %02x %02x %02x %02x %02x %02x %02x %02x %02x",
                 p[0], p[1], p[2], p[3], p[4], p[5], p[6], p[7], p[8], p[9]);
    }
    bsp_lvgl_unlock();
}

/* ---------- 屏幕中央状态提示 ----------
 * 功能：播放器在无卡/无角色时显示 "请插入SD卡"/"SD卡无内容"；
 *       text=NULL 隐藏。hint_24 为内嵌中文字库（"请插入无内容SD卡 "），
 *       不依赖 SD 字库——无卡场景也能显示。按内容去重：重复设置
 *       相同文本零开销。任何线程可调（内部持锁）。 */
void ui_core_screen_set_hint(const char *text)
{
    if (ui_menu_is_open()) {
        return;   /* 菜单打开时抑制中央提示（面板遮住，无需更新） */
    }
    if (!ui_options_on(UIOPT_HINT)) {
        text = NULL;   /* “中央提示”开关关闭：等同清除（去重/隐藏逻辑同下） */
    }
    if (s_hint == NULL) {
        return;
    }
    if ((text == NULL) == (s_hint_text[0] == '\0') &&
        (text == NULL || strcmp(text, s_hint_text) == 0)) {
        return;   /* 状态未变化 */
    }
    bsp_lvgl_lock();
    if (text != NULL && text[0] != '\0') {
        snprintf(s_hint_text, sizeof(s_hint_text), "%s", text);
        lv_label_set_text(s_hint, s_hint_text);
        lv_obj_clear_flag(s_hint, LV_OBJ_FLAG_HIDDEN);
        lv_obj_move_foreground(s_hint);
    } else {
        s_hint_text[0] = '\0';
        lv_obj_add_flag(s_hint, LV_OBJ_FLAG_HIDDEN);
    }
    bsp_lvgl_unlock();
}

/* ---------- 隐藏全部 UI 元素并重置揭示状态 ----------
 * 功能：热插卡播放 intro 前调用：隐藏 leftcol/footer/sub_text/横幅，
 *       重置 s_revealed/s_flash_timer——intro 播完后 intro_done 重新
 *       触发全套揭示动效（像最开始那样）。s_hint 保持不动（播放器
 *       另行 set_hint(NULL) 清除）。幂等；任何线程可调（内部持锁）。 */
void ui_core_screen_hide_ui(void)
{
    if (s_leftcol == NULL) {
        return;
    }
    bsp_lvgl_lock();
    if (s_flash_timer != NULL) {
        lv_timer_del(s_flash_timer);   /* 闪动链未完成则中止 */
        s_flash_timer = NULL;
    }
    lv_obj_add_flag(s_leftcol, LV_OBJ_FLAG_HIDDEN);
    lv_obj_add_flag(s_footer, LV_OBJ_FLAG_HIDDEN);
    if (s_banner != NULL) {
        lv_obj_add_flag(s_banner, LV_OBJ_FLAG_HIDDEN);
    }
    s_revealed = false;
    bsp_lvgl_unlock();
}

/* ---------- 开关即时生效（“UI 选项”页切换后调用） ----------
 * 功能：按当前开关掩码同步所有 UI 元素的可见性，无需等下次揭示：
 *   关闭 → 立即可见元素立即隐藏；开启 → 若本周期已揭示且仍有文本则恢复。
 * 横幅/提示关闭时同步清场（停计时器、不播收起动画）。
 * 注意：LVGL 任务在 lv_timer_handler 外围已持 bsp_lvgl_lock（非递归），
 *   本函数只允许从 LVGL 上下文调用（菜单切换按钮事件），**不可自锁**。
 * 空文本元素本就隐藏（空=不显示）；开关只影响“有内容”的元素。 */
void ui_core_screen_apply_show_opts(void)
{
    if (s_leftcol == NULL) {
        return;
    }

    /* 左侧立牌文字 */
    if (ui_options_on(UIOPT_LEFTCOL)) {
        if (s_revealed && lv_label_get_text(s_brand_label)[0] != '\0') {
            lv_obj_clear_flag(s_leftcol, LV_OBJ_FLAG_HIDDEN);
        }
    } else {
        lv_obj_add_flag(s_leftcol, LV_OBJ_FLAG_HIDDEN);
    }

    /* 底部面板：角色名/副标题按开关+现有文本恢复或隐藏；两者都不可见
     * 时整条面板隐藏（与 intro_done 判定一致） */
    bool name_ok = ui_options_on(UIOPT_OP_NAME) &&
                   lv_label_get_text(s_role_name)[0] != '\0';
    bool sub_ok  = ui_options_on(UIOPT_SUB_TEXT) &&
                   lv_label_get_text(s_sub_text)[0] != '\0';
    if (name_ok) {
        lv_obj_clear_flag(s_role_name, LV_OBJ_FLAG_HIDDEN);
    } else {
        lv_obj_add_flag(s_role_name, LV_OBJ_FLAG_HIDDEN);
    }
    if (sub_ok) {
        lv_obj_clear_flag(s_sub_text, LV_OBJ_FLAG_HIDDEN);
    } else {
        lv_obj_add_flag(s_sub_text, LV_OBJ_FLAG_HIDDEN);
    }
    if (s_revealed && (name_ok || sub_ok)) {
        lv_obj_clear_flag(s_footer, LV_OBJ_FLAG_HIDDEN);
    } else {
        lv_obj_add_flag(s_footer, LV_OBJ_FLAG_HIDDEN);
    }

    /* 名片横幅：关闭 → 立即清场 */
    if (!ui_options_on(UIOPT_BANNER) && s_banner != NULL &&
        !lv_obj_has_flag(s_banner, LV_OBJ_FLAG_HIDDEN)) {
        if (s_banner_timer != NULL) {
            lv_timer_del(s_banner_timer);
            s_banner_timer = NULL;
        }
        if (s_banner_rotate != NULL) {
            lv_timer_del(s_banner_rotate);
            s_banner_rotate = NULL;
        }
        lv_anim_del(s_banner, banner_anim_exec);
        lv_obj_add_flag(s_banner, LV_OBJ_FLAG_HIDDEN);
        ESP_LOGI(TAG, "banner hidden by UI option");
    }

    /* 中央提示：关闭 → 立即清除 */
    if (!ui_options_on(UIOPT_HINT) && !lv_obj_has_flag(s_hint, LV_OBJ_FLAG_HIDDEN)) {
        s_hint_text[0] = '\0';
        lv_obj_add_flag(s_hint, LV_OBJ_FLAG_HIDDEN);
    }
}


/* ---------- UI 字体引用切回内嵌 ----------
 * 功能：卸载 SD 字库（sd_fonts_unload，腾 PSRAM 给 intro）前必须先调
 *       本函数，否则 s_role_name/s_banner_name 仍指向将被释放的字体。
 *       SD 字库重载后 refresh_fonts 自动恢复。幂等；任何线程可调。 */
void ui_core_screen_fonts_fallback(void)
{
    if (s_role_name == NULL) {
        return;
    }
    bsp_lvgl_lock();
    lv_obj_set_style_text_font(s_role_name, &role_name_30, 0);
    if (s_banner != NULL && !lv_obj_has_flag(s_banner, LV_OBJ_FLAG_HIDDEN)) {
        const char *txt = lv_label_get_text(s_banner_name);
        const lv_font_t *f = &role_name_14;
        static const lv_font_t *embedded[4] = {
            &role_name_30, &role_name_24, &role_name_18, &role_name_14
        };
        for (int i = 0; i < 4; i++) {
            lv_point_t sz;
            lv_txt_get_size(&sz, txt, embedded[i], 0, 0, LV_COORD_MAX, LV_TEXT_FLAG_NONE);
            if (sz.x <= 160) { f = embedded[i]; break; }
        }
        lv_obj_set_style_text_font(s_banner_name, f, 0);
    }
    bsp_lvgl_unlock();
}

/* ---------- 顶部系统提示横幅（SD 自检提示：字库缺失/配置目录重建） ----------
 * 功能：屏幕顶部全宽 44px 暗条 + 两行纯 ASCII 文本（内嵌 montserrat，
 *   无字库卡也能渲染——中文提示在缺字库时只会出方块，故用英文）；
 *   显示 ms 毫秒后自动隐藏；再次调用刷新文本并重置计时。
 * 任意任务可调（内部持 LVGL 锁）。与名片横幅（底部）、中央提示（hint）独立。 */
static lv_obj_t *s_notice = NULL;
static lv_obj_t *s_notice_l1 = NULL;
static lv_obj_t *s_notice_l2 = NULL;
static lv_timer_t *s_notice_timer = NULL;
static bool s_notice_visible = false;

static void notice_timer_cb(lv_timer_t *tm)
{
    (void)tm;
    if (s_notice_visible && s_notice != NULL) {
        s_notice_visible = false;
        lv_obj_add_flag(s_notice, LV_OBJ_FLAG_HIDDEN);
        if (s_notice_timer != NULL) {
            lv_timer_pause(s_notice_timer);
        }
    }
}

void ui_core_screen_notice(const char *line1, const char *line2, uint32_t ms)
{
    bsp_lvgl_lock();
    if (s_notice == NULL) {
        s_notice = lv_obj_create(lv_layer_top());
        lv_obj_set_size(s_notice, 240, 44);
        lv_obj_set_pos(s_notice, 0, 0);
        lv_obj_clear_flag(s_notice, LV_OBJ_FLAG_SCROLLABLE);
        lv_obj_set_style_bg_color(s_notice, lv_color_hex(0x101418), 0);
        lv_obj_set_style_bg_opa(s_notice, LV_OPA_COVER, 0);
        lv_obj_set_style_border_color(s_notice, lv_color_hex(0xD8B45C), 0);
        lv_obj_set_style_border_width(s_notice, 1, 0);
        lv_obj_set_style_radius(s_notice, 0, 0);
        lv_obj_set_style_pad_all(s_notice, 0, 0);
        lv_obj_add_flag(s_notice, LV_OBJ_FLAG_HIDDEN);

        s_notice_l1 = lv_label_create(s_notice);
        lv_obj_set_width(s_notice_l1, 232);
        lv_obj_set_pos(s_notice_l1, 4, 3);
        lv_label_set_long_mode(s_notice_l1, LV_LABEL_LONG_DOT);
        lv_obj_set_style_text_font(s_notice_l1, &lv_font_montserrat_14, 0);
        lv_obj_set_style_text_color(s_notice_l1, lv_color_white(), 0);

        s_notice_l2 = lv_label_create(s_notice);
        lv_obj_set_width(s_notice_l2, 232);
        lv_obj_set_pos(s_notice_l2, 4, 25);
        lv_label_set_long_mode(s_notice_l2, LV_LABEL_LONG_DOT);
        lv_obj_set_style_text_font(s_notice_l2, &lv_font_montserrat_14, 0);
        lv_obj_set_style_text_color(s_notice_l2, lv_color_hex(0xD8B45C), 0);

        s_notice_timer = lv_timer_create(notice_timer_cb, 1000, NULL);
        lv_timer_pause(s_notice_timer);
    }
    lv_label_set_text(s_notice_l1, line1 ? line1 : "");
    lv_label_set_text(s_notice_l2, line2 ? line2 : "");
    lv_obj_clear_flag(s_notice, LV_OBJ_FLAG_HIDDEN);
    s_notice_visible = true;
    lv_timer_set_period(s_notice_timer, (ms > 0) ? ms : 8000);
    lv_timer_reset(s_notice_timer);
    lv_timer_resume(s_notice_timer);
    bsp_lvgl_unlock();
}

/* ---------- 电源状态 chip（右上角，USB 充电/低电/即将休眠） ----------
 * text=NULL 隐藏；level 0=绿(正常) 1=黄(警告) 2=红(危急)。
 * chip 建在 lv_layer_top（菜单/名片簿之上）；与 notice 顶部横幅重叠时
 * notice 置顶（其为高优先级临时提示）。任何线程可调（内部持锁）。 */
static lv_obj_t *s_pwr_chip = NULL;
static lv_obj_t *s_pwr_label = NULL;

void ui_core_screen_set_power(const char *text, int level)
{
    bsp_lvgl_lock();

    if (text == NULL || text[0] == '\0') {
        if (s_pwr_chip != NULL) lv_obj_add_flag(s_pwr_chip, LV_OBJ_FLAG_HIDDEN);
        bsp_lvgl_unlock();
        return;
    }

    if (s_pwr_chip == NULL) {
        s_pwr_chip = lv_obj_create(lv_layer_top());
        lv_obj_set_size(s_pwr_chip, LV_SIZE_CONTENT, LV_SIZE_CONTENT);
        lv_obj_align(s_pwr_chip, LV_ALIGN_TOP_RIGHT, -6, 6);
        lv_obj_clear_flag(s_pwr_chip, LV_OBJ_FLAG_SCROLLABLE | LV_OBJ_FLAG_CLICKABLE);
        lv_obj_set_style_bg_color(s_pwr_chip, lv_color_hex(0x101418), 0);
        lv_obj_set_style_bg_opa(s_pwr_chip, LV_OPA_80, 0);
        lv_obj_set_style_radius(s_pwr_chip, 4, 0);
        lv_obj_set_style_border_width(s_pwr_chip, 1, 0);
        lv_obj_set_style_pad_ver(s_pwr_chip, 3, 0);
        lv_obj_set_style_pad_hor(s_pwr_chip, 8, 0);

        s_pwr_label = lv_label_create(s_pwr_chip);
        lv_obj_set_style_text_font(s_pwr_label, &pwr_16, 0);
        lv_obj_set_style_text_color(s_pwr_label, lv_color_white(), 0);
        lv_obj_center(s_pwr_label);
    }

    uint32_t border = (level == 0) ? 0x4CC38A : (level == 1) ? 0xD8B45C : 0xE5534B;
    lv_obj_set_style_border_color(s_pwr_chip, lv_color_hex(border), 0);
    lv_label_set_text(s_pwr_label, text);
    lv_obj_clear_flag(s_pwr_chip, LV_OBJ_FLAG_HIDDEN);

    /* notice 为高优先级提示：可见时保证其在上层 */
    if (s_notice != NULL && !lv_obj_has_flag(s_notice, LV_OBJ_FLAG_HIDDEN)) {
        lv_obj_move_foreground(s_notice);
    }
    bsp_lvgl_unlock();
}
