/* ============================================================================
 * bsp_touch.c - XPT2046 电阻触摸驱动（GPIO 模拟 SPI，轮询式）
 *
 * 结构：bit-bang 发 24 位命令取 12 位 ADC → 映射到 240x320 屏幕坐标 →
 *       LVGL 指针输入设备（IRQ 低电平 = 按下）。
 * 为什么不用硬件 SPI：
 *   ESP32-S3 只有 SPI2/SPI3 两条用户总线，SPI2=屏幕，SPI3 已让给 SD 卡
 *   （见 bsp_sdcard.c）。XPT2046 是低速轮询设备（按压时 LVGL 每拍读一
 *   次，每次约 50us），GPIO 模拟 ~500kHz 时钟足够，性能无感知差异。
 * 可调整内容：
 *   - ADC 范围 300~3800：是 XPT2046 的实测有效区间。若触摸点整体偏移
 *     或边缘丢失，微调这两个值（缩小范围=灵敏度区间变窄=线性度更好但
 *     边角更难到达）。y 的 320 映射上下限同理。
 *   - esp_rom_delay_us(1)：模拟时钟的半周期（≈500kHz）。XPT2046 上限
 *     约 2.5MHz，留足余量；想更快可缩短/去掉延时（GPIO 翻转本身约
 *     100ns），但别超 2.5MHz。
 *   - IRQ 拉高（pull_up）：抬起时引脚悬空会误触发，勿删。
 * ==========================================================================*/

// bsp/bsp_touch.c
#include "bsp_touch.h"
#include "driver/gpio.h"
#include "esp_rom_sys.h"
#include "esp_log.h"
#include "nvs.h"
#include "nvs_flash.h"

static const char *TAG = "BSP_TOUCH";

/* 注册的 LVGL 指针设备句柄（bsp_touch_get_indev 返回；手势检测轮询用） */
static lv_indev_t *s_indev = NULL;

/* ---------- 触控范围校准（自动 + NVS 持久化） ----------
 * 原理：XPT2046 raw 有效范围因面板批次/接法而异（代码默认 300~3800 常
 *       与实物不符 → 整体偏移）。运行期持续记录按压 raw 的 min/max，
 *       范围足够大后按实际范围线性映射并存入 NVS，重启自动应用。
 * 操作：装上屏幕后点一下四角/四边，校准即完成（无需重编）。 */
#define TOUCH_CAL_NVS_NS  "touch"
#define TOUCH_CAL_KEY     "cal"
/* 校准数据版本：固件改动 raw 采集/映射算法后递增，旧数据自动作废重校
 * （1→2：xpt2046 位提取修正，旧 raw 范围与新读取不一致，映射必错） */
#define TOUCH_CAL_VERSION  3   /* PCB v6.2 换线序环境：旧校准失效，强制重校准 */
/* 校准有效阈值：跨度须覆盖面板大部分（XPT2046 典型 raw 300~3700，跨度
 * ~3400；要求 ≥2400/3000 可淘汰旧固件残留的中间区域坏数据——只点屏幕
 * 中部时跨度 ~2000 不达标，四角点满后必然达标）。 */
#define TOUCH_CAL_MIN_SPAN_X  2400
#define TOUCH_CAL_MIN_SPAN_Y  3000

/* ---------- 映射修正：轴反转 + 偏移 ----------
 * 显示画布在物理面板上偏右下（诊断框实测：左露黑 10px、上露黑 20px）。
 * 触控校准映射输出面板物理坐标；若面板 raw 轴方向与屏幕相反，则点击
 * 与显示镜像错位——先反转轴方向再偏移。
 * TOUCH_MIRROR_X/Y：1=该轴镜像（点左响应右 / 点上响应下）。
 * TOUCH_OFFSET_X/Y：镜像后的残余偏移（正值=向左上移）。当前 0。
 *
 * 实测 raw 范围（中央点 raw≈1008-1120/1008-1184 ↔ 屏幕中央 120,160，
 * 反推面板 raw 满量程）：X≈200~1900（跨度 1700）、Y≈200~2100（跨度 1900）。
 * 旧默认 300~3800 与实际不符 → 中央点映射偏右下 68/80px（点中下方按钮）。 */
#define TOUCH_MIRROR_X  0   /* 实测 X 轴反了：关闭镜像 */
#define TOUCH_MIRROR_Y  0   /* 实测疑似 Y 镜像反了：关闭 */
#define TOUCH_OFFSET_X  0   /* 偏移方向曾被验证是反的：撤掉，UI 宽度适配画布 */
#define TOUCH_OFFSET_Y (-10)

