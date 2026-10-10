# JPG 切换与审查风险修复

## 问题与修复

1. JPG 切换原来一直复用按 640x480 初始化的 MJPEG 解码器。现在先读取
   SOF 实际尺寸，每张独立照片重新创建解码状态，避免跨图尺寸/表状态残留。
   原始 MJPEG 帧边界解析只找第一个 FFD9；现按 JPEG marker 长度跳过
   APP/EXIF 等段，避免把内嵌缩略图的 EOI 当作外层照片结束。
2. U 盘 yln.jpg 是 2559x1403；CedarC 的真实分配平面是 2560x1408。
   原 G2D 使用可见高度计算 UV 起点，导致非对齐图片的色度寻址错误。
   现在分开描述可见裁剪区域、stride、分配高度，回退拷贝也保留偶数对齐。
3. 录制主线程不再执行 AVI 视频/PCM 写入。新增 record_writer，独占封装器，
   从有界视频队列和 PCM 队列消费。V4L2 帧复制后立即可归还，不等待 USB 写盘。
   视频队列饱和或写入失败会中止并保留 .part；正常停止先结束音频生产者，
   再排空写入队列、join 写线程、写尾/fsync/发布，禁止并发访问封装器。
4. AVI 发布改用 renameat2(RENAME_NOREPLACE) 的 syscall 兼容包装，
   即使录制中途其他程序创建同名正式文件，也不会被覆盖。
5. 音频播放不再只采信第一帧 PTS。每帧换算到采样时间线，扣除重采样延迟；
   时间空洞补静音、重叠裁掉，2ms 内舍入误差容忍，超过 10 秒跳变报错。
6. 显示 UI 使用 CPU staging + 两块扫描缓冲，视频 G2D 输出也使用双缓冲。
   SDK 的 framebuffer FBIO_WAITFORVSYNC 实际为空实现，所以采用
   /dev/disp 启用 VSYNC0 内核 uevent，通过 netlink 等待提交后的真实边界。
   单次等待上限 100ms，错误后 UI 停止继续复用可疑缓冲；不修改 UI 字体、颜色、位置。
7. FFmpeg 音视频输入新增 interrupt_callback，支持停止标志与 5 秒 I/O 截止。
   这只对 FFmpeg 可检查回调的位置有效，不宣称能取消内核 D 状态读写或 ioctl。

## 已完成回归

- Ubuntu Tina 工具链交叉编译成功；现有 FFmpeg 4.4 废弃 API 警告仍保留。
- JPEG marker/截断/尺寸、音频 gap/overlap/异常跳变、I/O 停止与截止逻辑测试通过。
- 写线程正常排空、模拟慢消费者导致队列饱和测试通过，提交路径未等待慢写入。
- 封装器 10 个用例通过：正常、abort、空文件、write、文件 fsync、rename、
  空间不足、目录 fsync、写头错误、录制期间同名文件冲突；冲突文件 KEEP 内容未改变。
- 真实 CedarC/G2D A/B/A：record1.jpg(640x480)、yln.jpg(2559x1403)、record1.jpg。
  A 两次 ARGB8888 输出哈希一致：456244cec3edd8d7；B 为 85e6fe612b3c7ad1。
  直通 DMABUF 和 VIDEO_SRC=copy 的结果也一致。
- 实际 UI 列表三轮循环加相机录制/停止/返回：成功预览 12 次 JPG，退出码 0，
  没有 VSYNC 超时、G2D 错误；按键录制 record1_005.avi 成功，64 帧，
  视频尾 3034ms、PCM 3080ms，差 46ms。
- 定时 5 秒录像 video_004.avi：132 帧，视频尾 4969ms、PCM 5005ms，差 36ms；
  全文件解封装读到 EOF，通过；回放 132 帧、丢帧 0、最大显示落后 36ms。
- PCM 暂停：533ms -> 533ms，恢复约 293ms，停止延迟 114ms，测试通过。
- H264 1280x720/AAC 44100Hz 双声道素材：1121 帧完整播放，丢帧 0，
  最大显示落后 39ms。双缓冲与真实边界等待增加部分呈现延迟，不伪报为免费优化。
- H265 Main/8-bit 640x480/AAC 48kHz：90 帧完整播放、丢帧 0、最大显示落后 25ms。
- AAC、S16_LE WAV 各 3 秒正常完成；MP3 暂停 506ms -> 506ms，
  恢复 293ms，停止延迟 116ms，通过。MP3 内嵌 PNG 封面不解码，会有探测警告。
- 59 秒新录像 video_005.avi：1702 帧，视频尾 58974ms、PCM 59006ms，差 32ms；
  采集样本累计漂移 256us，完整解封装读到 EOF；未替代多小时物理同步测试。
- 最终可执行文件已替换 /root/Project/T113Camera/t113_camera，chmod 755。
  正式入口启动/Ctrl+C 收尾退出码 0，板端测试工具及临时合成素材已移除。

原始日志在 output/review-logs，属于测试产物、不提交 Git。
复测脚本 tests/run_board_review.py 需要 Paramiko 和 T113_BOARD_PASSWORD，
会通过 event1 注入测试按键并产生新录像，不删除原有 USB 媒体。

## 验收边界

照片像素一致性与图层提交已验证，但没有物理拍摄屏幕，仍请用户检查实际颜色和切换观感。
音频时间戳异常路径包含单元测试，不等于已覆盖所有外部容器和所有时间戳缺陷。
真实拔盘/掉电/写满、多小时录制、FAT32 大文件分段、.part 恢复和统一状态机仍未完成。
独立写入线程隔离正常写盘抖动，不承诺介质或驱动长期阻塞时可以立即停止收尾。
容器首帧自动预览继续禁用；没有改 SDK 镜像、安装开机服务、提交或推送 Git。

## 本次产物

output/t113_camera 与板端 SHA-256 一致：

```text
449d277fcc2d4e50757e50ee63738383cc9e6c7a3a7fb59531007ef497a8c613
```

本轮产生的 video.avi、video_003.avi、video_004.avi、video_005.avi 和
record1_005.avi 保留供人工验收，原有素材未删除。
