#!/usr/bin/env python3
"""Download Flock-You Go USB records and normalize uptime timestamps to CSV.

The device has no real-time clock. Firmware emits USB JSON records with
uptime-relative timestamps, such as first_seen_ms and last_seen_ms. This tool
anchors those relative times to either the host download time or a user-supplied
collection time, then writes a normalized CSV.

Typical field workflow:

    python3 tools/download_flock_you_go.py --port /dev/cu.usbmodemXXXX \
      --collected-at 2026-10-13T10:05:00-07:00 \
      --csv downloads/flock_hits.csv

If the unit is still in the field and connected live, omit --collected-at and
the host RTC at download time is used as the anchor.
"""

from __future__ import annotations

import argparse
import csv
import json
import re
import time
from dataclasses import dataclass
from datetime import datetime, timedelta, timezone
from pathlib import Path
from typing import Any, Iterable


DEFAULT_BAUD = 115200
DEFAULT_TIMEOUT_SECONDS = 20.0
CSV_FIELDS = [
    "found_at",
    "last_seen_at",
    "anchor_time",
    "anchor_source",
    "session_source",
    "storage_path",
    "anchor_uptime_ms",
    "first_seen_ms",
    "last_seen_ms",
    "age_at_anchor_seconds",
    "mac_address",
    "oui",
    "detection_method",
    "confidence",
    "rssi",
    "channel",
    "frequency",
    "count",
    "observations",
    "encounters",
    "frame_kind",
    "ssid",
    "fingerprint_id",
    "schema",
]

MAC_RE = re.compile(r"^[0-9a-fA-F]{2}(?::[0-9a-fA-F]{2}){5}$")


@dataclass(frozen=True)
class DownloadResult:
    records: list[dict[str, Any]]
    raw_lines: list[str]
    anchor_uptime_ms: int


@dataclass(frozen=True)
class FlatRecord:
    source: str
    storage_path: str
    record: dict[str, Any]
    anchor_uptime_ms: int | None = None


def parse_datetime(value: str) -> datetime:
    text = value.strip()
    if text.endswith("Z"):
        text = text[:-1] + "+00:00"
    try:
        parsed = datetime.fromisoformat(text)
    except ValueError as exc:
        raise argparse.ArgumentTypeError(
            f"invalid datetime {value!r}; use ISO-8601, e.g. 2026-10-13T10:05:00-07:00"
        ) from exc
    if parsed.tzinfo is None:
        raise argparse.ArgumentTypeError(
            f"datetime {value!r} has no timezone; include an offset such as -07:00"
        )
    return parsed


def utc_now() -> datetime:
    return datetime.now(timezone.utc).astimezone()


def iso(dt: datetime) -> str:
    return dt.astimezone().isoformat(timespec="seconds")


def load_jsonl(path: Path) -> list[dict[str, Any]]:
    records: list[dict[str, Any]] = []
    with path.open("r", encoding="utf-8") as stream:
        for line_number, line in enumerate(stream, start=1):
            text = line.strip()
            if not text or not text.startswith("{"):
                continue
            try:
                records.append(json.loads(text))
            except json.JSONDecodeError as exc:
                raise ValueError(f"{path}:{line_number}: invalid JSON: {exc}") from exc
    if not records:
        raise ValueError(f"no JSON records found in {path}")
    return records


def find_anchor_uptime(records: Iterable[dict[str, Any]]) -> int:
    snapshot_begin: int | None = None
    snapshot_end: int | None = None
    max_seen = 0
    for record in records:
        uptime = int(record.get("uptime_ms") or 0)
        max_seen = max(max_seen, uptime)
        event = record.get("event")
        if event == "snapshot_begin":
            snapshot_begin = uptime
        elif event == "snapshot_end":
            snapshot_end = uptime
    if snapshot_end is not None:
        return snapshot_end
    if snapshot_begin is not None:
        return snapshot_begin
    if max_seen:
        return max_seen
    raise ValueError("cannot determine anchor uptime; no uptime_ms fields found")


def channel_frequency(channel: Any) -> int | str:
    try:
        ch = int(channel)
    except (TypeError, ValueError):
        return ""
    if 1 <= ch <= 14:
        return 2407 + 5 * ch
    return ""


def oui_from_mac(mac: str) -> str:
    mac = (mac or "").lower()
    if not MAC_RE.fullmatch(mac):
        return ""
    return ":".join(mac.split(":")[:3])


