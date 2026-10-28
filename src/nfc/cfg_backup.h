/* ============================================================================
 * cfg_backup.h - 关键 NVS 设置 ⇄ /sdcard/config/nvs_backup.txt 快照
 *
 *   动机：烧录固件/整片擦除会清空 NVS（UI 选项、本机档案），
 *   全量镜像烧录时无法靠"跳过区域"保留。本模块把关键设置周期性镜像到
 *   SD 卡（刷写固件不碰 SD），启动/热插卡后自动回灌 NVS。
 *
 *   覆盖键：nfc:uid/motto/uname、uiopt:m。
 *   （校准/冷却/时间基为可再生成数据，不入备份。）
 * ==========================================================================*/
#ifndef CFG_BACKUP_H
#define CFG_BACKUP_H

#include "esp_err.h"

/* 从 /sdcard/config/nvs_backup.txt 恢复关键设置到 NVS（键缺失才写，
 * 不覆盖设备当前值），并同步刷新 nfc_profile 缓存。无卡/无备份静默跳过。
 * 幂等，任何上下文可调（内部持 SD 锁）。 */
void cfg_backup_restore(void);

/* 启动周期快照任务：SD 在卡时每 20s 把关键设置写入备份文件（内容未变
 * 不重写）。启动后即调用一次。 */
void cfg_backup_start(void);

#endif
