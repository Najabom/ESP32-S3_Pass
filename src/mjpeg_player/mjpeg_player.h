/* ============================================================================
 * mjpeg_player.h - MJPEG 播放任务接口
 *
 *   mjpeg_player_task(pvParameters)：pvParameters 必须是全屏 lv_img*
 *   （main.c 创建），任务循环解码内嵌视频并把帧发布到该对象。
 *   实现细节与可调参数见 mjpeg_player.c 文件头。 */
#ifndef MJPEG_PLAYER_H
#define MJPEG_PLAYER_H

#ifdef __cplusplus
extern "C" {
#endif

// 声明 MJPEG 硬件解码任务处理函数
void mjpeg_player_task(void *pvParameters);

/* 请求热重载：写 current.txt 后调用（触控菜单/BLE 远程配置）。
 * 内部置 volatile 标志由播放任务消费：播放中 → 与拔卡同款清理
 * （释放 intro/loop/帧缓冲、重置 UI 揭示）后重扫角色并重新加载；
 * WAIT_SD 态 → 静默忽略（下轮扫描自然读到新 current.txt）。
 * 任意线程可调，零阻塞。 */
void mjpeg_player_request_reload(void);

/* ESP-NOW 交换门：视频进入播放态（intro/loop）满 3s 才允许交换。
 * 加载/待机/拔卡期返回 false。任意任务可调，零阻塞。 */
bool mjpeg_player_exchange_ok(void);

#ifdef __cplusplus
}
#endif

#endif // MJPEG_PLAYER_H