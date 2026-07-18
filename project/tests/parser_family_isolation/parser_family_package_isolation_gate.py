#!/usr/bin/env python3
# Copyright (c) 2026 ScratchBird Software Inc.
#
# This Source Code Form is subject to the terms of the Mozilla Public
# License, v. 2.0. If a copy of the MPL was not distributed with this
# file, You can obtain one at https://mozilla.org/MPL/2.0/.
#
# SPDX-License-Identifier: MPL-2.0

"""Empty-prefix install and operational package gate for parser families."""

from __future__ import annotations

import argparse
import datetime as dt
import json
import os
import pathlib
import shutil
import subprocess
import sys

sys.path.insert(0, str(pathlib.Path(__file__).resolve().parent))
import parser_family_binary_isolation_gate as identity  # noqa: E402


PACKAGE_DESCRIPTOR = "share/scratchbird/parsers/{family}/standalone-package.json"


def component_name(family: str) -> str:
    return f"parser_{family}_standalone"


def package_files(stage: pathlib.Path) -> list[pathlib.Path]:
    return sorted(path for path in stage.rglob("*") if path.is_file())


def relative_file_hashes(stage: pathlib.Path) -> dict[str, str]:
    return {
        path.relative_to(stage).as_posix(): identity.sha256_file(path)
        for path in package_files(stage)
    }


def safe_artifact_path(artifact: str) -> pathlib.PurePosixPath | None:
    path = pathlib.PurePosixPath(artifact)
    if path.is_absolute() or ".." in path.parts or not path.parts:
        return None
    return path


def resolve_declared_artifact(
    stage: pathlib.Path, target: str, artifact: str
) -> list[pathlib.Path]:
    if artifact.startswith("system/"):
        return []
    relative = safe_artifact_path(artifact)
    if relative is None:
        return []
    base = stage.joinpath(*relative.parts)
    candidates = [base]
    candidates.extend(base.parent.glob(base.name + ".*"))
    # Windows import/static libraries do not use the Unix `lib` prefix.
    if base.name.startswith("lib"):
        windows_base = base.with_name(base.name[len("lib"):])
        candidates.extend(windows_base.parent.glob(windows_base.name + ".*"))
    if target.startswith("sbp_"):
        candidates.append(base.with_name(base.name + ".exe"))
    return sorted({path.resolve() for path in candidates if path.is_file()})


def manifest_dependency_entries(manifest: dict[str, object]) -> list[dict[str, str]]:
    entries: list[dict[str, str]] = []
    for field in ("same_family_library_set", "neutral_dependency_set"):
        raw_entries = manifest.get(field)
        if not isinstance(raw_entries, list):
            continue
        for raw in raw_entries:
            if not isinstance(raw, dict):
                continue
            target = raw.get("target")
            artifact = raw.get("artifact")
            owner = raw.get("owner")
            if isinstance(target, str) and isinstance(artifact, str) and isinstance(owner, str):
                entries.append({"target": target, "artifact": artifact, "owner": owner})
    return entries


