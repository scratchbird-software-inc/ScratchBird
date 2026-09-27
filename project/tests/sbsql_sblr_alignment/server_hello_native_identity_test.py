#!/usr/bin/env python3
# Copyright (c) 2026 ScratchBird Software Inc.
# SPDX-License-Identifier: MPL-2.0
"""Real SBPS identity lifetime and malformed HELLO regression, not SQL coverage."""
import argparse
import os
from pathlib import Path
import shutil
import socket
import struct
import tempfile

from ia01_package_process_e2e import seed_database, stop, wait_unix
import subprocess

HEADER = struct.Struct("<IHHHHIIIIIQQ16s16s16s")


def require(condition, detail):
    if not condition:
        raise RuntimeError(detail)


def identity():
    value = bytearray(os.urandom(16))
    value[6] = (value[6] & 15) | 0x70
    value[8] = (value[8] & 63) | 0x80
    return bytes(value)


def valid_identity(value):
    return len(value) == 16 and value[6] >> 4 == 7 and value[8] >> 6 == 2


def crc32c(data):
    crc = 0xffffffff
    for octet in data:
        crc ^= octet
        for _ in range(8):
            crc = (crc >> 1) ^ (0x82f63b78 if crc & 1 else 0)
    return crc ^ 0xffffffff


def lp(value):
    return struct.pack("<H", len(value)) + value


def hello():
    value = bytearray(b"".join(identity() for _ in range(4)))
    value += struct.pack("<II", 3, 0)
    value += lp(b"SBPS") + lp(b"sif.test") + lp(b"sif.test.bundle")
    value += bytes(32)
    offsets = [0, 16, 32, 48, len(value), len(value) + 16]
    value += identity() + identity() + struct.pack("<Q", 1) + b"\x01" + bytes(31)
    return bytes(value), offsets


def exact(sock, size):
    data = bytearray()
    while len(data) < size:
        part = sock.recv(size - len(data))
        require(part, "server closed before complete SBPS response")
        data += part
    return bytes(data)


def exchange(sock, payload, sequence=1):
    request = identity()
    header = bytearray(HEADER.pack(
        0x53504253, 96, 1, 0, 1, 0, 1001, len(payload), 0,
        crc32c(payload), 0, sequence, request, bytes(16), bytes(16)))
    struct.pack_into("<I", header, 24, crc32c(header))
    sock.sendall(header + payload)
    raw = bytearray(exact(sock, 96))
    fields = HEADER.unpack(raw)
    require(fields[:4] == (0x53504253, 96, 1, 0), "invalid SBPS response header")
    require(fields[11] == sequence and fields[12] == request,
            "response request correlation changed")
    require(fields[7] <= 1024 * 1024, "oversized HELLO response")
    checksum = fields[8]
    struct.pack_into("<I", raw, 24, 0)
    require(crc32c(raw) == checksum, "response header checksum mismatch")
    body = exact(sock, fields[7])
    require(crc32c(body) == fields[9], "response payload checksum mismatch")
    return fields, body


def accepted(reply):
    fields, body = reply
    require(fields[4] == 2 and fields[5] == 5 and fields[6] == 1002,
            "valid HELLO was not accepted")
    require(len(body) == 102, "unexpected current HELLO_ACCEPT payload length")
    server, channel, registry = body[:16], body[16:32], body[82:98]
    require(all(valid_identity(v) for v in (server, channel, registry)),
            "HELLO_ACCEPT contains a malformed native UUID")
    return server, channel, registry, struct.unpack_from("<Q", body, 74)[0]


def connect(endpoint):
    sock = socket.socket(socket.AF_UNIX, socket.SOCK_STREAM)
    sock.settimeout(10)
    sock.connect(str(endpoint))
    return sock


def phase(endpoint):
    request, offsets = hello()
    with connect(endpoint) as first, connect(endpoint) as second:
        a = accepted(exchange(first, request))
        again = accepted(exchange(first, request, 2))
        second_request = hello()[0]
        b = accepted(exchange(second, second_request))
        require(a == again, "identical HELLO retry replaced retained owner/channel/snapshot")
        require(a[0] == b[0] and a[2:] == b[2:] and a[1] != b[1],
                "separate channels did not share exact owners with distinct channel UUIDs")
        changed = bytearray(request)
        changed[15] ^= 1
        rejected, _ = exchange(first, changed, 3)
        require(rejected[4] == 3 and rejected[5] & 2,
                "changed parser identity was accepted on an admitted channel")
        require(accepted(exchange(second, second_request, 2)) == b,
                "refused renegotiation disturbed a separate admitted channel")
    with connect(endpoint) as check:
        c = accepted(exchange(check, request))
        require(c[0] == a[0] and c[2:] == a[2:],
                "refused renegotiation replaced instance or registry owner")
    malformed = [request[:size] for size in range(len(request))]
    malformed += [request + b"\0", request + b"\xff" * 16]
    for offset in offsets:
        for kind in ("nil", "version", "variant"):
            broken = bytearray(request)
            if kind == "nil":
                broken[offset:offset + 16] = bytes(16)
            elif kind == "version":
                broken[offset + 6] = (broken[offset + 6] & 15) | 0x40
            else:
                broken[offset + 8] = (broken[offset + 8] & 63) | 0xc0
            malformed.append(bytes(broken))
    for payload in malformed:
        with connect(endpoint) as sock:
            fields, _ = exchange(sock, payload)
            require(fields[4] == 3 and fields[5] & 2,
                    "malformed/truncated/extended HELLO was not rejected")
    return a


def main():
    parser = argparse.ArgumentParser()
    parser.add_argument("--server", required=True, type=Path)
    args = parser.parse_args()
    work = Path(tempfile.mkdtemp(prefix="sbhelloid"))
    server = None
    passed = False
    try:
        database = work / "node.sbdb"
        seed_database(args.server, database)
        owners = []
        for ordinal in range(2):
            control = work / str(ordinal)
            endpoint = control / "s.sock"
            with (work / f"{ordinal}.out").open("wb") as out, (work / f"{ordinal}.err").open("wb") as err:
                server = subprocess.Popen([
                    str(args.server), "--foreground", "--no-listeners", "--database", str(database),
                    "--control-dir", str(control), "--runtime-dir", str(work / f"r{ordinal}"),
                    "--sbps-endpoint", str(endpoint)], stdout=out, stderr=err)
                wait_unix(endpoint)
                owners.append(phase(endpoint))
                stop(server)
                server = None
        require(owners[0][0] != owners[1][0] and owners[0][2] != owners[1][2],
                "restart reused the previous server or registry snapshot identity")
        passed = True
        print("server_hello_native_identity=passed")
        return 0
    except Exception as error:
        print(f"server_hello_native_identity=failed work={work}: {error}")
        return 1
    finally:
        stop(server)
        if passed:
            shutil.rmtree(work)


if __name__ == "__main__":
    raise SystemExit(main())
