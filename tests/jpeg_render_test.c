#include "jpeg_info.h"
#include "image_convert.h"
#include "g2d_display.h"
#include <stdio.h>
#include <stdlib.h>
#include <stdint.h>
#include <ion_mem_alloc.h>

/* Decode A/B/A with different dimensions and compare actual G2D output bytes. */
static int render(const char *path, uint64_t *hash)
{
    struct cedar_decoder decoder = {0};
    struct g2d_display g2d = {.fd = -1};
    struct yuv_image yuv = {0};
    FILE *file = fopen(path, "rb");
    unsigned char *data = NULL;
    unsigned int width, height;
    long size;
    int ret = -1;
    if (!file)
        return -1;
    if (fseek(file, 0, SEEK_END) || (size = ftell(file)) <= 0 || size > 16 * 1024 * 1024)
        goto out;
    rewind(file);
    data = malloc(size);
    if (!data || fread(data, 1, size, file) != (size_t)size ||
        jpeg_dimensions(data, size, &width, &height) < 0 ||
        g2d_display_open_sized(&g2d, width, height, 392, 232) < 0 ||
        image_convert_open(&decoder, width, height) < 0 ||
        image_convert_decode_mjpeg(&decoder, data, size, &yuv) < 0 ||
        g2d_display_stretch_yuv_to_dmabuf(&g2d, &yuv) < 0)
        goto out;
    *hash = 1469598103934665603ULL;
    for (size_t i = 0; i < g2d.dst_size; i++)
        *hash = (*hash ^ ((unsigned char *)g2d.dst_virt)[i]) * 1099511628211ULL;
    printf("render: %s input=%ux%u visible=%ux%u plane=%ux%u hash=%llx\n",
           path, width, height, yuv.width, yuv.height, yuv.stride, yuv.buffer_height,
           (unsigned long long)*hash);
    ret = 0;
out:
    image_convert_free_yuv(&yuv);
    image_convert_close(&decoder);
    g2d_display_close(&g2d);
    free(data);
    fclose(file);
    return ret;
}
int main(int argc, char **argv)
{
    uint64_t first, other, repeated;
    if (argc != 3 || render(argv[1], &first) || render(argv[2], &other) ||
        render(argv[1], &repeated) || first != repeated || first == other)
        return 1;
    puts("JPEG A/B/A pixels: PASS");
    return 0;
}
