"""Pair (bond) this Windows PC with the EmotiBit using its passkey, so that ble_smoke.py --paired can read it.

Windows only: bleak cannot enter a passkey, so this uses the Windows pairing API directly.
The EmotiBit must be in its pairing window (120 s after a boot without bonds, or after a short button press);
its passkey is printed on USB serial (`emotibit.py monitor --reset`). Pass it with --passkey or the environment
variable SWEA_BLE_PASSKEY; it is not stored.

    .venv-tools/Scripts/python tools/ble_pair_windows.py --passkey 123456
    .venv-tools/Scripts/python tools/ble_pair_windows.py --unpair
"""
import argparse
import asyncio
import os
import sys

from bleak import BleakScanner
from winrt.windows.devices.bluetooth import BluetoothLEDevice
from winrt.windows.devices.enumeration import (
    DevicePairingKinds,
    DevicePairingProtectionLevel,
    DevicePairingResultStatus,
)

SERVICE = "3f5737c4-b391-44c7-bd74-f2ee561c9775"


async def find_device():
    found = await BleakScanner.discover(timeout=8, service_uuids=[SERVICE], return_adv=True)
    if not found:
        return None, None
    address, (device, _adv) = next(iter(found.items()))
    winrt_device = await BluetoothLEDevice.from_bluetooth_address_async(int(address.replace(":", ""), 16))
    return device, winrt_device


async def main(passkey, unpair: bool) -> int:
    device, le = await find_device()
    if le is None:
        print("FAIL: no device advertises the SWEA service (is the EmotiBit on and in range?)")
        return 1
    pairing = le.device_information.pairing
    print(f"found {device.name} {device.address}, paired: {pairing.is_paired}")

    if unpair:
        result = await pairing.unpair_async()
        print(f"unpair: {result.status}")
        return 0

    if pairing.is_paired:
        print("PASS: already paired")
        return 0
    if passkey is None:
        print("FAIL: give the passkey with --passkey or SWEA_BLE_PASSKEY")
        return 1

    def on_request(_sender, args):
        if args.pairing_kind == DevicePairingKinds.PROVIDE_PIN:
            args.accept(f"{passkey:06d}")
        else:   # confirm-only / pin match: accept, the passkey was already checked by the device
            args.accept()

    custom = pairing.custom
    token = custom.add_pairing_requested(on_request)
    try:
        kinds = (DevicePairingKinds.PROVIDE_PIN | DevicePairingKinds.CONFIRM_ONLY
                 | DevicePairingKinds.CONFIRM_PIN_MATCH | DevicePairingKinds.DISPLAY_PIN)
        result = await custom.pair_with_protection_level_async(kinds, DevicePairingProtectionLevel.ENCRYPTION_AND_AUTHENTICATION)
    finally:
        custom.remove_pairing_requested(token)

    ok = result.status in (DevicePairingResultStatus.PAIRED, DevicePairingResultStatus.ALREADY_PAIRED)
    print(("PASS" if ok else "FAIL") + f": pairing {DevicePairingResultStatus(result.status).name}, "
          f"protection {DevicePairingProtectionLevel(result.protection_level_used).name}")
    return 0 if ok else 1


if __name__ == "__main__":
    ap = argparse.ArgumentParser()
    ap.add_argument("--passkey", type=int, default=int(os.environ["SWEA_BLE_PASSKEY"]) if os.environ.get("SWEA_BLE_PASSKEY") else None)
    ap.add_argument("--unpair", action="store_true")
    a = ap.parse_args()
    sys.exit(asyncio.run(main(a.passkey, a.unpair)))
