#!/usr/bin/env python3

import asyncio
import websockets
import numpy as np
import subprocess
import time

# ─── Config ───────────────────────────────────────────────────────────────────

ROUTER_IP       = "192.168.1.1"
SSH_KEY         = "/home/guy/.ssh/openWrt_key"
THRESHOLD_DBM   = -37
TRIGGER_SECONDS = 0.5
WATERFALL_MIN   = -88
WATERFALL_MAX   = -22
MONITOR_BW_HZ   = 3.5e6  # monitor ±3.5 MHz around the channel centre

BANDS = {
    "2.4GHz": {
        "ws":          "ws://192.168.1.140:8073/ws/",
        "channels":    [1, 6, 11],
        "device_idx":  0,
        "current_idx": 0,
        "start_time":  None,
        "samp_rate":   20e6,      # Hz — 20 MS/s = 20 MHz window
        "channel_freqs": {        # standard 2.4 GHz channel centres
            1:  2412e6,
            6:  2437e6,
            11: 2462e6,
        },
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

    next_freq_mhz = band["channel_freqs"][next_ch] / 1e6
    print(f"[!] [{band_name}] Jamming detected — switching ch{prev_ch} → ch{next_ch} ({next_freq_mhz:.0f} MHz)")
    subprocess.run([
        "ssh", "-i", SSH_KEY,
        f"root@{ROUTER_IP}",
        f"uci set wireless.@wifi-device[{dev}].channel={next_ch} && "
        f"uci commit wireless && wifi reload"
    ])
    print(f"[+] [{band_name}] Now on channel {next_ch} — switch OpenWebRX to {next_freq_mhz:.0f} MHz profile")

# ─── Per-band monitor ─────────────────────────────────────────────────────────

async def monitor_band(band_name: str, band: dict) -> None:
    print(f"[*] [{band_name}] Threshold: {THRESHOLD_DBM} dBm for {TRIGGER_SECONDS}s  "
          f"| starting ch: {band['channels'][band['current_idx']]}")
    last_print = 0

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

                    data      = np.frombuffer(msg[1:], dtype=np.uint8)
                    n         = len(data)
                    ch        = band["channels"][band["current_idx"]]
                    cf        = band["channel_freqs"][ch]
                    hz_per_bin = band["samp_rate"] / n
                    band_start = cf - band["samp_rate"] / 2
                    lo = max(0, int((cf - MONITOR_BW_HZ - band_start) / hz_per_bin))
                    hi = min(n, int((cf + MONITOR_BW_HZ - band_start) / hz_per_bin))
                    if lo >= hi:
                        continue
                    dbm   = (data[lo:hi] / 255.0) * (WATERFALL_MAX - WATERFALL_MIN) + WATERFALL_MIN
                    level = float(np.percentile(dbm, 90))

                    now = time.time()
                    if now - last_print >= 2:
                        ch       = band["channels"][band["current_idx"]]
                        ch_freq  = band["channel_freqs"][ch] / 1e6
                        print(f"[~] [{band_name}] ch{ch} ({ch_freq:.0f} MHz)  p90: {level:.2f} dBm  (threshold: {THRESHOLD_DBM})")
                        last_print = now

                    if level > THRESHOLD_DBM:
                        if band["start_time"] is None:
                            band["start_time"] = time.time()
                            print(f"[!] [{band_name}] High power: {level:.2f} dBm — timer started")
                        elif time.time() - band["start_time"] >= TRIGGER_SECONDS:
                            switch_channel(band_name, band)
                            band["start_time"] = None
                    else:
                        if band["start_time"] is not None:
                            print(f"[*] [{band_name}] Signal normal: {level:.2f} dBm — timer reset")
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