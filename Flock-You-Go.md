# AIPI Lite Display Integration

This document records the hardware-specific display configuration used by
Flock-You Go. It is intentionally narrow: the canonical project overview,
build instructions, and runtime behavior live in [`README.md`](README.md).

## Validated panel configuration

The AIPI Lite `XY006PL01` contains a 128 x 128 ST7735-compatible SPI display.
The working configuration is:

```text
SPI host:       SPI2_HOST
SPI mode:       0
SPI clock:      20 MHz
LCD size:       128 x 128
RGB format:     RGB565 / COLMOD 0x05
MADCTL:         0x68
Inversion:      off / INVOFF 0x20
X offset:       0
Y offset:       0
Pixel transfer: byte-swapped RGB565 words
```

`MADCTL 0x68` selects the physical panel's working orientation and BGR color
order. Applying generic ST7735 tab offsets causes a static strip on the left
and a shorter strip at the upper-left edge. The driver must address controller
coordinates `0..127` directly.

## Pin map

```text
GPIO3   LCD backlight
GPIO7   LCD D/C
GPIO15  LCD CS
GPIO16  LCD SCLK
GPIO17  LCD MOSI
GPIO18  LCD reset
```

The backlight is active high. Keep it low during controller reset and
initialization, then enable it only after `DISPON` completes.

## Initialization sequence

1. Hold hardware reset low for 20 ms.
2. Release reset and wait 120 ms.
3. Send software reset `0x01`; wait 150 ms.
4. Send sleep-out `0x11`; wait 120 ms.
5. Set RGB565 with `COLMOD 0x3A, 0x05`.
6. Set orientation/color order with `MADCTL 0x36, 0x68`.
7. Disable inversion with `INVOFF 0x20`.
8. Enable the display with `DISPON 0x29`; wait 20 ms.
9. Turn on GPIO3 backlight.

## Runtime UI

`aipi_display.cpp` owns the SPI device and provides four calls:

```cpp
bool aipiDisplayBegin();
void aipiDisplayShowScan(uint8_t channel, int detections);
void aipiDisplayShowDetection(const char* oui, int8_t rssi,
                              uint8_t channel, uint16_t count);
void aipiDisplayTick(uint8_t channel, int detections);
```

The Wi-Fi receive callback never touches the display. `drainAlertQueue()` calls
`aipiDisplayShowDetection()` from normal loop context, preserving the detector's
bounded callback behavior. `aipiDisplayTick()` restores the scan view after
3.5 seconds and skips redraws when channel and count are unchanged.

## Hardware conflicts removed

The inherited firmware used GPIO3 as a piezo buzzer and GPIO21 as an active-low
user LED. Those are XIAO board assumptions, not AIPI Lite assignments. This
build disables both paths:

- sound remains off until the ES8311 codec and GPIO9 amplifier sequencing are
  implemented
- visual alert output uses the LCD until the GPIO46 WS2812 driver is integrated

This prevents the alert path from toggling the LCD backlight or an unrelated
AiPi pin.

## Physical acceptance test

After flashing:

1. The image fills all 128 x 128 pixels without static edge strips.
2. The header is red, status text is white/gold/green, and colors are not
   exchanged.
3. The scan channel changes among 1, 6, and 11.
4. A test detection replaces the scan page and returns after 3.5 seconds.
5. Detection rendering does not stall channel hopping or USB JSON output.

Validated display research and AiPi integration by **@GGDM**.
