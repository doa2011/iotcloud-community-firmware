/* Authenticated G3 configuration backup payload.
 * Includes the ESP32-owned screen configuration record, but deliberately does
 * not copy the MCU's independent font bank or wallpaper pixel bank.
 */
#include "screen_config_restore_candidate.h"

struct ScreenConfigBackupSnapshot {
  std::unique_ptr<ScreenConfigRestoreCandidate> state;
  bool startupAuto = true;
  DynamicJsonDocument weather;
  DynamicJsonDocument settings;
  ScreenConfigBackupSnapshot()
      : weather(screenWeatherConfigBytes), settings(1024) {}
};
static ScreenConfigBackupSnapshot *screenBackupSnapshot = nullptr;

static void screenConfigBackupSnapshotDiscard() {
  delete screenBackupSnapshot;
  screenBackupSnapshot = nullptr;
}
static bool screenConfigBackupSnapshotCapture() {
  if (screenBackupSnapshot)
    return false;
  screenBackupSnapshot = new (std::nothrow) ScreenConfigBackupSnapshot();
  if (!screenBackupSnapshot)
    return false;
  if (!screenBackupSnapshot->weather.set(screenWeatherConfig) ||
      !screenBackupSnapshot->settings.set(screenSavedSettings) ||
      screenBackupSnapshot->weather.overflowed() ||
      screenBackupSnapshot->settings.overflowed()) {
    screenConfigBackupSnapshotDiscard();
    return false;
  }
   screenBackupSnapshot->startupAuto = screenStartupAuto;
  return true;
}
static void screenConfigBackupRuntimeReset() {
  screenSceneAck = screenSceneSession = 0;
  screenSyncStep = screenSyncDeadline = screenSyncRetryAt = 0;
  screenSyncError = "";
  screenPolicyAck = screenPolicySentAt = screenPolicySession = 0;
  screenPolicyError = "";
  screenAppearanceAck = screenAppearanceSession = screenAppearanceSentAt = 0;
  screenAppearanceAttempts = 0;
  screenAppearanceError = "";
  screenSettingsSession = 0;
  screenSettingsRestoreReset(screenSettingsRestoreState, screenBridge.session);
  screenSettingsRestoreToken = "";
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
}
static bool screenConfigBackupSnapshotApply(bool persist) {
  if (!screenBackupSnapshot || !screenBackupSnapshot->state)
    return false;
  const auto &old = *screenBackupSnapshot->state;
  screenScene = old.scene;
  screenDisplayPolicy = old.policy;
  screenAppearance = old.appearance;
  screenSchedule = old.schedule;
  memcpy(screenNotices, old.notices, sizeof(screenNotices));
  screenStartupAuto = screenBackupSnapshot->startupAuto;
  if (!screenWeatherConfig.set(screenBackupSnapshot->weather) ||
      !screenSavedSettings.set(screenBackupSnapshot->settings) ||
      screenWeatherConfig.overflowed() || screenSavedSettings.overflowed())
    return false;
  screenConfigBackupRuntimeReset();
  return !persist || screenSave();
}
static bool screenConfigBackupSnapshotRestore() {
  const bool ok = screenConfigBackupSnapshotApply(true);
  screenConfigBackupSnapshotDiscard();
  return ok;
}
class ScreenBackupCrcWriter {
  uint32_t crc = 0xffffffffu;
public:
  size_t write(uint8_t value) { return write(&value, 1); }
  size_t write(const uint8_t *data, size_t size) {
    for (size_t i = 0; i < size; ++i) {
      crc ^= data[i];
      for (unsigned bit = 0; bit < 8; ++bit)
        crc = (crc >> 1) ^ (0xedb88320u & (0u - (crc & 1u)));
    }
    return size;
  }
  uint32_t value() const { return crc ^ 0xffffffffu; }
};

