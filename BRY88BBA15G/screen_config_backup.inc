/* Authenticated G3 configuration backup payload.
 * Includes the ESP32-owned screen configuration record, but deliberately does
 * not copy the MCU's independent font bank or wallpaper pixel bank.
 */
static bool screenConfigBackupCapture(JsonObject parent) {
  if (!screenStoreAvailable || !screenStore.owned || !screenStoreMutex)
    return false;
  if (!screenSceneWire.prepare(screenScene))
    return false;
  String layout = screenSceneBase64();
  if (layout.length() != 4 * ((scene_wire_size(&screenScene) + 2) / 3))
    return false;
  uint8_t policyBytes[DISPLAY_POLICY_BYTES];
  if (!display_policy_encode(&screenDisplayPolicy, policyBytes,
                             sizeof(policyBytes)))
    return false;
  String policy = screenBase64(policyBytes, sizeof(policyBytes));
  if (policy.length() != 4 * ((sizeof(policyBytes) + 2) / 3))
    return false;
  uint8_t appearanceBytes[APPEARANCE_BYTES];
  if (!appearance_encode(&screenAppearance, appearanceBytes,
                         sizeof(appearanceBytes)))
    return false;
  String appearance = screenBase64(appearanceBytes, sizeof(appearanceBytes));
  if (appearance.length() != 4 * ((sizeof(appearanceBytes) + 2) / 3))
    return false;
  DynamicJsonDocument doc(screenConfigCapacity(screenWeatherConfig,
      screenSavedSettings, screenSchedule, screenNotices));
  if (!doc.capacity() ||
      !screenConfigDocument(doc, layout.c_str(), screenWeatherConfig,
          screenSavedSettings, screenSchedule, screenNotices,
          policy.c_str(), appearance.c_str(), screenStartupAuto) ||
      doc.overflowed())
    return false;
  parent["screen"] = doc.as<JsonObjectConst>();
  return !parent.overflowed();
}

static bool screenBackupPageValid(const screen_scene &scene, unsigned page) {
  return page == DISPLAY_KEEP_PAGE ||
      (page < scene.pages && scene.page[page].enabled);
}
static bool screenBackupPageId(const screen_scene &scene, uint32_t id,
                               bool enabled) {
  for (unsigned i = 0; i < scene.pages; ++i)
    if (scene.page[i].id == id && (!enabled || scene.page[i].enabled))
      return true;
  return false;
}
static bool screenBackupWeatherValid(JsonObjectConst weather) {
  if (!weather.size()) return true;
  if (!weather["enabled"].is<bool>() ||
      !weather["provider"].is<const char *>() ||
      !weather["latitude"].is<float>() ||
      !weather["longitude"].is<float>() ||
      !screenText(weather["label"], 49))
    return false;
  const char *provider = weather["provider"] | "";
  const char *host = weather["host"] | "";
  const char *key = weather["key"] | "";
  unsigned interval = weather["interval"] | 900u;
  float latitude = weather["latitude"], longitude = weather["longitude"];
  if ((strcmp(provider, "qweather") && strcmp(provider, "seniverse") &&
       strcmp(provider, "openweather") && strcmp(provider, "openmeteo")) ||
      strlen(host) > 127 || strlen(key) > 256 ||
      interval < 300 || interval > 86400 ||
      !isfinite(latitude) || latitude < -90 || latitude > 90 ||
      !isfinite(longitude) || longitude < -180 || longitude > 180 ||
      (weather.containsKey("verify_tls") &&
       !weather["verify_tls"].is<bool>()))
    return false;
  for (const char *p = key; *p; ++p)
    if (!isalnum((unsigned char)*p) && *p != '-' && *p != '_')
      return false;
  if ((weather["enabled"] | false) && strcmp(provider, "openmeteo") && !*key)
    return false;
  if (!strcmp(provider, "qweather")) {
    String h = host;
    if (h.length() < 16 || h.length() > 127 ||
        !h.endsWith(".qweatherapi.com"))
      return false;
    for (char c : h)
      if (!isalnum((unsigned char)c) && c != '-' && c != '.')
        return false;
  }
  return true;
}

/* Validate the complete candidate before changing live state. Persistence
 * writes the inactive SCFG slot and commits it only after readback CRC passes.
 * On any write failure, restore the previous in-memory configuration.
 */
