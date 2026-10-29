#!/usr/bin/env python3
"""SWEA EmotiBit tool: check, ports, backup, build, flash, restore, monitor, label, scan.

One cross-platform script (Windows, Linux) around esptool, PlatformIO and pyserial.
Set up its Python environment first with tools/setup.ps1 (Windows) or tools/setup.sh (Linux), then run it with
the Python of that environment, for example:

    .venv-tools/Scripts/python tools/emotibit.py ports            (Windows)
    .venv-tools/bin/python tools/emotibit.py ports                (Linux)

Safety rules built in:
  * `flash` refuses to run unless a full-flash backup of THAT board (matched by MAC address) exists.
  * `restore` checks the SHA-256 of the image before writing.
  * `monitor` hides Wi-Fi passwords that the stock EmotiBit firmware prints in clear text.
Documentation: SWEA.Documentation/docfx/articles/emotibit/flashning.md
"""
from __future__ import annotations

import argparse
import datetime
import hashlib
import json
import os
import pathlib
import platform
import re
import shutil
import subprocess
import sys
import time

ROOT = pathlib.Path(__file__).resolve().parents[1]
SKETCH = ROOT / "EmotiBit_swea_ble_firmware"
DEFAULT_ENV = "swea_ble_esp32"
STOCK_ENV = "adafruit_feather_esp32"
FLASH_SIZE = 0x400000  # Adafruit Feather HUZZAH32: 4 MB
IS_WINDOWS = platform.system() == "Windows"
CP210X = (0x10C4, 0xEA60)
# Stock EmotiBit firmware prints "Adding SSID: x -pass:secret" on Serial.
REDACT = re.compile(r"(-pass:|[Pp]asskey:? )\d*\S*")


def backups_root() -> pathlib.Path:
    return pathlib.Path(os.environ.get("EMOTIBIT_BACKUPS", ROOT.parent / "EmotiBit-backups"))


def pio_env() -> dict:
    """Environment for PlatformIO. On Windows a short core dir and git long paths avoid MAX_PATH failures."""
    env = os.environ.copy()
    if IS_WINDOWS:
        env.setdefault("PLATFORMIO_CORE_DIR", os.environ.get("EMOTIBIT_PIO_HOME", "C:/pio"))
        if "GIT_CONFIG_COUNT" not in env:
            env.update(GIT_CONFIG_COUNT="1", GIT_CONFIG_KEY_0="core.longpaths", GIT_CONFIG_VALUE_0="true")
    env.setdefault("PYTHONIOENCODING", "utf-8")
    return env


def run(cmd: list, cwd=None, env=None, capture=False, quiet_progress=True):
    print("$", " ".join(str(c) for c in cmd), flush=True)
    if capture:
        return subprocess.run(cmd, cwd=cwd, env=env, text=True, capture_output=True, encoding="utf-8", errors="replace")
    if not quiet_progress:
        return subprocess.run(cmd, cwd=cwd, env=env)
    # esptool prints a progress line for every 4 KB read; collapse them.
    p = subprocess.Popen(cmd, cwd=cwd, env=env, stdout=subprocess.PIPE, stderr=subprocess.STDOUT, text=True,
                         encoding="utf-8", errors="replace")
    last = ""
    for line in p.stdout:
        for part in line.replace("\r", "\n").split("\n"):
            part = part.strip()
            if not part or re.match(r"^(\d+ \(\d+ ?%\))+$", part):
                continue
            if part.startswith("Writing at ") and last.startswith("Writing at "):
                continue
            print(part, flush=True)
            last = part
    return p.wait()


def esptool(port: str, baud: int, *args) -> list:
    return [sys.executable, "-m", "esptool", "--chip", "esp32", "--port", port, "--baud", str(baud), *map(str, args)]


def need(module: str, hint: str):
    try:
        __import__(module)
    except ImportError:
        sys.exit(f"Missing Python package '{module}'. {hint}")