def select_port(port: str) -> str:
    if port != "auto":
        return port
    try:
        from serial.tools import list_ports  # type: ignore
    except ImportError as exc:
        raise RuntimeError("pyserial is required for --port auto: python3 -m pip install pyserial") from exc

    candidates = []
    for item in list_ports.comports():
        device = item.device or ""
        label = f"{device} {item.description or ''} {item.manufacturer or ''}".lower()
        if any(token in label for token in ("usb", "serial", "modem", "esp", "wch", "silicon labs")):
            candidates.append(device)
    if len(candidates) == 1:
        return candidates[0]
    if not candidates:
        raise RuntimeError("no likely USB serial port found; pass --port /dev/cu.usbmodemXXXX")
    raise RuntimeError("multiple USB serial ports found; pass one explicitly: " + ", ".join(candidates))


def download_from_device(port: str, baud: int, timeout_seconds: float) -> DownloadResult:
    try:
        import serial  # type: ignore
    except ImportError as exc:
        raise RuntimeError("pyserial is required: python3 -m pip install pyserial") from exc

    records: list[dict[str, Any]] = []
    raw_lines: list[str] = []
    selected_port = select_port(port)

    with serial.Serial(selected_port, baud, timeout=0.5) as ser:
        ser.reset_input_buffer()
        ser.write(b"DUMP\n")
        ser.flush()
        deadline = time.monotonic() + timeout_seconds
        saw_begin = False

        while time.monotonic() < deadline:
            raw = ser.readline()
            if not raw:
                continue
            line = raw.decode("utf-8", errors="replace").strip()
            if not line:
                continue
            raw_lines.append(line)
            if not line.startswith("{"):
                continue
            try:
                record = json.loads(line)
            except json.JSONDecodeError:
                continue
            records.append(record)
            event = record.get("event")
            if event == "snapshot_begin":
                saw_begin = True
            if saw_begin and event == "snapshot_end":
                break

    if not records:
        raise RuntimeError("no USB JSON records received; check port, cable, and that firmware is running")
    anchor_uptime_ms = find_anchor_uptime(records)
    return DownloadResult(records=records, raw_lines=raw_lines, anchor_uptime_ms=anchor_uptime_ms)


def flatten_records(records: Iterable[dict[str, Any]], current_anchor_uptime_ms: int) -> list[FlatRecord]:
    flat: list[FlatRecord] = []
    previous: list[FlatRecord] = []

    for outer in records:
        event = outer.get("event")
        if event in {"snapshot", "detection"}:
            flat.append(FlatRecord(source="current_session", storage_path="/session.json", record=outer))
            continue

        if event == "previous_session_snapshot":
            inner = outer.get("record")
            if not isinstance(inner, dict):
                continue
            record = {
                "event": "snapshot",
                "schema": outer.get("schema", "flock-you-go.usb.v2"),
                "uptime_ms": outer.get("uptime_ms", 0),
                "first_seen_ms": inner.get("first", inner.get("first_seen_ms", 0)),
                "last_seen_ms": inner.get("last", inner.get("last_seen_ms", inner.get("first", 0))),
                "mac_address": inner.get("mac", inner.get("mac_address", "")),
                "detection_method": inner.get("detection_method") or f"wifi_{inner.get('method', '')}",
                "confidence": inner.get("confidence", ""),
                "rssi": inner.get("rssi", ""),
                "channel": inner.get("channel", ""),
                "frequency": inner.get("frequency", channel_frequency(inner.get("channel"))),
                "count": inner.get("count", ""),
                "observations": inner.get("observations", inner.get("count", "")),
                "encounters": inner.get("encounters", ""),
                "frame_kind": inner.get("frame_kind", ""),
                "ssid": inner.get("ssid", ""),
            }
            record["oui"] = inner.get("oui") or oui_from_mac(str(record["mac_address"]))
            record["fingerprint_id"] = inner.get("fingerprint_id") or f"wifi_{inner.get('method', '')}:{record['mac_address']}"
            previous.append(
                FlatRecord(
                    source="previous_session",
                    storage_path=str(outer.get("path") or "/prev_session.json"),
                    record=record,
                )
            )

    previous_anchor = 0
    for item in previous:
        record = item.record
        previous_anchor = max(
            previous_anchor,
            int(record.get("last_seen_ms") or 0),
            int(record.get("first_seen_ms") or 0),
        )
    if previous_anchor <= 0:
        previous_anchor = current_anchor_uptime_ms

    flat.extend(
        FlatRecord(
            source=item.source,
            storage_path=item.storage_path,
            record=item.record,
            anchor_uptime_ms=previous_anchor,
        )
        for item in previous
    )
    return flat


