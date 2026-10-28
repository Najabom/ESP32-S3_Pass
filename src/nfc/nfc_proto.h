/* ============================================================================
 * nfc_proto.h - ESP-NOW 名片交换协议定义（线格式 + 文件格式）
 *
 * 协议概述：
 *   两台设备相遇 → 互相 HELLO 广播（带特征码 PSNF 验证同类设备）→
 *   MAC 地址小的一方当"主"，先发自己名片 → 从收完后回发自己的 →
 *   双方各自完成存储+展示。大数据（头像+表情包）按 192B 分片停等
 *   （每片等 DATA_ACK，50ms 超时重发，最多 3 次）。
 *
 * 可调整内容（改这里即改协议，两台设备需同版本才能互通）：
 *   - NFC_HELLO_PERIOD_US 200ms：广播周期。缩短=擦肩更易被捕捉但空口
 *     占用和功耗↑；加长反之。±10% 抖动防两台设备相位锁死。
 *   - NFC_SEG_SIZE 192：分片大小。必须 < NFC_MAX_PAYLOAD - 分片头；
 *     调大=片数少速度快，但单包越长丢包损失越大。
 *   - NFC_ACK_TIMEOUT_MS 80 / NFC_ACK_MAX_RETRY 10：重传策略。
 *   - CARD_AVATAR_SIZE 8192：头像 64x64 RGB565，与 UI/生成脚本绑定，
 *     改尺寸要连带改 tools/gen_default_avatar.py 和 ui_core_screen.c。
 * ==========================================================================*/

#ifndef NFC_PROTO_H
#define NFC_PROTO_H

#include <stdint.h>
#include <stddef.h>

