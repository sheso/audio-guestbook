#pragma once

// Wi-Fi access point (open network, no password)
#define AP_SSID "Guestbook"
#define AP_CHANNEL 6
#define AP_MAX_CLIENTS 8
#define MDNS_NAME "guestbook" // http://guestbook.local/
// Lower transmit power keeps current peaks within the 600mA limit of the Zelo Power Bank
#define WIFI_TX_POWER WIFI_POWER_8_5dBm

// Serial link to the Teensy (see ../xiao_link.ino for the protocol)
#define TEENSY_BAUD 2000000
#define TEENSY_RX_PIN 44 // D7, wired to Teensy pin 1 (TX1)
#define TEENSY_TX_PIN 43 // D6, wired to Teensy pin 0 (RX1)
#define TEENSY_CHUNK 4096 // must not exceed XIAO_MAX_CHUNK on the Teensy

// Sync timing [ms]
#define SYNC_INTERVAL 10000   // poll the Teensy this often when there is nothing to copy
#define SYNC_BUSY_RETRY 5000  // retry this soon when the Teensy is recording, playing etc.
#define TEENSY_ONLINE_TIMEOUT 30000 // show "no link" when the last answer is older than this

// microSD slot on the Sense expansion board
#define SD_CS_PIN 21
#define SD_FREQ 20000000

// Storage layout on the XIAO SD card
#define DATA_DIR "/guestbook"
#define INDEX_FILE DATA_DIR "/index.csv"
#define INDEX_TMP DATA_DIR "/index.tmp"

// Times are local wall-clock time stored as unix timestamps. Anything earlier than this means the
// Teensy clock was not set (without a VBAT battery it restarts from 2019-01-01 after power-up).
#define CLOCK_VALID_FROM 1767225600UL // 2026-01-01
// The browser time is only sent to the Teensy when the clocks differ by more than this [s]
#define CLOCK_TOLERANCE 60
