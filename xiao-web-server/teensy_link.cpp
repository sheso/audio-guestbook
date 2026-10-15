#include "teensy_link.h"
#include "config.h"
#include "crc32.h"

#define REPLY_LINE_MAX 128
#define REPLY_TIMEOUT 500   // [ms] PING, TIME and the DATA header
#define LIST_LINE_TIMEOUT 2000 // [ms] between LIST lines (the Teensy reads its SD directory meanwhile)

const char* linkResultName(LinkResult r) {
  switch (r) {
    case LinkResult::Ok:      return "ok";
    case LinkResult::Busy:    return "busy";
    case LinkResult::Timeout: return "timeout";
    case LinkResult::Error:   return "error";
  }
  return "?";
}

void TeensyLink::send(const char* cmd, const String& args) {
  // Anything still waiting is a late answer to a request we gave up on
  while (port.available()) port.read();
  seq++;
  String line = String(cmd) + " " + String(seq);
  if (args.length()) line += " " + args;
  line += "\n";
  if (trace) Serial.print(">> " + line);
  port.write((const uint8_t*)line.c_str(), line.length());
}

bool TeensyLink::readLine(String& line, uint32_t deadline) {
  line = "";
  bool overflow = false;
  while ((int32_t)(deadline - millis()) > 0) {
    int c = port.read();
    if (c < 0) {
      delay(1);
      continue;
    }
    if (c == '\r') continue;
    if (c == '\n') {
      if (!overflow) return true;
      line = ""; // binary leftovers or noise, keep looking
      overflow = false;
      continue;
    }
    if (line.length() < REPLY_LINE_MAX) line += (char)c;
    else overflow = true;
  }
  return false;
}

bool TeensyLink::readBytes(uint8_t* buf, uint32_t len, uint32_t deadline) {
  uint32_t got = 0;
  while (got < len) {
    int n = port.available();
    if (n > 0) {
      got += port.readBytes(buf + got, min((uint32_t)n, len - got));
    }
    else if ((int32_t)(deadline - millis()) <= 0) {
      return false;
    }
    else {
      delay(1);
    }
  }
  return true;
}

void TeensyLink::skipBytes(uint32_t len, uint32_t deadline) {
  while (len > 0 && (int32_t)(deadline - millis()) > 0) {
    if (port.read() >= 0) len--;
    else delay(1);
  }
}

bool TeensyLink::readReply(String& type, String& rest, uint32_t timeoutMs) {
  uint32_t deadline = millis() + timeoutMs;
  String line;
  while (readLine(line, deadline)) {
    if (trace) Serial.println("<< " + line);
    char t[8];
    unsigned long replySeq;
    int consumed = 0;
    if (sscanf(line.c_str(), "%7s %lu%n", t, &replySeq, &consumed) != 2) continue;
    const char* r = line.c_str() + consumed;
    while (*r == ' ') r++;
    if (replySeq != seq) {
      // Stale reply: a DATA header is followed by its payload, which must not be taken for lines
      unsigned long offset, len;
      if (strcmp(t, "DATA") == 0 && sscanf(r, "%*s %lu %lu", &offset, &len) == 2 && len <= TEENSY_CHUNK) {
        skipBytes(len, millis() + REPLY_TIMEOUT);
      }
      continue;
    }
    type = t;
    rest = r;
    return true;
  }
  if (trace) Serial.printf("<< (no reply in %lu ms, %d bytes waiting)\n", (unsigned long)timeoutMs, port.available());
  return false;
}

LinkResult TeensyLink::errorResult(const String& type, const String& rest) {
  error = type == "ERR" ? rest : "unexpected " + type + " " + rest;
  return rest == "BUSY" ? LinkResult::Busy : LinkResult::Error;
}

LinkResult TeensyLink::ping(String& mode, uint32_t& teensyTime) {
  send("PING", "");
  String type, rest;
  if (!readReply(type, rest, REPLY_TIMEOUT)) return LinkResult::Timeout;
  char m[16];
  unsigned long t;
  if (type != "PONG" || sscanf(rest.c_str(), "%15s %lu", m, &t) != 2) return errorResult(type, rest);
  mode = m;
  teensyTime = t;
  return LinkResult::Ok;
}

LinkResult TeensyLink::list(std::vector<RemoteFile>& files) {
  files.clear();
  send("LIST", "");
  String type, rest;
  while (readReply(type, rest, LIST_LINE_TIMEOUT)) {
    unsigned long num, size, mtime, count;
    if (type == "FILE" && sscanf(rest.c_str(), "%lu %lu %lu", &num, &size, &mtime) == 3) {
      files.push_back({(uint32_t)num, (uint32_t)size, (uint32_t)mtime});
    }
    else if (type == "END" && sscanf(rest.c_str(), "%lu", &count) == 1) {
      if (count == files.size()) return LinkResult::Ok;
      error = "LIST lost lines: got " + String(files.size()) + " of " + String(count);
      return LinkResult::Error;
    }
    else {
      return errorResult(type, rest);
    }
  }
  return LinkResult::Timeout;
}

LinkResult TeensyLink::eggs(std::vector<RemoteEgg>& eggs) {
  eggs.clear();
  send("EGGS", "");
  String type, rest;
  while (readReply(type, rest, LIST_LINE_TIMEOUT)) {
    char number[32];
    unsigned long size, mtime, count;
    if (type == "EGG" && sscanf(rest.c_str(), "%31s %lu %lu", number, &size, &mtime) == 3) {
      eggs.push_back({number, (uint32_t)size, (uint32_t)mtime});
    }
    else if (type == "END" && sscanf(rest.c_str(), "%lu", &count) == 1) {
      if (count == eggs.size()) return LinkResult::Ok;
      error = "EGGS lost lines: got " + String(eggs.size()) + " of " + String(count);
      return LinkResult::Error;
    }
    else {
      return errorResult(type, rest);
    }
  }
  return LinkResult::Timeout;
}

LinkResult TeensyLink::get(const String& id, uint32_t offset, uint8_t* buf, uint32_t len, uint32_t& got) {
  got = 0;
  send("GET", id + " " + String(offset) + " " + String(len));
  String type, rest;
  if (!readReply(type, rest, REPLY_TIMEOUT)) return LinkResult::Timeout;
  char rId[32];
  unsigned long rOffset, rLen, rCrc;
  if (type != "DATA" || sscanf(rest.c_str(), "%31s %lu %lu %lx", rId, &rOffset, &rLen, &rCrc) != 4) {
    return errorResult(type, rest);
  }
  if (id != rId || rOffset != offset || rLen > len) {
    error = "DATA does not match the request: " + rest;
    return LinkResult::Error;
  }
  // ~5us per byte at 2Mbaud, plus generous slack
  if (!readBytes(buf, rLen, millis() + REPLY_TIMEOUT + rLen / 100)) return LinkResult::Timeout;
  if (crc32Update(0, buf, rLen) != rCrc) {
    error = "CRC mismatch at offset " + String(offset);
    return LinkResult::Error;
  }
  got = rLen;
  return LinkResult::Ok;
}

LinkResult TeensyLink::setTime(uint32_t t) {
  send("TIME", String(t));
  String type, rest;
  if (!readReply(type, rest, REPLY_TIMEOUT)) return LinkResult::Timeout;
  if (type != "OK") return errorResult(type, rest);
  return LinkResult::Ok;
}
