#include "screen_config_document.h"
#include "backup_json.h"
#include "screen_settings_snapshot.h"
#include "screen_store_writer.h"
#include <cassert>
#include <cstdio>
#include <string>
#include <algorithm>
#include <vector>
#include <memory>
static uint8_t flash[BRY_STORE_SIZE];
static size_t largestWrite;
static bool readFlash(void *, uint32_t at, void *out, size_t n) {
  assert(at+n <= sizeof(flash));
  std::memcpy(out, flash+at, n); return true;
}
static bool writeFlash(void *, uint32_t at, const void *data, size_t n) {
  assert(at+n <= sizeof(flash));
  if (n > largestWrite) largestWrite = n;
  for (size_t i = 0; i < n; i++) flash[at+i] &= static_cast<const uint8_t *>(data)[i];
  return true;
}
static bool eraseFlash(void *, uint32_t at, size_t n) {
  assert(at+n <= sizeof(flash) && !(at%4096) && !(n%4096));
  std::memset(flash+at, 255, n); return true;
}
// Test-side base64 fixture codec: round trip real SCN3 bytes through JSON
// with exactly the same standard alphabet/padding as the device encoder.
static std::string fixtureBase64(const uint8_t *p, size_t n) {
  const char *alphabet="ABCDEFGHIJKLMNOPQRSTUVWXYZabcdefghijklmnopqrstuvwxyz0123456789+/";
  std::string out;
  for (size_t i=0;i<n;i+=3) {
    uint32_t a=p[i], b=i+1<n?p[i+1]:0, c=i+2<n?p[i+2]:0;
    out+=alphabet[a>>2];out+=alphabet[((a&3)<<4)|(b>>4)];
    out+=i+1<n?alphabet[((b&15)<<2)|(c>>6)]:'=';
    out+=i+2<n?alphabet[c&63]:'=';
  }
  return out;
}
static std::vector<uint8_t> fixtureDecode(const std::string &s) {
  const char *alphabet="ABCDEFGHIJKLMNOPQRSTUVWXYZabcdefghijklmnopqrstuvwxyz0123456789+/";
  std::vector<uint8_t> out;
  assert(!(s.size()%4));
  for(size_t i=0;i<s.size();i+=4) {
    uint32_t v=0;
    for(unsigned j=0;j<4;++j) {
      const char *p=std::strchr(alphabet,s[i+j]);
      assert(p||s[i+j]=='=');
      v=(v<<6)|(p?uint32_t(p-alphabet):0);
    }
    out.push_back(v>>16);
    if(s[i+2]!='=')out.push_back(v>>8);
    if(s[i+3]!='=')out.push_back(v);
  }
  return out;
}
int main() {
  // A delayed upstream snapshot must not overwrite the locally persisted
  // screen request shown by status and screen.settings.get.
  StaticJsonDocument<1024> snapshot, saved, parent;
  snapshot["display"]["luminance"] = 10;
  snapshot["display"]["data_type"] = 1;
  snapshot["display"]["power_screen_time"] = 2;
  snapshot["display"]["battery_screen_time"] = 1;
  snapshot["no_disturb"]["state"] = false;
  saved["display"]["luminance"] = 65;
  saved["display"]["data_type"] = 0;
  saved["display"]["power_screen_time"] = 1;
  saved["display"]["battery_screen_time"] = 2;
  screenSettingsSnapshot(parent.to<JsonObject>(), snapshot.as<JsonObjectConst>(),
                         saved.as<JsonObjectConst>());
  assert(parent["settings"]["display"]["luminance"] == 65);
  assert(parent["settings"]["display"]["data_type"] == 0);
  assert(parent["settings"]["display"]["power_screen_time"] == 1);
  assert(parent["settings"]["no_disturb"]["state"] == false);
  assert(parent["saved_settings"]["display"]["luminance"] == 65);
  StaticJsonDocument<512> emptySnapshot, fallbackParent;
  screenSettingsSnapshot(fallbackParent.to<JsonObject>(),
                         emptySnapshot.as<JsonObjectConst>(),
                         saved.as<JsonObjectConst>());
  assert(fallbackParent["settings"]["display"]["luminance"] == 65);
  std::memset(flash, 255, sizeof(flash));
  bry_store store = {};
  store.read = readFlash; store.write = writeFlash; store.erase = eraseFlash;
  assert(bry_store_probe(&store));
  reminder_schedule schedule = {};
  reminder_notice notices[REMINDER_MAX] = {};
  schedule.timezone = 480;
  StaticJsonDocument<4096> weather;
  StaticJsonDocument<1024> settings;
  weather["provider"] = "openmeteo";
  weather["label"] = "Guangzhou, Guangdong";
  weather["enabled"] = true;
  weather["key"] = std::string(256, 'k');
  settings["tvoc_warmup_minutes"] = 15;
  settings["display"]["luminance"] = 0;
  settings["display"]["data_type"] = 0;
  settings["display"]["power_screen_time"] = 2;
  settings["display"]["battery_screen_time"] = 1;
  std::unique_ptr<screen_scene> original(new screen_scene), decoded(new screen_scene);
  scene_defaults(original.get());
  original->revision=42;
  std::strcpy(original->page[5].title,"Album");
  std::vector<uint8_t> raw(SCENE_BYTES), encoded(SCENE_BYTES);
  assert(scene_encode(original.get(),raw.data()));
  std::string layout=fixtureBase64(raw.data(),raw.size()),policy(632,'B'),appearance(940,'C');
  assert(layout.size()==4*((SCENE_BYTES+2)/3));
  assert(fixtureDecode(layout)==raw);
  assert(scene_decode_candidate(decoded.get(),raw.data(),raw.size()));
  assert(scene_encode(decoded.get(),encoded.data())&&encoded==raw);
  auto malformed=raw;malformed[16]=2;
  assert(!scene_decode_candidate(decoded.get(),malformed.data(),malformed.size()));
  // Live decoder must still leave the current layout unchanged on rejection.
  assert(!scene_decode(original.get(),malformed.data(),malformed.size()));
  assert(scene_encode(original.get(),encoded.data())&&encoded==raw);
  assert(!scene_decode_candidate(decoded.get(),raw.data(),raw.size()-1));
  for (unsigned count : {0u, 1u, 32u}) {
    for (unsigned i = 0; i < REMINDER_MAX; i++) {
      auto &r = schedule.rules[i];
      r.id = i < count ? i + 1 : 0;
      r.kind = REM_WEEKLY;
      r.days = 31;
      r.minute = 570;
      r.enabled = true;
      r.last_fire = 1234;
      std::memset(r.title, 'T', sizeof(r.title) - 1);
      std::memset(r.text, 'R', sizeof(r.text) - 1);
      auto &n = notices[i];
      n.id = i < count ? 0x80000000u + i : 0;
      n.at = 5678;
      n.unread = true;
      std::memset(n.title, 'N', sizeof(n.title) - 1);
      std::memset(n.text, 'M', sizeof(n.text) - 1);
    }
    size_t capacity = screenConfigCapacity(weather, settings, schedule, notices);
    DynamicJsonDocument doc(capacity);
    assert(screenConfigDocument(doc, layout.c_str(), weather, settings,
                                 schedule, notices, policy.c_str(), appearance.c_str()));
    assert(doc["rules"].size() == count && doc["notices"].size() == count);
    assert(doc["settings"]["display"]["luminance"] == 0);
    assert(doc["weather"]["key"].as<std::string>() == std::string(256, 'k'));
    std::string wire;
    assert(serializeMsgPack(doc, wire) == measureMsgPack(doc));
    assert(wire.size() <= 48 * 1024);
    assert(screenPersistConfig(&store, doc));
    size_t persistedSize = 0;
    assert(bry_store_load(&store, nullptr, 0, &persistedSize));
    assert(persistedSize == wire.size());
    std::string persisted(persistedSize, '\0');
    assert(bry_store_load(&store, &persisted[0], persisted.size(), &persistedSize));
    assert(persisted == wire && largestWrite <= 256);
    DynamicJsonDocument restored(65536);
    assert(!deserializeMsgPack(restored, wire));
    assert(restored["settings"]["tvoc_warmup_minutes"] == 15);
    assert(restored["layout"].as<std::string>() == layout);
    assert(restored["policy"].as<std::string>() == policy);
    assert(restored["appearance"].as<std::string>() == appearance);
    assert(restored["schema"] == 2);
    assert(restored.size() == 11);
    assert(!restored.containsKey("font_data"));
    assert(!restored.containsKey("wallpaper_data"));
    // Exercise the device's single-pool composer at the actual payload size.
    // Temporary strings must remain valid after the capture scope exits.
    DynamicJsonDocument compact(capacity + layout.size() + policy.size() +
                                appearance.size() + 3 + 2048);
    {
      std::string a=layout, b=policy, c=appearance;
      JsonObject screen=compact.createNestedObject("screen");
      screenConfigFields(screen, "", weather, settings, schedule, notices, "", "");
      screen["layout"]=a; screen["policy"]=b; screen["appearance"]=c;
    }
    assert(!compact.overflowed());
    assert(compact["screen"]["layout"].as<std::string>() == layout);
    assert(compact["screen"]["policy"].as<std::string>() == policy);
    assert(compact["screen"]["appearance"].as<std::string>() == appearance);
    std::string compactWire;
    assert(serializeMsgPack(compact["screen"], compactWire) == wire.size());
    assert(compactWire == wire);
    // The public PHCB backup wraps this exact screen record. Its copy must
    // retain layout, brightness, sleep policy and appearance metadata without
    // embedding either independent MCU resource bank.
    DynamicJsonDocument envelope(131072);
    envelope["schema"] = 2;
    envelope["model"] = "daikin";
    envelope["variant"] = "g3";
    envelope["config_version"] = 1;
    envelope["firmware_version"] = "1.0.0-test";
    envelope["hardware_model"] = "BRY88BBA15G";
    std::string screenWire;
    assert(serializeMsgPack(restored, screenWire) == measureMsgPack(restored));
    envelope["screen_crc32"] = scene_crc32(screenWire.data(), screenWire.size());
    envelope["screen"] = restored.as<JsonObjectConst>();
    assert(!envelope.overflowed());
    std::string backup;
    assert(serializeJson(envelope, backup) == measureJson(envelope));
    // Actual full envelope, including long Base64 and escaped punctuation,
    // parsed in-place with a structural pool. Validate CRC and deep copies
    // even after destroying the backing upload.
    envelope["config"]["ssid"]=std::string("test,{[\\\"}ssid");
    envelope["config"]["nested"]["key"]=std::string("owned-value");
    backup.clear();
    assert(serializeJson(envelope, backup)==measureJson(envelope));
    const size_t parseCapacity=backupJsonPoolCapacity(backup.data(),backup.size(),65536);
    assert(parseCapacity && (count || parseCapacity<4096));
    DynamicJsonDocument zeroCopy(parseCapacity);
    assert(!deserializeJson(zeroCopy,&backup[0],backup.size()));
    std::string zeroWire;
    serializeMsgPack(zeroCopy["screen"],zeroWire);
    assert(zeroWire==screenWire);
    auto decodedWire=fixtureDecode(zeroCopy["screen"]["layout"].as<std::string>());
    assert(decodedWire==raw);
    assert(scene_decode_candidate(decoded.get(),decodedWire.data(),decodedWire.size()));
    assert(scene_encode(decoded.get(),encoded.data())&&encoded==raw);
    DynamicJsonDocument ownedConfig(2048), ownedWeather(4096), ownedSettings(1024);
    assert(backupJsonCopyOwned(ownedConfig,zeroCopy["config"]));
    assert(backupJsonCopyOwned(ownedWeather,zeroCopy["screen"]["weather"]));
    assert(backupJsonCopyOwned(ownedSettings,zeroCopy["screen"]["settings"]));
    zeroCopy.clear();
    std::fill(backup.begin(),backup.end(),'X');
    assert(ownedConfig["ssid"].as<std::string>()==std::string("test,{[\\\"}ssid"));
    assert(ownedConfig["nested"]["key"].as<std::string>()=="owned-value");
    assert(ownedWeather["key"].as<std::string>()==std::string(256,'k'));
    assert(ownedSettings["display"]["luminance"]==0);
    assert(!backupJsonPoolCapacity("\"unterminated",13,65536));
    std::printf("PASS zero-copy backup: %u records each, pool=%zu, upload=%zu\\n",
                count,parseCapacity,backup.size());
    backup.clear();
    serializeJson(envelope,backup);
    DynamicJsonDocument imported(131072);
    assert(!deserializeJson(imported, backup));
    assert(imported["hardware_model"] == "BRY88BBA15G");
    assert(imported["screen_crc32"] == scene_crc32(screenWire.data(), screenWire.size()));
    assert(imported["screen"]["schema"] == 2);
    assert(imported["screen"]["settings"]["display"]["luminance"] == 0);
    assert(imported["screen"]["policy"].as<std::string>() == policy);
    assert(imported["screen"]["appearance"].as<std::string>() == appearance);
    assert(!imported["screen"].containsKey("font_data"));
    assert(!imported["screen"].containsKey("wallpaper_data"));
    bool automatic=false;
    assert(screenConfigStartupAuto(restored.as<JsonObjectConst>(),automatic)&&automatic);
    doc["startup_auto_resume"]=false;
    assert(screenPersistConfig(&store,doc));
    assert(bry_store_load(&store,nullptr,0,&persistedSize));persisted.resize(persistedSize);
    assert(bry_store_load(&store,&persisted[0],persisted.size(),&persistedSize));
    restored.clear();assert(!deserializeMsgPack(restored,persisted));
    assert(screenConfigStartupAuto(restored.as<JsonObjectConst>(),automatic)&&!automatic);
    restored.remove("startup_auto_resume");
    assert(screenConfigStartupAuto(restored.as<JsonObjectConst>(),automatic)&&automatic);
    restored["startup_auto_resume"]="true";
    assert(!screenConfigStartupAuto(restored.as<JsonObjectConst>(),automatic)&&!automatic);
    assert(restored["rules"].size() == count && restored["notices"].size() == count);
    if (count) {
      assert(restored["rules"][count - 1]["last_fire"] == 1234);
      assert(restored["notices"][count - 1]["at"] == 5678);
      assert(restored["rules"][0]["text"].as<std::string>().size() == sizeof(schedule.rules[0].text) - 1);
    }
    if (!count) assert(capacity < 4096);
    assert(capacity < 40948);
    StaticJsonDocument<1> unavailable;
    assert(!screenConfigDocument(unavailable, layout.c_str(), weather, settings,
                                  schedule, notices, policy.c_str(), appearance.c_str()));
    std::printf("PASS real config composer: %u records each, capacity=%zu, "
                "binary bytes=%zu, complete restore\n", count, capacity, wire.size());
  }
}
