#!/usr/bin/env python3
# Copyright (c) 2026 ScratchBird Software Inc.
#
# This Source Code Form is subject to the terms of the Mozilla Public
# License, v. 2.0. If a copy of the MPL was not distributed with this
# file, You can obtain one at https://mozilla.org/MPL/2.0/.
#
# SPDX-License-Identifier: MPL-2.0

"""Build and enforce the public ScratchBird spec-implementation gap registry.

The input inventory is a human-readable audit report. This tool turns it into
stable machine-readable release evidence and fails the release gate until every
non-private entry is closed as Implemented in Full.
"""

from __future__ import annotations

import argparse
import csv
import hashlib
import json
import re
import sys
from dataclasses import dataclass, field
from pathlib import Path
from typing import Iterable


STATUS_MAP = {
    "Implemented in Full": "implemented_in_full",
    "Partial": "partial",
    "Drift": "drift",
    "Not Implemented": "not_implemented",
    "Private": "private",
}

OPEN_PUBLIC_STATUSES = {"partial", "drift", "not_implemented"}
TARGET_EVIDENCE_COLUMNS = {
    "gap_id",
    "registry_id",
    "target_group",
    "required_ctest_labels",
    "evidence_artifacts",
    "implementation_refs_required",
    "closure_rule",
    "status",
}
AGENT_STATUS_COLUMNS = [
    "timestamp_utc",
    "agent",
    "phase",
    "current_slice",
    "status",
    "blocked_by",
    "last_update",
    "next_action",
    "evidence_refs",
]
IMPLEMENTATION_AHEAD_ALLOWED_CLASSES = {
    "accepted_and_specified",
    "guarded_until_specified",
    "removed_or_refused",
}
IMPLEMENTATION_AHEAD_CLOSED_STATUSES = {"completed"}
GAP_ID_PATTERN = re.compile(r"^SB-PUBLIC-GAP-(\d{4})$")
SBSQL_DEPRECATED_NAME_PATTERN = re.compile(r"native(?:[_ -]?v3)", re.IGNORECASE)

SBSQL_NAME_DRIFT_SCAN_ROOTS = [
    "public_release_evidence",
    "project/drivers",
    "project/src",
    "project/tests",
]
SBSQL_NAME_DRIFT_ALLOWED_PATH_PREFIXES = (
    "project/src/parsers/native/v3/",
)
SBSQL_NAME_DRIFT_ALLOWED_LINES = {
    "public_contract_snapshot": (
        "TERM-SBSQL-CANONICAL-PRODUCT-NAME",
        "TERM-NATIVE-V3-DEPRECATED-ALIAS",
    ),
    "public_contract_snapshot": (
        "project/src/parsers/native/v3/",
    ),
    "public_contract_snapshot": (
        "deprecated compatibility alias",
        "MCP.DB_CONNECT_INTENT_UNSUPPORTED",
    ),
    "project/src/manager/node/manager_runtime.cpp": (
        "kDeprecatedNativeV3ProfileAlias",
    ),
    "project/src/storage/database/database_lifecycle.cpp": (
        '"legacy_alias", "native_v3_parser_package"',
    ),
    "project/tests/listener/mga_transaction_authority_gate.py": (
        "src/parsers/native/v3/package/native_v3_parser_package.cpp",
    ),
    "public_input_snapshot": (
        "original_row_preserved",
        "native v3",
        "appendix-native-v3",
    ),
    "public_input_snapshot": (
        "original_row_preserved",
        "native v3",
        "appendix-native-v3",
    ),
}


@dataclass
class InventoryEntry:
    title: str
    section: str
    title_line: int
    description_lines: list[str] = field(default_factory=list)
    status_raw: str = ""
    source_raw: str = ""
    implementation_raw: str = ""

    @property
    def status(self) -> str:
        return STATUS_MAP.get(self.status_raw, "unknown")

    @property
    def description(self) -> str:
        return " ".join(line.strip() for line in self.description_lines if line.strip()).strip()


def sha256_file(path: Path) -> str:
    digest = hashlib.sha256()
    with path.open("rb") as handle:
        for chunk in iter(lambda: handle.read(1024 * 1024), b""):
            digest.update(chunk)
    return digest.hexdigest()


def slugify(value: str) -> str:
    slug = re.sub(r"[^a-z0-9]+", "-", value.lower()).strip("-")
    return slug or "unnamed"


def split_refs(raw: str) -> list[str]:
    if not raw:
        return []
    return [part.strip() for part in raw.split(",") if part.strip()]


def infer_area(source_refs: list[str], title: str) -> str:
    for ref in source_refs:
        marker = "public_contract_snapshot"
        if marker in ref:
            rest = ref.split(marker, 1)[1]
            first = rest.split("/", 1)[0]
            first = first.removesuffix(".md")
            return slugify(first)
        marker = "public_release_evidence"
        if marker in ref:
            rest = ref.split(marker, 1)[1]
            first = rest.split("/", 1)[0]
            return slugify(first)
    return slugify(title.split()[0] if title else "unknown")


def parse_inventory(path: Path) -> list[InventoryEntry]:
    entries: list[InventoryEntry] = []
    section = ""
    current: InventoryEntry | None = None

    for line_number, line in enumerate(path.read_text(encoding="utf-8").splitlines(), start=1):
        if line.startswith("## ") and not line.startswith("### "):
            section = line[3:].strip()
            continue
        if line.startswith("### "):
            if current is not None:
                entries.append(current)
            current = InventoryEntry(
                title=line[4:].strip(),
                section=section,
                title_line=line_number,
            )
            continue
        if current is None:
            continue
        if line.startswith("**Status:**"):
            current.status_raw = line.split("**Status:**", 1)[1].strip()
        elif line.startswith("**Source documents:**"):
            current.source_raw = line.split("**Source documents:**", 1)[1].strip()
        elif line.startswith("**Implementation:**"):
            current.implementation_raw = line.split("**Implementation:**", 1)[1].strip()
        elif line and not line.startswith("**"):
            current.description_lines.append(line)

    if current is not None:
        entries.append(current)

    return entries


def entry_scope(entry: InventoryEntry) -> str:
    if entry.status == "private" or "private" in entry.section.lower():
        return "private_cluster"
    return "public_single_node"


def release_state(entry: InventoryEntry) -> str:
    scope = entry_scope(entry)
    if scope != "public_single_node":
        return "excluded_private"
    if entry.status == "implemented_in_full":
        return "closed"
    if entry.status in OPEN_PUBLIC_STATUSES:
        return "open"
    return "invalid"


def priority(entry: InventoryEntry) -> str:
    if entry_scope(entry) != "public_single_node":
        return "private"
    if entry.status == "implemented_in_full":
        return "closed"
    if entry.status == "drift":
        return "P0"
    if entry.status == "not_implemented":
        return "P1"
    if entry.status == "partial":
        return "P2"
    return "invalid"


def build_registry(inventory_path: Path) -> dict:
    entries = parse_inventory(inventory_path)
    rows = []
    public_gap_count = 0

    for index, entry in enumerate(entries, start=1):
        source_refs = split_refs(entry.source_raw)
        implementation_refs = split_refs(entry.implementation_raw)
        scope = entry_scope(entry)
        state = release_state(entry)
        gap_id = ""
        if scope == "public_single_node" and state == "open":
            public_gap_count += 1
            gap_id = f"SB-PUBLIC-GAP-{public_gap_count:04d}"

        rows.append(
            {
                "registry_id": f"SB-SPEC-IMPLEMENTATION-{index:04d}",
                "gap_id": gap_id,
                "title": entry.title,
                "slug": slugify(entry.title),
                "area": infer_area(source_refs, entry.title),
                "section": entry.section,
                "scope": scope,
                "status": entry.status,
                "status_raw": entry.status_raw,
                "public_release_state": state,
                "public_release_required": scope == "public_single_node",
                "priority": priority(entry),
                "inventory_line": entry.title_line,
                "source_documents": source_refs,
                "implementation_refs": implementation_refs,
                "gap_summary": entry.description,
            }
        )

    status_counts: dict[str, int] = {}
    scope_counts: dict[str, int] = {}
    release_counts: dict[str, int] = {}
    for row in rows:
        status_counts[row["status"]] = status_counts.get(row["status"], 0) + 1
        scope_counts[row["scope"]] = scope_counts.get(row["scope"], 0) + 1
        release_counts[row["public_release_state"]] = release_counts.get(row["public_release_state"], 0) + 1

    return {
        "schema": "scratchbird.public_spec_implementation_gap_registry.v1",
        "source_inventory": str(inventory_path),
        "source_inventory_sha256": sha256_file(inventory_path),
        "status_authority": {
            "release_required_scope": "public_single_node",
            "release_closed_status": "implemented_in_full",
            "release_open_statuses": sorted(OPEN_PUBLIC_STATUSES),
            "excluded_status": "private",
        },
        "summary": {
            "total_entries": len(rows),
            "status_counts": status_counts,
            "scope_counts": scope_counts,
            "release_state_counts": release_counts,
            "public_required_entries": scope_counts.get("public_single_node", 0),
            "public_open_entries": release_counts.get("open", 0),
        },
        "entries": rows,
    }


