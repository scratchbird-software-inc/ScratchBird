#!/usr/bin/env python3
# Copyright (c) 2026 ScratchBird Software Inc.
#
# This Source Code Form is subject to the terms of the Mozilla Public
# License, v. 2.0. If a copy of the MPL was not distributed with this
# file, You can obtain one at https://mozilla.org/MPL/2.0/.
#
# SPDX-License-Identifier: MPL-2.0

"""Verify a private ScratchBird pre-release packaging bundle."""

from __future__ import annotations

import argparse
import hashlib
import json
import os
from pathlib import Path
import re
import stat
import sys
from typing import Any


SCHEMA_ID = "scratchbird.prerelease_bundle_manifest.v1"
RELEASE_DATE_RE = re.compile(r"^[0-9]{4}[.][0-9]{2}[.][0-9]{2}$")
ALLOWED_TOP_LEVELS = {
    "docs",
    "installers",
    "proofs",
    "reference-parsers",
    "server",
    "source",
    "tools",
    "udr",
}
FORBIDDEN_CLIENT_TOP_LEVELS = frozenset(
    {"driver", "drivers", "adapter", "adapters", "adaptor", "adaptors", "mcp", "mcps"}
)
CLIENT_IDENTITY_MARKERS = (
    "dbeaver",
    "scratchbird_client",
    "scratchbird-client",
    "scratchbird_odbc",
    "scratchbird-odbc",
    "scratchbird_mojo_client_bridge",
    "scratchbird-mojo-client-bridge",
)
# This private manual-bundle verifier has no independently verified native
# artifact/profile admission.  It therefore accepts only the generated root
# metadata envelope and empty declared category directories.  An arbitrary
# document/proof can itself be source code, so no manually copied payload file
# is admissible here.
DOCUMENT_SUFFIXES = frozenset({
    ".conf",
    ".csv",
    ".ini",
    ".json",
    ".md",
    ".txt",
    ".xml",
    ".yaml",
    ".yml",
})
NARRATIVE_TOP_LEVELS = frozenset({"docs", "proofs"})
METADATA_ONLY_TOP_LEVELS = frozenset(
    {"installers", "reference-parsers", "server", "source", "tools", "udr"}
)
MAX_TEXT_PAYLOAD_BYTES = 2 * 1024 * 1024
REQUIRED_ROOT_FILES = (
    "FILE_LOCATION_MANIFEST.json",
    "README.md",
    "PRE_RELEASE_NOT_FINAL.txt",
    "RELEASE_MANIFEST.json",
    "SHA256SUMS",
)
FORBIDDEN_PATH_PARTS = {
    "." + "git",
    ".staging",
    "__pycache__",
    ".pytest_cache",
    "CMakeFiles",
    "Testing",
}
FORBIDDEN_TEXT_FRAGMENTS = (
    "ScratchBird" + "-Private",
    "/" + "home" + "/" + "dcalford",
    "/" + "local" + "_work",
)
CHECKSUM_EXCLUDE = {
    "RELEASE_MANIFEST.json",
    "SHA256SUMS",
}
GENERATED_METADATA_ROLE = "generated_private_prerelease_metadata"
GENERATED_METADATA_DESTINATION_HINT = "not_installable_metadata_attestation_only"


def repo_root_from_script() -> Path:
    return Path(__file__).resolve().parents[3]


def fail(message: str) -> None:
    print(f"verify_prerelease_packaging_bundle=fail:{message}", file=sys.stderr)
    raise SystemExit(1)


def sha256_file(path: Path) -> str:
    digest = hashlib.sha256()
    with path.open("rb") as handle:
        for chunk in iter(lambda: handle.read(1024 * 1024), b""):
            digest.update(chunk)
    return digest.hexdigest()


