/* ============================================================================
 * ip_config.h - 产品 IP 配置（ESP-NOW 分 IP 隔离）
 *
 * 功能：ESP-NOW 名片交换默认所有同类设备互通；本模块允许通过 SD 卡
 *       /config/ip.txt 声明本机所属的"产品 IP"列表：
 *         - 用户用自然语言（中文/英文/日文）每行写一个 IP 标识，
 *           模块把每个标识哈希成内部 16-bit 短识别码（FNV-1a，
 *           ASCII 大小写归一；UTF-8 其余字节原样参与），展示/记录
 *           均用该短码（PC 端同算法，可离线预览）。
 *         - 握手时（HELLO/HELLO_ACK）双方携带各自 IP 码列表：任一方
 *           全通（无配置）或双方列表有交集 → 允许交换；否则忽略并
 *           进入 60s 冷却（见 espnow_nfc.c）。
 *         - 未配置（无文件/空文件/无有效行）→ 全通（保持旧行为）。
 *
 * ip.txt 格式（/config/ip.txt，UTF-8，每行一个 IP 标识）：
 *   # 注释
 *   示例A              # 本机所属产品 IP 之一
 *   示例B              # 最多 8 个；空文件/全注释 = 全通
 *   （兼容旧版：my_name=/peer= 前缀行按纯标识行解析，可无缝迁移）
 *
 * 注意：两端设备的 IP 标识必须逐字节一致（同一份文件/同样的写法），
 *       哈希结果才相同；16-bit 哈希理论碰撞率 1/65536。
 * ==========================================================================*/

#ifndef IP_CONFIG_H
#define IP_CONFIG_H

#include <stdint.h>
#include <stdbool.h>

#ifdef __cplusplus
extern "C" {
#endif

#define IP_CONFIG_CODE_MAX  8   /* 本机 IP 码上限（须与 nfc_proto.h NFC_IP_CODES_MAX 同值） */
#define IP_CONFIG_WORD_MAX  64  /* 单行标识最大字节数（含 NUL） */

typedef struct {
    uint16_t codes[IP_CONFIG_CODE_MAX]; /* 本机所属 IP 短识别码集合 */
    int      cnt;                       /* 0 = 未配置（全通） */
} ip_config_t;

/* 自然语言 → 短识别码（FNV-1a 32 位取低 16 位；ASCII 大写归一为小写） */
uint16_t ip_code_from_text(const char *s);

/* 读取 /config/ip.txt（持 SD 锁），成功则替换当前配置。幂等：
 * 文件缺失/解析失败保持上次配置（首次调用后为全 0 = 全通）。 */
void ip_config_load(void);

/* 获取当前配置（load 后只读；未 load 时 cnt=0） */
const ip_config_t *ip_config_get(void);

#ifdef __cplusplus
}
#endif

#endif /* IP_CONFIG_H */
