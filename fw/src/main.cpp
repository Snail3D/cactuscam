// CACTUSCAM v1 — camera agent on the Seeed XIAO ESP32S3 Sense
//
// barkcam's pipeline (PDM mic -> bark detector -> photo alert) plus a
// Telegram command channel: text messages from the owner are parsed and
// executed. Every command gets an instant ack, then progress edits, so the
// user always knows the cactus heard them and is working.
//
// Commands (case-insensitive):
//   photo                      capture + send now
//   video [n] | clip [n]       n-second clip (default 5, 2..15) — frames are
//                              streamed to the studio service, which encodes
//                              an MP4 and posts it to the same chat
//   record [n] | voice [n]     n-second voice note from the mic (8 default)
//   exposure dim|medium|bright AEC target bias
//   brightness|contrast|saturation +1|-1   (sensor range -2..2)
//   effect none|negative|grayscale|...      special effects 0..6
//   wb auto|daylight|cloudy|office|home     white balance mode
//   night on|off               AEC night variant
//   every <n> s|min photo|video  recurring timer (photo >=10s, video >=20s)
//   stop | cancel              clear all timers
//   status                     what's running, wifi, heap
//
// The parser is deterministic keywords — no LLM in the loop — so every reply
// is predictable and nothing can hang on a model call.
//
// Limits are enforced here (see config.h): the device knows its own caps and
// refuses with a plain-language reason instead of crashing.
//
// Config: first 10 minutes after power-on the board broadcasts an open AP
// "cactuscam-config" serving a web UI at http://192.168.4.1 (same as barkcam).
//
// Serial commands (115200): t=photo v=video r=voice s=status c=clear cooldown
//   w=wifi scan i=wifi info a=reopen config AP  1/2=tune threshold

#include <Arduino.h>
#include <WiFi.h>
#include <WiFiClientSecure.h>
#include <HTTPClient.h>
#include <WebServer.h>
#include <ESPmDNS.h>
#include <Preferences.h>
#include <esp_camera.h>
#include <driver/i2s.h>
#include <time.h>
#include <esp_heap_caps.h>

#include "config.h"
#include "credentials.h"
#include "bark_detector.h"
#include "telegram_ca.h"
#include "ui_page.h"

// ============================ runtime config (NVS-persisted)

struct AppConfig {
    String ssid, pass;
    String botToken, chatId;
    float marginDb;       // bark sensitivity: threshold = noise floor + margin
    uint32_t cooldownMs;  // fixed at COOLDOWN_MS
    int rotate;           // 0=none 3=180
    int exposure;         // AEC bias: 0=dim 1=medium 2=bright
    int brightness, contrast, saturation;   // -2..2 (sensor DSP)
    int effect;           // 0..6 special effect
    int wbMode;           // 0..4 (0 = follow AWB)
    bool nightMode;       // AEC night variant
    uint32_t apWindowMs;
    uint8_t daysMask;     // quiet-hours: bit0=Mon..bit6=Sun
    uint32_t hoursMask;   // bit N set = on during hour N
};

static AppConfig cfg = {
    WIFI_SSID, WIFI_PASSWORD,
    TELEGRAM_BOT_TOKEN, TELEGRAM_CHAT_ID,
    THRESHOLD_MARGIN_DB, COOLDOWN_MS, CAM_ROTATE_DEFAULT, CAM_EXPOSURE_DEFAULT,
    0, 2, -1,            // brightness, contrast (barkcam look), saturation
    0, 0, false,         // effect none, wb auto, night off
    AP_WINDOW_MS, SCHED_DAYS_DEFAULT, SCHED_HOURS_DEFAULT
};

static Preferences prefs;   // namespace "cactuscam"

static void loadConfig() {
    prefs.begin("cactuscam", false);
    if (prefs.isKey("ssid"))     { String v = prefs.getString("ssid", "");   if (v.length()) cfg.ssid = v; }
    if (prefs.isKey("pass"))     { String v = prefs.getString("pass", "");   if (v.length()) cfg.pass = v; }
    if (prefs.isKey("token"))    { String v = prefs.getString("token", "");  if (v.length()) cfg.botToken = v; }
    if (prefs.isKey("chatId"))   { String v = prefs.getString("chatId", ""); if (v.length()) cfg.chatId = v; }
    if (prefs.isKey("margin"))   cfg.marginDb = prefs.getFloat("margin", THRESHOLD_MARGIN_DB);
    if (prefs.isKey("rotate"))   { int r = prefs.getInt("rotate", CAM_ROTATE_DEFAULT); cfg.rotate = (r == 0 || r == 3) ? r : CAM_ROTATE_DEFAULT; }
    if (prefs.isKey("exposure")) cfg.exposure = prefs.getInt("exposure", CAM_EXPOSURE_DEFAULT);
    if (prefs.isKey("bright"))   { int v = prefs.getInt("bright", 0);   cfg.brightness = (v >= -2 && v <= 2) ? v : 0; }
    if (prefs.isKey("contrast")) { int v = prefs.getInt("contrast", 2); cfg.contrast   = (v >= -2 && v <= 2) ? v : 2; }
    if (prefs.isKey("sat"))      { int v = prefs.getInt("sat", -1);     cfg.saturation = (v >= -2 && v <= 2) ? v : -1; }
    if (prefs.isKey("effect"))   { int v = prefs.getInt("effect", 0);   cfg.effect     = (v >= 0 && v <= 6) ? v : 0; }
    if (prefs.isKey("wbMode"))   { int v = prefs.getInt("wbMode", 0);   cfg.wbMode     = (v >= 0 && v <= 4) ? v : 0; }
    if (prefs.isKey("night"))    cfg.nightMode = prefs.getInt("night", 0) != 0;
    if (prefs.isKey("apWindow")) cfg.apWindowMs = prefs.getInt("apWindow", AP_WINDOW_MS);
    if (prefs.isKey("daysMask")) { int v = prefs.getInt("daysMask", SCHED_DAYS_DEFAULT); cfg.daysMask = (uint8_t)(v & 0x7F); }
    if (prefs.isKey("hoursMask")) cfg.hoursMask = (uint32_t)(prefs.getInt("hoursMask", (int)SCHED_HOURS_DEFAULT) & 0xFFFFFFu);
}

static void saveConfig() {
    prefs.putString("ssid", cfg.ssid);
    prefs.putString("pass", cfg.pass);
    prefs.putString("token", cfg.botToken);
    prefs.putString("chatId", cfg.chatId);
    prefs.putFloat("margin", cfg.marginDb);
    prefs.putInt("rotate", cfg.rotate);
    prefs.putInt("exposure", cfg.exposure);
    prefs.putInt("bright", cfg.brightness);
    prefs.putInt("contrast", cfg.contrast);
    prefs.putInt("sat", cfg.saturation);
    prefs.putInt("effect", cfg.effect);
    prefs.putInt("wbMode", cfg.wbMode);
    prefs.putInt("night", cfg.nightMode ? 1 : 0);
    prefs.putInt("apWindow", (int)cfg.apWindowMs);
    prefs.putInt("daysMask", (int)cfg.daysMask);
    prefs.putInt("hoursMask", (int)cfg.hoursMask);
}

// ============================ globals

static const i2s_port_t I2S_PORT = I2S_NUM_0;
static bool micReady = false;
static bool cameraReady = false;
static BarkDetector detector;

static uint32_t lastSendMs = 0;
static uint32_t lastBarkMs = 0;
static uint8_t episodeCount = 0;
static volatile uint32_t loopTick = 0;   // watchdog ping — every blocking path ticks this
static bool busy = false;                // camera/upload in flight: gates poller + scheduler

