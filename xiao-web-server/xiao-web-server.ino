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
 * every HTTP request.
 */

#include "catalog.h"
#include "config.h"
#include "sync.h"
#include "web.h"
#include <ESPmDNS.h>
#include <SD.h>
#include <SPI.h>
#include <WiFi.h>

static void printStatus() {
  SyncStatus s = syncStatus();
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

// Joins the network from wifi_secrets.h; returns false if there is none or it cannot be joined
static bool joinNetwork() {
  if (!strlen(WIFI_SSID)) return false;
  Serial.println("Wi-Fi: joining \"" WIFI_SSID "\"");
  WiFi.mode(WIFI_STA);
  WiFi.setAutoReconnect(true);
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

  while (!SD.begin(SD_CS_PIN, SPI, SD_FREQ)) {
    Serial.println("Unable to access the SD card, retrying");
    delay(5000);
  }
  Serial.printf("SD card: %llu MB used of %llu MB\n", SD.usedBytes() >> 20, SD.totalBytes() >> 20);
  catalog.begin();

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
    else if (cmd.length()) Serial.println("Commands: status, sync, trace on, trace off, http on, http off");
  }
  delay(20);
}
