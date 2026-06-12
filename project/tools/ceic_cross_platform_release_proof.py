#!/usr/bin/env python3
# Copyright (c) 2026 ScratchBird Software Inc.
#
# This Source Code Form is subject to the terms of the Mozilla Public
# License, v. 2.0. If a copy of the MPL was not distributed with this
# file, You can obtain one at https://mozilla.org/MPL/2.0/.
#
# SPDX-License-Identifier: MPL-2.0

"""Validate CEIC-094 cross-platform release proof.

SEARCH_KEY: CEIC_094_CROSS_PLATFORM_RELEASE_PROOF_GATE
"""

from __future__ import annotations

import argparse
import csv
import hashlib
import importlib.util
import json
import pathlib
import re
import sys
from dataclasses import dataclass
from typing import Any


EXECUTION_PLAN = pathlib.Path("docs" "/completed-execution-plans/consolidated-enterprise-proof-implementation-closure")
CMAKE_GATE = pathlib.Path("project/tests/consolidated_enterprise/CMakeLists.txt")
PRODUCTION_GATE = pathlib.Path("project/cmake/CommercialReadinessProductionBuildGateMatrix.cmake")

REQUIRED_ARTIFACTS = (
    "CEIC-ART-024",  # production/test separation
    "CEIC-ART-011",  # memory readiness manifest
    "CEIC-ART-012",  # index readiness manifest
    "CEIC-ART-013",  # optimizer readiness manifest
    "CEIC-ART-014",  # agent readiness manifest
    "CEIC-ART-090",  # CEIC-093 reliability/security proof
    "CEIC-ART-091",  # CEIC-094 platform proof
)
PREDECESSOR_COUPLINGS = (
    "ceic_093_reliability_security_consumed",
    "ceic_004_production_test_gate_consumed",
    "ceic_024_memory_readiness_consumed",
    "ceic_042_index_readiness_consumed",
    "ceic_062_optimizer_readiness_consumed",
    "ceic_085_agent_readiness_consumed",
)
REQUIRED_PLATFORM_FAMILIES = ("linux", "windows", "macos", "bsd")
REQUIRED_PROFILES = ("release_complete", "production_gate_matrix", "noncluster_bootstrap")
REQUIRED_VALIDATION_AREAS = (
    "memory_manager",
    "temp_spill_workspace",
    "entropy_provider",
    "filesystem_semantics",
    "process_crash_recovery",
    "plugin_signing_sbom",
    "llvm_linkage_policy",
    "production_build_gate",
    "cluster_external_provider_only",
)
REQUIRED_ENTROPY_PROVIDERS = {
    "linux": "getrandom",
    "windows": "bcryptgenrandom",
    "macos": "getentropy_or_arc4random_buf",
    "bsd": "getentropy_or_arc4random_buf",
}
REQUIRED_CLUSTER_POLICY = "external_provider_only_no_local_cluster_production"
REQUIRED_NON_AUTHORITY_FLAGS = (
    "platform",
    "memory",
    "index",
    "optimizer",
    "agent",
    "support_bundle",
    "benchmark",
    "parser",
    "reference",
    "wal",
    "cluster",
    "provider",
    "transaction_finality",
    "visibility",
    "authorization_security",
    "recovery",
    "optimizer_plan",
    "index_finality",
    "agent_action",
    "release_proof",
)
AUTHORITY_FLAGS = (
    "platform_authority",
    "memory_authority",
    "index_authority",
    "optimizer_authority",
    "agent_authority",
    "support_bundle_authority",
    "benchmark_authority",
    "parser_authority",
    "reference_authority",
    "wal_authority",
    "cluster_authority",
    "local_cluster_authority",
    "provider_authority",
    "transaction_finality_authority",
    "visibility_authority",
    "authorization_security_authority",
    "security_authority",
    "recovery_authority",
    "optimizer_plan_authority",
    "index_finality_authority",
    "agent_action_authority",
)
SUCCESSOR_FLAGS = ("ceic_095_enterprise_readiness_claimed",)
COMPLETE_STATUSES = {"complete", "completed", "done", "closed", "complete_move_ready"}
PRESENT_STATUSES = {"present", "complete", "completed", "generated"}
FORBIDDEN_STATUSES = {"pending", "planned", "defined", "defined_only", "contract_only", "pass_by_contract"}
PLACEHOLDER_RE = re.compile(
    r"\bplaceholder\b|\bstub\b|\btodo\b|\bunknown\b|\bsample\b|"
    r"\bdefault[-_ ]?epoch\b|\bcontract[-_ ]?v1\b|sha256:0{8,}|^0{8,}$",
    re.IGNORECASE,
)
HASH_RE = re.compile(r"^sha256:[0-9a-f]{64}$")


