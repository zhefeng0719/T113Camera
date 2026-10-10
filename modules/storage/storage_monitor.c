#include "storage_monitor.h" /* 存储状态机接口。 */
#include <stdio.h> /* 挂载记录和路径格式化。 */
#include <string.h> /* 快照比较和事件字段匹配。 */
#include <stdlib.h> /* realpath。 */
#include <mntent.h> /* 解析 /proc/mounts，包含转义路径处理。 */
#include <sys/stat.h> /* 校验块设备与挂载目录。 */
#include <sys/sysmacros.h> /* major/minor。 */
#include <sys/socket.h> /* 内核 uevent 套接字。 */
#include <linux/netlink.h> /* NETLINK_KOBJECT_UEVENT。 */
#include <poll.h> /* 等待事件或 300ms 探测周期。 */
#include <unistd.h> /* close。 */
#include <time.h> /* 单调时钟控制探测频率，不随 VSYNC 事件提高轮询速率。 */

/* mount ID 来自 mountinfo；只解析固定前缀，忽略文件系统特有尾部。 */
static unsigned int find_mount_id(dev_t device, const char *root)
{
    FILE *file = fopen("/proc/self/mountinfo", "r"); /* 获取当前进程的挂载视图。 */
    char line[4096], point[1024], decoded[1024]; /* mountinfo 使用八进制转义路径。 */
    unsigned int id = 0, parent, ma, mi, found = 0; /* 只返回目标挂载的编号。 */
    if (!file) return 0; /* 无法确认身份时拒绝使用该卷。 */
    while (fgets(line, sizeof(line), file)) { /* 遍历每条挂载记录。 */
        if (sscanf(line, "%u %u %u:%u %*s %1023s", &id, &parent, &ma, &mi, point) != 5) /* 只解析挂载 ID、父 ID、设备号和挂载路径，避免依赖文件系统尾字段。 */
            continue; /* 跳过格式不完整的记录。 */
        size_t i = 0, j = 0; /* 分别遍历输入和输出路径。 */
        while (point[i] && j + 1 < sizeof(decoded)) { /* 解码空格等挂载路径转义。 */
            unsigned int value; /* 三位八进制转义对应的字节。 */
            if (point[i] == '\\' && strlen(point + i) >= 4 &&
                point[i+1] >= '0' && point[i+1] <= '7' && point[i+2] >= '0' && point[i+2] <= '7' &&
                point[i+3] >= '0' && point[i+3] <= '7' && sscanf(point + i + 1, "%3o", &value) == 1) {
                decoded[j++] = (char)value; i += 4; /* 只消费一个转义序列。 */
            } else decoded[j++] = point[i++]; /* 普通路径字节直接复制。 */
        }
        decoded[j] = '\0'; /* 确保比较字符串结束。 */
        if (ma == major(device) && mi == minor(device) && !strcmp(decoded, root)) { found = id; break; } /* 设备号和解码后挂载路径同时匹配，确认同一挂载对象。 */
    }
    fclose(file); /* 关闭 proc 文件，不长期持有快照。 */
    return found; /* 同路径重新挂载后编号改变。 */
}