def validate_installed_closure(
    stage: pathlib.Path,
    family: str,
    manifest: dict[str, object],
    descriptor: dict[str, object],
) -> tuple[list[str], set[str]]:
    gaps: list[str] = []
    declared_files: set[str] = set()
    entries = manifest_dependency_entries(manifest)
    for entry in entries:
        artifact = entry["artifact"]
        matches = resolve_declared_artifact(stage, entry["target"], artifact)
        if artifact.startswith("system/"):
            continue
        if not matches:
            gaps.append(f"missing_declared_artifact:{entry['target']}:{artifact}")
            continue
        for match in matches:
            declared_files.add(match.relative_to(stage.resolve()).as_posix())

    expected_same = {
        entry["target"]
        for entry in entries
        if entry["owner"] == manifest.get("parser_family_uuid")
    }
    expected_neutral = {
        entry["target"]
        for entry in entries
        if entry["owner"] != manifest.get("parser_family_uuid")
        and not entry["artifact"].startswith("system/")
    }
    descriptor_same = descriptor.get("same_family_targets")
    descriptor_neutral = descriptor.get("neutral_targets")
    if not isinstance(descriptor_same, list) or set(descriptor_same) != expected_same:
        gaps.append("package_descriptor.same_family_targets=manifest_exact")
    if not isinstance(descriptor_neutral, list) or set(descriptor_neutral) != expected_neutral:
        gaps.append("package_descriptor.neutral_targets=manifest_exact")
    if descriptor.get("parser_family") != family:
        gaps.append("package_descriptor.parser_family=owner")
    if descriptor.get("install_component") != component_name(family):
        gaps.append("package_descriptor.install_component=family_component")

    descriptor_relative = PACKAGE_DESCRIPTOR.format(family=family)
    descriptor_path = stage / descriptor_relative
    if descriptor_path.is_file():
        declared_files.add(descriptor_relative)
    else:
        gaps.append("missing_package_descriptor")

    installed_files = {
        path.relative_to(stage).as_posix()
        for path in package_files(stage)
    }
    undeclared = sorted(installed_files - declared_files)
    if undeclared:
        gaps.append("undeclared_installed_artifacts:" + ",".join(undeclared))
    return sorted(set(gaps)), declared_files


def load_descriptor(stage: pathlib.Path, family: str) -> dict[str, object]:
    path = stage / PACKAGE_DESCRIPTOR.format(family=family)
    payload = identity.strict_json_loads(path.read_text(encoding="utf-8"))
    if not isinstance(payload, dict):
        raise ValueError(f"package descriptor root is not an object for {family}")
    return payload


def installed_worker(stage: pathlib.Path, family: str) -> pathlib.Path:
    candidates = [
        stage / "bin" / name
        for name in identity.executable_names(family)
        if (stage / "bin" / name).is_file()
        and os.access(stage / "bin" / name, os.X_OK)
    ]
    return identity.select_unique(candidates, f"installed {family} parser executable")


def operational_output_gaps(family: str, output: str) -> list[str]:
    try:
        payload = identity.strict_json_loads(output.strip())
    except (json.JSONDecodeError, identity.DuplicateJsonKeyError):
        return ["operational_output=strict_json"]
    if not isinstance(payload, dict):
        return ["operational_output=object"]
    gaps: list[str] = []
    if payload.get("dialect") != family:
        gaps.append("operational_output.dialect=owner")
    if payload.get("envelope") != "SBLRExecutionEnvelope.v3":
        gaps.append("operational_output.envelope=SBLRExecutionEnvelope.v3")
    if not isinstance(payload.get("operation_family"), str) or not payload["operation_family"]:
        gaps.append("operational_output.operation_family=nonempty")
    return gaps


