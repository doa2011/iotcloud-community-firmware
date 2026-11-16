#include "product_name.h"
#include <Arduino.h>
#include <ArduinoJson.h>
#include <cassert>
#include <functional>
#include <thread>
#include "wire_mp.h"
#include "http_read_snapshot.h"
#include "auth_storage_diagnostics.h"
extern "C" {
#include "typed_wire.h"
}
static AuthStorageDiagnostics authStorageSnapshot(){return {};}
ARDUINOJSON_BEGIN_PRIVATE_NAMESPACE
namespace string_traits_impl {template <> struct has_length<::String, void> : true_type {};}
ARDUINOJSON_END_PRIVATE_NAMESPACE
#include "hub_service.h"
#include "frames.h"
#include "cm1106.h"
#include "indicator_json.h"
static uint32_t clockMs = 240000;
static uint32_t millis() { return clockMs; }
enum Sensor { PM25, CO2, TEMPERATURE, HUMIDITY, TVOC, HCHO, PM10, SENSOR_COUNT };
static daikin::Sample readings[SENSOR_COUNT];
static const char *keys[] = {"pm25", "co2", "temperature", "humidity", "tvoc", "formaldehyde", "pm10"};
static const char *units[] = {"µg/m³", "ppm", "°C", "%", "ppb", "mg/m³", "µg/m³"};
static bool hchoPresent = true, indicator = true, configRecovery = false, hubRestoring = false;
static bool customLightSettings = false;
static daikin::LightSettings lightSettings = daikin::defaultLightSettings();
static bool sensorPresent(unsigned i) { return i == HCHO ? hchoPresent : i != PM10 || readings[PM10].received; }
static daikin::CM1106 co2Module;
static const char *hardwareModel = "BRY88AB151K", *pmModel = "PM2105", *version = "1.0.0-native-test";
static constexpr const char *DAIKIN_VARIANT = "hub";
static String deviceId("aabbccddeeff"), apName("DAIKIN-Test"), mqttRoot, sensorRoot;
static DynamicJsonDocument config(16384);
static iotcloud::HubService hubService(config);
static bool hubReady(){return !hubRestoring&&!configRecovery&&hubService.ready();}
static constexpr unsigned MALLOC_CAP_INTERNAL=2048,MALLOC_CAP_8BIT=4;
static size_t heap_caps_get_free_size(unsigned){return 200000;}
static size_t heap_caps_get_largest_free_block(unsigned){return 60000;}
static size_t heap_caps_get_minimum_free_size(unsigned){return 150000;}
// Hardware/SDK boundaries supply ordinary fixed state, not replacement JSON.
struct { bool getBool(const char *, bool value) const { return value; } } prefs;
struct {
  unsigned getFreeHeap() const { return 200000; }
  unsigned getMinFreeHeap() const { return 150000; }
  unsigned getMaxAllocHeap() const { return 60000; }
} ESP;
enum { WL_CONNECTED = 3, WIFI_AP = 2, LOW = 0 };
struct HostIP { String toString() const { return String("192.0.2.10"); } };
struct {
  int status() const { return WL_CONNECTED; }
  HostIP localIP() const { return {}; }
  HostIP softAPIP() const { return {}; }
  int RSSI() const { return -42; }
  unsigned getMode() const { return 1; }
} WiFi;
static void (*duringStateBuild)()=nullptr;
static int digitalRead(int) { if(duringStateBuild)duringStateBuild();return 1; }
static void digitalWrite(unsigned, bool) {}
static unsigned esp_reset_reason() { return 1; }
struct Partition { const char *label; unsigned type, subtype, address, size; };
static const Partition partitions[] = {
  {"nvs", 1, 2, 0x9000, 0x4000}, {"app0", 0, 16, 0x10000, 0x180000},
  {"app1", 0, 17, 0x190000, 0x180000}};
