#include "sensor_compensation.h"
#include <esp_sntp.h>
#include <time.h>

static bool writeI2c(TwoWire &bus,uint8_t address,const uint8_t *data,size_t n){bus.beginTransmission(address);bus.write(data,n);return bus.endTransmission()==0;}
static bool readI2c(TwoWire &bus,uint8_t address,uint8_t *data,size_t n){
  if(bus.requestFrom(address,(uint8_t)n)!=(int)n){while(bus.available())bus.read();return false;}
  for(size_t i=0;i<n;i++)data[i]=bus.read();
  return true;
}
static uint8_t crc8(const uint8_t *p){uint8_t c=0xff;for(int i=0;i<2;i++){c^=p[i];for(int b=0;b<8;b++)c=(c&128)?(c<<1)^0x31:c<<1;}return c;}

static daikin::Sample rawTemperature,rawHumidity,rawPm25,rawPm10,rawHcho,rawHchoUg;
static daikin::PmPower pmPower;
static bool pmLeadingByte=false,pmAwaitingMeasurement=true;
static uint8_t pmStatus=0;
static uint32_t nextPmCommand=0,pmFrameErrors=0;
static std::atomic<bool> sensorClockSynchronized{false};
static bool sensorClockStarted=false;
static uint64_t sensorUtc(){return sensorClockSynchronized.load()?uint64_t(time(nullptr)):0;}
static void pollSensorClock(){
  if(!sensorClockStarted&&WiFi.status()==WL_CONNECTED){
    sensorClockStarted=true;
    sntp_set_time_sync_notification_cb([](struct timeval *){sensorClockSynchronized.store(true);});
    // UTC is also required to check the age of a baseline after a power loss.
    // Night hours apply their own explicit offset; no DST/global TZ mutation.
    configTime(0,0,"pool.ntp.org","time.cloudflare.com");
  }
}
static uint32_t pmMaxAgeMs(uint32_t now){(void)now;return pmPower.maxAgeMs();}
static void pollPmPower(uint32_t now){
  const uint64_t utc=sensorUtc();
  const bool night=daikin::PmPower::nightAt(config["pm_power"]["enabled"]|false,
    config["pm_power"]["start_minute"]|1380U,config["pm_power"]["end_minute"]|420U,
    config["pm_power"]["timezone_minutes"]|480,utc!=0,utc);
  const uint8_t desired=pmPower.desired(now,night);
  readings[PM25].max_age_ms=readings[PM10].max_age_ms=pmMaxAgeMs(now);
  rawPm25.max_age_ms=rawPm10.max_age_ms=pmMaxAgeMs(now);
  if(desired==pmPower.mode||!due(now,nextPmCommand))return;
  nextPmCommand=now+1000;
  uint8_t packet[]={0x16,7,desired,0,0,0,0};
  for(unsigned i=0;i<6;i++)packet[6]^=packet[i];
  if(writeI2c(pmBus,0x28,packet,sizeof(packet))){
    pmPower.acknowledged(desired,now);
    if(desired==5)pmAwaitingMeasurement=true;
  }
}
static void pollParticulates(uint32_t now){
  if(pmPower.mode!=5)return;
  uint8_t packet[23];daikin::Particulates particles;
  bool decoded=false;
  if(pmLeadingByte){
    decoded=readI2c(pmBus,0x28,packet,23)&&daikin::pmFrame(packet,23,true,particles);
  }else if(readI2c(pmBus,0x28,packet,22)){
    decoded=daikin::pmFrame(packet,22,false,particles);
    if(!decoded&&packet[1]==0x16&&packet[2]==22){
      decoded=readI2c(pmBus,0x28,packet,23)&&daikin::pmFrame(packet,23,true,particles);
      if(decoded)pmLeadingByte=true;
    }
  }
  if(!decoded){pmFrameErrors++;return;}
  pmModel=pmLeadingByte?"PM2005":"PM2105";
  pmStatus=particles.status;
  if(pmStatus==3){pmAwaitingMeasurement=true;return;}
  if(pmStatus!=0x80||!pmPower.canRead(now)||!pmAwaitingMeasurement)return;
  // Mode 5 returns its old result while measuring. Only the first completed
  // frame after mode 5 / status 3 advances the source timestamp.
  pmAwaitingMeasurement=false;
  rawPm25.set(particles.pm25,now);rawPm10.set(particles.pm10,now);
  if(rawTemperature.valid(now))readings[PM25].set(daikin::compensatePm25(particles.pm25,rawTemperature.value,true),now);
  readings[PM10].set(particles.pm10,now);
}