def native_binary_kind(path: Path) -> str | None:
    """Return the executable-container kind for a native binary, if known."""

    try:
        with path.open("rb") as handle:
            header = handle.read(8)
    except OSError as exc:
        fail(f"payload_read_failed:{path}:{exc}")
    if header.startswith(b"\x7fELF"):
        return "elf"
    if header.startswith(b"MZ"):
        return "pe"
    if header[:4] in {
        b"\xfe\xed\xfa\xce",
        b"\xce\xfa\xed\xfe",
        b"\xfe\xed\xfa\xcf",
        b"\xcf\xfa\xed\xfe",
        b"\xca\xfe\xba\xbe",
        b"\xbe\xba\xfe\xca",
        b"\xca\xfe\xba\xbf",
        b"\xbf\xba\xfe\xca",
    }:
        return "macho"
    return None


def read_utf8_text_payload(path: Path) -> str | None:
    """Return bounded, NUL-free UTF-8 text; reject all other opaque payloads."""

    try:
        if path.stat().st_size > MAX_TEXT_PAYLOAD_BYTES:
            return None
        contents = path.read_bytes()
    except OSError as exc:
        fail(f"payload_read_failed:{path}:{exc}")
    if b"\x00" in contents:
        return None
    try:
        return contents.decode("utf-8")
    except UnicodeDecodeError:
        return None


def validate_document_path(relative: str) -> None:
    """Require a closed documentation/proof/metadata layout for every payload."""

    parts = Path(relative).parts
    if not parts or parts[0] not in ALLOWED_TOP_LEVELS:
        fail(f"invalid_payload_category:{relative}")
    if Path(relative).suffix.casefold() not in DOCUMENT_SUFFIXES:
        fail(f"payload_extension_forbidden:{relative}")
    if parts[0] in NARRATIVE_TOP_LEVELS:
        return
    if parts[0] in METADATA_ONLY_TOP_LEVELS and len(parts) >= 3 and parts[1] == "metadata":
        return
    fail(f"metadata_path_required:{relative}")


def validate_payload_file(
    path: Path,
    relative: str,
    *,
    enforce_document_path: bool = True,
) -> str:
    """Reject every binary/archive and executable payload in manual bundles."""

    binary_kind = native_binary_kind(path)
    if binary_kind is not None:
        fail(f"binary_payload_forbidden:path:{relative}:{binary_kind}")
    try:
        mode = path.stat(follow_symlinks=False).st_mode
    except OSError as exc:
        fail(f"payload_stat_failed:{relative}:{exc}")
    if mode & (stat.S_IXUSR | stat.S_IXGRP | stat.S_IXOTH):
        fail(f"executable_payload_forbidden:path:{relative}")
    text = read_utf8_text_payload(path)
    if text is None:
        fail(f"binary_payload_forbidden:path:{relative}:opaque")
    if text.startswith("#!"):
        fail(f"executable_text_payload_forbidden:path:{relative}")
    if enforce_document_path:
        validate_document_path(relative)
    return text


def release_tree_entries(release_root: Path) -> list[Path]:
    """Enumerate every entry without following directory links."""

    entries: list[Path] = []

    def visit(directory: Path) -> None:
        try:
            children = sorted(os.scandir(directory), key=lambda entry: entry.name)
        except OSError as exc:
            fail(f"release_tree_read_failed:{directory}:{exc}")
        for child in children:
            path = Path(child.path)
            entries.append(path)
            if child.is_symlink():
                continue
            if child.is_dir(follow_symlinks=False):
                visit(path)

    visit(release_root)
    return entries


def load_json(path: Path) -> dict[str, Any]:
    try:
        data = json.loads(path.read_text(encoding="utf-8"))
    except (OSError, json.JSONDecodeError) as exc:
        fail(f"manifest_invalid:{exc}")
    if not isinstance(data, dict):
        fail("manifest_not_object")
    return data


def client_payload_identity(value: str) -> bool:
    lowered = value.casefold().replace("\\", "/")
    if any(marker in lowered for marker in CLIENT_IDENTITY_MARKERS):
        return True
    components = [part for part in re.split(r"[/._-]+", lowered) if part]
    return any(component in FORBIDDEN_CLIENT_TOP_LEVELS for component in components)


def client_text_identity(value: str) -> bool:
    lowered = value.casefold()
    return any(marker in lowered for marker in CLIENT_IDENTITY_MARKERS)


