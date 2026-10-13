/*
 * Interactive Plan Scheduler Robot - complete firmware
 *
 *  - Syncs clock + alarms from the backend, rings alarms itself (works offline)
 *  - Button stops the alarm -> "Good morning <name>, do you want to know today's plans?"
 *    -> you answer by voice, and can keep talking ("move the 1 o'clock meeting to 3")
 *  - Button press while idle -> starts a conversation (push to talk to begin)
 *  - Conversation mode: after each reply the robot keeps listening until SESSION_WAIT_MS
 *    of silence, or you say "thanks / that's all", or press the button
 *  - LDR: room dark for a few seconds -> says good night, sleeping face; light -> wakes
 *
 * Needs: ESP32 Arduino core 3.x (Boards Manager: "esp32 by Espressif Systems")
 * Libraries: Adafruit SSD1306, Adafruit GFX Library, ArduinoJson (7.x)
 */
#include <WiFi.h>
#include <HTTPClient.h>
#include <Wire.h>
#include <Adafruit_GFX.h>
#include <Adafruit_SSD1306.h>
#include <ArduinoJson.h>
#include <ESP_I2S.h>
#include <freertos/FreeRTOS.h>
#include <freertos/task.h>
#include <freertos/stream_buffer.h>
#include <time.h>
#include "secrets.h"

const char* WIFI_SSID = SECRET_WIFI_SSID;
const char* WIFI_PASS = SECRET_WIFI_PASS;
const char* SERVER = "http://192.168.147.172:8000";

// LDR: values from the Serial Monitor. DARK_BELOW must be lower than BRIGHT_ABOVE.
const int DARK_BELOW   = 800;
const int BRIGHT_ABOVE = 1200;

// Microphone tuning (watch "mic level=" in the Serial Monitor)
const int MIC_SLOT         = 0;     // 0 = left slot (L/R pin to GND). If the level stays ~0, try 1
const int MIC_SHIFT        = 14;    // bigger = quieter. 16 = no gain, 14 = 4x gain
const int MIN_SPEECH_LEVEL = 500;   // speech must be louder than this (and 4x background noise)
const bool DEBUG_MIC       = true;  // print mic level while listening

const uint32_t DARK_DELAY_MS    = 5000;    // dark this long -> good night
const uint32_t BRIGHT_DELAY_MS  = 2000;    // bright this long -> wake up
const uint32_t SYNC_EVERY_MS    = 60000;   // ask the server for time + alarms
const uint32_t SESSION_WAIT_MS  = 20000;   // how long the robot waits for you to speak
const uint32_t END_SILENCE_MS   = 1200;    // pause that counts as "finished my sentence"
const uint32_t MAX_RECORD_MS    = 12000;   // longest single sentence
const uint32_t ALARM_MAX_MS     = 120000;  // alarm gives up after 2 minutes
const int      VOLUME           = 6000;    // alarm beep volume, 0..32767
const int      SPEECH_GAIN_PCT  = 60;      // voice volume in percent (raise if too quiet)
// ======================================================

// Pins (match the wiring table)
#define PIN_SDA      21
#define PIN_SCL      22
#define PIN_SPK_BCLK 27
#define PIN_SPK_LRC  26
#define PIN_SPK_DIN  23
#define PIN_MIC_SCK  32
#define PIN_MIC_WS   25
#define PIN_MIC_SD   33
#define PIN_LDR      34
#define PIN_BTN       4

#define SAMPLE_RATE  16000
#define BLOCK        320    // 20 ms of audio at 16 kHz
#define PREROLL      12     // keep 240 ms before speech is detected so the first word isn't cut

Adafruit_SSD1306 display(128, 64, &Wire, -1);
I2SClass i2sOut, i2sIn;

enum State { IDLE, SLEEPING, RINGING };
State state = IDLE;
enum Face { F_LISTEN, F_THINK, F_SPEAK };

struct Alarm { int id; int hour; int minute; bool weekdaysOnly; };
Alarm alarms[10];
int alarmCount = 0;