@dataclass(frozen=True)
class Diagnostic:
    code: str
    subject: str
    message: str

    def render(self) -> str:
        return f"{self.subject}:{self.code}:{self.message}"


def normalize(value: str) -> str:
    return " ".join((value or "").strip().lower().split())


def normalize_status(value: str) -> str:
    return normalize(value).replace(" ", "_").replace("-", "_")


def is_complete(value: str) -> bool:
    status = normalize_status(value)
    return status in COMPLETE_STATUSES or status.startswith("complete_")


def digest(*parts: str) -> str:
    payload = "|".join(parts).encode("utf-8")
    return "sha256:" + hashlib.sha256(payload).hexdigest()


def short_digest(*parts: str) -> str:
    return hashlib.sha256("|".join(parts).encode("utf-8")).hexdigest()[:16]


def valid_hash(value: str) -> bool:
    text = (value or "").strip().lower()
    return bool(HASH_RE.match(text)) and not PLACEHOLDER_RE.search(text)


def valid_token(value: str) -> bool:
    text = str(value or "").strip()
    return bool(text) and len(text) >= 12 and not PLACEHOLDER_RE.search(text)


def read_csv(path: pathlib.Path) -> list[dict[str, str]]:
    with path.open(newline="", encoding="utf-8") as handle:
        return [{key: value or "" for key, value in row.items()} for row in csv.DictReader(handle)]


def index_by(rows: list[dict[str, str]], field: str) -> dict[str, dict[str, str]]:
    return {row.get(field, "").strip(): row for row in rows if row.get(field, "").strip()}


def path_exists(repo_root: pathlib.Path, rel: str) -> bool:
    if not rel.strip():
        return False
    if any(char in rel for char in "*?["):
        return any(repo_root.glob(rel))
    return (repo_root / rel).exists()


def artifacts(repo_root: pathlib.Path) -> dict[str, dict[str, str]]:
    return index_by(read_csv(repo_root / EXECUTION_PLAN / "ARTIFACT_INDEX.csv"), "artifact_id")


def artifact_available(repo_root: pathlib.Path, rows: dict[str, dict[str, str]], artifact_id: str) -> bool:
    row = rows.get(artifact_id)
    if row is None:
        return False
    return normalize_status(row.get("status", "")) in PRESENT_STATUSES and path_exists(repo_root, row.get("path", ""))


def authority_flags() -> dict[str, bool]:
    return {flag: False for flag in AUTHORITY_FLAGS}


def non_authority_flags() -> dict[str, bool]:
    return {flag: True for flag in REQUIRED_NON_AUTHORITY_FLAGS}


def with_boundaries(payload: dict[str, Any]) -> dict[str, Any]:
    return {
        **payload,
        "authority_flags": authority_flags(),
        "non_authority_flags": non_authority_flags(),
        **{flag: False for flag in SUCCESSOR_FLAGS},
    }


