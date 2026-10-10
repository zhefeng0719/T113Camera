#ifndef MEDIA_LIST_H
#define MEDIA_LIST_H

#include <stdbool.h>
#include <stddef.h>

#define MEDIA_LIST_MAX_ITEMS 64 /* 媒体项上限，防止扫描占用无界内存。 */
#define MEDIA_LIST_PATH_MAX 512 /* 保存完整路径的字节容量，含字符串结束符。 */
#define MEDIA_LIST_NAME_MAX 128 /* 列表文件名容量，超长名字不截断收录。 */

enum media_type {
    MEDIA_TYPE_UNKNOWN = 0,
    MEDIA_TYPE_MJPEG,
    MEDIA_TYPE_JPEG,
    MEDIA_TYPE_WAV,
    MEDIA_TYPE_MP4,
    MEDIA_TYPE_MKV,
    MEDIA_TYPE_AVI,
    MEDIA_TYPE_AAC,
    MEDIA_TYPE_MP3,
};

struct media_item {
    char path[MEDIA_LIST_PATH_MAX]; /* 复制的完整路径，不借用目录枚举内存。 */
    char name[MEDIA_LIST_NAME_MAX]; /* 界面显示文件名。 */
    enum media_type type; /* 后缀分类，用于选择探测与播放入口。 */
    bool playable; /* 有播放入口不等于该文件内 codec 一定支持。 */
};

struct media_list {
    struct media_item items[MEDIA_LIST_MAX_ITEMS];
    size_t count; /* 本次扫描实际收录数。 */
};

void media_list_init(struct media_list *list);
int media_list_scan(struct media_list *list, const char *usb_root);
const char *media_type_name(enum media_type type);

#endif
