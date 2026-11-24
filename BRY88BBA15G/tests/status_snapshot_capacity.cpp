#include <ArduinoJson.h>
#include "business_status_capacity.h"
#include <cassert>
#include <cstdio>
#include <string>

template <typename Document>
static void buildFullStatus(Document &document) {
  JsonObject out = document.template to<JsonObject>();
  out["snapshot_uptime_ms"] = 796455;
  out["model"] = "daikin";
  out["firmware_variant"] = "g3";
  out["hardware_model"] = "BRY88BBA15G";
  out["pm_sensor"] = "SCD30";
  out["migration_state"] = "ready";
  out["language"] = "zh-CN";
  out["name"] = "Daikin G3";
  out["device_id"] = "001122334455";
  out["version"] = "1.0.0-test";
  out["co2_warmup_seconds_remaining"] = 0;
  JsonObject co2 = out.createNestedObject("co2_diagnostics");
  co2["calibration_available"] = true;
  co2["cooldown_seconds"] = 0;
  JsonObject calibration = co2.createNestedObject("calibration");
  calibration["state"] = "idle";
  calibration["pending"] = false;
  calibration["awaiting_measurement"] = false;
  JsonObject climate = out.createNestedObject("climate_compensation");
  climate["temperature_offset"] = 4.9f;
  climate["humidity_offset"] = 0.0f;
  out["button_pressed"] = false;
  out["wifi_connected"] = true;
  out["ssid"] = "test-network";
  out["ip"] = "192.168.99.155";
  out["rssi"] = -48;
  out["ap_active"] = true;
  out["ap_name"] = "Daikin-G3";
  out["wifi_password_set"] = true;
  out["web_password_set"] = true;
  out["mqtt_host"] = "192.168.99.10";
  out["mqtt_port"] = 1883;
  out["mqtt_user"] = "test";
  out["mqtt_password_set"] = true;
  out["mqtt_connected"] = true;
  out["mqtt_status"] = "connected";
  out["mqtt_protocol"] = "tmcp";
  out["mqtt_topic_mode"] = "tmcp";
  out["mqtt_protocol_version"] = 2;
  out["formaldehyde_present"] = true;
  out["indicator_enabled"] = true;
  JsonObject sensors = out.createNestedObject("sensors");
  for (const char *name : {"temperature", "humidity", "co2", "pm25", "tvoc", "hcho"}) {
    JsonObject sensor = sensors.createNestedObject(name);
    sensor["unit"] = "ppm";
    sensor["valid"] = true;
    sensor["received"] = true;
    sensor["value"] = 977;
    sensor["age_ms"] = 1173;
  }
  JsonObject screen = out.createNestedObject("screen");
#include "fixtures/business_screen_full.inc"
}

int main() {
  // Reproduce the previous G3 pool: the complete populated screen snapshot
  // must overflow 6400 bytes, matching the field diagnostic (6395/6400).
  DynamicJsonDocument oldPool(6400);
  buildFullStatus(oldPool);
  assert(oldPool.overflowed());

  DynamicJsonDocument fixedPool(G3_BUSINESS_STATUS_JSON_CAPACITY);
  buildFullStatus(fixedPool);
  assert(!fixedPool.overflowed());
  assert(fixedPool.memoryUsage() < G3_BUSINESS_STATUS_JSON_CAPACITY);

  std::string wire;
  assert(serializeMsgPack(fixedPool, wire) == measureMsgPack(fixedPool));
  DynamicJsonDocument decoded(8192);
  assert(!deserializeMsgPack(decoded, wire));
  assert(!decoded.overflowed());
  assert(decoded["screen"]["brightness"]["requested_percent"] == 100);
  std::printf("PASS G3 full status snapshot: %zu / %u JSON bytes, %zu MessagePack bytes\n",
              fixedPool.memoryUsage(), G3_BUSINESS_STATUS_JSON_CAPACITY, wire.size());
}
