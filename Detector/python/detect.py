#!/usr/bin/env python3

import subprocess
import time

# config
ROUTER_IP = "192.168.1.1"
SSH_KEY = "/home/guy/.ssh/openWrt_key"
THRESHOLD_DBM = -20
TRIGGER_SECONDS = 3
channels = [1, 6, 11]
current_index = 0

start_time = None


def switch_channel(channel):
    print(f"[!] Jamming detected. Switching to channel {channel}...")
    subprocess.run([
        "ssh", "-i", SSH_KEY,
        f"root@{ROUTER_IP}",
        f"uci set wireless.@wifi-device[0].channel={channel} && uci commit wireless && wifi reload"
    ])
    print(f"[+] Switched to channel {channel}.")


def parse_average(line):
    parts = line.strip().split(",")
    if len(parts) < 7:
        return None
    try:
        values = [float(x) for x in parts[6:]]
        return sum(values) / len(values)
    except ValueError:
        return None


def main():
    global current_index, start_time

    print(f"[*] Starting jammer detection on 2.4GHz band...")
    print(f"[*] Current channel: {channels[current_index]}")
    print(f"[*] Threshold: {THRESHOLD_DBM} dBm for {TRIGGER_SECONDS} seconds")

    process = subprocess.Popen(
        ["hackrf_sweep", "-f", "2400:2500", "-l", "40", "-g", "40", "-w", "100000"],
        stdout=subprocess.PIPE,
        text=True
    )

    for line in process.stdout:
        avg = parse_average(line)
        if avg is None:
            continue

        if avg > THRESHOLD_DBM:
            if start_time is None:
                start_time = time.time()
                print(f"[!] High power detected: {avg:.2f} dBm. Starting timer...")
            elif time.time() - start_time >= TRIGGER_SECONDS:
                current_index = (current_index + 1) % len(channels)
                next_channel = channels[current_index]
                switch_channel(next_channel)
                start_time = None
        else:
            if start_time is not None:
                print(f"[*] Signal dropped back to normal: {avg:.2f} dBm. Resetting timer.")
            start_time = None


if __name__ == "__main__":
    main()