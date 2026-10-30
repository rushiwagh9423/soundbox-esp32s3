/*
 * Simple Payment Soundbox (ESP32-S3) - Razorpay + Cloudflare + local CSV
 * ------------------------------------------------------------------
 * Display : SH1106 1.3" OLED (I2C)
 * Mic     : disabled in menu-question build (wiring preserved)
 * Amp     : MAX98357A (I2S TX)
 * Keypad  : 4x4 matrix
 * Cloud   : Razorpay (QR + verification), Cloudflare Workers AI (speech-to-text + answers),
 *           Cloudflare Workers AI for STT, answers, and speech
 *
 * KEYS
 *   0-9  digits         *  decimal point      #  backspace
 *   C    clear/cancel   D  dynamic QR: press D -> type amount -> press D again
 *   B    fixed QR              A  open/select approved question menu
 *
 * Libraries (Library Manager): U8g2, Keypad (Mark Stanley), QRCode (Richard Moore),
 *                              ArduinoJson (v7)
 * Board: "ESP32S3 Dev Module", USB CDC On Boot = Enabled,
 *        PSRAM = OPI PSRAM (N8R8/N16R8) or QSPI PSRAM (N8R2) / Disabled if none.
 * GPIO 16 is NOT used anywhere in this sketch.
 *
 * SETUP PORTAL (no keys inside the code):
 *   - First boot, or Wi-Fi fails, or you HOLD key C while powering on, or you HOLD key #
 *     for 2 s on the home screen  ->  device starts hotspot "Soundbox-Setup".
 *   - Phone: join Wi-Fi "Soundbox-Setup" (password soundbox2026), page opens
 *     automatically (or browse to 192.168.4.1). Fill Wi-Fi, Razorpay, and Cloudflare settings, then Save.
 *   - Settings are stored in flash (NVS). Device restarts and is ready to use.
 */

#include <Arduino.h>
#include <WiFi.h>
#include <WiFiClientSecure.h>
#include <WebServer.h>
#include <DNSServer.h>
#include <HTTPClient.h>
#include <Preferences.h>
#include <LittleFS.h>
#include <ArduinoJson.h>
#include <Wire.h>
#include <U8g2lib.h>
#include <Keypad.h>
#include "qrcode.h"
#include <driver/i2s.h>
#include <time.h>
#include <math.h>

// ======================= SETTINGS (filled from the setup portal) =======================
#define AP_NAME          "Soundbox-Setup"
#define AP_PASS          "soundbox2026"          // hotspot password (min 8 chars)

struct Cfg {
  String ssid, pass;
  String rzpId, rzpSecret, store;
  String cfWorkerUrl, cfToken;
  String fixedAmount = "100.00"; // B-key Payment Link amount in rupees
  int    vol = 80;                            // speaker volume 0-100
};
Cfg cfg;

#define WIFI_SSID        cfg.ssid.c_str()
#define WIFI_PASS        cfg.pass.c_str()
#define RZP_KEY_ID       cfg.rzpId.c_str()
#define RZP_KEY_SECRET   cfg.rzpSecret.c_str()
#define STORE_NAME       cfg.store.c_str()
#define SPEAKER_VOLUME   (cfg.vol / 100.0f)
// =========================================================================================

// ----------------------- PINS (GPIO16 avoided) --------------
#define OLED_SDA   8
#define OLED_SCL   9

#define MIC_SCK    4    // INMP441 SCK
#define MIC_WS     5    // INMP441 WS
#define MIC_SD     6    // INMP441 SD

#define SPK_BCLK   15   // MAX98357 BCLK
#define SPK_LRC    7    // MAX98357 LRC
#define SPK_DIN    17   // MAX98357 DIN

byte rowPins[4] = {10, 11, 12, 13};
byte colPins[4] = {14, 18, 21, 47};
// -----------------------------------------------------------

#define SPK_RATE         24000          // Worker returns WAV PCM16, mono, 24 kHz
#define SPK_PORT         I2S_NUM_1

// ----------------------- Globals ---------------------------
U8G2_SH1106_128X64_NONAME_F_HW_I2C u8g2(U8G2_R0, U8X8_PIN_NONE);

char keys[4][4] = {
  {'1', '2', '3', 'A'},
  {'4', '5', '6', 'B'},
  {'7', '8', '9', 'C'},
  {'*', '0', '#', 'D'}
};
Keypad kpd = Keypad(makeKeymap(keys), rowPins, colPins, 4, 4);

Preferences prefs;
bool csvReady = false;
uint32_t speakerI2sBytesWritten = 0;
volatile bool assistantBusy = false;
String pendingQuestion = "";
int selectedQuestion = -1;
int customDays = 30;
String customDaysInput = "";
const char* questionText[] = {
  "What was my latest payment?",
  "How much did I receive today?",
  "How many payments today?",
  "What is this month summary?",
  "How much in the last custom days?"
};
const int QUESTION_COUNT = 5;


enum State { ST_IDLE, ST_ENTER_AMOUNT, ST_Q_MENU, ST_Q_CUSTOM_DAYS, ST_Q_CONFIRM, ST_Q_QR_WAIT, ST_Q_ASKING, ST_Q_RESULT, ST_QR_WAIT };
State state = ST_IDLE;

String amountStr = "";
String activeQrId = "";
String activeQrData = "";
String lastSeenPaymentId = "";
String sttLastError = "";
bool   activeIsFixed = false;
long   activeAmountPaise = 0;
unsigned long lastPoll = 0;
unsigned long activeLinkStarted = 0;
unsigned long lastQrRefresh = 0;
const unsigned long PAYMENT_LINK_TTL_MS = 60000UL;

String fixedQrId, fixedQrData;


// ======================= Keypad events =======================
volatile bool portalRequested = false;

void keypadEvent(KeypadEvent k) {
  KeyState s = kpd.getState();
  if (k == '#' && s == HOLD) {
    portalRequested = true;                  // hold # for 2 s -> setup portal
  }
}

// ======================= Display helpers =====================
void oledText(const char* l1, const char* l2 = nullptr, const char* l3 = nullptr, const char* l4 = nullptr) {
  u8g2.clearBuffer();
  u8g2.setFont(u8g2_font_6x12_tf);
  if (l1) u8g2.drawStr(0, 12, l1);
  if (l2) u8g2.drawStr(0, 28, l2);
  if (l3) u8g2.drawStr(0, 44, l3);
  if (l4) u8g2.drawStr(0, 60, l4);
  u8g2.sendBuffer();
}

