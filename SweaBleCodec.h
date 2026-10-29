/**************************************************************************/
/*!
    @file     SweaBleCodec.h

    SWEA: pure C++ pieces of the BLE protocol (docs/SWEA_BLE_PROTOCOL.md):
      - parsing EmotiBit text packets (the lines written to the SD card),
      - the in-RAM record ring buffer,
      - building data notifications.
    No Arduino or ESP dependencies, so it is unit-tested on a host
    (testing/SweaBleCodec).
*/
/**************************************************************************/
#pragma once
#include <stdint.h>
#include <stddef.h>
#include <string.h>
#include <stdlib.h>

namespace swea
{
	static const uint8_t PROTOCOL_VERSION = 1;
	static const size_t NOTIFICATION_HEADER_LEN = 3;	// ver:u8 seq:u16
	static const size_t RECORD_HEADER_LEN = 7;			// tag[2] n:u8 t_ms:u32
	static const size_t MAX_VALUES = 128;				// values per record held in RAM

	inline void put16(uint8_t *p, uint16_t v) { p[0] = (uint8_t)(v & 0xFF); p[1] = (uint8_t)(v >> 8); }
	inline void put32(uint8_t *p, uint32_t v) { p[0] = (uint8_t)(v & 0xFF); p[1] = (uint8_t)((v >> 8) & 0xFF); p[2] = (uint8_t)((v >> 16) & 0xFF); p[3] = (uint8_t)(v >> 24); }
	inline uint16_t get16(const uint8_t *p) { return (uint16_t)(p[0] | (p[1] << 8)); }
	inline uint32_t get32(const uint8_t *p) { return (uint32_t)p[0] | ((uint32_t)p[1] << 8) | ((uint32_t)p[2] << 16) | ((uint32_t)p[3] << 24); }

	// Tags that carry numeric sample data. Everything else (control, mode, info, sync packets) is not forwarded.
	inline bool isDataTag(const char *t)
	{
		static const char *const tags[] = {
			"EA","EL","ER","PI","PR","PG","O2","T0","T1","TH","H0",
			"AX","AY","AZ","GX","GY","GZ","MX","MY","MZ",
			"BV","B%","SD","HR","BI","SA","SR","SF","DC","DO","UN" };
		for (size_t i = 0; i < sizeof(tags) / sizeof(tags[0]); i++)
			if (t[0] == tags[i][0] && t[1] == tags[i][1]) return true;
		return false;
	}

	// Profile 0 = all channels, 1 = EDA + temperature + motion (no PPG, no magnetometer), 2 = all (100 Hz PPG build).
	inline bool profileAllows(uint8_t profile, const char *t)
	{
		if (profile != 1) return true;
		if (t[0] == 'P' && (t[1] == 'I' || t[1] == 'R' || t[1] == 'G')) return false;	// PPG
		if (t[0] == 'M' && (t[1] == 'X' || t[1] == 'Y' || t[1] == 'Z')) return false;	// magnetometer
		return true;
	}

	struct ParsedPacket
	{
		char tag[2];
		uint8_t n;
		uint32_t t_ms;
		float v[MAX_VALUES];
	};

