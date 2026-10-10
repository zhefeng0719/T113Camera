#include "media_clock.h"
#include <string.h>
#include <time.h>

int64_t media_monotonic_ms(void) /* 播放调度使用单调时钟，不受系统日期调整影响。 */
{
    struct timespec ts;
    clock_gettime(CLOCK_MONOTONIC, &ts); /* 获取从系统启动后持续递增的时间。 */
    return (int64_t)ts.tv_sec * 1000 + ts.tv_nsec / 1000000;
}
int media_clock_init(struct media_clock *clock) /* 会话开始创建时钟；需在工作线程启动前调用。 */
{
    memset(clock, 0, sizeof(*clock)); /* 初始未就绪，视频在音频首次发布前使用备用调度。 */
    return pthread_mutex_init(&clock->mutex, NULL);
}
void media_clock_destroy(struct media_clock *clock) /* 工作线程全部 join 后才能销毁共享锁。 */
{
    pthread_mutex_destroy(&clock->mutex); /* 其他线程不再访问此对象后销毁互斥锁。 */
}
void media_clock_publish(struct media_clock *clock, int64_t position_ms) /* 由音频线程发布实际硬件消费位置。 */
{
    if (!clock)
        return;
    pthread_mutex_lock(&clock->mutex); /* 位置、状态和更新时间作为同一快照受锁保护。 */
    if (!clock->ready || position_ms > clock->position_ms) /* 禁止因驱动 delay 波动导致主时钟倒退。 */
        clock->position_ms = position_ms; /* 存储媒体位置，不是墙上时钟的绝对日期。 */
    clock->ready = 1; /* 允许视频线程开始采用音频主时钟。 */
    clock->updated_ms = media_monotonic_ms(); /* 保存快照时间，供音频结束后的尾部外推使用。 */
    pthread_mutex_unlock(&clock->mutex); /* 发布完整快照，其他线程随后可以读取。 */
}
void media_clock_pause(struct media_clock *clock, int paused) /* 暂停时冻结尾部外推，恢复时重设时间基准。 */
{
    if (!clock)
        return;
    pthread_mutex_lock(&clock->mutex); /* 位置、状态和更新时间作为同一快照受锁保护。 */
    if (clock->finished && !clock->paused) /* 已结束音频仍有视频尾部时，先结算未暂停期间的时间。 */
        clock->position_ms += media_monotonic_ms() - clock->updated_ms; /* 把尾部经过的单调时间折入冻结位置。 */
    clock->paused = paused; /* 保存暂停状态；音频暂停与视频调度共用此状态。 */
    clock->updated_ms = media_monotonic_ms(); /* 保存快照时间，供音频结束后的尾部外推使用。 */
    pthread_mutex_unlock(&clock->mutex); /* 发布完整快照，其他线程随后可以读取。 */
}
void media_clock_finish(struct media_clock *clock) /* 音频自然结束后允许视频尾部继续播放。 */
{
    if (!clock)
        return;
    pthread_mutex_lock(&clock->mutex); /* 位置、状态和更新时间作为同一快照受锁保护。 */
    clock->finished = 1; /* 不再等待新的音频消费进度。 */
    clock->updated_ms = media_monotonic_ms(); /* 保存快照时间，供音频结束后的尾部外推使用。 */
    pthread_mutex_unlock(&clock->mutex); /* 发布完整快照，其他线程随后可以读取。 */
}
int media_clock_read(struct media_clock *clock, int64_t *position_ms) /* 视频线程读取位置；返回值表示时钟是否已就绪。 */
{
    int ready;
    pthread_mutex_lock(&clock->mutex); /* 位置、状态和更新时间作为同一快照受锁保护。 */
    ready = clock->ready; /* 与位置在同一临界区读取，避免不一致状态。 */
    *position_ms = clock->position_ms; /* 取得音频已发布的媒体位置。 */
    /* 音频自然结束后，让较长的视频尾部按单调时钟继续。 */
    if (clock->finished && !clock->paused) /* 已结束音频仍有视频尾部时，先结算未暂停期间的时间。 */
        *position_ms += media_monotonic_ms() - clock->updated_ms; /* 尾部外推只作用于本次返回值，不重复累加到共享值。 */
    pthread_mutex_unlock(&clock->mutex); /* 发布完整快照，其他线程随后可以读取。 */
    return ready;
}
