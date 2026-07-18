#!/usr/bin/env python3
# Copyright (c) 2026 ScratchBird Software Inc.
#
# This Source Code Form is subject to the terms of the Mozilla Public
# License, v. 2.0. If a copy of the MPL was not distributed with this
# file, You can obtain one at https://mozilla.org/MPL/2.0/.
#
# SPDX-License-Identifier: MPL-2.0

"""Public-export policy for locally acquired reference-regression material.

The acquisition tree is a local-only payload boundary.  A public source export
may contain only the exact, hash-bound metadata files registered below.  Raw
suite files, downloaded tools, source trees, and even unregistered direct
``*_MANIFEST.csv`` files remain private inputs.  Adding or changing public
metadata requires an intentional registry update in this file.
"""

from __future__ import annotations

import hashlib
from pathlib import Path
from typing import Iterator


REFERENCE_ACQUISITION_PREFIX = (
    "project/tests/reference_regression/reference_release_acquisition"
)
_PREFIX_PARTS = tuple(REFERENCE_ACQUISITION_PREFIX.split("/"))
MAX_PUBLIC_REFERENCE_ACQUISITION_METADATA_BYTES = 1024 * 1024

_PUBLIC_REGRESSION_SCOPE_SHA256 = (
    "1a927e6dd7d6a264fcb5d17bf9d34d2ed9777a6e4e4af081f9b539e9da71eff9"
)
_PUBLIC_REGRESSION_SCOPE_RELEASES = (
    "apache_ignite/2.17.0",
    "cassandra/5.0.8",
    "clickhouse/25.12.10.7-stable",
    "cockroachdb/26.1.3",
    "dolt/1.86.6",
    "duckdb/1.5.2",
    "firebird/5.0.4",
    "foundationdb/7.3.77",
    "immudb/1.11.0",
    "influxdb/3.9.0",
    "mariadb/12.2.2",
    "milvus/2.6.5",
    "mongodb/8.2.6",
    "mysql/8.4.8",
    "mysql/9.7.0",
    "neo4j/2026.04.0",
    "opensearch/3.6.0",
    "postgresql/18.3",
    "redis/8.6.2",
    "sqlite/3.53.0",
    "tidb/8.5.6",
    "tikv/8.1.2",
    "tikv/8.5.6",
    "vitess/23.0.3",
    "xtdb/2.1.0",
    "yugabytedb/2025.2.2.2",
)

# This is the sole authority for public files below the otherwise private
# acquisition tree.  Keys are relative to REFERENCE_ACQUISITION_PREFIX and
# values bind the exact public bytes.  Do not replace this registry with a
# filename suffix rule: that would permit renamed raw payloads into exports.
PUBLIC_REFERENCE_ACQUISITION_METADATA_SHA256: dict[str, str] = {
    **{
        f"{release}/regression/PUBLIC_REGRESSION_SCOPE.md":
        _PUBLIC_REGRESSION_SCOPE_SHA256
        for release in _PUBLIC_REGRESSION_SCOPE_RELEASES
    },
    "firebird/5.0.4/regression/FIREBIRD_QA_CANDIDATE.md": (
        "d4643cdc263b74774f6b82d095ddb371fb4c84d9ec16f442f7c0062e31c4fb7d"
    ),
    "firebird/5.0.4/regression/FIREBIRD_QA_CANDIDATE_ASSET_HASH_MANIFEST.csv": (
        "e6791254c8bff660f0759b1ae6ee46f95fe3063f55f4f44227a852652da2f632"
    ),
    "firebird/5.0.4/regression/FIREBIRD_QA_CANDIDATE_TEST_INDEX.csv": (
        "10fc3e1b3c1831d74c97da80d272e643fcb41cbceb2771275b089a0f0be0644d"
    ),
    "firebird/5.0.4/regression/FIREBIRD_QA_CANONICAL_SCOPE_MANIFEST.csv": (
        "b66304a948bc96f8fde6735d0e3cc3a380a7a278736c60ba4fee360a3d828f6e"
    ),
    "firebird/5.0.4/regression/FIREBIRD_QA_CANONICAL_TEST_MANIFEST.csv": (
        "5b0aa5a61fc11917c100c779a79d16a1ce7288b571142181356dec3d416e521b"
    ),
    "firebird/5.0.4/regression/FIREBIRD_QA_REFERENCE_REPLAY_FAMILY_MANIFEST.csv": (
        "5fe70a289741b7929226ad836bf3e6d7af3f7ceceb8db15ce438c4cd63fe336c"
    ),
    "firebird/5.0.4/regression/FIREBIRD_QA_REFERENCE_REPLAY_MANIFEST.csv": (
        "c61b4792e7a7c60ed021393d6260aa764964c3ae03b36d3eac910bf04609e7f8"
    ),
    "firebird/5.0.4/regression/SOURCE_POINTERS.md": (
        "75a7958510c576172015e1ef9bb3ba1cca77e7fffddcd22b34e90b9c81557adb"
    ),
}


