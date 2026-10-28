/* ============================================================================
 * espnow_nfc.h - ESP-NOW 名片交换引擎接口
 *
 *   espnow_nfc_set_recv_cb(cb)  注册"收到完整名片"回调（弹横幅/存档前）
 *   espnow_nfc_init()           初始化 WiFi/ESP-NOW 并启动交换任务
 *
 * 交换/冷却/广播等行为参数在 espnow_nfc.c 与 nfc_proto.h，见各文件头。
 * ==========================================================================*/

#ifndef ESPNOW_NFC_H
#define ESPNOW_NFC_H

#include <stdint.h>
#include "esp_err.h"
#include "nfc_proto.h"

#ifdef __cplusplus
extern "C" {
#endif

/* 收到完整名片回调：mac=对端地址，hdr=名片头，payload=头像+表情包拼接
 * （指向交换缓冲区，回调返回后即失效，需拷贝使用）。NFC 任务上下文调用。 */
typedef void (*nfc_recv_card_cb_t)(const uint8_t mac[6], const card_blob_hdr_t *hdr,
                                   const uint8_t *payload, size_t payload_len);

esp_err_t espnow_nfc_init(void);
void      espnow_nfc_set_recv_cb(nfc_recv_card_cb_t cb);

/* 清空冷却状态：NVS ns "nfccd" 全部记录 + 内存镜像。配合 card_log_clear()
 * 用于"恢复出厂式清空交换历史"。 */
esp_err_t espnow_nfc_clear_cooldown(void);

#ifdef __cplusplus
}
#endif

#endif /* ESPNOW_NFC_H */
