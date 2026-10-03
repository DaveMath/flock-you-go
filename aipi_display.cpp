#include "aipi_display.h"

#include <Arduino.h>
#include <algorithm>
#include <array>
#include <ctype.h>
#include <stdio.h>
#include <string.h>

#include "driver/gpio.h"
#include "driver/spi_master.h"

namespace {

constexpr gpio_num_t kBacklight = GPIO_NUM_3;
constexpr gpio_num_t kLcdDc = GPIO_NUM_7;
constexpr gpio_num_t kLcdCs = GPIO_NUM_15;
constexpr gpio_num_t kLcdSclk = GPIO_NUM_16;
constexpr gpio_num_t kLcdMosi = GPIO_NUM_17;
constexpr gpio_num_t kLcdReset = GPIO_NUM_18;
constexpr int kWidth = 128;
constexpr int kHeight = 128;
constexpr uint8_t kMadctl = 0x68;

constexpr uint16_t kBlack = 0x0000;
constexpr uint16_t kWhite = 0xffff;
constexpr uint16_t kRed = 0xf800;
constexpr uint16_t kGreen = 0x07e0;
constexpr uint16_t kGold = 0xfea0;
constexpr uint16_t kGray = 0x4208;

struct Glyph {
  char character;
  uint8_t columns[5];
};

constexpr Glyph kGlyphs[] = {
  {' ', {0x00,0x00,0x00,0x00,0x00}}, {'-', {0x08,0x08,0x08,0x08,0x08}},
  {'.', {0x00,0x60,0x60,0x00,0x00}}, {':', {0x00,0x36,0x36,0x00,0x00}},
  {'%', {0x62,0x64,0x08,0x13,0x23}},
  {'/', {0x20,0x10,0x08,0x04,0x02}}, {'?', {0x02,0x01,0x51,0x09,0x06}},
  {'0', {0x3e,0x51,0x49,0x45,0x3e}}, {'1', {0x00,0x42,0x7f,0x40,0x00}},
  {'2', {0x42,0x61,0x51,0x49,0x46}}, {'3', {0x21,0x41,0x45,0x4b,0x31}},
  {'4', {0x18,0x14,0x12,0x7f,0x10}}, {'5', {0x27,0x45,0x45,0x45,0x39}},
  {'6', {0x3c,0x4a,0x49,0x49,0x30}}, {'7', {0x01,0x71,0x09,0x05,0x03}},
  {'8', {0x36,0x49,0x49,0x49,0x36}}, {'9', {0x06,0x49,0x49,0x29,0x1e}},
  {'A', {0x7e,0x11,0x11,0x11,0x7e}}, {'B', {0x7f,0x49,0x49,0x49,0x36}},
  {'C', {0x3e,0x41,0x41,0x41,0x22}}, {'D', {0x7f,0x41,0x41,0x22,0x1c}},
  {'E', {0x7f,0x49,0x49,0x49,0x41}}, {'F', {0x7f,0x09,0x09,0x09,0x01}},
  {'G', {0x3e,0x41,0x49,0x49,0x7a}}, {'H', {0x7f,0x08,0x08,0x08,0x7f}},
  {'I', {0x00,0x41,0x7f,0x41,0x00}}, {'J', {0x20,0x40,0x41,0x3f,0x01}},
  {'K', {0x7f,0x08,0x14,0x22,0x41}}, {'L', {0x7f,0x40,0x40,0x40,0x40}},
  {'M', {0x7f,0x02,0x0c,0x02,0x7f}}, {'N', {0x7f,0x04,0x08,0x10,0x7f}},
  {'O', {0x3e,0x41,0x41,0x41,0x3e}}, {'P', {0x7f,0x09,0x09,0x09,0x06}},
  {'Q', {0x3e,0x41,0x51,0x21,0x5e}}, {'R', {0x7f,0x09,0x19,0x29,0x46}},
  {'S', {0x46,0x49,0x49,0x49,0x31}}, {'T', {0x01,0x01,0x7f,0x01,0x01}},
  {'U', {0x3f,0x40,0x40,0x40,0x3f}}, {'V', {0x1f,0x20,0x40,0x20,0x1f}},
  {'W', {0x3f,0x40,0x38,0x40,0x3f}}, {'X', {0x63,0x14,0x08,0x14,0x63}},
  {'Y', {0x07,0x08,0x70,0x08,0x07}}, {'Z', {0x61,0x51,0x49,0x45,0x43}},
};

spi_device_handle_t lcd = nullptr;
std::array<uint16_t, kWidth * kHeight> frame;
bool ready = false;
bool detection_view = false;
uint32_t detection_until = 0;
uint8_t rendered_channel = 0;
int rendered_detections = -1;
bool rendered_all_channels = true;
uint8_t rendered_volume = 100;
uint32_t rendered_last_seen_at = 0;
uint32_t rendered_last_seen_seconds = UINT32_MAX;
int battery_percent = -1;
bool battery_charging = false;
bool battery_blink_visible = true;
uint32_t battery_blink_at = 0;

const uint8_t* glyphFor(char character) {
  const char normalized = static_cast<char>(toupper(static_cast<unsigned char>(character)));
  for (const auto& glyph : kGlyphs) {
    if (glyph.character == normalized) return glyph.columns;
  }
  return kGlyphs[5].columns;
}

void writeBytes(bool data, const void* bytes, size_t length) {
  gpio_set_level(kLcdDc, data ? 1 : 0);
  spi_transaction_t transaction = {};
  transaction.length = length * 8;
  transaction.tx_buffer = bytes;
  ESP_ERROR_CHECK(spi_device_polling_transmit(lcd, &transaction));
}

void command(uint8_t value) {
  writeBytes(false, &value, 1);
}

void commandData(uint8_t value, const uint8_t* data, size_t length) {
  command(value);
  if (length) writeBytes(true, data, length);
}

void fillRect(int x, int y, int width, int height, uint16_t rgb565) {
  if (width <= 0 || height <= 0) return;
  x = constrain(x, 0, kWidth - 1);
  y = constrain(y, 0, kHeight - 1);
  width = min(width, kWidth - x);
  height = min(height, kHeight - y);
  const uint16_t wire_color = static_cast<uint16_t>((rgb565 << 8) | (rgb565 >> 8));
  for (int row = y; row < y + height; ++row) {
    std::fill_n(frame.begin() + row * kWidth + x, width, wire_color);
  }
}

void flush() {
  if (!ready) return;
  const uint8_t columns[] = {
    0x00, 0x00, 0x00, static_cast<uint8_t>(kWidth - 1),
  };
  const uint8_t rows[] = {
    0x00, 0x00, 0x00, static_cast<uint8_t>(kHeight - 1),
  };
  commandData(0x2a, columns, sizeof(columns));
  commandData(0x2b, rows, sizeof(rows));
  command(0x2c);
  for (int row = 0; row < kHeight; row += 8) {
    writeBytes(true, frame.data() + row * kWidth, kWidth * 8 * sizeof(uint16_t));
  }
}

void drawChar(int x, int y, char character, uint16_t foreground, uint16_t background, uint8_t scale) {
  fillRect(x, y, 6 * scale, 8 * scale, background);
  const uint8_t* glyph = glyphFor(character);
  for (int column = 0; column < 5; ++column) {
    for (int row = 0; row < 7; ++row) {
      if (glyph[column] & (1U << row)) {
        fillRect(x + column * scale, y + row * scale, scale, scale, foreground);
      }
    }
  }
}

void drawText(int x, int y, const char* text, uint16_t foreground, uint16_t background, uint8_t scale = 1) {
  while (*text && x + 6 * scale <= kWidth) {
    drawChar(x, y, *text++, foreground, background, scale);
    x += 6 * scale;
  }
}

void drawTextCondensed(int x, int y, const char* text, uint16_t foreground,
                       uint16_t background, uint8_t scale) {
  const int advance = 5 * scale;
  while (*text && x + advance <= kWidth) {
    drawChar(x, y, *text++, foreground, background, scale);
    x += advance;
  }
}

void renderScan(uint8_t channel, int detections, bool allChannels,
                uint8_t volumePercent, uint32_t lastSeenAt) {
  fillRect(0, 0, kWidth, kHeight, kBlack);
  fillRect(0, 0, kWidth, 22, kRed);
  drawTextCondensed(4, 3, "FLOCK-YOU-GO", kWhite, kRed, 2);
  drawText(13, 28, allChannels ? "ALL CHANNELS" : "SCAN 1-6-11",
           kGold, kBlack);
  fillRect(8, 42, 112, 1, kGray);

  char value[24];
  snprintf(value, sizeof(value), "CH %02u", channel);
  drawText(13, 50, value, kWhite, kBlack, 1);
  if (battery_percent < 0) {
    snprintf(value, sizeof(value), "BAT --");
  } else {
    snprintf(value, sizeof(value), "BAT %d%%", battery_percent);
  }
  uint16_t batteryColor = kGreen;
  if (battery_percent < 0) batteryColor = kGray;
  else if (battery_percent < 10) batteryColor = kRed;
  else if (battery_percent < 50) batteryColor = kGold;
  drawText(75, 50, value, batteryColor, kBlack, 1);
  snprintf(value, sizeof(value), "SIGNALS %03d", min(detections, 999));
  drawText(13, 67, value, detections ? kGold : kWhite, kBlack, 1);
  uint32_t lastSeenSeconds = 0;
  if (lastSeenAt) lastSeenSeconds = (millis() - lastSeenAt) / 1000;
  if (lastSeenAt) {
    snprintf(value, sizeof(value), "LAST SEEN: %02lu:%02lu",
             static_cast<unsigned long>(lastSeenSeconds / 60),
             static_cast<unsigned long>(lastSeenSeconds % 60));
  } else {
    snprintf(value, sizeof(value), "LAST SEEN: --:--");
  }
  drawText(13, 84, value, lastSeenAt ? kWhite : kGray, kBlack, 1);
  if (volumePercent == 0) {
    snprintf(value, sizeof(value), "VOLUME MUTE");
  } else {
    snprintf(value, sizeof(value), "VOLUME %u%%", volumePercent);
  }
  drawText(13, 101, value, volumePercent ? kWhite : kGold, kBlack, 1);
  drawText(13, 116, "SCAN/OFF", kGray, kBlack, 1);
  drawText(67, 116, "VOL/SCREEN", kGray, kBlack, 1);
  flush();
  rendered_channel = channel;
  rendered_detections = detections;
  rendered_all_channels = allChannels;
  rendered_volume = volumePercent;
  rendered_last_seen_at = lastSeenAt;
  rendered_last_seen_seconds = lastSeenAt ? lastSeenSeconds : UINT32_MAX;
  detection_view = false;
}

void renderSleepSetting(uint8_t timeoutMinutes, const char* footer) {
  fillRect(0, 0, kWidth, kHeight, kBlack);
  fillRect(0, 0, kWidth, 24, kGold);
  drawText(13, 5, "SCREEN", kBlack, kGold, 2);
  drawText(13, 45, "SLEEP AFTER", kWhite, kBlack, 1);
  char value[24];
  if (timeoutMinutes == 0) snprintf(value, sizeof(value), "NEVER");
  else snprintf(value, sizeof(value), "%u MIN", timeoutMinutes);
  drawTextCondensed(13, 67, value, kGold, kBlack, 2);
  drawText(13, 105, footer, kGray, kBlack, 1);
  flush();
}

}  // namespace

