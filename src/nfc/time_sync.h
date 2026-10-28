/* ============================================================================
 * time_sync.h - 墙钟时间模块（上位机 BLE 校准 + RTC 域内部计时）
 *
 * 背景：设备无 RTC 电池，断电后无墙钟。设计上设备会与 PC/手机 BLE 连接，
 *   每次连接由上位机写入当前 unix 秒（BLE 设置服务 0xFFE9 time 特性）
 *   完成校准；未校准期间退回"内部计时器"时间 = RTC 域计数（跨软复位
 *   保留、断电清零），上电起秒数。
 *
 * 规则：
 *   - 校准基准存 NVS（校准时刻 unix 秒 + RTC 计数快照），之后
 *     now = unix_s + (rtc_now - rtc_us) / 1e6；
 *   - RTC 域断电清零后 rtc_now < 基准快照 → 基准失效，退回相对时间；
 *   - 绝对对齐：set_unix 无条件接受上位机时间（无防回退守卫）；与旧
 *     墙钟存在偏差时经 weak hook 通知存量记录平移（冷却 NVS / cards.txt）；
 *   - 时区 tz_min 存 NVS（默认 UTC+8），"当日"判定用本地日。
 *
 * 使用方：ESP-NOW 冷却（当天 3 次判定）、名片时间戳（hdr->ts / cards.txt
 *   up=）、开机动画时间显示等。
 * ==========================================================================*/

#ifndef TIME_SYNC_H
#define TIME_SYNC_H

#include <stdint.h>
#include <stdbool.h>
#include "esp_err.h"

#ifdef __cplusplus
extern "C" {
#endif

/* 初始化：读取 NVS 基准到内存缓存（app_main 启动早期调用一次） */
void time_sync_init(void);

/* 当前时间（秒）：校准有效 → 墙钟 unix 秒；未校准 → RTC 域相对秒 */
uint32_t time_sync_now(void);

/* 是否有有效墙钟校准（用于 UI 提示"时间已同步"等） */
bool time_sync_has_wall(void);

/* 上位机 BLE 校准入口：unix 秒。绝对对齐——无条件接受（含回退方向，
 * 仅拦明显垃圾值）。与旧墙钟的偏差自动平移存量记录。 */
esp_err_t time_sync_set_unix(uint32_t unix_s);

/* 时区偏移（分钟，UTC+8 = 480）：NVS 持久化；范围 [-720, +840] */
esp_err_t time_sync_set_tz(int16_t tz_min);
int16_t time_sync_get_tz(void);

/* 本地"日"编号 = (now + tz*60) / 86400：冷却封禁按日判定统一走此接口 */
uint32_t time_sync_local_day(void);

/* 无墙钟（未校准且无外置 RTC）时借用对端 ESP-NOW 时间戳：每 boot 至多
 * 一次，走 set_unix 通路（自动写外置 RTC / 平移存量记录）。 */
esp_err_t time_sync_adopt_peer(uint32_t unix_s);

#ifdef __cplusplus
}
#endif

#endif /* TIME_SYNC_H */
