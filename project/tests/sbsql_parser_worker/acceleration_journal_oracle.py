#!/usr/bin/env python3
# Copyright (c) 2026 ScratchBird Software Inc.
# SPDX-License-Identifier: MPL-2.0

"""Independent, fail-closed oracle for agent-only operational row appends.

This is deliberately not a permissive general MGA reader: the gate expects the
current general binary row format and the exact pre-existing agent table.
"""

import hashlib


class JournalError(ValueError):
    pass


class Reader:
    def __init__(self, data):
        self.data = memoryview(data)
        self.offset = 0

    def take(self, size):
        if size < 0 or size > len(self.data) - self.offset:
            raise JournalError("truncated binary journal")
        result = bytes(self.data[self.offset:self.offset + size])
        self.offset += size
        return result

    def integer(self, size):
        return int.from_bytes(self.take(size), "little")

    def field(self):
        return self.take(self.integer(4))

    def remaining(self):
        return len(self.data) - self.offset


def require_identity(value, optional=False):
    if optional and value == bytes(16):
        return
    if len(value) != 16 or value[6] >> 4 != 7 or value[8] >> 6 != 2:
        raise JournalError("invalid binary engine identity")


def agent_table_identity(metadata):
    reader = Reader(metadata)
    identities = set()
    while reader.remaining():
        start = reader.offset
        if reader.take(8) != b"SBMGAM02":
            raise JournalError("invalid metadata magic")
        size = reader.integer(8)
        if size > 64 * 1024 * 1024:
            raise JournalError("oversized metadata frame")
        payload = Reader(reader.take(size))
        digest = hashlib.sha256(reader.data[start:reader.offset]).digest()
        if reader.take(32) != digest:
            raise JournalError("metadata checksum mismatch")
        count = payload.integer(4)
        if not 0 < count <= min(65536, payload.remaining() // 4):
            raise JournalError("invalid metadata field count")
        fields = [payload.field() for _ in range(count)]
        if payload.remaining():
            raise JournalError("metadata trailing bytes")
        if fields[:2] == [b"SBMGA1", b"TABLE_METADATA"]:
            if len(fields) != 11:
                raise JournalError("invalid table metadata shape")
            identity, name = fields[4:6]
        elif fields[:2] == [b"SBMGA1", b"TABLE_METADATA_SEALED_DESCRIPTOR_V2"]:
            if len(fields) != 19:
                raise JournalError("invalid sealed table metadata shape")
            identity, name = fields[6:8]
        else:
            continue
        if name == b"sys.agent_durable_catalog_state":
            require_identity(identity)
            identities.add(identity)
    if len(identities) != 1:
        raise JournalError("agent table identity missing or ambiguous")
    return identities.pop()


AGENT_FIELDS = {
    b"record_kind", b"catalog_root_digest", b"encoded_catalog_image",
    b"catalog_generation", b"authority_evidence_uuid",
    b"storage_commit_evidence_uuid", b"storage_linkage_digest",
}


def require_agent_row_appends(data, table_identity):
    require_identity(table_identity)
    reader = Reader(data)
    if not reader.remaining():
        raise JournalError("empty row append")
    while reader.remaining():
        if reader.take(8) != b"SBMRBIN1" or reader.integer(2) != 8:
            raise JournalError("unexpected row journal format")
        if reader.integer(2) != 0 or reader.integer(4) != len(AGENT_FIELDS):
            raise JournalError("unexpected row header")
        count = reader.integer(8)
        names = [reader.field() for _ in AGENT_FIELDS]
        if set(names) != AGENT_FIELDS:
            raise JournalError("unexpected agent row columns")
        types = [reader.field() for _ in names]
        # The retained-row writer annotates its byte fields as text; those
        # labels are not datatype authority. The catalog schema, not a label
        # chosen by the row, determines which payloads must be binary UUIDs.
        if types != [b"text"] * len(names):
            raise JournalError("unexpected retained-row type annotations")
        if not 0 < count <= reader.remaining() // (105 + len(names)):
            raise JournalError("invalid row count")
        for _ in range(count):
            creator, sequence, previous = (reader.integer(8) for _ in range(3))
            deleted = reader.integer(1)
            table, row, version, predecessor, session = (reader.take(16) for _ in range(5))
            for value in (table, row, version):
                require_identity(value)
            require_identity(predecessor, optional=True)
            if (not creator or not sequence or deleted or table != table_identity or
                    session != bytes(16) or bool(previous) != (predecessor != bytes(16))):
                raise JournalError("non-agent or malformed operational row")
            if reader.take(len(names)) != bytes(len(names)):
                raise JournalError("agent row has non-value fields")
            values = dict(zip(names, (reader.field() for _ in names)))
            if values[b"record_kind"] != b"agent_catalog_image":
                raise JournalError("non-agent operational record kind")
            for name in (b"authority_evidence_uuid", b"storage_commit_evidence_uuid"):
                require_identity(values[name])
