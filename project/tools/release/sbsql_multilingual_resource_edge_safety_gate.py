#!/usr/bin/env python3
# Copyright (c) 2026 ScratchBird Software Inc.
#
# This Source Code Form is subject to the terms of the Mozilla Public
# License, v. 2.0. If a copy of the MPL was not distributed with this
# file, You can obtain one at https://mozilla.org/MPL/2.0/.
#
# SPDX-License-Identifier: MPL-2.0

"""Gate SBsql multilingual language-resource operational edge safety.

The gate is intentionally evidence-oriented: it scans a repository tree for
source, resource, release, and SBsql parser-worker anchors that prove the
multilingual resource workplan has operational coverage. It never reads draft
documentation and it never uses the network.
"""

from __future__ import annotations

import argparse
import csv
from dataclasses import dataclass
import json
from pathlib import Path
import sys
from typing import Iterable


TOOL_REL_PATH = "project/tools/release/sbsql_multilingual_resource_edge_safety_gate.py"
HARNESS_REL_PATH = "project/tests/sbsql_parser_worker/sbsql_multilingual_resource_edge_safety_harness.py"

EVIDENCE_ROOTS = (
    "project/src/core/resources",
    "project/src/engine/internal_api/backup_archive",
    "project/src/engine/internal_api/catalog",
    "project/src/engine/internal_api/dml",
    "project/src/parsers/sbsql_worker",
    "project/src/server",
    "project/tests/sbsql_parser_worker",
    "project/resources",
    "project/tools/release",
)

EVIDENCE_FILES = (
    "SBOM.json",
    "THIRD_PARTY_NOTICES.md",
    "LICENSE",
    "NOTICE",
    "project/src/engine/internal_api/api_types.hpp",
)

EXCLUDED_PREFIXES = (
    "docs/documentation/draft/",
    "build/",
    "build_clean/",
    "build_compare_current/",
    ".git/",
)

EXCLUDED_PARTS = {"__pycache__", ".git", ".pytest_cache", ".mypy_cache"}
EXCLUDED_SELF_PATHS = {TOOL_REL_PATH, HARNESS_REL_PATH}

TEXT_SUFFIXES = {
    ".c",
    ".cc",
    ".cpp",
    ".csv",
    ".h",
    ".hpp",
    ".json",
    ".md",
    ".py",
    ".txt",
    ".yaml",
    ".yml",
}
TEXT_BASENAMES = {
    "CONTRIBUTING",
    "LICENSE",
    "NOTICE",
    "README",
    "SECURITY",
    "version",
}
MAX_TEXT_BYTES = 2_000_000

RESOURCE_MANIFEST = "project/resources/seed-packs/initial-resource-pack/RESOURCE_SEED_MANIFEST.csv"
RESOURCE_ARTIFACTS = "project/resources/seed-packs/initial-resource-pack/RESOURCE_SEED_ARTIFACTS.csv"
SBOM = "SBOM.json"
THIRD_PARTY = "THIRD_PARTY_NOTICES.md"


@dataclass(frozen=True)
class EvidenceFile:
    rel_path: str
    text: str


@dataclass(frozen=True)
class TokenGroup:
    label: str
    tokens: tuple[str, ...]


@dataclass(frozen=True)
class Requirement:
    key: str
    title: str
    required_paths: tuple[str, ...]
    groups: tuple[TokenGroup, ...]


@dataclass(frozen=True)
class Match:
    rel_path: str
    line_number: int
    token: str

    def render(self) -> str:
        return f"{self.rel_path}:{self.line_number}:{self.token}"


@dataclass
class RequirementResult:
    requirement: Requirement
    errors: list[str]
    matches: dict[str, list[Match]]

    @property
    def passed(self) -> bool:
        return not self.errors


