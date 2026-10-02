#include "aipi_audio.h"

#include <Arduino.h>
#include <Wire.h>
#include <math.h>

#include "driver/gpio.h"
#include "driver/i2s.h"

namespace {

constexpr uint8_t kCodecAddress = 0x18;
constexpr int kI2cSda = 5;
constexpr int kI2cScl = 4;
constexpr gpio_num_t kSpeakerEnable = GPIO_NUM_9;
constexpr int kI2sBclk = 14;
constexpr int kI2sWordSelect = 12;
constexpr int kI2sDataOut = 11;
constexpr uint32_t kSampleRate = 16000;
constexpr i2s_port_t kI2sPort = I2S_NUM_0;
constexpr uint8_t kDacMuteMask = 0x60;

bool ready = false;

bool writeRegister(uint8_t reg, uint8_t value) {
  Wire.beginTransmission(kCodecAddress);
  Wire.write(reg);
  Wire.write(value);
  return Wire.endTransmission() == 0;
}

bool readRegister(uint8_t reg, uint8_t* value) {
  Wire.beginTransmission(kCodecAddress);
  Wire.write(reg);
  if (Wire.endTransmission(false) != 0) return false;
  if (Wire.requestFrom(kCodecAddress, static_cast<uint8_t>(1)) != 1) return false;
  *value = Wire.read();
  return true;
}

bool setMuted(bool muted) {
  uint8_t value = 0;
  if (!readRegister(0x31, &value)) return false;
  value = muted ? (value | kDacMuteMask) : (value & ~kDacMuteMask);
  return writeRegister(0x31, value);
}

bool writeSequence(const uint8_t sequence[][2], size_t count) {
  for (size_t i = 0; i < count; ++i) {
    if (!writeRegister(sequence[i][0], sequence[i][1])) return false;
  }
  return true;
}

int16_t amplitudeForVolume(uint8_t volumePercent) {
  if (volumePercent == 0) return 0;
  // Keep 100% below clipping; the onboard 0.8 W speaker distorts near full scale.
  return static_cast<int16_t>((14000U * min<uint8_t>(volumePercent, 100)) / 100U);
}

bool writeFrames(const int16_t* frames, size_t frameCount) {
  size_t bytesWritten = 0;
  const size_t bytes = frameCount * 2 * sizeof(int16_t);
  return i2s_write(kI2sPort, frames, bytes, &bytesWritten, portMAX_DELAY) == ESP_OK &&
         bytesWritten == bytes;
}

bool writeSilence(uint16_t durationMs) {
  int16_t frames[128 * 2] = {};
  size_t remaining = (kSampleRate * durationMs) / 1000;
  while (remaining > 0) {
    const size_t count = min<size_t>(remaining, 128);
    if (!writeFrames(frames, count)) return false;
    remaining -= count;
  }
  return true;
}

bool writeTone(uint16_t frequency, uint16_t durationMs, uint8_t volumePercent) {
  int16_t frames[128 * 2];
  const int16_t amplitude = amplitudeForVolume(volumePercent);
  const size_t sampleCount = (kSampleRate * durationMs) / 1000;
  const size_t period = max<size_t>(2, kSampleRate / frequency);
  const size_t halfPeriod = max<size_t>(1, period / 2);
  size_t generated = 0;
  while (generated < sampleCount) {
    const size_t count = min<size_t>(sampleCount - generated, 128);
    for (size_t i = 0; i < count; ++i) {
      const int16_t sample = ((generated + i) % period) < halfPeriod ? amplitude : -amplitude;
      frames[i * 2] = sample;
      frames[i * 2 + 1] = sample;
    }
    if (!writeFrames(frames, count)) return false;
    generated += count;
  }
  return true;
}

bool beginPlayback() {
  if (!ready || !setMuted(false)) return false;
  gpio_set_level(kSpeakerEnable, 1);
  delay(8);
  return writeSilence(12);
}

bool endPlayback(bool success) {
  success = writeSilence(40) && success;
  // i2s_write() returns after copying into DMA, not after the speaker has
  // emitted the final frame. Let all six 128-frame buffers drain before mute.
  delay(60);
  i2s_zero_dma_buffer(kI2sPort);
  delay(4);
  gpio_set_level(kSpeakerEnable, 0);
  success = setMuted(true) && success;
  return success;
}

bool playTwoTone(uint16_t firstHz, uint16_t secondHz, uint16_t noteMs,
                 uint16_t gapMs, uint8_t volumePercent) {
  if (volumePercent == 0 || !beginPlayback()) return volumePercent == 0;
  bool ok = writeTone(firstHz, noteMs, volumePercent);
  ok = writeSilence(gapMs) && ok;
  ok = writeTone(secondHz, noteMs, volumePercent) && ok;
  return endPlayback(ok);
}

}  // namespace

