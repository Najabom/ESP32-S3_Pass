/* ============================================================================
 * nfc_profile.h - 自己档案（uid / 寄语）接口
 *
 * 数据落在 NVS 分区（命名空间 "nfc"），断电不丢；无任何写死值：
 *   - uid：首启自动生成（MAC 后 4 字节十进制，天然每台唯一）
 *   - motto：首启为空，用户自行设置
 * 修改入口：串口控制台命令（nfc-uid/nfc-motto，见 .c），未来触摸 UI
 *   和手机 App 可复用同一组 set 接口。
 * ==========================================================================*/

#ifndef NFC_PROFILE_H
#define NFC_PROFILE_H

#include <stdint.h>
#include "esp_err.h"

#ifdef __cplusplus
extern "C" {
#endif
#define NFC_PROFILE_UID_MAX   20   /* uid 上限位数（纯数字） */

#define NFC_PROFILE_MOTTO_MAX 64   /* 寄语上限字节数（UTF-8；20 汉字 = 60B）——与 nfc_field_t.motto[64] 同步 */

#define NFC_PROFILE_UNAME_MAX 64   /* 用户名上限字节数（UTF-8；→ 20 个汉字 = 60B + NUL） */
/* 从 NVS 读档（缺失则生成默认并落盘），启动时调一次 */
esp_err_t    nfc_profile_init(void);
const char  *nfc_profile_get_uid(void);
const char  *nfc_profile_get_motto(void);
const char  *nfc_profile_get_uname(void);
/* 校验并写入：uid 必须 1..20 位纯数字；motto 最长 20 字节；
 * uname 最长 15 字符且仅限 ASCII 字母/数字/空格/_-.（横幅字库覆盖范围） */
esp_err_t    nfc_profile_set_uid(const char *uid);
esp_err_t    nfc_profile_set_motto(const char *motto);
esp_err_t    nfc_profile_set_uname(const char *uname);
/* 挂载 USB 串口 REPL 控制台与 5 条命令（esp_console） */
void         nfc_profile_console_register(void);

#ifdef __cplusplus
}
#endif

#endif /* NFC_PROFILE_H */
