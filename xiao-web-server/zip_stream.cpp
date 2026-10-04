#include "zip_stream.h"
#include <algorithm>
#include <string.h>
#include <time.h>

static void put16(std::string& s, uint16_t v) {
  s += (char)(v & 0xFF);
  s += (char)(v >> 8);
}

static void put32(std::string& s, uint32_t v) {
  put16(s, v & 0xFFFF);
  put16(s, v >> 16);
}

// MS-DOS date and time as used in ZIP headers; 1980-01-01 00:00 when the time is unknown
static void dosTime(uint32_t mtime, uint16_t& date, uint16_t& time) {
  date = (1 << 5) | 1;
  time = 0;
  if (mtime == 0) return;
  time_t t = mtime;
  struct tm tm;
  gmtime_r(&t, &tm); // mtime already is local time
  if (tm.tm_year < 80) return;
  date = ((tm.tm_year - 80) << 9) | ((tm.tm_mon + 1) << 5) | tm.tm_mday;
  time = (tm.tm_hour << 11) | (tm.tm_min << 5) | (tm.tm_sec / 2);
}

ZipStream::ZipStream(const std::vector<ZipEntry>& entries, Reader reader) : reader(reader) {
  std::string central;
  for (size_t i = 0; i < entries.size(); i++) {
    const ZipEntry& e = entries[i];
    uint16_t date, time;
    dosTime(e.mtime, date, time);
    uint32_t localOffset = total;

    std::string local;
    put32(local, 0x04034b50); // local file header signature
    put16(local, 10);         // version needed: 1.0, stored
    put16(local, 0);          // flags
    put16(local, 0);          // method: stored
    put16(local, time);
    put16(local, date);
    put32(local, e.crc);
    put32(local, e.size);     // compressed size
    put32(local, e.size);     // uncompressed size
    put16(local, e.name.size());
    put16(local, 0);          // extra field length
    local += e.name;
    addBytes(local);
    segments.push_back({total, e.size, (int)i, ""});
    total += e.size;

    put32(central, 0x02014b50); // central directory header signature
    put16(central, 20);         // version made by
    put16(central, 10);         // version needed
    put16(central, 0);          // flags
    put16(central, 0);          // method
    put16(central, time);
    put16(central, date);
    put32(central, e.crc);
    put32(central, e.size);
    put32(central, e.size);
    put16(central, e.name.size());
    put16(central, 0);          // extra field length
    put16(central, 0);          // comment length
    put16(central, 0);          // disk number
    put16(central, 0);          // internal attributes
    put32(central, 0);          // external attributes
    put32(central, localOffset);
    central += e.name;
  }

  uint32_t centralOffset = total;
  uint32_t centralSize = central.size();
  addBytes(central);

  std::string end;
  put32(end, 0x06054b50); // end of central directory signature
  put16(end, 0);          // this disk
  put16(end, 0);          // disk with the central directory
  put16(end, entries.size());
  put16(end, entries.size());
  put32(end, centralSize);
  put32(end, centralOffset);
  put16(end, 0);          // comment length
  addBytes(end);
}

void ZipStream::addBytes(std::string bytes) {
  size_t len = bytes.size();
  segments.push_back({total, len, -1, std::move(bytes)});
  total += len;
}

size_t ZipStream::read(size_t pos, uint8_t* buf, size_t len) {
  size_t done = 0;
  // First segment starting after pos, minus one
  auto it = std::upper_bound(segments.begin(), segments.end(), pos,
                             [](size_t p, const Segment& s) { return p < s.start; });
  if (it == segments.begin()) return 0;
  --it;
  while (done < len && it != segments.end()) {
    size_t inSeg = pos + done - it->start;
    if (inSeg >= it->len) {
      ++it;
      continue;
    }
    size_t n = std::min(len - done, it->len - inSeg);
    if (it->entry < 0) {
      memcpy(buf + done, it->bytes.data() + inSeg, n);
    }
    else {
      size_t got = reader(it->entry, inSeg, buf + done, n);
      done += got;
      if (got < n) return done;
      continue;
    }
    done += n;
  }
  return done;
}