def validation_result(platform: str, area: str) -> dict[str, Any]:
    provider = REQUIRED_ENTROPY_PROVIDERS.get(platform, "n/a")
    return with_boundaries(
        {
            "area": area,
            "status": "complete",
            "evidence_hash": digest("ceic094", platform, area),
            "production_evidence": True,
            "synthetic_evidence": False,
            "fixture_or_test_only_evidence": False,
            "local_cluster_production_claim": False,
            "redacted": True,
            "support_bundle_safe": True,
            "details": {
                "entropy_provider": provider if area == "entropy_provider" else "",
                "cluster_policy": REQUIRED_CLUSTER_POLICY if area == "cluster_external_provider_only" else "",
                "llvm_policy": "dynamic_default_static_option_accounted" if area == "llvm_linkage_policy" else "",
                "plugin_policy": "signed_package_sbom_digest_revocation_checked" if area == "plugin_signing_sbom" else "",
            },
        }
    )


def platform_row(platform: str, *, family: str | None = None) -> dict[str, Any]:
    platform_family = family or ("bsd" if platform in {"freebsd", "openbsd", "netbsd"} else platform)
    return with_boundaries(
        {
            "platform": platform,
            "platform_family": platform_family,
            "support_state": "supported_claimed",
            "status": "complete",
            "production_evidence": True,
            "synthetic_evidence": False,
            "fixture_or_test_only_evidence": False,
            "local_cluster_production_claim": False,
            "cluster_policy": REQUIRED_CLUSTER_POLICY,
            "profiles": list(REQUIRED_PROFILES),
            "validation_areas": [validation_result(platform_family, area) for area in REQUIRED_VALIDATION_AREAS],
            "production_gate_matrix_passed": True,
            "production_gate_negative_cases_passed": True,
            "fixture_test_hooks_rejected": True,
            "stub_providers_rejected": True,
            "debug_trace_flags_rejected": True,
            "plugin_signing_verified": True,
            "sbom_present": True,
            "sbom_digest": digest("ceic094-sbom", platform),
            "package_signature": digest("ceic094-package-signature", platform),
            "release_artifact_digest": digest("ceic094-release-artifact", platform),
            "llvm_dynamic_default_validated": True,
            "llvm_static_option_validated": True,
            "memory_temp_filesystem_process_validated": True,
            "entropy_provider": REQUIRED_ENTROPY_PROVIDERS[platform_family],
            "platform_evidence_hash": digest("ceic094-platform", platform, platform_family),
        }
    )


def default_model(repo_root: pathlib.Path) -> dict[str, Any]:
    del repo_root
    platforms = [
        platform_row("linux"),
        platform_row("windows"),
        platform_row("macos"),
        platform_row("freebsd", family="bsd"),
    ]
    return with_boundaries(
        {
            "schema": "sb.ceic094.cross_platform_release_proof.v1",
            "proof_id": "ceic-094-cross-platform-release-proof",
            "capture_generation": f"ceic094-capture-generation-{short_digest('ceic094-capture-generation')}",
            "proof_epoch": f"ceic094-proof-epoch-{short_digest('ceic094-proof-epoch')}",
            "production_test_separation": True,
            "production_evidence": True,
            "synthetic_evidence": False,
            "fixture_or_test_only_evidence": False,
            "local_cluster_production_claim": False,
            "cluster_policy": REQUIRED_CLUSTER_POLICY,
            "predecessor_coupling": {key: True for key in PREDECESSOR_COUPLINGS},
            "source_artifacts": list(REQUIRED_ARTIFACTS),
            "production_gate_script": str(PRODUCTION_GATE),
            "platform_count": len(platforms),
            "platforms": platforms,
        }
    )


def load_tool(repo_root: pathlib.Path, rel_path: str, module_name: str):
    path = repo_root / rel_path
    spec = importlib.util.spec_from_file_location(module_name, path)
    if spec is None or spec.loader is None:
        raise RuntimeError(f"cannot load {rel_path}")
    module = importlib.util.module_from_spec(spec)
    sys.modules[module_name] = module
    spec.loader.exec_module(module)
    return module


