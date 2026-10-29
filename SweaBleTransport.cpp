#include "SweaBleTransport.h"

#ifdef SWEA_BLE

#include <NimBLEDevice.h>
#include <Preferences.h>
#include <time.h>
#include <nvs.h>
#include <FS.h>

extern "C" int ble_rpa_get_num_peer_dev_records(void);
extern "C" void *ble_rpa_get_peer_dev_records(void);

extern "C" void ble_rpa_set_num_peer_dev_records(int num_rec);

// NimBLE's host-based privacy resolves a phone's private address with its "peer device records", which it persists in NVS next to
// the bonds. That persistence is unreliable (a record that gets its IRK after it was added is never written again, junk records
// of unknown addresses take the slots), so after a restart the records of a bonded phone can lack the IRK and its new private
// address is not recognised ("not bonded", encryption fails, the phone has to pair again). The bonds themselves are stored
// correctly, so the records are rebuilt from them at every start: one complete record per bond that has an IRK.
// Layout of struct ble_hs_dev_records (44 bytes): used, rand_addr_type, pseudo[6], rand[6], identity[6], then peer_sec: addr type,
// addr[6], irk[16], flags (bit0 irk_present). The record's IRK is the byte-reversed IRK of the bond.
#define SWEA_DEV_REC_BYTES 44
static void swea_rebuild_peer_records()
{
	uint8_t *recs = (uint8_t *)ble_rpa_get_peer_dev_records();
	if (!recs) return;
	memset(recs, 0, SWEA_DEV_REC_BYTES * (CONFIG_BT_NIMBLE_MAX_BONDS + 1));
	int out = 0, n = NimBLEDevice::getNumBonds();
	for (int i = 0; i < n && out < CONFIG_BT_NIMBLE_MAX_BONDS; i++)
	{
		NimBLEAddress a = NimBLEDevice::getBondedAddress(i);
		struct ble_store_key_sec key;
		struct ble_store_value_sec val;
		memset(&key, 0, sizeof(key)); memset(&val, 0, sizeof(val));
		key.peer_addr.type = a.getType();
		memcpy(key.peer_addr.val, a.getNative(), 6);
		if (ble_store_read_peer_sec(&key, &val) != 0 || !val.irk_present) continue;
		uint8_t *r = recs + out * SWEA_DEV_REC_BYTES;
		r[0] = 1;									// rec_used
		r[1] = BLE_ADDR_RANDOM;						// rand_addr_type (set again when a private address resolves)
		memcpy(r + 2, a.getNative(), 6);			// pseudo
		memcpy(r + 8, a.getNative(), 6);			// rand
		memcpy(r + 14, a.getNative(), 6);			// identity
		r[20] = a.getType();
		memcpy(r + 21, a.getNative(), 6);
		for (int k = 0; k < 16; k++) r[27 + k] = val.irk[15 - k];
		r[43] = 1;									// irk_present
		out++;
	}
	ble_rpa_set_num_peer_dev_records(out);
	Serial.print("SWEA BLE: rebuilt "); Serial.print(out); Serial.println(" peer record(s) from the bonds");
}

// Optional fixed passkey for development builds (-DSWEA_BLE_PASSKEY=123456). Without it every device
// generates its own random 6-digit passkey on first boot and keeps it in NVS (see loadPasskey()).
#ifndef SWEA_BLE_MBUF_RESERVE
#define SWEA_BLE_MBUF_RESERVE 8
#endif
#define SWEA_BLE_FW_STRING "swea-ble-0.12"
// Longest Control notification (opcode, status and arguments). Needs an MTU above 23, which the phone always asks for.
#define CONTROL_REPLY_MAX 64
// RAM for the record ring (newest data is kept when it is full; the SD card keeps everything).
#ifndef SWEA_BLE_RING_BYTES
#define SWEA_BLE_RING_BYTES 24576
#endif
// Sent notifications kept for RESUME_FROM (about 250 bytes each).
#ifndef SWEA_BLE_WINDOW_FRAMES
#define SWEA_BLE_WINDOW_FRAMES 128
#endif
// SD outbox of a session (notifications nobody received live): /swea/outbox.bin, capped at 4 MB and at 16384 notifications
// (4096 index slots, every 4th notification indexed; about 20 minutes at the full profile). Once it is full, new
// notifications are thrown away and the gap flag is raised.
#ifndef SWEA_BLE_OUTBOX_PATH
#define SWEA_BLE_OUTBOX_DIR "/swea"
#define SWEA_BLE_OUTBOX_PATH "/swea/outbox.bin"
#endif
#ifndef SWEA_BLE_OUTBOX_MAX_BYTES
#define SWEA_BLE_OUTBOX_MAX_BYTES (4u * 1024u * 1024u)
#endif
#define SWEA_BLE_OUTBOX_STRIDE 4
// The outbox file is flushed this often (ms) while no phone is listening.
#define SWEA_BLE_OUTBOX_SYNC_MS 2000
// The number of bonded devices is NimBLE's own limit (CONFIG_BT_NIMBLE_MAX_BONDS in platformio.ini). Firmware 0.12: when the
// table is full, onPassKeyRequest() deletes the oldest bond before a new pairing is accepted (the new bond is only written
// when the pairing completes, so this cannot delete the new phone's own bond). NimBLE's round-robin overflow handler stays
// as a second safety net.
#ifndef CONFIG_BT_NIMBLE_MAX_BONDS
#define CONFIG_BT_NIMBLE_MAX_BONDS 3
#endif

namespace
{
	const char *SERVICE_UUID = "3f5737c4-b391-44c7-bd74-f2ee561c9775";
	const char *DATA_UUID = "f14e01ab-48d0-461a-b54e-49fd7be9901a";
	const char *CONTROL_UUID = "1118282f-3dfe-44a7-a12f-6d06987f1092";
	const char *STATUS_UUID = "8c2502e8-eacd-45d1-bd20-361eb5a762a2";