bool aipiDisplayBegin() {
  gpio_config_t outputs = {};
  outputs.pin_bit_mask = (1ULL << kBacklight) | (1ULL << kLcdDc) | (1ULL << kLcdReset);
  outputs.mode = GPIO_MODE_OUTPUT;
  if (gpio_config(&outputs) != ESP_OK) return false;
  gpio_set_level(kBacklight, 0);

  spi_bus_config_t bus = {};
  bus.sclk_io_num = kLcdSclk;
  bus.mosi_io_num = kLcdMosi;
  bus.miso_io_num = -1;
  bus.quadwp_io_num = -1;
  bus.quadhd_io_num = -1;
  bus.max_transfer_sz = kWidth * 16 * sizeof(uint16_t);
  if (spi_bus_initialize(SPI2_HOST, &bus, SPI_DMA_CH_AUTO) != ESP_OK) return false;

  spi_device_interface_config_t device = {};
  device.clock_speed_hz = 20 * 1000 * 1000;
  device.mode = 0;
  device.spics_io_num = kLcdCs;
  device.queue_size = 1;
  if (spi_bus_add_device(SPI2_HOST, &device, &lcd) != ESP_OK) return false;

  gpio_set_level(kLcdReset, 0);
  delay(20);
  gpio_set_level(kLcdReset, 1);
  delay(120);
  command(0x01);
  delay(150);
  command(0x11);
  delay(120);
  const uint8_t color_mode = 0x05;
  commandData(0x3a, &color_mode, 1);
  commandData(0x36, &kMadctl, 1);
  command(0x20);
  command(0x29);
  delay(20);
  ready = true;
  gpio_set_level(kBacklight, 1);
  renderScan(11, 0, true, 100, 0);
  return true;
}

