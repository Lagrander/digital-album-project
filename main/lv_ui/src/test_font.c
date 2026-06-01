/*******************************************************************************
 * Size: 16 px
 * Bpp: 4
 * Opts: --font C:\Windows\Fonts\simhei.ttf --size 16 --bpp 4 --format lvgl --symbols 一二三 -o w:\Desktop\digital_album_project\main\lv_ui\src\test_font.c
 ******************************************************************************/

#ifdef LV_LVGL_H_INCLUDE_SIMPLE
#include "lvgl.h"
#else
#include "lvgl/lvgl.h"
#endif

#ifndef TEST_FONT
#define TEST_FONT 1
#endif

#if TEST_FONT

/*-----------------
 *    BITMAPS
 *----------------*/

/*Store the image of the glyphs*/
static LV_ATTRIBUTE_LARGE_CONST const uint8_t glyph_bitmap[] = {
    /* U+4E00 "一" */
    0x1a, 0xa4, 0xcf, 0xfd, 0x42, 0xa, 0x8e, 0xff,
    0xf2, 0x80,

    /* U+4E09 "三" */
    0x0, 0x22, 0xa2, 0x7e, 0x53, 0x0, 0xbe, 0xea,
    0xbf, 0x5c, 0x80, 0x57, 0xdd, 0xfe, 0xa0, 0xf,
    0xfe, 0xb2, 0xb2, 0xaf, 0x99, 0x0, 0x3a, 0x62,
    0xef, 0xd1, 0xa0, 0x1d, 0x9d, 0xdf, 0xa4, 0x3,
    0xff, 0xc6, 0x3f, 0xff, 0xfc, 0x21, 0xab, 0xbf,
    0xfa, 0xa8,

    /* U+4E8C "二" */
    0x0, 0x1b, 0x37, 0xf3, 0x98, 0x6, 0xb9, 0x9f,
    0xe8, 0x50, 0xd, 0x1f, 0xff, 0xe6, 0x0, 0xff,
    0xff, 0x80, 0x7f, 0xf2, 0xc6, 0x1d, 0xff, 0xe8,
    0x80, 0x1, 0xa6, 0x7f, 0xe6, 0x60, 0x0
};


/*---------------------
 *  GLYPH DESCRIPTION
 *--------------------*/

static const lv_font_fmt_txt_glyph_dsc_t glyph_dsc[] = {
    {.bitmap_index = 0, .adv_w = 0, .box_w = 0, .box_h = 0, .ofs_x = 0, .ofs_y = 0} /* id = 0 reserved */,
    {.bitmap_index = 0, .adv_w = 256, .box_w = 16, .box_h = 2, .ofs_x = 0, .ofs_y = 5},
    {.bitmap_index = 10, .adv_w = 256, .box_w = 15, .box_h = 13, .ofs_x = 0, .ofs_y = -1},
    {.bitmap_index = 52, .adv_w = 256, .box_w = 16, .box_h = 11, .ofs_x = 0, .ofs_y = 0}
};

/*---------------------
 *  CHARACTER MAPPING
 *--------------------*/

static const uint16_t unicode_list_0[] = {
    0x0, 0x9, 0x8c
};

/*Collect the unicode lists and glyph_id offsets*/
static const lv_font_fmt_txt_cmap_t cmaps[] =
{
    {
        .range_start = 19968, .range_length = 141, .glyph_id_start = 1,
        .unicode_list = unicode_list_0, .glyph_id_ofs_list = NULL, .list_length = 3, .type = LV_FONT_FMT_TXT_CMAP_SPARSE_TINY
    }
};



/*--------------------
 *  ALL CUSTOM DATA
 *--------------------*/

#if LVGL_VERSION_MAJOR == 8
/*Store all the custom data of the font*/
static  lv_font_fmt_txt_glyph_cache_t cache;
#endif

#if LVGL_VERSION_MAJOR >= 8
static const lv_font_fmt_txt_dsc_t font_dsc = {
#else
static lv_font_fmt_txt_dsc_t font_dsc = {
#endif
    .glyph_bitmap = glyph_bitmap,
    .glyph_dsc = glyph_dsc,
    .cmaps = cmaps,
    .kern_dsc = NULL,
    .kern_scale = 0,
    .cmap_num = 1,
    .bpp = 4,
    .kern_classes = 0,
    .bitmap_format = 1,
#if LVGL_VERSION_MAJOR == 8
    .cache = &cache
#endif
};



/*-----------------
 *  PUBLIC FONT
 *----------------*/

/*Initialize a public general font descriptor*/
#if LVGL_VERSION_MAJOR >= 8
const lv_font_t test_font = {
#else
lv_font_t test_font = {
#endif
    .get_glyph_dsc = lv_font_get_glyph_dsc_fmt_txt,    /*Function pointer to get glyph's data*/
    .get_glyph_bitmap = lv_font_get_bitmap_fmt_txt,    /*Function pointer to get glyph's bitmap*/
    .line_height = 13,          /*The maximum line height required by the font*/
    .base_line = 1,             /*Baseline measured from the bottom of the line*/
#if !(LVGL_VERSION_MAJOR == 6 && LVGL_VERSION_MINOR == 0)
    .subpx = LV_FONT_SUBPX_NONE,
#endif
#if LV_VERSION_CHECK(7, 4, 0) || LVGL_VERSION_MAJOR >= 8
    .underline_position = -1,
    .underline_thickness = 1,
#endif
    .dsc = &font_dsc,          /*The custom font data. Will be accessed by `get_glyph_bitmap/dsc` */
#if LV_VERSION_CHECK(8, 2, 0) || LVGL_VERSION_MAJOR >= 9
    .fallback = NULL,
#endif
    .user_data = NULL,
};



#endif /*#if TEST_FONT*/