def normalized_parts(relative_path: str | Path) -> tuple[str, ...]:
    """Return a repository-relative path as portable slash-separated parts."""

    value = str(relative_path).replace("\\", "/").strip("/")
    return tuple(part for part in value.split("/") if part and part != ".")


def _metadata_key(relative_path: str | Path) -> str | None:
    parts = normalized_parts(relative_path)
    if parts[: len(_PREFIX_PARTS)] != _PREFIX_PARTS:
        return None
    return "/".join(parts[len(_PREFIX_PARTS) :])


def is_under_reference_acquisition(relative_path: str | Path) -> bool:
    return _metadata_key(relative_path) is not None


def is_public_reference_acquisition_metadata(relative_path: str | Path) -> bool:
    """Return whether ``relative_path`` is an exact registered metadata path."""

    key = _metadata_key(relative_path)
    return key in PUBLIC_REFERENCE_ACQUISITION_METADATA_SHA256


def public_reference_acquisition_metadata_relative_paths() -> tuple[str, ...]:
    """Return the complete ordered registry as repository-relative paths."""

    return tuple(
        f"{REFERENCE_ACQUISITION_PREFIX}/{key}"
        for key in sorted(PUBLIC_REFERENCE_ACQUISITION_METADATA_SHA256)
    )


def public_reference_acquisition_metadata_validation_error(
    candidate: Path, relative_path: str | Path
) -> str | None:
    """Return a fail-closed validation error for one registered metadata file."""

    key = _metadata_key(relative_path)
    expected_sha256 = (
        None if key is None else PUBLIC_REFERENCE_ACQUISITION_METADATA_SHA256.get(key)
    )
    if expected_sha256 is None:
        return "unregistered_metadata_path"
    if candidate.is_symlink():
        return "metadata_must_not_be_symlink"
    if not candidate.is_file():
        return "metadata_must_be_regular_file"
    try:
        payload = candidate.read_bytes()
    except OSError as exc:
        return f"metadata_read_failed:{exc.__class__.__name__}"
    if len(payload) > MAX_PUBLIC_REFERENCE_ACQUISITION_METADATA_BYTES:
        return "metadata_exceeds_public_size_limit"
    actual_sha256 = hashlib.sha256(payload).hexdigest()
    if actual_sha256 != expected_sha256:
        return "metadata_content_hash_mismatch"
    return None


def validate_public_reference_acquisition_metadata_inventory(
    repo_root: Path,
) -> tuple[str, ...]:
    """Return every missing, non-regular, oversized, or altered registry file."""

    errors: list[str] = []
    for relative in public_reference_acquisition_metadata_relative_paths():
        error = public_reference_acquisition_metadata_validation_error(
            repo_root / relative, relative
        )
        if error is not None:
            errors.append(f"{relative}:{error}")
    return tuple(errors)


def iter_public_reference_acquisition_metadata(repo_root: Path) -> Iterator[Path]:
    """Yield only the registered direct metadata files in deterministic order."""

    for relative in public_reference_acquisition_metadata_relative_paths():
        yield repo_root / relative
