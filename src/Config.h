#ifndef MUSICBOX_CONFIG_H
#define MUSICBOX_CONFIG_H

// Top-level build-time configuration

#define DEBUG true
#define HW_REV 2
#define BLE false
#define WIFI false
#define WIFI_DOWNLOAD true
#define AllowSleep true

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
