/**
 * Web server for the audio guestbook, running on a Seeed XIAO ESP32S3 Sense.
 *
 * Copies new recordings from the Teensy over Serial1 to the microSD card of the Sense board, and
 * serves them over Wi-Fi: on the network from wifi_secrets.h if it is set and reachable (the address
 * is printed on the serial monitor), otherwise on its own open network at http://192.168.4.1/
 * See ../REQUIREMENTS.md for the whole design and ../xiao_link.ino for the Teensy side.
 *
 * Board: "XIAO_ESP32S3" (esp32 core by Espressif, 3.x). Libraries: ESPAsyncWebServer and AsyncTCP
 * by ESP32Async.
 *
 * Serial monitor (115200) commands: "status" prints the sync state, "sync" checks the Teensy now,
 * "trace on" / "trace off" prints the protocol traffic with the Teensy, "http on" / "http off" prints
 * every HTTP request, "get <n> [range]" downloads recording n from our own web server, "hdr <n>" prints
 * its WAV header.
 */

#include "catalog.h"
#include "config.h"
#include "sync.h"
#include "web.h"
#include <ESPmDNS.h>
#include <HTTPClient.h>
#include <SD.h>
#include <SPI.h>
#include <WiFi.h>

static void printStatus() {
  SyncStatus s = syncStatus();
  if (s.sdOk) Serial.printf("SD card: %llu MB used of %llu MB\n", SD.usedBytes() >> 20, SD.totalBytes() >> 20);
  else Serial.println("SD card: cannot be read, retrying");
  Serial.printf("Teensy: %s, mode %s, clock %lu (%s)\n", s.teensyOnline ? "online" : "offline",
                s.teensyMode.c_str(), (unsigned long)s.teensyTime, clockValid(s.teensyTime) ? "valid" : "not set");
  Serial.printf("Recordings on the Teensy: %d, copied: %u, pending: %d, last sync: %ld s ago\n",
                s.teensyFiles, (unsigned)catalog.snapshot().size(), s.pending, (long)s.lastSyncAgo);
  if (s.copyingSize) Serial.printf("Copying %lu: %lu of %lu bytes\n", (unsigned long)s.copyingNum,
                                   (unsigned long)s.copyingDone, (unsigned long)s.copyingSize);
  if (s.lastError.length()) Serial.println("Last error: " + s.lastError);
  if (WiFi.getMode() == WIFI_STA) {
    Serial.printf("Wi-Fi: %s \"%s\", IP %s, signal %d dBm\n", WiFi.isConnected() ? "connected to" : "reconnecting to",
                  WIFI_SSID, WiFi.localIP().toString().c_str(), WiFi.RSSI());
  }
  else {
    Serial.printf("Wi-Fi: access point \"" AP_SSID "\", %d clients\n", WiFi.softAPgetStationNum());
  }
  Serial.printf("Free heap: %lu\n", (unsigned long)ESP.getFreeHeap());
  Serial.println(webStats());
}

// Downloads a recording from our own web server, like a browser would, to test it without a phone
static void selfGet(uint32_t num, bool range) {
  HTTPClient http;
  http.setTimeout(5000);
  http.begin("http://127.0.0.1/api/rec?n=" + String(num));
  if (range) http.addHeader("Range", "bytes=0-1"); // Safari's first request for <audio>
  uint32_t started = millis();
  int code = http.GET();
  int len = http.getSize();
  Serial.printf("get: HTTP %d after %lu ms, Content-Length %d\n", code, (unsigned long)(millis() - started), len);
  if (code > 0) {
    NetworkClient* stream = http.getStreamPtr();
    static uint8_t buf[2048];
    uint32_t total = 0, lastData = millis();
    while (len < 0 || total < (uint32_t)len) {
      int n = stream->available();
      if (n > 0) {
        total += stream->readBytes(buf, min(n, (int)sizeof buf));
        lastData = millis();
      }
      else if (!http.connected() || millis() - lastData > 5000) {
        Serial.println("get: no more data");
        break;
      }
      else {
        delay(1);
      }
    }
    uint32_t ms = max(millis() - started, (uint32_t)1);
    Serial.printf("get: %lu bytes in %lu ms (%lu KB/s)\n", (unsigned long)total, (unsigned long)ms,
                  (unsigned long)(total / ms));
  }
  http.end();
}

