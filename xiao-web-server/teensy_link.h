#pragma once
#include <Arduino.h>
#include <vector>

// Client side of the Teensy protocol (see ../xiao_link.ino). Not thread-safe: only the sync task uses it.

struct RemoteFile {
  uint32_t num;
  uint32_t size;
  uint32_t mtime;
};

enum class LinkResult {
  Ok,
  Busy,     // Teensy is recording, playing etc.
  Timeout,  // no (valid) answer
  Error,    // ERR reply other than BUSY, or a corrupted chunk
};

const char* linkResultName(LinkResult r);

class TeensyLink {
public:
  // Start from a random sequence number, so replies to requests sent before a XIAO reboot are ignored
  TeensyLink(Stream& port, uint32_t firstSeq) : port(port), seq(firstSeq) {}

  LinkResult ping(String& mode, uint32_t& teensyTime);
  LinkResult list(std::vector<RemoteFile>& files);
  // Reads up to len bytes at offset into buf; got is set to the number of bytes received
  LinkResult get(uint32_t num, uint32_t offset, uint8_t* buf, uint32_t len, uint32_t& got);
  LinkResult setTime(uint32_t t);

  // Last ERR reason, for logging
  const String& lastError() const { return error; }

private:
  Stream& port;
  uint32_t seq;
  String error;

  void send(const char* cmd, const String& args);
  // Waits for the next line with the current sequence number. DATA replies to older requests are
  // skipped together with their payload. Returns false on timeout.
  bool readReply(String& type, String& rest, uint32_t timeoutMs);
  bool readLine(String& line, uint32_t deadline);
  bool readBytes(uint8_t* buf, uint32_t len, uint32_t deadline);
  void skipBytes(uint32_t len, uint32_t deadline);
  LinkResult errorResult(const String& type, const String& rest);
};
