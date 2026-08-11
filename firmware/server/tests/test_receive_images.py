#!/usr/bin/env python3
from __future__ import annotations

import importlib.util
import json
from pathlib import Path
import struct
import sys
import tempfile
import unittest


sys.dont_write_bytecode = True

TOOLS_FILE = Path(__file__).resolve().parents[1] / "tools" / "receive_images.py"
SPEC = importlib.util.spec_from_file_location("receive_images", TOOLS_FILE)
assert SPEC is not None and SPEC.loader is not None
receive_images = importlib.util.module_from_spec(SPEC)
sys.modules[SPEC.name] = receive_images
SPEC.loader.exec_module(receive_images)


def jpeg(payload: bytes = b"test-payload") -> bytes:
    return b"\xff\xd8" + payload + b"\xff\xd9"


def frame(
    image: bytes,
    *,
    source_addr: int = 0x0002,
    time_source: int = 0,
    event_time_ms: int = 1_800_000_000_123,
    sequence: int = 7,
    declared_len: int | None = None,
) -> bytes:
    length = len(image) if declared_len is None else declared_len
    header = receive_images.HEADER_STRUCT.pack(
        receive_images.MAGIC,
        receive_images.VERSION,
        receive_images.HEADER_SIZE,
        source_addr,
        time_source,
        0,
        event_time_ms,
        length,
        receive_images.crc32(image),
        sequence,
        0,
    )
    header = header[:-4] + struct.pack(
        "<I", receive_images.crc32(header[:-4])
    )
    return header + image


