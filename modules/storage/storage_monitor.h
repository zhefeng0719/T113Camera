#ifndef STORAGE_MONITOR_H /* 防止头文件重复包含。 */
#define STORAGE_MONITOR_H
#include <pthread.h> /* 监控线程及状态互斥锁。 */
#include <stdint.h> /* 会话编号使用固定宽度整数。 */

enum storage_state { STORAGE_MISSING, STORAGE_READ_ONLY, STORAGE_WRITABLE }; /* 未就绪、只读、可写。 */
struct storage_snapshot { /* 只传递数据，不持有 UI 或媒体模块对象。 */
    enum storage_state state; /* 已挂载且块设备仍存在才算就绪。 */
    uint64_t generation; /* 每次设备、挂载或权限变化递增。 */
    unsigned int major_id, minor_id, mount_id; /* 防止同路径的新挂载被当成旧盘。 */
    char root[512]; /* 系统实际挂载目录。 */
    char device[128]; /* 块设备节点，例如 /dev/sda1。 */
    char identity[512]; /* sysfs 设备路径，用于区分设备实例。 */
};
typedef int (*storage_probe_fn)(void *, struct storage_snapshot *); /* 可注入探测器，便于不拔盘测试状态机。 */
struct storage_monitor { /* 由应用持有，关闭前必须解绑所有任务。 */
    pthread_t thread; /* 仅检测状态的后台线程。 */
    pthread_mutex_t mutex; /* 保护快照和任务取消指针的生命周期。 */
    struct storage_snapshot current; /* 最近一次探测结果。 */
    volatile int cancel; /* 当前会话失效后保持为 1，直到主线程确认新会话。 */
    volatile int *task_stop; /* 可选：FFmpeg/ALSA 已有的任务停止标志。 */
    int stopping, started, event_fd; /* 线程退出、初始化状态和内核事件套接字。 */
    storage_probe_fn probe; /* 生产环境探测或测试替身。 */
    void *probe_context; /* 探测器私有数据。 */
};
int storage_probe_usb(void *unused, struct storage_snapshot *result); /* 只接受 /mnt/usb 下的真实 sd 块设备挂载。 */
int storage_monitor_start(struct storage_monitor *monitor, storage_probe_fn probe, void *context); /* NULL 探测器表示真实硬件。 */
void storage_monitor_snapshot(struct storage_monitor *monitor, struct storage_snapshot *result); /* 复制快照，不暴露内部可变数据。 */
int storage_monitor_adopt(struct storage_monitor *monitor, uint64_t generation); /* 旧任务清理后才能确认新会话。 */
void storage_monitor_bind(struct storage_monitor *monitor, volatile int *stop); /* NULL 解绑；返回后旧指针不再被监控访问。 */
int storage_monitor_cancelled(struct storage_monitor *monitor); /* 原子读取粘滞取消状态。 */
void storage_monitor_close(struct storage_monitor *monitor); /* join 后销毁互斥锁。 */
#endif
