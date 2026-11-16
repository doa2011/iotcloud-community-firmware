#include <ArduinoJson.h>
#include <atomic>
#include <cassert>
#include <cstring>
#include <cstdio>
#include <vector>
#include <time.h>
#include "frames.h"
#include "climate.h"
static bool failClear=false;
static unsigned clockMs;static uint64_t utc=1800000000;
static uint32_t millis(){return clockMs;}static void delay(unsigned){}
static time_t fake_time(time_t*){return utc;}
#define time fake_time
#define WL_CONNECTED 3
struct {int status(){return 0;}} WiFi;
static void configTime(int,int,const char*,const char*){}
struct Preferences {static std::vector<uint8_t> bytes;bool begin(const char*,bool){return true;}void end(){}size_t getBytesLength(const char*){return bytes.size();}size_t getBytes(const char*,void*p,size_t n){memcpy(p,bytes.data(),n);return n;}size_t putBytes(const char*,const void*p,size_t n){bytes.assign((const uint8_t*)p,(const uint8_t*)p+n);return n;}bool clear(){if(failClear)return false;bytes.clear();return true;}};
std::vector<uint8_t> Preferences::bytes;
static uint8_t check(const uint8_t*p){uint8_t c=255;for(unsigned i=0;i<2;i++){c^=p[i];for(unsigned j=0;j<8;j++)c=(c&128)?(c<<1)^0x31:c<<1;}return c;}
struct TwoWire {std::vector<uint8_t> packet,response;std::vector<std::vector<uint8_t>> writes;unsigned cursor=0;uint16_t serial=123;bool corrupt=false;void beginTransmission(uint8_t){packet.clear();}void write(const uint8_t*p,size_t n){packet.assign(p,p+n);}int endTransmission(){writes.push_back(packet);return 0;}int requestFrom(uint8_t,uint8_t n){response.clear();uint16_t cmd=packet[0]*256+packet[1];std::vector<uint16_t>w=cmd==0x3682?std::vector<uint16_t>{serial,2,3}:cmd==0x202f?std::vector<uint16_t>{0x22}:cmd==0x2015?std::vector<uint16_t>{0xabcd,0x1234}:std::vector<uint16_t>{400,0};for(auto v:w){response.push_back(v>>8);response.push_back(v);response.push_back(check(response.data()+response.size()-2));}if(corrupt)response.back()^=1;cursor=0;return response.size()==n?n:0;}int available(){return cursor<response.size();}uint8_t read(){return response[cursor++];}}pmBus,thBus;
struct Uart{int available(){return 0;}uint8_t read(){return 0;}void write(const uint8_t*,size_t){}}co2Uart,hchoUart;
struct {bool valid(uint32_t){return false;}void push(uint8_t,uint32_t){}size_t poll(uint32_t,uint8_t*){return 0;}unsigned raw=400,measuredAt=0;}co2Module;
enum{PM25,CO2,TEMPERATURE,HUMIDITY,TVOC,HCHO,PM10,SENSOR_COUNT};static daikin::Sample readings[SENSOR_COUNT];static bool sgpReady=false,hchoPresent=false,hchoWaiting=false;static unsigned sgpSince,nextSample,nextSgp,hchoUsed,hchoSent;static uint8_t hchoFrame[9];static const char*pmModel;static DynamicJsonDocument config(2048);static bool due(uint32_t n,uint32_t d){return int32_t(n-d)>=0;}
#include "../src/sensors.inc"
int main(){
 sensorClockSynchronized=true;assert(initSgp(0));assert(sgp.identity);
 readings[TEMPERATURE].set(25,0);readings[HUMIDITY].set(50,0);
 nextSgp=1000;for(unsigned i=1;i<=3600;i++){clockMs=i*1000;readings[TEMPERATURE].at=readings[HUMIDITY].at=clockMs;pollSgp(clockMs);}
 assert(readings[TVOC].received&&readings[TVOC].value==0);assert(sgp.eligible&&Preferences::bytes.size()==24);
 assert(sgp.record.co2==0xabcd&&sgp.record.tvoc==0x1234);
 assert(initSgp(clockMs)&&sgp.restored);auto restore=thBus.writes.back();assert(restore[0]==0x20&&restore[1]==0x1e&&restore[2]==0x12&&restore[3]==0x34&&restore[5]==0xab&&restore[6]==0xcd);
 thBus.serial=456;assert(initSgp(clockMs)&&!sgp.restored);
 thBus.corrupt=true;assert(!initSgp(clockMs));thBus.corrupt=false;
 assert(initSgp(clockMs));sgp.restorePending=false;nextSgp=clockMs+1000;clockMs+=3000;pollSgp(clockMs);assert(!readings[TVOC].received&&!sgp.eligible);
 sensorClockSynchronized=false;assert(initSgp(clockMs)&&!sgp.restored);
 failClear=true;assert(!resetSensorCalibration()&&!Preferences::bytes.empty());failClear=false;assert(resetSensorCalibration()&&Preferences::bytes.empty());assert(resetSensorCalibration()&&Preferences::bytes.empty());
 puts("PASS production SGP loop: hourly persistence, wire order/CRC, identity replacement, zero TVOC, gap and clock gates");
}
