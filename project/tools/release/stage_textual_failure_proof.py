#!/usr/bin/env python3
# Copyright (c) 2026 ScratchBird Software Inc.
#
# This Source Code Form is subject to the terms of the Mozilla Public
# License, v. 2.0. If a copy of the MPL was not distributed with this
# file, You can obtain one at https://mozilla.org/MPL/2.0/.
#
# SPDX-License-Identifier: MPL-2.0

"""Materialize a small, textual failure-proof artifact without payload bytes.

GitHub Actions ``if: always()`` uploads are useful for diagnostics, but an
installer smoke directory commonly contains an extracted server payload.  This
tool creates a new root containing only bounded UTF-8 proof files and an
inventory.  It never follows links and deliberately excludes all known
installed-payload envelopes, so a failed job cannot publish a raw executable
tree as a troubleshooting artifact.
"""

from __future__ import annotations

import argparse
import hashlib
import json
from pathlib import Path
import shutil
import sys
import tempfile
from typing import Any


MAX_TEXTUAL_PROOF_BYTES = 8 * 1024 * 1024
TEXTUAL_SUFFIXES = frozenset({".json", ".log", ".txt", ".xml", ".csv", ".md"})
# These names cover the canonical portable/system extraction envelopes on all
# supported hosts.  Excluding them by path component prevents a harmless
# filename extension from making a release profile or resource tree look like
# a diagnostic artifact.
PAYLOAD_PATH_COMPONENTS = frozenset(
    {
        "opt",
        "etc",
        "usr",
        "library",
        "program files",
        "payload",
        "archive",
        "extract",
        "extraction",
        "pkg-expanded",
        "pkg-payload",
        "pkg-scripts",
        "lifecycle",
        "fresh",
        "upgraded",
    }
)


class ProofError(RuntimeError):
    """A failure-proof staging error."""


def fail(message: str) -> None:
    raise ProofError(message)


def sha256_bytes(value: bytes) -> str:
    return hashlib.sha256(value).hexdigest()


def safe_relative(path: Path, context: str) -> Path:
    if path.is_absolute() or any(part in {"", ".", ".."} for part in path.parts):
        fail(f"failure_proof_unsafe_relative:{context}:{path}")
    return path


def lexical_absolute(path: Path) -> Path:
    """Make *path* absolute without resolving a possible symlink."""

    return path if path.is_absolute() else Path.cwd() / path


def selected_textual_file(path: Path, relative: Path) -> bytes | None:
    """Return safe textual bytes, or omit a non-proof/payload candidate."""

    if path.is_symlink() or not path.is_file():
        return None
    if any(part.casefold() in PAYLOAD_PATH_COMPONENTS for part in relative.parts):
        return None
    if path.suffix.casefold() not in TEXTUAL_SUFFIXES:
        return None
    try:
        size = path.stat().st_size
    except OSError:
        return None
    if size < 0 or size > MAX_TEXTUAL_PROOF_BYTES:
        return None
    try:
        value = path.read_bytes()
        value.decode("utf-8")
    except (OSError, UnicodeDecodeError):
        return None
    # UTF-8 decoding alone is not enough: an ELF header and many archive
    # prefixes are technically decodable control bytes.  Treat NUL/control
    # data and recognized executable/package magic as binary evidence.
    if b"\0" in value or value.startswith(
        (
            b"\x7fELF",
            b"MZ",
            b"!<arch>\n",
            b"PK\x03\x04",
            b"\x1f\x8b",
        )
    ):
        return None
    if any(byte < 0x20 and byte not in {0x09, 0x0A, 0x0D} for byte in value):
        return None
    return value


def stage_source(
    source: Path,
    destination: Path,
    source_id: str,
) -> tuple[list[dict[str, Any]], dict[str, int]]:
    """Copy eligible UTF-8 proof rows from one source without following links."""

    rows: list[dict[str, Any]] = []
    skipped = {"missing_source": 0, "payload_or_nontext": 0}
    if not source.is_dir() or source.is_symlink():
        skipped["missing_source"] = 1
        (destination / "NO_TEXTUAL_PROOF_AVAILABLE.txt").write_text(
            "No textual diagnostics were available for this source.\n",
            encoding="utf-8",
        )
        return rows, skipped

    for candidate in sorted(source.rglob("*")):
        try:
            relative = safe_relative(candidate.relative_to(source), source_id)
        except ValueError:
            fail(f"failure_proof_source_escape:{source_id}")
        value = selected_textual_file(candidate, relative)
        if value is None:
            if candidate.is_file() or candidate.is_symlink():
                skipped["payload_or_nontext"] += 1
            continue
        target = destination / relative
        target.parent.mkdir(parents=True, exist_ok=True)
        target.write_bytes(value)
        rows.append(
            {
                "path": (Path(source_id) / relative).as_posix(),
                "bytes": len(value),
                "sha256": sha256_bytes(value),
            }
        )
    if not rows:
        (destination / "NO_TEXTUAL_PROOF_AVAILABLE.txt").write_text(
            "No bounded UTF-8 logs or structured proof files were available.\n",
            encoding="utf-8",
        )
    return rows, skipped


def stage(sources: list[Path], output_root: Path) -> None:
    if output_root.exists() or output_root.is_symlink():
        fail(f"failure_proof_output_root_not_empty:{output_root}")
    output_root.parent.mkdir(parents=True, exist_ok=True)
    rows: list[dict[str, Any]] = []
    skipped_by_source: dict[str, dict[str, int]] = {}
    with tempfile.TemporaryDirectory(
        prefix=f".{output_root.name}.failure-proof-", dir=output_root.parent
    ) as temp_name:
        staging = Path(temp_name) / "proof"
        staging.mkdir()
        for index, source in enumerate(sources, start=1):
            source_id = f"source-{index}"
            destination = staging / source_id
            destination.mkdir()
            source_rows, skipped = stage_source(source, destination, source_id)
            rows.extend(source_rows)
            skipped_by_source[source_id] = skipped
        rows.sort(key=lambda row: str(row["path"]))
        index = {
            "schema_id": "scratchbird.textual_failure_proof.v1",
            "policy": "utf8_text_only_no_extracted_payload_or_binary_tree",
            "sources": [f"source-{index}" for index in range(1, len(sources) + 1)],
            "files": rows,
            "skipped": skipped_by_source,
        }
        (staging / "FAILURE_PROOF_MANIFEST.json").write_text(
            json.dumps(index, indent=2, sort_keys=True) + "\n", encoding="utf-8"
        )
        staging.rename(output_root)


def main() -> int:
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument(
        "--source",
        type=Path,
        action="append",
        required=True,
        help="A diagnostics tree; repeat for multiple independent sources.",
    )
    parser.add_argument("--output-root", type=Path, required=True)
    args = parser.parse_args()
    try:
        stage(
            [lexical_absolute(source) for source in args.source],
            lexical_absolute(args.output_root),
        )
    except ProofError as exc:
        print(f"stage_textual_failure_proof=fail:{exc}", file=sys.stderr)
        return 1
    print(f"stage_textual_failure_proof=passed:{lexical_absolute(args.output_root)}")
    return 0


if __name__ == "__main__":
    raise SystemExit(main())
