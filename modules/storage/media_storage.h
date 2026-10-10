#ifndef MEDIA_STORAGE_H
#define MEDIA_STORAGE_H
#include <stddef.h>
#include <stdint.h>

/* 仅接受真实挂载点；writable 用于录制，播放器可读取只读盘。 */
int media_storage_ready(const char *root, int writable);
/* record1 已存在时生成 record1_001，连同 .part 一起检查，不覆盖旧文件。 */
int media_storage_next_name(const char *root, const char *folder,
                            const char *base, const char *extension,
                            char *name, size_t capacity);
/* 保存原始 MJPEG 帧为 JPEG；完整同步后才发布正式文件。 */
int media_storage_save_jpeg(const char *root, const char *base,
                            const void *data, size_t size,
                            char *path, size_t capacity);
int media_storage_save_jpeg_cancelable(const char *root, const char *base,
                                       const void *data, size_t size, char *path, size_t capacity,
                                       const volatile int *cancel); /* 写入及发布接受通用取消。 */
#endif
