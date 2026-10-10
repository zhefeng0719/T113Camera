# T113Camera

T113Camera 是基于全志 T113S3 开发板的嵌入式相机和媒体播放器原型。项目面向 5 寸 800x480 屏幕、UVC USB 摄像头、板载音频、U 盘存储和三按键交互，核心目标是完成一个可录制、可播放、可扩展的嵌入式音视频应用。

## 功能概览

支持 MP3/AAC/WAV 等音频播放，支持 MP4/AVI 等容器解析，支持 H264/MJPEG 硬件解码显示，支持 JPG 图片预览，支持 UVC USB 摄像头实时预览，支持 MJPEG 视频录制并封装为 AVI，支持板载音频采集与播放，支持 U 盘媒体扫描与保存，支持 LVGL 图形界面和三按键交互，支持 G2D 硬件缩放与格式转换。

## 当前能力

- 播放模式扫描 U 盘媒体目录并显示播放列表。
- 录制模式使用 USB UVC 摄像头采集 MJPEG，并实时显示到 UI 视频区域。
- 录制文件以 `record1.avi` 到 `record5.avi` 保存到 U 盘。
- AVI 录制采用 MJPEG 视频流和 PCM S16_LE 音频流。
- MP4 播放支持 H264/H265 视频流，FFmpeg 负责解封装，CedarC 负责硬解码。
- MP3/AAC 使用 FFmpeg 解码为 PCM 后交给 ALSA 播放。
- WAV 使用 PCM WAV 播放链路。
- JPG/JPEG 可在视频区域预览。
- 音频播放时，视频区域显示绿色音量条。
- 三个实体按键用于选择、确认、返回和模式切换。

## 项目进度快照

记录日期：2026-10-10

当前项目已经从单项硬件测试进入总项目整合阶段。USB 存储、UVC 摄像头、屏幕显示、G2D、CedarC、ALSA 音频、FFmpeg 解封装/封装、LVGL UI 和三按键输入都已经接入主程序。

当前主程序状态：

- 可启动 LVGL 播放界面，并自动清理原系统 LVGL 演示程序对屏幕的干扰。
- 可扫描 U 盘 `/mnt/usb/sda/T113Camera` 下的音频、图片和视频文件。
- 可播放 MP3/AAC/WAV 音频，播放时视频区域显示绿色音量条。
- 可预览 JPG/JPEG 图片。
- 可播放 MP4/AVI 等视频文件，其中 H264/H265/MJPEG 走 CedarC 硬解显示链路。
- 可长按进入录制模式，显示 USB 摄像头实时画面。
- 可选择 `record1` 到 `record5` 作为录制文件名，并保存为 AVI。
- 录制 AVI 当前采用 MJPEG 视频 + PCM S16_LE 音频。

预优化状态中暴露过的问题：

- 早期录制只保存 MJPEG 裸流，没有容器时间戳，播放时会出现速度过快、时长不准。
- 早期视频显示存在 CPU 缩放、G2D 黑屏、NV12/NV21 色彩异常等问题。
- 早期录制和播放切换时资源释放顺序不稳定，刚录完 AVI 后切换播放列表可能导致板端卡死。
- 早期 LVGL 和视频层刷新互相覆盖，录制模式 UI 有时被视频层盖住。
- 早期音频录制使用错误 ADC 通道或过高增益时，出现尖声、底噪大、播放速度异常。

已经完成的优化：

- 视频显示主链路收敛为 CedarC 硬解 + G2D 缩放/格式转换 + `/dev/disp` 图层显示。
- 默认使用已验证正常的 NV12/NV21 显示路径，避免红蓝错位和灰度异常。
- 录制增加 FFmpeg AVI muxer，解决裸 MJPEG 播放时长和帧率不可控的问题。
- 播放增加 FFmpeg demuxer/audio decoder，支持更多容器和音频格式。
- 显示、解码、G2D、disp 释放顺序已经收敛，降低模式切换时的资源竞争。
- 容器视频自动首帧预览暂时关闭，避免刚录制完成后马上预览触发卡死。
- 音频采集确认 ADC1 链路较稳定，采样率保持 48 kHz，增益降低后底噪明显下降。
- 代码按 audio、video、display、storage、media、input、ui_lvgl 模块拆分，主函数负责状态调度。

仍需继续优化：

- 播放帧率应进一步使用容器真实时间戳调度，而不是只依赖固定帧率估算。
- 录制时音视频同步仍需长期测试，后续可加入时间戳队列和丢帧策略。
- AVI/MP4 首帧预览需要改为异步、可取消、限频实现。
- 音频采集仍需继续优化增益、底噪、滤波和自动增益策略。
- 录制文件需要增加剩余空间检测、文件存在提示、覆盖保护或自动递增。
- UI 状态机还可以继续收束，减少播放、录制、预览之间的隐式状态耦合。

## 技术栈

