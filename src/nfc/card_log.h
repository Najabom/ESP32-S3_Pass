/* ============================================================================
 * card_log.h - 收卡记录（SD 卡：整齐 txt + 完整 bin 备份）
 *
 * 每次成功收到名片（ESP-NOW 会话完成）时调用 card_log_save()：
 *   1. 向 /sdcard/cards.txt 追加一行可读记录：
 *      #0001 | up=0123s | mac=aa:bb:cc:dd:ee:ff | ip=1a2b |
 *      uname=".." | name=".." | uid=.. | motto=".." | avatar=8192 |
 *      portrait=0 | stk=2
 *      （up = 会话基准时间（主机打包时刻的绝对 unix 秒，双端逐字一致；
 *      主机未校时等极少数情况退回各自收包时刻）；ip = 握手命中的对端产品 IP
 *      短识别码 hex（0000 = 不可知/全通）；字段中的 | 和换行会转义为空格）
 *   2. 把完整名片 blob（hdr+payload，含头像/表情包二进制）写到
 *      /sdcard/cards/card_<mac12hex>.bin（同 MAC 覆盖，保留最新）。
 * SD 未挂载时返回 ESP_ERR_INVALID_STATE，调用方仅跳过记录，不影响
 * 名片横幅显示（横幅用的是会话内存里的实时数据）。
 * ==========================================================================*/

#ifndef CARD_LOG_H
#define CARD_LOG_H

#include <stdint.h>
#include "esp_err.h"

/* 记录一张收到的名片。
 * mac  = 对端 MAC（6 字节）；blob = hdr+payload 完整报文；total = blob 总长；
 * peer_ip_code = 握手命中的对端产品 IP 码（短识别码；field 直入/不可知 = 0，
 *                ip=0000）。
 * 返回 ESP_OK = txt+bin 均已写；ESP_ERR_INVALID_STATE = SD 未挂载；
 * 其他错误码 = 写入失败（bin 失败不影响 txt 已追加的事实，会记日志）。 */
esp_err_t card_log_save(const uint8_t mac[6], const uint8_t *blob, uint32_t total,
                        uint16_t peer_ip_code);

/* 清空交换历史：删除 /sdcard/cards.txt 与 /sdcard/cards/ 下全部 bin。
 * 返回 ESP_OK / ESP_ERR_INVALID_STATE（SD 未挂载）/ ESP_FAIL（部分删除失败） */
esp_err_t card_log_clear(void);

#endif /* CARD_LOG_H */
