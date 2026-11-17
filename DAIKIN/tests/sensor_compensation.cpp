#include "sensor_compensation.h"
#include "climate.h"
#include <assert.h>
#include <stdio.h>
int main(){
  // Fixed vectors independently evaluated from the recovered stock constants.
  assert(daikin::compensatePm25(0,25,true)==6);
  assert(daikin::compensatePm25(42,28,true)==48);
  assert(daikin::compensatePm25(100,33,true)==108);
  assert(daikin::compensatePm25(1000,28,true)==300);
  assert(daikin::compensatePm25(65535,60,true)==300);
  assert(daikin::compensateCo2(250,33,true)==400);
  assert(daikin::compensateCo2(1000,28,true)==1000);
  assert(daikin::compensateCo2(1000,33,true)==950);
  assert(daikin::compensateCo2(10000,33,true)==2850);
  assert(daikin::compensateCo2(1000,125,true)==400);
  assert(fabsf(daikin::compensateHcho(32)-0.0295f)<1e-6f);
  assert(fabsf(daikin::compensateHcho(60)-0.075f)<1e-6f);
  assert(fabsf(daikin::compensateHcho(61)-0.0753125f)<1e-6f);
  assert(daikin::compensateHcho(0)==0.01f&&daikin::compensateHcho(65535)==0.5f);
  uint32_t mg;uint16_t word;float t,h;
  assert(daikin::compensateClimate(30,40,t,h));
  assert(daikin::absoluteHumidity(t,h,mg,word)&&mg==11290&&word==2890);
  assert(daikin::absoluteHumidity(25,50,mg,word)&&mg==11483&&word==2939);
  assert(daikin::absoluteHumidity(25,0,mg,word)&&mg==0&&word==0);
  assert(!daikin::absoluteHumidity(NAN,50,mg,word));
  assert(daikin::absoluteHumidity(125,100,mg,word)&&word==65535);
  daikin::PmPower p;
  assert(p.desired(0,false)==5&&p.maxAgeMs()==120000);
  p.acknowledged(5,0);assert(!p.canRead(35999)&&p.canRead(36000));
  assert(p.desired(1000,true)==1&&p.maxAgeMs()==3600000);
  p.acknowledged(1,1000);assert(!p.canRead(37000));
  for(unsigned i=1;i<10;i++)assert(p.desired(1000+i*1000,true)==1);
  assert(p.desired(3540999,true)==1);
  assert(p.desired(3541000,true)==5);p.acknowledged(5,3541000);
  assert(!p.canRead(3576999)&&p.canRead(3577000));
  assert(p.desired(3599999,true)==5&&p.desired(3600000,true)==1);
  assert(p.desired(3541000+3*3540000+100000,true)==1); // missed windows do not replay
  assert(p.desired(20000000,false)==5);
  assert(p.desired(20000001,true)==1); // re-enter starts a new quiet interval
  daikin::PmPower wrap;
  assert(wrap.desired(0xfffffff0U,true)==1);
  assert(wrap.desired(uint32_t(0xfffffff0U+3540000U),true)==5);
  assert(daikin::PmPower::nightAt(true,1380,420,480,true,15*3600));
  assert(!daikin::PmPower::nightAt(true,1380,420,480,true,23*3600));
  assert(!daikin::PmPower::nightAt(false,1380,420,480,true,15*3600));
  assert(!daikin::PmPower::nightAt(true,1380,420,480,false,15*3600));
  assert(!daikin::PmPower::nightAt(true,300,300,480,true,15*3600));
  assert(daikin::PmPower::nightAt(true,600,660,0,true,10*3600));
  assert(!daikin::PmPower::nightAt(true,600,660,0,true,11*3600));
  daikin::SgpBaseline b;b.serial[0]=1;b.serial[1]=2;b.serial[2]=3;b.feature=0x22;b.co2=0xabcd;b.tvoc=0x1234;b.savedUtc=1700000000;
  uint8_t bytes[24];b.encode(bytes);daikin::SgpBaseline restored;
  assert(restored.decode(bytes,24));
  assert(restored.matches(b.serial,0x22,true,b.savedUtc+604800));
  assert(!restored.matches(b.serial,0x22,true,b.savedUtc+604801));
  assert(!restored.matches(b.serial,0x22,false,b.savedUtc));
  assert(!restored.matches(b.serial,0x22,true,b.savedUtc-1));
  assert(!restored.matches(b.serial,0x21,true,b.savedUtc));
  uint16_t replacement[]={1,2,4};assert(!restored.matches(replacement,0x22,true,b.savedUtc));
  assert(restored.co2==b.co2&&restored.tvoc==b.tvoc);
  bytes[3]=2;assert(!restored.decode(bytes,24));assert(!restored.decode(bytes,23));
  puts("PASS OEM compensation, SGP AH/baseline binding, PM power windows/clock/wraparound");
}
