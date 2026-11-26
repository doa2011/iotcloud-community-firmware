#pragma once

#include <memory>
#include <new>
#include <type_traits>
#include <cstring>

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

/* A fully validated candidate can become the rollback snapshot by exchanging
 * its plain-data fields with live configuration. A bounded scratch buffer
 * avoids a second full candidate allocation or large loop-task stack frame. */
template<typename T>
inline void screenConfigRestoreExchange(T &live, T &candidate) {
  static_assert(std::is_trivially_copyable<T>::value, "plain config required");
  unsigned char scratch[128];
  auto *a = reinterpret_cast<unsigned char *>(&live);
  auto *b = reinterpret_cast<unsigned char *>(&candidate);
  for (size_t at = 0; at < sizeof(T); at += sizeof(scratch)) {
    const size_t remaining = sizeof(T) - at;
    const size_t n = remaining < sizeof(scratch) ? remaining : sizeof(scratch);
    std::memcpy(scratch, a + at, n);
    std::memcpy(a + at, b + at, n);
    std::memcpy(b + at, scratch, n);
  }
}
