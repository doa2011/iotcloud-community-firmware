#pragma once
#include <stdint.h>
#include <string.h>
#include <ArduinoJson.h>

enum ScreenSettingsRestorePhase : uint8_t {
  SCREEN_SETTINGS_RESTORE_IDLE = 0,
  SCREEN_SETTINGS_RESTORE_WAIT_ACK = 1,
  SCREEN_SETTINGS_RESTORE_WAIT_READBACK = 2,
  SCREEN_SETTINGS_RESTORE_CONFIRMED = 3,
  SCREEN_SETTINGS_RESTORE_FAILED = 4
};

struct ScreenSettingsRestoreState {
  uint32_t session = 0;
  uint32_t retry_at = 0;
  uint32_t snapshot_at = 0;
  unsigned attempts = 0;
  int last_code = -1;
  ScreenSettingsRestorePhase phase = SCREEN_SETTINGS_RESTORE_IDLE;
};

static inline bool screenSettingsRestoreDue(uint32_t now, uint32_t deadline) {
  return int32_t(now - deadline) >= 0;
}

static inline void screenSettingsRestoreReset(ScreenSettingsRestoreState &s,
                                               uint32_t session) {
  s = ScreenSettingsRestoreState{};
  s.session = session;
}

static inline bool screenSettingsRestoreReady(ScreenSettingsRestoreState &s,
                                               uint32_t session,
                                               uint32_t now) {
  if (s.session != session) screenSettingsRestoreReset(s, session);
  if (s.phase == SCREEN_SETTINGS_RESTORE_FAILED &&
      screenSettingsRestoreDue(now, s.retry_at)) {
    s.attempts = 0;
    s.phase = SCREEN_SETTINGS_RESTORE_IDLE;
    s.retry_at = now;
  }
  return s.phase == SCREEN_SETTINGS_RESTORE_IDLE && s.attempts < 3 &&
         screenSettingsRestoreDue(now, s.retry_at);
}

static inline void screenSettingsRestoreBegin(ScreenSettingsRestoreState &s,
                                               uint32_t session, uint32_t now,
                                               uint32_t snapshot_at) {
  if (s.session != session) screenSettingsRestoreReset(s, session);
  ++s.attempts;
  s.snapshot_at = snapshot_at;
  s.last_code = -1;
  s.phase = SCREEN_SETTINGS_RESTORE_WAIT_ACK;
  s.retry_at = now + 10000u;
}

static inline void screenSettingsRestoreRetry(ScreenSettingsRestoreState &s,
                                               uint32_t now, int code) {
  s.last_code = code;
  s.phase = s.attempts >= 3 ? SCREEN_SETTINGS_RESTORE_FAILED
                            : SCREEN_SETTINGS_RESTORE_IDLE;
  s.retry_at = now + (s.attempts >= 3 ? 30000u : 2000u);
}

static inline bool screenSettingsRestoreAck(ScreenSettingsRestoreState &s,
                                             uint32_t session, uint32_t now,
                                             int code) {
  if (s.session != session || s.phase != SCREEN_SETTINGS_RESTORE_WAIT_ACK)
    return false;
  s.last_code = code;
  if (code == 0) {
    s.phase = SCREEN_SETTINGS_RESTORE_WAIT_READBACK;
    s.retry_at = now + 5000u;
    return true;
  }
  screenSettingsRestoreRetry(s, now, code);
  return false;
}

static inline void screenSettingsRestoreReadback(ScreenSettingsRestoreState &s,
                                                  uint32_t session,
                                                  uint32_t now, bool matches) {
  if (s.session != session ||
      s.phase != SCREEN_SETTINGS_RESTORE_WAIT_READBACK)
    return;
  if (matches) {
    s.phase = SCREEN_SETTINGS_RESTORE_CONFIRMED;
    s.last_code = 0;
    return;
  }
  screenSettingsRestoreRetry(s, now, -4);
}


static inline bool screenSettingsRestoreAckTimeout(
    ScreenSettingsRestoreState &s, uint32_t session, uint32_t now,
    int code = -2) {
  if (s.session != session ||
      s.phase != SCREEN_SETTINGS_RESTORE_WAIT_ACK ||
      !screenSettingsRestoreDue(now, s.retry_at))
    return false;
  screenSettingsRestoreRetry(s, now, code);
  return true;
}

static inline void screenSettingsRestoreTransportFailure(
    ScreenSettingsRestoreState &s, uint32_t session, uint32_t now, int code) {
  if (s.session != session) screenSettingsRestoreReset(s, session);
  if (s.phase == SCREEN_SETTINGS_RESTORE_IDLE ||
      s.phase == SCREEN_SETTINGS_RESTORE_WAIT_ACK) {
    if (s.phase == SCREEN_SETTINGS_RESTORE_IDLE) ++s.attempts;
    screenSettingsRestoreRetry(s, now, code);
  }
}

static inline bool screenSettingsJsonMatches(JsonVariantConst expected,
                                             JsonVariantConst actual) {
  if (expected.isNull()) return actual.isNull();
  if (expected.is<JsonObjectConst>()) {
    if (!actual.is<JsonObjectConst>()) return false;
    JsonObjectConst e = expected.as<JsonObjectConst>();
    JsonObjectConst a = actual.as<JsonObjectConst>();
    for (JsonPairConst pair : e) {
      if (!a.containsKey(pair.key()) ||
          !screenSettingsJsonMatches(pair.value(), a[pair.key()]))
        return false;
    }
    return true;
  }
  if (expected.is<JsonArrayConst>()) {
    if (!actual.is<JsonArrayConst>()) return false;
    JsonArrayConst e = expected.as<JsonArrayConst>();
    JsonArrayConst a = actual.as<JsonArrayConst>();
    if (e.size() != a.size()) return false;
    for (size_t i = 0; i < e.size(); ++i)
      if (!screenSettingsJsonMatches(e[i], a[i])) return false;
    return true;
  }
  if (expected.is<bool>())
    return actual.is<bool>() && expected.as<bool>() == actual.as<bool>();
  if (expected.is<const char *>()) {
    if (!actual.is<const char *>()) return false;
    const char *e = expected.as<const char *>();
    const char *a = actual.as<const char *>();
    return e && a && strcmp(e, a) == 0;
  }
  if (expected.is<double>())
    return actual.is<double>() &&
           expected.as<double>() == actual.as<double>();
  return false;
}

static inline const char *screenSettingsRestorePhaseName(
    ScreenSettingsRestorePhase phase) {
  switch (phase) {
  case SCREEN_SETTINGS_RESTORE_WAIT_ACK: return "waiting_ack";
  case SCREEN_SETTINGS_RESTORE_WAIT_READBACK: return "waiting_readback";
  case SCREEN_SETTINGS_RESTORE_CONFIRMED: return "confirmed";
  case SCREEN_SETTINGS_RESTORE_FAILED: return "failed";
  default: return "idle";
  }
}