// config access point state (same design as barkcam)
static WebServer server(80);
static bool apMode = false;
static uint32_t apStartMs = 0;
static uint32_t staDiscSince = 0;
static bool everConnected = false;
static uint32_t apEmptySince = 0;
static bool apClientEver = false;

static volatile bool pendingTest = false;
static String lastResult = "none";       // none | ok | fail

#define LED_ON()  digitalWrite(LED_PIN, LOW)   // active-LOW
#define LED_OFF() digitalWrite(LED_PIN, HIGH)

// ============================ helpers

static void *bigMalloc(size_t n) {
#if CONFIG_SPIRAM || defined(BOARD_HAS_PSRAM)
    void *p = heap_caps_malloc(n, MALLOC_CAP_SPIRAM | MALLOC_CAP_8BIT);
    if (p) return p;
#endif
    return malloc(n);
}

// ============================ microphone (PDM via I2S) — barkcam's, unchanged

static bool initMic() {
    i2s_config_t c = {};
    c.mode                 = (i2s_mode_t)(I2S_MODE_MASTER | I2S_MODE_RX | I2S_MODE_PDM);
    c.sample_rate          = SAMPLE_RATE;
    c.bits_per_sample      = I2S_BITS_PER_SAMPLE_16BIT;
    c.channel_format       = I2S_CHANNEL_FMT_ONLY_RIGHT;
    c.communication_format = I2S_COMM_FORMAT_STAND_I2S;
    c.intr_alloc_flags     = ESP_INTR_FLAG_LEVEL1;
    c.dma_buf_count        = 4;
    c.dma_buf_len          = 256;
    c.use_apll             = true;
    if (i2s_driver_install(I2S_PORT, &c, 0, NULL) != ESP_OK) return false;

    i2s_pin_config_t pins = {};
    pins.bck_io_num   = I2S_PIN_NO_CHANGE;
    pins.ws_io_num    = MIC_PDM_CLK;
    pins.data_out_num = I2S_PIN_NO_CHANGE;
    pins.data_in_num  = MIC_PDM_DATA;
    return i2s_set_pin(I2S_PORT, &pins) == ESP_OK;
}

// ============================ voice note recording (teed from the mic path)

static bool recActive = false;
static uint8_t *recBuf = nullptr;
static size_t recFill = 0, recTarget = 0;
static int recMs = 0;

static void recStart(int ms) {
    if (recActive) return;
    size_t need = (size_t)ms / 1000 * SAMPLE_RATE * 2;
    recBuf = (uint8_t *)bigMalloc(need);
    if (!recBuf) { Serial.println("!! voice: out of memory"); return; }
    recFill = 0;
    recTarget = need;
    recMs = ms;
    recActive = true;
}

// ============================ camera — barkcam's, plus the extended settings

static void applyCameraTuning() {
    sensor_t *s = esp_camera_sensor_get();
    if (!s) return;
    int flip = (cfg.rotate == 3) ? 1 : 0;
    s->set_vflip(s, flip);
    s->set_hmirror(s, 0);
    s->set_exposure_ctrl(s, 1);   // AEC on
    s->set_gain_ctrl(s, 1);       // AGC on
    static const int8_t EXPO_LV[] = { -2, 0, 2 };
    int e = cfg.exposure; if (e < 0) e = 0; else if (e > 2) e = 2;
    s->set_ae_level(s, EXPO_LV[e]);
    // Extended block — every call checked: the OV2640 rejects what it lacks.
    s->set_brightness(s, (int8_t)cfg.brightness);
    s->set_contrast(s, (int8_t)cfg.contrast);
    s->set_saturation(s, (int8_t)cfg.saturation);
    if (s->set_special_effect) s->set_special_effect(s, (uint8_t)cfg.effect);
    s->set_whitebal(s, 1);
    s->set_awb_gain(s, 1);
    s->set_wb_mode(s, (uint8_t)cfg.wbMode);
    if (s->set_aec2) s->set_aec2(s, cfg.nightMode ? 1 : 0);
}

static bool initCamera() {
    camera_config_t c = {};
    c.ledc_channel = LEDC_CHANNEL_0;
    c.ledc_timer   = LEDC_TIMER_0;
    c.pin_d0  = CAM_PIN_D0;   c.pin_d1  = CAM_PIN_D1;
    c.pin_d2  = CAM_PIN_D2;   c.pin_d3  = CAM_PIN_D3;
    c.pin_d4  = CAM_PIN_D4;   c.pin_d5  = CAM_PIN_D5;
    c.pin_d6  = CAM_PIN_D6;   c.pin_d7  = CAM_PIN_D7;
    c.pin_xclk   = CAM_PIN_XCLK;
    c.pin_pclk   = CAM_PIN_PCLK;
    c.pin_vsync  = CAM_PIN_VSYNC;
    c.pin_href   = CAM_PIN_HREF;
    c.pin_sccb_sda = CAM_PIN_SDA;
    c.pin_sccb_scl = CAM_PIN_SCL;
    c.pin_pwdn  = -1;
    c.pin_reset = -1;
    c.xclk_freq_hz = 20000000;
    c.pixel_format = PIXFORMAT_JPEG;
    c.frame_size   = FRAMESIZE_VGA;
    c.jpeg_quality = CAM_JPEG_QUALITY;
    c.fb_count     = 2;
    c.grab_mode    = CAMERA_GRAB_LATEST;

    if (esp_camera_init(&c) != ESP_OK) return false;

    sensor_t *s = esp_camera_sensor_get();
    if (s) {
        applyCameraTuning();
        s->set_lenc(s, 1);
    }
    cameraReady = true;
    Serial.println("camera ready (VGA q12)");
    return true;
}

static bool capturePhoto(uint8_t **out, size_t *outLen) {
    if (!cameraReady && !initCamera()) {
        Serial.println("!! camera init failed — is the Sense expansion attached? !!");
        return false;
    }
    if (!cameraReady) return false;

    for (int i = 0; i < AEC_SETTLE_FRAMES; i++) {
        camera_fb_t *warm = esp_camera_fb_get();
        if (warm) esp_camera_fb_return(warm);
        else break;
    }

    camera_fb_t *fb = esp_camera_fb_get();
    if (!fb) { Serial.println("camera: no frame"); return false; }

    uint8_t *buf = (uint8_t *)bigMalloc(fb->len);
    if (buf) { memcpy(buf, fb->buf, fb->len); *out = buf; *outLen = fb->len; }
    esp_camera_fb_return(fb);
    return buf != nullptr;
}

// ============================ time (NTP)

static void initTime() {
    configTime(TZ_OFFSET_HOURS * 3600, 0, "0.us.pool.ntp.org", "1.us.pool.ntp.org");
}

static String clockString() {
    time_t t = time(nullptr);
    if (t < 1600000000) return String();
    struct tm lt;
    localtime_r(&t, &lt);
    char b[16];
    strftime(b, sizeof(b), "%H:%M:%S", &lt);
    return String(b);
}

// ============================ telegram client

// One TLS client at a time: the poll task and the send path never overlap
// (both check `busy`). Pinned CA from barkcam.

