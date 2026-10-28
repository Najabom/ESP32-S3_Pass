/* ============================================================================
 * card_store.h - 名片存储层（cardstore 分区，SPIFFS）
 *
 * 现状（SD 卡迁移后）：收到的名片已改由 nfc/card_log.c 记录到 SD 卡
 *   （/sdcard/cards.txt + /sdcard/cards/card_<mac>.bin），本模块只剩
 *   self.bin（自己的头像+表情包）仍被发卡流程只读使用；收卡相关接口
 *   （save_card/list）已无人调用，暂时保留备用。
 *
 * 文件布局（挂载点 /cards）：
 *   self.bin          自己的头像+表情包（可改写）
 *   card_<mac>.bin    【已停用】收到的名片（现记录在 SD 卡）
 *   index.json        【已停用】名片元数据索引
 *
 * 可调整内容：
 *   - CARD_STORE_MAX_CARDS 32：内存中元数据表上限（也是索引文件上限）。
 *     调大需注意 RAM（每张 ~90B）。
 *   - 分区容量 4MB（partitions.csv 的 cardstore 行）：现仅 self.bin 使用，
 *     未来若自资料也迁 SD，该分区可整段退役（改分区表需整片擦除重烧）。
 * ==========================================================================*/

#ifndef CARD_STORE_H
#define CARD_STORE_H

#include <stdint.h>
#include <stddef.h>
#include "esp_err.h"

#ifdef __cplusplus
extern "C" {
#endif

#define CARD_STORE_MOUNT_POINT "/cards"
#define CARD_STORE_PARTITION   "cardstore"
#define CARD_STORE_MAX_CARDS   32

/* 挂载分区并扫描已有名片建立内存索引（一次，失败会尝试自动格式化） */
esp_err_t card_store_init(void);

/* 自己头像+表情包 blob（self.bin）读写 */
esp_err_t card_store_save_self(const uint8_t *data, size_t len);
esp_err_t card_store_load_self(uint8_t *buf, size_t buf_len, size_t *out_len);

/* 名片 blob（card_<mac>.bin）读写删（save 同 MAC 覆盖旧卡并更新索引） */
esp_err_t card_store_save_card(const uint8_t mac[6], const uint8_t *data, size_t len);
esp_err_t card_store_load_card(const uint8_t mac[6], uint8_t *buf, size_t buf_len, size_t *out_len);
esp_err_t card_store_delete_card(const uint8_t mac[6]);

/* 输出 index.json 同款 JSON 文本到缓冲（手机 App/控制台用） */
esp_err_t card_store_list(char *json_out, size_t max_len);

#ifdef __cplusplus
}
#endif

#endif /* CARD_STORE_H */