void drawHome() {
  u8g2.clearBuffer();
  u8g2.setFont(u8g2_font_helvB10_tf);
  const char* title = "PAY SOUNDBOX";
  int titleX = (128 - u8g2.getStrWidth(title)) / 2;
  if (titleX < 0) titleX = 0;
  u8g2.drawStr(titleX, 16, title);
  u8g2.setFont(u8g2_font_5x8_tf);
  u8g2.drawStr(4, 28, "D - Enter amount");
  u8g2.drawStr(4, 38, "B - Preset amount");
  u8g2.drawStr(4, 48, "A - Questions");
  u8g2.drawStr(4, 58, "Ready for payments");
  u8g2.sendBuffer();
}

void drawQuestionMenu() {
  u8g2.clearBuffer();
  u8g2.setFont(u8g2_font_5x8_tf);
  u8g2.drawStr(0, 8, "QUESTIONS  C=UP D=DOWN");
  for (int i = 0; i < QUESTION_COUNT; i++) {
    int y = 18 + i * 9;
    if (i == selectedQuestion) { u8g2.drawBox(0, y - 7, 128, 9); u8g2.setDrawColor(0); }
    char line[22]; snprintf(line, sizeof(line), "%d %s", i + 1, questionText[i]);
    u8g2.drawStr(2, y, line);
    if (i == selectedQuestion) u8g2.setDrawColor(1);
  }
  u8g2.drawStr(0, 63, "A=select  #=back");
  u8g2.sendBuffer();
}

void drawCustomDays() {
  u8g2.clearBuffer();
  u8g2.setFont(u8g2_font_6x12_tf);
  u8g2.drawStr(0, 13, "CUSTOM PERIOD");
  u8g2.setFont(u8g2_font_logisoso20_tf);
  u8g2.drawStr(0, 45, customDaysInput.length() ? customDaysInput.c_str() : "30");
  u8g2.setFont(u8g2_font_5x8_tf);
  u8g2.drawStr(0, 62, "digits  A=OK  #=del  C=back");
  u8g2.sendBuffer();
}

void drawQuestionConfirm() {
  u8g2.clearBuffer();
  u8g2.setFont(u8g2_font_5x8_tf);
  u8g2.drawStr(0, 8, "CONFIRM QUESTION");
  String q = String(questionText[selectedQuestion]);
  if (selectedQuestion == 4) q = "How much in last " + String(customDays) + " days?";
  for (int i = 0; i < 4; i++) u8g2.drawStr(0, 20 + i * 10, q.substring(i * 21, (i + 1) * 21).c_str());
  u8g2.drawStr(0, 62, "A=ask  #=cancel");
  u8g2.sendBuffer();
}

void drawAmount() {
  u8g2.clearBuffer();
  u8g2.setFont(u8g2_font_6x12_tf);
  u8g2.drawStr(0, 12, "Enter amount (Rs)");
  u8g2.setFont(u8g2_font_logisoso20_tf);
  String s = amountStr.length() ? amountStr : "0";
  u8g2.drawStr(0, 46, s.c_str());
  u8g2.setFont(u8g2_font_5x8_tf);
  u8g2.drawStr(0, 62, "D=link C=back #=del *=.");
  u8g2.sendBuffer();
}

void drawQR(const String& data, const char* line1, const char* line2) {
  static uint8_t qrBuf[1024];
  QRCode qr;
  bool ok = false;
  for (int ver = 3; ver <= 10; ver++) {
    if (qrcode_getBufferSize(ver) > sizeof(qrBuf)) break;
    if (qrcode_initText(&qr, qrBuf, ver, ECC_LOW, data.c_str()) == 0) { ok = true; break; }
  }
  if (!ok) { oledText("QR too long", "cannot display"); return; }
  u8g2.clearBuffer();
  int scale = 60 / qr.size;
  if (scale < 1) scale = 1;
  int box = qr.size * scale + 4;
  int ox = 1, oy = (64 - box) / 2;
  if (oy < 0) oy = 0;
  u8g2.setDrawColor(1);
  u8g2.drawBox(ox, oy, box, box);          // white quiet zone
  u8g2.setDrawColor(0);
  for (uint8_t y = 0; y < qr.size; y++)
    for (uint8_t x = 0; x < qr.size; x++)
      if (qrcode_getModule(&qr, x, y))
        u8g2.drawBox(ox + 2 + x * scale, oy + 2 + y * scale, scale, scale);
  u8g2.setDrawColor(1);
  u8g2.setFont(u8g2_font_6x12_tf);
  int tx = ox + box + 4;
  if (tx < 128) {
    u8g2.drawStr(tx, 24, line1);
    u8g2.drawStr(tx, 40, line2);
    u8g2.setFont(u8g2_font_5x8_tf);
    u8g2.drawStr(tx, 60, "C=cancel");
  }
  u8g2.sendBuffer();
}

void showQrScreen() {
  char amount[16];
  char seconds[12];
  snprintf(amount, sizeof(amount), "Rs %.2f", activeAmountPaise / 100.0);
  unsigned long elapsed = millis() - activeLinkStarted;
  unsigned long left = elapsed >= PAYMENT_LINK_TTL_MS ? 0 : (PAYMENT_LINK_TTL_MS - elapsed + 999) / 1000;
  snprintf(seconds, sizeof(seconds), "%lus left", left);
  drawQR(activeQrData, amount, seconds);
}

void successAnimation(const char* amountText) {
  const unsigned long animationStarted = millis();
  for (int r = 4; r <= 34; r += 5) {
    u8g2.clearBuffer();
    u8g2.drawCircle(64, 32, r);
    if (r > 10) u8g2.drawCircle(64, 32, r - 8);
    u8g2.sendBuffer();
    delay(16);
  }
  // draw check mark progressively
  int pts[3][2] = {{46, 33}, {58, 45}, {82, 20}};
  u8g2.clearBuffer();
  u8g2.drawDisc(64, 32, 30);
  u8g2.setDrawColor(0);
  for (int t = 0; t <= 10; t++) {
    int x = 46 + (58 - 46) * t / 10, y = 33 + (45 - 33) * t / 10;
    u8g2.drawDisc(x, y, 2);
    u8g2.sendBuffer();
    delay(8);
  }
  for (int t = 0; t <= 14; t++) {
    int x = 58 + (82 - 58) * t / 14, y = 45 + (20 - 45) * t / 14;
    u8g2.drawDisc(x, y, 2);
    u8g2.sendBuffer();
    delay(6);
  }
  (void)pts;
  u8g2.setDrawColor(1);
  delay(100);
  u8g2.clearBuffer();
  u8g2.setFont(u8g2_font_helvB10_tf);
  u8g2.drawStr(8, 22, "PAYMENT");
  u8g2.drawStr(8, 38, "RECEIVED");
  u8g2.setFont(u8g2_font_6x12_tf);
  u8g2.drawStr(8, 58, amountText);
  u8g2.sendBuffer();
  unsigned long elapsed = millis() - animationStarted;
  if (elapsed < 450) delay(450 - elapsed);
}

