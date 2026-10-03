#include <Arduino.h>
#include "esp_wifi.h"
#include "esp_err.h"
#include "esp_sleep.h"
#include "driver/gpio.h"
#include <ctype.h>
#include <string.h>
#include <SPIFFS.h>
#include "aipi_audio.h"
#include "aipi_battery.h"
#include "aipi_display.h"

// ============================================================
// CONFIG
// ============================================================

// AiPi status light is a GPIO46 WS2812, not a single active-low GPIO.
#define USE_LED          0
#define LED_ACTIVE_HIGH  0
#define LED_FLASH_MS     120

// GPIO43 mirroring was part of the earlier dev-board configuration.
#define MIRROR_SERIAL    0

#define CHANNEL_DWELL_MS 700

typedef enum : uint8_t {
  SCAN_1_6_11 = 0,
  SCAN_ALL_CHANNELS = 1,
} ScanMode;

// Prioritize the three non-overlapping US channels, then fill in the gaps.
// Channels 12/13 are deliberately excluded from the default regulatory plan.
static const uint8_t fastScanChannels[] = {1, 6, 11};
static const uint8_t allScanChannels[] = {11, 6, 1, 10, 5, 2, 9, 4, 3, 8, 7};

#define LEFT_BUTTON_PIN  GPIO_NUM_1
#define RIGHT_BUTTON_PIN GPIO_NUM_42
#define BUTTON_DEBOUNCE_MS 35
#define SHUTDOWN_HOLD_MS 3000
#define SHUTDOWN_COUNTDOWN_MS 3000
#define SCREEN_TIMEOUT_HOLD_MS 2000
#define SCREEN_TIMEOUT_CYCLE_MS 2000
#define BATTERY_POLL_MS 30000

#define HEARTBEAT_MS    30000
#define RSSI_MIN        -100
#define RSSI_LOW_CONFIDENCE_MIN -95
#define ALERT_COOLDOWN_MS (5UL * 60UL * 1000UL)
#define ENCOUNTER_ABSENCE_MS (10UL * 60UL * 1000UL)

// Audio cadence: two fast ascending beeps on a NEW MAC, then while any
// target is still in range (seen within HB_DEVICE_ACTIVE_MS), two monotone
// heartbeat beeps every HB_BEEP_INTERVAL_MS.
#define HB_DEVICE_ACTIVE_MS    3000
#define HB_BEEP_INTERVAL_MS    10000
#define NEW_CHIRP_LO_HZ        2000
#define NEW_CHIRP_HI_HZ        2800
#define NEW_CHIRP_NOTE_MS      55
#define NEW_CHIRP_GAP_MS       25
#define HB_BEEP_HZ             1500
#define HB_BEEP_NOTE_MS        70
#define HB_BEEP_GAP_MS         70

#define ENABLE_SSID_MATCH 1
#define CHECK_ADDR1 1   // dst/rx — catches Flock STAs receiving probe responses
#define CHECK_ADDR3 0   // bssid fallback for randomised addr2
static const char* target_ssid_keywords[] = { "flock" };
static const size_t SSID_KEYWORD_COUNT = sizeof(target_ssid_keywords) / sizeof(target_ssid_keywords[0]);

#define STOP_ON_SSID_HIT 0
#define STOP_ON_OUI_HIT  0
#define PROCESS_MGMT_FRAMES 1
#define PROCESS_DATA_FRAMES 1

// Persistence
#define MAX_DETECTIONS       200
#define FY_SESSION_FILE      "/session.json"
#define FY_SESSION_TMP       "/session.tmp"
#define FY_PREV_FILE         "/prev_session.json"
#define AUTOSAVE_INTERVAL_MS 60000

// ============================================================
// TARGET OUI LIST  (all lowercase, colons only)
// ============================================================

static const char* target_ouis[] = {
  "70:c9:4e", "3c:91:80", "d8:f3:bc", "80:30:49", "b8:35:32",
  "14:5a:fc", "74:4c:a1", "08:3a:88", "9c:2f:9d", "c0:35:32",
  "94:08:53", "e4:aa:ea", "f4:6a:dd", "f8:a2:d6", "24:b2:b9",
  "00:f4:8d", "d0:39:57", "e8:d0:fc", "e0:4f:43", "b8:1e:a4",
  "70:08:94", "58:8e:81", "ec:1b:bd", "3c:71:bf", "58:00:e3",
  "90:35:ea", "5c:93:a2", "64:6e:69", "48:27:ea", "a4:cf:12",
  // Contributed by Michael / DeFlockJoplin — discovered via wildcard-probe
  // + OUI signature during field testing. The 12th camera in his drive-test
  // used this prefix and wasn't in @NitekryDPaul's original 30.
  "82:6b:f2"

};
static const size_t OUI_COUNT = sizeof(target_ouis) / sizeof(target_ouis[0]);

// Pre-compiled byte table — populated once in setup(), never touched again.
// Keeps matchOuiRaw entirely in IRAM with no flash-resident function calls.
static uint8_t oui_bytes[OUI_COUNT][3];

// ============================================================
// ALERT QUEUE  (callback → loop, avoids Serial in WiFi task)
// ============================================================

#define ALERT_QUEUE_SIZE 64

typedef enum : uint8_t {
  CONFIDENCE_LOW    = 0,
  CONFIDENCE_MEDIUM = 1,
  CONFIDENCE_HIGH   = 2,
} DetectionConfidence;

typedef enum : uint8_t {
  ALERT_OUI_ADDR2       = 0,
  ALERT_OUI_ADDR1       = 1,
  ALERT_OUI_ADDR3       = 2,
  ALERT_SSID            = 3,
  // Probe Request + wildcard SSID (tag 0, length 0) from a known-OUI addr2.
  // Tight signature from Michael / DeFlockJoplin field research:
  //   https://github.com/DeflockJoplin/flock-you
  ALERT_WILDCARD_PROBE  = 4,
} AlertType;

typedef struct {
  AlertType type;
  uint8_t   mac[6];
  int8_t    rssi;
  uint8_t   channel;
  DetectionConfidence confidence;
  char      ssid[33];     // populated for SSID hits
  char      frameKind[12];
} AlertEntry;

static volatile AlertEntry alertQueue[ALERT_QUEUE_SIZE];
static volatile size_t alertHead = 0;  // written by callback
static volatile size_t alertTail = 0;  // read by loop()
static portMUX_TYPE    queueMux  = portMUX_INITIALIZER_UNLOCKED;

static volatile uint32_t queueDrops = 0;

static void IRAM_ATTR enqueueAlert(AlertType type, DetectionConfidence confidence,
                                    const uint8_t* mac, int8_t rssi, uint8_t ch,
                                    const char* ssid, const char* kind) {
  if (rssi < RSSI_MIN) return;
  if (confidence == CONFIDENCE_LOW && rssi < RSSI_LOW_CONFIDENCE_MIN) return;

  portENTER_CRITICAL_ISR(&queueMux);
  size_t next = (alertHead + 1) % ALERT_QUEUE_SIZE;
  if (next == alertTail) {                         // drop if full — loop() is behind
    queueDrops++;
    portEXIT_CRITICAL_ISR(&queueMux);
    return;
  }

  AlertEntry* e = (AlertEntry*)&alertQueue[alertHead];
  e->type    = type;
  e->rssi    = rssi;
  e->channel = ch;
  e->confidence = confidence;
  memcpy((void*)e->mac, mac, 6);

  if (ssid)  { strncpy((char*)e->ssid,      ssid, 32); ((char*)e->ssid)[32] = '\0'; }
  else        { ((char*)e->ssid)[0] = '\0'; }

  if (kind)  { strncpy((char*)e->frameKind, kind, 11); ((char*)e->frameKind)[11] = '\0'; }
  else        { ((char*)e->frameKind)[0] = '\0'; }

  alertHead = next;
  portEXIT_CRITICAL_ISR(&queueMux);
}

// ============================================================
// DETECTION TABLE  (on-device storage, persisted to SPIFFS)
// ============================================================
//
// Single-threaded: only touched from loop() — drainAlertQueue() adds, and
// fySaveSession() reads. No mutex needed. The WiFi-task callback never
// touches this table; it only writes to the lock-free alert ring buffer.

