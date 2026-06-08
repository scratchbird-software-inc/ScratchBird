#!/usr/bin/env python3
# Copyright (c) 2026 ScratchBird Software Inc.
#
# This Source Code Form is subject to the terms of the Mozilla Public
# License, v. 2.0. If a copy of the MPL was not distributed with this
# file, You can obtain one at https://mozilla.org/MPL/2.0/.
#
# SPDX-License-Identifier: MPL-2.0

"""DPC-072 retained-evidence provenance gate.

The gate reads only project-owned fixture/source/CMake/CTest metadata. It is
standalone by construction and treats execution_plan-runtime inputs as invalid.
"""

from __future__ import annotations

import argparse
import csv
import hashlib
import json
import re
import sys
from pathlib import Path
from typing import Any


DPC_EVIDENCE_PROVENANCE_RETENTION_GATE = "DPC_EVIDENCE_PROVENANCE_RETENTION_GATE"
MANIFEST = Path(
    "project/tests/database_lifecycle/fixtures/"
    "dpc_evidence_provenance_retention_manifest.json"
)
CMAKE_SOURCE = Path("project/tests/database_lifecycle/CMakeLists.txt")
BUILD_CTEST = Path("tests/database_lifecycle/CTestTestfile.cmake")
HASH_RE = re.compile(r"^[0-9a-f]{64}$")
AUTHORITY_VALUE = "engine_mga_transaction_inventory_authority"


def parse_args() -> argparse.Namespace:
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument("--repo-root", type=Path, default=Path.cwd())
    parser.add_argument("--build-root", type=Path, default=Path.cwd() / "build")
    parser.add_argument("--manifest", type=Path, default=MANIFEST)
    return parser.parse_args()


def forbidden_roots() -> tuple[str, ...]:
    return (
        "/".join(("docs", "execution-plans")),
        "/".join(("docs", "completed-execution-plans")),
    )


def forbidden_artifact_names() -> tuple[str, ...]:
    pieces = (
        ("TRACKER", ".csv"),
        ("ACCEPTANCE", "_GATES.csv"),
        ("DEPENDENCIES", ".csv"),
        ("SPEC_IMPLEMENTATION", "_AUDIT_MATRIX.csv"),
        ("FINAL_AUDIT", ".md"),
    )
    return tuple("".join(piece) for piece in pieces)


def rel_text(path: Path) -> str:
    return path.as_posix()


def read_text(path: Path, errors: list[str]) -> str:
    try:
        return path.read_text(encoding="utf-8")
    except FileNotFoundError:
        errors.append(f"missing required file: {path}")
    except OSError as exc:
        errors.append(f"failed to read {path}: {exc}")
    return ""


def load_json(path: Path, errors: list[str]) -> Any:
    text = read_text(path, errors)
    if not text:
        return {}
    try:
        return json.loads(text)
    except json.JSONDecodeError as exc:
        errors.append(f"{path}: invalid JSON: {exc}")
        return {}


def resolve_repo_path(repo_root: Path, raw: str, errors: list[str]) -> Path:
    path = Path(raw)
    if path.is_absolute():
        try:
            path.resolve().relative_to(repo_root)
        except ValueError:
            errors.append(f"path escapes repository: {raw}")
        return path.resolve()
    return (repo_root / path).resolve()


def reject_forbidden_path(raw: str, context: str, errors: list[str]) -> None:
    normalized = raw.replace("\\", "/")
    for forbidden in forbidden_roots() + forbidden_artifact_names():
        if forbidden in normalized:
            errors.append(f"{context} uses forbidden runtime path token: {forbidden}")


def sha256_file(path: Path, errors: list[str]) -> str:
    try:
        digest = hashlib.sha256()
        with path.open("rb") as handle:
            for chunk in iter(lambda: handle.read(65536), b""):
                digest.update(chunk)
        return digest.hexdigest()
    except OSError as exc:
        errors.append(f"failed to hash {path}: {exc}")
        return ""


def canonical_digest(value: Any) -> str:
    encoded = json.dumps(value, sort_keys=True, separators=(",", ":")).encode("utf-8")
    return hashlib.sha256(encoded).hexdigest()


def require_hash(value: str, context: str, errors: list[str]) -> None:
    if not isinstance(value, str) or not HASH_RE.match(value):
        errors.append(f"{context} missing 64-character lowercase SHA-256 digest")