	enum Opcode : uint8_t
	{
		OP_GET_INFO = 0x01,
		OP_START_STREAM = 0x02,
		OP_STOP_STREAM = 0x03,
		OP_TIME_SYNC = 0x04,
		OP_RESUME_FROM = 0x05,
		OP_ACK = 0x06,
		OP_SET_PROFILE = 0x07,
		OP_MARK_EVENT = 0x08,
		OP_RECORD = 0x09,
		OP_ENTER_PAIRING = 0x0A,
		OP_CLEAR_BONDS = 0x0B,
		OP_SLEEP = 0x0C,
		OP_FINISH = swea::OPCODE_FINISH	// 0x0D
	};
	enum ReplyStatus : uint8_t
	{
		ST_OK = 0,
		ST_BAD_ARGS = 1,
		ST_NOT_ALLOWED = 2,
		ST_BUSY = 3,
		ST_UNSUPPORTED = 4
	};

	NimBLEServer *g_server = nullptr;
	NimBLECharacteristic *g_data = nullptr;
	NimBLECharacteristic *g_control = nullptr;
	NimBLECharacteristic *g_status = nullptr;
	SweaBleTransport *g_owner = nullptr;
	uint8_t g_bootCount = 0;
	uint32_t g_passkey = 123456;
	// Firmware 0.12: an unknown phone may pair at any time; the passkey is the only secret and PairingGuard rate-limits wrong
	// guesses. The guard is touched by the NimBLE host task (callbacks) and by openPairing() (main task), hence the mutex.
	swea::PairingGuard g_guard;
	portMUX_TYPE g_guardMux = portMUX_INITIALIZER_UNLOCKED;
	volatile bool g_pairAttempt = false;	// the passkey was handed to the stack for this connection: the outcome counts
	volatile bool g_peerBonded = false;		// the current peer's identity is already in the bond table

	using swea::get16;
	using swea::put16;
	using swea::put32;

	// Connection parameters (protocol doc, section 1): interval 30-50 ms, no latency, supervision timeout 4 s.
	// A phone follows these (Windows did not): at 100-200 ms with latency 4 an Android phone carried only about
	// 2.5 notifications/s, less than the full profile produces, and the backlog starved the Control channel.
	const uint16_t CONN_MIN_INTERVAL = 24;	// units of 1.25 ms
	const uint16_t CONN_MAX_INTERVAL = 40;
	const uint16_t CONN_LATENCY = 0;
	const uint16_t CONN_TIMEOUT = 400;		// units of 10 ms
	// At most one data notification per this many ms (about 10 kB/s at full MTU, five times the full profile), so NimBLE's
	// buffers never fill up and Control replies and writes still get through.
	const uint32_t NOTIFY_SPACING_MS = 25;
	// A connection that is not encrypted by then (no bond and no pairing) is dropped. While the pairing window is open or
	// a pairing is under way the user may still be tapping the phone's pairing notification and typing the passkey, so it
	// gets much longer.
	const uint32_t ENCRYPT_TIMEOUT_MS = 15000;
	const uint32_t PAIRING_ENCRYPT_TIMEOUT_MS = 90000;

	class ServerCallbacks : public NimBLEServerCallbacks
	{
		void onConnect(NimBLEServer *server, ble_gap_conn_desc *desc) override
		{
			// The address is not checked here: a bonded Android phone connects with a resolvable private address that is
			// only known to be "ours" once the link is encrypted with its bond. The gate is encryption: a bonded phone
			// encrypts with its stored keys, a new pairing needs the correct passkey (onPassKeyRequest, rate limited by
			// PairingGuard), and a link that is not encrypted within ENCRYPT_TIMEOUT_MS is dropped (update()). No data or command is
			// served before encryption.
			if (!g_owner) return;
			g_owner->onConnected(desc->conn_handle);
			server->updateConnParams(desc->conn_handle, CONN_MIN_INTERVAL, CONN_MAX_INTERVAL, CONN_LATENCY, CONN_TIMEOUT);
			// A bonded phone (its private address resolved to its identity) is asked to encrypt right away. An unknown one
			// is given a few seconds first: if the phone's app starts the pairing itself, Android asks for the passkey
			// directly and the app can supply it; our Security Request would instead make Android ask the user to confirm.
			NimBLEAddress id(desc->peer_id_addr);
			bool bonded = NimBLEDevice::isBonded(id);
			g_peerBonded = bonded;
			g_pairAttempt = false;
			Serial.print("SWEA BLE: connected "); Serial.print(NimBLEAddress(desc->peer_ota_addr).toString().c_str());
			Serial.print(" id "); Serial.print(id.toString().c_str());
			Serial.print(bonded ? " (bonded)" : " (not bonded)");
			Serial.print(", bonds "); Serial.print(NimBLEDevice::getNumBonds());
			Serial.print(", peer records "); Serial.println(ble_rpa_get_num_peer_dev_records());
			if (bonded) NimBLEDevice::startSecurity(desc->conn_handle);
			else g_owner->requestSecurityLater();
		}
		void onDisconnect(NimBLEServer *server) override
		{
			g_pairAttempt = false;	// a pairing abandoned before it finished tells nothing about the passkey: not counted
			if (g_owner) g_owner->onDisconnected();
			NimBLEDevice::startAdvertising();
		}
		void onMTUChange(uint16_t mtu, ble_gap_conn_desc *desc) override
		{
			if (g_owner) g_owner->onMtu(mtu);
		}
		uint32_t onPassKeyRequest() override
		{
			// Only called for a new pairing (a bonded phone re-encrypts without it). The board shows the passkey (label / QR
			// code), the phone types it: the phone only learns whether it was right when the pairing completes. Never logged.
			uint32_t now = millis();
			portENTER_CRITICAL(&g_guardMux);
			bool locked = g_guard.locked(now);
			uint32_t left = g_guard.remainingMs(now);
			portEXIT_CRITICAL(&g_guardMux);
			if (locked)
			{
				Serial.print("SWEA BLE: refused a new pairing (locked after failed attempts, ");
				Serial.print((left + 999) / 1000); Serial.println(" s left)");
				g_pairAttempt = false;
				return (g_passkey + 1) % 1000000;	// cannot match the phone's passkey, so the pairing fails
			}
			// Make room: with a full bond table the oldest bond goes (the new bond is only stored when this pairing
			// completes). Skipped when this phone's identity is already bonded: NimBLE replaces that stale bond itself
			// (BLE_GAP_EVENT_REPEAT_PAIRING deletes it) and an unrelated bond must not be sacrificed for it.
			if (!g_peerBonded)
			{
				int guard = CONFIG_BT_NIMBLE_MAX_BONDS;
				while (guard-- > 0 && NimBLEDevice::getNumBonds() >= CONFIG_BT_NIMBLE_MAX_BONDS)
				{
					NimBLEAddress oldest = NimBLEDevice::getBondedAddress(0);
					Serial.print("SWEA BLE: bond table full, forgetting the oldest bond "); Serial.println(oldest.toString().c_str());
					if (!NimBLEDevice::deleteBond(oldest)) break;
				}
			}
			g_pairAttempt = true;
			return g_passkey;
		}
		void onAuthenticationComplete(ble_gap_conn_desc *desc) override
		{
			if (g_owner) g_owner->onEncryptionChange(desc->sec_state.encrypted);
			Serial.print("SWEA BLE: encryption ");
			Serial.print(desc->sec_state.encrypted ? "on" : "FAILED");
			Serial.print(desc->sec_state.bonded ? ", bonded" : ", not bonded");
			Serial.print(", id "); Serial.println(NimBLEAddress(desc->peer_id_addr).toString().c_str());
			if (g_pairAttempt)
			{
				// The outcome of a pairing that used the passkey: right passkey = success, anything else = a failed guess.
				g_pairAttempt = false;
				portENTER_CRITICAL(&g_guardMux);
				if (desc->sec_state.encrypted) g_guard.recordSuccess();
				else g_guard.recordFailure(millis());
				portEXIT_CRITICAL(&g_guardMux);
			}
			if (!desc->sec_state.encrypted)
				NimBLEDevice::getServer()->disconnect(desc->conn_handle);	// failed pairing: drop the link
		}
	};