typedef struct {
  char     mac[18];
  char     method[16];     // "oui_addr2" / "oui_addr1" / "oui_addr3" / "ssid"
  DetectionConfidence confidence;
  int8_t   rssi;
  uint8_t  channel;
  uint32_t firstSeen;      // millis() at first hit
  uint32_t lastSeen;       // millis() at latest hit
  uint16_t count;          // raw observations; does not drive user-facing hits
  uint16_t encounters;     // first sighting plus returns after a long absence
  char     ssid[33];       // "" unless an SSID hit populated it
} FYDetection;

static FYDetection fyDet[MAX_DETECTIONS];
static int           fyDetCount       = 0;
static uint16_t      fyEncounterCount = 0;
static bool          fySpiffsReady    = false;
static bool          fyDirty          = false;
static unsigned long fyLastSaveAt     = 0;
static int           fyLastSaveCount  = 0;

// ============================================================
// STATE
// ============================================================

static uint8_t  currentChannel = 1;
static size_t   channelIndex = 0;
static ScanMode scanMode = SCAN_ALL_CHANNELS;
static const uint8_t volumeLevels[] = {0, 10, 50, 100};
static size_t volumeIndex = 3;
static uint8_t outputVolumePercent = volumeLevels[volumeIndex];
static bool batteryReady = false;
static AipiBatteryReading batteryReading = {0, 0, false, false};
static unsigned long lastBatteryRead = 0;
static unsigned long lastHop = 0;
static unsigned long lastHeartbeat = 0;
static volatile bool sniffingStopped = false;

static volatile uint32_t rxFrames = 0;
static volatile uint32_t rxMgmtFrames = 0;
static volatile uint32_t rxDataFrames = 0;
static volatile uint32_t rxByChannel[14] = {0};
static uint32_t channelSwitchFailures = 0;

typedef struct {
  gpio_num_t pin;
  const char* name;
  bool rawPressed;
  bool stablePressed;
  uint32_t changedAt;
} ButtonState;

static ButtonState leftButton = {LEFT_BUTTON_PIN, "left", false, false, 0};
static ButtonState rightButton = {RIGHT_BUTTON_PIN, "right", false, false, 0};

// Dedupe table (small circular, avoids single-slot eviction bug).
// This is the output-rate limiter. The detection table still refreshes RSSI,
// last-seen, and raw observations even when output is suppressed.
#define DEDUPE_SLOTS 8
static struct {
  char mac[18];
  unsigned long ts;
} dedupeTable[DEDUPE_SLOTS];
static size_t dedupeIdx = 0;

// LED one-shot pulse timer
static volatile unsigned long ledOffAt = 0;

// Heartbeat audio state: last time any target was seen, last time the
// heartbeat beep-pair was played. When nothing has been seen for
// HB_DEVICE_ACTIVE_MS the heartbeat stops until the next new detection.
static unsigned long fyLastTargetSeen  = 0;
static unsigned long fyLastHeartbeatAt = 0;
static unsigned long fyLastBogeySeenAt = 0;

static const uint8_t screenTimeoutMinutes[] = {1, 5, 0};
static size_t screenTimeoutIndex = 0;
static unsigned long screenLastActivityAt = 0;
static unsigned long leftButtonPressedAt = 0;
static unsigned long leftShutdownCountdownStartedAt = 0;
static uint8_t leftShutdownCountdownShown = 0xff;
static unsigned long rightButtonPressedAt = 0;
static unsigned long rightButtonNextCycleAt = 0;
static bool leftButtonLongPressApplied = false;
static bool leftShutdownCountdownActive = false;
static bool rightButtonLongPressApplied = false;
static bool screenBacklightOn = true;

// ============================================================
// 802.11 HEADER
// ============================================================

typedef struct __attribute__((packed)) {
  uint16_t frame_ctrl;
  uint16_t duration;
  uint8_t  addr1[6];
  uint8_t  addr2[6];
  uint8_t  addr3[6];
  uint16_t seq_ctrl;
} wifi_ieee80211_mac_hdr_t;

// ============================================================
// HELPERS
// ============================================================

// Dual-output: prints to both Serial (USB) and Serial1 (GPIO43)
static char _dualBuf[384];

static void dualPrintf(const char* fmt, ...) __attribute__((format(printf, 1, 2)));
static void dualPrintf(const char* fmt, ...) {
  va_list args;
  va_start(args, fmt);
  int n = vsnprintf(_dualBuf, sizeof(_dualBuf), fmt, args);
  va_end(args);
  if (n > 0) {
    Serial.write(_dualBuf, n);
#if MIRROR_SERIAL
    Serial1.write(_dualBuf, n);
#endif
  }
}

static void dualPrintln(const char* str) {
  Serial.println(str);
#if MIRROR_SERIAL
  Serial1.println(str);
#endif
}

static inline void ledSet(bool on) {
#if USE_LED
#if LED_ACTIVE_HIGH
  digitalWrite(LED_PIN, on ? HIGH : LOW);
#else
  digitalWrite(LED_PIN, on ? LOW  : HIGH);
#endif
#endif
}

static void ledFlash(unsigned ms) {
#if USE_LED
  ledSet(true);
  ledOffAt = millis() + ms;
  if (ledOffAt == 0) ledOffAt = 1;  // avoid the "off" sentinel
#endif
}

static void ledTick() {
#if USE_LED
  if (ledOffAt && (long)(millis() - ledOffAt) >= 0) {
    ledSet(false);
    ledOffAt = 0;
  }
#endif
}

// Two monotone beeps — periodic heartbeat while at least one target is still
// in range (last seen within HB_DEVICE_ACTIVE_MS).
static void heartbeatBeep() {
  if (outputVolumePercent == 0) return;
  if (!aipiAudioPlayHeartbeat(outputVolumePercent)) {
    dualPrintln("[flockyou] heartbeat sound failed");
  }
}
static void startupBeep() {
  if (!aipiAudioPlayStartup(50)) {
    dualPrintln("[flockyou] startup sound failed");
  }
}

static void macToStr(const uint8_t* mac, char* buf, size_t len) {
  snprintf(buf, len, "%02x:%02x:%02x:%02x:%02x:%02x",
           mac[0], mac[1], mac[2], mac[3], mac[4], mac[5]);
}
static void ouiFromMac(const uint8_t* mac, char* buf, size_t len) {
  snprintf(buf, len, "%02x:%02x:%02x", mac[0], mac[1], mac[2]);
}

static void precompileOuis() {
  for (size_t i = 0; i < OUI_COUNT; i++) {
    const char* o  = target_ouis[i];
    oui_bytes[i][0] = (uint8_t)strtol(o,     nullptr, 16);
    oui_bytes[i][1] = (uint8_t)strtol(o + 3, nullptr, 16);
    oui_bytes[i][2] = (uint8_t)strtol(o + 6, nullptr, 16);
  }
}

// Bit 0 of byte 0 set = multicast/broadcast — never a real device transmitter or receiver
// we care about. Guards addr1 checks against 01:xx, 33:33:xx, ff:ff:ff:ff:ff:ff etc.
static inline bool IRAM_ATTR isMulticast(const uint8_t* mac) {
  return mac[0] & 0x01;
}

static bool IRAM_ATTR matchOuiRaw(const uint8_t* mac) {
  // Locally-administered (randomised) MACs have bit 1 of byte 0 set.
  // Fixed infrastructure devices never use them — skip immediately.
  if (mac[0] & 0x02) return false;

  for (size_t i = 0; i < OUI_COUNT; i++) {
    if (mac[0] == oui_bytes[i][0] &&
        mac[1] == oui_bytes[i][1] &&
        mac[2] == oui_bytes[i][2]) return true;
  }
  return false;
}

static char* strcasestr_local(const char* haystack, const char* needle) {
  if (!*needle) return (char*)haystack;
  for (; *haystack; ++haystack) {
    const char* h = haystack; const char* n = needle;
    while (*h && *n && tolower((unsigned char)*h) == tolower((unsigned char)*n)) { ++h; ++n; }
    if (!*n) return (char*)haystack;
  }
  return nullptr;
}
static bool matchSsidKeyword(const char* ssid) {
  for (size_t i = 0; i < SSID_KEYWORD_COUNT; i++)
    if (strcasestr_local(ssid, target_ssid_keywords[i])) return true;
  return false;
}

static const char* channelModeName() {
  return scanMode == SCAN_ALL_CHANNELS ? "ALL_CHANNELS" : "SCAN_1_6_11";
}

static const char* confidenceName(DetectionConfidence confidence) {
  switch (confidence) {
    case CONFIDENCE_HIGH:   return "high";
    case CONFIDENCE_MEDIUM: return "medium";
    default:                return "low";
  }
}

