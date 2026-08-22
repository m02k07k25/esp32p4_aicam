#!/usr/bin/env python3
"""Monitor server logs and extract completed BLE Mesh JPEG records.

The server writes ordinary ESP-IDF text logs and framed JPEG records to the
same console UART.  This program must own that COM port instead of
``idf.py monitor``: it prints the text bytes and saves only validated images.
"""

from __future__ import annotations

import argparse
import codecs
from dataclasses import dataclass
from datetime import datetime, timezone
import json
import os
from pathlib import Path
import struct
import sys
import tempfile
import time
from typing import Callable, Iterable, Optional
import zlib


MAGIC = b"BMJPEG01"
VERSION = 1
MAX_JPEG_BYTES = 30_720
SERVER_MESH_ADDR = 0x0001
MAX_DEVICE_ID = 0x7FFE
HEADER_STRUCT = struct.Struct("<8sHHHBBQIIII")
HEADER_SIZE = HEADER_STRUCT.size
HEADER_CRC_BYTES = HEADER_SIZE - 4

TIME_SOURCE_NAMES = {
    0: "P4_DETECTED",
    1: "RX_ESTIMATE",
    2: "UNKNOWN",
}


class RecordError(ValueError):
    """A candidate UART record did not pass protocol validation."""


@dataclass(frozen=True)
class RecordHeader:
    source_addr: int
    device_id: int
    time_source: int
    event_time_ms: int
    jpeg_len: int
    jpeg_crc32: int
    sequence: int


@dataclass(frozen=True)
class ImageRecord:
    header: RecordHeader
    jpeg: bytes


@dataclass(frozen=True)
class ParserEvent:
    kind: str
    message: str


def crc32(data: bytes) -> int:
    """CRC-32/IEEE used by esp_crc32_le(0, data, length)."""

    return zlib.crc32(data) & 0xFFFFFFFF


def device_id_from_source(source_addr: int) -> int:
    """Return the compile-time C6 ID encoded by deterministic provisioning."""

    device_id = source_addr - SERVER_MESH_ADDR
    if device_id < 1 or device_id > MAX_DEVICE_ID:
        raise RecordError(
            f"source address 0x{source_addr:04x} is not a managed C6 address"
        )
    return device_id


def load_locations(path: Path) -> dict[int, str]:
    """Load JSON such as {"1": "front-door", "2": "warehouse"}."""

    try:
        raw = json.loads(path.read_text(encoding="utf-8"))
    except (OSError, json.JSONDecodeError) as exc:
        raise ValueError(f"cannot load locations file {path}: {exc}") from exc
    if not isinstance(raw, dict):
        raise ValueError("locations JSON must be an object keyed by device ID")

    result: dict[int, str] = {}
    for raw_id, raw_location in raw.items():
        try:
            device_id = int(str(raw_id), 0)
        except ValueError as exc:
            raise ValueError(f"invalid device ID key {raw_id!r}") from exc
        if device_id < 1 or device_id > MAX_DEVICE_ID:
            raise ValueError(f"device ID {device_id} is outside 1..{MAX_DEVICE_ID}")
        if isinstance(raw_location, dict):
            raw_location = raw_location.get("name")
        if not isinstance(raw_location, str) or not raw_location.strip():
            raise ValueError(f"location for device ID {device_id} must be text")
        result[device_id] = raw_location.strip()
    return result