bool timeValid = false;
time_t baseTime = 0;
uint32_t baseMillis = 0;
long lastRungKey = -1;
int ringingId = -1;
int pendingFiredId = -1;
uint32_t lastSync = 0;
bool lastSyncOk = false;
uint32_t stayAwakeUntil = 0;
int lastDrawKey = -1;

float ldrAvg = 0;
uint32_t darkSince = 0, brightSince = 0;

String srvHost;
uint16_t srvPort = 80;

StreamBufferHandle_t micBuf;
volatile bool micRunning = false;

// ---------------------------------------------------------------- time
bool getLocal(struct tm &t) {
  if (!timeValid) return false;
  time_t n = baseTime + (millis() - baseMillis) / 1000;
  gmtime_r(&n, &t);
  return true;
}

// -------------------------------------------------------------- button
bool buttonDown() { return digitalRead(PIN_BTN) == LOW; }

bool buttonPressedEdge() {
  static bool prev = false;
  static uint32_t lastEdge = 0;
  bool cur = buttonDown(), edge = false;
  if (cur && !prev && millis() - lastEdge > 50) { edge = true; lastEdge = millis(); }
  prev = cur;
  return edge;
}

void waitRelease() {
  while (buttonDown()) delay(10);
  delay(80);
  buttonPressedEdge();
}

// ------------------------------------------------------------- speaker
void spkWrite(int16_t *buf, size_t bytes) { i2sOut.write((uint8_t *)buf, bytes); }

void spkSilence() {
  static int16_t zeros[512] = {0};
  for (int i = 0; i < 4; i++) spkWrite(zeros, sizeof zeros);
}

// Plays a tone (freq 0 = silence). Returns false if the button was pressed.
bool playTone(int freq, int ms) {
  static int16_t buf[512];
  int total = SAMPLE_RATE / 1000 * ms, done = 0;
  float phase = 0, inc = 2.0f * PI * freq / SAMPLE_RATE;
  while (done < total) {
    if (buttonDown()) return false;
    int n = min(256, total - done);
    for (int i = 0; i < n; i++) {
      int16_t v = freq ? (int16_t)(sinf(phase) * VOLUME) : 0;
      phase += inc;
      if (phase > 2.0f * PI) phase -= 2.0f * PI;
      buf[2 * i] = v;
      buf[2 * i + 1] = v;
    }
    spkWrite(buf, n * 4);
    done += n;
  }
  return true;
}

// ---------------------------------------------------------- microphone
// A background task reads the I2S mic into a buffer, so no audio is lost while the
// main code is busy opening the network connection.
void micTask(void *) {
  static int32_t raw[512];  // 256 stereo frames of 32 bits
  static int16_t pcm[256];
  for (;;) {
    if (!micRunning) { vTaskDelay(pdMS_TO_TICKS(20)); continue; }
    size_t n = i2sIn.readBytes((char *)raw, sizeof raw);
    int frames = n / 8;
    for (int i = 0; i < frames; i++) {
      int32_t v = raw[2 * i + MIC_SLOT] >> MIC_SHIFT;
      pcm[i] = (int16_t)constrain(v, -32768, 32767);
    }
    if (frames) xStreamBufferSend(micBuf, pcm, frames * 2, 0);
  }
}

bool readBlock(int16_t *dst, uint32_t timeoutMs) {
  size_t need = BLOCK * 2, got = 0;
  uint32_t deadline = millis() + timeoutMs;
  while (got < need && millis() < deadline)
    got += xStreamBufferReceive(micBuf, (uint8_t *)dst + got, need - got, pdMS_TO_TICKS(20));
  return got == need;
}

int blockLevel(const int16_t *b) {
  long s = 0;
  for (int i = 0; i < BLOCK; i++) s += abs(b[i]);
  return s / BLOCK;
}