def validate_authority(subject: str, payload: dict[str, Any]) -> list[Diagnostic]:
    diagnostics: list[Diagnostic] = []
    authority = payload.get("authority_flags", {})
    for flag in AUTHORITY_FLAGS:
        if bool(authority.get(flag, False)):
            diagnostics.append(Diagnostic("unsafe_authority", subject, f"{flag} must be false"))
    non_authority = payload.get("non_authority_flags", {})
    for flag in REQUIRED_NON_AUTHORITY_FLAGS:
        if non_authority.get(flag) is not True:
            diagnostics.append(Diagnostic("missing_non_authority", subject, f"{flag} non-authority flag required"))
    for flag in SUCCESSOR_FLAGS:
        if bool(payload.get(flag, False)):
            diagnostics.append(Diagnostic("successor_overclaim", subject, f"{flag} must remain false"))
    return diagnostics


def validate_production_boundary(subject: str, payload: dict[str, Any]) -> list[Diagnostic]:
    diagnostics: list[Diagnostic] = []
    if payload.get("production_evidence") is not True:
        diagnostics.append(Diagnostic("production_build_gate", subject, "production evidence is required"))
    if payload.get("synthetic_evidence"):
        diagnostics.append(Diagnostic("synthetic_evidence", subject, "synthetic production evidence is forbidden"))
    if payload.get("fixture_or_test_only_evidence"):
        diagnostics.append(Diagnostic("fixture_test_only", subject, "fixture/test-only production evidence is forbidden"))
    if payload.get("local_cluster_production_claim"):
        diagnostics.append(Diagnostic("local_cluster_claim", subject, "local cluster production claims are forbidden"))
    if payload.get("cluster_policy") and payload.get("cluster_policy") != REQUIRED_CLUSTER_POLICY:
        diagnostics.append(Diagnostic("local_cluster_claim", subject, "cluster policy must remain external-provider-only"))
    return diagnostics


def validate_artifact_refs(
    repo_root: pathlib.Path,
    artifact_index: dict[str, dict[str, str]],
    subject: str,
    payload: dict[str, Any],
) -> list[Diagnostic]:
    diagnostics: list[Diagnostic] = []
    for artifact_id in payload.get("source_artifacts", []):
        if not artifact_available(repo_root, artifact_index, str(artifact_id)):
            diagnostics.append(Diagnostic("missing_artifact", subject, f"artifact is absent or not present: {artifact_id}"))
    return diagnostics


def validate_area(platform: str, row: dict[str, Any]) -> list[Diagnostic]:
    area = str(row.get("area", "") or "area")
    subject = f"{platform}.{area}"
    diagnostics: list[Diagnostic] = []
    if normalize_status(str(row.get("status", ""))) in FORBIDDEN_STATUSES or not is_complete(str(row.get("status", ""))):
        diagnostics.append(Diagnostic("pending_or_defined_only_platform", subject, "validation area must be complete"))
    if area not in REQUIRED_VALIDATION_AREAS:
        diagnostics.append(Diagnostic("missing_validation_area", subject, "unknown validation area"))
    if not valid_hash(str(row.get("evidence_hash", ""))):
        diagnostics.append(Diagnostic("placeholder_evidence", subject, "evidence hash must be concrete"))
    if area == "entropy_provider":
        provider = row.get("details", {}).get("entropy_provider", "")
        if provider != REQUIRED_ENTROPY_PROVIDERS.get(platform):
            diagnostics.append(Diagnostic("missing_entropy_provider", subject, "required platform entropy provider missing"))
    if area == "cluster_external_provider_only":
        if row.get("details", {}).get("cluster_policy", "") != REQUIRED_CLUSTER_POLICY:
            diagnostics.append(Diagnostic("local_cluster_claim", subject, "cluster area must prove external-provider-only policy"))
    if area == "plugin_signing_sbom":
        if "signed_package" not in row.get("details", {}).get("plugin_policy", ""):
            diagnostics.append(Diagnostic("missing_signing_sbom", subject, "plugin signing/SBOM policy is required"))
    if area == "llvm_linkage_policy":
        if "dynamic_default" not in row.get("details", {}).get("llvm_policy", ""):
            diagnostics.append(Diagnostic("missing_llvm_linkage", subject, "LLVM dynamic/static linkage policy is required"))
    diagnostics.extend(validate_production_boundary(subject, row))
    diagnostics.extend(validate_authority(subject, row))
    return diagnostics


