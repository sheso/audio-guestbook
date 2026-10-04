#pragma once
#include <Arduino.h>

struct SyncStatus {
  bool teensyOnline = false;  // answered within TEENSY_ONLINE_TIMEOUT
  String teensyMode;          // Ready, Recording, ...
  uint32_t teensyTime = 0;    // Teensy clock now (estimated from the last PONG)
  int32_t lastSyncAgo = -1;   // [s] since the last complete sync, -1 = never
  int pending = 0;            // recordings on the Teensy not yet copied
  uint32_t copyingNum = 0;    // recording being copied right now, if copyingSize > 0
  uint32_t copyingDone = 0;
  uint32_t copyingSize = 0;
  String lastError;
};

// Starts the background task that copies new recordings from the Teensy
void syncBegin();
SyncStatus syncStatus();
// Time from a browser (local time as unix timestamp). The Teensy clock is set from it when it is off.
void syncSetBrowserTime(uint32_t t);
// Run a sync cycle now instead of waiting for the next one
void syncNow();