void initAudio() {
  i2sOut.setPins(PIN_SPK_BCLK, PIN_SPK_LRC, PIN_SPK_DIN);
  if (!i2sOut.begin(I2S_MODE_STD, SAMPLE_RATE, I2S_DATA_BIT_WIDTH_16BIT, I2S_SLOT_MODE_STEREO))
    Serial.println("Speaker I2S init failed");

  i2sIn.setPins(PIN_MIC_SCK, PIN_MIC_WS, -1, PIN_MIC_SD);
  if (!i2sIn.begin(I2S_MODE_STD, SAMPLE_RATE, I2S_DATA_BIT_WIDTH_32BIT, I2S_SLOT_MODE_STEREO))
    Serial.println("Microphone I2S init failed");
  i2sIn.setTimeout(200);

  micBuf = xStreamBufferCreate(40000, 1);  // about 1.2 s of audio
  xTaskCreatePinnedToCore(micTask, "mic", 4096, NULL, 2, NULL, 1);
}

// ---------------------------------------------------------------- OLED
int wrapText(const String &text, String *lines, int maxLines, int width = 21) {
  int n = 0, i = 0, len = text.length();
  String cur = "";
  while (i < len && n < maxLines) {
    int j = i;
    while (j < len && text[j] != ' ') j++;
    String w = text.substring(i, j);
    i = j + 1;
    while ((int)w.length() > width) {
      if (cur.length()) { lines[n++] = cur; cur = ""; if (n >= maxLines) return n; }
      lines[n++] = w.substring(0, width);
      w = w.substring(width);
      if (n >= maxLines) return n;
    }
    if (cur.length() == 0) cur = w;
    else if ((int)(cur.length() + 1 + w.length()) <= width) cur += " " + w;
    else { lines[n++] = cur; cur = w; }
  }
  if (cur.length() && n < maxLines) lines[n++] = cur;
  return n;
}

void showMessage(const String &text, uint32_t holdMs = 0) {
  static String lines[8];
  int n = wrapText(text, lines, 8);
  display.clearDisplay();
  display.setTextSize(1);
  display.setTextColor(WHITE);
  for (int k = 0; k < n; k++) { display.setCursor(0, k * 8); display.print(lines[k]); }
  display.display();
  if (holdMs) delay(holdMs);
}

// Long text on the OLED, 7 lines per page; button skips to the next page.
void showPaged(const String &text) {
  static String lines[48];
  const int PER = 7;
  int n = wrapText(text, lines, 48);
  for (int p = 0; p < n; p += PER) {
    display.clearDisplay();
    display.setTextSize(1);
    display.setTextColor(WHITE);
    for (int k = 0; k < PER && p + k < n; k++) {
      display.setCursor(0, k * 9);
      display.print(lines[p + k]);
    }
    display.display();
    uint32_t start = millis();
    while (millis() - start < 4500) {
      if (buttonPressedEdge()) break;
      delay(20);
    }
  }
}

void drawFace(bool blink) {
  display.clearDisplay();
  int h = blink ? 3 : 26, y = blink ? 22 : 10;
  display.fillRoundRect(28, y, 22, h, blink ? 1 : 6, WHITE);
  display.fillRoundRect(78, y, 22, h, blink ? 1 : 6, WHITE);
  struct tm t;
  display.setTextSize(1);
  display.setTextColor(WHITE);
  if (getLocal(t)) {
    char b[8];
    snprintf(b, sizeof b, "%02d:%02d", t.tm_hour, t.tm_min);
    display.setCursor(49, 52);
    display.print(b);
  }
  if (!lastSyncOk) { display.setCursor(0, 56); display.print("!"); }
  display.display();
}

// Listening / thinking / speaking faces. mouth = 0..14 (how open while speaking)
void drawModeFace(Face f, int mouth = 0) {
  display.clearDisplay();
  display.setTextSize(1);
  display.setTextColor(WHITE);
  if (f == F_LISTEN) {
    display.fillRoundRect(28, 2, 22, 36, 8, WHITE);
    display.fillRoundRect(78, 2, 22, 36, 8, WHITE);
    display.setCursor(41, 52);
    display.print("listening");
  } else if (f == F_THINK) {
    display.fillRoundRect(22, 6, 22, 22, 6, WHITE);
    display.fillRoundRect(72, 6, 22, 22, 6, WHITE);
    for (int i = 0; i < 3; i++) display.fillCircle(50 + i * 14, 52, 3, WHITE);
  } else {
    display.fillRoundRect(28, 6, 22, 22, 6, WHITE);
    display.fillRoundRect(78, 6, 22, 22, 6, WHITE);
    int h = 4 + mouth;
    display.fillRoundRect(48, 50 - h / 2, 32, h, 3, WHITE);
  }
  display.display();
}

