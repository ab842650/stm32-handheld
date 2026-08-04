/* wifi_bridge - ESP32-S3 acting as the handheld's WiFi co-processor.
 *
 * The STM32F407 has no networking hardware, so the whole network half lives
 * here: WiFi, HTTPS, NTP and JSON, exposed to the STM32 as one-line text
 * commands. That reduces "the internet" to a UART driver on the other side.
 *
 *   STM32 PB10 (USART3_TX) --> ESP32 GPIO18 (STM_RX)
 *   STM32 PB11 (USART3_RX) <-- ESP32 GPIO17 (STM_TX)
 *   GND                    --- GND       (required; both sides are 3.3 V)
 *
 * Protocol table and setup notes are in ../README.md.
 * Credentials live in secrets.h, which is gitignored. */

#include <WiFi.h>
#include <WiFiClientSecure.h>
#include <HTTPClient.h>
#include <ArduinoJson.h>          // v7.x; the v6 API will not compile
#include <time.h>

#include "secrets.h"              // copy secrets.h.example and fill it in

#define STM_RX 18   // to STM32 PB10 (TX)
#define STM_TX 17   // to STM32 PB11 (RX)

const uint32_t DC_POLL_MS = 3000;

const long  GMT_OFFSET = 8 * 3600;   // UTC+8, no DST
const int   DST_OFFSET = 0;

String line;   // accumulates STM32 bytes until a newline

void downloadFile(String url);

/* The ESP32 polls Discord on its own schedule into this queue; the STM32 just
 * asks MSG? for one at a time. Rate limiting stays on the network side. */
#define MSGQ_N 8
String   msgq[MSGQ_N];
int      msgq_head = 0, msgq_tail = 0;
String   dc_lastId = "";          // snowflake of the last handled message
uint32_t dc_next_poll = 0;

void msgq_push(const String& s) {
  int n = (msgq_head + 1) % MSGQ_N;
  if (n != msgq_tail) { msgq[msgq_head] = s; msgq_head = n; }   // full: drop it
}
bool msgq_pop(String& s) {
  if (msgq_head == msgq_tail) return false;
  s = msgq[msgq_tail];
  msgq_tail = (msgq_tail + 1) % MSGQ_N;
  return true;
}

// The STM32 font is ASCII-only and the protocol is one message per line.
String toAscii(const String& s, int maxlen) {
  String out;
  for (size_t i = 0; i < s.length() && (int)out.length() < maxlen; i++) {
    char c = s[i];
    if (c == '\n' || c == '\r' || c == '\t') c = ' ';
    if ((uint8_t)c >= 0x20 && (uint8_t)c < 0x7F) out += c;
  }
  return out;
}

void setup() {
  Serial.begin(115200);
  Serial1.begin(115200, SERIAL_8N1, STM_RX, STM_TX);
  Serial.println("=== ESP32-S3 wifi_bridge ===");

  WiFi.mode(WIFI_STA);
  WiFi.begin(WIFI_SSID, WIFI_PASS);
  Serial.print("Connecting WiFi");
  for (int i = 0; i < 20 && WiFi.status() != WL_CONNECTED; i++) {
    delay(500);
    Serial.print(".");
  }
  Serial.println();
  if (WiFi.status() == WL_CONNECTED) {
    Serial.printf("WiFi OK, IP = %s\n", WiFi.localIP().toString().c_str());
    configTime(GMT_OFFSET, DST_OFFSET, "pool.ntp.org", "time.nist.gov");
    Serial.println("NTP sync started");
  } else {
    Serial.println("WiFi FAILED");
  }
}

/* The first poll fetches a single message purely to establish a baseline ID,
 * otherwise the whole channel history dumps out on boot. */
void discordPoll() {
  if (WiFi.status() != WL_CONNECTED || strlen(DC_TOKEN) == 0) return;

  WiFiClientSecure client;
  client.setInsecure();            // no cert validation; fine for personal use
  HTTPClient http;

  String url = "https://discord.com/api/v10/channels/" + String(DC_CHANNEL) + "/messages?limit=";
  bool first = (dc_lastId.length() == 0);
  url += first ? "1" : ("5&after=" + dc_lastId);

  http.begin(client, url);
  http.addHeader("Authorization", String("Bot ") + DC_TOKEN);
  int code = http.GET();

  if (code == 200) {
    JsonDocument doc;
    if (deserializeJson(doc, http.getString()) == DeserializationError::Ok) {
      JsonArray arr = doc.as<JsonArray>();
      // Discord returns newest first, so walk it backwards for chronological order
      for (int i = arr.size() - 1; i >= 0; i--) {
        JsonObject m = arr[i];
        dc_lastId = m["id"].as<String>();          // advance past skipped ones too

        if (first) continue;                        // baseline only
        if (m["author"]["bot"] | false) continue;   // or it reads back its own posts

        String user = toAscii(m["author"]["username"].as<String>(), 12);
        String text = toAscii(m["content"].as<String>(), 80);
        if (text.length() == 0) continue;           // image or sticker, no text

        msgq_push(user + ": " + text);
        Serial.printf("DC recv> %s: %s\n", user.c_str(), text.c_str());
      }
    }
  } else if (code != 200) {
    Serial.printf("DC poll err %d\n", code);
  }
  http.end();
}

