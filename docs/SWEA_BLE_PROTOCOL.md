# SWEA EmotiBit BLE protocol, version 1 (draft)

Shared contract between the firmware (`EmotiBit_swea_ble_firmware`, build flag `SWEA_BLE`) and SWEA.Mobile. All multi-byte integers and floats are **little-endian**. Wi-Fi is not used.

Design rules
1. **Data is carried as EmotiBit produces it**: same 2-character TypeTags, same units, same device timestamps (ms since boot) and float values as the packets written to the SD card. No resampling, no unit changes.
2. Binary, compact, versioned, with a sequence number so gaps are detectable and recoverable.
3. All characteristics require an **encrypted, bonded** link (LE Secure Connections, passkey).

## 1. GATT layout

| Item | UUID | Properties | Notes |
|---|---|---|---|
| Service `SWEA Stream` | `3f5737c4-b391-44c7-bd74-f2ee561c9775` | – | advertised, with short name `EmotiBit-<last4 of device id>` |
| `Data` | `f14e01ab-48d0-461a-b54e-49fd7be9901a` | notify | sample records (section 2) |
| `Control` | `1118282f-3dfe-44a7-a12f-6d06987f1092` | write (with response), notify | commands in, responses out (section 4) |
| `Status` | `8c2502e8-eacd-45d1-bd20-361eb5a762a2` | read, notify | section 5 |
| Device Information | `0x180A` | read | manufacturer, model, firmware revision, serial |
| Battery | `0x180F` / `0x2A19` | read, notify | percent |

Connection: central requests MTU 247 (usable notification payload = MTU − 3; fall back to 20 bytes at MTU 23). Preferred connection interval 30–50 ms, no slave latency, supervision timeout 4 s (firmware 0.7; 100–200 ms with latency 4 was too slow on an Android phone for the full profile). The device sends at most one data notification per 25 ms, so Control traffic always gets through.

## 2. Data notifications

```
Notification := Header Record+          (total length <= ATT_MTU - 3)
Header       := ver:u8 (=1)  seq:u16      seq increments by 1 per notification, wraps at 65535
Record       := tag:char[2]  n:u8  t_ms:u32  value:f32[n]
```
- `tag`: the EmotiBit TypeTag (`EA`, `EL`, `ER`, `PI`, `PR`, `PG`, `T0`, `T1`, `TH`, `H0`, `AX`…`MZ`, `HR`, `BI`, `SA`, `SR`, `SF`, `BV`, `B%`, `SD`, plus event tags `BS`, `BL`, `DC`, `DO`, `UN`). Exactly as in `upstream-src/EmotiBitPacket.cpp`.
- `t_ms`: the EmotiBit packet timestamp (uint32 ms since boot) exactly as the firmware stamps the same packet on the SD card. The consumer interprets it the way the EmotiBit DataParser does.
- `n` floats follow. A record is never split across notifications. If `6 + 4n` exceeds the room, the firmware emits several records with the **same** `t_ms` and consecutive halves; this is documented behaviour of the packet batching and consumers append in order.
- Order within a tag is preserved; ordering across tags is not guaranteed.
- The consumer must treat unknown tags as opaque (skip `6 + 4n` bytes) so new tags do not break old apps.

Gap detection: if `seq` is not previous+1 (mod 65536) the app has missed notifications (only possible around disconnects or buffer overrun). It then issues `RESUME_FROM` (section 4).

