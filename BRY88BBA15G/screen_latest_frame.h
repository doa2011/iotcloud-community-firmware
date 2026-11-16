/* SPDX-License-Identifier: GPL-3.0-or-later
 * MJPEG parser snapshot from screen MCU 83f8de2, with a bounded store callback.
 * The socket owner drains independently of UART credit. Three ownership slots:
 * immutable transmitting JPEG, replaceable complete pending JPEG, input JPEG.
 */
#pragma once
#include <stdlib.h>
#include <string.h>
#include <stdio.h>
#include "relay_mjpeg.inc"
class ScreenLatestFrame {
  struct Image {
    uint8_t *data = nullptr;
    size_t capacity = 0, size = 0;
    uint32_t crc = 0, at = 0;
  } input, pending, current;
  relay_mjpeg_parser parser = {};
  size_t budget, allocated = 0, offset = 0;
  unsigned phase = 0;
  char header[768] = {};
  size_t headerSize = 0, headerAt = 0;
  bool responseSent = false, finishing = false;
  uint32_t now = 0;
  bool (*canAllocate)(size_t) = nullptr;
  static bool want(void *p) {
    auto &s = *static_cast<ScreenLatestFrame *>(p);
    if (!s.current.size && !s.finishing && s.current.capacity > s.input.capacity) {
      Image old = s.input; s.input = s.current; s.current = old;
    }
    if (!s.pending.size && s.pending.capacity > s.input.capacity) {
      Image old = s.input; s.input = s.pending; s.pending = old;
    }
    s.input.size = 0;
    size_t wanted = s.parser.length_known ? s.parser.content_length :
                    (s.budget / 3u) & ~size_t(1023u);
    if (wanted > s.input.capacity) {
      // No partial image exists at begin_frame: release undersized scratch
      // before allocation, avoiding a realloc's transient double allocation.
      s.allocated -= s.input.capacity; free(s.input.data); s.input = Image{};
      size_t capacity = (wanted + 1023u) & ~size_t(1023u);
      if (capacity > s.budget - s.allocated ||
          (s.canAllocate && !s.canAllocate(capacity))) { s.rejected++; return false; }
      s.input.data = static_cast<uint8_t *>(malloc(capacity));
      if (!s.input.data) { s.rejected++; return false; }
      s.input.capacity = capacity; s.allocated += capacity;
      if (s.allocated > s.peakBytes) s.peakBytes = s.allocated;
    }
    return true;
  }
  static bool store(void *p, size_t at, uint8_t byte) {
    auto &s = *static_cast<ScreenLatestFrame *>(p);
    if (at == s.input.capacity) {
      size_t capacity = (at + 1024u) & ~size_t(1023u);
      size_t growth = capacity - s.input.capacity;
      // Unknown-length JPEG growth includes both old and new scratch until
      // copying finishes; the 32KiB bound includes that transient allocation.
      if (capacity > s.budget - s.allocated ||
          (s.canAllocate && !s.canAllocate(capacity))) {
        s.rejected++;
        return false;
      }
      auto *data = static_cast<uint8_t *>(malloc(capacity));
      if (!data) { s.rejected++; return false; }
      if (s.allocated + capacity > s.peakBytes) s.peakBytes = s.allocated + capacity;
      memcpy(data, s.input.data, s.input.capacity); free(s.input.data);
      s.input.data = data;
      s.allocated += growth;
      s.input.capacity = capacity;
      if (s.allocated > s.peakBytes) s.peakBytes = s.allocated;
    }
    s.input.data[at] = byte;
    return true;
  }
  static void frame(void *p, size_t n) {
    auto &s = *static_cast<ScreenLatestFrame *>(p);
    s.received++;
    s.input.size = n;
    s.input.at = s.now;
    s.input.crc = media_crc32(s.input.data, n);
    s.receivedCrc = s.input.crc;
    if (s.pending.size) s.replaced++;
    Image old = s.pending;
    s.pending = s.input;
    s.input = old;
    s.input.size = 0;
  }
  void stage(const char *text) {
    headerSize = strlen(text); headerAt = 0;
    memcpy(header, text, headerSize);
  }
public:
  uint32_t received = 0, replaced = 0, rejected = 0, completed = 0,
           receivedCrc = 0, completedCrc = 0, completedAge = 0,
           maxCompletedAge = 0;
  size_t peakBytes = 0;
  explicit ScreenLatestFrame(size_t bytes = 32768,
                            bool (*allow)(size_t) = nullptr)
      : budget(bytes), canAllocate(allow) {
    parser.context = this; parser.want = want;
    parser.store = store; parser.frame = frame;
    relay_mjpeg_init(&parser);
  }
  ~ScreenLatestFrame() { clear(); }
  void clear() {
    free(input.data); free(pending.data); free(current.data);
    input = Image{}; pending = Image{}; current = Image{};
    allocated = offset = phase = headerSize = headerAt = 0;
    responseSent = finishing = false;
    relay_mjpeg_init(&parser);
  }
  void feed(const uint8_t *data, size_t size, uint32_t at) {
    now = at; relay_mjpeg_feed(&parser, data, size);
  }
  unsigned error() const { return parser.error; }
  bool ended() const { return parser.ended; }
  bool empty() const { return !pending.size && !current.size && !finishing && (!responseSent || headerAt == headerSize); }
  size_t bytes() const { return allocated; }
  // A credit acknowledges the preceding complete UART block. Only after the
  // final JPEG/tail block is acknowledged may its immutable slot be recycled.
  void ack(uint32_t at) {
    if (!finishing) return;
    completed++; completedCrc = current.crc;
    completedAge = at - current.at;
    if (completedAge > maxCompletedAge) maxCompletedAge = completedAge;
    current.size = 0; finishing = false; phase = 0;
  }
  size_t read(uint8_t *out, size_t cap) {
    if (finishing) return 0;
    if (!responseSent) {
      if (parser.status == 401 && parser.error) {
        snprintf(header, sizeof(header), "HTTP/1.1 401 Unauthorized\r\nWWW-Authenticate: %s\r\nContent-Length: 0\r\n\r\n", parser.challenge);
        headerSize = strlen(header); headerAt = 0;
      } else {
      if (!parser.state) return 0;
      stage("HTTP/1.1 200 OK\r\nContent-Type: multipart/x-mixed-replace; boundary=iotcloud-latest\r\nConnection: close\r\n\r\n");
      }
      responseSent = true;
    }
    if (headerAt < headerSize) {
      size_t n = headerSize - headerAt;
      if (n > cap) n = cap;
      memcpy(out, header + headerAt, n); headerAt += n;
      return n;
    }
    if (!current.size) {
      if (!pending.size) return 0;
      Image old = current; current = pending; pending = old;
      offset = 0; phase = 1;
      snprintf(header, sizeof(header), "--iotcloud-latest\r\nContent-Type: image/jpeg\r\nContent-Length: %zu\r\n\r\n", current.size);
      headerSize = strlen(header); headerAt = 0;
      return read(out, cap);
    }
    if (phase == 1) {
      size_t n = current.size - offset;
      if (n > cap) n = cap;
      memcpy(out, current.data + offset, n); offset += n;
      if (offset == current.size) phase = 2;
      return n;
    }
    out[0] = '\r'; out[1] = '\n'; finishing = true;
    return 2;
  }
};