REQUIREMENTS = (
    Requirement(
        # SEARCH_KEY: SML-094 built-in recovery profile
        "SML-094",
        "built-in recovery profile",
        (
            "project/resources/README.md",
            "project/resources/seed-packs/initial-resource-pack/README.md",
            "project/src/core/resources/resource_seed_pack.cpp",
            "project/src/core/resources/resource_seed_pack.hpp",
        ),
        (
            TokenGroup("explicit degraded profile", ("allow_minimal_bootstrap", "minimal-bootstrap", "repair-only")),
            TokenGroup("fail-closed seed refusal", ("SB_RESOURCE_SEED_MISSING", "SB_RESOURCE_SEED_INCOMPLETE", "SB_RESOURCE_SEED_INVALID")),
            TokenGroup("database readiness gates", ("database_create_ready", "database_open_ready")),
            TokenGroup("runtime resource dependency refusal", ("resource-dependent operations", "EvaluateResourceSeedRuntimeCache")),
        ),
    ),
    Requirement(
        # SEARCH_KEY: SML-095 persistent source retention
        "SML-095",
        "persistent source retention",
        (
            RESOURCE_MANIFEST,
            RESOURCE_ARTIFACTS,
            "project/resources/seed-packs/initial-resource-pack/resources/timezones/tzdata2025c.tar.gz",
            "project/resources/seed-packs/initial-resource-pack/resources/timezones/tzcode2025c.tar.gz",
        ),
        (
            TokenGroup("source provenance action", ("retain_source_provenance", "source lineage")),
            TokenGroup("canonical artifact identity", ("canonical_path", "content_hash", "content_size_bytes")),
            TokenGroup("timezone source archives", ("tzdata2025c.tar.gz", "tzcode2025c.tar.gz")),
            TokenGroup("normalized seed-pack authority", ("normalized, checksummed seed pack", "checksum")),
        ),
    ),
    Requirement(
        # SEARCH_KEY: SML-096 locale literal policy
        "SML-096",
        "locale literal policy",
        (
            "project/tests/sbsql_parser_worker/generated/i18n/sbsql_locale_charset_collation_timezone_gate.cpp",
            "project/src/parsers/sbsql_worker/lexer/lexer.cpp",
        ),
        (
            TokenGroup("national string literal", ("national_string", "national string literal was not preserved")),
            TokenGroup("temporal literal families", ("TokenKind::kTemporalLiteral", "timezone timestamp literal", "INTERVAL literal")),
            TokenGroup("collation literal surface", ("COLLATE keyword was not preserved", "C.utf8 collation alias did not resolve")),
            TokenGroup("literal family retention", ("literal_family", "lexer_payload")),
        ),
    ),
    Requirement(
        # SEARCH_KEY: SML-097 confusable/mixed-script policy
        "SML-097",
        "confusable and mixed-script policy",
        (
            "project/tests/sbsql_parser_worker/generated/i18n/sbsql_locale_charset_collation_timezone_gate.cpp",
            "project/src/engine/internal_api/catalog/name_registry.cpp",
            "project/src/engine/internal_api/api_types.hpp",
        ),
        (
            TokenGroup("mixed-script fixture", ("non-Latin UTF-8 identifier", "unquoted UTF-8 identifier", "quoted UTF-8 identifier")),
            TokenGroup("exact quoted identifier handling", ("requires_exact_match", "quoted localized name")),
            TokenGroup("raw/display name retention", ("raw_name_text", "display_name")),
            TokenGroup("identifier profile authority", ("identifier_profile_uuid", "NameRegistryLookupKey")),
            TokenGroup("resource epoch bound names", ("resource_epoch", "name_resolution_epoch")),
        ),
    ),
    Requirement(
        # SEARCH_KEY: SML-098 malformed resource-pack hardening
        "SML-098",
        "malformed resource-pack hardening",
        (
            "project/src/core/resources/resource_seed_pack.cpp",
            RESOURCE_MANIFEST,
            RESOURCE_ARTIFACTS,
        ),
        (
            TokenGroup("invalid manifest rows", ("resource.seed_pack.manifest_row_invalid", "resource.seed_pack.unknown_family")),
            TokenGroup("invalid artifact index rows", ("resource.seed_pack.artifact_index_row_invalid", "resource.seed_pack.artifact_index_size_invalid")),
            TokenGroup("artifact integrity checks", ("resource.seed_pack.artifact_hash_mismatch", "resource.seed_pack.artifact_not_in_index")),
            TokenGroup("required artifact refusal", ("resource.seed_pack.required_artifact_missing", "resource.seed_pack.no_artifacts")),
            TokenGroup("family lifecycle validation", ("resource.seed_pack.family_version_missing", "resource.seed_pack.family_hash_missing")),
        ),
    ),
    Requirement(
        # SEARCH_KEY: SML-099 language data licensing/SBOM
        "SML-099",
        "language data licensing and SBOM",
        (
            SBOM,
            THIRD_PARTY,
            "project/resources/seed-packs/initial-resource-pack/resources/timezones/LICENSE",
            "project/resources/seed-packs/initial-resource-pack/resources/collations/uca/uca_manifest.json",
        ),
        (
            TokenGroup("release SBOM schema", ("scratchbird.public_source_review_sbom.v1", "components")),
            TokenGroup("resource notice category", ("Resource packs", "Upstream license retained with the resource")),
            TokenGroup("timezone license source", ("public domain", "BSD 3-clause", "LICENSE file")),
            TokenGroup("UCA provenance hashes", ("Unicode UCA", "sha256")),
        ),
    ),
    Requirement(
        # SEARCH_KEY: SML-100 backup/restore/import/export resource identity
        "SML-100",
        "backup restore import export resource identity",
        (
            "project/tests/sbsql_parser_worker/sbsql_sbsfc_073_archive_replication_conformance.cpp",
            "project/tests/sbsql_parser_worker/sbsql_dml_exact_route_conformance.cpp",
        ),
        (
            TokenGroup("backup archive authority", ("backup_archive.start_logical_backup", "backup_archive.restore_logical_backup", "EngineRestoreLogicalBackup")),
            TokenGroup("manifest URI binding", ("manifest_uri_present",)),
            TokenGroup("target object UUID binding", ("database:session",)),
            TokenGroup("resource epoch on routes", ("context.resource_epoch", "resource_epoch")),
            TokenGroup("copy import/export surface", ("copy_import_export",)),
            TokenGroup("source handle exclusion", ("source_handle_included=false", "source_handle_included\\\":false")),
            TokenGroup("parser byte-decode exclusion", ("parser_decodes_bytes=false",)),
            TokenGroup("descriptor identity refs", ("sys.backup.archive_lifecycle", "sys.catalog.object_descriptor", "sys.storage.row_descriptor")),
        ),
    ),
    Requirement(
        # SEARCH_KEY: SML-101 common editor/tool protocol
        "SML-101",
        "common editor tool protocol",
        (
            "project/src/parsers/sbsql_worker/common/common.hpp",
            "project/src/parsers/sbsql_worker/runtime/parser_runtime.cpp",
        ),
        (
            TokenGroup("protocol version bounds", ("kSbsqlWorkerProtocolCurrentVersion", "kSbsqlWorkerProtocolMinSupported", "kSbsqlWorkerProtocolMaxSupported")),
            TokenGroup("registry/API version", ("kSbsqlWorkerParserApiCurrentMajor", "kSbsqlWorkerRegistryCurrentVersion")),
            TokenGroup("protocol refusal diagnostic", ("PARSER_SERVER_IPC.PROTOCOL_VERSION_UNSUPPORTED",)),
            TokenGroup("tool resource budgets", ("max_statement_bytes", "max_identifier_bytes", "max_literal_bytes")),
            TokenGroup("message vector limits", ("max_diagnostic_payload_bytes", "max_message_vector_count")),
        ),
    ),
)


