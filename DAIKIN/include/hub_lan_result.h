/* SPDX-License-Identifier: GPL-3.0-or-later */
#pragma once
#include "hub_device_bus.h"
extern "C" {
#include "../../common/typed_wire.h"
}
namespace iotcloud {
// An authenticated, completely reassembled UDP receipt is distinct from a
// measured snapshot. Only the correlated status.get data feeds state routes.
inline bool routeHubLanResult(HubDeviceBus &bus,const std::string &command,
                             const void *bytes,size_t length,bool status,
                             const std::string &expectedId){
  if(command.size()<8||command.compare(command.size()-8,8,"/command"))return false;
  ph_value root,code,id,data;char actualId[65]={};
  if(!phv_parse(PH_WIRE_MSGPACK,bytes,length,&root)||root.kind!=PHV_MAP)return false;
  if(!expectedId.empty()&&(!phv_get(&root,"request_id",&id)||!phv_text(&id,actualId,sizeof(actualId))||expectedId!=actualId))return false;
  const std::string base=command.substr(0,command.size()-8);
  bus.publish(base+"/availability","online",false,HubDeviceBus::Origin::Local);
  bus.publish(base+"/result",std::string(static_cast<const char*>(bytes),length),false,HubDeviceBus::Origin::Local);
  if(status&&phv_get(&root,"code",&code)&&phv_equal_text(&code,"ok")&&phv_get(&root,"data",&data)&&data.kind==PHV_MAP){
    // The complete envelope already validated this MessagePack subtree. Own
    // its exact bytes without another writer, pool growth or container rewrite.
    return bus.publish(base+"/state",std::string(reinterpret_cast<const char*>(data.begin),size_t(data.end-data.begin)),false,HubDeviceBus::Origin::Local);
  }
  return true;
}
}