- SoC：Allwinner T113S3
- 系统：Tina / Buildroot Linux
- 语言：C
- UI：LVGL 9 + NXP GUI Guider
- 显示：`/dev/disp` 图层
- 视频采集：V4L2 / UVC / MJPEG
- 视频解码：CedarC 硬件解码
- 图像处理：G2D 缩放和 YUV 到 ARGB8888 转换
- 音频：ALSA `hw:0,0`
- 容器和音频编解码：FFmpeg API
- 存储：U 盘 `/mnt/usb/sda/T113Camera`

## 目录结构

```text
src/
  main.c                    程序入口、模式切换、按键调度和主流程

modules/audio/
  audio_alsa.*              ALSA 播放/采集基础封装
  audio_capture.*           录制音频采集线程
  ffmpeg_audio_player.*     MP3/AAC 等音频解码播放
  wav_player.*              WAV 播放

modules/video/
  video_capture.*           V4L2 UVC 摄像头采集
  image_convert.*           CedarC 解码封装

modules/display/
  disp_layer_display.*      /dev/disp 图层显示
  g2d_display.*             G2D 缩放和格式转换
  framebuffer_display.*     framebuffer fallback

modules/storage/
  ffmpeg_mjpeg_muxer.*      AVI 封装
  ffmpeg_mjpeg_demuxer.*    AVI/MP4/MKV 解封装视频 packet
  ffmpeg_pcm_demuxer.*      PCM 音频解封装
  mjpeg_save.*              MJPEG 裸流保存工具

modules/media/
  media_list.*              U 盘媒体扫描和文件类型识别

modules/input/
  input_key.*               三按键输入

modules/ui_lvgl/
  t113_ui_lvgl.*            LVGL 到 disp 图层的适配

ui/
  GUI Guider 生成代码和 LVGL 依赖
```

## 媒体目录

板端 U 盘目录约定如下：

```text
/mnt/usb/sda/T113Camera/
  AUDIO/                    MP3 / AAC / WAV
  DCIM/                     JPG / JPEG
  VIDEO/                    AVI / MP4 / MJPEG
```

`Dat/` 目录只作为本地测试素材暂存目录，不建议提交到 GitHub。

## 数据链路

录制链路：

```text
UVC 摄像头
-> V4L2 MJPEG
-> CedarC 解码
-> G2D 缩放/转换
-> /dev/disp 显示

MJPEG packet
-> FFmpeg AVI muxer
-> U 盘 VIDEO/recordN.avi

板载麦克风
-> ALSA PCM S16_LE
-> FFmpeg AVI muxer
```

播放链路：

```text
U 盘媒体文件
-> FFmpeg demux
-> MJPEG/H264/H265 packet
-> CedarC 硬解
-> G2D 缩放/转换
-> /dev/disp 显示

MP3/AAC/WAV
-> FFmpeg 或 WAV parser
-> PCM S16_LE
-> ALSA 播放
```

## 按键交互

播放模式：

```text
NEXT  短按：下一项
ENTER 短按：上一项
BACK  短按：播放/暂停
BACK  长按：进入录制模式
```

录制模式：

```text
BACK  长按：返回播放模式
BACK  短按：开始/停止录制
NEXT  短按：选择下一个录制文件名
ENTER 短按：选择上一个录制文件名
```

## 编译

在 Ubuntu 虚拟机中编译：

```sh
cd /mnt/hgfs/QZ-T113S/Project/T113Camera
make
```

输出文件：

```text
output/t113_camera
```

## 上传和运行

从 Windows 上传到板卡：

```powershell
scp E:\QZ-T113S\Project\T113Camera\output\t113_camera root@192.168.1.77:/root/Project/T113Camera/t113_camera
```

在板卡运行：

```sh
cd /root/Project/T113Camera
chmod +x t113_camera
./t113_camera
```

指定文件播放测试：

```sh
./t113_camera ui /mnt/usb/sda/T113Camera/VIDEO/guimie.mp4
./t113_camera ui /mnt/usb/sda/T113Camera/AUDIO/Abdulla.mp3
./t113_camera ui /mnt/usb/sda/T113Camera/DCIM/yln.jpg
```

命令行录制测试：

```sh
./t113_camera 5
```

## 当前限制

- 容器视频首帧自动预览已暂时关闭，避免刚录制完成后切换 AVI 时触发 CedarC/G2D/disp 资源竞争。
- 录制采用 MJPEG + PCM AVI，文件体积较大，但链路简单、适合当前硬件。
- MP4/H264 播放已接入硬解，但帧率调度仍需继续优化为读取真实容器帧率。
- 麦克风录音质量仍有优化空间，包括增益、底噪和滤波。
- 当前交互基于三按键，暂不使用触摸。

## 后续计划

- 将播放、录制、UI 刷新整理成更清晰的状态机。
- 为 AVI/MP4 首帧预览设计异步、可取消、限频版本。
- 增加录制剩余空间检测和文件名自动递增。
- 完善音视频同步和真实帧率调度。
- 增加拍照模式，保存 JPG 到 `DCIM/`。
- 增加音量控制、录音增益配置和简单降噪。
- 增加开机自启动脚本和发布版镜像配置。
