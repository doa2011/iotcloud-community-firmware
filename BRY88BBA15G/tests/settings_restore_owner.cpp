#include "screen_settings_restore.h"
#include <cassert>
#include <cstdio>

int main() {
  StaticJsonDocument<512> expected, actual;
  expected["display"]["luminance"] = 22;
  expected["display"]["data_type"] = 0;
  expected["display"]["power_screen_time"] = 2;
  expected["display"]["battery_screen_time"] = 1;
  expected["no_disturb"]["state"] = true;
  actual.set(expected);
  actual["display"]["luminance"] = 68;
  assert(!screenSettingsJsonMatches(expected.as<JsonVariantConst>(),
                                    actual.as<JsonVariantConst>()));
  actual["display"]["luminance"] = 22;
  assert(screenSettingsJsonMatches(expected.as<JsonVariantConst>(),
                                   actual.as<JsonVariantConst>()));
  actual["no_disturb"]["state"] = false;
  assert(!screenSettingsJsonMatches(expected.as<JsonVariantConst>(),
                                    actual.as<JsonVariantConst>()));

  ScreenSettingsRestoreState state;
  assert(screenSettingsRestoreReady(state, 101, 100));
  screenSettingsRestoreBegin(state, 101, 100, 90);
  assert(state.phase == SCREEN_SETTINGS_RESTORE_WAIT_ACK);
  assert(!screenSettingsRestoreReady(state, 101, 101));
  screenSettingsRestoreAck(state, 101, 200, 0);
  assert(state.phase == SCREEN_SETTINGS_RESTORE_WAIT_READBACK);
  screenSettingsRestoreReadback(state, 101, 201, false);
  assert(state.phase == SCREEN_SETTINGS_RESTORE_IDLE);
  assert(state.last_code == -4 && state.retry_at == 2201);
  assert(!screenSettingsRestoreReady(state, 101, 2200));
  assert(screenSettingsRestoreReady(state, 101, 2201));

  screenSettingsRestoreBegin(state, 101, 2201, 200);
  screenSettingsRestoreAck(state, 101, 2202, 0);
  screenSettingsRestoreReadback(state, 101, 2203, true);
  assert(state.phase == SCREEN_SETTINGS_RESTORE_CONFIRMED);
  assert(!screenSettingsRestoreReady(state, 101, 9999));

  screenSettingsRestoreReset(state, 202);
  assert(screenSettingsRestoreReady(state, 202, 0));
  screenSettingsRestoreBegin(state, 202, 0, 0);
  screenSettingsRestoreAck(state, 202, 1, 7);
  assert(state.phase == SCREEN_SETTINGS_RESTORE_IDLE);
  assert(state.last_code == 7 && state.retry_at == 2001);
  assert(!screenSettingsRestoreReady(state, 202, 2000));
  assert(screenSettingsRestoreReady(state, 202, 2001));

  screenSettingsRestoreBegin(state, 202, 2001, 0);
  screenSettingsRestoreAck(state, 202, 2002, -2);
  screenSettingsRestoreBegin(state, 202, 4002, 0);
  screenSettingsRestoreAck(state, 202, 4003, -2);
  assert(state.phase == SCREEN_SETTINGS_RESTORE_FAILED);
  assert(state.retry_at == 34003);
  assert(!screenSettingsRestoreReady(state, 202, 34002));
  assert(screenSettingsRestoreReady(state, 202, 34003));
  assert(state.attempts == 0);

  screenSettingsRestoreBegin(state, 0xFFFFFFFFu, 0xFFFFFF00u, 0);
  screenSettingsRestoreAck(state, 0xFFFFFFFFu, 0xFFFFFF10u, 0);
  screenSettingsRestoreReadback(state, 0xFFFFFFFFu, 0xFFFFFF20u, false);
  assert(state.phase == SCREEN_SETTINGS_RESTORE_IDLE);
  assert(screenSettingsRestoreReady(state, 0xFFFFFFFFu, 0x000006E8u));
  std::puts("Settings restore owner: ACK, retry, readback, mismatch, "
            "session reset and millis wrap passed");
}
