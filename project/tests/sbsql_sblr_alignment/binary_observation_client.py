# Copyright (c) 2026 ScratchBird Software Inc.
# SPDX-License-Identifier: MPL-2.0
"""Strict client-side display decoder for binary observation evidence."""
from pathlib import Path
import uuid


def render_observations(path: Path) -> str:
    data = path.read_bytes()
    cursor = 0
    records = []
    while cursor < len(data):
        if data[cursor:cursor + 8] != b"SBOBS002" or len(data) - cursor < 16:
            raise ValueError("invalid observation record header")
        size = int.from_bytes(data[cursor + 8:cursor + 16], "little")
        cursor += 16
        if size > 64 * 1024 * 1024 or size > len(data) - cursor:
            raise ValueError("invalid observation record length")
        payload = data[cursor:cursor + size]
        cursor += size
        offset = 0
        def take(count):
            nonlocal offset
            if count > len(payload) - offset:
                raise ValueError("truncated observation field")
            value = payload[offset:offset + count]
            offset += count
            return value
        def number(width):
            return int.from_bytes(take(width), "little")
        if take(8) != b"SBRES002":
            raise ValueError("invalid observation packet")
        count = number(4)
        if not count or count > (len(payload) - offset) // 14:
            raise ValueError("invalid observation field count")
        pieces = []
        for index in range(count):
            name = take(number(4))
            kind = number(1)
            value = take(number(8))
            if index == 0:
                if (name, kind, value) != (b"contract", 0, b"binary_status_json.v1"):
                    raise ValueError("invalid observation contract")
            elif name == b"text" and kind == 0:
                pieces.append(value.decode("utf-8"))
            elif name == b"uuid" and kind == 2 and len(value) == 16:
                identity = uuid.UUID(bytes=value)
                if identity.version != 7 or identity.variant != uuid.RFC_4122:
                    raise ValueError("invalid engine identity evidence")
                pieces.append(str(identity))
            else:
                raise ValueError("invalid observation field")
        if offset != len(payload):
            raise ValueError("trailing observation bytes")
        records.append("".join(pieces))
    if not records:
        raise ValueError("empty observation evidence")
    return "".join(records)


def read_trace_evidence(paths: tuple[Path, ...]) -> str:
    text = [path.read_text(encoding="utf-8", errors="strict") for path in paths]
    binary = [Path(str(path) + ".sbobs") for path in paths]
    present = [path for path in binary if path.exists()]
    if not present:
        raise ValueError("binary executor identity evidence missing")
    return "\n".join(text + [render_observations(path) for path in present])