static int tgPost(const char *method, const uint8_t *body, size_t bodyLen,
                  const char *contentType, String &resp) {
    if (!cfg.botToken.length()) return -1;
    String url = String("https://api.telegram.org/bot") + cfg.botToken + "/" + method;
    WiFiClientSecure client;
    client.setCACert(TELEGRAM_ROOT_CA);
    client.setTimeout(15000);
    HTTPClient http;
    if (!http.begin(client, url)) { http.end(); return -1; }
    http.addHeader("Content-Type", contentType);
    int code = http.POST((uint8_t *)body, (int)bodyLen);
    resp = http.getString();
    http.end();
    client.stop();
    return code;
}

static bool tgSendText(const String &text, int *outMsgId) {
    if (!cfg.chatId.length()) return false;
    // Minimal JSON escape: backslash and quote are the only ones that matter here.
    String esc = text;
    esc.replace("\\", "\\\\");
    esc.replace("\"", "\\\"");
    String body = "{\"chat_id\":\"" + cfg.chatId + "\",\"text\":\"" + esc + "\"}";
    String resp;
    int code = tgPost("sendMessage", (const uint8_t *)body.c_str(), body.length(), "application/json", resp);
    if (code != 200 || !resp.startsWith("{\"ok\":true")) {
        Serial.printf("telegram text failed (%d): %s\n", code, resp.c_str());
        return false;
    }
    if (outMsgId) {
        int p = resp.indexOf("\"message_id\":");
        *outMsgId = (p >= 0) ? resp.substring(p + 13).toInt() : -1;
    }
    return true;
}

static bool tgEditText(int msgId, const String &text) {
    if (msgId <= 0) return false;
    String esc = text;
    esc.replace("\\", "\\\\");
    esc.replace("\"", "\\\"");
    String body = "{\"chat_id\":\"" + cfg.chatId + "\",\"message_id\":" + String(msgId)
                + ",\"text\":\"" + esc + "\"}";
    String resp;
    int code = tgPost("editMessageText", (const uint8_t *)body.c_str(), body.length(), "application/json", resp);
    return code == 200 && resp.startsWith("{\"ok\":true");
}

static bool sendTelegramPhoto(const uint8_t *jpeg, size_t jpegLen, const String &caption) {
    if (!jpeg || !jpegLen) return false;

    WiFiClientSecure client;
    client.setCACert(TELEGRAM_ROOT_CA);
    client.setTimeout(15000);
    HTTPClient http;
    String url = String("https://api.telegram.org/bot") + cfg.botToken + "/sendPhoto";
    if (!http.begin(client, url)) { http.end(); return false; }

    String boundary = "CactusCam" + String(millis(), HEX);
    String head = "--" + boundary + "\r\n"
                  "Content-Disposition: form-data; name=\"chat_id\"\r\n\r\n" + cfg.chatId + "\r\n"
                  "--" + boundary + "\r\n"
                  "Content-Disposition: form-data; name=\"caption\"\r\n\r\n" + caption + "\r\n"
                  "--" + boundary + "\r\n"
                  "Content-Disposition: form-data; name=\"photo\"; filename=\"cactuscam.jpg\"\r\n"
                  "Content-Type: image/jpeg\r\n\r\n";
    String tail = "\r\n--" + boundary + "--\r\n";

    size_t total = head.length() + jpegLen + tail.length();
    uint8_t *body = (uint8_t *)bigMalloc(total);
    if (!body) { Serial.println("telegram: out of memory for body"); http.end(); return false; }
    size_t off = 0;
    memcpy(body, head.c_str(), head.length());   off += head.length();
    memcpy(body + off, jpeg, jpegLen);           off += jpegLen;
    memcpy(body + off, tail.c_str(), tail.length());

    http.addHeader("Content-Type", "multipart/form-data; boundary=" + boundary);
    int code = http.POST(body, total);
    free(body);
    String resp = http.getString();
    http.end();
    client.stop();

    if (code != 200 || !resp.startsWith("{\"ok\":true")) {
        Serial.printf("telegram photo failed (%d): %s\n", code, resp.c_str());
        return false;
    }
    Serial.printf("telegram accepted photo (%u KB)\n", (unsigned)(jpegLen / 1024));
    return true;
}

static bool sendTelegramAudio(const uint8_t *pcm, size_t pcmLen) {
    if (!pcm || !pcmLen) return false;

    // 44-byte WAV header: 16 kHz mono s16.
    uint32_t dataSize = (uint32_t)pcmLen;
    uint8_t hdr[44];
    memcpy(hdr, "RIFF", 4);
    hdr[4] = dataSize + 36 & 0xFF; hdr[5] = (dataSize + 36) >> 8 & 0xFF;
    hdr[6] = (dataSize + 36) >> 16 & 0xFF; hdr[7] = (dataSize + 36) >> 24 & 0xFF;
    memcpy(hdr + 8, "WAVEfmt ", 8);
    uint32_t fmtLen = 16;
    hdr[16] = fmtLen; hdr[17] = 0; hdr[18] = 0; hdr[19] = 0;
    hdr[20] = 1; hdr[21] = 0;                 // PCM
    hdr[22] = 1; hdr[23] = 0;                 // mono
    uint32_t sr = SAMPLE_RATE;
    hdr[24] = sr & 0xFF; hdr[25] = sr >> 8 & 0xFF; hdr[26] = sr >> 16 & 0xFF; hdr[27] = sr >> 24 & 0xFF;
    uint32_t byteRate = sr * 2;
    hdr[28] = byteRate & 0xFF; hdr[29] = byteRate >> 8 & 0xFF;
    hdr[30] = byteRate >> 16 & 0xFF; hdr[31] = byteRate >> 24 & 0xFF;
    hdr[32] = 2; hdr[33] = 0;                 // block align
    hdr[34] = 16; hdr[35] = 0;                // bits
    memcpy(hdr + 36, "data", 4);
    hdr[40] = dataSize & 0xFF; hdr[41] = dataSize >> 8 & 0xFF;
    hdr[42] = dataSize >> 16 & 0xFF; hdr[43] = dataSize >> 24 & 0xFF;

    WiFiClientSecure client;
    client.setCACert(TELEGRAM_ROOT_CA);
    client.setTimeout(20000);
    HTTPClient http;
    String url = String("https://api.telegram.org/bot") + cfg.botToken + "/sendAudio";
    if (!http.begin(client, url)) { http.end(); return false; }

    String boundary = "CactusCam" + String(millis(), HEX);
    String head = "--" + boundary + "\r\n"
                  "Content-Disposition: form-data; name=\"chat_id\"\r\n\r\n" + cfg.chatId + "\r\n"
                  "--" + boundary + "\r\n"
                  "Content-Disposition: form-data; name=\"audio\"; filename=\"note.wav\"\r\n"
                  "Content-Type: audio/wav\r\n\r\n";
    String tail = "\r\n--" + boundary + "--\r\n";

    size_t total = 44 + head.length() + pcmLen + tail.length();
    uint8_t *body = (uint8_t *)bigMalloc(total);
    if (!body) { Serial.println("telegram: out of memory for audio"); http.end(); return false; }
    size_t off = 0;
    memcpy(body, head.c_str(), head.length()); off += head.length();
    memcpy(body + off, hdr, 44);               off += 44;
    memcpy(body + off, pcm, pcmLen);           off += pcmLen;
    memcpy(body + off, tail.c_str(), tail.length());

    http.addHeader("Content-Type", "multipart/form-data; boundary=" + boundary);
    int code = http.POST(body, total);
    free(body);
    String resp = http.getString();
    http.end();
    client.stop();

    if (code != 200 || !resp.startsWith("{\"ok\":true")) {
        Serial.printf("telegram audio failed (%d): %s\n", code, resp.c_str());
        return false;
    }
    Serial.printf("telegram accepted voice note (%u KB)\n", (unsigned)(pcmLen / 1024));
    return true;
}

