#include <pspkernel.h>
#include <pspdebug.h>
#include <pspctrl.h>
#include <pspdisplay.h>
#include <psppower.h>
#include <psprtc.h>
#include <string.h>
#include <stdio.h>
#include <stdlib.h>
#include <unistd.h>
#include <zlib.h>
#include <jpeglib.h>
#include <setjmp.h>

#define STB_IMAGE_IMPLEMENTATION
#define STBI_NO_STDIO
#define STBI_ONLY_JPEG
#define STBI_ONLY_PNG
#include "stb_image.h"

#define SCREEN_WIDTH   480
#define SCREEN_HEIGHT  272
#define BUFFER_WIDTH   512
#define BUFFER_SIZE    (BUFFER_WIDTH * SCREEN_HEIGHT * 4)

#define VRAM_CACHED_0  ((void *)0x04000000)
#define VRAM_CACHED_1  ((void *)(0x04000000 + BUFFER_SIZE))
#define VRAM_UNCACHED_0 ((void *)0x44000000)
#define VRAM_UNCACHED_1 ((void *)(0x44000000 + BUFFER_SIZE))

#define MAX_SERIES     32
#define MAX_CHAPTERS   128
#define MAX_PAGES      1024
#define MAX_PATH_LEN   512
#define MAX_NAME_LEN   128

#define THUMB_WIDTH    90
#define THUMB_HEIGHT   128

PSP_MODULE_INFO("tsundoku", 0, 1, 0);
PSP_MAIN_THREAD_ATTR(THREAD_ATTR_USER | THREAD_ATTR_VFPU);
PSP_HEAP_SIZE_KB(-1024);

typedef enum {
    STATE_LIBRARY,
    STATE_SERIES_DETAIL,
    STATE_READER
} AppState;

typedef enum {
    LIB_VIEW_GRID = 0,
    LIB_VIEW_LIST
} LibraryViewMode;

typedef enum {
    VIEW_FIT_WIDTH = 0,
    VIEW_FIT_SCREEN,
    VIEW_ROTATE_90
} ViewMode;

typedef enum {
    BG_AUTO = 0,
    BG_FORCE_BLACK,
    BG_FORCE_WHITE
} BgMode;

typedef enum {
    READ_MANGA_RTL = 0,
    READ_WESTERN_LTR
} ReadDirection;

typedef struct {
    unsigned char *data;
    int width;
    int height;
    int channels;
    int crop_x0;
    int crop_y0;
    int crop_x1;
    int crop_y1;
} TextureImage;

typedef struct {
    char name[MAX_NAME_LEN];
    unsigned int local_header_offset;
    unsigned int compressed_size;
    unsigned int uncompressed_size;
    unsigned short compression_method;
} PageEntry;

typedef struct {
    char name[MAX_NAME_LEN];
    char folder_path[MAX_PATH_LEN];
    char cover_path[MAX_PATH_LEN];
    char chapter_files[MAX_CHAPTERS][MAX_NAME_LEN];
    int chapter_count;
    TextureImage cover_thumb;
} MangaSeries;

typedef struct {
    PageEntry pages[MAX_PAGES];
    int total_pages;
    int current_page_index;
    char full_path[MAX_PATH_LEN];
    char current_file[MAX_NAME_LEN];
} ComicBook;

typedef struct {
    ViewMode view_mode;
    BgMode bg_mode;
    ReadDirection read_direction;
    int auto_crop;
} Settings;

MangaSeries library[MAX_SERIES];
int series_count = 0;
ComicBook current_comic;
TextureImage current_image = { NULL, 0, 0, 0, 0, 0, 0, 0 };

char base_dir[MAX_PATH_LEN] = {0};
char mangas_dir[MAX_PATH_LEN] = {0};
char cache_dir[MAX_PATH_LEN] = {0};
char bookmarks_path[MAX_PATH_LEN] = {0};

AppState state = STATE_LIBRARY;
LibraryViewMode lib_view = LIB_VIEW_GRID;

Settings config = {
    .view_mode = VIEW_FIT_WIDTH,
    .bg_mode = BG_AUTO,
    .read_direction = READ_MANGA_RTL,
    .auto_crop = 0
};

int lib_selected_index = 0;
int chapter_selected_index = 0;
int scroll_y = 0;
int current_buffer = 0;

int show_help_modal = 0;
int show_settings_modal = 0;
int settings_cursor = 0;
int hud_display_frames = 0;

int is_magnified = 0;
int mag_offset_x = 0;
int mag_offset_y = 0;

unsigned int detected_bg_color = 0xFF000000;
char last_read_path[MAX_PATH_LEN] = {0};

static const unsigned char font8x8_modern[95][8] = {
    {0x00,0x00,0x00,0x00,0x00,0x00,0x00,0x00}, {0x18,0x18,0x18,0x18,0x18,0x00,0x18,0x00},
    {0x66,0x66,0x66,0x00,0x00,0x00,0x00,0x00}, {0x6C,0xFE,0x6C,0x6C,0xFE,0x6C,0x00,0x00},
    {0x18,0x7E,0xC0,0x7C,0x06,0x7E,0x18,0x00}, {0x00,0xC6,0xCC,0x18,0x30,0x66,0xC6,0x00},
    {0x38,0x6C,0x38,0x76,0xDC,0xCC,0x76,0x00}, {0x18,0x18,0x30,0x00,0x00,0x00,0x00,0x00},
    {0x0C,0x18,0x30,0x30,0x30,0x18,0x0C,0x00}, {0x30,0x18,0x0C,0x0C,0x0C,0x18,0x30,0x00},
    {0x00,0x66,0x3C,0xFF,0x3C,0x66,0x00,0x00}, {0x00,0x18,0x18,0x7E,0x18,0x18,0x00,0x00},
    {0x00,0x00,0x00,0x00,0x00,0x18,0x18,0x30}, {0x00,0x00,0x00,0x7E,0x00,0x00,0x00,0x00},
    {0x00,0x00,0x00,0x00,0x00,0x18,0x18,0x00}, {0x06,0x0C,0x18,0x30,0x60,0xC0,0x80,0x00},
    {0x3C,0x66,0x6E,0x76,0x66,0x66,0x3C,0x00}, {0x18,0x38,0x18,0x18,0x18,0x18,0x7E,0x00},
    {0x7C,0x06,0x06,0x3C,0x60,0x60,0x7E,0x00}, {0x7E,0x06,0x1C,0x06,0x06,0x66,0x3C,0x00},
    {0x0C,0x1C,0x3C,0x6C,0xFE,0x0C,0x0C,0x00}, {0x7E,0x60,0x7C,0x06,0x06,0x66,0x3C,0x00},
    {0x1C,0x30,0x60,0x7C,0x66,0x66,0x3C,0x00}, {0x7E,0x06,0x0C,0x18,0x18,0x18,0x18,0x00},
    {0x3C,0x66,0x66,0x3C,0x66,0x66,0x3C,0x00}, {0x3C,0x66,0x66,0x3E,0x06,0x0C,0x38,0x00},
    {0x00,0x18,0x18,0x00,0x18,0x18,0x00,0x00}, {0x00,0x18,0x18,0x00,0x18,0x18,0x30,0x00},
    {0x06,0x0C,0x18,0x30,0x18,0x0C,0x06,0x00}, {0x00,0x00,0x7E,0x00,0x7E,0x00,0x00,0x00},
    {0x60,0x30,0x18,0x0C,0x18,0x30,0x60,0x00}, {0x3C,0x66,0x0C,0x18,0x18,0x00,0x18,0x00},
    {0x3C,0x66,0x6E,0x6E,0x60,0x62,0x3C,0x00}, {0x18,0x3C,0x66,0x66,0x7E,0x66,0x66,0x00},
    {0x7C,0x66,0x66,0x7C,0x66,0x66,0x7C,0x00}, {0x3C,0x66,0x60,0x60,0x60,0x66,0x3C,0x00},
    {0x78,0x6C,0x66,0x66,0x66,0x6C,0x78,0x00}, {0x7E,0x60,0x60,0x7C,0x60,0x60,0x7E,0x00},
    {0x7E,0x60,0x60,0x7C,0x60,0x60,0x60,0x00}, {0x3C,0x66,0x60,0x6E,0x66,0x66,0x3A,0x00},
    {0x66,0x66,0x66,0x7E,0x66,0x66,0x66,0x00}, {0x3C,0x18,0x18,0x18,0x18,0x18,0x3C,0x00},
    {0x06,0x06,0x06,0x06,0x06,0x66,0x3C,0x00}, {0x66,0x6C,0x78,0x70,0x78,0x6C,0x66,0x00},
    {0x60,0x60,0x60,0x60,0x60,0x60,0x7E,0x00}, {0x63,0x77,0x7F,0x6B,0x63,0x63,0x63,0x00},
    {0x66,0x76,0x7E,0x7E,0x6E,0x66,0x66,0x00}, {0x3C,0x66,0x66,0x66,0x66,0x66,0x3C,0x00},
    {0x7C,0x66,0x66,0x7C,0x60,0x60,0x60,0x00}, {0x3C,0x66,0x66,0x66,0x6E,0x3C,0x06,0x00},
    {0x7C,0x66,0x66,0x7C,0x78,0x6C,0x66,0x00}, {0x3C,0x66,0x60,0x3C,0x06,0x66,0x3C,0x00},
    {0x7E,0x18,0x18,0x18,0x18,0x18,0x18,0x00}, {0x66,0x66,0x66,0x66,0x66,0x66,0x3C,0x00},
    {0x66,0x66,0x66,0x66,0x66,0x3C,0x18,0x00}, {0x63,0x63,0x63,0x6B,0x7F,0x77,0x63,0x00},
    {0x66,0x66,0x3C,0x18,0x3C,0x66,0x66,0x00}, {0x66,0x66,0x66,0x3C,0x18,0x18,0x18,0x00},
    {0x7E,0x06,0x0C,0x18,0x30,0x60,0x7E,0x00}, {0x3C,0x30,0x30,0x30,0x30,0x30,0x3C,0x00},
    {0xC0,0x60,0x30,0x18,0x0C,0x06,0x02,0x00}, {0x3C,0x0C,0x0C,0x0C,0x0C,0x0C,0x3C,0x00},
    {0x18,0x3C,0x66,0x00,0x00,0x00,0x00,0x00}, {0x00,0x00,0x00,0x00,0x00,0x00,0xFF,0x00},
    {0x30,0x18,0x0C,0x00,0x00,0x00,0x00,0x00}, {0x00,0x00,0x3C,0x06,0x3E,0x66,0x3B,0x00},
    {0x60,0x60,0x7C,0x66,0x66,0x66,0x7C,0x00}, {0x00,0x00,0x3C,0x66,0x60,0x66,0x3C,0x00},
    {0x06,0x06,0x3E,0x66,0x66,0x66,0x3E,0x00}, {0x00,0x00,0x3C,0x66,0x7E,0x60,0x3C,0x00},
    {0x0E,0x18,0x7E,0x18,0x18,0x18,0x18,0x00}, {0x00,0x00,0x3B,0x66,0x66,0x3E,0x06,0x3C},
    {0x60,0x60,0x7C,0x66,0x66,0x66,0x66,0x00}, {0x18,0x00,0x38,0x18,0x18,0x18,0x3C,0x00},
    {0x06,0x00,0x0E,0x06,0x06,0x66,0x3C,0x00}, {0x60,0x60,0x66,0x6C,0x78,0x6C,0x66,0x00},
    {0x38,0x18,0x18,0x18,0x18,0x18,0x3C,0x00}, {0x00,0x00,0x66,0x7F,0x7F,0x6B,0x63,0x00},
    {0x00,0x00,0x7C,0x66,0x66,0x66,0x66,0x00}, {0x00,0x00,0x3C,0x66,0x66,0x66,0x3C,0x00},
    {0x00,0x00,0x7C,0x66,0x66,0x7C,0x60,0x60}, {0x00,0x00,0x3E,0x66,0x66,0x3E,0x06,0x07},
    {0x00,0x00,0x7C,0x66,0x60,0x60,0x60,0x00}, {0x00,0x00,0x3E,0x60,0x3C,0x06,0x7C,0x00},
    {0x18,0x18,0x7E,0x18,0x18,0x18,0x0E,0x00}, {0x00,0x00,0x66,0x66,0x66,0x66,0x3B,0x00},
    {0x00,0x00,0x66,0x66,0x66,0x3C,0x18,0x00}, {0x00,0x00,0x63,0x6B,0x7F,0x3E,0x36,0x00},
    {0x00,0x00,0x66,0x3C,0x18,0x3C,0x66,0x00}, {0x00,0x00,0x66,0x66,0x66,0x3E,0x06,0x3C},
    {0x00,0x00,0x7E,0x0C,0x18,0x30,0x7E,0x00}, {0x0E,0x18,0x18,0x70,0x18,0x18,0x0E,0x00},
    {0x18,0x18,0x18,0x18,0x18,0x18,0x18,0x00}, {0x70,0x18,0x18,0x0E,0x18,0x18,0x70,0x00},
    {0x76,0xDC,0x00,0x00,0x00,0x00,0x00,0x00}
};

