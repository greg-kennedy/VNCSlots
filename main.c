/*
** VNCSlots - a slot machine played over RFB protocol
*/

#include "image.h"

#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <unistd.h>
#include <sys/types.h>
#include <sys/socket.h>
#include <netinet/in.h>
#include <arpa/inet.h>
#include <netdb.h>
#include <sys/time.h>

#define PORT "5900"   // port we're listening on

#define INTERVAL (1000000 / 25)

// /////////////////////////////////
// types
enum fruit {
    cherry,
    orange,
    plum,
    bell,
    bar
};

enum encoding {
    Raw = 0,
    CopyRect = 1,
    RRE = 2,
    HexTile = 4,
    TRLE = 8,
    ZRLE = 16,
    Cursor = 32
};

// GLOBALS
// gamestate - what the slot machine is currently doing
enum {
    waiting,
    coin,
    handle_down,
    handle_up,
    spin,
    payout
} gamestate;

// economy
int plays;
int profit;
// which of the 20 stops is each reel on
unsigned char reel_stop[3];

// GRAPHICS -
static struct image * framebuffer;
static uint16_t * palette[3];
//  actual Y position on the reel
short reel_position[3];
// temporary usage for screen drawing
short coin_y;
short handle_y;
short reel_left[3];
short payout_left;

// a slightly cooked pixel format, where the _max is converted to a _div
struct pixel_format {
    uint8_t bpp;
//    uint8_t depth;
    uint8_t big_endian_flag;
    uint8_t true_color_flag;
    uint16_t red_div;
    uint16_t green_div;
    uint16_t blue_div;
    uint8_t red_shift;
    uint8_t green_shift;
    uint8_t blue_shift;
};

// LINKED LISTS for network sockets
struct listener {
    int fd;
    struct listener * next;
};

struct client {
    int fd;
    struct client * next;

    enum {
        none = 0,
        handshake_protocolversion,
        handshake_security,
        handshake_securityresult,

        init_client,

        client_message,

        client_message_setpixelformat,
        client_message_setencodings_0,
        client_message_setencodings_n,
        client_message_framebufferupdaterequest,
        client_message_keyevent,
        client_message_pointerevent,
        client_message_clientcuttext_0,
        client_message_clientcuttext_n
    } state;

    // data read from the TCP socket
    unsigned int read, needed, extra;
    unsigned char buffer[20];
    // statistics
    unsigned int bytes_sent;

    // client state
    struct pixel_format format;

    // bit field of encodings supported
    uint8_t encodings;

    // key-down status
    uint8_t key_down;
    // mouse-down hotspots
    uint8_t mouse_down;

    // ready for update?
    uint8_t ready;

    // some indicators of Last Time Things Happened, which tells us when they need a Rectangle update
    unsigned char sent_cursor;
    unsigned char sent_palette;
    short coin_y;
    short handle_y;
    short reel_position[3];
    int plays, profit;
};

// /////////////////////////////////
// Helper functions

static unsigned char * encode_pixel(unsigned char * p, const struct pixel_format * f, const uint8_t color)
{
    uint32_t pixel = ((palette[0][color] / f->red_div) << f->red_shift) |
                     ((palette[1][color] / f->green_div) << f->green_shift) |
                     ((palette[2][color] / f->blue_div) << f->blue_shift);

    if (f->bpp == 8) {
        *p = (pixel & 0xFF);
        p ++;
    } else if (f->bpp == 16) {
        if (f->big_endian_flag) {
            *p = (pixel & 0xFF00) >> 8;
            p ++;
            *p = (pixel & 0xFF);
            p ++;
        } else {
            *p = (pixel & 0xFF);
            p ++;
            *p = (pixel & 0xFF00) >> 8;
            p ++;
        }
    } else {
        if (f->big_endian_flag) {
            *p = (pixel & 0xFF000000) >> 24;
            p ++;
            *p = (pixel & 0xFF0000) >> 16;
            p ++;
            *p = (pixel & 0xFF00) >> 8;
            p ++;
            *p = (pixel & 0xFF);
            p ++;
        } else {
            *p = (pixel & 0xFF);
            p ++;
            *p = (pixel & 0xFF00) >> 8;
            p ++;
            *p = (pixel & 0xFF0000) >> 16;
            p ++;
            *p = (pixel & 0xFF000000) >> 24;
            p ++;
        }
    }
    return p;
}

