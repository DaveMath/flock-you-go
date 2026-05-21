# Flock-You Go

Flock-You Go is a portable ESP32-S3 build of the open-source Flock-You surveillance-awareness firmware, adapted for the AIPI-Lite AI Robot hardware platform.

It turns a small desktop AI companion device into a portable, self-powered, pocketable, screen-equipped, audio-capable privacy awareness tool for detecting nearby Flock Safety-style wireless signatures while staying passive and local-first.

> **Status:** concept / fork README draft  
> **Upstream inspiration:** `colonelpanichacks/flock-you`  
> **Target device:** AIPI-Lite AI Robot / X-Origin AI Pi Lite ESP32-S3 platform

---

## What It Is

Flock-You Go is a fork of the Flock-You ESP32 firmware concept, repackaged for the AIPI-Lite AI Robot form factor.

The goal is simple:

- Run on an inexpensive ESP32-S3-based handheld/desktop device.
- Passively listen for known Flock-style wireless identifiers.
- Show detections on the built-in display.
- Use the onboard speaker for local alerts.
- Provide a friendly device experience instead of a bare dev board.

This is not a jammer, spoofer, scanner-for-hire, or bypass tool. It is intended as a passive RF awareness device.

---

## Buy the Target Hardware

Flock-You Go is designed around the **AIPI-Lite AI Robot**.

Buy the device here:

https://www.amazon.com/dp/B0FQNHRFSL?tag=gadgetguydavemat-20&th=1

Note: that URL contains an affiliate tag. If you use it, the software builder gets a tiny commission.

---

## Why the AIPI-Lite?

The AIPI-Lite is a strong fit for this fork because it packages the parts normally scattered across a breadboard or dev kit into one small enclosure.

Useful hardware traits:

- ESP32-S3 platform
- Built-in display
- Built-in microphone
- Built-in speaker
- Wi-Fi
- USB-C power/programming
- Compact enclosure
- Exposed/tinkerable hardware
- Optional magnetic battery accessory

For Flock-You Go, the most important pieces are the ESP32-S3 radio, display, speaker, and portable form factor.

---

## What Flock-You Go Does

Flock-You Go watches for wireless signatures associated with Flock Safety-style infrastructure using passive Wi-Fi detection methods inherited from the Flock-You project.

Planned device behavior:

1. Boot directly into detection mode.
2. Hop common 2.4 GHz Wi-Fi channels.
3. Listen in promiscuous mode.
4. Compare observed MAC/OUI patterns against the detection table.
5. Show a detection summary on the AIPI-Lite display.
6. Play a short local audio alert.
7. Store detections locally.
8. Optionally export detections over USB serial as JSON.

---

## Key Features

### Passive Detection

The device listens only. It does not need to join a Wi-Fi network, create an access point, or transmit probe traffic for the detection loop.

### Built-In Screen UI

The AIPI-Lite screen can show:

- Current scan channel
- Detection count
- Last detection timestamp
- Matched OUI/signature
- Signal strength, if available
- Local logging status
- USB export status

### Local Audio Alerts

The built-in speaker can replace the piezo buzzer used in simpler ESP32 builds.

Possible alert modes:

- Silent
- Short beep
- Double chirp
- Spoken alert, if supported by the firmware branch
- Volume-adjusted alert profile

### Local-First Logging

Detections should stay on-device unless exported.

Suggested log fields:

```json
{
  "ts_ms": 123456789,
  "channel": 6,
  "rssi": -71,
  "match_type": "oui",
  "mac_prefix": "xx:xx:xx",
  "source": "wifi_promiscuous",
  "device": "aipi-lite",
  "firmware": "flock-you-go"
}
```

### USB Serial Export

When plugged into a laptop, Flock-You Go can emit newline-delimited JSON for dashboards, scripts, or later analysis.

---

## What This Is Not

Flock-You Go is **not**:

- A camera hacking tool
- A Flock Safety bypass tool
- A traffic interference device
- A Wi-Fi jammer
- A BLE spoofer
- A tool for impersonating infrastructure
- A cloud tracking product

It should remain passive, transparent, and user-controlled.

---

## Intended Users

Flock-You Go is for:

- Privacy researchers
- Civic technology groups
- RF hobbyists
- Journalists documenting surveillance infrastructure
- Makers who want a polished ESP32-S3 field device
- People who want local awareness without a cloud dependency

---

## Proposed Firmware Architecture