	class ControlCallbacks : public NimBLECharacteristicCallbacks
	{
		void onWrite(NimBLECharacteristic *c) override
		{
			std::string v = c->getValue();
			if (g_owner) g_owner->onControlWrite((const uint8_t *)v.data(), v.size());
		}
	};

	class DataCallbacks : public NimBLECharacteristicCallbacks
	{
		void onSubscribe(NimBLECharacteristic *c, ble_gap_conn_desc *desc, uint16_t subValue) override
		{
			if (g_owner) g_owner->onSubscribe(subValue & 1);	// bit 0 = notifications enabled
		}
	};

	ServerCallbacks g_serverCb;
	ControlCallbacks g_controlCb;
	DataCallbacks g_dataCb;
}

// ---------------------------------------------------------------------------------------
uint8_t SweaBleTransport::begin(int32_t, uint8_t, uint16_t)
{
	if (!_on) startBle();
	return 0;	// SUCCESS
}

void SweaBleTransport::end()
{
	if (_on) stopBle();
}

int8_t SweaBleTransport::disconnect()
{
	if (g_server && _connected) g_server->disconnect(_connHandle);
	return 0;
}

int8_t SweaBleTransport::update(String &, uint16_t &)
{
	if (!_on) return 0;
	uint32_t now = millis();
	if (_advPending)
	{
		_advPending = false;
		NimBLEDevice::startAdvertising();
		Serial.println("SWEA BLE: advertising started (main loop is running)");
	}

	if (_pairingOpen && (int32_t)(now - _pairingUntil) >= 0)
	{
		_pairingOpen = false;
		Serial.println("SWEA BLE: pairing window closed");
	}
	if (_securityAtMs && _connected && !_encrypted && (int32_t)(now - _securityAtMs) >= 0)
	{
		_securityAtMs = 0;
		NimBLEDevice::startSecurity(_connHandle);	// the phone did not start a pairing itself: ask for one
	}
	if (_connected && !_encrypted && now - _connectedAtMs > ((_pairingOpen || g_pairAttempt) ? PAIRING_ENCRYPT_TIMEOUT_MS : ENCRYPT_TIMEOUT_MS))
	{
		Serial.println("SWEA BLE: dropped a connection that did not encrypt (not bonded, no pairing finished)");
		_connectedAtMs = now;	// once
		disconnect();
	}
	processCommands();
	drain();
	if (_connected && now - _lastStatusMs >= 1000)
	{
		_lastStatusMs = now;
		sendStatus(_encrypted);
	}
	return 0;
}

// EmotiBit's own control parser (parseIncomingControlPackets) reads text packets here. Commands that map onto EmotiBit
// features (recording, event markers, sleep) are turned into the same packets the Oscilloscope would send.
uint8_t SweaBleTransport::readControl(String &packet)
{
	if (_ctrlTail == _ctrlHead) return 0;
	packet = _ctrlOut[_ctrlTail];
	_ctrlOut[_ctrlTail] = "";
	_ctrlTail = (_ctrlTail + 1) % CTRL_OUT;
	return packet.length() > 255 ? 255 : (uint8_t)packet.length();
}

bool SweaBleTransport::pushControlPacket(const String &packet)
{
	uint8_t next = (_ctrlHead + 1) % CTRL_OUT;
	if (next == _ctrlTail) return false;	// queue full
	_ctrlOut[_ctrlHead] = packet;
	_ctrlHead = next;
	return true;
}

// Receives exactly the text packets that EmotiBit also writes to the SD card (one or more lines).
int8_t SweaBleTransport::sendData(const String &message)
{
	if (!_on || !_ringMem) return 0;
	const char *s = message.c_str();
	size_t len = message.length(), i = 0;
	while (i < len)
	{
		size_t j = i;
		while (j < len && s[j] != '\n') j++;
		size_t l = j - i;
		if (l > 0 && s[i + l - 1] == '\r') l--;
		if (l > 0) handleLine(s + i, l);
		i = j + 1;
	}
	return 0;
}

void SweaBleTransport::handleLine(const char *s, size_t len)
{
	if (_finished) return;	// FINISH: no new values are taken in
	if (!swea::parsePacketLine(s, len, _pp)) return;
	if (_pp.tag[0] == 'B' && _pp.tag[1] == '%')
	{
		float pct = _pp.v[_pp.n - 1];
		_batteryPct = pct < 0 ? 0 : (pct > 100 ? 100 : (uint8_t)(pct + 0.5f));
	}
	if (swea::profileAllows(_profile, _pp.tag)) _ring.push(_pp.tag, _pp.n, _pp.t_ms, _pp.v);
}