## 3. Ring buffer and reliability
- The device keeps the newest **records** in a RAM ring (default 24 KB, `SWEA_BLE_RING_BYTES`; roughly 20 s at the full profile, 1–2 kB/s). When it is full the oldest records are dropped and counted in `Status.dropped`. Notifications are built from the ring at send time. Longer history exists only on the SD card (CSV), which is never modified by this protocol.
- A notification is only sent when a bonded phone has subscribed to `Data`; if the BLE stack reports back-pressure the records stay in the ring and are retried. Replay by `seq` (`RESUME_FROM`) uses a window of the last 32 sent notifications (`SWEA_BLE_WINDOW_FRAMES`, about 8 KB); data older than that is only on the SD card.
- The app acknowledges progress with `ACK(seq)` at least every 5 s while streaming; the device frees window slots up to and including `seq`. `ACK` has no reply.
- After reconnecting the app sends `RESUME_FROM(last_seq+1)`. The device replays from that sequence if still in the window, then continues live data (replay has priority over new records). The reply carries the first replayed sequence and `flags` bit0 = gap. If the requested frames are no longer held, replay starts at `Status.oldest_seq`, the gap bit is set in both the reply and `Status.flags`, and the missing part must be retrieved from the SD card later (FTP/USB), and the app marks the gap in its data.
- If the phone is absent the device keeps recording to the SD card and keeps sampling; BLE is optional for acquisition.

### Session mode and the SD outbox (firmware 0.11)
- A **session** starts with `START_STREAM` and ends with `STOP_STREAM`. It survives a disconnect, so a phone that drops out and reconnects does not reset anything (`START_STREAM` while a session is active only changes the profile). `Status.state` is 3 while a session is active. Without a session nothing is turned into notifications.
- During a session notifications are built from the ring with a **continuous `seq` even when no phone is connected** (or subscribed). They are kept in the RAM window and appended to `/swea/outbox.bin` on the SD card as records `seq:u16, len:u8, bytes` (the notification exactly as it is sent). Frames sent live to a phone are not written to the outbox while it is empty, except when the ring is more than 3/4 full (a backlog): then frames go to the outbox instead of waiting in the ring. **The outbox holds one contiguous run of `seq` numbers** (firmware 0.12): when a run starts, the unacknowledged frames of the RAM window (128 frames, enough for the 3-4 s before a dead link is noticed) are written first, and while a run holds undelivered notifications every live frame is appended to it too, so that nothing the phone has not asked for yet can leave a hole. The run is deleted when the phone's `ACK` covers its last frame (no replay running), at `STOP_STREAM`, or when a frame does not follow the newest record (then a new run starts).
- A RAM index (4096 slots, every 4th record, 16 kB) maps `seq` to file offset: `OutboxIndex` in `SweaBleCodec.h`. The outbox is capped at 4 MB and 16384 notifications (about 20 minutes at the full profile; `SWEA_BLE_OUTBOX_MAX_BYTES`). When it is full, **new notifications are thrown away** (the old ones stay replayable, `seq` is not advanced) and the gap flag (`Status.flags` bit2, `FINISH` flag bit0) is raised. An SD error disables the outbox until `STOP_STREAM` (frames then only live in the window, the ring keeps the newest records).
- `RESUME_FROM(seq)` replays from the RAM window or, for older frames, from the outbox, in order at the 25 ms notification pacing, and then continues live. Frames produced during a replay are part of it. `first_replayed_seq` in the reply is the oldest sequence held (window or outbox); a sequence that is in neither (a live frame that has left the window without being in the outbox) is skipped and raises the gap flag.
- `STOP_STREAM` ends the session and deletes the outbox. The outbox is also removed when the next session starts, and a file left over from a power cycle is truncated when the first record of a new session is written.

## 4. Control characteristic

Write: `opcode:u8, args...`. The device answers by notifying `0x80 | opcode, status:u8, args...` (status 0 = OK, 1 = bad args, 2 = not allowed (not bonded/not in mode), 3 = busy, 4 = unsupported). Lengths are validated; unknown opcodes return status 4.

