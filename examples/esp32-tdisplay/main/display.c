#include "display.h"

#include <string.h>

#include "driver/gpio.h"
#include "driver/spi_master.h"
#include "esp_lcd_panel_io.h"
#include "esp_lcd_panel_ops.h"
#include "esp_lcd_panel_vendor.h"
#include "font8x8_basic.h"

/* LilyGO T-Display (classic ESP32) wiring */
#define PIN_SCLK 18
#define PIN_MOSI 19
#define PIN_CS 5
#define PIN_DC 16
#define PIN_RST 23
#define PIN_BL 4 /* backlight, active high */

static esp_lcd_panel_handle_t s_panel;
/* Full frame, RGB565 with bytes pre-swapped for the panel: 240*135*2 =
 * ~63 KiB of .bss. Rendering into a full frame keeps the code trivial. */
static uint16_t s_fb[DISP_W * DISP_H];

void display_init(void) {
  spi_bus_config_t buscfg = {
      .sclk_io_num = PIN_SCLK,
      .mosi_io_num = PIN_MOSI,
      .miso_io_num = -1,
      .quadwp_io_num = -1,
      .quadhd_io_num = -1,
      .max_transfer_sz = DISP_W * DISP_H * 2 + 16,
  };
  ESP_ERROR_CHECK(spi_bus_initialize(SPI3_HOST, &buscfg, SPI_DMA_CH_AUTO));

  esp_lcd_panel_io_handle_t io = NULL;
  esp_lcd_panel_io_spi_config_t io_config = {
      .dc_gpio_num = PIN_DC,
      .cs_gpio_num = PIN_CS,
      .pclk_hz = 26 * 1000 * 1000,
      .lcd_cmd_bits = 8,
      .lcd_param_bits = 8,
      .spi_mode = 0,
      .trans_queue_depth = 10,
  };
  ESP_ERROR_CHECK(
      esp_lcd_new_panel_io_spi((esp_lcd_spi_bus_handle_t)SPI3_HOST, &io_config, &io));

  esp_lcd_panel_dev_config_t panel_config = {
      .reset_gpio_num = PIN_RST,
      .rgb_ele_order = LCD_RGB_ELEMENT_ORDER_RGB,
      .bits_per_pixel = 16,
  };
  ESP_ERROR_CHECK(esp_lcd_new_panel_st7789(io, &panel_config, &s_panel));
  ESP_ERROR_CHECK(esp_lcd_panel_reset(s_panel));
  ESP_ERROR_CHECK(esp_lcd_panel_init(s_panel));
  /* The T-Display panel needs inversion, and its 135x240 glass sits offset
   * inside the controller's 240x320 memory; landscape = swap + mirror. */
  ESP_ERROR_CHECK(esp_lcd_panel_invert_color(s_panel, true));
  ESP_ERROR_CHECK(esp_lcd_panel_swap_xy(s_panel, true));
  ESP_ERROR_CHECK(esp_lcd_panel_mirror(s_panel, false, true));
  ESP_ERROR_CHECK(esp_lcd_panel_set_gap(s_panel, 40, 53));
  ESP_ERROR_CHECK(esp_lcd_panel_disp_on_off(s_panel, true));

  gpio_config_t bl = {.pin_bit_mask = 1ULL << PIN_BL, .mode = GPIO_MODE_OUTPUT};
  ESP_ERROR_CHECK(gpio_config(&bl));
  gpio_set_level(PIN_BL, 1);

  display_clear();
  display_flush();
}

void display_clear(void) { memset(s_fb, 0, sizeof s_fb); }

static void put_px(int x, int y, uint16_t swapped) {
  if (x < 0 || x >= DISP_W || y < 0 || y >= DISP_H) return;
  s_fb[y * DISP_W + x] = swapped;
}

void display_text(int x, int y, int scale, uint16_t color, const char *text) {
  uint16_t swapped = (uint16_t)((color >> 8) | (color << 8));
  for (const char *p = text; *p; p++) {
    unsigned char c = (unsigned char)*p;
    if (c > 127) c = '?';
    for (int row = 0; row < 8; row++) {
      unsigned char bits = (unsigned char)font8x8_basic[c][row];
      for (int col = 0; col < 8; col++) {
        if (!(bits & (1u << col))) continue; /* bit 0 = leftmost pixel */
        for (int sy = 0; sy < scale; sy++) {
          for (int sx = 0; sx < scale; sx++) {
            put_px(x + col * scale + sx, y + row * scale + sy, swapped);
          }
        }
      }
    }
    x += 8 * scale;
  }
}

void display_flush(void) {
  ESP_ERROR_CHECK(
      esp_lcd_panel_draw_bitmap(s_panel, 0, 0, DISP_W, DISP_H, s_fb));
}
