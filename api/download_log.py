#!/usr/bin/env python3
"""Download the AiPi's current on-device fingerprint session over USB CDC."""

import argparse
import json
import sys
import time
from pathlib import Path

import serial


def download(port: str, output: Path, timeout: float) -> int:
    records = []
    snapshot_open = False
    deadline = time.monotonic() + timeout

    with serial.Serial(port, 115200, timeout=0.25) as device:
        device.reset_input_buffer()
        device.write(b"DUMP\n")
        device.flush()

        while time.monotonic() < deadline:
            line = device.readline().decode("utf-8", errors="replace").strip()
            if not line.startswith("{"):
                continue
            try:
                event = json.loads(line)
            except json.JSONDecodeError:
                continue

            if event.get("event") == "snapshot_begin":
                snapshot_open = True
                continue
            if event.get("event") == "snapshot" and snapshot_open:
                records.append(event)
                continue
            if event.get("event") == "snapshot_end" and snapshot_open:
                output.parent.mkdir(parents=True, exist_ok=True)
                with output.open("w", encoding="utf-8") as stream:
                    for record in records:
                        stream.write(json.dumps(record, separators=(",", ":")) + "\n")
                print(f"Saved {len(records)} fingerprints to {output}")
                return 0

    print("No complete snapshot received. Confirm the AiPi USB port and retry.", file=sys.stderr)
    return 1


def main() -> int:
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument("--port", required=True, help="AiPi USB CDC port, e.g. /dev/cu.usbmodem1301")
    parser.add_argument("--output", default="flock-you-go-session.jsonl", type=Path)
    parser.add_argument("--timeout", default=12.0, type=float)
    args = parser.parse_args()
    return download(args.port, args.output, args.timeout)


if __name__ == "__main__":
    raise SystemExit(main())
