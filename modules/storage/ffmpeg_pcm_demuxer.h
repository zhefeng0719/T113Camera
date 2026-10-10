#ifndef FFMPEG_PCM_DEMUXER_H
#define FFMPEG_PCM_DEMUXER_H

#include <stddef.h>
#include <stdint.h>

#include <libavformat/avformat.h>

struct ffmpeg_pcm_demuxer {
    AVFormatContext *fmt; /* 解封装上下文，只由所属任务读写。 */
    int audio_stream; /* 所选 PCM 音频流索引，-1 表示没有。 */
    AVRational time_base; /* 所选流 PTS 的时间刻度。 */
    AVPacket packet; /* 本轮原始 packet，read 返回借用其负载。 */
    int packet_valid; /* 负载引用有效时关闭或下次 read 必须 unref。 */
    int64_t duration_ms; /* 媒体总时长，未知时为负值。 */
    unsigned int sample_rate; /* 音频采样率，单位 Hz。 */
    unsigned int channels; /* 每个音频帧中的声道数量。 */
    unsigned int bits_per_sample; /* PCM 每声道样本位数。 */
};

int ffmpeg_pcm_demuxer_open(struct ffmpeg_pcm_demuxer *demuxer,
                            const char *path);
int ffmpeg_pcm_demuxer_read(struct ffmpeg_pcm_demuxer *demuxer,
                            const unsigned char **data,
                            size_t *size,
                            int64_t *pts_ms);
void ffmpeg_pcm_demuxer_close(struct ffmpeg_pcm_demuxer *demuxer);

#endif