void aipiDisplaySetBattery(int percent, bool charging) {
  if (!ready) return;
  percent = constrain(percent, 0, 100);
  if (percent == battery_percent && charging == battery_charging) return;
  battery_percent = percent;
  battery_charging = charging;
  battery_blink_visible = true;
  battery_blink_at = millis();
  if (!detection_view) {
    renderScan(rendered_channel, rendered_detections, rendered_all_channels,
               rendered_volume, rendered_last_seen_at);
  }
}

void aipiDisplaySetBacklight(bool enabled) {
  if (!ready) return;
  gpio_set_level(kBacklight, enabled ? 1 : 0);
}

void aipiDisplayShowSleepSetting(uint8_t timeoutMinutes) {
  if (!ready) return;
  renderSleepSetting(timeoutMinutes, "RELEASE TO SAVE");
  detection_view = true;
  detection_until = UINT32_MAX;
}

void aipiDisplayConfirmSleepSetting(uint8_t timeoutMinutes) {
  if (!ready) return;
  renderSleepSetting(timeoutMinutes, "SAVED");
  detection_view = true;
  detection_until = millis() + 1500;
}

void aipiDisplayShowScan(uint8_t channel, int detections, bool allChannels,
                         uint8_t volumePercent, uint32_t lastSeenAt) {
  if (!ready) return;
  if (channel == rendered_channel && detections == rendered_detections &&
      allChannels == rendered_all_channels && volumePercent == rendered_volume &&
      lastSeenAt == rendered_last_seen_at && !detection_view) {
    return;
  }
  renderScan(channel, detections, allChannels, volumePercent, lastSeenAt);
}