def normalize_rel(path: Path, repo_root: Path) -> str:
    return path.resolve().relative_to(repo_root).as_posix()


def is_excluded(rel_path: str, path: Path) -> bool:
    if rel_path in EXCLUDED_SELF_PATHS:
        return True
    if any(rel_path.startswith(prefix) for prefix in EXCLUDED_PREFIXES):
        return True
    return any(part in EXCLUDED_PARTS for part in path.parts)


def is_text_candidate(path: Path) -> bool:
    return path.suffix in TEXT_SUFFIXES or path.name in TEXT_BASENAMES


def read_text(path: Path) -> str:
    return path.read_text(encoding="utf-8", errors="replace")


def iter_inventory(repo_root: Path) -> list[str]:
    paths: set[str] = set()
    for rel_root in EVIDENCE_ROOTS:
        root = repo_root / rel_root
        if not root.exists():
            continue
        if root.is_file():
            rel = normalize_rel(root, repo_root)
            if not is_excluded(rel, root):
                paths.add(rel)
            continue
        for path in root.rglob("*"):
            if not path.is_file():
                continue
            rel = normalize_rel(path, repo_root)
            if is_excluded(rel, path):
                continue
            paths.add(rel)
    for rel_file in EVIDENCE_FILES:
        path = repo_root / rel_file
        if path.is_file() and not is_excluded(rel_file, path):
            paths.add(rel_file)
    return sorted(paths)