def read_mac(port: str) -> str:
    r = run(esptool(port, 115200, "read_mac"), capture=True)
    m = re.search(r"MAC:\s*([0-9a-f:]{17})", r.stdout + r.stderr, re.I)
    if not m:
        sys.exit("Could not read the MAC address (is the board connected, HIB switch on, correct port?):\n"
                 + r.stdout[-800:] + r.stderr[-800:])
    return m.group(1).lower()


def sha256(path: pathlib.Path) -> str:
    h = hashlib.sha256()
    with open(path, "rb") as f:
        for chunk in iter(lambda: f.read(1 << 20), b""):
            h.update(chunk)
    return h.hexdigest()


def find_backup(mac: str):
    """Return (folder, info) of a backup whose info.json has this MAC, or None."""
    root = backups_root()
    if not root.exists():
        return None
    for info_file in sorted(root.glob("*/info.json")):
        try:
            info = json.loads(info_file.read_text(encoding="utf-8"))
        except (OSError, ValueError):
            continue
        if info.get("mac", "").lower() == mac and (info_file.parent / info.get("image", "flash_full_4MB.bin")).exists():
            return info_file.parent, info
    return None


def partition_summary(image: pathlib.Path) -> list:
    import struct
    data = image.read_bytes()
    out = []
    tbl = data[0x8000:0x8C00]
    for i in range(0, len(tbl), 32):
        e = tbl[i:i + 32]
        if e[:2] != b"\xaa\x50":
            break
        off, size = struct.unpack("<II", e[4:12])
        name = e[12:28].split(b"\x00")[0].decode(errors="replace")
        out.append(f"{name:<8} 0x{off:06x} {size // 1024:>5} KB")
    return out


# ---------------------------------------------------------------- commands
def cmd_check(args):
    ok = True
    print(f"OS: {platform.system()} {platform.release()}  Python {platform.python_version()} ({sys.executable})")
    for mod, name in (("esptool", "esptool"), ("serial", "pyserial"), ("platformio", "PlatformIO"), ("bleak", "bleak (scan only)")):
        try:
            m = __import__(mod)
            print(f"  {name:<20} {getattr(m, '__version__', 'installed')}")
        except ImportError:
            print(f"  {name:<20} MISSING  -> run tools/setup")
            ok = ok and mod == "bleak"
    print("  git                  " + (shutil.which("git") or "MISSING"))
    ok = ok and shutil.which("git") is not None
    if IS_WINDOWS:
        try:
            import winreg
            k = winreg.OpenKey(winreg.HKEY_LOCAL_MACHINE, r"SYSTEM\CurrentControlSet\Control\FileSystem")
            lp = winreg.QueryValueEx(k, "LongPathsEnabled")[0]
        except OSError:
            lp = 0
        print(f"  Windows long paths   {'enabled' if lp else 'off (the script uses a short PlatformIO dir instead)'}")
        print(f"  PlatformIO core dir  {pio_env()['PLATFORMIO_CORE_DIR']}")
    else:
        import grp
        groups = [grp.getgrgid(g).gr_name for g in os.getgroups()]
        serial_groups = {"dialout", "uucp", "plugdev"} & set(groups)
        print(f"  serial port access   {'OK, in group ' + ', '.join(sorted(serial_groups)) if serial_groups else 'NOT in dialout/uucp: add yourself and log in again'}")
        if shutil.which("brltty") or pathlib.Path("/usr/lib/udev/rules.d/85-brltty.rules").exists():
            print("  brltty               installed: it can grab CP210x adapters (sudo apt remove brltty)")
    print(f"  backups folder       {backups_root()}")
    cmd_ports(args)
    return 0 if ok else 1


def cmd_ports(args):
    need("serial", "Run tools/setup first.")
    from serial.tools import list_ports
    found = False
    for p in sorted(list_ports.comports(), key=lambda x: x.device):
        tag = ""
        if (p.vid, p.pid) == CP210X:
            tag = "   <-- CP210x (EmotiBit / Feather HUZZAH32)"
            found = True
        vp = f"{p.vid:04X}:{p.pid:04X}" if p.vid else "----:----"
        print(f"  {p.device:<14} {vp}  {p.description}{tag}")
    if not found:
        print("No CP210x adapter found. Check: data cable, HIB switch ON, driver (Windows) or dialout group (Linux).")
    return 0


