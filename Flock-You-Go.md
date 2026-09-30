# AIPI Lite Display Integration

This document records the hardware-specific display and passive Wi-Fi receiver
configuration used by Flock-You Go. The canonical project overview and build
instructions live in [`README.md`](README.md).

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

Low-confidence address observations remain available in the USB JSON and
SPIFFS session record, but do not replace the scan view. Only medium- and
high-confidence observations produce a local alert screen.

## Passive Wi-Fi receiver

The ESP32-S3 radio runs only as a promiscuous receiver. It does not associate
with a network, create an access point, initiate an active network scan, or
transmit probe requests. The firmware contains no calls to
`WiFi.scanNetworks()`, `esp_wifi_scan_start()`, `esp_wifi_connect()`,
`WiFi.begin()`, `WiFi.softAP()`, or `esp_wifi_80211_tx()`.

Probe-request references in the detector are receive-side parsing. The AiPi
observes probe requests transmitted by other radios and uses their information
elements as evidence; it never generates those frames itself.

The default US channel plan is:

```text
11, 6, 1, 10, 5, 2, 9, 4, 3, 8, 7
```

The dwell time is 700 ms, giving a complete 1-11 sweep in approximately 7.7
seconds. Channels 12 and 13 are excluded from the default plan. A compile-time
fast mode retains the `1, 6, 11` sequence for environments where cycle time is
more important than complete channel coverage.

## Confidence model

Observations are classified before local alerting:

| Confidence | Evidence | Device behavior |
|---|---|---|
| High | Known transmitter OUI plus received wildcard probe signature | Display and alert |
| Medium | Known transmitter OUI in a management frame, or configured SSID | Display and alert |
| Low | Receiver/BSSID address match or known OUI in a data frame | Log and persist only |

The general receive floor is `-100 dBm`. Low-confidence address-only evidence
uses the stricter `-95 dBm` floor. If later traffic promotes an existing MAC
from low confidence to medium or high confidence, its stored method and
confidence are upgraded and the local alert path is activated.

## Receiver health and initialization

The receive queue contains 64 entries. The 30-second serial heartbeat reports:

- all received management and data frame counts
- received frames on the current channel
- queue drops
- channel-switch failures
- current channel, mode, and observation count

Every ESP-IDF Wi-Fi initialization, channel, filter, callback, and promiscuous
mode transition is checked. Wi-Fi NVS and CSI are disabled, receive buffers are
bounded, and AMPDU/AMSDU aggregation is disabled for the passive detector. A
startup failure leaves the scanner stopped and prints the ESP-IDF error instead
of presenting a false scanning state.

USB detection JSON and CRC-protected SPIFFS session records include the
`confidence` field alongside the detection method, MAC, OUI, RSSI, channel,
frequency, and optional SSID.

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
3. The scan channel follows the prioritized 1-11 sequence at 700 ms dwell.
4. Serial health output shows increasing receive counters with zero queue drops
   and channel-switch failures under normal operation.
5. A medium/high test observation replaces the scan page and returns after 3.5
   seconds; a low-confidence observation is logged without replacing the page.
6. Detection rendering does not stall channel hopping or USB JSON output.

## Verified build

PlatformIO environment `aipi_lite` builds successfully for `ESP32-S3` using
the `seeed_xiao_esp32s3` toolchain definition and the repository's 16 MB flash
override. The passive-scanner revision produced:

```text
RAM:   95,292 / 327,680 bytes (29.1%)
Flash: 694,385 / 6,291,456 bytes (11.0%)
Image: ESP32-S3 firmware.bin
```

Validated display research and AiPi integration by **@GGDM**.
