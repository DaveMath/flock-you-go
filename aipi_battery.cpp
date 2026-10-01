#include "aipi_battery.h"

#include "driver/adc.h"
#include "driver/gpio.h"
#include "esp_adc_cal.h"

namespace {

constexpr gpio_num_t kChargeStatusPin = GPIO_NUM_8;
constexpr gpio_num_t kPowerHoldPin = GPIO_NUM_10;
constexpr adc1_channel_t kBatteryAdcChannel = ADC1_CHANNEL_1;
constexpr adc_atten_t kBatteryAttenuation = ADC_ATTEN_DB_12;
constexpr uint32_t kBatteryDividerMultiplierMilli = 2500;
constexpr int kSampleCount = 10;
constexpr uint32_t kDefaultAdcVrefMillivolts = 1100;

esp_adc_cal_characteristics_t adcCharacteristics = {};
bool adcReady = false;
bool calibrationEnabled = false;

uint8_t interpolatePercent(uint32_t millivolts, uint32_t lowMillivolts,
                           uint8_t lowPercent, uint32_t highMillivolts,
                           uint8_t highPercent) {
  return static_cast<uint8_t>(
      lowPercent + ((millivolts - lowMillivolts) * (highPercent - lowPercent)) /
                       (highMillivolts - lowMillivolts));
}

}  // namespace

uint8_t aipiBatteryPercentFromMillivolts(uint32_t millivolts) {
  if (millivolts >= 4200) return 100;
  if (millivolts >= 4100) return interpolatePercent(millivolts, 4100, 90, 4200, 100);
  if (millivolts >= 3950) return interpolatePercent(millivolts, 3950, 70, 4100, 90);
  if (millivolts >= 3800) return interpolatePercent(millivolts, 3800, 50, 3950, 70);
  if (millivolts >= 3700) return interpolatePercent(millivolts, 3700, 30, 3800, 50);
  if (millivolts >= 3500) return interpolatePercent(millivolts, 3500, 10, 3700, 30);
  if (millivolts >= 3300) return interpolatePercent(millivolts, 3300, 0, 3500, 10);
  return 0;
}

esp_err_t aipiBatteryBegin() {
  gpio_config_t powerHold = {};
  powerHold.pin_bit_mask = 1ULL << kPowerHoldPin;
  powerHold.mode = GPIO_MODE_OUTPUT;
  esp_err_t result = gpio_config(&powerHold);
  if (result != ESP_OK) return result;
  result = gpio_set_level(kPowerHoldPin, 1);
  if (result != ESP_OK) return result;

  gpio_config_t chargeStatus = {};
  chargeStatus.pin_bit_mask = 1ULL << kChargeStatusPin;
  chargeStatus.mode = GPIO_MODE_INPUT;
  chargeStatus.pull_up_en = GPIO_PULLUP_ENABLE;
  chargeStatus.pull_down_en = GPIO_PULLDOWN_DISABLE;
  chargeStatus.intr_type = GPIO_INTR_DISABLE;
  result = gpio_config(&chargeStatus);
  if (result != ESP_OK) return result;

  result = adc1_config_width(ADC_WIDTH_BIT_12);
  if (result != ESP_OK) return result;
  result = adc1_config_channel_atten(kBatteryAdcChannel, kBatteryAttenuation);
  if (result != ESP_OK) return result;

  const esp_adc_cal_value_t calibrationSource = esp_adc_cal_characterize(
      ADC_UNIT_1, kBatteryAttenuation, ADC_WIDTH_BIT_12,
      kDefaultAdcVrefMillivolts, &adcCharacteristics);
  calibrationEnabled = calibrationSource != ESP_ADC_CAL_VAL_DEFAULT_VREF;
  adcReady = true;
  return ESP_OK;
}

esp_err_t aipiBatteryRead(AipiBatteryReading* reading) {
  if (reading == nullptr || !adcReady) return ESP_ERR_INVALID_STATE;

  uint32_t pinMillivoltsSum = 0;
  for (int sample = 0; sample < kSampleCount; ++sample) {
    const int raw = adc1_get_raw(kBatteryAdcChannel);
    if (raw < 0) return ESP_FAIL;
    pinMillivoltsSum += esp_adc_cal_raw_to_voltage(raw, &adcCharacteristics);
  }

  const uint32_t averagePinMillivolts = pinMillivoltsSum / kSampleCount;
  reading->millivolts = (averagePinMillivolts * kBatteryDividerMultiplierMilli) / 1000;
  reading->percent = aipiBatteryPercentFromMillivolts(reading->millivolts);
  reading->charging = aipiBatteryIsCharging();
  reading->calibrated = calibrationEnabled;
  return ESP_OK;
}

bool aipiBatteryIsCharging() {
  return gpio_get_level(kChargeStatusPin) == 0;
}
