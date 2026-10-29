"""PC-only repro: stream, drop the link for OUT seconds while the session stays active, reconnect, RESUME_FROM.
Board serial goes to board-pc.log (passkey redacted). Run from SWEA.EmotiBit_FeatherWing with .venv-tools python."""
import asyncio, os, re, struct, subprocess, sys, threading, time
import serial
from bleak import BleakClient, BleakScanner

SERVICE = "3f5737c4-b391-44c7-bd74-f2ee561c9775"
DATA = "f14e01ab-48d0-461a-b54e-49fd7be9901a"
CONTROL = "1118282f-3dfe-44a7-a12f-6d06987f1092"
STATUS = "8c2502e8-eacd-45d1-bd20-361eb5a762a2"
SP = os.path.dirname(os.path.abspath(__file__))
PORT, STREAM_S, OUT_S, RESUME_S = "COM7", int(os.environ.get("STREAM_S", 60)), int(os.environ.get("OUT_S", 180)), int(os.environ.get("RESUME_S", 120))
T0 = time.time()
passkey = {"v": None}
logf = open(os.path.join(SP, "board-pc.log"), "w", encoding="utf-8")


def ts():
    return f"{time.time() - T0:7.1f}s"


def say(msg):
    print(f"{ts()} {msg}", flush=True)


def serial_thread(ser):
    buf = b""
    while True:
        try:
            buf += ser.read(512)
        except Exception as e:
            logf.write(f"{ts()} SERIAL ERROR {e}\n"); logf.flush(); return
        while b"\n" in buf:
            line, buf = buf.split(b"\n", 1)
            text = line.decode("utf-8", "replace").rstrip()
            m = re.search(r"label: id \S+ passkey (\d{6})", text)
            if m:
                passkey["v"] = m.group(1)
            text = re.sub(r"[Pp]asskey:? \d{6}", "passkey ******", text)
            text = re.sub(r"passkey \d{6}", "passkey ******", text)
            logf.write(f"{ts()} {text}\n"); logf.flush()


def reset_board():
    ser = serial.Serial(PORT, 2000000, timeout=0.2)
    ser.dtr = False; ser.rts = True; time.sleep(0.2); ser.rts = False
    threading.Thread(target=serial_thread, args=(ser,), daemon=True).start()
    return ser


async def connect():
    found = await BleakScanner.discover(timeout=10, service_uuids=[SERVICE], return_adv=True)
    if not found:
        raise SystemExit("board not advertising")
    addr = next(iter(found))
    drops = []
    cl = BleakClient(addr, timeout=30, disconnected_callback=lambda c: (drops.append(time.time()), say("*** LINK DROPPED")))
    await cl.connect()
    return cl, drops


class Rx:
    def __init__(self): self.frames = []; self.replies = asyncio.Queue()
    def data(self, _h, d):
        d = bytes(d)
        if len(d) >= 3: self.frames.append((time.time(), struct.unpack_from("<H", d, 1)[0], len(d)))
    def ctrl(self, _h, d): self.replies.put_nowait(bytes(d))


async def cmd(cl, rx, payload, timeout=8):
    await cl.write_gatt_char(CONTROL, payload, response=True)
    while True:
        r = await asyncio.wait_for(rx.replies.get(), timeout)
        if r[0] == (0x80 | payload[0]): return r


def summarise(rx, since, label):
    fr = [f for f in rx.frames if f[0] >= since]
    if not fr:
        say(f"{label}: no data frames"); return
    seqs = [f[1] for f in fr]
    gaps = sum(1 for a, b in zip(seqs, seqs[1:]) if b != (a + 1) & 0xFFFF and b != a)
    worst = max((b[0] - a[0] for a, b in zip(fr, fr[1:])), default=0)
    jumps=[(a,b) for a,b in zip(seqs,seqs[1:]) if b!=(a+1)&0xFFFF and b!=a]
    say(f"{label}: jumps {jumps[:8]} first-seqs {seqs[:3]}")
    say(f"{label}: {len(fr)} frames, seq {seqs[0]}..{seqs[-1]}, seq jumps {gaps}, longest silence {worst:.2f}s, rate {len(fr)/max(fr[-1][0]-fr[0][0],1e-3):.1f}/s")


async def watch(cl, rx, secs, label):
    start = time.time(); last_n = 0
    while time.time() - start < secs and cl.is_connected:
        await asyncio.sleep(10)
        n = len(rx.frames)
        say(f"{label}: +{n - last_n} frames in 10s, connected={cl.is_connected}"); last_n = n
    summarise(rx, start, label)


async def main():
    ser = reset_board()
    await asyncio.sleep(18)
    if not passkey["v"]:
        raise SystemExit("no passkey line seen (board asleep?)")
    env = dict(os.environ, SWEA_BLE_PASSKEY=passkey["v"])
    r = subprocess.run([sys.executable, os.path.join(os.path.dirname(SP), "x")] if False else
                       [sys.executable, "tools/ble_pair_windows.py"], env=env, capture_output=True, text=True)
    say("pair: " + (r.stdout.strip().splitlines() or ["?"])[-1])
    rx = Rx()
    cl, drops = await connect()
    await cl.start_notify(CONTROL, rx.ctrl); await cl.start_notify(DATA, rx.data)
    say("connected, status " + (await cl.read_gatt_char(STATUS)).hex())
    await cmd(cl, rx, bytes([0x04, 1])); r = await cmd(cl, rx, bytes([0x02, 0]))
    say(f"START_STREAM status {r[1]}")
    t_a = time.time(); await watch(cl, rx, STREAM_S, "A live")
    last = rx.frames[-1][1] if rx.frames else 0
    say(f"disconnecting for {OUT_S}s (session stays active), last seq {last}")
    try: await cl.disconnect()
    except Exception as e: say(f"disconnect: {e}")
    await asyncio.sleep(OUT_S)
    say("reconnecting")
    rx2 = Rx()
    cl, drops = await connect()
    await cl.start_notify(CONTROL, rx2.ctrl); await cl.start_notify(DATA, rx2.data)
    st = await cl.read_gatt_char(STATUS); say("status after reconnect " + st.hex())
    t_c = time.time()
    r = await cmd(cl, rx2, bytes([0x05]) + struct.pack("<H", (last + 1) & 0xFFFF))
    say("RESUME_FROM reply " + r.hex())
    await watch(cl, rx2, RESUME_S, "C resume")
    say(f"link drops after reconnect: {len([d for d in drops if d >= t_c])}")
    if cl.is_connected:
        try: await cmd(cl, rx2, bytes([0x03])); say("STOP_STREAM sent"); await cl.disconnect()
        except Exception as e: say(f"stop: {e}")
    ser.close()

asyncio.run(main())