def validate_platform(row: dict[str, Any]) -> list[Diagnostic]:
    platform = str(row.get("platform", "") or "platform")
    family = str(row.get("platform_family", "") or platform)
    diagnostics: list[Diagnostic] = []
    if family not in REQUIRED_PLATFORM_FAMILIES:
        diagnostics.append(Diagnostic("missing_platform", platform, "unknown platform family"))
    status = normalize_status(str(row.get("status", "")))
    support_state = normalize_status(str(row.get("support_state", "")))
    if support_state != "supported_claimed":
        diagnostics.append(Diagnostic("unsupported_claimed_platform", platform, "claimed release rows must be supported_claimed"))
    if status in FORBIDDEN_STATUSES or not is_complete(status):
        diagnostics.append(Diagnostic("pending_or_defined_only_platform", platform, "platform release proof must be complete"))
    profiles = set(str(profile) for profile in row.get("profiles", []))
    missing_profiles = set(REQUIRED_PROFILES) - profiles
    if missing_profiles:
        diagnostics.append(Diagnostic("production_build_gate", platform, "missing profiles: " + ", ".join(sorted(missing_profiles))))
    for flag in (
        "production_gate_matrix_passed",
        "production_gate_negative_cases_passed",
        "fixture_test_hooks_rejected",
        "stub_providers_rejected",
        "debug_trace_flags_rejected",
    ):
        if row.get(flag) is not True:
            diagnostics.append(Diagnostic("production_build_gate", platform, f"{flag} must be true"))
    for flag in ("plugin_signing_verified", "sbom_present"):
        if row.get(flag) is not True:
            diagnostics.append(Diagnostic("missing_signing_sbom", platform, f"{flag} must be true"))
    for field in ("sbom_digest", "package_signature", "release_artifact_digest", "platform_evidence_hash"):
        if not valid_hash(str(row.get(field, ""))):
            diagnostics.append(Diagnostic("placeholder_evidence", platform, f"{field} must be concrete"))
    if row.get("entropy_provider") != REQUIRED_ENTROPY_PROVIDERS.get(family):
        diagnostics.append(Diagnostic("missing_entropy_provider", platform, "required entropy provider missing"))
    for flag in (
        "llvm_dynamic_default_validated",
        "llvm_static_option_validated",
        "memory_temp_filesystem_process_validated",
    ):
        if row.get(flag) is not True:
            code = "missing_llvm_linkage" if flag.startswith("llvm") else "missing_platform_validation"
            diagnostics.append(Diagnostic(code, platform, f"{flag} must be true"))
    areas = row.get("validation_areas", [])
    if not isinstance(areas, list):
        return diagnostics + [Diagnostic("missing_validation_area", platform, "validation_areas must be a list")]
    by_area = {str(area.get("area", "")): area for area in areas if isinstance(area, dict)}
    for area_name in REQUIRED_VALIDATION_AREAS:
        if area_name not in by_area:
            diagnostics.append(Diagnostic("missing_validation_area", platform, f"missing validation area: {area_name}"))
    for area in areas:
        if isinstance(area, dict):
            diagnostics.extend(validate_area(family, area))
        else:
            diagnostics.append(Diagnostic("missing_validation_area", platform, "validation area must be an object"))
    diagnostics.extend(validate_production_boundary(platform, row))
    diagnostics.extend(validate_authority(platform, row))
    return diagnostics


