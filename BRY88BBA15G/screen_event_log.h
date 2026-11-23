/* SPDX-License-Identifier: GPL-3.0-or-later
 * Fixed-storage G3 diagnostic event ring. No heap, strings, credentials or
 * filesystem access; single owner task writes and reads all records.
 */
#pragma once
#include <stdint.h>
#include <stddef.h>

namespace g3diag {
enum Severity : uint8_t { INFO=1, WARN=2, ERROR=3 };
enum Event : uint16_t {
  BOOT=1, UART_LOSS=2, SNAPSHOT_LOST=3, SNAPSHOT_RESTORED=4,
  MCU_UART_SILENT=5, MCU_UART_RESTORED=6, MCU_OTA_TIMEOUT=7,
  GUARD_REPLY_TIMEOUT=8, OTA_STATE_CHANGED=9
};
struct Record {
  uint32_t uptime_ms;
  uint32_t argument;
  uint32_t repeat;
  uint16_t code;
  uint8_t severity;
  uint8_t reserved;
};
static_assert(sizeof(Record)==16, "Flash diagnostic record ABI");
constexpr size_t CAPACITY=1024;  // 16 KiB, bounded diagnostic capture
constexpr size_t SAVED=32;     // 512 B plus header, persisted only on faults
class Ring {
 public:
  void add(uint32_t now, uint16_t code, uint8_t severity, uint32_t arg=0) {
    // Merge adjacent identical events (one per burst rather than per byte).
    if (count_ && at(0).code==code && at(0).argument==arg &&
        at(0).severity==severity && uint32_t(now-at(0).uptime_ms)<30000u) {
      Record &last=entries_[(next_+CAPACITY-1)%CAPACITY];
      if (last.repeat!=UINT32_MAX) ++last.repeat;
      return;
    }
    entries_[next_]={now,arg,1,code,severity,0};
    next_=(next_+1)%CAPACITY;
    if (count_<CAPACITY) ++count_;
    else ++overwritten_;
    if (severity>=ERROR) ++faults_;
  }
  const Record &at(size_t newest_index) const {
    return entries_[(next_+CAPACITY-1-newest_index%CAPACITY)%CAPACITY];
  }
  size_t size() const {return count_;}
  uint32_t overwritten() const {return overwritten_;}
  uint32_t faults() const {return faults_;}
 private:
  Record entries_[CAPACITY]={};
  size_t next_=0, count_=0;
  uint32_t overwritten_=0, faults_=0;
};
struct Saved {
  uint32_t magic;
  uint32_t version;
  uint32_t count;
  Record entries[SAVED];
  uint32_t checksum;
};
static_assert(sizeof(Saved)==528,"Bounded fault snapshot ABI");
constexpr uint32_t MAGIC=0x47334447u;
inline uint32_t checksum(const Saved &s) {
  const uint8_t *p=reinterpret_cast<const uint8_t *>(&s);
  uint32_t h=2166136261u;
  for(size_t i=0;i<offsetof(Saved,checksum);i++)h=(h^p[i])*16777619u;
  return h;
}
inline Saved capture(const Ring &ring) {
  Saved out={};
  out.magic=MAGIC;out.version=1;
  out.count=ring.size()<SAVED?ring.size():SAVED;
  for(size_t i=0;i<out.count;i++)out.entries[i]=ring.at(i);
  out.checksum=checksum(out);
  return out;
}
inline bool valid(const Saved &s) {
  return s.magic==MAGIC&&s.version==1&&s.count<=SAVED&&s.checksum==checksum(s);
}
} // namespace g3diag