static const char *screenConfigBackupRestore(JsonObjectConst backup) {
  if (!screenStoreAvailable || !screenStoreMutex)
    return "storage_error";
  if (backup.size() != 11 || backup["schema"] != 2 ||
      !backup["startup_auto_resume"].is<bool>() ||
      !backup["layout"].is<const char *>() ||
      !backup["policy"].is<const char *>() ||
      !backup["appearance"].is<const char *>() ||
      !backup["weather"].is<JsonObjectConst>() ||
      !backup["settings"].is<JsonObjectConst>() ||
      !backup["timezone"].is<int>() ||
      !backup["wake"].is<unsigned>() ||
      !backup["rules"].is<JsonArrayConst>() ||
      !backup["notices"].is<JsonArrayConst>())
    return "invalid_backup";

  std::unique_ptr<uint8_t[]> layout(new (std::nothrow) uint8_t[SCENE_BYTES]);
  if (!layout) return "busy";
  size_t layoutBytes = 0;
  screen_scene nextScene = {};
  if (!screenUnbase64(backup["layout"], layout.get(), SCENE_BYTES,
                      layoutBytes) ||
      layoutBytes != SCENE_BYTES ||
      !scene_decode(&nextScene, layout.get(), layoutBytes) ||
      !scene_validate(&nextScene))
    return "invalid_backup";

  uint8_t policyBytes[DISPLAY_POLICY_BYTES], appearanceBytes[APPEARANCE_BYTES];
  size_t policyLength = 0, appearanceLength = 0;
  display_policy nextPolicy = {};
  screen_appearance nextAppearance = {};
  if (!screenUnbase64(backup["policy"], policyBytes, sizeof(policyBytes),
                      policyLength) ||
      policyLength != sizeof(policyBytes) ||
      !display_policy_decode(&nextPolicy, policyBytes, policyLength) ||
      !display_policy_valid(&nextPolicy) ||
      !screenUnbase64(backup["appearance"], appearanceBytes,
                      sizeof(appearanceBytes), appearanceLength) ||
      appearanceLength != sizeof(appearanceBytes) ||
      !appearance_decode(&nextAppearance, appearanceBytes, appearanceLength))
    return "invalid_backup";
  if (!screenBackupPageValid(nextScene, nextPolicy.idle_page))
    return "invalid_backup";
  for (unsigned i = 0; i < nextPolicy.count; ++i)
    if (!screenBackupPageValid(nextScene, nextPolicy.plans[i].page))
      return "invalid_backup";
  if (nextPolicy.home_page_id &&
      !screenBackupPageId(nextScene, nextPolicy.home_page_id, true))
    return "invalid_backup";
  for (unsigned i = 0; i < nextPolicy.page_count; ++i)
    if (!screenBackupPageId(nextScene,
            nextPolicy.page_overrides[i].page_id, false))
      return "invalid_backup";

  DynamicJsonDocument nextWeather(screenWeatherConfigBytes);
  DynamicJsonDocument nextSettings(1024);
  if (!nextWeather.set(backup["weather"]) ||
      !nextSettings.set(backup["settings"]) ||
      nextWeather.overflowed() || nextSettings.overflowed() ||
      !screenBackupWeatherValid(nextWeather.as<JsonObjectConst>()))
    return "invalid_backup";
  if (nextSettings.containsKey("tvoc_warmup_minutes")) {
    JsonVariant value = nextSettings["tvoc_warmup_minutes"];
    if (!value.is<int>() || value.as<int>() < -1 ||
        value.as<int>() == 0 || value.as<int>() > 60)
      return "invalid_backup";
  }

  const int timezone = backup["timezone"].as<int>();
  const unsigned wake = backup["wake"].as<unsigned>();
  if (timezone < -720 || timezone > 840 || wake > 2 ||
      backup["rules"].size() > REMINDER_MAX ||
      backup["notices"].size() > REMINDER_MAX)
    return "invalid_backup";
  reminder_schedule nextSchedule = {};
  nextSchedule.timezone = timezone;
  nextSchedule.wake = wake;
  reminder_notice nextNotices[REMINDER_MAX] = {};
  unsigned index = 0;
  for (JsonObjectConst o : backup["rules"].as<JsonArrayConst>()) {
    if (o.size() != 10 || !o["id"].is<uint32_t>() ||
        !o["kind"].is<unsigned>() || !o["epoch"].is<uint32_t>() ||
        !o["minute"].is<unsigned>() || !o["days"].is<unsigned>() ||
        !o["wake"].is<unsigned>() || !o["enabled"].is<bool>() ||
        !screenText(o["title"], 65) || !screenText(o["text"], 129) ||
        !o["last_fire"].is<uint32_t>())
      return "invalid_backup";
    reminder_rule r = {};
    r.id = o["id"]; r.kind = o["kind"]; r.epoch = o["epoch"];
    r.minute = o["minute"]; r.days = o["days"]; r.wake = o["wake"];
    r.enabled = o["enabled"]; r.last_fire = o["last_fire"];
    bry::textCopy(r.title, sizeof(r.title), o["title"]);
    bry::textCopy(r.text, sizeof(r.text), o["text"]);
    if (!r.id || r.id >= 0x80000000u || !reminder_validate(&r))
      return "invalid_backup";
    for (unsigned i = 0; i < index; ++i)
      if (nextSchedule.rules[i].id == r.id) return "invalid_backup";
    nextSchedule.rules[index++] = r;
  }
  index = 0;
  for (JsonObjectConst o : backup["notices"].as<JsonArrayConst>()) {
    if (o.size() != 6 || !o["id"].is<uint32_t>() ||
        !o["at"].is<uint32_t>() || !o["wake"].is<unsigned>() ||
        !o["unread"].is<bool>() || !screenText(o["title"], 65) ||
        !screenText(o["text"], 129))
      return "invalid_backup";
    reminder_notice r = {};
    r.id = o["id"]; r.at = o["at"]; r.wake = o["wake"];
    r.unread = o["unread"];
    bry::textCopy(r.title, sizeof(r.title), o["title"]);
    bry::textCopy(r.text, sizeof(r.text), o["text"]);
    if (!r.id || r.wake > 2)
      return "invalid_backup";
    for (unsigned i = 0; i < index; ++i)
      if (nextNotices[i].id == r.id) return "invalid_backup";
    nextNotices[index++] = r;
  }

  DynamicJsonDocument oldWeather(screenWeatherConfigBytes);
  DynamicJsonDocument oldSettings(1024);
  if (!oldWeather.set(screenWeatherConfig) ||
      !oldSettings.set(screenSavedSettings) ||
      oldWeather.overflowed() || oldSettings.overflowed())
    return "busy";
  const screen_scene oldScene = screenScene;
  const display_policy oldPolicy = screenDisplayPolicy;
  const screen_appearance oldAppearance = screenAppearance;
  const reminder_schedule oldSchedule = screenSchedule;
  reminder_notice oldNotices[REMINDER_MAX];
  memcpy(oldNotices, screenNotices, sizeof(oldNotices));
  const bool oldStartupAuto = screenStartupAuto;

  screenScene = nextScene;
  screenDisplayPolicy = nextPolicy;
  screenAppearance = nextAppearance;
  screenSchedule = nextSchedule;
  memcpy(screenNotices, nextNotices, sizeof(nextNotices));
  screenStartupAuto = backup["startup_auto_resume"].as<bool>();
  screenWeatherConfig.set(nextWeather);
  screenSavedSettings.set(nextSettings);
  if (screenWeatherConfig.overflowed() || screenSavedSettings.overflowed() ||
      !screenSave()) {
    screenScene = oldScene;
    screenDisplayPolicy = oldPolicy;
    screenAppearance = oldAppearance;
    screenSchedule = oldSchedule;
    memcpy(screenNotices, oldNotices, sizeof(oldNotices));
    screenStartupAuto = oldStartupAuto;
    screenWeatherConfig.set(oldWeather);
    screenSavedSettings.set(oldSettings);
    screenStorageError = "config_restore_write";
    return "storage_error";
  }

  screenSceneAck = screenSceneSession = 0;
  screenSyncStep = screenSyncDeadline = screenSyncRetryAt = 0;
  screenSyncError = "";
  screenPolicyAck = screenPolicySentAt = screenPolicySession = 0;
  screenPolicyError = "";
  screenAppearanceAck = screenAppearanceSession = screenAppearanceSentAt = 0;
  screenAppearanceAttempts = 0;
  screenAppearanceError = "";
  screenSettingsSession = 0;
  screenSchedule.started = false;
  screenWeatherGeneration++;
  screenWeather = {};
  screenWeatherRetry = {};
  screenWeatherRefresh = screenWeatherConfig["enabled"] | false;
  screenWeatherStockPending = true;
  screenWeatherError = "";
  screenNoticeId = 0x80000000u;
  for (const auto &r : screenNotices)
    if (r.id >= screenNoticeId)
      screenNoticeId = r.id == UINT32_MAX ? 0x80000000u : r.id + 1;
  invalidateBusinessStatusCache();
  return "ok";
}
