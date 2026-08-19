# 🌵 Cactus Cam

A half-dollar-size **camera agent** on the [Seeed XIAO ESP32S3 Sense](https://www.seeedstudio.com/xiao-esp32s3-sense-p-5057.html). Message it on **Telegram** and it answers back: snaps photos, rolls video clips, records voice notes from its own mic, keeps timers — and it still **listens for barks** (the [barkcam](https://github.com/Snail3D/barkcam) pipeline rides along, so a loud dog still gets you a photo).

No cloud. No AI model in the loop. Deterministic signal processing on the board, with a plain-words command parser you can read in an afternoon.

```
you › photo                 →  🌵 got it — needle-ing the cactus…   (instant ack)
                               📷 you asked · 14:32:07              (photo, edited in place)
you › video 10s             →  🌵 rolling… 10 s   →   🎬 MP4 posted back
you › record 8s             →  voice note from the PDM mic (WAV)
you › every 5 min photo     →  recurring timer (up to 3, rate-limited)
you › exposure bright       →  camera tuning: exposure · brightness · contrast ·
                               saturation · white balance · night mode
you › stop / status         →  clear timers · see what's running
```

## Flash it in your browser

The fastest path — no install, Chrome or Edge:

👉 **[cactuscam.flash](https://snail3d.github.io/cactuscam/)** — one button, WebSerial + esptool.

Or the terminal:

```bash
# download the merged image (bootloader + partitions + app) and flash it
curl -LO https://raw.githubusercontent.com/Snail3D/cactuscam/main/docs/firmware/cactuscam-v1.0.bin
esptool.py --chip esp32s3 write_flash 0x0 cactuscam-v1.0.bin
```

> The XIAO ESP32S3 uses **native USB** — plug it in and it auto-enters the bootloader on reset. If it's already running, hold **BOOT** while plugging in USB to force bootloader mode.

## First-time setup (2 minutes, one time)

After flashing, the board broadcasts an open network **`cactuscam-config`** for 10 minutes:

1. Connect your phone to **`cactuscam-config`**.
2. Open **`http://cactuscam.local`** (or `192.168.4.1`).
3. Enter your home WiFi, a Telegram **bot token** (from [@BotFather](https://t.me/BotFather)) and your **user ID** (from [@userinfobot](https://t.me/userinfobot)). Hit **Save settings**.

Everything is stored on the board (NVS) — no account, no cloud. The board closes its access point when your phone leaves and reconnects to home WiFi on its own.

## Hardware

| | |
|---|---|
| Board | Seeed **XIAO ESP32S3 Sense** (OV2640 camera + PDM mic, OPI PSRAM) |
| Flash | 8 MB (`default_8MB` partition layout, QIO OPI) |
| Camera | OV2640 DVP — VGA JPEG, 10 fps capture for clips |
| Mic | PDM, 16 kHz mono — bark detection + voice notes |

Wired identically to [barkcam](https://github.com/Snail3D/barkcam) — the same pin map, so a barkcam harness drops straight in.

## The studio (Mac side)

The ESP32 can't encode video, so **clips are streamed as raw MJPEG frames** to a small local service that encodes them with `ffmpeg` and posts the MP4 back to your Telegram chat. Photos are archived as a bonus.

```bash
python3 tools/studio.py        # listens on :8377, writes to ./media/{clips,photos}
```

The board's `STUDIO_HOST` (in `fw/include/config.h`) must point at the machine running it. If that machine's IP changes, edit + reflash — or just keep the studio on a static LAN address.

## Build from source

```bash
pio run            # in fw/  (PlatformIO, espressif32@6.9.0 / Arduino core 2.x)
pio run -t upload  # flash over native USB (--no-stub is set in platformio.ini)
```

The legacy I2S PDM API matters here: **core 3.x breaks the mic**, so the platform pin is deliberate.

## Repo layout

```
fw/            firmware (PlatformIO) — src/main.cpp, include/{config,bark_detector,telegram_ca,ui_page}.h
tools/         studio.py — the Mac-side clip encoder + media archiver
docs/          flasher page (WebSerial), merged firmware download, config-UI screenshot
media/         studio output — clips + photos (gitignored)
```

## Serial console (115200, native USB)

`t` photo · `v` video test · `r` voice note · `s` status · `c` clear cooldown · `w` scan · `i` wifi info · `a` reopen config AP · `1`/`2` tune bark threshold

---

Built by [snail3d](https://snail3d.com) · El Paso, TX