// Opens a TCP connection to another device on the network. "refused" still proves that packets get
// through; a timeout means the device is unreachable (e.g. the router isolates its Wi-Fi clients).
static void probe(const String& args) {
  int space = args.indexOf(' ');
  String host = space < 0 ? args : args.substring(0, space);
  uint16_t port = space < 0 ? 80 : args.substring(space + 1).toInt();
  IPAddress ip;
  if (!ip.fromString(host)) {
    Serial.println("probe: bad address " + host);
    return;
  }
  Serial.printf("probe: gateway %s, mask %s, channel %d\n", WiFi.gatewayIP().toString().c_str(),
                WiFi.subnetMask().toString().c_str(), WiFi.channel());
  NetworkClient client;
  uint32_t started = millis();
  bool ok = client.connect(ip, port, 3000);
  uint32_t ms = millis() - started;
  Serial.printf("probe %s:%u: %s after %lu ms\n", host.c_str(), port,
                ok ? "connected" : (ms < 2500 ? "refused (reachable)" : "timeout (unreachable)"), (unsigned long)ms);
  client.stop();
}

// Prints the size and WAV header of a copied recording
static void printHeader(uint32_t num) {
  File f = SD.open(Catalog::path(num), FILE_READ);
  if (!f) {
    Serial.printf("hdr: cannot open %s\n", Catalog::path(num).c_str());
    return;
  }
  uint8_t h[44] = {};
  size_t n = f.read(h, sizeof h);
  Serial.printf("hdr: %s, %lu bytes, header:", Catalog::path(num).c_str(), (unsigned long)f.size());
  for (size_t i = 0; i < n; i++) Serial.printf(" %02x", h[i]);
  auto u32 = [&](int i) { return (unsigned long)(h[i] | h[i + 1] << 8 | h[i + 2] << 16 | (uint32_t)h[i + 3] << 24); };
  auto u16 = [&](int i) { return (unsigned)(h[i] | h[i + 1] << 8); };
  Serial.printf("\nhdr: %.4s size %lu, %.4s, %.4s format %u, %u ch, %lu Hz, %u bit, %.4s size %lu (file has %ld after the header)\n",
                (char*)h, u32(4), (char*)h + 8, (char*)h + 12, u16(20), u16(22), u32(24), u16(34), (char*)h + 36,
                u32(40), (long)f.size() - 44);
  f.close();
}

// Joins the network from wifi_secrets.h; returns false if there is none or it cannot be joined
static bool joinNetwork() {
  if (!strlen(WIFI_SSID)) return false;
  Serial.println("Wi-Fi: joining \"" WIFI_SSID "\"");
  WiFi.mode(WIFI_STA);
  WiFi.setAutoReconnect(true);
  // In modem sleep the XIAO misses packets from the router (mDNS queries, new TCP connections)
  WiFi.setSleep(false);
  // The router hands out the address, so print it on every (re)connection
  WiFi.onEvent([](WiFiEvent_t, WiFiEventInfo_t) {
    Serial.println("Wi-Fi: connected, open http://" + WiFi.localIP().toString() + "/ or http://" MDNS_NAME ".local/");
  }, ARDUINO_EVENT_WIFI_STA_GOT_IP);
  WiFi.begin(WIFI_SSID, WIFI_PASSWORD);
  uint32_t started = millis();
  while (!WiFi.isConnected() && millis() - started < WIFI_CONNECT_TIMEOUT) delay(100);
  if (WiFi.isConnected()) return true;
  Serial.println("Wi-Fi: cannot join \"" WIFI_SSID "\", starting the own access point");
  WiFi.disconnect(true);
  return false;
}

static void startAccessPoint() {
  WiFi.mode(WIFI_AP);
  WiFi.softAP(AP_SSID, nullptr, AP_CHANNEL, 0, AP_MAX_CLIENTS);
  WiFi.setTxPower(WIFI_TX_POWER);
  Serial.println("Wi-Fi access point \"" AP_SSID "\" at " + WiFi.softAPIP().toString());
}

void setup() {
  Serial.begin(115200);
  delay(1000);
  Serial.println("Audio guestbook web server");

  // The SD card is mounted by the sync task, which keeps retrying: without it the network and the
  // page still come up and show the problem
  if (!joinNetwork()) startAccessPoint();
  if (MDNS.begin(MDNS_NAME)) MDNS.addService("http", "tcp", 80);

  webBegin();
  syncBegin();
}

void loop() {
  if (Serial.available()) {
    String cmd = Serial.readStringUntil('\n');
    cmd.trim();
    if (cmd == "status") printStatus();
    else if (cmd == "sync") syncNow();
    else if (cmd == "trace on" || cmd == "trace off") {
      syncSetTrace(cmd == "trace on");
      syncNow();
    }
    else if (cmd == "http on" || cmd == "http off") webSetLog(cmd == "http on");
    else if (cmd.startsWith("get ")) selfGet(cmd.substring(4).toInt(), cmd.endsWith("range"));
    else if (cmd.startsWith("hdr ")) printHeader(cmd.substring(4).toInt());
    else if (cmd.startsWith("probe ")) probe(cmd.substring(6));
    else if (cmd.length()) {
      Serial.println("Commands: status, sync, trace on|off, http on|off, get <n> [range], hdr <n>, probe <ip> [port]");
    }
  }
  delay(20);
}