static inline unsigned int read_uint32_safe(const unsigned char *p) {
    return (unsigned int)p[0] | ((unsigned int)p[1] << 8) | ((unsigned int)p[2] << 16) | ((unsigned int)p[3] << 24);
}

static inline unsigned short read_uint16_safe(const unsigned char *p) {
    return (unsigned short)p[0] | ((unsigned short)p[1] << 8);
}

static inline unsigned int convert_rgba_to_abgr(unsigned int pixel) {
    unsigned char r = pixel & 0xFF;
    unsigned char g = (pixel >> 8) & 0xFF;
    unsigned char b = (pixel >> 16) & 0xFF;
    unsigned char a = (pixel >> 24) & 0xFF;
    return (a << 24) | (b << 16) | (g << 8) | r;
}

void draw_char(unsigned int *vram, int x, int y, char c, unsigned int color) {
    if (c < 32 || c > 126) c = '?';
    const unsigned char *glyph = font8x8_modern[c - 32];

    for (int cy = 0; cy < 8; cy++) {
        int py = y + cy;
        if (py < 0 || py >= SCREEN_HEIGHT) continue;
        unsigned char row = glyph[cy];
        unsigned int *dst = vram + py * BUFFER_WIDTH;

        for (int cx = 0; cx < 8; cx++) {
            int px = x + cx;
            if (px < 0 || px >= SCREEN_WIDTH) continue;
            if (row & (0x80 >> cx)) {
                dst[px] = color;
            }
        }
    }
}

void draw_text(unsigned int *vram, int x, int y, const char *str, unsigned int color) {
    int cur_x = x;
    while (*str) {
        draw_char(vram, cur_x, y, *str, color);
        cur_x += 8;
        str++;
    }
}

void draw_rect_solid(unsigned int *vram, int x, int y, int w, int h, unsigned int color) {
    for (int j = 0; j < h; j++) {
        int py = y + j;
        if (py < 0 || py >= SCREEN_HEIGHT) continue;
        unsigned int *dst = vram + py * BUFFER_WIDTH;
        for (int i = 0; i < w; i++) {
            int px = x + i;
            if (px < 0 || px >= SCREEN_WIDTH) continue;
            dst[px] = color;
        }
    }
}

void draw_rect_dimmed(unsigned int *vram, int x, int y, int w, int h) {
    for (int j = 0; j < h; j++) {
        int py = y + j;
        if (py < 0 || py >= SCREEN_HEIGHT) continue;
        unsigned int *dst = vram + py * BUFFER_WIDTH;
        for (int i = 0; i < w; i++) {
            int px = x + i;
            if (px < 0 || px >= SCREEN_WIDTH) continue;
            unsigned int p = dst[px];
            dst[px] = (p >> 1) & 0x7F7F7F7F;
        }
    }
}

void draw_item_glow(unsigned int *vram, int x, int y, int w, int h) {
    for (int j = 0; j < h; j++) {
        int py = y + j;
        if (py < 0 || py >= SCREEN_HEIGHT) continue;
        unsigned int *dst = vram + py * BUFFER_WIDTH;

        for (int i = 0; i < w; i++) {
            int px = x + i;
            if (px < 0 || px >= SCREEN_WIDTH) continue;

            float factor = 1.0f - (float)abs(i - (w / 2)) / (float)(w / 2);
            if (factor < 0.0f) factor = 0.0f;

            unsigned int alpha = (unsigned int)(180.0f * factor);
            unsigned int base_r = (unsigned int)(220.0f * factor);
            unsigned int base_g = (unsigned int)(240.0f * factor);
            unsigned int base_b = (unsigned int)(255.0f * factor);

            dst[px] = (alpha << 24) | (base_b << 16) | (base_g << 8) | base_r;
        }
    }
}

void draw_ps_glyph(unsigned int *vram, int x, int y, char type) {
    if (type == 'X') {
        for (int i = 0; i < 7; i++) {
            vram[(y + i) * BUFFER_WIDTH + (x + i)] = 0xFFFFFFFF;
            vram[(y + i) * BUFFER_WIDTH + (x + 6 - i)] = 0xFFFFFFFF;
        }
    } else if (type == 'O') {
        draw_rect_solid(vram, x + 2, y, 4, 1, 0xFFFFFFFF);
        draw_rect_solid(vram, x + 2, y + 7, 4, 1, 0xFFFFFFFF);
        draw_rect_solid(vram, x, y + 2, 1, 4, 0xFFFFFFFF);
        draw_rect_solid(vram, x + 7, y + 2, 1, 4, 0xFFFFFFFF);
        vram[(y + 1) * BUFFER_WIDTH + (x + 1)] = 0xFFFFFFFF;
        vram[(y + 1) * BUFFER_WIDTH + (x + 6)] = 0xFFFFFFFF;
        vram[(y + 6) * BUFFER_WIDTH + (x + 1)] = 0xFFFFFFFF;
        vram[(y + 6) * BUFFER_WIDTH + (x + 6)] = 0xFFFFFFFF;
    } else if (type == 'S') {
        draw_rect_solid(vram, x, y, 8, 1, 0xFFFFFFFF);
        draw_rect_solid(vram, x, y + 7, 8, 1, 0xFFFFFFFF);
        draw_rect_solid(vram, x, y, 1, 8, 0xFFFFFFFF);
        draw_rect_solid(vram, x + 7, y, 1, 8, 0xFFFFFFFF);
    } else if (type == 'T') {
        vram[y * BUFFER_WIDTH + (x + 3)] = 0xFFFFFFFF;
        vram[y * BUFFER_WIDTH + (x + 4)] = 0xFFFFFFFF;
        vram[(y + 1) * BUFFER_WIDTH + (x + 2)] = 0xFFFFFFFF;
        vram[(y + 1) * BUFFER_WIDTH + (x + 5)] = 0xFFFFFFFF;
        vram[(y + 2) * BUFFER_WIDTH + (x + 2)] = 0xFFFFFFFF;
        vram[(y + 2) * BUFFER_WIDTH + (x + 5)] = 0xFFFFFFFF;
        vram[(y + 3) * BUFFER_WIDTH + (x + 1)] = 0xFFFFFFFF;
        vram[(y + 3) * BUFFER_WIDTH + (x + 6)] = 0xFFFFFFFF;
        vram[(y + 4) * BUFFER_WIDTH + (x + 1)] = 0xFFFFFFFF;
        vram[(y + 4) * BUFFER_WIDTH + (x + 6)] = 0xFFFFFFFF;
        vram[(y + 5) * BUFFER_WIDTH + x] = 0xFFFFFFFF;
        vram[(y + 5) * BUFFER_WIDTH + (x + 7)] = 0xFFFFFFFF;
        draw_rect_solid(vram, x, y + 6, 8, 1, 0xFFFFFFFF);
    } else if (type == 'U') {
        vram[y * BUFFER_WIDTH + (x + 3)] = 0xFFFFFFFF;
        draw_rect_solid(vram, x + 2, y + 1, 3, 1, 0xFFFFFFFF);
        draw_rect_solid(vram, x + 1, y + 2, 5, 1, 0xFFFFFFFF);
        draw_rect_solid(vram, x + 2, y + 3, 3, 4, 0xFFFFFFFF);
    } else if (type == 'D') {
        draw_rect_solid(vram, x + 2, y, 3, 4, 0xFFFFFFFF);
        draw_rect_solid(vram, x + 1, y + 4, 5, 1, 0xFFFFFFFF);
        draw_rect_solid(vram, x + 2, y + 5, 3, 1, 0xFFFFFFFF);
        vram[(y + 6) * BUFFER_WIDTH + (x + 3)] = 0xFFFFFFFF;
    } else if (type == 'L') {
        vram[(y + 3) * BUFFER_WIDTH + x] = 0xFFFFFFFF;
        draw_rect_solid(vram, x + 1, y + 2, 1, 3, 0xFFFFFFFF);
        draw_rect_solid(vram, x + 2, y + 1, 1, 5, 0xFFFFFFFF);
        draw_rect_solid(vram, x + 3, y + 2, 4, 3, 0xFFFFFFFF);
    } else if (type == 'R') {
        draw_rect_solid(vram, x, y + 2, 4, 3, 0xFFFFFFFF);
        draw_rect_solid(vram, x + 4, y + 1, 1, 5, 0xFFFFFFFF);
        draw_rect_solid(vram, x + 5, y + 2, 1, 3, 0xFFFFFFFF);
        vram[(y + 3) * BUFFER_WIDTH + (x + 6)] = 0xFFFFFFFF;
    }
}

