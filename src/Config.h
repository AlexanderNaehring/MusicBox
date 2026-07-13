#ifndef MUSICBOX_CONFIG_H
#define MUSICBOX_CONFIG_H

// Top-level build-time feature toggles. Shared across translation units so
// every module can branch on them (macros don't cross .cpp files), instead
// of only main.cpp knowing about them.

#define DEBUG true
#define HW_REV 1
#define BLE false
#define WIFI false
#define WIFI_DOWNLOAD true
#define AllowSleep false

// Debug logging: each .cpp defines LOG_TAG (its own file name, as a plain
// string literal) before using these, so every Serial line can be traced
// back to the module it came from.
#define LOGF(fmt, ...) Serial.printf("[" LOG_TAG "] " fmt, ##__VA_ARGS__)
#define LOGLN(msg)                  \
  do {                              \
    Serial.print("[" LOG_TAG "] "); \
    Serial.println(msg);            \
  } while (0)

#endif
