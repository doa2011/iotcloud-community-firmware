/* SPDX-License-Identifier: GPL-3.0-or-later */
#include "hub_lan_client.h"
#include <atomic>
#include <cstdlib>
#include <memory>
#include <vector>
#include <deque>
#include <algorithm>
#include <cassert>
#include <cstdio>
#include <new>
using String=std::string;using esp_mqtt_client_handle_t=void*;using QueueHandle_t=void*;
#include "globals.inc"
static size_t messageSize=sizeof(HubMessage),liveMessageBytes=0,peakMessageBytes=0;
struct MessageAllocation {void *pointer;size_t bytes;};
static MessageAllocation messageAllocations[16];
void *operator new(size_t n){
 void *p=std::malloc(n?n:1);if(!p)throw std::bad_alloc();
 if(n==messageSize){bool saved=false;for(auto &a:messageAllocations)if(!a.pointer){a.pointer=p;a.bytes=n;saved=true;break;}if(!saved)std::abort();liveMessageBytes+=n;peakMessageBytes=std::max(peakMessageBytes,liveMessageBytes);}
 return p;
}
void operator delete(void *p)noexcept{
 if(!p)return;
 for(auto &a:messageAllocations)if(a.pointer==p){liveMessageBytes-=a.bytes;a.pointer=nullptr;a.bytes=0;break;}
 std::free(p);
}
void *operator new(size_t n,const std::nothrow_t &)noexcept{try{return ::operator new(n);}catch(...){return nullptr;}}
void operator delete(void *p,const std::nothrow_t &)noexcept{::operator delete(p);}
static std::deque<HubMessage*> queue;
static std::vector<std::string> events;
static bool mqttRestart=false,wifiRestart=false,configRecovery=false;
struct HttpCommand{void cancel(const char*){events.push_back("http-cancel");}} hubHttpCommand;
static iotcloud::HubLanClient client;
static unsigned sent=0,completed=0;
static void hubLanReset(){events.push_back("udp-reset");client.reset();}
static void homeQuiesce(){events.push_back("home-quiesce");}
static void homeChanged(){events.push_back("home-changed");}
struct Service{bool ready(){return true;}void clearSessions(){events.push_back("sessions-clear");}} hubService;
struct Bus{void(*dispatch)()=nullptr;} hubDeviceBus;
static std::vector<std::string> delivered;
struct Relay{
 std::string status;
 void reset(){events.push_back("relay-reset");}
 void tick(uint32_t){}
 void incoming(const char*,const std::string &body,bool){assert(liveMessageBytes>=(queue.size()+1)*messageSize);delivered.push_back(body);}
 void incomingBemfa(const char*,const std::string &body,bool){assert(liveMessageBytes>=(queue.size()+1)*messageSize);delivered.push_back(body);}
} hubRelay;
static size_t availableHeap=size_t(-1),largestBlock=size_t(-1);
static bool hubAllocationReady(size_t n){return n<=size_t(-1)-16384&&availableHeap>n+16384&&largestBlock>n;}
static const int pdTRUE=1;
static unsigned pdMS_TO_TICKS(unsigned ms){return ms;}
static int xQueueSend(QueueHandle_t,HubMessage *const *p,int wait){
 assert(wait==50);
 if(queue.size()==3)return 0;
 queue.push_back(*p);return pdTRUE;
}
static int xQueueReceive(QueueHandle_t,HubMessage **p,int wait){
 assert(wait==0);if(queue.empty())return 0;*p=queue.front();queue.pop_front();return pdTRUE;
}
static unsigned uxQueueMessagesWaiting(QueueHandle_t){return queue.size();}
static void xQueueReset(QueueHandle_t){events.push_back("inbox-reset");assert(queue.empty());}
struct Event {int current_data_offset=0,topic_len=0,total_data_len=0,data_len=0;const char *topic=nullptr,*data=nullptr;bool retain=false;};
using esp_mqtt_event_handle_t=Event*;
#include "capture.inc"
static void stopMqtt(){events.push_back("mqtt-join");}
static void stopBemfa(){events.push_back("bemfa-join");bemfaConnected=false;bemfaTopics.clear();}
static void startBemfa(){events.push_back("bemfa-start");}
#include "lifecycle.inc"
static bool otaActive=false,mqttOnline=false,haBirth=false;
static uint32_t nextPublish=0;
static void pollHubLan(uint32_t){}
#include "reload.inc"
static void noop(){}
static void begin(){uint8_t token[16]={};assert(client.begin("02c400000001",7,"02c400000002","10.0.0.2",11288,token,"route",std::string(1,char(0x80)),1));assert(client.busy());
 // Hold an owned, partially received 1000-byte response before lifecycle reset.
 uint8_t chunk[LAN_V4_CHUNK]={};lan_v4_packet part={};part.op=LAN_V4_RESULT;strcpy(part.src,"02c400000002");strcpy(part.dst,"02c400000001");strcpy(part.code,"ok");part.id=client.lastId;part.request_boot=7;part.boot=9;part.total=2;part.size=1000;part.body=chunk;part.body_length=sizeof(chunk);assert(client.receive(part,"10.0.0.2",11288,2));assert(client.busy());}