static bool wifiOk(esp_err_t err, const char* operation) {
  if (err == ESP_OK) return true;
  dualPrintf("[flockyou] WiFi error: %s failed: %s (0x%x)\n",
             operation, esp_err_to_name(err), (unsigned)err);
  return false;
}

static bool setScanChannel(uint8_t channel) {
  esp_err_t err = esp_wifi_set_channel(channel, WIFI_SECOND_CHAN_NONE);
  if (err == ESP_OK) return true;
  channelSwitchFailures++;
  return wifiOk(err, "set_channel");
}

static const uint8_t* activeChannels(size_t* count) {
  if (scanMode == SCAN_ALL_CHANNELS) {
    *count = sizeof(allScanChannels) / sizeof(allScanChannels[0]);
    return allScanChannels;
  }
  *count = sizeof(fastScanChannels) / sizeof(fastScanChannels[0]);
  return fastScanChannels;
}

static void selectFirstChannel() {
  size_t count = 0;
  const uint8_t* channels = activeChannels(&count);
  channelIndex = 0;
  currentChannel = channels[0];
  setScanChannel(currentChannel);
  lastHop = millis();
}

static bool buttonIsPressed(const ButtonState* button) {
  return gpio_get_level(button->pin) == 0;
}

static void initButtons() {
  // Clear any configuration inherited from boot/JTAG before assigning these
  // pads to the two active-low AiPi buttons.
  gpio_reset_pin(LEFT_BUTTON_PIN);
  gpio_reset_pin(RIGHT_BUTTON_PIN);

  gpio_config_t config = {};
  config.pin_bit_mask = (1ULL << LEFT_BUTTON_PIN) | (1ULL << RIGHT_BUTTON_PIN);
  config.mode = GPIO_MODE_INPUT;
  config.pull_up_en = GPIO_PULLUP_ENABLE;
  config.pull_down_en = GPIO_PULLDOWN_DISABLE;
  config.intr_type = GPIO_INTR_DISABLE;
  esp_err_t err = gpio_config(&config);
  if (err != ESP_OK) {
    dualPrintf("[flockyou] button GPIO setup failed: %s (0x%x)\n",
               esp_err_to_name(err), (unsigned)err);
    return;
  }
  gpio_pullup_en(LEFT_BUTTON_PIN);
  gpio_pullup_en(RIGHT_BUTTON_PIN);

  leftButton.rawPressed = buttonIsPressed(&leftButton);
  leftButton.stablePressed = leftButton.rawPressed;
  leftButton.changedAt = millis();
  rightButton.rawPressed = buttonIsPressed(&rightButton);
  rightButton.stablePressed = rightButton.rawPressed;
  rightButton.changedAt = millis();
  dualPrintf("[flockyou] buttons ready left_gpio=1 level=%d right_gpio=42 level=%d\n",
             gpio_get_level(LEFT_BUTTON_PIN), gpio_get_level(RIGHT_BUTTON_PIN));
}

static bool pressedEdge(ButtonState* button) {
  bool pressed = buttonIsPressed(button);
  uint32_t now = millis();
  if (pressed != button->rawPressed) {
    button->rawPressed = pressed;
    button->changedAt = now;
    dualPrintf("[flockyou] button=%s raw=%s gpio=%d\n",
               button->name, pressed ? "pressed" : "released",
               gpio_get_level(button->pin));
  }
  if (pressed != button->stablePressed &&
      now - button->changedAt >= BUTTON_DEBOUNCE_MS) {
    button->stablePressed = pressed;
    return pressed;
  }
  return false;
}

static void resyncButtonAfterBlockingAudio(ButtonState* button) {
  const bool pressed = buttonIsPressed(button);
  button->rawPressed = pressed;
  button->stablePressed = pressed;
  button->changedAt = millis();
}

static void screenWake() {
  screenLastActivityAt = millis();
  if (!screenBacklightOn) {
    aipiDisplaySetBacklight(true);
    screenBacklightOn = true;
  }
}

static void fySaveSession();

static void screenSleepTick() {
  const uint8_t timeoutMinutes = screenTimeoutMinutes[screenTimeoutIndex];
  if (timeoutMinutes == 0 || !screenBacklightOn) return;
  if (millis() - screenLastActivityAt < timeoutMinutes * 60000UL) return;
  aipiDisplaySetBacklight(false);
  screenBacklightOn = false;
}

static void shutdownToDeepSleep() {
  dualPrintln("[flockyou] shutdown requested; saving state and entering deep sleep");
  if (fySpiffsReady && fyDirty) {
    fySaveSession();
  }
  sniffingStopped = true;
  wifiOk(esp_wifi_set_promiscuous(false), "shutdown_promiscuous");
  wifiOk(esp_wifi_stop(), "shutdown_wifi");
  aipiDisplaySetBacklight(false);
  screenBacklightOn = false;

  const uint64_t wakeMask = 1ULL << LEFT_BUTTON_PIN;
  const esp_err_t wakeResult = esp_sleep_enable_ext1_wakeup(
      wakeMask, ESP_EXT1_WAKEUP_ANY_LOW);
  if (wakeResult != ESP_OK) {
    dualPrintf("[flockyou] deep-sleep wake setup failed: %s (0x%x)\n",
               esp_err_to_name(wakeResult), static_cast<unsigned>(wakeResult));
    return;
  }
  delay(25);
  esp_deep_sleep_start();
}

static void handleButtons() {
  bool displayChanged = false;
  bool playVolumeSample = false;
  const bool leftWasPressed = leftButton.stablePressed;
  const bool leftPressed = pressedEdge(&leftButton);
  if (leftPressed) {
    leftButtonPressedAt = millis();
    leftButtonLongPressApplied = false;
    leftShutdownCountdownActive = false;
    leftShutdownCountdownShown = 0xff;
    screenWake();
  }

  const unsigned long now = millis();
  if (leftButton.stablePressed && !leftButtonLongPressApplied) {
    if (!leftShutdownCountdownActive && now - leftButtonPressedAt >= SHUTDOWN_HOLD_MS) {
      leftShutdownCountdownActive = true;
      leftShutdownCountdownStartedAt = now;
      leftShutdownCountdownShown = 3;
      aipiDisplayShowShutdownCountdown(leftShutdownCountdownShown);
      dualPrintln("[flockyou] shutdown countdown started");
    }
    if (leftShutdownCountdownActive) {
      const unsigned long elapsed = now - leftShutdownCountdownStartedAt;
      const uint8_t remaining = elapsed >= SHUTDOWN_COUNTDOWN_MS
                                    ? 0
                                    : static_cast<uint8_t>(3 - (elapsed / 1000));
      if (remaining != leftShutdownCountdownShown) {
        leftShutdownCountdownShown = remaining;
        aipiDisplayShowShutdownCountdown(remaining);
      }
      if (elapsed >= SHUTDOWN_COUNTDOWN_MS) {
        leftButtonLongPressApplied = true;
        dualPrintln("[flockyou] shutdown armed; release left button to sleep");
      }
    }
  }

  const bool leftReleased = leftWasPressed && !leftButton.stablePressed;
  if (leftReleased && leftButtonLongPressApplied) {
    // GPIO1 is also the active-low wake source. Waiting for release prevents
    // its held-low state from immediately waking the freshly sleeping chip.
    shutdownToDeepSleep();
  } else if (leftReleased && leftShutdownCountdownActive) {
    leftShutdownCountdownActive = false;
    dualPrintln("[flockyou] shutdown countdown cancelled");
    screenWake();
    displayChanged = true;
  } else if (leftReleased) {
    scanMode = scanMode == SCAN_ALL_CHANNELS ? SCAN_1_6_11 : SCAN_ALL_CHANNELS;
    selectFirstChannel();
    dualPrintf("[flockyou] scan mode=%s start_channel=%u\n",
               channelModeName(), currentChannel);
    screenWake();
    displayChanged = true;
  }

  const bool rightWasPressed = rightButton.stablePressed;
  const bool rightPressed = pressedEdge(&rightButton);
  if (rightPressed) {
    rightButtonPressedAt = millis();
    rightButtonNextCycleAt = 0;
    rightButtonLongPressApplied = false;
    screenWake();
  }

  if (rightButton.stablePressed && !rightButtonLongPressApplied &&
      now - rightButtonPressedAt >= SCREEN_TIMEOUT_HOLD_MS) {
    screenTimeoutIndex = (screenTimeoutIndex + 1) %
                         (sizeof(screenTimeoutMinutes) / sizeof(screenTimeoutMinutes[0]));
    rightButtonLongPressApplied = true;
    rightButtonNextCycleAt = now + SCREEN_TIMEOUT_CYCLE_MS;
    screenWake();
    aipiDisplayShowSleepSetting(screenTimeoutMinutes[screenTimeoutIndex]);
    dualPrintf("[flockyou] screen sleep selection=%s\n",
               screenTimeoutMinutes[screenTimeoutIndex] ?
                   (screenTimeoutMinutes[screenTimeoutIndex] == 1 ? "1_min" : "5_min") :
                   "never");
  } else if (rightButton.stablePressed && rightButtonLongPressApplied &&
             static_cast<long>(now - rightButtonNextCycleAt) >= 0) {
    screenTimeoutIndex = (screenTimeoutIndex + 1) %
                         (sizeof(screenTimeoutMinutes) / sizeof(screenTimeoutMinutes[0]));
    rightButtonNextCycleAt = now + SCREEN_TIMEOUT_CYCLE_MS;
    screenWake();
    aipiDisplayShowSleepSetting(screenTimeoutMinutes[screenTimeoutIndex]);
    dualPrintf("[flockyou] screen sleep selection=%s\n",
               screenTimeoutMinutes[screenTimeoutIndex] ?
                   (screenTimeoutMinutes[screenTimeoutIndex] == 1 ? "1_min" : "5_min") :
                   "never");
  }

  const bool rightReleased = rightWasPressed && !rightButton.stablePressed;
  if (rightReleased && rightButtonLongPressApplied) {
    screenWake();
    aipiDisplayConfirmSleepSetting(screenTimeoutMinutes[screenTimeoutIndex]);
    rightButtonLongPressApplied = false;
  } else if (rightReleased) {
    volumeIndex = (volumeIndex + 1) %
                  (sizeof(volumeLevels) / sizeof(volumeLevels[0]));
    outputVolumePercent = volumeLevels[volumeIndex];
    if (outputVolumePercent == 0) {
      dualPrintln("[flockyou] alert volume=MUTE");
    } else {
      dualPrintf("[flockyou] alert volume=%u%%\n", outputVolumePercent);
    }
    displayChanged = true;
    playVolumeSample = true;
  }

  if (displayChanged) {
    aipiDisplayShowScan(currentChannel, fyDetCount, fyEncounterCount,
                        scanMode == SCAN_ALL_CHANNELS, outputVolumePercent,
                        fyLastBogeySeenAt);
  }
  if (playVolumeSample && outputVolumePercent > 0) {
    if (!aipiAudioPlayVolumeSample(outputVolumePercent)) {
      dualPrintln("[flockyou] volume sample failed");
    }
    // Playback is intentionally bounded but synchronous. The user commonly
    // releases GPIO42 while the tone is playing, so restore its real state
    // before looking for the next press in the cycle.
    resyncButtonAfterBlockingAudio(&rightButton);
  }
}

