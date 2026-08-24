#!/usr/bin/env python3
"""Monitor server logs, save BLE Mesh JPEG records, and show them locally.

The server writes ordinary ESP-IDF text logs and framed JPEG records to the
same console UART. This program must own that COM port instead of
``idf.py monitor``: it prints the text bytes, saves only validated images,
serves the latest result from a PC-local HTTP viewer, and sends the laptop
wall clock back to the firmware.
"""

from __future__ import annotations

import argparse
import codecs
from dataclasses import dataclass
from datetime import datetime, timezone
from http.server import BaseHTTPRequestHandler, ThreadingHTTPServer
import json
import os
from pathlib import Path
import struct
import sys
import tempfile
import threading
import time
from typing import Callable, Iterable, Optional
from urllib.parse import urlsplit
import webbrowser
import zlib


MAGIC = b"BMJPEG01"
VERSION = 1
MAX_JPEG_BYTES = 30_720
SERVER_MESH_ADDR = 0x0001
MAX_DEVICE_ID = 0x7FFE
HEADER_STRUCT = struct.Struct("<8sHHHBBQIIII")
HEADER_SIZE = HEADER_STRUCT.size
HEADER_CRC_BYTES = HEADER_SIZE - 4

LAPTOP_TIME_MAGIC = b"BMTIME01"
LAPTOP_TIME_VERSION = 1
LAPTOP_TIME_STRUCT = struct.Struct("<8sHHQII")
LAPTOP_TIME_PACKET_SIZE = LAPTOP_TIME_STRUCT.size
LAPTOP_TIME_CRC_BYTES = LAPTOP_TIME_PACKET_SIZE - 4

TIME_SOURCE_NAMES = {
    0: "P4_DETECTED",
    1: "RX_ESTIMATE",
    2: "UNKNOWN",
}
DEFAULT_LOCATIONS_PATH = Path(__file__).with_name("locations.json")


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


def encode_laptop_time_packet(unix_ms: int, sequence: int) -> bytes:
    """Encode one PC-to-server wall-clock update for the console UART."""

    if unix_ms < 0 or unix_ms > 0xFFFFFFFFFFFFFFFF:
        raise ValueError("unix_ms must fit in uint64")
    if sequence < 1 or sequence > 0xFFFFFFFF:
        raise ValueError("sequence must be in 1..0xffffffff")
    wire = LAPTOP_TIME_STRUCT.pack(
        LAPTOP_TIME_MAGIC,
        LAPTOP_TIME_VERSION,
        LAPTOP_TIME_PACKET_SIZE,
        unix_ms,
        sequence,
        0,
    )
    return wire[:LAPTOP_TIME_CRC_BYTES] + struct.pack(
        "<I", crc32(wire[:LAPTOP_TIME_CRC_BYTES])
    )


def _send_laptop_time(uart, sequence: int) -> int:
    """Send the current laptop time and return the next nonzero sequence."""

    packet = encode_laptop_time_packet(
        time.time_ns() // 1_000_000, sequence
    )
    written = uart.write(packet)
    if written != len(packet):
        raise OSError(
            f"short laptop-time write {written}/{len(packet)} bytes"
        )
    uart.flush()
    return 1 if sequence == 0xFFFFFFFF else sequence + 1


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