void drawSleep() {
  display.clearDisplay();
  display.fillRect(28, 30, 22, 3, WHITE);
  display.fillRect(78, 30, 22, 3, WHITE);
  display.setTextSize(1);
  display.setTextColor(WHITE);
  display.setCursor(98, 12); display.print("z");
  display.setCursor(108, 4); display.print("Z");
  display.display();
}

void drawAlarmScreen(bool inverted) {
  display.clearDisplay();
  display.setTextColor(inverted ? BLACK : WHITE, inverted ? WHITE : BLACK);
  if (inverted) display.fillRect(0, 0, 128, 64, WHITE);
  display.setTextSize(2);
  display.setCursor(30, 6);
  display.print("ALARM");
  struct tm t;
  if (getLocal(t)) {
    char b[8];
    snprintf(b, sizeof b, "%02d:%02d", t.tm_hour, t.tm_min);
    display.setTextSize(3);
    display.setCursor(22, 34);
    display.print(b);
  }
  display.display();
  display.setTextColor(WHITE);
}

// ---------------------------------------------------------------- HTTP
void parseServer() {
  String s = SERVER;
  s.replace("http://", "");
  int slash = s.indexOf('/');
  if (slash >= 0) s = s.substring(0, slash);
  int colon = s.indexOf(':');
  if (colon >= 0) { srvHost = s.substring(0, colon); srvPort = s.substring(colon + 1).toInt(); }
  else { srvHost = s; srvPort = 80; }
}

String httpRequest(const String &path, bool post) {
  if (WiFi.status() != WL_CONNECTED) {
    Serial.println("HTTP skipped: WiFi not connected");
    return "";
  }
  HTTPClient http;
  http.setConnectTimeout(3000);
  http.setTimeout(15000);  // voice replies can take a few seconds
  if (!http.begin(String(SERVER) + path)) return "";
  int code = post ? http.POST("") : http.GET();
  if (code != 200)
    Serial.printf("HTTP %s %s%s -> code %d (%s)\n", post ? "POST" : "GET", SERVER,
                  path.c_str(), code, code < 0 ? HTTPClient::errorToString(code).c_str() : "");
  String body = (code == 200) ? http.getString() : "";
  http.end();
  return body;
}

// The OLED only has plain ASCII, so tidy up curly quotes, dashes and emoji.
String sanitize(const String &s) {
  String out;
  for (unsigned i = 0; i < s.length(); i++) {
    uint8_t c = s[i];
    if (c < 128) out += (char)c;
    else if (c == 0xE2 && i + 2 < s.length()) {
      uint8_t b3 = s[i + 2];
      out += (b3 == 0x98 || b3 == 0x99) ? '\'' : (b3 == 0x9C || b3 == 0x9D) ? '"' : '-';
      i += 2;
    }
  }
  return out;
}

// Fetches an endpoint that returns {"reply": "...", "audio_id": "..."}
bool fetchSpeakable(const String &path, bool post, String &text, String &audioId) {
  String body = httpRequest(path, post);
  if (!body.length()) return false;
  JsonDocument d;
  if (deserializeJson(d, body)) return false;
  text = sanitize(String((const char *)(d["reply"] | "")));
  audioId = String((const char *)(d["audio_id"] | ""));
  return true;
}

