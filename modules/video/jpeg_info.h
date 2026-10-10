#ifndef JPEG_INFO_H
#define JPEG_INFO_H
#include <stddef.h>
/* Validate marker lengths before trusting dimensions from an external JPEG. */
int jpeg_dimensions(const unsigned char *data, size_t size,
                    unsigned int *width, unsigned int *height);
int jpeg_next_frame(const unsigned char *data, size_t size, size_t *offset,
                    const unsigned char **frame, size_t *frame_size);
#endif
