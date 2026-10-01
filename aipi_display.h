#pragma once

#include <stdint.h>

bool aipiDisplayBegin();
void aipiDisplaySetBattery(int percent, bool charging);
void aipiDisplayShowScan(uint8_t channel, int detections, bool allChannels,
                         uint8_t volumePercent);
void aipiDisplayShowDetection(const char* oui, int8_t rssi, uint8_t channel, uint16_t count);
void aipiDisplayTick(uint8_t channel, int detections, bool allChannels,
                     uint8_t volumePercent);
