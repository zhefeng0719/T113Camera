#include "media_list.h"
#include "media_storage.h"

#include <dirent.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/stat.h>

static int str_ends_with_ci(const char *text, const char *suffix) /* 大小写无关的 ASCII 后缀比较，兼容 .JPG 和 .jpg。 */
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

    tail = text + text_len - suffix_len; /* 仅比较文件名尾部，不能用子串匹配误认格式。 */
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

static enum media_type detect_media_type(const char *name) /* 扩展名只决定候选类型，实际 codec 支持由播放探测验证。 */
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

static bool is_first_version_playable(enum media_type type) /* 表示有对应播放入口，不保证文件内编码一定可解。 */
{
    return type == MEDIA_TYPE_JPEG || type == MEDIA_TYPE_MJPEG ||
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

    if (list->count >= MEDIA_LIST_MAX_ITEMS) /* 有界列表限制内存与 UI 项目数量。 */
        return 0;
    if (type == MEDIA_TYPE_UNKNOWN) /* 临时 .part 或其他未识别文件不加入媒体列表。 */
        return 0;

    if (strlen(path) >= sizeof(list->items[0].path) ||
        strlen(name) >= sizeof(list->items[0].name))
        return 0;
    item = &list->items[list->count++]; /* 长度检查通过后才占用列表槽位。 */
    snprintf(item->path, sizeof(item->path), "%s", path);
    snprintf(item->name, sizeof(item->name), "%s", name);
    item->type = type; /* 记录分派类型，应用据此选择照片、音频或容器播放。 */
    item->playable = is_first_version_playable(type); /* 保留支持标记，不在扫描时打开硬件解码器。 */
    return 1;
}

static void scan_dir(struct media_list *list, const char *dir_path)
{
    DIR *dir;
    struct dirent *entry;

    dir = opendir(dir_path); /* 逐目录扫描；目录不存在时该分类为空。 */
    if (dir == NULL)
        return;

    while ((entry = readdir(dir)) != NULL) { /* 遍历目录项，不递归进入任意子目录。 */
        char path[MEDIA_LIST_PATH_MAX];
        struct stat st;
        enum media_type type;

        if (strcmp(entry->d_name, ".") == 0 || strcmp(entry->d_name, "..") == 0)
            continue;

        int n = snprintf(path, sizeof(path), "%s/%s", dir_path, entry->d_name);
        if (n < 0 || (size_t)n >= sizeof(path) ||
            lstat(path, &st) < 0 || !S_ISREG(st.st_mode)) /* 只接收真实普通文件，拒绝符号链接与目录。 */
            continue;

        type = detect_media_type(entry->d_name); /* 按名字后缀分类，扫描不进行解码。 */
        add_media_item(list, path, entry->d_name, type); /* 路径和名称复制到有界列表，不借用 readdir 临时内存。 */
    }

    closedir(dir); /* 释放目录扫描句柄。 */
}

void media_list_init(struct media_list *list)
{
    if (list != NULL)
        memset(list, 0, sizeof(*list));
}

static int compare_items(const void *a, const void *b)
{
    const struct media_item *left = a, *right = b;
    return strcmp(left->path, right->path); /* 按完整路径稳定排序，使重新扫描后的顺序可预测。 */
}

int media_list_scan(struct media_list *list, const char *usb_root)
{
    char dir[MEDIA_LIST_PATH_MAX];
    struct stat st;

    if (list == NULL || usb_root == NULL)
        return -1;

    media_list_init(list);
    if (!media_storage_ready(usb_root, 0) || stat(usb_root, &st) < 0 || !S_ISDIR(st.st_mode))
        return -1;

    snprintf(dir, sizeof(dir), "%s/T113Camera/VIDEO", usb_root);
    scan_dir(list, dir);
    snprintf(dir, sizeof(dir), "%s/T113Camera/DCIM", usb_root);
    scan_dir(list, dir);
    snprintf(dir, sizeof(dir), "%s/T113Camera/AUDIO", usb_root);
    scan_dir(list, dir);
    qsort(list->items, list->count, sizeof(list->items[0]), compare_items); /* 扫描完成再统一排序，避免文件系统枚举顺序影响选择。 */
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
