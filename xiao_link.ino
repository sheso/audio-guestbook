/**
 * Link to the XIAO ESP32S3 web server over Serial1 (pin 0 = RX1, pin 1 = TX1).
 *
 * The XIAO sends one request at a time and waits for the answer. Requests and reply headers
 * are text lines ending in '\n'; DATA replies are followed by raw file bytes. Every request
 * starts with a sequence number that is echoed back, so the XIAO can drop stale replies to
 * requests it has already given up on (e.g. while the Teensy was busy playing the greeting).
 *
 *   PING <seq>                      -> PONG <seq> <mode> <unixtime>
 *   LIST <seq>                      -> FILE <seq> <num> <size> <mtime>  (one per recording)
 *                                      END <seq> <count>
 *   GET <seq> <num> <offset> <len>  -> DATA <seq> <num> <offset> <len> <crc32> + <len> bytes
 *   TIME <seq> <unixtime>           -> OK <seq>
 *   (any error)                     -> ERR <seq> <reason>
 *
 * Recordings are identified by their number: <num> 17 is the file " 00017.wav". Times are local
 * wall-clock time expressed as a unix timestamp, the same way the SD card file dates are stored.
 * LIST and GET are only answered in Ready mode, otherwise the reply is ERR <seq> BUSY: recording
 * always has priority over syncing. GET returns at most XIAO_MAX_CHUNK bytes and may return fewer
 * at the end of the file (0 bytes when <offset> is at or past the end).
 */

#define XIAO_BAUD 2000000
#define XIAO_MAX_CHUNK 4096
#define XIAO_LINE_MAX 64
#define noDEBUG_XIAO_LINK // print every request on the USB serial monitor

static char xiaoLine[XIAO_LINE_MAX];
static uint8_t xiaoLineLen = 0;
static bool xiaoLineOverflow = false;
// Extra serial buffers, so a whole DATA chunk is queued without blocking the main loop
static uint8_t xiaoTxBuffer[XIAO_MAX_CHUNK + 128];
static uint8_t xiaoRxBuffer[256];
static uint8_t xiaoChunk[XIAO_MAX_CHUNK];

void xiaoBegin() {
  Serial1.begin(XIAO_BAUD);
  Serial1.addMemoryForWrite(xiaoTxBuffer, sizeof xiaoTxBuffer);
  Serial1.addMemoryForRead(xiaoRxBuffer, sizeof xiaoRxBuffer);
  Serial.println("XIAO link on Serial1 started");
}

// Call once per loop(): handles at most one complete request
void xiaoLoop() {
  while (Serial1.available()) {
    char c = Serial1.read();
    if (c == '\r') continue;
    if (c == '\n') {
      bool complete = !xiaoLineOverflow && xiaoLineLen > 0;
      xiaoLine[xiaoLineLen] = 0;
      xiaoLineLen = 0;
      xiaoLineOverflow = false;
      if (complete) {
        xiaoHandleLine(xiaoLine);
        return;
      }
    }
    else if (xiaoLineLen < XIAO_LINE_MAX - 1) {
      xiaoLine[xiaoLineLen++] = c;
    }
    else {
      xiaoLineOverflow = true; // garbage on the line, drop it
    }
  }
}

static const char* modeName() {
  switch (mode) {
    case Mode::Initialising: return "Initialising";
    case Mode::Ready:        return "Ready";
    case Mode::Prompting:    return "Prompting";
    case Mode::Recording:    return "Recording";
    case Mode::Playing:      return "Playing";
    case Mode::Dialing:      return "Dialing";
  }
  return "Unknown";
}