def write_json(path: Path, registry: dict) -> None:
    path.parent.mkdir(parents=True, exist_ok=True)
    path.write_text(json.dumps(registry, indent=2, sort_keys=True) + "\n", encoding="utf-8")


def write_csv(path: Path, registry: dict) -> None:
    path.parent.mkdir(parents=True, exist_ok=True)
    fieldnames = [
        "registry_id",
        "gap_id",
        "title",
        "area",
        "scope",
        "status",
        "public_release_state",
        "public_release_required",
        "priority",
        "inventory_line",
        "source_documents",
        "implementation_refs",
    ]
    with path.open("w", encoding="utf-8", newline="") as handle:
        writer = csv.DictWriter(handle, fieldnames=fieldnames, lineterminator="\n")
        writer.writeheader()
        for row in registry["entries"]:
            csv_row = {field: row.get(field, "") for field in fieldnames}
            csv_row["source_documents"] = ";".join(row["source_documents"])
            csv_row["implementation_refs"] = ";".join(row["implementation_refs"])
            writer.writerow(csv_row)


def load_registry(path: Path) -> dict:
    return json.loads(path.read_text(encoding="utf-8"))


def read_csv_rows(path: Path) -> list[dict[str, str]]:
    with path.open("r", encoding="utf-8", newline="") as handle:
        return list(csv.DictReader(handle))


def split_semicolon(raw: str) -> list[str]:
    return [part.strip() for part in raw.split(";") if part.strip()]


def parse_simple_driver_checklist_registry(path: Path) -> list[dict[str, str]]:
    rows: list[dict[str, str]] = []
    current: dict[str, str] | None = None
    for raw_line in path.read_text(encoding="utf-8").splitlines():
        line = raw_line.rstrip()
        if line.startswith("  - id: "):
            if current is not None:
                rows.append(current)
            current = {"id": line.split(": ", 1)[1].strip()}
            continue
        if current is None:
            continue
        stripped = line.strip()
        for field in ("section", "capability", "requirement", "applies_to", "required_status_for_closure"):
            prefix = f"{field}: "
            if stripped.startswith(prefix):
                current[field] = stripped.split(": ", 1)[1].strip().strip("'").strip('"')
    if current is not None:
        rows.append(current)
    return rows


def driver_checklist_errors(
    registry_path: Path,
    target_rows_path: Path,
    *,
    require_closed: bool,
) -> list[str]:
    errors: list[str] = []
    registry_rows = parse_simple_driver_checklist_registry(registry_path)
    if not registry_rows:
        return [f"driver checklist registry has no rows: {registry_path}"]
    target_rows = read_csv_rows(target_rows_path)
    if not target_rows:
        return [f"driver checklist target row file has no rows: {target_rows_path}"]

    registry_by_id = {row.get("id", ""): row for row in registry_rows}
    target_by_id = {row.get("id", ""): row for row in target_rows}
    if len(registry_by_id) != len(registry_rows):
        errors.append("driver checklist registry contains duplicate ids")
    if len(target_by_id) != len(target_rows):
        errors.append("driver checklist target rows contain duplicate ids")

    missing = sorted(set(registry_by_id) - set(target_by_id))
    extra = sorted(set(target_by_id) - set(registry_by_id))
    if missing:
        errors.append(f"target rows missing registry ids: {missing[:10]}")
    if extra:
        errors.append(f"target rows contain unknown ids: {extra[:10]}")

    valid_requirements = {
        "required",
        "conditional",
        "required_where_host_has_async",
        "required_where_host_has_cancellation_tokens",
        "required_where_cluster_present",
        "required_for_admitted_methods",
    }
    for row in registry_rows:
        row_id = row.get("id", "")
        requirement = row.get("requirement", "")
        if requirement not in valid_requirements:
            errors.append(f"{row_id} has invalid requirement {requirement!r}")
        for field in ("section", "capability", "applies_to", "required_status_for_closure"):
            if not row.get(field):
                errors.append(f"{row_id} missing {field}")

    if require_closed:
        for row in target_rows:
            row_id = row.get("id", "")
            requirement = registry_by_id.get(row_id, {}).get("requirement", row.get("requirement", ""))
            closure = row.get("closure_status", "")
            spec_status = row.get("spec_status", "")
            implementation_status = row.get("implementation_status", "")
            test_status = row.get("test_status", "")
            if requirement == "conditional":
                if closure not in {"implemented_and_proven", "not_applicable_with_citation"}:
                    errors.append(f"{row_id} conditional row not closed: {closure}")
            elif closure != "implemented_and_proven":
                errors.append(f"{row_id} required row not implemented_and_proven: {closure}")
            for field, value in (
                ("spec_status", spec_status),
                ("implementation_status", implementation_status),
                ("test_status", test_status),
            ):
                if value in {"", "pending_reconcile", "pending_audit", "pending_evidence", "server_unspecified", "undocumented_implementation", "implemented_without_evidence", "not_started"}:
                    errors.append(f"{row_id} has non-closure {field}={value!r}")
    return errors


def repo_root_for_fixture(execution_plan_root: Path) -> Path:
    current = execution_plan_root.resolve()
    for candidate in (current, *current.parents):
        if (candidate / "project/CMakeLists.txt").exists():
            return candidate
    return execution_plan_root.parent.parent.parent


def execution_plan_path(execution_plan_root: Path, value: str) -> Path:
    if value.startswith("artifacts/"):
        return execution_plan_root / value
    path = Path(value)
    if path.is_absolute():
        return path
    parts = path.parts
    if len(parts) >= 4 and parts[0] == "docs" and parts[1] == "execution-plans":
        fixture_relative = execution_plan_root.joinpath(*parts[3:])
        if fixture_relative.exists():
            return fixture_relative
    return repo_root_for_fixture(execution_plan_root) / value


def target_gap_path(execution_plan_root: Path) -> Path:
    modern = execution_plan_root / "artifacts/TARGET_GAPS.csv"
    if modern.exists():
        return modern
    return execution_plan_root / "artifacts/PUBLIC_RELEASE_FOUNDATION_TARGET_GAPS.csv"


def registry_by_gap_id(registry: dict) -> dict[str, dict]:
    return {
        row["gap_id"]: row
        for row in registry.get("entries", [])
        if row.get("gap_id")
    }


def gap_id_number(gap_id: str) -> int:
    match = GAP_ID_PATTERN.fullmatch(gap_id or "")
    if match is None:
        return 0
    return int(match.group(1))


def read_gap_id_authority(path: Path | None) -> tuple[dict[str, str], list[str]]:
    if path is None:
        return {}, []
    if not path.exists():
        return {}, [f"missing gap-id authority {path}"]

    authority: dict[str, str] = {}
    errors: list[str] = []
    rows = read_csv_rows(path)
    seen_gap_ids: dict[str, str] = {}
    for row in rows:
        registry_id = row.get("registry_id", "").strip()
        gap_id = row.get("gap_id", "").strip()
        if not registry_id:
            errors.append(f"{path} has row missing registry_id")
            continue
        if not gap_id:
            continue
        if not GAP_ID_PATTERN.fullmatch(gap_id):
            errors.append(f"{registry_id} has invalid gap_id {gap_id!r}")
        if registry_id in authority and authority[registry_id] != gap_id:
            errors.append(
                f"{registry_id} has conflicting authority gap_id values "
                f"{authority[registry_id]!r} and {gap_id!r}"
            )
        previous_registry = seen_gap_ids.get(gap_id)
        if previous_registry is not None and previous_registry != registry_id:
            errors.append(
                f"gap_id {gap_id} is assigned to both {previous_registry} and {registry_id}"
            )
        authority[registry_id] = gap_id
        seen_gap_ids[gap_id] = registry_id
    return authority, errors