def decode_header(wire: bytes) -> RecordHeader:
    if len(wire) != HEADER_SIZE:
        raise RecordError(f"header size {len(wire)} != {HEADER_SIZE}")

    (magic, version, header_size, source_addr, time_source, reserved,
     event_time_ms, jpeg_len, jpeg_crc32, sequence,
     header_crc32) = HEADER_STRUCT.unpack(wire)

    if magic != MAGIC:
        raise RecordError("bad magic")
    if version != VERSION:
        raise RecordError(f"unsupported version {version}")
    if header_size != HEADER_SIZE:
        raise RecordError(f"header_size {header_size} != {HEADER_SIZE}")
    if reserved != 0:
        raise RecordError(f"reserved byte is {reserved}, expected 0")
    if time_source not in TIME_SOURCE_NAMES:
        raise RecordError(f"invalid time source {time_source}")
    if jpeg_len == 0 or jpeg_len > MAX_JPEG_BYTES:
        raise RecordError(f"invalid JPEG length {jpeg_len}")
    if sequence == 0:
        raise RecordError("invalid zero sequence")
    device_id = device_id_from_source(source_addr)

    calculated = crc32(wire[:HEADER_CRC_BYTES])
    if calculated != header_crc32:
        raise RecordError(
            f"header CRC expected=0x{header_crc32:08x} "
            f"calculated=0x{calculated:08x}"
        )

    return RecordHeader(
        source_addr=source_addr,
        device_id=device_id,
        time_source=time_source,
        event_time_ms=event_time_ms,
        jpeg_len=jpeg_len,
        jpeg_crc32=jpeg_crc32,
        sequence=sequence,
    )


def validate_jpeg(header: RecordHeader, jpeg: bytes) -> None:
    if len(jpeg) != header.jpeg_len:
        raise RecordError(
            f"JPEG size {len(jpeg)} != declared {header.jpeg_len}"
        )
    calculated = crc32(jpeg)
    if calculated != header.jpeg_crc32:
        raise RecordError(
            f"JPEG CRC expected=0x{header.jpeg_crc32:08x} "
            f"calculated=0x{calculated:08x}"
        )
    if len(jpeg) < 4 or jpeg[:2] != b"\xff\xd8" or jpeg[-2:] != b"\xff\xd9":
        raise RecordError("JPEG SOI/EOI markers are missing")


class ImageStreamParser:
    """Incremental parser that resynchronizes at the next valid magic."""

    def __init__(
        self,
        event_callback: Optional[Callable[[ParserEvent], None]] = None,
        log_callback: Optional[Callable[[bytes], None]] = None,
    ) -> None:
        self._buffer = bytearray()
        self._event_callback = event_callback
        self._log_callback = log_callback
        self._suppress_noise = False

    @property
    def buffered_bytes(self) -> int:
        return len(self._buffer)

    @property
    def has_incomplete_record(self) -> bool:
        """True only for a CRC-valid header awaiting declared JPEG bytes."""

        if len(self._buffer) < HEADER_SIZE or not self._buffer.startswith(MAGIC):
            return False
        try:
            header = decode_header(bytes(self._buffer[:HEADER_SIZE]))
        except RecordError:
            return False
        return len(self._buffer) < HEADER_SIZE + header.jpeg_len

    def _event(self, kind: str, message: str) -> None:
        if self._event_callback is not None:
            self._event_callback(ParserEvent(kind, message))

    def _log(self, data: bytes) -> None:
        if data and not self._suppress_noise and self._log_callback is not None:
            self._log_callback(data)

    def _discard_noise_without_magic(self) -> None:
        keep = min(len(self._buffer), len(MAGIC) - 1)
        while keep and self._buffer[-keep:] != MAGIC[:keep]:
            keep -= 1
        discard = len(self._buffer) - keep
        if discard:
            self._log(bytes(self._buffer[:discard]))
            del self._buffer[:discard]

    def feed(self, data: bytes) -> list[ImageRecord]:
        if data:
            self._buffer.extend(data)
        records: list[ImageRecord] = []

        while True:
            magic_at = self._buffer.find(MAGIC)
            if magic_at < 0:
                self._discard_noise_without_magic()
                break
            if magic_at:
                self._log(bytes(self._buffer[:magic_at]))
                del self._buffer[:magic_at]
                # Bytes following a rejected binary candidate are discarded,
                # not printed as console text.  Finding the next magic ends
                # that recovery window.
                self._suppress_noise = False

            if len(self._buffer) < HEADER_SIZE:
                break

            try:
                header = decode_header(bytes(self._buffer[:HEADER_SIZE]))
            except RecordError as exc:
                # Drop only the first magic byte.  Searching again through the
                # remaining bytes also recovers from an inserted/deleted UART
                # byte immediately before the following record.
                del self._buffer[0]
                self._suppress_noise = True
                self._event("header_error", str(exc))
                continue

            # A structurally and CRC-valid header is a trustworthy record
            # boundary even if the prior recovery ended on a split magic.
            self._suppress_noise = False

            record_size = HEADER_SIZE + header.jpeg_len
            if len(self._buffer) < record_size:
                break

            jpeg = bytes(self._buffer[HEADER_SIZE:record_size])
            try:
                validate_jpeg(header, jpeg)
            except RecordError as exc:
                # Do not trust the declared boundary after payload corruption.
                # Keeping all but the first byte lets the normal magic search
                # recover a valid frame that follows a truncated payload.
                del self._buffer[0]
                self._suppress_noise = True
                self._event(
                    "image_error",
                    f"src=0x{header.source_addr:04x} "
                    f"seq={header.sequence} {exc}",
                )
                continue

            del self._buffer[:record_size]
            records.append(ImageRecord(header=header, jpeg=jpeg))

        return records

    def expire_incomplete_record(self) -> list[ImageRecord]:
        """Drop a CRC-valid header whose payload stopped arriving.

        A valid record cannot contain console text because firmware holds the
        stdout lock for the whole write.  A timeout therefore means that the
        UART record was truncated.  Dropping one byte and scanning again can
        still recover a following frame already buffered behind it.
        """

        if len(self._buffer) < HEADER_SIZE or not self._buffer.startswith(MAGIC):
            return []
        try:
            header = decode_header(bytes(self._buffer[:HEADER_SIZE]))
        except RecordError:
            return self.feed(b"")
        expected = HEADER_SIZE + header.jpeg_len
        if len(self._buffer) >= expected:
            return self.feed(b"")
        received = max(0, len(self._buffer) - HEADER_SIZE)
        del self._buffer[0]
        self._suppress_noise = True
        self._event(
            "image_error",
            f"src=0x{header.source_addr:04x} seq={header.sequence} "
            f"incomplete record timeout payload={received}/{header.jpeg_len}",
        )
        records = self.feed(b"")
        # Suppression applies only to bytes already buffered as the truncated
        # binary candidate. If there was no nested next magic, future console
        # text must become visible immediately instead of waiting for another
        # image record.
        self._suppress_noise = False
        return records