def expected_prerelease_marker() -> str:
    return (
        "ScratchBird private pre-release metadata attestation.\n"
        "This bundle is for internal/test coordination only and is not an official release.\n"
        "It contains no installable payload, driver, adaptor, MCP component, or DBeaver artifact.\n"
    )


def expected_readme(release_date: str) -> str:
    return (
        f"# ScratchBird Pre-Release Metadata Attestation {release_date}\n\n"
        "This directory records a non-publishable, metadata-only manual pre-release state.\n"
        "Native tester artifacts must be generated through the verified installer release route.\n"
    )


def parse_sha256sums(path: Path) -> dict[str, str]:
    rows: dict[str, str] = {}
    text = path.read_text(encoding="utf-8")
    for lineno, line in enumerate(text.splitlines(), start=1):
        if not line.strip():
            continue
        if "  " not in line:
            fail(f"sha256sums_invalid_line:{lineno}")
        digest, rel = line.split("  ", 1)
        if not re.fullmatch(r"[0-9a-f]{64}", digest):
            fail(f"sha256sums_invalid_digest:{lineno}")
        if rel in rows:
            fail(f"sha256sums_duplicate:{rel}")
        rows[rel] = digest
    return rows


def collect_files(root: Path) -> dict[str, Path]:
    files: dict[str, Path] = {}
    for path in sorted(item for item in root.rglob("*") if item.is_file()):
        rel = path.relative_to(root).as_posix()
        if rel in CHECKSUM_EXCLUDE:
            continue
        files[rel] = path
    return files


def validate_root(repo_root: Path, release_root: Path) -> str:
    resolved = release_root.resolve()
    packaging_root = (repo_root / "packaging").resolve()
    if packaging_root not in resolved.parents:
        fail(f"release_root_not_under_packaging:{resolved}")
    release_date = resolved.name
    if not RELEASE_DATE_RE.match(release_date):
        fail(f"invalid_release_date_directory:{release_date}")
    return release_date


def scan_paths_and_text(release_root: Path, release_date: str) -> None:
    """Require the closed generated metadata envelope, with no payload files."""

    actual_categories: set[str] = set()
    actual_root_files: set[str] = set()
    for path in release_tree_entries(release_root):
        relative = path.relative_to(release_root)
        rel = relative.as_posix()
        if path.is_symlink():
            fail(f"symlink_forbidden:{rel}")
        if any(part in FORBIDDEN_PATH_PARTS for part in relative.parts):
            fail(f"forbidden_path_part:{rel}")
        if len(relative.parts) != 1:
            fail(f"manual_bundle_payload_entry_forbidden:{rel}")
        try:
            mode = path.stat(follow_symlinks=False).st_mode
        except OSError as exc:
            fail(f"release_tree_stat_failed:{rel}:{exc}")
        if stat.S_ISDIR(mode):
            if rel not in ALLOWED_TOP_LEVELS:
                fail(f"unknown_top_level_category:{rel}")
            actual_categories.add(rel)
            continue
        if not stat.S_ISREG(mode):
            fail(f"unsupported_payload_type:{rel}")
        if rel not in REQUIRED_ROOT_FILES:
            fail(f"unknown_root_file:{rel}")
        text = validate_payload_file(path, rel, enforce_document_path=False)
        for fragment in FORBIDDEN_TEXT_FRAGMENTS:
            if fragment in text:
                fail(f"private_reference_fragment:{rel}:{fragment}")
        if rel == "README.md" and text != expected_readme(release_date):
            fail("manual_bundle_generated_text_mismatch:README.md")
        if rel == "PRE_RELEASE_NOT_FINAL.txt" and text != expected_prerelease_marker():
            fail("manual_bundle_generated_text_mismatch:PRE_RELEASE_NOT_FINAL.txt")
        actual_root_files.add(rel)
    if actual_categories != ALLOWED_TOP_LEVELS:
        fail(
            "manual_bundle_category_set_mismatch:"
            f"missing={sorted(ALLOWED_TOP_LEVELS - actual_categories)}:"
            f"unexpected={sorted(actual_categories - ALLOWED_TOP_LEVELS)}"
        )
    if actual_root_files != set(REQUIRED_ROOT_FILES):
        fail(
            "manual_bundle_root_file_set_mismatch:"
            f"missing={sorted(set(REQUIRED_ROOT_FILES) - actual_root_files)}:"
            f"unexpected={sorted(actual_root_files - set(REQUIRED_ROOT_FILES))}"
        )


