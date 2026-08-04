/* ━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━
 * wifi_bridge —— ESP32-S3 當 STM32 掌機的 WiFi 協處理器
 *
 * STM32F407 沒有網路硬體，而且把 TCP/IP + TLS + JSON 塞進去也不划算。
 * 所以網路那一半整個交給 ESP32：它跑 WiFi/HTTPS/JSON，對 STM32 只暴露
 * 一組「一行一則」的文字指令。STM32 那邊就只是一個 UART 驅動而已。
 *
 *   STM32  ──UART 115200 8N1──  ESP32-S3  ──WiFi──  Internet
 *          「WX?」→                    → HTTP GET wttr.in
 *          ← 「WX Taipei: +33C」       ←
 *
 * 接線（兩邊都是 3.3V，不需要準位轉換，但 GND 一定要共接）：
 *
 *   STM32 PB10 (USART3_TX) ──→ ESP32 GPIO18 (STM_RX)
 *   STM32 PB11 (USART3_RX) ←── ESP32 GPIO17 (STM_TX)
 *   GND                    ─── GND
 *
 * 設定與踩過的坑見同目錄 ../README.md。憑證放 secrets.h（已 gitignore）。
 * ━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━ */

#include <WiFi.h>
#include <WiFiClientSecure.h>
#include <HTTPClient.h>
#include <ArduinoJson.h>          // 程式庫管理員裝 ArduinoJson 7.x
#include <time.h>

/* WiFi 帳密與 Discord token。複製 secrets.h.example 成 secrets.h 再填。
 * secrets.h 在 .gitignore 裡 —— 真實憑證絕不進版控。 */
#include "secrets.h"

#define STM_RX 18   // 接 STM32 PB10 (TX)
#define STM_TX 17   // 接 STM32 PB11 (RX)

const uint32_t DC_POLL_MS = 3000; // 每 3 秒問一次 Discord 有沒有新訊息

// NTP：台灣 UTC+8，無日光節約
const long  GMT_OFFSET = 8 * 3600;
const int   DST_OFFSET = 0;

String line;   // 累積 STM32 送來的字元，直到換行

void downloadFile(String url);

/* ── 訊息佇列：ESP32 自己輪詢 Discord 收進來，STM32 用 MSG? 一則一則拿 ──
 * 這樣兩邊解耦：網路節奏（rate limit）由 ESP32 管，STM32 只做便宜的 UART 詢問。*/
#define MSGQ_N 8
String   msgq[MSGQ_N];
int      msgq_head = 0, msgq_tail = 0;
String   dc_lastId = "";          // 已處理到哪一則（Discord snowflake ID）
uint32_t dc_next_poll = 0;

void msgq_push(const String& s) {
  int n = (msgq_head + 1) % MSGQ_N;
  if (n != msgq_tail) { msgq[msgq_head] = s; msgq_head = n; }   // 滿了就丟掉最新的
}
bool msgq_pop(String& s) {
  if (msgq_head == msgq_tail) return false;
  s = msgq[msgq_tail];
  msgq_tail = (msgq_tail + 1) % MSGQ_N;
  return true;
}

// STM32 字型只認 ASCII，且協定是「一行一則」→ 濾掉非 ASCII、換行改空格
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

/* ── Discord：輪詢新訊息 ──
 * 第一次只抓最新一則當基準（不把舊訊息全倒出來），之後用 ?after=<id> 只拿新的。*/
void discordPoll() {
  if (WiFi.status() != WL_CONNECTED || strlen(DC_TOKEN) == 0) return;

  WiFiClientSecure client;
  client.setInsecure();            // 不驗證憑證（自用專案夠了，省下內建 CA 的麻煩）
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
      // Discord 回傳「新→舊」，反向走才是時間順序
      for (int i = arr.size() - 1; i >= 0; i--) {
        JsonObject m = arr[i];
        dc_lastId = m["id"].as<String>();          // 推進進度（含被略過的）

        if (first) continue;                        // 第一次只取基準，不顯示
        if (m["author"]["bot"] | false) continue;   // 略過 bot 自己（避免自問自答）

        String user = toAscii(m["author"]["username"].as<String>(), 12);
        String text = toAscii(m["content"].as<String>(), 80);
        if (text.length() == 0) continue;           // 純圖片/貼圖，沒有文字內容

        msgq_push(user + ": " + text);
        Serial.printf("DC recv> %s: %s\n", user.c_str(), text.c_str());
      }
    }
  } else if (code != 200) {
    Serial.printf("DC poll err %d\n", code);
  }
  http.end();
}