// Best-effort archive copy of a photo to the studio (never blocks long).
static void archivePhoto(const uint8_t *jpeg, size_t jpegLen) {
    if (!jpeg || !jpegLen) return;
    WiFiClient client;
    client.setTimeout(3000);
    if (!client.connect(STUDIO_HOST, STUDIO_PORT)) return;
    String req = "POST /photo HTTP/1.1\r\nHost: " + String(STUDIO_HOST)
               + "\r\nContent-Type: image/jpeg\r\nContent-Length: " + String(jpegLen) + "\r\n\r\n";
    client.write(req.c_str(), req.length());
    client.write(jpeg, jpegLen);
    uint32_t t0 = millis();
    while (client.available() && millis() - t0 < 2000) client.read();
    client.stop();
}

// ============================ telegram poller (own task, queue to loop)

struct TgUpdate { int msgId; String text; };
static QueueHandle_t tgQueue = nullptr;
static bool tgPolling = false;
static int tgOffset = 0;

static void tgPollTask(void *) {
    while (true) {
        if (!tgPolling && !busy && cfg.botToken.length() && WiFi.isConnected()) {
            tgPolling = true;
            String url = String("https://api.telegram.org/bot") + cfg.botToken
                       + "/getUpdates?timeout=20&offset=" + String(tgOffset);
            WiFiClientSecure client;
            client.setCACert(TELEGRAM_ROOT_CA);
            client.setTimeout(30000);   // long-poll: 20 s server hold + margin
            HTTPClient http;
            if (http.begin(client, url)) {
                http.addHeader("Content-Type", "application/json");
                int code = http.GET();
                String body = http.getString();
                http.end();
                client.stop();
                if (code == 200 && body.startsWith("{\"ok\":true")) {
                    // Walk every message object in order. One user talks to this
                    // bot, so a flat scan is sufficient and allocation-light.
                    int pos = 0;
                    while ((pos = body.indexOf("\"message\":", pos)) != -1) {
                        int msgId = -1;
                        int p2 = body.indexOf("\"message_id\":", pos);
                        if (p2 >= 0 && p2 < pos + 4000) msgId = body.substring(p2 + 13).toInt();
                        int p3 = body.indexOf("\"text\":\"", pos);
                        if (msgId > 0 && p3 >= 0 && p3 < pos + 4000) {
                            String text;
                            for (int i = p3 + 7; i < body.length(); i++) {
                                char ch = body[i];
                                if (ch == '\\' && i + 1 < body.length()) { i++; continue; }
                                if (ch == '"') break;
                                text += ch;
                            }
                            TgUpdate u; u.msgId = msgId; u.text = text;
                            xQueueSend(tgQueue, &u, 0);   // drop if full — never block the task
                        }
                        pos = p3 > pos ? p3 : pos + 10;
                    }
                } else {
                    Serial.printf("telegram poll failed (%d)\n", code);
                }
            } else {
                http.end();
                client.stop();
            }
            tgPolling = false;
        }
        vTaskDelay(pdMS_TO_TICKS(200));
    }
}

// ============================ the agent: acks, parser, actions

static const char *const ACKS[] = {
    "\xF0\x9F\x8C\xB5 got it — needle-ing the cactus…",
    "\xF0\x9F\x8C\xB5 on it.",
    "\xF0\x9F\x8C\xB5 copying that down.",
    "\xF0\x9F\x8C\xB5 heard you. working…",
    "\xF0\x9F\x8C\xB5 spiking the cactus into action…"
};

static int ack(const String &text) {
    const char *a = ACKS[millis() % (sizeof(ACKS) / sizeof(ACKS[0]))];
    int id = -1;
    if (!tgSendText(a, &id)) return -1;
    return id;
}

static void progress(int msgId, const String &what) {
    if (msgId > 0) tgEditText(msgId, "\xF0\x9F\x8C\xB5 " + what);
}

// --- actions -------------------------------------------------------------

static bool doPhoto(const char *why) {
    uint32_t waitStart = millis();
    while (!WiFi.isConnected() && (millis() - waitStart < 20000)) { loopTick++; delay(100); }
    if (!WiFi.isConnected()) return false;

    uint8_t *jpg = nullptr;
    size_t jpgLen = 0;
    bool ok = capturePhoto(&jpg, &jpgLen) &&
              sendTelegramPhoto(jpg, jpgLen, String("\xF0\x9F\x93\xB7 ") + why);
    if (ok) archivePhoto(jpg, jpgLen);
    if (jpg) free(jpg);
    return ok;
}

static bool doVideo(int ms) {
    if (!cameraReady && !initCamera()) return false;
    for (int i = 0; i < AEC_SETTLE_FRAMES; i++) {
        camera_fb_t *w = esp_camera_fb_get();
        if (w) esp_camera_fb_return(w); else break;
    }

    int frames = (ms / 1000) * VIDEO_FPS;
    WiFiClient client;
    client.setTimeout(20000);
    if (!client.connect(STUDIO_HOST, STUDIO_PORT)) {
        Serial.println("studio unreachable — clip dropped");
        return false;
    }
    String hdr = "POST /clip HTTP/1.1\r\nHost: " + String(STUDIO_HOST) + "\r\n"
                "Content-Type: video/mjpeg\r\nTransfer-Encoding: chunked\r\n"
                "X-Fps: " + String(VIDEO_FPS) + "\r\nX-Duration-Ms: " + String(ms) + "\r\n\r\n";
    if (client.write(hdr.c_str(), hdr.length()) != (int)hdr.length()) { client.stop(); return false; }

    int got = 0;
    uint32_t t0 = millis();
    while (got < frames && millis() - t0 < 45000) {
        camera_fb_t *fb = esp_camera_fb_get();
        if (!fb) break;
        char sz[16];
        int slen = snprintf(sz, sizeof(sz), "%x\r\n", (unsigned)fb->len);
        client.write((const uint8_t *)sz, slen);
        client.write(fb->buf, fb->len);
        client.write("\r\n", 2);
        esp_camera_fb_return(fb);
        got++;
        loopTick++;   // a 15 s clip must never trip the watchdog
    }
    if (got == 0) { client.stop(); return false; }
    client.write("0\r\n\r\n", 5);

    // Read the response headers; anything but 200 means the studio rejected it.
    String resp;
    uint32_t rt = millis();
    while (resp.indexOf("\r\n\r\n") < 0 && millis() - rt < 10000) {
        if (client.available()) resp += (char)client.read();
        else delay(5);
    }
    client.stop();
    bool ok = resp.startsWith("HTTP/1.1 200");
    Serial.printf("clip: %d frames -> studio (%s)\n", got, ok ? "accepted" : "rejected");
    return ok;
}

static bool doVoiceNote() {
    // recBuf was filled by the mic tee in loop().
    if (!recBuf) return false;
    bool ok = sendTelegramAudio(recBuf, recFill);
    free(recBuf);
    recBuf = nullptr;
    return ok;
}

// --- camera setting commands ---------------------------------------------

static const int8_t STEP_OK = 1;
static int stepSetting(int &v, int delta, int lo, int hi) {
    int n = v + delta;
    if (n < lo || n > hi) return -1;   // already at the limit
    v = n;
    return STEP_OK;
}

