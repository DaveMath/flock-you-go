#pragma once

#include <stdint.h>

bool aipiAudioBegin();
bool aipiAudioReady();
bool aipiAudioPlayVolumeSample(uint8_t volumePercent);
bool aipiAudioPlayNewDetection(uint8_t volumePercent);
bool aipiAudioPlayHeartbeat(uint8_t volumePercent);
bool aipiAudioPlayStartup(uint8_t volumePercent);