| Opcode | Name | Args | Reply args |
|---|---|---|---|
| 0x01 | `GET_INFO` | – | `proto:u8`, `fw` ASCII nul-terminated, `device_id` ASCII nul-terminated (EmotiBit device id, e.g. `MD-V5-0000302`; the phone registers the device on the server with it). Up to 62 argument bytes, so read it after the MTU exchange. Firmware before 0.5 sent no id |
| 0x02 | `START_STREAM` | `profile:u8` (0..2) | – (opens a session, see section 3; data notifications are sent while `Data` is subscribed) |
| 0x03 | `STOP_STREAM` | – | – (ends the session and clears the SD outbox) |
| 0x04 | `TIME_SYNC` | `id:u8` | `id:u8, device_ms:u32` taken when the write is handled |
| 0x05 | `RESUME_FROM` | `seq:u16` | `first_replayed_seq:u16, flags:u8` |
| 0x06 | `ACK` | `seq:u16` | – (no reply needed; reply suppressed) |
| 0x07 | `SET_PROFILE` | `profile:u8` (0..2) | – (implemented in firmware 0.2) |
| 0x08 | `MARK_EVENT` | `code:u8` | `device_ms:u32`. The marker is written to the SD card as a `UN` packet and comes back in the data stream as a `UN` record with the code as its value (implemented in firmware 0.4) |
| 0x09 | `RECORD` | off: `0`. on: `1, utc_seconds:u32, milliseconds:u16` | – . Starts/stops SD recording exactly like the Oscilloscope's `RB`/`RE` packets; the files are named `YYYY-MM-DD_HH-MM-SS-mmm.csv` (UTC) from the time given. A time before 2020 is refused (status 1). Implemented in firmware 0.4; `Status.flags` bit0 reports the commanded state |
| 0x0A | `ENTER_PAIRING` | `seconds:u8` | – (only accepted while bonded; button also opens pairing; since 0.12 pairing works without it, see section 6) |
| 0x0B | `CLEAR_BONDS` | – | – |
| 0x0C | `SLEEP` | – | – . Sends EmotiBit's own hibernate packet (`MH`); the board flushes the SD card and powers down. Implemented in firmware 0.4 |
| 0x0D | `FINISH` | – | `last_seq:u16, flags:u8` (firmware 0.11). End of the measurement: the device stops taking in new values, turns everything still in the ring into notifications (kept in the window and the outbox, not sent), and answers with the last sequence number of the session. The phone then fetches what it is missing with `RESUME_FROM`, up to and including `last_seq`, and finally sends `STOP_STREAM`, which deletes the outbox. `flags` bit0 = notifications were lost during the session (outbox full or SD error, same as `Status.flags` bit2), bit1 = the session produced no notification (`last_seq` is meaningless). Status 2 when no session is active, 1 for arguments. Repeating `FINISH` just repeats the reply |

Profiles (initial): `0` = all channels, `1` = EDA + temperature + motion only (no PPG/MAG, lowest power), `2` = all channels with 100 Hz PPG (only on the 100 Hz firmware build).

### Time synchronisation
Phone performs 8 `TIME_SYNC` exchanges after connecting, picks the pair with the smallest round trip, and sets `offset = phone_epoch_mid − device_ms`. Repeats every 60 s to track drift. Phone stores raw `t_ms` **and** the derived UTC estimate. Device clock wraps at 2^32 ms (≈ 49.7 days) and restarts at reboot (detected via `Status.boot_count`).

## 5. Status characteristic (read / notify, 20 bytes)

| Offset | Field | Type |
|---|---|---|
| 0 | `proto` | u8 |
| 1 | `state` | u8 (0 idle, 1 advertising, 2 connected, 3 session active, 4 error) |
| 2 | `flags` | u8 bit0 recording, bit1 SD ok, bit2 gap, bit3 low battery, bit4 pairing open |
| 3 | `battery_pct` | u8 |
| 4 | `profile` | u8 |
| 5 | `boot_count` | u8, kept in NVS (firmware 0.12+): +1 at every start, 1..255 then wraps to 1, never 0 |
| 6 | `seq` | u16 (next notification sequence) |
| 8 | `oldest_seq` | u16 (oldest sequence still held: SD outbox if it has records, else the RAM window) |
| 10 | `dropped` | u16 (records dropped because the ring or BLE queue was full) |
| 12 | `device_ms` | u32 |
| 16 | `free_heap_kb` | u16 |
| 18 | `outbox_count` | u16 (notifications held in the SD outbox; 0 before firmware 0.11) |

