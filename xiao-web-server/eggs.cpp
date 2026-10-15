#include "eggs.h"
#include "config.h"
#include <SD.h>
#include <algorithm>
#include <map>
#include <mutex>

static std::mutex mtimeMutex;
static std::map<String, uint32_t> mtimes;

bool validEggNumber(const String& number) {
  if (number.length() == 0 || number.length() > 24) return false;
  for (char c : number) {
    if (!isdigit(c)) return false;
  }
  return true;
}

String eggPath(const String& number) {
  return EGGS_DIR "/" + number + ".wav";
}

String eggPartPath(const String& number) {
  return EGGS_DIR "/" + number + ".part";
}

std::vector<Egg> eggsList() {
  std::vector<Egg> eggs;
  File dir = SD.open(EGGS_DIR);
  if (!dir) return eggs;
  while (File f = dir.openNextFile()) {
    String name = f.name();
    name = name.substring(name.lastIndexOf('/') + 1);
    if (!f.isDirectory() && name.endsWith(".wav")) {
      String number = name.substring(0, name.length() - 4);
      if (validEggNumber(number)) eggs.push_back({number, (uint32_t)f.size(), 0});
    }
    f.close();
  }
  dir.close();
  {
    std::lock_guard<std::mutex> lock(mtimeMutex);
    for (Egg& e : eggs) {
      auto it = mtimes.find(e.number);
      if (it != mtimes.end()) e.mtime = it->second;
    }
  }
  // Shorter numbers first, so "67" comes before "312"
  std::sort(eggs.begin(), eggs.end(), [](const Egg& a, const Egg& b) {
    return a.number.length() != b.number.length() ? a.number.length() < b.number.length() : a.number < b.number;
  });
  return eggs;
}

bool eggFind(const String& number, Egg& egg) {
  if (!validEggNumber(number)) return false;
  File f = SD.open(eggPath(number), FILE_READ);
  if (!f) return false;
  egg = {number, (uint32_t)f.size(), 0};
  f.close();
  std::lock_guard<std::mutex> lock(mtimeMutex);
  auto it = mtimes.find(number);
  if (it != mtimes.end()) egg.mtime = it->second;
  return true;
}

void eggsSetMtime(const String& number, uint32_t mtime) {
  std::lock_guard<std::mutex> lock(mtimeMutex);
  mtimes[number] = mtime;
}
