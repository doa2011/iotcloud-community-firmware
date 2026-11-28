#pragma once
#include <ArduinoJson.h>
#include <memory>
#include <new>

// For mutable-input ArduinoJson, string bytes live in the upload, not in
// the pool. Each collection needs at most one slot per comma plus one;
// count opening collections too (empty collections merely overestimate).
// Ignore quoted punctuation, including escaped quotes and backslashes.
static size_t backupJsonPoolCapacity(const char *data, size_t bytes, size_t limit) {
  if (!data || !bytes || limit < 256) return 0;
  size_t slots = 0;
  const size_t maxSlots = (limit - 256) / JSON_ARRAY_SIZE(1);
  bool quoted = false, escaped = false;
  for (size_t i = 0; i < bytes; ++i) {
    const char ch = data[i];
    if (quoted) {
      if (escaped) escaped = false;
      else if (ch == '\\') escaped = true;
      else if (ch == '"') quoted = false;
    } else if (ch == '"') quoted = true;
    else if (ch == '{' || ch == '[' || ch == ',') {
      if (++slots > maxSlots) return 0;
    }
  }
  if (quoted || escaped) return 0;
  return JSON_ARRAY_SIZE(slots) + 256;
}

// A zero-copy variant can contain linked keys and strings. Never use a
// shallow copy for state that must outlive the original HTTP upload.
// Serialize only the small owned subtree, then parse through the const
// overload to ensure that all keys/strings are owned by the destination.
template<typename Source>
static bool backupJsonCopyOwned(JsonDocument &target, const Source &source) {
  const size_t bytes = measureMsgPack(source);
  if (!bytes || !target.capacity()) return false;
  std::unique_ptr<char[]> wire(new (std::nothrow) char[bytes]);
  if (!wire || serializeMsgPack(source, wire.get(), bytes) != bytes) return false;
  return !deserializeMsgPack(target, static_cast<const char *>(wire.get()), bytes)
      && !target.overflowed();
}
