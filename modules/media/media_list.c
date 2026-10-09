#include "media_list.h"

#include <dirent.h>
#include <stdio.h>
#include <string.h>
#include <sys/stat.h>

static int str_ends_with_ci(const char *text, const char *suffix)
{
    size_t text_len;
    size_t suffix_len;
    const char *tail;

    if (text == NULL || suffix == NULL)
        return 0;

    text_len = strlen(text);
    suffix_len = strlen(suffix);
    if (suffix_len > text_len)
        return 0;

    tail = text + text_len - suffix_len;
    while (*tail != '\0' && *suffix != '\0') {
        char a = *tail++;
        char b = *suffix++;

        if (a >= 'A' && a <= 'Z')
            a = (char)(a - 'A' + 'a');
        if (b >= 'A' && b <= 'Z')
            b = (char)(b - 'A' + 'a');
        if (a != b)
            return 0;
    }

    return 1;
}

static enum media_type detect_media_type(const char *name)
{
    if (str_ends_with_ci(name, ".mjpeg") || str_ends_with_ci(name, ".mjpg"))
        return MEDIA_TYPE_MJPEG;
    if (str_ends_with_ci(name, ".jpg") || str_ends_with_ci(name, ".jpeg"))
        return MEDIA_TYPE_JPEG;
    if (str_ends_with_ci(name, ".wav"))
        return MEDIA_TYPE_WAV;
    if (str_ends_with_ci(name, ".aac") || str_ends_with_ci(name, ".m4a"))
        return MEDIA_TYPE_AAC;
    if (str_ends_with_ci(name, ".mp3"))
        return MEDIA_TYPE_MP3;
    if (str_ends_with_ci(name, ".mp4"))
        return MEDIA_TYPE_MP4;
    if (str_ends_with_ci(name, ".mkv"))
        return MEDIA_TYPE_MKV;
    if (str_ends_with_ci(name, ".avi"))
        return MEDIA_TYPE_AVI;
    return MEDIA_TYPE_UNKNOWN;
}

static bool is_first_version_playable(enum media_type type)
{
    return type == MEDIA_TYPE_MJPEG ||
           type == MEDIA_TYPE_WAV ||
           type == MEDIA_TYPE_AAC ||
           type == MEDIA_TYPE_MP3 ||
           type == MEDIA_TYPE_MP4 ||
           type == MEDIA_TYPE_MKV ||
           type == MEDIA_TYPE_AVI;
}

static int add_media_item(struct media_list *list,
                          const char *path,
                          const char *name,
                          enum media_type type)
{
    struct media_item *item;

    if (list->count >= MEDIA_LIST_MAX_ITEMS)
        return 0;
    if (type == MEDIA_TYPE_UNKNOWN)
        return 0;

    item = &list->items[list->count++];
    snprintf(item->path, sizeof(item->path), "%s", path);
    snprintf(item->name, sizeof(item->name), "%s", name);
    item->type = type;
    item->playable = is_first_version_playable(type);
    return 1;
}

static void scan_dir(struct media_list *list, const char *dir_path)
{
    DIR *dir;
    struct dirent *entry;

    dir = opendir(dir_path);
    if (dir == NULL)
        return;

    while ((entry = readdir(dir)) != NULL) {
        char path[MEDIA_LIST_PATH_MAX];
        struct stat st;
        enum media_type type;

        if (strcmp(entry->d_name, ".") == 0 || strcmp(entry->d_name, "..") == 0)
            continue;

        snprintf(path, sizeof(path), "%s/%s", dir_path, entry->d_name);
        if (stat(path, &st) < 0 || !S_ISREG(st.st_mode))
            continue;

        type = detect_media_type(entry->d_name);
        add_media_item(list, path, entry->d_name, type);
    }

    closedir(dir);
}

void media_list_init(struct media_list *list)
{
    if (list != NULL)
        memset(list, 0, sizeof(*list));
}

int media_list_scan(struct media_list *list, const char *usb_root)
{
    char dir[MEDIA_LIST_PATH_MAX];
    struct stat st;

    if (list == NULL || usb_root == NULL)
        return -1;

    media_list_init(list);
    if (stat(usb_root, &st) < 0 || !S_ISDIR(st.st_mode))
        return -1;

    snprintf(dir, sizeof(dir), "%s/T113Camera/VIDEO", usb_root);
    scan_dir(list, dir);
    snprintf(dir, sizeof(dir), "%s/T113Camera/DCIM", usb_root);
    scan_dir(list, dir);
    snprintf(dir, sizeof(dir), "%s/T113Camera/AUDIO", usb_root);
    scan_dir(list, dir);
    return 0;
}

const char *media_type_name(enum media_type type)
{
    switch (type) {
    case MEDIA_TYPE_MJPEG:
        return "MJPEG";
    case MEDIA_TYPE_JPEG:
        return "JPG";
    case MEDIA_TYPE_WAV:
        return "WAV";
    case MEDIA_TYPE_MP4:
        return "MP4";
    case MEDIA_TYPE_MKV:
        return "MKV";
    case MEDIA_TYPE_AVI:
        return "AVI";
    case MEDIA_TYPE_AAC:
        return "AAC";
    case MEDIA_TYPE_MP3:
        return "MP3";
    default:
        return "UNKNOWN";
    }
}