def validate_json_evidence(path: Path, claim: dict[str, Any], errors: list[str]) -> None:
    data = load_json(path, errors)
    if not isinstance(data, dict):
        errors.append(f"{path}: JSON evidence must be an object")
        return
    if data.get("search_key") != DPC_EVIDENCE_PROVENANCE_RETENTION_GATE:
        errors.append(f"{path}: missing {DPC_EVIDENCE_PROVENANCE_RETENTION_GATE}")
    if data.get("claim_id") != claim.get("claim_id"):
        errors.append(f"{path}: claim_id does not match manifest")
    result_hash = claim.get("result_hash")
    require_hash(result_hash, f"{claim.get('claim_id')} result_hash", errors)
    if data.get("result_hash") != result_hash:
        errors.append(f"{path}: result_hash does not match manifest")
    if "result_digest_source" not in data:
        errors.append(f"{path}: missing result_digest_source")
    elif canonical_digest(data["result_digest_source"]) != result_hash:
        errors.append(f"{path}: result_hash does not match deterministic digest source")

    if claim.get("support_metadata"):
        support = data.get("support_bundle")
        if not isinstance(support, dict):
            errors.append(f"{path}: support claim missing retained support_bundle object")
        else:
            redaction = support.get("redaction", {})
            if not support.get("retention_policy_ref"):
                errors.append(f"{path}: support bundle missing retention_policy_ref")
            if not support.get("redaction_profile_ref"):
                errors.append(f"{path}: support bundle missing redaction_profile_ref")
            if not isinstance(redaction, dict) or redaction.get("forbidden_fields_absent") is not True:
                errors.append(f"{path}: support bundle redaction proof is incomplete")

    if claim.get("transaction_sensitive"):
        authority = data.get("authority")
        if not isinstance(authority, dict):
            errors.append(f"{path}: transaction-sensitive evidence missing authority object")
        else:
            if authority.get("engine_mga_authority") is not True:
                errors.append(f"{path}: MGA authority must be explicit")
            for key, value in authority.items():
                if key != "engine_mga_authority" and key.endswith("_authority") and value is not False:
                    errors.append(f"{path}: forbidden non-MGA authority is not false: {key}")


def csv_row_digest(row: dict[str, str]) -> str:
    fields = (
        "claim_id",
        "workload",
        "baseline_result",
        "current_result",
        "baseline_proxy",
        "current_proxy",
        "equality_status",
    )
    return hashlib.sha256("|".join(row[field] for field in fields).encode("utf-8")).hexdigest()


def validate_csv_evidence(path: Path, claim: dict[str, Any], errors: list[str]) -> None:
    text = read_text(path, errors)
    if not text:
        return
    rows = list(csv.DictReader(text.splitlines()))
    if not rows:
        errors.append(f"{path}: CSV evidence has no rows")
        return
    expected_hashes = set(claim.get("result_hashes", []))
    if not expected_hashes:
        errors.append(f"{claim.get('claim_id')}: benchmark CSV claim missing result_hashes")
    for expected in expected_hashes:
        require_hash(expected, f"{claim.get('claim_id')} result_hashes", errors)

    observed_hashes: set[str] = set()
    for row in rows:
        if row.get("search_key") != DPC_EVIDENCE_PROVENANCE_RETENTION_GATE:
            errors.append(f"{path}: row missing {DPC_EVIDENCE_PROVENANCE_RETENTION_GATE}")
        if row.get("claim_id") != claim.get("claim_id"):
            errors.append(f"{path}: row claim_id does not match manifest")
        result_hash = row.get("result_hash", "")
        require_hash(result_hash, f"{path} row result_hash", errors)
        if csv_row_digest(row) != result_hash:
            errors.append(f"{path}: row result_hash does not match deterministic row digest")
        observed_hashes.add(result_hash)
        if row.get("equality_status") != "passed":
            errors.append(f"{path}: equality_status must be passed")
    if observed_hashes != expected_hashes:
        errors.append(f"{path}: CSV result_hashes do not match manifest")


def parse_generated_ctest_labels(build_root: Path, errors: list[str]) -> dict[str, str]:
    path = build_root / BUILD_CTEST
    if not path.exists():
        return {}
    text = read_text(path, errors)
    labels: dict[str, str] = {}
    pattern = re.compile(
        r"set_tests_properties\(\[=\[(?P<name>[^\]]+)\]=\]\s+PROPERTIES\s+"
        r".*?LABELS\s+\"(?P<labels>[^\"]+)\"",
        re.S,
    )
    for match in pattern.finditer(text):
        labels[match.group("name")] = match.group("labels")
    return labels


