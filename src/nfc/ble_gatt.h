/* ============================================================================
 * ble_gatt.h - BLE GATT 服务（手机/PC 远程配置与素材上传）
 *
 *   ble_gatt_init()              初始化 NimBLE（app_main 中调用一次；
 *                                广播默认关闭，/sdcard/config/bt.txt 可配
 *                                enable=1 开机自动开、name= 自定义设备名）
 *                                须在 espnow_nfc_init() 之后）
 *   ble_gatt_is_connected()      是否已有客户端连接
 *   ble_gatt_adv_enabled()       广播是否开启
 *   ble_gatt_set_adv(on)         开/关广播（会话内有效，不持久化）
 *   ble_gatt_dev_name()          设备名 "PASS-II-XXXX"（MAC 尾 4 位）
 *   ble_gatt_upload_progress()   素材上传进度 -1=空闲 0..100
 *
 * 服务一览（16 位自定义 UUID，无配对直连 Just Works）：
 *   0x180A 设备信息: 0x2A28 软件版本（只读）
 *   0xFFE0 设置: current.txt / ui_text.txt / ip.txt 读写、日志级别、
 *               reload 触发、uid / motto / uname 读写
 *   0xFFF0 SD: 角色目录列表、文件分段读取、素材上传（START/分块/END，
 *              状态走 0xFFF4 通知）、上传状态通知
 * ==========================================================================*/

#ifndef BLE_GATT_H
#define BLE_GATT_H

#include <stdbool.h>
#include <stdint.h>

#ifdef __cplusplus
extern "C" {
#endif

/* 初始化 NimBLE 主机与 GATT 服务（内部创建 nimble host 任务）。 */
void ble_gatt_init(void);

/* 当前是否有客户端连接（NimBLE 任务外读取，volatile 语义） */
bool ble_gatt_is_connected(void);

/* 广播开关状态 */
bool ble_gatt_adv_enabled(void);

/* 开关广播（断开后会自动重开，此开关仅在连接建立前生效） */
void ble_gatt_set_adv(bool on);

/* 设备名（静态字符串，指向内部缓冲） */
const char *ble_gatt_dev_name(void);

/* 素材上传进度：-1=空闲，0..100=进行中百分比 */
int ble_gatt_upload_progress(void);

#ifdef __cplusplus
}
#endif

#endif /* BLE_GATT_H */