// ======================= I2S audio ===========================
void i2sInit() {
  // Speaker TX is always ready; microphone operation is disabled in menu-question mode.
  // ---- Speaker (TX) ----
  i2s_config_t tx = {};
  tx.mode = (i2s_mode_t)(I2S_MODE_MASTER | I2S_MODE_TX);
  tx.sample_rate = SPK_RATE;
  tx.bits_per_sample = I2S_BITS_PER_SAMPLE_16BIT;
  tx.channel_format = I2S_CHANNEL_FMT_RIGHT_LEFT;    // we duplicate mono -> L+R
  tx.communication_format = I2S_COMM_FORMAT_STAND_I2S;
  tx.intr_alloc_flags = ESP_INTR_FLAG_LEVEL1;
  tx.dma_buf_count = 8;
  tx.dma_buf_len = 512;
  tx.use_apll = false;
  tx.tx_desc_auto_clear = true;
  i2s_pin_config_t txp = {};
  txp.mck_io_num = I2S_PIN_NO_CHANGE;
  txp.bck_io_num = SPK_BCLK;
  txp.ws_io_num = SPK_LRC;
  txp.data_out_num = SPK_DIN;
  txp.data_in_num = I2S_PIN_NO_CHANGE;
  i2s_driver_install(SPK_PORT, &tx, 0, NULL);
  i2s_set_pin(SPK_PORT, &txp);
  i2s_zero_dma_buffer(SPK_PORT);
}

void spkWriteMono(const int16_t* in, size_t n) {
  // MAX98357 receives standard-I2S stereo slots; duplicate mono into L/R.
  static int16_t st[512 * 2];
  size_t i = 0;
  while (i < n) {
    size_t chunk = min((size_t)512, n - i);
    for (size_t k = 0; k < chunk; k++) {
      int32_t sample = in[i + k];
      float gain = 1.8f * SPEAKER_VOLUME;
      if (sample >= 32760 || sample <= -32760) gain = SPEAKER_VOLUME; // don't boost already-clipped input
      int32_t scaled = (int32_t)((float)sample * gain);
      if (scaled > 32767) scaled = 32767;
      if (scaled < -32768) scaled = -32768;
      st[2 * k] = (int16_t)scaled;
      st[2 * k + 1] = (int16_t)scaled;
    }
    const uint8_t* bytes = (const uint8_t*)st;
    size_t target = chunk * 4;
    size_t offset = 0;
    while (offset < target) {
      size_t written = 0;
      esp_err_t e = i2s_write(SPK_PORT, bytes + offset, target - offset, &written, portMAX_DELAY);
      if (e != ESP_OK || written == 0) {
        Serial.printf("I2S TX write error=%d written=%u\n", (int)e, (unsigned)written);
        return;
      }
      offset += written;
      speakerI2sBytesWritten += written;
    }
    i += chunk;
  }
}

void playTone(int freq, int ms) {
  int n = SPK_RATE * ms / 1000;
  static int16_t b[256];
  int done = 0;
  while (done < n) {
    int c = min(256, n - done);
    for (int k = 0; k < c; k++)
      b[k] = (int16_t)(9000.0f * sinf(2.0f * PI * freq * (done + k) / SPK_RATE));
    spkWriteMono(b, c);
    done += c;
  }
}

void fallbackChime() { playTone(880, 150); playTone(1175, 150); playTone(1568, 250); }

// ======================= Networking ==========================
bool ensureWifi() {
  if (WiFi.status() == WL_CONNECTED) return true;
  WiFi.disconnect();
  WiFi.begin(WIFI_SSID, WIFI_PASS);
  unsigned long t = millis();
  while (WiFi.status() != WL_CONNECTED && millis() - t < 15000) delay(250);
  return WiFi.status() == WL_CONNECTED;
}

String rzp(const char* method, const String& path, const String& body, int* codeOut = nullptr) {
  WiFiClientSecure c; c.setInsecure();      // prototype only; pin a root CA for production
  HTTPClient http; http.setTimeout(15000);
  http.begin(c, String("https://api.razorpay.com") + path);
  http.setAuthorization(RZP_KEY_ID, RZP_KEY_SECRET);
  if (body.length()) http.addHeader("Content-Type", "application/json");
  int code = body.length() ? http.sendRequest(method, body)
                           : http.sendRequest(method, (uint8_t*)nullptr, 0);
  String resp = http.getString();
  http.end();
  if (codeOut) *codeOut = code;
  if (code < 200 || code >= 300) Serial.printf("Razorpay %s %s -> %d: %s\n", method, path.c_str(), code, resp.c_str());
  return resp;
}

// remove one optional "&key=value" parameter from a UPI link (smaller QR = easier to scan on a 1.3" OLED)
static void dropParam(String& s, const char* key) {
  String k = String("&") + key + "=";
  int i = s.indexOf(k);
  if (i < 0) return;
  int j = s.indexOf('&', i + 1);
  if (j < 0) s.remove(i); else s.remove(i, j - i);
}

bool createPaymentLink(bool fixed, long paise, String& idOut, String& dataOut) {
  JsonDocument d;
  d["amount"] = paise;
  d["currency"] = "INR";
  d["accept_partial"] = false;
  d["description"] = fixed ? String("Soundbox fixed amount - ") + STORE_NAME
                             : String("Soundbox payment - ") + STORE_NAME;
  d["notify"]["sms"] = false;
  d["notify"]["email"] = false;
  d["reminder_enable"] = false;
  String body; serializeJson(d, body);
  int code = 0;
  String r = rzp("POST", "/v1/payment_links", body, &code);
  if (code < 200 || code >= 300) return false;
  JsonDocument res;
  if (deserializeJson(res, r)) return false;
  idOut = res["id"].as<String>();
  dataOut = res["short_url"].as<String>();
  Serial.println("Payment Link ID: " + idOut);
  Serial.println("Payment Link URL: " + dataOut);
  return idOut.startsWith("plink_") && dataOut.startsWith("https://");
}