// Returns a reply string; empty means "handled silently" (never happens — every path replies).
static String handleSettings(const String &t) {
    sensor_t *s = nullptr;
    if (cameraReady) s = esp_camera_sensor_get();
    else if (initCamera()) s = esp_camera_sensor_get();

    if (t.indexOf("exposure") >= 0 || t == "dim" || t == "bright") {
        int v = cfg.exposure;
        if (t.indexOf("dim") >= 0) v = 0;
        else if (t.indexOf("bright") >= 0) v = 2;
        else if (t.indexOf("medium") >= 0 || t.indexOf("mid") >= 0) v = 1;
        cfg.exposure = v; saveConfig();
        if (s) applyCameraTuning();
        return "\xF0\x9F\x93\xB7 exposure set to " + String(v == 0 ? "dim" : v == 2 ? "bright" : "medium");
    }
    const char *names[] = { "brightness", "contrast", "saturation" };
    int *vals[] = { &cfg.brightness, &cfg.contrast, &cfg.saturation };
    for (int i = 0; i < 3; i++) {
        if (t.indexOf(names[i]) >= 0) {
            int delta = t.indexOf("+") >= 0 ? 1 : -1;
            if (stepSetting(*vals[i], delta, -2, 2) != STEP_OK)
                return String("\xF0\x9F\x8C\xB5 ") + names[i] + " is already at its limit (" + String(*vals[i]) + ").";
            saveConfig();
            if (s) applyCameraTuning();
            return String("\xF0\x9F\x8C\xB5 ") + names[i] + " now " + String(*vals[i]);
        }
    }
    if (t.indexOf("effect") >= 0 || t.indexOf("negative") >= 0 || t.indexOf("grayscale") >= 0) {
        int v = 0;
        if (t.indexOf("none") >= 0) v = 0;
        else if (t.indexOf("negative") >= 0) v = 1;
        else if (t.indexOf("grayscale") >= 0 || t.indexOf("gray") >= 0) v = 2;
        else if (t.indexOf("antique") >= 0) v = 3;
        else if (t.indexOf("sepia") >= 0) v = 4;
        cfg.effect = v; saveConfig();
        if (s && s->set_special_effect) s->set_special_effect(s, (uint8_t)v);
        return "\xF0\x9F\x8C\xB5 effect set";
    }
    if (t.indexOf("wb") >= 0 || t.indexOf("white balance") >= 0) {
        int v = cfg.wbMode;
        if (t.indexOf("daylight") >= 0 || t.indexOf("sunny") >= 0) v = 1;
        else if (t.indexOf("cloudy") >= 0) v = 2;
        else if (t.indexOf("office") >= 0) v = 3;
        else if (t.indexOf("home") >= 0 || t.indexOf("incandescent") >= 0) v = 4;
        else v = 0;   // auto
        cfg.wbMode = v; saveConfig();
        if (s) applyCameraTuning();
        return "\xF0\x9F\x8C\xB5 white balance set";
    }
    if (t.indexOf("night") >= 0) {
        bool on = t.indexOf("on") >= 0 && t.indexOf("off") < 0;
        cfg.nightMode = on; saveConfig();
        if (s && s->set_aec2) s->set_aec2(s, on ? 1 : 0);
        return "\xF0\x9F\x8C\xB5 night mode " + String(on ? "on" : "off");
    }
    return "";
}

// --- scheduler -------------------------------------------------------------

struct Job { bool used; int action; uint32_t intervalMs; uint32_t nextRunMs; };  // action 0=photo 1=video
static Job jobs[SCHED_MAX_JOBS];

static void loadJobs() {
    for (int i = 0; i < SCHED_MAX_JOBS; i++) {
        char k[8]; snprintf(k, sizeof(k), "job%d", i);
        jobs[i].used = prefs.getInt(k, 0) != 0;
        if (jobs[i].used) {
            char ka[12], ki[12];
            snprintf(ka, sizeof(ka), "job%da", i);
            snprintf(ki, sizeof(ki), "job%di", i);
            jobs[i].action = prefs.getInt(ka, 0);
            jobs[i].intervalMs = (uint32_t)prefs.getInt(ki, 0);
            if (jobs[i].intervalMs < 1000) { jobs[i].used = false; continue; }
        } else {
            jobs[i].action = 0;
            jobs[i].intervalMs = 0;
        }
        jobs[i].nextRunMs = 0;   // first tick one interval after boot
    }
}

static void saveJobs() {
    for (int i = 0; i < SCHED_MAX_JOBS; i++) {
        char k[8], ka[12], ki[12];
        snprintf(k, sizeof(k), "job%d", i);
        snprintf(ka, sizeof(ka), "job%da", i);
        snprintf(ki, sizeof(ki), "job%di", i);
        prefs.putInt(k, jobs[i].used ? 1 : 0);
        if (jobs[i].used) { prefs.putInt(ka, jobs[i].action); prefs.putInt(ki, (int)jobs[i].intervalMs); }
    }
}

static int freeJobSlot() {
    for (int i = 0; i < SCHED_MAX_JOBS; i++) if (!jobs[i].used) return i;
    return -1;
}

static String jobSummary() {
    String j = "";
    for (int i = 0; i < SCHED_MAX_JOBS; i++) {
        if (!jobs[i].used) continue;
        if (j.length()) j += ", ";
        j += String(jobs[i].action == 0 ? "photo" : "video") + " every " + String(jobs[i].intervalMs / 1000) + "s";
    }
    return j.length() ? j : "none";
}

// Parse "every 5 min video" / "video every 60 s". Returns ms, or -1.
static int parseInterval(const String &t) {
    int num = -1;
    for (int i = 0; i < t.length(); i++) {
        if (isDigit(t[i])) { num = t[i] - '0'; }
        else if (num >= 0) break;
    }
    if (num <= 0) return -1;
    int ms = num * 1000;
    if (t.indexOf("min") >= 0) ms *= 60;
    return ms;
}

static String handleSchedule(const String &t) {
    bool video = t.indexOf("video") >= 0 || t.indexOf("clip") >= 0;
    int ms = parseInterval(t);
    if (ms < 0) return "\xF0\x9F\x8C\xB5 how often? e.g. \u201cevery 5 min photo\u201d";
    int minMs = video ? SCHED_VIDEO_MS_MIN : SCHED_PHOTO_MS_MIN;
    if (ms < minMs)
        return "\xF0\x9F\x8C\xB5 too fast — " + String(video ? "clips" : "photos") +
               " need at least " + String(minMs / 1000) + " s between runs.";
    if (ms > SCHED_MS_MAX)
        return "\xF0\x9F\x8C\xB5 that's over an hour — I cap timers at 3600 s.";
    int slot = freeJobSlot();
    if (slot < 0)
        return "\xF0\x9F\x8C\xB5 all " + String(SCHED_MAX_JOBS) + " timers are full. Send \u201cstop\u201d first.";
    jobs[slot].used = true;
    jobs[slot].action = video ? 1 : 0;
    jobs[slot].intervalMs = ms;
    jobs[slot].nextRunMs = millis() + ms;
    saveJobs();
    return "\xF0\x9F\x8C\xB5 timer set: " + String(video ? "video" : "photo") + " every " +
           (ms % 60000 == 0 ? String(ms / 60000) + " min" : String(ms / 1000) + " s") + ".";
}

static void clearJobs() {
    for (int i = 0; i < SCHED_MAX_JOBS; i++) jobs[i].used = false;
    saveJobs();
}

// --- command dispatch --------------------------------------------------------

