#!/usr/bin/env python3
"""Send a serial hard-reset pulse, then capture >=75s via the serial-monitor skill script."""

import argparse
import subprocess
import sys
import time

import serial

PORT = "/dev/cu.usbmodem21201"
BAUD = 115200
SKILL = "/Users/hushaohong/.codex/skills/serial-monitor/scripts/serial_monitor.py"


def serial_reset(port: str, baud: int) -> None:
    ser = serial.Serial(port=port, baudrate=baud, timeout=0.1)
    time.sleep(0.1)
    # Classic ESP32 hard-reset-to-run sequence via DTR/RTS.
    ser.setDTR(False)
    ser.setRTS(True)   # EN low: hold in reset
    time.sleep(0.15)
    ser.setRTS(False)  # EN high: run
    time.sleep(0.05)
    ser.close()


def main() -> int:
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument("--save", default="/Volumes/ML/vibe-coding/ESP32-C6-AMOLED-2.16/logs/serial_115200_75s.log")
    parser.add_argument("--duration", type=int, default=75)
    args = parser.parse_args()

    print(f"Reset {PORT} @ {BAUD} ...", flush=True)
    serial_reset(PORT, BAUD)
    print(f"Reset pulse sent; starting {args.duration}s capture.", flush=True)
    cmd = [
        sys.executable,
        SKILL,
        "--port", PORT,
        "--baud", str(BAUD),
        "--duration", str(args.duration),
        "--clear",
        "--save", args.save,
        "--timestamp",
        "-v",
    ]
    return subprocess.call(cmd)


if __name__ == "__main__":
    sys.exit(main())