// Verify via Razorpay's fetch-by-ID API. Only announce after the link is paid
// and Razorpay includes a captured payment in its payments array.
bool latestPayment(const String& linkId, String& pid, long& paise) {
  int code = 0;
  String r = rzp("GET", "/v1/payment_links/" + linkId, "", &code);
  if (code != 200) return false;
  JsonDocument res;
  if (deserializeJson(res, r)) return false;
  const char* status = res["status"] | "";
  if (strcmp(status, "paid") != 0) return false;
  JsonArray items = res["payments"].as<JsonArray>();
  if (items.isNull() || items.size() == 0) return false;
  JsonObject payment = items[0];
  const char* paymentStatus = payment["status"] | "";
  if (strcmp(paymentStatus, "captured") != 0) return false;
  pid = payment["payment_id"].as<String>();
  paise = payment["amount"] | 0;
  return pid.length() && paise > 0;
}

void closeQr(const String& id) {
  if (id.length()) rzp("POST", "/v1/payment_links/" + id + "/cancel", "");
}

// ---------- Cloudflare speech pipeline ----------
// Speech-to-text: send raw WAV to the protected Cloudflare Worker.
// Cloudflare TTS contract: authenticated POST /speak with {"text":"..."};
// response is a canonical 44-byte-header WAV, PCM signed 16-bit little-endian,
// mono, 24 kHz. The Worker adapts its provider output to this device format.
bool cloudflareSpeak(const String& text, const char* language = "en") {
  if (!text.length() || !cfg.cfWorkerUrl.length() || !cfg.cfToken.length()) return false;
  String url = cfg.cfWorkerUrl; url.trim(); while (url.endsWith("/")) url.remove(url.length() - 1); if (!url.endsWith("/speak")) url += "/speak";
  JsonDocument payload; payload["text"] = text; payload["language"] = language; String body; serializeJson(payload, body);
  if (WiFi.status() != WL_CONNECTED && !ensureWifi()) { Serial.println("Cloudflare TTS: Wi-Fi unavailable"); return false; }
  if (!csvReady && !LittleFS.begin(false)) return false;
  LittleFS.remove("/tts_temp.wav");
  WiFiClientSecure client; client.setInsecure(); HTTPClient http; http.setTimeout(60000); http.setReuse(false);
  if (!http.begin(client, url)) return false;
  http.addHeader("Content-Type", "application/json"); http.addHeader("Authorization", String("Bearer ") + cfg.cfToken);
  int code = http.POST(body);
  if (code != 200) { Serial.printf("Cloudflare TTS HTTP %d: %s\n", code, http.getString().c_str()); http.end(); return false; }
  WiFiClient* stream = http.getStreamPtr();
  File out = LittleFS.open("/tts_temp.wav", FILE_WRITE);
  if (!out) { http.end(); return false; }
  int expected = http.getSize(); size_t total = 0; unsigned long last = millis(); uint8_t buf[1024];
  while ((expected < 0 || total < (size_t)expected) && millis() - last < 65000) {
    size_t avail = stream->available();
    if (!avail) { delay(2); continue; }
    size_t want = min(avail, sizeof(buf)); int n = stream->readBytes(buf, want);
    if (n <= 0) continue; out.write(buf, n); total += n; last = millis();
  }
  out.flush(); out.close(); http.end();
  if (total < 44) { LittleFS.remove("/tts_temp.wav"); return false; }
  File wav = LittleFS.open("/tts_temp.wav", FILE_READ); uint8_t h[44];
  if (!wav || wav.read(h, 44) != 44 || memcmp(h, "RIFF", 4) || memcmp(h + 8, "WAVE", 4) || memcmp(h + 12, "fmt ", 4) || memcmp(h + 36, "data", 4)) { if (wav) wav.close(); LittleFS.remove("/tts_temp.wav"); return false; }
  uint16_t format = h[20] | (uint16_t(h[21]) << 8), channels = h[22] | (uint16_t(h[23]) << 8), bits = h[34] | (uint16_t(h[35]) << 8);
  uint32_t rate = (uint32_t)h[24] | ((uint32_t)h[25] << 8) | ((uint32_t)h[26] << 16) | ((uint32_t)h[27] << 24);
  uint32_t pcmBytes = (uint32_t)h[40] | ((uint32_t)h[41] << 8) | ((uint32_t)h[42] << 16) | ((uint32_t)h[43] << 24);
  Serial.printf("WAV format=%u channels=%u rate=%lu bits=%u\n", format, channels, (unsigned long)rate, bits);
  if (format != 1 || channels != 1 || rate != SPK_RATE || bits != 16 || pcmBytes == 0 || pcmBytes > total - 44 || (pcmBytes & 1)) { wav.close(); LittleFS.remove("/tts_temp.wav"); return false; }
  i2s_set_clk(SPK_PORT, SPK_RATE, I2S_BITS_PER_SAMPLE_16BIT, I2S_CHANNEL_STEREO); i2s_zero_dma_buffer(SPK_PORT); speakerI2sBytesWritten = 0;
  Serial.println("TTS response complete"); Serial.printf("PCM bytes=%lu\n", (unsigned long)pcmBytes); Serial.println("I2S playback started");
  int16_t pcm[512]; uint8_t raw[sizeof(pcm)]; uint32_t left = pcmBytes;
  while (left) { size_t want = min((uint32_t)sizeof(raw), left); size_t n = wav.read(raw, want); if (n == 0) break; size_t samples = n / 2; for (size_t i = 0; i < samples; i++) pcm[i] = (int16_t)(raw[2*i] | ((uint16_t)raw[2*i+1] << 8)); spkWriteMono(pcm, samples); left -= samples * 2; }
  delay(180); wav.close(); LittleFS.remove("/tts_temp.wav"); bool ok = left == 0; Serial.printf("I2S bytes written=%lu\n", (unsigned long)speakerI2sBytesWritten); Serial.printf("I2S playback %s\n", ok ? "complete" : "incomplete"); return ok;
}