VIEWER_HTML = """<!doctype html>
<html lang="ko">
<head>
  <meta charset="utf-8">
  <meta name="viewport" content="width=device-width,initial-scale=1">
  <title>BLE Mesh image receiver</title>
  <style>
    :root { color-scheme: dark; font-family: system-ui, sans-serif; }
    body { margin: 0; background: #111827; color: #e5e7eb; }
    [hidden] { display: none !important; }
    main { width: min(960px, calc(100% - 32px)); margin: 20px auto; }
    header { display: flex; align-items: center; gap: 12px; flex-wrap: wrap; }
    h1 { margin: 0; font-size: 1.35rem; }
    h2 { margin: 0 0 12px; color: #dbeafe; font-size: 1rem; }
    #state { padding: 5px 10px; border-radius: 999px; background: #374151; }
    #state.ready { background: #065f46; }
    .panel { padding: 16px; border-radius: 14px; background: #1f2937;
             box-shadow: 0 10px 28px #0005; }
    .latest { margin-top: 14px; }
    .image-stage { min-height: min(42vh, 420px); display: grid;
                   place-items: center; border-radius: 10px; background: #111827; }
    #image { display: block; width: min(100%, 420px); max-height: min(42vh, 420px);
             object-fit: contain;
             margin: 0 auto; image-rendering: auto; border-radius: 10px; }
    #empty { color: #9ca3af; text-align: center; }
    .info-grid { display: grid; grid-template-columns: repeat(2, minmax(0, 1fr));
                 gap: 14px; margin-top: 14px; }
    dl { display: grid; grid-template-columns: max-content 1fr; gap: 8px 16px;
         margin: 0; }
    dt { color: #9ca3af; } dd { margin: 0; overflow-wrap: anywhere; }
    @media (max-width: 680px) {
      .info-grid { grid-template-columns: 1fr; }
      .image-stage { min-height: 224px; }
    }
  </style>
</head>
<body>
<main>
  <header><h1>BLE Mesh 이미지 수신기</h1><span id="state">대기 중</span></header>
  <section class="panel latest">
    <h2>최신 사진</h2>
    <div class="image-stage">
      <div id="empty">검증 완료된 첫 번째 이미지를 기다리고 있습니다.</div>
      <img id="image" alt="최근 수신한 JPEG" hidden>
    </div>
  </section>
  <div class="info-grid">
    <section class="panel">
      <h2>기기 정보</h2>
      <dl>
        <dt>기기 ID</dt><dd id="device">-</dd>
        <dt>설치 위치</dt><dd id="location">-</dd>
        <dt>Mesh 송신 주소</dt><dd id="source">-</dd>
      </dl>
    </section>
    <section class="panel">
      <h2>이벤트 정보</h2>
      <dl>
        <dt>검출 시각</dt><dd id="event-time">-</dd>
        <dt>시각 출처</dt><dd id="time-source">-</dd>
        <dt>PC 수신 시각</dt><dd id="receive-time">-</dd>
        <dt>JPEG 크기</dt><dd id="image-info">-</dd>
        <dt>수신 순번</dt><dd id="sequence">-</dd>
      </dl>
    </section>
  </div>
</main>
<script>
const byId = id => document.getElementById(id);
let lastToken = "";
function koreanTime(ms) {
  const value = Number(ms);
  if (value <= 0) return "알 수 없음";
  return new Date(value).toLocaleString("ko-KR", {
    timeZone: "Asia/Seoul",
    year: "numeric", month: "2-digit", day: "2-digit",
    hour: "2-digit", minute: "2-digit", second: "2-digit",
    hour12: false
  }) + " KST";
}
async function refresh() {
  try {
    const response = await fetch(`/status.json?t=${Date.now()}`, {cache: "no-store"});
    if (!response.ok) throw new Error(`HTTP ${response.status}`);
    const status = await response.json();
    if (!status.image_available) {
      byId("state").textContent = "대기 중";
      return;
    }
    const item = status.latest;
    const token = `${item.sequence}:${item.received_at_ms}:${item.jpeg_crc32}`;
    if (token !== lastToken) {
      lastToken = token;
      byId("image").src = `/latest.jpg?v=${encodeURIComponent(token)}`;
    }
    byId("empty").hidden = true;
    byId("image").hidden = false;
    byId("state").textContent = "수신 완료";
    byId("state").className = "ready";
    byId("device").textContent = item.device_id;
    byId("location").textContent = item.location || "미설정";
    byId("source").textContent = item.source_addr_hex;
    byId("event-time").textContent = koreanTime(item.event_time_ms);
    byId("time-source").textContent = item.time_source;
    byId("receive-time").textContent = koreanTime(item.received_at_ms);
    byId("image-info").textContent = `${(item.jpeg_len / 1024).toFixed(1)} KB`;
    byId("sequence").textContent = item.sequence;
  } catch (error) {
    byId("state").textContent = "화면 오류";
    byId("state").className = "";
  } finally {
    window.setTimeout(refresh, 750);
  }
}
refresh();
</script>
</body>
</html>
""".encode("utf-8")


@dataclass(frozen=True)
class LatestSnapshot:
    metadata: dict
    image_path: Path