// The ring is turned into notifications here. During a session that happens whether or not a phone listens:
//  - phone subscribed: first any requested replay (window, then SD outbox), then live frames, sent and kept in the window;
//  - no phone, or the ring is backed up (more than 3/4 full): frames go to the SD outbox and the window instead, with
//    the same continuous seq, and the phone fetches them later with RESUME_FROM.
// A few frames per call keep the main loop short.
void SweaBleTransport::drain()
{
	if (!_session || !_ringMem) return;
	size_t cap = _mtu > 3 ? (size_t)_mtu - 3 : 20;
	if (!_connected || cap > 244) cap = 244;	// nobody asked yet: the phone always negotiates an MTU of 247
	bool live = _connected && _encrypted && _dataSubscribed && _streamGo;
	uint8_t buf[244];

	for (int frame = 0; frame < 6 && (!live || _ring.used() * 4 > _ring.capacity() * 3); frame++)
		if (!produceFrame(cap)) break;
	if (!live)
	{
		if (_outOpen && millis() - _outSyncMs >= SWEA_BLE_OUTBOX_SYNC_MS) { _outFile.flush(); _outSyncMs = millis(); }
		return;
	}

	if (_replay)
	{
		_replayEnd = _seq;									// frames produced during the replay are part of it
		for (int frame = 0; frame < 6 && _replay; frame++)
		{
			if (millis() - _lastNotifyMs < NOTIFY_SPACING_MS) break;
			if (!swea::seqBefore(_replaySeq, _replayEnd)) { _replay = false; break; }
			size_t l = 0;
			if (_win.get(_replaySeq, buf, l) || outboxRead(_replaySeq, buf, l))
			{
				if (l > (size_t)_mtu - 3) _gap = true;			// cannot be delivered with this MTU
				else
				{
					if (!notifyData(buf, l)) { _notifyFailures++; return; }	// retry the same frame next time
					_lastNotifyMs = millis();
				}
			}
			else _gap = true;									// held neither in the window nor in the outbox
			_replaySeq++;
		}
		return;													// live data resumes after the replay
	}

	for (int frame = 0; frame < 6; frame++)
	{
		if (millis() - _lastNotifyMs < NOTIFY_SPACING_MS) break;
		swea::RecordRing::Cursor c = _ring.cursor();
		size_t n = swea::buildNotification(_ring, _seq, buf, cap, _profile);
		if (n == 0) break;
		if (!notifyData(buf, n))
		{
			_ring.restore(c);		// not sent: keep the records and try again later
			_notifyFailures++;
			break;
		}
		_win.store(_seq, buf, n);
		if (_outIdx.count()) outboxAppend(_seq, buf, n);	// an undelivered outbox run goes on without a hole (the phone may not have asked for it yet)
		_seq++;
		_sessionFrames = true;
		_lastNotifyMs = millis();
	}
}

// ---- SD outbox -------------------------------------------------------------------------
bool SweaBleTransport::outboxUsable()
{
	return _sd && _outIdxMem && !_outFail;
}

void SweaBleTransport::outboxSeedFromWindow(uint16_t beforeSeq)
{
	_outSeeding = true;
	uint8_t buf[244];
	for (uint16_t s = _win.oldestSeq(); swea::seqBefore(s, beforeSeq); s++)
	{
		size_t l = 0;
		if (!_win.get(s, buf, l) || l == 0 || l > sizeof(buf) || !outboxAppend(s, buf, l)) break;
	}
	_outSeeding = false;
	if (_outIdx.count() && beforeSeq != _outIdx.nextSeq()) { _outIdx.clear(); _replayPosValid = false; }	// not contiguous after all: start at beforeSeq
}

bool SweaBleTransport::outboxAppend(uint16_t seq, const uint8_t *data, size_t len)
{
	if (!outboxUsable()) return false;
	// The outbox holds one contiguous run of seq numbers. Frames sent live in between are not in it: when this frame does not
	// follow the newest record, the old run was delivered (or is covered by the window) and a new run starts here.
	if (_outIdx.count() && seq != _outIdx.nextSeq()) { _outIdx.clear(); _replayPosValid = false; }
	// A new run starts here: first take over the frames that were sent live but never acknowledged (they may have gone into a
	// link that was already dead, 3-4 s before the drop was noticed), so the phone can fetch them with RESUME_FROM.
	if (_outIdx.count() == 0 && !_outSeeding && _win.count()) outboxSeedFromWindow(seq);
	if (!_outIdx.canAppend(len)) { _outIdx.append(seq, len); return false; }	// refused and counted
	if (!_outOpen)
	{
		_sd->mkdir(SWEA_BLE_OUTBOX_DIR);
		_outFile = _sd->open(SWEA_BLE_OUTBOX_PATH, "w+");		// truncates anything left from an earlier session
		if (!_outFile) { _outFail = true; Serial.println("SWEA BLE: cannot open the outbox on the SD card"); return false; }
		_outOpen = true;
		_outSyncMs = millis();
	}
	uint8_t rec[swea::OUTBOX_RECORD_OVERHEAD + 244];
	size_t n = swea::outboxEncode(rec, seq, data, len);
	// FAT seeks walk the cluster chain (slow on a long file): seek only when the file is not already at the write position.
	if ((_outFile.position() != _outIdx.bytes() && !_outFile.seek(_outIdx.bytes())) || _outFile.write(rec, n) != n)
	{
		_outFail = true;										// SD trouble: stop using the outbox, sampling and logging go on
		_outFile.close(); _outOpen = false;
		Serial.println("SWEA BLE: outbox write failed, outbox disabled until STOP");
		return false;
	}
	_outIdx.append(seq, len);
	return true;
}

