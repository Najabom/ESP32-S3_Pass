/* ============================================================================
 * bsp_sdcard.h - SD 卡（SPI3 总线 + FATFS + 热插拔）接口
 *
 * 引脚（独立占用 SPI3；SPI3 原为触摸总线，触摸已改 GPIO 模拟让出）：
 *   SD_CS 7 / SD_MOSI 6 / SD_MISO 4 / SD_SCK 5
 *   宏来自 pins_pass.h（PCB v6.2 实机唯一来源，引脚见那里）。
 * 用法：
 *   1) app_main 里 bsp_sdcard_init()（初始化总线，无卡也返回 ESP_OK）；
 *   2) bsp_sdcard_start_monitor() 启动 1s 轮询任务，之后插/拔卡自动
 *      挂载/卸载，业务层只管轮询 bsp_sdcard_is_mounted()；
 *   3) 文件 IO 必须持锁：bsp_sdcard_lock() → 判 is_mounted → 读写 →
 *      bsp_sdcard_unlock()；整文件读取直接用 bsp_sdcard_load_file()
 *      （内部持锁，缓冲分配在 PSRAM）。
 * 可调整内容：仅接线变更时改 pins_pass.h（需同步重接线）。
 * ==========================================================================*/
#ifndef BSP_SDCARD_H
#define BSP_SDCARD_H

#include <stdbool.h>
#include <stddef.h>
#include <stdint.h>
#include "esp_err.h"
#include "pins_pass.h"

#define BSP_SDCARD_MOUNT_POINT "/sdcard"

/* 单个文件读取上限（PSRAM 预算保护：视频预载 intro+loop 同驻内存） */
#define BSP_SDCARD_LOAD_MAX    (3 * 1024 * 1024)

/* 初始化 SPI3 总线与互斥锁，并做首次挂载尝试。
 * 返回 ESP_OK = 总线就绪（此时无卡不影响返回值，由 monitor 后续自动挂载）；
 * 返回错误码 = 总线级失败（SPI3 被占等硬件问题，SD 功能整体不可用）。 */
esp_err_t bsp_sdcard_init(void);

/* 启动热插拔监控任务（1s 轮询：未挂载→尝试挂载；已挂载→CMD13 探测，
 * 拔出则卸载）。重复调用安全（只建一次任务）。 */
void bsp_sdcard_start_monitor(void);

/* SD 卡是否已挂载（未挂载时不要对挂载点做文件 IO） */
bool bsp_sdcard_is_mounted(void);

/* 连续挂载失败次数（monitor 维护，成功后清零）。UI 可据此分型提示：
 * 0 = 卡尚未尝试/刚拔出；≥3 = 卡插着但一直初始化失败（换卡/检查连接）。 */
int bsp_sdcard_fail_streak(void);

/* 最近一次探测是否"卡在且可初始化"：false = 卡不在/没插好。
 * 配合 fail_streak≥3：probe_ok → "SD卡无法识别"，否则 → "请插入SD卡"。 */
bool bsp_sdcard_probe_ok(void);

/* 挂载/卸载/文件 IO 三方的互斥锁。持锁期间 monitor 不会卸载卡。 */
void bsp_sdcard_lock(void);
void bsp_sdcard_unlock(void);

/* 整文件读取进度回调：分块读完成一块后调用一次（done/total 字节数）。
 * 回调运行在调用线程（持 SD 锁、未持 LVGL 锁），内部如需更新 LVGL
 * 控件请自行加 bsp_lvgl_lock（与 SD 锁无环）。 */
typedef void (*bsp_sdcard_progress_cb_t)(uint32_t done, uint32_t total, void *ctx);

/* 把整个文件读进 PSRAM（内部持锁 + 判挂载 + 大小上限检查）。
 * 成功返回缓冲指针（调用方用完 heap_caps_free）并填 *out_size；
 * 无卡/文件不存在/超限/读失败返回 NULL。
 * cb/ctx 可选：分块读取时回调进度（重试时进度会回跳，属真实情况）。 */
uint8_t *bsp_sdcard_load_file_ex(const char *path, size_t *out_size,
                                 bsp_sdcard_progress_cb_t cb, void *ctx);

/* bsp_sdcard_load_file_ex 的无进度回调便捷包装（多数调用点用这个）。 */
uint8_t *bsp_sdcard_load_file(const char *path, size_t *out_size);

/* SD 卷容量统计（FATFS f_getfree，SD 锁内；MBR/GPT 均注册盘符 "0:"）。
 * 成功返回 ESP_OK 并填 total/free 字节数；未挂载返回 ESP_ERR_INVALID_STATE。 */
esp_err_t bsp_sdcard_stats(uint64_t *total_bytes, uint64_t *free_bytes);

#endif