	/** Parse one EmotiBit text packet line: "TIMESTAMP,PACKET#,COUNT,TAG,VERSION,RELIABILITY,v0,v1,...".
	 *  Returns true only for a well formed packet with a data tag and at least one numeric value. */
	inline bool parsePacketLine(const char *s, size_t len, ParsedPacket &out)
	{
		const char *p = s, *end = s + len;
		auto nextField = [&](const char *&f, const char *&fe) -> bool {
			if (p > end) return false;
			f = p;
			while (p < end && *p != ',') p++;
			fe = p;
			if (p < end) p++; else p = end + 1;	// consumed the last field
			return true;
		};
		const char *f, *fe;
		char tmp[24];
		auto toNum = [&](const char *a, const char *b, char *dst) -> bool {
			size_t l = (size_t)(b - a);
			if (l == 0 || l >= 24) return false;
			memcpy(dst, a, l); dst[l] = 0; return true;
		};
		if (!nextField(f, fe) || !toNum(f, fe, tmp)) return false;
		char *ep;
		unsigned long ts = strtoul(tmp, &ep, 10);
		if (*ep) return false;
		if (!nextField(f, fe)) return false;			// packet number (not forwarded)
		if (!nextField(f, fe) || !toNum(f, fe, tmp)) return false;
		unsigned long count = strtoul(tmp, &ep, 10);	// declared value count
		if (*ep) return false;
		if (!nextField(f, fe) || (fe - f) != 2) return false;
		out.tag[0] = f[0]; out.tag[1] = f[1];
		if (!isDataTag(out.tag)) return false;
		if (!nextField(f, fe)) return false;			// protocol version
		if (!nextField(f, fe)) return false;			// reliability
		size_t n = 0;
		while (p <= end && n < MAX_VALUES)
		{
			if (!nextField(f, fe)) break;
			if (!toNum(f, fe, tmp)) return false;
			float v = strtof(tmp, &ep);
			if (*ep) return false;
			out.v[n++] = v;
		}
		if (n == 0 || (count != 0 && n > count)) return false;
		out.n = (uint8_t)n;
		out.t_ms = (uint32_t)ts;
		return true;
	}

	/** Byte ring of records. Each stored record is [len:u16][tag:2][n:u8][t_ms:u32][f32 x n].
	 *  When full, the oldest records are dropped (and counted) so the newest data is kept. */
	class RecordRing
	{
	public:
		struct View
		{
			char tag[2];
			uint8_t n;
			uint32_t t_ms;
			float v[MAX_VALUES];
		};
		struct Cursor { size_t head; size_t used; uint8_t partial; };

		void init(uint8_t *buf, size_t cap) { _b = buf; _cap = cap; _head = 0; _used = 0; _partial = 0; _dropped = 0; }
		size_t used() const { return _used; }
		size_t capacity() const { return _cap; }
		uint32_t dropped() const { return _dropped; }
		bool empty() const { return _used == 0; }

		bool push(const char *tag, uint8_t n, uint32_t t_ms, const float *v)
		{
			if (!_b || n == 0) return false;
			size_t body = RECORD_HEADER_LEN + 4u * n;
			size_t need = 2 + body;
			if (need > _cap) return false;
			while (_cap - _used < need) dropHead();
			uint8_t tmp[2 + RECORD_HEADER_LEN + 4 * MAX_VALUES];
			put16(tmp, (uint16_t)body);
			tmp[2] = (uint8_t)tag[0]; tmp[3] = (uint8_t)tag[1]; tmp[4] = n;
			put32(&tmp[5], t_ms);
			for (uint8_t i = 0; i < n; i++) memcpy(&tmp[9 + 4 * i], &v[i], 4);
			size_t tail = (_head + _used) % _cap;
			for (size_t i = 0; i < need; i++) _b[(tail + i) % _cap] = tmp[i];
			_used += need;
			return true;
		}

		bool peek(View &out) const
		{
			if (_used == 0) return false;
			uint8_t hdr[2];
			rd(_head, hdr, 2);
			size_t body = get16(hdr);
			uint8_t rec[RECORD_HEADER_LEN + 4 * MAX_VALUES];
			rd((_head + 2) % _cap, rec, body);
			out.tag[0] = (char)rec[0]; out.tag[1] = (char)rec[1];
			out.n = rec[2];
			out.t_ms = get32(&rec[3]);
			for (uint8_t i = 0; i < out.n; i++) memcpy(&out.v[i], &rec[RECORD_HEADER_LEN + 4 * i], 4);
			return true;
		}

		void popHead() { if (_used) { dropHeadInternal(); } }
		uint8_t partial() const { return _partial; }
		void setPartial(uint8_t k) { _partial = k; }
		Cursor cursor() const { Cursor c = { _head, _used, _partial }; return c; }
		void restore(const Cursor &c) { _head = c.head; _used = c.used; _partial = c.partial; }

	private:
		uint8_t *_b = nullptr;
		size_t _cap = 0, _head = 0, _used = 0;
		uint8_t _partial = 0;	// floats of the head record already sent
		uint32_t _dropped = 0;

