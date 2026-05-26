#!/usr/bin/env python3

import asyncio
import websockets
import numpy as np
import subprocess
import time

# config
OPENWEBRX_WS = "ws://192.168.1.140:8073/ws/"
ROUTER_IP = "192.168.1.1"
SSH_KEY = "/home/guy/.ssh/openWrt_key"
THRESHOLD_DBM = -20
TRIGGER_SECONDS = 3
WATERFALL_MIN = -88
WATERFALL_MAX = -20

channels = [1, 6, 11]
current_index = 0
start_time = None


def switch_channel():
    global current_index
    current_index = (current_index + 1) % len(channels)
    next_channel = channels[current_index]
    prev_channel = channels[(current_index - 1) % len(channels)]
    print(f"[!] Jamming detected. Switching from channel {prev_channel} to channel {next_channel}...")
    subprocess.run([
        "ssh", "-i", SSH_KEY,
        f"root@{ROUTER_IP}",
        f"uci set wireless.@wifi-device[0].channel={next_channel} && uci commit wireless && wifi reload"
    ])
    print(f"[+] Now on channel {next_channel}.")


async def monitor():
    global start_time

    print(f"[*] Threshold: {THRESHOLD_DBM} dBm for {TRIGGER_SECONDS} seconds")
    print(f"[*] Current channel: {channels[current_index]}")

    while True:
        try:
            print(f"[*] Connecting to OpenWebRX...")
            async with websockets.connect(OPENWEBRX_WS, ping_interval=20, ping_timeout=60) as ws:
                await ws.send("SERVER DE CLIENT client=openwebrx.js type=receiver")
                print("[*] Connected. Monitoring 2.4GHz band...\n")

                while True:
                    msg = await ws.recv()

                    if not isinstance(msg, bytes) or msg[0] != 1:
                        continue

                    data = np.frombuffer(msg[1:], dtype=np.uint8)
                    dbm = (data / 255.0) * (WATERFALL_MAX - WATERFALL_MIN) + WATERFALL_MIN
                    avg = dbm.mean()
                   # print(f"[*] Current avg: {avg:.2f} dBm")
                    if avg > THRESHOLD_DBM:
                        if start_time is None:
                            start_time = time.time()
                            print(f"[!] High power detected: {avg:.2f} dBm. Starting timer...")
                        elif time.time() - start_time >= TRIGGER_SECONDS:
                            switch_channel()
                            start_time = None
                    else:
                        if start_time is not None:
                            print(f"[*] Signal back to normal: {avg:.2f} dBm. Resetting timer.")
                        start_time = None

        except Exception as e:
            print(f"[!] Connection lost: {e}. Reconnecting in 5 seconds...")
            start_time = None
            await asyncio.sleep(5)

if __name__ == "__main__":
    asyncio.run(monitor())