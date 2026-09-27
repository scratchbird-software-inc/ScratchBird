# Copyright (c) 2026 ScratchBird Software Inc.
# SPDX-License-Identifier: MPL-2.0
"""Strict test-client decoder; UUID formatting never runs in the server."""
import uuid


def decode_binary_status(data: bytes) -> tuple[tuple[bytes, int, bytes], ...]:
    offset = 0

    def take(size: int) -> bytes:
        nonlocal offset
        if size > len(data) - offset:
            raise ValueError("truncated binary status")
        value = data[offset:offset + size]
        offset += size
        return value

    def number(size: int) -> int:
        return int.from_bytes(take(size), "little")

    if take(8) != b"SBRES002":
        raise ValueError("binary status magic missing")
    count = number(4)
    if not count or count > (len(data) - offset) // 14:
        raise ValueError("invalid binary status field count")
    fields = []
    for index in range(count):
        name = take(number(4))
        kind = number(1)
        value = take(number(8))
        if index == 0:
            if (name, kind, value) != (b"contract", 0, b"binary_status_json.v1"):
                raise ValueError("invalid binary status contract")
        elif name == b"text" and kind == 0:
            value.decode("utf-8", errors="strict")
        elif name == b"uuid" and kind == 2 and len(value) == 16:
            pass  # Raw data bits; identity-role admission is not display policy.
        else:
            raise ValueError("invalid binary status field")
        fields.append((name, kind, value))
    if offset != len(data):
        raise ValueError("trailing binary status bytes")
    return tuple(fields)


def render_binary_status(data: bytes) -> str:
    return "".join(value.decode("utf-8", errors="strict") if kind == 0
                   else str(uuid.UUID(bytes=value))
                   for _, kind, value in decode_binary_status(data)[1:])


def status_failure_display(data: bytes) -> str:
    """Do not let diagnostic printing mask the original malformed-packet error."""
    try:
        return render_binary_status(data)
    except ValueError as error:
        return f"invalid binary status: {error}; raw prefix={data[:4096]!r}"
