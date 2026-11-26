#pragma once

#include <memory>
#include <new>

#include "protocol/appearance.h"
#include "protocol/display_policy.h"
#include "protocol/scene.h"
#include "protocol/services.h"

/* Candidate state for validating a complete screen backup before changing
 * live configuration. This object is intentionally heap-allocated: the
 * reminder arrays and scene model together exceed the ESP32 loop-task stack.
 */
struct ScreenConfigRestoreCandidate {
  screen_scene scene = {};
  display_policy policy = {};
  screen_appearance appearance = {};
  reminder_schedule schedule = {};
  reminder_notice notices[REMINDER_MAX] = {};
};

inline std::unique_ptr<ScreenConfigRestoreCandidate>
screenConfigRestoreCandidateCreate() {
  return std::unique_ptr<ScreenConfigRestoreCandidate>(
      new (std::nothrow) ScreenConfigRestoreCandidate());
}
