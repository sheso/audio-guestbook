#include "sync.h"
#include "catalog.h"
#include "config.h"
#include "crc32.h"
#include "eggs.h"
#include "teensy_link.h"
#include <SD.h>
#include <atomic>
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
static std::atomic<bool> sdOk{false};
static uint32_t browserTime, browserTimeMs;

SyncStatus syncStatus() {
  std::lock_guard<std::mutex> lock(statusMutex);
  SyncStatus s = status;
  s.sdOk = sdOk;
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

// Copies a Teensy file (id as in TeensyLink::get) into its .part file, resuming a previous attempt,
// then moves it to dest. progressNum is the recording number shown on the page, or -1 for none.
static LinkResult copyFile(const String& id, uint32_t size, const String& part, const String& dest,
                           int32_t progressNum, uint32_t& crc) {
  uint32_t offset = 0;
  crc = 0;

  if (SD.exists(part)) {
    File f = SD.open(part, FILE_READ);
    if (f && f.size() <= size) {
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

  Serial.printf("Sync: copying %s from %lu of %lu bytes\n", id.c_str(), (unsigned long)offset, (unsigned long)size);
  uint32_t started = millis(), startOffset = offset;
  LinkResult r = LinkResult::Ok;
  int retries = 0;
  while (offset < size) {
    if (progressNum >= 0) setProgress(progressNum, offset, size);
    uint32_t got;
    r = teensy->get(id, offset, chunk, min((uint32_t)sizeof chunk, size - offset), got);
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
  if (progressNum >= 0) setProgress(0, 0, 0);

  if (offset < size) {
    // Busy is expected when a guest lifts the handset; the copy resumes later
    if (r != LinkResult::Busy) {
      setError("copying " + id + " stopped at " + offset + ": " + linkResultName(r) + " " + teensy->lastError());
    }
    return r == LinkResult::Ok ? LinkResult::Error : r;
  }

  SD.remove(dest);
  if (!SD.rename(part, dest)) {
    setError("cannot rename " + part);
    return LinkResult::Error;
  }
  uint32_t ms = max(millis() - started, (uint32_t)1);
  Serial.printf("Sync: %s done, %lu KB/s\n", id.c_str(), (unsigned long)((offset - startOffset) / ms));
  return LinkResult::Ok;
}

static LinkResult copyRecording(const RemoteFile& remote) {
  uint32_t crc;
  LinkResult r = copyFile(String(remote.num), remote.size, Catalog::partPath(remote.num), Catalog::path(remote.num),
                          remote.num, crc);
  if (r == LinkResult::Ok) catalog.add({remote.num, remote.size, remote.mtime, crc});
  return r;
}

// Copies easter eggs that are new or changed on the Teensy
static LinkResult syncEggs() {
  std::vector<RemoteEgg> eggs;
  LinkResult r = teensy->eggs(eggs);
  if (r != LinkResult::Ok) {
    if (r != LinkResult::Busy) setError(String("EGGS: ") + linkResultName(r) + " " + teensy->lastError());
    return r;
  }
  if (!SD.exists(EGGS_DIR)) SD.mkdir(EGGS_DIR);
  for (const RemoteEgg& e : eggs) {
    if (!validEggNumber(e.number)) continue;
    eggsSetMtime(e.number, e.mtime);
    Egg local;
    if (eggFind(e.number, local) && local.size == e.size) continue;
    uint32_t crc;
    r = copyFile("egg-" + e.number, e.size, eggPartPath(e.number), eggPath(e.number), -1, crc);
    if (r != LinkResult::Ok) return r;
  }
  return LinkResult::Ok;
}

// Appends what is new in the Teensy dial log. It only ever grows, so the local size is the offset.
static LinkResult syncDialLog() {
  File f = SD.open(DIAL_LOG_FILE, FILE_APPEND);
  if (!f) {
    setError("cannot write " DIAL_LOG_FILE);
    return LinkResult::Error;
  }
  uint32_t offset = f.size();
  LinkResult r;
  for (;;) {
    uint32_t got;
    r = teensy->get("dials", offset, chunk, sizeof chunk, got);
    if (r != LinkResult::Ok || got == 0) break;
    if (f.write(chunk, got) != got) {
      setError("SD write failed for " DIAL_LOG_FILE);
      r = LinkResult::Error;
      break;
    }
    offset += got;
  }
  f.close();
  if (r == LinkResult::Error && teensy->lastError() == "NOT_FOUND") return LinkResult::Ok; // nothing dialled yet
  if (r != LinkResult::Ok && r != LinkResult::Busy) {
    setError(String("dial log: ") + linkResultName(r) + " " + teensy->lastError());
  }
  return r;
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
  if (syncEggs() != LinkResult::Ok) return SYNC_BUSY_RETRY;
  if (syncDialLog() != LinkResult::Ok) return SYNC_BUSY_RETRY;

  std::lock_guard<std::mutex> lock(statusMutex);
  lastSyncMs = millis();
  everSynced = true;
  status.lastError = "";
  return SYNC_INTERVAL;
}

// Mounts the SD card, retrying until it works, and loads the index
static void mountSd() {
  while (!SD.begin(SD_CS_PIN, SPI, SD_FREQ)) {
    setError("cannot read the SD card of the XIAO, retrying");
    SD.end();
    delay(5000);
  }
  Serial.printf("SD card: %llu MB used of %llu MB\n", SD.usedBytes() >> 20, SD.totalBytes() >> 20);
  catalog.begin();
  sdOk = true;
  std::lock_guard<std::mutex> lock(statusMutex);
  status.lastError = "";
}

static void syncTask(void*) {
  mountSd();
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