void draw_image_scaled(unsigned int *vram, const TextureImage *img, int dst_x, int dst_y, int dst_w, int dst_h) {
    if (!img || !img->data || img->width <= 0 || img->height <= 0) {
        draw_rect_solid(vram, dst_x, dst_y, dst_w, dst_h, 0xFF141414);
        draw_rect_solid(vram, dst_x, dst_y, dst_w, 1, 0xFF2A2A2A);
        draw_rect_solid(vram, dst_x, dst_y + dst_h - 1, dst_w, 1, 0xFF2A2A2A);
        draw_rect_solid(vram, dst_x, dst_y, 1, dst_h, 0xFF2A2A2A);
        draw_rect_solid(vram, dst_x + dst_w - 1, dst_y, 1, dst_h, 0xFF2A2A2A);
        draw_text(vram, dst_x + (dst_w - 64) / 2, dst_y + dst_h / 2 - 4, "NO COVER", 0xFF555555);
        return;
    }

    const unsigned int *src = (const unsigned int *)img->data;

    for (int y = 0; y < dst_h; y++) {
        int py = dst_y + y;
        if (py < 0 || py >= SCREEN_HEIGHT) continue;

        int src_y = (y * img->height) / dst_h;
        if (src_y >= img->height) src_y = img->height - 1;

        unsigned int *dst_row = vram + py * BUFFER_WIDTH;
        const unsigned int *src_row = src + src_y * img->width;

        for (int x = 0; x < dst_w; x++) {
            int px = dst_x + x;
            if (px < 0 || px >= SCREEN_WIDTH) continue;

            int src_x = (x * img->width) / dst_w;
            if (src_x >= img->width) src_x = img->width - 1;

            dst_row[px] = src_row[src_x];
        }
    }
}

unsigned int detect_edge_background(const TextureImage *img) {
    if (!img->data || img->width <= 0 || img->height <= 0) return 0xFF000000;

    const unsigned int *src = (const unsigned int *)img->data;
    long long total_luma = 0;
    int samples = 0;

    for (int i = 0; i < 32; i++) {
        int sx = (i * (img->width - 1)) / 31;
        unsigned int p_top = src[sx];
        unsigned int p_bot = src[(img->height - 1) * img->width + sx];
        total_luma += (int)(0.299f * (p_top & 0xFF) + 0.587f * ((p_top >> 8) & 0xFF) + 0.114f * ((p_top >> 16) & 0xFF));
        total_luma += (int)(0.299f * (p_bot & 0xFF) + 0.587f * ((p_bot >> 8) & 0xFF) + 0.114f * ((p_bot >> 16) & 0xFF));
        samples += 2;
    }

    for (int i = 0; i < 32; i++) {
        int sy = (i * (img->height - 1)) / 31;
        unsigned int p_left = src[sy * img->width];
        unsigned int p_right = src[sy * img->width + (img->width - 1)];
        total_luma += (int)(0.299f * (p_left & 0xFF) + 0.587f * ((p_left >> 8) & 0xFF) + 0.114f * ((p_left >> 16) & 0xFF));
        total_luma += (int)(0.299f * (p_right & 0xFF) + 0.587f * ((p_right >> 8) & 0xFF) + 0.114f * ((p_right >> 16) & 0xFF));
        samples += 2;
    }

    int avg_luma = (int)(total_luma / samples);
    return (avg_luma > 128) ? 0xFFFFFFFF : 0xFF000000;
}

void calculate_autocrop_box(TextureImage *img) {
    img->crop_x0 = 0;
    img->crop_y0 = 0;
    img->crop_x1 = img->width - 1;
    img->crop_y1 = img->height - 1;

    if (!img->data || img->width <= 32 || img->height <= 32) return;

    const unsigned int *src = (const unsigned int *)img->data;
    int step_x = img->width / 64;
    if (step_x < 1) step_x = 1;
    int step_y = img->height / 64;
    if (step_y < 1) step_y = 1;

    for (int y = 0; y < img->height / 4; y += step_y) {
        int has_content = 0;
        for (int x = 0; x < img->width; x += step_x) {
            unsigned int p = src[y * img->width + x];
            int luma = (int)(0.299f * (p & 0xFF) + 0.587f * ((p >> 8) & 0xFF) + 0.114f * ((p >> 16) & 0xFF));
            if (luma < 220) { has_content = 1; break; }
        }
        if (has_content) { img->crop_y0 = y; break; }
    }

    for (int y = img->height - 1; y > (img->height * 3) / 4; y -= step_y) {
        int has_content = 0;
        for (int x = 0; x < img->width; x += step_x) {
            unsigned int p = src[y * img->width + x];
            int luma = (int)(0.299f * (p & 0xFF) + 0.587f * ((p >> 8) & 0xFF) + 0.114f * ((p >> 16) & 0xFF));
            if (luma < 220) { has_content = 1; break; }
        }
        if (has_content) { img->crop_y1 = y; break; }
    }

    for (int x = 0; x < img->width / 4; x += step_x) {
        int has_content = 0;
        for (int y = 0; y < img->height; y += step_y) {
            unsigned int p = src[y * img->width + x];
            int luma = (int)(0.299f * (p & 0xFF) + 0.587f * ((p >> 8) & 0xFF) + 0.114f * ((p >> 16) & 0xFF));
            if (luma < 220) { has_content = 1; break; }
        }
        if (has_content) { img->crop_x0 = x; break; }
    }

    for (int x = img->width - 1; x > (img->width * 3) / 4; x -= step_x) {
        int has_content = 0;
        for (int y = 0; y < img->height; y += step_y) {
            unsigned int p = src[y * img->width + x];
            int luma = (int)(0.299f * (p & 0xFF) + 0.587f * ((p >> 8) & 0xFF) + 0.114f * ((p >> 16) & 0xFF));
            if (luma < 220) { has_content = 1; break; }
        }
        if (has_content) { img->crop_x1 = x; break; }
    }
}

int load_saved_page(const char *filename) {
    SceUID fd = sceIoOpen(bookmarks_path, PSP_O_RDONLY, 0777);
    if (fd < 0) return 0;

    char line[512];
    int saved_page = 0;
    int bytes_read = sceIoRead(fd, line, sizeof(line) - 1);
    sceIoClose(fd);

    if (bytes_read > 0) {
        line[bytes_read] = '\0';
        char *entry = strtok(line, "\n");
        while (entry) {
            char name[MAX_NAME_LEN];
            int p = 0;
            if (sscanf(entry, "%127[^=]=%d", name, &p) == 2) {
                if (strcmp(name, filename) == 0) {
                    saved_page = p;
                    break;
                }
            }
            entry = strtok(NULL, "\n");
        }
    }
    return saved_page;
}

void load_last_read_target(void) {
    SceUID fd = sceIoOpen(bookmarks_path, PSP_O_RDONLY, 0777);
    if (fd < 0) return;

    char line[512];
    int bytes_read = sceIoRead(fd, line, sizeof(line) - 1);
    sceIoClose(fd);

    if (bytes_read > 0) {
        line[bytes_read] = '\0';
        char *entry = strtok(line, "\n");
        while (entry) {
            if (strncmp(entry, "LAST_PATH=", 10) == 0) {
                snprintf(last_read_path, sizeof(last_read_path), "%s", entry + 10);
                break;
            }
            entry = strtok(NULL, "\n");
        }
    }
}

void save_current_progress(void) {
    if (strlen(current_comic.current_file) == 0) return;

    char buffer[2048] = {0};
    SceUID fd = sceIoOpen(bookmarks_path, PSP_O_RDONLY, 0777);
    if (fd >= 0) {
        int r = sceIoRead(fd, buffer, sizeof(buffer) - 512);
        if (r >= 0) buffer[r] = '\0';
        sceIoClose(fd);
    }

    char out_buf[2048] = {0};
    char temp[MAX_PATH_LEN + 32];
    snprintf(temp, sizeof(temp), "LAST_PATH=%s\n", current_comic.full_path);
    strcat(out_buf, temp);

    char *entry = strtok(buffer, "\n");
    int replaced = 0;

    while (entry) {
        if (strncmp(entry, "LAST_PATH=", 10) == 0) {
            entry = strtok(NULL, "\n");
            continue;
        }

        char name[MAX_NAME_LEN];
        int p = 0;
        if (sscanf(entry, "%127[^=]=%d", name, &p) == 2) {
            if (strcmp(name, current_comic.current_file) == 0) {
                snprintf(temp, sizeof(temp), "%s=%d\n", current_comic.current_file, current_comic.current_page_index);
                strcat(out_buf, temp);
                replaced = 1;
            } else {
                strcat(out_buf, entry);
                strcat(out_buf, "\n");
            }
        }
        entry = strtok(NULL, "\n");
    }

    if (!replaced) {
        snprintf(temp, sizeof(temp), "%s=%d\n", current_comic.current_file, current_comic.current_page_index);
        strcat(out_buf, temp);
    }

    fd = sceIoOpen(bookmarks_path, PSP_O_WRONLY | PSP_O_CREAT | PSP_O_TRUNC, 0777);
    if (fd >= 0) {
        sceIoWrite(fd, out_buf, strlen(out_buf));
        sceIoClose(fd);
    }
}

int exit_callback(int arg1, int arg2, void *common) {
    save_current_progress();
    sceKernelExitGame();
    return 0;
}

int callback_thread(SceSize args, void *argp) {
    int cbid = sceKernelCreateCallback("Exit Callback", exit_callback, NULL);
    sceKernelRegisterExitCallback(cbid);
    sceKernelSleepThreadCB();
    return 0;
}

void setup_callbacks(void) {
    int thid = sceKernelCreateThread("update_thread", callback_thread, 0x11, 0xFA0, 0, 0);
    if (thid >= 0) {
        sceKernelStartThread(thid, 0, 0);
    }
}

int is_cbz_file(const char *name) {
    int len = strlen(name);
    if (len < 4) return 0;
    return (strcasecmp(name + len - 4, ".cbz") == 0);
}