// Reads the notification with this sequence number. Sequential reads continue from the previous position; otherwise
// the index gives the nearest anchor and the records in between are skipped by their length bytes.
bool SweaBleTransport::outboxRead(uint16_t seq, uint8_t *out, size_t &len)
{
	if (!_outOpen || !_outIdx.contains(seq)) return false;
	uint32_t pos;
	uint8_t h[swea::OUTBOX_RECORD_OVERHEAD];
	if (_replayPosValid && _replayPosSeq == seq) pos = _replayPos;
	else
	{
		uint16_t skip;
		if (!_outIdx.find(seq, pos, skip)) return false;
		for (; skip; skip--)
		{
			if (!_outFile.seek(pos) || _outFile.read(h, sizeof(h)) != sizeof(h)) { _replayPosValid = false; return false; }
			pos += swea::OUTBOX_RECORD_OVERHEAD + h[2];
		}
	}
	_replayPosValid = false;
	if ((_outFile.position() != pos && !_outFile.seek(pos)) || _outFile.read(h, sizeof(h)) != sizeof(h)) return false;
	size_t l = h[2];
	if (get16(h) != seq || l == 0 || l > 244 || _outFile.read(out, l) != l) return false;
	len = l;
	_replayPos = pos + swea::OUTBOX_RECORD_OVERHEAD + (uint32_t)l;
	_replayPosSeq = (uint16_t)(seq + 1);
	_replayPosValid = true;
	return true;
}

void SweaBleTransport::outboxClear()
{
	if (_outOpen) { _outFile.close(); _outOpen = false; }
	if (_sd && _outIdxMem && _sd->exists(SWEA_BLE_OUTBOX_PATH)) _sd->remove(SWEA_BLE_OUTBOX_PATH);
	_outIdx.clear();
	_outFail = false;
	_replayPosValid = false;
}

// Turns one notification's worth of records into a frame for the outbox and the window (no phone, or a backlog).
// False when there is nothing to do: the ring is empty or the outbox is unusable (the records then stay in the ring, which
// keeps the newest data). When the outbox is full the frame is thrown away: seq is not used and the gap flag is raised.
bool SweaBleTransport::produceFrame(size_t cap)
{
	if (!outboxUsable()) return false;
	uint8_t buf[244];
	size_t n = swea::buildNotification(_ring, _seq, buf, cap, _profile);
	if (n == 0) return false;
	if (!outboxAppend(_seq, buf, n))
	{
		_lost = true;
		return !_outFail;			// full: the ring was consumed, go on discarding
	}
	_win.store(_seq, buf, n);
	_seq++;
	_sessionFrames = true;
	return true;
}

void SweaBleTransport::beginSession()
{
	if (_session) return;
	if (!_outIdxMem)
	{
		_outIdxMem = (uint32_t *)malloc(swea::OutboxIndex::bytesFor(swea::OutboxIndex::DEFAULT_SLOTS));
		if (_outIdxMem) _outIdx.init(_outIdxMem, swea::OutboxIndex::DEFAULT_SLOTS, SWEA_BLE_OUTBOX_STRIDE, SWEA_BLE_OUTBOX_MAX_BYTES);
		else Serial.println("WARNING: no memory for the outbox index; a session without a phone loses data");
	}
	outboxClear();
	_finished = false;
	_sessionFrames = false;
	_gap = false;
	_lost = false;
	_session = true;
}

void SweaBleTransport::endSession()
{
	_session = false;
	_replay = false;
	_finished = false;
	outboxClear();
}

// FINISH: stop taking in values and turn everything still in the ring into notifications. They are kept in the window
// and the outbox (not sent here), so the phone fetches what it is missing with RESUME_FROM up to the returned last_seq.
void SweaBleTransport::flushRing()
{
	_finished = true;
	if (!_ringMem) return;
	size_t cap = _mtu > 3 ? (size_t)_mtu - 3 : 20;
	if (!_connected || cap > 244) cap = 244;
	uint8_t buf[244];
	while (!_ring.empty())
	{
		if (outboxUsable()) { if (!produceFrame(cap)) break; continue; }
		// No outbox: the frames can only be kept in the window, as far as it reaches.
		size_t n = swea::buildNotification(_ring, _seq, buf, cap, _profile);
		if (n == 0) break;
		_win.store(_seq, buf, n);
		_seq++;
		_sessionFrames = true;
		_lost = true;
	}
}

int8_t SweaBleTransport::addCredential(const String &, const String &, const String &, const String &) { return 0; }
void SweaBleTransport::setDeviceId(const String emotibitDeviceId) { _deviceId = emotibitDeviceId; }
bool SweaBleTransport::isConnected() { return _connected && _encrypted; }
bool SweaBleTransport::isOff() { return !_on; }
void SweaBleTransport::updateStatus() {}
uint8_t SweaBleTransport::status(bool) { return _on ? WL_CONNECTED : WL_DISCONNECTED; }
uint8_t SweaBleTransport::getNumCredentials() { return 0; }
bool SweaBleTransport::isEnterpriseNetworkListed() { return false; }
uint8_t SweaBleTransport::getMaxNumCredentialAllowed() { return MAX_CREDENTIALS; }

void SweaBleTransport::openPairing(uint16_t seconds)
{
	if (!_on) return;
	_pairingUntil = millis() + (uint32_t)seconds * 1000UL;
	_pairingOpen = true;
	// Button, boot and a bonded phone's ENTER_PAIRING are authenticated by physical access or by a bond: lift a lockout.
	portENTER_CRITICAL(&g_guardMux);
	g_guard.reset();
	portEXIT_CRITICAL(&g_guardMux);
	Serial.print("SWEA BLE: pairing open for "); Serial.print(seconds);
	Serial.print(" s. Passkey: "); Serial.println(_passkey);	// shown on Serial only (USB); also goes on the device label / QR code
}

// ---------------------------------------------------------------------------------------
// Per-device passkey: random 6 digits, generated on first boot and kept in NVS. A development build can force one.
void SweaBleTransport::loadPasskey()
{
#ifdef SWEA_BLE_PASSKEY
	_passkey = SWEA_BLE_PASSKEY;
	Serial.println("WARNING: fixed development passkey in use");
#else
	uint32_t pk = 0;
	Preferences prefs;
	if (prefs.begin("swea_ble", false))
	{
		pk = prefs.getUInt("passkey", 0);
		if (pk < 100000 || pk > 999999)
		{
			pk = 100000 + (esp_random() % 900000);
			prefs.putUInt("passkey", pk);
			Serial.println("SWEA BLE: generated a new device passkey");
		}
		prefs.end();
	}
	if (pk < 100000 || pk > 999999)
	{
		pk = 123456;
		Serial.println("WARNING: NVS unavailable, falling back to a fixed passkey");
	}
	_passkey = pk;
#endif
	g_passkey = _passkey;
	// USB serial only (physical access): `emotibit.py label` reads it to print the QR label. `emotibit.py monitor` hides it.
	Serial.print("SWEA BLE label: id "); Serial.print(_deviceId.length() ? _deviceId : String("?"));
	Serial.print(" passkey "); Serial.println(_passkey);
}

