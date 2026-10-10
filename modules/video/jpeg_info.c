#include "jpeg_info.h"

int jpeg_next_frame(const unsigned char *data, size_t size, size_t *offset,
                    const unsigned char **frame, size_t *frame_size)
{
    size_t pos = *offset, start; /* 从上次结束位置继续扫描 MJPEG 中下一张完整 JPEG。 */
    int entropy = 0; /* 区分 JPEG 标记段与 SOS 后的熵编码数据。 */
    while (pos + 1 < size && !(data[pos] == 0xff && data[pos + 1] == 0xd8))
        pos++;
    if (pos + 1 >= size)
        return 0;
    start = pos; /* 找到 SOI 后记住当前帧起点。 */
    pos += 2; /* 跳过两个字节的 SOI 标记。 */
    while (pos < size) {
        unsigned int marker, length;
        if (entropy) { /* 压缩数据区只在 0xff 处检查转义与标记。 */
            while (pos < size && data[pos] != 0xff)
                pos++;
        }
        if (pos >= size || data[pos++] != 0xff)
            return 0;
        while (pos < size && data[pos] == 0xff) /* JPEG 允许重复 0xff 填充，跳过后再解释标记码。 */
            pos++;
        if (pos >= size)
            return 0;
        marker = data[pos++]; /* 取得标记码；前面的 0xff 已消耗。 */
        if (entropy && (marker == 0 || (marker >= 0xd0 && marker <= 0xd7))) /* 0xff00 是数据转义，RST0..7 是重启标记，均不是帧结束。 */
            continue;
        if (marker == 0xd9) { /* 只有当前 JPEG 的真实 EOI 才结束帧。 */
            *frame = data + start; /* 返回源数据中的借用指针，不分配 JPEG 副本。 */
            *frame_size = pos - start; /* 完整帧包含 SOI 到 EOI。 */
            *offset = pos; /* 下一次从当前 EOI 后继续扫描。 */
            return 1;
        }
        if (marker == 0x01)
            continue;
        if (marker == 0xd8 || marker == 0 || size - pos < 2)
            return 0;
        length = ((unsigned int)data[pos] << 8) | data[pos + 1]; /* JPEG 段长度为大端，并包含长度字段本身的两字节。 */
        if (length < 2 || length > size - pos) /* 验证段长度，防止截断文件触发越界读取。 */
            return 0;
        pos += length; /* Skip APP/EXIF segments including any embedded thumbnail EOI. */
        entropy = marker == 0xda; /* SOS 段之后进入压缩扫描数据区。 */
    }
    return 0;
}

int jpeg_dimensions(const unsigned char *data, size_t size,
                    unsigned int *width, unsigned int *height)
{
    size_t pos = 2; /* 尺寸解析从 SOI 后第一个标记开始。 */
    if (!data || !width || !height || size < 4 || data[0] != 0xff || data[1] != 0xd8)
        return -1;
    while (pos < size) {
        unsigned int marker, length;
        if (data[pos++] != 0xff)
            return -1;
        while (pos < size && data[pos] == 0xff) /* JPEG 允许重复 0xff 填充，跳过后再解释标记码。 */
            pos++;
        if (pos >= size)
            return -1;
        marker = data[pos++]; /* 取得标记码；前面的 0xff 已消耗。 */
        if (marker == 0xda || marker == 0xd9) /* 到扫描数据或结束仍无支持的 SOF，就无法取得尺寸。 */
            return -1;
        if (marker == 0x01 || (marker >= 0xd0 && marker <= 0xd7))
            continue;
        if (size - pos < 2)
            return -1;
        length = ((unsigned int)data[pos] << 8) | data[pos + 1]; /* JPEG 段长度为大端，并包含长度字段本身的两字节。 */
        if (length < 2 || length > size - pos) /* 验证段长度，防止截断文件触发越界读取。 */
            return -1;
        if (marker == 0xc0 || marker == 0xc1 || marker == 0xc2) { /* 解析 baseline、extended sequential 或 progressive 的 SOF 几何。 */
            if (length < 8 || data[pos + 2] != 8) /* 仅接受八位样本精度并检查固定字段长度。 */
                return -1;
            *height = ((unsigned int)data[pos + 3] << 8) | data[pos + 4]; /* SOF 先存图像高度，字段为大端。 */
            *width = ((unsigned int)data[pos + 5] << 8) | data[pos + 6]; /* 读取实际宽度，不能沿用上一张照片尺寸。 */
            return *width && *height && *width <= 4096 && *height <= 4096 ? 0 : -1; /* 限制几何上限；能读 SOF 不代表硬件必然支持此 JPEG 编码。 */
        }
        pos += length; /* 按段长度跳过 APP/EXIF，避免把嵌入缩略图的标记当主图。 */
    }
    return -1;
}
