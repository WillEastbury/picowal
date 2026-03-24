#ifndef XPT2046_H
#define XPT2046_H

#include <stdint.h>
#include <stdbool.h>

// Touch controller shares SPI1 with LCD, different CS
#define TP_SPI_PORT spi1
#define TP_CS_PIN   16
#define TP_IRQ_PIN  17

typedef struct {
    uint16_t x;
    uint16_t y;
    bool pressed;
} touch_point_t;

void touch_init(void);
touch_point_t touch_read(void);

#endif
