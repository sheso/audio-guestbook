#include "sync.h"
#include "catalog.h"
#include "config.h"
#include "crc32.h"
#include "teensy_link.h"
#include <SD.h>
#include <mutex>

#define CHUNK_RETRIES 3

static TeensyLink* teensy;
static TaskHandle_t task;
static uint8_t chunk[TEENSY_CHUNK];

static std::mutex statusMutex;
static SyncStatus status;
static uint32_t lastPongMs, lastPongTime; // Teensy clock at the last PONG
static uint32_t lastSyncMs;
static bool everSynced = false;
static bool browserTimePending = false;
static uint32_t browserTime, browserTimeMs;

SyncStatus syncStatus() {
  std::lock_guard<std::mutex> lock(statusMutex);
  SyncStatus s = status;
  uint32_t now = millis();
  s.teensyOnline = lastPongMs != 0 && now - lastPongMs < TEENSY_ONLINE_TIMEOUT;
  s.teensyTime = lastPongMs ? lastPongTime + (now - lastPongMs) / 1000 : 0;
  s.lastSyncAgo = everSynced ? (now - lastSyncMs) / 1000 : -1;
  return s;
}

void syncSetBrowserTime(uint32_t t) {
  {
    std::lock_guard<std::mutex> lock(statusMutex);
    browserTime = t;
    browserTimeMs = millis();
    browserTimePending = true;
  }
  syncNow();
}

void syncNow() {
  if (task) xTaskNotifyGive(task);
}

void syncSetTrace(bool on) {
  if (teensy) teensy->trace = on;
}

static void setError(const String& e) {
  Serial.println("Sync: " + e);
  std::lock_guard<std::mutex> lock(statusMutex);
  status.lastError = e;
}

static void setProgress(uint32_t num, uint32_t done, uint32_t size) {
  std::lock_guard<std::mutex> lock(statusMutex);
  status.copyingNum = num;
  status.copyingDone = done;
  status.copyingSize = size;
}

// Sends the browser time to the Teensy if its clock is wrong
static void updateTeensyClock(uint32_t teensyTime) {
  uint32_t t;
  {
    std::lock_guard<std::mutex> lock(statusMutex);
    if (!browserTimePending) return;
    browserTimePending = false;
    t = browserTime + (millis() - browserTimeMs) / 1000;
  }
  if (abs((int32_t)(t - teensyTime)) <= CLOCK_TOLERANCE) return;
  LinkResult r = teensy->setTime(t);
  if (r == LinkResult::Ok) {
    Serial.printf("Sync: Teensy clock set to %lu (was %lu)\n", (unsigned long)t, (unsigned long)teensyTime);
    std::lock_guard<std::mutex> lock(statusMutex);
    lastPongTime = t;
    lastPongMs = millis();
  }
  else {
    setError(String("setting the Teensy clock failed: ") + linkResultName(r) + " " + teensy->lastError());
  }
}

