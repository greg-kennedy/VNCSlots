#include "image.h"

#include <stdlib.h>
#include <stdio.h>
#include <string.h>
#include <errno.h>

struct image * make_image(unsigned short w, unsigned short h)
{
    struct image * img = malloc(sizeof(struct image));
    if (img == NULL) {
        fprintf(stderr, "ERROR: malloc(%zu) img in make_image(%d, %d): %d: %s\n", sizeof(struct image), w, h, errno, strerror(errno));
        return NULL;
    }

    img->width = w;
    img->height = h;

    size_t size = w * h;

    img->data = malloc(size);
    if (img->data == NULL) {
        fprintf(stderr, "ERROR: malloc(%zu) img->data in make_image(%d, %d): %d: %s\n", size, w, h, errno, strerror(errno));
        free(img);
        return NULL;
    }

    return img;
}

void free_image(struct image * img)
{
    free(img->data);
    free(img);
}

struct image * read_image(const char * filename)
{
    // READ IMAGES FROM DISK
    //  my stupid format is w / h and then a blast of uint8 bgr233 pixels
    unsigned char buffer[2];

    FILE * fp = fopen(filename, "rb");
    if (fp == NULL) {
        fprintf(stderr, "fopen(%s): %d: %s\n", filename, errno, strerror(errno));
        return NULL;
    }

    if (fread(buffer, 1, 2, fp) != 2) {
        fprintf(stderr, "fread(%s) width: %d: %s\n", filename, errno, strerror(errno));
        return NULL;
    }
    unsigned short width = (buffer[0] << 8) | buffer[1];
    if (fread(buffer, 1, 2, fp) != 2) {
        fprintf(stderr, "fread(%s) height: %d: %s\n", filename, errno, strerror(errno));
        return NULL;
    }
    unsigned short height = (buffer[0] << 8) | buffer[1];

    struct image * img = make_image(width, height);
    if (img == NULL) {
        fprintf(stderr, "Failed to allocate image reading %s\n", filename);
        return NULL;
    }

    if (fread(img->data, img->width, img->height, fp) != img->height) {
        fprintf(stderr, "fread(%s) data: %d: %s\n", filename, errno, strerror(errno));
        return NULL;
    }
    fclose(fp);

    return img;
}

// solid color fill of an image area
void fill(struct image * dst, unsigned short x, unsigned short y, unsigned short w, unsigned short h, unsigned char color)
{
    for (int dy = y; dy < y + h; dy ++)
        memset(&dst->data[dy * dst->width + x], color, w);
}

void blit_simple(const struct image * src, unsigned short src_x, unsigned short src_y,
                 struct image * dst, unsigned short dst_x, unsigned short dst_y,
                 unsigned short w, unsigned short h)
{
    int doff = dst_y * dst->width + dst_x;
    int soff = src_y * src->width + src_x;
    for (int y = 0; y < h; y ++) {
        memcpy(&dst->data[doff], &src->data[soff], w);
        doff += dst->width;
        soff += src->width;
    }
}

// as above but support for color tint and transparency
void blit_special(const struct image * src, unsigned short src_x, unsigned short src_y,
                  struct image * dst, unsigned short dst_x, unsigned short dst_y,
                  unsigned short w, unsigned short h, unsigned char transparency, unsigned char tint)
{
    int doff = dst_y * dst->width + dst_x;
    int soff = src_y * src->width + src_x;
    for (int y = 0; y < h; y ++) {
        for (int x = 0; x < w; x ++) {
            if ( ! (transparency && src->data[soff] == transparency)) dst->data[doff] = src->data[soff] | tint;
            doff ++;
            soff ++;
        }
        doff += (dst->width - w);
        soff += (src->width - w);
    }
}


// a blit that scales vertically and does transparency too
void blit_scaled(const struct image * src, unsigned short src_x, unsigned short src_y, unsigned short src_h,
                 struct image * dst, unsigned short dst_x, unsigned short dst_y, unsigned short dst_h,
                 unsigned short w, unsigned char transparency)
{
    const float row_skip = (float)src_h / dst_h;

    int doff = dst_y * dst->width + dst_x;
    for (int y = 0; y < dst_h; y ++) {
        int soff = ((int)(y * row_skip + .5) + src_y) * src->width + src_x;
        for (int x = 0; x < w; x ++) {
            if (src->data[soff] != transparency) dst->data[doff] = src->data[soff];
            doff ++;
            soff ++;
        }
        doff += (dst->width - w);
    }
}

