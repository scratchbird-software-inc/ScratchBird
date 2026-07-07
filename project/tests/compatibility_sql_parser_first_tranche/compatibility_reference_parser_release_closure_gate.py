#!/usr/bin/env python3
# Copyright (c) 2026 ScratchBird Software Inc.
#
# This Source Code Form is subject to the terms of the Mozilla Public
# License, v. 2.0. If a copy of the MPL was not distributed with this
# file, You can obtain one at https://mozilla.org/MPL/2.0/.
#
# SPDX-License-Identifier: MPL-2.0

"""Aggregate release-closure gates for reference parser implementation lanes."""

from __future__ import annotations

import argparse
import csv
import json
import os
import subprocess
import sys
from pathlib import Path


REQUIRED_SURFACE_KEYS = (
    "datatype_surfaces",
    "builtin_function_surfaces",
    "catalog_overlay_surfaces",
    "diagnostic_surfaces",
)

ALLOWED_SURFACE_OWNERS = {
    "$expr",
    "authority_invariant",
    "catalog_policy",
    "catalog_projection",
    "cluster_control_reserved",
    "compile_time_cluster_stub",
    "descriptor",
    "descriptor_affinity",
    "descriptor_alias",
    "descriptor_policy",
    "diagnostic_redaction",
    "engine_context",
    "fail_closed",
    "parser",
    "parser_support_udr",
    "policy_overlay",
    "sblr",
    "sblr_optional",
    "scratchbird_mga_authority",
    "security_projection",
    "session_descriptor",
    "trusted_package_or_refusal",
}


def require(condition: bool, message: str) -> None:
    if not condition:
        raise AssertionError(message)


def read_lanes(repo_root: Path) -> list[str]:
    manifest = repo_root / "project/src/parsers/compatibility/CompatibilityProfileManifest.csv"
    with manifest.open(newline="", encoding="utf-8-sig") as handle:
        rows = list(csv.DictReader(handle))
    lanes = [
        row["family_id"].strip()
        for row in rows
        if row.get("profile_class", "").strip() == "compatibility_emulation"
    ]
    require(len(lanes) == 25, f"expected 25 implementation lanes, found {len(lanes)}")
    return sorted(lanes)


def run(command: list[str], cwd: Path, timeout: int = 120) -> subprocess.CompletedProcess[str]:
    return subprocess.run(
        command,
        cwd=cwd,
        text=True,
        stdin=subprocess.DEVNULL,
        stdout=subprocess.PIPE,
        stderr=subprocess.PIPE,
        timeout=timeout,
        check=False,
    )


def parse_json_stdout(result: subprocess.CompletedProcess[str], label: str) -> dict[str, object]:
    require(result.returncode == 0, f"{label} returned {result.returncode}: {result.stderr}")
    try:
        value = json.loads(result.stdout)
    except json.JSONDecodeError as exc:
        raise AssertionError(f"{label} did not emit JSON: {result.stdout}") from exc
    require(isinstance(value, dict), f"{label} did not emit a JSON object")
    return value


def parser_binary(parser_bin_root: Path, lane: str) -> Path:
    binary = parser_bin_root / f"sbp_{lane}"
    candidates = [binary]
    if os.name == "nt" and binary.suffix.lower() != ".exe":
        candidates.append(binary.with_name(binary.name + ".exe"))
    for candidate in candidates:
        if candidate.exists():
            return candidate
    return candidates[-1]


