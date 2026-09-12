#!/usr/bin/env python3
# Copyright (c) 2026 ScratchBird Software Inc.
# SPDX-License-Identifier: MPL-2.0
"""Independent JSON parser oracle, not canonical MessageVector admission."""
import argparse
import hashlib
import json
from pathlib import Path
import struct
import subprocess


def cases():
    values = [(f"ascii/{a:02x}", chr(a)) for a in range(128)]
    values += [(f"pair/{a:02x}/{b:02x}", chr(a) + chr(b))
               for a in range(128) for b in range(128)]
    values += [(f"scalar/{cp:x}", chr(cp)) for cp in
               (0x80, 0x81, 0x7ff, 0x800, 0xd7ff, 0xe000, 0xffff,
                0x10000, 0x10001, 0x10ffff)]
    values += list(zip(("empty", "mixed_controls", "quoted", "literal_escape",
                        "combining", "supplementary", "line_separators",
                        "long_plain", "long_escapes", "long_unicode"),
                       ("", '"\\\n\r\t\b\f\0', 'quote: " and slash: \\',
                        "literal backslash-n: \\n", "e\u0301", "\U0001f600",
                        "\u2028\u2029", "x" * 4096, '\n"\\\t' * 1024,
                        "e\u0301\U0001f600" * 256)))
    return values


def unique_object(pairs):
    obj = {}
    for key, value in pairs:
        if key in obj:
            raise ValueError("duplicate JSON object member")
        obj[key] = value
    return obj


def main():
    parser = argparse.ArgumentParser()
    parser.add_argument("--producer", type=Path)
    parser.add_argument("--describe", action="store_true")
    args = parser.parse_args()
    expected = cases()
    manifest = b"".join(name.encode() + b"\0" + value.encode("utf-8") + b"\xff"
                        for name, value in expected)
    digest = hashlib.sha256(manifest).hexdigest()
    records = 3 * len(expected) + 256
    print(f"expected_cases={len(expected)} expected_records={records} manifest_sha256={digest}",
          flush=True)
    if args.describe:
        return 0
    if args.producer is None:
        parser.error("--producer is required without --describe")
    framed_parts = [struct.pack("<I", len(expected))]
    for _, value in expected:
        raw = value.encode("utf-8")
        framed_parts.extend((struct.pack("<I", len(raw)), raw))
    run = subprocess.run([str(args.producer.resolve())], input=b"".join(framed_parts),
                         capture_output=True, timeout=60, check=False)
    if run.returncode:
        print(f"FAIL producer exit={run.returncode} stderr={run.stderr[:1000]!r}")
        return 1
    offset = 0
    failures = 0
    observed = 0
    for ordinal, (name, value) in enumerate(expected):
        kinds = ["string", "public", "private"]
        if ordinal < 128:
            kinds += ["lifecycle_public", "lifecycle_private"]
        for kind in kinds:
            try:
                if len(run.stdout) - offset < 4:
                    raise ValueError("missing output frame header")
                size, = struct.unpack_from("<I", run.stdout, offset)
                offset += 4
                if size > len(run.stdout) - offset:
                    raise ValueError("truncated output frame")
                raw = run.stdout[offset:offset + size]
                offset += size
                observed += 1
                decoded = json.loads(raw.decode("utf-8"), object_pairs_hook=unique_object)
                if kind == "string":
                    assert decoded == value, "string value changed after JSON decoding"
                elif kind in ("public", "private"):
                    vector = decoded["message_vector"]
                    assert vector["safe_message"] == value, "safe message changed"
                    assert vector["fields"]["value"] == value, "field value changed"
                    assert vector["fields"]["key_" + value] == value, "field key/value changed"
                elif kind == "lifecycle_private":
                    assert decoded["message_vector"]["fields"]["private_detail"] == value, \
                        "lifecycle private detail changed"
                else:
                    assert "private_detail" not in decoded["message_vector"]["fields"], \
                        "lifecycle public projection exposed private detail"
            except (ValueError, AssertionError, KeyError, TypeError) as error:
                failures += 1
                if failures <= 12:
                    print(f"FAIL {name}/{kind}: {error}")
    if offset != len(run.stdout) or observed != records:
        failures += 1
        print("FAIL missing or extra producer output")
    print(f"observed_records={observed} failures={failures} canonical_acceptance=0")
    return 1 if failures else 0


if __name__ == "__main__":
    raise SystemExit(main())