def _iso8601_ms(epoch_ms: int) -> Optional[str]:
    if epoch_ms <= 0:
        return None
    try:
        value = datetime.fromtimestamp(epoch_ms / 1000.0, tz=timezone.utc)
    except (OverflowError, OSError, ValueError):
        return None
    return value.isoformat(timespec="milliseconds").replace("+00:00", "Z")


def _atomic_write(path: Path, data: bytes) -> None:
    path.parent.mkdir(parents=True, exist_ok=True)
    temp_name: Optional[str] = None
    try:
        with tempfile.NamedTemporaryFile(
            mode="wb", prefix=f".{path.name}.", suffix=".tmp",
            dir=path.parent, delete=False,
        ) as temporary:
            temp_name = temporary.name
            temporary.write(data)
            temporary.flush()
            os.fsync(temporary.fileno())
        os.replace(temp_name, path)
        temp_name = None
    finally:
        if temp_name is not None:
            try:
                os.unlink(temp_name)
            except FileNotFoundError:
                pass


class ImageSink:
    def __init__(self, output: Path,
                 locations: Optional[dict[int, str]] = None) -> None:
        self.output = output
        self.locations = dict(locations or {})
        self.output.mkdir(parents=True, exist_ok=True)

    def save(self, record: ImageRecord) -> tuple[Path, dict]:
        received_at_ms = time.time_ns() // 1_000_000
        received_tag = (
            datetime.fromtimestamp(
                received_at_ms / 1000.0, tz=timezone.utc
            ).strftime("%Y%m%dT%H%M%S")
            + f"{received_at_ms % 1000:03d}Z"
        )
        header = record.header
        location = self.locations.get(header.device_id)
        stem = (
            f"image_{received_tag}_id{header.device_id:04d}_"
            f"src{header.source_addr:04x}_"
            f"seq{header.sequence:010d}"
        )
        image_path = self.output / f"{stem}.jpg"
        metadata_path = self.output / f"{stem}.json"

        metadata = {
            "protocol": "BMJPEG01",
            "version": VERSION,
            "device_id": header.device_id,
            "location": location,
            "source_addr": header.source_addr,
            "source_addr_hex": f"0x{header.source_addr:04x}",
            "time_source": TIME_SOURCE_NAMES.get(
                header.time_source, f"UNKNOWN_{header.time_source}"
            ),
            "event_time_ms": header.event_time_ms,
            "event_time_utc": _iso8601_ms(header.event_time_ms),
            "jpeg_len": header.jpeg_len,
            "jpeg_crc32": f"0x{header.jpeg_crc32:08x}",
            "sequence": header.sequence,
            "received_at_ms": received_at_ms,
            "received_at_utc": _iso8601_ms(received_at_ms),
            "file": image_path.name,
        }
        metadata_wire = (
            json.dumps(metadata, ensure_ascii=False, indent=2) + "\n"
        ).encode("utf-8")

        _atomic_write(image_path, record.jpeg)
        _atomic_write(metadata_path, metadata_wire)
        _atomic_write(self.output / "latest.jpg", record.jpeg)
        _atomic_write(self.output / "latest.json", metadata_wire)
        return image_path, metadata