String cloudflareAsk(const String& question, int questionId, int days) {
  sttLastError = "";
  if (!cfg.cfWorkerUrl.length() || !cfg.cfToken.length()) {
    sttLastError = "Cloudflare not configured";
    return "";
  }
  String url = cfg.cfWorkerUrl;
  url.trim();
  while (url.endsWith("/")) url.remove(url.length() - 1);
  if (!url.endsWith("/ask")) url += "/ask";
  JsonDocument payload;
  payload["question"] = question;
  payload["question_id"] = questionId;
  payload["language"] = "en";
  if (questionId == 5) payload["days"] = days;
  String body;
  serializeJson(payload, body);
  if (WiFi.status() != WL_CONNECTED && !ensureWifi()) {
    sttLastError = "Network error";
    return "";
  }
  WiFiClientSecure client;
  client.setInsecure();
  HTTPClient http;
  http.setTimeout(45000);
  http.setReuse(false);
  if (!http.begin(client, url)) {
    sttLastError = "Network error";
    return "";
  }
  http.addHeader("Content-Type", "application/json");
  http.addHeader("Authorization", String("Bearer ") + cfg.cfToken);
  int code = http.POST(body);
  String response = code > 0 ? http.getString() : "";
  http.end();
  if (code < 200 || code >= 300) {
    Serial.printf("Cloudflare ask HTTP %d: %s\n", code, response.c_str());
    sttLastError = code == 401 ? "Worker auth error" : "Cloudflare answer error";
    return "";
  }
  JsonDocument result;
  if (deserializeJson(result, response)) {
    sttLastError = "Invalid Worker response";
    return "";
  }
  String answer = result["answer"] | result["text"] | "";
  answer.trim();
  if (!answer.length()) sttLastError = "Empty answer";
  return answer;
}

void openQuestionMenu() { selectedQuestion = 0; state = ST_Q_MENU; drawQuestionMenu(); }
void selectQuestion() { if (selectedQuestion == 4) { customDaysInput = ""; state = ST_Q_CUSTOM_DAYS; drawCustomDays(); } else { state = ST_Q_CONFIRM; drawQuestionConfirm(); } }
void confirmCustomDays() { int d = customDaysInput.length() ? customDaysInput.toInt() : 30; customDays = constrain(d, 1, 3650); state = ST_Q_CONFIRM; drawQuestionConfirm(); }
void cancelQuestion() { selectedQuestion = -1; customDaysInput = ""; pendingQuestion = ""; state = ST_IDLE; drawHome(); }
void handleSelectedQuestion() {
  if (assistantBusy || selectedQuestion < 0) return;
  assistantBusy = true; state = ST_Q_ASKING;
  String q = String(questionText[selectedQuestion]);
  if (selectedQuestion == 4) q = "How much did I receive in the last " + String(customDays) + " days?";
  oledText("Checking payments", "Please wait...");
  String ans = cloudflareAsk(q, selectedQuestion + 1, selectedQuestion == 4 ? customDays : 0);
  if (!ans.length()) { oledText("Answer failed", "Press # to cancel"); fallbackChime(); assistantBusy = false; state = ST_Q_RESULT; return; }
  Serial.println("A: " + ans); state = ST_Q_RESULT;
  u8g2.clearBuffer(); u8g2.setFont(u8g2_font_6x12_tf); u8g2.drawStr(0, 12, "A:"); for (int i = 0; i < 4; i++) u8g2.drawStr(0, 26 + i * 12, ans.substring(i * 21, (i + 1) * 21).c_str()); u8g2.sendBuffer();
  if (!cloudflareSpeak(ans, "en")) { fallbackChime(); }
  // Answer remains visible throughout speech; brief pause after playback.
  delay(1200);
  assistantBusy = false; state = ST_IDLE; drawHome();
}

// ======================= Payment flow ========================
String paiseToSpeech(long paise) {
  long r = paise / 100, p = paise % 100;
  String s = String(r) + " rupees";
  if (p) s += " " + String(p) + " paise";
  return s;
}

bool cloudflareLogPayment(const String& paymentId, const char* qrType) {
  if (!cfg.cfWorkerUrl.length() || !cfg.cfToken.length()) {
    Serial.println("Cloudflare payment log skipped: Worker URL/token not configured");
    return false;
  }
  String url = cfg.cfWorkerUrl;
  url.trim();
  while (url.endsWith("/")) url.remove(url.length() - 1);
  if (!url.endsWith("/payment")) url += "/payment";

  JsonDocument payload;
  payload["payment_id"] = paymentId;
  payload["qr_type"] = qrType;
  payload["store"] = cfg.store;
  String body;
  serializeJson(payload, body);

  if (WiFi.status() != WL_CONNECTED && !ensureWifi()) {
    Serial.println("Cloudflare payment log failed: Wi-Fi disconnected");
    return false;
  }
  WiFiClientSecure client;
  client.setInsecure();
  HTTPClient http;
  http.setTimeout(20000);
  http.setReuse(false);
  if (!http.begin(client, url)) {
    Serial.println("Cloudflare payment log failed: HTTPS session could not start");
    return false;
  }
  http.addHeader("Content-Type", "application/json");
  http.addHeader("Authorization", String("Bearer ") + cfg.cfToken);
  int code = http.POST(body);
  String response = code > 0 ? http.getString() : "";
  http.end();
  Serial.printf("Cloudflare payment log: HTTP %d %s\n", code, response.c_str());
  return code >= 200 && code < 300;
}

void onPaymentReceived(long paise, const String& pid) {
  char amt[24]; snprintf(amt, sizeof(amt), "Rs %.2f", paise / 100.0);
  Serial.printf("PAID %s %s\n", amt, pid.c_str());
  successAnimation(amt);
  String announcement = "Payment received. " + paiseToSpeech(paise);
  if (!cloudflareSpeak(announcement, "en")) {
    Serial.println("Payment voice failed; playing fallback chime");
    fallbackChime();
  }
  // Do payer lookups and cloud logging after starting the spoken confirmation.
  String payerName, payerId;
  getPayerDetails(pid, payerName, payerId);
  savePaymentCsv(paise, pid, payerName, payerId);
  cloudflareLogPayment(pid, activeIsFixed ? "fixed" : "dynamic");
}

void startDynamicQr() {
  float amt = amountStr.toFloat();
  long paise = lroundf(amt * 100.0f);
  if (paise < 100) { oledText("Minimum Rs 1", "Enter amount again"); delay(1200); drawAmount(); return; }
  if (!ensureWifi()) { oledText("No WiFi"); delay(1200); drawAmount(); return; }
  oledText("Creating payment link...");
  String id, data;
  if (!createPaymentLink(false, paise, id, data)) { oledText("Link failed", "see Serial log"); delay(1800); drawAmount(); return; }
  activeQrId = id; activeQrData = data; activeIsFixed = false;
  activeAmountPaise = paise; lastSeenPaymentId = "";
  activeLinkStarted = millis(); lastQrRefresh = 0;
  state = ST_QR_WAIT; lastPoll = millis();
  showQrScreen();
}

