#pragma once
#include <stddef.h>
#include <stdint.h>

// Standard CRC-32, same as zlib's crc32() and the one used in ZIP files:
// start with crc = 0, or pass the previous result to continue a running CRC
inline uint32_t crc32Update(uint32_t crc, const uint8_t* data, size_t len) {
  static uint32_t table[256];
  if (table[1] == 0) {
    for (uint32_t i = 0; i < 256; i++) {
      uint32_t c = i;
      for (int k = 0; k < 8; k++) c = (c & 1) ? 0xEDB88320 ^ (c >> 1) : c >> 1;
      table[i] = c;
    }
  }
  crc = ~crc;
  while (len--) crc = table[(crc ^ *data++) & 0xFF] ^ (crc >> 8);
  return ~crc;
}