bool syncWithServer() {
  if (WiFi.status() != WL_CONNECTED) return false;
  if (pendingFiredId >= 0 &&
      httpRequest("/api/robot/alarms/" + String(pendingFiredId) + "/fired", true).length())
    pendingFiredId = -1;

  String body = httpRequest("/api/robot/sync", false);
  if (!body.length()) return false;
  JsonDocument doc;
  if (deserializeJson(doc, body)) return false;

  int Y, M, D, h, m, s;
  const char *st = doc["server_time"] | "";
  if (sscanf(st, "%d-%d-%dT%d:%d:%d", &Y, &M, &D, &h, &m, &s) != 6) return false;
  struct tm t = {};
  t.tm_year = Y - 1900; t.tm_mon = M - 1; t.tm_mday = D;
  t.tm_hour = h; t.tm_min = m; t.tm_sec = s; t.tm_isdst = 0;
  baseTime = mktime(&t);
  baseMillis = millis();
  timeValid = true;

  alarmCount = 0;
  for (JsonObject a : doc["alarms"].as<JsonArray>()) {
    if (alarmCount >= 10) break;
    int hh = 0, mm = 0;
    sscanf(a["time"] | "00:00", "%d:%d", &hh, &mm);
    bool wk = strcmp(a["repeat"] | "once", "weekdays") == 0;
    alarms[alarmCount++] = Alarm{a["id"] | 0, hh, mm, wk};
  }
  Serial.printf("Synced: %s, %d alarm(s)\n", st, alarmCount);
  return true;
}

// ------------------------------------------------ speaking (download + play)
// Streams 16 kHz mono PCM from the server straight into the amplifier.
void playAudio(const String &audioId) {
  HTTPClient http;
  http.setConnectTimeout(3000);
  http.setTimeout(10000);
  if (!http.begin(String(SERVER) + "/api/robot/audio/" + audioId)) return;
  if (http.GET() != 200) { http.end(); return; }

  WiFiClient *s = http.getStreamPtr();
  int remaining = http.getSize();  // -1 if unknown
  static uint8_t in[1024];
  static int16_t out[1024];  // 512 mono samples -> 512 stereo frames
  bool carryValid = false;
  uint8_t carry = 0;
  uint32_t lastData = millis(), lastMouth = 0;

  while ((http.connected() || s->available()) && remaining != 0) {
    if (buttonDown()) break;  // press the button to interrupt
    size_t avail = s->available();
    if (!avail) {
      if (millis() - lastData > 5000) break;
      delay(2);
      continue;
    }
    lastData = millis();
    size_t off = carryValid ? 1 : 0;
    if (carryValid) in[0] = carry;
    size_t n = min(avail, sizeof(in) - off);
    if (remaining > 0 && (int)n > remaining) n = remaining;
    n = s->readBytes(in + off, n);
    if (remaining > 0) remaining -= n;

    size_t total = off + n, pairs = total / 2;
    carryValid = total & 1;
    if (carryValid) carry = in[total - 1];

    long energy = 0;
    for (size_t i = 0; i < pairs; i++) {
      int32_t v = (int16_t)(in[2 * i] | (in[2 * i + 1] << 8));
      v = constrain(v * SPEECH_GAIN_PCT / 100, -32768, 32767);
      out[2 * i] = out[2 * i + 1] = (int16_t)v;
      energy += abs(v);
    }
    if (pairs) {
      spkWrite(out, pairs * 4);
      if (millis() - lastMouth > 120) {  // animate the mouth with the voice
        lastMouth = millis();
        drawModeFace(F_SPEAK, constrain((int)(energy / pairs / 250), 0, 14));
      }
    }
  }
  http.end();
  spkSilence();
  waitRelease();
}

void speakReply(const String &text, const String &audioId) {
  if (audioId.length()) {
    drawModeFace(F_SPEAK, 0);
    playAudio(audioId);
  } else if (text.length()) {
    showPaged(text);  // no audio available (TTS failed): read it on the OLED instead
  }
}

// ------------------------------------------------ listening (record + upload)
#define CAP_HEARD   0
#define CAP_SILENCE 1
#define CAP_ABORT   2
#define CAP_ERROR   3


void sendChunk(WiFiClient &c, const uint8_t *d, size_t n) {
  char h[12];
  int hl = snprintf(h, sizeof h, "%X\r\n", (unsigned)n);
  c.write((const uint8_t *)h, hl);
  c.write(d, n);
  c.write((const uint8_t *)"\r\n", 2);
}

String readLine(WiFiClient &c, uint32_t deadline) {
  String line;
  while (millis() < deadline) {
    if (c.available()) {
      char ch = c.read();
      if (ch == '\n') return line;
      if (ch != '\r') line += ch;
    } else if (!c.connected()) {
      break;
    } else {
      delay(2);
    }
  }
  return line;
}

