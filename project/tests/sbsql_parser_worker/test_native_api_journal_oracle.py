# Copyright (c) 2026 ScratchBird Software Inc.
# SPDX-License-Identifier: MPL-2.0
import hashlib
import struct
import unittest

from native_api_journal_oracle import read_api_journal


IDENTITY = bytes.fromhex("01a07c0a0d5c70008000ff0000000001")


def framed(body):
    return struct.pack("<I", len(body) + 32) + body + hashlib.sha256(body).digest()


def body(fields=(b"ddl.create_procedure", b"procedure", b"name", b"\0\xff\n\r\t", b"active")):
    return (b"SBAPI002" + struct.pack("<Q", 42) + IDENTITY + bytes(48)
            + b"".join(struct.pack("<I", len(field)) + field for field in fields) + b"\0")


class NativeApiJournalOracleTest(unittest.TestCase):
    def test_exact_binary_rows_and_opaque_payload(self):
        encoded = framed(body())
        rows = read_api_journal(encoded + encoded)
        self.assertEqual(len(rows), 2)
        self.assertEqual(rows[0].framed_bytes, encoded)
        self.assertEqual(rows[0].object_uuid, IDENTITY)
        self.assertEqual(rows[0].creator_transaction, 42)
        self.assertEqual(rows[0].target_uuids, (bytes(16),) * 3)
        self.assertEqual(rows[0].payload, b"\0\xff\n\r\t")
        self.assertFalse(rows[0].deleted)
        self.assertEqual(read_api_journal(b""), ())

    def test_every_truncation_and_invalid_suffix(self):
        encoded = framed(body())
        for length in range(1, len(encoded)):
            with self.subTest(length=length), self.assertRaises(ValueError):
                read_api_journal(encoded[:length])
            with self.assertRaises(ValueError):
                read_api_journal(encoded + encoded[:length])
        with self.assertRaises(ValueError):
            read_api_journal(encoded + b"\0")

    def test_identity_bits_even_with_valid_hash(self):
        for field in range(16, 80, 16):
            for at in (6, 8):
                for octet in range(256):
                    payload = bytearray(body())
                    payload[field:field + 16] = IDENTITY
                    payload[field + at] = octet
                    valid = octet >> (4 if at == 6 else 6) == (7 if at == 6 else 2)
                    with self.subTest(field=field, at=at, octet=octet):
                        if valid:
                            self.assertEqual(len(read_api_journal(framed(payload))), 1)
                        else:
                            with self.assertRaises(ValueError):
                                read_api_journal(framed(payload))
        payload = bytearray(body())
        payload[16:32] = bytes(16)
        with self.assertRaises(ValueError):
            read_api_journal(framed(payload))

    def test_closed_structure_and_hash(self):
        valid = body()
        for malformed in (b"SBAPI001" + valid[8:], valid + b"\0", valid[:-1] + b"\x02",
                          valid[:80] + b"\xff" * 4 + valid[84:]):
            with self.assertRaises(ValueError):
                read_api_journal(framed(malformed))
        for at in (0, 1, 4):
            fields = [b"op", b"kind", b"name", b"payload", b"state"]
            fields[at] = b""
            with self.assertRaises(ValueError):
                read_api_journal(framed(body(fields)))
        encoded = bytearray(framed(valid))
        encoded[-1] ^= 1
        with self.assertRaises(ValueError):
            read_api_journal(encoded)
        for size in (0, 132, 64 * 1024 * 1024 + 1, 0xffffffff):
            with self.assertRaises(ValueError):
                read_api_journal(struct.pack("<I", size) + encoded[4:])


if __name__ == "__main__":
    unittest.main()
