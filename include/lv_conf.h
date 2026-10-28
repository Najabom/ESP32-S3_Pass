/*
 * lv_conf.h - LVGL 配置覆盖层
 *
 * 说明：
 * 本文件只覆盖默认配置中需要修改的项，其余配置项沿用
 * LVGL 库内 lv_conf_internal.h 的默认值。
 *
 * 1) 压缩字体支持：HOS_SC.c 字库由 lv_font_conv 以 RLE 压缩格式生成
 *    （font_dsc.bitmap_format = 1, COMPRESSED）。若 LV_USE_FONT_COMPRESSED
 *    为 0，LVGL 8.4 在渲染时会直接返回 NULL，文字无法显示。
 *
 * 2) RGB565 字节序：ST7789 SPI 屏期望高字节在前，LVGL 渲染时即按该
 *    字节序写入缓冲区，flush 直接发送即可。
 *
 * 3) GIF 动画：启用 LVGL 内置 GIF 解码器（lv_gif）。
 *
 * 4) 自定义内存分配：ESP32-S3 N16R8 有 8MB PSRAM，LVGL 全部内存
 *    从 PSRAM 分配（GIF 解码单帧缓冲需约 1.8MB，默认 48KB 静态堆不够）。
 */
#ifndef LV_CONF_H
#define LV_CONF_H

/* RLE 压缩字体支持（HOS_SC.c + SD 卡 bin 字库均为 RLE 压缩格式）*/
#define LV_USE_FONT_COMPRESSED 1

/* 字体大格局形（max 4GB）：SD 30px GB2312 全集压缩流 >1MB，
 * 默认 20-bit bitmap_index 会溢出回卷（>1MB 字形全部错位显示花字）。
 * 32-bit 版描述符与 bin 布局完全兼容（lv_font_conv 生成时即按此考虑）。 */
#define LV_FONT_FMT_TXT_LARGE 1

/* SD 卡 bin 字库：LVGL 字体加载器 + stdio 文件系统接口。
 * 盘符 'S'：lv_font_load("S:/sdcard/fonts/xxx.bin") → fopen("/sdcard/...")。
 * 加载的字体内存走 LV_MEM_CUSTOM → PSRAM，加载完成后拔卡不影响显示。 */
#define LV_USE_FONT_LOADER 1
#define LV_USE_FS_STDIO 1
#define LV_FS_STDIO_LETTER 'S'
#define LV_FS_STDIO_PATH ""
#define LV_FS_STDIO_CACHE_SIZE 0

/* RGB565 字节序交换（SPI 屏）*/
#define LV_COLOR_16_SWAP 1

/* 允许半透明 layer（旋转对象必需，否则 layer 创建失败不绘制）*/
#define LV_COLOR_SCREEN_TRANSP 1

/* 渲染节流周期：8ms 使 LVGL 在 mjpeg 发布后立即渲染并在下一帧发布前完成，
   避免渲染窗口（~17ms）与发布时刻碰撞造成的锁等待（实测 25fps 的根因）*/
#define LV_DISP_DEF_REFR_PERIOD 8

/* 性能监控（诊断用，已关闭）*/
#define LV_USE_PERF_MONITOR 0

/* GIF 动画支持 */
#define LV_USE_GIF 1

/* LVGL 内存使用 PSRAM（heap_caps 分配器）*/
#define LV_MEM_CUSTOM 1
#define LV_MEM_CUSTOM_INCLUDE <esp_heap_caps.h>
#define LV_MEM_CUSTOM_ALLOC(size)    heap_caps_malloc((size), MALLOC_CAP_SPIRAM | MALLOC_CAP_8BIT)
#define LV_MEM_CUSTOM_FREE(p)        heap_caps_free(p)
#define LV_MEM_CUSTOM_REALLOC(p, size) heap_caps_realloc((p), (size), MALLOC_CAP_SPIRAM | MALLOC_CAP_8BIT)

#define LV_FONT_MONTSERRAT_8  0
#define LV_FONT_MONTSERRAT_10 1
#define LV_FONT_MONTSERRAT_12 1
#define LV_FONT_MONTSERRAT_14 1
#define LV_FONT_MONTSERRAT_16 0
#define LV_FONT_MONTSERRAT_18 0
#define LV_FONT_MONTSERRAT_20 0
#define LV_FONT_MONTSERRAT_22 0
#define LV_FONT_MONTSERRAT_24 1
#define LV_FONT_MONTSERRAT_26 0
#define LV_FONT_MONTSERRAT_28 0
#define LV_FONT_MONTSERRAT_30 1
#define LV_FONT_MONTSERRAT_32 0
#define LV_FONT_MONTSERRAT_34 0
#define LV_FONT_MONTSERRAT_36 0
#define LV_FONT_MONTSERRAT_38 0
#define LV_FONT_MONTSERRAT_40 1
#define LV_FONT_MONTSERRAT_42 0
#define LV_FONT_MONTSERRAT_44 0
#define LV_FONT_MONTSERRAT_46 0
#define LV_FONT_MONTSERRAT_48 0

/* 像素字体（开机终端标题/进度条字符块使用）*/
#define LV_FONT_UNSCII_16 1

#endif /*LV_CONF_H*/
