#ifndef IMAGE_H_
#define IMAGE_H_

// some functions for working with Images

struct image {
    unsigned short width;
    unsigned short height;
    unsigned char * data;
};

struct image * make_image(unsigned short w, unsigned short h);
void free_image(struct image * img);

struct image * read_image(const char * filename);

void fill(struct image * dst, unsigned short x, unsigned short y, unsigned short w, unsigned short h, unsigned char color);

void blit_simple(const struct image * src, unsigned short src_x, unsigned short src_y,
                 struct image * dst, unsigned short dst_x, unsigned short dst_y,
                 unsigned short w, unsigned short h);

// as above but support for color tint and transparency
void blit_special(const struct image * src, unsigned short src_x, unsigned short src_y,
                  struct image * dst, unsigned short dst_x, unsigned short dst_y,
                  unsigned short w, unsigned short h, unsigned char transparency, unsigned char tint);

// a blit that scales vertically and does transparency too
void blit_scaled(const struct image * src, unsigned short src_x, unsigned short src_y, unsigned short src_h,
                 struct image * dst, unsigned short dst_x, unsigned short dst_y, unsigned short dst_h,
                 unsigned short w, unsigned char transparency);

#endif