static void updateBattery(bool force = false) {
  if (!batteryReady) return;
  const bool chargingChanged = aipiBatteryIsCharging() != batteryReading.charging;
  if (!force && !chargingChanged && millis() - lastBatteryRead < BATTERY_POLL_MS) return;

  AipiBatteryReading reading = {};
  const esp_err_t result = aipiBatteryRead(&reading);
  if (result != ESP_OK) {
    dualPrintf("[flockyou] battery read failed: %s (0x%x)\n",
               esp_err_to_name(result), (unsigned)result);
    lastBatteryRead = millis();
    return;
  }

  batteryReading = reading;
  lastBatteryRead = millis();
  aipiDisplaySetBattery(reading.percent, reading.charging);
  dualPrintf("[flockyou] battery=%lu.%03luV percent=%u charging=%d adc_calibrated=%d\n",
             (unsigned long)(reading.millivolts / 1000),
             (unsigned long)(reading.millivolts % 1000), reading.percent,
             reading.charging ? 1 : 0, reading.calibrated ? 1 : 0);
}

static inline uint16_t channelFreqMhz(uint8_t ch) {
  return (ch >= 1 && ch <= 14) ? (uint16_t)(2407 + 5 * ch) : 0;
}

static bool shouldSuppressDuplicate(const char* macStr) {
  unsigned long now = millis();
  for (size_t i = 0; i < DEDUPE_SLOTS; i++) {
    if (strcmp(dedupeTable[i].mac, macStr) == 0) {
      if ((now - dedupeTable[i].ts) < ALERT_COOLDOWN_MS) return true;
      dedupeTable[i].ts = now;
      return false;
    }
  }
  // Not found — insert into next slot
  strlcpy(dedupeTable[dedupeIdx].mac, macStr, 18);
  dedupeTable[dedupeIdx].ts = now;
  dedupeIdx = (dedupeIdx + 1) % DEDUPE_SLOTS;
  return false;
}

static void stopSniffing(const char* reason) {
  if (sniffingStopped) return;
  sniffingStopped = true;
  wifiOk(esp_wifi_set_promiscuous(false), "disable_promiscuous");
  dualPrintf("[flockyou] sniffing stopped: %s\n", reason);
}

static void applyInitialChannel() {
  selectFirstChannel();
}

static void updateChannelMode() {
  if (sniffingStopped) return;
  if (millis() - lastHop < CHANNEL_DWELL_MS) return;
  size_t count = 0;
  const uint8_t* channels = activeChannels(&count);
  channelIndex = (channelIndex + 1) % count;
  currentChannel = channels[channelIndex];
  setScanChannel(currentChannel);
  lastHop = millis();
}

static void printHeartbeat() {
  if (millis() - lastHeartbeat >= HEARTBEAT_MS) {
    uint32_t channelFrames = currentChannel <= 13 ? rxByChannel[currentChannel] : 0;
    dualPrintf("[flockyou] passive scan ch=%u mode=%s obs=%d rx=%lu mgmt=%lu data=%lu ch_rx=%lu drops=%lu hop_err=%lu battery=%u%% charging=%d\n",
               currentChannel, channelModeName(), fyDetCount,
               (unsigned long)rxFrames, (unsigned long)rxMgmtFrames,
               (unsigned long)rxDataFrames, (unsigned long)channelFrames,
               (unsigned long)queueDrops, (unsigned long)channelSwitchFailures,
               batteryReading.percent, batteryReading.charging ? 1 : 0);
    lastHeartbeat = millis();
  }
}

// ============================================================
// DETECTION TABLE OPS
// ============================================================

static const char* alertTypeToMethod(AlertType t) {
  switch (t) {
    case ALERT_OUI_ADDR2:      return "oui_addr2";
    case ALERT_OUI_ADDR1:      return "oui_addr1";
    case ALERT_OUI_ADDR3:      return "oui_addr3";
    case ALERT_SSID:           return "ssid";
    case ALERT_WILDCARD_PROBE: return "wildcard_probe";
    default:                   return "unknown";
  }
}

typedef struct {
  int index;
  bool encounter;
  bool newSensor;
} DetectionUpdate;

// An encounter is a first sighting or a return after ENCOUNTER_ABSENCE_MS.
// Packet repeats only refresh evidence for the existing sensor.
static DetectionUpdate fyAddDetection(const char* mac, const char* method,
                                      DetectionConfidence confidence, int8_t rssi,
                                      uint8_t ch, const char* ssid) {
  uint32_t now = millis();
  for (int i = 0; i < fyDetCount; i++) {
    if (strcasecmp(fyDet[i].mac, mac) == 0) {
      const bool encounter = (now - fyDet[i].lastSeen) >= ENCOUNTER_ABSENCE_MS;
      bool promoted = confidence > fyDet[i].confidence;
      if (fyDet[i].count < 0xFFFF) fyDet[i].count++;
      if (encounter && fyDet[i].encounters < 0xFFFF) {
        fyDet[i].encounters++;
        if (fyEncounterCount < 0xFFFF) fyEncounterCount++;
      }
      fyDet[i].lastSeen = now;
      fyDet[i].rssi     = rssi;
      fyDet[i].channel  = ch;
      if (promoted) {
        fyDet[i].confidence = confidence;
        strlcpy(fyDet[i].method, method ? method : "", sizeof(fyDet[i].method));
      }
      if (ssid && ssid[0] && !fyDet[i].ssid[0]) {
        strlcpy(fyDet[i].ssid, ssid, sizeof(fyDet[i].ssid));
      }
      fyDirty = true;
      return {i, encounter, false};
    }
  }
  if (fyDetCount >= MAX_DETECTIONS) {
    return {-1, false, false};
  }
  FYDetection& d = fyDet[fyDetCount];
  strlcpy(d.mac,    mac,                       sizeof(d.mac));
  strlcpy(d.method, method ? method : "",      sizeof(d.method));
  d.confidence = confidence;
  d.rssi      = rssi;
  d.channel   = ch;
  d.firstSeen = now;
  d.lastSeen  = now;
  d.count     = 1;
  d.encounters = 1;
  if (ssid && ssid[0]) strlcpy(d.ssid, ssid, sizeof(d.ssid));
  else                 d.ssid[0] = '\0';
  fyDetCount++;
  if (fyEncounterCount < 0xFFFF) fyEncounterCount++;
  fyDirty = true;
  return {fyDetCount - 1, true, true};
}