def validate_manifest(
    release_root: Path,
    release_date: str,
    manifest: dict[str, Any],
    files: dict[str, Path],
    allow_empty: bool,
) -> None:
    if manifest.get("schema_id") != SCHEMA_ID:
        fail("manifest_schema_mismatch")
    if manifest.get("channel") != "prerelease":
        fail("manifest_channel_must_be_prerelease")
    if manifest.get("release_date") != release_date:
        fail("manifest_release_date_mismatch")
    if manifest.get("pre_release_not_final") is not True:
        fail("manifest_missing_prerelease_flag")
    policy = manifest.get("policy")
    if not isinstance(policy, dict) or policy.get("official_release") is not False:
        fail("manifest_official_release_policy_invalid")
    if policy.get("history_cleanup_required_before_public_release") is not True:
        fail("manifest_missing_history_cleanup_policy")
    if policy.get("component_admission_controller") != "native_server_only":
        fail("manifest_component_admission_controller_invalid")
    if policy.get("client_artifacts_permitted") is not False:
        fail("manifest_client_artifact_policy_invalid")
    if policy.get("admitted_driver_adaptor_mcp_components") != []:
        fail("manifest_driver_adaptor_mcp_admission_policy_invalid")
    if policy.get("dbeaver_hard_excluded") is not True:
        fail("manifest_dbeaver_exclusion_policy_invalid")
    if policy.get("manual_bundle_binary_payloads_permitted") is not False:
        fail("manifest_manual_binary_payload_policy_invalid")
    if policy.get("manual_bundle_payload_files_permitted") is not False:
        fail("manifest_manual_payload_files_policy_invalid")
    if policy.get("manual_bundle_external_copy_permitted") is not False:
        fail("manifest_manual_external_copy_policy_invalid")
    categories = set(manifest.get("categories", []))
    if categories != ALLOWED_TOP_LEVELS:
        fail("manifest_categories_mismatch")
    if manifest.get("promoted_paths") != []:
        fail("manifest_promoted_paths_must_be_empty")
    artifact_rows = manifest.get("artifacts")
    if not isinstance(artifact_rows, list):
        fail("manifest_artifacts_not_list")
    manifest_paths: set[str] = set()
    for row in artifact_rows:
        if not isinstance(row, dict):
            fail("manifest_artifact_not_object")
        rel = row.get("path")
        if not isinstance(rel, str) or rel not in files:
            fail(f"manifest_artifact_missing_file:{rel}")
        if rel in manifest_paths:
            fail(f"manifest_artifact_duplicate:{rel}")
        manifest_paths.add(rel)
        if row.get("sha256") != sha256_file(files[rel]):
            fail(f"manifest_artifact_checksum_mismatch:{rel}")
        if row.get("bytes") != files[rel].stat().st_size:
            fail(f"manifest_artifact_size_mismatch:{rel}")
    if set(files) != manifest_paths:
        missing = sorted(set(files) - manifest_paths)
        extra = sorted(manifest_paths - set(files))
        if missing:
            fail(f"manifest_missing_files:{','.join(missing[:10])}")
        if extra:
            fail(f"manifest_extra_files:{','.join(extra[:10])}")
    payload_files = [
        rel for rel in files
        if not rel.endswith("/README.md") and rel not in {"README.md", "PRE_RELEASE_NOT_FINAL.txt"}
    ]
    if not allow_empty and not payload_files:
        fail("bundle_has_no_payload_files")