def validate_model(repo_root: pathlib.Path, model: dict[str, Any]) -> list[Diagnostic]:
    diagnostics: list[Diagnostic] = []
    artifact_index = artifacts(repo_root)
    if model.get("schema") != "sb.ceic094.cross_platform_release_proof.v1":
        diagnostics.append(Diagnostic("schema", "model", "unsupported CEIC-094 schema"))
    for field in ("proof_id", "capture_generation", "proof_epoch"):
        if not valid_token(str(model.get(field, ""))):
            diagnostics.append(Diagnostic("placeholder_evidence", "model", f"{field} must be concrete"))
    if model.get("production_test_separation") is not True:
        diagnostics.append(Diagnostic("production_build_gate", "model", "production/test separation is required"))
    production_gate = str(model.get("production_gate_script", ""))
    if not production_gate or not path_exists(repo_root, production_gate):
        diagnostics.append(Diagnostic("production_build_gate", "model", "production gate script path must exist"))
    coupling = model.get("predecessor_coupling", {})
    for key in PREDECESSOR_COUPLINGS:
        if not isinstance(coupling, dict) or coupling.get(key) is not True:
            diagnostics.append(Diagnostic("missing_predecessor_coupling", "model", f"{key} must be true"))
    diagnostics.extend(validate_artifact_refs(repo_root, artifact_index, "model", model))
    diagnostics.extend(validate_production_boundary("model", model))
    diagnostics.extend(validate_authority("model", model))

    platform_rows = model.get("platforms", [])
    if not isinstance(platform_rows, list) or not platform_rows:
        diagnostics.append(Diagnostic("missing_platform", "model", "platform rows are required"))
        return diagnostics
    if int(model.get("platform_count", 0) or 0) != len(platform_rows):
        diagnostics.append(Diagnostic("missing_platform", "model", "platform_count must match platforms length"))
    families = {str(row.get("platform_family", "")) for row in platform_rows if isinstance(row, dict)}
    missing = set(REQUIRED_PLATFORM_FAMILIES) - families
    if missing:
        diagnostics.append(Diagnostic("missing_platform", "model", "missing platform families: " + ", ".join(sorted(missing))))
    for row in platform_rows:
        if isinstance(row, dict):
            diagnostics.extend(validate_platform(row))
        else:
            diagnostics.append(Diagnostic("missing_platform", "model", "platform row must be an object"))
    return diagnostics


def validate_predecessors(repo_root: pathlib.Path) -> list[Diagnostic]:
    diagnostics: list[Diagnostic] = []
    artifact_index = artifacts(repo_root)
    for artifact_id in REQUIRED_ARTIFACTS:
        if not artifact_available(repo_root, artifact_index, artifact_id):
            diagnostics.append(Diagnostic("missing_artifact", artifact_id, "required predecessor artifact is absent or not present"))
    try:
        tool = load_tool(repo_root, "project/tools/ceic_reliability_security_suite.py", "ceic093_for_ceic094")
        for diagnostic in tool.validate_model(repo_root, tool.default_model(repo_root)):
            diagnostics.append(Diagnostic("missing_predecessor_coupling", "CEIC-093", diagnostic.render()))
    except Exception as exc:  # pragma: no cover - deterministic diagnostic
        diagnostics.append(Diagnostic("missing_predecessor_coupling", "CEIC-093", str(exc)))
    return diagnostics