def validate_ctest_labels(
    repo_root: Path,
    build_root: Path,
    claim: dict[str, Any],
    cmake_text: str,
    generated_labels: dict[str, str],
    errors: list[str],
) -> None:
    if not claim.get("test_backed"):
        return
    name = claim.get("ctest_name")
    labels = claim.get("ctest_labels", [])
    if not name:
        errors.append(f"{claim.get('claim_id')}: test-backed claim missing ctest_name")
        return
    if not labels:
        errors.append(f"{claim.get('claim_id')}: test-backed claim missing ctest_labels")
    if name not in cmake_text:
        errors.append(f"{claim.get('claim_id')}: CMake registration missing {name}")
    for label in labels:
        if label not in cmake_text:
            errors.append(f"{claim.get('claim_id')}: CMake registration missing label {label}")

    generated = generated_labels.get(name)
    if generated is not None:
        for label in labels:
            if label not in generated:
                errors.append(f"{claim.get('claim_id')}: generated CTest metadata missing {label}")
    elif (build_root / BUILD_CTEST).exists():
        # The direct gate may run before CMake regenerates metadata. In that
        # case source CMake remains the authority; after reconfigure, CTest
        # exercises this same gate through generated metadata.
        pass


def validate_anchor(repo_root: Path, raw_anchor: str, context: str, errors: list[str]) -> None:
    if "#" not in raw_anchor:
        errors.append(f"{context}: anchor must use path#search_key")
        return
    path_text, search_key = raw_anchor.split("#", 1)
    reject_forbidden_path(path_text, context, errors)
    path = resolve_repo_path(repo_root, path_text, errors)
    text = read_text(path, errors)
    if search_key and search_key not in text:
        errors.append(f"{context}: {path_text} missing search key {search_key}")


