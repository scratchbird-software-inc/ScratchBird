# Copyright (c) 2026 ScratchBird Software Inc.
# SPDX-License-Identifier: MPL-2.0
"""Independent framing/JSON oracle for client display, including all UUID bits."""
import json
from pathlib import Path
import struct
import sys
import uuid

sys.path.insert(0, str(Path(__file__).resolve().parents[1] / "support"))
from binary_status_client import decode_binary_status, render_binary_status


def field(name, kind, value):
    return struct.pack("<I", len(name)) + name + struct.pack("<BQ", kind, len(value)) + value


def packet(identity, kind=2, name=b"uuid", contract=b"binary_status_json.v1"):
    return b"SBRES002" + struct.pack("<I", 4) + b"".join((
        field(b"contract", 0, contract), field(b"text", 0, b'{"identity":"'),
        field(name, kind, identity), field(b"text", 0, b'","note":"caf\xc3\xa9"}')))


def reject(data):
    try:
        render_binary_status(data)
    except ValueError:
        return
    raise AssertionError("malformed status was accepted")


values = [bytes(16), bytes([255]) * 16]
values += [bytes((1 << bit) if n == byte else 0 for n in range(16))
           for byte in range(16) for bit in range(8)]
for value in values:
    encoded = packet(value)
    decoded = decode_binary_status(encoded)
    assert decoded[2] == (b"uuid", 2, value), "UUID atom changed before display"
    display = json.loads(render_binary_status(encoded))
    assert uuid.UUID(display["identity"]).bytes == value and display["note"] == "café"
for end in range(len(encoded)):
    reject(encoded[:end])
for size in (0, 1, 15, 17, 36):
    reject(packet(bytes(size)))
for kind in (0, 1, 3, 4, 5, 255):
    reject(packet(bytes(16), kind=kind))
reject(packet(bytes(16), name=b"wrong"))
reject(packet(bytes(16), contract=b"binary_status_json.v0"))
reject(encoded + b"x")
reject(b'{"identity":"00000000-0000-0000-0000-000000000000"}')
reject(encoded[:8] + struct.pack("<I", 0) + encoded[12:])
reject(encoded[:8] + struct.pack("<I", 0xffffffff) + encoded[12:])
reject(encoded[:-4] + b"\xff" + encoded[-3:])
print("binary status client: 130 exact UUID roundtrips and malformed-frame checks passed")