static bool screenConfigBackupCapture(JsonObject parent) {
  if (!screenStoreAvailable || !screenStoreMutex)
    return false;
  if (!screenStore.owned && !screenSave())
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
  // Compose directly into the envelope; no second JSON pool. Store owned
  // copies of all three temporary Base64 strings before leaving this scope.
  JsonObject doc = parent.createNestedObject("screen");
  if (doc.isNull()) return false;
  screenConfigFields(doc, "", screenWeatherConfig, screenSavedSettings,
      screenSchedule, screenNotices, "", "", screenStartupAuto);
  doc["layout"] = layout;
  doc["policy"] = policy;
  doc["appearance"] = appearance;
  if (doc.size() != 11 ||
      !doc["layout"].is<const char *>() ||
      !doc["policy"].is<const char *>() ||
      !doc["appearance"].is<const char *>() ||
      strlen(doc["layout"].as<const char *>()) != layout.length() ||
      strlen(doc["policy"].as<const char *>()) != policy.length() ||
      strlen(doc["appearance"].as<const char *>()) != appearance.length())
    return false;
  ScreenBackupCrcWriter writer;
  if (serializeMsgPack(doc, writer) != measureMsgPack(doc))
    return false;
  parent["screen_crc32"] = writer.value();
  return parent.containsKey("screen_crc32");
}
static bool screenConfigBackupVerify(JsonObjectConst parent) {
  if (!parent["screen"].is<JsonObjectConst>() ||
      !parent["screen_crc32"].is<uint32_t>())
    return false;
  JsonObjectConst screen = parent["screen"].as<JsonObjectConst>();
  ScreenBackupCrcWriter writer;
  if (serializeMsgPack(screen, writer) != measureMsgPack(screen))
    return false;
  return writer.value() == parent["screen_crc32"].as<uint32_t>();
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
static const char *screenBackupRestoreStage = "idle";
static const char *screenConfigBackupRestore(DynamicJsonDocument &envelope) {
  screenBackupRestoreStage = "screen_envelope";
  JsonObjectConst backup = envelope["screen"].as<JsonObjectConst>();
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

  screenBackupRestoreStage = "screen_candidate";
  auto next = screenConfigRestoreCandidateCreate();
  if (!next) return "busy";
  screenBackupRestoreStage = "layout_buffer";
  std::unique_ptr<uint8_t[]> layout(new (std::nothrow) uint8_t[SCENE_BYTES]);
  if (!layout) return "busy";
  screenBackupRestoreStage = "layout_decode";
  size_t layoutBytes = 0;
  if (!screenUnbase64(backup["layout"], layout.get(), SCENE_BYTES,
                      layoutBytes) ||
      layoutBytes != SCENE_BYTES ||
      !scene_decode(&next->scene, layout.get(), layoutBytes) ||
      !scene_validate(&next->scene))
    return "invalid_backup";
  layout.reset();

  screenBackupRestoreStage = "policy_appearance";
  uint8_t policyBytes[DISPLAY_POLICY_BYTES], appearanceBytes[APPEARANCE_BYTES];
  size_t policyLength = 0, appearanceLength = 0;
  if (!screenUnbase64(backup["policy"], policyBytes, sizeof(policyBytes),
                      policyLength) ||
      policyLength != sizeof(policyBytes) ||
      !display_policy_decode(&next->policy, policyBytes, policyLength) ||
      !display_policy_valid(&next->policy) ||
      !screenUnbase64(backup["appearance"], appearanceBytes,
                      sizeof(appearanceBytes), appearanceLength) ||
      appearanceLength != sizeof(appearanceBytes) ||
      !appearance_decode(&next->appearance, appearanceBytes, appearanceLength))
    return "invalid_backup";
  if (!screenBackupPageValid(next->scene, next->policy.idle_page))
    return "invalid_backup";
  for (unsigned i = 0; i < next->policy.count; ++i)
    if (!screenBackupPageValid(next->scene, next->policy.plans[i].page))
      return "invalid_backup";
  if (next->policy.home_page_id &&
      !screenBackupPageId(next->scene, next->policy.home_page_id, true))
    return "invalid_backup";
  for (unsigned i = 0; i < next->policy.page_count; ++i)
    if (!screenBackupPageId(next->scene,
            next->policy.page_overrides[i].page_id, false))
      return "invalid_backup";

  screenBackupRestoreStage = "weather_settings";
  DynamicJsonDocument nextWeather(screenWeatherConfigBytes);
  DynamicJsonDocument nextSettings(1024);
  if (!nextWeather.capacity() || !nextSettings.capacity()) return "busy";
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

  screenBackupRestoreStage = "reminders";
  const int timezone = backup["timezone"].as<int>();
  const unsigned wake = backup["wake"].as<unsigned>();
  if (timezone < -720 || timezone > 840 || wake > 2 ||
      backup["rules"].size() > REMINDER_MAX ||
      backup["notices"].size() > REMINDER_MAX)
    return "invalid_backup";
  next->schedule.timezone = timezone;
  next->schedule.wake = wake;
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
      if (next->schedule.rules[i].id == r.id) return "invalid_backup";
    next->schedule.rules[index++] = r;
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
      if (next->notices[i].id == r.id) return "invalid_backup";
    next->notices[index++] = r;
  }

  const bool startupAuto = backup["startup_auto_resume"].as<bool>();
  // Every field has been validated and copied. The device config candidate
  // is also owned by the caller. Release the complete input JSON now.
  envelope = DynamicJsonDocument(0);
  screenBackupRestoreStage = "rollback_snapshot";
  if (!screenConfigBackupSnapshotCapture())
    return "busy";

  screenConfigRestoreExchange(screenScene, next->scene);
  screenConfigRestoreExchange(screenDisplayPolicy, next->policy);
  screenConfigRestoreExchange(screenAppearance, next->appearance);
  screenConfigRestoreExchange(screenSchedule, next->schedule);
  screenConfigRestoreExchange(screenNotices, next->notices);
  screenBackupSnapshot->state = std::move(next);
  screenStartupAuto = startupAuto;
  const bool weatherOk = screenWeatherConfig.set(nextWeather);
  const bool settingsOk = screenSavedSettings.set(nextSettings);
  nextWeather = DynamicJsonDocument(0);
  nextSettings = DynamicJsonDocument(0);
  screenBackupRestoreStage = "screen_persist";
  if (!weatherOk || !settingsOk || screenWeatherConfig.overflowed() ||
      screenSavedSettings.overflowed() || !screenSave()) {
    const bool rolledBack = screenConfigBackupSnapshotApply(false);
    screenConfigBackupSnapshotDiscard();
    if (!rolledBack) {
      screenStorageError = "config_restore_rollback";
      return "restore_rollback_failed";
    }
    screenStorageError = "config_restore_write";
    return "storage_error";
  }

  screenConfigBackupRuntimeReset();
  screenBackupRestoreStage = "complete";
  return "ok";
}