// ============================================================
// JSON ESCAPE  — only needed for SSIDs (user-controlled bytes)
// ============================================================

static size_t jsonEscape(char* dst, size_t cap, const char* src) {
  size_t o = 0;
  if (cap == 0) return 0;
  for (size_t i = 0; src[i]; i++) {
    char c = src[i];
    if (c == '"' || c == '\\') {
      if (o + 2 >= cap) break;
      dst[o++] = '\\'; dst[o++] = c;
    } else if ((unsigned char)c < 0x20) {
      if (o + 6 >= cap) break;
      int n = snprintf(dst + o, cap - o, "\\u%04x", (unsigned)(unsigned char)c);
      if (n <= 0 || (size_t)n >= cap - o) break;
      o += (size_t)n;
    } else {
      if (o + 1 >= cap) break;
      dst[o++] = c;
    }
  }
  dst[o] = '\0';
  return o;
}

// ============================================================
// CRC32  (zlib / SPIFFS-tool compatible polynomial 0xEDB88320)
// ============================================================

static uint32_t fyCRC32Update(uint32_t crc, const uint8_t* data, size_t len) {
  crc = ~crc;
  for (size_t i = 0; i < len; i++) {
    crc ^= data[i];
    for (int k = 0; k < 8; k++)
      crc = (crc >> 1) ^ (0xEDB88320u & -(int32_t)(crc & 1));
  }
  return ~crc;
}

// ============================================================
// SPIFFS SESSION PERSISTENCE  — bulletproof envelope format
// ============================================================
//
// Wire format on disk:
//   Line 1: {"v":1,"count":N,"bytes":B,"crc":"0xXXXXXXXX"}\n
//   Line 2+: [{"mac":...},...]     (exactly B bytes, CRC32 == X)
//
// Atomic write procedure:
//   1. Compute payload size + CRC (pass 1)
//   2. Write envelope + payload to /session.tmp (pass 2)
//   3. Re-validate /session.tmp from disk
//   4. Remove /session.json, rename tmp → main (with copy+delete fallback)
//
// Boot-time recovery:
//   - Try /session.json. If missing or CRC-invalid, try /session.tmp.
//   - Copy whichever validates to /prev_session.json, then delete both.

static size_t fySerializeDet(const FYDetection& d, char* dst, size_t cap) {
  char ssidEsc[sizeof(d.ssid) * 6 + 1];
  jsonEscape(ssidEsc, sizeof(ssidEsc), d.ssid);
  int n = snprintf(dst, cap,
      "{\"mac\":\"%s\",\"method\":\"%s\",\"confidence\":\"%s\",\"rssi\":%d,\"channel\":%u,"
      "\"first\":%lu,\"last\":%lu,\"count\":%u,\"observations\":%u,"
      "\"encounters\":%u,\"ssid\":\"%s\"}",
      d.mac, d.method, confidenceName(d.confidence), d.rssi, (unsigned)d.channel,
      (unsigned long)d.firstSeen, (unsigned long)d.lastSeen, (unsigned)d.count,
      (unsigned)d.count, (unsigned)d.encounters, ssidEsc);
  return (n > 0 && (size_t)n < cap) ? (size_t)n : 0;
}

static uint32_t fyComputePayloadCRC(size_t& outBytes) {
  char line[384];
  uint32_t crc = 0;
  outBytes = 0;
  crc = fyCRC32Update(crc, (const uint8_t*)"[", 1); outBytes += 1;
  for (int i = 0; i < fyDetCount; i++) {
    if (i > 0) { crc = fyCRC32Update(crc, (const uint8_t*)",", 1); outBytes += 1; }
    size_t n = fySerializeDet(fyDet[i], line, sizeof(line));
    if (n == 0) continue;
    crc = fyCRC32Update(crc, (const uint8_t*)line, n);
    outBytes += n;
  }
  crc = fyCRC32Update(crc, (const uint8_t*)"]", 1); outBytes += 1;
  return crc;
}

// Minimal envelope parser: pulls bytes + crc fields by substring search.
// Robust to field reordering; rejects anything without both required keys.
static bool fyParseEnvelope(const char* hdr, size_t& outBytes, uint32_t& outCrc) {
  const char* b = strstr(hdr, "\"bytes\":");
  const char* c = strstr(hdr, "\"crc\":\"0x");
  if (!b || !c) return false;
  b += 8;
  long long bv = 0;
  if (sscanf(b, "%lld", &bv) != 1 || bv < 0) return false;
  c += 9;
  unsigned cv = 0;
  if (sscanf(c, "%x", &cv) != 1) return false;
  outBytes = (size_t)bv;
  outCrc   = (uint32_t)cv;
  return true;
}

static bool fyValidateSessionFile(const char* path) {
  if (!SPIFFS.exists(path)) return false;
  File f = SPIFFS.open(path, "r");
  if (!f) return false;

  String hdr = f.readStringUntil('\n');
  if (hdr.length() < 10 || hdr[0] != '{') { f.close(); return false; }

  size_t   expectedBytes = 0;
  uint32_t expectedCRC   = 0;
  if (!fyParseEnvelope(hdr.c_str(), expectedBytes, expectedCRC)) {
    f.close(); return false;
  }

  size_t bodyOffset = hdr.length() + 1;
  size_t fileSize   = f.size();
  if (fileSize < bodyOffset + expectedBytes) { f.close(); return false; }
  if ((fileSize - bodyOffset) != expectedBytes) { f.close(); return false; }

  uint8_t buf[256];
  uint32_t crc = 0;
  size_t remaining = expectedBytes;
  while (remaining > 0) {
    int n = f.read(buf, remaining < sizeof(buf) ? remaining : sizeof(buf));
    if (n <= 0) break;
    crc = fyCRC32Update(crc, buf, (size_t)n);
    remaining -= (size_t)n;
  }
  f.close();
  return (remaining == 0 && crc == expectedCRC);
}

static bool fySpiffsCopy(const char* src, const char* dst) {
  File s = SPIFFS.open(src, "r");
  if (!s) return false;
  File d = SPIFFS.open(dst, "w");
  if (!d) { s.close(); return false; }
  uint8_t buf[256];
  int n;
  bool ok = true;
  while ((n = s.read(buf, sizeof(buf))) > 0) {
    if (d.write(buf, (size_t)n) != (size_t)n) { ok = false; break; }
  }
  s.close();
  d.close();
  return ok;
}

static bool fyAtomicPromote(const char* src, const char* dst) {
  if (SPIFFS.rename(src, dst)) return true;
  if (!fySpiffsCopy(src, dst)) return false;
  SPIFFS.remove(src);
  return true;
}