def collect_closure_overlays(execution_plan_roots: list[Path]) -> tuple[dict[str, dict[str, str]], list[str]]:
    overlays: dict[str, dict[str, str]] = {}
    errors: list[str] = []
    for execution_plan_root in execution_plan_roots:
        manifest = execution_plan_root / "artifacts/TARGET_EVIDENCE_MANIFEST.csv"
        if not manifest.exists():
            errors.append(f"missing closure evidence manifest {manifest}")
            continue
        for row in read_csv_rows(manifest):
            if row.get("status") != "implemented_in_full":
                continue
            registry_id = row.get("registry_id", "").strip()
            gap_id = row.get("gap_id", "").strip()
            if not registry_id:
                errors.append(f"{manifest} has closure row missing registry_id")
                continue
            if not gap_id:
                errors.append(f"{registry_id} closure row missing gap_id in {manifest}")
                continue
            if not GAP_ID_PATTERN.fullmatch(gap_id):
                errors.append(f"{registry_id} closure row has invalid gap_id {gap_id!r}")
                continue
            existing = overlays.get(registry_id)
            if existing is not None and existing.get("gap_id") != gap_id:
                errors.append(
                    f"{registry_id} has conflicting closure gap_id values "
                    f"{existing.get('gap_id')!r} and {gap_id!r}"
                )
                continue
            overlays[registry_id] = {
                "gap_id": gap_id,
                "execution_plan_root": str(execution_plan_root),
            }
    return overlays, errors


def apply_closure_overlays(registry: dict, overlays: dict[str, dict[str, str]]) -> list[str]:
    errors: list[str] = []
    rows_by_registry_id = {row.get("registry_id", ""): row for row in registry.get("entries", [])}
    for registry_id, overlay in sorted(overlays.items()):
        row = rows_by_registry_id.get(registry_id)
        if row is None:
            errors.append(f"closure overlay references unknown registry_id {registry_id}")
            continue
        if row.get("scope") != "public_single_node":
            errors.append(f"closure overlay references non-public registry_id {registry_id}")
            continue
        row["gap_id"] = overlay["gap_id"]
        row["status"] = "implemented_in_full"
        row["status_raw"] = "Implemented in Full"
        row["public_release_state"] = "closed"
        row["priority"] = "closed"
    return errors


def normalize_gap_ids(registry: dict, authority: dict[str, str]) -> list[str]:
    errors: list[str] = []
    entries = registry.get("entries", [])
    rows_by_registry_id = {row.get("registry_id", ""): row for row in entries}

    for registry_id, gap_id in authority.items():
        row = rows_by_registry_id.get(registry_id)
        if row is None:
            errors.append(f"gap-id authority references unknown registry_id {registry_id}")
            continue
        existing_gap_id = row.get("gap_id", "")
        if existing_gap_id and existing_gap_id != gap_id and row.get("public_release_state") == "closed":
            errors.append(
                f"{registry_id} gap_id changed from authority {gap_id} to {existing_gap_id}"
            )
            continue
        row["gap_id"] = gap_id

    used_gap_ids: dict[str, str] = {}
    max_gap_id = 0
    for row in entries:
        gap_id = row.get("gap_id", "")
        if not gap_id:
            continue
        registry_id = row.get("registry_id", "")
        if not GAP_ID_PATTERN.fullmatch(gap_id):
            errors.append(f"{registry_id} has invalid gap_id {gap_id!r}")
            row["gap_id"] = ""
            continue
        previous = used_gap_ids.get(gap_id)
        if previous is not None and previous != registry_id:
            row["gap_id"] = ""
            continue
        used_gap_ids[gap_id] = registry_id
        max_gap_id = max(max_gap_id, gap_id_number(gap_id))

    next_gap_id = max_gap_id + 1
    for row in entries:
        if not row.get("public_release_required") or row.get("public_release_state") != "open":
            continue
        if row.get("gap_id"):
            continue
        while True:
            candidate = f"SB-PUBLIC-GAP-{next_gap_id:04d}"
            next_gap_id += 1
            if candidate not in used_gap_ids:
                break
        row["gap_id"] = candidate
        used_gap_ids[candidate] = row.get("registry_id", "")

    return errors


def registry_errors(registry: dict, inventory_path: Path | None = None) -> list[str]:
    errors: list[str] = []
    if registry.get("schema") != "scratchbird.public_spec_implementation_gap_registry.v1":
        errors.append("registry schema mismatch")
    entries = registry.get("entries")
    if not isinstance(entries, list) or not entries:
        errors.append("registry has no entries")
        return errors

    seen_ids = set()
    valid_statuses = set(STATUS_MAP.values())
    for row in entries:
        registry_id = row.get("registry_id", "")
        if not registry_id:
            errors.append("registry row missing registry_id")
        elif registry_id in seen_ids:
            errors.append(f"duplicate registry_id {registry_id}")
        seen_ids.add(registry_id)
        if row.get("status") not in valid_statuses:
            errors.append(f"{registry_id} has unknown status {row.get('status')}")
        if row.get("scope") == "public_single_node" and not row.get("public_release_required"):
            errors.append(f"{registry_id} public row is not release-required")
        if row.get("scope") == "private_cluster" and row.get("public_release_required"):
            errors.append(f"{registry_id} private row is release-required")
        if row.get("public_release_required") and not row.get("source_documents"):
            errors.append(f"{registry_id} release-required row has no source_documents")
        gap_id = row.get("gap_id", "")
        if gap_id and not GAP_ID_PATTERN.fullmatch(gap_id):
            errors.append(f"{registry_id} has invalid gap_id {gap_id!r}")
        if row.get("public_release_required") and row.get("public_release_state") == "open" and not gap_id:
            errors.append(f"{registry_id} open public row has no stable gap_id")

    gap_id_owners: dict[str, str] = {}
    for row in entries:
        gap_id = row.get("gap_id", "")
        if not gap_id:
            continue
        registry_id = row.get("registry_id", "")
        previous = gap_id_owners.get(gap_id)
        if previous is not None and previous != registry_id:
            errors.append(f"gap_id {gap_id} is assigned to both {previous} and {registry_id}")
        gap_id_owners[gap_id] = registry_id

    summary = registry.get("summary", {})
    if summary.get("total_entries") != len(entries):
        errors.append("summary total_entries does not match entries length")

    if inventory_path is not None:
        expected_sha = sha256_file(inventory_path)
        actual_sha = registry.get("source_inventory_sha256")
        if actual_sha != expected_sha:
            errors.append(
                f"registry source hash is stale: registry={actual_sha} inventory={expected_sha}"
            )

    return errors


def contains_deprecated_sbsql_alias(value: object) -> bool:
    return isinstance(value, str) and SBSQL_DEPRECATED_NAME_PATTERN.search(value) is not None


def relative_repo_path(repo_root: Path, path: Path) -> str:
    try:
        return path.relative_to(repo_root).as_posix()
    except ValueError:
        return path.as_posix()


def line_is_allowed_sbsql_name_drift(rel_path: str, line: str) -> bool:
    if rel_path.startswith(SBSQL_NAME_DRIFT_ALLOWED_PATH_PREFIXES):
        normalized = line.lower()
        if Path(rel_path).suffix == ".md":
            return "native_v3" in normalized
        return "native_v3" in normalized or "nativev3" in normalized
    return any(marker in line for marker in SBSQL_NAME_DRIFT_ALLOWED_LINES.get(rel_path, ()))


def iter_repo_text_files(root: Path) -> Iterable[Path]:
    skip_dirs = {"." + "git", ".pytest_cache", "__pycache__", "build", "fixtures"}
    for path in root.rglob("*"):
        if not path.is_file():
            continue
        if any(part in skip_dirs for part in path.parts):
            continue
        yield path


