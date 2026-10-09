# Copyright (c) 2026 ScratchBird Software Inc.
# SPDX-License-Identifier: MPL-2.0
"""Strict binary observation decoding, including record boundaries."""
import json
from pathlib import Path
import unittest
from unittest.mock import patch
import uuid

from binary_observation_client import read_observation_records, render_observations


def field(name, kind, value):
    return (len(name).to_bytes(4, "little") + name + bytes([kind]) +
            len(value).to_bytes(8, "little") + value)


def record(*fields):
    fields = (field(b"contract", 0, b"binary_status_json.v1"),) + fields
    packet = b"SBRES002" + len(fields).to_bytes(4, "little") + b"".join(fields)
    return b"SBOBS002" + len(packet).to_bytes(8, "little") + packet


class BinaryObservationClientTest(unittest.TestCase):
    def decode(self, data):
        with patch.object(Path, "read_bytes", return_value=data):
            return read_observation_records(Path("unused"))

    def test_record_boundaries_do_not_depend_on_newlines(self):
        data = record(field(b"text", 0, b'{"first":1}'))
        data += record(field(b"text", 0, b'{"second":2}'))
        self.assertEqual([json.loads(x) for x in self.decode(data)],
                         [{"first": 1}, {"second": 2}])
        with patch.object(Path, "read_bytes", return_value=data):
            self.assertEqual(render_observations(Path("unused")),
                             '{"first":1}{"second":2}')

    def test_binary_identity_rendered_only_at_client(self):
        identity = uuid.UUID("019f0000-0000-7000-8000-000000024101")
        data = record(field(b"text", 0, b'{"identity":"'),
                      field(b"uuid", 2, identity.bytes), field(b"text", 0, b'"}'))
        self.assertEqual(json.loads(self.decode(data)[0])["identity"], str(identity))

    def test_all_truncations_refused(self):
        data = record(field(b"text", 0, b'{"ok":true}'))
        for end in range(len(data)):
            with self.subTest(end=end), self.assertRaises(ValueError):
                self.decode(data[:end])

    def test_corrupt_later_record_not_skipped_after_good_event(self):
        with self.assertRaises(ValueError):
            self.decode(record(field(b"text", 0, b'{"ok":true}')) + b"garbage")

    def test_text_evidence_cannot_substitute_for_binary(self):
        with self.assertRaises(ValueError):
            self.decode(b'{"ok":true}\n')

    def test_engine_identity_requires_native_v7(self):
        for value, kind in [(b"0" * 36, 0), (bytes(16), 2),
                            (uuid.UUID("019f0000-0000-4000-8000-000000024101").bytes, 2),
                            (b"x" * 15, 2)]:
            with self.subTest(value=value), self.assertRaises(ValueError):
                self.decode(record(field(b"uuid", kind, value)))

    def test_unknown_field_and_invalid_utf8_refused(self):
        for item in [field(b"other", 0, b"a"), field(b"text", 0, b"\xff")]:
            with self.subTest(item=item), self.assertRaises(ValueError):
                self.decode(record(item))

    def test_packet_contract_and_trailing_payload_refused(self):
        data = record(field(b"text", 0, b"ok"))
        for bad in [data.replace(b"binary_status_json.v1", b"binary_status_json.v0"),
                    data[:8] + (len(data) - 15).to_bytes(8, "little") + data[16:] + b"x",
                    data.replace(b"SBRES002", b"SBRES001")]:
            with self.subTest(bad=bad), self.assertRaises(ValueError):
                self.decode(bad)


if __name__ == "__main__":
    unittest.main()