def validate_family(
    registry: identity.Registry,
    family: str,
    build_dir: pathlib.Path,
    evidence_dir: pathlib.Path,
    config: str,
) -> tuple[dict[str, object], list[identity.Finding]]:
    stage = evidence_dir / "prefixes" / family
    reports = evidence_dir / "reports"
    reports.mkdir(parents=True, exist_ok=True)
    if stage.exists():
        shutil.rmtree(stage)
    if stage.exists():
        raise ValueError(f"failed to empty install prefix for {family}")

    install_args = [
        "cmake", "--install", str(build_dir), "--prefix", str(stage),
        "--component", component_name(family),
    ]
    if config:
        install_args.extend(("--config", config))
    install = identity.command(install_args, timeout=300)
    install_log = reports / f"{family}.install.txt"
    install_log.write_text(install.stdout, encoding="utf-8")
    if install.returncode != 0:
        raise ValueError(f"cmake --install failed for {family}: {install.stdout[:300]}")
    if not stage.is_dir():
        raise ValueError(f"cmake --install did not create prefix for {family}")

    worker = installed_worker(stage, family)
    runtime_env = {
        "HOME": str(stage),
        "LANG": "C",
        "LC_ALL": "C",
        "PATH": "/usr/bin:/bin",
        "LD_LIBRARY_PATH": str(stage / "lib"),
    }
    package_identity = identity.command(
        [str(worker), "--package-identity"], timeout=60, cwd=stage, env=runtime_env
    )
    if package_identity.returncode != 0:
        raise ValueError(f"installed identity probe failed for {family}")
    manifest, manifest_gaps = identity.parse_package_identity(
        family, package_identity.stdout
    )
    if manifest is None:
        raise ValueError(f"installed package identity is invalid for {family}")

    descriptor = load_descriptor(stage, family)
    closure_gaps, declared_files = validate_installed_closure(
        stage, family, manifest, descriptor
    )
    operational = identity.command(
        [str(worker)], timeout=60, cwd=stage, env=runtime_env
    )
    if operational.returncode != 0:
        raise ValueError(
            f"installed operational parser invocation failed for {family}: "
            f"{operational.stdout[:300]}"
        )
    operational_gaps = operational_output_gaps(family, operational.stdout)
    ldd = identity.command(["ldd", str(worker)], timeout=60)
    if ldd.returncode != 0:
        raise ValueError(f"installed dynamic dependency inspection failed for {family}")

    listing = "\n".join(relative_file_hashes(stage))
    findings: list[identity.Finding] = []
    findings.extend(identity.scan_text(registry, family, "installed_package", listing))
    findings.extend(identity.scan_text(registry, family, "installed_manifest", package_identity.stdout))
    findings.extend(identity.scan_text(registry, family, "installed_descriptor", json.dumps(descriptor, sort_keys=True)))
    findings.extend(identity.scan_text(registry, family, "installed_dynamic_dependencies", ldd.stdout))
    findings.extend(identity.scan_text(registry, family, "installed_operational_output", operational.stdout))

    gaps = sorted(set(manifest_gaps + closure_gaps + operational_gaps))
    identity_report = reports / f"{family}.identity.json"
    identity_report.write_text(package_identity.stdout, encoding="utf-8")
    operational_report = reports / f"{family}.operational.json"
    operational_report.write_text(operational.stdout, encoding="utf-8")
    dynamic_report = reports / f"{family}.dynamic.txt"
    dynamic_report.write_text(ldd.stdout, encoding="utf-8")
    row: dict[str, object] = {
        "family": family,
        "parser_family_uuid": manifest.get("parser_family_uuid"),
        "parser_package_uuid": f"package.parser.{family}",
        "package_version": "same-build",
        "build_profile": manifest.get("isolated_build_profile"),
        "package_profile": manifest.get("isolated_package_profile"),
        "install_component": component_name(family),
        "empty_prefix_before_install": True,
        "cmake_install_exit_code": install.returncode,
        "installed_worker": str(worker),
        "installed_file_count": len(package_files(stage)),
        "installed_file_sha256": relative_file_hashes(stage),
        "declared_installed_files": sorted(declared_files),
        "package_manifest_hash": identity.sha256_text(package_identity.stdout),
        "operational_output_hash": identity.sha256_text(operational.stdout),
        "dynamic_dependency_hash": identity.sha256_text(ldd.stdout),
        "package_gaps": gaps,
        "foreign_dependency_count": len(findings),
        "passed_gate_ids": ["PARSER-ISO-006"] if not gaps and not findings else [],
        "failed_gate_ids": [] if not gaps and not findings else ["PARSER-ISO-006"],
        "diagnostic_codes": gaps,
        "evidence_timestamp": dt.datetime.now(dt.timezone.utc).isoformat(),
    }
    return row, findings