void aipiDisplayShowDetection(const char* oui, int8_t rssi, uint8_t channel,
                              uint16_t signalHit) {
  if (!ready) return;
  fillRect(0, 0, kWidth, kHeight, kBlack);
  fillRect(0, 0, kWidth, 24, kGold);
  drawTextCondensed(4, 4, "SIGNAL HIT", kBlack, kGold, 2);
  drawText(8, 34, "OUI", kGray, kBlack);
  drawText(8, 47, oui && oui[0] ? oui : "UNKNOWN", kWhite, kBlack, 2);
  char value[24];
  snprintf(value, sizeof(value), "RSSI %d DBM", rssi);
  drawText(8, 72, value, kWhite, kBlack);
  snprintf(value, sizeof(value), "CH %02u  HIT %u", channel, signalHit);
  drawText(8, 88, value, kWhite, kBlack);
  drawText(8, 108, "SAVED LOCAL", kGreen, kBlack);
  flush();
  detection_view = true;
  detection_until = millis() + 3500;
}

void aipiDisplayTick(uint8_t channel, int detections, bool allChannels,
                     uint8_t volumePercent, uint32_t lastSeenAt) {
  if (!ready) return;
  if (detection_view) {
    if (detection_until != UINT32_MAX &&
        static_cast<int32_t>(millis() - detection_until) >= 0) {
      renderScan(channel, detections, allChannels, volumePercent, lastSeenAt);
    }
    return;
  }
  const uint32_t lastSeenSeconds = lastSeenAt ? (millis() - lastSeenAt) / 1000 : UINT32_MAX;
  if (lastSeenSeconds != rendered_last_seen_seconds) {
    renderScan(channel, detections, allChannels, volumePercent, lastSeenAt);
    return;
  }
  aipiDisplayShowScan(channel, detections, allChannels, volumePercent, lastSeenAt);
}
