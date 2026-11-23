# G3 bounded field diagnostic logging (phase 1)

This is a **diagnostic-only** implementation and does not repair MCU OTA or sensor transport by itself. Only G3 / BRY88BBA15G is touched.

## Implementation and storage limits

- RAM: `128 * 16 = 2048 bytes` event ring, newest first, overwritten oldest.
- Persistent NVS: **one fixed 528-byte blob**, key `g3fault` in the existing `bry88-screen` namespace. The blob contains the latest 32 events and CRC-like FNV-1a checksum, version and count. It is written **only if a new ERROR event occurred**, no more often than once per 30 minutes in the current boot, and never while an MCU OTA or ESP OTA owns the protocol. NVS is not a filesystem or a 256 KiB rotating log.
- No arbitrary text, UART payloads, MQTT traffic, credentials, personal data, heap allocations, or per-sensor high-rate samples are logged.
- Records carry `uptime_ms, code, severity, argument, repeat`. Adjacent identical events within 30 seconds are coalesced.
- The previous saved snapshot is loaded at startup and retained until replaced. An unclean reset may lose ordinary RAM records or faults that have not yet been persisted.
- The normal G3 OTA/partition table is **unchanged**. Addresses `0x3d0000..0x6fffff` have not been proven safe for overwrite, and `0x700000..0x7fffff` is owned by the screen resource store. Do not auto-provision a raw flash partition.

## Read the log

Use the existing authenticated `POST /api/v1/command` API:

```json
{"action":"screen.log.get","params":{}}
```

Read-only response `data` contains:
- `schema`, `ram_capacity`, `ram_used`, `ram_overwritten`, `fault_events`
- `persistent_bytes_max`, `persistent_valid`, `persistent_events`
- `snapshot_stale`, `mcu_uart_silent`, `last_persist_uptime_ms`
- `recent`: newest 16 RAM events
- `saved`: newest 16 of the last durable fault snapshot, when valid

Each row is `[uptime_ms, code, severity, argument, repeat]`, with severities `1 INFO / 2 WARN / 3 ERROR`.

The ordinary `status.get` output has only a compact `screen.event_log` summary; it does not include record arrays.

## Event codes

| Code | Meaning | Argument |
|---|---|---|
| 1 | ESP32 boot | ESP reset reason |
| 2 | ESP32-to-MCU UART RX fault | Error timestamp (ms) |
| 3 | MCU snapshot freshness exceeded 20 s | Age of last snapshot (ms) |
| 4 | MCU snapshot received again | Age at recovery (ms) |
| 5 | MCU UART silent 20 s | Age of last frame (ms) |
| 6 | MCU UART traffic resumed | Age at recovery (ms) |
| 7 | Screen MCU OTA image deadline expired | Last byte offset |
| 8 | MCU Guard command deadline expired | Operation number |
| 9 | OTA phase transition | Reserved for later extension |

The first 30 s after ESP32 boot are exempted from initial 'silent'/'snapshot lost' alarms, to allow association and MCU boot.

## Scope and next phase

This first stage adds bounded, persistent **ESP32-side** evidence with no MCU binary or protocol changes. It should be compiled and field-tested with the existing MCU. A later MCU branch can add upstream UART1/2/3 counters, bounded flash journal once the unallocated flash region is proven safe, browser log viewer and fault-triggered automatic capture. Do not treat a successful host test or CI build as real MCU OTA validation.