def validate_sha256sums(rows: dict[str, str], files: dict[str, Path]) -> None:
    if set(rows) != set(files):
        missing = sorted(set(files) - set(rows))
        extra = sorted(set(rows) - set(files))
        if missing:
            fail(f"sha256sums_missing_files:{','.join(missing[:10])}")
        if extra:
            fail(f"sha256sums_extra_files:{','.join(extra[:10])}")
    for rel, path in files.items():
        if rows[rel] != sha256_file(path):
            fail(f"sha256sums_checksum_mismatch:{rel}")


def validate_file_location_manifest(release_root: Path, release_date: str, files: dict[str, Path]) -> None:
    manifest = load_json(release_root / "FILE_LOCATION_MANIFEST.json")
    if manifest.get("schema_id") != "scratchbird.prerelease_file_location_manifest.v1":
        fail("file_location_manifest_schema_mismatch")
    if manifest.get("release_date") != release_date:
        fail("file_location_manifest_release_date_mismatch")
    rows = manifest.get("files")
    if not isinstance(rows, list):
        fail("file_location_manifest_files_not_list")
    expected_paths = set(files) - {"FILE_LOCATION_MANIFEST.json"}
    seen: set[str] = set()
    for row in rows:
        if not isinstance(row, dict):
            fail("file_location_manifest_row_not_object")
        rel = row.get("path")
        if not isinstance(rel, str) or rel not in expected_paths:
            fail(f"file_location_manifest_missing_file:{rel}")
        if rel in seen:
            fail(f"file_location_manifest_duplicate:{rel}")
        seen.add(rel)
        if row.get("role") != GENERATED_METADATA_ROLE:
            fail(f"file_location_manifest_role_invalid:{rel}")
        if row.get("installer_destination_hint") != GENERATED_METADATA_DESTINATION_HINT:
            fail(f"file_location_manifest_destination_hint_invalid:{rel}")
        if row.get("sha256") != sha256_file(files[rel]):
            fail(f"file_location_manifest_checksum_mismatch:{rel}")
        if row.get("bytes") != files[rel].stat().st_size:
            fail(f"file_location_manifest_size_mismatch:{rel}")
    if seen != expected_paths:
        missing = sorted(expected_paths - seen)
        extra = sorted(seen - expected_paths)
        if missing:
            fail(f"file_location_manifest_missing_paths:{','.join(missing[:10])}")
        if extra:
            fail(f"file_location_manifest_extra_paths:{','.join(extra[:10])}")


def main() -> int:
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument("release_root", type=Path)
    parser.add_argument("--repo-root", type=Path, default=repo_root_from_script())
    parser.add_argument("--allow-empty", action="store_true")
    args = parser.parse_args()

    repo_root = args.repo_root.resolve()
    requested_release_root = (
        args.release_root if args.release_root.is_absolute() else Path.cwd() / args.release_root
    )
    if requested_release_root.is_symlink():
        fail(f"release_root_symlink_forbidden:{requested_release_root}")
    release_root = requested_release_root.resolve()
    release_date = validate_root(repo_root, release_root)
    for rel in REQUIRED_ROOT_FILES:
        if not (release_root / rel).is_file():
            fail(f"missing_root_file:{rel}")
    marker = (release_root / "PRE_RELEASE_NOT_FINAL.txt").read_text(encoding="utf-8").lower()
    if "pre-release" not in marker and "pre release" not in marker:
        fail("prerelease_marker_missing_text")
    for category in sorted(ALLOWED_TOP_LEVELS):
        if not (release_root / category).is_dir():
            fail(f"missing_category_dir:{category}")
    scan_paths_and_text(release_root, release_date)
    files = collect_files(release_root)
    manifest = load_json(release_root / "RELEASE_MANIFEST.json")
    validate_manifest(release_root, release_date, manifest, files, args.allow_empty)
    validate_file_location_manifest(release_root, release_date, files)
    validate_sha256sums(parse_sha256sums(release_root / "SHA256SUMS"), files)
    print(f"verify_prerelease_packaging_bundle=pass:{release_root.relative_to(repo_root).as_posix()}")
    return 0


if __name__ == "__main__":
    raise SystemExit(main())