def cmd_backup(args):
    mac = read_mac(args.port)
    name = args.name or mac.replace(":", "-")
    out = pathlib.Path(args.out) if args.out else backups_root() / name
    image = out / "flash_full_4MB.bin"
    if image.exists() and not args.force:
        sys.exit(f"{image} already exists. A backup of the original firmware must never be overwritten; use --name or --force.")
    out.mkdir(parents=True, exist_ok=True)
    rc = run(esptool(args.port, args.baud, "read_flash", 0, FLASH_SIZE, image))
    if rc != 0:
        sys.exit("read_flash failed")
    digest = sha256(image)
    (out / "SHA256SUMS.txt").write_text(f"{digest}  {image.name}\n", encoding="utf-8", newline="\n")
    info = {"mac": mac, "chip": "ESP32", "flash_bytes": FLASH_SIZE, "image": image.name, "sha256": digest,
            "created": datetime.datetime.now().isoformat(timespec="seconds"), "note": args.note or ""}
    (out / "info.json").write_text(json.dumps(info, indent=2), encoding="utf-8", newline="\n")
    print(f"\nBackup written to {out}\n  sha256 {digest}")
    for line in partition_summary(image):
        print("  partition", line)
    print("The NVS partition (0x9000-0xE000) is rewritten by the running firmware, so a second read can differ there. That is normal.")
    return 0


def cmd_build(args):
    rc = run([sys.executable, "-m", "platformio", "run", "-e", args.env], cwd=SKETCH, env=pio_env(), quiet_progress=False)
    return rc.returncode


def cmd_flash(args):
    mac = read_mac(args.port)
    print(f"Board MAC {mac}")
    if not args.skip_backup_check:
        b = find_backup(mac)
        if not b:
            sys.exit(f"No backup of this board (MAC {mac}) in {backups_root()}.\n"
                     f"Take one first:  emotibit.py backup --port {args.port}\n"
                     "(or pass --skip-backup-check if you accept that the original firmware is lost)")
        print(f"Backup found: {b[0]}  ({b[1].get('created', '?')})")
    rc = run([sys.executable, "-m", "platformio", "run", "-e", args.env, "-t", "upload", "--upload-port", args.port],
             cwd=SKETCH, env=pio_env(), quiet_progress=False)
    if rc.returncode != 0:
        return rc.returncode
    if args.monitor:
        return cmd_monitor(argparse.Namespace(port=args.port, baud=2000000, seconds=args.monitor, reset=True, raw=False, grep=None))
    return 0


def cmd_restore(args):
    mac = read_mac(args.port)
    if args.image:
        image = pathlib.Path(args.image)
        expected = None
        sums = image.parent / "SHA256SUMS.txt"
        if sums.exists():
            expected = sums.read_text(encoding="utf-8").split()[0].lower()
    else:
        b = find_backup(mac)
        if not b:
            sys.exit(f"No backup for MAC {mac} in {backups_root()}. Pass --image.")
        image, expected = b[0] / b[1]["image"], b[1]["sha256"].lower()
    actual = sha256(image)
    if expected and actual != expected:
        sys.exit(f"Checksum mismatch for {image}: {actual} != {expected}. Not writing a damaged image.")
    print(f"Image {image}  sha256 {actual}  {'(checked)' if expected else '(no checksum file to compare)'}")
    if args.dry_run:
        print("--dry-run: nothing written.")
        return 0
    rc = run(esptool(args.port, args.baud, "write_flash", 0, image))   # esptool verifies with MD5 after writing
    if rc != 0:
        sys.exit("write_flash failed")
    print("Restored. Press Reset on the Feather. Expected serial output: 'Firmware version: 1.14.0' for the original stock firmware.")
    return 0


