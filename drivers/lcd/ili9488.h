#ifndef ILI9488_H
#define ILI9488_H

#include <stdint.h>
#include <stdbool.h>

// Waveshare Pico-ResTouch-LCD-3.5 pin definitions
#define LCD_SPI_PORT spi1
#define LCD_CLK_PIN  10
#define LCD_MOSI_PIN 11
#define LCD_MISO_PIN 12
#define LCD_CS_PIN   9
#define LCD_DC_PIN   8
#define LCD_RST_PIN  15
#define LCD_BL_PIN   13

#define LCD_WIDTH    480
#define LCD_HEIGHT   320

// Colors (RGB565)
#define COLOR_BLACK   0x0000
#define COLOR_WHITE   0xFFFF
#define COLOR_RED     0xF800
#define COLOR_GREEN   0x07E0
#define COLOR_BLUE    0x001F
#define COLOR_YELLOW  0xFFE0
#define COLOR_CYAN    0x07FF
#define COLOR_MAGENTA 0xF81F

void lcd_init(void);
void lcd_set_backlight(uint8_t brightness);
void lcd_clear(uint16_t color);
void lcd_set_window(uint16_t x0, uint16_t y0, uint16_t x1, uint16_t y1);
void lcd_draw_pixel(uint16_t x, uint16_t y, uint16_t color);
void lcd_fill_rect(uint16_t x, uint16_t y, uint16_t w, uint16_t h, uint16_t color);
void lcd_draw_char(uint16_t x, uint16_t y, char c, uint16_t fg, uint16_t bg, uint8_t size);
void lcd_draw_string(uint16_t x, uint16_t y, const char *str, uint16_t fg, uint16_t bg, uint8_t size);

#endif