def load_evidence(repo_root: Path, inventory: Iterable[str]) -> list[EvidenceFile]:
    evidence: list[EvidenceFile] = []
    for rel_path in inventory:
        path = repo_root / rel_path
        if not is_text_candidate(path):
            continue
        try:
            if path.stat().st_size > MAX_TEXT_BYTES:
                continue
            evidence.append(EvidenceFile(rel_path, read_text(path)))
        except OSError:
            continue
    return evidence


def find_token(evidence: list[EvidenceFile], token: str) -> list[Match]:
    matches: list[Match] = []
    for item in evidence:
        if token not in item.text:
            continue
        for line_number, line in enumerate(item.text.splitlines(), start=1):
            if token in line:
                matches.append(Match(item.rel_path, line_number, token))
                break
    return matches


def find_group(evidence: list[EvidenceFile], group: TokenGroup) -> list[Match]:
    matches: list[Match] = []
    for token in group.tokens:
        matches.extend(find_token(evidence, token))
    return sorted(matches, key=lambda match: (match.rel_path, match.line_number, match.token))


def validate_requirement(
    repo_root: Path,
    inventory: set[str],
    evidence: list[EvidenceFile],
    requirement: Requirement,
) -> RequirementResult:
    errors: list[str] = []
    matches: dict[str, list[Match]] = {}
    for rel_path in requirement.required_paths:
        if rel_path not in inventory and not (repo_root / rel_path).is_file():
            errors.append(f"missing required evidence path: {rel_path}")
    for group in requirement.groups:
        group_matches = find_group(evidence, group)
        matches[group.label] = group_matches[:5]
        if not group_matches:
            errors.append(
                f"missing token group {group.label!r}; expected one of: {', '.join(group.tokens)}"
            )
    return RequirementResult(requirement, errors, matches)


def read_csv_rows(repo_root: Path, rel_path: str) -> list[dict[str, str]]:
    path = repo_root / rel_path
    with path.open(newline="", encoding="utf-8") as handle:
        return list(csv.DictReader(handle))


def validate_resource_manifest(repo_root: Path) -> list[str]:
    errors: list[str] = []
    try:
        rows = read_csv_rows(repo_root, RESOURCE_MANIFEST)
    except OSError as exc:
        return [f"{RESOURCE_MANIFEST}: unreadable: {exc}"]

    by_family = {row.get("seed_family", ""): row for row in rows}
    required = {
        "charset",
        "charset_mapping",
        "collation",
        "locale",
        "uca",
        "uca_manifest",
        "i18n_version",
        "timezone_version",
        "timezone_source",
        "timezone_tables",
        "timezone_leaps",
        "timezone_archives",
    }
    missing = sorted(required - set(by_family))
    if missing:
        errors.append(f"{RESOURCE_MANIFEST}: missing seed families: {missing}")

    archives = by_family.get("timezone_archives", {})
    if archives.get("create_time_action") != "retain_source_provenance":
        errors.append(f"{RESOURCE_MANIFEST}: timezone_archives must retain_source_provenance")
    if "tzdata*.tar.gz" not in archives.get("source_pattern", ""):
        errors.append(f"{RESOURCE_MANIFEST}: timezone_archives missing tzdata source pattern")
    if "tzcode*.tar.gz" not in archives.get("source_pattern", ""):
        errors.append(f"{RESOURCE_MANIFEST}: timezone_archives missing tzcode source pattern")

    for family, row in sorted(by_family.items()):
        if row.get("status") != "specified":
            errors.append(f"{RESOURCE_MANIFEST}: {family} status must be specified")
        if not row.get("required_catalog_rows", "").strip():
            errors.append(f"{RESOURCE_MANIFEST}: {family} missing required_catalog_rows")
    return errors