int readHttpResponse(WiFiClient &c, String &body, uint32_t timeoutMs) {
  uint32_t deadline = millis() + timeoutMs;
  String status = readLine(c, deadline);  // "HTTP/1.1 200 OK"
  int code = status.substring(status.indexOf(' ') + 1).toInt();
  long clen = -1;
  for (;;) {
    String h = readLine(c, deadline);
    if (h.length() == 0) break;
    h.toLowerCase();
    if (h.startsWith("content-length:")) clen = h.substring(15).toInt();
  }
  body = "";
  while (millis() < deadline && (clen < 0 || (long)body.length() < clen)) {
    if (c.available()) body += (char)c.read();
    else if (!c.connected()) break;
    else delay(1);
  }
  return code;
}

// Waits for speech (up to waitMs), streams it to the server while you talk, then
// reads the server's answer into resp.
int captureAndUpload(uint32_t waitMs, JsonDocument &resp) {
    static int16_t blk[BLOCK];
  static int16_t preroll[PREROLL][BLOCK];

  xStreamBufferReset(micBuf);
  micRunning = true;
  drawModeFace(F_LISTEN);

  // skip start-up noise, then measure the background level (the quietest block)
  for (int i = 0; i < 8; i++) readBlock(blk, 100);
  int noise = 32767;
  for (int i = 0; i < 12; i++)
    if (readBlock(blk, 100)) noise = min(noise, blockLevel(blk));
  if (noise == 32767) {  // not a single block arrived: the microphone is not delivering data
    Serial.println("MIC: no audio data. Check INMP441 wiring (SCK 32, WS 25, SD 33, VDD 3.3V, GND).");
    micRunning = false;
    showMessage("Mic not working. Check wiring.", 3000);
    return CAP_ABORT;
  }
  int thr = max(MIN_SPEECH_LEVEL, noise * 4);
  if (DEBUG_MIC) Serial.printf("mic noise=%d threshold=%d\n", noise, thr);

  int ringPos = 0, filled = 0, loud = 0;
  uint32_t t0 = millis(), lastDbg = 0;
  while (millis() - t0 < waitMs) {
    if (buttonPressedEdge()) { micRunning = false; return CAP_ABORT; }
    if (!readBlock(blk, 100)) continue;
    memcpy(preroll[ringPos], blk, sizeof blk);
    ringPos = (ringPos + 1) % PREROLL;
    if (filled < PREROLL) filled++;
    int level = blockLevel(blk);
    if (DEBUG_MIC && millis() - lastDbg > 500) {
      lastDbg = millis();
      Serial.printf("mic level=%d thr=%d\n", level, thr);
    }
    loud = level > thr ? loud + 1 : 0;
    if (loud >= 4) break;  // speech started
  }
  if (loud < 4) { micRunning = false; return CAP_SILENCE; }

  WiFiClient client;
  if (!client.connect(srvHost.c_str(), srvPort, 5000)) { micRunning = false; return CAP_ERROR; }
  client.printf("POST /api/robot/voice?session_id=robot HTTP/1.1\r\nHost: %s:%u\r\n"
                "Content-Type: application/octet-stream\r\nTransfer-Encoding: chunked\r\n"
                "Connection: close\r\n\r\n", srvHost.c_str(), srvPort);

  int start = (filled == PREROLL) ? ringPos : 0;  // oldest block first
  for (int k = 0; k < filled; k++)
    sendChunk(client, (uint8_t *)preroll[(start + k) % PREROLL], sizeof blk);

  uint32_t rec0 = millis();
  int silent = 0, misses = 0;
  while (millis() - rec0 < MAX_RECORD_MS) {
    if (buttonPressedEdge()) break;  // pressing the button = "I'm done talking"
    if (!readBlock(blk, 200)) { if (++misses > 20) break; continue; }
    sendChunk(client, (uint8_t *)blk, sizeof blk);
    silent = blockLevel(blk) < thr ? silent + 1 : 0;
    if (silent * 20 >= (int)END_SILENCE_MS) break;
  }
  micRunning = false;
  client.print("0\r\n\r\n");
  drawModeFace(F_THINK);

  String body;
  int code = readHttpResponse(client, body, 30000);  // speech-to-text + AI + voice
  client.stop();
  if (code != 200) { Serial.printf("voice request failed, HTTP %d\n", code); return CAP_ERROR; }
  if (deserializeJson(resp, body)) return CAP_ERROR;
  return CAP_HEARD;
}

