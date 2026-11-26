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

  std::printf("PASS heap restore candidate: %zu bytes\n",
              sizeof(ScreenConfigRestoreCandidate));
}
