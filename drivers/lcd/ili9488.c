#include "ili9488.h"
#include "pico/stdlib.h"
#include "hardware/spi.h"
#include "hardware/pwm.h"
#include <string.h>

// Basic 5x7 font (ASCII 32-90)
static const uint8_t font5x7[][5] = {
    {0x00,0x00,0x00,0x00,0x00}, // space
    {0x00,0x00,0x5F,0x00,0x00}, // !
    {0x00,0x07,0x00,0x07,0x00}, // "
    {0x14,0x7F,0x14,0x7F,0x14}, // #
    {0x24,0x2A,0x7F,0x2A,0x12}, // $
    {0x23,0x13,0x08,0x64,0x62}, // %
    {0x36,0x49,0x55,0x22,0x50}, // &
    {0x00,0x05,0x03,0x00,0x00}, // '
    {0x00,0x1C,0x22,0x41,0x00}, // (
    {0x00,0x41,0x22,0x1C,0x00}, // )
    {0x08,0x2A,0x1C,0x2A,0x08}, // *
    {0x08,0x08,0x3E,0x08,0x08}, // +
    {0x00,0x50,0x30,0x00,0x00}, // ,
    {0x08,0x08,0x08,0x08,0x08}, // -
    {0x00,0x60,0x60,0x00,0x00}, // .
    {0x20,0x10,0x08,0x04,0x02}, // /
    {0x3E,0x51,0x49,0x45,0x3E}, // 0
    {0x00,0x42,0x7F,0x40,0x00}, // 1
    {0x42,0x61,0x51,0x49,0x46}, // 2
    {0x21,0x41,0x45,0x4B,0x31}, // 3
    {0x18,0x14,0x12,0x7F,0x10}, // 4
    {0x27,0x45,0x45,0x45,0x39}, // 5
    {0x3C,0x4A,0x49,0x49,0x30}, // 6
    {0x01,0x71,0x09,0x05,0x03}, // 7
    {0x36,0x49,0x49,0x49,0x36}, // 8
    {0x06,0x49,0x49,0x29,0x1E}, // 9
    {0x00,0x36,0x36,0x00,0x00}, // :
    {0x00,0x56,0x36,0x00,0x00}, // ;
    {0x00,0x08,0x14,0x22,0x41}, // <
    {0x14,0x14,0x14,0x14,0x14}, // =
    {0x41,0x22,0x14,0x08,0x00}, // >
    {0x02,0x01,0x51,0x09,0x06}, // ?
    {0x32,0x49,0x79,0x41,0x3E}, // @
    {0x7E,0x11,0x11,0x11,0x7E}, // A
    {0x7F,0x49,0x49,0x49,0x36}, // B
    {0x3E,0x41,0x41,0x41,0x22}, // C
    {0x7F,0x41,0x41,0x22,0x1C}, // D
    {0x7F,0x49,0x49,0x49,0x41}, // E
    {0x7F,0x09,0x09,0x01,0x01}, // F
    {0x3E,0x41,0x41,0x51,0x32}, // G
    {0x7F,0x08,0x08,0x08,0x7F}, // H
    {0x00,0x41,0x7F,0x41,0x00}, // I
    {0x20,0x40,0x41,0x3F,0x01}, // J
    {0x7F,0x08,0x14,0x22,0x41}, // K
    {0x7F,0x40,0x40,0x40,0x40}, // L
    {0x7F,0x02,0x04,0x02,0x7F}, // M
    {0x7F,0x04,0x08,0x10,0x7F}, // N
    {0x3E,0x41,0x41,0x41,0x3E}, // O
    {0x7F,0x09,0x09,0x09,0x06}, // P
    {0x3E,0x41,0x51,0x21,0x5E}, // Q
    {0x7F,0x09,0x19,0x29,0x46}, // R
    {0x46,0x49,0x49,0x49,0x31}, // S
    {0x01,0x01,0x7F,0x01,0x01}, // T
    {0x3F,0x40,0x40,0x40,0x3F}, // U
    {0x1F,0x20,0x40,0x20,0x1F}, // V
    {0x7F,0x20,0x18,0x20,0x7F}, // W
    {0x63,0x14,0x08,0x14,0x63}, // X
    {0x03,0x04,0x78,0x04,0x03}, // Y
    {0x61,0x51,0x49,0x45,0x43}, // Z
};