def _load_latest_snapshot(
    output: Path, locations: Optional[dict[int, str]] = None
) -> Optional[LatestSnapshot]:
    """Resolve a metadata/image pair without exposing arbitrary files."""

    try:
        metadata = json.loads(
            (output / "latest.json").read_text(encoding="utf-8")
        )
    except (OSError, json.JSONDecodeError, UnicodeError):
        return None
    if not isinstance(metadata, dict):
        return None
    metadata = dict(metadata)

    device_id = metadata.get("device_id")
    if isinstance(device_id, int) and locations and device_id in locations:
        # The PC-side installation map is authoritative and can relabel old
        # saved frames without touching the BLE/serial wire format.
        metadata["location"] = locations[device_id]

    file_name = metadata.get("file")
    jpeg_len = metadata.get("jpeg_len")
    if (
        not isinstance(file_name, str)
        or not file_name
        or Path(file_name).name != file_name
        or not isinstance(jpeg_len, int)
        or isinstance(jpeg_len, bool)
        or jpeg_len < 1
        or jpeg_len > MAX_JPEG_BYTES
    ):
        return None

    image_path = output / file_name
    try:
        if not image_path.is_file() or image_path.stat().st_size != jpeg_len:
            return None
    except OSError:
        return None
    return LatestSnapshot(metadata=metadata, image_path=image_path)


class _ViewerHTTPServer(ThreadingHTTPServer):
    # SO_REUSEADDR permits multiple live listeners on the same port on
    # Windows, which can randomly serve a stale viewer process. Require one
    # owner so a second receiver fails with a clear --http-port suggestion.
    allow_reuse_address = False
    daemon_threads = True


def _viewer_handler(
    output: Path,
    started_at_ms: int,
    locations: Optional[dict[int, str]] = None,
):
    class ViewerHandler(BaseHTTPRequestHandler):
        protocol_version = "HTTP/1.1"

        def log_message(self, format_string: str, *args) -> None:
            # Serial and image events remain readable in the terminal; routine
            # browser polling must not flood it with one line every 750 ms.
            del format_string, args

        def _send(
            self, status: int, body: bytes, content_type: str
        ) -> None:
            self.send_response(status)
            self.send_header("Content-Type", content_type)
            self.send_header("Content-Length", str(len(body)))
            self.send_header("Cache-Control", "no-store, max-age=0")
            self.send_header("X-Content-Type-Options", "nosniff")
            self.send_header("Connection", "close")
            self.end_headers()
            if body:
                try:
                    self.wfile.write(body)
                except (BrokenPipeError, ConnectionResetError):
                    pass

        def _json(self, status: int, value: dict) -> None:
            body = json.dumps(
                value, ensure_ascii=False, separators=(",", ":")
            ).encode("utf-8")
            self._send(status, body, "application/json; charset=utf-8")

        def do_GET(self) -> None:  # noqa: N802 - BaseHTTPRequestHandler API
            route = urlsplit(self.path).path
            if route in ("/", "/index.html"):
                self._send(200, VIEWER_HTML, "text/html; charset=utf-8")
                return
            if route == "/favicon.ico":
                self._send(204, b"", "image/x-icon")
                return

            snapshot = _load_latest_snapshot(output, locations)
            if route == "/status.json":
                self._json(
                    200,
                    {
                        "image_available": snapshot is not None,
                        "viewer_started_at_ms": started_at_ms,
                        "latest": (
                            snapshot.metadata if snapshot is not None else None
                        ),
                    },
                )
                return
            if route == "/latest.json":
                if snapshot is None:
                    self._json(404, {"error": "no completed image yet"})
                else:
                    self._json(200, snapshot.metadata)
                return
            if route == "/latest.jpg":
                if snapshot is None:
                    self._json(404, {"error": "no completed image yet"})
                    return
                try:
                    image = snapshot.image_path.read_bytes()
                except OSError as exc:
                    self._json(503, {"error": f"cannot read image: {exc}"})
                    return
                self._send(200, image, "image/jpeg")
                return
            self._json(404, {"error": "not found"})

    return ViewerHandler