def cmd_monitor(args):
    need("serial", "Run tools/setup first.")
    import serial
    # Open with DTR and RTS released: on the Feather they drive EN/IO0, and asserting them while opening restarts
    # the board (which also closes an open pairing window). Only --reset restarts it on purpose.
    s = serial.Serial()
    s.port, s.baudrate, s.timeout = args.port, args.baud, 0.2
    s.dtr = False
    s.rts = False
    try:
        s.open()
    except serial.SerialException as e:
        sys.exit(f"Cannot open {args.port}: {e}")
    if args.reset:
        s.setDTR(False)
        s.setRTS(True)
        time.sleep(0.15)
        s.setRTS(False)
    end = time.time() + args.seconds if args.seconds else None
    buf = b""
    try:
        while end is None or time.time() < end:
            buf += s.read(4096)
            while b"\n" in buf:
                line, buf = buf.split(b"\n", 1)
                text = line.decode("utf-8", errors="replace").replace("\x00", "").rstrip("\r")
                if not text.strip():
                    continue
                if not args.raw:
                    text = REDACT.sub(r"\1***", text)
                if args.grep and not re.search(args.grep, text):
                    continue
                print(text, flush=True)
    except KeyboardInterrupt:
        pass
    finally:
        s.close()
    return 0


LABEL_LINE = re.compile(r"SWEA BLE label: id (\S+) passkey (\d{6})")


def read_label_info(port: str, seconds: int = 25):
    """Restart the board and read its device id and passkey from the boot log (firmware 0.8+). Nothing is printed."""
    import serial
    try:
        s = serial.Serial(port, 2000000, timeout=0.2)
    except serial.SerialException as e:
        sys.exit(f"Cannot open {port}: {e}")
    s.setDTR(False)
    s.setRTS(True)
    time.sleep(0.15)
    s.setRTS(False)
    end, buf = time.time() + seconds, b""
    try:
        while time.time() < end:
            buf += s.read(4096)
            m = LABEL_LINE.search(buf.decode("utf-8", errors="replace"))
            if m and m.group(1) != "?":
                return m.group(1), m.group(2)
    finally:
        s.close()
    sys.exit("No 'SWEA BLE label' line in the boot log. Is SWEA BLE firmware 0.8 or later on the board?")


def label_svg(qr_svg_body: str, device_id: str) -> str:
    """A small label: the QR code, the advertised name, the id and how to open pairing. The passkey is only inside the QR."""
    name = "EmotiBit-" + device_id[-4:]
    w, h = 300, 140
    return f"""<svg xmlns="http://www.w3.org/2000/svg" width="{w}" height="{h}" viewBox="0 0 {w} {h}">
  <rect width="{w}" height="{h}" fill="#fff" stroke="#111827" stroke-width="1" rx="8"/>
  {qr_svg_body.replace("<svg ", '<svg x="10" y="10" width="120" height="120" ', 1)}
  <text x="142" y="40" font-family="Inter,Arial,sans-serif" font-size="18" font-weight="700" fill="#111827">{name}</text>
  <text x="142" y="62" font-family="Inter,Arial,sans-serif" font-size="12" fill="#4b5563">{device_id}</text>
  <text x="142" y="92" font-family="Inter,Arial,sans-serif" font-size="11" fill="#4b5563">Skanna i SWEA Toolbox.</text>
  <text x="142" y="108" font-family="Inter,Arial,sans-serif" font-size="11" fill="#4b5563">Para: tryck kort på knappen,</text>
  <text x="142" y="122" font-family="Inter,Arial,sans-serif" font-size="11" fill="#4b5563">lampan blinkar i 2 minuter.</text>
</svg>
"""


def cmd_label(args):
    """Make the pairing label for a board: a QR code with its id and passkey that SWEA Toolbox scans."""
    need("serial", "Run tools/setup first.")
    need("segno", "Run tools/setup again (segno is in requirements.txt).")
    import io, json, segno
    device_id, passkey = read_label_info(args.port)
    payload = json.dumps({"t": "emotibit", "sn": device_id, "pk": passkey}, separators=(",", ":"))
    qr = segno.make(payload, error="m")
    out = pathlib.Path(args.out) if args.out else backups_root() / device_id / "label"
    out.mkdir(parents=True, exist_ok=True)
    qr.save(out / "qr.png", scale=8, border=2)
    buf = io.BytesIO()
    qr.save(buf, kind="svg", xmldecl=False, svgns=False, border=0, omitsize=True)
    body = buf.getvalue().decode("utf-8")
    (out / "label.svg").write_text(label_svg(body, device_id), encoding="utf-8")
    print(f"Label for {device_id} written to {out} (label.svg, qr.png).")
    print("The QR code contains the passkey: keep these files private (they are not in git) and stick the label on the device.")
    return 0