int is_image_file(const char *name) {
    int len = strlen(name);
    if (len < 4) return 0;
    if (strcasecmp(name + len - 4, ".jpg") == 0 || strcasecmp(name + len - 4, ".png") == 0) return 1;
    if (len >= 5 && strcasecmp(name + len - 5, ".jpeg") == 0) return 1;
    return 0;
}

int compare_strings(const void *a, const void *b) {
    return strcmp((const char *)a, (const char *)b);
}

int load_cached_thumbnail(const char *series_name, TextureImage *out_thumb) {
    char cache_file[MAX_PATH_LEN];
    snprintf(cache_file, sizeof(cache_file), "%s/%s_v6.bin", cache_dir, series_name);

    SceUID fd = sceIoOpen(cache_file, PSP_O_RDONLY, 0777);
    if (fd < 0) return 0;

    int expected_size = THUMB_WIDTH * THUMB_HEIGHT * 4;
    unsigned int *buf = (unsigned int *)malloc(expected_size);
    if (!buf) {
        sceIoClose(fd);
        return 0;
    }

    int read_bytes = sceIoRead(fd, buf, expected_size);
    sceIoClose(fd);

    if (read_bytes == expected_size) {
        out_thumb->data = (unsigned char *)buf;
        out_thumb->width = THUMB_WIDTH;
        out_thumb->height = THUMB_HEIGHT;
        out_thumb->channels = 4;
        return 1;
    }

    free(buf);
    return 0;
}

void save_cached_thumbnail(const char *series_name, const TextureImage *thumb) {
    if (!thumb->data) return;

    sceIoMkdir(cache_dir, 0777);

    char cache_file[MAX_PATH_LEN];
    snprintf(cache_file, sizeof(cache_file), "%s/%s_v6.bin", cache_dir, series_name);

    SceUID fd = sceIoOpen(cache_file, PSP_O_WRONLY | PSP_O_CREAT | PSP_O_TRUNC, 0777);
    if (fd >= 0) {
        sceIoWrite(fd, thumb->data, THUMB_WIDTH * THUMB_HEIGHT * 4);
        sceIoClose(fd);
    }
}

struct my_error_mgr {
    struct jpeg_error_mgr pub;
    jmp_buf setjmp_buffer;
};

static void my_error_exit(j_common_ptr cinfo) {
    struct my_error_mgr *myerr = (struct my_error_mgr *)cinfo->err;
    longjmp(myerr->setjmp_buffer, 1);
}

int decode_jpeg_scaled_thumbnail(const char *filepath, const char *series_name, TextureImage *out_thumb) {
    FILE *infile = fopen(filepath, "rb");
    if (!infile) return 0;

    struct jpeg_decompress_struct cinfo;
    struct my_error_mgr jerr;

    cinfo.err = jpeg_std_error(&jerr.pub);
    jerr.pub.error_exit = my_error_exit;

    if (setjmp(jerr.setjmp_buffer)) {
        jpeg_destroy_decompress(&cinfo);
        fclose(infile);
        return 0;
    }

    jpeg_create_decompress(&cinfo);
    jpeg_stdio_src(&cinfo, infile);
    jpeg_read_header(&cinfo, TRUE);

    cinfo.scale_num = 1;
    cinfo.scale_denom = 8;
    cinfo.out_color_space = JCS_RGB;

    jpeg_start_decompress(&cinfo);

    int scaled_w = cinfo.output_width;
    int scaled_h = cinfo.output_height;

    unsigned char *temp_rgb = (unsigned char *)malloc(scaled_w * scaled_h * 3);
    if (!temp_rgb) {
        jpeg_finish_decompress(&cinfo);
        jpeg_destroy_decompress(&cinfo);
        fclose(infile);
        return 0;
    }

    while (cinfo.output_scanline < cinfo.output_height) {
        unsigned char *row_ptr = temp_rgb + (cinfo.output_scanline * scaled_w * 3);
        jpeg_read_scanlines(&cinfo, &row_ptr, 1);
    }

    jpeg_finish_decompress(&cinfo);
    jpeg_destroy_decompress(&cinfo);
    fclose(infile);

    int crop_w, crop_h, crop_x, crop_y;
    if (scaled_w * THUMB_HEIGHT > scaled_h * THUMB_WIDTH) {
        crop_h = scaled_h;
        crop_w = (scaled_h * THUMB_WIDTH) / THUMB_HEIGHT;
        crop_x = (scaled_w - crop_w) / 2;
        crop_y = 0;
    } else {
        crop_w = scaled_w;
        crop_h = (scaled_w * THUMB_HEIGHT) / THUMB_WIDTH;
        crop_x = 0;
        crop_y = (scaled_h - crop_h) / 2;
    }

    unsigned int *thumb_pixels = (unsigned int *)malloc(THUMB_WIDTH * THUMB_HEIGHT * 4);
    if (thumb_pixels) {
        for (int y = 0; y < THUMB_HEIGHT; y++) {
            int sy = crop_y + (y * crop_h) / THUMB_HEIGHT;
            for (int x = 0; x < THUMB_WIDTH; x++) {
                int sx = crop_x + (x * crop_w) / THUMB_WIDTH;
                unsigned char *p = temp_rgb + (sy * scaled_w + sx) * 3;
                unsigned int r = p[0];
                unsigned int g = p[1];
                unsigned int b = p[2];
                thumb_pixels[y * THUMB_WIDTH + x] = (0xFF << 24) | (b << 16) | (g << 8) | r;
            }
        }
        out_thumb->data = (unsigned char *)thumb_pixels;
        out_thumb->width = THUMB_WIDTH;
        out_thumb->height = THUMB_HEIGHT;
        out_thumb->channels = 4;

        save_cached_thumbnail(series_name, out_thumb);
    }

    free(temp_rgb);
    return (out_thumb->data != NULL);
}

void load_cover_thumbnail(MangaSeries *series) {
    if (load_cached_thumbnail(series->name, &series->cover_thumb)) {
        return;
    }

    if (strlen(series->cover_path) > 0) {
        int len = strlen(series->cover_path);
        if (len >= 4 && (strcasecmp(series->cover_path + len - 4, ".jpg") == 0 || strcasecmp(series->cover_path + len - 5, ".jpeg") == 0)) {
            if (decode_jpeg_scaled_thumbnail(series->cover_path, series->name, &series->cover_thumb)) {
                return;
            }
        }

        SceUID fd = sceIoOpen(series->cover_path, PSP_O_RDONLY, 0777);
        if (fd >= 0) {
            SceOff fsize = sceIoLseek(fd, 0, PSP_SEEK_END);
            sceIoLseek(fd, 0, PSP_SEEK_SET);
            if (fsize > 0 && fsize < (2 * 1024 * 1024)) {
                unsigned char *raw = (unsigned char *)malloc(fsize);
                if (raw) {
                    sceIoRead(fd, raw, fsize);
                    int orig_w, orig_h, comp;
                    unsigned char *full = stbi_load_from_memory(raw, fsize, &orig_w, &orig_h, &comp, 4);
                    if (full) {
                        int crop_w, crop_h, crop_x, crop_y;
                        if (orig_w * THUMB_HEIGHT > orig_h * THUMB_WIDTH) {
                            crop_h = orig_h;
                            crop_w = (orig_h * THUMB_WIDTH) / THUMB_HEIGHT;
                            crop_x = (orig_w - crop_w) / 2;
                            crop_y = 0;
                        } else {
                            crop_w = orig_w;
                            crop_h = (orig_w * THUMB_HEIGHT) / THUMB_WIDTH;
                            crop_x = 0;
                            crop_y = (orig_h - crop_h) / 2;
                        }

                        unsigned int *thumb_pixels = (unsigned int *)malloc(THUMB_WIDTH * THUMB_HEIGHT * 4);
                        if (thumb_pixels) {
                            unsigned int *src = (unsigned int *)full;
                            for (int y = 0; y < THUMB_HEIGHT; y++) {
                                int sy = crop_y + (y * crop_h) / THUMB_HEIGHT;
                                for (int x = 0; x < THUMB_WIDTH; x++) {
                                    int sx = crop_x + (x * crop_w) / THUMB_WIDTH;
                                    unsigned int pixel = src[sy * orig_w + sx];
                                    unsigned char r = pixel & 0xFF;
                                    unsigned char g = (pixel >> 8) & 0xFF;
                                    unsigned char b = (pixel >> 16) & 0xFF;
                                    unsigned char a = (pixel >> 24) & 0xFF;
                                    thumb_pixels[y * THUMB_WIDTH + x] = (a << 24) | (b << 16) | (g << 8) | r;
                                }
                            }
                            series->cover_thumb.data = (unsigned char *)thumb_pixels;
                            series->cover_thumb.width = THUMB_WIDTH;
                            series->cover_thumb.height = THUMB_HEIGHT;
                            series->cover_thumb.channels = 4;
                            save_cached_thumbnail(series->name, &series->cover_thumb);
                        }
                        stbi_image_free(full);
                    }
                    free(raw);
                }
            }
            sceIoClose(fd);
        }
    }
}

void scan_mangas_library(const char *root_path) {
    for (int i = 0; i < series_count; i++) {
        if (library[i].cover_thumb.data) {
            free(library[i].cover_thumb.data);
            library[i].cover_thumb.data = NULL;
        }
    }
    series_count = 0;

    char dir_names[MAX_SERIES][MAX_NAME_LEN];
    int found_dirs = 0;

    SceUID dfd = sceIoDopen(root_path);
    if (dfd >= 0) {
        SceIoDirent dir;
        memset(&dir, 0, sizeof(SceIoDirent));

        while (sceIoDread(dfd, &dir) > 0 && found_dirs < MAX_SERIES) {
            if (dir.d_name[0] == '.') continue;

            if ((dir.d_stat.st_attr & 0x0010) || FIO_SO_ISDIR(dir.d_stat.st_attr)) {
                snprintf(dir_names[found_dirs], sizeof(dir_names[found_dirs]), "%s", dir.d_name);
                found_dirs++;
            }
        }
        sceIoDclose(dfd);
    }

    for (int i = 0; i < found_dirs && series_count < MAX_SERIES; i++) {
        MangaSeries *s = &library[series_count];
        memset(s, 0, sizeof(MangaSeries));

        snprintf(s->name, sizeof(s->name), "%s", dir_names[i]);
        snprintf(s->folder_path, sizeof(s->folder_path), "%s/%s", root_path, dir_names[i]);

        char current_folder[MAX_PATH_LEN];
        snprintf(current_folder, sizeof(current_folder), "%s", s->folder_path);

        SceUID sub_dfd = sceIoDopen(current_folder);
        if (sub_dfd >= 0) {
            SceIoDirent sub_dir;
            memset(&sub_dir, 0, sizeof(SceIoDirent));

            while (sceIoDread(sub_dfd, &sub_dir) > 0) {
                if (sub_dir.d_name[0] == '.') continue;

                if (is_cbz_file(sub_dir.d_name) && s->chapter_count < MAX_CHAPTERS) {
                    snprintf(s->chapter_files[s->chapter_count], sizeof(s->chapter_files[s->chapter_count]), "%s", sub_dir.d_name);
                    s->chapter_count++;
                } else if (is_image_file(sub_dir.d_name)) {
                    if (strncasecmp(sub_dir.d_name, "cover", 5) == 0 || strncasecmp(sub_dir.d_name, "folder", 6) == 0) {
                        snprintf(s->cover_path, sizeof(s->cover_path), "%s/%s", current_folder, sub_dir.d_name);
                    }
                }
            }
            sceIoDclose(sub_dfd);
        }

        if (s->chapter_count > 0) {
            qsort(s->chapter_files, s->chapter_count, MAX_NAME_LEN, compare_strings);
            load_cover_thumbnail(s);
            series_count++;
        }
    }

    if (lib_selected_index >= series_count && series_count > 0) {
        lib_selected_index = series_count - 1;
    }
}

