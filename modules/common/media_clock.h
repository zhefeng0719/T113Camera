#ifndef MEDIA_CLOCK_H
#define MEDIA_CLOCK_H
#include <pthread.h>
#include <stdint.h>

/* 音频线程发布硬件消费进度，视频线程读取；不共享 ALSA 句柄。 */
struct media_clock {
    pthread_mutex_t mutex; /* 保护时钟状态组合，防止跨线程读到半更新快照。 */
    int64_t position_ms; /* 实际已播放媒体位置，单位毫秒。 */
    int64_t updated_ms; /* 最近发布或暂停切换的单调时间。 */
    int ready; /* 是否至少发布过一次有效音频位置。 */
    int finished; /* 音频结束后视频尾部可改用单调时钟外推。 */
    int paused; /* 暂停时禁止尾部时钟继续前进。 */
};
int64_t media_monotonic_ms(void);
int media_clock_init(struct media_clock *clock);
void media_clock_destroy(struct media_clock *clock);
void media_clock_publish(struct media_clock *clock, int64_t position_ms);
void media_clock_pause(struct media_clock *clock, int paused);
void media_clock_finish(struct media_clock *clock);
int media_clock_read(struct media_clock *clock, int64_t *position_ms);
#endif