```text
flock-you-go/
├── firmware/
│   ├── src/
│   │   ├── main.cpp
│   │   ├── detector_wifi.cpp
│   │   ├── detection_table.cpp
│   │   ├── display_ui.cpp
│   │   ├── audio_alerts.cpp
│   │   ├── storage.cpp
│   │   └── usb_export.cpp
│   ├── include/
│   ├── platformio.ini
│   └── partitions.csv
├── tools/
│   ├── serial_logger.py
│   └── convert_logs.py
├── docs/
│   ├── hardware.md
│   ├── flashing.md
│   └── legal-and-ethics.md
└── README.md
```

---

## Display UI Concept

```text
┌────────────────────┐
│ Flock-You Go        │
│ Passive RF Watch    │
├────────────────────┤
│ Scan: CH 1 / 6 / 11 │
│ Hits Today: 003     │
│ Last: 14:22:31      │
│ RSSI: -68 dBm       │
│ Log: ON             │
└────────────────────┘
```

---

## Detection Modes

### Mode 1: Quiet Watch

Silent display-only scan mode.

Best for desk use or long battery runs.

### Mode 2: Field Alert

Display plus short audio chirp on detection.

Best for walking, driving, or field surveying.

### Mode 3: USB Logger

Serial JSON export for a connected laptop.

Best for testing, dashboards, or research logging.

---

## Flashing Overview

Final flashing steps will depend on the exact AIPI-Lite board revision and exposed USB/serial behavior.

Expected flow:

1. Install PlatformIO or ESP-IDF tooling.
2. Connect the AIPI-Lite over USB-C.
3. Put the device into bootloader mode if required.
4. Build the firmware for ESP32-S3.
5. Flash the firmware.
6. Open a serial monitor.
7. Confirm boot logs and scan status.
8. Confirm display output.
9. Confirm audio alert output.
10. Run a passive field test.

Example placeholder command:

```bash
pio run -t upload
pio device monitor
```

---

## Configuration

Suggested compile-time settings:

```cpp
#define FYG_DEVICE_NAME          "Flock-You Go"
#define FYG_SCAN_CHANNELS        {1, 6, 11}
#define FYG_CHANNEL_DWELL_MS     350
#define FYG_AUDIO_ALERTS         1
#define FYG_DISPLAY_ENABLED      1
#define FYG_USB_JSON_ENABLED     1
#define FYG_LOCAL_LOGGING        1
```

Suggested runtime settings:

```json
{
  "audio": "chirp",
  "brightness": 80,
  "log_enabled": true,
  "usb_json": true,
  "scan_channels": [1, 6, 11],
  "privacy_mode": true
}
```

---

## Privacy Position

Flock-You Go should be designed around three rules:

1. **Listen only.**
2. **Store locally.**
3. **Export only when the owner chooses.**

No cloud account should be required for detection. No detection logs should be uploaded by default.

---

## Legal and Ethics Note

Wireless monitoring rules vary by jurisdiction. Use this only where passive RF observation and logging are lawful. Do not use this project to interfere with networks, impersonate devices, evade law enforcement systems, or harass individuals.

This project is about awareness and research, not disruption.

---

## Roadmap

### Alpha

- Confirm AIPI-Lite board pinout
- Confirm display controller
- Confirm speaker/audio path
- Build minimal ESP32-S3 scan loop
- Port upstream detection table
- Print detections over USB serial

### Beta

- Add display UI
- Add audio alert profiles
- Add local log storage
- Add settings screen
- Add firmware version screen
- Add USB JSON export compatibility

### Release Candidate

- Battery testing
- False-positive review
- Field logging format freeze
- Flashing guide
- Hardware teardown notes
- Safety/legal page
- Prebuilt binaries, if licensing allows

---

## Credits

Flock-You Go is a fork/adaptation concept based on the open-source Flock-You ESP32 work.

Modifications of Flock-You from Dave Mathews x.com/ggdm 
Linkedin.com/in/DaveMathews
Github.com/DaveMath

Upstream project:

https://github.com/colonelpanichacks/flock-you

Related public references:

- AIPI-Lite product page: https://aipi.com/products/aipi-lite
- AIPI-Lite Amazon listing: https://www.amazon.com/dp/B0FQNHRFSL?tag=gadgetguydavemat-20&th=1

---

## License

Use the upstream Flock-You license if this fork directly incorporates upstream code.

If the upstream license is missing or unclear, do not publish copied source code until licensing is resolved. Publish only original wrapper code, documentation, and hardware notes until the license is confirmed.

---

## One-Line Pitch

**Flock-You Go turns a cheap ESP32-S3 AI companion into a pocketable, passive, local-first surveillance-awareness device.**