void startFixedQr() {
  long paise = lroundf(cfg.fixedAmount.toFloat() * 100.0f);
  if (paise < 100) { oledText("Set B amount", "Min Rs 1 in portal"); delay(1500); drawHome(); return; }
  if (!ensureWifi()) { oledText("No WiFi"); delay(1200); drawHome(); return; }
  oledText("Creating fixed link...");
  String id, data;
  if (!createPaymentLink(true, paise, id, data)) {
    oledText("Link failed", "see Serial log"); delay(1800); drawHome(); return;
  }
  activeQrId = id; activeQrData = data; activeIsFixed = true;
  activeAmountPaise = paise; lastSeenPaymentId = "";
  activeLinkStarted = millis(); lastQrRefresh = 0;
  state = ST_QR_WAIT; lastPoll = millis();
  showQrScreen();
}

void pollPayment() {
  if (millis() - lastQrRefresh >= 1000) {
    lastQrRefresh = millis();
    showQrScreen();
  }
  // Check at the polling interval, then cancel locally at 60 seconds.
  if (millis() - lastPoll >= 3000) {
    lastPoll = millis();
    if (ensureWifi()) {
      String pid; long paise;
      if (latestPayment(activeQrId, pid, paise) && pid != lastSeenPaymentId) {
        lastSeenPaymentId = pid;
        onPaymentReceived(paise, pid);
        state = ST_IDLE; amountStr = ""; activeQrId = ""; activeQrData = "";
        drawHome();
        return;
      }
    }
  }
  if (millis() - activeLinkStarted >= PAYMENT_LINK_TTL_MS) {
    String expiredId = activeQrId;
    closeQr(expiredId);
    oledText("QR expired", "60 seconds ended");
    state = ST_IDLE; amountStr = ""; activeQrId = ""; activeQrData = "";
    delay(1200);
    drawHome();
  }
}

void cancelToIdle() {
  if (state == ST_QR_WAIT) closeQr(activeQrId);   // cancel the unused Payment Link
  state = ST_IDLE; amountStr = ""; activeQrId = "";
  drawHome();
}

// ======================= Settings storage ====================
void loadCfg() {
  Preferences p; p.begin("cfg", true);
  cfg.ssid      = p.getString("ssid", "");
  cfg.pass      = p.getString("pass", "");
  cfg.rzpId     = p.getString("rid", "");
  cfg.rzpSecret = p.getString("rsec", "");
  cfg.store     = p.getString("store", "My Soundbox Store");
  cfg.fixedAmount = p.getString("famt", "100.00");
  cfg.cfWorkerUrl = p.getString("cfurl", "");
  cfg.cfToken   = p.getString("cftok", "");
  cfg.vol       = p.getInt("vol", 80);
  p.end();

}

void saveCfg(const Cfg& c) {
  Preferences p; p.begin("cfg", false);
  p.putString("ssid", c.ssid);   p.putString("pass", c.pass);
  p.putString("rid", c.rzpId);   p.putString("rsec", c.rzpSecret); p.putString("store", c.store);
  p.putString("famt", c.fixedAmount);
  p.putString("cfurl", c.cfWorkerUrl); p.putString("cftok", c.cfToken);
  p.putInt("vol", c.vol);
  p.end();
}

bool cfgComplete(const Cfg& c) {
  return c.ssid.length() && c.rzpId.length() && c.rzpSecret.length();
}

// ======================= Setup portal ========================
WebServer web(80);
DNSServer dnsServer;
String netOptions;

void getPayerDetails(const String& paymentId, String& payerName, String& payerId) {
  payerName = "";
  payerId = "";
  int code = 0;
  String r = rzp("GET", "/v1/payments/" + paymentId, "", &code);
  if (code != 200) { payerName = "not returned"; payerId = paymentId; return; }
  JsonDocument payment;
  if (deserializeJson(payment, r)) { payerName = "not returned"; payerId = paymentId; return; }
  String email = payment["email"] | "";
  String contact = payment["contact"] | "";
  String customerId = payment["customer_id"] | "";
  payerId = customerId.length() ? customerId : paymentId;
  if (customerId.length()) {
    int customerCode = 0;
    String c = rzp("GET", "/v1/customers/" + customerId, "", &customerCode);
    if (customerCode == 200) {
      JsonDocument customer;
      if (!deserializeJson(customer, c)) payerName = customer["name"] | "";
    }
  }
  if (!payerName.length()) payerName = email.length() ? email : contact;
  if (!payerName.length()) payerName = "not provided";
}

static String csvQuote(const String& in) {
  String out = "\"";
  for (size_t i = 0; i < in.length(); ++i) {
    if (in[i] == '\"') out += "\"\"";
    else out += in[i];
  }
  out += "\"";
  return out;
}

void initCsvLog() {
  csvReady = LittleFS.begin(true);
  if (!csvReady) { Serial.println("CSV storage unavailable"); return; }
  if (!LittleFS.exists("/payments.csv")) {
    File f = LittleFS.open("/payments.csv", FILE_WRITE);
    if (f) { f.println("timestamp_ist,amount_inr,payment_id,qr_type,payer_name_or_contact,razorpay_customer_or_payment_id"); f.close(); }
  }
}

void savePaymentCsv(long paise, const String& pid, const String& payerName, const String& payerId) {
  if (!csvReady) return;
  struct tm ti;
  char ts[24] = "";
  if (getLocalTime(&ti, 100)) strftime(ts, sizeof(ts), "%Y-%m-%d %H:%M:%S", &ti);
  File f = LittleFS.open("/payments.csv", FILE_APPEND);
  if (!f) { Serial.println("CSV append failed"); return; }
  f.printf("%s,%.2f,%s,%s,%s,%s\n", ts, paise / 100.0, csvQuote(pid).c_str(),
           activeIsFixed ? "fixed" : "dynamic", csvQuote(payerName).c_str(), csvQuote(payerId).c_str());
  f.close();
}

void handleCsvDownload() {
  if (!csvReady || !LittleFS.exists("/payments.csv")) {
    web.send(404, "text/plain", "No CSV payment log found yet"); return;
  }
  File f = LittleFS.open("/payments.csv", FILE_READ);
  web.sendHeader("Content-Disposition", "attachment; filename=payments.csv");
  web.streamFile(f, "text/csv");
  f.close();
}

void handleDeviceRoot() {
  web.send(200, "text/html", "<!doctype html><meta name='viewport' content='width=device-width'><h2>Payment Soundbox</h2><p><a href='/export.csv'>Download payments.csv for Excel</a></p>");
}

String esc(const String& s) {
  String o; o.reserve(s.length() + 8);
  for (size_t i = 0; i < s.length(); i++) {
    char c = s[i];
    if (c == '&') o += "&amp;";
    else if (c == '<') o += "&lt;";
    else if (c == '>') o += "&gt;";
    else if (c == '"') o += "&quot;";
    else if (c == '\'') o += "&#39;";
    else o += c;
  }
  return o;
}