static void xiaoHandleLine(char* line) {
  char cmd[8];
  unsigned long seq;
  int consumed = 0;
  if (sscanf(line, "%7s %lu%n", cmd, &seq, &consumed) != 2) return; // not a request, e.g. noise
  const char* args = line + consumed;

#if defined(DEBUG_XIAO_LINK)
  Serial.printf("XIAO: %s\n", line);
#endif // defined(DEBUG_XIAO_LINK)

  if (strcmp(cmd, "PING") == 0) {
    Serial1.printf("PONG %lu %s %lu\n", seq, modeName(), (unsigned long)now());
  }
  else if (strcmp(cmd, "TIME") == 0) {
    unsigned long t;
    if (sscanf(args, "%lu", &t) != 1) {
      Serial1.printf("ERR %lu BAD_ARGS\n", seq);
      return;
    }
    Teensy3Clock.set(t);
    setTime(t);
    Serial1.printf("OK %lu\n", seq);
    Serial.printf("Clock set by XIAO to %04d-%02d-%02d %02d:%02d:%02d\n",
                  year(), month(), day(), hour(), minute(), second());
  }
  else if (strcmp(cmd, "LIST") == 0 || strcmp(cmd, "GET") == 0) {
    if (mode != Mode::Ready) {
      Serial1.printf("ERR %lu BUSY\n", seq);
    }
    else if (cmd[0] == 'L') {
      xiaoList(seq);
    }
    else {
      xiaoGet(seq, args);
    }
  }
  else {
    Serial1.printf("ERR %lu UNKNOWN_COMMAND\n", seq);
  }
}

// Recognise " NNNNN.wav" (the names startRecording() creates) and return NNNNN, or -1
static long recordingNumber(const char* name) {
  if (strlen(name) != 10 || name[0] != ' ' || strcmp(name + 6, ".wav") != 0) return -1;
  long num = 0;
  for (int i = 1; i <= 5; i++) {
    if (!isdigit(name[i])) return -1;
    num = num * 10 + (name[i] - '0');
  }
  return num;
}

static void xiaoList(unsigned long seq) {
  File dir = SD.open("/");
  unsigned count = 0;
  while (File entry = dir.openNextFile()) {
    long num = entry.isDirectory() ? -1 : recordingNumber(entry.name());
    if (num >= 0) {
      DateTimeFields tm;
      unsigned long mtime = entry.getModifyTime(tm) ? makeTime(tm) : 0;
      Serial1.printf("FILE %lu %ld %lu %lu\n", seq, num, (unsigned long)entry.size(), mtime);
      count++;
    }
    entry.close();
  }
  dir.close();
  Serial1.printf("END %lu %u\n", seq, count);
}

static void xiaoGet(unsigned long seq, const char* args) {
  unsigned long num, offset, len;
  if (sscanf(args, "%lu %lu %lu", &num, &offset, &len) != 3 || num > 99999) {
    Serial1.printf("ERR %lu BAD_ARGS\n", seq);
    return;
  }
  char name[12];
  snprintf(name, sizeof name, " %05lu.wav", num);
  File f = SD.open(name, FILE_READ);
  if (!f) {
    Serial1.printf("ERR %lu NOT_FOUND\n", seq);
    return;
  }
  unsigned long size = f.size();
  if (len > XIAO_MAX_CHUNK) len = XIAO_MAX_CHUNK;
  if (offset >= size) len = 0;
  else if (len > size - offset) len = size - offset;

  size_t got = 0;
  if (len > 0 && f.seek(offset)) got = f.read(xiaoChunk, len);
  f.close();
  if (got != len) {
    Serial1.printf("ERR %lu READ_FAILED\n", seq);
    return;
  }
  Serial1.printf("DATA %lu %lu %lu %lu %08lx\n", seq, num, offset, len, (unsigned long)crc32(0, xiaoChunk, len));
  Serial1.write(xiaoChunk, len);
  if (offset == 0) Serial.printf("XIAO: sending%s (%lu bytes)\n", name, size);
}

// Standard CRC-32, same as zlib's crc32(): start with crc = 0, or pass the previous result to continue
uint32_t crc32(uint32_t crc, const uint8_t* data, size_t len) {
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