static void seedInbox(){
 hubInbox=reinterpret_cast<void*>(1);hubRemoteTopics={"up"};bemfaTopics={"bemfa"};
 for(unsigned i=0;i<3;i++){
  unsigned source=i%2;const char *topic=source?"bemfa":"up";Event event;event.topic=topic;event.topic_len=strlen(topic);event.total_data_len=event.data_len=1;event.data="x";assert(hubCapture(&event,source));
 }
 for(unsigned source=0;source<2;source++){
  const char *topic=source?"bemfa":"up";Event event;event.topic=topic;event.topic_len=strlen(topic);event.total_data_len=2;event.data="x";event.data_len=1;assert(hubCapture(&event,source));
  assert(hubMessages[source]&&hubMessages[source]->length==1);
 }
 assert(queue.size()==3&&liveMessageBytes==5*messageSize);
}
static size_t index(const char *event){auto i=std::find(events.begin(),events.end(),event);assert(i!=events.end());return size_t(i-events.begin());}
int main(){
 // A full inbox rejects the fourth allocation without taking ownership. The
 // poll path transfers one pointer into unique_ptr and frees it after use.
 hubInbox=reinterpret_cast<void*>(1);hubRemoteTopics={"up"};
 Event burst;burst.topic="up";burst.topic_len=2;burst.total_data_len=burst.data_len=1;burst.data="x";
 for(unsigned i=0;i<3;++i)assert(hubCapture(&burst,0));
 assert(queue.size()==3&&!hubInboxDropped&&liveMessageBytes==3*messageSize);
 burst.data="y";assert(hubCapture(&burst,0));
 assert(queue.size()==3&&hubInboxDropped==1&&!hubMessages[0]&&liveMessageBytes==3*messageSize);
 availableHeap=messageSize+16384;pollHub(1);
 assert(queue.size()==3&&liveMessageBytes==3*messageSize&&delivered.empty());
 availableHeap=size_t(-1);largestBlock=messageSize;pollHub(2);
 assert(queue.size()==3&&liveMessageBytes==3*messageSize&&delivered.empty());
 availableHeap=messageSize+16385;largestBlock=messageSize+1;pollHub(3);
 assert(queue.size()==2&&liveMessageBytes==2*messageSize&&delivered.size()==1&&delivered[0]=="x");
 availableHeap=largestBlock=size_t(-1);hubResetInbox();
 assert(queue.empty()&&!liveMessageBytes&&hubEpoch==1&&hubInboxDropped==1);
 hubInboxDropped=0;events.clear();

 (void)bemfaMqtt;hubUpstreamChanged();assert(hubReload&&!mqttRestart);events.clear();
 client.authorize=[](const std::string&,const std::string&,uint16_t,const uint8_t*){return true;};
 client.send=[](const std::string&,uint16_t,const void*,size_t){++sent;return true;};
 client.complete=[](const std::string&,const void*,size_t){++completed;};
 seedInbox();begin();uint32_t first=client.lastId,epoch=hubEpoch;hubReload=true;hubCredentials=true;pollHub(2);
 assert(!client.busy()&&client.lastId==first&&hubEpoch==epoch+1&&queue.empty()&&!hubMessages[0]&&!hubMessages[1]&&!liveMessageBytes);
 assert(index("mqtt-join")<index("inbox-reset")&&index("bemfa-join")<index("inbox-reset")&&index("inbox-reset")<index("bemfa-start"));
 for(unsigned source=0;source<2;source++){Event tail;tail.current_data_offset=1;tail.total_data_len=2;tail.data_len=1;tail.data="y";assert(!hubCapture(&tail,source));}assert(queue.empty());
 unsigned sends=sent;client.tick(1000);assert(sent==sends&&completed==0);assert(mqttRestart&&!hubReload&&!hubCredentials);
 // A completion from the retired transaction cannot be delivered after reset.
 lan_v4_packet packet={};packet.op=LAN_V4_RESULT;strcpy(packet.src,"02c400000002");strcpy(packet.dst,"02c400000001");packet.id=first;packet.request_boot=7;assert(!client.receive(packet,"10.0.0.2",11288,1001));
 events.clear();seedInbox();begin();assert(client.lastId>first);epoch=hubEpoch;hubDeviceBus.dispatch=noop;hubReload=hubBirth=hubCredentials=wifiRestart=true;hubRestoreChanged();
 assert(hubRestoring&&!client.busy()&&client.lastId>first&&hubEpoch==epoch+1&&queue.empty()&&!hubMessages[0]&&!hubMessages[1]&&!liveMessageBytes);
 assert(!hubDeviceBus.dispatch&&hubRemoteTopics.empty()&&!hubReload&&!hubBirth&&!hubCredentials&&!wifiRestart&&!mqttRestart);
 assert(index("home-quiesce")<index("udp-reset")&&index("http-cancel")<index("udp-reset")&&index("mqtt-join")<index("inbox-reset")&&index("bemfa-join")<index("inbox-reset"));
 Event event;event.topic="up";event.topic_len=2;event.total_data_len=event.data_len=1;event.data="x";assert(!hubCapture(&event,0));assert(!hubCapture(&event,1));sends=sent;client.tick(2000);pollHub(2000);assert(sent==sends&&queue.empty()&&completed==0&&!liveMessageBytes);
 assert(peakMessageBytes==5*messageSize);
 printf("production UDP lifecycle PASS: pointer inbox sizeof=%zu peak_owned=%zu, reload/restore drain, low-memory retention\n",messageSize,peakMessageBytes);
}