bool discordSend(const String& text) {
  if (WiFi.status() != WL_CONNECTED || strlen(DC_TOKEN) == 0) return false;

  WiFiClientSecure client;
  client.setInsecure();
  HTTPClient http;

  http.begin(client, "https://discord.com/api/v10/channels/" + String(DC_CHANNEL) + "/messages");
  http.addHeader("Authorization", String("Bot ") + DC_TOKEN);
  http.addHeader("Content-Type", "application/json");

  JsonDocument doc;
  doc["content"] = text;
  String body;
  serializeJson(doc, body);

  int code = http.POST(body);
  http.end();
  Serial.printf("DC send> [%s] code=%d\n", text.c_str(), code);
  return (code == 200 || code == 201);
}

void handleCommand(String cmd) {
  cmd.trim();
  if (cmd.length() == 0) return;   // stray newline
  Serial.printf("STM32 said: [%s]\n", cmd.c_str());

  if (cmd == "PING") {
    Serial1.print("PONG\n");
  } else if (cmd == "WIFI?") {
    if (WiFi.status() == WL_CONNECTED)
      Serial1.printf("WIFI OK %s\n", WiFi.localIP().toString().c_str());
    else
      Serial1.print("WIFI DOWN\n");
  } else if (cmd == "TIME?") {
    struct tm t;
    if (getLocalTime(&t, 100)) {
      Serial1.printf("TIME %02d:%02d:%02d\n", t.tm_hour, t.tm_min, t.tm_sec);
    } else {
      Serial1.print("TIME NONE\n");   // NTP has not synced yet
    }
  } else if (cmd == "WX?") {
    if (WiFi.status() != WL_CONNECTED) { Serial1.print("WX DOWN\n"); return; }
    HTTPClient http;
    // Ask for "place: +temp" only — the default wttr.in output is full of
    // emoji the screen font cannot draw.
    http.begin("http://wttr.in/Taipei?format=%l:+%t");
    http.setTimeout(5000);
    int code = http.GET();
    if (code == 200) {
      String body = http.getString();
      body.trim();
      Serial1.printf("WX %s\n", toAscii(body, 40).c_str());
    } else {
      Serial1.printf("WX ERR %d\n", code);
    }
    http.end();
  } else if (cmd == "MSG?") {
    String m;
    if (msgq_pop(m)) Serial1.printf("MSG %s\n", m.c_str());
    else             Serial1.print("MSGNONE\n");
  } else if (cmd.startsWith("SEND ")) {
    Serial1.print(discordSend(cmd.substring(5)) ? "SENDOK\n" : "SENDERR\n");
  } else if (cmd.startsWith("DL ")) {
    downloadFile(cmd.substring(3));
  } else {
    Serial1.print("ERR unknown\n");
  }
}

// Waits for the STM32's single-byte 'A' chunk ack.
bool waitAck(uint32_t timeout_ms) {
  uint32_t start = millis();
  while (millis() - start < timeout_ms) {
    if (Serial1.available() && Serial1.read() == 'A') return true;
  }
  return false;
}

/* Chunked download, paced by the STM32.
 *
 * The STM32 half (net_download) was removed - the protocol worked but was
 * never wired to a UI action. Restore it with:
 *     git show c04cb1b -- Core/Src/main.c
 *
 * "DL <url>" -> "BEGIN", then repeated "C <len>" plus raw bytes, each waiting
 * for the STM32 to ack with "A", ending with "C 0". Each chunk carries its own
 * length so the total never has to be known in advance - HTTP/1.0
 * close-delimited responses have no Content-Length. The ack is flow control:
 * it stops the ESP32 outrunning the STM32's RX ring while it writes to SD. */
void downloadFile(String url) {
  url.trim();
  if (WiFi.status() != WL_CONNECTED) { Serial1.print("SIZE -1\n"); return; }

  HTTPClient http;
  http.begin(url);
  http.setFollowRedirects(HTTPC_STRICT_FOLLOW_REDIRECTS);
  int code = http.GET();
  Serial.printf("DL: url=%s code=%d\n", url.c_str(), code);

  if (code != 200) { Serial1.print("ERR\n"); http.end(); return; }

  String body = http.getString();   // also handles chunked transfer decoding
  http.end();
  int total = body.length();
  const uint8_t* p = (const uint8_t*)body.c_str();
  Serial.printf("DL: got %d bytes, sending...\n", total);

  Serial1.print("BEGIN\n");
  int sent = 0;
  while (sent < total) {
    int n = min(512, total - sent);
    Serial1.printf("C %d\n", n);        // length header
    Serial1.write(p + sent, n);         // raw payload
    if (!waitAck(3000)) return;
    sent += n;
  }
  Serial1.print("C 0\n");               // zero length ends it
  Serial.printf("download done: %d bytes\n", total);
}

void loop() {
  while (Serial1.available()) {
    char c = Serial1.read();
    if (c == '\n') { handleCommand(line); line = ""; }
    else           { line += c; }
  }

  if (millis() >= dc_next_poll) {
    dc_next_poll = millis() + DC_POLL_MS;
    discordPoll();
  }

  /* Debug aid: typing in the ESP32 serial monitor posts straight to Discord,
   * which verifies the ESP32<->Discord half without the STM32 attached.
   * Splitting bring-up that way was worth doing. */
  if (Serial.available()) {
    String s = Serial.readStringUntil('\n');
    s.trim();
    if (s.length()) discordSend(s);
  }
}