def validate_resource_artifact_index(repo_root: Path) -> list[str]:
    errors: list[str] = []
    try:
        rows = read_csv_rows(repo_root, RESOURCE_ARTIFACTS)
    except OSError as exc:
        return [f"{RESOURCE_ARTIFACTS}: unreadable: {exc}"]

    required_columns = {"canonical_path", "content_hash", "content_size_bytes"}
    if rows:
        missing_columns = required_columns - set(rows[0])
        if missing_columns:
            errors.append(f"{RESOURCE_ARTIFACTS}: missing columns: {sorted(missing_columns)}")
    if not rows:
        errors.append(f"{RESOURCE_ARTIFACTS}: no rows")
        return errors

    indexed_paths = {row.get("canonical_path", "") for row in rows}
    for required in (
        "resources/collations/locales/locales_manifest.json",
        "resources/collations/uca/uca_manifest.json",
        "resources/timezones/tzdata2025c.tar.gz",
        "resources/timezones/tzcode2025c.tar.gz",
        "resources/timezones/version",
    ):
        if required not in indexed_paths:
            errors.append(f"{RESOURCE_ARTIFACTS}: missing artifact row: {required}")

    for row in rows:
        rel = row.get("canonical_path", "")
        content_hash = row.get("content_hash", "")
        size = row.get("content_size_bytes", "")
        if not content_hash.startswith("fnv1a64:"):
            errors.append(f"{RESOURCE_ARTIFACTS}: {rel} content_hash must use fnv1a64")
        if not size.isdigit() or int(size) <= 0:
            errors.append(f"{RESOURCE_ARTIFACTS}: {rel} content_size_bytes must be positive")
    return errors


def validate_sbom(repo_root: Path) -> list[str]:
    path = repo_root / SBOM
    try:
        payload = json.loads(path.read_text(encoding="utf-8"))
    except (OSError, json.JSONDecodeError) as exc:
        return [f"{SBOM}: invalid or unreadable JSON: {exc}"]

    errors: list[str] = []
    if payload.get("schema") != "scratchbird.public_source_review_sbom.v1":
        errors.append(f"{SBOM}: schema must be scratchbird.public_source_review_sbom.v1")
    components = payload.get("components")
    if not isinstance(components, list) or not components:
        errors.append(f"{SBOM}: components must be a non-empty list")
    return errors


def validate_no_draft_docs_scanned(evidence: list[EvidenceFile], inventory: Iterable[str]) -> list[str]:
    scanned = [item.rel_path for item in evidence]
    scanned.extend(inventory)
    draft_hits = sorted({path for path in scanned if path.startswith("docs/documentation/draft/")})
    if draft_hits:
        return [f"draft documentation was scanned: {path}" for path in draft_hits[:20]]
    return []


def render_result(result: RequirementResult, verbose: bool) -> list[str]:
    req = result.requirement
    lines = [f"{req.key} {req.title}: {'passed' if result.passed else 'failed'}"]
    if result.errors:
        for error in result.errors:
            lines.append(f"  error: {error}")
    if verbose:
        for label, matches in sorted(result.matches.items()):
            if not matches:
                continue
            rendered = "; ".join(match.render() for match in matches[:3])
            lines.append(f"  evidence {label}: {rendered}")
    return lines


def main() -> int:
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument("--repo-root", type=Path, default=Path(__file__).resolve().parents[3])
    parser.add_argument("--verbose", action="store_true", help="print matched evidence anchors")
    args = parser.parse_args()

    repo_root = args.repo_root.resolve()
    if not repo_root.is_dir():
        print(f"sbsql_multilingual_resource_edge_safety_gate=fail: repo root not found: {repo_root}", file=sys.stderr)
        return 2

    inventory_list = iter_inventory(repo_root)
    inventory = set(inventory_list)
    evidence = load_evidence(repo_root, inventory_list)

    results = [
        validate_requirement(repo_root, inventory, evidence, requirement)
        for requirement in REQUIREMENTS
    ]
    structural_errors: list[str] = []
    structural_errors.extend(validate_no_draft_docs_scanned(evidence, inventory_list))
    structural_errors.extend(validate_resource_manifest(repo_root))
    structural_errors.extend(validate_resource_artifact_index(repo_root))
    structural_errors.extend(validate_sbom(repo_root))

    failed = [result for result in results if not result.passed]
    if failed or structural_errors:
        print(
            "sbsql_multilingual_resource_edge_safety_gate=failed:"
            f" requirements_failed={len(failed)} structural_errors={len(structural_errors)}",
            file=sys.stderr,
        )
        for result in results:
            if not result.passed or args.verbose:
                for line in render_result(result, args.verbose):
                    print(line, file=sys.stderr)
        for error in structural_errors:
            print(f"structural error: {error}", file=sys.stderr)
        return 1

    print(
        "sbsql_multilingual_resource_edge_safety_gate=passed:"
        f" requirements={len(REQUIREMENTS)} files_scanned={len(evidence)}"
    )
    if args.verbose:
        for result in results:
            for line in render_result(result, True):
                print(line)
    return 0


if __name__ == "__main__":
    raise SystemExit(main())