int load_cbz_metadata(const char *full_path) {
    SceUID fd = sceIoOpen(full_path, PSP_O_RDONLY, 0777);
    if (fd < 0) return 0;

    SceOff file_size = sceIoLseek(fd, 0, PSP_SEEK_END);
    if (file_size < 22) {
        sceIoClose(fd);
        return 0;
    }

    int search_buf_size = file_size < 1024 ? (int)file_size : 1024;
    unsigned char buffer[1024];
    sceIoLseek(fd, file_size - search_buf_size, PSP_SEEK_SET);
    sceIoRead(fd, buffer, search_buf_size);

    int eocd_pos = -1;
    for (int i = search_buf_size - 22; i >= 0; i--) {
        if (read_uint32_safe(buffer + i) == 0x06054B50) {
            eocd_pos = i;
            break;
        }
    }

    if (eocd_pos < 0) {
        sceIoClose(fd);
        return 0;
    }

    const unsigned char *eocd_ptr = buffer + eocd_pos;
    unsigned short total_entries = read_uint16_safe(eocd_ptr + 10);
    unsigned int central_dir_offset = read_uint32_safe(eocd_ptr + 16);

    current_comic.total_pages = 0;
    snprintf(current_comic.full_path, sizeof(current_comic.full_path), "%s", full_path);

    sceIoLseek(fd, central_dir_offset, PSP_SEEK_SET);

    unsigned char cd_raw[46];
    char filename_buffer[MAX_NAME_LEN];

    for (int i = 0; i < total_entries && current_comic.total_pages < MAX_PAGES; i++) {
        if (sceIoRead(fd, cd_raw, 46) != 46) break;
        if (read_uint32_safe(cd_raw) != 0x02014B50) break;

        unsigned short compression_method = read_uint16_safe(cd_raw + 10);
        unsigned int compressed_size = read_uint32_safe(cd_raw + 20);
        unsigned int uncompressed_size = read_uint32_safe(cd_raw + 24);
        unsigned short filename_length = read_uint16_safe(cd_raw + 28);
        unsigned short extra_field_length = read_uint16_safe(cd_raw + 30);
        unsigned short file_comment_length = read_uint16_safe(cd_raw + 32);
        unsigned int relative_offset = read_uint32_safe(cd_raw + 42);

        int name_len = filename_length >= MAX_NAME_LEN ? MAX_NAME_LEN - 1 : filename_length;
        memset(filename_buffer, 0, sizeof(filename_buffer));
        sceIoRead(fd, filename_buffer, name_len);

        int skip = (filename_length - name_len) + extra_field_length + file_comment_length;
        if (skip > 0) sceIoLseek(fd, skip, PSP_SEEK_CUR);

        if (is_image_file(filename_buffer)) {
            PageEntry *p = &current_comic.pages[current_comic.total_pages];
            snprintf(p->name, sizeof(p->name), "%s", filename_buffer);
            p->local_header_offset = relative_offset;
            p->compressed_size = compressed_size;
            p->uncompressed_size = uncompressed_size;
            p->compression_method = compression_method;
            current_comic.total_pages++;
        }
    }

    sceIoClose(fd);

    if (current_comic.total_pages > 0) {
        int saved = load_saved_page(current_comic.current_file);
        current_comic.current_page_index = (saved >= 0 && saved < current_comic.total_pages) ? saved : 0;
        return 1;
    }

    return 0;
}

int load_current_page(void) {
    if (current_image.data) {
        stbi_image_free(current_image.data);
        current_image.data = NULL;
    }

    scroll_y = 0;
    mag_offset_x = 0;
    mag_offset_y = 0;
    hud_display_frames = 90;

    if (current_comic.total_pages == 0 || current_comic.current_page_index >= current_comic.total_pages) {
        return 0;
    }

    save_current_progress();

    PageEntry *page = &current_comic.pages[current_comic.current_page_index];
    SceUID fd = sceIoOpen(current_comic.full_path, PSP_O_RDONLY, 0777);
    if (fd < 0) return 0;

    sceIoLseek(fd, page->local_header_offset, PSP_SEEK_SET);
    
    unsigned char local_raw[30];
    if (sceIoRead(fd, local_raw, 30) != 30) {
        sceIoClose(fd);
        return 0;
    }

    unsigned short fn_len = read_uint16_safe(local_raw + 26);
    unsigned short extra_len = read_uint16_safe(local_raw + 28);
    sceIoLseek(fd, fn_len + extra_len, PSP_SEEK_CUR);

    unsigned char *compressed_data = (unsigned char *)malloc(page->compressed_size);
    if (!compressed_data) {
        sceIoClose(fd);
        return 0;
    }

    sceIoRead(fd, compressed_data, page->compressed_size);
    sceIoClose(fd);

    unsigned char *raw_image_data = NULL;
    int raw_image_size = page->uncompressed_size;

    if (page->compression_method == 0) {
        raw_image_data = compressed_data;
    } else if (page->compression_method == 8) {
        raw_image_data = (unsigned char *)malloc(page->uncompressed_size);
        if (!raw_image_data) {
            free(compressed_data);
            return 0;
        }

        z_stream stream;
        memset(&stream, 0, sizeof(stream));
        stream.next_in = compressed_data;
        stream.avail_in = page->compressed_size;
        stream.next_out = raw_image_data;
        stream.avail_out = page->uncompressed_size;

        inflateInit2(&stream, -MAX_WBITS);
        inflate(&stream, Z_FINISH);
        inflateEnd(&stream);
        free(compressed_data);
    } else {
        free(compressed_data);
        return 0;
    }

    current_image.data = stbi_load_from_memory(raw_image_data, raw_image_size, 
                                              &current_image.width, 
                                              &current_image.height, 
                                              &current_image.channels, 4);
    free(raw_image_data);

    if (current_image.data) {
        detected_bg_color = detect_edge_background(&current_image);
        calculate_autocrop_box(&current_image);
    }

    return (current_image.data != NULL);
}

void draw_snes_statusbar(unsigned int *vram, const char *title) {
    draw_rect_solid(vram, 0, 0, SCREEN_WIDTH, 14, 0xFF000000);
    draw_rect_solid(vram, 0, 14, SCREEN_WIDTH, 1, 0xFFFFFFFF);

    draw_text(vram, 4, 3, title, 0xFFFFFFFF);

    ScePspDateTime time;
    sceRtcGetCurrentClockLocalTime(&time);

    int bat_percent = scePowerGetBatteryLifePercent();
    int bat_time = scePowerGetBatteryLifeTime();

    char stat_str[64];
    if (bat_time >= 0) {
        snprintf(stat_str, sizeof(stat_str), "%02d:%02d Bat:%d%%(%02dh%02dm)", 
                 time.hour, time.minute, bat_percent, bat_time / 60, bat_time % 60);
    } else {
        snprintf(stat_str, sizeof(stat_str), "%02d:%02d Bat:%d%%", time.hour, time.minute, bat_percent);
    }

    int stat_x = SCREEN_WIDTH - (strlen(stat_str) * 8) - 4;
    draw_text(vram, stat_x, 3, stat_str, 0xFFFFFFFF);
}

void render_help_modal(unsigned int *vram) {
    int modal_w = 390;
    int modal_h = 200;
    int modal_x = (SCREEN_WIDTH - modal_w) / 2;
    int modal_y = (SCREEN_HEIGHT - modal_h) / 2;

    draw_rect_dimmed(vram, 0, 0, SCREEN_WIDTH, SCREEN_HEIGHT);
    draw_rect_solid(vram, modal_x, modal_y, modal_w, modal_h, 0xFF101010);
    draw_rect_solid(vram, modal_x, modal_y, modal_w, 2, 0xFFFFFFFF);
    draw_rect_solid(vram, modal_x, modal_y + modal_h - 2, modal_w, 2, 0xFF333333);
    draw_rect_solid(vram, modal_x, modal_y, 2, modal_h, 0xFF333333);
    draw_rect_solid(vram, modal_x + modal_w - 2, modal_y, 2, modal_h, 0xFF333333);

    draw_text(vram, modal_x + 135, modal_y + 12, "COMMANDS GUIDE", 0xFFFFFFFF);
    draw_rect_solid(vram, modal_x + 20, modal_y + 26, modal_w - 40, 1, 0xFF333333);

    draw_text(vram, modal_x + 24, modal_y + 38,  "ANALOG STICK    : 2D Pan (Zoom) / Smooth Scroll", 0xFFDDDDDD);
    draw_text(vram, modal_x + 24, modal_y + 56,  "D-PAD UP/DOWN   : Fast scroll up/down", 0xFFDDDDDD);
    draw_text(vram, modal_x + 24, modal_y + 74,  "L / R TRIGGERS  : Prev / Next page", 0xFFDDDDDD);
    draw_ps_glyph(vram, modal_x + 24, modal_y + 92, 'S');
    draw_text(vram, modal_x + 36, modal_y + 92,  "Toggle Magnifier Lens", 0xFFFFFFFF);
    draw_ps_glyph(vram, modal_x + 24, modal_y + 110, 'O');
    draw_text(vram, modal_x + 36, modal_y + 110, "Switch View Mode", 0xFFFFFFFF);
    draw_ps_glyph(vram, modal_x + 24, modal_y + 128, 'T');
    draw_text(vram, modal_x + 36, modal_y + 128, "Close Manga", 0xFFFFFFFF);
    draw_text(vram, modal_x + 24, modal_y + 146, "START           : Settings (Auto-Crop, etc)", 0xFFFFFFFF);
    draw_text(vram, modal_x + 24, modal_y + 164, "SELECT          : Toggle this Help", 0xFFFFFFFF);

    draw_text(vram, modal_x + 100, modal_y + 182, "Press SELECT to close", 0xFFAAAAAA);
}