#define TOUCH_RAW_MIN_X  200
#define TOUCH_RAW_MAX_X  1900
#define TOUCH_RAW_MIN_Y  200
#define TOUCH_RAW_MAX_Y  2100

typedef struct {
    uint16_t x_min, x_max, y_min, y_max;   /* raw 值范围；0,0,0,0=未校准 */
} touch_cal_t;

static touch_cal_t s_cal;
static volatile uint16_t s_cal_taps;   /* 本次开机未校准期间的按压次数 */

static uint16_t xpt2046_read_cmd(uint8_t cmd);

bool bsp_touch_cal_valid(void) {
    return s_cal.x_min != 0 && s_cal.x_max > s_cal.x_min &&
           (uint32_t)(s_cal.x_max - s_cal.x_min) >= TOUCH_CAL_MIN_SPAN_X &&
           (uint32_t)(s_cal.y_max - s_cal.y_min) >= TOUCH_CAL_MIN_SPAN_Y;
}

int bsp_touch_cal_taps(void) {
    return (int)s_cal_taps;
}

void bsp_touch_cal_reset_taps(void) {
    s_cal_taps = 0;
}

void bsp_touch_cal_reset(void) {
    memset(&s_cal, 0, sizeof(s_cal));
    nvs_handle_t h;
    if (nvs_open(TOUCH_CAL_NVS_NS, NVS_READWRITE, &h) == ESP_OK) {
        nvs_erase_key(h, TOUCH_CAL_KEY);
        nvs_commit(h);
        nvs_close(h);
    }
    ESP_LOGI(TAG, "cal reset (re-run corner taps)");
}

static void touch_cal_load(void) {
    memset(&s_cal, 0, sizeof(s_cal));
    nvs_handle_t h;
    if (nvs_open(TOUCH_CAL_NVS_NS, NVS_READONLY, &h) == ESP_OK) {
        uint32_t ver = 0;
        bool stale = (nvs_get_u32(h, "ver", &ver) != ESP_OK || ver != TOUCH_CAL_VERSION);
        nvs_close(h);
        if (stale) {
            /* 旧算法数据：作废并写新版本标记（含引导标记，重校后可再弹） */
            if (nvs_open(TOUCH_CAL_NVS_NS, NVS_READWRITE, &h) == ESP_OK) {
                nvs_erase_key(h, TOUCH_CAL_KEY);
                nvs_erase_key(h, "guided");
                nvs_set_u32(h, "ver", TOUCH_CAL_VERSION);
                nvs_commit(h);
                nvs_close(h);
            }
            ESP_LOGW(TAG, "cal data stale (ver %u != %u), cleared for re-cal",
                     (unsigned)ver, (unsigned)TOUCH_CAL_VERSION);
            return;
        }
        if (nvs_open(TOUCH_CAL_NVS_NS, NVS_READONLY, &h) == ESP_OK) {
            size_t sz = sizeof(s_cal);
            if (nvs_get_blob(h, TOUCH_CAL_KEY, &s_cal, &sz) != ESP_OK) {
                memset(&s_cal, 0, sizeof(s_cal));
            }
            nvs_close(h);
        }
    }
    if (s_cal.x_min != 0 || s_cal.x_max != 0) {
    ESP_LOGI(TAG, "cal loaded: x %u-%u y %u-%u", s_cal.x_min, s_cal.x_max,
             s_cal.y_min, s_cal.y_max);
    }
}

/* 校准引导层"已完成一次"标记：开机弹一次引导，完成即落盘，以后不打扰 */
bool bsp_touch_cal_guided(void) {
    nvs_handle_t h;
    bool guided = false;
    if (nvs_open(TOUCH_CAL_NVS_NS, NVS_READONLY, &h) == ESP_OK) {
        uint8_t v = 0;
        if (nvs_get_u8(h, "guided", &v) == ESP_OK && v != 0) guided = true;
        nvs_close(h);
    }
    return guided;
}

void bsp_touch_cal_mark_guided(void) {
    nvs_handle_t h;
    if (nvs_open(TOUCH_CAL_NVS_NS, NVS_READWRITE, &h) == ESP_OK) {
        nvs_set_u8(h, "guided", 1);
        nvs_commit(h);
        nvs_close(h);
    }
}

/* 校准机制已弃用：固定实测范围映射（TOUCH_RAW_* 宏），s_cal/API 保留
 * 供菜单"触控校准"按钮与兼容调用，不再参与坐标计算。 */

