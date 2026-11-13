/* SPDX-License-Identifier: GPL-3.0-or-later */
#include <string>
#include <map>
#include <cstring>
#include <cassert>
#include <cstdio>
#include <cstdlib>
#include <algorithm>
#include <limits>
#include <map>
#include <ArduinoJson.h>
struct String:std::string {using std::string::string;String(const std::string&s):std::string(s){}bool startsWith(const char*p)const{return compare(0,strlen(p),p)==0;}bool reserve(size_t n){std::string::reserve(n);return true;}bool concat(const char*p,size_t n){append(p,n);return true;}};
#include "wire_mp.h"
using esp_err_t=int;static constexpr int ESP_OK=0,ESP_FAIL=-1;
struct httpd_req_t{std::string status,type,body;std::map<std::string,std::string> headers;};
static void httpd_resp_set_hdr(httpd_req_t*r,const char*k,const char*v){r->headers[k]=v;}
static void httpd_resp_set_type(httpd_req_t*r,const char*v){r->type=v;}
static void httpd_resp_set_status(httpd_req_t*r,const char*v){r->status=v;}
static int httpd_resp_send(httpd_req_t*r,const char*p,size_t n){r->body.assign(p,n);return ESP_OK;}
static size_t allocatorLimit=std::numeric_limits<size_t>::max(),largestAllocation=0;
static size_t allocationAttempts=0,allocationDenials=0,peakLiveBlocks=0;
static bool allocationWhileLive=false;
static bool rejectAllAllocations=false;
static std::map<void*,size_t> liveJsonBlocks;
struct BoundedAllocator{
 void* allocate(size_t n){++allocationAttempts;largestAllocation=std::max(largestAllocation,n);if(!liveJsonBlocks.empty())allocationWhileLive=true;if(rejectAllAllocations||n>allocatorLimit){++allocationDenials;return nullptr;}void*p=std::malloc(n);if(p){liveJsonBlocks[p]=n;peakLiveBlocks=std::max(peakLiveBlocks,liveJsonBlocks.size());}return p;}
 void deallocate(void*p){if(p){auto it=liveJsonBlocks.find(p);assert(it!=liveJsonBlocks.end());liveJsonBlocks.erase(it);}std::free(p);}
 void* reallocate(void*p,size_t n){++allocationAttempts;largestAllocation=std::max(largestAllocation,n);if(!liveJsonBlocks.empty()&&!liveJsonBlocks.count(p))allocationWhileLive=true;if(rejectAllAllocations||n>allocatorLimit){++allocationDenials;return nullptr;}if(!p){void*q=std::malloc(n);if(q){liveJsonBlocks[q]=n;peakLiveBlocks=std::max(peakLiveBlocks,liveJsonBlocks.size());}return q;}auto it=liveJsonBlocks.find(p);assert(it!=liveJsonBlocks.end());void*q=std::realloc(p,n);if(q){liveJsonBlocks.erase(it);liveJsonBlocks[q]=n;peakLiveBlocks=std::max(peakLiveBlocks,liveJsonBlocks.size());}return q;}
};
using TestJsonDocument=BasicJsonDocument<BoundedAllocator>;
#ifdef IOTCLOUD_HUB
static void limitAllocator(size_t largest,bool reject=false){assert(liveJsonBlocks.empty());allocatorLimit=largest;largestAllocation=0;allocationAttempts=allocationDenials=peakLiveBlocks=0;allocationWhileLive=false;rejectAllAllocations=reject;}
#endif
#define DynamicJsonDocument TestJsonDocument
#include "response.inc"
#ifdef IOTCLOUD_HUB
#include "mutation.inc"
#endif
#undef DynamicJsonDocument
static StaticJsonDocument<12288> decode(const httpd_req_t&r){assert(r.type=="application/msgpack"&&r.headers.at("X-Content-Type-Options")=="nosniff");StaticJsonDocument<12288>d;assert(!deserializeMsgPack(d,r.body));return d;}
#ifdef IOTCLOUD_HUB
static String packJson(const std::string&json){StaticJsonDocument<8192> d;assert(!deserializeJson(d,json));String bytes;assert(serializeWireMP(d,bytes));return bytes;}
static void assertInvalidParams(httpd_req_t&r){auto d=decode(r);assert(r.status=="400 Bad Request"&&d["code"]=="invalid_params");}
static void assertNoPoolsRemain(){assert(liveJsonBlocks.empty());}
#endif
int main(){
 httpd_req_t r;assert(jsonResponse(&r,"200 OK","{\"value\":0,\"enabled\":false}")==ESP_OK);auto d=decode(r);assert(d.is<JsonObject>()&&d["value"]==0&&d["enabled"]==false);
 r={};assert(jsonResponse(&r,"200 OK","[{\"device_id\":\"02c400000001\"}]")==ESP_OK);d=decode(r);assert(d.is<JsonArray>()&&d.size()==1&&d[0]["device_id"]=="02c400000001");
 r={};assert(jsonResponse(&r,"200 OK","[]")==ESP_OK);d=decode(r);assert(d.is<JsonArray>()&&d.size()==0&&r.body==std::string(1,char(0x90)));
#ifdef IOTCLOUD_HUB
 // The Hub's adaptive pool retries from body length and releases each failed
 // document before asking for the next block. Ten KiB fits; the old 12 KiB
 // fixed allocation below cannot fit on the same fragmented heap.
 std::string json="{\"devices\":[";for(unsigned i=0;i<120;i++){if(i)json+=",";json+="{\"value\":0}";}json+="]}";assert(json.size()>384);
 limitAllocator(10*1024);r={};assert(jsonResponse(&r,"200 OK",json)==ESP_OK);d=decode(r);assert(d.is<JsonObject>()&&d["devices"].size()==120&&d["devices"][119]["value"]==0);
 assert(allocationAttempts>1&&allocationDenials==0&&largestAllocation<=10*1024&&peakLiveBlocks==1&&!allocationWhileLive);assertNoPoolsRemain();
 limitAllocator(10*1024);{TestJsonDocument oldFixed(12288);assert(deserializeJson(oldFixed,json.c_str(),json.size())==DeserializationError::NoMemory);assert(allocationAttempts==1&&allocationDenials==1&&largestAllocation==12288);}assertNoPoolsRemain();
#endif
 r={};assert(jsonResponse(&r,"503 Service Unavailable","{\"code\":\"busy\"}")==ESP_OK);d=decode(r);assert(r.status=="503 Service Unavailable"&&d["code"]=="busy");
 r={};assert(jsonResponse(&r,"200 OK","{\"name\":\"a\\u0000b\"}")==ESP_OK);d=decode(r);assert(d["name"].as<JsonString>().size()==3);
 // An already encoded response must be passed through unchanged.
 String encoded;serializeWireMP(d,encoded);r={};assert(jsonResponse(&r,"503 Service Unavailable",encoded)==ESP_OK);assert(r.body==encoded);
 r={};assert(jsonResponse(&r,"200 OK","[")==ESP_FAIL&&r.body.empty());
#ifdef IOTCLOUD_HUB
 // Exhaust every adaptive pool size. Allocation failure remains an explicit,
 // retryable 503 and does not retain a failed document.
 limitAllocator(0,true);r={};assert(jsonResponse(&r,"200 OK",json)==ESP_OK&&r.status=="503 Service Unavailable");d=decode(r);assert(d["code"]=="busy");assert(allocationDenials>1&&largestAllocation==12288);assertNoPoolsRemain();
 // A real three-field DAIKIN/G3 registration fits a fragmented 16 KiB heap.
 const String registration=packJson(R"({"mac":"9c9c1ff76748","model":"daikin","version":"v2"})");
 limitAllocator(16*1024);r={};String registrationJson=registration;
 assert(decodeHubMutation(registrationJson,false,&r)==ESP_OK&&r.status.empty());
 assert(largestAllocation<=16*1024&&allocationDenials==0);
 DynamicJsonDocument registrationDoc(1024);assert(!deserializeJson(registrationDoc,registrationJson.c_str(),registrationJson.length()));
 assert(registrationDoc.is<JsonObject>()&&registrationDoc.size()==3&&registrationDoc["mac"]=="9c9c1ff76748"&&registrationDoc["model"]=="daikin"&&registrationDoc["version"]=="v2");
 // This reproduces the former unconditional 24 KiB pool request on the same heap.
 limitAllocator(16*1024);TestJsonDocument fixed(24576);
 assert(deserializeMsgPack(fixed,registration.c_str(),registration.length())==DeserializationError::NoMemory);
 assert(largestAllocation>16*1024&&allocationDenials==1);
 // The valid eight-route bridge mutation survives MessagePack -> JSON intact.
 std::string bridge="{\"enabled\":true,\"mappings\":[";
 for(unsigned i=0;i<8;i++){if(i)bridge+=',';bridge+="{\"direction\":\"out\",\"local_topic\":\"local/"+std::to_string(i)+"\",\"remote_topic\":\"remote/"+std::to_string(i)+"\",\"retain\":false,\"allow_retained\":false}";}
 bridge+="]}";const String bridgeMsgpack=packJson(bridge);
 limitAllocator(16*1024);r={};String bridgeBody=bridgeMsgpack;
 assert(decodeHubMutation(bridgeBody,false,&r)==ESP_OK&&r.status.empty());
 DynamicJsonDocument bridgeDoc(8192);assert(!deserializeJson(bridgeDoc,bridgeBody.c_str(),bridgeBody.length()));
 assert(bridgeDoc["enabled"]==true&&bridgeDoc["mappings"].size()==8);
 for(unsigned i=0;i<8;i++){auto route=bridgeDoc["mappings"][i];assert(route["direction"]=="out"&&route["local_topic"]=="local/"+std::to_string(i)&&route["remote_topic"]=="remote/"+std::to_string(i)&&route["retain"]==false&&route["allow_retained"]==false);}
 String bridgeRoundTrip;serializeJson(bridgeDoc,bridgeRoundTrip);assert(bridgeRoundTrip==bridge);
 // Heap exhaustion maps to a retryable busy response; malformed/non-map inputs stay client errors.
 limitAllocator(16*1024,true);r={};String rejected=registration;
 assert(decodeHubMutation(rejected,false,&r)==ESP_OK&&r.status=="503 Service Unavailable"&&allocationDenials==1);
 d=decode(r);assert(d["code"]=="busy");
 limitAllocator(16*1024);r={};String malformed(1,char(0xc1));
 assert(decodeHubMutation(malformed,false,&r)==ESP_OK);assertInvalidParams(r);
 r={};String nonObject=packJson("[1,2,3]");
 assert(decodeHubMutation(nonObject,false,&r)==ESP_OK);assertInvalidParams(r);
 // Device-command MessagePack is opaque here, including binary payload bytes.
 const String binary(std::string("\xc4\x03\x00\xc1\xff",5));limitAllocator(0,true);r={};String command=binary;
 assert(decodeHubMutation(command,true,&r)==ESP_OK&&command==binary&&largestAllocation==0&&r.status.empty());
 puts("Hub HTTP emitter and extracted mutation decode: adaptive pool, G3 registration, fixed 24 KiB failure, 8-route round-trip, 503 busy, malformed/nonobject 400, binary command pass-through PASS");
 limitAllocator(std::numeric_limits<size_t>::max());
#endif
 // Preserve the original full/G3 dynamic object and array emitter coverage.
 std::string object="{\"devices\":[";for(unsigned i=0;i<80;i++){if(i)object+=',';object+="{\"value\":0}";}object+="]}";
 r={};assert(jsonResponse(&r,"200 OK",object)==ESP_OK);d=decode(r);assert(d.is<JsonObject>()&&d["devices"].size()==80&&d["devices"][79]["value"]==0);
 String large="[";for(unsigned i=0;i<80;i++){if(i)large+=",";large+="{\"value\":0}";}large+="]";r={};assert(jsonResponse(&r,"200 OK",large)==ESP_OK);d=decode(r);assert(d.is<JsonArray>()&&d.size()==80&&d[79]["value"]==0);
 puts("production HTTP emitter: object/array/empty/error/binary pass-through PASS");
}