// Copies one recording into its .part file, resuming a previous attempt, then moves it in place
static LinkResult copyRecording(const RemoteFile& remote) {
  String part = Catalog::partPath(remote.num);
  uint32_t offset = 0, crc = 0;

  if (SD.exists(part)) {
    File f = SD.open(part, FILE_READ);
    if (f && f.size() <= remote.size) {
      int n;
      while ((n = f.read(chunk, sizeof chunk)) > 0) crc = crc32Update(crc, chunk, n);
      offset = f.size();
    }
    f.close();
    if (offset == 0) SD.remove(part);
  }
  File out = SD.open(part, offset ? FILE_APPEND : FILE_WRITE);
  if (!out) {
    setError("cannot write " + part);
    return LinkResult::Error;
  }

  Serial.printf("Sync: copying %05lu.wav from %lu of %lu bytes\n", (unsigned long)remote.num,
                (unsigned long)offset, (unsigned long)remote.size);
  uint32_t started = millis(), startOffset = offset;
  LinkResult r = LinkResult::Ok;
  int retries = 0;
  while (offset < remote.size) {
    setProgress(remote.num, offset, remote.size);
    uint32_t got;
    r = teensy->get(remote.num, offset, chunk, min((uint32_t)sizeof chunk, remote.size - offset), got);
    if (r == LinkResult::Ok && got == 0) {
      r = LinkResult::Error; // the file got shorter since LIST
    }
    if (r != LinkResult::Ok) {
      if (r != LinkResult::Busy && ++retries <= CHUNK_RETRIES) continue;
      break;
    }
    retries = 0;
    if (out.write(chunk, got) != got) {
      setError("SD write failed for " + part);
      r = LinkResult::Error;
      break;
    }
    crc = crc32Update(crc, chunk, got);
    offset += got;
  }
  out.close();
  setProgress(0, 0, 0);

  if (offset < remote.size) {
    // Busy is expected when a guest lifts the handset; the copy resumes later
    if (r != LinkResult::Busy) {
      setError(String("copying ") + remote.num + " stopped at " + offset + ": " + linkResultName(r) +
               " " + teensy->lastError());
    }
    return r == LinkResult::Ok ? LinkResult::Error : r;
  }

  String wav = Catalog::path(remote.num);
  SD.remove(wav);
  if (!SD.rename(part, wav)) {
    setError("cannot rename " + part);
    return LinkResult::Error;
  }
  catalog.add({remote.num, remote.size, remote.mtime, crc});
  uint32_t ms = max(millis() - started, (uint32_t)1);
  Serial.printf("Sync: %05lu.wav done, %lu KB/s\n", (unsigned long)remote.num,
                (unsigned long)((offset - startOffset) / ms));
  return LinkResult::Ok;
}

// One sync cycle; returns the time to wait before the next one [ms]
static uint32_t syncCycle() {
  String mode;
  uint32_t teensyTime;
  LinkResult r = teensy->ping(mode, teensyTime);
  if (r != LinkResult::Ok) {
    // A timeout is normal while the Teensy plays the greeting or a recording
    if (r != LinkResult::Timeout) setError(String("PING: ") + linkResultName(r) + " " + teensy->lastError());
    return SYNC_BUSY_RETRY;
  }
  {
    std::lock_guard<std::mutex> lock(statusMutex);
    lastPongMs = millis();
    lastPongTime = teensyTime;
    status.teensyMode = mode;
  }
  updateTeensyClock(teensyTime);
  if (mode != "Ready") return SYNC_BUSY_RETRY;

  std::vector<RemoteFile> files;
  r = teensy->list(files);
  if (r != LinkResult::Ok) {
    if (r != LinkResult::Busy) setError(String("LIST: ") + linkResultName(r) + " " + teensy->lastError());
    return SYNC_BUSY_RETRY;
  }

  std::vector<RemoteFile> todo;
  for (const RemoteFile& f : files) {
    Recording rec;
    if (catalog.find(f.num, rec) && rec.size == f.size) catalog.updateMtime(f.num, f.mtime);
    else todo.push_back(f);
  }
  int pending = todo.size();
  {
    std::lock_guard<std::mutex> lock(statusMutex);
    if (status.teensyFiles != (int)files.size() || pending) {
      Serial.printf("Sync: Teensy has %u recordings, %d to copy\n", (unsigned)files.size(), pending);
    }
    status.teensyFiles = files.size();
    status.pending = pending;
  }

  for (const RemoteFile& f : todo) {
    if (copyRecording(f) != LinkResult::Ok) return SYNC_BUSY_RETRY;
    std::lock_guard<std::mutex> lock(statusMutex);
    status.pending = --pending;
  }

  std::lock_guard<std::mutex> lock(statusMutex);
  lastSyncMs = millis();
  everSynced = true;
  status.lastError = "";
  return SYNC_INTERVAL;
}

static void syncTask(void*) {
  catalog.adoptOrphans();
  for (;;) {
    uint32_t wait = syncCycle();
    // Sleep until the next cycle, or until syncNow() wakes us up
    ulTaskNotifyTake(pdTRUE, pdMS_TO_TICKS(wait));
  }
}

void syncBegin() {
  Serial1.setRxBufferSize(2 * TEENSY_CHUNK + 1024);
  Serial1.begin(TEENSY_BAUD, SERIAL_8N1, TEENSY_RX_PIN, TEENSY_TX_PIN);
  teensy = new TeensyLink(Serial1, esp_random());
  xTaskCreatePinnedToCore(syncTask, "sync", 8192, nullptr, 1, &task, 1);
}
