#pragma once
#include <stddef.h>
#include <stdint.h>
#include <functional>
#include <string>
#include <vector>

// Uncompressed ("stored") ZIP archive produced on the fly. The CRCs and sizes of all files are known
// in advance, so the total size is known before the first byte is sent and any part of the archive
// can be read without holding the files in memory. Archives must stay below 4 GB (no ZIP64).
// Plain C++ without Arduino dependencies, so it can be tested on a computer.

struct ZipEntry {
  std::string name;
  uint32_t size;
  uint32_t crc;
  uint32_t mtime; // local time as unix timestamp, 0 if unknown
};

class ZipStream {
public:
  // Reads len bytes of entry number `entry` starting at offset; returns the number of bytes read
  using Reader = std::function<size_t(size_t entry, uint32_t offset, uint8_t* buf, size_t len)>;

  ZipStream(const std::vector<ZipEntry>& entries, Reader reader);

  size_t size() const { return total; }
  // Copies up to len bytes of the archive starting at pos; returns fewer only at the end or when
  // the reader fails
  size_t read(size_t pos, uint8_t* buf, size_t len);

private:
  struct Segment {
    size_t start;
    size_t len;
    int entry;          // file data of this entry, or -1 for the bytes below
    std::string bytes;  // headers
  };
  std::vector<Segment> segments;
  Reader reader;
  size_t total = 0;

  void addBytes(std::string bytes);
};