def sbsql_name_drift_errors(repo_root: Path) -> list[str]:
    errors: list[str] = []
    repo_root = repo_root.resolve()

    for scan_root in SBSQL_NAME_DRIFT_SCAN_ROOTS:
        root = repo_root / scan_root
        if not root.exists():
            errors.append(f"missing SBsql name-drift scan root {scan_root}")
            continue
        for path in iter_repo_text_files(root):
            rel_path = relative_repo_path(repo_root, path)
            try:
                lines = path.read_text(encoding="utf-8", errors="ignore").splitlines()
            except OSError as exc:
                errors.append(f"could not read {rel_path}: {exc}")
                continue
            for line_number, line in enumerate(lines, start=1):
                if not contains_deprecated_sbsql_alias(line):
                    continue
                if line_is_allowed_sbsql_name_drift(rel_path, line):
                    continue
                errors.append(f"{rel_path}:{line_number}: deprecated SBsql alias in product-facing text")

    registry_json = repo_root / "public_audit_summary"
    if registry_json.exists():
        try:
            registry = json.loads(registry_json.read_text(encoding="utf-8"))
        except json.JSONDecodeError as exc:
            errors.append(f"{relative_repo_path(repo_root, registry_json)} is not valid JSON: {exc}")
        else:
            checked_fields = ("title", "slug", "area", "section", "gap_summary")
            for row in registry.get("entries", []):
                registry_id = row.get("registry_id", "<missing-registry-id>")
                for field in checked_fields:
                    if contains_deprecated_sbsql_alias(row.get(field, "")):
                        errors.append(f"{registry_id}: deprecated SBsql alias in registry field {field}")
                for source in row.get("source_documents", []):
                    if contains_deprecated_sbsql_alias(source):
                        errors.append(f"{registry_id}: deprecated SBsql alias in source document {source}")

    registry_csv = repo_root / "public_audit_summary"
    if registry_csv.exists():
        for row in read_csv_rows(registry_csv):
            registry_id = row.get("registry_id", "<missing-registry-id>")
            for field in ("title", "area", "source_documents"):
                if contains_deprecated_sbsql_alias(row.get(field, "")):
                    errors.append(f"{registry_id}: deprecated SBsql alias in CSV field {field}")

    authority_csv = repo_root / "public_audit_summary"
    if authority_csv.exists():
        for row in read_csv_rows(authority_csv):
            registry_id = row.get("registry_id", "<missing-registry-id>")
            if contains_deprecated_sbsql_alias(row.get("title", "")):
                errors.append(f"{registry_id}: deprecated SBsql alias in gap-id authority title")

    return errors


def target_evidence_errors(
    registry: dict,
    execution_plan_root: Path,
    *,
    require_closed: bool,
    require_artifact_files: bool,
) -> list[str]:
    errors: list[str] = []
    target_path = target_gap_path(execution_plan_root)
    evidence_path = execution_plan_root / "artifacts/TARGET_EVIDENCE_MANIFEST.csv"
    if not target_path.exists():
        return [f"missing target gap list {target_path}"]
    if not evidence_path.exists():
        return [f"missing target evidence manifest {evidence_path}"]

    target_rows = read_csv_rows(target_path)
    evidence_rows = read_csv_rows(evidence_path)
    if not target_rows:
        errors.append(f"target gap list is empty: {target_path}")
    if len(evidence_rows) != len(target_rows):
        errors.append(
            f"expected {len(target_rows)} target evidence rows, found {len(evidence_rows)}"
        )

    evidence_fields = set(evidence_rows[0].keys()) if evidence_rows else set()
    missing_columns = sorted(TARGET_EVIDENCE_COLUMNS - evidence_fields)
    if missing_columns:
        errors.append(f"target evidence manifest missing columns: {', '.join(missing_columns)}")

    target_ids = [row.get("gap_id", "") for row in target_rows]
    evidence_ids = [row.get("gap_id", "") for row in evidence_rows]
    if len(target_ids) != len(set(target_ids)):
        errors.append("target gap list has duplicate gap_id values")
    if len(evidence_ids) != len(set(evidence_ids)):
        errors.append("target evidence manifest has duplicate gap_id values")
    if set(target_ids) != set(evidence_ids):
        missing = sorted(set(target_ids) - set(evidence_ids))
        extra = sorted(set(evidence_ids) - set(target_ids))
        errors.append(f"target/evidence gap_id mismatch missing={missing} extra={extra}")

    registry_rows = registry_by_gap_id(registry)
    for row in evidence_rows:
        gap_id = row.get("gap_id", "")
        registry_row = registry_rows.get(gap_id)
        if registry_row is None:
            errors.append(f"{gap_id} missing from public registry")
            continue
        if row.get("registry_id") != registry_row.get("registry_id"):
            errors.append(
                f"{gap_id} registry_id mismatch evidence={row.get('registry_id')} "
                f"registry={registry_row.get('registry_id')}"
            )
        for field in ("target_group", "required_ctest_labels", "implementation_refs_required", "closure_rule"):
            if not row.get(field):
                errors.append(f"{gap_id} missing {field}")
        for label in split_semicolon(row.get("required_ctest_labels", "")):
            if not re.fullmatch(r"[A-Za-z0-9_]+", label):
                errors.append(f"{gap_id} invalid CTest label {label!r}")
        if require_artifact_files:
            for artifact in split_semicolon(row.get("evidence_artifacts", "")):
                path = execution_plan_path(execution_plan_root, artifact)
                if not path.exists():
                    errors.append(f"{gap_id} missing evidence artifact {artifact}")
        if require_closed:
            if registry_row.get("status") != "implemented_in_full":
                errors.append(f"{gap_id} is not implemented_in_full in registry")
            if row.get("status") != "implemented_in_full":
                errors.append(f"{gap_id} evidence status is not implemented_in_full")

    return errors