void render_settings_modal(unsigned int *vram) {
    int modal_w = 350;
    int modal_h = 175;
    int modal_x = (SCREEN_WIDTH - modal_w) / 2;
    int modal_y = (SCREEN_HEIGHT - modal_h) / 2;

    draw_rect_dimmed(vram, 0, 0, SCREEN_WIDTH, SCREEN_HEIGHT);
    draw_rect_solid(vram, modal_x, modal_y, modal_w, modal_h, 0xFF101010);
    draw_rect_solid(vram, modal_x, modal_y, modal_w, 2, 0xFFFFFFFF);
    draw_rect_solid(vram, modal_x, modal_y + modal_h - 2, modal_w, 2, 0xFF333333);
    draw_rect_solid(vram, modal_x, modal_y, 2, modal_h, 0xFF333333);
    draw_rect_solid(vram, modal_x + modal_w - 2, modal_y, 2, modal_h, 0xFF333333);

    draw_text(vram, modal_x + 120, modal_y + 14, "SETTINGS MENU", 0xFFFFFFFF);
    draw_rect_solid(vram, modal_x + 20, modal_y + 28, modal_w - 40, 1, 0xFF333333);

    const char *view_str = (config.view_mode == VIEW_FIT_WIDTH) ? "Fit Width" :
                           (config.view_mode == VIEW_FIT_SCREEN) ? "Fit Screen" : "Rotate 90";

    const char *bg_str = (config.bg_mode == BG_AUTO) ? "Auto Detect" :
                         (config.bg_mode == BG_FORCE_BLACK) ? "Force Black" : "Force White";

    const char *dir_str = (config.read_direction == READ_MANGA_RTL) ? "Manga (R->L)" : "Western (L->R)";
    const char *crop_str = config.auto_crop ? "Enabled (Cut Margin)" : "Disabled (Full Scans)";

    char buf[128];

    snprintf(buf, sizeof(buf), "%s View Mode:   <%s>", (settings_cursor == 0 ? "->" : "  "), view_str);
    draw_text(vram, modal_x + 20, modal_y + 44, buf, (settings_cursor == 0 ? 0xFFFFFFFF : 0xFF888888));

    snprintf(buf, sizeof(buf), "%s Background:  <%s>", (settings_cursor == 1 ? "->" : "  "), bg_str);
    draw_text(vram, modal_x + 20, modal_y + 66, buf, (settings_cursor == 1 ? 0xFF00FFFF : 0xFF888888));

    snprintf(buf, sizeof(buf), "%s Direction:   <%s>", (settings_cursor == 2 ? "->" : "  "), dir_str);
    draw_text(vram, modal_x + 20, modal_y + 88, buf, (settings_cursor == 2 ? 0xFF00FFFF : 0xFF888888));

    snprintf(buf, sizeof(buf), "%s Auto-Crop:   <%s>", (settings_cursor == 3 ? "->" : "  "), crop_str);
    draw_text(vram, modal_x + 20, modal_y + 110, buf, (settings_cursor == 3 ? 0xFF00FFFF : 0xFF888888));

    draw_text(vram, modal_x + 40, modal_y + 148, "[LEFT/RIGHT] Change  |  [START] Save", 0xFFAAAAAA);
}

void render_library_screen(unsigned int *vram) {
    memset(vram, 0, BUFFER_SIZE);

    draw_snes_statusbar(vram, "[MangaReader] - Library");

    if (series_count == 0) {
        char err_str[MAX_PATH_LEN + 32];
        snprintf(err_str, sizeof(err_str), "No mangas in: %s", mangas_dir);
        draw_text(vram, 20, 110, err_str, 0xFF888888);
        draw_text(vram, 20, 130, "Put folders inside with .cbz files", 0xFF666666);
    } else if (lib_view == LIB_VIEW_GRID) {
        int thumb_w = THUMB_WIDTH;
        int thumb_h = THUMB_HEIGHT;
        int spacing_x = 24;
        int spacing_y = 12;
        int start_x = 20;
        int start_y = 20;

        int items_per_row = 4;
        int row_index = lib_selected_index / items_per_row;
        int max_visible_rows = 1;
        int row_offset = row_index >= max_visible_rows ? row_index - max_visible_rows + 1 : 0;

        for (int i = 0; i < series_count; i++) {
            int col = i % items_per_row;
            int row = i / items_per_row;

            if (row < row_offset || row >= row_offset + 2) continue;

            int x = start_x + col * (thumb_w + spacing_x);
            int y = 22 + start_y + (row - row_offset) * (thumb_h + spacing_y);

            draw_image_scaled(vram, &library[i].cover_thumb, x, y, thumb_w, thumb_h);

            if (i == lib_selected_index) {
                draw_rect_solid(vram, x - 2, y - 2, thumb_w + 4, 2, 0xFFFFFFFF);
                draw_rect_solid(vram, x - 2, y + thumb_h, thumb_w + 4, 2, 0xFFFFFFFF);
                draw_rect_solid(vram, x - 2, y - 2, 2, thumb_h + 4, 0xFFFFFFFF);
                draw_rect_solid(vram, x + thumb_w, y - 2, 2, thumb_h + 4, 0xFFFFFFFF);
            }

            char short_title[12];
            snprintf(short_title, sizeof(short_title), "%.10s", library[i].name);
            draw_text(vram, x + 2, y + thumb_h + 2, short_title, i == lib_selected_index ? 0xFFFFFFFF : 0xFF888888);
        }
    } else {
        int start_y = 24;
        int page_offset = (lib_selected_index / 15) * 15;

        for (int i = 0; i < 15; i++) {
            int idx = page_offset + i;
            if (idx >= series_count) break;

            int y = start_y + i * 15;
            if (idx == lib_selected_index) {
                draw_item_glow(vram, 8, y, SCREEN_WIDTH - 20, 13);
                draw_text(vram, 12, y + 2, library[idx].name, 0xFFFFFFFF);
            } else {
                draw_text(vram, 12, y + 2, library[idx].name, 0xFFAAAAAA);
            }

            char count_str[16];
            snprintf(count_str, sizeof(count_str), "%dV", library[idx].chapter_count);
            draw_text(vram, SCREEN_WIDTH - 45, y + 2, count_str, 0xFF666666);
        }
    }

    draw_rect_solid(vram, 0, SCREEN_HEIGHT - 16, SCREEN_WIDTH, 16, 0xFF000000);
    draw_rect_solid(vram, 0, SCREEN_HEIGHT - 16, SCREEN_WIDTH, 1, 0xFFFFFFFF);

    draw_ps_glyph(vram, 8, SCREEN_HEIGHT - 12, 'X');
    draw_text(vram, 20, SCREEN_HEIGHT - 12, "Open", 0xFFFFFFFF);

    draw_ps_glyph(vram, 74, SCREEN_HEIGHT - 12, 'S');
    draw_text(vram, 86, SCREEN_HEIGHT - 12, "View", 0xFFFFFFFF);

    draw_text(vram, 140, SCREEN_HEIGHT - 12, "SELECT", 0xFFFFFFFF);
    draw_text(vram, 192, SCREEN_HEIGHT - 12, "Scan", 0xFFFFFFFF);

    draw_ps_glyph(vram, 246, SCREEN_HEIGHT - 12, 'O');
    draw_text(vram, 258, SCREEN_HEIGHT - 12, "Resume", 0xFFFFFFFF);

    draw_ps_glyph(vram, 340, SCREEN_HEIGHT - 12, 'U');
    draw_ps_glyph(vram, 349, SCREEN_HEIGHT - 12, 'D');
    draw_ps_glyph(vram, 358, SCREEN_HEIGHT - 12, 'L');
    draw_ps_glyph(vram, 367, SCREEN_HEIGHT - 12, 'R');
    draw_text(vram, 378, SCREEN_HEIGHT - 12, "Browse", 0xFFFFFFFF);
}

void render_series_detail_screen(unsigned int *vram) {
    memset(vram, 0, BUFFER_SIZE);

    MangaSeries *s = &library[lib_selected_index];

    char title_buf[128];
    snprintf(title_buf, sizeof(title_buf), "[MangaReader] - %s", s->name);
    draw_snes_statusbar(vram, title_buf);

    int cover_w = 120;
    int cover_h = 170;
    draw_image_scaled(vram, &s->cover_thumb, 14, 30, cover_w, cover_h);

    draw_rect_solid(vram, 13, 29, cover_w + 2, 1, 0xFF333333);
    draw_rect_solid(vram, 13, 30 + cover_h, cover_w + 2, 1, 0xFF333333);
    draw_rect_solid(vram, 13, 29, 1, cover_h + 2, 0xFF333333);
    draw_rect_solid(vram, 14 + cover_w, 29, 1, cover_h + 2, 0xFF333333);

    int start_x = 146;
    int start_y = 22;
    int max_visible = 15;
    int page_offset = (chapter_selected_index / max_visible) * max_visible;

    for (int i = 0; i < max_visible; i++) {
        int idx = page_offset + i;
        if (idx >= s->chapter_count) break;

        int y = start_y + i * 14;
        if (idx == chapter_selected_index) {
            draw_item_glow(vram, start_x, y, SCREEN_WIDTH - start_x - 14, 13);
            draw_text(vram, start_x + 4, y + 2, s->chapter_files[idx], 0xFFFFFFFF);
        } else {
            draw_text(vram, start_x + 4, y + 2, s->chapter_files[idx], 0xFFAAAAAA);
        }
    }

    if (s->chapter_count > max_visible) {
        int scroll_track_h = max_visible * 14;
        int scrollbar_h = (max_visible * scroll_track_h) / s->chapter_count;
        if (scrollbar_h < 8) scrollbar_h = 8;
        int scrollbar_y = start_y + (chapter_selected_index * (scroll_track_h - scrollbar_h)) / (s->chapter_count - 1);

        int scroll_x = SCREEN_WIDTH - 8;
        draw_rect_solid(vram, scroll_x, start_y, 2, scroll_track_h, 0xFF222222);
        draw_rect_solid(vram, scroll_x, scrollbar_y, 2, scrollbar_h, 0xFFFFFFFF);
    }

    draw_rect_solid(vram, 0, SCREEN_HEIGHT - 16, SCREEN_WIDTH, 16, 0xFF000000);
    draw_rect_solid(vram, 0, SCREEN_HEIGHT - 16, SCREEN_WIDTH, 1, 0xFFFFFFFF);

    draw_ps_glyph(vram, 8, SCREEN_HEIGHT - 12, 'X');
    draw_text(vram, 20, SCREEN_HEIGHT - 12, "Read Volume", 0xFFFFFFFF);

    draw_ps_glyph(vram, 140, SCREEN_HEIGHT - 12, 'T');
    draw_text(vram, 152, SCREEN_HEIGHT - 12, "Parent dir", 0xFFFFFFFF);

    draw_ps_glyph(vram, 340, SCREEN_HEIGHT - 12, 'U');
    draw_ps_glyph(vram, 349, SCREEN_HEIGHT - 12, 'D');
    draw_text(vram, 360, SCREEN_HEIGHT - 12, "Browse", 0xFFFFFFFF);
}