static constexpr unsigned ESP_PARTITION_TYPE_ANY = 255, ESP_PARTITION_SUBTYPE_ANY = 255;
static const Partition *esp_ota_get_running_partition() { return &partitions[1]; }
static const Partition *esp_ota_get_next_update_partition(void *) { return &partitions[2]; }
static const Partition *esp_partition_find(unsigned, unsigned, void *) { return partitions; }
static const Partition *esp_partition_get(const Partition *p) { return p; }
static const Partition *esp_partition_next(const Partition *p) { return p == &partitions[2] ? nullptr : p + 1; }
static void esp_partition_iterator_release(const Partition *) {}
static int mqttLastEvent = 1, mqttErrorType = 0, mqttSocketError = 0;
static bool mqttConnected = true;
#ifdef IOTCLOUD_BRY88_SCREEN
static bool mqttCommandReady = true;
static void screenStatus(JsonObject out);
static void screenPowerStatus(JsonObject,bool=false) {}
#endif
#include "indicators.inc"
static void sensorDiagnostics(JsonObject) {}
#include "climate.h"
#include "i18n.h"
#include "config_validation.inc"
#include "state.inc"
#include "public_read.inc"
#include "account_scope.inc"

#ifdef IOTCLOUD_BRY88_SCREEN
extern "C" {
#include "../../../BRY88BBA15G/protocol/screen_caps.h"
#include "../../../BRY88BBA15G/protocol/scene.h"
}
static struct {bool extended=true;uint32_t session=1,capabilities=SCREEN_CAP_BACKLIGHT|SCREEN_CAP_SCENE_V3|SCREEN_CAP_RESOURCE_IO|SCREEN_CAP_MODULE_CONTROL;} screenBridge;
static struct {bool active=false;} screenImage;
static struct {unsigned mode=0;} screenGuard;
static bool screenRecovering=false;
static bool screenGuardAvailable(){return true;}
static void delay(unsigned n){clockMs+=n;}
static bool screenFields(JsonObjectConst,const char *){return true;}
static bool bry_bridge_send(void *,const void *,size_t,unsigned){return true;}
#include "../../../BRY88BBA15G/screen_brightness.inc"
#include "../../../BRY88BBA15G/screen_module.inc"
static void screenStatus(JsonObject out){
 out["sensor_source"]="screen_mcu";
 JsonObject screen=out.createNestedObject("screen");
 screenBrightnessStatus(screen.createNestedObject("brightness"));
 screenResourceStatus(screen.createNestedObject("resources"));
 screenModuleStatus(screen.createNestedObject("module"));
 if(duringStateBuild)duringStateBuild();
}
static void telemetryKeepsPublication(){
 auto receive=[&](const std::function<void()> &callback){
  String before,copy;assert(businessReadSnapshot.copy(before,false,clockMs));
  const unsigned builds=businessStatusBuilds;
  callback();assert(businessStatusInvalid);
  assert(businessReadSnapshot.copy(copy,false,clockMs)&&copy==before);
  duringStateBuild=[](){String previous;assert(businessReadSnapshot.copy(previous,false,clockMs));};
  assert(pollBusinessStatusCache(clockMs));duringStateBuild=nullptr;
  assert(businessStatusBuilds==builds+1);
 };
 uint8_t backlight[SCREEN_BACKLIGHT_BYTES];
 assert(screen_backlight_encode(backlight,60,10,10,false));
 receive([&](){assert(screenBrightnessReceive(backlight,sizeof(backlight)));});
 DynamicJsonDocument doc(8192);state(doc.to<JsonObject>());
 assert(doc["screen"]["brightness"]["available"]==true&&doc["screen"]["brightness"]["applied_percent"]==10);
 uint8_t bank[24]={0x2a,2,2,0,1,0,0,0,57,0,0,0,0x12,0x34,0x56,0x78};
 receive([&](){screenResourceReceive(bank,sizeof(bank));});
 doc.clear();state(doc.to<JsonObject>());
 assert(doc["screen"]["resources"]["available"]==true&&doc["screen"]["resources"]["glyphs"]==1);
 uint8_t module[20]={SCREEN_MSG_MODULE_STATUS,1,0,5};
 scene_put32(module+8,10000);
 receive([&](){screenModuleReceive(module,sizeof(module));});
 doc.clear();state(doc.to<JsonObject>());
 assert(doc["screen"]["module"]["available"]==true&&doc["screen"]["module"]["set_high"]==false);
 // An ordinary telemetry refresh must not weaken invalid-sample withdrawal.
 readings[PM25].received=false;
 duringStateBuild=[](){String previous;assert(!businessReadSnapshot.copy(previous,false,clockMs));};
 assert(pollBusinessStatusCache(clockMs));duringStateBuild=nullptr;
 doc.clear();state(doc.to<JsonObject>());assert(doc["sensors"]["pm25"]["valid"]==false);
 readings[PM25].set(52,clockMs);assert(pollBusinessStatusCache(clockMs));
}
#endif

