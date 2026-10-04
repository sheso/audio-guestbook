#include "catalog.h"
#include "config.h"
#include "crc32.h"
#include <SD.h>
#include <algorithm>
#include <time.h>

Catalog catalog;

bool clockValid(uint32_t t) {
  return t >= CLOCK_VALID_FROM;
}

String Catalog::path(uint32_t num) {
  char buf[32];
  snprintf(buf, sizeof buf, DATA_DIR "/%05lu.wav", (unsigned long)num);
  return buf;
}

String Catalog::partPath(uint32_t num) {
  char buf[32];
  snprintf(buf, sizeof buf, DATA_DIR "/%05lu.part", (unsigned long)num);
  return buf;
}

String Catalog::downloadName(const Recording& rec) {
  char buf[40];
  if (clockValid(rec.mtime)) {
    // mtime already is local time, so format it without any timezone conversion
    time_t t = rec.mtime;
    struct tm tm;
    gmtime_r(&t, &tm);
    snprintf(buf, sizeof buf, "%04d-%02d-%02d_%02d-%02d_%05lu.wav", tm.tm_year + 1900, tm.tm_mon + 1,
             tm.tm_mday, tm.tm_hour, tm.tm_min, (unsigned long)rec.num);
  }
  else {
    snprintf(buf, sizeof buf, "%05lu.wav", (unsigned long)rec.num);
  }
  return buf;
}

void Catalog::begin() {
  std::lock_guard<std::mutex> lock(mutex);
  recordings.clear();
  if (!SD.exists(DATA_DIR)) SD.mkdir(DATA_DIR);
  // Power was cut in save() between removing the old index and renaming the new one
  if (!SD.exists(INDEX_FILE) && SD.exists(INDEX_TMP)) SD.rename(INDEX_TMP, INDEX_FILE);

  File f = SD.open(INDEX_FILE, FILE_READ);
  bool dropped = false;
  if (f) {
    while (f.available()) {
      String line = f.readStringUntil('\n');
      unsigned long num, size, mtime, crc;
      if (sscanf(line.c_str(), "%lu,%lu,%lu,%lx", &num, &size, &mtime, &crc) != 4) continue;
      File wav = SD.open(path(num), FILE_READ);
      if (wav && wav.size() == size) {
        insert({(uint32_t)num, (uint32_t)size, (uint32_t)mtime, (uint32_t)crc});
      }
      else {
        Serial.printf("Index: %05lu.wav is missing or has the wrong size, will copy it again\n", num);
        dropped = true;
      }
    }
    f.close();
  }
  if (dropped) save();
  Serial.printf("Index: %u recordings\n", (unsigned)recordings.size());
}

void Catalog::adoptOrphans() {
  std::vector<uint32_t> orphans;
  {
    std::lock_guard<std::mutex> lock(mutex);
    File dir = SD.open(DATA_DIR);
    while (File f = dir.openNextFile()) {
      String name = f.name();
      name = name.substring(name.lastIndexOf('/') + 1);
      unsigned long num;
      char ext[5] = "";
      if (name.length() == 9 && sscanf(name.c_str(), "%5lu.%3s", &num, ext) == 2 && strcmp(ext, "wav") == 0) {
        bool known = std::any_of(recordings.begin(), recordings.end(),
                                 [&](const Recording& r) { return r.num == num; });
        if (!known) orphans.push_back(num);
      }
      f.close();
    }
    dir.close();
  }

  static uint8_t buf[4096];
  for (uint32_t num : orphans) {
    File f = SD.open(path(num), FILE_READ);
    if (!f) continue;
    uint32_t crc = 0;
    int n;
    while ((n = f.read(buf, sizeof buf)) > 0) crc = crc32Update(crc, buf, n);
    Recording rec = {num, (uint32_t)f.size(), 0, crc}; // the date comes with the next LIST
    f.close();
    Serial.printf("Index: adopted %05lu.wav\n", (unsigned long)num);
    add(rec);
  }
}

std::vector<Recording> Catalog::snapshot() {
  std::lock_guard<std::mutex> lock(mutex);
  return recordings;
}

bool Catalog::find(uint32_t num, Recording& rec) {
  std::lock_guard<std::mutex> lock(mutex);
  for (const Recording& r : recordings) {
    if (r.num == num) {
      rec = r;
      return true;
    }
  }
  return false;
}

void Catalog::add(const Recording& rec) {
  std::lock_guard<std::mutex> lock(mutex);
  insert(rec);
  save();
}

void Catalog::updateMtime(uint32_t num, uint32_t mtime) {
  std::lock_guard<std::mutex> lock(mutex);
  for (Recording& r : recordings) {
    if (r.num == num && r.mtime != mtime) {
      r.mtime = mtime;
      save();
    }
  }
}

void Catalog::insert(const Recording& rec) {
  auto it = std::lower_bound(recordings.begin(), recordings.end(), rec.num,
                             [](const Recording& r, uint32_t num) { return r.num < num; });
  if (it != recordings.end() && it->num == rec.num) *it = rec;
  else recordings.insert(it, rec);
}

void Catalog::save() {
  // Write a new file and swap it in, so a power cut never leaves a half-written index (see begin())
  File f = SD.open(INDEX_TMP, FILE_WRITE);
  if (!f) {
    Serial.println("Index: cannot write " INDEX_TMP);
    return;
  }
  for (const Recording& r : recordings) {
    f.printf("%lu,%lu,%lu,%08lx\n", (unsigned long)r.num, (unsigned long)r.size,
             (unsigned long)r.mtime, (unsigned long)r.crc);
  }
  f.close();
  SD.remove(INDEX_FILE);
  SD.rename(INDEX_TMP, INDEX_FILE);
}