		void rd(size_t pos, uint8_t *dst, size_t n) const { for (size_t i = 0; i < n; i++) dst[i] = _b[(pos + i) % _cap]; }
		void dropHeadInternal()
		{
			uint8_t hdr[2];
			rd(_head, hdr, 2);
			size_t total = 2 + (size_t)get16(hdr);
			_head = (_head + total) % _cap;
			_used -= total;
			_partial = 0;
		}
		void dropHead() { dropHeadInternal(); _dropped++; }
	};

	/** Window of the most recently sent notifications, for RESUME_FROM replay. Fixed slots, oldest overwritten.
	 *  Sequence numbers are compared modulo 2^16. */
	class FrameWindow
	{
	public:
		static const size_t MAX_FRAME = 244;
		static const size_t SLOT_BYTES = 2 + 1 + MAX_FRAME;	// seq:u16 len:u8 data

		void init(uint8_t *mem, size_t slots) { _m = mem; _slots = slots; _head = 0; _count = 0; _next = 0; }
		static constexpr size_t bytesFor(size_t slots) { return slots * SLOT_BYTES; }
		size_t count() const { return _count; }
		uint16_t nextSeq() const { return _next; }	// sequence number after the newest stored frame
		uint16_t oldestSeq() const { return (uint16_t)(_next - _count); }

		/** Store a frame that was just sent. Frames must be stored with consecutive sequence numbers. */
		bool store(uint16_t seq, const uint8_t *data, size_t len)
		{
			if (!_m || len == 0 || len > MAX_FRAME) return false;
			if (_count == _slots) { _head = (_head + 1) % _slots; _count--; }
			size_t idx = (_head + _count) % _slots;
			uint8_t *slot = _m + idx * SLOT_BYTES;
			put16(slot, seq); slot[2] = (uint8_t)len; memcpy(slot + 3, data, len);
			_count++;
			_next = (uint16_t)(seq + 1);
			return true;
		}

		/** Copy the frame with this sequence number. False if it is not (or no longer) in the window. */
		bool get(uint16_t seq, uint8_t *out, size_t &len) const
		{
			uint16_t off = (uint16_t)(seq - oldestSeq());
			if (off >= _count) return false;
			const uint8_t *slot = _m + ((_head + off) % _slots) * SLOT_BYTES;
			len = slot[2];
			memcpy(out, slot + 3, len);
			return true;
		}

		/** The phone has received everything up to and including seq: free those slots. */
		void ack(uint16_t seq)
		{
			while (_count > 0 && (int16_t)(oldestSeq() - seq) <= 0) { _head = (_head + 1) % _slots; _count--; }
		}

	private:
		uint8_t *_m = nullptr;
		size_t _slots = 0, _head = 0, _count = 0;
		uint16_t _next = 0;
	};

	/** True if sequence a is before b (modulo 2^16). */
	inline bool seqBefore(uint16_t a, uint16_t b) { return (int16_t)(a - b) < 0; }

	/** Build one data notification from the head of the ring.
	 *  maxLen is the usable notification payload (ATT_MTU - 3). Returns the length (0 = nothing to send or no room).
	 *  Records that do not fit entirely are split into several records with the same t_ms.
	 *  The ring advances; on a failed send call ring.restore(cursor) with a cursor taken before.
	 *  Records the stream profile does not allow are dropped here too: the ring keeps filling while nobody streams,
	 *  so it can hold records from before a profile change. */
	inline size_t buildNotification(RecordRing &ring, uint16_t seq, uint8_t *out, size_t maxLen, uint8_t profile = 0)
	{
		if (maxLen < NOTIFICATION_HEADER_LEN + RECORD_HEADER_LEN + 4) return 0;
		size_t len = NOTIFICATION_HEADER_LEN;
		out[0] = PROTOCOL_VERSION;
		put16(&out[1], seq);
		RecordRing::View r;
		while (ring.peek(r))
		{
			if (ring.partial() == 0 && !profileAllows(profile, r.tag)) { ring.popHead(); continue; }
			uint8_t sent = ring.partial();
			size_t remaining = (size_t)r.n - sent;
			size_t space = maxLen - len;
			if (space < RECORD_HEADER_LEN + 4) break;
			size_t fit = (space - RECORD_HEADER_LEN) / 4;
			size_t m = remaining < fit ? remaining : fit;
			out[len] = (uint8_t)r.tag[0]; out[len + 1] = (uint8_t)r.tag[1];
			out[len + 2] = (uint8_t)m;
			put32(&out[len + 3], r.t_ms);
			for (size_t i = 0; i < m; i++) memcpy(&out[len + RECORD_HEADER_LEN + 4 * i], &r.v[sent + i], 4);
			len += RECORD_HEADER_LEN + 4 * m;
			if (m == remaining) ring.popHead(); else ring.setPartial((uint8_t)(sent + m));
			if (m < remaining) break;	// frame is full
		}
		return len > NOTIFICATION_HEADER_LEN ? len : 0;
	}