static void fySaveSession() {
  if (!fySpiffsReady) return;
  if (!fyDirty && fyDetCount == fyLastSaveCount) return;

  size_t   payloadBytes = 0;
  uint32_t crc          = fyComputePayloadCRC(payloadBytes);
  int      savedCount   = fyDetCount;

  File f = SPIFFS.open(FY_SESSION_TMP, "w");
  if (!f) {
    dualPrintf("[flockyou] save failed: cannot open %s\n", FY_SESSION_TMP);
    return;
  }
  f.printf("{\"v\":1,\"count\":%d,\"bytes\":%u,\"crc\":\"0x%08lX\"}\n",
           savedCount, (unsigned)payloadBytes, (unsigned long)crc);

  char line[384];
  size_t wrote = 0;
  f.write((uint8_t*)"[", 1); wrote++;
  for (int i = 0; i < fyDetCount; i++) {
    if (i > 0) { f.write((uint8_t*)",", 1); wrote++; }
    size_t n = fySerializeDet(fyDet[i], line, sizeof(line));
    if (n == 0) continue;
    f.write((uint8_t*)line, n);
    wrote += n;
  }
  f.write((uint8_t*)"]", 1); wrote++;
  f.close();

  if (wrote != payloadBytes) {
    dualPrintf("[flockyou] save WARNING: wrote %u expected %u — aborting\n",
               (unsigned)wrote, (unsigned)payloadBytes);
    return;
  }

  if (!fyValidateSessionFile(FY_SESSION_TMP)) {
    dualPrintf("[flockyou] save verify FAILED — old session preserved\n");
    return;
  }

  SPIFFS.remove(FY_SESSION_FILE);
  if (!fyAtomicPromote(FY_SESSION_TMP, FY_SESSION_FILE)) {
    dualPrintf("[flockyou] promote FAILED — data in %s for recovery\n", FY_SESSION_TMP);
    return;
  }

  fyLastSaveAt    = millis();
  fyLastSaveCount = savedCount;
  fyDirty         = false;
  dualPrintf("[flockyou] session saved: %d det, %u bytes, crc=0x%08lX\n",
             savedCount, (unsigned)payloadBytes, (unsigned long)crc);
}

// Promote any valid session file from last boot into /prev_session.json, then
// start this boot with a fresh empty table. Preserves history across power cycles.
static void fyPromotePrevSession() {
  if (!fySpiffsReady) return;

  const char* source = nullptr;
  if      (fyValidateSessionFile(FY_SESSION_FILE)) source = FY_SESSION_FILE;
  else if (fyValidateSessionFile(FY_SESSION_TMP))  source = FY_SESSION_TMP;

  if (!source) {
    if (SPIFFS.exists(FY_SESSION_FILE)) SPIFFS.remove(FY_SESSION_FILE);
    if (SPIFFS.exists(FY_SESSION_TMP))  SPIFFS.remove(FY_SESSION_TMP);
    dualPrintln("[flockyou] no valid prior session to promote");
    return;
  }

  if (!fySpiffsCopy(source, FY_PREV_FILE)) {
    dualPrintf("[flockyou] failed to promote %s → %s\n", source, FY_PREV_FILE);
    return;
  }
  if (SPIFFS.exists(FY_SESSION_FILE)) SPIFFS.remove(FY_SESSION_FILE);
  if (SPIFFS.exists(FY_SESSION_TMP))  SPIFFS.remove(FY_SESSION_TMP);

  File v = SPIFFS.open(FY_PREV_FILE, "r");
  size_t sz = v ? v.size() : 0;
  if (v) v.close();
  dualPrintf("[flockyou] prior session promoted from %s (%u bytes)\n",
             source, (unsigned)sz);
}

// ============================================================
// USB JSON EXPORT
// ============================================================
//
// The dashboard and a terminal-connected computer read one JSON object per
// line from USB CDC. Each record retains the radio fingerprint and its latest
// on-device state; commands below export the whole current session on demand.
//
// GPS is handled Flask-side via its own USB NMEA puck or browser geolocation;
// we don't embed GPS here because there's no on-device AP / phone link.

static void emitDetectionJSON(const char* event, const FYDetection& d,
                              const char* frameKind) {
  char ssidEsc[sizeof(d.ssid) * 6 + 1];
  jsonEscape(ssidEsc, sizeof(ssidEsc), d.ssid);
  char oui[9];
  uint8_t mbytes[6] = {0};
  sscanf(d.mac, "%hhx:%hhx:%hhx:%hhx:%hhx:%hhx",
         &mbytes[0], &mbytes[1], &mbytes[2], &mbytes[3], &mbytes[4], &mbytes[5]);
  ouiFromMac(mbytes, oui, sizeof(oui));

  dualPrintf(
      "{\"event\":\"%s\","
      "\"schema\":\"flock-you-go.usb.v2\","
      "\"uptime_ms\":%lu,"
      "\"first_seen_ms\":%lu,"
      "\"last_seen_ms\":%lu,"
      "\"fingerprint_id\":\"wifi_%s:%s\","
      "\"detection_method\":\"wifi_%s\","
      "\"confidence\":\"%s\","
      "\"protocol\":\"wifi_2_4ghz\","
      "\"mac_address\":\"%s\","
      "\"oui\":\"%s\","
      "\"device_name\":\"\","
      "\"rssi\":%d,"
      "\"channel\":%u,"
      "\"frequency\":%u,"
      "\"count\":%u,\"observations\":%u,\"encounters\":%u,"
      "\"frame_kind\":\"%s\","
      "\"ssid\":\"%s\"}\n",
      event, (unsigned long)millis(), (unsigned long)d.firstSeen,
      (unsigned long)d.lastSeen, d.method, d.mac, d.method,
      confidenceName(d.confidence), d.mac, oui, d.rssi,
      (unsigned)d.channel, (unsigned)channelFreqMhz(d.channel),
      (unsigned)d.count, (unsigned)d.count, (unsigned)d.encounters,
      frameKind ? frameKind : "", ssidEsc);
}

static void emitUsbStatusJSON() {
  dualPrintf(
      "{\"event\":\"status\",\"schema\":\"flock-you-go.usb.v2\","
      "\"uptime_ms\":%lu,\"session_detections\":%d,\"unique_sensors\":%d,"
      "\"encounters\":%u,\"scan_mode\":\"%s\","
      "\"channel\":%u,\"scanner_active\":%s,\"queue_drops\":%lu,"
      "\"battery_percent\":%u,\"charging\":%s}\n",
      (unsigned long)millis(), fyDetCount, fyDetCount, (unsigned)fyEncounterCount,
      channelModeName(), currentChannel,
      sniffingStopped ? "false" : "true", (unsigned long)queueDrops,
      batteryReading.percent, batteryReading.charging ? "true" : "false");
}

static void emitUsbSnapshot() {
  if (fySpiffsReady && fyDirty) fySaveSession();
  dualPrintf("{\"event\":\"snapshot_begin\",\"schema\":\"flock-you-go.usb.v2\","
             "\"uptime_ms\":%lu,\"sensors\":%d,\"encounters\":%u}\n",
             (unsigned long)millis(), fyDetCount, (unsigned)fyEncounterCount);
  for (int i = 0; i < fyDetCount; i++) {
    emitDetectionJSON("snapshot", fyDet[i], "");
  }
  dualPrintf("{\"event\":\"snapshot_end\",\"schema\":\"flock-you-go.usb.v2\","
             "\"uptime_ms\":%lu,\"sensors\":%d,\"encounters\":%u}\n",
             (unsigned long)millis(), fyDetCount, (unsigned)fyEncounterCount);
}

static void usbCommandTick() {
  static char command[16] = {};
  static size_t length = 0;
  while (Serial.available() > 0) {
    const char c = static_cast<char>(Serial.read());
    if (c == '\r') continue;
    if (c == '\n') {
      command[length] = '\0';
      for (size_t i = 0; i < length; i++) command[i] = toupper(command[i]);
      if (strcmp(command, "DUMP") == 0) {
        emitUsbSnapshot();
      } else if (strcmp(command, "STATUS") == 0) {
        emitUsbStatusJSON();
      } else if (strcmp(command, "SAVE") == 0) {
        fySaveSession();
        dualPrintln("{\"event\":\"save_complete\",\"schema\":\"flock-you-go.usb.v2\"}");
      } else if (strcmp(command, "HELP") == 0) {
        dualPrintln("[flockyou] USB commands: DUMP, STATUS, SAVE, HELP");
      } else if (length != 0) {
        dualPrintln("[flockyou] unknown USB command; send HELP");
      }
      length = 0;
      continue;
    }
    if (length + 1 < sizeof(command) && c >= 0x20 && c <= 0x7e) {
      command[length++] = c;
    }
  }
}

// ============================================================
// PROMISCUOUS CALLBACK  — keep it fast, no Serial, no malloc
// ============================================================

static bool IRAM_ATTR extractSsidFromMgmtBody(const uint8_t* body, int len,
                                     char* outSsid, size_t outLen) {
  if (!body || len <= 0 || !outSsid || outLen == 0) return false;
  while (len >= 2) {
    uint8_t id = body[0], elen = body[1];
    if ((int)elen + 2 > len) break;
    if (id == 0) {
      size_t n = (elen < (outLen - 1)) ? elen : (outLen - 1);
      memcpy(outSsid, body + 2, n);
      outSsid[n] = '\0';
      return true;
    }
    body += elen + 2; len -= elen + 2;
  }
  return false;
}