class SequenceTracker:
    """Detect a lost/reordered exported record in the global sequence."""

    def __init__(self) -> None:
        self._last_global: Optional[int] = None
        self._last_by_source: dict[int, int] = {}

    def observe(self, header: RecordHeader) -> Optional[str]:
        warning: Optional[str] = None
        if self._last_global is not None:
            expected = self._last_global + 1
            if expected > 0xFFFFFFFF:
                expected = 1
            if header.sequence != expected:
                previous_source = self._last_by_source.get(header.source_addr)
                source_detail = (
                    "none" if previous_source is None else str(previous_source)
                )
                warning = (
                    f"expected={expected} got={header.sequence} "
                    f"src=0x{header.source_addr:04x} "
                    f"previous_for_src={source_detail}"
                )
        self._last_global = header.sequence
        self._last_by_source[header.source_addr] = header.sequence
        return warning


class PartialRecordDeadline:
    """Absolute deadline that incoming bytes cannot postpone."""

    def __init__(self, timeout_s: float) -> None:
        self.timeout_s = timeout_s
        self.started_at: Optional[float] = None

    def observe(self, incomplete: bool, now: float) -> bool:
        if not incomplete:
            self.started_at = None
            return False
        if self.started_at is None:
            self.started_at = now
            return False
        if now - self.started_at < self.timeout_s:
            return False
        # If expiry reveals another nested incomplete candidate, it receives a
        # fresh deadline rather than firing on every following read.
        self.started_at = now
        return True


def _print_parser_event(event: ParserEvent) -> None:
    print(
        f"\nSERIAL {event.kind.upper()} {event.message}",
        file=sys.stderr,
        flush=True,
    )


class ConsoleLogWriter:
    """Incrementally decode text split across arbitrary serial reads."""

    def __init__(self) -> None:
        # ESP32 ROM boot text is emitted at 115200 baud before the application
        # switches to the configured 921600-baud console. Those initial bytes
        # are therefore often invalid UTF-8. ``replace`` produces U+FFFD,
        # which a strict Windows CP949 stdout cannot encode and used to stop
        # the receiver. ASCII ``\\xNN`` escapes remain printable on every
        # Windows console while preserving the offending byte values.
        self._decoder = codecs.getincrementaldecoder("utf-8")(
            errors="backslashreplace"
        )

    def write(self, data: bytes) -> None:
        text = self._decoder.decode(data, final=False)
        if text:
            sys.stdout.write(text)
            sys.stdout.flush()