static unsigned char * encode_hextile(unsigned char * p, struct pixel_format * f, uint16_t x, uint16_t y, uint16_t w, uint16_t h)
{
    // some enums
    enum {
        H_None = 0,
        H_Raw = 1,
        H_BGSpec = 2,
        H_FGSpec = 4,
        H_AnySub = 8,
        H_SubColor = 16
    };

    short background = -1;
    short foreground = -1;

    // we break the area into 16x16 tiles and analyze them
    while (h > 0) {
        int th = h > 16 ? 16 : h;

        int dx = x;
        int w_left = w;
        while (w_left > 0) {
            int tw = w_left > 16 ? 16 : w_left;

            // histogram of the colors
            // for 8bpp we can use pigeonhole
            unsigned short colors[256] = { 0 };
            for (int j = 0; j < th; j ++)
                for (int i = 0; i < tw; i ++)
                    colors[framebuffer->data[(y + j) * 512 + dx + i]] ++;

            short newbg = -1;
            short newfg = -1;
            unsigned short color_count = 0;
            for (int i = 0; i < 256; i ++) {
                if (colors[i] > 0) {
                    color_count ++;
                    if (newbg < 0 || colors[i] > colors[newbg]) {
                        newfg = newbg;
                        newbg = i;
                    } else if (newfg < 0 || colors[i] > colors[newfg]) {
                        newfg = i;
                    }
                }
            }


            // log this.  if we do the work and find this is more expensive than raw encoding the whole tile,
            //  backtrack to this point and do raw.
            unsigned char * tile_start = p;

            // ok!  we have determined a count of how many colors are in the tile,
            // the most common (newbg) and the second-most-common (newfg)

            if (color_count == 1) {
                // solid colored tile
                if (newbg == background) {
                    // can carry over color from before
                    *p = H_None;
                    p ++;
                } else {
                    *p = H_BGSpec;
                    p ++;
                    p = encode_pixel(p, f, newbg);
                    background = newbg;
                }
            } else {
                if (color_count == 2) {
                    // two-tone tile
                    if (newbg == background && newfg == foreground) {
                        *p = H_AnySub;
                        p ++;
                    } else if (newbg != background && newfg == foreground) {
                        *p = H_AnySub | H_BGSpec;
                        p ++;
                        p = encode_pixel(p, f, newbg);
                        background = newbg;
                    } else if (newbg == background && newfg != foreground) {
                        *p = H_AnySub | H_FGSpec;
                        p ++;
                        p = encode_pixel(p, f, newfg);
                        foreground = newfg;
                    } else {
                        *p = H_AnySub | H_FGSpec | H_BGSpec;
                        p ++;
                        p = encode_pixel(p, f, newbg);
                        background = newbg;
                        p = encode_pixel(p, f, newfg);
                        foreground = newfg;
                    }
                }  else {
                    if (newbg == background) {
                        // can carry over color from before
                        *p = H_AnySub | H_SubColor;
                        p ++;
                    } else {
                        *p = H_AnySub | H_SubColor | H_BGSpec;
                        p ++;
                        p = encode_pixel(p, f, newbg);
                        background = newbg;
                    }
                    foreground = -1;
                }

                unsigned char * rect_count = p;
                p ++;
                *rect_count = 0;

                // at last do RRE on the tile
                unsigned char coverage[16][16] = { 0 };
                for (int j = 0; j < th; j ++) {
                    for (int i = 0; i < tw; i ++) {
                        // square already "covered", skip
                        if (coverage[j][i]) continue;

                        // mark it now
                        coverage[j][i] = 1;

                        // check for background-color
                        unsigned char color = framebuffer->data[(y + j) * 512 + dx + i];

                        if (color == background) continue;

                        // an uncovered, new color.
                        *rect_count += 1;

                        //  try to expand our ending box as far right as we can
                        int i2 = i + 1;
                        while (i2 < tw && framebuffer->data[(y + j) * 512 + dx + i2] == color)
                        {
                            coverage[j][i2] = 1;
                            i2 ++;
                        }

                        // and now a check to see how tall we can make the box
                        int j2 = j + 1;
                        while (j2 < th) {
                            unsigned char full_row = 1;

                            // check the row first
                            for (int q = i; q < i2; q ++) {
                                if (color != framebuffer->data[(y + j2) * 512 + dx + q]) {
                                    full_row = 0;
                                    break;
                                }
                            }
                            if (! full_row) break;

                            // mark the row now
                            for (int q = i; q < i2; q ++)
                                coverage[j2][q] = 1;
                            j2 ++;
                        }

                        // ok we have everything we need!  send the rectangle
                        if (color_count > 2) p = encode_pixel(p, f, color);
                        *p = ((i & 0xF) << 4) | (j & 0xF);
                        p ++;
                        *p = (((i2 - i - 1) & 0xF) << 4) | ((j2 - j - 1) & 0xF);
                        p ++;
                    }
                }
            }

            // RAW ENCODE THE TILE
            if (p - tile_start > tw * th * f->bpp / 8) {
                p = tile_start;
                *p = 1;
                p ++;
                for (int j = 0; j < th; j ++)
                    for (int i = 0; i < tw; i ++)
                        p = encode_pixel(p, f, framebuffer->data[(y + j) * 512 + dx + i]);
                background = foreground = -1;
            }

            //printf("%2x ", *tile_start);
            dx += tw;
            w_left -= tw;
        }
        //printf("\n");
        y += th;
        h -= th;
    }

    return p;
}
static unsigned char * encode_rre(unsigned char * p, struct pixel_format * f, uint16_t x, uint16_t y, uint16_t w, uint16_t h)
{
    // we need this to go update the subrectangle count later
    unsigned char * start = p;
    p += 4;

    unsigned int subrects = 0;

    // find background color
    //  this is a histogram across the region, tracking each pixel and its frequency
    // for 8bpp we can use pigeonhole

    unsigned char max_color = 0;
    unsigned int colors[256] = { 0 };
    for (int src_y = y; src_y < y + h; src_y ++) {
        int i = src_y * 512;
        for (int src_x = x; src_x < x + w; src_x ++) {

            unsigned char color = framebuffer->data[i + src_x];
            colors[color] ++;
            if (colors[color] > colors[max_color])
                max_color = color;
        }
    }

    p = encode_pixel(p, f, max_color);

    // now we calloc a region and then walk it trying to build RRE blocks and send them
    unsigned char * coverage = calloc(h, w);
    if (coverage == NULL) {
        perror("calloc coverage");
        exit(EXIT_FAILURE);
    }

    for (int src_y = 0; src_y < h; src_y ++) {
        int i = (y + src_y) * 512;
        int j = src_y * w;
        for (int src_x = 0; src_x < w; src_x ++) {
            // square already "covered", skip
            if (coverage[j + src_x]) continue;

            // mark it now
            coverage[j + src_x] = 1;

            // check for background-color
            unsigned char color = framebuffer->data[i + x + src_x];
            if (color == max_color) continue;

            // an uncovered, new color.
            subrects ++;

            //  try to expand our ending box as far right as we can
            int src_x2 = src_x + 1;
            while (src_x2 < w && framebuffer->data[i + x + src_x2] == color)
            {
                coverage[j + src_x2] = 1;
                src_x2 ++;
            }

            // and now a check to see how tall we can make the box
            int src_y2 = src_y + 1;
            while (src_y2 < h) {
                int k = (y + src_y2) * 512;
                unsigned char full_row = 1;
                // check the row first
                for (int l = src_x; l < src_x2; l ++) {
                    if (color != framebuffer->data[l + x + k]) {
                        full_row = 0;
                        break;
                    }
                }
                if (! full_row) break;
                // mark the row now
                k = src_y2 * w;
                for (int l = src_x; l < src_x2; l ++)
                    coverage[k + l] = 1;
                src_y2 ++;
            }

            // ok we have everything we need!  send the rectangle
            p = encode_pixel(p, f, color);
            *p = src_x / 256;
            p ++;
            *p = src_x % 256;
            p ++;
            *p = src_y / 256;
            p ++;
            *p = src_y % 256;
            p ++;
            *p = (src_x2 - src_x) / 256;
            p ++;
            *p = (src_x2 - src_x) % 256;
            p ++;
            *p = (src_y2 - src_y) / 256;
            p ++;
            *p = (src_y2 - src_y) % 256;
            p ++;
        }
    }
    *start = (subrects & 0xFF000000) >> 24;
    start ++;
    *start = (subrects & 0xFF0000) >> 16;
    start ++;
    *start = (subrects & 0xFF00) >> 8;
    start ++;
    *start = subrects & 0xFF;

    free(coverage);

    return p;

}

static unsigned char * encode_raw(unsigned char * p, const struct pixel_format * f, uint16_t x, uint16_t y, uint16_t w, uint16_t h)
{
    // if their format exactly matches ours, we can just send the pixels directly
    if (f->bpp == 8 && (f->true_color_flag == 0 || (f->red_div == (65536 / 8) && f->red_shift == 0 && f->green_div == (65536 / 8) && f->green_shift == 3 && f->blue_div == (65536 / 4) && f->blue_shift == 6))) {

        for (int row = y; row < y + h; row ++) {
            memcpy(p, & framebuffer->data[512 * row + x], w);
            p += w;
        }
    } else {
        // hmm ok A Conversion Is Needed

        for (int src_y = y; src_y < y + h; src_y ++) {
            int i = src_y * 512;
            for (int src_x = x; src_x < x + w; src_x ++)
                p = encode_pixel(p, f, framebuffer->data[i + src_x]);
        }
    }
    return p;
}