// The conversation: listen, answer, keep listening until you go quiet.
void conversation() {
  uint32_t deadline = millis() + SESSION_WAIT_MS;
  for (;;) {
    int32_t remaining = (int32_t)(deadline - millis());
    if (remaining <= 0) break;

    JsonDocument resp;
    int r = captureAndUpload(remaining, resp);
    if (r == CAP_ABORT || r == CAP_SILENCE) break;
    if (r == CAP_ERROR) { showMessage("Could not reach the server.", 2000); break; }
    if (resp["no_speech"] | false) continue;

    String heard = sanitize(String((const char *)(resp["heard"] | "")));
    String reply = sanitize(String((const char *)(resp["reply"] | "")));
    String audioId = String((const char *)(resp["audio_id"] | ""));
    Serial.printf("Heard: %s\nReply: %s\n", heard.c_str(), reply.c_str());

    speakReply(reply, audioId);
    if (resp["end_session"] | false) break;
    delay(250);  // let the speaker tail die out before listening again
    deadline = millis() + SESSION_WAIT_MS;
  }
  httpRequest("/api/robot/end-session", true);
  lastDrawKey = -1;
}

// -------------------------------------------------------------- alarms
void checkAlarms() {
  struct tm t;
  if (state == RINGING || !getLocal(t)) return;
  long minuteKey = (baseTime + (millis() - baseMillis) / 1000) / 60;
  if (minuteKey == lastRungKey) return;
  for (int i = 0; i < alarmCount; i++) {
    Alarm &a = alarms[i];
    if (a.hour != t.tm_hour || a.minute != t.tm_min) continue;
    if (a.weekdaysOnly && (t.tm_wday == 0 || t.tm_wday == 6)) continue;
    lastRungKey = minuteKey;
    ringingId = a.id;
    state = RINGING;
    return;
  }
}

void morningFlow() {
  showMessage("Good morning...");
  String text, audioId;
  if (!fetchSpeakable("/api/robot/morning", true, text, audioId)) {
    showMessage("Good morning. Server not reachable.", 3000);
    return;
  }
  speakReply(text, audioId);  // "Good morning <name>, do you want to know today's plans?"
  conversation();             // you answer "yes", then keep talking
}

void ringAlarm() {
  display.dim(false);
  uint32_t start = millis();
  bool stopped = false, inv = false;
  while (!stopped && millis() - start < ALARM_MAX_MS) {
    drawAlarmScreen(inv);
    inv = !inv;
    for (int i = 0; i < 3 && !stopped; i++) {
      if (!playTone(1000, 120) || !playTone(0, 80)) stopped = true;
    }
    if (!stopped && !playTone(0, 500)) stopped = true;
  }
  spkSilence();
  waitRelease();

  int id = ringingId;
  ringingId = -1;
  if (id >= 0 && httpRequest("/api/robot/alarms/" + String(id) + "/fired", true).length() == 0)
    pendingFiredId = id;

  stayAwakeUntil = millis() + 600000UL;  // no "good night" for 10 minutes after an alarm
  state = IDLE;
  if (stopped) morningFlow();
}

// ----------------------------------------------------------------- LDR
void updateLDR() {
  static uint32_t last = 0, lastPrint = 0;
  uint32_t now = millis();
  if (now - last < 100) return;
  last = now;
  int raw = analogRead(PIN_LDR);
  ldrAvg = ldrAvg * 0.9f + raw * 0.1f;
  if (now - lastPrint > 1000) {
    lastPrint = now;
    Serial.printf("LDR raw=%d avg=%.0f\n", raw, ldrAvg);
  }
  if (ldrAvg < DARK_BELOW) { if (!darkSince) darkSince = now; } else darkSince = 0;
  if (ldrAvg > BRIGHT_ABOVE) { if (!brightSince) brightSince = now; } else brightSince = 0;
}