def required_hardening_artifacts(execution_plan_root: Path) -> list[str]:
    if (execution_plan_root / "artifacts/TARGET_CHECKLIST_ROWS.csv").exists():
        return [
            "artifacts/TARGET_CHECKLIST_ROWS.csv",
            "artifacts/TARGET_EVIDENCE_MANIFEST.csv",
            "artifacts/IMPLEMENTATION_AHEAD_CLASSIFICATION.csv",
            "artifacts/AGENT_WRITE_SCOPE_MATRIX.csv",
            "artifacts/AGENT_STATUS.csv",
            "artifacts/AI_BUDGET_CONTINGENCY.md",
            "artifacts/HARDENING_REQUIREMENTS.md",
            "artifacts/SPEC_AUTHORITY_CLOSURE_MODEL.md",
            "artifacts/WIRE_SESSION_SPEC_CLOSURE_MODEL.md",
            "artifacts/SECURITY_AUTH_RECONCILIATION_MODEL.md",
            "artifacts/DATATYPE_RESULT_METADATA_CLOSURE_MODEL.md",
            "artifacts/DRIVER_LANE_EVIDENCE_MODEL.md",
            "artifacts/FULL_ROUTE_DRIVER_ACCEPTANCE_FIXTURE.md",
            "artifacts/PROTOCOL_VERSION_SKEW_MATRIX.md",
            "artifacts/DETERMINISTIC_REFUSAL_MATRIX.md",
            "artifacts/FUZZ_FAULT_INJECTION_MATRIX.md",
            "artifacts/PACKAGING_DISTRIBUTION_EVIDENCE_MODEL.md",
            "artifacts/PERFORMANCE_BUDGET_GATE.md",
            "artifacts/DOCUMENTATION_SAMPLE_APP_GATE.md",
            "artifacts/REFERENCE_DRIVER_COMPATIBILITY_ROUTE_GATE.md",
            "artifacts/RELEASE_DECLARATION_GENERATOR_MODEL.md",
            "artifacts/P0_EXECUTION_PLAN_CREATION_EVIDENCE.md",
        ]
    if (execution_plan_root / "artifacts/PROTECTED_VERSIONED_MATERIAL_CATALOG_MODEL.md").exists():
        return [
            "artifacts/TARGET_GAPS.csv",
            "artifacts/TARGET_EVIDENCE_MANIFEST.csv",
            "artifacts/AGENT_WRITE_SCOPE_MATRIX.csv",
            "artifacts/AGENT_STATUS.csv",
            "artifacts/AI_BUDGET_CONTINGENCY.md",
            "artifacts/HARDENING_REQUIREMENTS.md",
            "artifacts/PROTECTED_VERSIONED_MATERIAL_CATALOG_MODEL.md",
            "artifacts/CLOUD_PROVIDER_CAPABILITY_REGISTRY_MODEL.md",
            "artifacts/CLOUD_IDENTITY_KMS_SECRETLESS_MODEL.md",
            "artifacts/KUBERNETES_OPERATOR_LIFECYCLE_MODEL.md",
            "artifacts/EDGE_CACHE_CDN_INVALIDATION_MODEL.md",
            "artifacts/FULL_ROUTE_ACCEPTANCE_FIXTURE.md",
            "artifacts/THREAT_MODEL_AND_SECRET_REDACTION_MATRIX.md",
            "artifacts/DIAGNOSTIC_CODE_MATRIX.md",
            "artifacts/BOOTSTRAP_AND_DEFAULT_POLICY_MATRIX.md",
            "artifacts/LOCAL_EMULATOR_FIXTURE_POLICY.md",
            "artifacts/BACKUP_RESTORE_PITR_PROTECTED_MATERIAL_POLICY.md",
            "artifacts/PUBLIC_PRIVATE_BOUNDARY_MATRIX.md",
            "artifacts/METRICS_AND_AUDIT_EVIDENCE_MODEL.md",
            "artifacts/FINAL_RELEASE_DECLARATION_MODEL.md",
            "artifacts/MANAGEMENT_SURFACE_AND_SBLR_OPERATION_MATRIX.md",
            "artifacts/PERSISTED_FORMAT_AND_MIGRATION_POLICY.md",
            "artifacts/EXTERNAL_EFFECT_OUTBOX_AND_IDEMPOTENCY_POLICY.md",
            "artifacts/RESOURCE_LIMITS_AND_BACKPRESSURE_POLICY.md",
            "artifacts/PUBLIC_ABI_AND_PACKAGE_MANIFEST_MATRIX.md",
            "artifacts/CONFORMANCE_MANIFEST_AND_RELEASE_GATE_RECORDS.md",
            "artifacts/ADMIN_DOCS_AND_SAMPLE_FLOW.md",
            "artifacts/P0_EXECUTION_PLAN_CREATION_EVIDENCE.md",
        ]
    if (execution_plan_root / "artifacts/HARDENING_REQUIREMENTS.md").exists():
        return [
            "artifacts/TARGET_GAPS.csv",
            "artifacts/TARGET_EVIDENCE_MANIFEST.csv",
            "artifacts/AGENT_WRITE_SCOPE_MATRIX.csv",
            "artifacts/AGENT_STATUS.csv",
            "artifacts/AI_BUDGET_CONTINGENCY.md",
            "artifacts/HARDENING_REQUIREMENTS.md",
            "artifacts/PARSER_PROFILE_CLOSURE_MODEL.md",
            "artifacts/STORAGE_FORMAT_AND_PROVIDER_POLICY.md",
            "artifacts/DATATYPE_INDEX_EXECUTION_CLOSURE_MODEL.md",
            "artifacts/SECURITY_AUTH_AUDIT_CLOSURE_MODEL.md",
            "artifacts/WIRE_DRIVER_OPERATIONAL_CLOSURE_MODEL.md",
            "artifacts/REFERENCE_REGRESSION_POLICY.md",
            "artifacts/FULL_ROUTE_ACCEPTANCE_FIXTURE.md",
        ]
    return [
        "artifacts/TARGET_EVIDENCE_MANIFEST.csv",
        "artifacts/PREFLIGHT_BASELINE_INVENTORY.csv",
        "artifacts/PERSISTENT_FORMAT_MIGRATION_POLICY.md",
        "artifacts/FAULT_INJECTION_MATRIX.csv",
        "artifacts/CATALOG_PHYSICAL_INDEX_PROFILE.md",
        "artifacts/INFORMATION_PROJECTION_NAMING_DECISION.md",
        "artifacts/SYNONYM_OBJECT_SEMANTICS.md",
        "artifacts/TLS_FIXTURE_POLICY.md",
        "artifacts/CONSTRAINT_INDEX_DEPENDENCY_POLICY.md",
        "artifacts/FULL_ROUTE_ACCEPTANCE_FIXTURE.md",
        "artifacts/AGENT_WRITE_SCOPE_MATRIX.csv",
        "artifacts/AGENT_STATUS.csv",
        "artifacts/MGA_RECOVERY_PROOF_MODEL.md",
    ]


def hardening_errors(execution_plan_root: Path) -> list[str]:
    errors: list[str] = []
    for relative in required_hardening_artifacts(execution_plan_root):
        if not (execution_plan_root / relative).exists():
            errors.append(f"missing hardening artifact {relative}")

    agent_status = execution_plan_root / "artifacts/AGENT_STATUS.csv"
    if agent_status.exists():
        with agent_status.open("r", encoding="utf-8", newline="") as handle:
            fieldnames = list(csv.DictReader(handle).fieldnames or [])
        if fieldnames != AGENT_STATUS_COLUMNS:
            errors.append(f"AGENT_STATUS.csv schema mismatch: {fieldnames!r}")

    proof = execution_plan_root / "artifacts/MGA_RECOVERY_PROOF_MODEL.md"
    if proof.exists():
        text = proof.read_text(encoding="utf-8")
        for needle in (
            "PUBLIC_RELEASE_FOUNDATION_MGA_RECOVERY_PROOF_MODEL",
            "must not introduce WAL",
            "PRF-041 must not start until PRF-040",
        ):
            if needle not in text:
                errors.append(f"MGA recovery proof model missing {needle!r}")

    return errors


def implementation_ahead_errors(execution_plan_root: Path, repo_root: Path) -> list[str]:
    classification_path = execution_plan_root / "artifacts/IMPLEMENTATION_AHEAD_CLASSIFICATION.csv"
    gates_path = execution_plan_root / "ACCEPTANCE_GATES.csv"
    if not classification_path.exists():
        return [f"missing implementation-ahead classification {classification_path}"]
    if not gates_path.exists():
        return [f"missing acceptance gate registry {gates_path}"]

    rows = read_csv_rows(classification_path)
    gate_rows = read_csv_rows(gates_path)
    if not rows:
        return [f"implementation-ahead classification has no rows: {classification_path}"]

    gate_status = {row.get("gate_id", ""): row.get("status", "") for row in gate_rows}
    errors: list[str] = []
    seen_ids: set[str] = set()
    for row in rows:
        audit_id = row.get("audit_id", "").strip()
        if not audit_id:
            errors.append("implementation-ahead row missing audit_id")
            continue
        if audit_id in seen_ids:
            errors.append(f"duplicate implementation-ahead audit_id {audit_id}")
        seen_ids.add(audit_id)

        current_class = row.get("current_class", "").strip()
        if current_class not in IMPLEMENTATION_AHEAD_ALLOWED_CLASSES:
            errors.append(f"{audit_id} invalid current_class={current_class!r}")
        if row.get("status", "").strip() not in IMPLEMENTATION_AHEAD_CLOSED_STATUSES:
            errors.append(f"{audit_id} is not closed: status={row.get('status', '')!r}")

        for field in (
            "source_audit_item",
            "severity",
            "required_action",
            "owner",
            "spec_target",
            "implementation_target",
            "validation_gate",
        ):
            if not row.get(field, "").strip():
                errors.append(f"{audit_id} missing {field}")

        gate_id = row.get("validation_gate", "").strip()
        if gate_id not in gate_status:
            errors.append(f"{audit_id} validation_gate {gate_id!r} is not registered")
        elif gate_status[gate_id] != "completed":
            errors.append(f"{audit_id} validation_gate {gate_id} is not completed: {gate_status[gate_id]!r}")

        for field in ("spec_target", "implementation_target"):
            for raw_path in split_semicolon(row.get(field, "")):
                path = repo_root / raw_path
                if not path.exists():
                    errors.append(f"{audit_id} {field} path does not exist: {raw_path}")

    return errors