static void touch_read_cb(lv_indev_drv_t * drv, lv_indev_data_t * data) {
    static bool s_last_pressed;   /* 按压沿检测：按下→计数+1；抬起复位 */
    bool now = (gpio_get_level(PIN_NUM_TOUCH_IRQ) == 0);
    if (now) {
        if (!s_last_pressed) {
            s_cal_taps++;
            uint16_t raw_x = xpt2046_read_cmd(0x90);
            uint16_t raw_y = xpt2046_read_cmd(0xD0);
            ESP_LOGD(TAG, "press edge: taps=%u raw=%u,%u",
                     (unsigned)s_cal_taps, (unsigned)raw_x, (unsigned)raw_y);
        }
        s_last_pressed = true;

        uint16_t raw_x = xpt2046_read_cmd(0x90);
        uint16_t raw_y = xpt2046_read_cmd(0xD0);

        /* 固定实测范围线性映射（校准机制已弃用：校准流程无效果，且旧
         * 校准数据会覆盖正确映射；范围见文件头 TOUCH_RAW_* 宏） */
        int32_t x = (int32_t)(raw_x - TOUCH_RAW_MIN_X) * 240 /
                    (TOUCH_RAW_MAX_X - TOUCH_RAW_MIN_X);
        int32_t y = (int32_t)(raw_y - TOUCH_RAW_MIN_Y) * 320 /
                    (TOUCH_RAW_MAX_Y - TOUCH_RAW_MIN_Y);

        /* 轴反转（镜像）：面板 raw 轴方向与屏幕相反时，先反转再偏移 */
        if (TOUCH_MIRROR_X) x = 239 - x;
        if (TOUCH_MIRROR_Y) y = 319 - y;
        /* 映射修正偏移：校准映射输出面板坐标，减去偏移得画布坐标 */
        x -= TOUCH_OFFSET_X;
        y -= TOUCH_OFFSET_Y;

        data->point.x = (lv_coord_t)(x < 0 ? 0 : (x > 239 ? 239 : x));
        data->point.y = (lv_coord_t)(y < 0 ? 0 : (y > 319 ? 319 : y));
        data->state = LV_INDEV_STATE_PRESSED;
    } else {
        s_last_pressed = false;      /* 抬起复位：保证每次按压都能被计数 */
        data->state = LV_INDEV_STATE_RELEASED;
    }
}

/* ---------- GPIO 模拟 SPI 读取一次 ADC ----------
 * 功能：与硬件 SPI 完全一致的模式 0（CPOL=0/CPHA=0）24 位全双工时序：
 *       CS 拉低 → MSB 先发 3 字节（cmd + 2 个哑字节）→ CS 拉高，取回
 *       12 位结果。时钟低电平期间换 DIN 数据，上升沿后 XPT2046 已锁存
 *       DIN 且 DOUT 稳定，此时采样 DO。
 * 可调整内容：循环 24 次对应 8 位命令 + 16 位转换窗口，是 XPT2046 的
 *       固定帧格式，勿改。 */
static uint16_t xpt2046_read_cmd(uint8_t cmd) {
    uint8_t tx_data[3] = {cmd, 0x00, 0x00};
    uint8_t rx_data[3] = {0};

    gpio_set_level(PIN_NUM_TOUCH_CS, 0);
    for (int i = 0; i < 24; i++) {
        gpio_set_level(PIN_NUM_TOUCH_CLK, 0);
        gpio_set_level(PIN_NUM_TOUCH_DIN, (tx_data[i >> 3] >> (7 - (i & 7))) & 0x01);
        esp_rom_delay_us(1);
        gpio_set_level(PIN_NUM_TOUCH_CLK, 1);    // 上升沿：XPT2046 锁存 DIN
        esp_rom_delay_us(1);
        if (gpio_get_level(PIN_NUM_TOUCH_DO)) {  // DOUT 在时钟高电平期间稳定
            rx_data[i >> 3] |= (0x80 >> (i & 7));
        }
    }
    gpio_set_level(PIN_NUM_TOUCH_CLK, 0);
    gpio_set_level(PIN_NUM_TOUCH_CS, 1);         // CS 空闲为高（低有效）

    /* 12 位结果：第 9-16 时钟（rx_data[1]）= 结果 bit11..4；
     * 第 17-20 时钟（rx_data[2] 低 4 位）= 结果 bit3..0 */
    return ((uint16_t)rx_data[1] << 4) | (rx_data[2] & 0x0F);
}

