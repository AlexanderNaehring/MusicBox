# MusicBox

A hardware MP3 player for kids: insert NFC-tagged card,
and it plays that card's music from an SD card - 
no screen or app required to operate it.
Built on an ESP32, an MFRC522 RFID/NFC reader, and I2S MAX98357 amplifier,
powered by [ESP8266Audio](https://github.com/earlephilhower/ESP8266Audio).

## Hardware

Two board revisions currently supported, selected at build time via `HW_REV` in
[`src/Config.h`](src/Config.h):

| | HW_REV 1 | HW_REV 2 |
|---|---|---|
| Volume | Next/Prev buttons double as volume up/down; hold either to skip, hold both to restart | Dedicated rotary encoder |
| Next/Prev | Long-press Next/Prev button | Click Next/Prev button |
| Card-detect pin | GPIO 17 | GPIO 4 (allows wake from deep sleep) |
| Sleep | Light sleep only | Light sleep + deep sleep after a card sits idle in STOPPED |

Other pins (SD card over VSPI, RFID reader over HSPI, RGB status LED) are the
same across revisions - see [`src/Board.h`](src/Board.h).

## How it works

The firmware is a small state machine (see [`src/DeviceState.h`](src/DeviceState.h)):

```
IDLE -> READING_NFC -> PLAYING <-> PAUSED -> STOPPED -> IDLE
                 \-> ERROR
```

A card inserted in `IDLE` moves to `READING_NFC`, where the NFC tag's NDEF
text record is decoded into a file or folder path on the SD card. A folder is
played back as an album (tracks sorted by filename); removing and
re-inserting the *same* card resumes exactly where playback left off - the
current track index and byte offset are written to a `last.int64` file
alongside the folder every 30 seconds and on pause, and read back the next
time that same folder starts.

The status LED shows status information: white while idle/reading a
card, green while playing, yellow while paused, red on any error.


### Source layout

| File | Responsibility |
|---|---|
| `Config.h` | Build-time feature toggles (`HW_REV`, `BLE`, `WIFI`, `WIFI_DOWNLOAD`, `AllowSleep`, `DEBUG`) |
| `Board.h/.cpp` | Pins, SPI buses, the RGB status LED, and the shared I2S audio output |
| `Battery.h/.cpp` | Battery percentage from the ADC voltage divider (not HW Rev 1) |
| `DeviceState.h` | The state enum and the `StateMachine` that tracks/announces transitions |
| `Player.h/.cpp` | The playback queue, ESP8266Audio pipeline, gain, and position persistence |
| `Nfc.h/.cpp` | MFRC522 reading and NDEF decoding |
| `Controls.h/.cpp` | Buttons/encoder and their per-`HW_REV` behavior |
| `Power.h/.cpp` | Light/deep sleep |
| `main.cpp` | Wires the above together: `setup()`/`loop()` and the state machine dispatch |
| `MusicBoxBLE.*` / `MusicBoxWifi.*` / `MusicBoxWifiDownloader.*` | Optional features, see below |

## Optional features (`Config.h`)

| Flag | Effect when enabled |
|---|---|
| `BLE` | Advertises a BLE service for a companion app to read now-playing/battery status and send next/prev/restart/diagnostics commands ([`MusicBoxBLE`](src/MusicBoxBLE.h)) |
| `WIFI` | Holding the Prev button at boot starts a WiFi + web file manager for browsing/uploading/deleting files on the SD card ([`MusicBoxWifi`](src/MusicBoxWifi.h)) |
| `WIFI_DOWNLOAD` | Before playing a tag whose path isn't on the SD card yet, connects to WiFi and downloads it from a content server ([`MusicBoxWifiDownloader`](src/MusicBoxWifiDownloader.h)) |
| `AllowSleep` | Enables the light/deep sleep calls in `Power.*` (otherwise they're no-ops, useful while developing over serial) |

`WIFI` and `WIFI_DOWNLOAD` both need WiFi credentials in a local
`credentials.ini` (gitignored - not included in this repo):

```ini
[credentials]
wifi_ssid = your-ssid
wifi_password = your-password
download_base_url = http://your-content-server-address
```

## Content workflow

For `WIFI_DOWNLOAD`, MP3s live on an HTTP server rather than only on the
SD card, and NFC tags are written from a phone. The [`tools/`](tools/README.md)
directory generates the manifest files the downloader expects plus a
Web-NFC tag-writer page - see its README for the full workflow.

```
uv run main.py generate /path/to/root/content/folder
```

## Build & flash

This is a [PlatformIO](https://platformio.org/) project (env `esp32dev`):

```
pio run              # build
pio run -t upload    # build and flash
pio device monitor   # serial monitor (115200 baud)
```