void render_reader_screen(unsigned int *vram) {
    unsigned int active_bg = (config.bg_mode == BG_FORCE_BLACK) ? 0xFF000000 :
                             (config.bg_mode == BG_FORCE_WHITE) ? 0xFFFFFFFF : detected_bg_color;

    if (!current_image.data) {
        memset(vram, 0, BUFFER_SIZE);
        return;
    }

    const unsigned int *src = (const unsigned int *)current_image.data;

    int src_x0 = config.auto_crop ? current_image.crop_x0 : 0;
    int src_y0 = config.auto_crop ? current_image.crop_y0 : 0;
    int src_w  = config.auto_crop ? (current_image.crop_x1 - current_image.crop_x0 + 1) : current_image.width;
    int src_h  = config.auto_crop ? (current_image.crop_y1 - current_image.crop_y0 + 1) : current_image.height;

    if (config.view_mode == VIEW_ROTATE_90) {
        for (int i = 0; i < BUFFER_WIDTH * SCREEN_HEIGHT; i++) vram[i] = active_bg;

        float scale_x = (float)SCREEN_HEIGHT / (float)src_w;
        float scale_y = (float)SCREEN_WIDTH / (float)src_h;
        float base_scale = scale_x < scale_y ? scale_x : scale_y;
        float scale = is_magnified ? (base_scale * 1.25f) : base_scale;

        int render_w = (int)(src_w * scale);
        int render_h = (int)(src_h * scale);

        int max_pan_x = render_h > SCREEN_WIDTH ? render_h - SCREEN_WIDTH : 0;
        int max_pan_y = render_w > SCREEN_HEIGHT ? render_w - SCREEN_HEIGHT : 0;

        if (mag_offset_x > max_pan_x) mag_offset_x = max_pan_x;
        if (mag_offset_x < 0) mag_offset_x = 0;
        if (mag_offset_y > max_pan_y) mag_offset_y = max_pan_y;
        if (mag_offset_y < 0) mag_offset_y = 0;

        int offset_screen_x = (render_h < SCREEN_WIDTH) ? (SCREEN_WIDTH - render_h) / 2 : 0;
        int offset_screen_y = (render_w < SCREEN_HEIGHT) ? (SCREEN_HEIGHT - render_w) / 2 : 0;

        for (int y = 0; y < render_h; y++) {
            int src_y = src_y0 + (int)(y / scale);
            if (src_y >= current_image.height) src_y = current_image.height - 1;

            int dst_x = offset_screen_x + (render_h - 1 - y) - (is_magnified ? mag_offset_x : 0);
            if (dst_x < 0 || dst_x >= SCREEN_WIDTH) continue;

            for (int x = 0; x < render_w; x++) {
                int src_x = src_x0 + (int)(x / scale);
                if (src_x >= current_image.width) src_x = current_image.width - 1;

                int dst_y = offset_screen_y + x - (is_magnified ? mag_offset_y : 0);
                if (dst_y < 0 || dst_y >= SCREEN_HEIGHT) continue;

                vram[dst_y * BUFFER_WIDTH + dst_x] = src[src_y * current_image.width + src_x];
            }
        }
    } else if (config.view_mode == VIEW_FIT_SCREEN) {
        for (int i = 0; i < BUFFER_WIDTH * SCREEN_HEIGHT; i++) vram[i] = active_bg;

        float scale_x = (float)SCREEN_WIDTH / (float)src_w;
        float scale_y = (float)SCREEN_HEIGHT / (float)src_h;
        float base_scale = scale_x < scale_y ? scale_x : scale_y;
        float scale = is_magnified ? (base_scale * 1.75f) : base_scale;

        int render_w = (int)(src_w * scale);
        int render_h = (int)(src_h * scale);

        int max_pan_x = render_w > SCREEN_WIDTH ? render_w - SCREEN_WIDTH : 0;
        int max_pan_y = render_h > SCREEN_HEIGHT ? render_h - SCREEN_HEIGHT : 0;

        if (mag_offset_x > max_pan_x) mag_offset_x = max_pan_x;
        if (mag_offset_x < 0) mag_offset_x = 0;
        if (mag_offset_y > max_pan_y) mag_offset_y = max_pan_y;
        if (mag_offset_y < 0) mag_offset_y = 0;

        int offset_x = (render_w < SCREEN_WIDTH) ? (SCREEN_WIDTH - render_w) / 2 : 0;
        int offset_y = (render_h < SCREEN_HEIGHT) ? (SCREEN_HEIGHT - render_h) / 2 : 0;

        for (int y = 0; y < render_h; y++) {
            int src_y = src_y0 + (int)(y / scale);
            if (src_y >= current_image.height) src_y = current_image.height - 1;

            int dst_y = offset_y + y - (is_magnified ? mag_offset_y : 0);
            if (dst_y < 0 || dst_y >= SCREEN_HEIGHT) continue;

            unsigned int *dst_row = vram + dst_y * BUFFER_WIDTH;
            const unsigned int *src_row = src + src_y * current_image.width;

            for (int x = 0; x < render_w; x++) {
                int src_x = src_x0 + (int)(x / scale);
                if (src_x >= current_image.width) src_x = current_image.width - 1;

                int dst_x = offset_x + x - (is_magnified ? mag_offset_x : 0);
                if (dst_x < 0 || dst_x >= SCREEN_WIDTH) continue;

                dst_row[dst_x] = src_row[src_x];
            }
        }
    } else {
        float base_scale = (float)SCREEN_WIDTH / (float)src_w;
        float scale = is_magnified ? (base_scale * 1.25f) : base_scale;
        int total_scaled_height = (int)(src_h * scale);
        int total_scaled_width  = (int)(src_w * scale);

        int max_pan_x = total_scaled_width > SCREEN_WIDTH ? total_scaled_width - SCREEN_WIDTH : 0;
        int max_scroll = total_scaled_height > SCREEN_HEIGHT ? total_scaled_height - SCREEN_HEIGHT : 0;

        if (is_magnified) {
            if (mag_offset_x > max_pan_x) mag_offset_x = max_pan_x;
            if (mag_offset_x < 0) mag_offset_x = 0;
            if (mag_offset_y > max_scroll) mag_offset_y = max_scroll;
            if (mag_offset_y < 0) mag_offset_y = 0;
        } else {
            if (scroll_y > max_scroll) scroll_y = max_scroll;
            if (scroll_y < 0) scroll_y = 0;
        }

        int active_y_offset = is_magnified ? mag_offset_y : scroll_y;
        int active_x_offset = is_magnified ? mag_offset_x : 0;

        for (int y = 0; y < SCREEN_HEIGHT; y++) {
            int virtual_y = y + active_y_offset;
            int off_y = (int)(virtual_y / scale);
            int src_y = src_y0 + off_y;

            if (off_y >= src_h || src_y >= current_image.height) {
                for (int x = 0; x < SCREEN_WIDTH; x++) vram[y * BUFFER_WIDTH + x] = active_bg;
                continue;
            }

            unsigned int *dst_row = vram + y * BUFFER_WIDTH;
            const unsigned int *src_row = src + src_y * current_image.width;

            for (int x = 0; x < SCREEN_WIDTH; x++) {
                int virtual_x = x + active_x_offset;
                int src_x = src_x0 + (int)(virtual_x / scale);
                if (src_x >= current_image.width) src_x = current_image.width - 1;
                dst_row[x] = src_row[src_x];
            }
        }
    }

    if (hud_display_frames > 0 && !show_help_modal && !show_settings_modal) {
        char osd[32];
        snprintf(osd, sizeof(osd), "%02d / %02d", current_comic.current_page_index + 1, current_comic.total_pages);
        int w = strlen(osd) * 8 + 16;
        draw_rect_solid(vram, SCREEN_WIDTH - w - 12, 12, w, 18, 0xCC000000);
        draw_text(vram, SCREEN_WIDTH - w - 4, 17, osd, 0xFFFFFFFF);
        hud_display_frames--;
    }

    if (show_help_modal) {
        render_help_modal(vram);
    } else if (show_settings_modal) {
        render_settings_modal(vram);
    }
}

void flip_display(void (*render_func)(unsigned int *)) {
    void *draw_cached = (current_buffer == 0) ? VRAM_CACHED_1 : VRAM_CACHED_0;
    void *draw_uncached = (current_buffer == 0) ? VRAM_UNCACHED_1 : VRAM_UNCACHED_0;

    render_func((unsigned int *)draw_cached);

    sceKernelDcacheWritebackRange(draw_cached, BUFFER_SIZE);
    sceDisplaySetFrameBuf(draw_uncached, BUFFER_WIDTH, PSP_DISPLAY_PIXEL_FORMAT_8888, PSP_DISPLAY_SETBUF_NEXTFRAME);
    sceDisplayWaitVblankStart();

    current_buffer = 1 - current_buffer;
}

