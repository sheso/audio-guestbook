#pragma once
#include <Arduino.h>
#include <mutex>
#include <vector>

// A recording that has been completely copied to the XIAO SD card
struct Recording {
  uint32_t num;   // 17 for 00017.wav on the Teensy
  uint32_t size;  // bytes
  uint32_t mtime; // local time as unix timestamp, see CLOCK_VALID_FROM
  uint32_t crc;   // CRC-32 of the whole file, needed for the ZIP download
};

// Index of copied recordings, kept in memory and in INDEX_FILE. Thread-safe: the sync task adds
// recordings while the web server reads them.
class Catalog {
public:
  // Loads the index and drops entries whose file is missing or has the wrong size
  void begin();
  // Adds .wav files found in DATA_DIR but missing from the index (e.g. the index was lost).
  // Reads each such file to compute its CRC, so it can take a while: call from the sync task.
  void adoptOrphans();

  std::vector<Recording> snapshot();   // sorted by number
  bool find(uint32_t num, Recording& rec);
  void add(const Recording& rec);      // adds or replaces, then saves the index
  void updateMtime(uint32_t num, uint32_t mtime);

  static String path(uint32_t num);      // /guestbook/00017.wav
  static String partPath(uint32_t num);  // /guestbook/00017.part, while copying
  // Name offered to the browser: 2026-10-03_18-45_00017.wav, or 00017.wav if the date is unknown
  static String downloadName(const Recording& rec);

private:
  std::mutex mutex;
  std::vector<Recording> recordings;

  void save(); // call with mutex held
  void insert(const Recording& rec); // call with mutex held
};

bool clockValid(uint32_t t);

extern Catalog catalog;