static unsigned char * encode_cursor(unsigned char * p, const struct pixel_format * f)
{
    // the cursor shape - Windows "hand"
    static const unsigned char cursor_colormap[47] = { 0x00, 0x00, 0x03, 0x00, 0x01, 0x80, 0x00, 0xc0, 0x00, 0x60, 0x00, 0x30, 0x00, 0x1b, 0x00, 0x0d, 0xb0, 0x06, 0xda, 0x03, 0x6d, 0x99, 0xfe, 0xce, 0xff, 0xe3, 0x7f, 0xf0, 0xbf, 0xf8, 0x7f, 0xfc, 0x1f, 0xfe, 0x0f, 0xfe, 0x03, 0xff, 0x01, 0xff, 0x80, 0x7f, 0x80, 0x3f, 0xc0, 0x00, 0x00 };
    static const unsigned char cursor_tmap[66] = { 0x06, 0x00, 0x00, 0x0f, 0x00, 0x00, 0x0f, 0x00, 0x00, 0x0f, 0x00, 0x00, 0x0f, 0x00, 0x00, 0x0f, 0xc0, 0x00, 0x0f, 0xf8, 0x00, 0x0f, 0xfe, 0x00, 0x0f, 0xff, 0x00, 0xef, 0xff, 0x80, 0xff, 0xff, 0x80, 0xff, 0xff, 0x80, 0x7f, 0xff, 0x80, 0x3f, 0xff, 0x80, 0x3f, 0xff, 0x80, 0x1f, 0xff, 0x80, 0x1f, 0xff, 0x00, 0x0f, 0xff, 0x00, 0x0f, 0xff, 0x00, 0x07, 0xfe, 0x00, 0x07, 0xfe, 0x00, 0x07, 0xfe, 0x00 };

    // rectangle header
    p[0] = p[2] = p[4] = p[6] = 0;
    // hotspot
    p[1] = 5;
    p[3] = 1;
    // cursor size
    p[5] = 17;
    p[7] = 22;
    // encoding
    p[8] = p[9] = p[10] = 0xFF;
    p[11] = 0x11;

    p += 12;

    // decode the cursor colormap
    char bit = 7;
    int byte = 0;
    for (int i = 0; i < 17 * 22; i ++) {
        p = encode_pixel(p, f, cursor_colormap[byte] & (1 << bit) ? 0xFF : 0);
        bit --;
        if (bit < 0) {
            byte ++;
            bit = 7;
        }
    }

    // dump the transparency map
    memcpy(p, cursor_tmap, 66);
    p += 66;

    return p;
}

// Sends a consolidated Update packet to the client.
static unsigned char * encode(unsigned char * p, struct client * c, uint16_t x, uint16_t y, uint16_t w, uint16_t h)
{
    // pack an update
    // x
    p[0] = (x / 256);
    p[1] = (x % 256);
    p[2] = (y / 256);
    p[3] = (y % 256);
    p[4] = (w / 256);
    p[5] = (w % 256);
    p[6] = (h / 256);
    p[7] = (h % 256);
    // encoding
    p[8] = p[9] = p[10] = 0;
    p += 11;

    if (c->encodings & HexTile) {
        *p = 5;
        unsigned char * hextile = encode_hextile(p + 1, & c->format, x, y, w, h);
        if (hextile - p <= w * h * (c->format.bpp / 8)) return hextile;

        // it seems HexTile made it worse than Raw so toss that encoding attempt
    }

    if (c->encodings & RRE) {
        *p = 2;
        unsigned char * rre = encode_rre(p + 1, & c->format, x, y, w, h);
        if (rre - p <= w * h * (c->format.bpp / 8)) return rre;

        // it seems RRE made it worse than Raw so toss that encoding attempt
    }

    // RAW encoding
    *p = 0;
    return encode_raw(p + 1, & c->format, x, y, w, h);
}

// Sends a consolidated Update packet to the client.
static int update(struct client * c, uint16_t x, uint16_t y, uint16_t w, uint16_t h, unsigned char incremental)
{
    // cap the region to just our screen limits
    if (x > 511) x = 511;
    if (y > 383) y = 383;
    if (x + w > 512) w = 512 - x;
    if (y + h > 384) h = 384 - y;

    // this is a staging area for building the complete packet we'll send
    //  the largest possible packet is a full screen 32-bit refresh, plus maybe a cursor
    static unsigned char * packet = NULL, *p;
    if (packet == NULL) {
        packet = malloc(4 +
                        12 + 512 * 384 * 4 +
                        12 + 17 * 22 * 4 + 3 * 22);
        if (packet == NULL) {
            perror("packet malloc");
            exit(EXIT_FAILURE);
        }

    }

    // paletted modes should get a copy of the palette on first update
    if (! c->format.true_color_flag && ! c->sent_palette) {
        // type + padding
        packet[1] = packet[2] = packet[3] = packet[5] = 0;
        packet[0] = packet[4] = 1;

        p = &packet[6];
        for (int i = 0; i < 256; i ++) {
            for (int c = 0; c < 3; c ++) {
                *p = (palette[c][i] >> 8) & 0xFF;
                p ++;
                *p = palette[c][i] & 0xFF;
                p ++;
            }
        }

        c->bytes_sent += (p - packet);
        //printf("Sending %d bytes\n", p - packet);
        if ( send(c->fd, packet, p - packet, 0) == -1) {
            perror("send");
            return 0;
        }

        c->sent_palette = 1;
    }

    // framebuffer update
    // type + padding
    packet[0] = packet[1] = 0;
    p = &packet[4];

    unsigned short rectangle_count = 0;

    // Incremental update can take just the changes in the area
    if (incremental)
    {
        // coin drop
        if (c->coin_y != coin_y) {
            rectangle_count ++;
            p = encode(p, c, 388, 185, 29, 37);
        }

        // handle
        if (c->handle_y != handle_y) {
            rectangle_count ++;
            int skip = (c->handle_y < handle_y ? c->handle_y : handle_y);
            p = encode(p, c, 447, 73 + skip, 40, 248 - skip);

        }

        // reels
        for (int i = 0; i < 3; i ++) {
            if (c->reel_position[i] != reel_position[i]) {
                rectangle_count ++;
                p = encode(p, c, 222 + 50 * i, 67, 32, 114);
            }
        }

        // scoreboard
        if (c->profit - c->plays != profit - plays) {
            rectangle_count ++;
            p = encode(p, c, 19, 353, 63, 11);
        }

        if (c->plays != plays) {
            rectangle_count ++;
            p = encode(p, c, 19, 293, 63, 11);
        }

        if (c->profit != profit) {
            rectangle_count ++;
            p = encode(p, c, 19, 323, 63, 11);

// ding!
            *p = 0x02;
            p ++;
        }

// nothing to do!  don't send anything.
        if (rectangle_count == 0) return 1;
    } else {
// encode the entire region
        rectangle_count ++;
        p = encode(p, c, x, y, w, h);
    }

    if ((c->encodings & Cursor) && ! c->sent_cursor)
    {
        rectangle_count ++;
        p = encode_cursor(p, &c->format);
    }

    packet[2] = rectangle_count / 256;
    packet[3] = rectangle_count % 256;

// All done!  Send the packet.

    c->bytes_sent += (p - packet);
    //printf("Sending %d bytes\n", p - packet);
    if ( send(c->fd, packet, p - packet, 0) == -1) {
        perror("send");
        return 0;
    }

    c->sent_cursor = 1;
    c->coin_y = coin_y;
    c->handle_y = handle_y;
    for (int i = 0; i < 3; i ++)
        c->reel_position[i] = reel_position[i];
    c->plays = plays;
    c->profit = profit;
    c->ready = 0;

    return 1;
}