static const char HELP[] =
    "photo — send one now\n"
    "video [s] — clip, default 5 s (2–15)\n"
    "record [s] — voice note, default 8 s (2–20)\n"
    "exposure dim|medium|bright\n"
    "brightness +1 / -1 · contrast +1 / -1 · saturation +1 / -1\n"
    "effect none|negative|grayscale|antique|sepia\n"
    "wb auto|daylight|cloudy|office|home\n"
    "night on|off\n"
    "every <n> s|min photo|video — timer\n"
    "stop — clear timers\n"
    "status";

static void handleCommand(const TgUpdate &u) {
    String t = u.text;
    t.trim();
    if (t.length() > 120) t = t.substring(0, 120);   // parser horizon
    t.toLowerCase();

    int msgId = ack("received");
    if (msgId < 0) return;   // no network — nothing else to do

    if (t.indexOf("status") >= 0) {
        uint32_t now = millis();
        String r = "\xF0\x9F\x8C\xB5 status\n"
                 "wifi: " + (WiFi.isConnected() ? "up (" + String(WiFi.RSSI()) + " dBm)" : "down") + "\n"
                 "heap: " + String(ESP.getFreeHeap() / 1024) + " KB, psram free: " +
                 String(heap_caps_get_free_size(MALLOC_CAP_SPIRAM) / 1024) + " KB\n"
                 "timers: " + jobSummary() + "\n"
                 "last send: " + lastResult;
        tgEditText(msgId, r);
        return;
    }

    if (t.indexOf("stop") >= 0 || t.indexOf("cancel") >= 0) {
        int before = 0;
        for (int i = 0; i < SCHED_MAX_JOBS; i++) if (jobs[i].used) before++;
        clearJobs();
        tgEditText(msgId, before ? "\xF0\x9F\x8C\xB5 stopped " + String(before) + " timer(s)."
                                 : "\xF0\x9F\x8C\xB5 nothing was running.");
        return;
    }

    if (t.indexOf("every") >= 0 || t.indexOf("timer") >= 0) {
        tgEditText(msgId, handleSchedule(t));
        return;
    }

    String settingsReply = handleSettings(t);
    if (settingsReply.length()) { tgEditText(msgId, settingsReply); return; }

    if (t.indexOf("photo") >= 0 || t == "pic" || t.indexOf("picture") >= 0) {
        progress(msgId, "capturing…");
        bool ok = doPhoto("you asked");
        lastResult = ok ? "ok" : "fail";
        tgEditText(msgId, ok ? "\xF0\x9F\x8C\xB5 done." : "\xF0\x9F\x8C\xB5 the capture failed — check serial.");
        return;
    }

    if (t.indexOf("video") >= 0 || t.indexOf("clip") >= 0) {
        int ms = VIDEO_MS_DEFAULT;
        int n = parseInterval(t);
        if (n > 0) ms = n;
        if (ms < VIDEO_MS_MIN) ms = VIDEO_MS_MIN;
        if (ms > VIDEO_MS_MAX) {
            tgEditText(msgId, "\xF0\x9F\x8C\xB5 I cap clips at " + String(VIDEO_MS_MAX / 1000) + " s — doing that instead.");
            ms = VIDEO_MS_MAX;
        }
        progress(msgId, "rolling… " + String(ms / 1000) + " s");
        bool ok = doVideo(ms);
        lastResult = ok ? "ok" : "fail";
        tgEditText(msgId, ok ? "\xF0\x9F\x8C\xB5 clip sent to the studio — MP4 in a moment."
                             : "\xF0\x9F\x8C\xB5 the studio didn't answer. Is this Mac awake?");
        return;
    }

    if (t.indexOf("record") >= 0 || t.indexOf("voice") >= 0) {
        int ms = AUDIO_MS_DEFAULT;
        int n = parseInterval(t);
        if (n > 0) ms = n;
        if (ms < AUDIO_MS_MIN) ms = AUDIO_MS_MIN;
        if (ms > AUDIO_MS_MAX) {
            tgEditText(msgId, "\xF0\x9F\x8C\xB5 I cap notes at " + String(AUDIO_MS_MAX / 1000) + " s — doing that instead.");
            ms = AUDIO_MS_MAX;
        }
        if (recActive) { tgEditText(msgId, "\xF0\x9F\x8C\xB5 already recording — one at a time."); return; }
        recStart(ms);
        if (!recActive) { tgEditText(msgId, "\xF0\x9F\x8C\xB5 out of memory for that."); return; }
        progress(msgId, "listening… " + String(ms / 1000) + " s");
        // The mic tee in loop() fills recBuf; doVoiceNote runs when it's full.
        return;
    }

    if (t.indexOf("help") >= 0 || t == "?") {
        tgEditText(msgId, HELP);
        return;
    }

    tgEditText(msgId, String("\xF0\x9F\x8C\xB5 I didn't catch that. ") + HELP);
}

// ============================ bark event handler (barkcam's)

static bool scheduleActive() {
    time_t t = time(nullptr);
    if (t < 1600000000) return true;
    struct tm lt;
    localtime_r(&t, &lt);
    int bit = (lt.tm_wday == 0) ? 6 : (lt.tm_wday - 1);
    if (!((cfg.daysMask >> bit) & 1)) return false;
    return ((cfg.hoursMask >> lt.tm_hour) & 1) != 0;
}

static void handleBarkEvent(const char *why, bool force) {
    uint32_t now = millis();

    if (!force && !scheduleActive()) {
        static uint32_t lastSchedPrint = 0;
        if (now - lastSchedPrint > 10000) {
            Serial.println("bark event — schedule off (quiet hours), skipping");
            lastSchedPrint = now;
        }
        return;
    }

    if (!force) {
        if ((now - lastBarkMs) > EPISODE_GAP_MS) episodeCount = 0;
        lastBarkMs = now;
    }

    bool allow = force ||
                 (now - lastSendMs >= cfg.cooldownMs) ||
                 (now - lastSendMs >= EPISODE_REPEAT_MS && episodeCount < EPISODE_MAX);
    if (!allow) {
        static uint32_t lastSkipPrint = 0;
        if (now - lastSkipPrint > 10000) {
            Serial.printf("bark event (%s) throttled — skipping\n", why);
            lastSkipPrint = now;
        }
        return;
    }

    lastSendMs = now;
    busy = true;
    LED_ON();
    uint32_t waitStart = millis();
    while (!WiFi.isConnected() && (millis() - waitStart < 20000)) { loopTick++; delay(100); }

    Serial.printf("BARK EVENT (%s) — capturing + sending\n", why);

    uint8_t *jpg = nullptr;
    size_t jpgLen = 0;
    bool ok = false;
    if (WiFi.isConnected()) {
        ok = capturePhoto(&jpg, &jpgLen) &&
             sendTelegramPhoto(jpg, jpgLen, String("\xF0\x9F\x95\x95 Bark detected") + (clockString().length() ? " " + clockString() : ""));
    } else {
        Serial.println("wifi not connected — skipping send");
    }
    if (ok) archivePhoto(jpg, jpgLen);
    if (jpg) free(jpg);

    busy = false;
    LED_OFF();
    lastResult = ok ? "ok" : "fail";

    if (!ok) {
        uint32_t retry = cfg.cooldownMs < 30000 ? cfg.cooldownMs : 30000;
        lastSendMs = now - (cfg.cooldownMs > retry ? cfg.cooldownMs - retry : 0);
    } else if (!force) {
        episodeCount++;
    }
}

// ============================ config access point (barkcam's, renamed)