static struct SgpState {
  uint16_t serial[3]={},feature=0,humidity=0;
  bool identity=false,humidityKnown=false,recordLoaded=false,recordPresent=false;
  bool restorePending=false,restored=false,eligible=false;
  uint32_t continuousSince=0,lastMeasure=0,lastBaselineAttempt=0,errors=0,storageErrors=0;
  uint32_t absoluteHumidityMg=0;
  daikin::SgpBaseline record;
} sgp;
static bool sgpReadWords(uint16_t command,uint16_t *words,unsigned count,unsigned waitMs){
  const uint8_t cmd[]={uint8_t(command>>8),uint8_t(command)};
  uint8_t result[9];
  if(!writeI2c(thBus,0x58,cmd,2))return false;
  delay(waitMs);
  if(!readI2c(thBus,0x58,result,count*3))return false;
  for(unsigned i=0;i<count;i++){
    if(crc8(result+i*3)!=result[i*3+2])return false;
    words[i]=(uint16_t(result[i*3])<<8)|result[i*3+1];
  }
  return true;
}
static bool sgpWriteWords(uint16_t command,const uint16_t *words,unsigned count){
  uint8_t packet[8]={uint8_t(command>>8),uint8_t(command)};
  for(unsigned i=0;i<count;i++){
    packet[2+i*3]=words[i]>>8;packet[3+i*3]=words[i];packet[4+i*3]=crc8(packet+2+i*3);
  }
  if(!writeI2c(thBus,0x58,packet,2+count*3))return false;
  delay(10);return true;
}
static void loadSgpBaseline(){
  if(sgp.recordLoaded)return;
  sgp.recordLoaded=true;
  Preferences storage;
  if(!storage.begin("sgp30-cal",false)){sgp.storageErrors++;return;}
  uint8_t bytes[24];
  if(storage.getBytesLength("baseline")==sizeof(bytes)&&storage.getBytes("baseline",bytes,sizeof(bytes))==sizeof(bytes))
    sgp.recordPresent=sgp.record.decode(bytes,sizeof(bytes));
  storage.end();
  sgp.restorePending=sgp.recordPresent;
}
static bool initSgp(uint32_t now,bool restore){
  readings[TVOC].received=false;
  sgpReady=false;sgp.humidityKnown=false;sgp.eligible=false;sgp.restored=false;
  {
    if(!sgpReadWords(0x3682,sgp.serial,3,1)||!sgpReadWords(0x202f,&sgp.feature,1,10)){sgp.errors++;return false;}
    sgp.identity=true;
  }
  loadSgpBaseline();
  if(!sgpWriteWords(0x2003,nullptr,0)){sgp.errors++;return false;}
  const uint64_t utc=sensorUtc();
  if(restore&&sgp.recordPresent&&sgp.record.matches(sgp.serial,sgp.feature,utc!=0,utc)){
    // get_iaq_baseline returns CO2eq, TVOC; set_iaq_baseline takes TVOC, CO2eq.
    const uint16_t words[]={sgp.record.tvoc,sgp.record.co2};
    if(!sgpWriteWords(0x201e,words,2)){sgp.errors++;return false;}
    sgp.restored=true;
  }
  if(utc)sgp.restorePending=false;
  sgpReady=true;sgpSince=now;sgp.continuousSince=now;sgp.lastMeasure=now;sgp.lastBaselineAttempt=now;
  return true;
}
static void saveSgpBaseline(uint32_t now){
  if(!sgp.eligible||uint32_t(now-sgp.lastBaselineAttempt)<3600000)return;
  const uint64_t utc=sensorUtc();if(!utc)return;
  sgp.lastBaselineAttempt=now;
  uint16_t words[2];
  if(!sgpReadWords(0x2015,words,2,10)||!words[0]||!words[1]){sgp.errors++;return;}
  daikin::SgpBaseline record;
  for(unsigned i=0;i<3;i++)record.serial[i]=sgp.serial[i];
  record.feature=sgp.feature;record.co2=words[0];record.tvoc=words[1];record.savedUtc=utc;
  uint8_t bytes[24];record.encode(bytes);
  Preferences storage;
  if(!storage.begin("sgp30-cal",false)){sgp.storageErrors++;return;}
  const bool saved=storage.putBytes("baseline",bytes,sizeof(bytes))==sizeof(bytes);storage.end();
  if(saved){sgp.record=record;sgp.recordPresent=true;}else sgp.storageErrors++;
}
static void pollSgp(uint32_t now){
  if(!due(now,nextSgp))return;
  const uint64_t utc=sensorUtc();
  if(!sgpReady){nextSgp=now+1000;initSgp(now,true);return;}
  if(sgp.restorePending&&utc){
    sgp.restorePending=false;
    if(sgp.record.matches(sgp.serial,sgp.feature,true,utc)){
      nextSgp=now+1000;initSgp(now,true);return;
    }
  }
  // The algorithm requires one measurement each second. A missed full slot
  // starts a fresh run; do not issue a burst of catch-up measurements.
  if(uint32_t(now-sgp.lastMeasure)>=2000){nextSgp=now+1000;initSgp(now,true);return;}
  nextSgp+=1000;
  uint32_t humidityMg=0;uint16_t humidityWord=0;
  if(readings[TEMPERATURE].valid(now)&&readings[HUMIDITY].valid(now))
    daikin::absoluteHumidity(readings[TEMPERATURE].value,readings[HUMIDITY].value,humidityMg,humidityWord);
  sgp.absoluteHumidityMg=humidityMg;
  if(!sgp.humidityKnown||humidityWord!=sgp.humidity){
    sgp.humidityKnown=sgpWriteWords(0x2061,&humidityWord,1);
    if(sgp.humidityKnown)sgp.humidity=humidityWord;else sgp.errors++;
  }
  uint16_t words[2];
  if(!sgpReadWords(0x2008,words,2,12)){
    sgp.errors++;sgpReady=false;sgp.eligible=false;readings[TVOC].received=false;nextSgp=now+1000;return;
  }
  sgp.lastMeasure=now;
  if(uint32_t(now-sgpSince)>=15000)readings[TVOC].set(words[1],now);
  if(uint32_t(now-sgp.continuousSince)>=3600000)sgp.eligible=true;
  saveSgpBaseline(now);
}
static bool resetSensorCalibration(){
  Preferences storage;
  if(!storage.begin("sgp30-cal",false)){sgp.storageErrors++;return false;}
  const bool cleared=storage.clear();storage.end();
  if(!cleared){sgp.storageErrors++;return false;}
  sgp.recordPresent=false;sgp.restorePending=false;sgp.recordLoaded=true;sgpReady=false;
  sgp.eligible=false;sgp.restored=false;readings[TVOC].received=false;nextSgp=millis();
  return true;
}
static void sensorDiagnostics(JsonObject out){
  const uint32_t now=millis();const uint64_t utc=sensorUtc();
  out["clock_trusted"]=utc!=0;
  JsonObject climate=out.createNestedObject("climate_raw");
  if(rawTemperature.received){climate["temperature"]=rawTemperature.value;climate["humidity"]=rawHumidity.value;climate["age_ms"]=uint32_t(now-rawTemperature.at);}
  climate["valid"]=rawTemperature.valid(now);
  JsonObject pm=out.createNestedObject("pm_raw");
  if(rawPm25.received){pm["pm25"]=rawPm25.value;pm["pm10"]=rawPm10.value;pm["age_ms"]=uint32_t(now-rawPm25.at);}
  pm["status"]=pmStatus;pm["mode"]=pmPower.mode;pm["night_active"]=pmPower.night;
  pm["warming"]=pmPower.mode==5&&!pmPower.canRead(now);pm["frame_errors"]=pmFrameErrors;
  pm["leading_byte"]=pmLeadingByte;
  JsonObject hcho=out.createNestedObject("hcho_raw");
  if(rawHcho.received){hcho["ppb"]=rawHcho.value;hcho["ug_m3"]=rawHchoUg.value;hcho["age_ms"]=uint32_t(now-rawHcho.at);}
  JsonObject gas=out.createNestedObject("sgp30");
  gas["ready"]=sgpReady;gas["warming"]=!sgpReady||uint32_t(now-sgpSince)<15000;
  gas["absolute_humidity_mg_m3"]=sgp.absoluteHumidityMg;gas["humidity_q8_8"]=sgp.humidityKnown?sgp.humidity:0;
  gas["baseline_restored"]=sgp.restored;gas["baseline_eligible"]=sgp.eligible;
  gas["baseline_saved"]=sgp.recordPresent;gas["restore_pending_clock"]=sgp.restorePending&&!utc;
  if(sgp.recordPresent&&utc&&utc>=sgp.record.savedUtc)gas["baseline_age_s"]=utc-sgp.record.savedUtc;
  gas["errors"]=sgp.errors;gas["storage_errors"]=sgp.storageErrors;
  if(sgp.identity){char serial[13];snprintf(serial,sizeof(serial),"%04x%04x%04x",sgp.serial[0],sgp.serial[1],sgp.serial[2]);gas["serial"]=serial;gas["feature_set"]=sgp.feature;}
}
static void pollSensors(uint32_t now){
  static bool warmedUp=false;warmedUp=warmedUp||now>=180000;
  pollSensorClock();pollPmPower(now);
  while(co2Uart.available())co2Module.push(co2Uart.read(),now);
  uint8_t command[6];size_t commandSize=co2Module.poll(now,command);
  if(commandSize){while(co2Uart.available())co2Uart.read();co2Uart.write(command,commandSize);}
  if(!co2Module.valid(now))readings[CO2].received=false;
  else if(rawTemperature.valid(now))readings[CO2].set(daikin::compensateCo2(co2Module.raw,rawTemperature.value,true),co2Module.measuredAt);
  while(hchoUart.available()){
    uint8_t b=hchoUart.read();if(!hchoWaiting)continue;
    if(hchoUsed==0&&b!=0xff)continue;
    hchoFrame[hchoUsed++]=b;
    if(hchoUsed==9){uint16_t v;if(daikin::ch2o(hchoFrame,9,v)){hchoPresent=true;rawHcho.set(v,now);rawHchoUg.set((uint16_t(hchoFrame[2])<<8)|hchoFrame[3],now);if(warmedUp)readings[HCHO].set(daikin::compensateHcho(v),now);}hchoUsed=0;hchoWaiting=false;}
  }
  if(hchoWaiting&&uint32_t(now-hchoSent)>500){hchoUsed=0;hchoWaiting=false;}
  if(due(now,nextSample)){
    nextSample=now+5000;
    // Sample HDC first: OEM PM/CO2 use raw T, SGP uses the corrected T/RH.
    const uint8_t reg[]={0};
    if(writeI2c(thBus,0x40,reg,1)){
      delay(15);uint8_t th[4];
      if(readI2c(thBus,0x40,th,4)){
        const float t=((th[0]<<8)|th[1])*165.0f/65536.0f-40;
        const float h=((th[2]<<8)|th[3])*100.0f/65536.0f;
        float compensatedT,compensatedH;
        if(daikin::compensateClimate(t,h,compensatedT,compensatedH,config["climate"]["temperature_offset"]|4.9f,config["climate"]["humidity_offset"]|0.0f)){
          rawTemperature.set(t,now);rawHumidity.set(h,now);
          readings[TEMPERATURE].set(compensatedT,now);readings[HUMIDITY].set(compensatedH,now);
        }
      }
    }
    pollParticulates(now);
    if(rawPm25.valid(now)&&rawTemperature.valid(now))
      readings[PM25].set(daikin::compensatePm25(uint16_t(rawPm25.value),rawTemperature.value,true),rawPm25.at);
    const uint8_t hchoQuery[]={0xff,1,0x86,0,0,0,0,0,0x79};
    while(hchoUart.available())hchoUart.read();
    hchoUart.write(hchoQuery,9);hchoSent=now;hchoWaiting=true;hchoUsed=0;
  }
  pollSgp(now);
}
