#include "web.h"
#include "catalog.h"
#include "config.h"
#include "sync.h"
#include "web_page.h"
#include "zip_stream.h"
#include <ESPAsyncWebServer.h>
#include <SD.h>
#include <memory>

static AsyncWebServer server(80);
static bool requestLog = false;
static uint32_t requestCount = 0, lastRequestMs = 0; // written only by the async_tcp task
static IPAddress lastClient;

void webSetLog(bool on) {
  requestLog = on;
}

String webStats() {
  if (!requestCount) return "HTTP: no requests yet";
  return "HTTP: " + String(requestCount) + " requests, last " + String((millis() - lastRequestMs) / 1000) +
         " s ago from " + lastClient.toString();
}

static String jsonEscape(const String& s) {
  String out;
  for (char c : s) {
    if (c == '"' || c == '\\') out += '\\';
    if ((uint8_t)c >= 0x20) out += c;
  }
  return out;
}

static void handleStatus(AsyncWebServerRequest* req) {
  SyncStatus s = syncStatus();
  String json = "{";
  json += "\"teensyOnline\":" + String(s.teensyOnline ? "true" : "false");
  json += ",\"teensyMode\":\"" + jsonEscape(s.teensyMode) + "\"";
  json += ",\"clockValid\":" + String(clockValid(s.teensyTime) ? "true" : "false");
  json += ",\"lastSyncAgo\":" + String(s.lastSyncAgo);
  json += ",\"teensyFiles\":" + String(s.teensyFiles);
  json += ",\"pending\":" + String(s.pending);
  json += ",\"copyingNum\":" + String(s.copyingNum);
  json += ",\"copyingDone\":" + String(s.copyingDone);
  json += ",\"copyingSize\":" + String(s.copyingSize);
  json += ",\"lastError\":\"" + jsonEscape(s.lastError) + "\"";
  json += ",\"sdTotal\":" + String((double)SD.totalBytes(), 0);
  json += ",\"sdUsed\":" + String((double)SD.usedBytes(), 0);
  json += "}";
  req->send(200, "application/json", json);
}

static void handleRecordings(AsyncWebServerRequest* req) {
  std::vector<Recording> recs = catalog.snapshot();
  String json;
  json.reserve(recs.size() * 80 + 2);
  json = "[";
  for (size_t i = 0; i < recs.size(); i++) {
    const Recording& r = recs[i];
    if (i) json += ",";
    json += "{\"n\":" + String(r.num) + ",\"size\":" + String(r.size) +
            ",\"t\":" + String(clockValid(r.mtime) ? r.mtime : 0) +
            ",\"name\":\"" + Catalog::downloadName(r) + "\"}";
  }
  json += "]";
  req->send(200, "application/json", json);
}

static void handleTime(AsyncWebServerRequest* req) {
  if (!req->hasParam("t")) {
    req->send(400, "text/plain", "missing t");
    return;
  }
  syncSetBrowserTime(strtoul(req->getParam("t")->value().c_str(), nullptr, 10));
  req->send(204);
}

// Parses "bytes=a-b", "bytes=a-" and "bytes=-n"; returns false if the range is invalid
static bool parseRange(const String& header, uint32_t size, uint32_t& start, uint32_t& end) {
  if (!header.startsWith("bytes=") || size == 0) return false;
  String spec = header.substring(6);
  int dash = spec.indexOf('-');
  if (dash < 0 || spec.indexOf(',') >= 0) return false;
  String a = spec.substring(0, dash), b = spec.substring(dash + 1);
  if (a.length() == 0) {
    uint32_t n = b.toInt();
    if (n == 0) return false;
    start = n >= size ? 0 : size - n;
    end = size - 1;
  }
  else {
    start = a.toInt();
    end = b.length() ? (uint32_t)b.toInt() : size - 1;
    if (end >= size) end = size - 1;
  }
  return start <= end && start < size;
}

#define WAV_HEADER_SIZE 44

// The Teensy sketch used to store a "data" chunk size 2 bytes larger than the file, and Safari stops
// with an error at the end of such a file. Returns the header with both sizes matching the file.
static std::shared_ptr<uint8_t[]> fixedWavHeader(File& file, uint32_t size) {
  std::shared_ptr<uint8_t[]> h(new uint8_t[WAV_HEADER_SIZE]);
  if (size < WAV_HEADER_SIZE || file.read(h.get(), WAV_HEADER_SIZE) != WAV_HEADER_SIZE ||
      memcmp(h.get(), "RIFF", 4) || memcmp(h.get() + 8, "WAVE", 4) || memcmp(h.get() + 36, "data", 4)) {
    return nullptr;
  }
  auto put32 = [&](int i, uint32_t v) {
    for (int b = 0; b < 4; b++) h[i + b] = v >> (8 * b);
  };
  put32(4, size - 8);
  put32(40, size - WAV_HEADER_SIZE);
  return h;
}