static void httpReads(){
 String original;
 assert(businessReadSnapshot.copy(original,false,clockMs));
 DynamicJsonDocument request(512),reply(8192);
 for(const char *action:{"status.get","config.get"}){
  request.clear();request["action"]=action;request["request_id"]="read-01";request.createNestedObject("params");
  String body,out;serializeWireMP(request,body);
  assert(publicReadCommand(body,out));assert(!deserializeMsgPack(reply,out.c_str(),out.length()));
  if(reply["code"]!="ok"||reply["request_id"]!="read-01"||reply["data"]["name"]!=config["device"]["name"]){
   std::string dump;serializeJson(reply,dump);fprintf(stderr,"%s\n",dump.c_str());
  }
  assert(reply["code"]=="ok"&&reply["request_id"]=="read-01"&&reply["data"]["name"]==config["device"]["name"]);
  assert(!reply["data"].containsKey("wifi")&&!reply["data"].containsKey("mqtt"));
 }
 request["action"]="indicator.set";String body,out;serializeWireMP(request,body);assert(!publicReadCommand(body,out));
 request["action"]="status.get";request["params"]["unexpected"]=true;body="";serializeWireMP(request,body);assert(!publicReadCommand(body,out));
 request["params"].clear();request["request_id"]="";body="";serializeWireMP(request,body);assert(!publicReadCommand(body,out));
 request["request_id"]="valid";request["unexpected"]=true;body="";serializeWireMP(request,body);assert(!publicReadCommand(body,out));
 request.remove("unexpected");body="";serializeWireMP(request,body);body.concat("\0",1);assert(!publicReadCommand(body,out));
 // Copy lifetime, invalidation and a bounded lease including the clock wrap.
 invalidateBusinessStatusCache();String copy;assert(!businessReadSnapshot.copy(copy,false,clockMs));
 request["params"].to<JsonObject>();request["request_id"]="busy-id";body="";serializeWireMP(request,body);out="";
 assert(publicReadCommand(body,out));assert(!deserializeJson(reply,out));
 assert(reply["code"]=="busy"&&reply["request_id"]=="busy-id");
 out="";reply.clear();assert(publicReadCommand(body,out,true));
 assert(!deserializeMsgPack(reply,out.c_str(),out.length()));
 assert(reply["code"]=="busy"&&reply["request_id"]=="busy-id");
 assert(pollBusinessStatusCache(clockMs));assert(businessReadSnapshot.copy(copy,false,clockMs));
}

static void largeEncodedRead(){
 // A complete G3 publication fits the UDP response while its decoded object
 // needs more than the former second 6 KiB JSON pool.
 DynamicJsonDocument full(24576),reply(24576),request(512),formerPool(6144);
 assert(!deserializeMsgPack(full,cachedBusinessState().c_str(),cachedBusinessState().length()));
 JsonObject details=full["screen"].createNestedObject("settings");
 for(unsigned i=0;i<80;i++)details[std::string("setting_")+std::to_string(i)]=std::string(32,'a'+i%26);
 assert(!full.overflowed());String wire;serializeWireMP(full,wire);assert(wire.length()<8192);
 assert(deserializeMsgPack(formerPool,wire.c_str(),wire.length())==DeserializationError::NoMemory);
 String http("unused");assert(businessReadSnapshot.publish(String(wire),std::move(http),clockMs));
 request["action"]="status.get";request["request_id"]="large-read";request.createNestedObject("params");
 String body,out;serializeWireMP(request,body);assert(publicReadCommand(body,out,true));
 assert(!deserializeMsgPack(reply,out.c_str(),out.length()));
 assert(reply["code"]=="ok"&&reply["request_id"]=="large-read");
 String returned;serializeWireMP(reply["data"],returned);assert(returned==wire);
}