def normalize_records(
    records: Iterable[dict[str, Any]],
    anchor_time: datetime,
    anchor_source: str,
    anchor_uptime_ms: int,
) -> list[dict[str, Any]]:
    rows: list[dict[str, Any]] = []
    for item in flatten_records(records, anchor_uptime_ms):
        record = item.record
        row_anchor_uptime_ms = item.anchor_uptime_ms or anchor_uptime_ms
        first_seen_ms = int(record.get("first_seen_ms") or record.get("uptime_ms") or 0)
        last_seen_ms = int(record.get("last_seen_ms") or first_seen_ms)
        first_delta_ms = max(0, row_anchor_uptime_ms - first_seen_ms)
        last_delta_ms = max(0, row_anchor_uptime_ms - last_seen_ms)
        found_at = anchor_time - timedelta(milliseconds=first_delta_ms)
        last_seen_at = anchor_time - timedelta(milliseconds=last_delta_ms)

        rows.append(
            {
                "found_at": iso(found_at),
                "last_seen_at": iso(last_seen_at),
                "anchor_time": iso(anchor_time),
                "anchor_source": anchor_source,
                "session_source": item.source,
                "storage_path": item.storage_path,
                "anchor_uptime_ms": row_anchor_uptime_ms,
                "first_seen_ms": first_seen_ms,
                "last_seen_ms": last_seen_ms,
                "age_at_anchor_seconds": round(first_delta_ms / 1000.0, 3),
                "mac_address": record.get("mac_address", ""),
                "oui": record.get("oui", ""),
                "detection_method": record.get("detection_method", ""),
                "confidence": record.get("confidence", ""),
                "rssi": record.get("rssi", ""),
                "channel": record.get("channel", ""),
                "frequency": record.get("frequency", ""),
                "count": record.get("count", ""),
                "observations": record.get("observations", ""),
                "encounters": record.get("encounters", ""),
                "frame_kind": record.get("frame_kind", ""),
                "ssid": record.get("ssid", ""),
                "fingerprint_id": record.get("fingerprint_id", ""),
                "schema": record.get("schema", ""),
            }
        )
    return rows


def write_csv(path: Path, rows: list[dict[str, Any]]) -> None:
    path.parent.mkdir(parents=True, exist_ok=True)
    with path.open("w", newline="", encoding="utf-8") as stream:
        writer = csv.DictWriter(stream, fieldnames=CSV_FIELDS)
        writer.writeheader()
        writer.writerows(rows)


def write_jsonl(path: Path, lines: Iterable[str]) -> None:
    path.parent.mkdir(parents=True, exist_ok=True)
    with path.open("w", encoding="utf-8") as stream:
        for line in lines:
            stream.write(line.rstrip("\n") + "\n")


def build_parser() -> argparse.ArgumentParser:
    parser = argparse.ArgumentParser(description=__doc__)
    source = parser.add_mutually_exclusive_group(required=True)
    source.add_argument("--port", help="USB serial port, or 'auto' to pick a likely port")
    source.add_argument("--input-jsonl", type=Path, help="previously captured USB JSONL/log file")
    parser.add_argument("--baud", type=int, default=DEFAULT_BAUD)
    parser.add_argument("--timeout", type=float, default=DEFAULT_TIMEOUT_SECONDS)
    parser.add_argument("--collected-at", type=parse_datetime, help="ISO-8601 collection time with timezone")
    parser.add_argument("--downloaded-at", type=parse_datetime, help="override host download time; defaults to now")
    parser.add_argument("--csv", type=Path, default=Path("downloads/flock_you_go_hits.csv"))
    parser.add_argument("--raw-jsonl", type=Path, help="optional file for raw USB lines/JSONL")
    return parser


def main() -> int:
    args = build_parser().parse_args()
    download_time = args.downloaded_at or utc_now()

    if args.input_jsonl:
        records = load_jsonl(args.input_jsonl)
        raw_lines = [json.dumps(record, separators=(",", ":")) for record in records]
        anchor_uptime_ms = find_anchor_uptime(records)
    else:
        result = download_from_device(args.port, args.baud, args.timeout)
        records = result.records
        raw_lines = result.raw_lines
        anchor_uptime_ms = result.anchor_uptime_ms

    if args.collected_at:
        anchor_time = args.collected_at
        anchor_source = "collection_time"
    else:
        anchor_time = download_time
        anchor_source = "download_time"

    rows = normalize_records(records, anchor_time, anchor_source, anchor_uptime_ms)
    write_csv(args.csv, rows)
    if args.raw_jsonl:
        write_jsonl(args.raw_jsonl, raw_lines)

    current_count = sum(1 for row in rows if row["session_source"] == "current_session")
    previous_count = sum(1 for row in rows if row["session_source"] == "previous_session")
    print(f"Wrote {len(rows)} normalized detection row(s) to {args.csv}")
    print(f"Rows: current_session={current_count} previous_session={previous_count}")
    print(f"Anchor: {anchor_source}={iso(anchor_time)} current_uptime_ms={anchor_uptime_ms}")
    if not args.collected_at:
        print("No --collected-at supplied; normalized times are anchored to download time.")
    return 0


if __name__ == "__main__":
    raise SystemExit(main())