// special draw functions
static void draw_number(struct image * dst, const struct image * src, int number, unsigned short dst_x, unsigned short dst_y)
{
    static char num[12] = "";
    sprintf(num, "%8d", number);
    for (int i = 0; i < 8; i ++) {
        if (num[i] >= '0' && num[i] <= '9') {
            blit_special(src, 0, 11 * (num[i] - '0'), dst, dst_x, dst_y, src->width, 11, 0, (number < 0 ? 7 : 0));
        } else if (num[i] == '-') {
            blit_special(src, 0, 110, dst, dst_x, dst_y, src->width, 11, 0, (number < 0 ? 7 : 0));
        } else {
            // white square
            fill(dst, dst_x, dst_y, 6, 11, 0xFF);
        }
        dst_x += 8;
    }
}

static void darken_row(struct image * dst, unsigned short x, unsigned short y, unsigned short w, unsigned char amount)
{
    unsigned char *p = &dst->data[y * dst->width + x];
    while (w > 0) {
        short b = ((*p & 0xC0) >> 6) - (amount >> 1);
        if (b < 0) b = 0;
        short g = ((*p & 0x38) >> 3) - amount;
        if (g < 0) g = 0;
        short r = (*p & 0x07) - amount;
        if (r < 0) r = 0;
        *p = (b << 6) | (g << 3) | r;
        p ++;
        w --;
    }
}

static void draw_reel(struct image * dst, const struct image * src, short reel_position, unsigned short dst_x, unsigned short dst_y)
{
    // reel is 114 pixels high
    short dst_h = 114;
    if (reel_position + dst_h > src->height) {
        // reel won't fit
        int h = src->height - reel_position;
        blit_simple(src, 0, reel_position, dst, dst_x, dst_y, src->width, h);
        blit_simple(src, 0, 0, dst, dst_x, dst_y + h, src->width, dst_h - h);
    } else {
        blit_simple(src, 0, reel_position, dst, dst_x, dst_y, src->width, dst_h);
    }

    // darken top and bottom
    for (int y = 0; y < 14; y ++) {
        darken_row(dst, dst_x, dst_y + y, 32, (14 - y) >> 1);
        darken_row(dst, dst_x, dst_y + dst_h - y - 1, 32, (14 - y) >> 1);
    }
}

static void draw_handle(struct image * dst, const struct image * img_background, const struct image * img_handle, const struct image * img_ball, int scale)
{
    blit_simple(img_background, 447, 73, dst, 447, 73, 40, img_ball->height + img_handle->height);
    blit_special(img_ball, 0, 0, dst, 451, 73 + scale, img_ball->height, img_ball->width, 0xFF, 0);
    blit_scaled(img_handle, 0, 0, img_handle->height, dst, 447, img_ball->height + 73 + scale, img_handle->height - scale, img_handle->width, 0xFF);
}

// get sockaddr, IPv4 or IPv6:
static const void *get_in_addr(const struct sockaddr *sa)
{
    if (sa->sa_family == AF_INET) {
        return &(((struct sockaddr_in *)sa)->sin_addr);
    }
    return &(((struct sockaddr_in6 *)sa)->sin6_addr);
}

/*
static const char * en2s(int fruit)
{
    if (fruit == bar) return "BAR";
    if (fruit == bell) return "BELL";
    if (fruit == plum) return "PLUM";
    if (fruit == orange) return "ORANGE";
    if (fruit == cherry) return "CHERRY";
    return "UNKNOWN";
}
*/