def verify_identity_and_surface(repo_root: Path,
                                parser_bin_root: Path,
                                lanes: list[str]) -> list[dict[str, object]]:
    evidence: list[dict[str, object]] = []
    for lane in lanes:
        parser = parser_binary(parser_bin_root, lane)
        require(parser.is_file(), f"{lane}: missing parser binary {parser}")
        identity = parse_json_stdout(
            run([str(parser), "--package-identity"], repo_root),
            f"{lane} --package-identity",
        )
        require(identity.get("dialect") == lane, f"{lane}: identity dialect mismatch")
        require(identity.get("authority_policy") == "engine_sblr_mga_only",
                f"{lane}: authority policy drift")
        for key in (
            "reference_sql_execution",
            "reference_storage_authority",
            "reference_recovery_authority",
        ):
            require(identity.get(key) is False, f"{lane}: {key} must be false")
        require(identity.get("standalone_dialect_package") is True,
                f"{lane}: parser is not marked standalone")
        counts = identity.get("surface_counts")
        require(isinstance(counts, dict), f"{lane}: missing surface_counts")
        require(counts.get("parser_surface_rows", 0) > 0,
                f"{lane}: parser surface count is zero")

        surface = parse_json_stdout(
            run([str(parser), "--surface-report"], repo_root),
            f"{lane} --surface-report",
        )
        require(surface.get("dialect") == lane, f"{lane}: surface dialect mismatch")
        for key in REQUIRED_SURFACE_KEYS:
            rows = surface.get(key)
            require(isinstance(rows, list) and rows, f"{lane}: missing {key}")
            for row in rows:
                require(isinstance(row, dict), f"{lane}: malformed {key} row")
                owner = row.get("owner")
                require(owner in ALLOWED_SURFACE_OWNERS,
                        f"{lane}: unexpected {key} owner {owner!r}")
                require(row.get("family"), f"{lane}: {key} row missing family")
                require(row.get("surface"), f"{lane}: {key} row missing surface")
        diagnostic_owners = {
            row.get("owner")
            for row in surface["diagnostic_surfaces"]
            if isinstance(row, dict)
        }
        require("fail_closed" in diagnostic_owners or "authority_invariant" in diagnostic_owners,
                f"{lane}: diagnostics do not prove fail-closed/authority invariant")
        evidence.append({
            "lane": lane,
            "parser_binary": str(parser),
            "parser_surface_rows": counts.get("parser_surface_rows"),
            "datatype_surface_count": len(surface["datatype_surfaces"]),
            "function_surface_count": len(surface["builtin_function_surfaces"]),
            "catalog_surface_count": len(surface["catalog_overlay_surfaces"]),
            "diagnostic_surface_count": len(surface["diagnostic_surfaces"]),
        })
    return evidence


def run_full_lane_vectors(repo_root: Path,
                          parser_bin_root: Path,
                          build_root: Path,
                          mode: str) -> dict[str, object]:
    evidence_file = build_root / "tests/compatibility_sql_parser_first_tranche" / f"{mode}_full_lane_vectors.json"
    script = repo_root / "project/tests/compatibility_sql_parser_first_tranche/compatibility_reference_parser_full_lane_command_vectors.py"
    result = run(
        [
            sys.executable,
            str(script),
            "--repo-root",
            str(repo_root),
            "--parser-bin-root",
            str(parser_bin_root),
            "--evidence-file",
            str(evidence_file),
        ],
        repo_root,
        timeout=240,
    )
    require(result.returncode == 0, f"full-lane command vectors failed:\n{result.stdout}\n{result.stderr}")
    payload = json.loads(evidence_file.read_text(encoding="utf-8"))
    require(payload.get("lane_count") == 25, "full-lane vector evidence must cover 25 lanes")
    require(payload.get("file_presence_is_completion") is False,
            "full-lane vector gate must reject file-presence completion")
    vector_total = 0
    fail_closed_total = 0
    for lane in payload.get("lanes", []):
        require(lane.get("vector_count", 0) > 0,
                f"{lane.get('lane')}: no command vectors executed")
        vector_total += int(lane.get("vector_count", 0))
        fail_closed_total += int(lane.get("fail_closed_count", 0))
        for vector in lane.get("vectors", []):
            require(vector.get("status") == "passed",
                    f"{lane.get('lane')}: vector failed {vector}")
    require(vector_total >= 1000, f"expected at least 1000 vectors, saw {vector_total}")
    require(fail_closed_total > 0, "no fail-closed vectors were proven")
    return {
        "vector_total": vector_total,
        "fail_closed_total": fail_closed_total,
        "evidence_file": str(evidence_file),
    }


def verify_negative_security_resource(repo_root: Path,
                                      parser_bin_root: Path,
                                      lanes: list[str]) -> list[dict[str, object]]:
    evidence: list[dict[str, object]] = []
    for lane in lanes:
        parser = parser_binary(parser_bin_root, lane)
        result = run([str(parser), "__SCRATCHBIRD_UNSUPPORTED_AUTHORITY_PROBE__"], repo_root)
        require(result.returncode != 0, f"{lane}: unsupported probe was admitted")
        combined = result.stdout + result.stderr
        require(
            "PARSE.UNSUPPORTED_SURFACE" in combined or
            "PARSE.INVALID_INPUT" in combined,
            f"{lane}: unsupported probe did not return parse refusal: {combined}",
        )
        require("SBLRExecutionEnvelope.v3" not in result.stdout,
                f"{lane}: unsupported probe emitted an executable envelope")
        identity = parse_json_stdout(
            run([str(parser), "--package-identity"], repo_root),
            f"{lane} --package-identity",
        )
        require(identity.get("reference_storage_authority") is False,
                f"{lane}: reference storage authority drift")
        require(identity.get("reference_recovery_authority") is False,
                f"{lane}: reference recovery authority drift")
        evidence.append({"lane": lane, "unsupported_probe_refused": True})
    return evidence