def synonym_spec_errors(repo_root: Path) -> list[str]:
    errors: list[str] = []
    spec_relative = "chapters/catalog-schema/appendix-sql-object-synonym-semantics.md"
    manifest = repo_root / "public_contract_snapshot"
    spec = repo_root / "public_release_evidence" / spec_relative
    if not manifest.exists():
        return [f"missing manifest {manifest}"]
    if spec_relative not in manifest.read_text(encoding="utf-8"):
        errors.append(f"manifest does not list {spec_relative}")
    if not spec.exists():
        errors.append(f"missing synonym spec {spec}")
        return errors
    text = spec.read_text(encoding="utf-8")
    for needle in (
        "first-class SQL object",
        "It is not an additional name",
        "SYNONYM_DEPTH_EXCEEDED",
        "more than five synonym hops",
        "sys.information_schema",
        "SYNONYM-CONF-008",
    ):
        if needle not in text:
            errors.append(f"synonym spec missing {needle!r}")
    return errors


def conformance_manifest_errors(execution_plan_root: Path, repo_root: Path) -> list[str]:
    errors: list[str] = []
    target_rows = read_csv_rows(execution_plan_root / "artifacts/TARGET_EVIDENCE_MANIFEST.csv")
    target_gap_ids = {row["gap_id"] for row in target_rows}
    required_labels = {
        label
        for row in target_rows
        for label in split_semicolon(row.get("required_ctest_labels", ""))
    }
    record_path = repo_root / "public_contract_snapshot"
    manifest_path = repo_root / "public_contract_snapshot"
    for path in (record_path, manifest_path):
        if not path.exists():
            errors.append(f"missing release conformance artifact {path}")
            continue
        text = path.read_text(encoding="utf-8")
        for gap_id in sorted(target_gap_ids):
            if gap_id not in text:
                errors.append(f"{path} missing {gap_id}")
        for needle in ("PUBLIC-RELEASE-FOUNDATION", "public_release_foundation"):
            if needle not in text:
                errors.append(f"{path} missing {needle}")
    if manifest_path.exists():
        text = manifest_path.read_text(encoding="utf-8")
        for label in sorted(required_labels):
            if label not in text:
                errors.append(f"public-release-foundation conformance manifest missing label {label}")
    return errors


def open_public_gaps(registry: dict) -> list[dict]:
    return [
        row
        for row in registry.get("entries", [])
        if row.get("public_release_required") and row.get("status") != "implemented_in_full"
    ]


def print_summary(registry: dict, stream=sys.stdout) -> None:
    summary = registry.get("summary", {})
    print(
        "public_spec_gap_registry "
        f"total={summary.get('total_entries', 0)} "
        f"public_required={summary.get('public_required_entries', 0)} "
        f"public_open={summary.get('public_open_entries', 0)}",
        file=stream,
    )
    for key, value in sorted(summary.get("status_counts", {}).items()):
        print(f"status.{key}={value}", file=stream)


def recompute_summary(registry: dict) -> None:
    entries = registry.get("entries", [])
    status_counts: dict[str, int] = {}
    scope_counts: dict[str, int] = {}
    release_counts: dict[str, int] = {}
    for row in entries:
        status_counts[row["status"]] = status_counts.get(row["status"], 0) + 1
        scope_counts[row["scope"]] = scope_counts.get(row["scope"], 0) + 1
        release_counts[row["public_release_state"]] = (
            release_counts.get(row["public_release_state"], 0) + 1
        )
    registry["summary"] = {
        "total_entries": len(entries),
        "status_counts": status_counts,
        "scope_counts": scope_counts,
        "release_state_counts": release_counts,
        "public_required_entries": scope_counts.get("public_single_node", 0),
        "public_open_entries": release_counts.get("open", 0),
    }


def write_registry_csv_from_json(path: Path, registry: dict) -> None:
    write_csv(path, registry)


def command_write_registry(args: argparse.Namespace) -> int:
    registry = build_registry(args.inventory)
    errors: list[str] = []
    authority, authority_errors = read_gap_id_authority(args.gap_id_authority)
    closure_overlays, closure_errors = collect_closure_overlays(args.closure_execution_plan_root or [])
    errors.extend(authority_errors)
    errors.extend(closure_errors)
    errors.extend(apply_closure_overlays(registry, closure_overlays))
    errors.extend(normalize_gap_ids(registry, authority))
    recompute_summary(registry)
    errors.extend(registry_errors(registry, args.inventory))
    if errors:
        for error in errors:
            print(f"SB-PUBLIC-ZERO-GREY-WRITE-ERROR: {error}", file=sys.stderr)
        return 2
    write_json(args.out_json, registry)
    if args.out_csv:
        write_csv(args.out_csv, registry)
    print_summary(registry)
    return 0


def command_audit(args: argparse.Namespace) -> int:
    registry = load_registry(args.registry_json)
    errors = registry_errors(registry, args.inventory)
    print_summary(registry)
    if errors:
        for error in errors:
            print(f"SB-PUBLIC-ZERO-GREY-REGISTRY-ERROR: {error}", file=sys.stderr)
        return 2
    return 0


def command_gate(args: argparse.Namespace) -> int:
    registry = load_registry(args.registry_json)
    errors = registry_errors(registry, args.inventory)
    if errors:
        for error in errors:
            print(f"SB-PUBLIC-ZERO-GREY-REGISTRY-ERROR: {error}", file=sys.stderr)
        return 2

    gaps = open_public_gaps(registry)
    print_summary(registry)
    if gaps:
        print(
            f"SB-PUBLIC-ZERO-GREY-GATE-FAILED: {len(gaps)} public release-required entries are open",
            file=sys.stderr,
        )
        for row in gaps[: args.limit]:
            print(
                f"{row['gap_id'] or row['registry_id']} "
                f"{row['status']} {row['priority']} {row['area']} :: {row['title']}",
                file=sys.stderr,
            )
        if len(gaps) > args.limit:
            print(f"... {len(gaps) - args.limit} additional public gaps omitted", file=sys.stderr)
        return 1

    print("SB-PUBLIC-ZERO-GREY-GATE-PASSED: all public entries are implemented in full")
    return 0


def command_update_target_statuses(args: argparse.Namespace) -> int:
    registry = load_registry(args.registry_json)
    errors = registry_errors(registry, args.inventory)
    errors.extend(
        target_evidence_errors(
            registry,
            args.execution_plan_root,
            require_closed=False,
            require_artifact_files=False,
        )
    )
    if errors:
        for error in errors:
            print(f"SB-PUBLIC-TARGET-STATUS-UPDATE-ERROR: {error}", file=sys.stderr)
        return 2

    evidence_rows = read_csv_rows(args.execution_plan_root / "artifacts/TARGET_EVIDENCE_MANIFEST.csv")
    evidence_by_gap = {row["gap_id"]: row for row in evidence_rows}
    updated = 0
    for row in registry.get("entries", []):
        gap_id = row.get("gap_id", "")
        evidence = evidence_by_gap.get(gap_id)
        if evidence is None:
            continue
        if evidence.get("status") != "implemented_in_full":
            continue
        artifact_errors = target_evidence_errors(
            registry,
            args.execution_plan_root,
            require_closed=False,
            require_artifact_files=True,
        )
        if artifact_errors:
            for error in artifact_errors:
                print(f"SB-PUBLIC-TARGET-STATUS-UPDATE-ERROR: {error}", file=sys.stderr)
            return 2
        row["status"] = "implemented_in_full"
        row["status_raw"] = "Implemented in Full"
        row["public_release_state"] = "closed"
        row["priority"] = "closed"
        updated += 1

    recompute_summary(registry)
    out_json = args.out_json or args.registry_json
    write_json(out_json, registry)
    if args.out_csv:
        write_registry_csv_from_json(args.out_csv, registry)
    print(f"SB-PUBLIC-TARGET-STATUS-UPDATED: {updated} target rows closed from evidence")
    print_summary(registry)
    return 0


def command_target_evidence(args: argparse.Namespace) -> int:
    registry = load_registry(args.registry_json)
    errors = registry_errors(registry, args.inventory)
    errors.extend(
        target_evidence_errors(
            registry,
            args.execution_plan_root,
            require_closed=False,
            require_artifact_files=False,
        )
    )
    if errors:
        for error in errors:
            print(f"SB-PUBLIC-TARGET-EVIDENCE-ERROR: {error}", file=sys.stderr)
        return 2
    print("SB-PUBLIC-TARGET-EVIDENCE-PASSED: target evidence manifest is structurally valid")
    return 0