String field(const char* label, const char* name, const String& val, const char* hint = "") {
  String s = "<label>"; s += label; s += "</label><input name='"; s += name; s += "' value='"; s += esc(val);
  s += "' autocomplete='off' autocapitalize='off' spellcheck='false'>";
  if (hint[0]) { s += "<small>"; s += hint; s += "</small>"; }
  return s;
}
// secrets are never sent back to the browser; blank = keep the saved value
String secretField(const char* label, const char* name, bool saved) {
  String s = "<label>"; s += label; s += "</label><input type='password' name='"; s += name;
  s += "' placeholder='"; s += saved ? "saved - leave blank to keep" : "required"; s += "' autocomplete='new-password'>";
  return s;
}

void handleRoot() {
  String p = F("<!DOCTYPE html><html><head><meta charset='utf-8'><meta name='viewport' content='width=device-width,initial-scale=1'>"
    "<title>Soundbox Setup</title><style>body{font-family:sans-serif;margin:0;background:#f2f4f8}.w{max-width:480px;margin:auto;padding:16px}"
    "h1{font-size:20px}h2{font-size:15px;margin:24px 0 2px;color:#0b5ed7;border-bottom:1px solid #cfd8ea}label{display:block;margin-top:10px;font-size:13px;color:#333}"
    "input{width:100%;box-sizing:border-box;padding:10px;font-size:15px;border:1px solid #bbb;border-radius:6px}small{color:#666;font-size:11px}"
    "button{margin-top:22px;width:100%;padding:13px;font-size:16px;background:#0b5ed7;color:#fff;border:0;border-radius:8px}</style></head>"
    "<body><div class='w'><h1>AI Soundbox Setup</h1><p style='font-size:13px;color:#555'>Fill the required fields and press Save. "
    "Blank password / key fields keep the value already saved on the device.</p><form method='POST' action='/save'>");
  p += "<h2>Wi-Fi</h2>";
  p += "<label>Network name (SSID)</label><input name='ssid' list='nets' value='" + esc(cfg.ssid) + "' autocomplete='off' autocapitalize='off'><datalist id='nets'>" + netOptions + "</datalist>";
  p += secretField("Wi-Fi password (leave empty for an open network)", "pass", cfg.pass.length() > 0);
  p += "<h2>Razorpay Payment Links</h2>";
  p += field("B key fixed link amount (INR)", "famt", cfg.fixedAmount, "B creates a fresh single-use link at this amount");
  p += field("Key ID", "rid", cfg.rzpId, "starts with rzp_test_ or rzp_live_");
  p += secretField("Key secret", "rsec", cfg.rzpSecret.length() > 0);
  p += field("Store name (shown on QR)", "store", cfg.store);
  p += "<h2>Cloudflare speech-to-text</h2>";
  p += field("Cloudflare Worker URL", "cfurl", cfg.cfWorkerUrl, "HTTPS base URL; routes are added automatically");
  p += secretField("Soundbox access token", "cftok", cfg.cfToken.length() > 0);
  p += "<p><small>Dashboard ADMIN_TOKEN is entered on the Cloudflare dashboard login only. Do not enter it here; the device uses only the Soundbox access token.</small></p>";
  p += "<h2>Cloudflare AI (speech, answers, voice)</h2>";
  p += "<p><small>The Worker handles transcription, payment-data answers, and speech generation. ADMIN_TOKEN is only entered on the private dashboard, never here.</small></p>";
  p += "<h2>Speaker</h2>";
  p += field("Volume (0 - 100)", "vol", String(cfg.vol));
  p += "<button type='submit'>Save and restart</button></form>";
  p += "<form method='POST' action='/reset' onsubmit=\"return confirm('Erase ALL saved settings?')\"><button style='background:#b00020'>Erase all saved settings</button></form>";
  p += "</div></body></html>";
  web.send(200, "text/html", p);
}

String simplePage(const String& title, const String& msg) {
  return "<!DOCTYPE html><html><head><meta charset='utf-8'><meta name='viewport' content='width=device-width,initial-scale=1'></head>"
         "<body style='font-family:sans-serif;padding:24px'><h2>" + title + "</h2><p>" + msg + "</p></body></html>";
}

String argTrim(const char* n) { String v = web.arg(n); v.trim(); return v; }

void handleSave() {
  Cfg n = cfg;
  String v;
  v = argTrim("ssid");  if (v.length()) { if (v != n.ssid) n.pass = ""; n.ssid = v; }
  v = web.arg("pass");  if (v.length()) n.pass = v;
  v = argTrim("rid");   if (v.length()) n.rzpId = v;
  v = argTrim("rsec");  if (v.length()) n.rzpSecret = v;
  v = argTrim("store"); if (v.length()) n.store = v;
  v = argTrim("famt"); if (v.length()) n.fixedAmount = v;
  v = argTrim("cfurl"); if (v.length()) n.cfWorkerUrl = v;
  v = argTrim("cftok"); if (v.length()) n.cfToken = v;
  v = argTrim("vol");   if (v.length()) n.vol = constrain(v.toInt(), 0, 100);

  if (!cfgComplete(n)) {
    web.send(400, "text/html", simplePage("Missing fields",
      "Wi-Fi name and Razorpay keys are required. Set the Cloudflare Worker URL and soundbox token to enable speech, answers, and cloud payment logging. "
      "<a href='/'>Go back</a>"));
    return;
  }
  if (n.rzpId != cfg.rzpId) {                 // new Razorpay account -> forget old fixed QR
    Preferences sb; sb.begin("soundbox", false); sb.remove("fqid"); sb.remove("fqdata"); sb.end();
  }
  saveCfg(n);
  web.send(200, "text/html", simplePage("Saved", "Settings stored. The soundbox is restarting and will connect to your Wi-Fi. You can close this page."));
  delay(1500);
  ESP.restart();
}

void handleReset() {
  Preferences p; p.begin("cfg", false); p.clear(); p.end();
  Preferences sb; sb.begin("soundbox", false); sb.clear(); sb.end();
  web.send(200, "text/html", simplePage("Erased", "All settings were erased. The soundbox is restarting into setup mode."));
  delay(1500);
  ESP.restart();
}

void handleRedirect() { handleRoot(); }

