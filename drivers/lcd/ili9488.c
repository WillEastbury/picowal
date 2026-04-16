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
// SPI primitives
// ============================================================

static void lcd_cmd(uint8_t cmd) {
    gpio_put(LCD_CS_PIN, 0);
    gpio_put(LCD_DC_PIN, 0);
    spi_write_blocking(LCD_SPI_PORT, &cmd, 1);
    gpio_put(LCD_CS_PIN, 1);
}

static void lcd_data8(uint8_t data) {
    gpio_put(LCD_CS_PIN, 0);
    gpio_put(LCD_DC_PIN, 1);
    spi_write_blocking(LCD_SPI_PORT, &data, 1);
    gpio_put(LCD_CS_PIN, 1);
}

// Bulk pixel write: 3 bytes per pixel (RGB666), buffered
static void lcd_write_pixels(uint16_t color, uint32_t count) {
    uint8_t r = (color >> 11) & 0x1F;
    uint8_t g = (color >> 5) & 0x3F;
    uint8_t b = color & 0x1F;
    uint8_t r8 = (r << 3) | (r >> 2);
    uint8_t g8 = (g << 2) | (g >> 4);
    uint8_t b8 = (b << 3) | (b >> 2);

    // Fill a scanline buffer (160 pixels × 3 bytes = 480 bytes)
    uint8_t buf[480];
    uint16_t fill = (count < 160) ? count : 160;
    for (uint16_t i = 0; i < fill; i++) {
        buf[i * 3]     = r8;
        buf[i * 3 + 1] = g8;
        buf[i * 3 + 2] = b8;
    }

    gpio_put(LCD_CS_PIN, 0);
    gpio_put(LCD_DC_PIN, 1);
    while (count > 0) {
        uint32_t chunk = (count < 160) ? count : 160;
        spi_write_blocking(LCD_SPI_PORT, buf, chunk * 3);
        count -= chunk;
    }
    gpio_put(LCD_CS_PIN, 1);
}

void lcd_set_backlight(uint8_t brightness) {
    pwm_set_gpio_level(LCD_BL_PIN, brightness * brightness);
}

void lcd_init(void) {
    spi_init(LCD_SPI_PORT, 30 * 1000 * 1000);  // 30MHz
    gpio_set_function(LCD_CLK_PIN, GPIO_FUNC_SPI);
    gpio_set_function(LCD_MOSI_PIN, GPIO_FUNC_SPI);
    gpio_set_function(LCD_MISO_PIN, GPIO_FUNC_SPI);

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

    // ILI9488 init — single-byte register params, RGB666 pixel format
    lcd_cmd(0xE0);  // Positive Gamma
    lcd_data8(0x00); lcd_data8(0x03); lcd_data8(0x09); lcd_data8(0x08);
    lcd_data8(0x16); lcd_data8(0x0A); lcd_data8(0x3F); lcd_data8(0x78);
    lcd_data8(0x4C); lcd_data8(0x09); lcd_data8(0x0A); lcd_data8(0x08);
    lcd_data8(0x16); lcd_data8(0x1A); lcd_data8(0x0F);

    lcd_cmd(0xE1);  // Negative Gamma
    lcd_data8(0x00); lcd_data8(0x16); lcd_data8(0x19); lcd_data8(0x03);
    lcd_data8(0x0F); lcd_data8(0x05); lcd_data8(0x32); lcd_data8(0x45);
    lcd_data8(0x46); lcd_data8(0x04); lcd_data8(0x0E); lcd_data8(0x0D);
    lcd_data8(0x35); lcd_data8(0x37); lcd_data8(0x0F);

    lcd_cmd(0xC0); lcd_data8(0x17); lcd_data8(0x15);  // Power Control 1
    lcd_cmd(0xC1); lcd_data8(0x41);                    // Power Control 2
    lcd_cmd(0xC5); lcd_data8(0x00); lcd_data8(0x12); lcd_data8(0x80); // VCOM

    lcd_cmd(0x36); lcd_data8(0x28);  // Memory Access: landscape
    lcd_cmd(0x3A); lcd_data8(0x66);  // Pixel Format: 18-bit RGB666

    lcd_cmd(0xB0); lcd_data8(0x00);  // Interface Mode Control
    lcd_cmd(0xB1); lcd_data8(0xA0);  // Frame Rate: 60Hz
    lcd_cmd(0xB4); lcd_data8(0x02);  // Display Inversion Control
    lcd_cmd(0xB6); lcd_data8(0x02); lcd_data8(0x02); // Display Function Control

    lcd_cmd(0xE9); lcd_data8(0x00);  // Set Image Function
    lcd_cmd(0xF7); lcd_data8(0xA9); lcd_data8(0x51); lcd_data8(0x2C); lcd_data8(0x82); // Adjust Control 3

    lcd_cmd(0x11);  // Sleep Out
    sleep_ms(120);
    lcd_cmd(0x29);  // Display On
}