int storage_probe_usb(void *unused, struct storage_snapshot *result)
{
    FILE *mounts; struct mntent *entry, record; char buffer[4096]; /* 使用可重入解析，避免与主线程争用静态缓冲。 */
    struct storage_snapshot candidate; /* 一次候选探测全部完成后才发布。 */
    struct stat device, directory; /* 块设备号和实际文件系统设备号。 */
    char sysfs[128]; /* major:minor 对应的内核设备路径。 */
    (void)unused; memset(result, 0, sizeof(*result)); /* 默认没有就绪 U 盘。 */
    mounts = setmntent("/proc/mounts", "r"); /* 复用系统自动挂载，不在应用里挂载。 */
    if (!mounts) return -1; /* 探测失败时仍保留 missing 输出。 */
    while ((entry = getmntent_r(mounts, &record, buffer, sizeof(buffer)))) { /* 支持 sda、sda1、sdb1 等挂载。 */
        if (strncmp(entry->mnt_dir, "/mnt/usb/", 9) || strncmp(entry->mnt_fsname, "/dev/sd", 7)) continue; /* 只选系统 USB 自动挂载下的 sd 块设备，不扫描板载 flash。 */
        memset(&candidate, 0, sizeof(candidate)); /* 候选记录不继承上一设备数据。 */
        if (strlen(entry->mnt_dir) >= sizeof(candidate.root) || strlen(entry->mnt_fsname) >= sizeof(candidate.device)) continue; /* 过长路径不截断采用，防止身份比较误认不同卷。 */
        if (stat(entry->mnt_fsname, &device) < 0 || !S_ISBLK(device.st_mode)) continue; /* 节点必须是真实块设备。 */
        snprintf(sysfs, sizeof(sysfs), "/sys/dev/block/%u:%u", major(device.st_rdev), minor(device.st_rdev));
        char *identity = realpath(sysfs, NULL); /* libc 分配足够大的路径缓冲，避免 PATH_MAX 溢出。 */
        if (!identity) continue; /* 拔出后挂载记录可能残留，sysfs 可以排除它。 */
        if (strlen(identity) >= sizeof(candidate.identity)) { free(identity); continue; }
        strcpy(candidate.identity, identity); free(identity); /* 快照只持有路径副本，不持有分配对象。 */
        if (stat(entry->mnt_dir, &directory) < 0 || !S_ISDIR(directory.st_mode) || directory.st_dev != device.st_rdev) continue;
        candidate.mount_id = find_mount_id(device.st_rdev, entry->mnt_dir); /* 校验实际挂载而非空目录。 */
        if (!candidate.mount_id) continue; /* 无法确认挂载身份则暂不使用。 */
        candidate.major_id = major(device.st_rdev); candidate.minor_id = minor(device.st_rdev); /* 保存设备编号。 */
        strcpy(candidate.root, entry->mnt_dir); strcpy(candidate.device, entry->mnt_fsname); /* 保存实际路径。 */
        candidate.state = hasmntopt(entry, "ro") ? STORAGE_READ_ONLY : STORAGE_WRITABLE; /* 播放允许只读。 */
        if (!result->state || strcmp(candidate.root, result->root) < 0) *result = candidate; /* 多盘时固定选字典序第一卷。 */
    }
    endmntent(mounts); return 0; /* 不执行磁盘读写，避免探测线程被文件 I/O 拖住。 */
}

static void invalidate_locked(struct storage_monitor *monitor)
{
    __atomic_store_n(&monitor->cancel, 1, __ATOMIC_RELEASE); /* 失效信号保持，快速重插也不能恢复旧任务。 */
    if (monitor->task_stop) __atomic_store_n(monitor->task_stop, 1, __ATOMIC_RELEASE); /* 只通知，不关闭别的线程资源。 */
}

/* 只比较有效字段；C 结构体 padding 不参与身份判断，避免误取消。 */
static int same_volume(const struct storage_snapshot *left, const struct storage_snapshot *right)
{
    if (left->state != right->state) return 0; /* 就绪和读写权限变化均产生新会话。 */
    if (left->state == STORAGE_MISSING) return 1; /* 缺盘状态不依赖残留的设备路径。 */
    return left->major_id == right->major_id && left->minor_id == right->minor_id &&
           left->mount_id == right->mount_id && !strcmp(left->root, right->root) &&
           !strcmp(left->device, right->device) && !strcmp(left->identity, right->identity); /* 检查完整设备与挂载身份。 */
}

