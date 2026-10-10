#include "media_storage.h"
#include <errno.h>
#include <fcntl.h>
#include <limits.h>
#include <mntent.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/stat.h>
#include <sys/statvfs.h>
#include <sys/syscall.h>
#include <unistd.h>

int media_storage_ready(const char *root, int writable) /* 必须是真挂载点，防止拔盘后写入同名 flash 目录。 */
{
    char resolved[PATH_MAX];
    struct mntent *entry;
    struct mntent record; char buffer[4096]; /* 独立缓冲，不与存储监控线程共享 getmntent 静态内存。 */
    FILE *mounts;
    int found = 0;
    if (!root || !realpath(root, resolved))
        return 0;
    mounts = setmntent("/proc/mounts", "r");
    if (!mounts)
        return 0;
    while ((entry = getmntent_r(mounts, &record, buffer, sizeof(buffer)))) { /* 使用线程私有缓冲解析挂载表，避免与监控线程共享静态结果。 */
        if (!strcmp(entry->mnt_dir, resolved)) {
            found = !writable || !hasmntopt(entry, "ro"); /* 读操作允许只读盘，录制要求挂载选项不是 ro。 */
            break;
        }
    }
    endmntent(mounts); /* 关闭挂载记录文件，快照不长期占用句柄。 */
    return found && (!writable || access(root, W_OK) == 0); /* 挂载验证与路径写权限同时满足才允许保存。 */
}

static int valid_component(const char *text) /* 保存文件名仅允许单级组件，不允许目录穿越。 */
{
    return text && *text && !strchr(text, '/') && !strstr(text, "..");
}

int media_storage_next_name(const char *root, const char *folder,
                            const char *base, const char *extension,
                            char *name, size_t capacity)
{
    unsigned int index;
    char path[PATH_MAX];
    struct stat st;
    if (!valid_component(folder) || !valid_component(base) ||
        !valid_component(extension) || !name || !capacity)
        return -1;
    for (index = 0; index < 10000; index++) { /* 原名被占用时尝试数字后缀，数量有上限。 */
        int n = index ? snprintf(name, capacity, "%s_%03u", base, index) :
                        snprintf(name, capacity, "%s", base);
        if (n < 0 || (size_t)n >= capacity)
            return -1;
        n = snprintf(path, sizeof(path), "%s/T113Camera/%s/%s.%s", root, folder, name, extension);
        if (n < 0 || (size_t)n + 5 >= sizeof(path)) /* 同时预留 .part 后缀和结束符，禁止超长路径截断。 */
            return -1;
        if (lstat(path, &st) == 0) /* 已存在的正式文件或符号链接都不能覆盖。 */
            continue;
        if (errno != ENOENT) /* 仅确认为不存在才可选名，权限或 I/O 错误不能当空闲名称。 */
            return -1;
        strcat(path, ".part"); /* 失败残留的临时文件同样占用此名字。 */
        if (lstat(path, &st) < 0) {
            if (errno == ENOENT)
                return 0;
            return -1;
        }
    }
    errno = EEXIST; /* 达到命名尝试上限后明确报告名字被占用。 */
    return -1;
}

int media_storage_save_jpeg(const char *root, const char *base,
                            const void *data, size_t size,
                            char *path, size_t capacity)
{
    return media_storage_save_jpeg_cancelable(root, base, data, size, path, capacity, NULL); /* 保留独立测试接口。 */
}

int media_storage_save_jpeg_cancelable(const char *root, const char *base,
                                       const void *data, size_t size, char *path, size_t capacity,
                                       const volatile int *cancel)
{
    char directory[PATH_MAX], name[128], final[160], part[170];
    const unsigned char *jpeg = data;
    struct statvfs space;
    int dirfd = -1, fd = -1, ret = -1;
    size_t offset = 0;
    int n;
    if ((cancel && __atomic_load_n(cancel, __ATOMIC_ACQUIRE)) || !media_storage_ready(root, 1) || !jpeg || size < 4 ||
        jpeg[0] != 0xff || jpeg[1] != 0xd8 || !path || !capacity)
        return -1;
    n = snprintf(directory, sizeof(directory), "%s/T113Camera", root);
    if (n < 0 || (size_t)n >= sizeof(directory) ||
        (mkdir(directory, 0777) < 0 && errno != EEXIST))
        return -1;
    n = snprintf(directory, sizeof(directory), "%s/T113Camera/DCIM", root);
    if (n < 0 || (size_t)n >= sizeof(directory) ||
        (mkdir(directory, 0777) < 0 && errno != EEXIST))
        return -1;
    dirfd = open(directory, O_RDONLY | O_DIRECTORY | O_NOFOLLOW); /* 绑定目标目录对象并拒绝目录符号链接。 */
    if (dirfd < 0 || fstatvfs(dirfd, &space) < 0 ||
        (uint64_t)space.f_bavail * space.f_frsize < size + 8ULL * 1024 * 1024)
        goto out;
    if (media_storage_next_name(root, "DCIM", base, "jpg", name, sizeof(name)) < 0)
        goto out;
    snprintf(final, sizeof(final), "%s.jpg", name);
    snprintf(part, sizeof(part), "%s.part", final);
    n = snprintf(path, capacity, "%s/%s", directory, final);
    if (n < 0 || (size_t)n >= capacity)
        goto out;
    fd = openat(dirfd, part, O_WRONLY | O_CREAT | O_EXCL | O_NOFOLLOW, 0666); /* 相对目录独占创建临时文件，防止覆盖同名文件。 */
    if (fd < 0)
        goto out;
    while (offset < size) { /* 逐次补齐 JPEG 短写，完整成功才允许发布。 */
        if (cancel && __atomic_load_n(cancel, __ATOMIC_ACQUIRE)) { errno = ECANCELED; goto out; } /* 停止旧盘写入。 */
        ssize_t wrote = write(fd, jpeg + offset, size - offset); /* 短写时继续写余下 JPEG 负载。 */
        if (wrote < 0 && errno == EINTR) /* 信号中断时重试未写入尾部。 */
            continue;
        if (wrote <= 0)
            goto out;
        offset += (size_t)wrote; /* 只累加实际写入字节数。 */
    }
    if (fsync(fd) < 0) /* 完整写入后先请求数据落盘，再发布正式名称。 */
        goto out;
    ret = close(fd); /* 检查关闭错误，已同步成功也不能忽略此结果。 */
    fd = -1; /* 句柄已关闭，统一清理出口不能再次 close。 */
    if (ret < 0)
        goto out;
    ret = -1; /* 正式改名与目录同步成功前始终保持失败结果。 */
    /* RENAME_NOREPLACE 在发布瞬间也保护外部创建的同名照片；不做覆盖式回退。 */
    if (cancel && __atomic_load_n(cancel, __ATOMIC_ACQUIRE)) { errno = ECANCELED; goto out; } /* 不发布取消照片。 */
    if (syscall(SYS_renameat2, dirfd, part, dirfd, final, 1U) < 0 || fsync(dirfd) < 0) /* 原子且不覆盖地改名，再同步目录项；失败不能宣告照片保存成功。 */
        goto out;
    ret = 0;
out:
    if (fd >= 0)
        close(fd);
    if (dirfd >= 0 && close(dirfd) < 0)
        ret = -1; /* 正式改名与目录同步成功前始终保持失败结果。 */
    return ret;
}