/* ---------- 触控初始化 ----------
 * 功能：IRQ 输入 + CLK/DIN/CS 推挽输出 + DO 输入 + 注册 LVGL 指针设备。
 * 可调整内容：CS 空闲必须为高（低有效选中）、CLK 空闲为低（模式 0），
 *       上电即设好电平，避免第一笔传输前多出一个假时钟沿。 */
void bsp_touch_init(void) {
    ESP_LOGI(TAG, "Initializing XPT2046 Touch via GPIO bit-bang (GPIO 38-42)");

    touch_cal_load();

    gpio_config_t irq_config = {
        .pin_bit_mask = (1ULL << PIN_NUM_TOUCH_IRQ),
        .mode = GPIO_MODE_INPUT,
        .pull_up_en = GPIO_PULLUP_ENABLE,
    };
    gpio_config(&irq_config);

    // CLK / DIN / CS 推挽输出，并立即给空闲电平（CS 高、CLK 低）
    gpio_config_t out_config = {
        .pin_bit_mask = (1ULL << PIN_NUM_TOUCH_CLK) |
                        (1ULL << PIN_NUM_TOUCH_DIN) |
                        (1ULL << PIN_NUM_TOUCH_CS),
        .mode = GPIO_MODE_OUTPUT,
    };
    gpio_config(&out_config);
    gpio_set_level(PIN_NUM_TOUCH_CS, 1);
    gpio_set_level(PIN_NUM_TOUCH_CLK, 0);
    gpio_set_level(PIN_NUM_TOUCH_DIN, 0);

    // DO 由 XPT2046 推挽驱动，纯输入即可（CS 高时高阻，但此时不会被读）
    gpio_config_t do_config = {
        .pin_bit_mask = (1ULL << PIN_NUM_TOUCH_DO),
        .mode = GPIO_MODE_INPUT,
    };
    gpio_config(&do_config);

    static lv_indev_drv_t indev_drv;
    lv_indev_drv_init(&indev_drv);
    indev_drv.type = LV_INDEV_TYPE_POINTER;
    indev_drv.read_cb = touch_read_cb;
    s_indev = lv_indev_drv_register(&indev_drv);
    /* 诊断（开机终端可见）：indev 或 read_timer 为 NULL 即触摸永不采样 */
    ESP_LOGW(TAG, "indev=%p read_timer=%p",
             (void *)s_indev,
             s_indev ? (void *)s_indev->driver->read_timer : NULL);
    /* 100Hz 采样：LVGL indev 默认 33ms 采样粒度太粗（点击判定更敏感） */
    if (s_indev != NULL && s_indev->driver != NULL && s_indev->driver->read_timer != NULL) {
        lv_timer_set_period(s_indev->driver->read_timer, 10);
    }

    ESP_LOGI(TAG, "Touch Controller Loaded!");
}

lv_indev_t *bsp_touch_get_indev(void) {
    return s_indev;
}

/* 直接读 IRQ 电平判断当前是否按压（不依赖 LVGL indev 采样，
 * 供上滑手势等低频手势检测使用） */
bool bsp_touch_pressed(void) {
    return gpio_get_level(PIN_NUM_TOUCH_IRQ) == 0;
}

/* 读取当前触点并映射到屏幕坐标（与 touch_read_cb 相同的固定映射）：
 * 手势检测在按下期间调用，IRQ 非低直接返回 false。 */
bool bsp_touch_point(int16_t *x, int16_t *y) {
    if (gpio_get_level(PIN_NUM_TOUCH_IRQ) != 0) return false;

    uint16_t raw_x = xpt2046_read_cmd(0x90);
    uint16_t raw_y = xpt2046_read_cmd(0xD0);

    int32_t mx = (int32_t)(raw_x - TOUCH_RAW_MIN_X) * 240 /
                 (TOUCH_RAW_MAX_X - TOUCH_RAW_MIN_X);
    int32_t my = (int32_t)(raw_y - TOUCH_RAW_MIN_Y) * 320 /
                 (TOUCH_RAW_MAX_Y - TOUCH_RAW_MIN_Y);

    if (TOUCH_MIRROR_X) mx = 239 - mx;
    if (TOUCH_MIRROR_Y) my = 319 - my;
    mx -= TOUCH_OFFSET_X;
    my -= TOUCH_OFFSET_Y;

    *x = (int16_t)(mx < 0 ? 0 : (mx > 239 ? 239 : mx));
    *y = (int16_t)(my < 0 ? 0 : (my > 319 ? 319 : my));
    return true;
}
