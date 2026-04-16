add_library(lcd_driver INTERFACE)
target_sources(lcd_driver INTERFACE
    ${CMAKE_CURRENT_LIST_DIR}/lcd/ili9488.c
)
target_include_directories(lcd_driver INTERFACE
    ${CMAKE_CURRENT_LIST_DIR}/lcd
)

add_library(touch_driver INTERFACE)
target_sources(touch_driver INTERFACE
    ${CMAKE_CURRENT_LIST_DIR}/touch/xpt2046.c
)
target_include_directories(touch_driver INTERFACE
    ${CMAKE_CURRENT_LIST_DIR}/touch
)

add_library(sd_driver INTERFACE)
target_sources(sd_driver INTERFACE
    ${CMAKE_CURRENT_LIST_DIR}/sd/sd_card.c
)
target_include_directories(sd_driver INTERFACE
    ${CMAKE_CURRENT_LIST_DIR}/sd
)