// Serves one recording, with Range support (Safari needs it to play and seek audio)
static void handleRecording(AsyncWebServerRequest* req) {
  Recording rec;
  if (!req->hasParam("n") || !catalog.find(req->getParam("n")->value().toInt(), rec)) {
    req->send(404, "text/plain", "no such recording");
    return;
  }
  auto file = std::make_shared<File>(SD.open(Catalog::path(rec.num), FILE_READ));
  if (!*file) {
    req->send(500, "text/plain", "cannot open the file");
    return;
  }
  std::shared_ptr<uint8_t[]> header = fixedWavHeader(*file, rec.size);

  uint32_t start = 0, end = rec.size - 1;
  bool partial = false;
  if (req->hasHeader("Range")) {
    if (!parseRange(req->getHeader("Range")->value(), rec.size, start, end)) {
      AsyncWebServerResponse* res = req->beginResponse(416, "text/plain", "bad range");
      res->addHeader("Content-Range", "bytes */" + String(rec.size));
      req->send(res);
      return;
    }
    partial = true;
  }
  uint32_t len = rec.size ? end - start + 1 : 0;

  auto sent = std::make_shared<uint32_t>(0);
  if (requestLog) {
    Serial.printf("HTTP   rec %lu, %s, bytes %lu-%lu of %lu\n", (unsigned long)rec.num,
                  partial ? req->getHeader("Range")->value().c_str() : "no range", (unsigned long)start,
                  (unsigned long)end, (unsigned long)rec.size);
    uint32_t num = rec.num, started = millis();
    req->onDisconnect([sent, num, len, started]() {
      Serial.printf("HTTP   rec %lu: closed after %lu of %lu bytes, %lu ms\n", (unsigned long)num,
                    (unsigned long)*sent, (unsigned long)len, (unsigned long)(millis() - started));
    });
  }
  AsyncWebServerResponse* res = req->beginResponse(
      "audio/wav", len, [file, start, sent, header](uint8_t* buf, size_t maxLen, size_t index) -> size_t {
        uint32_t pos = start + index;
        if (file->position() != pos) file->seek(pos);
        int n = file->read(buf, maxLen);
        if (n <= 0) return 0;
        if (header) {
          for (uint32_t i = pos; i < WAV_HEADER_SIZE && i < pos + n; i++) buf[i - pos] = header[i];
        }
        *sent = index + n;
        return n;
      });
  res->addHeader("Accept-Ranges", "bytes");
  if (partial) {
    res->setCode(206);
    res->addHeader("Content-Range", "bytes " + String(start) + "-" + String(end) + "/" + String(rec.size));
  }
  if (req->hasParam("dl")) {
    res->addHeader("Content-Disposition", "attachment; filename=\"" + Catalog::downloadName(rec) + "\"");
  }
  req->send(res);
}

// All copied recordings as one uncompressed ZIP, generated while it is being sent
static void handleZip(AsyncWebServerRequest* req) {
  struct ZipState {
    std::vector<Recording> recs;
    std::unique_ptr<ZipStream> zip;
    int openEntry = -1;
    File file;
  };
  auto state = std::make_shared<ZipState>();
  state->recs = catalog.snapshot();
  std::vector<ZipEntry> entries;
  for (const Recording& r : state->recs) {
    entries.push_back({Catalog::downloadName(r).c_str(), r.size, r.crc, clockValid(r.mtime) ? r.mtime : 0});
  }
  ZipState* s = state.get(); // the reader lives inside the state, so it must not hold a shared_ptr to it
  state->zip.reset(new ZipStream(entries, [s](size_t entry, uint32_t offset, uint8_t* buf, size_t len) -> size_t {
    if (s->openEntry != (int)entry) {
      s->file.close();
      s->file = SD.open(Catalog::path(s->recs[entry].num), FILE_READ);
      s->openEntry = entry;
    }
    if (!s->file) return 0;
    if (s->file.position() != offset) s->file.seek(offset);
    int n = s->file.read(buf, len);
    return n > 0 ? n : 0;
  }));

  AsyncWebServerResponse* res = req->beginResponse(
      "application/zip", state->zip->size(), [state](uint8_t* buf, size_t maxLen, size_t index) -> size_t {
        return state->zip->read(index, buf, maxLen);
      });
  res->addHeader("Content-Disposition", "attachment; filename=\"guestbook.zip\"");
  req->send(res);
}

void webBegin() {
  server.addMiddleware([](AsyncWebServerRequest* req, ArMiddlewareNext next) {
    requestCount++;
    lastRequestMs = millis();
    lastClient = req->client()->remoteIP();
    if (requestLog) {
      Serial.printf("HTTP %s %s %s\n", lastClient.toString().c_str(), req->methodToString(), req->url().c_str());
    }
    next();
  });
  server.on("/", HTTP_GET, [](AsyncWebServerRequest* req) {
    req->send(200, "text/html; charset=utf-8", (const uint8_t*)INDEX_HTML, sizeof INDEX_HTML - 1);
  });
  server.on("/api/status", HTTP_GET, handleStatus);
  server.on("/api/recordings", HTTP_GET, handleRecordings);
  server.on("/api/time", HTTP_POST, handleTime);
  server.on("/api/rec", HTTP_GET, handleRecording);
  server.on("/guestbook.zip", HTTP_GET, handleZip);
  server.onNotFound([](AsyncWebServerRequest* req) { req->send(404, "text/plain", "not found"); });
  server.begin();
}