void goodNight() {
  display.dim(false);
  String text = "Good night", audioId;
  fetchSpeakable("/api/robot/good-night", false, text, audioId);  // fine if this fails
  showMessage(text);
  if (audioId.length()) playAudio(audioId);
  else delay(2500);
  drawSleep();
  display.dim(true);
  state = SLEEPING;
}

// ------------------------------------------------------- main loop bits
void idleTick() {
  if (buttonPressedEdge()) {
    waitRelease();
    playTone(1320, 80);  // little "I'm listening" beep
    conversation();
    return;
  }
  bool canSleep = (int32_t)(millis() - stayAwakeUntil) > 0;
  if (darkSince && millis() - darkSince > DARK_DELAY_MS && canSleep) {
    goodNight();
    return;
  }
  bool blink = (millis() % 4000) < 150;
  struct tm t;
  int minuteOfDay = getLocal(t) ? t.tm_hour * 60 + t.tm_min : 0;
  int key = (blink ? 10000 : 0) + minuteOfDay + (lastSyncOk ? 0 : 20000);
  if (key != lastDrawKey) { drawFace(blink); lastDrawKey = key; }
}

void sleepTick() {
  if (brightSince && millis() - brightSince > BRIGHT_DELAY_MS) {
    display.dim(false);
    state = IDLE;
    lastDrawKey = -1;
  }
}

void setup() {
  Serial.begin(115200);
  pinMode(PIN_BTN, INPUT_PULLUP);
  analogReadResolution(12);
  Wire.begin(PIN_SDA, PIN_SCL);
  if (!display.begin(SSD1306_SWITCHCAPVCC, 0x3C)) {  // try 0x3D if the screen stays blank
    Serial.println("OLED not found");
    while (true) delay(1000);
  }
  display.setTextColor(WHITE);

  if (strcmp(WIFI_SSID, "YOUR_WIFI_NAME") == 0) {  // forgot to edit the settings at the top
    Serial.println("Edit WIFI_SSID / WIFI_PASS / SERVER at the top of the sketch!");
    showMessage("Edit WIFI_SSID and WIFI_PASS in the sketch");
    while (true) delay(1000);
  }

  initAudio();
  parseServer();

  showMessage("Connecting to WiFi...");
  WiFi.persistent(false);
  WiFi.mode(WIFI_STA);
  WiFi.disconnect(true, true);
  delay(500);
  WiFi.setSleep(false);
  WiFi.setTxPower(WIFI_POWER_11dBm);
  WiFi.begin(WIFI_SSID, WIFI_PASS);
  uint32_t t0 = millis();
  while (WiFi.status() != WL_CONNECTED && millis() - t0 < 20000) delay(250);

  if (WiFi.status() == WL_CONNECTED)
    Serial.printf("WiFi connected. Robot IP: %s\n", WiFi.localIP().toString().c_str());
  else
    Serial.printf("WiFi NOT connected, status code %d\n", WiFi.status());
  showMessage(WiFi.status() == WL_CONNECTED ? "WiFi OK. Syncing..." : "No WiFi. Will keep trying.");

  lastSyncOk = syncWithServer();
  lastSync = millis();
  if (!lastSyncOk) showMessage("Server not reachable. Check SERVER address.", 2000);

  ldrAvg = analogRead(PIN_LDR);
  playTone(880, 100);
  playTone(1320, 120);
}

void loop() {
  uint32_t now = millis();

  static uint32_t lastWifiTry = 0;
  if (WiFi.status() != WL_CONNECTED && now - lastWifiTry > 15000) {
    lastWifiTry = now;
    WiFi.disconnect();
    WiFi.begin(WIFI_SSID, WIFI_PASS);
  }

  if (state != RINGING && now - lastSync > (lastSyncOk ? SYNC_EVERY_MS : 10000)) {
    lastSyncOk = syncWithServer();
    lastSync = millis();
  }

  updateLDR();
  checkAlarms();

  switch (state) {
    case RINGING:  ringAlarm(); lastDrawKey = -1; break;
    case IDLE:     idleTick(); break;
    case SLEEPING: sleepTick(); break;
  }
  delay(10);
}