void SweaBleTransport::startBle()
{
	g_owner = this;
	// The boot counter lives in NVS so that a restart is visible in the data (device_ms and seq start over). 1..255, never 0.
	{
		Preferences prefs;
		uint8_t boot = 0;
		if (prefs.begin("swea_ble", false)) { boot = prefs.getUChar("boot", 0); boot = (uint8_t)(boot == 255 ? 1 : boot + 1); prefs.putUChar("boot", boot); prefs.end(); }
		g_bootCount = boot ? boot : (uint8_t)(g_bootCount + 1);	// NVS unavailable: at least count restarts of the BLE stack
	}
	g_guard.reset();	// a reboot lifts a lockout
	g_pairAttempt = false; g_peerBonded = false;
	if (!_ringMem)
	{
		size_t bytes = SWEA_BLE_RING_BYTES;
		while (bytes >= 4096 && !(_ringMem = (uint8_t *)malloc(bytes))) bytes /= 2;	// shrink if the heap is tight
		if (_ringMem) _ring.init(_ringMem, bytes);
		else Serial.println("WARNING: no memory for the BLE record ring; data is not forwarded");
	}
	if (!_winMem)
	{
		_winMem = (uint8_t *)malloc(swea::FrameWindow::bytesFor(SWEA_BLE_WINDOW_FRAMES));
		if (_winMem) _win.init(_winMem, SWEA_BLE_WINDOW_FRAMES);
		else Serial.println("WARNING: no memory for the resume window; RESUME_FROM can only report a gap");
	}
	_replay = false; _gap = false;
	_session = false; _finished = false; _sessionFrames = false; _lost = false;
	_cmdHead = _cmdTail = 0;
	_ctrlHead = _ctrlTail = 0; _recording = false;
	loadPasskey();

	String suffix = _deviceId.length() >= 4 ? _deviceId.substring(_deviceId.length() - 4) : String("0000");
	String name = "EmotiBit-" + suffix;

	NimBLEDevice::init(name.c_str());
	swea_rebuild_peer_records();	// before the first connection can arrive
	NimBLEDevice::setMTU(247);
	NimBLEDevice::setSecurityAuth(true, true, true);			// bonding, MITM protection, LE Secure Connections
	NimBLEDevice::setSecurityIOCap(BLE_HS_IO_DISPLAY_ONLY);	// passkey is shown on the label / QR code
	// No setSecurityPasskey(): NimBLE-Arduino only asks onPassKeyRequest() while the static passkey is its default, and that
	// callback is where the per-device passkey is handed out and the PairingGuard lockout is enforced (the pairing window
	// no longer gates anything since firmware 0.12; it only lengthens the encryption timeout and sets Status flag bit4).
#ifdef SWEA_BLE_PRIVACY
	// Resolvable private address so the phone cannot be tracked by a fixed address. Needs hardware verification.
	NimBLEDevice::setOwnAddrType(BLE_OWN_ADDR_RPA_PUBLIC_DEFAULT);
#endif

	g_server = NimBLEDevice::createServer();
	g_server->setCallbacks(&g_serverCb);
	NimBLEService *svc = g_server->createService(SERVICE_UUID);

	g_data = svc->createCharacteristic(DATA_UUID, NIMBLE_PROPERTY::NOTIFY);
	g_data->setCallbacks(&g_dataCb);
	g_control = svc->createCharacteristic(CONTROL_UUID,
		NIMBLE_PROPERTY::WRITE | NIMBLE_PROPERTY::WRITE_ENC | NIMBLE_PROPERTY::WRITE_AUTHEN | NIMBLE_PROPERTY::NOTIFY);
	g_control->setCallbacks(&g_controlCb);
	g_status = svc->createCharacteristic(STATUS_UUID,
		NIMBLE_PROPERTY::READ | NIMBLE_PROPERTY::READ_ENC | NIMBLE_PROPERTY::READ_AUTHEN | NIMBLE_PROPERTY::NOTIFY);
	svc->start();

	NimBLEAdvertising *adv = NimBLEDevice::getAdvertising();
	adv->addServiceUUID(SERVICE_UUID);	// advertising carries only the service UUID and the short name
	adv->setScanResponse(true);
	// Advertising starts from the first update() in the main loop, not here: this runs inside EmotiBit::setup(), which keeps going for
	// several seconds (SD card, sensors). A phone that reconnects the moment the board shows up would otherwise get a link that
	// nobody serves yet (no commands, no encryption timers) and give up on it.
	_advPending = true;

	_on = true;
	_connected = _encrypted = false;
	sendStatus(false);
	Serial.print("SWEA BLE advertising as "); Serial.println(name);
	Serial.print("SWEA BLE: bonds "); Serial.print(NimBLEDevice::getNumBonds());
	Serial.print(", peer records "); Serial.println(ble_rpa_get_num_peer_dev_records());
	if (NimBLEDevice::getNumBonds() == 0) openPairing(120);	// first use: no phone is bonded yet
}

void SweaBleTransport::stopBle()
{
	_on = false;
	_pairingOpen = false;
	_connected = _encrypted = false;
	endSession();
	NimBLEDevice::deinit(true);
	if (_ringMem) { free(_ringMem); _ringMem = nullptr; }
	if (_winMem) { free(_winMem); _winMem = nullptr; _win.init(nullptr, 0); }
	if (_outIdxMem) { free(_outIdxMem); _outIdxMem = nullptr; _outIdx.init(nullptr, 0, 1, 0); }
	g_server = nullptr; g_data = g_control = g_status = nullptr;
}

bool SweaBleTransport::notifyData(const uint8_t *buf, size_t len)
{
	if (!g_data || !_connected || !_encrypted) return false;
	if (len > (size_t)(_mtu - 3)) return false;
	// NimBLE-Arduino 1.4.3 notify() returns nothing, so call the host API directly: it reports
	// back-pressure (out of mbufs) as an error and the caller keeps the records for the next try.
	// Keep a reserve of mbufs for incoming ATT requests: when notifications use them all, the phone's Control writes fail with
	// ATT error 0x11 (insufficient resources). The caller keeps the records and tries again.
	if (os_msys_num_free() < SWEA_BLE_MBUF_RESERVE) return false;
	os_mbuf *om = ble_hs_mbuf_from_flat(buf, len);
	if (!om) return false;
	return ble_gattc_notify_custom(_connHandle, g_data->getHandle(), om) == 0;	// takes ownership of om
}