	/** Opcode of the FINISH command, and the flags in its reply (docs/SWEA_BLE_PROTOCOL.md). */
	static const uint8_t OPCODE_FINISH = 0x0D;
	static const uint8_t FINISH_FLAG_GAP = 0x01;	// frames were thrown away (outbox full or SD error) during the session
	static const uint8_t FINISH_FLAG_EMPTY = 0x02;	// the session produced no notification at all (last_seq is meaningless)

	/** FINISH reply arguments: last_seq:u16, flags:u8. Returns the length written (0 if cap < 3). */
	inline size_t buildFinishReply(uint8_t *out, size_t cap, uint16_t lastSeq, bool gap, bool empty)
	{
		if (cap < 3) return 0;
		put16(out, lastSeq);
		out[2] = (uint8_t)((gap ? FINISH_FLAG_GAP : 0) | (empty ? FINISH_FLAG_EMPTY : 0));
		return 3;
	}

	/** One record in /swea/outbox.bin: seq:u16 len:u8 bytes (the finished notification). Returns the length written. */
	static const size_t OUTBOX_RECORD_OVERHEAD = 3;
	inline size_t outboxEncode(uint8_t *out, uint16_t seq, const uint8_t *data, size_t len)
	{
		put16(out, seq); out[2] = (uint8_t)len;
		memcpy(out + OUTBOX_RECORD_OVERHEAD, data, len);
		return OUTBOX_RECORD_OVERHEAD + len;
	}

	/** Index of the SD outbox: sequence number -> file offset of the record.
	 *  Records are appended with consecutive sequence numbers (mod 2^16), so the file position of a sequence is found
	 *  from a table that holds the offset of every `stride`-th record; the caller reads forward from that anchor over
	 *  `skip` records (each is seq, len, bytes). With the defaults (4096 slots, stride 4) it spans 16384 records.
	 *  The outbox is capped by record count (slots * stride) and by file bytes. A capped append is refused and counted:
	 *  new notifications are thrown away, the old ones stay replayable. */
	class OutboxIndex
	{
	public:
		static const size_t DEFAULT_SLOTS = 4096;

		void init(uint32_t *mem, size_t slots, size_t stride, uint32_t maxBytes)
		{
			_off = mem; _slots = slots; _stride = stride ? stride : 1; _maxBytes = maxBytes;
			clear();
			_dropped = 0;
		}
		static constexpr size_t bytesFor(size_t slots) { return slots * sizeof(uint32_t); }
		void clear() { _count = 0; _bytes = 0; _first = 0; }

		size_t count() const { return _count; }
		uint32_t bytes() const { return _bytes; }			// file size, also the offset of the next record
		uint32_t dropped() const { return _dropped; }		// appends refused because of the cap
		size_t maxRecords() const { return _off ? _slots * _stride : 0; }
		uint16_t firstSeq() const { return _first; }		// oldest record (valid when count() > 0)
		uint16_t nextSeq() const { return (uint16_t)(_first + _count); }