static void lcd_write_cmd(uint8_t cmd) {
    gpio_put(LCD_CS_PIN, 0);
    gpio_put(LCD_DC_PIN, 0);
    spi_write_blocking(LCD_SPI_PORT, &cmd, 1);
    gpio_put(LCD_CS_PIN, 1);
}

// Waveshare 3.5" sends register data as 16-bit words (high byte first)
static void lcd_write_data(uint16_t data) {
    gpio_put(LCD_CS_PIN, 0);
    gpio_put(LCD_DC_PIN, 1);
    uint8_t buf[2] = {data >> 8, data & 0xFF};
    spi_write_blocking(LCD_SPI_PORT, buf, 2);
    gpio_put(LCD_CS_PIN, 1);
}

static void lcd_write_data16(uint16_t data) {
    uint8_t buf[2] = {data >> 8, data & 0xFF};
    gpio_put(LCD_CS_PIN, 0);
    gpio_put(LCD_DC_PIN, 1);
    spi_write_blocking(LCD_SPI_PORT, buf, 2);
    gpio_put(LCD_CS_PIN, 1);
}

// Write bulk pixel data — 3 bytes per pixel (RGB666)
// ILI9488 SPI always receives pixel data as 18-bit (3 bytes) regardless
// of register 0x3A setting. Convert RGB565 → RGB666 on the fly.
static void lcd_write_pixels(uint16_t color, uint32_t count) {
    uint8_t r = (color >> 11) & 0x1F;
    uint8_t g = (color >> 5) & 0x3F;
    uint8_t b = color & 0x1F;
    uint8_t buf[3] = {
        (r << 3) | (r >> 2),
        (g << 2) | (g >> 4),
        (b << 3) | (b >> 2),
    };
    gpio_put(LCD_CS_PIN, 0);
    gpio_put(LCD_DC_PIN, 1);
    for (uint32_t i = 0; i < count; i++) {
        spi_write_blocking(LCD_SPI_PORT, buf, 3);
    }
    gpio_put(LCD_CS_PIN, 1);
}

void lcd_set_backlight(uint8_t brightness) {
    pwm_set_gpio_level(LCD_BL_PIN, brightness * brightness);
}

void lcd_init(void) {
    // Init SPI at 40MHz
    spi_init(LCD_SPI_PORT, 40 * 1000 * 1000);
    gpio_set_function(LCD_CLK_PIN, GPIO_FUNC_SPI);
    gpio_set_function(LCD_MOSI_PIN, GPIO_FUNC_SPI);
    gpio_set_function(LCD_MISO_PIN, GPIO_FUNC_SPI);

    // Control pins
    gpio_init(LCD_CS_PIN);
    gpio_set_dir(LCD_CS_PIN, GPIO_OUT);
    gpio_put(LCD_CS_PIN, 1);

    gpio_init(LCD_DC_PIN);
    gpio_set_dir(LCD_DC_PIN, GPIO_OUT);

    gpio_init(LCD_RST_PIN);
    gpio_set_dir(LCD_RST_PIN, GPIO_OUT);

    // Backlight PWM
    gpio_set_function(LCD_BL_PIN, GPIO_FUNC_PWM);
    uint slice = pwm_gpio_to_slice_num(LCD_BL_PIN);
    pwm_set_wrap(slice, 65535);
    pwm_set_enabled(slice, true);
    lcd_set_backlight(255);

    // Hardware reset
    gpio_put(LCD_RST_PIN, 1);
    sleep_ms(500);
    gpio_put(LCD_RST_PIN, 0);
    sleep_ms(500);
    gpio_put(LCD_RST_PIN, 1);
    sleep_ms(500);

    // Waveshare Pico-ResTouch-LCD-3.5 init sequence (from reference driver)
    lcd_write_cmd(0x21);  // Display Inversion ON

    lcd_write_cmd(0xC2);  // Power Control 3
    lcd_write_data(0x33);

    lcd_write_cmd(0xC5);  // VCOM Control
    lcd_write_data(0x00);
    lcd_write_data(0x1E);
    lcd_write_data(0x80);

    lcd_write_cmd(0xB1);  // Frame Rate Control
    lcd_write_data(0xB0);

    lcd_write_cmd(0x36);  // Memory Access Control (landscape)
    lcd_write_data(0x28);

    lcd_write_cmd(0xE0);  // Positive Gamma Control
    lcd_write_data(0x00);
    lcd_write_data(0x13);
    lcd_write_data(0x18);
    lcd_write_data(0x04);
    lcd_write_data(0x0F);
    lcd_write_data(0x06);
    lcd_write_data(0x3A);
    lcd_write_data(0x56);
    lcd_write_data(0x4D);
    lcd_write_data(0x03);
    lcd_write_data(0x0A);
    lcd_write_data(0x06);
    lcd_write_data(0x30);
    lcd_write_data(0x3E);
    lcd_write_data(0x0F);

    lcd_write_cmd(0xE1);  // Negative Gamma Control
    lcd_write_data(0x00);
    lcd_write_data(0x13);
    lcd_write_data(0x18);
    lcd_write_data(0x01);
    lcd_write_data(0x11);
    lcd_write_data(0x06);
    lcd_write_data(0x38);
    lcd_write_data(0x34);
    lcd_write_data(0x4D);
    lcd_write_data(0x06);
    lcd_write_data(0x0D);
    lcd_write_data(0x0B);
    lcd_write_data(0x31);
    lcd_write_data(0x37);
    lcd_write_data(0x0F);

    lcd_write_cmd(0x3A);  // Pixel Format — 18-bit RGB666 (ILI9488 SPI requires 3 bytes/pixel)
    lcd_write_data(0x66);

    // Display Function Control — set scan direction
    lcd_write_cmd(0xB6);
    lcd_write_data(0x00);
    lcd_write_data(0x02);

    lcd_write_cmd(0x11);  // Sleep Out
    sleep_ms(120);

    lcd_write_cmd(0x29);  // Display On
    sleep_ms(200);
}