#ifdef __cplusplus
extern "C" {
#endif

/* ============ ESP-NOW 包格式 ============ */

#define NFC_MAGIC        0x464E5350UL /* "PSNF" (LE) */
#define NFC_VER          4             /* v4：DATA_FIELD 增会话基准时间 ts（双方记录同一时间戳）；v3 加 portrait_len，与 v1/v2 不互通 */
#define NFC_FEATURE_CODE 0x314E5350UL /* "PSN1" (LE) 同类设备特征码 */
#define NFC_MAX_PAYLOAD  250
#define NFC_SEG_SIZE     192
#define NFC_ACK_TIMEOUT_MS 80
#define NFC_ACK_MAX_RETRY   10
#define NFC_HELLO_PERIOD_US 200000UL
#define NFC_HELLO_JITTER_US 20000UL

typedef enum {
    NFC_TYPE_HELLO      = 1,
    NFC_TYPE_HELLO_ACK  = 2,
    NFC_TYPE_DATA_FIELD = 3,
    NFC_TYPE_DATA_SEG   = 4,
    NFC_TYPE_DATA_ACK   = 5,
    NFC_TYPE_DONE       = 6,
} nfc_type_t;

/* 通用包：magic 防串扰 + ver 防版本错配 + seq/ack_seq 支撑停等重传 */
typedef struct {
    uint32_t magic;
    uint8_t  ver;
    uint8_t  type;
    uint8_t  seq;
    uint8_t  ack_seq;
    uint16_t len;
    uint8_t  payload[NFC_MAX_PAYLOAD];
} __attribute__((packed)) nfc_packet_t;

#define NFC_PACKET_HDR_SIZE 10

/* HELLO / HELLO_ACK payload：特征码校验同类设备 + 产品 IP 码列表（分 IP 隔离）。
 * 布局（v3，长度 = 6 + cnt*2 + 4）：
 *   [0..3]  feature_code = NFC_FEATURE_CODE
 *   [4]     cnt   = 本机所属 IP 短识别码个数（0 = 未配置/全通）
 *   [5]     ver   = 3（payload 形状标记；兼容解析 v2=2）
 *   [6..]   codes = FNV-1a 16-bit 短识别码（小端），最多 IP_CONFIG_CODE_MAX 个
 *   [6+cnt*2..] ts = 本机墙钟 unix 秒（UTC，小端；未校准时为内部相对秒，
 *              接收方以 < 1e9 判弃用）——双方据此对齐冷却时间；时区
 *              不参与（存的全是 UTC，跨时区天然一致）。
 * 兼容旧版 6B payload {feature_code, group_code}（旧固件，ver 位为 0/1）：
 *   len == 6 时按旧形状解析：group_code==0 视为全通，否则视为对方仅有
 *   这一个码；v2 空表（6B，ver=2）照旧识别为全通。
 * 校验规则（接收方）：
 *   - 本机未配置（cnt==0）→ 全部接受（保持旧行为）；
 *   - 对方含全通码（旧版 group 0）→ 接受；
 *   - 本机与对方列表任一码相同 → 接受（交集）；否则忽略（不回 ACK、
 *     不进入会话）并给对方记 60s 失配冷却。 */
#define NFC_HELLO_VER 2
#define NFC_HELLO_VER_LATEST 3   /* 发送形状：v3 带时间戳 */
#define NFC_HELLO_TS_SIZE 4
#define NFC_IP_CODES_MAX 8   /* HELLO 携带的 IP 码个数上限（与 ip_config.h 同值） */

typedef struct {
    uint32_t feature_code;
    uint8_t  cnt;
    uint8_t  ver;
    uint16_t codes[NFC_IP_CODES_MAX];
} __attribute__((packed)) nfc_hello_payload_t;

/* HELLO payload 头部固定字节数（feature 4 + cnt 1 + ver 1） */
#define NFC_HELLO_HDR_SIZE 6

/* DATA_FIELD payload：字段元信息（不含头像/表情包数据本体） */
typedef struct {
    char     name[24];
    char     uid[20];
    char     motto[64];
    uint16_t avatar_len;   /* 用户头像字节长（0=未配置） */
    uint16_t portrait_len; /* 角色头图字节长（0=该角色未配头图） */
    uint8_t  sticker_cnt;
    uint16_t sticker_len[3];
    uint16_t total_size;
    char     uname[64];   /* 用户名（UTF-8 可配置；空串=未配置，UI 回退显示 name）。
                           * 与 CARD_UNAME_MAX 保持同步（64 字节 = 20 汉字 + NUL）。 */
    uint32_t ts;          /* v4 新增：会话基准时间 = 主机打包自己名片时刻的
                           * 墙钟 unix 秒（UTC）。双方收卡记录都用它落盘，
                           * 两边校时差几秒也记成同一个时间。 */
} __attribute__((packed)) nfc_field_t;

/* DATA_SEG payload 头 */
typedef struct {
    uint16_t offset;
    uint16_t seg_len;
} __attribute__((packed)) nfc_seg_hdr_t;

/* ============ 名片 blob 格式（cardstore 文件） ============
 * card_<mac>.bin = card_blob_hdr_t + 角色头图(portrait_len) + 用户头像(avatar_len)
 *                  + 表情包们。收到即原样落盘，index.json 只存元数据摘要。 */

#define CARD_BLOB_MAGIC   0x31445243UL /* "CRD1" (LE) */
#define CARD_BLOB_VER     5             /* v5：name 扩至 64B（v4 name=24B，motto64B uname64B） */
#define CARD_NAME_MAX     64
#define CARD_UID_MAX      20
#define CARD_MOTTO_MAX    64
#define CARD_UNAME_MAX    64             /* 用户名：最多 63 字符（UTF-8，20 汉字）+ NUL */
#define CARD_AVATAR_SIZE  8192   /* 64*64*2（用户头像/角色头图同规格） */
#define CARD_PORTRAIT_SIZE 8192  /* 角色头图 64*64*2（SD 卡角色文件夹 portrait.bin） */
#define CARD_STICKER_MAX  3
#define CARD_BLOB_MAX     (sizeof(card_blob_hdr_t) + CARD_PORTRAIT_SIZE + CARD_AVATAR_SIZE * (CARD_STICKER_MAX + 1))

typedef struct {
    uint32_t magic;
    uint8_t  ver;
    uint8_t  name_len;
    char     name[CARD_NAME_MAX];
    uint8_t  uid_len;
    char     uid[CARD_UID_MAX];
    uint8_t  motto_len;
    char     motto[CARD_MOTTO_MAX];
    uint16_t avatar_len;
    uint8_t  sticker_cnt;
    uint16_t sticker_len[CARD_STICKER_MAX];
    uint32_t ts;
    uint8_t  uname_len;              /* v2 新增：用户名（空=未配置） */
    char     uname[CARD_UNAME_MAX];
    uint16_t portrait_len;           /* v3 新增：角色头图（紧跟 hdr 之后，头像之前） */
} __attribute__((packed)) card_blob_hdr_t;

/* ============ self.bin blob 格式（自己头像+表情包） ============
 * 由用户通过控制台/未来 UI 写入（无则用固件默认头像），发送时打包进
 * card_blob_hdr_t 一起交换。 */

#define SELF_BLOB_MAX (sizeof(self_blob_hdr_t) + CARD_AVATAR_SIZE * (CARD_STICKER_MAX + 1))

typedef struct {
    uint16_t avatar_len;
    uint8_t  sticker_cnt;
    uint16_t sticker_len[CARD_STICKER_MAX];
} __attribute__((packed)) self_blob_hdr_t;

#ifdef __cplusplus
}
#endif

#endif /* NFC_PROTO_H */