def receive_forever(port_name: str, baud: int, output: Path,
                    locations: Optional[dict[int, str]] = None) -> int:
    try:
        import serial  # type: ignore
    except ModuleNotFoundError:
        print(
            "pyserial is required. Install it with: python -m pip install pyserial",
            file=sys.stderr,
        )
        return 2

    if not hasattr(serial, "Serial"):
        print(
            "The installed 'serial' package is not pyserial. Remove it and "
            "install pyserial: python -m pip install pyserial",
            file=sys.stderr,
        )
        return 2

    log_writer = ConsoleLogWriter()
    parser = ImageStreamParser(
        event_callback=_print_parser_event,
        log_callback=log_writer.write,
    )
    sink = ImageSink(output, locations)
    sequences = SequenceTracker()

    def process_records(records: Iterable[ImageRecord]) -> None:
        for record in records:
            sequence_warning = sequences.observe(record.header)
            if sequence_warning is not None:
                print(
                    f"\nSERIAL SEQUENCE_GAP {sequence_warning}",
                    file=sys.stderr,
                    flush=True,
                )
            try:
                path, metadata = sink.save(record)
            except OSError as exc:
                print(
                    f"SAVE_ERROR src=0x{record.header.source_addr:04x} "
                    f"seq={record.header.sequence} {exc}",
                    file=sys.stderr,
                    flush=True,
                )
                continue
            print(
                f"IMAGE id={metadata['device_id']} "
                f"location={metadata['location'] or '-'} "
                f"src={metadata['source_addr_hex']} "
                f"seq={metadata['sequence']} "
                f"event_ms={metadata['event_time_ms']} "
                f"time={metadata['time_source']} "
                f"bytes={metadata['jpeg_len']} file={path.name}",
                flush=True,
            )

    try:
        uart = serial.Serial(port=port_name, baudrate=baud, timeout=0.25)
    except Exception as exc:  # pyserial exception types are unavailable above
        print(f"cannot open {port_name}: {exc}", file=sys.stderr)
        return 2

    print(
        f"listening port={port_name} baud={baud} output={output.resolve()}",
        flush=True,
    )
    # Three complete wire-times plus margin avoids false expiry even when the
    # user deliberately selects a much slower console than the 921600 default.
    partial_timeout_s = max(
        2.0, ((HEADER_SIZE + MAX_JPEG_BYTES) * 10.0 / baud) * 3.0
    )
    partial_deadline = PartialRecordDeadline(partial_timeout_s)
    try:
        while True:
            waiting = getattr(uart, "in_waiting", 0)
            chunk = uart.read(min(max(int(waiting), 1), 8192))
            now = time.monotonic()
            if chunk:
                process_records(parser.feed(chunk))

            # Empty reads and ordinary bytes both advance the same absolute
            # deadline; only resolving/replacing the candidate resets it.
            if partial_deadline.observe(parser.has_incomplete_record, now):
                process_records(parser.expire_incomplete_record())
                partial_deadline.observe(parser.has_incomplete_record, now)
    except KeyboardInterrupt:
        print("stopped", flush=True)
        return 0
    except Exception as exc:
        print(f"serial read failed: {exc}", file=sys.stderr)
        return 2
    finally:
        uart.close()


def _parse_args(argv: Optional[Iterable[str]] = None) -> argparse.Namespace:
    parser = argparse.ArgumentParser(
        description="Monitor the server console and save validated JPEG frames"
    )
    parser.add_argument("--port", required=True, help="server console COM port")
    parser.add_argument("--baud", type=int, default=921600)
    parser.add_argument(
        "--output", type=Path, default=Path("received_images"),
        help="output directory (default: received_images)",
    )
    parser.add_argument(
        "--locations", type=Path,
        help='optional JSON mapping of C6 IDs to locations, e.g. {"1":"entrance"}',
    )
    args = parser.parse_args(argv)
    if args.baud <= 0:
        parser.error("--baud must be positive")
    return args


def main(argv: Optional[Iterable[str]] = None) -> int:
    args = _parse_args(argv)
    try:
        locations = load_locations(args.locations) if args.locations else {}
    except ValueError as exc:
        print(exc, file=sys.stderr)
        return 2
    return receive_forever(args.port, args.baud, args.output, locations)


if __name__ == "__main__":
    raise SystemExit(main())