void runPortal() {
  oledText("Scanning Wi-Fi...");
  WiFi.disconnect(true);
  WiFi.mode(WIFI_STA);
  delay(100);
  netOptions = "";
  int n = WiFi.scanNetworks();                 // scan BEFORE the hotspot starts (scanning drops AP clients)
  for (int i = 0; i < n && i < 20; i++) {
    String s = WiFi.SSID(i);
    if (s.length()) netOptions += "<option value='" + esc(s) + "'>";
  }
  WiFi.scanDelete();

  WiFi.mode(WIFI_AP);
  WiFi.setSleep(false);
  Serial.printf("Heap before setup AP: %u bytes\n", (unsigned)ESP.getFreeHeap());
  IPAddress apIP(192,168,4,1), gateway(192,168,4,1), subnet(255,255,255,0);
  WiFi.softAPConfig(apIP, gateway, subnet);
  bool apStarted = WiFi.softAP(AP_NAME, AP_PASS);
  delay(500);
  IPAddress ip = WiFi.softAPIP();
  Serial.printf("Setup AP start=%s, SSID=%s, IP=%s, heap=%u bytes\n", apStarted ? "OK" : "FAILED", AP_NAME, ip.toString().c_str(), (unsigned)ESP.getFreeHeap());
  dnsServer.start(53, "*", ip);                // captive portal: every name -> this device
  web.on("/", HTTP_GET, handleRoot);
  web.on("/", HTTP_POST, handleRoot);
  web.on("/export.csv", HTTP_GET, handleCsvDownload);
  web.on("/save", HTTP_POST, handleSave);
  web.on("/reset", HTTP_POST, handleReset);
  web.onNotFound(handleRedirect);
  web.begin();
  Serial.println("Setup web server listening on port 80; open http://192.168.4.1/");

  u8g2.clearBuffer();
  u8g2.setFont(u8g2_font_6x12_tf);
  u8g2.drawStr(0, 12, "SETUP MODE");
  u8g2.drawStr(0, 28, "Wi-Fi: " AP_NAME);
  u8g2.drawStr(0, 42, "Pass : " AP_PASS);
  u8g2.drawStr(0, 56, ("Open " + ip.toString()).c_str());
  u8g2.sendBuffer();
  Serial.println("Setup portal at http://" + ip.toString());

  while (true) {
    dnsServer.processNextRequest();
    web.handleClient();
    // Keep portal running until the user explicitly saves or erases settings.
    // Do not treat the C key used to enter setup as an immediate restart command.
    delay(2);
  }
}

// ======================= Setup / Loop ========================
void setup() {
  Serial.begin(115200);
  delay(300);
  Wire.begin(OLED_SDA, OLED_SCL);
  u8g2.begin();
  oledText("Payment Box", "Starting...", "Hold C = setup");

  kpd.addEventListener(keypadEvent);
  kpd.setDebounceTime(50);
  kpd.setHoldTime(2000);

  i2sInit();


  loadCfg();
  initCsvLog();
  web.on("/export.csv", HTTP_GET, handleCsvDownload);

  // hold C while powering on -> setup portal
  bool forcePortal = false;
  for (unsigned long t = millis(); millis() - t < 1500;) {
    kpd.getKeys();
    if (kpd.isPressed('C')) { forcePortal = true; break; }
    delay(10);
  }
  if (forcePortal || !cfgComplete(cfg)) runPortal();

  oledText("Connecting WiFi...");
  WiFi.mode(WIFI_STA);
  if (!ensureWifi()) {
    oledText("WiFi FAILED", "Opening setup...");
    delay(1500);
    runPortal();
  }
  configTime(19800, 0, "pool.ntp.org", "time.google.com");   // IST
  struct tm t; getLocalTime(&t, 8000);

  web.on("/", HTTP_GET, handleDeviceRoot);
  web.onNotFound(handleDeviceRoot);
  web.begin();
  Serial.println("Payment CSV download: http://" + WiFi.localIP().toString() + "/export.csv");

  prefs.begin("soundbox", false);
  fixedQrId = prefs.getString("fqid", "");
  fixedQrData = prefs.getString("fqdata", "");

  playTone(1000, 120);
  drawHome();
}

void loop() {
  web.handleClient();
  char k = kpd.getKey();
  if (portalRequested) { portalRequested = false; if (state == ST_IDLE) runPortal(); }
  if (assistantBusy) return;
  if (state == ST_IDLE) {
    if (k == 'A') { openQuestionMenu(); return; }
    if (k == 'D') { state = ST_ENTER_AMOUNT; amountStr = ""; drawAmount(); return; }
    if (k == 'B') { startFixedQr(); return; }
  } else if (state == ST_Q_MENU) {
    if (k == 'C') { selectedQuestion = (selectedQuestion + QUESTION_COUNT - 1) % QUESTION_COUNT; drawQuestionMenu(); }
    else if (k == 'D') { selectedQuestion = (selectedQuestion + 1) % QUESTION_COUNT; drawQuestionMenu(); }
    else if (k == 'A') selectQuestion();
    else if (k == '#') cancelQuestion();
    return;
  } else if (state == ST_Q_CUSTOM_DAYS) {
    if (k >= '0' && k <= '9' && customDaysInput.length() < 4) { customDaysInput += k; drawCustomDays(); }
    else if (k == '#') { if (customDaysInput.length()) { customDaysInput.remove(customDaysInput.length()-1); drawCustomDays(); } else cancelQuestion(); }
    else if (k == 'C') cancelQuestion();
    else if (k == 'A') confirmCustomDays();
    return;
  } else if (state == ST_Q_CONFIRM) {
    if (k == 'A') { handleSelectedQuestion(); return; }
    if (k == '#') { cancelQuestion(); return; }
    return;
  } else if (state == ST_Q_RESULT) {
    if (k == '#') { cancelQuestion(); return; }
    return;
  }
  if (state == ST_ENTER_AMOUNT) {
    if (k >= '0' && k <= '9') { if (amountStr.length() < 8) { int dot = amountStr.indexOf('.'); if (dot < 0 || (int)amountStr.length() - dot <= 2) if (!(amountStr == "0" && k == '0')) amountStr += k; } drawAmount(); }
    else if (k == '*') { if (amountStr.indexOf('.') < 0) amountStr += (amountStr.length() ? "." : "0."); drawAmount(); }
    else if (k == '#') { if (amountStr.length()) { amountStr.remove(amountStr.length()-1); drawAmount(); } else cancelToIdle(); }
    else if (k == 'C') { if (amountStr.length()) { amountStr = ""; drawAmount(); } else cancelToIdle(); }
    else if (k == 'D') startDynamicQr();
    return;
  }
  if (state == ST_QR_WAIT && k == 'C') cancelToIdle();
  if (state == ST_QR_WAIT) pollPayment();
}
