#include "frames.h"
#include <assert.h>
#include <stdio.h>
int main(){
 uint16_t value=0;
 uint8_t co2[]={0x16,5,1,3,0xe8,0,0,0xf9};
 assert(daikin::co2(co2,8,value)&&value==1000);
 co2[7]^=1;assert(!daikin::co2(co2,8,value));assert(!daikin::co2(co2,7,value));
 uint8_t ch2o[]={0xff,0x86,0,100,0,0,0,80,0};
 ch2o[8]=0-(ch2o[1]+ch2o[3]+ch2o[7]);
 assert(daikin::ch2o(ch2o,9,value)&&value==80);
 ch2o[8]^=1;assert(!daikin::ch2o(ch2o,9,value));
 ch2o[8]^=1;ch2o[1]=0x17;assert(!daikin::ch2o(ch2o,9,value));
 daikin::Particulates particles;
 uint8_t pm2105[22]={0x16,22,0x80,0,0,0,42,1,44};
 for(unsigned i=0;i<21;i++)pm2105[21]^=pm2105[i];
 assert(daikin::pm(pm2105,22,false,particles)&&particles.pm25==42&&particles.pm10==300);
 assert(!daikin::pm(pm2105,22,true,particles));
 assert(!daikin::pm(pm2105,12,false,particles));
 for(unsigned i=0;i<22;i++){pm2105[i]^=1;assert(!daikin::pm(pm2105,22,false,particles));pm2105[i]^=1;}
 uint8_t pm2005[23]={0};for(unsigned i=0;i<22;i++)pm2005[i+1]=pm2105[i];
 assert(daikin::pm(pm2005,23,true,particles)&&particles.pm25==42&&particles.pm10==300);
 pm2005[3]=3;pm2005[22]^=0x80^3;
 assert(daikin::pmFrame(pm2005,23,true,particles)&&particles.status==3);
 assert(!daikin::pm(pm2005,23,true,particles));
 const uint8_t query[]={0xff,0x86,0,0x28,0,0,0,0x20,0x32};
 assert(daikin::ch2o(query,9,value)&&value==32);
 daikin::Sample s;assert(!s.valid(0));s.set(0,10);assert(s.valid(11));assert(!s.valid(20010));
 s.max_age_ms=120000;s.set(2,0);assert(s.valid(119999)&&!s.valid(120000));
 s.set(1,0xfffffff0);assert(s.valid(20));
 puts("PASS: sensor frame validation, variant offsets, freshness and wraparound");
}
