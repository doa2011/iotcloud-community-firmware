#pragma once
#include <ArduinoJson.h>

/* The upstream snapshot can lag a just-acknowledged setting write. The
 * locally persisted request is the reconnect/restore source of truth, so it
 * must overlay the snapshot before status or screen.settings.get is exposed. */
static void screenSettingsSnapshot(JsonObject parent, JsonObjectConst upstream,
                                   JsonObjectConst saved) {
  JsonObject effective = parent.createNestedObject("settings");
  if (upstream) effective.set(upstream);
  for (JsonPairConst pair : saved)
    effective[pair.key()] = pair.value();
  parent["saved_settings"] = saved;
}