bool aipiAudioBegin() {
  gpio_config_t amp = {};
  amp.pin_bit_mask = 1ULL << kSpeakerEnable;
  amp.mode = GPIO_MODE_OUTPUT;
  if (gpio_config(&amp) != ESP_OK) return false;
  gpio_set_level(kSpeakerEnable, 0);

  Wire.begin(kI2cSda, kI2cScl, 100000);
  Wire.beginTransmission(kCodecAddress);
  if (Wire.endTransmission() != 0) return false;

  static constexpr uint8_t kCodecSequence[][2] = {
      {0x00, 0x1F}, {0x00, 0x00},
      {0x01, 0x9F}, {0x02, 0x10}, {0x03, 0x10}, {0x04, 0x20},
      {0x05, 0x00}, {0x06, 0x03}, {0x07, 0x00}, {0x08, 0xFF},
      {0x09, 0x0C}, {0x0A, 0x0C},
      {0x32, 0xBF}, {0x12, 0x00}, {0x13, 0x10},
      {0x31, kDacMuteMask}, {0x37, 0x08},
      {0x0D, 0x01}, {0x0E, 0x02}, {0x00, 0x80},
  };
  if (!writeSequence(kCodecSequence, sizeof(kCodecSequence) / sizeof(kCodecSequence[0]))) {
    return false;
  }

  i2s_config_t config = {};
  config.mode = static_cast<i2s_mode_t>(I2S_MODE_MASTER | I2S_MODE_TX);
  config.sample_rate = kSampleRate;
  config.bits_per_sample = I2S_BITS_PER_SAMPLE_16BIT;
  config.channel_format = I2S_CHANNEL_FMT_RIGHT_LEFT;
  config.communication_format = I2S_COMM_FORMAT_STAND_I2S;
  config.intr_alloc_flags = ESP_INTR_FLAG_LEVEL1;
  config.dma_buf_count = 6;
  config.dma_buf_len = 128;
  config.use_apll = false;
  config.tx_desc_auto_clear = true;
  config.fixed_mclk = 0;
  if (i2s_driver_install(kI2sPort, &config, 0, nullptr) != ESP_OK) return false;

  i2s_pin_config_t pins = {};
  pins.mck_io_num = I2S_PIN_NO_CHANGE;
  pins.bck_io_num = kI2sBclk;
  pins.ws_io_num = kI2sWordSelect;
  pins.data_out_num = kI2sDataOut;
  pins.data_in_num = I2S_PIN_NO_CHANGE;
  if (i2s_set_pin(kI2sPort, &pins) != ESP_OK) return false;
  i2s_zero_dma_buffer(kI2sPort);
  ready = true;
  return true;
}

bool aipiAudioReady() { return ready; }

bool aipiAudioPlayVolumeSample(uint8_t volumePercent) {
  if (volumePercent == 0) return true;
  if (volumePercent <= 10) {
    if (!beginPlayback()) return false;
    return endPlayback(writeTone(900, 180, volumePercent));
  }
  if (volumePercent <= 50) return playTwoTone(1500, 1500, 75, 70, volumePercent);
  return playTwoTone(2000, 2800, 110, 40, volumePercent);
}

bool aipiAudioPlayNewDetection(uint8_t volumePercent) {
  return playTwoTone(2000, 2800, 55, 25, volumePercent);
}

bool aipiAudioPlayHeartbeat(uint8_t volumePercent) {
  return playTwoTone(1500, 1500, 70, 70, volumePercent);
}

bool aipiAudioPlayStartup(uint8_t volumePercent) {
  if (volumePercent == 0 || !beginPlayback()) return volumePercent == 0;
  bool ok = writeTone(523, 70, volumePercent);
  ok = writeSilence(25) && ok;
  ok = writeTone(659, 70, volumePercent) && ok;
  ok = writeSilence(25) && ok;
  ok = writeTone(784, 110, volumePercent) && ok;
  return endPlayback(ok);
}
