#ifndef AUDIO_TIMELINE_H
#define AUDIO_TIMELINE_H
#include <stdint.h>
/* Timestamp gaps become silence; overlapping samples are discarded, not played twice. */
static inline int audio_timeline_plan(int64_t target, uint64_t submitted, unsigned int rate,
                                      unsigned int frames, unsigned int *gap, unsigned int *skip)
{
    int64_t delta = target - (int64_t)submitted; /* 目标样本位置减已提交数量：正数缺样本，负数有重叠。 */
    *gap = *skip = 0; /* 默认连续，不补静音也不跳过数据。 */
    if (delta > (int64_t)rate * 10 || delta < -(int64_t)rate * 10) /* 超过十秒的异常跳变拒绝处理，避免失控补样本。 */
        return -1; /* Corrupt/unsupported discontinuities must not allocate an unbounded gap. */
    if (delta > (int64_t)rate / 500) /* 容忍两毫秒以内的采集时间估计抖动。 */
        *gap = (unsigned int)delta; /* 缺口以输出采样率对应的样本帧数补静音。 */
    else if (delta < -(int64_t)rate / 500) /* 负偏差意味着这一块的前部已经播放过。 */
        *skip = -delta > frames ? frames : (unsigned int)-delta; /* 最多丢弃本块全部样本，防止指针越过缓冲。 */
    return 0;
}
#endif
