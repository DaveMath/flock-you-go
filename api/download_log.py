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
    next_request = 0.0
    requests_sent = 0
    diagnostics = []

    with serial.Serial(port, 115200, timeout=0.25) as device:
        # ESP32-S3 USB CDC can re-enumerate when a host opens the port. Give
        # the running firmware time to return before sending the first command.
        time.sleep(1.0)
        device.reset_input_buffer()

        while time.monotonic() < deadline:
            now = time.monotonic()
            if now >= next_request:
                device.write(b"DUMP\n")
                device.flush()
                requests_sent += 1
                next_request = now + 2.0

            line = device.readline().decode("utf-8", errors="replace").strip()
            if line and not line.startswith("{") and len(diagnostics) < 8:
                diagnostics.append(line)
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

    print(
        f"No complete snapshot received after {requests_sent} DUMP requests. "
        "Confirm that Flock-You-Go is the firmware currently running, then retry.",
        file=sys.stderr,
    )
    if diagnostics:
        print("Device output seen:", file=sys.stderr)
        for line in diagnostics:
            print(f"  {line}", file=sys.stderr)
    return 1


def main() -> int:
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument("--port", required=True, help="AiPi USB CDC port, e.g. /dev/cu.usbmodem1301")
    parser.add_argument("--output", default="flock-you-go-session.jsonl", type=Path)
    parser.add_argument("--timeout", default=20.0, type=float)
    args = parser.parse_args()
    return download(args.port, args.output, args.timeout)


if __name__ == "__main__":
    raise SystemExit(main())
