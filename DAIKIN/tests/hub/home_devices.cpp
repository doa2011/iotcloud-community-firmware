/* SPDX-License-Identifier: GPL-3.0-or-later */
// Exercise the registered-device path with real JSON and MessagePack payloads.
#include "hub_home.h"
#include <cassert>
#include <cstdio>
using Home = iotcloud::HubHome;
static std::string packed(const char *json) {
  DynamicJsonDocument doc(4096); assert(!deserializeJson(doc, json));
  std::string body; serializeMsgPack(doc, body); return body;
}
int main() {
  DynamicJsonDocument saved(2048), request(1024), discovery(2048);
  assert(!deserializeJson(saved, R"([{"model":"a1","mac":"aabbcc000002"},{"model":"daikin","mac":"aabbcc000003"},{"model":"s30-v1b","mac":"aabbcc000004"},{"model":"tc1","mac":"aabbcc000005"}])"));
  iotcloud::HubDeviceBus bus; Home home(bus);
  iotcloud::hubWire.clear();
  unsigned serial=0, sent=0;
  home.randomHex=[&](size_t n){auto s=std::to_string(++serial);return std::string(n*2-s.size(),'0')+s;};
  assert(home.configure(saved.as<JsonArrayConst>(),"lab/","aabbcc000001"));
  std::string topic, body;
  bus.dispatch=[&](const std::string &t,const std::string &b,bool retain,iotcloud::HubDeviceBus::Origin){assert(!retain);++sent;topic=t;body=b;return true;};
  const auto a1=home.devices[1].base, g3=home.devices[2].base;
  const auto s30=home.devices[3].base, tc1=home.devices[4].base;
  home.tick(100);
  for(size_t i=0;i<home.size;++i){home.devices[i].nameCapabilitiesKnown=true;home.devices[i].nameCapabilitiesAt=100;}
  for(unsigned i:{1u,2u}) {
    const auto &d=home.devices[i]; discovery.clear();
    discovery["schema"]=2; discovery["protocol"]="tmcp"; discovery["protocol_version"]=2;
    discovery["version"]="v2"; discovery["model"]=d.model; discovery["device_id"]=d.mac;
    discovery["firmware_version"]="v1.0-test"; discovery["variant"]=i==1?"fresh-air":"full";
    discovery["wire_version"]=1; discovery["default_wire_format"]="msgpack";
    discovery.createNestedArray("wire_formats").add("msgpack");
    std::string wire; serializeMsgPack(discovery,wire);
    home.observe("other/discovery/"+d.mac,wire); // Namespace must match.
    iotcloud::hubWire.observe("lab/discovery/"+d.mac,wire);
  }
  const char *fresh=R"({"model":"a1","device_id":"aabbcc000002","fan_on":true,"percentage":80,"fresh_air_on":true,"fresh_air_speed":40,"fresh_air_mode":"manual","led_visible":true,"brightness":128})";
  home.observe(a1+"/state",packed(fresh));
  assert(home.devices[1].freshAir&&home.devices[1].speed==40&&home.devices[1].controls);
  auto sequence=home.devices[1].sequence;
  home.observe(a1+"/state",fresh); // No JSON fallback for a bound binary peer.
  assert(home.devices[1].sequence==sequence);
  // An incomplete announcement cannot downgrade the selected wire contract.
  iotcloud::hubWire.observe("lab/discovery/aabbcc000002",packed(R"({"schema":2,"model":"a1","device_id":"aabbcc000002","protocol":"tmcp","protocol_version":2})"));
  assert(home.begin(1,Home::FanSpeed,0,60));
  assert(!deserializeMsgPack(request,body)&&request["action"]=="fresh_air.set"&&request["params"]["speed"]==60);
  const auto ack=std::string("{\"request_id\":\"")+request["request_id"].as<std::string>()+"\",\"code\":\"accepted\"}";
  home.observe(a1+"/result",packed(ack.c_str()));
  assert(!deserializeMsgPack(request,body)&&request["action"]=="status.get");
  home.observe(a1+"/state",packed(fresh)); assert(!home.finished());
  home.observe(a1+"/state",packed(R"({"fresh_air_on":true,"fresh_air_speed":60,"fresh_air_mode":"manual","led_visible":true,"brightness":128})"));
  assert(home.finished()&&home.finish());
  // Purifier auto/manual writes preserve independent fresh-air routing.
  assert(home.begin(1,Home::FanMode,0,1,true,false));
  assert(!deserializeMsgPack(request,body)&&request["action"]=="fresh_air.set"&&request["params"]["mode"]=="auto"&&request["params"]["on"]==false);
  auto id=request["request_id"].as<std::string>();
  home.observe(a1+"/result",packed(("{\"request_id\":\""+id+"\",\"code\":\"ok\"}").c_str()));
  home.observe(a1+"/state",packed(R"({"fresh_air_on":false,"fresh_air_speed":0,"fresh_air_mode":"auto","led_visible":true,"brightness":128})"));
  assert(home.finished()&&home.finish()&&home.devices[1].automatic);
  home.observe(g3+"/state",packed(R"({"model":"daikin","device_id":"aabbcc000003","sensors":{"temperature":{"valid":true,"value":23.75,"unit":"°C","age_ms":50},"pm25":{"valid":true,"value":7,"unit":"µg/m³","age_ms":25},"co2":{"valid":true,"value":723,"unit":"ppm","age_ms":0}}})"));
  assert(home.devices[2].reachable(100)&&home.devices[2].values[Home::Temperature].number==23.75&&home.devices[2].values[Home::CO2].number==723);
  // S30 is binary-only, including before its first discovery publication.
  home.observe(s30+"/state",R"({"socket_states":[true]})"); assert(!home.devices[3].seen);
  const char *socket=R"({"model":"s30-v1b","device_id":"aabbcc000004","socket_states":[true],"socket_names":["打印机"],"metering":{"valid_fields":4,"active_power":17.25,"energy_kwh":1.125}})";
  home.observe(s30+"/state",packed(socket)); const auto &s=home.devices[3];
  assert(s.outletCount()==1&&s.controls&&s.outlets[0]&&s.outletNames[0]=="打印机");
  assert(s.power.fresh(100)&&s.power.number==17.25&&s.energy.number==1.125);
  assert(!home.begin(3,Home::Outlet,1,0)&&home.begin(3,Home::Outlet,0,0));
  assert(!deserializeMsgPack(request,body)&&request["action"]=="outlet.set"&&request["params"]["index"]==0&&request["params"]["on"]==false);
  home.finish();
  home.observe(s30+"/state",packed(R"({"socket_states":[false],"socket_names":["改名"],"metering":{"valid_fields":0,"active_power":123,"energy_kwh":1.125}})"));
  assert(s.outletNames[0]=="改名"&&!s.power.valid&&s.energy.valid);
  home.observe(s30+"/state",packed(R"({"socket_states":[1],"socket_names":["invalid"]})"));
  assert(!s.controls&&s.outletNames[0]=="改名");
  home.observe(s30+"/state",packed(R"({"socket_states":[false],"socket_names":[""]})"));
  assert(s.controls&&s.outletNames[0]=="改名");
  home.observe(s30+"/availability-junk","offline"); assert(s.reachable(100));
  home.observe(s30+"/availability","offline"); assert(!s.reachable(100)&&!s.energy.valid);
  // A full status query fetches names even while relay-only reports are fresh.
  home.observe(tc1+"/state",R"({"socket_states":[0,1,0,1,0,1]})");
  for(unsigned i=0;i<home.size;++i)home.refresh();
  assert(topic==tc1+"/command"&&!deserializeJson(request,body)&&request["action"]=="status.get");
  id=request["request_id"].as<std::string>();
  home.observe(tc1+"/result","{\"request_id\":\""+id+R"(","code":"ok","data":{"socket_states":[0,1,0,1,0,1],"socket_names":["电脑","显示器","灯","路由器","音箱","充电器"]}})");
  const auto &t=home.devices[4]; assert(t.namesKnown&&t.outletNames[5]=="充电器");
  home.observe(tc1+"/state",R"({"socket_states":[0,1,0,1,0,1]})"); assert(t.outletNames[0]=="电脑");
  home.tick(10100); home.observe(tc1+"/state",R"({"socket_states":[0,1,0,1,0,1]})");
  for(unsigned i=0;i<home.size;++i)home.refresh();
  assert(topic==tc1+"/command"&&!deserializeJson(request,body)&&request["action"]=="power.get");
  id=request["request_id"].as<std::string>();
  home.observe(tc1+"/result","{\"request_id\":\""+id+R"(","code":"ok","data":{"power_watts":45.125,"energy_kwh":7.75}})");
  assert(t.power.fresh(10100)&&t.power.number==45.125&&t.energy.number==7.75);
  assert(!t.power.fresh(30100));
  home.tick(20100); home.observe(tc1+"/state",R"({"socket_states":[0,1,0,1,0,1]})");
  for(unsigned i=0;i<home.size;++i)home.refresh();
  assert(!deserializeJson(request,body)); id=request["request_id"].as<std::string>();
  home.observe(tc1+"/result","{\"request_id\":\""+id+R"(","code":"ok","data":{"power_watts":-1,"energy_kwh":null}})");
  assert(!t.power.valid&&!t.energy.valid);
  home.observe(g3+"/state/ignored",packed(R"({"sensors":{}})"));
  assert(home.devices[2].at==100);
  assert(home.configure(saved.as<JsonArrayConst>(),"lab/","aabbcc000001"));
  iotcloud::hubWire.clear();
  home.observe(a1+"/state",fresh); // Reconfiguration discards codec/state caches.
  assert(home.devices[1].controls&&home.devices[1].freshAir);
  assert(sent>0);
  puts("Hub HomeKit binary fresh-air/G3/S30, outlet names and metering tests passed");
}
