#include "screen_jpeg_upload.h"
#include <cassert>
#include <cstring>
int main(){
 uint8_t h[16]={};media_put32(h,7);media_put32(h+4,47732);
 media_put32(h+8,0x12345678);h[12]=480&255;h[13]=480>>8;h[14]=854&255;h[15]=854>>8;
 ScreenJpegHeader x;assert(x.parse(h,47732+16));
 assert(!x.parse(h,47732+15));assert(x.parse(h,47732+16));
 uint8_t p[MEDIA_CHUNK+9];x.begin(p);assert(p[0]==MEDIA_BEGIN&&media_u32(p+1)==7);
 unsigned blocks=0;uint32_t offset=0;
 while(offset<x.total){size_t n=x.chunkSize(offset);assert(n&&n<=512);x.chunk(p,offset);
 assert(p[0]==MEDIA_WRITE&&media_u32(p+5)==offset);offset+=n;++blocks;}
 assert(blocks==94&&offset==47732&&x.chunkSize(offset)==0);
 uint8_t slot[17]={};memcpy(slot,h,8);media_put32(slot+8,47732);media_put32(slot+12,x.crc);slot[16]=2;
 assert(x.matches(slot,47732,2));assert(!x.matches(slot,47731,2));assert(!x.matches(slot,47732,1));
 slot[12]^=1;assert(!x.matches(slot,47732,2));
 x.terminal(p,false);assert(p[0]==MEDIA_COMMIT);x.terminal(p,true);assert(p[0]==MEDIA_CANCEL);
 uint8_t batch[24]={};memcpy(batch,h,16);media_put32(batch+20,123);
 ScreenAlbumBatch y;assert(y.parse(batch,24+4096)&&y.offset==0&&y.epoch==123);
 media_put32(batch+16,45056);assert(y.parse(batch,24+2676));
 assert(!y.parse(batch,24+2675));assert(!y.parse(batch,24+4097));
 media_put32(batch+16,1);assert(!y.parse(batch,24+512));
 media_put32(batch+16,0);media_put32(batch+20,0);assert(!y.parse(batch,24+512));
 h[12]=255;h[13]=255;assert(!x.parse(h,47732+16));
}