def main() -> int:
    parser = argparse.ArgumentParser()
    parser.add_argument("--repo-root", required=True, type=pathlib.Path)
    parser.add_argument("--build-dir", required=True, type=pathlib.Path)
    parser.add_argument("--ownership-map", required=True, type=pathlib.Path)
    parser.add_argument("--evidence-dir", required=True, type=pathlib.Path)
    parser.add_argument("--config", default="")
    parser.add_argument("--allow-no-artifacts", action="store_true")
    parser.add_argument("--require-complete-registry", action="store_true")
    args = parser.parse_args()

    repo_root = args.repo_root.resolve()
    build_dir = args.build_dir.resolve()
    ownership = args.ownership_map
    if not ownership.is_absolute():
        ownership = repo_root / ownership
    evidence_dir = args.evidence_dir.resolve()
    evidence_dir.mkdir(parents=True, exist_ok=True)
    evidence_file = evidence_dir / "parser_family_package_isolation_evidence.json"

    try:
        ownership_payload = identity.strict_json_loads(
            ownership.read_text(encoding="utf-8")
        )
        if not isinstance(ownership_payload, dict):
            raise ValueError("ownership map root must be an object")
        registry = identity.Registry(ownership_payload)
        artifacts = identity.discover_artifacts(build_dir, registry)
        families = sorted(family for family in artifacts if family != "sbsql")
        expected = sorted(family for family in registry.families if family != "sbsql")
        if not families and args.allow_no_artifacts:
            print("parser_family_package_isolation_gate=skipped no compatibility parser artifacts")
            return 77
        if args.require_complete_registry and families != expected:
            missing = sorted(set(expected) - set(families))
            extra = sorted(set(families) - set(expected))
            raise ValueError(
                f"compatibility parser artifact registry mismatch missing={missing} extra={extra}"
            )

        rows: list[dict[str, object]] = []
        findings: list[identity.Finding] = []
        errors: list[str] = []
        for family in families:
            try:
                row, family_findings = validate_family(
                    registry, family, build_dir, evidence_dir, args.config
                )
                rows.append(row)
                findings.extend(family_findings)
            except (
                OSError,
                ValueError,
                subprocess.TimeoutExpired,
                json.JSONDecodeError,
                identity.DuplicateJsonKeyError,
            ) as exc:
                errors.append(f"{family}: {exc}")

        gap_count = sum(len(row["package_gaps"]) for row in rows)
        passed = (
            not errors
            and not findings
            and gap_count == 0
            and rows
            and (not args.require_complete_registry or len(rows) == len(expected))
        )
        payload = {
            "gate": "parser_family_package_isolation_gate",
            "scope": "PARSER-ISO-006 empty-prefix install, package closure, and direct operational parser invocation",
            "scope_limitations": [
                "does not prove listener attach or original client-tool execution",
                "does not claim PARSER-ISO-007 or PARSER-ISO-008 runtime isolation",
            ],
            "build_dir": str(build_dir),
            "family_count": len(rows),
            "expected_family_count": len(expected),
            "families": rows,
            "package_gap_count": gap_count,
            "foreign_dependency_count": len(findings),
            "findings": [finding.to_json() for finding in findings],
            "errors": errors,
            "conformance": {
                "PARSER-ISO-006": "passed" if passed else "failed",
                "PARSER-ISO-007": "not_evaluated_listener_attach_and_tool_session_required",
                "PARSER-ISO-008": "not_evaluated_full_runtime_trace_required",
            },
            "package_gate_status": "passed" if passed else "failed",
            "evidence_timestamp": dt.datetime.now(dt.timezone.utc).isoformat(),
        }
        evidence_file.write_text(
            json.dumps(payload, indent=2, sort_keys=True) + "\n", encoding="utf-8"
        )
        print(
            f"parser_family_package_isolation_gate={payload['package_gate_status']} "
            f"families={len(rows)} package_gaps={gap_count} "
            f"foreign_dependencies={len(findings)} errors={len(errors)}"
        )
        if not passed:
            print(json.dumps(payload, indent=2, sort_keys=True), file=sys.stderr)
            return 1
        return 0
    except (
        OSError,
        ValueError,
        json.JSONDecodeError,
        identity.DuplicateJsonKeyError,
    ) as exc:
        print(f"parser_family_package_isolation_gate: {exc}", file=sys.stderr)
        return 2


if __name__ == "__main__":
    raise SystemExit(main())