def command_target_gate(args: argparse.Namespace) -> int:
    registry = load_registry(args.registry_json)
    errors = registry_errors(registry, args.inventory)
    errors.extend(
        target_evidence_errors(
            registry,
            args.execution_plan_root,
            require_closed=True,
            require_artifact_files=True,
        )
    )
    if errors:
        for error in errors[: args.limit]:
            print(f"SB-PUBLIC-TARGET-ZERO-GREY-ERROR: {error}", file=sys.stderr)
        if len(errors) > args.limit:
            print(f"... {len(errors) - args.limit} additional target errors omitted", file=sys.stderr)
        return 1
    print("SB-PUBLIC-TARGET-ZERO-GREY-PASSED: all target gaps are closed with evidence")
    return 0


def command_hardening(args: argparse.Namespace) -> int:
    errors = hardening_errors(args.execution_plan_root)
    if errors:
        for error in errors:
            print(f"SB-PUBLIC-HARDENING-ERROR: {error}", file=sys.stderr)
        return 2
    print("SB-PUBLIC-HARDENING-PASSED: pre-implementation artifacts and schemas are present")
    return 0


def command_synonym_spec(args: argparse.Namespace) -> int:
    errors = synonym_spec_errors(args.repo_root)
    if errors:
        for error in errors:
            print(f"SB-PUBLIC-SYNONYM-SPEC-ERROR: {error}", file=sys.stderr)
        return 2
    print("SB-PUBLIC-SYNONYM-SPEC-PASSED: canonical synonym spec authority is present")
    return 0


def command_conformance_manifest(args: argparse.Namespace) -> int:
    errors = conformance_manifest_errors(args.execution_plan_root, args.repo_root)
    if errors:
        for error in errors:
            print(f"SB-PUBLIC-CONFORMANCE-MANIFEST-ERROR: {error}", file=sys.stderr)
        return 2
    print("SB-PUBLIC-CONFORMANCE-MANIFEST-PASSED: release records and conformance manifest are complete")
    return 0


def command_non_target_regression(args: argparse.Namespace) -> int:
    registry = load_registry(args.registry_json)
    errors = registry_errors(registry, args.inventory)
    errors.extend(
        target_evidence_errors(
            registry,
            args.execution_plan_root,
            require_closed=True,
            require_artifact_files=True,
        )
    )
    if errors:
        for error in errors[: args.limit]:
            print(f"SB-PUBLIC-NON-TARGET-REGRESSION-ERROR: {error}", file=sys.stderr)
        return 1

    target_ids = {
        row["gap_id"]
        for row in read_csv_rows(args.execution_plan_root / "artifacts/TARGET_EVIDENCE_MANIFEST.csv")
    }
    non_target_open = [
        row
        for row in open_public_gaps(registry)
        if row.get("gap_id") not in target_ids
    ]
    print_summary(registry)
    print(
        "SB-PUBLIC-NON-TARGET-REGRESSION-PASSED: "
        f"{len(non_target_open)} non-target public gaps remain open"
    )
    return 0


def command_driver_checklist(args: argparse.Namespace) -> int:
    errors = driver_checklist_errors(
        args.registry,
        args.target_rows,
        require_closed=args.require_closed,
    )
    if errors:
        for error in errors[: args.limit]:
            print(f"SB-DRIVER-CHECKLIST-GATE-ERROR: {error}", file=sys.stderr)
        if len(errors) > args.limit:
            print(f"... {len(errors) - args.limit} additional driver checklist errors omitted", file=sys.stderr)
        return 1 if args.require_closed else 2
    mode = "closure" if args.require_closed else "structure"
    print(f"SB-DRIVER-CHECKLIST-GATE-PASSED: {mode} validation passed")
    return 0


def command_implementation_ahead(args: argparse.Namespace) -> int:
    errors = implementation_ahead_errors(args.execution_plan_root, args.repo_root)
    if errors:
        for error in errors[: args.limit]:
            print(f"SB-IMPLEMENTATION-AHEAD-ZERO-GREY-ERROR: {error}", file=sys.stderr)
        if len(errors) > args.limit:
            print(f"... {len(errors) - args.limit} additional implementation-ahead errors omitted", file=sys.stderr)
        return 1
    print("SB-IMPLEMENTATION-AHEAD-ZERO-GREY-PASSED: all implementation-ahead items are specified, guarded, or refused with completed gates")
    return 0


def command_sbsql_name_drift(args: argparse.Namespace) -> int:
    errors = sbsql_name_drift_errors(args.repo_root)
    if errors:
        for error in errors[: args.limit]:
            print(f"SBSQL-NAME-DRIFT-GATE-ERROR: {error}", file=sys.stderr)
        if len(errors) > args.limit:
            print(f"... {len(errors) - args.limit} additional SBsql name-drift errors omitted", file=sys.stderr)
        return 1
    print("SBSQL-NAME-DRIFT-GATE-PASSED: product-facing SBsql names are canonical")
    return 0


def command_export_gap_id_authority(args: argparse.Namespace) -> int:
    registry = load_registry(args.registry_json)
    errors = registry_errors(registry, None)
    if errors:
        for error in errors:
            print(f"SB-PUBLIC-GAP-ID-AUTHORITY-EXPORT-ERROR: {error}", file=sys.stderr)
        return 2

    args.out_csv.parent.mkdir(parents=True, exist_ok=True)
    fieldnames = [
        "registry_id",
        "gap_id",
        "title",
        "status",
        "public_release_state",
        "priority",
        "authority_source",
    ]
    with args.out_csv.open("w", encoding="utf-8", newline="") as handle:
        writer = csv.DictWriter(handle, fieldnames=fieldnames, lineterminator="\n")
        writer.writeheader()
        for row in registry.get("entries", []):
            if not row.get("gap_id"):
                continue
            writer.writerow(
                {
                    "registry_id": row.get("registry_id", ""),
                    "gap_id": row.get("gap_id", ""),
                    "title": row.get("title", ""),
                    "status": row.get("status", ""),
                    "public_release_state": row.get("public_release_state", ""),
                    "priority": row.get("priority", ""),
                    "authority_source": "public_spec_gap_registry",
                }
            )
    print(f"SB-PUBLIC-GAP-ID-AUTHORITY-EXPORTED: {args.out_csv}")
    return 0


def command_gap_id_authority(args: argparse.Namespace) -> int:
    registry = load_registry(args.registry_json)
    authority, errors = read_gap_id_authority(args.authority_csv)
    errors.extend(registry_errors(registry, None))
    registry_rows = {row.get("registry_id", ""): row for row in registry.get("entries", [])}
    for registry_id, gap_id in authority.items():
        row = registry_rows.get(registry_id)
        if row is None:
            errors.append(f"gap-id authority references unknown registry_id {registry_id}")
            continue
        if row.get("gap_id", "") != gap_id:
            errors.append(
                f"{registry_id} registry gap_id {row.get('gap_id', '')!r} "
                f"does not match authority {gap_id!r}"
            )
    for row in registry.get("entries", []):
        gap_id = row.get("gap_id", "")
        if not gap_id:
            continue
        registry_id = row.get("registry_id", "")
        if authority.get(registry_id) != gap_id:
            errors.append(f"{registry_id} gap_id {gap_id} is missing from authority")
    if errors:
        for error in errors:
            print(f"SB-PUBLIC-GAP-ID-AUTHORITY-ERROR: {error}", file=sys.stderr)
        return 2
    print("SB-PUBLIC-GAP-ID-AUTHORITY-PASSED: registry gap IDs are stable and unique")
    return 0


def read_reopen_records(path: Path | None) -> tuple[set[str], list[str]]:
    if path is None:
        return set(), []
    if not path.exists():
        return set(), [f"missing reopen record file {path}"]
    rows = read_csv_rows(path)
    allowed: set[str] = set()
    errors: list[str] = []
    for row in rows:
        registry_id = row.get("registry_id", "").strip()
        if not registry_id:
            errors.append(f"{path} has reopen row missing registry_id")
            continue
        for field in ("reason", "approved_by", "approved_at_utc"):
            if not row.get(field, "").strip():
                errors.append(f"{registry_id} reopen row missing {field}")
        allowed.add(registry_id)
    return allowed, errors