Notified on state change and at 1 Hz while connected.

## 6. Security
- Pairing: LE Secure Connections, bonding, **passkey entry**. The device has no display, so the 6-digit passkey is fixed per device, generated randomly on first boot and kept in NVS (a factory-provisioned secret is the production alternative), and provided to the user as a QR code JSON `{"t":"emotibit","sn":"<deviceId>","pk":"<passkey>"}` (made by `tools/emotibit.py label`) on the device label or in the SWEA backend. The app scans it with the existing QR scanner.
- **Pairing without the button (firmware 0.12).** The EmotiBit button is often out of reach (case, strap), so pairing does not need the pairing window: a phone that is not bonded may pair at any time, and the correct passkey is the only secret (it is never sent over the air or logged). Pairing mode (short button press, automatically at boot while no phone is bonded, or `ENTER_PAIRING` from a bonded phone, 120 s) is still available; it sets `Status.flags` bit4 and gives the phone more time to finish the pairing, but it no longer gates anything. A connection that does not encrypt is dropped after 15 s (90 s while a pairing is under way or the window is open). `ENTER_PAIRING` remains "only while bonded" and `CLEAR_BONDS` erases all bonds.
- **Rate limit.** Because the passkey has only 10^6 values, the board counts consecutive failed pairings (a pairing that used the passkey and did not complete). After 5 failures it refuses all new pairings for 60 s (the phone's pairing then fails like a wrong passkey); every further lockout doubles the time (60 s, 2, 4, 8 min, capped at 15 min). A successful pairing, a reboot, or a button-opened / `ENTER_PAIRING` window resets the counters. Failures while locked are not counted. An already bonded phone reconnects with its bond and is never affected. The label / QR code must still be treated as a secret; a reboot or button press is the owner's way to lift a lockout (which also means someone with physical access can reset the limit).
- **Bonds.** Max 3 bonds. When the table is full the oldest bond is deleted to make room for a new pairing (before the pairing starts, so the new bond is never the one removed); a phone should therefore be re-paired if it stops being recognised. A stale bond (the phone forgot the board) does not block a fresh pairing: if the phone presents the same identity address, NimBLE deletes the old bond when it sees the repeat pairing; a phone with a new private address is simply an unknown phone, and its old bond ages out through the oldest-first rule or `CLEAR_BONDS`. NimBLE keeps resolving-list peer records of deleted bonds until `CLEAR_BONDS` / reboot; this does not affect pairing.
- All characteristics use encrypted-read/encrypted-write permissions; unencrypted access fails with ATT "insufficient authentication".
- Advertising contains only the service UUID and short name, never personal data. A resolvable private address is available with `-DSWEA_BLE_PRIVACY` (off by default until verified on hardware).
- The firmware validates every length and index in Control writes.

## 7. Versioning
`ver` in every data notification and `proto` in Status identify the protocol; version 1 is this document. New tags and opcodes may be added in version 1 (unknown ones are skipped / return status 4); layout changes bump the version.

## 8. Test vectors (hex)
Notification with one EDA record, seq 7, `t_ms`=531459, two samples 0.030269 and 0.030269:
`01 07 00  45 41 02 03 1c 08 00  b2 f6 f7 3c  b2 f6 f7 3c`
(header `ver=1 seq=7`; record `tag="EA" n=2 t_ms=0x00081C03`; two float32 0.030269.) 18 bytes total, generated with Python `struct.pack('<BH',1,7)+b'EA'+struct.pack('<BI',2,531459)+struct.pack('<ff',.030269,.030269)`. The same vector is used by the firmware host test and the app's decoder test.

Reply to `FINISH` (opcode 0x0D) after a session whose last notification had seq 0x0107, without losses:
`8d 00  07 01  00`
