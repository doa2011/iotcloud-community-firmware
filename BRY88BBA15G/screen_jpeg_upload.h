#pragma once
#include <stdint.h>
#include <stddef.h>
#include <string.h>
extern "C" {
#include "protocol/media_wire.h"
}
// HTTP binary envelope: little-endian id, total, CRC32, width, height; JPEG follows.
struct ScreenJpegHeader {
  uint32_t id=0, total=0, crc=0;
  uint16_t width=0,height=0;
  bool parse(const uint8_t *p, size_t body) {
    if (!p || body<16 || body>16u+MEDIA_JPEG_MAX) return false;
    id=media_u32(p);total=media_u32(p+4);crc=media_u32(p+8);
    width=uint16_t(p[12])|(uint16_t(p[13])<<8);
    height=uint16_t(p[14])|(uint16_t(p[15])<<8);
    return id && total && total==body-16 && width && width<=MEDIA_WIDTH_MAX &&
           height && height<=MEDIA_HEIGHT_MAX;
  }
  void begin(uint8_t *p) const {
    p[0]=MEDIA_BEGIN;media_put32(p+1,id);media_put32(p+5,total);
    media_put32(p+9,crc);p[13]=width;p[14]=width>>8;p[15]=height;p[16]=height>>8;
  }
  size_t chunkSize(uint32_t offset) const {
    if(offset>=total)return 0;
    return total-offset>MEDIA_CHUNK?MEDIA_CHUNK:total-offset;
  }
  void chunk(uint8_t *p,uint32_t offset) const {
    p[0]=MEDIA_WRITE;media_put32(p+1,id);media_put32(p+5,offset);
  }
  void terminal(uint8_t *p,bool cancel) const {
    p[0]=cancel?MEDIA_CANCEL:MEDIA_COMMIT;media_put32(p+1,id);
  }
  bool matches(const uint8_t *slot,uint32_t offset,uint8_t state) const {
    return media_u32(slot)==id && media_u32(slot+4)==total &&
           media_u32(slot+8)==offset && media_u32(slot+12)==crc && slot[16]==state;
  }
};
