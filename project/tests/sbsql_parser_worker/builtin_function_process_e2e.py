#!/usr/bin/env python3
# Copyright (c) 2026 ScratchBird Software Inc.
# SPDX-License-Identifier: MPL-2.0
"""Authenticated parser-process -> binary SBPS -> server -> engine regression."""
import argparse
import hashlib
import os
import struct
from pathlib import Path
import shutil
import subprocess
import sys
import tempfile

sys.path.insert(0, str(Path(__file__).resolve().parents[1] / "sbsql_sblr_alignment"))
from ia01_package_process_e2e import seed_database, stop, wait_unix


def check_name_resolution_trace(path):
    """Independent byte/UUID oracle for the actual server's diagnostic stream."""
    def fields(packet):
        assert packet[:8] == b"SBRES002" and len(packet) >= 12
        count = struct.unpack_from("<I", packet, 8)[0]
        offset, result = 12, {}
        for _ in range(count):
            assert offset + 4 <= len(packet)
            width = struct.unpack_from("<I", packet, offset)[0]
            offset += 4
            assert width and offset + width + 9 <= len(packet)
            name = packet[offset:offset + width].decode("ascii")
            offset += width
            kind, width = struct.unpack_from("<BQ", packet, offset)
            offset += 9
            assert kind <= 5 and offset + width <= len(packet) and name not in result
            value = packet[offset:offset + width]
            offset += width
            assert kind != 2 or width == 16
            assert kind != 3 or width == 8
            result[name] = kind, value
        assert offset == len(packet)
        return result

    def identity(field):
        kind, value = field
        assert kind == 2 and len(value) == 16
        assert value[6] & 0xf0 == 0x70 and value[8] & 0xc0 == 0x80
        return value

    stream = path.read_bytes()
    offset = records = keys = 0
    databases = set()
    while offset < len(stream):
        assert stream[offset:offset + 8] == b"SBNRT003"
        assert offset + 16 <= len(stream)
        width = struct.unpack_from("<Q", stream, offset + 8)[0]
        assert width <= 64 * 1024 * 1024
        end = offset + 16 + width
        assert end + 32 <= len(stream)
        assert hashlib.sha256(stream[offset:end]).digest() == stream[end:end + 32]
        record = fields(stream[offset + 16:end])
        assert record["format"][0] == 0 and record["format"][1].endswith(b".v3")
        for name in ("cache_key", "stable_cache_key"):
            if name not in record:
                continue
            assert record[name][0] == 4
            key = fields(record[name][1])
            assert key["format"] == (0, b"server.public_name_resolution_key.v3")
            for field in ("database_uuid", "effective_user_uuid", "dialect_profile_uuid"):
                value = identity(key[field])
                if field in record:
                    assert key[field] == record[field]
                if field == "database_uuid":
                    databases.add(value)
            assert key["stable"][0] == 3 and key["qualified"][0] == 3
            assert struct.unpack("<Q", key["stable"][1])[0] in (0, 1)
            assert struct.unpack("<Q", key["qualified"][1])[0] in (0, 1)
            keys += 1
        records += 1
        offset = end + 32
    assert records and keys and len(databases) == 1
    return databases


def main():
    arguments = argparse.ArgumentParser()
    arguments.add_argument("--server", required=True, type=Path)
    arguments.add_argument("--client", required=True, type=Path)
    args = arguments.parse_args()
    work = Path(tempfile.mkdtemp(prefix="sbfn"))
    server = None
    passed = False
    try:
        database = work / "node.sbdb"
        password = seed_database(args.server, database)
        trace_databases = None
        for phase in ("initial", "restart"):
            control = work / (phase + "-control")
            endpoint = control / "s.sock"
            trace_path = work / (phase + ".name-resolution.bin")
            server_env = dict(os.environ, SCRATCHBIRD_PUBLIC_NAME_RESOLUTION_TRACE_FILE=str(trace_path))
            with (work / (phase + ".server.out")).open("wb") as out, (work / (phase + ".server.err")).open("wb") as err:
                server = subprocess.Popen(
                    [str(args.server), "--foreground", "--no-listeners", "--database", str(database),
                     "--control-dir", str(control), "--runtime-dir", str(work / (phase + "-runtime")),
                     "--sbps-endpoint", str(endpoint)], stdout=out, stderr=err, env=server_env)
                wait_unix(endpoint)
                for ordinal in range(2):
                    process = subprocess.run([str(args.client), "unix:" + str(endpoint), str(database), password,
                                              phase + str(ordinal)], capture_output=True, text=True, timeout=300)
                    (work / f"{phase}.{ordinal}.client.log").write_text(process.stdout + process.stderr)
                    if process.returncode or process.stderr or process.stdout != f"builtin_function_binary_process=passed phase={phase}{ordinal}\n":
                        raise RuntimeError(f"client exited {process.returncode}: {process.stdout} {process.stderr}")
                stop(server)
                server = None
                observed = check_name_resolution_trace(trace_path)
                if trace_databases is None:
                    trace_databases = observed
                else:
                    assert observed == trace_databases, "restart changed native name-cache database binding"
        passed = True
        print("builtin_function_binary_process_e2e=passed")
        return 0
    except Exception as error:
        print(f"builtin_function_binary_process_e2e=failed work={work}: {error}", file=sys.stderr)
        return 1
    finally:
        stop(server)
        if passed:
            shutil.rmtree(work)


if __name__ == "__main__":
    raise SystemExit(main())
