#include "web.h"
#include "catalog.h"
#include "config.h"
#include "eggs.h"
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
// Cookie value of a logged-in service view, new after every reboot
static String serviceToken;

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

// Serves a WAV file, with Range support (Safari needs it to play and seek audio)
static void sendWav(AsyncWebServerRequest* req, const String& path, uint32_t size, const String& label,
                    const String& downloadName) {
  auto file = std::make_shared<File>(SD.open(path, FILE_READ));
  if (!*file) {
    req->send(500, "text/plain", "cannot open the file");
    return;
  }
  std::shared_ptr<uint8_t[]> header = fixedWavHeader(*file, size);

  uint32_t start = 0, end = size - 1;
  bool partial = false;
  if (req->hasHeader("Range")) {
    if (!parseRange(req->getHeader("Range")->value(), size, start, end)) {
      AsyncWebServerResponse* res = req->beginResponse(416, "text/plain", "bad range");
      res->addHeader("Content-Range", "bytes */" + String(size));
      req->send(res);
      return;
    }
    partial = true;
  }
  uint32_t len = size ? end - start + 1 : 0;

  auto sent = std::make_shared<uint32_t>(0);
  if (requestLog) {
    Serial.printf("HTTP   %s, %s, bytes %lu-%lu of %lu\n", label.c_str(),
                  partial ? req->getHeader("Range")->value().c_str() : "no range", (unsigned long)start,
                  (unsigned long)end, (unsigned long)size);
    uint32_t started = millis();
    req->onDisconnect([sent, label, len, started]() {
      Serial.printf("HTTP   %s: closed after %lu of %lu bytes, %lu ms\n", label.c_str(),
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
    res->addHeader("Content-Range", "bytes " + String(start) + "-" + String(end) + "/" + String(size));
  }
  if (req->hasParam("dl")) {
    res->addHeader("Content-Disposition", "attachment; filename=\"" + downloadName + "\"");
  }
  req->send(res);
}

static void handleRecording(AsyncWebServerRequest* req) {
  Recording rec;
  if (!req->hasParam("n") || !catalog.find(req->getParam("n")->value().toInt(), rec)) {
    req->send(404, "text/plain", "no such recording");
    return;
  }
  sendWav(req, Catalog::path(rec.num), rec.size, "rec " + String(rec.num), Catalog::downloadName(rec));
}

// The service view: the password is checked here, the browser then keeps a session cookie
static bool serviceLoggedIn(AsyncWebServerRequest* req) {
  if (!req->hasHeader("Cookie")) return false;
  String cookies = "; " + req->getHeader("Cookie")->value() + ";";
  return cookies.indexOf("; svc=" + serviceToken + ";") >= 0;
}

static void handleServiceLogin(AsyncWebServerRequest* req) {
  if (strlen(SERVICE_PASSWORD) == 0) {
    req->send(503, "text/plain", "no service password in service_secrets.h");
    return;
  }
  if (!req->hasParam("p", true) || req->getParam("p", true)->value() != SERVICE_PASSWORD) {
    Serial.println("HTTP service login with a wrong password from " + req->client()->remoteIP().toString());
    req->send(403, "text/plain", "wrong password");
    return;
  }
  AsyncWebServerResponse* res = req->beginResponse(204);
  res->addHeader("Set-Cookie", "svc=" + serviceToken + "; Path=/; HttpOnly; SameSite=Strict");
  req->send(res);
}

static void handleEggs(AsyncWebServerRequest* req) {
  if (!serviceLoggedIn(req)) {
    req->send(403, "text/plain", "log in first");
    return;
  }
  std::vector<Egg> eggs = eggsList();
  String json = "[";
  for (size_t i = 0; i < eggs.size(); i++) {
    const Egg& e = eggs[i];
    if (i) json += ",";
    json += "{\"n\":\"" + e.number + "\",\"size\":" + String(e.size) +
            ",\"t\":" + String(clockValid(e.mtime) ? e.mtime : 0) + "}";
  }
  json += "]";
  req->send(200, "application/json", json);
}

static void handleEgg(AsyncWebServerRequest* req) {
  if (!serviceLoggedIn(req)) {
    req->send(403, "text/plain", "log in first");
    return;
  }
  Egg egg;
  if (!req->hasParam("n") || !eggFind(req->getParam("n")->value(), egg)) {
    req->send(404, "text/plain", "no such easter egg");
    return;
  }
  sendWav(req, eggPath(egg.number), egg.size, "egg " + egg.number, "egg-" + egg.number + ".wav");
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

// The end of the dial log, without the first (possibly cut) line
static void handleDials(AsyncWebServerRequest* req) {
  if (!serviceLoggedIn(req)) {
    req->send(403, "text/plain", "log in first");
    return;
  }
  String text;
  File f = SD.open(DIAL_LOG_FILE, FILE_READ);
  if (f) {
    uint32_t size = f.size(), start = size > DIAL_LOG_SHOWN ? size - DIAL_LOG_SHOWN : 0;
    f.seek(start);
    text = f.readString();
    f.close();
    if (start > 0) text = text.substring(text.indexOf('\n') + 1);
  }
  req->send(200, "text/plain; charset=utf-8", text);
}

void webBegin() {
  char token[33];
  for (int i = 0; i < 4; i++) snprintf(token + 8 * i, 9, "%08lx", (unsigned long)esp_random());
  serviceToken = token;
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
  server.on("/api/service/login", HTTP_POST, handleServiceLogin);
  server.on("/api/eggs", HTTP_GET, handleEggs);
  server.on("/api/egg", HTTP_GET, handleEgg);
  server.on("/api/dials", HTTP_GET, handleDials);
  server.onNotFound([](AsyncWebServerRequest* req) { req->send(404, "text/plain", "not found"); });
  server.begin();
}