void SweaBleTransport::sendStatus(bool notify)
{
	if (!g_status) return;
	uint8_t s[20] = {0};
	s[0] = PROTOCOL_VERSION;
	s[1] = !_on ? 0 : (_session ? 3 : (_connected ? 2 : 1));
	// flags: bit1 SD ok (the firmware does not start without a card), bit2 gap, bit3 low battery, bit4 pairing open.
	// bit0 recording: as commanded over BLE.
	s[2] = 0x02 | (_recording ? 0x01 : 0) | ((_gap || _lost) ? 0x04 : 0) | ((_batteryPct != 0xFF && _batteryPct < 20) ? 0x08 : 0) | (_pairingOpen ? 0x10 : 0);
	s[3] = _batteryPct;						// 0xFF = unknown
	s[4] = _profile;
	s[5] = g_bootCount;
	put16(&s[6], _seq);
	put16(&s[8], _outIdx.count() ? _outIdx.firstSeq() : (_win.count() ? _win.oldestSeq() : _seq));
	put16(&s[10], (uint16_t)_ring.dropped());
	put32(&s[12], millis());
	put16(&s[16], (uint16_t)(ESP.getFreeHeap() / 1024));
	put16(&s[18], (uint16_t)_outIdx.count());	// notifications held in the SD outbox
	g_status->setValue(s, sizeof(s));
	if (notify && _connected) g_status->notify();
}

void SweaBleTransport::sendControlReply(uint8_t opcode, uint8_t status, const uint8_t *args, size_t n)
{
	if (!g_control || !_connected) return;
	uint8_t buf[CONTROL_REPLY_MAX];
	if (n > sizeof(buf) - 2) n = sizeof(buf) - 2;
	buf[0] = 0x80 | opcode;
	buf[1] = status;
	if (n) memcpy(&buf[2], args, n);
	g_control->setValue(buf, n + 2);
	g_control->notify();
}

// ---- BLE callbacks (host task): flags only -------------------------------------------
void SweaBleTransport::onConnected(uint16_t connHandle) { _connHandle = connHandle; _connectedAtMs = millis(); _connected = true; _encrypted = false; _dataSubscribed = false; _streamGo = false; _mtu = 23; }
void SweaBleTransport::onDisconnected() { _streamGo = false; _connected = false; _encrypted = false; _dataSubscribed = false; _replay = false; _connHandle = 0xFFFF; }
void SweaBleTransport::onEncryptionChange(bool encrypted) { _encrypted = encrypted; }
void SweaBleTransport::onMtu(uint16_t mtu) { _mtu = mtu; }
void SweaBleTransport::onSubscribe(bool subscribed) { _dataSubscribed = subscribed; }

// A write to Control is queued and executed by update() in the main loop, so the ring, the window and
// the replay state are only ever touched from one task.
void SweaBleTransport::onControlWrite(const uint8_t *data, size_t len)
{
	if (len < 1) return;
	Command c;
	c.len = len > CMD_MAX_LEN ? 0xFF : (uint8_t)len;	// 0xFF marks an over-long (invalid) command
	memcpy(c.d, data, len > CMD_MAX_LEN ? CMD_MAX_LEN : len);
	portENTER_CRITICAL(&_cmdMux);
	uint8_t next = (_cmdHead + 1) % CMD_QUEUE;
	if (next != _cmdTail) { _cmds[_cmdHead] = c; _cmdHead = next; }	// a full queue drops the write
	portEXIT_CRITICAL(&_cmdMux);
}

void SweaBleTransport::processCommands()
{
	for (int i = 0; i < CMD_QUEUE; i++)
	{
		Command c;
		portENTER_CRITICAL(&_cmdMux);
		bool have = _cmdTail != _cmdHead;
		if (have) { c = _cmds[_cmdTail]; _cmdTail = (_cmdTail + 1) % CMD_QUEUE; }
		portEXIT_CRITICAL(&_cmdMux);
		if (!have) return;
		execute(c);
	}
}

