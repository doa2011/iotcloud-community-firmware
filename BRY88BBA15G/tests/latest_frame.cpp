extern "C" {
#include "protocol/media_wire.h"
}
#include "screen_latest_frame.h"
#include <assert.h>
#include <vector>
#include <algorithm>
static uint8_t decoded[MEDIA_JPEG_MAX];
static uint32_t clockMs, lastSequence, maxAge, count;
static void presented(void *, size_t n) {
  assert(n >= 16);
  uint32_t sequence = media_u32(decoded + 6), at = media_u32(decoded + 10);
  assert(sequence > lastSequence);
  lastSequence = sequence; count++;
  maxAge = std::max(maxAge, clockMs - at);
}
static std::vector<uint8_t> jpeg(unsigned seq, uint32_t at, size_t size = 8500) {
  std::vector<uint8_t> p(size, 42);
  p[0]=255;p[1]=216;p[2]=255;p[3]=254;
  p[4]=(size-6)>>8;p[5]=(size-6)&255;
  media_put32(p.data()+6,seq);media_put32(p.data()+10,at);
  p[size-2]=255;p[size-1]=217;
  return p;
}
static void feed(ScreenLatestFrame &s, unsigned seq, uint32_t at, size_t size=8500) {
  auto p=jpeg(seq,at,size);
  char h[100];int n=snprintf(h,sizeof(h),"--camera\r\nContent-Type: image/jpeg\r\nContent-Length: %zu\r\n\r\n",size);
  s.feed(reinterpret_cast<const uint8_t *>(h),n,at);
  for(size_t i=0;i<p.size();i+=317)s.feed(p.data()+i,std::min(size_t(317),p.size()-i),at);
  s.feed(reinterpret_cast<const uint8_t *>("\r\n"),2,at);
}
static void begin(ScreenLatestFrame &s) {
 const char *h="HTTP/1.1 200 OK\r\nContent-Type: multipart/x-mixed-replace; boundary=camera\r\n\r\n";
 s.feed(reinterpret_cast<const uint8_t *>(h),strlen(h),0);
}
int main() {
 ScreenLatestFrame s;begin(s);
 relay_mjpeg_parser sink={};sink.image=decoded;sink.frame=presented;relay_mjpeg_init(&sink);
 uint8_t block[512];unsigned sequence=0;uint32_t nextRead=0;
 for(clockMs=0;clockMs<=600000;clockMs++) {
  if(clockMs%1000==0) feed(s,++sequence,clockMs);
  if(clockMs>=nextRead) {
   s.ack(clockMs);size_t n=s.read(block,sizeof(block));
   if(n)relay_mjpeg_feed(&sink,block,n);
   nextRead=clockMs+64;
  }
  assert(!s.error()&&!sink.error&&s.bytes()<=32768);
 }
 assert(count>400&&count<550&&s.replaced>50&&maxAge<4000);
 printf("10min 1fps input / ~0.82fps consumer: produced=%u presented=%u replaced=%u max-source-age=%ums peak=%zuB\n",sequence,count,s.replaced,maxAge,s.peakBytes);
 s.clear();begin(s);lastSequence=count=maxAge=0;relay_mjpeg_init(&sink);
 for(unsigned i=1;i<=100;i++)feed(s,i,clockMs);
 assert(s.replaced>=99);
 for(unsigned i=0;i<100;i++){s.ack(clockMs);size_t n=s.read(block,sizeof(block));if(n)relay_mjpeg_feed(&sink,block,n);}
 assert(count==1&&lastSequence==100&&!sink.error);
 // Scratch allocation exhaustion drops a whole frame; the last complete
 // pending image survives and the next smaller image can still replace it.
 ScreenLatestFrame small(4096);begin(small);feed(small,1,0,1800);
 feed(small,2,0,8500);assert(small.rejected==1&&small.received==1);
 feed(small,3,0,1800);assert(small.received==2&&small.replaced==1&&small.bytes()<=4096);
 ScreenLatestFrame fail(32768,[](size_t){return false;});begin(fail);feed(fail,1,0);
 assert(fail.rejected==1&&!fail.error()&&fail.bytes()==0);
 // Two large frames fit but a third scratch image cannot. Once the current
 // frame is ACKed, recycle its empty slot so larger frames resume indefinitely.
 ScreenLatestFrame large;begin(large);relay_mjpeg_init(&sink);lastSequence=count=maxAge=0;
 for(clockMs=0;clockMs<30000;clockMs++) {
  if(clockMs%1000==0)feed(large,clockMs/1000+1,clockMs,15000);
  if(clockMs%64==0){large.ack(clockMs);size_t n=large.read(block,sizeof(block));if(n)relay_mjpeg_feed(&sink,block,n);}
  assert(!large.error()&&large.bytes()<=32768);
 }
 assert(large.rejected>0&&count>8&&lastSequence>25);
 printf("15KiB two-slot pressure: presented=%u rejected=%u last-sequence=%u peak=%zuB; continued after starvation boundary\n",count,large.rejected,lastSequence,large.peakBytes);
 // 401 is passed as a normalized challenge so Basic/Digest stays MCU-owned.
 ScreenLatestFrame auth;const char *a="HTTP/1.1 401 Unauthorized\r\nWWW-Authenticate: Digest realm=\"test\", nonce=\"n\", qop=\"auth\"\r\n\r\n";
 auth.feed(reinterpret_cast<const uint8_t *>(a),strlen(a),0);
 size_t n=auth.read(block,sizeof(block));assert(n&&strstr(reinterpret_cast<char *>(block),"Digest realm=\"test\""));
 // Chunked input without per-JPEG Content-Length, every chunk and wire
 // header split at single-byte boundaries; the output parser sees intact JPEG.
 ScreenLatestFrame chunked;relay_mjpeg_init(&sink);lastSequence=count=maxAge=0;
 const char *ch="HTTP/1.1 200 OK\r\nContent-Type: multipart/x-mixed-replace; boundary=camera\r\nTransfer-Encoding: chunked\r\n\r\n";
 for(size_t i=0;i<strlen(ch);i++)chunked.feed(reinterpret_cast<const uint8_t *>(ch+i),1,clockMs);
 auto picture=jpeg(123,clockMs);std::vector<uint8_t> wire;
 const char *part="--camera\r\nContent-Type: image/jpeg\r\n\r\n";
 wire.insert(wire.end(),part,part+strlen(part));wire.insert(wire.end(),picture.begin(),picture.end());
 for(size_t at=0;at<wire.size();) {
  size_t take=std::min(size_t(37),wire.size()-at);char line[20];int len=snprintf(line,sizeof(line),"%zx;ext=ok\r\n",take);
  for(int i=0;i<len;i++)chunked.feed(reinterpret_cast<const uint8_t *>(line+i),1,clockMs);
  for(size_t i=0;i<take;i++)chunked.feed(wire.data()+at+i,1,clockMs);
  chunked.feed(reinterpret_cast<const uint8_t *>("\r\n"),2,clockMs);at+=take;
 }
 chunked.feed(reinterpret_cast<const uint8_t *>("0\r\n\r\n"),5,clockMs);
 assert(!chunked.error()&&chunked.ended()&&chunked.received==1);
 for(unsigned i=0;i<30;i++){chunked.ack(clockMs);size_t n=chunked.read(block,sizeof(block));if(n)relay_mjpeg_feed(&sink,block,n);}
 assert(count==1&&lastSequence==123&&!sink.error);
 // Broken JPEG is not published; stop/cancel releases all three slots.
 ScreenLatestFrame bad;begin(bad);const char *b="--camera\r\nContent-Type: image/jpeg\r\nContent-Length: 4\r\n\r\nnope";
 bad.feed(reinterpret_cast<const uint8_t *>(b),strlen(b),0);assert(bad.error());
 s.clear();assert(s.bytes()==0&&s.empty());
 puts("burst latest-only, budget exhaustion/recovery, allocation failure, auth, bad JPEG and cancellation passed");
}