static void concurrentCopies(){
 HttpReadSnapshot cache;assert(cache.begin());
 String data("initial"),envelope("envelope");assert(cache.publish(std::move(data),std::move(envelope),UINT32_MAX-1000));
 String before;assert(cache.copy(before,false,999));assert(!cache.copy(before,false,1000));
 // HTTP captures now before entering copy(); the owner may refresh between
 // that capture and the atomic load. A newer publication is still fresh.
 cache.refresh(1001);assert(cache.copy(before,false,1000));
 assert(!cache.copy(before,false,3002));
 cache.refresh(1);assert(cache.copy(before,false,UINT32_MAX));
 assert(!cache.copy(before,false,2002));
 // A reader briefly holding the publication lock cannot invalidate an older
 // complete snapshot merely by overlapping a replacement attempt.
 snapshotMutex.lock();String pending("pending"),pendingHttp("http");
 assert(!cache.publish(std::move(pending),std::move(pendingHttp),1));snapshotMutex.unlock();
 assert(cache.copy(before,false,2)&&before=="initial");
 String limited("limited"),limitedHttp("http");assert(cache.publish(std::move(limited),std::move(limitedHttp),100,1000));
 assert(cache.copy(before,false,1099));cache.refresh(1100);assert(!cache.copy(before,false,1100));
 String initial("initial"),initialHttp("envelope");assert(cache.publish(std::move(initial),std::move(initialHttp),1));
 assert(cache.copy(before,false,1)&&before=="initial");
 std::atomic<bool> done{false};std::atomic<unsigned> successes{0};
 std::thread reader([&](){while(!done.load()){
  String value;if(cache.copy(value,false,5000)){
   assert(value.length()==512);for(unsigned i=1;i<value.length();i++)assert(value[i]==value[0]);++successes;
  }std::this_thread::yield();
 }});
 for(unsigned i=0;i<10000;i++){
  char bytes[513];memset(bytes,'a'+i%26,512);bytes[512]=0;
  String value(bytes),http("envelope");cache.publish(std::move(value),std::move(http),5000);
  if(!(i%100))std::this_thread::yield();
 }
 done.store(true);reader.join();assert(successes>0&&before=="initial");
 cache.invalidate();String missing;assert(!cache.copy(missing,false,5000));
}

