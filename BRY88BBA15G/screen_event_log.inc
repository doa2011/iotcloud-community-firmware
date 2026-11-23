/* SPDX-License-Identifier: GPL-3.0-or-later
 * Diagnostic owner runs only from G3's application loop.
 * RAM ring: 128 x 16 bytes (2 KiB). NVS snapshot: one fixed 528-byte
 * blob, at most once per 30 minutes and only after a new ERROR event.
 * Never writes while OTA or a screen upload is active. No credentials.
 */
#include "screen_event_log.h"
static g3diag::Ring screenEventLog;
static g3diag::Saved screenFaultSnapshot={};
static bool screenFaultSnapshotValid=false;
static uint32_t screenLogSavedFaults=0, screenLogSavedAt=0;
static bool screenLogEverSaved=false;
static bool screenLogSnapshotStale=false, screenLogUartSilent=false;
static uint32_t screenLogHeartbeatAt=0;
static void screenLogEvent(uint16_t code,uint8_t severity,uint32_t arg=0) {
  screenEventLog.add(millis(),code,severity,arg);
}
static void screenLogInit() {
  if(prefs.getBytesLength("g3fault")==sizeof(screenFaultSnapshot) &&
     prefs.getBytes("g3fault",&screenFaultSnapshot,sizeof(screenFaultSnapshot))
       ==sizeof(screenFaultSnapshot))
    screenFaultSnapshotValid=g3diag::valid(screenFaultSnapshot);
  screenLogEvent(g3diag::BOOT,g3diag::INFO,esp_reset_reason());
}
static void screenLogPoll(uint32_t now) {
  if(uint32_t(now-screenLogHeartbeatAt)>=1000u) {
    screenLogHeartbeatAt=now;
    const uint32_t rxAge=screenLastRx?uint32_t(now-screenLastRx):0xFFFFu;
    const uint32_t snapshotAge=screenSnapshotAt?uint32_t(now-screenSnapshotAt):0xFFFFu;
    screenLogEvent(g3diag::HEARTBEAT,g3diag::INFO,
                   ((rxAge>65535u?65535u:rxAge)<<16)|(snapshotAge>65535u?65535u:snapshotAge));
  }
  // Allow first boot/association to finish before diagnosing absent data.
  const bool stale=screenSnapshot.containsKey("status")
      ? uint32_t(now-screenSnapshotAt)>=20000u
      : now>=30000u;
  if(stale!=screenLogSnapshotStale) {
    screenLogSnapshotStale=stale;
    screenLogEvent(stale?g3diag::SNAPSHOT_LOST:g3diag::SNAPSHOT_RESTORED,
                   stale?g3diag::ERROR:g3diag::INFO,
                   screenSnapshotAt?uint32_t(now-screenSnapshotAt):0);
  }
  const bool silent=screenLastRx
      ? uint32_t(now-screenLastRx)>=20000u : now>=30000u;
  if(silent!=screenLogUartSilent) {
    screenLogUartSilent=silent;
    screenLogEvent(silent?g3diag::MCU_UART_SILENT:g3diag::MCU_UART_RESTORED,
                   silent?g3diag::WARN:g3diag::INFO,
                   screenLastRx?uint32_t(now-screenLastRx):0);
  }
  // Avoid a write on every boot in a device stuck in a reset loop.
  if(now<60000u || screenEventLog.faults()==screenLogSavedFaults ||
     screenOtaActive() ||
     (screenLogEverSaved&&uint32_t(now-screenLogSavedAt)<1800000u))
    return;
  const g3diag::Saved snapshot=g3diag::capture(screenEventLog);
  if(prefs.putBytes("g3fault",&snapshot,sizeof(snapshot))==sizeof(snapshot)) {
    screenFaultSnapshot=snapshot;
    screenFaultSnapshotValid=true;
    screenLogSavedFaults=screenEventLog.faults();
    screenLogSavedAt=now;
    screenLogEverSaved=true;
  }
}

// Bounded UART trace: decoded RX frames and TX writes. Never store arbitrary
// text payloads: provisioning, tokens, MQTT JSON and OTA image may be secret.
// Preserve command prefix and length; for binary frames record a short HEX
// prefix only when not in an OTA session.
struct ScreenUartTrace {
  uint32_t ms;
  uint16_t length;
  uint8_t direction; // 0=RX, 1=TX
  uint8_t prefix_length;
  uint8_t prefix[16];
};
static ScreenUartTrace screenUartTrace[256]={};
static uint16_t screenUartTraceNext=0,screenUartTraceCount=0;
static uint32_t screenUartTraceOverwritten=0;
static void screenTraceUart(bool tx,const uint8_t *bytes,size_t n,size_t payloadAt) {
  if(!bytes||!n)return;
  ScreenUartTrace &e=screenUartTrace[screenUartTraceNext];
  e.ms=millis();e.length=n>65535?65535:n;e.direction=tx?1:0;
  e.prefix_length=0;
  // Only a command identifier, never argument, credentials or payload.
  // TX +sub_recv may contain JSON; capture its fixed command name only.
  if(!screenOtaActive() && !payloadAt) {
    while(e.prefix_length<sizeof(e.prefix) && e.prefix_length<n) {
      uint8_t b=bytes[e.prefix_length];
      if(b==' '||b=='\\r'||b=='\\n'||b==':'||b==','||b<0x21||b>0x7e)break;
      e.prefix[e.prefix_length++]=b;
    }
  }
  screenUartTraceNext=(screenUartTraceNext+1)%256;
  if(screenUartTraceCount<256)++screenUartTraceCount;
  else ++screenUartTraceOverwritten;
}

static void screenLogStatus(JsonObject out) {
  out["schema"]=1;
  out["ram_capacity"]=g3diag::CAPACITY;
  out["ram_used"]=screenEventLog.size();
  out["ram_overwritten"]=screenEventLog.overwritten();
  out["fault_events"]=screenEventLog.faults();
  out["persistent_bytes_max"]=sizeof(g3diag::Saved);
  out["persistent_valid"]=screenFaultSnapshotValid;
  out["persistent_events"]=screenFaultSnapshotValid?screenFaultSnapshot.count:0;
  out["snapshot_stale"]=screenLogSnapshotStale;
  out["mcu_uart_silent"]=screenLogUartSilent;
  out["last_persist_uptime_ms"]=screenLogEverSaved?screenLogSavedAt:0;
}
static void screenLogRead(JsonObject out) {
  screenLogStatus(out);
  JsonArray live=out.createNestedArray("recent");
  const size_t limit=screenEventLog.size()<16?screenEventLog.size():16;
  for(size_t i=0;i<limit;i++){
    const auto &r=screenEventLog.at(i);
    JsonArray row=live.createNestedArray();
    row.add(r.uptime_ms);row.add(r.code);row.add(r.severity);
    row.add(r.argument);row.add(r.repeat);
  }
  if(screenFaultSnapshotValid){
    JsonArray saved=out.createNestedArray("saved");
    const size_t limit=screenFaultSnapshot.count<16?screenFaultSnapshot.count:16;
    for(size_t i=0;i<limit;i++){
      const auto &r=screenFaultSnapshot.entries[i];
      JsonArray row=saved.createNestedArray();
      row.add(r.uptime_ms);row.add(r.code);row.add(r.severity);
      row.add(r.argument);row.add(r.repeat);
    }
  }
}
