# USB 热插拔与分层审查

## 本次修改

- 新增 `modules/storage/storage_monitor.c/.h`：内核 block uevent + 300ms 定时核查。
- 只接受 `/mnt/usb/` 下真实 sd 块设备挂载；校验 sysfs、设备号、mount ID 与读写属性。
- 不固定 `/mnt/usb/sda`；支持系统挂载为 sda1/sdb1 等路径，多卷时选择挂载目录字典序第一卷。
- generation 区分存储会话；cancel 为粘滞信号，同路径重插不能复活旧任务。
- bind/unbind 使用互斥锁保护任务停止指针，任务返回前解绑，避免监控访问失效栈变量。
- 主控制器 `sync_storage_ui` 统一完成旧任务关闭、清屏、新会话确认、列表扫描及 UI 刷新。
- 播放、暂停、容器探测/读取接受取消；存储丢失不再被误报为正常 EOF。
- 录制写线程、AVIO 写入、AVI 收尾发布及 JPEG 保存检查通用取消信号；失败保留 .part。
- 缺盘时实际保存根目录为空，不回退到板载 flash 创建录像目录。
- 将挂载解析改为 `getmntent_r`，避免后台监控与主线程共享 libc 静态缓冲。
- 读取照片/裸流分为 64 KiB 块，在块间检查取消；不承诺打断单次内核 read。
- 新增/修改路径补充中文逐行学习注释；录制写线程补充所有权、环形队列与 join 顺序注释。

## 分层结论

```text
storage_monitor：设备与挂载状态，不包含媒体或 UI 接口
main/sync_storage_ui：模式协调，拥有会话与任务生命周期
media_list：目录扫描及排序，不负责检测或挂载设备
audio/video：采集、解码、播放，只接收通用 stop/cancel
record_writer/muxer/media_storage：写入、封装、发布，不调用 LVGL
ui_lvgl：显示数据与状态，所有调用由主线程执行
```

热插拔没有加入 LVGL 内部，也没有让存储线程直接关闭 CedarC、ALSA 或 FFmpeg。
写入线程独占运行中的封装器；主线程先停止生产者、join 写线程，再执行收尾。
取消信号的生命周期覆盖写入及最终发布，新会话只在旧任务全部收尾后确认。
快照比较使用具体字段，不比较 C 结构体的 padding；只有实际身份变化才中止任务。
`main.c` 仍然较厚，保留既有模式控制和播放会话函数，本次未大规模迁移无关业务。
未来新增播放功能时可按“播放会话/录制会话”继续抽取，不应把业务移进 UI 或监控层。

## 调试过程

监听内核事件后发现屏幕 VSYNC 也走同一条 uevent 总线。
最初每个事件都触发挂载探测，会把 300ms 探测提升到屏幕刷新频率；
改为只让 block 事件提前探测，其他事件不改变定时截止时间，并限制每批事件数。

连续重开音频/摄像头时，显示曾报告 VSYNC 超时。
仅将等待上限由 100ms 改为 500ms 并未解决；进一步发现接收代码遇到
netlink `ENOBUFS` 就退出，却统一输出“超时”，掩盖了事件队列溢出。
现在遇到溢出继续排空并等待提交之后的真实 VSYNC，不以 sleep 猜测显示完成。
最终实测日志出现 `event overflow, waiting for fresh boundary`，随后录制正常完成。
未修改屏幕时序、色彩格式、LVGL 生成布局或音频增益。

## 已验证

回归脚本：`tests/run_hotplug_board.py`，证据：`output/hotplug-logs/`。
测试只模拟卸载/重挂载，不声称等价于真实拔出 USB。

| 检查 | 结果 |
| --- | --- |
| 缺盘/插入/移除/同路径重插/只读/路径变化状态机 | PASS |
| bind 生命周期、过期 generation 拒绝、粘滞取消 | PASS |
| JPEG 取消、命名保护、.part 排除、媒体扫描 | PASS |
| 写线程排空、饱和失败、会话取消 | PASS |
| 封装器 11 类正常/故障场景，含发布前取消与同名竞争 | PASS |
| 空闲 UI 卸载、改挂 `/mnt/usb/t113-hotplug-test`、恢复原路径 | PASS |
| 只读挂载识别与相机入口阻止 | PASS |
| MP3 暂停期间丢失挂载，重挂后回到列表，不自动续播 | PASS |
| H264/AAC MP4 播放期间丢失挂载，内部线程退出 | PASS |
| 定时录制丢失挂载，退出失败，仅保留 .part | PASS |
| UI 录制中丢失挂载，音视频停止并回到列表，不发布 AVI | PASS |
| 重挂后正常 3 秒录制，检查文件直到 EOF | PASS |
| GCC -Wall/-Wextra/-Werror + ASan/UBSan 状态机测试 | PASS |
| diff --check | PASS |

最终恢复后正常录像：75 个 MJPEG packet，视频末尾 2980ms，PCM 3024ms，
尾部差约 44ms，`INSPECT: PASS`；上一轮同类回归尾部差约 26ms。
这些是短时回归数据，不代替长时间同步测试。
测试恢复原 vfat 挂载参数，清理本次生成的录像、.part 和板端测试工具，保留既有媒体。
正式板端仅更新 `/root/Project/T113Camera/t113_camera`，权限 0755；未安装开机服务或推送 Git。
最终候选 SHA256：`8b338e0640e71fc93f27dd1334a988f493093b9a2c228ecd47cc605b3fa1c0d9`。

## 仍需人工验收

- 真实拔插时的 USB remove 事件与板端自动挂载行为，包括快速拔插、换盘和多次循环。
- 真实拔出可能使 FAT 损坏、设备遗留挂载，不能保证当前录像完整或自动修复文件系统。
- FFmpeg 回调及 cancel 不能强制打断阻塞在内核中的 read/write/fsync/ioctl；join 可能延迟。
- 未实现 UI 安全弹出：先停止录制，再 sync，卸载实际挂载点后才能安全拔出。
- 自动选择范围仅 `/mnt/usb/`，不管理多盘切换菜单、其他挂载目录或多分区选择策略。
- 不自动恢复 .part，不自动续播/续录，不修改系统自动挂载脚本。
