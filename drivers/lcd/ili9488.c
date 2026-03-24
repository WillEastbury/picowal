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
// SPI primitives — exact match to Waveshare official demo
// ============================================================

static void spi_write_byte(uint8_t val) {
    uint8_t rx;
    spi_write_read_blocking(LCD_SPI_PORT, &val, &rx, 1);
}

static void lcd_cmd(uint8_t cmd) {
    gpio_put(LCD_DC_PIN, 0);
    gpio_put(LCD_CS_PIN, 0);
    spi_write_byte(cmd);
    gpio_put(LCD_CS_PIN, 1);
}

// Data write: sends 2 bytes (high, low) per Waveshare 3.5" protocol
static void lcd_data(uint16_t data) {
    gpio_put(LCD_DC_PIN, 1);
    gpio_put(LCD_CS_PIN, 0);
    spi_write_byte(data >> 8);
    spi_write_byte(data & 0xFF);
    gpio_put(LCD_CS_PIN, 1);
}

// Bulk pixel write: 2 bytes per pixel, CS held low, buffered
static void lcd_write_pixels(uint16_t color, uint32_t count) {
    // Fill a scanline buffer
    uint8_t buf[480 * 2];
    uint16_t fill = (count < 480) ? count : 480;
    for (uint16_t i = 0; i < fill; i++) {
        buf[i * 2]     = color >> 8;
        buf[i * 2 + 1] = color & 0xFF;
    }

    gpio_put(LCD_DC_PIN, 1);
    gpio_put(LCD_CS_PIN, 0);
    while (count > 0) {
        uint32_t chunk = (count < 480) ? count : 480;
        spi_write_blocking(LCD_SPI_PORT, buf, chunk * 2);
        count -= chunk;
    }
    gpio_put(LCD_CS_PIN, 1);
}

void lcd_set_backlight(uint8_t brightness) {
    pwm_set_gpio_level(LCD_BL_PIN, brightness * brightness);
}

void lcd_init(void) {
    // Waveshare demo uses 4MHz SPI
    spi_init(LCD_SPI_PORT, 4000000);
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

    // Hardware reset (500ms per Waveshare demo)
    gpio_put(LCD_RST_PIN, 1);
    sleep_ms(500);
    gpio_put(LCD_RST_PIN, 0);
    sleep_ms(500);
    gpio_put(LCD_RST_PIN, 1);
    sleep_ms(500);

    // ---- Waveshare official 3.5" init sequence (LCD_InitReg) ----
    lcd_cmd(0x21);     // Display Inversion ON
    lcd_cmd(0xC2);     lcd_data(0x33);
    lcd_cmd(0xC5);     lcd_data(0x00); lcd_data(0x1E); lcd_data(0x80);
    lcd_cmd(0xB1);     lcd_data(0xB0);
    lcd_cmd(0x36);     lcd_data(0x28);
    lcd_cmd(0xE0);     // Positive Gamma
    lcd_data(0x00); lcd_data(0x13); lcd_data(0x18); lcd_data(0x04);
    lcd_data(0x0F); lcd_data(0x06); lcd_data(0x3A); lcd_data(0x56);
    lcd_data(0x4D); lcd_data(0x03); lcd_data(0x0A); lcd_data(0x06);
    lcd_data(0x30); lcd_data(0x3E); lcd_data(0x0F);
    lcd_cmd(0xE1);     // Negative Gamma
    lcd_data(0x00); lcd_data(0x13); lcd_data(0x18); lcd_data(0x01);
    lcd_data(0x11); lcd_data(0x06); lcd_data(0x38); lcd_data(0x34);
    lcd_data(0x4D); lcd_data(0x06); lcd_data(0x0D); lcd_data(0x0B);
    lcd_data(0x31); lcd_data(0x37); lcd_data(0x0F);
    lcd_cmd(0x3A);     lcd_data(0x55);  // 16-bit RGB565
    lcd_cmd(0x11);     // Sleep Out
    sleep_ms(120);
    lcd_cmd(0x29);     // Display On

    // ---- LCD_SetGramScanWay (landscape U2D_R2L) ----
    lcd_cmd(0xB6);     lcd_data(0x00); lcd_data(0x02);
    lcd_cmd(0x36);     lcd_data(0x28);
}

void lcd_set_window(uint16_t x0, uint16_t y0, uint16_t x1, uint16_t y1) {
    lcd_cmd(0x2A);
    lcd_data(x0 >> 8); lcd_data(x0 & 0xFF);
    lcd_data(x1 >> 8); lcd_data(x1 & 0xFF);

    lcd_cmd(0x2B);
    lcd_data(y0 >> 8); lcd_data(y0 & 0xFF);
    lcd_data(y1 >> 8); lcd_data(y1 & 0xFF);

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