void SweaBleTransport::execute(const Command &c)
{
	uint8_t op = c.d[0];
	size_t len = c.len;
	const uint8_t *data = c.d;
	if (!_encrypted)
	{
		sendControlReply(op, ST_NOT_ALLOWED, nullptr, 0);
		return;
	}
	if (c.len == 0xFF)
	{
		sendControlReply(op, ST_BAD_ARGS, nullptr, 0);
		return;
	}
	switch (op)
	{
	case OP_GET_INFO:
	{
		// The phone registers the device on the server by this id, so it is sent in full.
		uint8_t out[CONTROL_REPLY_MAX - 2];
		size_t n = swea::buildInfoReply(out, sizeof(out), PROTOCOL_VERSION, SWEA_BLE_FW_STRING, _deviceId.c_str());
		sendControlReply(op, ST_OK, out, n);
		break;
	}
	case OP_START_STREAM:
		if (len != 2 || data[1] > 2) { sendControlReply(op, ST_BAD_ARGS, nullptr, 0); break; }
		_profile = data[1];
		beginSession();			// no-op while a session is active (a reconnecting phone must not reset it)
		_streamGo = true;		// live data and replay start only now: a reconnecting phone first does its setup on a quiet link
		sendControlReply(op, ST_OK, nullptr, 0);
		break;
	case OP_STOP_STREAM:
		endSession();			// the outbox is cleared
		sendControlReply(op, ST_OK, nullptr, 0);
		break;
	case OP_TIME_SYNC:
	{
		if (len != 2) { sendControlReply(op, ST_BAD_ARGS, nullptr, 0); break; }
		uint8_t out[5];
		out[0] = data[1];
		put32(&out[1], millis());
		sendControlReply(op, ST_OK, out, sizeof(out));
		break;
	}
	case OP_RESUME_FROM:
	{
		if (len != 3) { sendControlReply(op, ST_BAD_ARGS, nullptr, 0); break; }
		uint16_t req = get16(&data[1]);
		_streamGo = true;
		if (swea::seqBefore(_seq, req)) { sendControlReply(op, ST_BAD_ARGS, nullptr, 0); break; }	// asks for frames that were never sent
		uint16_t oldest = _win.count() ? _win.oldestSeq() : _seq;
		if (_outIdx.count() && swea::seqBefore(_outIdx.firstSeq(), oldest)) oldest = _outIdx.firstSeq();	// the outbox reaches further back
		bool gap = swea::seqBefore(req, oldest);
		uint16_t first = gap ? oldest : req;
		_gap = gap;
		_replaySeq = first;
		_replayPosValid = false;
		_replayEnd = _seq;
		_replay = first != _seq;
		uint8_t out[3];
		put16(out, first);
		out[2] = gap ? 1 : 0;
		sendControlReply(op, ST_OK, out, sizeof(out));
		break;
	}
	case OP_ACK:
		if (len != 3) break;			// no reply for ACK (protocol doc)
		_win.ack(get16(&data[1]));
		if (_outIdx.count() && !swea::seqBefore(get16(&data[1]), (uint16_t)(_outIdx.nextSeq() - 1)) && !_replay) outboxClear();	// all of it delivered
		break;
	case OP_SET_PROFILE:
		if (len != 2 || data[1] > 2) { sendControlReply(op, ST_BAD_ARGS, nullptr, 0); break; }
		_profile = data[1];
		sendControlReply(op, ST_OK, nullptr, 0);
		break;
	case OP_MARK_EVENT:
	{
		// A UN packet through EmotiBit's control path is logged on the SD card with a packet number and comes back
		// through sendData(), so the event reaches the phone in the data stream too. Reply: the device clock.
		if (len != 2) { sendControlReply(op, ST_BAD_ARGS, nullptr, 0); break; }
		if (!pushControlPacket("0,0,1,UN,1,100," + String((unsigned)data[1]))) { sendControlReply(op, ST_BUSY, nullptr, 0); break; }
		uint8_t out[4];
		put32(out, millis());
		sendControlReply(op, ST_OK, out, sizeof(out));
		break;
	}
	case OP_RECORD:
	{
		// off: 09 00      on: 09 01 <utc seconds u32> <milliseconds u16>   (names the files like the Oscilloscope: YYYY-MM-DD_HH-MM-SS-mmm)
		if (len == 2 && data[1] == 0)
		{
			if (!pushControlPacket("0,0,0,RE,1,100")) { sendControlReply(op, ST_BUSY, nullptr, 0); break; }
			_recording = false;
			sendControlReply(op, ST_OK, nullptr, 0);
			break;
		}
		if (len != 8 || data[1] != 1) { sendControlReply(op, ST_BAD_ARGS, nullptr, 0); break; }
		time_t t = (time_t)swea::get32(&data[2]);
		uint16_t ms = get16(&data[6]);
		struct tm tmv;
		gmtime_r(&t, &tmv);
		if (tmv.tm_year + 1900 < 2020 || ms > 999) { sendControlReply(op, ST_BAD_ARGS, nullptr, 0); break; }	// a phone with a wrong clock would name files 1970
		char name[32];
		snprintf(name, sizeof(name), "%04d-%02d-%02d_%02d-%02d-%02d-%03u", tmv.tm_year + 1900, tmv.tm_mon + 1, tmv.tm_mday, tmv.tm_hour, tmv.tm_min, tmv.tm_sec, (unsigned)ms);
		if (!pushControlPacket(String("0,0,1,RB,1,100,") + name)) { sendControlReply(op, ST_BUSY, nullptr, 0); break; }
		_recording = true;
		sendControlReply(op, ST_OK, nullptr, 0);
		break;
	}
	case OP_SLEEP:
		// MH = hibernate, the same packet the Oscilloscope sends. EmotiBit flushes the SD card and powers down in update().
		if (!pushControlPacket("0,0,0,MH,1,100")) { sendControlReply(op, ST_BUSY, nullptr, 0); break; }
		sendControlReply(op, ST_OK, nullptr, 0);
		break;
	case OP_ENTER_PAIRING:
	{
		if (len != 2) { sendControlReply(op, ST_BAD_ARGS, nullptr, 0); break; }
		openPairing(data[1] == 0 ? 120 : data[1]);
		sendControlReply(op, ST_OK, nullptr, 0);
		break;
	}
	case OP_CLEAR_BONDS:
	{
		// deleteAllBonds() leaves NimBLE's resolving list and peer records behind, which later breaks the recognition of
		// phones with private addresses. Wipe NimBLE's whole store and restart (the passkey lives elsewhere and stays).
		sendControlReply(op, ST_OK, nullptr, 0);
		delay(300);
		// Erase while NimBLE still runs (deinit can close the NVS handle first), then restart at once so nothing in RAM is
		// written back.
		NimBLEDevice::deleteAllBonds();
		nvs_handle_t h;
		esp_err_t err = nvs_open("nimble_bond", NVS_READWRITE, &h);
		if (err == ESP_OK)
		{
			err = nvs_erase_all(h);
			if (err == ESP_OK) err = nvs_commit(h);
			nvs_close(h);
		}
		Serial.print("SWEA BLE: cleared all bonds (nvs result "); Serial.print((int)err); Serial.println("), restarting");
		delay(100);
		ESP.restart();
		break;
	}
	case OP_FINISH:
	{
		// End of the measurement: no new values are taken in, the ring is turned into notifications, and the reply names the
		// last sequence number. The phone fetches what it misses with RESUME_FROM; STOP_STREAM then clears the outbox.
		if (len != 1) { sendControlReply(op, ST_BAD_ARGS, nullptr, 0); break; }
		if (!_session) { sendControlReply(op, ST_NOT_ALLOWED, nullptr, 0); break; }
		if (!_finished) flushRing();
		uint8_t out[3];
		size_t n = swea::buildFinishReply(out, sizeof(out), (uint16_t)(_seq - 1), _gap || _lost, !_sessionFrames);
		sendControlReply(op, ST_OK, out, n);
		break;
	}
	default:
		sendControlReply(op, ST_UNSUPPORTED, nullptr, 0);
	}
}

#endif // SWEA_BLE