void lcd_set_window(uint16_t x0, uint16_t y0, uint16_t x1, uint16_t y1) {
    lcd_cmd(0x2A);
    lcd_data8(x0 >> 8); lcd_data8(x0 & 0xFF);
    lcd_data8(x1 >> 8); lcd_data8(x1 & 0xFF);

    lcd_cmd(0x2B);
    lcd_data8(y0 >> 8); lcd_data8(y0 & 0xFF);
    lcd_data8(y1 >> 8); lcd_data8(y1 & 0xFF);

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

// Render RGB666 pixels for a character into buf. Returns byte count written.
// For size=1: 5×7 pixels + 1-col gap = 6×7 = 42 pixels × 3 = 126 bytes
// For size>1: (6*size)×(7*size) pixels × 3 bytes
static uint16_t render_char_rgb(char c, uint16_t fg, uint16_t bg, uint8_t size,
                                uint8_t *buf, uint16_t buf_size) {
    if (c < ' ' || c > 'Z') c = ' ';
    int idx = c - ' ';

    uint8_t fg_r = (uint8_t)(((fg >> 11) & 0x1F) << 3);
    uint8_t fg_g = (uint8_t)(((fg >> 5) & 0x3F) << 2);
    uint8_t fg_b = (uint8_t)((fg & 0x1F) << 3);
    uint8_t bg_r = (uint8_t)(((bg >> 11) & 0x1F) << 3);
    uint8_t bg_g = (uint8_t)(((bg >> 5) & 0x3F) << 2);
    uint8_t bg_b = (uint8_t)((bg & 0x1F) << 3);

    uint16_t char_w = 6u * size;  // 5 cols + 1 gap
    uint16_t char_h = 7u * size;
    uint16_t total = (uint16_t)(char_w * char_h * 3u);
    if (total > buf_size) return 0;

    uint16_t pos = 0;
    for (uint8_t row = 0; row < 7; row++) {
        for (uint8_t sy = 0; sy < size; sy++) {
            for (uint8_t col = 0; col < 6; col++) {
                bool lit = false;
                if (col < 5) lit = (font5x7[idx][col] & (1 << row)) != 0;
                uint8_t r = lit ? fg_r : bg_r;
                uint8_t g = lit ? fg_g : bg_g;
                uint8_t b = lit ? fg_b : bg_b;
                for (uint8_t sx = 0; sx < size; sx++) {
                    buf[pos++] = r;
                    buf[pos++] = g;
                    buf[pos++] = b;
                }
            }
        }
    }
    return pos;
}

void lcd_draw_char(uint16_t x, uint16_t y, char c, uint16_t fg, uint16_t bg, uint8_t size) {
    if (size == 0 || size > 2) return;
    uint16_t w = 6u * size;
    uint16_t h = 7u * size;
    if (x + w > LCD_WIDTH || y + h > LCD_HEIGHT) return;

    uint8_t buf[504]; // size=2 max: 12×14×3 = 504
    uint16_t n = render_char_rgb(c, fg, bg, size, buf, sizeof(buf));
    if (n == 0) return;

    lcd_set_window(x, y, x + w - 1, y + h - 1);
    gpio_put(LCD_CS_PIN, 0);
    gpio_put(LCD_DC_PIN, 1);
    spi_write_blocking(LCD_SPI_PORT, buf, n);
    gpio_put(LCD_CS_PIN, 1);
}

void lcd_draw_string(uint16_t x, uint16_t y, const char *str, uint16_t fg, uint16_t bg, uint8_t size) {
    while (*str) {
        lcd_draw_char(x, y, *str, fg, bg, size);
        x += 6 * size;
        str++;
    }
}
