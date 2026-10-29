"""PC repro of the app's Stop drain: stream, drop the link OUT s, reconnect, then FINISH and fetch like EmotiBitSession.FinishAsync
(RESUME_FROM(first missing) every RETRY s) and report the progress."""
import asyncio, os, re, struct, subprocess, sys, threading, time
import serial
from bleak import BleakClient, BleakScanner

SERVICE = "3f5737c4-b391-44c7-bd74-f2ee561c9775"
DATA = "f14e01ab-48d0-461a-b54e-49fd7be9901a"
CONTROL = "1118282f-3dfe-44a7-a12f-6d06987f1092"
SP = os.path.dirname(os.path.abspath(__file__))
OUT_S = int(os.environ.get("OUT_S", 120)); RETRY = float(os.environ.get("RETRY", 2)); STREAM_S = 40
T0 = time.time(); passkey = {"v": None}
logf = open(os.path.join(SP, "board-drain.log"), "w", encoding="utf-8")
say = lambda m: print(f"{time.time() - T0:7.1f}s {m}", flush=True)


def serial_thread(ser):
    buf = b""
    while True:
        try: buf += ser.read(512)
        except Exception: return
        while b"\n" in buf:
            line, buf = buf.split(b"\n", 1)
            t = line.decode("utf-8", "replace").rstrip()
            m = re.search(r"label: id \S+ passkey (\d{6})", t)
            if m: passkey["v"] = m.group(1)
            t = re.sub(r"passkey:? \d{6}", "passkey ******", t, flags=re.I)
            logf.write(f"{time.time() - T0:7.1f} {t}\n"); logf.flush()


async def connect():
    found = await BleakScanner.discover(timeout=10, service_uuids=[SERVICE], return_adv=True)
    if not found: raise SystemExit("board not advertising")
    cl = BleakClient(next(iter(found)), timeout=30); await cl.connect(); return cl


class Rx:
    def __init__(self): self.seqs = set(); self.times = []; self.replies = asyncio.Queue()
    def data(self, _h, d):
        d = bytes(d)
        if len(d) >= 3: self.seqs.add(struct.unpack_from("<H", d, 1)[0]); self.times.append(time.time())
    def ctrl(self, _h, d): self.replies.put_nowait(bytes(d))


async def cmd(cl, rx, payload, timeout=8):
    await cl.write_gatt_char(CONTROL, payload, response=True)
    while True:
        r = await asyncio.wait_for(rx.replies.get(), timeout)
        if r[0] == (0x80 | payload[0]): return r


async def main():
    ser = serial.Serial("COM7", 2000000, timeout=0.2); ser.dtr = False; ser.rts = True; time.sleep(0.2); ser.rts = False
    threading.Thread(target=serial_thread, args=(ser,), daemon=True).start()
    await asyncio.sleep(18)
    env = dict(os.environ, SWEA_BLE_PASSKEY=passkey["v"])
    subprocess.run([sys.executable, "tools/ble_pair_windows.py"], env=env, capture_output=True)
    rx = Rx(); cl = await connect()
    await cl.start_notify(CONTROL, rx.ctrl); await cl.start_notify(DATA, rx.data)
    await cmd(cl, rx, bytes([0x04, 1])); await cmd(cl, rx, bytes([0x02, 0]))
    await asyncio.sleep(STREAM_S)
    first_missing = lambda: next(s for s in range(0, 70000) if s not in rx.seqs)  # contiguous prefix from 0
    last_live = max(rx.seqs); say(f"live to seq {last_live}; dropping {OUT_S}s")
    await cl.disconnect(); await asyncio.sleep(OUT_S)
    rx2 = Rx(); rx2.seqs = set(rx.seqs); rx = rx2
    cl = await connect(); await cl.start_notify(CONTROL, rx.ctrl); await cl.start_notify(DATA, rx.data)
    say("reconnected; FINISH + drain like the app")
    r = await cmd(cl, rx, bytes([0x0D])); last = struct.unpack_from("<H", r, 2)[0]; say(f"FINISH reply last_seq {last} flags {r[4]}")
    t_start = time.time(); next_req = 0; reqs = 0
    while time.time() - t_start < 300:
        if time.time() >= next_req:
            fm = min(set(range(last_live + 1, last + 1)) - rx.seqs, default=None)
            if fm is None: break
            rr = await cmd(cl, rx, bytes([0x05]) + struct.pack("<H", fm)); reqs += 1; next_req = time.time() + RETRY
        await asyncio.sleep(1)
        if int(time.time() - t_start) % 10 == 0:
            say(f"remaining {len(set(range(last_live + 1, last + 1)) - rx.seqs)}  requests {reqs}")
    left = len(set(range(last_live + 1, last + 1)) - rx.seqs)
    say(f"drain done in {time.time() - t_start:.0f}s, requests {reqs}, still missing {left} of {last - last_live}")
    try: await cmd(cl, rx, bytes([0x03])); await cl.disconnect()
    except Exception as e: say(f"stop: {e}")

asyncio.run(main())
