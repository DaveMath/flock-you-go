# Flock-You Go

Portable, passive 2.4 GHz awareness firmware for the X-Origin AIPI Lite
ESP32-S3. It watches locally for configured wireless signatures, records
detections in flash, presents live status on the built-in 128 x 128 display,
and can stream JSON over USB for the included dashboard.

**Free by @GGDM.** No subscription, cloud account, or bridge is required.

## Hardware

The target is the AIPI Lite `XY006PL01`, a roughly $20 ESP32-S3 device with
16 MB flash, 8 MB octal PSRAM, a color LCD, microphone, speaker, two buttons,
Wi-Fi, USB-C, a WS2812 status LED, and optional battery operation.

[Purchase the AIPI Lite from Amazon](https://www.amazon.com/stores/page/C88F1EA2-88AD-4BED-84F0-046A3DC763AA?ingress=2&lp_context_asin=B0FQNNVV36&visitId=356e09b1-3791-4637-9b0d-8cc265449ec1&ref_=ast_bln)

### Active pin map

| Function | GPIO | Status |
|---|---:|---|
| LCD backlight | 3 | Implemented |
| LCD D/C | 7 | Implemented |
| LCD CS | 15 | Implemented |
| LCD SCLK | 16 | Implemented |
| LCD MOSI | 17 | Implemented |
| LCD reset | 18 | Implemented |
| Right button | 42 | Alert volume: mute, 10%, 50%, 100% |
| Left button | 1 | Toggle 1/6/11 and all-channel scanning |
| WS2812 data | 46 | Planned |
| Speaker amplifier | 9 | Planned |

GPIO3 is the LCD backlight on this device. The older XIAO configuration used
it as a piezo output, which blanked or flickered the AiPi screen. Piezo and
GPIO21 LED feedback are disabled until the ES8311 speaker and GPIO46 WS2812
paths are integrated correctly.

## Display

The display driver in `aipi_display.cpp` uses settings validated on physical
AIPI Lite hardware:

| Setting | Value |
|---|---:|
| Controller | ST7735-compatible |
| Logical size | 128 x 128 |
| SPI host | `SPI2_HOST` |
| SPI mode | 0 |
| SPI clock | 20 MHz |
| Pixel format | RGB565, 16 bit |
| `MADCTL` | `0x68` |
| Orientation | XY swap plus X mirror |
| Color order | BGR bit enabled |
| Inversion | Off, command `0x20` |
| X/Y offsets | `0,0` |
| Pixel byte order | RGB565 words byte-swapped before SPI |

Those values fix the reversed colors and the static strips previously visible
along the left and upper edges.

The normal screen shows the current scan channel, detection count, and local
logging state. A detection temporarily replaces it with:

- matched OUI
- RSSI
- Wi-Fi channel
- observation count
- local-save confirmation

## Detection behavior

The radio remains in ESP32 promiscuous mode and passively sweeps US channels
1-11 in this priority order: `11, 6, 1, 10, 5, 2, 9, 4, 3, 8, 7`. The default
dwell is 700 ms. Press the left button to switch at runtime between the full
plan and the faster `1, 6, 11` plan. The active mode is written on-screen as
`ALL CHANNELS` or `SCAN 1-6-11`. The receive callback performs only bounded
matching and queues events;
display rendering, USB output, alerts, and SPIFFS writes happen from `loop()`.

The right button cycles alert volume through `MUTE`, `10%`, `50%`, and `100%`;
the selection is shown on-screen. The setting gates alert audio, but this AiPi
build remains silent until the ES8311 codec and GPIO9 amplifier driver are
enabled.

This firmware never calls `WiFi.scanNetworks()`, associates with an access
point, creates an access point, or transmits probe requests. References to
probe requests in the detector describe frames received passively from other
devices. The scanner does not generate those frames.

Observations are ranked before alerting:

- **High:** known-OUI wildcard probe signature observed on the air
- **Medium:** known transmitter OUI in a management frame, or configured SSID
- **Low:** address-only receiver/BSSID or data-frame OUI match

Low-confidence observations are logged and persisted for analysis, but do not
take over the screen or trigger physical alerts. Medium and high confidence
events do. Serial health output includes management/data frame totals,
per-channel traffic, queue drops, and channel-switch failures.

Each accepted observation is:

- added to the in-memory table
- persisted to SPIFFS using a CRC-protected temporary file and atomic promote
- emitted as newline-delimited JSON over USB CDC

Medium- and high-confidence observations are also displayed locally.

The firmware does not join a Wi-Fi network, create an access point, interfere
with traffic, or upload detection data.

## Build

Install PlatformIO, then run:

```bash
pio run
pio run -t upload --upload-port /dev/cu.usbmodem1301
pio device monitor --port /dev/cu.usbmodem1301 --baud 115200
```

The `aipi_lite` environment uses the compatible `seeed_xiao_esp32s3` Arduino
toolchain definition while overriding the physical flash size to 16 MB.
Flashing must identify the chip as `ESP32-S3`; an `esp32` esptool target is
incorrect.

Press `Ctrl-C` to leave the serial monitor.

## USB dashboard

The optional local dashboard ingests the firmware's JSON stream:

```bash
cd api
python3 -m pip install -r requirements.txt
python3 flockyou.py
```

Open `http://localhost:5000` and select the AiPi serial port. GPS enrichment is
performed by the local dashboard or phone browser, not by a cloud service.

## Configuration

The main compile-time settings are at the top of `main.cpp`:

| Define | Default | Purpose |
|---|---:|---|
| `CHANNEL_DWELL_MS` | 700 | Channel dwell time |
| `LEFT_BUTTON_PIN` | 1 | Runtime scan-mode control |
| `RIGHT_BUTTON_PIN` | 42 | Runtime alert-volume control |
| `RSSI_MIN` | -100 | Medium/high-confidence weak-frame cutoff |
| `RSSI_LOW_CONFIDENCE_MIN` | -95 | Address-only observation cutoff |
| `ALERT_COOLDOWN_MS` | 5000 | Per-MAC output rate limit |
| `CHECK_ADDR1` | 1 | Receiver-side matching |
| `CHECK_ADDR3` | 0 | Optional BSSID matching |
| `MAX_DETECTIONS` | 200 | Local table capacity |
| `AUTOSAVE_INTERVAL_MS` | 60000 | SPIFFS save interval |
| `USE_BUZZER` | 0 | Disabled pending ES8311 support |
| `USE_LED` | 0 | Disabled pending GPIO46 WS2812 support |

## Safety and provenance

Use the project only where passive RF observation is lawful. It is an
awareness and research tool, not a jammer, bypass device, or intrusion tool.

The detection engine and datasets entered this repository through earlier
Flock-You development history. That provenance remains available in Git history
and the source dataset notes without carrying unrelated project biographies in
the operating guide. AiPi hardware adaptation, validated display work, and this
firmware integration are maintained by **@GGDM**.

## Repository

[github.com/DaveMath/Flock-You-Go](https://github.com/DaveMath/Flock-You-Go)

Detailed AiPi display integration notes are in
[`Flock-You-Go.md`](Flock-You-Go.md).
