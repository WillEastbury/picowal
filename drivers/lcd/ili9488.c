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

// ============================================================
// SPI primitives — CS stays low for entire command+data sequence
// ============================================================

// Send command byte (DC=0)
static void lcd_cmd(uint8_t cmd) {
    gpio_put(LCD_CS_PIN, 0);
    gpio_put(LCD_DC_PIN, 0);
    spi_write_blocking(LCD_SPI_PORT, &cmd, 1);
    gpio_put(LCD_CS_PIN, 1);
}

// Send command followed by N data bytes in one CS frame
static void lcd_cmd_data(uint8_t cmd, const uint8_t *data, uint8_t len) {
    gpio_put(LCD_CS_PIN, 0);
    gpio_put(LCD_DC_PIN, 0);
    spi_write_blocking(LCD_SPI_PORT, &cmd, 1);
    if (len > 0) {
        gpio_put(LCD_DC_PIN, 1);
        spi_write_blocking(LCD_SPI_PORT, data, len);
    }
    gpio_put(LCD_CS_PIN, 1);
}

// Shorthand for command + 1 data byte
static void lcd_cmd_1(uint8_t cmd, uint8_t d0) {
    lcd_cmd_data(cmd, &d0, 1);
}

// Write bulk pixel data — RGB565 (2 bytes/pixel) for ILI9488W variant
static void lcd_write_pixels(uint16_t color, uint32_t count) {
    uint8_t buf[2] = {color >> 8, color & 0xFF};
    gpio_put(LCD_CS_PIN, 0);
    gpio_put(LCD_DC_PIN, 1);
    for (uint32_t i = 0; i < count; i++) {
        spi_write_blocking(LCD_SPI_PORT, buf, 2);
    }
    gpio_put(LCD_CS_PIN, 1);
}

void lcd_set_backlight(uint8_t brightness) {
    pwm_set_gpio_level(LCD_BL_PIN, brightness * brightness);
}

void lcd_init(void) {
    // Init SPI at 20MHz (conservative for reliable init)
    spi_init(LCD_SPI_PORT, 20 * 1000 * 1000);
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
    sleep_ms(100);
    gpio_put(LCD_RST_PIN, 0);
    sleep_ms(100);
    gpio_put(LCD_RST_PIN, 1);
    sleep_ms(120);

    // ---- ILI9488W (Waveshare) init sequence ----
    // From PicoMite reference: ILI9488W variant, RGB565, 16-bit pixels

    static const uint8_t c2d[] = {0x33};
    lcd_cmd_data(0xC2, c2d, 1);  // Power Control 3

    static const uint8_t c5d[] = {0x00, 0x1E, 0x80};
    lcd_cmd_data(0xC5, c5d, 3);  // VCOM

    lcd_cmd_1(0xB1, 0xB0);  // Frame rate 70Hz

    lcd_cmd_1(0x36, 0x28);  // Memory Access: landscape, BGR

    static const uint8_t pgamma[] = {0x00,0x13,0x18,0x04,0x0F,0x06,0x3A,0x56,
                                     0x4D,0x03,0x0A,0x06,0x30,0x3E,0x0F};
    lcd_cmd_data(0xE0, pgamma, 15);

    static const uint8_t ngamma[] = {0x00,0x13,0x18,0x01,0x11,0x06,0x38,0x34,
                                     0x4D,0x06,0x0D,0x0B,0x31,0x37,0x0F};
    lcd_cmd_data(0xE1, ngamma, 15);

    lcd_cmd_1(0x3A, 0x55);  // Pixel Format: 16-bit RGB565

    lcd_cmd(0x11);  // Sleep Out
    sleep_ms(120);

    lcd_cmd(0x29);  // Display On
    sleep_ms(25);

    // Ramp SPI up to 40MHz for pixel data
    spi_set_baudrate(LCD_SPI_PORT, 40 * 1000 * 1000);
}

void lcd_set_window(uint16_t x0, uint16_t y0, uint16_t x1, uint16_t y1) {
    uint8_t xd[] = {x0 >> 8, x0 & 0xFF, x1 >> 8, x1 & 0xFF};
    lcd_cmd_data(0x2A, xd, 4);

    uint8_t yd[] = {y0 >> 8, y0 & 0xFF, y1 >> 8, y1 & 0xFF};
    lcd_cmd_data(0x2B, yd, 4);

    lcd_cmd(0x2C);
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
