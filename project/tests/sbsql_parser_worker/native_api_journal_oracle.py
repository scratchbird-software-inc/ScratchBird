# Copyright (c) 2026 ScratchBird Software Inc.
# SPDX-License-Identifier: MPL-2.0
"""Independent reader for the binary UUID API journal used by route tests."""
import hashlib
import struct
from dataclasses import dataclass


@dataclass(frozen=True)
class ApiJournalRecord:
    framed_bytes: bytes
    creator_transaction: int
    object_uuid: bytes
    target_uuids: tuple[bytes, ...]
    operation: bytes
    object_kind: bytes
    default_name: bytes
    payload: bytes
    state: bytes
    deleted: bool


def read_api_journal(raw: bytes) -> tuple[ApiJournalRecord, ...]:
    """Reject malformed/truncated journals; never return a partial prefix."""
    rows = []
    offset = 0
    while offset < len(raw):
        if len(raw) - offset < 4:
            raise ValueError("truncated API journal frame length")
        size = struct.unpack_from("<I", raw, offset)[0]
        end = offset + 4 + size
        if not 133 <= size <= 64 * 1024 * 1024 or end > len(raw):
            raise ValueError("invalid API journal frame extent")
        frame = raw[offset:end]
        body = frame[4:-32]
        if body[:8] != b"SBAPI002" or hashlib.sha256(body).digest() != frame[-32:]:
            raise ValueError("invalid API journal magic or digest")
        creator = struct.unpack_from("<Q", body, 8)[0]
        identities = tuple(body[at:at + 16] for at in range(16, 80, 16))
        for index, identity in enumerate(identities):
            if index and identity == bytes(16):
                continue
            if identity[6] >> 4 != 7 or identity[8] >> 6 != 2:
                raise ValueError("invalid binary system UUID in API journal")
        fields = []
        cursor = 80
        for _ in range(5):
            if len(body) - cursor < 4:
                raise ValueError("truncated API journal field length")
            length = struct.unpack_from("<I", body, cursor)[0]
            cursor += 4
            if length > len(body) - cursor:
                raise ValueError("truncated API journal field")
            fields.append(body[cursor:cursor + length])
            cursor += length
        if cursor + 1 != len(body) or body[cursor] not in (0, 1):
            raise ValueError("invalid API journal terminal field or trailing bytes")
        if not fields[0] or not fields[1] or not fields[4]:
            raise ValueError("missing required API journal field")
        rows.append(ApiJournalRecord(frame, creator, identities[0], identities[1:],
                                     *fields, bool(body[cursor])))
        offset = end
    return tuple(rows)