def cmd_scan(args):
    need("bleak", "Install it with: pip install bleak")
    import asyncio
    from bleak import BleakScanner
    service = "3f5737c4-b391-44c7-bd74-f2ee561c9775"

    async def go():
        seen = {}

        def cb(dev, adv):
            seen[dev.address] = (adv.local_name or dev.name or "", adv.rssi, [u.lower() for u in adv.service_uuids])
        async with BleakScanner(cb):
            await asyncio.sleep(args.seconds)
        hits = {a: v for a, v in seen.items() if service in v[2] or v[0].startswith("EmotiBit")}
        print(f"{len(seen)} BLE devices seen, {len(hits)} EmotiBit")
        for a, (n, r, u) in hits.items():
            print(f"  {a}  {n}  rssi {r} dBm  SWEA service: {'yes' if service in u else 'no'}")
        return 0 if hits else 1
    return asyncio.run(go())


def main():
    ap = argparse.ArgumentParser(description=__doc__, formatter_class=argparse.RawDescriptionHelpFormatter)
    sub = ap.add_subparsers(dest="cmd", required=True)

    def add(name, fn, help_, port=True):
        p = sub.add_parser(name, help=help_)
        if port:
            p.add_argument("--port", required=True, help="serial port, e.g. COM7 or /dev/ttyUSB0")
        p.set_defaults(fn=fn)
        return p

    add("check", cmd_check, "check requirements on this computer", port=False)
    add("ports", cmd_ports, "list serial ports", port=False)
    p = add("backup", cmd_backup, "read the full 4 MB flash to a backup folder")
    p.add_argument("--baud", type=int, default=921600)
    p.add_argument("--name", help="folder name (default: the board's MAC address)")
    p.add_argument("--out", help="explicit output folder")
    p.add_argument("--note", help="free text stored in info.json")
    p.add_argument("--force", action="store_true", help="overwrite an existing image (never for an original backup)")
    p = add("build", cmd_build, "compile the firmware", port=False)
    p.add_argument("--env", default=DEFAULT_ENV, choices=[DEFAULT_ENV, STOCK_ENV])
    p = add("flash", cmd_flash, "build and upload the firmware (needs a backup of this board)")
    p.add_argument("--env", default=DEFAULT_ENV, choices=[DEFAULT_ENV, STOCK_ENV])
    p.add_argument("--monitor", type=int, default=0, metavar="SECONDS", help="show the serial log after flashing")
    p.add_argument("--skip-backup-check", action="store_true")
    p = add("restore", cmd_restore, "write a full-flash backup back to the board")
    p.add_argument("--image", help="image to write (default: the backup of this board)")
    p.add_argument("--baud", type=int, default=921600)
    p.add_argument("--dry-run", action="store_true", help="only check the image")
    p = add("monitor", cmd_monitor, "show the serial log (Wi-Fi passwords are hidden)")
    p.add_argument("--baud", type=int, default=2000000)
    p.add_argument("--seconds", type=int, default=0, help="stop after N seconds (default: until Ctrl+C)")
    p.add_argument("--reset", action="store_true", help="restart the board first to see the whole boot log")
    p.add_argument("--raw", action="store_true", help="do not hide passwords")
    p.add_argument("--grep", help="only show lines matching this regex")
    p = add("label", cmd_label, "make the pairing label: QR code with the device id and passkey (firmware 0.8+)")
    p.add_argument("--out", help="output folder (default: EmotiBit-backups/<device id>/label)")
    p = add("scan", cmd_scan, "scan for the EmotiBit BLE advertisement", port=False)
    p.add_argument("--seconds", type=int, default=10)

    args = ap.parse_args()
    sys.exit(args.fn(args))


if __name__ == "__main__":
    main()