class LocalImageViewer:
    """Small PC-only HTTP viewer backed by ImageSink's atomic files."""

    def __init__(
        self,
        output: Path,
        host: str = "127.0.0.1",
        port: int = 8000,
        locations: Optional[dict[int, str]] = None,
    ) -> None:
        self.output = output.resolve()
        self.host = host
        self.port = port
        self.locations = dict(locations or {})
        self._server: Optional[_ViewerHTTPServer] = None
        self._thread: Optional[threading.Thread] = None

    @property
    def url(self) -> str:
        if self._server is None:
            raise RuntimeError("viewer has not started")
        actual_port = int(self._server.server_address[1])
        display_host = (
            "127.0.0.1" if self.host in ("", "0.0.0.0") else self.host
        )
        return f"http://{display_host}:{actual_port}/"

    def start(self) -> str:
        if self._server is not None:
            raise RuntimeError("viewer is already running")
        self.output.mkdir(parents=True, exist_ok=True)
        started_at_ms = time.time_ns() // 1_000_000
        handler = _viewer_handler(
            self.output, started_at_ms, self.locations
        )
        server = _ViewerHTTPServer((self.host, self.port), handler)
        thread = threading.Thread(
            target=server.serve_forever,
            name="local-image-http",
            daemon=True,
        )
        self._server = server
        self._thread = thread
        thread.start()
        return self.url

    def close(self) -> None:
        server = self._server
        thread = self._thread
        self._server = None
        self._thread = None
        if server is None:
            return
        server.shutdown()
        server.server_close()
        if thread is not None:
            thread.join(timeout=2.0)


def _open_viewer_browser(viewer_url: str) -> None:
    try:
        opened = webbrowser.open(viewer_url, new=2)
    except Exception as exc:
        print(
            f"cannot open browser automatically: {exc}; "
            f"open {viewer_url} manually",
            file=sys.stderr,
        )
        return
    if not opened:
        print(
            f"browser did not open; open {viewer_url} manually",
            file=sys.stderr,
        )


def view_only(
    output: Path,
    http_host: str = "127.0.0.1",
    http_port: int = 8000,
    open_browser: bool = True,
    locations: Optional[dict[int, str]] = None,
) -> int:
    """Serve saved images without opening a board COM port."""

    viewer = LocalImageViewer(output, http_host, http_port, locations)
    try:
        viewer_url = viewer.start()
    except (OSError, RuntimeError) as exc:
        print(
            f"cannot start local HTTP viewer on "
            f"{http_host}:{http_port}: {exc}",
            file=sys.stderr,
        )
        return 2

    print(
        f"view-only output={output.resolve()} viewer={viewer_url}",
        flush=True,
    )
    if open_browser:
        _open_viewer_browser(viewer_url)
    try:
        while True:
            time.sleep(1.0)
    except KeyboardInterrupt:
        print("stopped", flush=True)
        return 0
    finally:
        viewer.close()
    return 0


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


def _open_console_uart(serial_module, port_name: str, baud: int):
    """Open the server UART without asserting ESP32 boot-control lines.

    pyserial defaults DTR and RTS to ``True``.  On ESP32-CAM USB-UART
    adapters those lines commonly drive EN and IO0 through the automatic
    download circuit, so opening the receiver could leave a button-less
    board in ROM download mode.  Configure the line states while the port is
    still closed, then open it with both signals deasserted (normal boot).
    """

    uart = serial_module.Serial(
        port=None,
        baudrate=baud,
        timeout=0.25,
        write_timeout=1.0,
        dsrdtr=False,
        rtscts=False,
    )
    uart.port = port_name
    uart.dtr = False
    uart.rts = False
    uart.open()
    # Keep them deasserted if a backend reapplies its defaults on open.
    uart.dtr = False
    uart.rts = False
    return uart