// Returns:
//   1  = wildcard SSID IE found (tag 0, length 0)  → Flock-style probe
//   0  = SSID IE found, non-zero length            → directed probe, not ours
//  -1  = no SSID IE found at all                   → caller should retry with
//                                                    FCS-stripped length, then bail
static int IRAM_ATTR isWildcardProbeIE(const uint8_t* body, int len) {
  if (!body || len < 2) return -1;
  while (len >= 2) {
    uint8_t id   = body[0];
    uint8_t elen = body[1];
    if ((int)elen + 2 > len) break;
    if (id == 0) return (elen == 0) ? 1 : 0;
    body += elen + 2;
    len  -= elen + 2;
  }
  return -1;
}

static void IRAM_ATTR wifiSniffer(void* buf, wifi_promiscuous_pkt_type_t type) {
  if (!buf || sniffingStopped) return;

#if PROCESS_MGMT_FRAMES && PROCESS_DATA_FRAMES
  if (type != WIFI_PKT_MGMT && type != WIFI_PKT_DATA) return;
#elif PROCESS_MGMT_FRAMES
  if (type != WIFI_PKT_MGMT) return;
#elif PROCESS_DATA_FRAMES
  if (type != WIFI_PKT_DATA) return;
#else
  return;  // nothing configured to process
#endif

  wifi_promiscuous_pkt_t*      pkt = (wifi_promiscuous_pkt_t*)buf;
  if (pkt->rx_ctrl.sig_len < sizeof(wifi_ieee80211_mac_hdr_t)) return;
  wifi_ieee80211_mac_hdr_t*    hdr = (wifi_ieee80211_mac_hdr_t*)pkt->payload;
  int8_t rssi = pkt->rx_ctrl.rssi;
  uint8_t ch = (uint8_t)pkt->rx_ctrl.channel;  // actual rx channel from driver
  rxFrames++;
  if (type == WIFI_PKT_MGMT) rxMgmtFrames++;
  if (type == WIFI_PKT_DATA) rxDataFrames++;
  if (ch <= 13) rxByChannel[ch]++;

  if (rssi < RSSI_MIN) return;

  // --- OUI check: addr2 (transmitter/source) ---
  //
  // For mgmt Probe Requests (type=0 subtype=4) from a matched OUI, tighten
  // to the DeFlockJoplin wildcard-probe signature: SSID IE (tag 0) length
  // must be zero. This reduces false positives dramatically (Michael's field
  // test: 11/12 true-positive with only 2 false-positives in Joplin).
  //
  // Non-probe frames from the same OUI still emit the broad ADDR2 alert.
  // See: https://github.com/DeflockJoplin/flock-you
  if (matchOuiRaw(hdr->addr2)) {
    bool emitted = false;
    if (type == WIFI_PKT_MGMT) {
      uint8_t fc0     = hdr->frame_ctrl & 0xFF;
      uint8_t ftype   = (fc0 >> 2) & 0x03;
      uint8_t subtype = (fc0 >> 4) & 0x0F;
      if (ftype == 0 && subtype == 4) {                        // Probe Request
        int sigLen  = (int)pkt->rx_ctrl.sig_len;
        int bodyLen = sigLen - (int)sizeof(wifi_ieee80211_mac_hdr_t);
        const uint8_t* body = pkt->payload + sizeof(wifi_ieee80211_mac_hdr_t);
        int r = (bodyLen > 0) ? isWildcardProbeIE(body, bodyLen) : -1;
        // FCS-trailer retry: only when the first parse found no SSID IE AT
        // ALL (-1). A found-but-nonzero (0) means legit directed probe; do
        // not retry — it would mis-classify.
        if (r == -1 && bodyLen > 4) r = isWildcardProbeIE(body, bodyLen - 4);
        if (r == 1) {
          enqueueAlert(ALERT_WILDCARD_PROBE, CONFIDENCE_HIGH,
                       hdr->addr2, rssi, ch,
                       nullptr, "probe_req");
          emitted = true;
        }
      }
    }
    if (!emitted) {
      DetectionConfidence confidence =
          (type == WIFI_PKT_MGMT) ? CONFIDENCE_MEDIUM : CONFIDENCE_LOW;
      enqueueAlert(ALERT_OUI_ADDR2, confidence,
                   hdr->addr2, rssi, ch, nullptr,
                   (type == WIFI_PKT_MGMT) ? "mgmt_addr2" : "data_addr2");
    }
  }

#if CHECK_ADDR1
  // addr1 (receiver/destination): catches Flock STAs that appear only as the
  // dst of probe responses and data frames — never transmitting in the capture
  // window due to their burst-sleep duty cycle. Multicast guard is mandatory
  // here since addr1 is broadcast (ff:ff:ff:ff:ff:ff) in beacons/broadcasts.
  if (!isMulticast(hdr->addr1) && matchOuiRaw(hdr->addr1)) {
    enqueueAlert(ALERT_OUI_ADDR1, CONFIDENCE_LOW,
                 hdr->addr1, rssi, ch, nullptr, "addr1");
  }
#endif

#if CHECK_ADDR3
  // addr3 fallback: catches cases where addr2 is randomised but addr3
  // carries the real BSSID OUI (management frames only).
  if (type == WIFI_PKT_MGMT && matchOuiRaw(hdr->addr3)) {
    enqueueAlert(ALERT_OUI_ADDR3, CONFIDENCE_LOW,
                 hdr->addr3, rssi, ch, nullptr, "addr3");
  }
#endif

#if ENABLE_SSID_MATCH
  if (type == WIFI_PKT_MGMT) {
    uint8_t fc0     = hdr->frame_ctrl & 0xFF;
    uint8_t subtype = (fc0 >> 4) & 0x0F;
    uint8_t ftype   = (fc0 >> 2) & 0x03;

    if (ftype == 0) {
      int sigLen = pkt->rx_ctrl.sig_len - 4;  // strip 4-byte FCS
      if (sigLen < (int)sizeof(wifi_ieee80211_mac_hdr_t)) return;

      const uint8_t* mgmtBody    = nullptr;
      int            mgmtBodyLen = 0;
      const char*    frameKind   = nullptr;

      if (subtype == 8 || subtype == 5) {
        // Beacon / Probe Response: fixed params = 12 bytes after MAC hdr
        int off = sizeof(wifi_ieee80211_mac_hdr_t) + 12;
        if (sigLen > off) {
          frameKind   = (subtype == 8) ? "beacon" : "probe_resp";
          mgmtBody    = pkt->payload + off;
          mgmtBodyLen = sigLen - off;
        }
      } else if (subtype == 4) {
        // Probe Request: IEs follow directly after MAC hdr
        int off = sizeof(wifi_ieee80211_mac_hdr_t);
        if (sigLen > off) {
          frameKind   = "probe_req";
          mgmtBody    = pkt->payload + off;
          mgmtBodyLen = sigLen - off;
        }
      }

      if (mgmtBody && mgmtBodyLen > 0) {
        char ssid[33] = {0};
        if (extractSsidFromMgmtBody(mgmtBody, mgmtBodyLen, ssid, sizeof(ssid))) {
          if (matchSsidKeyword(ssid)) {
            enqueueAlert(ALERT_SSID, CONFIDENCE_MEDIUM,
                         hdr->addr2, rssi, ch, ssid, frameKind);
          }
        }
      }
    }
  }
#endif
}

// ============================================================
// DRAIN QUEUE — called from loop(), safe to Serial.print here
// ============================================================

