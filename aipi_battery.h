#pragma once

#include <stdint.h>

#include "esp_err.h"

struct AipiBatteryReading {
  uint32_t millivolts;
  uint8_t percent;
  bool charging;
  bool calibrated;
};

esp_err_t aipiBatteryBegin();
esp_err_t aipiBatteryRead(AipiBatteryReading* reading);
bool aipiBatteryIsCharging();
uint8_t aipiBatteryPercentFromMillivolts(uint32_t millivolts);