/* 监听 remove 事件弥补两次轮询之间快速拔插的窗口。 */
static int drain_events(struct storage_monitor *monitor)
{
    char data[4096]; struct sockaddr_nl sender; socklen_t length; ssize_t size; /* 内核消息及发送者信息。 */
    int relevant = 0; /* 屏幕 VSYNC 也使用此总线，但不能触发磁盘探测。 */
    for (int count = 0; count < 64; count++) { /* 限制单批事件量，避免事件风暴阻止退出。 */
        length = sizeof(sender); memset(&sender, 0, sizeof(sender)); /* 每次 recvfrom 重置地址。 */
        size = recvfrom(monitor->event_fd, data, sizeof(data), MSG_DONTWAIT, (struct sockaddr *)&sender, &length);
        if (size <= 0) break; /* 套接字没有更多数据。 */
        if (sender.nl_pid != 0) continue; /* 不接受用户进程伪造的设备事件。 */
        int removed = 0, block = 0; const char *name = NULL; /* 仅关注块设备删除。 */
        for (size_t offset = 0; offset < (size_t)size;) { /* uevent 是多个 NUL 结束字段。 */
            size_t n = strnlen(data + offset, (size_t)size - offset); /* 不越过消息边界。 */
            if (n == (size_t)size - offset) break; /* 截断字段不参与解析。 */
            const char *field = data + offset; /* 当前完整事件字段。 */
            if (!strcmp(field, "ACTION=remove")) removed = 1; /* 记录内核块设备删除动作。 */
            if (!strcmp(field, "SUBSYSTEM=block")) block = 1; /* 只处理磁盘事件，VSYNC 不能触发挂载探测。 */
            if (!strncmp(field, "DEVNAME=", 8)) name = field + 8; /* 借用当前消息内的设备名，仅在本轮消息处理期间有效。 */
            offset += n + 1; /* 移动到下一个字段。 */
        }
        pthread_mutex_lock(&monitor->mutex); /* 保护当前设备和绑定指针。 */
        if (block) relevant = 1; /* 只有块设备事件需要提前重新探测挂载。 */
        if (removed && block && name && monitor->current.state) {
            const char *active = monitor->current.device + 5; size_t n = strlen(name); /* 去掉 /dev/ 前缀。 */
            if (!strncmp(active, name, n) && (!active[n] || (active[n] >= '0' && active[n] <= '9'))) {
                memset(&monitor->current.root, 0, sizeof(monitor->current.root)); /* 先使当前卷失效。 */
                monitor->current.state = STORAGE_MISSING; monitor->current.generation++; /* 即使立即重插也产生新会话。 */
                invalidate_locked(monitor); /* 旧任务必须退出。 */
            }
        }
        pthread_mutex_unlock(&monitor->mutex); /* 不在锁内做设备或文件操作。 */
    }
    return relevant; /* 其他内核事件仅被消费，不提高轮询速率。 */
}

static int64_t monotonic_ms(void)
{
    struct timespec now; /* 不受系统时间或 RTC 调整影响。 */
    clock_gettime(CLOCK_MONOTONIC, &now); /* 控制 300ms 的兜底探测间隔。 */
    return (int64_t)now.tv_sec * 1000 + now.tv_nsec / 1000000; /* 返回毫秒。 */
}

static void *monitor_thread(void *context)
{
    struct storage_monitor *monitor = context; /* 线程只持有存储模块上下文。 */
    int64_t next_check = monotonic_ms() + 300; /* 启动阶段已探测，不立即重复扫描。 */
    while (!__atomic_load_n(&monitor->stopping, __ATOMIC_ACQUIRE)) { /* close 通知退出。 */
        struct pollfd event = {monitor->event_fd, POLLIN, 0}; struct storage_snapshot next; /* 当前探测候选。 */
        int64_t remaining = next_check - monotonic_ms(); /* 非块设备事件不能重置截止时间。 */
        poll(&event, monitor->event_fd >= 0 ? 1 : 0, remaining > 0 ? (int)remaining : 0); /* 事件唤醒或定期探测。 */
        int relevant = monitor->event_fd >= 0 ? drain_events(monitor) : 0; /* 优先处理 remove。 */
        if (!relevant && monotonic_ms() < next_check) continue; /* 过滤 VSYNC 等高频无关事件。 */
        next_check = monotonic_ms() + 300; /* 探测周期独立于屏幕刷新率。 */
        memset(&next, 0, sizeof(next)); monitor->probe(monitor->probe_context, &next); /* 探测无需应用模块参与。 */
        next.generation = 0; /* 代号由状态机生成，不由探测器决定。 */
        pthread_mutex_lock(&monitor->mutex); /* 原子发布完整快照。 */
        if (!same_volume(&monitor->current, &next)) { /* 不使用 memcmp 比较未规定的结构体填充字节。 */
            next.generation = monitor->current.generation + 1; monitor->current = next; /* 保留单调递增编号。 */
            invalidate_locked(monitor); /* 直到主线程清理并 adopt 才允许下一任务。 */
        }
        pthread_mutex_unlock(&monitor->mutex); /* LVGL 和媒体操作不在此线程执行。 */
    }
    return NULL; /* 返回后 close 才销毁上下文。 */
}