// /////////////////////////////////
int main(void)
{
    puts("VNCSlots - starting up!");

    // local constants
    static const uint8_t reels[3][20] = {
        { orange, bar, plum, cherry, plum, orange, bell, plum, orange, cherry, orange, bar, orange, plum, orange, plum, cherry, bar, orange, plum },
        { bell, cherry, bell, cherry, bell, cherry, bell, orange, bell, cherry, bell, cherry, bell, bar, bell, cherry, bell, cherry, bell, plum },
        { orange, cherry, orange, plum, orange, bar, orange, plum, orange, bell, orange, cherry, orange, plum, orange, plum, orange, cherry, orange, plum }
    };

    // important variables
    plays = 0;
    profit = 0;
    FILE * stats = fopen("stats.ini", "r");
    if (stats != NULL) {
        fscanf(stats, "%d %d\n", &plays, &profit);
        fclose(stats);
    }

    // reel positions
    reel_stop[0] = reel_stop[1] = reel_stop[2] = 0;
    reel_position[0] = reel_position[1] = reel_position[2] = 960 - 57 + 16;

    //  linked lists of listeners and clients
    struct listener * listeners = NULL;
    struct client * clients = NULL;
    // nEtwork socketstuff
    int listener_fd_max = 0;    // maximum file descriptor number
    fd_set master;    // master file descriptor list
    FD_ZERO(&master);    // clear the master set

    // images
    puts("Loading images...");
    const struct image * img_background = read_image("background.bin");
    const struct image * img_digits = read_image("digits.bin");
    const struct image * img_ball = read_image("ball.bin");
    const struct image * img_handle = read_image("handle.bin");
    const struct image * img_coin = read_image("coin.bin");
    const struct image * img_coinslot = read_image("coinslot.bin");
    struct image * img_fruit = read_image("fruit.bin");

    // build three large reel images
    struct image * img_reels[3];
    for (int i = 0; i < 3; i ++) {
        img_reels[i] = make_image(32, 48 * 20);
        for (int k = 0; k < 20; k ++) {
            blit_simple(img_fruit, 0, 32 * reels[i][k], img_reels[i], 0, 48 * k, 32, 32);
            fill(img_reels[i], 0, 48 * k + 32, 32, 16, 0xFF);
        }
    }
    free_image(img_fruit);

    // build BGR233 palette
    //  the format of a RFB palette is uint16
    for (int i = 0; i < 3; i ++)
    {
        palette[i] = malloc(256 * sizeof(uint16_t));
        if (palette[i] == NULL) {
            perror("malloc palette");
            return EXIT_FAILURE;
        }
    }

    int i = 0;
    for (int b = 0; b < 4; b ++)
    {
        for (int g = 0; g < 8; g ++)
        {
            for (int r = 0; r < 8; r ++)
            {
                palette[0][i] = (r << 13) | (r << 10) | (r << 7) | (r << 4) | (r << 1) | (r >> 2);
                palette[1][i] = (g << 13) | (g << 10) | (g << 7) | (g << 4) | (g << 1) | (g >> 2);
                palette[2][i] = (b << 14) | (b << 12) | (b << 10) | (b << 8) | (b << 6) | (b << 4) | (b << 2) | b;
                i ++;
            }
        }
    }

    // BUILD FRAMEBUFFER
    framebuffer = make_image(512, 384);

    // blit
    blit_simple(img_background, 0, 0, framebuffer, 0, 0, img_background->width, img_background->height);
    draw_handle(framebuffer, img_background, img_handle, img_ball, 0);
    draw_number(framebuffer, img_digits, plays, 19, 293);
    draw_number(framebuffer, img_digits, profit, 19, 323);
    draw_number(framebuffer, img_digits, profit - plays, 19, 353);

    // visuals - the center position is 57 pixels down but then we also have to remove 16px for the top half of the fruit

    draw_reel(framebuffer, img_reels[0], reel_position[0], 222, 67);
    draw_reel(framebuffer, img_reels[1], reel_position[1], 272, 67);
    draw_reel(framebuffer, img_reels[2], reel_position[2], 322, 67);

    // BIND LISTENERS
    puts("Binding listen sockets (port " PORT ")...");
    {
        // get us a socket and bind it - any family, TCP / stream
        static const struct addrinfo hints = {
            .ai_family = AF_UNSPEC,
            .ai_socktype = SOCK_STREAM,
            .ai_protocol = IPPROTO_TCP,
            .ai_flags = AI_ADDRCONFIG | AI_PASSIVE
        };

        struct addrinfo *ai;
        int rv = getaddrinfo(NULL, PORT, &hints, &ai);
        if (rv != 0) {
            fputs("getaddrinfo: ", stderr);
            fputs(gai_strerror(rv), stderr);
            return EXIT_FAILURE;
        }

        for(struct addrinfo * p = ai; p != NULL; p = p->ai_next) {
            int fd = socket(p->ai_family, p->ai_socktype, p->ai_protocol);
            if (fd < 0) {
                perror("socket");
                continue;
            }

            // lose the pesky "address already in use" error message
            static const int yes=1;
            setsockopt(fd, SOL_SOCKET, SO_REUSEADDR, &yes, sizeof(int));

            if (bind(fd, p->ai_addr, p->ai_addrlen)) {
                perror("bind");
                close(fd);
                continue;
            }

            if (listen(fd, 1)) {
                perror("listen");
                close(fd);
                continue;
            }

            // looks good!  print some info and put this into the master set
            char ip[INET6_ADDRSTRLEN];
            inet_ntop(p->ai_addr->sa_family,
                      get_in_addr(p->ai_addr),
                      ip, INET6_ADDRSTRLEN);
            printf(" . Bound to %s on socket %d\n", ip, fd);

            // add the listener to the master set
            FD_SET(fd, &master);

            // keep track of the biggest file descriptor
            if (fd > listener_fd_max) listener_fd_max = fd;

            struct listener * l = malloc(sizeof(struct listener));
            l->fd = fd;
            l->next = listeners;
            listeners = l;

        }

        freeaddrinfo(ai); // all done with this
    }

    // if we got here, it means we didn't get bound
    if (listeners == NULL) {
        fputs("selectserver: failed to bind to any sockets\n", stderr);
        return EXIT_FAILURE;
    }

    puts("Ready to accept new connections!");

    // state info about the game display
    gamestate = waiting;

    struct timeval tv_now, tv_next;

    /*
    	gettimeofday(&tv_now, NULL);
    	//printf("The time is %d.%06d sec\n", tv_now.tv_sec, tv_now.tv_usec);
    	tv_next.tv_sec = tv_now.tv_sec;
    	tv_next.tv_usec = tv_now.tv_usec + INTERVAL;

    	if (tv_next.tv_usec > 999999) {
    		tv_next.tv_sec ++;
    		tv_next.tv_usec -= 1000000;
    	}
    */

    // main loop
    int fd_max = listener_fd_max;
    for(;;) {
        // temp file descriptor list for select()
        fd_set read_fds = master;

        int ready_fds;
        if (gamestate != waiting) {
            // set timer for remaining duration between now and next tick
            struct timeval tv;

            if (tv_now.tv_usec > tv_next.tv_usec) {
                tv.tv_sec = tv_next.tv_sec - tv_now.tv_sec - 1;
                tv.tv_usec = tv_next.tv_usec + 1000000 - tv_now.tv_usec;
            } else {
                tv.tv_sec = tv_next.tv_sec - tv_now.tv_sec;
                tv.tv_usec = tv_next.tv_usec - tv_now.tv_usec;
            }
            ready_fds = select(fd_max+1, &read_fds, NULL, NULL, &tv);
        } else {
            ready_fds = select(fd_max+1, &read_fds, NULL, NULL, NULL);
        }

        if (ready_fds < 0) {
            perror("select");
            return EXIT_FAILURE;
        } else if (ready_fds > 0) {
            // run through the existing connections looking for data to read
            fd_max = listener_fd_max;

            //  check the accept sockets first
            for (struct listener * l = listeners; l != NULL; l = l->next) {
                // keep the max fd updated
                if (FD_ISSET(l->fd, &read_fds)) {
                    // handle new connections
                    struct sockaddr_storage remoteaddr; // client address
                    socklen_t addrlen = sizeof remoteaddr;
                    int fd = accept(l->fd, (struct sockaddr *)&remoteaddr, &addrlen);
                    if (fd == -1) {
                        // an error occurred trying to accept the new connection - maybe they disconnected in the meantime or something
                        perror("accept");
                        continue;
                    }
                    // Connection success!
                    char ip[INET6_ADDRSTRLEN];
                    inet_ntop(remoteaddr.ss_family, get_in_addr((struct sockaddr*)&remoteaddr), ip, INET6_ADDRSTRLEN);
                    printf("+ Received new connection from %s on socket %d\n", ip, fd);

                    // send protocol-version message before anything else - if this fails, we can skip allocation etc
                    // "RFB 003.008\n"
                    static const unsigned char protocol_version[] = { 0x52, 0x46, 0x42, 0x20, 0x30, 0x30, 0x33, 0x2e, 0x30, 0x30, 0x38, 0x0a };
                    if (send(fd, protocol_version, 12, 0) == -1) {
                        perror("send");
                        continue;
                    }
                    FD_SET(fd, &master); // add to master set
                    if (fd > fd_max) fd_max = fd;

                    struct client * c = malloc(sizeof(struct client));

                    // initialize all client state
                    c->fd = fd;

                    c->state = handshake_protocolversion;
                    static const struct pixel_format format = { 8, 1, 1, 65536 / 8, 65536 / 8, 65536 / 4, 5, 2, 0 };
                    c->format = format;
                    c->bytes_sent = 0;
                    c->read = 0;
                    c->needed = 12;
                    c->extra = 0;
                    c->encodings = 0;
                    c->key_down = 0;
                    c->mouse_down = 0;
                    c->ready = 0;
                    c->sent_cursor = 0;
                    c->sent_palette = 0;

                    // insert the client into the list
                    c->next = clients;
                    clients = c;

                } // END got new incoming connection
            } // END looping through listener file descriptors

// a helper macro to drop a client from the list
#define DROP_CLIENT { printf("- Client %d took %u bytes\n", c->fd, c->bytes_sent); close(c->fd); FD_CLR(c->fd, &master); if (c == clients) { clients = c->next; free(c); c = clients; } else { p->next = c->next; free(c); c = p->next; } continue; }

            struct client *c = clients, *p = NULL;
            while (c != NULL) {
                if (FD_ISSET(c->fd, &read_fds)) {
                    // handle data from a client
                    int nbytes = recv(c->fd, &c->buffer[c->read], c->needed - c->read, 0);
                    if (nbytes <= 0) {
                        // got error or connection closed by client
                        if (nbytes == 0) {
                            // connection closed
                            printf("- Socket %d hung up\n", c->fd);
                        } else {
                            perror("recv");
                        }
                        DROP_CLIENT

                    }
                    // we got some data from a client
                    c->read += nbytes;
                    if (c->needed == c->read) {
                        // we have a full packet and can process it depending on the current client state
                        switch (c->state) {
                        case handshake_protocolversion:
                            // 7.1.1 ProtocolVersion Handshake
                            //  The client is replying to our protocolversion with theirs - an ASCII string
                            /*
                                                        c->buffer[12] = '\0';
                                                        printf(". Client %d sent protocol version %s", c->fd, c->buffer);
                            */

                            // Basically ignore whatever they sent, and
                            // send Security Types - only one, "no auth"
                            ;
                            static const unsigned char security_handshake[] = { 0x01, 0x01 };
                            if (send(c->fd, security_handshake, 2, 0) == -1) {
                                perror("send");
                                DROP_CLIENT
                            }
                            c->state = handshake_security;
                            c->read = 0;
                            c->needed = 1;
                            break;
                        case handshake_security:
                            // 7.1.2 Security Handshake
                            /*
                                                        printf("Client %d requested security type %u...\n", c->fd, c->buffer[0]);
                            */

                            // send Security Result - always OK (no auth)
                            ;
                            static const unsigned char security_result[] = { 0x00, 0x00, 0x00, 0x00 };
                            if (send(c->fd, security_result, 4, 0) == -1) {
                                perror("send");
                                DROP_CLIENT
                            }
                            c->state = init_client;
                            c->read = 0;
                            c->needed = 1;
                            break;
                        case init_client:
                            // 7.3.1 ClientInit
                            /*
                                                        printf("Client %d sent client_init flag %u...\n", c->fd, c->buffer[0]);
                            */
                            // ignore the flag :P
                            // we send the parameters of the window

                            // 512 x 384
                            ;
                            static const unsigned char server_init[] = { 0x02, 0x00, 0x01, 0x80,
                                                                         // bpp   depth big-e tcol  red-max     green-max   blue-max    r - g - b shift      padding
                                                                         0x08, 0x08, 0x01, 0x01, 0x00, 0x07, 0x00, 0x07, 0x00, 0x03, 0x00, 0x03, 0x06, 0x00, 0x00, 0x00,
                                                                         // window title, 8 chars: "VNCSlots"
                                                                         0x00, 0x00, 0x00, 0x08, 0x56, 0x4e, 0x43, 0x53, 0x6c, 0x6f, 0x74, 0x73
                                                                       };
                            if (send(c->fd, server_init, 32, 0) == -1) {
                                perror("send");
                                DROP_CLIENT
                            }
                            c->state = client_message;
                            c->read = 0;
                            c->needed = 1;
                            break;
                        case client_message:
                            // 7.5 Client Message
                            //  We read only one byte - the message-type - and then switch to a new state to handle each specific message
                            /*
                                                        printf("Client %d sent message %u... ", c->fd, c->buffer[0]);
                            */
                            // the bytes-needed are dependent on the message
                            switch(c->buffer[0]) {
                            case 0:
                                //printf("SetPixelFormat\n");
                                c->state = client_message_setpixelformat;
                                c->needed = 20;
                                break;
                            case 2:
                                //printf("SetEncodings\n");
                                c->state = client_message_setencodings_0;
                                c->needed = 4;
                                break;
                            case 3:
                                //printf("FramebufferUpdateRequest\n");
                                c->state = client_message_framebufferupdaterequest;
                                c->needed = 10;
                                break;
                            case 4:
                                //printf("KeyEvent\n");
                                c->state = client_message_keyevent;
                                c->needed = 8;
                                break;
                            case 5:
                                //printf("PointerEvent\n");
                                c->state = client_message_pointerevent;
                                c->needed = 6;
                                break;
                            case 6:
                                //printf("ClientCutText\n");
                                c->state = client_message_clientcuttext_0;
                                c->needed = 8;
                                break;
                            default:
                                // Got an unknown message-type from the client!  This is bad.
                                fprintf(stderr, "Got unknown message-type %d from client %d!\n", c->buffer[0], c->fd);
                                DROP_CLIENT
                            }
                            break;
                        case client_message_setpixelformat:
                            // 7.5.1 SetPixelFormat
                            //printf("Client %d requested new pixel format...\n", c->fd);
                            c->format.bpp = c->buffer[4];
                            //c->format.depth = c->buffer[5];
                            c->format.big_endian_flag = c->buffer[6];
                            c->format.true_color_flag = c->buffer[7];
                            c->format.red_div = 65536 / ( 1 + ntohs(*(uint16_t*)(&c->buffer[8])));
                            c->format.green_div = 65536 / (1 + ntohs(*(uint16_t*)(&c->buffer[10])));
                            c->format.blue_div = 65536 / (1 + ntohs(*(uint16_t*)(&c->buffer[12])) );
                            c->format.red_shift = c->buffer[14];
                            c->format.green_shift = c->buffer[15];
                            c->format.blue_shift = c->buffer[16];

                            /*
                                                        printf("bpp=%d depth=%d be=%d tc=%d rmax=%08x gmax=%08x bmax=%08x rshft=%d gshft=%d bshft=%d\n",
                                                               c->format.bpp,
                                                               c->buffer[5], //c->format.depth,
                                                               c->format.big_endian_flag,
                                                               c->format.true_color_flag,
                                                               c->format.red_div,
                                                               c->format.green_div,
                                                               c->format.blue_div,
                                                               c->format.red_shift,
                                                               c->format.green_shift,
                                                               c->format.blue_shift
                                                              );
                            */

                            c->state = client_message;
                            c->read = 0;
                            c->needed = 1;
                            break;
                        case client_message_setencodings_0:
                            // 7.5.2 SetEncodings
                            //  this is a "multi-part" message: the first part (this) tells a number of encodings,
                            //  we set ->extra to this, and then the _n part is reading the array entry-by-entry
                            c->extra = ntohs(*(uint16_t*)(&(c->buffer[2])));

                            // blank the encodings bitfield
                            c->encodings = 0;
                            // determine next-state based on ->extra
                            goto foo;
                        case client_message_setencodings_n:
                            // 7.5.2 SetEncodings - interpret the encoding type and set flags if appropriate
                            switch ((int)ntohl(*(uint32_t*)(c->buffer))) {
                            case 0:
                                // RAW encoding - but we always support this
                                break;
                            case 1:
                                // CopyRect encoding
                                c->encodings |= CopyRect;
                                break;
                            case 2:
                                // RRE
                                c->encodings |= RRE;
                                break;
                            case 5:
                                // HexTile
                                c->encodings |= HexTile;
                                break;
                            case 15:
                                // TRLE
                                c->encodings |= TRLE;
                                break;
                            case 16:
                                // ZRLE
                                c->encodings |= ZRLE;
                                break;
                            case -239:
                                // Cursor
                                c->encodings |= Cursor;
                                break;
                            case -223:
                                // DesktopSize
                                break;
                            default:
                                // Other, unknown, unused
                                break;
                            }
                            c->extra --;
foo:
                            c->read = 0;
                            if (c->extra == 0) {
                                // read all the encodings!  back to the regular loop
                                c->state = client_message;
                                c->needed = 1;
                            } else {
                                c->state = client_message_setencodings_n;
                                c->needed = 4;
                            }
                            break;
                        case client_message_framebufferupdaterequest:
                            if (c->buffer[1]) {
                                // incremental request - and so client can just wait
                                c->ready = 1;
                            } else {
                                // they want a whole the entire full complete edition rectangle
                                if (! update(c,
                                             ntohs(*(uint16_t*)(&c->buffer[2])),
                                             ntohs(*(uint16_t*)(&c->buffer[4])),
                                             ntohs(*(uint16_t*)(&c->buffer[6])),
                                             ntohs(*(uint16_t*)(&c->buffer[8])), 0))
                                    DROP_CLIENT

                                    c->ready = 0;
                            }
                            c->state = client_message;
                            c->read = 0;
                            c->needed = 1;
                            break;
                        case client_message_keyevent:
                        {
                            int key = ntohl(*(uint32_t*)(&c->buffer[4]));
                            // space, return, enter, down-arrow
                            if (key == 32 || key == 65421 || key == 65293 || key == 65364) {
                                if (c->buffer[1] && ! c->key_down) {
                                    c->key_down = 1;
                                    if (gamestate == waiting) {
                                        gettimeofday(&tv_next, NULL);
                                        gamestate = coin;
                                        coin_y = 0;
                                    }
                                } else if (! c->buffer[1]) c->key_down = 0;
                            }
                        }

                        c->state = client_message;
                        c->read = 0;
                        c->needed = 1;
                        break;
                        case client_message_pointerevent:
                            // we only really care about the places button 1 state changes
                            if (c->mouse_down != 0 && (c->buffer[1] & 1) == 0) {
                                // button release
                                uint16_t x = ntohs(*(uint16_t*)(&c->buffer[2]));
                                uint16_t y = ntohs(*(uint16_t*)(&c->buffer[4]));
                                if (x >= 451 && x <= 487 && y >= 73 && y <= 109 && c->mouse_down == 1) {
                                    // clicked on handle
                                    if (gamestate == waiting) {
                                        gettimeofday(&tv_next, NULL);
                                        gamestate = coin;

                                        coin_y = 0;
                                    }
                                } else if (x >= 472 && x <= 490 && y >= 365 && y <= 383 && c->mouse_down == 2) {
                                    // clicked COPY button - set cuttext to our github URL
                                    static const unsigned char url_msg[] = { 0x03, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 40,
                                                                             0x68, 0x74, 0x74, 0x70, 0x73, 0x3A, 0x2F, 0x2F, 0x67, 0x69, 0x74, 0x68, 0x75, 0x62, 0x2E, 0x63, 0x6F, 0x6D, 0x2F, 0x67, 0x72, 0x65, 0x67, 0x2D, 0x6B, 0x65, 0x6E, 0x6E, 0x65, 0x64, 0x79, 0x2F, 0x56, 0x4E, 0x43, 0x53, 0x6C, 0x6F, 0x74, 0x73
                                                                           };
                                    if (send(c->fd, url_msg, 48, 0) == -1) {
                                        perror("send");
                                        DROP_CLIENT
                                    }

                                }
                                c->mouse_down = 0;
                            }
                            else if (c->mouse_down == 0 && (c->buffer[1] & 1) == 1)
                            {
                                // check hotspots
                                uint16_t x = ntohs(*(uint16_t*)(&c->buffer[2]));
                                uint16_t y = ntohs(*(uint16_t*)(&c->buffer[4]));
                                if (x >= 451 && x <= 487 && y >= 73 && y <= 109) {
                                    // clicked on handle
                                    c->mouse_down = 1;
                                } else if (x >= 472 && x <= 490 && y >= 365 && y <= 383) {
                                    // clicked COPY button - set cuttext to our github URL
                                    c->mouse_down = 2;
                                }
                            }
                            c->state = client_message;
                            c->read = 0;
                            c->needed = 1;
                            break;
                        case client_message_clientcuttext_0:
                            c->extra = ntohl(*(uint32_t*)(&c->buffer[4]));
                        // printf("Client %d plans to send us %u cut-text\n", c->fd, c->extra);

                        // We don't actually care about the cut-text and just plan to read and discard it,
                        // 20 bytes at a time.

                        // fallthrough

                        case client_message_clientcuttext_n:
                            c->read = 0;
                            if (c->extra == 0) {
                                c->needed = 1;
                                c->state = client_message;
                            } else {
                                c->needed = (c->extra < 20 ? c->extra : 20);
                                c->extra -= c->needed;
                                c->state = client_message_clientcuttext_n;
                            }
                            break;
                        default:
                            fprintf(stderr, "Ended up in unhandled state %d for client %d!\n", c->state, c->fd);
                            DROP_CLIENT

                        }
                    }

                }
                // keep the max fd updated
                if (c->fd > fd_max) fd_max = c->fd;
                p = c;

                c = c->next;
            } // END handle data from client
        } // END looping through file descriptors

        if (gamestate != waiting) {
            // check clock and do any gamestate advancement
            gettimeofday(&tv_now, NULL);

            if (tv_now.tv_sec > tv_next.tv_sec ||
                    (tv_now.tv_sec == tv_next.tv_sec && tv_now.tv_usec > tv_next.tv_usec)) {
                // set timer for next update
                tv_next.tv_sec = tv_now.tv_sec;
                tv_next.tv_usec = tv_now.tv_usec + INTERVAL;

                if (tv_next.tv_usec > 999999) {
                    tv_next.tv_sec ++;
                    tv_next.tv_usec -= 1000000;
                }

                // printf("State %d -> ", state);
                // do game updates now
                switch(gamestate) {
                case coin:
                    coin_y += 2;
                    blit_simple(img_background, 388, 186, framebuffer, 388, 186, 29, 36);
                    blit_special(img_coin, 0, 0, framebuffer, 388, 185 + coin_y, 29, (coin_y < 8 ? 29 : 36 - coin_y), 0xC7, 0);
                    blit_special(img_coinslot, 0, 0, framebuffer, 388, 213, 29, 8, 0xFF, 0);
                    if (coin_y >= 36) {
                        plays ++;

                        draw_number(framebuffer, img_digits, plays, 19, 293);
                        draw_number(framebuffer, img_digits, profit - plays, 19, 353);
                        handle_y = 0;
                        gamestate = handle_down;
                    }
                    break;
                case handle_down:
                    handle_y += 10;
                    draw_handle(framebuffer, img_background, img_handle, img_ball, handle_y);
                    if (handle_y >= 100)
                    {
                        handle_y = 100;
                        gamestate = handle_up;
                    }
                    break;
                case handle_up:
                    handle_y -= 20;
                    draw_handle(framebuffer, img_background, img_handle, img_ball, handle_y);
                    if (handle_y <= 0)
                    {
                        handle_y = 0;

                        // determine three Amounts To Spin - i.e. find the three new Reel Stops
                        //  do this by picking three random values from /dev/urandom...
                        FILE * rng = fopen("/dev/urandom", "rb");
                        if (! rng) {
                            perror("Failed to open /dev/urandom");
                            exit(EXIT_FAILURE);
                        }
                        int rand_result;
                        do {
                            unsigned char rngbuf[2];
                            fread(rngbuf, 1, 2, rng);
                            rand_result = (rngbuf[0] << 8) | rngbuf[1];
                        } while (rand_result >= 64000);
                        fclose(rng);

                        // 960 is a full rotation: spin at least once and each subsequent spin must be longer than the previous
                        unsigned short new_rp = rand_result % 20;
                        rand_result /= 20;
                        reel_left[0] = (reel_stop[0] - new_rp) * 48;
                        while (reel_left[0] < 960) reel_left[0] += 960;
                        reel_stop[0] = new_rp;
                        new_rp = rand_result % 20;
                        rand_result /= 20;
                        reel_left[1] = (reel_stop[1] - new_rp) * 48;
                        while (reel_left[1] <= reel_left[0]) reel_left[1] += 960;
                        reel_stop[1] = new_rp;
                        new_rp = rand_result % 20; // rand_result /= 20;
                        reel_left[2] = (reel_stop[2] - new_rp) * 48;
                        while (reel_left[2] <= reel_left[1]) reel_left[2] += 960;
                        reel_stop[2] = new_rp;
                        gamestate = spin;
                    }
                    break;
                case spin:
                    for (int i = 0; i < 3; i ++) {
                        int amt = (reel_left[i] > 21 ? 21 : reel_left[i]);
                        if (amt > 0) {
                            reel_position[i] -= amt;
                            reel_left[i] -= amt;
                            if (reel_position[i] < 0) reel_position[i] += img_reels[i]->height;
                            draw_reel(framebuffer, img_reels[i], reel_position[i], 222 + 50 * i, 67);
                        }
                    }

                    if (reel_left[0] == 0 && reel_left[1] == 0 && reel_left[2] == 0)
                    {
                        // determine the payout amounts
                        int r0 = reels[0][reel_stop[0]];
                        int r1 = reels[1][reel_stop[1]];
                        int r2 = reels[2][reel_stop[2]];
                        if (r0 == bar && r1 == bar && r2 == bar) payout_left = 100;
                        else if (r0 == bell && r1 == bell && (r2 == bell || r2 == bar)) payout_left = 18;
                        else if (r0 == plum && r1 == plum && (r2 == plum || r2 == bar)) payout_left = 13;
                        else if (r0 == orange && r1 == orange && (r2 == orange || r2 == bar)) payout_left = 11;
                        else if (r0 == cherry && r1 == cherry && r2 == cherry) payout_left = 11;
                        else if (r0 == cherry && r1 == cherry) payout_left = 5;
                        else if (r0 == cherry) payout_left = 3;
                        else payout_left = 0;

                        //printf("SPIN: %s - %s - %s => %d\n", en2s(r0), en2s(r1), en2s(r2), payout_left);

                        gamestate = payout;
                    }

                    break;
                case payout:
                    if (payout_left <= 0) {
                        FILE * stats = fopen("stats.ini", "w");
                        if (stats != NULL) {
                            fprintf(stats, "%d %d\n", plays, profit);
                            fclose(stats);
                        } else perror("stats fopen");

                        gamestate = waiting;
                    } else {
                        payout_left --;
                        profit ++;
                        draw_number(framebuffer, img_digits, profit, 19, 323);
                        draw_number(framebuffer, img_digits, profit - plays, 19, 353);
                    }
                    break;
                default:
                    break;

                }


                // update any waiting clients
                struct client *c = clients, *p = NULL;
                while (c != NULL) {
                    if (c->ready) {
                        if (! update(c, 0, 0, 512, 384, 1)) DROP_CLIENT
                        }
                    c = c->next;
                }
            }
        }
    } // END for(;;)--and you thought it would never end!

    return 0;
}
