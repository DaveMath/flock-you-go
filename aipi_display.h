#pragma once

#include <stdint.h>

bool aipiDisplayBegin();
void aipiDisplaySetBattery(int percent, bool charging);
void aipiDisplaySetBacklight(bool enabled);
void aipiDisplayShowSleepSetting(uint8_t timeoutMinutes);
void aipiDisplayConfirmSleepSetting(uint8_t timeoutMinutes);
void aipiDisplayShowShutdownCountdown(uint8_t secondsRemaining);
void aipiDisplayShowScan(uint8_t channel, int sensors, int encounters, bool allChannels,
                         uint8_t volumePercent, uint32_t lastSeenAt);
void aipiDisplayShowDetection(const char* oui, int8_t rssi, uint8_t channel,
                              uint16_t signalHit);
void aipiDisplayTick(uint8_t channel, int sensors, int encounters, bool allChannels,
                     uint8_t volumePercent, uint32_t lastSeenAt);
