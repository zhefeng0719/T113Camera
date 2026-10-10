/* 在独立挂载的 /tmp 上验证保存、命名和媒体扫描，不修改用户 U 盘。 */
#include "media_storage.h"
#include "media_list.h"
#include <stdio.h>
#include <string.h>
#include <sys/stat.h>
#include <unistd.h>
#include <fcntl.h>

int main(void)
{
    const unsigned char jpeg[] = {0xff, 0xd8, 0xff, 0xd9};
    char base[64], first[512], second[512], name[128], part[512];
    struct media_list list;
    size_t i;
    int found = 0, fd, ret = 1;
    volatile int cancel = 1; /* 取消的拍照任务不得创建或发布文件。 */
    snprintf(base, sizeof(base), "storage_test_%ld", (long)getpid());
    first[0] = second[0] = part[0] = '\0';
    if (!media_storage_ready("/tmp", 1) || media_storage_ready("/tmp/T113Camera", 1))
        goto out;
    if (media_storage_save_jpeg_cancelable("/tmp", base, jpeg, sizeof(jpeg), first, sizeof(first), &cancel) == 0)
        goto out; /* 保留原保存测试，同时增加取消入口验证。 */
    if (media_storage_save_jpeg("/tmp", base, jpeg, sizeof(jpeg), first, sizeof(first)) < 0 ||
        media_storage_save_jpeg("/tmp", base, jpeg, sizeof(jpeg), second, sizeof(second)) < 0 ||
        !strcmp(first, second))
        goto out;
    if (media_storage_next_name("/tmp", "DCIM", base, "jpg", name, sizeof(name)) < 0 ||
        !strstr(name, "_002"))
        goto out;
    snprintf(part, sizeof(part), "/tmp/T113Camera/DCIM/%s.jpg.part", name);
    fd = open(part, O_WRONLY | O_CREAT | O_EXCL, 0600);
    if (fd < 0)
        goto out;
    close(fd);
    if (media_storage_next_name("/tmp", "DCIM", base, "jpg", name, sizeof(name)) < 0 ||
        !strstr(name, "_003"))
        goto out;
    if (media_storage_next_name("/tmp", "DCIM", "../escape", "jpg", name, sizeof(name)) == 0)
        goto out;
    if (media_list_scan(&list, "/tmp") < 0)
        goto out;
    for (i = 0; i < list.count; i++) {
        if (!strcmp(list.items[i].path, first) || !strcmp(list.items[i].path, second)) {
            if (!list.items[i].playable || list.items[i].type != MEDIA_TYPE_JPEG)
                goto out;
            found++;
        }
        if (strstr(list.items[i].path, ".part"))
            goto out;
    }
    if (found != 2)
        goto out;
    ret = 0;
out:
    if (*first) unlink(first);
    if (*second) unlink(second);
    if (*part) unlink(part);
    printf("storage: mount guard, unique names, part guard, path guard, JPEG cancel and scan\nRESULT: %s\n", ret ? "FAIL" : "PASS");
    return ret;
}
