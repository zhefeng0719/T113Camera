# T113Camera

全志 T113S3 相机与媒体播放器，使用 800x480 屏幕、UVC 摄像头、板载麦克风、
耳机/外接喇叭、U 盘和三个按键，暂不使用触摸。

状态：**功能验收候选版，等待用户审批**。按用户要求整理并提交 GitHub，
未安装开机服务或修改 SDK 镜像。改动、实测数据和待验收事项见
[验收报告](reports/2026-10-10-acceptance.md)。
JPG 切换与审查风险的后续修复见 [修复回归](reports/2026-10-10-review-fixes.md)，
旧验收报告中的二进制哈希属于上一版，不代表本次产物。

2026-10-10 清理：移除本地构建缓存、测试二进制、临时日志、测试素材副本和候选压缩包，
保留 `output/t113_camera`、测试源码及报告；清理板卡空的应用临时目录，
保留正式程序、独立测试工具和 U 盘媒体。构建产物与本地 `Dat/` 素材不提交。

## 当前功能

- MP3、AAC、PCM S16_LE WAV 播放，绿色音量条、播放进度和暂停。
- AVI/MP4/MKV 解封装；MJPEG/H264/H265 经 CedarC 硬解、G2D 缩放转换。
- JPG/JPEG 静态预览，媒体列表与当前项高亮。
- 摄像头实时预览，MJPEG + 48 kHz/S16_LE/mono PCM 录制，FFmpeg API 封装 AVI。
- 拍照直接保存摄像头 JPEG 压缩帧，不二次编码。
- U 盘热插拔监控：块设备事件 + 300ms 挂载核查，支持动态挂载路径；只读盘可播放但不可录制。
- 自动避开同名文件与 .part，例如 record1_001.avi、record1_001.jpg。
- 先写 .part，排尾/fsync 成功才发布；空间不足或 I/O 失败不报告保存成功。
- 单实例锁，切换先关闭旧任务，退出清理音频、解码器及显示图层。
- JPG 按实际尺寸独立解码，正确处理可见尺寸、stride 和 UV 平面高度。
- 录制使用独立写入线程；队列饱和时明确失败，不堵住预览线程或静默丢帧。
- UI 与视频采用交替显示缓冲，通过真实 VSYNC 事件等待图层提交生效。

## 按键

| 模式 | 操作 | 行为 |
| --- | --- | --- |
| 播放 | NEXT / ENTER 短按 | 下一项 / 上一项；先停止旧播放 |
| 播放 | BACK 短按 / 长按 | 播放暂停 / 停止播放后进入相机 |
| 相机 | BACK 长按 | 收尾录像，返回播放列表 |
| 相机 | NEXT / ENTER 短按 | 选择 record1..record5 命名基名 |
| 相机待机 | NEXT 长按 | 拍照 / 录像切换 |
| 相机 | BACK 短按 | 拍照，或开始 / 停止录像 |

长按阈值 1.5 秒；录像中不能换名字或类型。Ctrl+C/SIGTERM 正常收尾。
强制复位、SIGKILL、突然掉电及拔盘不能保证当前文件完整。

## 数据链路

### 源码阅读

业务源码和头文件已补充中文行内注释，重点说明数据单位、缓冲区所有权、
采集时间戳、音频主时钟、队列同步及失败收尾。多行调用按完整语句阅读，
空行、大括号和自明的日志不重复解释。LVGL 适配层、GUI Guider 生成代码
和第三方源码不在本轮注释范围内。建议从 `src/main.c` 的录制启动、逐帧处理、
停止收尾，再进入 `audio_capture`、`record_writer`、`ffmpeg_mjpeg_muxer` 阅读。
本轮仅修改注释，验证记录见 [注释核查](reports/2026-10-10-source-comments.md)。

```text
录制：V4L2 MJPEG + ALSA PCM 队列 -> FFmpeg AVI muxer -> U 盘
预览：MJPEG -> CedarC -> NV12/NV21 dmabuf -> G2D -> disp 视频层
播放：文件 -> FFmpeg demux -> CedarC -> G2D -> disp
      音轨 -> FFmpeg decoder / swresample -> ALSA
界面：LVGL 9 + GUI Guider -> 独立 disp UI 层
拍照：V4L2 JPEG 帧 -> 原始 JPEG 文件 -> DCIM
```

录制视频 PTS 来自采集时间，音频来自累计样本数，共用单调时钟起点。
主线程负责视频采集/预览，音频线程负责 PCM 采集，写入线程独占 AVI 封装器。
视频队列最多 8 帧、每帧最多 512 KiB，PCM 队列最多 64 个 block；
长期写入过慢会停止本次录制并保留 .part，而不是增长内存或伪报保存成功。
播放以 ALSA 实际消费进度为主时钟，视频按解码输出 PTS 显示；
落后超过 100 ms 丢弃旧画面，不跳过参考帧解码。暂停保存待播 PCM、
冻结时钟并在恢复后重放，避开本板硬件 pause 恢复的进度前跳。
播放处理逐帧音频 PTS：空洞补静音、重叠跳过，超过 10 秒的跳变明确报错。
FFmpeg 输入增加停止回调和每次 I/O 的 5 秒截止时间，但不能打断内核阻塞的系统调用。

## 目录与编译