int storage_monitor_start(struct storage_monitor *monitor, storage_probe_fn probe, void *context)
{
    memset(monitor, 0, sizeof(*monitor)); monitor->event_fd = -1; /* 初始化所有拥有的资源。 */
    monitor->probe = probe ? probe : storage_probe_usb; monitor->probe_context = context; /* 测试可替换探测。 */
    if (pthread_mutex_init(&monitor->mutex, NULL)) return -1; /* 互斥锁创建失败不能启动线程。 */
    monitor->probe(context, &monitor->current); monitor->current.generation = 1; /* 启动立即探测，不等首个周期。 */
    if (!probe) { /* 测试替身不接收真实系统事件。 */
        struct sockaddr_nl address = {0}; address.nl_family = AF_NETLINK; address.nl_groups = 1; /* 自动分配端口。 */
        monitor->event_fd = socket(AF_NETLINK, SOCK_DGRAM | SOCK_CLOEXEC | SOCK_NONBLOCK, NETLINK_KOBJECT_UEVENT);
        if (monitor->event_fd >= 0 && bind(monitor->event_fd, (struct sockaddr *)&address, sizeof(address)) < 0) {
            close(monitor->event_fd); monitor->event_fd = -1; /* 无 netlink 权限时仍能轮询。 */
        }
    }
    if (pthread_create(&monitor->thread, NULL, monitor_thread, monitor)) { /* 启动检测线程。 */
        if (monitor->event_fd >= 0) close(monitor->event_fd); /* 启动失败释放套接字。 */
        pthread_mutex_destroy(&monitor->mutex); return -1; /* 不留下未启动的互斥锁。 */
    }
    monitor->started = 1; return 0; /* 主线程可以开始读取快照。 */
}

void storage_monitor_snapshot(struct storage_monitor *monitor, struct storage_snapshot *result)
{
    pthread_mutex_lock(&monitor->mutex); *result = monitor->current; pthread_mutex_unlock(&monitor->mutex); /* 读者取得一致快照。 */
}

int storage_monitor_adopt(struct storage_monitor *monitor, uint64_t generation)
{
    int ret = -1; pthread_mutex_lock(&monitor->mutex); /* 必须确认清理期间没有再次换盘。 */
    if (!monitor->task_stop && generation == monitor->current.generation) { /* 未解绑的任务不能重置取消信号。 */
        __atomic_store_n(&monitor->cancel, 0, __ATOMIC_RELEASE); ret = 0; /* 确认新会话。 */
    }
    pthread_mutex_unlock(&monitor->mutex); return ret; /* 失败时应用继续重试快照。 */
}

void storage_monitor_bind(struct storage_monitor *monitor, volatile int *stop)
{
    pthread_mutex_lock(&monitor->mutex); monitor->task_stop = stop; /* 解绑也使用同一把锁，防止悬空栈指针。 */
    if (stop && __atomic_load_n(&monitor->cancel, __ATOMIC_ACQUIRE)) __atomic_store_n(stop, 1, __ATOMIC_RELEASE);
    pthread_mutex_unlock(&monitor->mutex); /* 绑定已失效会话会立即收到取消。 */
}

int storage_monitor_cancelled(struct storage_monitor *monitor)
{
    return __atomic_load_n(&monitor->cancel, __ATOMIC_ACQUIRE); /* 无锁查询，适合播放循环。 */
}

void storage_monitor_close(struct storage_monitor *monitor)
{
    if (!monitor->started) return; /* 未启动或已关闭时无需释放。 */
    __atomic_store_n(&monitor->stopping, 1, __ATOMIC_RELEASE); pthread_join(monitor->thread, NULL); /* 最多等待一个探测周期。 */
    if (monitor->event_fd >= 0) close(monitor->event_fd); /* 关闭内核事件订阅。 */
    pthread_mutex_destroy(&monitor->mutex); monitor->started = 0; /* 不留下后台线程或互斥锁。 */
}