class StreamParserTest(unittest.TestCase):
    def test_fragmented_records_and_noise_resynchronize(self) -> None:
        events = []
        logs = []
        parser = receive_images.ImageStreamParser(events.append, logs.append)
        first = frame(jpeg(b"one"), sequence=1)
        second = frame(jpeg(b"two"), source_addr=3, sequence=2)
        before = b"I (101) mesh: before\n"
        middle = b"I (102) mesh: middle\n"
        after = b"I (103) mesh: after\n"
        wire = before + first + middle + second + after

        records = []
        cuts = [1, 2, 7, 3, 19, 5, 64, 4, 1024]
        offset = 0
        for size in cuts:
            records.extend(parser.feed(wire[offset:offset + size]))
            offset += size
        records.extend(parser.feed(wire[offset:]))

        self.assertEqual([r.header.sequence for r in records], [1, 2])
        self.assertEqual(records[0].jpeg, jpeg(b"one"))
        self.assertEqual(records[1].header.source_addr, 3)
        self.assertEqual(b"".join(logs), before + middle + after)
        self.assertEqual(events, [])

    def test_magic_split_across_feeds_preserves_exact_logs(self) -> None:
        logs = []
        parser = receive_images.ImageStreamParser(log_callback=logs.append)
        wire = frame(jpeg(b"split"), sequence=5)

        self.assertEqual(parser.feed(b"I first\n" + wire[:3]), [])
        records = parser.feed(wire[3:] + b"I second\n")

        self.assertEqual([record.header.sequence for record in records], [5])
        self.assertEqual(b"".join(logs), b"I first\nI second\n")

    def test_bad_header_crc_is_rejected_then_next_record_survives(self) -> None:
        events = []
        logs = []
        parser = receive_images.ImageStreamParser(events.append, logs.append)
        bad = bytearray(frame(jpeg(b"bad-header"), sequence=10))
        bad[receive_images.HEADER_SIZE - 1] ^= 0x80
        good = frame(jpeg(b"good"), sequence=11)

        self.assertEqual(parser.feed(bytes(bad)), [])
        records = parser.feed(good[:4])
        records.extend(parser.feed(good[4:] + b"I recovered\n"))

        self.assertEqual([r.header.sequence for r in records], [11])
        self.assertTrue(any(e.kind == "header_error" for e in events))
        self.assertEqual(b"".join(logs), b"I recovered\n")

    def test_oversize_header_is_rejected(self) -> None:
        events = []
        parser = receive_images.ImageStreamParser(events.append)
        oversized = frame(
            b"", declared_len=receive_images.MAX_JPEG_BYTES + 1, sequence=20
        )
        good = frame(jpeg(b"after-oversize"), sequence=21)

        records = parser.feed(oversized + good)

        self.assertEqual([r.header.sequence for r in records], [21])
        self.assertTrue(
            any("invalid JPEG length" in e.message for e in events)
        )

    def test_bad_jpeg_crc_and_truncated_payload_resynchronize(self) -> None:
        events = []
        parser = receive_images.ImageStreamParser(events.append)
        damaged = bytearray(frame(jpeg(b"crc-damage"), sequence=30))
        damaged[-3] ^= 0x11
        truncated_full = frame(jpeg(b"payload-that-will-be-cut"), sequence=31)
        truncated = truncated_full[:receive_images.HEADER_SIZE + 5]
        good = frame(jpeg(b"recovered"), sequence=32)

        records = parser.feed(bytes(damaged) + truncated + good)

        self.assertEqual([r.header.sequence for r in records], [32])
        image_errors = [e for e in events if e.kind == "image_error"]
        self.assertGreaterEqual(len(image_errors), 2)
        self.assertTrue(any("JPEG CRC" in e.message for e in image_errors))

    def test_marker_error_with_valid_crc_is_rejected(self) -> None:
        events = []
        parser = receive_images.ImageStreamParser(events.append)
        not_jpeg = b"NO" + b"payload" + b"END"
        good = frame(jpeg(b"valid"), sequence=42)

        records = parser.feed(frame(not_jpeg, sequence=41) + good)

        self.assertEqual([r.header.sequence for r in records], [42])
        self.assertTrue(
            any("SOI/EOI" in e.message for e in events)
        )

    def test_unknown_time_source_is_rejected(self) -> None:
        events = []
        parser = receive_images.ImageStreamParser(events.append)
        good = frame(jpeg(b"after-time-source"), sequence=52)

        records = parser.feed(
            frame(jpeg(b"unknown"), time_source=9, sequence=51) + good
        )

        self.assertEqual([record.header.sequence for record in records], [52])
        self.assertTrue(
            any("invalid time source" in event.message for event in events)
        )

    def test_zero_sequence_is_rejected(self) -> None:
        events = []
        parser = receive_images.ImageStreamParser(events.append)
        good = frame(jpeg(b"after-zero"), sequence=61)

        records = parser.feed(frame(jpeg(b"zero"), sequence=0) + good)

        self.assertEqual([record.header.sequence for record in records], [61])
        self.assertTrue(
            any("zero sequence" in event.message for event in events)
        )

    def test_incomplete_record_timeout_recovers_nested_next_frame(self) -> None:
        events = []
        logs = []
        parser = receive_images.ImageStreamParser(events.append, logs.append)
        truncated = frame(
            b"\xff\xd8x", declared_len=receive_images.MAX_JPEG_BYTES,
            sequence=70,
        )
        good = frame(jpeg(b"after-timeout"), sequence=71)

        self.assertEqual(parser.feed(truncated + good), [])
        records = parser.expire_incomplete_record()
        records.extend(parser.feed(b"I after timeout\n"))

        self.assertEqual([record.header.sequence for record in records], [71])
        self.assertEqual(b"".join(logs), b"I after timeout\n")
        self.assertTrue(
            any("incomplete record timeout" in event.message for event in events)
        )

    def test_incomplete_candidate_persists_while_log_bytes_arrive(self) -> None:
        events = []
        logs = []
        parser = receive_images.ImageStreamParser(events.append, logs.append)
        truncated = frame(
            b"\xff\xd8x", declared_len=receive_images.MAX_JPEG_BYTES,
            sequence=72,
        )

        deadline = receive_images.PartialRecordDeadline(timeout_s=2.0)
        self.assertEqual(parser.feed(truncated), [])
        self.assertTrue(parser.has_incomplete_record)
        self.assertFalse(deadline.observe(parser.has_incomplete_record, 10.0))
        for now, line in (
            (10.8, b"I (1) still alive\n"),
            (11.6, b"I (2) still alive\n"),
        ):
            self.assertEqual(parser.feed(line), [])
            self.assertTrue(parser.has_incomplete_record)
            self.assertFalse(deadline.observe(parser.has_incomplete_record, now))

        # Continuous bytes did not move the original t=10.0 deadline.
        self.assertTrue(deadline.observe(parser.has_incomplete_record, 12.1))
        self.assertEqual(parser.expire_incomplete_record(), [])
        self.assertFalse(deadline.observe(parser.has_incomplete_record, 12.1))
        self.assertFalse(parser.has_incomplete_record)
        self.assertEqual(logs, [])
        self.assertEqual(parser.feed(b"I recovered log\n"), [])
        self.assertEqual(b"".join(logs), b"I recovered log\n")
        self.assertTrue(
            any("incomplete record timeout" in event.message for event in events)
        )

    def test_global_sequence_gap_reports_source_context(self) -> None:
        tracker = receive_images.SequenceTracker()
        records = receive_images.ImageStreamParser().feed(
            frame(jpeg(b"one"), source_addr=2, sequence=10)
            + frame(jpeg(b"two"), source_addr=3, sequence=12)
        )

        self.assertIsNone(tracker.observe(records[0].header))
        warning = tracker.observe(records[1].header)
        self.assertIsNotNone(warning)
        self.assertIn("expected=11 got=12", warning)
        self.assertIn("src=0x0003", warning)


class ImageSinkTest(unittest.TestCase):
    def test_atomic_image_latest_and_metadata_outputs(self) -> None:
        image = jpeg(b"saved")
        parsed = receive_images.ImageStreamParser().feed(
            frame(image, source_addr=0x23, time_source=1, sequence=99)
        )
        self.assertEqual(len(parsed), 1)

        with tempfile.TemporaryDirectory() as directory:
            output = Path(directory)
            path, metadata = receive_images.ImageSink(output).save(parsed[0])

            self.assertEqual(path.read_bytes(), image)
            self.assertEqual((output / "latest.jpg").read_bytes(), image)
            latest = json.loads(
                (output / "latest.json").read_text(encoding="utf-8")
            )
            self.assertEqual(latest["source_addr_hex"], "0x0023")
            self.assertEqual(latest["time_source"], "RX_ESTIMATE")
            self.assertEqual(latest["sequence"], 99)
            self.assertEqual(metadata, latest)
            self.assertTrue((output / path.with_suffix(".json").name).is_file())
            self.assertFalse(list(output.glob("*.tmp")))


if __name__ == "__main__":
    unittest.main(verbosity=2)
