#include "storage_monitor.h" /* 被测模块，不引入 UI、FFmpeg 或硬件解码依赖。 */
#include <stdio.h> /* 测试结果输出。 */
#include <string.h> /* 构造探测快照。 */
#include <unistd.h> /* 等待监控线程。 */
#define CHECK(x) do { if (!(x)) { fprintf(stderr, "FAIL line %d\n", __LINE__); goto out; } } while (0) /* 统一清理失败路径。 */
struct fixture { pthread_mutex_t mutex; struct storage_snapshot snapshot; }; /* 测试线程安全的设备替身。 */
static int fake_probe(void *context, struct storage_snapshot *result)
{
    struct fixture *fixture = context; /* 从上下文取得可控设备状态。 */
    pthread_mutex_lock(&fixture->mutex); *result = fixture->snapshot; pthread_mutex_unlock(&fixture->mutex); /* 模拟一致快照。 */
    return 0; /* 不访问任何真实磁盘。 */
}
static void change(struct fixture *fixture, enum storage_state state, unsigned int mount_id, const char *root)
{
    pthread_mutex_lock(&fixture->mutex); /* 与探测线程同步。 */
    memset(&fixture->snapshot, 0, sizeof(fixture->snapshot)); /* 清除旧盘身份。 */
    fixture->snapshot.state = state; fixture->snapshot.mount_id = mount_id; /* 模拟挂载变化及只读属性。 */
    snprintf(fixture->snapshot.root, sizeof(fixture->snapshot.root), "%s", root); /* 模拟动态路径。 */
    pthread_mutex_unlock(&fixture->mutex); /* 发布新探测输入。 */
}
static int wait_change(struct storage_monitor *monitor, uint64_t generation, struct storage_snapshot *snapshot)
{
    for (int i = 0; i < 200; i++) { /* 最多等待 2 秒，避免失败测试无限等待。 */
        storage_monitor_snapshot(monitor, snapshot); /* 不访问模块内部可变快照。 */
        if (snapshot->generation != generation) return 0; /* 新会话已被检测到。 */
        usleep(10000); /* 让出 CPU 等后台线程工作。 */
    }
    return -1; /* 超时视为测试失败。 */
}
int main(void)
{
    struct storage_monitor monitor; struct storage_snapshot snapshot; /* 模块实例和读取副本。 */
    struct fixture fixture = {.mutex = PTHREAD_MUTEX_INITIALIZER}; /* 启动时没有 U 盘。 */
    volatile int stop = 0; int ret = 1; /* 模拟 FFmpeg 停止标志，默认测试失败。 */
    if (storage_monitor_start(&monitor, fake_probe, &fixture) < 0) return 1; /* 线程创建失败不能继续。 */
    storage_monitor_snapshot(&monitor, &snapshot); CHECK(snapshot.state == STORAGE_MISSING); /* 缺盘启动。 */
    change(&fixture, STORAGE_WRITABLE, 10, "/mnt/usb/sda1"); /* 第一张盘挂载。 */
    CHECK(wait_change(&monitor, snapshot.generation, &snapshot) == 0); /* 观察插入。 */
    CHECK(storage_monitor_cancelled(&monitor)); CHECK(storage_monitor_adopt(&monitor, snapshot.generation) == 0); /* 主线程确认后允许任务。 */
    storage_monitor_bind(&monitor, &stop); /* 任务绑定栈变量，测试结束前必须解绑。 */
    change(&fixture, STORAGE_MISSING, 0, ""); /* 模拟拔出。 */
    CHECK(wait_change(&monitor, snapshot.generation, &snapshot) == 0);
    CHECK(__atomic_load_n(&stop, __ATOMIC_ACQUIRE) == 1 && storage_monitor_cancelled(&monitor)); /* 必须取消任务。 */
    CHECK(storage_monitor_adopt(&monitor, snapshot.generation) < 0); /* 活跃绑定不得确认新会话。 */
    change(&fixture, STORAGE_WRITABLE, 11, "/mnt/usb/sda1"); /* 同路径重插，但挂载编号不同。 */
    CHECK(wait_change(&monitor, snapshot.generation, &snapshot) == 0);
    CHECK(storage_monitor_cancelled(&monitor)); /* 不能因为盘已回来就恢复旧任务。 */
    storage_monitor_bind(&monitor, NULL); CHECK(storage_monitor_adopt(&monitor, snapshot.generation) == 0); /* 收尾后重置取消。 */
    change(&fixture, STORAGE_READ_ONLY, 12, "/mnt/usb/sdb1"); /* 路径与权限变化。 */
    CHECK(wait_change(&monitor, snapshot.generation, &snapshot) == 0);
    CHECK(snapshot.state == STORAGE_READ_ONLY && !strcmp(snapshot.root, "/mnt/usb/sdb1")); /* 支持只读播放和动态挂载点。 */
    CHECK(storage_monitor_adopt(&monitor, snapshot.generation - 1) < 0); /* 禁止过期快照确认。 */
    __atomic_store_n(&stop, 0, __ATOMIC_RELEASE); /* 排除前一任务遗留停止值。 */
    storage_monitor_bind(&monitor, &stop); /* 已失效会话绑定时立即取消。 */
    CHECK(__atomic_load_n(&stop, __ATOMIC_ACQUIRE) == 1); /* 不丢失取消通知。 */
    ret = 0; /* 全部断言通过。 */
out:
    storage_monitor_bind(&monitor, NULL); storage_monitor_close(&monitor); /* 先解绑再 join，栈指针不会悬空。 */
    pthread_mutex_destroy(&fixture.mutex); /* 清理测试探测器资源。 */
    printf("Storage sessions, unplug, reinsert, read-only, path change and binding lifetime: %s\n", ret ? "FAIL" : "PASS");
    return ret; /* 可由脚本检查退出码。 */
}
