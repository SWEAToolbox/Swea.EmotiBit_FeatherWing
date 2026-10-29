"""Stream test against a bonded EmotiBit from a PC (pair first with ble_pair_windows.py).

Checks over the real link: TIME_SYNC, START_STREAM, data notifications (frame format, sequence continuity, rate per
signal from the device clock), MARK_EVENT coming back as a UN record, RESUME_FROM replay, ACK and STOP_STREAM.

    .venv-tools/Scripts/python tools/ble_stream_test.py --seconds 20 --profile 0
"""
import argparse
import asyncio
import struct
import sys
import time
from collections import defaultdict

from bleak import BleakClient, BleakScanner

SERVICE = "3f5737c4-b391-44c7-bd74-f2ee561c9775"
DATA = "f14e01ab-48d0-461a-b54e-49fd7be9901a"
CONTROL = "1118282f-3dfe-44a7-a12f-6d06987f1092"
STATUS = "8c2502e8-eacd-45d1-bd20-361eb5a762a2"


def parse_frame(b: bytes):
    """-> (seq, [(tag, t_ms, [values])]) or raises ValueError."""
    if len(b) < 3 or b[0] != 1:
        raise ValueError(f"bad header {b[:3].hex()}")
    seq = struct.unpack_from("<H", b, 1)[0]
    records, i = [], 3
    while i < len(b):
        if i + 7 > len(b):
            raise ValueError("truncated record header")
        tag = b[i:i + 2].decode("ascii", "replace")
        n = b[i + 2]
        t_ms = struct.unpack_from("<I", b, i + 3)[0]
        end = i + 7 + 4 * n
        if end > len(b):
            raise ValueError("truncated values")
        values = list(struct.unpack_from(f"<{n}f", b, i + 7))
        records.append((tag, t_ms, values))
        i = end
    return seq, records


class Device:
    def __init__(self, client):
        self.client = client
        self.replies: asyncio.Queue = asyncio.Queue()
        self.frames = []          # (arrival, seq, records)
        self.bad = 0

    async def start(self):
        await self.client.start_notify(CONTROL, lambda _h, d: self.replies.put_nowait(bytes(d)))
        await self.client.start_notify(DATA, self._on_data)

    def _on_data(self, _h, d):
        try:
            seq, recs = parse_frame(bytes(d))
            self.frames.append((time.time(), seq, recs))
        except ValueError:
            self.bad += 1

    async def command(self, payload: bytes, expect_reply=True, timeout=5.0):
        await self.client.write_gatt_char(CONTROL, payload, response=True)
        if not expect_reply:
            return None
        while True:
            r = await asyncio.wait_for(self.replies.get(), timeout)
            if r[0] == (0x80 | payload[0]):
                return r


def check(ok, text):
    print(("PASS: " if ok else "FAIL: ") + text)
    return ok


async def main(seconds: float, profile: int) -> int:
    found = await BleakScanner.discover(timeout=8, service_uuids=[SERVICE], return_adv=True)
    if not found:
        return int(not check(False, "no device advertises the SWEA service"))
    address = next(iter(found))
    ok = True
    async with BleakClient(address, timeout=20) as client:
        dev = Device(client)
        await dev.start()

        # Clock: 8 exchanges, best round trip.
        best = None
        for i in range(8):
            t0 = time.time()
            r = await dev.command(bytes([0x04, i]))
            t1 = time.time()
            dms = struct.unpack_from("<I", r, 3)[0]
            if best is None or (t1 - t0) < best[0]:
                best = (t1 - t0, (t0 + t1) / 2 - dms / 1000.0)
        ok &= check(best[0] < 1.0, f"TIME_SYNC: best round trip {best[0] * 1000:.0f} ms")
        offset = best[1]

        r = await dev.command(bytes([0x02, profile]))
        ok &= check(r[1] == 0, f"START_STREAM profile {profile}: status {r[1]}")

        await asyncio.sleep(seconds / 2)
        r = await dev.command(bytes([0x08, 42]))
        ok &= check(r[1] == 0, "MARK_EVENT 42 accepted")
        await asyncio.sleep(seconds / 2)

        # Replay the last few frames: they must come again with the same content.
        if len(dev.frames) > 5:
            want = dev.frames[-4][1]
            before = len(dev.frames)
            r = await dev.command(bytes([0x05]) + struct.pack("<H", want))
            first, flags = struct.unpack_from("<HB", r, 2)
            await asyncio.sleep(2)
            replayed = [f for f in dev.frames[before:] if f[1] == want]
            ok &= check(r[1] == 0 and first == want and not (flags & 1) and bool(replayed),
                        f"RESUME_FROM {want}: first {first}, gap flag {flags & 1}, replayed {'yes' if replayed else 'no'}")
        await dev.command(bytes([0x06]) + struct.pack("<H", dev.frames[-1][1]), expect_reply=False)

        r = await dev.command(bytes([0x03]))
        ok &= check(r[1] == 0, "STOP_STREAM")
        await asyncio.sleep(1.5)
        tail = len(dev.frames)
        await asyncio.sleep(2)
        ok &= check(len(dev.frames) == tail, "no data after STOP_STREAM")

        status = await client.read_gatt_char(STATUS)

    # ---- analysis
    frames = dev.frames
    ok &= check(dev.bad == 0, f"{len(frames)} notifications, {dev.bad} malformed")
    unique = {}
    for _t, seq, recs in frames:
        unique.setdefault(seq, recs)          # replays repeat a seq
    seqs = sorted(unique)
    gaps = sum(1 for a, b in zip(seqs, seqs[1:]) if b != a + 1)
    ok &= check(gaps == 0, f"sequence {seqs[0]}..{seqs[-1]}, gaps: {gaps}")

    per_tag = defaultdict(lambda: [0, None, None])
    markers = []
    for seq in seqs:
        for tag, t_ms, values in unique[seq]:
            e = per_tag[tag]
            e[0] += len(values)
            e[1] = t_ms if e[1] is None else min(e[1], t_ms)
            e[2] = t_ms if e[2] is None else max(e[2], t_ms)
            if tag == "UN":
                markers.extend(values)
    print("signal  samples  rate(Hz, device clock)  first value")
    for tag in sorted(per_tag):
        n, a, b = per_tag[tag]
        rate = n / ((b - a) / 1000.0) if b and a is not None and b > a else float("nan")
        first = next(v[0] for s in seqs for t, _, v in unique[s] if t == tag and v)
        print(f"  {tag:4} {n:8}  {rate:10.1f}            {first:.6g}")
    ok &= check("EA" in per_tag, "EDA (EA) present")
    ok &= check(42.0 in markers, f"marker came back as a UN record: {markers}")
    # Arrival time minus the newest sample's time in that notification (device clock mapped to UTC by TIME_SYNC).
    latency = [arr - (offset + max(t for _, t, _ in recs) / 1000.0) for arr, _s, recs in frames if recs]
    if latency:
        latency.sort()
        print(f"latency device->PC (median): {latency[len(latency) // 2] * 1000:.0f} ms")
    print(f"status after stop: {status.hex()}")
    return 0 if ok else 1


if __name__ == "__main__":
    ap = argparse.ArgumentParser()
    ap.add_argument("--seconds", type=float, default=20)
    ap.add_argument("--profile", type=int, default=0)
    a = ap.parse_args()
    sys.exit(asyncio.run(main(a.seconds, a.profile)))