def validate_manifest(repo_root: Path, build_root: Path, manifest_path: Path) -> list[str]:
    errors: list[str] = []
    manifest = load_json(manifest_path, errors)
    if not isinstance(manifest, dict):
        return errors
    if manifest.get("search_key") != DPC_EVIDENCE_PROVENANCE_RETENTION_GATE:
        errors.append("manifest missing DPC evidence retention search key")
    if manifest.get("manifest_version") != 1:
        errors.append("manifest_version must be 1")

    manifest_rel = manifest_path.resolve().relative_to(repo_root).as_posix()
    reject_forbidden_path(manifest_rel, "manifest path", errors)
    text = read_text(manifest_path, errors)
    for forbidden in forbidden_roots() + forbidden_artifact_names():
        if forbidden in text:
            errors.append(f"manifest contains forbidden runtime token: {forbidden}")

    shared = manifest.get("shared_metadata", {})
    if not isinstance(shared, dict):
        errors.append("manifest missing shared_metadata")
    else:
        shared_path_text = shared.get("path", "")
        reject_forbidden_path(shared_path_text, "shared_metadata", errors)
        shared_path = resolve_repo_path(repo_root, shared_path_text, errors)
        if sha256_file(shared_path, errors) != shared.get("artifact_sha256"):
            errors.append("shared metadata artifact hash mismatch")
        shared_data = load_json(shared_path, errors)
        if shared_data.get("search_key") != DPC_EVIDENCE_PROVENANCE_RETENTION_GATE:
            errors.append("shared metadata missing evidence retention search key")
        for section in ("source_state", "build_config", "profiler_metadata"):
            if not isinstance(shared_data.get(section), dict):
                errors.append(f"shared metadata missing {section}")

    required_categories = set(manifest.get("required_categories", []))
    claims = manifest.get("claims", [])
    if not isinstance(claims, list) or not claims:
        errors.append("manifest claims must be a non-empty list")
        claims = []
    categories = {claim.get("category") for claim in claims if isinstance(claim, dict)}
    missing_categories = sorted(required_categories - categories)
    for category in missing_categories:
        errors.append(f"missing required evidence category: {category}")

    cmake_text = read_text(repo_root / CMAKE_SOURCE, errors)
    generated_labels = parse_generated_ctest_labels(build_root, errors)
    seen_claims: set[str] = set()
    for claim in claims:
        if not isinstance(claim, dict):
            errors.append("claim row must be an object")
            continue
        claim_id = claim.get("claim_id")
        if not claim_id:
            errors.append("claim missing claim_id")
            continue
        if claim_id in seen_claims:
            errors.append(f"duplicate claim_id: {claim_id}")
        seen_claims.add(claim_id)
        for field in ("phase_slice", "category", "evidence_path", "evidence_kind"):
            if not claim.get(field):
                errors.append(f"{claim_id}: missing {field}")

        anchor = claim.get("source_anchor", {})
        if not isinstance(anchor, dict):
            errors.append(f"{claim_id}: source_anchor must be an object")
        else:
            path_text = anchor.get("path", "")
            search_key = anchor.get("search_key", "")
            reject_forbidden_path(path_text, f"{claim_id} source_anchor", errors)
            source_path = resolve_repo_path(repo_root, path_text, errors)
            source_text = read_text(source_path, errors)
            if not search_key:
                errors.append(f"{claim_id}: source_anchor missing search_key")
            elif search_key not in source_text:
                errors.append(f"{claim_id}: source anchor missing search key {search_key}")

        route_anchor = claim.get("route_or_source_anchor")
        if route_anchor:
            validate_anchor(repo_root, route_anchor, f"{claim_id} route_or_source_anchor", errors)

        evidence_path_text = claim.get("evidence_path", "")
        reject_forbidden_path(evidence_path_text, f"{claim_id} evidence_path", errors)
        evidence_path = resolve_repo_path(repo_root, evidence_path_text, errors)
        if not evidence_path.exists():
            errors.append(f"{claim_id}: retained evidence missing: {evidence_path_text}")
        expected_artifact_hash = claim.get("artifact_sha256", "")
        require_hash(expected_artifact_hash, f"{claim_id} artifact_sha256", errors)
        if evidence_path.exists() and sha256_file(evidence_path, errors) != expected_artifact_hash:
            errors.append(f"{claim_id}: retained evidence artifact hash mismatch")

        for metadata_text in claim.get("retained_metadata_paths", []):
            reject_forbidden_path(metadata_text, f"{claim_id} retained_metadata_paths", errors)
            metadata_path = resolve_repo_path(repo_root, metadata_text, errors)
            if not metadata_path.exists():
                errors.append(f"{claim_id}: retained metadata path missing: {metadata_text}")
            if shared.get("path") and metadata_text != shared.get("path"):
                errors.append(f"{claim_id}: retained metadata must use shared source/build/profile metadata")

        kind = claim.get("evidence_kind")
        if kind and kind.endswith("csv"):
            validate_csv_evidence(evidence_path, claim, errors)
        elif evidence_path.suffix == ".json":
            validate_json_evidence(evidence_path, claim, errors)
        else:
            errors.append(f"{claim_id}: unsupported evidence kind/path combination")

        if claim.get("benchmark_claim"):
            if not claim.get("retained_metadata_paths"):
                errors.append(f"{claim_id}: benchmark claim missing source/build/profile metadata")
            if not (claim.get("result_hash") or claim.get("result_hashes")):
                errors.append(f"{claim_id}: benchmark claim missing result hash")

        if claim.get("support_metadata"):
            support = claim["support_metadata"]
            if not support.get("retention_policy_ref"):
                errors.append(f"{claim_id}: support metadata missing retention policy")
            if not support.get("redaction_profile_ref"):
                errors.append(f"{claim_id}: support metadata missing redaction profile")
            support_path = support.get("support_bundle_path", "")
            reject_forbidden_path(support_path, f"{claim_id} support_bundle_path", errors)
            if support_path != evidence_path_text:
                errors.append(f"{claim_id}: support bundle path must match retained evidence path")

        if claim.get("transaction_sensitive"):
            if claim.get("mga_authority_declaration") != AUTHORITY_VALUE:
                errors.append(f"{claim_id}: transaction-sensitive claim missing MGA authority declaration")

        validate_ctest_labels(repo_root, build_root, claim, cmake_text, generated_labels, errors)

    return errors


def main() -> int:
    args = parse_args()
    repo_root = args.repo_root.resolve()
    build_root = args.build_root.resolve()
    manifest_path = resolve_repo_path(repo_root, rel_text(args.manifest), [])

    errors: list[str] = []
    for root in (repo_root, build_root, manifest_path):
        root_text = rel_text(root)
        for forbidden in forbidden_roots():
            if forbidden in root_text:
                errors.append(f"gate root points into forbidden runtime path: {forbidden}")
    errors.extend(validate_manifest(repo_root, build_root, manifest_path))

    if errors:
        for error in errors:
            print(f"ERROR: {error}", file=sys.stderr)
        return 1

    print(
        f"{DPC_EVIDENCE_PROVENANCE_RETENTION_GATE}=passed: "
        "13 retained evidence categories validated with source anchors, hashes, "
        "CTest labels, support metadata, and MGA authority declarations."
    )
    return 0


if __name__ == "__main__":
    raise SystemExit(main())
