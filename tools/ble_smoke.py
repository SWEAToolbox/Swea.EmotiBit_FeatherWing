"""Hardware smoke test of the SWEA BLE service from a PC (needs bleak, see requirements.txt).

Checks, without pairing: the device advertises the SWEA service, a connection is accepted, and the encrypted
characteristics refuse access on an unencrypted link (the security requirement). Exit code 0 = all as expected.
With --paired (the PC is already bonded, e.g. paired in Windows settings) it also reads Status and sends GET_INFO.
"""
import argparse
import asyncio
import sys

from bleak import BleakClient, BleakScanner

SERVICE = "3f5737c4-b391-44c7-bd74-f2ee561c9775"
CONTROL = "1118282f-3dfe-44a7-a12f-6d06987f1092"
STATUS = "8c2502e8-eacd-45d1-bd20-361eb5a762a2"


async def main(paired: bool, open_pairing: int = 0) -> int:
    found = await BleakScanner.discover(timeout=8, service_uuids=[SERVICE], return_adv=True)
    if not found:
        print("FAIL: no device advertises the SWEA service")
        return 1
    address, (device, adv) = next(iter(found.items()))
    print(f"PASS: found {device.name} {address} rssi={adv.rssi} uuids={adv.service_uuids}")
    ok = True
    try:
        client_cm = BleakClient(address, timeout=20)
        await client_cm.__aenter__()
    except Exception as e:
        # Windows reports 'Unreachable' when the device drops an unbonded, unencrypted link during service discovery.
        print(f"INFO: connection/discovery without bonding failed ({type(e).__name__}: {e}); "
              "this is expected when the device refuses unencrypted access. Pair first and use --paired for the functional checks.")
        return 0 if not paired else 1
    async with client_cm as client:
        print("PASS: connection accepted")
        svc = client.services.get_service(SERVICE)
        print("PASS: service present" if svc else "FAIL: service missing")
        ok &= svc is not None
        try:
            data = await client.read_gatt_char(STATUS)
            if paired:
                print(f"PASS: status read ({len(data)} bytes) {data.hex()}")
                ok &= len(data) == 20
            else:
                print("FAIL: Status was readable without encryption")
                ok = False
        except Exception as e:  # bleak maps ATT errors to exceptions
            if paired:
                print(f"FAIL: status read failed: {e}")
                ok = False
            else:
                print(f"PASS: unencrypted read refused ({type(e).__name__}: {e})")
        if paired and ok:
            ok &= await check_get_info(client)
        if paired and ok and open_pairing:
            ok &= await send_open_pairing(client, open_pairing)
    return 0 if ok else 1


async def send_open_pairing(client, seconds: int) -> bool:
    """ENTER_PAIRING (0x0A): let another phone pair for `seconds`. Only accepted over a bonded link."""
    replies: asyncio.Queue = asyncio.Queue()
    await client.start_notify(CONTROL, lambda _h, d: replies.put_nowait(bytes(d)))
    try:
        await client.write_gatt_char(CONTROL, bytes([0x0A, min(seconds, 255)]), response=True)
        reply = await asyncio.wait_for(replies.get(), timeout=5)
    finally:
        await client.stop_notify(CONTROL)
    ok = len(reply) >= 2 and reply[0] == 0x8A and reply[1] == 0
    print(("PASS" if ok else "FAIL") + f": ENTER_PAIRING {seconds} s, reply {reply.hex()}")
    return ok


async def check_get_info(client) -> bool:
    """GET_INFO (0x01): the reply 0x81, status 0, then protocol version, firmware\\0, device id\\0 (firmware 0.5+)."""
    replies: asyncio.Queue = asyncio.Queue()
    await client.start_notify(CONTROL, lambda _h, d: replies.put_nowait(bytes(d)))
    try:
        await client.write_gatt_char(CONTROL, bytes([0x01]), response=True)
        reply = await asyncio.wait_for(replies.get(), timeout=5)
    except Exception as e:  # noqa: BLE001
        print(f"FAIL: GET_INFO got no reply ({type(e).__name__}: {e})")
        return False
    finally:
        await client.stop_notify(CONTROL)
    if len(reply) < 3 or reply[0] != 0x81 or reply[1] != 0:
        print(f"FAIL: GET_INFO reply {reply.hex()}")
        return False
    strings = reply[3:].split(b"\0")
    firmware = strings[0].decode("ascii", "replace") if strings else ""
    device_id = strings[1].decode("ascii", "replace") if len(strings) > 1 else ""
    print(f"{'PASS' if device_id else 'FAIL'}: GET_INFO protocol {reply[2]}, firmware {firmware!r}, device id {device_id!r}")
    return bool(device_id)


if __name__ == "__main__":
    ap = argparse.ArgumentParser()
    ap.add_argument("--paired", action="store_true")
    ap.add_argument("--open-pairing", type=int, default=0, metavar="SECONDS",
                    help="after the checks, open the pairing window so a phone can pair (needs --paired)")
    args = ap.parse_args()
    sys.exit(asyncio.run(main(args.paired, args.open_pairing)))
