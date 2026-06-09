#include "xpt2046.h"
#include "ili9488.h"
#include "pico/stdlib.h"
#include "hardware/spi.h"

#define CMD_X 0xD0
#define CMD_Y 0x90

void touch_init(void) {
    gpio_init(TP_CS_PIN);
    gpio_set_dir(TP_CS_PIN, GPIO_OUT);
    gpio_put(TP_CS_PIN, 1);

    gpio_init(TP_IRQ_PIN);
    gpio_set_dir(TP_IRQ_PIN, GPIO_IN);
    gpio_pull_up(TP_IRQ_PIN);
}

static uint16_t touch_read_channel(uint8_t cmd) {
    uint8_t tx[3] = {cmd, 0x00, 0x00};
    uint8_t rx[3] = {0};

    gpio_put(TP_CS_PIN, 0);
    spi_set_baudrate(TP_SPI_PORT, 1 * 1000 * 1000);
    spi_write_read_blocking(TP_SPI_PORT, tx, rx, 3);
    spi_set_baudrate(TP_SPI_PORT, 40 * 1000 * 1000);
    gpio_put(TP_CS_PIN, 1);

    return ((rx[1] << 8) | rx[2]) >> 3;
}

touch_point_t touch_read(void) {
    touch_point_t pt = {0, 0, false};

    if (gpio_get(TP_IRQ_PIN) == 0) {
        uint32_t sum_x = 0, sum_y = 0;
        const int samples = 8;

        for (int i = 0; i < samples; i++) {
            sum_x += touch_read_channel(CMD_X);
            sum_y += touch_read_channel(CMD_Y);
        }

        uint16_t raw_x = sum_x / samples;
        uint16_t raw_y = sum_y / samples;

        // Map to screen coordinates (landscape calibration)
        if (raw_x > 200 && raw_y > 200) {
            pt.x = (uint16_t)(((uint32_t)(raw_x - 200) * LCD_WIDTH) / 3700);
            pt.y = (uint16_t)(((uint32_t)(raw_y - 200) * LCD_HEIGHT) / 3700);
            if (pt.x >= LCD_WIDTH) pt.x = LCD_WIDTH - 1;
            if (pt.y >= LCD_HEIGHT) pt.y = LCD_HEIGHT - 1;
            pt.pressed = true;
        }
    }
    return pt;
}
