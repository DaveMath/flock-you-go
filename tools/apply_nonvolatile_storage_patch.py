#!/usr/bin/env python3
"""Patch main.cpp so saved Flock-You Go sessions survive reboot/deep sleep exports.

This is intentionally a deterministic source patcher instead of a broad regex
rewrite. It changes three things:

1. Reduces autosave from 60 seconds to 10 seconds.
2. Adds USB export of /prev_session.json, where the firmware stores the last
   boot's saved session.
3. Adds DUMP_PREV and updates HELP.

Run from the repository root:

    python3 tools/apply_nonvolatile_storage_patch.py

Then build/flash with PlatformIO.
"""

from __future__ import annotations

from pathlib import Path


ROOT = Path(__file__).resolve().parents[1]
MAIN = ROOT / "main.cpp"

STORED_SESSION_HELPER = r'''
static void emitStoredSessionFile(const char* eventName, const char* path) {
  if (!fySpiffsReady || !SPIFFS.exists(path) || !fyValidateSessionFile(path)) {
    dualPrintf(
        "{\"event\":\"%s_missing\",\"schema\":\"flock-you-go.usb.v2\","
        "\"uptime_ms\":%lu,\"path\":\"%s\"}\n",
        eventName, (unsigned long)millis(), path);
    Serial.flush();
    return;
  }

  File f = SPIFFS.open(path, "r");
  if (!f) {
    dualPrintf(
        "{\"event\":\"%s_open_failed\",\"schema\":\"flock-you-go.usb.v2\","
        "\"uptime_ms\":%lu,\"path\":\"%s\"}\n",
        eventName, (unsigned long)millis(), path);
    Serial.flush();
    return;
  }

  String envelope = f.readStringUntil('\n');
  dualPrintf(
      "{\"event\":\"%s_begin\",\"schema\":\"flock-you-go.usb.v2\","
      "\"uptime_ms\":%lu,\"path\":\"%s\",\"envelope\":%s}\n",
      eventName, (unsigned long)millis(), path, envelope.c_str());
  Serial.flush();

  bool inString = false;
  bool escaped = false;
  int depth = 0;
  char obj[512];
  size_t pos = 0;

  while (f.available()) {
    char c = (char)f.read();

    if (depth == 0) {
      if (c == '{') {
        depth = 1;
        pos = 0;
        obj[pos++] = c;
        inString = false;
        escaped = false;
      }
      continue;
    }

    if (pos + 1 < sizeof(obj)) {
      obj[pos++] = c;
    }

    if (escaped) {
      escaped = false;
      continue;
    }
    if (c == '\\' && inString) {
      escaped = true;
      continue;
    }
    if (c == '"') {
      inString = !inString;
      continue;
    }
    if (!inString) {
      if (c == '{') depth++;
      else if (c == '}') depth--;
    }

    if (depth == 0) {
      obj[pos] = '\0';
      dualPrintf(
          "{\"event\":\"%s_snapshot\",\"schema\":\"flock-you-go.usb.v2\","
          "\"uptime_ms\":%lu,\"path\":\"%s\",\"record\":%s}\n",
          eventName, (unsigned long)millis(), path, obj);
      Serial.flush();
      pos = 0;
    }
  }

  f.close();
  dualPrintf(
      "{\"event\":\"%s_end\",\"schema\":\"flock-you-go.usb.v2\","
      "\"uptime_ms\":%lu,\"path\":\"%s\"}\n",
      eventName, (unsigned long)millis(), path);
  Serial.flush();
}

'''.lstrip()

OLD_SNAPSHOT = r'''static void emitUsbSnapshot() {
  if (fySpiffsReady && fyDirty) fySaveSession();
  // Regular logs must never stall a USB-optional device, but a requested dump
  // is an explicit export transaction. Allow the CDC TX queue to drain after
  // each record so JSON framing stays intact.
  Serial.setTxTimeoutMs(100);
  dualPrintf("{\"event\":\"snapshot_begin\",\"schema\":\"flock-you-go.usb.v2\","
             "\"uptime_ms\":%lu,\"sensors\":%d,\"encounters\":%u}\n",
             (unsigned long)millis(), fyDetCount, (unsigned)fyEncounterCount);
  Serial.flush();
  for (int i = 0; i < fyDetCount; i++) {
    emitDetectionJSON("snapshot", fyDet[i], "");
    Serial.flush();
  }
  dualPrintf("{\"event\":\"snapshot_end\",\"schema\":\"flock-you-go.usb.v2\","
             "\"uptime_ms\":%lu,\"sensors\":%d,\"encounters\":%u}\n",
             (unsigned long)millis(), fyDetCount, (unsigned)fyEncounterCount);
  Serial.flush();
  Serial.setTxTimeoutMs(0);
}
'''

