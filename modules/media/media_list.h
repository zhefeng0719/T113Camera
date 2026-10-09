#ifndef MEDIA_LIST_H
#define MEDIA_LIST_H

#include <stdbool.h>
#include <stddef.h>

#define MEDIA_LIST_MAX_ITEMS 64
#define MEDIA_LIST_PATH_MAX 512
#define MEDIA_LIST_NAME_MAX 128

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
    char path[MEDIA_LIST_PATH_MAX];
    char name[MEDIA_LIST_NAME_MAX];
    enum media_type type;
    bool playable;
};

struct media_list {
    struct media_item items[MEDIA_LIST_MAX_ITEMS];
    size_t count;
};

void media_list_init(struct media_list *list);
int media_list_scan(struct media_list *list, const char *usb_root);
const char *media_type_name(enum media_type type);

#endif
