#include "screen_config_restore_candidate.h"

#include <cassert>
#include <cstdio>

static_assert(sizeof(ScreenConfigRestoreCandidate) > 16 * 1024,
              "Restore candidate must stay off the ESP32 loop-task stack");

int main() {
  auto candidate = screenConfigRestoreCandidateCreate();
  assert(candidate);

  candidate->scene.revision = 7;
  candidate->scene.pages = 1;
  candidate->policy.count = 1;
  candidate->appearance.revision = 1;
  candidate->schedule.timezone = 480;
  candidate->schedule.rules[REMINDER_MAX - 1].id = 32;
  candidate->notices[REMINDER_MAX - 1].id = 0x80000020u;

  assert(candidate->scene.revision == 7);
  assert(candidate->schedule.rules[REMINDER_MAX - 1].id == 32);
  assert(candidate->notices[REMINDER_MAX - 1].id == 0x80000020u);

  // Exchange into the live object and retain the previous values in the
  // same heap allocation for rollback; include the last reminder entries.
  auto live = screenConfigRestoreCandidateCreate();
  assert(live);
  live->scene.revision=99;
  live->policy.count=2;
  live->appearance.revision=5;
  live->schedule.timezone=60;
  live->notices[REMINDER_MAX-1].id=42;
  auto exchange = [&]() {
    screenConfigRestoreExchange(live->scene,candidate->scene);
    screenConfigRestoreExchange(live->policy,candidate->policy);
    screenConfigRestoreExchange(live->appearance,candidate->appearance);
    screenConfigRestoreExchange(live->schedule,candidate->schedule);
    screenConfigRestoreExchange(live->notices,candidate->notices);
  };
  exchange();
  assert(live->scene.revision==7 && candidate->scene.revision==99);
  assert(live->schedule.timezone==480 && candidate->schedule.timezone==60);
  assert(live->notices[REMINDER_MAX-1].id==0x80000020u);
  assert(candidate->notices[REMINDER_MAX-1].id==42);
  exchange();
  assert(live->scene.revision==99 && live->policy.count==2);
  assert(live->appearance.revision==5 && live->schedule.timezone==60);
  assert(live->notices[REMINDER_MAX-1].id==42);
  std::printf("PASS heap restore candidate and reversible exchange: %zu bytes\n",
              sizeof(ScreenConfigRestoreCandidate));
}