		bool canAppend(size_t len) const
		{
			if (!_off || len == 0 || len > 255) return false;
			if (_count >= maxRecords()) return false;
			return (uint64_t)_bytes + OUTBOX_RECORD_OVERHEAD + len <= _maxBytes;
		}

		/** Register a record of `len` data bytes written at bytes(). Refused (false) when capped, or when seq does not
		 *  follow the newest record. The first record after clear() starts the sequence. */
		bool append(uint16_t seq, size_t len)
		{
			if (!canAppend(len)) { _dropped++; return false; }
			if (_count == 0) _first = seq;
			else if (seq != nextSeq()) return false;
			if (_count % _stride == 0) _off[_count / _stride] = _bytes;
			_count++;
			_bytes += (uint32_t)(OUTBOX_RECORD_OVERHEAD + len);
			return true;
		}

		bool contains(uint16_t seq) const { return (uint16_t)(seq - _first) < _count; }

		/** Offset of the nearest indexed record at or before seq, and how many records to skip from there. */
		bool find(uint16_t seq, uint32_t &anchorOffset, uint16_t &skip) const
		{
			if (!contains(seq)) return false;
			size_t idx = (size_t)(uint16_t)(seq - _first);
			anchorOffset = _off[idx / _stride];
			skip = (uint16_t)(idx % _stride);
			return true;
		}

	private:
		uint32_t *_off = nullptr;
		size_t _slots = 0, _stride = 1, _count = 0;
		uint32_t _maxBytes = 0, _bytes = 0, _dropped = 0;
		uint16_t _first = 0;
	};

	// GET_INFO reply arguments: proto:u8, firmware string\0, device id\0. Strings are cut to fit; both stay
	// nul-terminated. Returns the length written (0 if cap is too small for the version byte and two nuls).
	inline size_t buildInfoReply(uint8_t *out, size_t cap, uint8_t proto, const char *fw, const char *deviceId)
	{
		if (cap < 3) return 0;
		size_t n = 0;
		out[n++] = proto;
		for (size_t i = 0; fw && fw[i] && n < cap - 2; i++) out[n++] = (uint8_t)fw[i];
		out[n++] = 0;
		for (size_t i = 0; deviceId && deviceId[i] && n < cap - 1; i++) out[n++] = (uint8_t)deviceId[i];
		out[n++] = 0;
		return n;
	}

	// Rate limiter for new pairings (firmware 0.12: an unknown phone may pair at any time, the passkey is the only secret).
	// After FAILS_BEFORE_LOCKOUT consecutive failed pairings the board refuses new pairings for 60 s; every further lockout
	// doubles the time, up to 15 min. A success or reset() (reboot, button) starts over. Times are millis(), wrap-safe.
	class PairingGuard
	{
	public:
		static const uint8_t FAILS_BEFORE_LOCKOUT = 5;
		static const uint32_t BASE_LOCKOUT_MS = 60000u;
		static const uint32_t MAX_LOCKOUT_MS = 900000u;

		bool locked(uint32_t now) const { return _locked && (int32_t)(now - _until) < 0; }
		uint32_t remainingMs(uint32_t now) const { return locked(now) ? (uint32_t)(_until - now) : 0; }
		uint8_t lockouts() const { return _lockouts; }

		// A pairing attempt that failed. Ignored while locked (the board refused it anyway).
		void recordFailure(uint32_t now)
		{
			if (locked(now)) return;
			_locked = false;
			if (++_fails < FAILS_BEFORE_LOCKOUT) return;
			_fails = 0;
			uint32_t d = BASE_LOCKOUT_MS;
			for (uint8_t i = 0; i < _lockouts && d < MAX_LOCKOUT_MS; i++) d *= 2;
			if (d > MAX_LOCKOUT_MS) d = MAX_LOCKOUT_MS;
			if (_lockouts < 255) _lockouts++;
			_until = now + d;
			_locked = true;
		}
		void recordSuccess() { reset(); }
		void reset() { _fails = 0; _lockouts = 0; _locked = false; _until = 0; }

	private:
		uint8_t _fails = 0, _lockouts = 0;
		bool _locked = false;
		uint32_t _until = 0;
	};
}