NEW_SNAPSHOT = r'''static void emitUsbSnapshot() {
  if (fySpiffsReady && fyDirty) fySaveSession();
  // Regular logs must never stall a USB-optional device, but a requested dump
  // is an explicit export transaction. Allow the CDC TX queue to drain after
  // each record so JSON framing stays intact.
  Serial.setTxTimeoutMs(100);
  dualPrintf("{\"event\":\"snapshot_begin\",\"schema\":\"flock-you-go.usb.v2\","
             "\"uptime_ms\":%lu,\"sensors\":%d,\"encounters\":%u,"
             "\"current_path\":\"%s\",\"previous_path\":\"%s\"}\n",
             (unsigned long)millis(), fyDetCount, (unsigned)fyEncounterCount,
             FY_SESSION_FILE, FY_PREV_FILE);
  Serial.flush();
  for (int i = 0; i < fyDetCount; i++) {
    emitDetectionJSON("snapshot", fyDet[i], "");
    Serial.flush();
  }

  // Export the previous persisted session too. Without this, a rebooted or
  // deep-slept device can look empty even when the last scan was saved.
  emitStoredSessionFile("previous_session", FY_PREV_FILE);

  dualPrintf("{\"event\":\"snapshot_end\",\"schema\":\"flock-you-go.usb.v2\","
             "\"uptime_ms\":%lu,\"sensors\":%d,\"encounters\":%u}\n",
             (unsigned long)millis(), fyDetCount, (unsigned)fyEncounterCount);
  Serial.flush();
  Serial.setTxTimeoutMs(0);
}
'''

OLD_COMMANDS = r'''      if (strcmp(command, "DUMP") == 0) {
        emitUsbSnapshot();
      } else if (strcmp(command, "STATUS") == 0) {
        emitUsbStatusJSON();
      } else if (strcmp(command, "SAVE") == 0) {
        fySaveSession();
        dualPrintln("{\"event\":\"save_complete\",\"schema\":\"flock-you-go.usb.v2\"}");
      } else if (strcmp(command, "HELP") == 0) {
        dualPrintln("[flockyou] USB commands: DUMP, STATUS, SAVE, HELP");
'''

NEW_COMMANDS = r'''      if (strcmp(command, "DUMP") == 0) {
        emitUsbSnapshot();
      } else if (strcmp(command, "DUMP_PREV") == 0) {
        Serial.setTxTimeoutMs(100);
        emitStoredSessionFile("previous_session", FY_PREV_FILE);
        Serial.setTxTimeoutMs(0);
      } else if (strcmp(command, "STATUS") == 0) {
        emitUsbStatusJSON();
      } else if (strcmp(command, "SAVE") == 0) {
        fySaveSession();
        dualPrintln("{\"event\":\"save_complete\",\"schema\":\"flock-you-go.usb.v2\"}");
      } else if (strcmp(command, "HELP") == 0) {
        dualPrintln("[flockyou] USB commands: DUMP, DUMP_PREV, STATUS, SAVE, HELP");
'''


def replace_once(text: str, old: str, new: str, label: str) -> str:
    count = text.count(old)
    if count == 0:
        raise SystemExit(f"Could not find expected block for {label}. main.cpp may already be patched or changed.")
    if count > 1:
        raise SystemExit(f"Found {count} copies of {label}; refusing ambiguous patch.")
    return text.replace(old, new)


def main() -> int:
    text = MAIN.read_text(encoding="utf-8")

    text = text.replace("#define AUTOSAVE_INTERVAL_MS 60000", "#define AUTOSAVE_INTERVAL_MS 10000")

    if "static void emitStoredSessionFile" not in text:
        marker = "static void emitUsbSnapshot() {"
        if marker not in text:
            raise SystemExit("Could not find emitUsbSnapshot() insertion point")
        text = text.replace(marker, STORED_SESSION_HELPER + marker, 1)

    if OLD_SNAPSHOT in text:
        text = replace_once(text, OLD_SNAPSHOT, NEW_SNAPSHOT, "emitUsbSnapshot")
    elif "emitStoredSessionFile(\"previous_session\", FY_PREV_FILE);" not in text:
        raise SystemExit("emitUsbSnapshot() did not match and does not appear patched")

    if OLD_COMMANDS in text:
        text = replace_once(text, OLD_COMMANDS, NEW_COMMANDS, "USB command parser")
    elif "DUMP_PREV" not in text:
        raise SystemExit("USB command parser did not match and does not appear patched")

    MAIN.write_text(text, encoding="utf-8")
    print("Patched main.cpp for nonvolatile session export and 10s autosave.")
    return 0


if __name__ == "__main__":
    raise SystemExit(main())