static void connectSTA();
static void startAP() {
    WiFi.mode(WIFI_AP_STA);
    WiFi.softAP(AP_SSID);
    apMode = true;
    apStartMs = millis();
    static bool mdnsUp = false;
    if (!mdnsUp) { MDNS.begin("cactuscam"); mdnsUp = true; }
    apEmptySince = 0;
    apClientEver = false;
    server.begin();
    connectSTA();
    Serial.printf("config AP '%s' up for %lu min — phone: http://cactuscam.local (or 192.168.4.1)\n",
                  AP_SSID, (unsigned long)(cfg.apWindowMs / 60000));
}

static void exitAP() {
    if (!apMode) return;
    apMode = false;
    server.close();
    WiFi.softAPdisconnect(true);
    WiFi.mode(WIFI_STA);
    WiFi.setSleep(true);
}

static void connectSTA() {
    if (cfg.ssid.length()) WiFi.begin(cfg.ssid, cfg.pass);
}

static void handleRoot() { server.send_P(200, "text/html", UI_PAGE); }

static float normDb(float db) {
    float v = (db + 80.0f) / 60.0f;
    return v < 0 ? 0 : (v > 1 ? 1 : v);
}

static void handleLevel() {
    String j = "{\"mode\":\"";
    j += apMode ? (WiFi.isConnected() ? "config + online" : "config, no wifi") : (WiFi.isConnected() ? "online" : "offline");
    j += "\",\"db\":" + String(normDb(detector.lastFrameDb()), 2)
       + ",\"noise\":" + String(normDb(detector.noiseDb()), 2)
       + ",\"thr\":" + String(normDb(detector.thresholdDb()), 2) + ",\"hist\":[";
    const float *h = detector.hist();
    int idx = detector.histIdx();
    for (int i = 0; i < BarkDetector::HIST_LEN; i++) {
        if (i) j += ",";
        j += String(normDb(h[(idx + i) % BarkDetector::HIST_LEN]), 2);
    }
    j += "]}";
    server.send(200, "application/json", j);
}

static void handleGetConfig() {
    String j = "{\"ssid\":\"" + cfg.ssid + "\",\"pass\":\"" + cfg.pass
             + "\",\"token\":\"" + cfg.botToken + "\",\"chatId\":\"" + cfg.chatId
             + "\",\"margin\":" + String(cfg.marginDb, 1)
             + ",\"rotate\":" + String(cfg.rotate)
             + ",\"exposure\":" + String(cfg.exposure)
             + ",\"apWindowMs\":" + String((uint32_t)cfg.apWindowMs)
             + ",\"daysMask\":" + String((int)cfg.daysMask)
             + ",\"hoursMask\":" + String((int)cfg.hoursMask) + "}";
    server.send(200, "application/json", j);
}

static void handlePostConfig() {
    bool changed = false;
    if (server.hasArg("ssid"))       { cfg.ssid = server.arg("ssid"); changed = true; }
    if (server.hasArg("pass"))       { cfg.pass = server.arg("pass"); changed = true; }
    if (server.hasArg("token"))      { cfg.botToken = server.arg("token"); changed = true; }
    if (server.hasArg("chatId"))     { cfg.chatId = server.arg("chatId"); changed = true; }
    if (server.hasArg("margin"))     { cfg.marginDb = server.arg("margin").toFloat(); detector.setMargin(cfg.marginDb); changed = true; }
    if (server.hasArg("rotate"))     { int v = server.arg("rotate").toInt(); if (v == 0 || v == 3) { cfg.rotate = v; changed = true; } }
    if (server.hasArg("exposure"))   { int v = server.arg("exposure").toInt(); if (v >= 0 && v <= 2) { cfg.exposure = v; changed = true; } }
    if (server.hasArg("apWindowMs")) { uint32_t v = server.arg("apWindowMs").toInt(); if (v >= 5000) { cfg.apWindowMs = v; changed = true; } }
    if (server.hasArg("daysMask"))   { int v = server.arg("daysMask").toInt(); cfg.daysMask = (uint8_t)(v & 0x7F); changed = true; }
    if (server.hasArg("hoursMask"))  { int v = server.arg("hoursMask").toInt(); cfg.hoursMask = (uint32_t)(v & 0xFFFFFF); changed = true; }

    if (changed) { saveConfig(); if (cameraReady) applyCameraTuning(); }
    Serial.println("config updated via web UI");

    if (apMode && server.hasArg("ssid") && cfg.ssid.length()) {
        WiFi.disconnect();
        connectSTA();
    }
    server.send(200, "application/json", changed ? "{\"ok\":true}" : "{\"ok\":false}");
}

static void handlePostTest() {
    pendingTest = true;
    lastResult = "pending";
    server.send(200, "application/json", "{\"ok\":true,\"msg\":\"test started\"}");
}

static void handleStatus() {
    uint32_t now = millis();
    String j = "{\"mode\":\"";
    j += apMode ? "ap" : (WiFi.isConnected() ? "sta" : "offline");
    j += "\",\"rssi\":" + String(WiFi.RSSI())
       + ",\"pendingTest\":" + (pendingTest ? "true" : "false")
       + ",\"lastResult\":\"" + lastResult + "\"}";
    server.send(200, "application/json", j);
}

static void handleClose() {
    if (apMode) { Serial.println("config closed via web UI"); exitAP(); }
    server.send(200, "application/json", "{\"ok\":true}");
}

static void setupWeb() {
    server.on("/", HTTP_GET, handleRoot);
    server.on("/level", HTTP_GET, handleLevel);
    server.on("/config", HTTP_GET, handleGetConfig);
    server.on("/config", HTTP_POST, handlePostConfig);
    server.on("/test", HTTP_POST, handlePostTest);
    server.on("/close", HTTP_POST, handleClose);
    server.on("/status", HTTP_GET, handleStatus);
}

// ============================ status + watchdog (barkcam's)

static void printStatus() {
    uint32_t now = millis();
    Serial.printf(
        "--- cactuscam status ---\n"
        "mode        : %s\n"
        "noise floor : %6.1f dBFS\n"
        "threshold   : %6.1f dBFS (margin %.1f)\n"
        "wifi rssi   : %d dBm  heap: %u KB  psram free: %u KB\n"
        "timers      : %s\n",
        apMode ? "config AP" : (WiFi.isConnected() ? "sta" : "offline"),
        detector.noiseDb(), detector.thresholdDb(), detector.marginDb(),
        WiFi.RSSI(),
        (unsigned)(ESP.getFreeHeap() / 1024),
        (unsigned)(heap_caps_get_free_size(MALLOC_CAP_SPIRAM) / 1024),
        jobSummary().c_str());
}

struct NetInfo { String ssid; int rssi; uint8_t ch; };
static void wifiScan() {
    Serial.println("scanning...");
    int n = WiFi.scanNetworks();
    if (n <= 0) { Serial.println("no networks found"); return; }
    int m = min(n, 24);
    static NetInfo nets[24];
    for (int i = 0; i < m; i++) { nets[i].ssid = WiFi.SSID(i); nets[i].rssi = WiFi.RSSI(i); nets[i].ch = WiFi.channel(i); }
    for (int i = 1; i < m; i++) {
        NetInfo key = nets[i];
        int j = i - 1;
        while (j >= 0 && nets[j].rssi < key.rssi) { nets[j + 1] = nets[j]; j--; }
        nets[j + 1] = key;
    }
    for (int i = 0; i < m && i < 12; i++)
        Serial.printf("%2d. %-24s %3d dBm  ch%u\n", i + 1, nets[i].ssid.c_str(), nets[i].rssi, nets[i].ch);
}

