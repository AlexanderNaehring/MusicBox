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

#endif