def receive_forever(
    port_name: str,
    baud: int,
    output: Path,
    locations: Optional[dict[int, str]] = None,
    *,
    http_enabled: bool = True,
    http_host: str = "127.0.0.1",
    http_port: int = 8000,
    open_browser: bool = True,
    laptop_time_sync: bool = True,
    laptop_time_interval_s: float = 60.0,
) -> int:
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
        uart = _open_console_uart(serial, port_name, baud)
    except Exception as exc:  # pyserial exception types are unavailable above
        print(f"cannot open {port_name}: {exc}", file=sys.stderr)
        return 2

    time_sequence = 1
    next_time_sync_at = 0.0
    if laptop_time_sync:
        try:
            time_sequence = _send_laptop_time(uart, time_sequence)
        except Exception as exc:
            uart.close()
            print(f"initial laptop-time write failed: {exc}", file=sys.stderr)
            return 2
        next_time_sync_at = time.monotonic() + laptop_time_interval_s

    viewer: Optional[LocalImageViewer] = None
    if http_enabled:
        viewer = LocalImageViewer(
            output, http_host, http_port, locations
        )
        try:
            viewer_url = viewer.start()
        except (OSError, RuntimeError) as exc:
            uart.close()
            print(
                f"cannot start local HTTP viewer on "
                f"{http_host}:{http_port}: {exc}",
                file=sys.stderr,
            )
            print(
                "choose another port with --http-port, or use --no-http",
                file=sys.stderr,
            )
            return 2
        print(f"viewer={viewer_url}", flush=True)
        if open_browser:
            _open_viewer_browser(viewer_url)

    print(
        f"listening port={port_name} baud={baud} output={output.resolve()}",
        flush=True,
    )
    if laptop_time_sync:
        print(
            "laptop time sync enabled "
            f"interval={laptop_time_interval_s:g}s",
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
            now = time.monotonic()
            if (laptop_time_sync and now >= next_time_sync_at and
                    not parser.has_incomplete_record):
                time_sequence = _send_laptop_time(uart, time_sequence)
                next_time_sync_at = now + laptop_time_interval_s

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
        if viewer is not None:
            viewer.close()
        uart.close()


def _parse_args(argv: Optional[Iterable[str]] = None) -> argparse.Namespace:
    parser = argparse.ArgumentParser(
        description=(
            "Monitor the server console, save validated JPEG frames, and "
            "show them in a PC-local browser"
        )
    )
    parser.add_argument(
        "--port", help="server console COM port (not needed with --view-only)"
    )
    parser.add_argument("--baud", type=int, default=921600)
    parser.add_argument(
        "--output", type=Path, default=Path("received_images"),
        help="output directory (default: received_images)",
    )
    parser.add_argument(
        "--locations", type=Path, default=DEFAULT_LOCATIONS_PATH,
        help=(
            "JSON mapping of C6 IDs to locations "
            f"(default: {DEFAULT_LOCATIONS_PATH})"
        ),
    )
    parser.add_argument(
        "--http-host", default="127.0.0.1",
        help="PC HTTP viewer bind address (default: 127.0.0.1)",
    )
    parser.add_argument(
        "--http-port", type=int, default=8000,
        help="PC HTTP viewer port (default: 8000; 0 selects a free port)",
    )
    parser.add_argument(
        "--no-http", action="store_true",
        help="disable the PC HTTP viewer and only save files/print logs",
    )
    parser.add_argument(
        "--no-browser", action="store_true",
        help="run the viewer without opening the default browser",
    )
    parser.add_argument(
        "--view-only", action="store_true",
        help="preview saved images over HTTP without a board or COM port",
    )
    parser.add_argument(
        "--no-time-sync", action="store_true",
        help="do not send the laptop wall clock to the server firmware",
    )
    parser.add_argument(
        "--time-sync-interval", type=float, default=60.0,
        help="laptop clock update interval in seconds (default: 60)",
    )
    args = parser.parse_args(argv)
    if not args.view_only and not args.port:
        parser.error("--port is required unless --view-only is used")
    if args.view_only and args.no_http:
        parser.error("--view-only cannot be combined with --no-http")
    if args.baud <= 0:
        parser.error("--baud must be positive")
    if not args.http_host.strip():
        parser.error("--http-host must not be empty")
    if args.http_port < 0 or args.http_port > 65535:
        parser.error("--http-port must be between 0 and 65535")
    if args.time_sync_interval < 1.0 or args.time_sync_interval > 300.0:
        parser.error("--time-sync-interval must be between 1 and 300 seconds")
    return args


def main(argv: Optional[Iterable[str]] = None) -> int:
    args = _parse_args(argv)
    try:
        locations = load_locations(args.locations) if args.locations else {}
    except ValueError as exc:
        print(exc, file=sys.stderr)
        return 2
    if args.view_only:
        return view_only(
            args.output,
            http_host=args.http_host,
            http_port=args.http_port,
            open_browser=not args.no_browser,
            locations=locations,
        )
    return receive_forever(
        args.port,
        args.baud,
        args.output,
        locations,
        http_enabled=not args.no_http,
        http_host=args.http_host,
        http_port=args.http_port,
        open_browser=not args.no_browser,
        laptop_time_sync=not args.no_time_sync,
        laptop_time_interval_s=args.time_sync_interval,
    )


if __name__ == "__main__":
    raise SystemExit(main())
