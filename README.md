# ESP32-S3 Payment Soundbox

Final year engineering project: a standalone UPI payment soundbox built on the ESP32-S3.
It shows a payment QR code on an OLED, confirms received payments through Razorpay,
announces them through a speaker, and lets the shopkeeper ask spoken questions about
sales (e.g. "How much did I receive today?") answered through a Cloudflare Worker.

## Features

- Dynamic QR (enter an amount) and fixed-amount QR via Razorpay Payment Links
- Payment verification and spoken confirmation (I2S audio, MAX98357A amplifier)
- Approved-question menu for sales summaries (latest payment, today's total, payment count, monthly summary, last N days)
- Cloudflare Worker backend: speech generation, answers, and cloud payment log
- Local payment CSV in flash (LittleFS), downloadable at `http://<device-ip>/export.csv`
- Wi-Fi/API setup through a phone captive portal; no secrets in the source code (stored in NVS flash)

## Hardware

| Part | Pins (ESP32-S3 GPIO) |
|---|---|
| SH1106 1.3" OLED (I2C) | SDA 8, SCL 9 |
| MAX98357A amplifier (I2S) | BCLK 15, LRC 7, DIN 17 |
| INMP441 microphone (wired, disabled in this build) | SCK 4, WS 5, SD 6 |
| 4x4 matrix keypad | Rows 10, 11, 12, 13 / Cols 14, 18, 21, 47 |

GPIO 16 is not used.

## Keypad

| Key | Action |
|---|---|
| `0-9`, `*` | Digits, decimal point |
| `#` | Backspace (hold 2 s on home screen: open setup portal) |
| `C` | Clear / cancel (hold while powering on: open setup portal) |
| `D` | Dynamic QR: press D, type amount, press D again |
| `B` | Fixed-amount QR |
| `A` | Open / select the question menu |

## Repository layout

```
firmware/soundbox_english/   Arduino sketch (.ino) + QR code library files
release/                     Prebuilt binaries + SHA256SUMS.txt
```

## Build from source

1. Install Arduino IDE and the **esp32 by Espressif** board package (built with core 3.3.12).
2. Install libraries: **U8g2 2.36.19**, **Keypad 3.1.1**, **QRCode 0.0.1**, **ArduinoJson 7.3.2**.
3. Open `firmware/soundbox_english/soundbox_english.ino`. Keep `qrcode.h` and `qrcode_impl.c` in the same folder.
4. Select board **ESP32S3 Dev Module** (`esp32:esp32:esp32s3`), USB CDC On Boot = Enabled.
   Set PSRAM to match your module (OPI / QSPI / Disabled). The release binaries were built with PSRAM disabled.
5. Upload.

## Flash the prebuilt binary

`release/soundbox_esp32s3_merged_4MB.bin` is a complete 4 MB image (bootloader + partition table + app).
It **erases the whole flash**, including saved settings. Use it only if your board has 4 MB flash with the same layout.

```bash
pip install esptool
esptool.py --chip esp32s3 --port <PORT> write_flash 0x0 release/soundbox_esp32s3_merged_4MB.bin
```

`soundbox_esp32s3_app.bin` is the application image only, for uploading at the app offset of your partition table.
Verify downloads against `release/SHA256SUMS.txt`.

## First-time setup

1. Power on for the first time (or hold `C` while powering on).
2. Join Wi-Fi **Soundbox-Setup** (password `soundbox2026`). The portal opens automatically, or go to `192.168.4.1`.
3. Enter Wi-Fi, Razorpay Key ID/Secret, and Cloudflare Worker URL + access token, then Save.

Use Razorpay **test** keys (`rzp_test_...`) while testing.

## Status

Compile-checked; not yet verified on physical hardware in this build. If your module uses different flash or PSRAM options, rebuild with matching board settings.

## Third-party

- QR code generator: Richard Moore, MIT (`qrcode.h`, `qrcode_impl.c`), inspired by Nayuki's QR-Code-generator
- Libraries: U8g2, Keypad (Mark Stanley), ArduinoJson, QRCode

## License

MIT, see [LICENSE](LICENSE).