def verify_reference_packaging(repo_root: Path,
                               parser_bin_root: Path,
                               release_root: Path,
                               lanes: list[str]) -> dict[str, object]:
    promote = repo_root / "packaging/scripts/promote_reference_parser_release_artifacts.py"
    result = run(
        [
            sys.executable,
            str(promote),
            "--repo-root",
            str(repo_root),
            "--release-root",
            str(release_root),
            "--build-bin-root",
            str(parser_bin_root),
            "--verify-only",
        ],
        repo_root,
    )
    require(result.returncode == 0, f"reference parser packaging verify failed:\n{result.stdout}\n{result.stderr}")
    package_root = release_root / "reference-parsers"
    require((package_root / "package_manifest.json").is_file(),
            "reference parser package manifest missing")
    require((package_root / "SBOM.json").is_file(), "reference parser SBOM missing")
    require((package_root / "SHA256SUMS").is_file(), "reference parser SHA256SUMS missing")
    require((package_root / "proofs/reference_parser_packaging_handoff.json").is_file(),
            "reference parser handoff proof missing")
    for lane in lanes:
        require(parser_binary(package_root / "bin", lane).is_file(),
                f"{lane}: packaged parser worker missing")
        require((release_root / "udr/optional-parser-support/lib" / f"libsbu_{lane}_parser_support.a").is_file(),
                f"{lane}: packaged support UDR missing")
    file_location = release_root / "FILE_LOCATION_MANIFEST.json"
    payload = json.loads(file_location.read_text(encoding="utf-8"))
    paths = {
        row.get("path")
        for row in payload.get("files", [])
        if isinstance(row, dict)
    }
    for lane in lanes:
        parser_paths = {
            f"reference-parsers/bin/sbp_{lane}",
            f"reference-parsers/bin/sbp_{lane}.exe",
        }
        require(any(path in paths for path in parser_paths),
                f"{lane}: file location manifest missing parser worker")
    return {"package_root": str(package_root), "lane_count": len(lanes)}


def main() -> int:
    parser = argparse.ArgumentParser()
    parser.add_argument("--repo-root", required=True, type=Path)
    parser.add_argument("--parser-bin-root", required=True, type=Path)
    parser.add_argument("--build-root", required=True, type=Path)
    parser.add_argument("--release-root", type=Path)
    parser.add_argument("--mode", choices=("exactness", "wire-security-resource", "packaging"), required=True)
    parser.add_argument("--evidence-file", required=True, type=Path)
    args = parser.parse_args()

    repo_root = args.repo_root.resolve()
    parser_bin_root = args.parser_bin_root.resolve()
    build_root = args.build_root.resolve()
    release_root = args.release_root
    if release_root is None:
        release_root = repo_root / "packaging" / "2026.07.03"
    elif not release_root.is_absolute():
        release_root = repo_root / release_root
    lanes = read_lanes(repo_root)
    payload: dict[str, object] = {
        "schema_id": "scratchbird.reference_parser_release_closure_gate.v1",
        "mode": args.mode,
        "lane_count": len(lanes),
        "engine_authority": "scratchbird_sblr_uuid_only",
        "reference_engine_authority": False,
        "mga_transaction_authority": True,
    }
    if args.mode == "exactness":
        payload["surface_evidence"] = verify_identity_and_surface(repo_root, parser_bin_root, lanes)
        payload["command_vector_evidence"] = run_full_lane_vectors(
            repo_root, parser_bin_root, build_root, args.mode
        )
    elif args.mode == "wire-security-resource":
        payload["surface_evidence"] = verify_identity_and_surface(repo_root, parser_bin_root, lanes)
        payload["negative_security_resource_evidence"] = verify_negative_security_resource(
            repo_root, parser_bin_root, lanes
        )
        payload["command_vector_evidence"] = run_full_lane_vectors(
            repo_root, parser_bin_root, build_root, args.mode
        )
    else:
        payload["packaging_evidence"] = verify_reference_packaging(
            repo_root, parser_bin_root, release_root.resolve(), lanes
        )
    args.evidence_file.parent.mkdir(parents=True, exist_ok=True)
    args.evidence_file.write_text(json.dumps(payload, indent=2, sort_keys=True) + "\n", encoding="utf-8")
    print(f"compatibility_reference_parser_{args.mode}=passed")
    return 0


if __name__ == "__main__":
    raise SystemExit(main())
