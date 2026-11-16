#pragma once
#include <stdint.h>
#include <stddef.h>
namespace daikin {
inline bool checksum(const uint8_t *p, size_t n, size_t start=0) {
  uint8_t sum=0; for(size_t i=start;i<n;i++) sum+=p[i]; return sum==0;
}
inline bool co2(const uint8_t *p,size_t n,uint16_t &value) {
  if(n!=8||p[0]!=0x16||p[1]!=5||p[2]!=1||!checksum(p,n)) return false;
  value=(p[3]<<8)|p[4]; return value>=250&&value<=10000;
}
inline bool ch2o(const uint8_t *p,size_t n,uint16_t &value) {
  if(n!=9||p[0]!=0xff||p[1]!=0x86||!checksum(p,n,1)) return false;
  // ZE08 query replies carry ug/m3 in bytes 2/3 and ppb in bytes 6/7.
  // Keep ppb here, matching the stock FF17 active-report conversion.
  value=(p[6]<<8)|p[7]; return true;
}
struct Particulates { uint16_t pm25=0,pm10=0; uint8_t status=0; };
inline bool pmFrame(const uint8_t *p,size_t n,bool pm2005,Particulates &value) {
  const size_t o=pm2005?1:0;
  if(n!=22+o||p[o]!=0x16||p[o+1]!=22) return false;
  uint8_t sum=0;for(size_t i=o;i<o+21;i++)sum^=p[i];
  if(sum!=p[o+21])return false;
  // A leading bus byte is retained for the reference PM2005 variant, but
  // never substitutes for the complete 22-byte, XOR-protected frame.
  value.status=p[o+2];
  value.pm25=(p[o+5]<<8)|p[o+6];
  value.pm10=(p[o+7]<<8)|p[o+8];
  return true;
}
inline bool pm(const uint8_t *p,size_t n,bool pm2005,Particulates &value) {
  return pmFrame(p,n,pm2005,value)&&value.status==0x80;
}
struct Sample {
  float value=0; uint32_t at=0,max_age_ms=20000; bool received=false;
  void set(float v,uint32_t now) {value=v;at=now;received=true;}
  bool valid(uint32_t now) const {return valid(now,max_age_ms);}
  bool valid(uint32_t now,uint32_t ttl) const {return received&&uint32_t(now-at)<ttl;}
};
}
