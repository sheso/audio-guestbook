/**
 * Web server for the audio guestbook, running on a Seeed XIAO ESP32S3 Sense.
 *
 * Copies new recordings from the Teensy over Serial1 to the microSD card of the Sense board, and
 * serves them on its own open Wi-Fi network: http://192.168.4.1/ or http://guestbook.local/
 * See ../REQUIREMENTS.md for the whole design and ../xiao_link.ino for the Teensy side.
 *
 * Board: "XIAO_ESP32S3" (esp32 core by Espressif, 3.x). Libraries: ESPAsyncWebServer and AsyncTCP
 * by ESP32Async.
 *
 * Serial monitor (115200) commands: "status" prints the sync state, "sync" checks the Teensy now.
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
  Serial.printf("Recordings copied: %u, pending: %d, last sync: %ld s ago\n",
                (unsigned)catalog.snapshot().size(), s.pending, (long)s.lastSyncAgo);
  if (s.copyingSize) Serial.printf("Copying %lu: %lu of %lu bytes\n", (unsigned long)s.copyingNum,
                                   (unsigned long)s.copyingDone, (unsigned long)s.copyingSize);
  if (s.lastError.length()) Serial.println("Last error: " + s.lastError);
  Serial.printf("Wi-Fi clients: %d, free heap: %lu\n", WiFi.softAPgetStationNum(), (unsigned long)ESP.getFreeHeap());
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

  WiFi.mode(WIFI_AP);
  WiFi.softAP(AP_SSID, nullptr, AP_CHANNEL, 0, AP_MAX_CLIENTS);
  WiFi.setTxPower(WIFI_TX_POWER);
  Serial.println("Wi-Fi access point \"" AP_SSID "\" at " + WiFi.softAPIP().toString());
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
    else if (cmd.length()) Serial.println("Commands: status, sync");
  }
  delay(20);
}