static void watchdogTask(void *) {
    uint32_t last = loopTick;
    uint32_t staleMs = 0;
    while (true) {
        vTaskDelay(pdMS_TO_TICKS(1000));
        if (loopTick != last) { last = loopTick; staleMs = 0; }
        else                  { staleMs += 1000; }
        if (staleMs > WATCHDOG_TIMEOUT_MS) {
            Serial.println("WATCHDOG: loop stalled — rebooting");
            ESP.restart();
        }
    }
}

// ============================ setup / loop

void setup() {
    Serial.begin(115200);

    pinMode(LED_PIN, OUTPUT);
    LED_OFF();

    loadConfig();
    detector.begin();
    detector.setMargin(cfg.marginDb);

    micReady = initMic();

    setupWeb();
    startAP();

    uint32_t t0 = millis();
    while (millis() - t0 < 3000) { server.handleClient(); delay(50); }

    Serial.println();
    Serial.printf("=== CACTUSCAM v1 (fw %d) ===\n", FIRMWARE_VERSION);
    if (micReady) Serial.println("mic ready (PDM 16 kHz)");
    else          Serial.println("!! MIC INIT FAILED — is the Sense expansion attached? !!");

    tgQueue = xQueueCreate(4, sizeof(TgUpdate));
    xTaskCreatePinnedToCore(tgPollTask, "tgpoll", 16384, nullptr, 2, nullptr, 0);
    xTaskCreatePinnedToCore(watchdogTask, "wd", 2048, nullptr, 1, nullptr, 1);

    loadJobs();
    if (jobs[0].used || jobs[1].used || jobs[2].used) {
        for (int i = 0; i < SCHED_MAX_JOBS; i++)
            if (jobs[i].used) jobs[i].nextRunMs = millis() + jobs[i].intervalMs;
    }

    Serial.println("commands: t=photo v=video r=voice s=status c=clear cooldown w=scan i=wifi a=config AP 1/2=tune");
}

void loop() {
    loopTick++;

    if (millis() > PERIODIC_REBOOT_MS) { Serial.println("uptime reached — periodic reboot"); ESP.restart(); }

    // --- audio: detector always; voice note tees the same samples ---
    if (micReady) {
        static int16_t frameBuf[FRAME_SAMPLES];
        static size_t fill = 0;
        int16_t chunk[256];
        size_t gotBytes = 0;
        i2s_read(I2S_PORT, chunk, sizeof(chunk), &gotBytes, pdMS_TO_TICKS(50));
        size_t n = gotBytes / 2;
        for (size_t i = 0; i < n; i++) {
            if (recActive && recBuf && recFill + 1 < recTarget) {
                recBuf[recFill++] = (uint8_t)(chunk[i] & 0xFF);
                if (recFill + 1 < recTarget) recBuf[recFill++] = (uint8_t)((chunk[i] >> 8) & 0xFF);
            }
            frameBuf[fill++] = chunk[i];
            if (fill == FRAME_SAMPLES) { detector.processFrame(frameBuf, millis()); fill = 0; }
        }
    } else {
        delay(50);
    }

    // --- voice note finished? send it. ---
    if (recActive && recFill >= recTarget) {
        recActive = false;
        busy = true;
        bool ok = doVoiceNote();
        busy = false;
        lastResult = ok ? "ok" : "fail";
        Serial.printf("voice note sent: %s\n", ok ? "ok" : "failed");
    }

    if (apMode) server.handleClient();

    if (apMode && (millis() - apStartMs >= cfg.apWindowMs)) {
        Serial.println("config window closed — dropping config AP");
        exitAP();
    }

    if (apMode) {
        if (WiFi.softAPgetStationNum() > 0) { apClientEver = true; apEmptySince = 0; }
        else if (apClientEver) {
            if (!apEmptySince) apEmptySince = millis();
            else if (millis() - apEmptySince > 10000) {
                Serial.println("config client gone — dropping config AP");
                apEmptySince = 0;
                exitAP();
            }
        }
    }

    // --- serial commands ---
    while (Serial.available()) {
        char c = Serial.read();
        switch (c) {
            case 't': if (!busy) handleBarkEvent("manual test", true); break;
            case 'v': if (!busy) { busy = true; bool ok = doVideo(3000); busy = false; lastResult = ok ? "ok" : "fail"; } break;
            case 'r': if (!busy && !recActive) recStart(4000); break;
            case 's': printStatus(); break;
            case 'c': lastSendMs = 0; Serial.println("cooldown cleared"); break;
            case 'w': wifiScan(); break;
            case 'i': Serial.printf("wifi: ssid=%s ip=%s rssi=%d\n", WiFi.SSID().c_str(),
                      WiFi.localIP().toString().c_str(), WiFi.RSSI()); break;
            case 'a': startAP(); Serial.println("config AP reopened"); break;
            case '1': detector.setMargin(detector.marginDb() + 2.0f);
                      cfg.marginDb = detector.marginDb(); saveConfig(); break;
            case '2': detector.setMargin(detector.marginDb() - 2.0f);
                      cfg.marginDb = detector.marginDb(); saveConfig(); break;
        }
    }

    // --- incoming telegram commands (one per loop pass keeps the mic fed) ---
    if (tgQueue && !busy) {
        TgUpdate u;
        if (xQueueReceive(tgQueue, &u, 0) == pdTRUE) handleCommand(u);
    }

    // --- scheduler: fire due jobs when idle ---
    if (!busy) {
        for (int i = 0; i < SCHED_MAX_JOBS; i++) {
            if (!jobs[i].used) continue;
            if (jobs[i].nextRunMs == 0) { jobs[i].nextRunMs = millis() + jobs[i].intervalMs; continue; }
            if ((int32_t)(millis() - jobs[i].nextRunMs) >= 0) {
                jobs[i].nextRunMs = millis() + jobs[i].intervalMs;
                Serial.printf("timer %d firing (%s)\n", i, jobs[i].action ? "video" : "photo");
                if (jobs[i].action == 0) handleBarkEvent("timer", true);
                else { busy = true; doVideo(VIDEO_MS_DEFAULT); busy = false; }
            }
        }
    }

    // --- web test request ---
    if (pendingTest && !busy) {
        pendingTest = false;
        handleBarkEvent("web test", true);
    }

    // --- bark event? ---
    if (!busy && detector.eventDetected()) handleBarkEvent("barks", false);

    // --- STA recovery (barkcam's) ---
    if (!apMode && WiFi.isConnected()) {
        everConnected = true;
        staDiscSince = 0;
    } else if (!apMode && !WiFi.isConnected()) {
        uint32_t grace = everConnected ? 60000 : 15000;
        static uint32_t lastConnLog = 0;
        if (millis() - lastConnLog > 5000) {
            lastConnLog = millis();
            Serial.printf("wifi: not connected (status %d)\n", WiFi.status());
        }
        if (!staDiscSince) staDiscSince = millis();
        else if (millis() - staDiscSince > grace) {
            Serial.println("wifi lost — reopening config AP");
            staDiscSince = 0;
            startAP();
        }
    }

    // --- LED: AP = 2 Hz blink, idle = 1 Hz, working = solid ---
    if (!busy) {
        static uint32_t lastBlink = 0;
        static bool on = false;
        uint32_t period = apMode ? 250 : 500;
        if (millis() - lastBlink > period) {
            on = !on;
            digitalWrite(LED_PIN, on ? LOW : HIGH);
            lastBlink = millis();
        }
    }
}