```text
src/main.c          模式、按键、播放与录制调度
modules/audio       ALSA、PCM 采集、FFmpeg 音频播放
modules/video       V4L2、CedarC
modules/display     G2D、disp、framebuffer 基础工具
modules/storage     AVI、命名、JPEG 保存
modules/common      时钟与队列
modules/media       媒体扫描（非递归、最多 64 项、固定排序）
modules/input       input_event 三按键
modules/ui_lvgl     LVGL 显示适配
ui                  GUI Guider 生成代码与 LVGL
tests               存储、封装故障注入、播放时钟回归
tools               FFmpeg 构建与可选启动脚本
reports             验收报告
output              编译产物（不提交 Git）
```

### U 盘热插拔与模块边界

`modules/storage/storage_monitor.c/.h` 独立检测 `/mnt/usb/` 下的 sd 块设备挂载，
同时核查 sysfs、设备号和 mount ID；不执行挂载，不依赖 LVGL、ALSA 或 CedarC。
系统先自动挂载，应用再使用实际路径。多卷时固定选择挂载目录字典序第一卷。

```text
内核块设备事件 / 定时核查 -> 存储快照 + generation + 粘滞 cancel
主控制器 sync_storage_ui -> 旧任务收尾 -> 确认新会话 -> 清屏 / 重扫列表
播放模块 -> 通用 stop -> 取消 FFmpeg 读取 / ALSA 播放 -> join
录制模块 -> 通用 cancel -> 写线程失败 -> 不发布 AVI，保留 .part
LVGL 适配 -> 仅显示主线程提供的状态，不检测 U 盘或操作封装器
```

拔盘、换盘、重新挂载或只读属性变化均使旧会话失效；重新插入不会自动恢复
旧音频、旧录像或旧列表下标。监控通过锁保护任务停止指针，任务返回前必须解绑。
JPEG 写入及 AVI 写入/发布均检查取消信号，避免旧任务写到新会话。
这不是突然拔盘的完整性保证：内核阻塞 I/O 不能强制打断，FAT 也可能损坏。
当前没有 UI“安全弹出”操作；需要先停止录制，再 `sync` 并卸载实际挂载点后拔出。

编译回归：`make hotplug-test storage-test integrity-test media-regression-test`。
板端回归脚本为 `tests/run_hotplug_board.py`，需要显式设置 `T113_BOARD_PASSWORD`；
它会临时卸载/挂载 U 盘，不能在用户正在播放或录制时运行。
测试证据在 `output/hotplug-logs/`，详细结果与未验收边界见
[热插拔报告](reports/2026-10-10-usb-hotplug.md)。

Ubuntu 中编译；FFmpeg 缺失时先准备 4.4.4 源码，再执行构建脚本：

```sh
cd /mnt/hgfs/QZ-T113S/Project/T113Camera
# sh tools/build_ffmpeg.sh
make -j4
make storage-test integrity-test playback-timing-test
make media-regression-test
```

依赖 Tina SDK sysroot 的 ALSA、CedarC 和 ION 库，FFmpeg 静态链接。
最小 FFmpeg 包含 WAV/AVI/MOV/MKV 等 demuxer、PCM/MP3/AAC decoder、
AVI muxer 与 H264/HEVC Annex-B filter；配置见 tools/build_ffmpeg.sh。

Windows 上传，板端仅需应用可执行文件：

```powershell
scp E:\QZ-T113S\Project\T113Camera\output\t113_camera root@192.168.1.77:/root/Project/T113Camera/
```

```sh
cd /root/Project/T113Camera
chmod +x t113_camera
./t113_camera
./t113_camera 5  # 初始化后定时 5 秒带音频录制，同时预览
./t113_camera ui /mnt/usb/sda/T113Camera/VIDEO/guimie.mp4
AV_TIMING=1 ./t113_camera ui /mnt/usb/sda/T113Camera/VIDEO/record3.avi
T113_MIC_GAIN=8 T113_ADC_VOLUME=120 ./t113_camera
```

录制默认恢复已验证的 MIC1=8、ADC1=120，不改变耳机/喇叭自动路由或放大播放音量。
增益环境变量须为合法整数且在实际 mixer 范围内。
U 盘固定 /mnt/usb/sda/T113Camera/{VIDEO,AUDIO,DCIM}，设备固定
/dev/video0、/dev/input/event1、hw:0,0；换镜像需重新核对。

## 验收边界

- 容器首帧自动预览继续禁用，避免重启此前导致切换卡死的路径。
- 实际音质、外放、屏幕显示范围与物理口型同步仍需用户验收。
- H265 仅验证 640x480 Main/8-bit 素材，不承诺所有 profile、10-bit 或 4K。
- JPG/裸 MJPEG 整文件读取限制 16 MiB，大视频应使用容器。
- 文件 seek 为 64 位；真实写满、超过 2 GiB/接近 FAT32 4 GiB 上限、
  多小时录制和拔盘故障仍未实测，不能据此宣称量产可靠。
- 暂无自动降噪/AGC、音量 UI、掉电修复或后台自动恢复。
- 显示同步依赖 Tina 内核 DISP_VSYNC_EVENT_EN/VSYNC0 uevent；本 SDK 的
  FBIO_WAITFORVSYNC 未实现，不能作为缓冲复用的同步依据。
- tools/start_camera.sh 仅为可选前台入口，审批前不安装开机启动。