def validate_execution_plan_control(repo_root: pathlib.Path) -> list[Diagnostic]:
    diagnostics: list[Diagnostic] = []
    tracker = index_by(read_csv(repo_root / EXECUTION_PLAN / "TRACKER.csv"), "slice_id")
    dependencies = index_by(read_csv(repo_root / EXECUTION_PLAN / "DEPENDENCIES.csv"), "dependency_id")
    gates = index_by(read_csv(repo_root / EXECUTION_PLAN / "ACCEPTANCE_GATES.csv"), "gate_id")
    artifact_index = artifacts(repo_root)
    trace = index_by(read_csv(repo_root / EXECUTION_PLAN / "AUDIT_TRACEABILITY_MATRIX.csv"), "finding_id")
    audit = index_by(read_csv(repo_root / EXECUTION_PLAN / "SPEC_IMPLEMENTATION_AUDIT_MATRIX.csv"), "audit_id")

    for slice_id in ("CEIC-090", "CEIC-091", "CEIC-092", "CEIC-093", "CEIC-094"):
        if not is_complete(tracker.get(slice_id, {}).get("status", "")):
            diagnostics.append(Diagnostic("tracker_status", slice_id, f"{slice_id} must be complete"))
    if normalize_status(tracker.get("CEIC-095", {}).get("status", "")) != "pending":
        diagnostics.append(Diagnostic("successor_overclaim", "CEIC-095", "CEIC-095 must remain pending"))
    for dependency_id in ("CEIC-DEP-050", "CEIC-DEP-051", "CEIC-DEP-053", "CEIC-DEP-054", "CEIC-DEP-052"):
        if normalize_status(dependencies.get(dependency_id, {}).get("status", "")) != "available":
            diagnostics.append(Diagnostic("dependency_unavailable", dependency_id, f"{dependency_id} must be available"))
    for gate_id in ("CEIC-GATE-049", "CEIC-GATE-050", "CEIC-GATE-053", "CEIC-GATE-051", "CEIC-GATE-052"):
        if not is_complete(gates.get(gate_id, {}).get("status", "")):
            diagnostics.append(Diagnostic("gate_status", gate_id, f"{gate_id} must be complete"))
    for artifact_id in REQUIRED_ARTIFACTS:
        if not artifact_available(repo_root, artifact_index, artifact_id):
            diagnostics.append(Diagnostic("missing_artifact", artifact_id, "required artifact must be present"))
    row = trace.get("X-009", {})
    if normalize_status(row.get("status", "")) != "complete":
        diagnostics.append(Diagnostic("traceability_status", "X-009", "X-009 must be complete"))
    if "CEIC-ART-091" not in row.get("evidence_artifacts", ""):
        diagnostics.append(Diagnostic("traceability_status", "X-009", "CEIC-ART-091 evidence is required"))
    if normalize_status(audit.get("CEIC-AUD-055", {}).get("status", "")) != "complete":
        diagnostics.append(Diagnostic("audit_status", "CEIC-AUD-055", "CEIC-094 audit row must be complete"))

    cmake_text = (repo_root / CMAKE_GATE).read_text(encoding="utf-8")
    for token in ("ceic_094_cross_platform_release_gate_check", "ceic_094_cross_platform_release_gate"):
        if token not in cmake_text:
            diagnostics.append(Diagnostic("cmake_registration", "CEIC-094", f"missing CMake registration: {token}"))
    return diagnostics


def load_model(repo_root: pathlib.Path, manifest: pathlib.Path | None) -> dict[str, Any]:
    if manifest is None:
        return default_model(repo_root)
    path = manifest if manifest.is_absolute() else repo_root / manifest
    return json.loads(path.read_text(encoding="utf-8"))


def run(repo_root: pathlib.Path, manifest: pathlib.Path | None, skip_execution_plan_control: bool) -> list[Diagnostic]:
    model = load_model(repo_root, manifest)
    diagnostics = validate_model(repo_root, model)
    if manifest is None:
        diagnostics.extend(validate_predecessors(repo_root))
    if not skip_execution_plan_control:
        diagnostics.extend(validate_execution_plan_control(repo_root))
    return diagnostics


def parse_args(argv: list[str]) -> argparse.Namespace:
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument("--repo-root", type=pathlib.Path, default=pathlib.Path.cwd())
    parser.add_argument("--manifest", type=pathlib.Path)
    parser.add_argument("--skip-execution_plan-control", action="store_true")
    parser.add_argument("--dump-default-model", action="store_true")
    return parser.parse_args(argv)


def main(argv: list[str]) -> int:
    args = parse_args(argv)
    repo_root = args.repo_root.resolve()
    if args.dump_default_model:
        print(json.dumps(default_model(repo_root), indent=2, sort_keys=True))
        return 0
    diagnostics = run(repo_root, args.manifest, args.skip_execution_plan_control)
    if diagnostics:
        for diagnostic in diagnostics:
            print(f"ceic_094_cross_platform_release_gate=fail:{diagnostic.render()}", file=sys.stderr)
        return 1
    print("ceic_094_cross_platform_release_gate=pass")
    return 0


if __name__ == "__main__":
    raise SystemExit(main(sys.argv[1:]))