// Discord：送一則訊息到頻道
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
  if (cmd.length() == 0) return;   // 空指令（殘留換行等）忽略
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
    if (getLocalTime(&t, 100)) {   // 最多等 100ms 拿本地時間
      Serial1.printf("TIME %02d:%02d:%02d\n", t.tm_hour, t.tm_min, t.tm_sec);
    } else {
      Serial1.print("TIME NONE\n");   // 還沒對到時
    }
  } else if (cmd == "WX?") {
    if (WiFi.status() != WL_CONNECTED) { Serial1.print("WX DOWN\n"); return; }
    HTTPClient http;
    // wttr.in 天氣：format 用 "地點: +溫度"，避開 emoji（螢幕字型顯示不了）
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
    // 有新訊息就給一則，沒有就 MSGNONE
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

// 等 STM32 回一個 'A'（chunk ack），逾時回 false
bool waitAck(uint32_t timeout_ms) {
  uint32_t start = millis();
  while (millis() - start < timeout_ms) {
    if (Serial1.available() && Serial1.read() == 'A') return true;
  }
  return false;
}

/* 下載 url，分塊 + 逐塊等 STM32 ack，把裸 byte 送給 STM32。
 *
 * ⚠️ STM32 端的對應實作（net_download）已移除 —— 協定驗證過可用，但一直
 *    沒接到任何 UI 動作。這半邊保留著，要復原 STM32 那半就：
 *        git show c04cb1b -- Core/Src/main.c
 *
 * 協定：STM32 送 "DL <url>" → ESP32 回 "BEGIN" → 迴圈送「"C <len>" + 裸資料」
 *       並等 STM32 回 "A" → 最後送 "C 0" 表示結束。
 *       每塊自帶長度，所以不需要事先知道總大小（HTTP/1.0 close-delimited
 *       回應沒有 Content-Length，這是當初改成這樣的原因）。 */
void downloadFile(String url) {
  url.trim();
  if (WiFi.status() != WL_CONNECTED) { Serial1.print("SIZE -1\n"); return; }

  HTTPClient http;
  http.begin(url);
  http.setFollowRedirects(HTTPC_STRICT_FOLLOW_REDIRECTS);   // 自動跟隨轉址
  int code = http.GET();
  Serial.printf("DL: url=%s code=%d\n", url.c_str(), code);

  if (code != 200) { Serial1.print("ERR\n"); http.end(); return; }

  // 先把完整內容抓進記憶體（getString 會處理 chunked 解碼），再分塊送
  String body = http.getString();
  http.end();
  int total = body.length();
  const uint8_t* p = (const uint8_t*)body.c_str();
  Serial.printf("DL: got %d bytes, sending...\n", total);

  Serial1.print("BEGIN\n");             // 通知 STM32 開始
  int sent = 0;
  while (sent < total) {
    int n = min(512, total - sent);
    Serial1.printf("C %d\n", n);        // 這塊的長度標頭
    Serial1.write(p + sent, n);         // 這塊的裸資料
    if (!waitAck(3000)) return;         // 等 STM32 ack 才送下一塊
    sent += n;
  }
  Serial1.print("C 0\n");               // 0 長度 = 傳完
  Serial.printf("download done: %d bytes\n", total);
}

void loop() {
  // STM32 的指令優先處理
  while (Serial1.available()) {
    char c = Serial1.read();
    if (c == '\n') { handleCommand(line); line = ""; }
    else           { line += c; }
  }

  // 自己定期去 Discord 收信
  if (millis() >= dc_next_poll) {
    dc_next_poll = millis() + DC_POLL_MS;
    discordPoll();
  }

  /* 除錯用：在 ESP32 的序列埠監看視窗直接打字 → 送到 Discord。
   * 不需要接 STM32 就能單獨驗證 Discord 那一半通不通，當初 bring-up
   * 就是靠這個把「ESP32↔Discord」和「STM32↔ESP32」兩段分開來測。 */
  if (Serial.available()) {
    String s = Serial.readStringUntil('\n');
    s.trim();
    if (s.length()) discordSend(s);
  }
}
