/* One owner, persistent policy and negotiated single-packet replacement. */
static void screenPolicyGet(JsonObject out, bool detail) {
  const display_policy &p = screenDisplayPolicy;
  out["supported"] = bool(screenBridge.capabilities & SCREEN_CAP_DISPLAY);
  if(detail){out["page_policy_supported"] = bool(screenBridge.capabilities & SCREEN_CAP_PAGE_POLICY);
  out["home_page_id"] = p.home_page_id;
  JsonArray overrides = out.createNestedArray("page_overrides");
  for(unsigned i=0;i<p.page_count;i++){const display_page_override &r=p.page_overrides[i];auto o=overrides.createNestedObject();o["page_id"]=r.page_id;o["external_minutes"]=r.external_minutes;o["battery_minutes"]=r.battery_minutes;o["idle_brightness"]=r.idle_brightness;}}
  out["revision"] = p.revision; out["applied_revision"] = screenPolicyAck;
  out["error"] = screenPolicyError; out["max_plans"] = DISPLAY_PLANS;
  out["ceiling"] = p.ceiling; out["external_minutes"] = p.external_minutes;
  out["battery_minutes"] = p.battery_minutes;
  out["idle_brightness"] = p.idle_brightness; out["idle_page"] = p.idle_page;
  JsonArray plans = out.createNestedArray("plans");
  for (unsigned i = 0; i < p.count; i++) {
    const display_plan &r = p.plans[i]; JsonObject o = plans.createNestedObject();
    o["name"] = r.name; o["enabled"] = r.enabled; o["days"] = r.days;
    o["start"] = r.start; o["end"] = r.end; o["brightness"] = r.brightness; o["page"] = r.page;
  }
}
static bool screenPolicyPage(unsigned page) {
  return page == DISPLAY_KEEP_PAGE ||
         (page < screenScene.pages && screenScene.page[page].enabled);
}
static bool screenPolicyId(uint32_t id,bool enabled=false) {
  for(unsigned i=0;i<screenScene.pages;i++)if(screenScene.page[i].id==id&&(!enabled||screenScene.page[i].enabled))return true;
  return false;
}
static const char *screenPolicySet(JsonObjectConst params, JsonObject out) {
  if (!screenBridge.extended || !(screenBridge.capabilities & SCREEN_CAP_DISPLAY))
    return "unsupported";
  if (screenImage.active) return "busy";
  bool extended=params.containsKey("home_page_id")||params.containsKey("page_overrides");
  if ((params.size() != (extended?8u:6u)) || !params["ceiling"].is<unsigned>() ||
      !params["external_minutes"].is<unsigned>() || !params["battery_minutes"].is<unsigned>() ||
      !params["idle_brightness"].is<unsigned>() || !params["idle_page"].is<unsigned>() ||
      !params["plans"].is<JsonArrayConst>()) return "invalid_params";
  for (JsonPairConst field : params) {
    const char *k=field.key().c_str();
    if (strcmp(k,"ceiling")&&strcmp(k,"external_minutes")&&strcmp(k,"battery_minutes")&&
        strcmp(k,"idle_brightness")&&strcmp(k,"idle_page")&&strcmp(k,"plans")&&strcmp(k,"home_page_id")&&strcmp(k,"page_overrides"))return "invalid_params";
  }
  unsigned ceiling=params["ceiling"],external=params["external_minutes"],battery=params["battery_minutes"],
           idle=params["idle_brightness"],page=params["idle_page"];
  if (!ceiling || ceiling>100 || external>1440 || battery>1440 || idle>100 || !screenPolicyPage(page))
    return "invalid_params";
  display_policy next = {};
  next.revision = screenDisplayPolicy.revision;
  next.ceiling=ceiling;next.external_minutes=external;next.battery_minutes=battery;
  next.idle_brightness=idle;next.idle_page=page;
  for (JsonObjectConst o : params["plans"].as<JsonArrayConst>()) {
    if (next.count >= DISPLAY_PLANS || o.size()!=7 || !o["name"].is<const char *>() ||
        !o["enabled"].is<bool>() || !o["days"].is<unsigned>() || !o["start"].is<unsigned>() ||
        !o["end"].is<unsigned>() || !o["brightness"].is<unsigned>() || !o["page"].is<unsigned>() ||
        !scene_utf8(o["name"],32)) return "invalid_params";
    unsigned days=o["days"],start=o["start"],end=o["end"],brightness=o["brightness"],dest=o["page"];
    if (days>127 || start>=1440 || end>=1440 || brightness>100 || !screenPolicyPage(dest))return "invalid_params";
    display_plan &r=next.plans[next.count++];r.enabled=o["enabled"];r.days=days;
    r.start=start;r.end=end;r.brightness=brightness;r.page=dest;
    bry::textCopy(r.name,sizeof(r.name),o["name"]);
  }
  next.home_page_id=screenDisplayPolicy.home_page_id;
  next.page_count=screenDisplayPolicy.page_count;
  memcpy(next.page_overrides,screenDisplayPolicy.page_overrides,sizeof(next.page_overrides));
  if(extended){
    if(!(screenBridge.capabilities&SCREEN_CAP_PAGE_POLICY))return "unsupported";
    if(!params["home_page_id"].is<uint32_t>()||!params["page_overrides"].is<JsonArrayConst>())return "invalid_params";
    next.home_page_id=params["home_page_id"];
    if(next.home_page_id&&!screenPolicyId(next.home_page_id,true))return "invalid_params";
    next.page_count=0;memset(next.page_overrides,0,sizeof(next.page_overrides));
    for(JsonVariantConst value:params["page_overrides"].as<JsonArrayConst>()){
      auto o=value.as<JsonObjectConst>();
      if(next.page_count>=SCENE_PAGES||o.size()!=4||!o["page_id"].is<uint32_t>()||!o["external_minutes"].is<unsigned>()||!o["battery_minutes"].is<unsigned>()||!o["idle_brightness"].is<unsigned>())return "invalid_params";
      uint32_t id=o["page_id"];unsigned ex=o["external_minutes"],ba=o["battery_minutes"],level=o["idle_brightness"];
      if(!id||!screenPolicyId(id)||ex>1440||ba>1440||level>100)return "invalid_params";
      for(unsigned i=0;i<next.page_count;i++)if(next.page_overrides[i].page_id==id)return "invalid_params";
      auto &r=next.page_overrides[next.page_count++];r.page_id=id;r.external_minutes=ex;r.battery_minutes=ba;r.idle_brightness=level;
    }
  }
  if (!display_policy_valid(&next)) return "invalid_params";
  if(!memcmp(&next,&screenDisplayPolicy,sizeof(next))){screenPolicyGet(out,true);return "ok";}
  if(++next.revision==0)return "invalid_params";
  display_policy previous=screenDisplayPolicy;screenDisplayPolicy=next;
  if (!screenSave()) {screenDisplayPolicy=previous;return "storage_error";}
  screenPolicyAck=screenPolicySentAt=0;screenPolicyError="";
  screenPolicyGet(out,true);invalidateBusinessStatusCache();return "accepted";
}
static void screenPolicyPoll() {
  if (!screenBridge.extended || !(screenBridge.capabilities & SCREEN_CAP_DISPLAY) || screenImage.active)return;
  if (screenPolicySession!=screenBridge.session) {
    screenPolicySession=screenBridge.session;screenPolicyAck=screenPolicySentAt=0;screenPolicyError="";
  }
  if (screenPolicyAck==screenDisplayPolicy.revision)return;
  if (screenPolicySentAt && uint32_t(millis()-screenPolicySentAt)<3000)return;
  uint8_t wire[DISPLAY_POLICY_BYTES];
  size_t length=screenBridge.capabilities&SCREEN_CAP_PAGE_POLICY?DISPLAY_POLICY_BYTES:DISPLAY_POLICY_V1_BYTES;
  if (display_policy_encode(&screenDisplayPolicy,wire,length) &&
      bry_bridge_send(&screenBridge,wire,length,SCREEN_CAP_DISPLAY))screenPolicySentAt=millis();
}
static void screenPolicyRemap(const screen_scene &oldScene,const screen_scene &next) {
  auto remap=[&](uint8_t &page) {
    if (page==DISPLAY_KEEP_PAGE)return;
    uint32_t id=page<oldScene.pages?oldScene.page[page].id:0;
    for(unsigned i=0;i<next.pages;i++)if(next.page[i].enabled&&id&&next.page[i].id==id) {page=i;return;}
    page=DISPLAY_KEEP_PAGE;
  };
  display_policy old=screenDisplayPolicy;
  remap(screenDisplayPolicy.idle_page);
  for(unsigned i=0;i<screenDisplayPolicy.count;i++)remap(screenDisplayPolicy.plans[i].page);
  auto exists=[&](uint32_t id,bool enabled){for(unsigned i=0;i<next.pages;i++)if(next.page[i].id==id&&(!enabled||next.page[i].enabled))return true;return false;};
  if(screenDisplayPolicy.home_page_id&&!exists(screenDisplayPolicy.home_page_id,true))screenDisplayPolicy.home_page_id=0;
  unsigned count=0;
  for(unsigned i=0;i<screenDisplayPolicy.page_count;i++)if(exists(screenDisplayPolicy.page_overrides[i].page_id,false))screenDisplayPolicy.page_overrides[count++]=screenDisplayPolicy.page_overrides[i];
  if(count<screenDisplayPolicy.page_count)memset(screenDisplayPolicy.page_overrides+count,0,(screenDisplayPolicy.page_count-count)*sizeof(display_page_override));
  screenDisplayPolicy.page_count=count;
  if(memcmp(&old,&screenDisplayPolicy,sizeof(old))) {
    if(++screenDisplayPolicy.revision==0)screenDisplayPolicy.revision=1;
    screenPolicyAck=screenPolicySentAt=0;
  }
}
