#pragma once

// ============================================================
// CACTUSCAM — XIAO ESP32S3 Sense pin map + tuning knobs
// Pin map for the Seeed XIAO ESP32S3 Sense (identical to barkcam).
// ============================================================

// --- microphone (PDM, via I2S) — do NOT reuse these pins ---
#define MIC_PDM_CLK   42
#define MIC_PDM_DATA  41

// --- camera (DVP parallel) — do NOT reuse these pins ---
#define CAM_PIN_XCLK   10
#define CAM_PIN_PCLK   13
#define CAM_PIN_VSYNC  38
#define CAM_PIN_HREF   47
#define CAM_PIN_SDA    40   // SCCB (SIOD)
#define CAM_PIN_SCL    39   // SCCB (SIOC)
#define CAM_PIN_D0     15
#define CAM_PIN_D1     17
#define CAM_PIN_D2     18
#define CAM_PIN_D3     16
#define CAM_PIN_D4     14
#define CAM_PIN_D5     12
#define CAM_PIN_D6     11
#define CAM_PIN_D7     48

// --- misc ---
#define LED_PIN        21   // USER LED, active-LOW

#define CAM_JPEG_QUALITY 12 // lower = sharper but bigger file (5–30)
#define AEC_SETTLE_FRAMES  4

// --- audio / bark detection tuning (same as barkcam) ---
#define SAMPLE_RATE          16000
#define FRAME_SAMPLES        256     // 16 ms per analysis frame
#define HP_CUTOFF_HZ         250.0f
#define NOISE_FLOOR_DB       -60.0f
#define NOISE_RISE_COEF      0.10f
#define NOISE_FALL_COEF      0.01f
#define THRESHOLD_MARGIN_DB  15.0f
#define BURST_MIN_FRAMES     3
#define BURST_MAX_FRAMES     24
#define BURST_DECAY_DB       6.0f
#define BURST_DECAY_FRAMES   2
#define BARKS_TO_CONFIRM     2
#define BARK_WINDOW_MS       4000

// --- bark behavior (same as barkcam) ---
#define COOLDOWN_MS          120000
#define EPISODE_REPEAT_MS    8000
#define EPISODE_MAX          3
#define EPISODE_GAP_MS       12000
#define SCHED_DAYS_DEFAULT   0x7F
#define SCHED_HOURS_DEFAULT  0xFFFFFFu
#define WIFI_TIMEOUT_MS      20000

// --- watchdog / reboot ---
// Video upload adds ~10 s to the worst path; 90 s still bounds a stuck loop.
#define WATCHDOG_TIMEOUT_MS   90000
#define PERIODIC_REBOOT_MS   21600000ULL // every 6 h

#define TZ_OFFSET_HOURS      0
#define FIRMWARE_VERSION     1

// --- config access point (open AP, first N minutes after power-on) ---
#define AP_SSID          "cactuscam-config"
#define AP_WINDOW_MS     600000

// --- camera orientation setting (0=none 3=180; 90° not supported by this sensor) ---
#define CAM_ROTATE_DEFAULT 3
// --- camera exposure setting (0=dim 1=medium 2=bright) ---
#define CAM_EXPOSURE_DEFAULT 1

// ============================================================
// CACTUSCAM additions — the agent must know its limits.
// Every cap below is enforced in firmware; commands that exceed
// them are rejected with a plain-language reply, never crashed on.
// ============================================================

// --- video clips (JPEG frames concatenated = MJPEG, encoded to MP4 by the studio) ---
#define VIDEO_FPS          10
#define VIDEO_MS_DEFAULT   5000
#define VIDEO_MS_MIN       2000     // shorter than this is not a clip
#define VIDEO_MS_MAX       15000    // longer risks PSRAM overflow mid-capture

// --- voice notes (mic -> WAV -> Telegram sendAudio) ---
#define AUDIO_MS_DEFAULT   8000
#define AUDIO_MS_MIN       2000
#define AUDIO_MS_MAX       20000

// --- scheduler caps ---
#define SCHED_MAX_JOBS         3    // at most three recurring timers at once
#define SCHED_PHOTO_MS_MIN     10000   // never more often than one photo / 10 s
#define SCHED_VIDEO_MS_MIN     20000   // clips are heavy — one per 20 s minimum
#define SCHED_MS_MAX           3600000 // no point ticking hourly+

// --- studio service (this Mac): encodes clips, archives media ---
#define STUDIO_HOST "192.168.1.13"   // this Mac — edit + reflash if it changes
#define STUDIO_PORT 8377