static void drainAlertQueue() {
  while (true) {
    portENTER_CRITICAL(&queueMux);
    if (alertTail == alertHead) { portEXIT_CRITICAL(&queueMux); break; }
    AlertEntry e;
    memcpy(&e, (const void*)&alertQueue[alertTail], sizeof(AlertEntry));
    alertTail = (alertTail + 1) % ALERT_QUEUE_SIZE;
    portEXIT_CRITICAL(&queueMux);

    char macStr[18];
    macToStr(e.mac, macStr, sizeof(macStr));
    const char* method = alertTypeToMethod(e.type);

    // Always update the on-device detection table (survives reboot via SPIFFS).
    const DetectionUpdate update = fyAddDetection(
        macStr, method, e.confidence, e.rssi, e.channel,
        (e.type == ALERT_SSID) ? e.ssid : nullptr);
    const int idx = update.index;

    // Only corroborated observations drive the physical-alert heartbeat.
    // Update before serial dedupe so repeated medium/high frames keep it alive.
    if (e.confidence >= CONFIDENCE_MEDIUM) fyLastTargetSeen = millis();
    fyLastBogeySeenAt = millis();

    // Raw observations are retained silently. Only a new encounter is an
    // operator alert, and the output gate remains a final burst safeguard.
    if (!update.encounter || shouldSuppressDuplicate(macStr)) continue;

    // Human-readable line (for serial terminal / mirror).
    char oui[9];
    ouiFromMac(e.mac, oui, sizeof(oui));
    if (e.type == ALERT_SSID) {
      dualPrintf("[flockyou] DETECT-SSID confidence=%s type=%s mac=%s ssid=\"%s\" rssi=%d ch=%u count=%d\n",
                 confidenceName(e.confidence), e.frameKind, macStr, e.ssid,
                 e.rssi, e.channel,
                 (idx >= 0) ? (int)fyDet[idx].count : 0);
    } else {
      dualPrintf("[flockyou] %s-OUI confidence=%s mac=%s oui=%s rssi=%d ch=%u addr=%s count=%d\n",
                 e.confidence == CONFIDENCE_LOW ? "OBSERVE" : "DETECT",
                 confidenceName(e.confidence), macStr, oui, e.rssi, e.channel,
                 e.frameKind[0] ? e.frameKind : "addr2",
                 (idx >= 0) ? (int)fyDet[idx].count : 0);
    }

    screenWake();
    aipiDisplayShowDetection(oui, e.rssi, e.channel, fyEncounterCount);

    // USB JSON line contains the updated persistent fingerprint record.
    if (idx >= 0) {
      emitDetectionJSON("detection", fyDet[idx], e.frameKind);
    }

    // Every accepted alert is audible. The per-MAC cooldown above bounds this
    // to one chirp per five minutes, preventing a stationary sensor from
    // sounding continuously while the operator remains nearby.
    if (outputVolumePercent > 0) {
      const bool played = aipiAudioPlayNewDetection(outputVolumePercent);
      dualPrintf("[flockyou] alert audio mac=%s volume=%u%% result=%s\n",
                 macStr, outputVolumePercent, played ? "ok" : "failed");
      // Reset the heartbeat phase so it does not overlap a fresh alert chirp.
      fyLastHeartbeatAt = millis();
    } else {
      dualPrintf("[flockyou] alert audio mac=%s volume=MUTE\n", macStr);
    }
    if (e.confidence >= CONFIDENCE_MEDIUM) ledFlash(LED_FLASH_MS);

#if STOP_ON_OUI_HIT
    if (e.type != ALERT_SSID) stopSniffing("OUI hit");
#endif
#if STOP_ON_SSID_HIT
    if (e.type == ALERT_SSID) stopSniffing("SSID hit");
#endif
  }
}

// ============================================================
// AUTOSAVE
// ============================================================

static void autosaveTick() {
  if (!fySpiffsReady || !fyDirty) return;
  if (millis() - fyLastSaveAt < AUTOSAVE_INTERVAL_MS) return;
  fySaveSession();
}

// Heartbeat beep while at least one target was seen in the last
// HB_DEVICE_ACTIVE_MS. Fires HB_BEEP_INTERVAL_MS apart.
static void heartbeatTick() {
  if (fyLastTargetSeen == 0) return;                           // never seen one
  unsigned long now = millis();
  if (now - fyLastTargetSeen > HB_DEVICE_ACTIVE_MS) return;    // gone silent
  if (now - fyLastHeartbeatAt < HB_BEEP_INTERVAL_MS) return;   // too soon
  heartbeatBeep();
  fyLastHeartbeatAt = now;
}

// ============================================================
// SETUP / LOOP
// ============================================================

void setup() {
  Serial.begin(115200);
  // Crucial for USB-optional operation: without this, Serial.write() will
  // block indefinitely on an ESP32-S3 USB-CDC port when no host is attached.
  Serial.setTxTimeoutMs(0);
  const esp_err_t batteryInit = aipiBatteryBegin();
  batteryReady = batteryInit == ESP_OK;
  delay(300);

  const bool displayReady = aipiDisplayBegin();
  initButtons();
  const bool audioReady = aipiAudioBegin();
  dualPrintf("[flockyou] ES8311 audio=%s i2c=0x18 amp_gpio=9 i2s=16kHz\n",
             audioReady ? "ready" : "failed");
  if (batteryReady) {
    updateBattery(true);
  } else {
    dualPrintf("[flockyou] battery setup failed: %s (0x%x)\n",
               esp_err_to_name(batteryInit), (unsigned)batteryInit);
  }

#if MIRROR_SERIAL
  Serial1.begin(115200, SERIAL_8N1, -1, 43);
#endif

#if USE_LED
  #error "Implement the GPIO46 WS2812 driver before enabling USE_LED"
#endif

  startupBeep();
#if USE_LED
  ledFlash(200);
#endif

  precompileOuis();
  memset(dedupeTable, 0, sizeof(dedupeTable));

  // SPIFFS — format on first boot if missing. Non-fatal if it fails.
  if (SPIFFS.begin(true)) {
    fySpiffsReady = true;
    dualPrintln("[flockyou] SPIFFS ready");
    fyPromotePrevSession();
  } else {
    dualPrintln("[flockyou] SPIFFS init FAILED — running without persistence");
  }

  wifi_init_config_t cfg = WIFI_INIT_CONFIG_DEFAULT();
  cfg.static_rx_buf_num  = 6;
  cfg.dynamic_rx_buf_num = 6;
  cfg.csi_enable         = false;
  cfg.ampdu_rx_enable    = false;
  cfg.ampdu_tx_enable    = false;
  cfg.amsdu_tx_enable    = false;
  cfg.nvs_enable         = false;
  cfg.rx_ba_win          = 6;

  if (!wifiOk(esp_wifi_init(&cfg), "init") ||
      !wifiOk(esp_wifi_set_storage(WIFI_STORAGE_RAM), "set_storage") ||
      !wifiOk(esp_wifi_set_mode(WIFI_MODE_NULL), "set_mode_null") ||
      !wifiOk(esp_wifi_start(), "start")) {
    dualPrintln("[flockyou] WiFi startup failed; passive scanner disabled");
    sniffingStopped = true;
    return;
  }

  applyInitialChannel();

  wifi_promiscuous_filter_t filt = {
    .filter_mask = 0
#if PROCESS_MGMT_FRAMES
        | WIFI_PROMIS_FILTER_MASK_MGMT
#endif
#if PROCESS_DATA_FRAMES
        | WIFI_PROMIS_FILTER_MASK_DATA
#endif
  };
  if (!wifiOk(esp_wifi_set_promiscuous_filter(&filt), "set_promiscuous_filter") ||
      !wifiOk(esp_wifi_set_promiscuous_rx_cb(&wifiSniffer), "set_promiscuous_callback") ||
      !wifiOk(esp_wifi_set_promiscuous(true), "enable_promiscuous")) {
    dualPrintln("[flockyou] promiscuous receive setup failed; scanner disabled");
    sniffingStopped = true;
    return;
  }

  dualPrintln("[flockyou] passive-only WiFi detector started (no association, active scan, or probe transmission)");
  dualPrintf("[flockyou] AIPI display=%s madctl=0x68 offsets=0,0 inversion=off\n",
             displayReady ? "ready" : "failed");
  dualPrintf("[flockyou] mode=%s dwell_ms=%u start_channel=%u rssi_min=%d low_conf_rssi_min=%d spiffs=%d\n",
                channelModeName(), CHANNEL_DWELL_MS, currentChannel,
                RSSI_MIN, RSSI_LOW_CONFIDENCE_MIN, fySpiffsReady ? 1 : 0);

  lastHeartbeat = millis();
  fyLastSaveAt  = millis();
  screenLastActivityAt = millis();
}

void loop() {
  usbCommandTick();
  handleButtons();
  updateBattery();
  updateChannelMode();
  drainAlertQueue();   // Serial.printf happens here, not in callback
  autosaveTick();      // periodic SPIFFS write if dirty
  heartbeatTick();     // audible beep-pair while a target is still in range
  ledTick();           // turn off LED after LED_FLASH_MS
  aipiDisplayTick(currentChannel, fyDetCount, fyEncounterCount,
                  scanMode == SCAN_ALL_CHANNELS, outputVolumePercent,
                  fyLastBogeySeenAt);
  screenSleepTick();
  printHeartbeat();
  delay(1);
}
