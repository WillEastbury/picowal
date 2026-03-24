@echo off
REM Flash pico2w_lcd.uf2 to Pico 2W
REM Hold BOOTSEL button, plug in USB, then run this script

set UF2=build\pico2w_lcd.uf2

REM Find the Pico drive (usually shows as RPI-RP2)
for %%d in (D E F G H I J K L) do (
    if exist "%%d:\INFO_UF2.TXT" (
        echo Found Pico at %%d:
        copy %UF2% %%d:\
        echo Flashed! Pico will reboot automatically.
        goto :done
    )
)

echo ERROR: Pico not found in BOOTSEL mode.
echo Hold BOOTSEL button while plugging in USB, then retry.

:done
