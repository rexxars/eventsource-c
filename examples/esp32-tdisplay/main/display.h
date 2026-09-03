/* Minimal text renderer for the LilyGO T-Display's ST7789 (135x240, used in
 * landscape as 240x135), on top of ESP-IDF's esp_lcd. No UI framework: a
 * static RGB565 framebuffer plus the public-domain font8x8. */
#ifndef TDISPLAY_DISPLAY_H_
#define TDISPLAY_DISPLAY_H_

#include <stdint.h>

#define DISP_W 240
#define DISP_H 135

/* RGB565 colors (native order; the renderer byte-swaps for the panel) */
#define COL_BLACK 0x0000
#define COL_WHITE 0xFFFF
#define COL_GREEN 0x07E0
#define COL_RED 0xF800
#define COL_YELLOW 0xFFE0
#define COL_CYAN 0x07FF
#define COL_GREY 0x8410

void display_init(void);
void display_clear(void);
/* Draws NUL-terminated ASCII at pixel (x, y), each glyph 8*scale px wide. */
void display_text(int x, int y, int scale, uint16_t color, const char *text);
/* Pushes the framebuffer to the panel. */
void display_flush(void);

#endif /* TDISPLAY_DISPLAY_H_ */