void lcd_set_window(uint16_t x0, uint16_t y0, uint16_t x1, uint16_t y1) {
    // Match Waveshare reference: each coordinate byte sent as 16-bit word
    lcd_write_cmd(0x2A);
    lcd_write_data(x0 >> 8);
    lcd_write_data(x0 & 0xFF);
    lcd_write_data(x1 >> 8);
    lcd_write_data(x1 & 0xFF);

    lcd_write_cmd(0x2B);
    lcd_write_data(y0 >> 8);
    lcd_write_data(y0 & 0xFF);
    lcd_write_data(y1 >> 8);
    lcd_write_data(y1 & 0xFF);

    lcd_write_cmd(0x2C);
}

void lcd_draw_pixel(uint16_t x, uint16_t y, uint16_t color) {
    lcd_set_window(x, y, x, y);
    lcd_write_pixels(color, 1);
}

void lcd_clear(uint16_t color) {
    lcd_set_window(0, 0, LCD_WIDTH - 1, LCD_HEIGHT - 1);
    lcd_write_pixels(color, (uint32_t)LCD_WIDTH * LCD_HEIGHT);
}

void lcd_fill_rect(uint16_t x, uint16_t y, uint16_t w, uint16_t h, uint16_t color) {
    lcd_set_window(x, y, x + w - 1, y + h - 1);
    lcd_write_pixels(color, (uint32_t)w * h);
}

void lcd_draw_char(uint16_t x, uint16_t y, char c, uint16_t fg, uint16_t bg, uint8_t size) {
    if (c < ' ' || c > 'Z') c = ' ';
    int idx = c - ' ';

    for (uint8_t col = 0; col < 5; col++) {
        uint8_t line = font5x7[idx][col];
        for (uint8_t row = 0; row < 7; row++) {
            uint16_t color = (line & (1 << row)) ? fg : bg;
            if (size == 1) {
                lcd_draw_pixel(x + col, y + row, color);
            } else {
                lcd_fill_rect(x + col * size, y + row * size, size, size, color);
            }
        }
    }
    // Gap between chars
    for (uint8_t row = 0; row < 7; row++) {
        if (size == 1) {
            lcd_draw_pixel(x + 5, y + row, bg);
        } else {
            lcd_fill_rect(x + 5 * size, y + row * size, size, size, bg);
        }
    }
}

void lcd_draw_string(uint16_t x, uint16_t y, const char *str, uint16_t fg, uint16_t bg, uint8_t size) {
    while (*str) {
        lcd_draw_char(x, y, *str, fg, bg, size);
        x += 6 * size;
        str++;
    }
}