def command_closure_regression(args: argparse.Namespace) -> int:
    registry = load_registry(args.registry_json)
    errors = registry_errors(registry, None)
    overlays, overlay_errors = collect_closure_overlays(args.closed_execution_plan_root or [])
    allowed_reopens, reopen_errors = read_reopen_records(args.reopen_records)
    errors.extend(overlay_errors)
    errors.extend(reopen_errors)

    rows_by_registry_id = {row.get("registry_id", ""): row for row in registry.get("entries", [])}
    for registry_id, overlay in sorted(overlays.items()):
        row = rows_by_registry_id.get(registry_id)
        if row is None:
            errors.append(f"closed execution_plan references unknown registry_id {registry_id}")
            continue
        if registry_id in allowed_reopens:
            continue
        if row.get("status") != "implemented_in_full" or row.get("public_release_state") != "closed":
            errors.append(
                f"{registry_id} was closed by {overlay['execution_plan_root']} but is now "
                f"{row.get('status')}/{row.get('public_release_state')}"
            )

    public_open = registry.get("summary", {}).get("public_open_entries", 0)
    allowed_open = args.max_public_open + len(allowed_reopens)
    if public_open > allowed_open:
        errors.append(
            f"public_open_entries increased to {public_open}; allowed maximum is {allowed_open}"
        )

    if errors:
        for error in errors[: args.limit]:
            print(f"SB-PUBLIC-CLOSED-EXECUTION_PLAN-REGRESSION-ERROR: {error}", file=sys.stderr)
        if len(errors) > args.limit:
            print(f"... {len(errors) - args.limit} additional closure regression errors omitted", file=sys.stderr)
        return 1
    print(
        "SB-PUBLIC-CLOSED-EXECUTION_PLAN-REGRESSION-PASSED: "
        f"closed execution_plan rows remain closed and public_open_entries={public_open}"
    )
    return 0


def build_parser() -> argparse.ArgumentParser:
    parser = argparse.ArgumentParser(description=__doc__)
    subparsers = parser.add_subparsers(dest="command", required=True)

    write_parser = subparsers.add_parser("write-registry")
    write_parser.add_argument("--inventory", type=Path, required=True)
    write_parser.add_argument("--out-json", type=Path, required=True)
    write_parser.add_argument("--out-csv", type=Path)
    write_parser.add_argument(
        "--closure-fixture-root",
        "--closure-execution_plan-root",
        dest="closure_execution_plan_root",
        type=Path,
        action="append",
    )
    write_parser.add_argument("--gap-id-authority", type=Path)
    write_parser.set_defaults(func=command_write_registry)

    audit_parser = subparsers.add_parser("audit")
    audit_parser.add_argument("--inventory", type=Path, required=True)
    audit_parser.add_argument("--registry-json", type=Path, required=True)
    audit_parser.set_defaults(func=command_audit)

    gate_parser = subparsers.add_parser("gate")
    gate_parser.add_argument("--inventory", type=Path, required=True)
    gate_parser.add_argument("--registry-json", type=Path, required=True)
    gate_parser.add_argument("--limit", type=int, default=25)
    gate_parser.set_defaults(func=command_gate)

    update_parser = subparsers.add_parser("update-target-statuses")
    update_parser.add_argument("--inventory", type=Path, required=True)
    update_parser.add_argument("--registry-json", type=Path, required=True)
    update_parser.add_argument("--fixture-root", "--execution_plan-root", dest="execution_plan_root", type=Path, required=True)
    update_parser.add_argument("--out-json", type=Path)
    update_parser.add_argument("--out-csv", type=Path)
    update_parser.set_defaults(func=command_update_target_statuses)

    target_evidence_parser = subparsers.add_parser("target-evidence")
    target_evidence_parser.add_argument("--inventory", type=Path, required=True)
    target_evidence_parser.add_argument("--registry-json", type=Path, required=True)
    target_evidence_parser.add_argument("--fixture-root", "--execution_plan-root", dest="execution_plan_root", type=Path, required=True)
    target_evidence_parser.set_defaults(func=command_target_evidence)

    target_gate_parser = subparsers.add_parser("target-gate")
    target_gate_parser.add_argument("--inventory", type=Path, required=True)
    target_gate_parser.add_argument("--registry-json", type=Path, required=True)
    target_gate_parser.add_argument("--fixture-root", "--execution_plan-root", dest="execution_plan_root", type=Path, required=True)
    target_gate_parser.add_argument("--limit", type=int, default=50)
    target_gate_parser.set_defaults(func=command_target_gate)

    hardening_parser = subparsers.add_parser("hardening")
    hardening_parser.add_argument("--fixture-root", "--execution_plan-root", dest="execution_plan_root", type=Path, required=True)
    hardening_parser.set_defaults(func=command_hardening)

    synonym_spec_parser = subparsers.add_parser("synonym-spec")
    synonym_spec_parser.add_argument("--repo-root", type=Path, required=True)
    synonym_spec_parser.set_defaults(func=command_synonym_spec)

    conformance_parser = subparsers.add_parser("conformance-manifest")
    conformance_parser.add_argument("--repo-root", type=Path, required=True)
    conformance_parser.add_argument("--fixture-root", "--execution_plan-root", dest="execution_plan_root", type=Path, required=True)
    conformance_parser.set_defaults(func=command_conformance_manifest)

    non_target_parser = subparsers.add_parser("non-target-regression")
    non_target_parser.add_argument("--inventory", type=Path, required=True)
    non_target_parser.add_argument("--registry-json", type=Path, required=True)
    non_target_parser.add_argument("--fixture-root", "--execution_plan-root", dest="execution_plan_root", type=Path, required=True)
    non_target_parser.add_argument("--limit", type=int, default=50)
    non_target_parser.set_defaults(func=command_non_target_regression)

    driver_checklist_parser = subparsers.add_parser("driver-checklist")
    driver_checklist_parser.add_argument("--registry", type=Path, required=True)
    driver_checklist_parser.add_argument("--target-rows", type=Path, required=True)
    driver_checklist_parser.add_argument("--require-closed", action="store_true")
    driver_checklist_parser.add_argument("--limit", type=int, default=50)
    driver_checklist_parser.set_defaults(func=command_driver_checklist)

    implementation_ahead_parser = subparsers.add_parser("implementation-ahead")
    implementation_ahead_parser.add_argument("--repo-root", type=Path, required=True)
    implementation_ahead_parser.add_argument("--fixture-root", "--execution_plan-root", dest="execution_plan_root", type=Path, required=True)
    implementation_ahead_parser.add_argument("--limit", type=int, default=50)
    implementation_ahead_parser.set_defaults(func=command_implementation_ahead)

    sbsql_name_drift_parser = subparsers.add_parser("sbsql-name-drift")
    sbsql_name_drift_parser.add_argument("--repo-root", type=Path, required=True)
    sbsql_name_drift_parser.add_argument("--limit", type=int, default=50)
    sbsql_name_drift_parser.set_defaults(func=command_sbsql_name_drift)

    export_gap_id_parser = subparsers.add_parser("export-gap-id-authority")
    export_gap_id_parser.add_argument("--registry-json", type=Path, required=True)
    export_gap_id_parser.add_argument("--out-csv", type=Path, required=True)
    export_gap_id_parser.set_defaults(func=command_export_gap_id_authority)

    gap_id_authority_parser = subparsers.add_parser("gap-id-authority")
    gap_id_authority_parser.add_argument("--registry-json", type=Path, required=True)
    gap_id_authority_parser.add_argument("--authority-csv", type=Path, required=True)
    gap_id_authority_parser.set_defaults(func=command_gap_id_authority)

    closure_regression_parser = subparsers.add_parser("closure-regression")
    closure_regression_parser.add_argument("--registry-json", type=Path, required=True)
    closure_regression_parser.add_argument(
        "--closed-fixture-root",
        "--closed-execution_plan-root",
        dest="closed_execution_plan_root",
        type=Path,
        action="append",
    )
    closure_regression_parser.add_argument("--max-public-open", type=int, required=True)
    closure_regression_parser.add_argument("--reopen-records", type=Path)
    closure_regression_parser.add_argument("--limit", type=int, default=50)
    closure_regression_parser.set_defaults(func=command_closure_regression)

    return parser


def main(argv: Iterable[str] | None = None) -> int:
    parser = build_parser()
    args = parser.parse_args(argv)
    return args.func(args)


if __name__ == "__main__":
    raise SystemExit(main())
