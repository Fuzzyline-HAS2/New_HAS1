import io
import json
from pathlib import Path
import struct
import sys
import tempfile
import unittest
import zlib

sys.path.insert(0, str(Path(__file__).resolve().parents[1]))
from decode_flash import decode
from helpers import record


def metadata(generation=1, ring=0x0123456789ABCDEF, acked=0):
    raw = bytearray(64)
    raw[:4] = b"BBR1"
    struct.pack_into("<8I", raw, 4, 1, generation, ring & 0xFFFFFFFF, ring >> 32, 65, acked, 1920, 0x434F4D31)
    struct.pack_into("<I", raw, 60, zlib.crc32(raw[:60]))
    return raw


class FlashTests(unittest.TestCase):
    def decode_bytes(self, raw):
        with tempfile.TemporaryDirectory() as directory:
            path = Path(directory) / "partition.bin"
            path.write_bytes(raw)
            output = io.StringIO()
            try:
                decode(path, output)
            finally:
                self.assertEqual(path.read_bytes(), raw)
            return [json.loads(line) for line in output.getvalue().splitlines()]

    def test_legacy_192k_partition_uses_first_128k(self):
        raw = bytearray(b"\xff" * 196608)
        raw[:64] = metadata()
        raw[8192:8256] = record(1)
        raw[131072 + 252:131072 + 254] = b"\x39\x04"  # block 32 magic of a 48-block SPIFFS
        rows = self.decode_bytes(bytes(raw))
        self.assertEqual(rows[0]["kind"], "metadata")
        self.assertEqual([row["seq"] for row in rows if row["kind"] == "record"], [1])
        for size in (131072 + 4096, 262144):
            with self.assertRaises(ValueError):
                self.decode_bytes(b"\xff" * size)

    def test_metadata_selection_records_damage_and_no_mutation(self):
        raw = bytearray(b"\xff" * 131072)
        raw[:64] = metadata()
        raw[4096:4160] = metadata(generation=2, acked=1)
        raw[8192:8256] = record(1)
        raw[8256:8320] = record(2)
        raw[8320] = 0
        with tempfile.TemporaryDirectory() as directory:
            path = Path(directory) / "partition.bin"
            path.write_bytes(raw)
            output = io.StringIO()
            decode(path, output)
            rows = [json.loads(line) for line in output.getvalue().splitlines()]
            self.assertEqual(rows[0]["generation"], 2)
            self.assertTrue(rows[1]["acked"])
            self.assertFalse(rows[2]["acked"])
            self.assertEqual(rows[-1], {"kind": "summary", "valid": 2, "damaged": 1, "erased": 1917})
            self.assertEqual(path.read_bytes(), raw)

    def test_bad_size_rejected(self):
        with tempfile.TemporaryDirectory() as directory:
            path = Path(directory) / "partition.bin"
            path.write_bytes(b"bad")
            with self.assertRaises(ValueError):
                decode(path, io.StringIO())

    def test_full_metadata_journal_uses_last_slot_not_sector_start(self):
        raw = bytearray(b"\xff" * 131072)
        for slot in range(128):
            raw[slot * 64:(slot + 1) * 64] = metadata(generation=slot + 1)
        raw[8192:8256] = record(1)
        rows = self.decode_bytes(raw)
        self.assertEqual(rows[0]["generation"], 128)
        self.assertEqual(rows[0]["slot"], 127)
        self.assertEqual(rows[0]["offset"], 8128)
        self.assertEqual(rows[0]["valid_headers"], 128)
        self.assertEqual(rows[1]["seq"], 1)

    def test_rollover_newest_header_can_have_lower_offset(self):
        raw = bytearray(b"\xff" * 131072)
        for slot in range(64, 128):
            raw[slot * 64:(slot + 1) * 64] = metadata(generation=slot + 1)
        # First sector was reclaimed while second-sector headers remained valid.
        raw[:64] = metadata(generation=129, acked=2)
        rows = self.decode_bytes(raw)
        self.assertEqual(rows[0]["generation"], 129)
        self.assertEqual(rows[0]["slot"], 0)
        self.assertEqual(rows[0]["acked"], 2)

    def test_torn_header_skipped_and_preserved_in_output(self):
        raw = bytearray(b"\xff" * 131072)
        raw[64:128] = metadata(generation=3)
        torn = metadata(generation=4)
        torn[32:] = b"\xff" * 32
        raw[4160:4224] = torn
        rows = self.decode_bytes(raw)
        self.assertEqual(rows[0]["generation"], 3)
        self.assertEqual(rows[0]["damaged_headers"], 1)
        self.assertEqual(rows[1]["kind"], "damaged_metadata")
        self.assertEqual(rows[1]["offset"], 4160)
        self.assertEqual(rows[1]["raw_hex"], torn.hex())

    def test_any_journal_identity_conflict_rejected(self):
        raw = bytearray(b"\xff" * 131072)
        raw[64:128] = metadata(generation=1, ring=7)
        raw[8128:8192] = metadata(generation=999)
        with self.assertRaisesRegex(ValueError, "ambiguous metadata"):
            self.decode_bytes(raw)

    def test_same_generation_conflicting_bytes_rejected(self):
        raw = bytearray(b"\xff" * 131072)
        raw[128:192] = metadata(generation=10, acked=0)
        raw[4224:4288] = metadata(generation=10, acked=1)
        with self.assertRaisesRegex(ValueError, "ambiguous metadata"):
            self.decode_bytes(raw)

    def test_identical_duplicate_header_allowed(self):
        raw = bytearray(b"\xff" * 131072)
        raw[128:192] = metadata(generation=10)
        raw[4224:4288] = metadata(generation=10)
        rows = self.decode_bytes(raw)
        self.assertEqual(rows[0]["generation"], 10)
        self.assertEqual(rows[0]["valid_headers"], 2)

    def test_no_valid_metadata_and_unknown_data_are_preserved(self):
        raw = bytearray(b"\xff" * 131072)
        raw[64:128] = b"?" * 64
        with self.assertRaisesRegex(ValueError, "no valid BBR1 metadata"):
            self.decode_bytes(raw)

    def test_invalid_zero_generation_and_ack_boundary_are_skipped(self):
        raw = bytearray(b"\xff" * 131072)
        raw[:64] = metadata(generation=1)
        raw[128:192] = metadata(generation=0)
        raw[4224:4288] = metadata(generation=100, acked=65)
        rows = self.decode_bytes(raw)
        self.assertEqual(rows[0]["generation"], 1)
        self.assertEqual(rows[0]["damaged_headers"], 2)


if __name__ == "__main__":
    unittest.main()
