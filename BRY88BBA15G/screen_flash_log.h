/* SPDX-License-Identifier: GPL-3.0-or-later
 * Persistent G3 UART diagnostic ring, 256 KiB, 64-byte aligned records.
 * Only the G3 application owner calls these methods. Flash is never written
 * from the UART callback or HTTP task.
 */
#pragma once
#include <esp_partition.h>
#include <string.h>
namespace g3flashlog {
constexpr uint32_t MAGIC=0x47334C47u;
constexpr size_t SLOT=64, SECTOR=4096, TOTAL=0x40000;
struct Entry {
  uint32_t magic, sequence, ms;
  uint16_t length;
  uint8_t direction, captured;
  uint8_t bytes[44];
  uint32_t checksum;
};
static_assert(sizeof(Entry)==64,"persistent record size");
inline uint32_t hash(const Entry &e) {
  const uint8_t *p=(const uint8_t *)&e;
  uint32_t h=2166136261u;
  for(size_t i=0;i<offsetof(Entry,checksum);i++)h=(h^p[i])*16777619u;
  return h;
}
class Store {
 public:
  bool begin() {
    part_=esp_partition_find_first(ESP_PARTITION_TYPE_DATA,
       (esp_partition_subtype_t)0x40,"g3log");
    if(!part_ || part_->address!=0x3d0000 || part_->size!=TOTAL)return false;
    Entry e={};
    for(uint32_t i=0;i<TOTAL/SLOT;i++) {
      if(esp_partition_read(part_,i*SLOT,&e,sizeof(e))!=ESP_OK)return false;
      if(e.magic!=MAGIC || e.checksum!=hash(e))continue;
      if(!valid_ || (int32_t)(e.sequence-sequence_)>0) {
        sequence_=e.sequence;next_=(i+1)%(TOTAL/SLOT);valid_=true;
      }
    }
    return ready_=true;
  }
  bool append(uint32_t ms,bool tx,const uint8_t *data,size_t n) {
    if(!ready_||!data||!n)return false;
    // Erase each 4 KiB sector only at its first slot, not per record.
    const uint32_t off=next_*SLOT;
    if(off%SECTOR==0 &&
       esp_partition_erase_range(part_,off,SECTOR)!=ESP_OK)return false;
    Entry e={};
    e.magic=MAGIC;e.sequence=++sequence_;e.ms=ms;
    e.length=n>65535?65535:n;e.direction=tx?1:0;
    e.captured=n<sizeof(e.bytes)?n:sizeof(e.bytes);
    memcpy(e.bytes,data,e.captured);e.checksum=hash(e);
    if(esp_partition_write(part_,off,&e,sizeof(e))!=ESP_OK)return false;
    next_=(next_+1)%(TOTAL/SLOT);valid_=true;return true;
  }
  bool read(uint32_t index,Entry &out) const {
    if(!ready_||index>=TOTAL/SLOT)return false;
    if(esp_partition_read(part_,index*SLOT,&out,sizeof(out))!=ESP_OK)return false;
    return out.magic==MAGIC && out.checksum==hash(out);
  }
  uint32_t slots() const {return TOTAL/SLOT;}
  bool ready() const {return ready_;}
  uint32_t sequence() const {return sequence_;}
 private:
  const esp_partition_t *part_=nullptr;
  uint32_t sequence_=0,next_=0;
  bool ready_=false,valid_=false;
};
} // namespace g3flashlog