int main(int argc, char *argv[]) {
    setup_callbacks();
    scePowerSetClockFrequency(333, 333, 166);

    sceCtrlSetSamplingCycle(0);
    sceCtrlSetSamplingMode(PSP_CTRL_MODE_ANALOG);

    sceDisplaySetMode(0, SCREEN_WIDTH, SCREEN_HEIGHT);
    sceDisplaySetFrameBuf(VRAM_UNCACHED_0, BUFFER_WIDTH, PSP_DISPLAY_PIXEL_FORMAT_8888, PSP_DISPLAY_SETBUF_NEXTFRAME);

    memset(base_dir, 0, sizeof(base_dir));
    if (getcwd(base_dir, sizeof(base_dir) - 1) == NULL || strlen(base_dir) == 0) {
        if (argc > 0 && argv[0] != NULL) {
            snprintf(base_dir, sizeof(base_dir), "%s", argv[0]);
            char *last_slash = strrchr(base_dir, '/');
            if (last_slash) *last_slash = '\0';
        }
    }

    if (strlen(base_dir) == 0) {
        snprintf(base_dir, sizeof(base_dir), ".");
    }

    snprintf(mangas_dir, sizeof(mangas_dir), "%s/mangas", base_dir);
    snprintf(cache_dir, sizeof(cache_dir), "%s/.thumbs", base_dir);
    snprintf(bookmarks_path, sizeof(bookmarks_path), "%s/bookmarks.dat", base_dir);

    SceUID test_dfd = sceIoDopen(mangas_dir);
    if (test_dfd < 0) {
        snprintf(mangas_dir, sizeof(mangas_dir), "./mangas");
        snprintf(cache_dir, sizeof(cache_dir), "./.thumbs");
        snprintf(bookmarks_path, sizeof(bookmarks_path), "./bookmarks.dat");
    } else {
        sceIoDclose(test_dfd);
    }

    load_last_read_target();
    scan_mangas_library(mangas_dir);

    SceCtrlData pad;
    SceCtrlData last_pad;
    memset(&last_pad, 0, sizeof(SceCtrlData));

    int needs_redraw = 1;

    while (1) {
        sceCtrlReadBufferPositive(&pad, 1);
        int pressed = pad.Buttons & ~last_pad.Buttons;

        if (state == STATE_LIBRARY) {
            if (pressed & PSP_CTRL_SQUARE) {
                lib_view = (lib_view == LIB_VIEW_GRID) ? LIB_VIEW_LIST : LIB_VIEW_GRID;
                needs_redraw = 1;
            }

            if (pressed & PSP_CTRL_SELECT) {
                scan_mangas_library(mangas_dir);
                needs_redraw = 1;
            }

            if (pressed & PSP_CTRL_CIRCLE) {
                if (strlen(last_read_path) > 0) {
                    char *slash = strrchr(last_read_path, '/');
                    if (slash) {
                        snprintf(current_comic.current_file, sizeof(current_comic.current_file), "%s", slash + 1);
                    }
                    if (load_cbz_metadata(last_read_path)) {
                        state = STATE_READER;
                        show_help_modal = 0;
                        show_settings_modal = 0;
                        is_magnified = 0;
                        load_current_page();
                        needs_redraw = 1;
                    }
                }
            }

            if (series_count > 0) {
                if (lib_view == LIB_VIEW_GRID) {
                    if (pressed & PSP_CTRL_RIGHT) { lib_selected_index = (lib_selected_index + 1) % series_count; needs_redraw = 1; }
                    if (pressed & PSP_CTRL_LEFT)  { lib_selected_index = (lib_selected_index - 1 + series_count) % series_count; needs_redraw = 1; }
                    if (pressed & PSP_CTRL_DOWN)  { if (lib_selected_index + 4 < series_count) lib_selected_index += 4; needs_redraw = 1; }
                    if (pressed & PSP_CTRL_UP)    { if (lib_selected_index - 4 >= 0) lib_selected_index -= 4; needs_redraw = 1; }
                } else {
                    if (pressed & PSP_CTRL_DOWN) { lib_selected_index = (lib_selected_index + 1) % series_count; needs_redraw = 1; }
                    if (pressed & PSP_CTRL_UP)   { lib_selected_index = (lib_selected_index - 1 + series_count) % series_count; needs_redraw = 1; }
                }

                if (pressed & PSP_CTRL_CROSS) {
                    state = STATE_SERIES_DETAIL;
                    chapter_selected_index = 0;
                    needs_redraw = 1;
                }
            }
        } 
        else if (state == STATE_SERIES_DETAIL) {
            MangaSeries *s = &library[lib_selected_index];

            if (pressed & PSP_CTRL_TRIANGLE) {
                state = STATE_LIBRARY;
                needs_redraw = 1;
            }

            if (s->chapter_count > 0) {
                if (pressed & PSP_CTRL_DOWN) { chapter_selected_index = (chapter_selected_index + 1) % s->chapter_count; needs_redraw = 1; }
                if (pressed & PSP_CTRL_UP)   { chapter_selected_index = (chapter_selected_index - 1 + s->chapter_count) % s->chapter_count; needs_redraw = 1; }

                if (pressed & PSP_CTRL_CROSS) {
                    char full_path[MAX_PATH_LEN];
                    snprintf(full_path, sizeof(full_path), "%s/%s", s->folder_path, s->chapter_files[chapter_selected_index]);
                    snprintf(current_comic.current_file, sizeof(current_comic.current_file), "%s", s->chapter_files[chapter_selected_index]);

                    if (load_cbz_metadata(full_path)) {
                        state = STATE_READER;
                        show_help_modal = 0;
                        show_settings_modal = 0;
                        is_magnified = 0;
                        load_current_page();
                        needs_redraw = 1;
                    }
                }
            }
        } 
        else if (state == STATE_READER) {
            if (pressed & PSP_CTRL_SELECT) {
                if (!show_settings_modal) {
                    show_help_modal = !show_help_modal;
                    needs_redraw = 1;
                }
            }

            if (pressed & PSP_CTRL_START) {
                if (!show_help_modal) {
                    show_settings_modal = !show_settings_modal;
                    needs_redraw = 1;
                }
            }

            if (show_settings_modal) {
                if (pressed & PSP_CTRL_DOWN) {
                    settings_cursor = (settings_cursor + 1) % 4;
                    needs_redraw = 1;
                }
                if (pressed & PSP_CTRL_UP) {
                    settings_cursor = (settings_cursor - 1 + 4) % 4;
                    needs_redraw = 1;
                }
                if ((pressed & PSP_CTRL_RIGHT) || (pressed & PSP_CTRL_CROSS)) {
                    if (settings_cursor == 0) {
                        config.view_mode = (config.view_mode + 1) % 3;
                        is_magnified = 0;
                    }
                    else if (settings_cursor == 1) config.bg_mode = (config.bg_mode + 1) % 3;
                    else if (settings_cursor == 2) config.read_direction = (config.read_direction + 1) % 2;
                    else if (settings_cursor == 3) config.auto_crop = !config.auto_crop;
                    needs_redraw = 1;
                }
                if (pressed & PSP_CTRL_LEFT) {
                    if (settings_cursor == 0) {
                        config.view_mode = (config.view_mode - 1 + 3) % 3;
                        is_magnified = 0;
                    }
                    else if (settings_cursor == 1) config.bg_mode = (config.bg_mode - 1 + 3) % 3;
                    else if (settings_cursor == 2) config.read_direction = (config.read_direction - 1 + 2) % 2;
                    else if (settings_cursor == 3) config.auto_crop = !config.auto_crop;
                    needs_redraw = 1;
                }
                if (pressed & PSP_CTRL_CIRCLE) {
                    show_settings_modal = 0;
                    needs_redraw = 1;
                }
            }
            else if (!show_help_modal) {
                if (pressed & PSP_CTRL_SQUARE) {
                    is_magnified = !is_magnified;
                    needs_redraw = 1;
                }

                if (pressed & PSP_CTRL_CIRCLE) {
                    config.view_mode = (config.view_mode + 1) % 3;
                    is_magnified = 0;
                    needs_redraw = 1;
                }

                int go_next = 0;
                int go_prev = 0;

                if (config.view_mode == VIEW_ROTATE_90) {
                    if ((pressed & PSP_CTRL_LTRIGGER) || (pressed & PSP_CTRL_UP)) go_next = 1;
                    if ((pressed & PSP_CTRL_RTRIGGER) || (pressed & PSP_CTRL_DOWN)) go_prev = 1;
                } else {
                    if (config.read_direction == READ_MANGA_RTL) {
                        if ((pressed & PSP_CTRL_LTRIGGER) || (pressed & PSP_CTRL_LEFT)) go_next = 1;
                        if ((pressed & PSP_CTRL_RTRIGGER) || (pressed & PSP_CTRL_RIGHT)) go_prev = 1;
                    } else {
                        if ((pressed & PSP_CTRL_RTRIGGER) || (pressed & PSP_CTRL_RIGHT)) go_next = 1;
                        if ((pressed & PSP_CTRL_LTRIGGER) || (pressed & PSP_CTRL_LEFT)) go_prev = 1;
                    }
                }

                if (go_next) {
                    if (current_comic.current_page_index < current_comic.total_pages - 1) {
                        current_comic.current_page_index++;
                        load_current_page();
                        needs_redraw = 1;
                    }
                } else if (go_prev) {
                    if (current_comic.current_page_index > 0) {
                        current_comic.current_page_index--;
                        load_current_page();
                        needs_redraw = 1;
                    }
                }

                int analog_x = (int)pad.Lx - 128;
                int analog_y = (int)pad.Ly - 128;

                if (is_magnified) {
                    if (analog_x > 30 || analog_x < -30) { mag_offset_x += (analog_x * 14) / 128; needs_redraw = 1; }
                    if (analog_y > 30 || analog_y < -30) { mag_offset_y += (analog_y * 14) / 128; needs_redraw = 1; }
                } else if (config.view_mode == VIEW_FIT_WIDTH) {
                    if (analog_y > 30 || analog_y < -30) { scroll_y += (analog_y * 18) / 128; needs_redraw = 1; }
                    if (pad.Buttons & PSP_CTRL_DOWN) { scroll_y += 16; needs_redraw = 1; }
                    if (pad.Buttons & PSP_CTRL_UP)   { scroll_y -= 16; needs_redraw = 1; }
                }

                if (pressed & PSP_CTRL_TRIANGLE) {
                    save_current_progress();
                    if (current_image.data) {
                        stbi_image_free(current_image.data);
                        current_image.data = NULL;
                    }
                    hud_display_frames = 0;
                    is_magnified = 0;
                    state = STATE_SERIES_DETAIL;
                    needs_redraw = 1;
                }
            }
        }

        if (state == STATE_READER && hud_display_frames > 0) {
            needs_redraw = 1;
        }

        if (needs_redraw) {
            if (state == STATE_LIBRARY) {
                flip_display(render_library_screen);
            } else if (state == STATE_SERIES_DETAIL) {
                flip_display(render_series_detail_screen);
            } else if (state == STATE_READER) {
                flip_display(render_reader_screen);
            }
            needs_redraw = 0;
        }

        last_pad = pad;
        sceKernelDelayThread(10000);
    }

    sceKernelExitGame();
    return 0;
}