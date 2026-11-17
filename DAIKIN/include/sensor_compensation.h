#pragma once
#include <stdint.h>
#include <stddef.h>
#include <math.h>

namespace daikin {
// G1/G2 stock image: PM 0x400e6408, CO2 0x400e6284,
// HCHO 0x4012b038; final publication limits in 0x400e6188.
inline float stockTemperatureFactor(float rawTemperature,bool fresh) {
  return fresh&&rawTemperature>28.0f?1.0f-(rawTemperature-28.0f)/100.0f:1.0f;
}
inline uint16_t compensatePm25(uint16_t raw,float rawTemperature,bool freshTemperature) {
  const float p=raw<1?1:(raw>1000?1000:raw);
  const float corrected=(p+0.0013f*p*p-0.0357f*p+5.2954f)*stockTemperatureFactor(rawTemperature,freshTemperature);
  return uint16_t(fminf(300.0f,fmaxf(1.0f,corrected)));
}
inline uint16_t compensateCo2(uint16_t raw,float rawTemperature,bool freshTemperature) {
  const float p=raw<400?400:(raw>3000?3000:raw);
  return uint16_t(fminf(3000.0f,fmaxf(400.0f,p*stockTemperatureFactor(rawTemperature,freshTemperature))));
}
inline float compensateHcho(uint16_t ppb) {
  const float x=ppb/1000.0f*1.25f;
  const float corrected=x<=0.075f?x*1.3f-0.0225f:x*1.25f-0.02f;
  return fminf(0.5f,fmaxf(0.01f,corrected));
}
// Stock SGP input uses the already compensated HDC temperature/RH. Its mg/m3
// integer is converted to Sensirion's unsigned Q8.8 g/m3 (zero disables).
inline bool absoluteHumidity(float temperature,float humidity,uint32_t &mg,uint16_t &word) {
  if(!isfinite(temperature)||!isfinite(humidity)||temperature< -60||temperature>125||humidity<0||humidity>100)return false;
  const double density=216.7*(humidity/100.0)*6.112*exp(17.62*temperature/(243.12+temperature))/(273.15+temperature)*1000.0;
  if(!isfinite(density)||density<0)return false;
  mg=uint32_t(density);
  const uint64_t fixed=uint64_t(mg)*256/1000;
  word=fixed>65535?65535:uint16_t(fixed);
  return true;
}

struct PmPower {
  static constexpr uint32_t windowMs=59000,periodMs=3540000,warmupMs=36000;
  bool night=false,started=false,window=false;
  uint8_t mode=0;
  uint32_t nextWake=0,openedAt=0,wakeAt=0;
  static bool nightAt(bool enabled,unsigned start,unsigned end,int timezoneMinutes,bool trusted,uint64_t utc) {
    if(!enabled||!trusted||start==end)return false;
    int minute=int((utc/60)%1440)+timezoneMinutes;
    minute=(minute%1440+1440)%1440;
    return start<end?(unsigned(minute)>=start&&unsigned(minute)<end):(unsigned(minute)>=start||unsigned(minute)<end);
  }
  uint8_t desired(uint32_t now,bool isNight) {
    if(!started||night!=isNight){
      started=true;night=isNight;
      // Enter the quiet interval immediately. Never replay missed cycles.
      window=false;if(night)nextWake=now+periodMs;
    }
    if(!night)return 5;
    if(int32_t(now-nextWake)>=0){
      const uint32_t elapsed=now-nextWake;
      nextWake+=((elapsed/periodMs)+1)*periodMs;
      // A delayed caller past the window remains asleep.
      window=elapsed%periodMs<windowMs;wakeAt=now-elapsed%periodMs;
    }
    return window&&uint32_t(now-wakeAt)<windowMs?5:1;
  }
  void acknowledged(uint8_t sent,uint32_t now) {mode=sent;if(sent==5)openedAt=now;}
  bool canRead(uint32_t now) const {return mode==5&&uint32_t(now-openedAt)>=warmupMs;}
  uint32_t maxAgeMs() const {return night?3600000:120000;}
};

struct SgpBaseline {
  uint16_t serial[3]={},feature=0,co2=0,tvoc=0;
  uint64_t savedUtc=0;
  bool matches(const uint16_t *identity,uint16_t featureSet,bool trusted,uint64_t utc) const {
    return trusted&&savedUtc&&utc>=savedUtc&&utc-savedUtc<=7*24*60*60&&feature==featureSet&&
      serial[0]==identity[0]&&serial[1]==identity[1]&&serial[2]==identity[2];
  }
  void encode(uint8_t (&bytes)[24]) const {
    bytes[0]='S';bytes[1]='G';bytes[2]='P';bytes[3]=1;
    const uint16_t words[]={serial[0],serial[1],serial[2],feature,co2,tvoc};
    for(unsigned i=0;i<6;i++){bytes[4+i*2]=words[i]>>8;bytes[5+i*2]=words[i];}
    for(unsigned i=0;i<8;i++)bytes[16+i]=uint8_t(savedUtc>>(56-i*8));
  }
  bool decode(const uint8_t *bytes,size_t n) {
    if(n!=24||bytes[0]!='S'||bytes[1]!='G'||bytes[2]!='P'||bytes[3]!=1)return false;
    uint16_t words[6];for(unsigned i=0;i<6;i++)words[i]=(uint16_t(bytes[4+i*2])<<8)|bytes[5+i*2];
    for(unsigned i=0;i<3;i++)serial[i]=words[i];
    feature=words[3];co2=words[4];tvoc=words[5];savedUtc=0;
    for(unsigned i=0;i<8;i++)savedUtc=(savedUtc<<8)|bytes[16+i];
    return savedUtc!=0;
  }
};
}
