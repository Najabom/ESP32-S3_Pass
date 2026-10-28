/* ============================================================================
 * roles.h - 角色动态发现（SD 卡后端）
 *
 * 角色数据已从固件迁到 SD 卡：角色名 = /sdcard 下的文件夹名，文件夹内：
 *   loop.mjpeg    必须：循环动画
 *   intro.mjpeg   可选：开场动画（播一遍后接 loop）
 *   portrait.bin  可选：64x64 RGB565 8192 字节，随名片发出
 * 选择规则：/sdcard/config/current.txt 里写的文件夹名优先；文件不存在或指定的
 *   文件夹无效（缺 loop.mjpeg）时，回退为字母序第一个含 loop.mjpeg 的
 *   文件夹；都没有则"无角色"（roles_current_name() 返回 NULL）。
 *
 * 生命周期：SD 挂载后由播放器任务调 roles_scan()；卡拔出调
 *   roles_clear()。名字缓冲是模块内静态区，读取方直接拿指针用。
 * 可调整内容：ROLE_NAME_MAX 为角色名（UTF-8 字节）上限，中文名
 *   每字 3 字节；ROLE_PATH_MAX 为完整路径缓冲长度。
 * ==========================================================================*/

#ifndef ROLES_H
#define ROLES_H

#include <stddef.h>
#include <stdint.h>
#include "esp_err.h"

#define ROLE_NAME_MAX  64    // 角色名（=文件夹名）最大 UTF-8 字节数
#define ROLE_PATH_MAX  96    // "/sdcard/<name>/intro.mjpeg" 级别路径缓冲

/* 扫描 /sdcard 并选定当前角色（需 SD 已挂载）。
 * 返回 ESP_OK = 选到角色；ESP_ERR_NOT_FOUND = 卡上没有任何有效角色文件夹；
 * 其他错误码 = SD 未挂载等。 */
esp_err_t roles_scan(void);

/* 清空当前角色（卡拔出时调）。之后 roles_current_name() 返回 NULL。 */
void roles_clear(void);

/* 当前角色名；无角色返回 NULL。指针指向模块内静态缓冲，
 * 下次 scan/clear 时内容可能变化（良性竞态，最坏拿到截断的旧名）。 */
const char *roles_current_name(void);

/* 取当前角色的 intro/loop 完整路径。
 * intro 不存在时 intro_path[0] 置 '\0'（属正常情况，不算错误）。
 * 任一出参可为 NULL（表示不要这项）。无角色返回 ESP_ERR_INVALID_STATE。 */
esp_err_t roles_get_paths(char *intro_path, size_t intro_cap,
                              char *loop_path, size_t loop_cap);

/* 读当前角色的 portrait.bin 到 out（须 >= 8192 字节）。
 * 返回 ESP_OK = 8192 字节已填；无文件/尺寸不符/无角色返回错误码。 */
esp_err_t roles_read_portrait(uint8_t *out);

/* ---------- 触控 UI：角色枚举与选择 ---------- */

/* 把全部角色名拼进 dst（空格分隔，末项后无空格）。cap 足够容纳
 * max*ROLE_NAME_MAX 字节；无角色时 dst[0]='\0'。内部持 SD 锁。 */
void mjpeg_role_names(char *dst, size_t cap);

/* 单个角色条目（名字 + 是否有 portrait.bin，供列表页显示头像） */
typedef struct {
    char name[ROLE_NAME_MAX];
    bool has_portrait;
} role_entry_t;

/* 枚举 /sdcard 下所有有效角色（含 loop.mjpeg 的顶层目录），字母序填入
 * out（须 >= max 项），返回实际条数。无卡/未挂载返回 0（内部持 SD 锁）。 */
int roles_list_all(role_entry_t *out, int max);

/* 把当前角色改为 name（写 /sdcard/config/current.txt，内部持 SD 锁）：
 * 返回 ESP_OK = 已写入；ESP_ERR_INVALID_ARG = 名字为空/过长；
 * ESP_ERR_INVALID_STATE = SD 未挂载；ESP_ERR_NOT_FOUND = 该角色无效
 * （无 loop.mjpeg，不写入）。之后由播放器 request_reload 生效。 */
esp_err_t roles_set_current(const char *name);

/* ---------- 显示名归一化 ----------
 * UTF-8 弯引号（U+2018/2019 → '，U+201C/201D → "）转 ASCII 直引号，
 * 其余字节原样拷贝。所有显示出口（UI、名片发送、字库白名单）用它，
 * 保证与 PC 端字形一致。out 容量 cap，结果 '\0' 结尾。 */
void role_text_display(const char *in, char *out, size_t cap);

#endif /* ROLES_H */
