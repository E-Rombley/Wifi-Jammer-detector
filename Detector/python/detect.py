#!/usr/bin/env python3

import asyncio
import websockets
import numpy as np
import subprocess
import time

# ─── Config ───────────────────────────────────────────────────────────────────

ROUTER_IP       = "192.168.1.1"
SSH_KEY         = "/home/guy/.ssh/openWrt_key"
THRESHOLD_DBM   = -55
TRIGGER_SECONDS = 2
WATERFALL_MIN   = -88
WATERFALL_MAX   = -20

BANDS = {
    "2.4GHz": {
        "ws":          "ws://192.168.1.140:8073/ws/",
        "channels":    [1, 6, 11],
        "device_idx":  0,
        "current_idx": 0,
        "start_time":  None,
    },
}

# ─── Startup channel probe ───────────────────────────────────────────────────

def resolve_start_index(band: dict) -> int:
    dev = band["device_idx"]
    result = subprocess.run(
        ["ssh", "-i", SSH_KEY, f"root@{ROUTER_IP}",
         f"uci get wireless.@wifi-device[{dev}].channel"],
        capture_output=True, text=True
    )
    try:
        ch = int(result.stdout.strip())
        return band["channels"].index(ch)
    except (ValueError, IndexError):
        return 0

# ─── Channel switch ───────────────────────────────────────────────────────────

def switch_channel(band_name: str, band: dict) -> None:
    band["current_idx"] = (band["current_idx"] + 1) % len(band["channels"])
    next_ch = band["channels"][band["current_idx"]]
    prev_ch = band["channels"][(band["current_idx"] - 1) % len(band["channels"])]
    dev     = band["device_idx"]

    print(f"[!] [{band_name}] Jamming detected — switching ch{prev_ch} → ch{next_ch}")
    subprocess.run([
        "ssh", "-i", SSH_KEY,
        f"root@{ROUTER_IP}",
        f"uci set wireless.@wifi-device[{dev}].channel={next_ch} && "
        f"uci commit wireless && wifi reload"
    ])
    print(f"[+] [{band_name}] Now on channel {next_ch}.")

# ─── Per-band monitor ─────────────────────────────────────────────────────────

async def monitor_band(band_name: str, band: dict) -> None:
    print(f"[*] [{band_name}] Threshold: {THRESHOLD_DBM} dBm for {TRIGGER_SECONDS}s  "
          f"| starting ch: {band['channels'][band['current_idx']]}")

    while True:
        try:
            print(f"[*] [{band_name}] Connecting to {band['ws']} ...")
            async with websockets.connect(
                band["ws"], ping_interval=20, ping_timeout=60
            ) as ws:
                await ws.send("SERVER DE CLIENT client=openwebrx.js type=receiver")
                print(f"[*] [{band_name}] Connected. Monitoring...\n")

                while True:
                    msg = await ws.recv()

                    if not isinstance(msg, bytes) or msg[0] != 1:
                        continue

                    data = np.frombuffer(msg[1:], dtype=np.uint8)
                    dbm  = (data / 255.0) * (WATERFALL_MAX - WATERFALL_MIN) + WATERFALL_MIN
                    avg  = float(dbm.mean())

                    if avg > THRESHOLD_DBM:
                        if band["start_time"] is None:
                            band["start_time"] = time.time()
                            print(f"[!] [{band_name}] High power: {avg:.2f} dBm — timer started")
                        elif time.time() - band["start_time"] >= TRIGGER_SECONDS:
                            switch_channel(band_name, band)
                            band["start_time"] = None
                    else:
                        if band["start_time"] is not None:
                            print(f"[*] [{band_name}] Signal normal: {avg:.2f} dBm — timer reset")
                        band["start_time"] = None

        except Exception as e:
            print(f"[!] [{band_name}] Connection lost: {e} — retrying in 5s")
            band["start_time"] = None
            await asyncio.sleep(5)

# ─── Entry point ─────────────────────────────────────────────────────────────

async def main() -> None:
    for name, band in BANDS.items():
        band["current_idx"] = resolve_start_index(band)
        print(f"[*] [{name}] Router is on channel {band['channels'][band['current_idx']]}")
    await asyncio.gather(*(
        monitor_band(name, band) for name, band in BANDS.items()
    ))

if __name__ == "__main__":
    asyncio.run(main())