int main(){
 DynamicJsonDocument power(512);assert(!deserializeJson(power,"{\"enabled\":false,\"start_minute\":1380,\"end_minute\":420,\"timezone_minutes\":480}"));
#ifdef IOTCLOUD_BRY88_SCREEN
 assert(!validSection("pm_power",power.as<JsonObjectConst>()));
#else
 assert(validSection("pm_power",power.as<JsonObjectConst>()));
 power["end_minute"]=1440;assert(!validSection("pm_power",power.as<JsonObjectConst>()));
 power["end_minute"]=420;power["enabled"]=1;assert(!validSection("pm_power",power.as<JsonObjectConst>()));
 power["enabled"]=false;power["timezone_minutes"]=841;assert(!validSection("pm_power",power.as<JsonObjectConst>()));
 power["timezone_minutes"]=480;power.remove("start_minute");assert(!validSection("pm_power",power.as<JsonObjectConst>()));
#endif

 assert(mqttAccountScope("")=="local");
 assert(mqttAccountScope("a/B-9")=="u_612f422d39");
 assert(mqttAccountScope("\xe6\xb5\x8b\xe8\xaf\x95")=="u_e6b58be8af95");
 assert(businessReadSnapshot.begin());
 config["hub"]["username"]="owner";config["device"]["name"]="initial";
 co2Module.warmed=true;co2Module.received=true;co2Module.measuredAt=clockMs;
 for(auto &s:readings)s.set(42,clockMs);
 assert(pollBusinessStatusCache(clockMs));const unsigned count=businessStatusBuilds;
 const String wire=cachedBusinessState(),http=cachedHttpStatus();
 clockMs+=1000;assert(pollBusinessStatusCache(clockMs));assert(businessStatusBuilds==count);
 assert(cachedBusinessState()==wire&&cachedHttpStatus()==http);
 DynamicJsonDocument doc(8192);state(doc.to<JsonObject>());
 assert(doc["snapshot_uptime_ms"]==240000&&doc["sensors"]["pm25"]["valid"]==true);
 assert(!doc.containsKey("free_memory")&&!doc.containsKey("ota_partitions"));
 assert(doc["co2_diagnostics"]["calibration"].is<JsonObject>());
 clockMs=260000;assert(pollBusinessStatusCache(clockMs));assert(businessStatusBuilds==count+1);
 doc.clear();state(doc.to<JsonObject>());assert(doc["sensors"]["pm25"]["valid"]==false);
 readings[PM25].set(51,clockMs);assert(pollBusinessStatusCache(clockMs));
 doc.clear();state(doc.to<JsonObject>());assert(doc["sensors"]["pm25"]["value"]==51);
 // Both the source check and JSON construction call digitalRead(). The old
 // publication must remain available throughout an ordinary sensor refresh.
 readings[PM25].set(52,clockMs);
 duringStateBuild=[](){String previous;assert(businessReadSnapshot.copy(previous,false,clockMs));};
 assert(pollBusinessStatusCache(clockMs));duringStateBuild=nullptr;
 // A periodic screen refresh also replaces a complete publication. Readers
 // must retain the previous snapshot while JSON and MessagePack are built.
 const String beforePeriodic=cachedBusinessState();
 const unsigned beforePeriodicBuilds=businessStatusBuilds;
 clockMs+=1000;requestBusinessStatusRefresh();
 String periodicCopy;assert(businessReadSnapshot.copy(periodicCopy,false,clockMs));
 assert(periodicCopy==beforePeriodic);
 duringStateBuild=[](){String previous;assert(businessReadSnapshot.copy(previous,false,clockMs));};
 assert(pollBusinessStatusCache(clockMs));duringStateBuild=nullptr;
 assert(businessStatusBuilds==beforePeriodicBuilds+1);
 assert(cachedBusinessState()!=beforePeriodic);
 doc.clear();state(doc.to<JsonObject>());assert(doc["snapshot_uptime_ms"]==clockMs);
#ifdef IOTCLOUD_BRY88_SCREEN
 telemetryKeepsPublication();
#endif
 config["device"]["name"]="changed";invalidateBusinessStatusCache();
 String invalidated;assert(!businessReadSnapshot.copy(invalidated,false,clockMs));
 doc.clear();state(doc.to<JsonObject>());assert(doc["name"]=="changed");
 clockMs=100000;co2Module.warmed=false;assert(pollBusinessStatusCache(clockMs));unsigned warm=businessStatusBuilds;
 clockMs+=1000;assert(pollBusinessStatusCache(clockMs));assert(businessStatusBuilds==warm+1);
 co2Module.attempted=true;co2Module.requestedAt=clockMs;assert(pollBusinessStatusCache(clockMs));unsigned cool=businessStatusBuilds;
 clockMs+=1000;assert(pollBusinessStatusCache(clockMs));assert(businessStatusBuilds==cool+1);
 doc.clear();diagnostics(doc.to<JsonObject>());assert(doc["schema"]==1);
 bool zero=false;for(JsonArrayConst pair:doc["fields"].as<JsonArrayConst>()){
 assert(pair.size()==2&&pair[0].is<unsigned>());if(pair[0]==PH_DIAG_MQTT_ERROR_TYPE){assert(pair[1]==0);zero=true;}}
 assert(zero);co2Module.received=false;doc.clear();diagnostics(doc.to<JsonObject>());
 for(JsonArrayConst pair:doc["fields"].as<JsonArrayConst>())assert(pair[0]!=PH_DIAG_CO2_DIAGNOSTICS_RAW_PPM);
 // A held PM sample has a long source age but cannot underflow the cache deadline.
 clockMs=2000000;readings[PM25].set(55,clockMs-1800000);readings[PM25].max_age_ms=3600000;
 assert(pollBusinessStatusCache(clockMs));assert(businessStatusKey.samples[PM25].valid);
 unsigned beforeTTL=businessStatusBuilds;readings[PM25].max_age_ms=120000;
 assert(pollBusinessStatusCache(clockMs));assert(!businessStatusKey.samples[PM25].valid);
 assert(businessStatusBuilds==beforeTTL+1);readings[PM25].max_age_ms=20000;
 httpReads();largeEncodedRead();concurrentCopies();
}
