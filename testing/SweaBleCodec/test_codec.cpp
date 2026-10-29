// Host unit test for SweaBleCodec.h. Build and run with run_test.bat (MSVC) or run_test.sh (g++).
#include <stdio.h>
#include <string.h>
#include <vector>
#include "../../SweaBleCodec.h"

static int g_fail = 0, g_checks = 0;
#define CHECK(c) do { g_checks++; if (!(c)) { g_fail++; printf("FAIL line %d: %s\n", __LINE__, #c); } } while (0)

static bool parse(const char *s, swea::ParsedPacket &p) { return swea::parsePacketLine(s, strlen(s), p); }

int main()
{
	using namespace swea;

	// 1. Protocol document test vector (docs/SWEA_BLE_PROTOCOL.md section 8).
	{
		static uint8_t mem[256]; RecordRing ring; ring.init(mem, sizeof(mem));
		float v[2] = { 0.030269f, 0.030269f };
		CHECK(ring.push("EA", 2, 531459, v));
		uint8_t out[244];
		size_t n = buildNotification(ring, 7, out, sizeof(out));
		const uint8_t expect[] = { 0x01,0x07,0x00, 0x45,0x41,0x02,0x03,0x1c,0x08,0x00, 0xb2,0xf6,0xf7,0x3c, 0xb2,0xf6,0xf7,0x3c };
		CHECK(n == sizeof(expect));
		CHECK(n == sizeof(expect) && memcmp(out, expect, n) == 0);
		CHECK(ring.empty());
	}

	// 2. Parsing SD-card lines.
	{
		ParsedPacket p;
		CHECK(parse("531459,17301,2,EA,1,100,0.030269,0.030269", p));
		CHECK(p.tag[0] == 'E' && p.tag[1] == 'A' && p.n == 2 && p.t_ms == 531459);
		CHECK(p.v[0] > 0.03026f && p.v[0] < 0.03028f);
		CHECK(parse("531473,17298,3,PI,1,100,112870,112866,112867", p));
		CHECK(p.n == 3 && p.v[0] == 112870.0f && p.v[2] == 112867.0f);
		CHECK(parse("531459,17302,2,EL,1,100,26425.000000,26425.000000", p) && p.v[0] == 26425.0f);
		CHECK(parse("1000,1,1,B%,1,100,87", p) && p.v[0] == 87.0f);
		CHECK(parse("1000,1,3,AX,1,100,-0.436,0.1,-9.8", p) && p.v[0] < -0.43f && p.v[2] < -9.7f);
		CHECK(parse("2000,3,1,UN,1,100,7", p) && p.n == 1 && p.v[0] == 7.0f);	// event marker with a numeric code
		CHECK(!parse("2000,3,1,UN,1,100,hello", p));	// free-text user notes are not forwarded
		CHECK(!parse("531473,5,1,EM,1,100,MN", p));		// mode packet, not a data tag
		CHECK(!parse("531473,5,1,AK,1,100,12", p));		// control
		CHECK(!parse("abc,1,1,EA,1,100,1", p));			// bad timestamp
		CHECK(!parse("1000,1,1,EA,1,100", p));			// no payload
		CHECK(!parse("1000,1,1,EA,1,100,12x", p));		// non numeric payload
		CHECK(!parse("", p));
		CHECK(!parse("1000,1", p));
		CHECK(!parse("1000,1,1,EAA,1,100,1", p));		// tag length 3
	}

	// 3. Profiles.
	CHECK(profileAllows(0, "PI") && profileAllows(0, "MX") && profileAllows(2, "PG"));
	CHECK(!profileAllows(1, "PI") && !profileAllows(1, "PG") && !profileAllows(1, "MZ"));
	CHECK(profileAllows(1, "EA") && profileAllows(1, "AX") && profileAllows(1, "T1") && profileAllows(1, "HR"));

	// 4. Splitting a long record over small frames; values keep order and t_ms.
	{
		static uint8_t mem[512]; RecordRing ring; ring.init(mem, sizeof(mem));
		float v[10]; for (int i = 0; i < 10; i++) v[i] = (float)(i + 1);
		CHECK(ring.push("PG", 10, 4242, v));
		std::vector<float> got;
		uint8_t out[3 + 7 + 4 * 4];	// room for 4 floats
		int frames = 0; uint16_t seq = 0;
		while (true)
		{
			size_t n = buildNotification(ring, seq++, out, sizeof(out));
			if (!n) break;
			frames++;
			CHECK(out[0] == 1);
			CHECK(out[3] == 'P' && out[4] == 'G');
			CHECK(get32(&out[6]) == 4242);
			for (int i = 0; i < out[5]; i++) { float f; memcpy(&f, &out[10 + 4 * i], 4); got.push_back(f); }
		}
		CHECK(frames == 3 && got.size() == 10);
		for (int i = 0; i < (int)got.size() && i < 10; i++) CHECK(got[i] == (float)(i + 1));
		CHECK(ring.empty());
	}

	// 5. Several records share one frame; a record that does not fit starts the next frame.
	{
		static uint8_t mem[512]; RecordRing ring; ring.init(mem, sizeof(mem));
		float a[2] = { 1, 2 }, b[2] = { 3, 4 };
		ring.push("EA", 2, 1, a); ring.push("T1", 2, 2, b); ring.push("HR", 1, 3, a);
		uint8_t out[3 + 15 + 15 + 11];
		size_t n = buildNotification(ring, 1, out, sizeof(out));
		CHECK(n == 3 + 15 + 15 + 11);
		CHECK(ring.empty());
	}

	// 6. A failed send is rolled back and the same bytes are produced again.
	{
		static uint8_t mem[256]; RecordRing ring; ring.init(mem, sizeof(mem));
		float v[3] = { 1, 2, 3 };
		ring.push("AX", 3, 10, v); ring.push("AY", 3, 10, v);
		RecordRing::Cursor c = ring.cursor();
		uint8_t o1[100], o2[100];
		size_t n1 = buildNotification(ring, 5, o1, 3 + 19);	// fits one 3-value record
		ring.restore(c);
		size_t n2 = buildNotification(ring, 5, o2, 3 + 19);
		CHECK(n1 == n2 && n1 == 22 && memcmp(o1, o2, n1) == 0);
		CHECK(!ring.empty());
	}

	// 7. Full ring drops the oldest records, keeps the newest, and counts the drops. Wrap-around works.
	{
		static uint8_t mem[100]; RecordRing ring; ring.init(mem, sizeof(mem));
		for (uint32_t t = 1; t <= 20; t++) { float v[2] = { (float)t, 0 }; CHECK(ring.push("EA", 2, t, v)); }	// 17 bytes each
		CHECK(ring.dropped() > 0 && ring.used() <= sizeof(mem));
		RecordRing::View r; uint32_t last = 0; int count = 0;
		while (ring.peek(r)) { CHECK(r.t_ms > last); last = r.t_ms; ring.popHead(); count++; }
		CHECK(last == 20);
		CHECK((uint32_t)count + ring.dropped() == 20);
	}

	// 8. Round trip: SD text line -> parse -> ring -> notification -> decode.
	{
		static uint8_t mem[512]; RecordRing ring; ring.init(mem, sizeof(mem));
		ParsedPacket p;
		CHECK(parse("531473,17298,3,PI,1,100,112870,112866,112867", p));
		CHECK(ring.push(p.tag, p.n, p.t_ms, p.v));
		uint8_t out[244];
		size_t n = buildNotification(ring, 0, out, sizeof(out));
		CHECK(n == 3 + 7 + 12);
		CHECK(out[3] == 'P' && out[4] == 'I' && out[5] == 3 && get32(&out[6]) == 531473);
		float f; memcpy(&f, &out[10], 4); CHECK(f == 112870.0f);
	}

	// 9. Too small a frame produces nothing instead of looping.
	{
		static uint8_t mem[64]; RecordRing ring; ring.init(mem, sizeof(mem));
		float v[1] = { 1 }; ring.push("EA", 1, 1, v);
		uint8_t out[8];
		CHECK(buildNotification(ring, 0, out, sizeof(out)) == 0);
	}


	// 10. FrameWindow: store, get, overwrite oldest, ack, sequence wrap-around.
	{
		static uint8_t mem[FrameWindow::bytesFor(4)];
		FrameWindow w; w.init(mem, 4);
		uint8_t f[10], o[FrameWindow::MAX_FRAME]; size_t l;
		CHECK(w.count() == 0);
		for (uint16_t seq = 65533; seq != (uint16_t)(65533 + 6); seq++)	// crosses the 16-bit wrap
		{
			memset(f, (uint8_t)seq, sizeof(f));
			CHECK(w.store(seq, f, sizeof(f)));
		}
		CHECK(w.count() == 4);								// 6 stored, 4 slots
		CHECK(w.nextSeq() == (uint16_t)(65533 + 6));
		CHECK(w.oldestSeq() == (uint16_t)(65533 + 2));
		CHECK(!w.get(65533, o, l) && !w.get(65534, o, l));	// overwritten
		CHECK(w.get(65535, o, l) && l == 10 && o[0] == 0xFF);
		CHECK(w.get(0, o, l) && o[0] == 0);				// after the wrap
		CHECK(w.get(2, o, l) && o[0] == 2);
		CHECK(!w.get(3, o, l));							// not sent yet
		w.ack(0);											// frees 65535 and 0
		CHECK(w.count() == 2 && w.oldestSeq() == 1);
		w.ack(1000);										// acking beyond the newest empties the window
		CHECK(w.count() == 0);
		CHECK(seqBefore(65535, 1) && !seqBefore(1, 65535) && !seqBefore(5, 5));
	}

	{	// Records already in the ring under another profile: profile 1 must not send PPG or magnetometer data
		static uint8_t mem[2048];
		RecordRing ring;
		ring.init(mem, sizeof(mem));
		float v[2] = {1.0f, 2.0f};
		ring.push("PI", 2, 100, v);
		ring.push("EA", 2, 100, v);
		ring.push("MX", 1, 100, v);
		ring.push("T1", 1, 100, v);
		uint8_t out[244];
		size_t n = buildNotification(ring, 3, out, sizeof(out), 1);
		CHECK(n == 3 + (7 + 8) + (7 + 4));					// EA (2 values) and T1 (1 value) only
		CHECK(out[3] == 'E' && out[4] == 'A');
		CHECK(out[3 + 15] == 'T' && out[3 + 16] == '1');
		CHECK(ring.empty());								// the skipped records are gone, not left for later
		ring.push("PG", 1, 200, v);
		CHECK(buildNotification(ring, 4, out, sizeof(out), 1) == 0 && ring.empty());	// nothing allowed: nothing sent
		ring.push("PG", 1, 300, v);
		CHECK(buildNotification(ring, 5, out, sizeof(out)) == 3 + 7 + 4);				// profile 0 (default) sends it
	}

	{	// GET_INFO reply: proto, firmware\0, device id\0 (the phone registers the device by this id)
		uint8_t out[64];
		size_t n = buildInfoReply(out, sizeof(out), 1, "swea-ble-0.5", "MD-V5-0000302");
		CHECK(n == 1 + 12 + 1 + 13 + 1);
		CHECK(out[0] == 1);
		CHECK(strcmp((const char *)&out[1], "swea-ble-0.5") == 0);
		CHECK(strcmp((const char *)&out[14], "MD-V5-0000302") == 0);
		CHECK(out[n - 1] == 0);
		uint8_t small[20];									// a long id is cut, never overflows, stays nul-terminated
		size_t m = buildInfoReply(small, sizeof(small), 1, "swea-ble-0.5", "ABCDEFGHIJKLMNOP");
		CHECK(m == sizeof(small) && small[m - 1] == 0);
		CHECK(strcmp((const char *)&small[1], "swea-ble-0.5") == 0);
		n = buildInfoReply(out, sizeof(out), 1, "fw", "");	// unknown id: empty string
		CHECK(n == 1 + 3 + 1 && out[4] == 0);
	}

	{	// FINISH reply: last_seq:u16, flags:u8
		uint8_t out[8];
		CHECK(OPCODE_FINISH == 0x0D);
		CHECK(buildFinishReply(out, sizeof(out), 0x1234, false, false) == 3);
		CHECK(out[0] == 0x34 && out[1] == 0x12 && out[2] == 0);
		CHECK(buildFinishReply(out, sizeof(out), 7, true, false) == 3 && out[2] == FINISH_FLAG_GAP);
		CHECK(buildFinishReply(out, sizeof(out), 0xFFFF, false, true) == 3 && out[0] == 0xFF && out[1] == 0xFF && out[2] == FINISH_FLAG_EMPTY);
		CHECK(buildFinishReply(out, sizeof(out), 1, true, true) == 3 && out[2] == (FINISH_FLAG_GAP | FINISH_FLAG_EMPTY));
		CHECK(buildFinishReply(out, 2, 1, false, false) == 0);
	}

	{	// outbox record: seq:u16 len:u8 bytes
		uint8_t data[5] = {9, 8, 7, 6, 5}, rec[16];
		CHECK(outboxEncode(rec, 0x0102, data, sizeof(data)) == 3 + 5);
		CHECK(rec[0] == 0x02 && rec[1] == 0x01 && rec[2] == 5 && memcmp(rec + 3, data, 5) == 0);
	}

	{	// OutboxIndex, stride 1: append, find, count, bytes
		static uint32_t mem[16];
		OutboxIndex ix;
		ix.init(mem, 16, 1, 1000000);
		CHECK(ix.count() == 0 && ix.bytes() == 0 && ix.maxRecords() == 16);
		uint32_t off; uint16_t skip;
		CHECK(!ix.find(5, off, skip));						// empty
		CHECK(ix.append(100, 10) && ix.append(101, 20) && ix.append(102, 5));
		CHECK(ix.count() == 3 && ix.firstSeq() == 100 && ix.nextSeq() == 103);
		CHECK(ix.bytes() == (3 + 10) + (3 + 20) + (3 + 5));
		CHECK(ix.find(100, off, skip) && off == 0 && skip == 0);
		CHECK(ix.find(101, off, skip) && off == 13 && skip == 0);
		CHECK(ix.find(102, off, skip) && off == 13 + 23 && skip == 0);
		CHECK(!ix.find(99, off, skip) && !ix.find(103, off, skip));
		CHECK(!ix.append(110, 4));							// not the next sequence
		CHECK(ix.count() == 3);
		CHECK(!ix.append(103, 0) && !ix.append(103, 256));	// bad lengths
		ix.clear();
		CHECK(ix.count() == 0 && ix.bytes() == 0 && !ix.find(100, off, skip));
		CHECK(ix.append(500, 4) && ix.firstSeq() == 500 && ix.find(500, off, skip) && off == 0);	// a new session starts anywhere
	}

	{	// OutboxIndex, stride 4: anchors every 4th record, the caller skips forward
		static uint32_t mem[8];
		OutboxIndex ix;
		ix.init(mem, 8, 4, 1000000);
		CHECK(ix.maxRecords() == 32);
		uint32_t expect[10], pos = 0;
		for (int i = 0; i < 10; i++) { expect[i] = pos; CHECK(ix.append((uint16_t)(7 + i), 10 + i)); pos += 3 + 10 + i; }
		CHECK(ix.bytes() == pos);
		for (int i = 0; i < 10; i++)
		{
			uint32_t off; uint16_t skip;
			CHECK(ix.find((uint16_t)(7 + i), off, skip));
			CHECK(skip == (uint16_t)(i % 4) && off == expect[i - i % 4]);
		}
	}

	{	// OutboxIndex: the sequence number wraps at 65535
		static uint32_t mem[8];
		OutboxIndex ix;
		ix.init(mem, 8, 2, 1000000);
		uint16_t s0 = 65534;
		for (int i = 0; i < 6; i++) CHECK(ix.append((uint16_t)(s0 + i), 8));	// 65534 65535 0 1 2 3
		CHECK(ix.firstSeq() == 65534 && ix.nextSeq() == 4);
		uint32_t off; uint16_t skip;
		CHECK(ix.find(65535, off, skip) && off == 0 && skip == 1);
		CHECK(ix.find(0, off, skip) && off == 2 * 11 && skip == 0);
		CHECK(ix.find(3, off, skip) && off == 4 * 11 && skip == 1);
		CHECK(!ix.find(4, off, skip) && !ix.find(65533, off, skip));
		CHECK(!ix.append(5, 8) && ix.append(4, 8));
	}

	{	// OutboxIndex: cap by record count and by bytes; capped appends are refused and counted, old data stays
		static uint32_t mem[4];
		OutboxIndex ix;
		ix.init(mem, 4, 2, 1000000);						// 8 records at most
		for (int i = 0; i < 8; i++) CHECK(ix.append((uint16_t)i, 10));
		CHECK(!ix.canAppend(10) && !ix.append(8, 10) && !ix.append(8, 10));
		CHECK(ix.count() == 8 && ix.dropped() == 2);
		uint32_t off; uint16_t skip;
		CHECK(ix.find(0, off, skip) && off == 0 && ix.find(7, off, skip) && off == 3 * 26 && skip == 1);
		ix.clear();											// STOP: emptied, ready for a new session
		CHECK(ix.count() == 0 && ix.canAppend(10) && ix.append(8, 10));

		static uint32_t mem2[16];
		OutboxIndex bx;
		bx.init(mem2, 16, 1, 100);							// 100 bytes
		CHECK(bx.append(0, 47) && bx.bytes() == 50);		// 50
		CHECK(!bx.append(1, 48) && bx.bytes() == 50);		// 50 + 51 > 100
		CHECK(bx.append(1, 47) && bx.bytes() == 100);		// exactly at the cap
		CHECK(!bx.append(2, 1) && bx.dropped() == 1 + 1);
		CHECK(bx.count() == 2);
		OutboxIndex none;									// not initialised: nothing accepted
		CHECK(!none.canAppend(5) && !none.append(0, 5) && none.maxRecords() == 0);
	}

	{	// OutboxIndex at the real size: 4096 slots span 16384 records
		static uint32_t mem[OutboxIndex::DEFAULT_SLOTS];
		OutboxIndex ix;
		ix.init(mem, OutboxIndex::DEFAULT_SLOTS, 4, 4u * 1024 * 1024);
		CHECK(OutboxIndex::bytesFor(OutboxIndex::DEFAULT_SLOTS) == 16384);
		uint16_t seq = 60000;
		int n = 0;
		while (ix.append(seq, 200)) { seq++; n++; }			// wraps past 65535; ends at the record cap (4 MB / 203 B = 20662 > 16384)
		CHECK(n == 16384 && ix.count() == 16384);
		uint32_t off; uint16_t skip;
		uint16_t probe = (uint16_t)(60000 + 12345);
		CHECK(ix.find(probe, off, skip) && skip == 12345 % 4 && off == (uint32_t)(12345 - 12345 % 4) * 203);
	}

	{	// PairingGuard: 5 failed pairings lock for 60 s, doubling to 15 min; reset on success or reset()
		PairingGuard g;
		uint32_t t = 1000;
		CHECK(!g.locked(t) && g.lockouts() == 0);
		for (int i = 0; i < 4; i++) { g.recordFailure(t); CHECK(!g.locked(t)); }
		g.recordSuccess();									// a success clears the failure count
		for (int i = 0; i < 4; i++) g.recordFailure(t);
		CHECK(!g.locked(t));
		g.recordFailure(t);									// fifth consecutive failure
		CHECK(g.locked(t) && g.remainingMs(t) == 60000 && g.lockouts() == 1);
		CHECK(g.locked(t + 59999) && !g.locked(t + 60000) && g.remainingMs(t + 60000) == 0);
		g.recordFailure(t + 10);							// ignored while locked
		CHECK(g.remainingMs(t + 10) == 59990);
		t += 60000;											// lockout over: five more failures, twice as long
		for (int i = 0; i < 4; i++) { g.recordFailure(t); CHECK(!g.locked(t)); }
		g.recordFailure(t);
		CHECK(g.locked(t) && g.remainingMs(t) == 120000 && g.lockouts() == 2);
		uint32_t expect[] = { 240000, 480000, 900000, 900000 };	// 4, 8 min, then capped at 15 min
		for (int k = 0; k < 4; k++)
		{
			t += g.remainingMs(t);
			for (int i = 0; i < 5; i++) g.recordFailure(t);
			CHECK(g.remainingMs(t) == expect[k]);
		}
		g.recordSuccess();
		CHECK(!g.locked(t) && g.lockouts() == 0);
		for (int i = 0; i < 5; i++) g.recordFailure(t);
		CHECK(g.remainingMs(t) == 60000);					// back to the first step after a success
		g.reset();											// button, reboot
		CHECK(!g.locked(t) && g.lockouts() == 0);
		for (int i = 0; i < 5; i++) g.recordFailure(t);
		CHECK(g.remainingMs(t) == 60000);
		// millis() wrap-around
		PairingGuard w;
		uint32_t near = 0xFFFFFFFFu - 20000u;
		for (int i = 0; i < 5; i++) w.recordFailure(near);
		CHECK(w.locked(near + 59999u) && !w.locked(near + 60000u) && w.remainingMs(near + 30000u) == 30000);
	}

	printf("%d checks, %d failed\n", g_checks, g_fail);
	return g_fail ? 1 : 0;
}
