#!/usr/bin/env python3
# Copyright (c) 2026 ScratchBird Software Inc.
#
# This Source Code Form is subject to the terms of the Mozilla Public
# License, v. 2.0. If a copy of the MPL was not distributed with this
# file, You can obtain one at https://mozilla.org/MPL/2.0/.
#
# SPDX-License-Identifier: MPL-2.0

"""Validate and aggregate a complete sharded Firebird QA replay."""

from __future__ import annotations

import argparse
import glob
import hashlib
import json
import re
from pathlib import Path
from typing import Any

from reference_firebird_qa_replay_gate import (
    REQUIRED_FIREBIRD_TOOLS,
    load_canonical_scope,
)


NODE_OUTCOME_KEYS = (
    "collected",
    "passed",
    "failed",
    "errors",
    "skipped",
    "expected_version_or_platform_deselected",
    "expected_upstream_static_skipped",
    "unexpected_skipped",
    "xfailed",
    "xpassed",
)


def integer(value: Any, label: str, errors: list[str]) -> int:
    if isinstance(value, bool) or not isinstance(value, int) or value < 0:
        errors.append(f"invalid non-negative integer {label}: {value!r}")
        return 0
    return value


def valid_executable_identity(identity: Any) -> bool:
    return (
        isinstance(identity, dict)
        and identity.get("is_file") is True
        and identity.get("executable") is True
        and bool(identity.get("resolved_path"))
        and isinstance(identity.get("size_bytes"), int)
        and identity["size_bytes"] > 0
        and re.fullmatch(r"[0-9a-f]{64}", str(identity.get("sha256", "")))
        is not None
    )


def validate_tool_identities(
    identities: Any, label: str, errors: list[str]
) -> None:
    if not isinstance(identities, dict) or identities.get("identity_complete") is not True:
        errors.append(f"{label} lacks complete tool identity evidence")
        return
    scratchbird = identities.get("scratchbird", {})
    if not isinstance(scratchbird, dict) or set(scratchbird) != {
        "server", "listener", "parser_worker"
    }:
        errors.append(f"{label} has incomplete ScratchBird executable identities")
    elif not all(valid_executable_identity(item) for item in scratchbird.values()):
        errors.append(f"{label} has invalid ScratchBird executable identity")
    firebird_tools = identities.get("firebird_tools", {})
    if not isinstance(firebird_tools, dict) or set(firebird_tools) != set(
        REQUIRED_FIREBIRD_TOOLS
    ):
        errors.append(f"{label} has incomplete genuine Firebird tool identities")
    elif not all(valid_executable_identity(item) for item in firebird_tools.values()):
        errors.append(f"{label} has invalid genuine Firebird tool identity")
    adapter = identities.get("firebird_tool_adapter", {})
    if adapter.get("fabricated_tool_output") is not False:
        errors.append(f"{label} does not prove non-fabricating Firebird tool adapters")


def validate_package_identities(
    identities: Any, label: str, errors: list[str]
) -> None:
    if not isinstance(identities, dict):
        errors.append(f"missing package identity evidence: {label}")
        return
    for package in ("pytest", "firebird_qa", "firebird_driver"):
        identity = identities.get(package)
        if not (
            isinstance(identity, dict)
            and identity.get("version") not in {None, "", "unknown"}
            and bool(identity.get("origin"))
            and re.fullmatch(
                r"[0-9a-f]{64}", str(identity.get("origin_sha256", ""))
            ) is not None
        ):
            errors.append(f"invalid {package} package identity: {label}")
    if not valid_executable_identity(identities.get("python")):
        errors.append(f"invalid pytest Python executable identity: {label}")


def canonical_case_id(relative_path: str) -> str:
    return "firebird.qa." + relative_path.replace("/", ".").removesuffix(".py")


def aggregate_evidence(
    paths: list[Path], canonical_scope: dict[str, Any] | None = None
) -> dict[str, Any]:
    if canonical_scope is None:
        canonical_scope = load_canonical_scope()
    canonical_entries = canonical_scope["entries"]
    canonical_count = int(canonical_scope["case_file_count"])
    canonical_hash = str(canonical_scope["manifest_sha256"])
    canonical_shard_count = int(canonical_scope["shard_count"])
    ordered_paths = sorted({path.resolve() for path in paths})
    errors: list[str] = []
    payloads: list[tuple[Path, dict[str, Any]]] = []
    evidence_hash = hashlib.sha256()
    for path in ordered_paths:
        if not path.is_file():
            errors.append(f"missing evidence file: {path}")
            continue
        content = path.read_bytes()
        evidence_hash.update(path.name.encode("utf-8"))
        evidence_hash.update(b"\0")
        evidence_hash.update(hashlib.sha256(content).digest())
        try:
            payload = json.loads(content)
        except json.JSONDecodeError as exc:
            errors.append(f"invalid JSON evidence {path}: {exc}")
            continue
        if not isinstance(payload, dict):
            errors.append(f"evidence root is not an object: {path}")
            continue
        payloads.append((path, payload))

    if canonical_shard_count != 12:
        errors.append(
            f"canonical Firebird QA shard count must be 12, got {canonical_shard_count}"
        )
    shard_count = canonical_shard_count
    if len(payloads) != canonical_shard_count:
        errors.append(
            "canonical aggregate requires exactly 12 evidence files: "
            f"actual={len(payloads)}"
        )

    by_index: dict[int, tuple[Path, dict[str, Any]]] = {}
    for path, payload in payloads:
        label = str(path)
        if payload.get("schema_version") != "scratchbird_firebird_qa_replay_gate_v1":
            errors.append(f"unexpected replay schema: {label}")
        if payload.get("gate") != "reference_firebird_qa_replay_gate":
            errors.append(f"unexpected replay gate identity: {label}")
        if payload.get("family") != "firebird":
            errors.append(f"unexpected replay family: {label}")
        if payload.get("scope_kind") != "canonical_full":
            errors.append(f"non-canonical or diagnostic scope evidence: {label}")
        if payload.get("canonical_scope_claimed") is not True:
            errors.append(f"evidence does not claim checked canonical scope: {label}")
        if payload.get("status") != "passed":
            errors.append(f"shard status is not passed: {label}")
        if payload.get("assigned_scope_complete") is not True:
            errors.append(f"assigned shard scope is incomplete: {label}")
        if payload.get("completion_claim") != (
            "canonical_shard_evidence_requires_12_shard_aggregate"
        ):
            errors.append(f"unexpected shard completion claim: {label}")
        shard = payload.get("shard", {})
        if not isinstance(shard, dict):
            errors.append(f"missing shard object: {label}")
            continue
        if shard.get("count") != canonical_shard_count:
            errors.append(f"non-canonical shard count: {label}")
        if shard.get("assignment") != "zero_based_manifest_ordinal_modulo_shard_count":
            errors.append(f"unexpected shard assignment policy: {label}")
        raw_index = shard.get("index")
        if isinstance(raw_index, bool) or not isinstance(raw_index, int):
            errors.append(f"missing shard index: {path}")
            continue
        if raw_index in by_index:
            errors.append(f"duplicate shard index {raw_index}: {path} and {by_index[raw_index][0]}")
            continue
        by_index[raw_index] = (path, payload)
    expected_indices = set(range(shard_count))
    if set(by_index) != expected_indices:
        errors.append(
            "shard index coverage mismatch: "
            f"expected={sorted(expected_indices)} actual={sorted(by_index)}"
        )

    full_count = canonical_count
    manifest_hash = canonical_hash
    expected_entries_by_shard = {
        index: [
            entry
            for entry in canonical_entries
            if int(entry["ordinal"]) % canonical_shard_count == index
        ]
        for index in range(canonical_shard_count)
    }

    case_ids: set[str] = set()
    duplicate_case_ids: set[str] = set()
    aggregate_expected_exclusions: list[dict[str, Any]] = []
    aggregate_expected_upstream_static_skips: list[dict[str, Any]] = []
    tool_snapshots: set[str] = set()
    package_snapshots: set[str] = set()
    for path, payload in payloads:
        label = str(path)
        shard = payload.get("shard", {})
        shard_index = shard.get("index") if isinstance(shard, dict) else None
        full_manifest = payload.get("full_discovery_manifest", {})
        if payload.get("full_discovered_case_count") != canonical_count:
            errors.append(f"canonical case count mismatch: {label}")
        if (
            not isinstance(full_manifest, dict)
            or full_manifest.get("case_file_count") != canonical_count
            or full_manifest.get("manifest_sha256") != canonical_hash
        ):
            errors.append(f"canonical discovery manifest mismatch: {label}")
        reported_scope = payload.get("canonical_scope", {})
        if (
            not isinstance(reported_scope, dict)
            or reported_scope.get("case_file_count") != canonical_count
            or reported_scope.get("manifest_sha256") != canonical_hash
            or reported_scope.get("shard_count") != canonical_shard_count
        ):
            errors.append(f"checked canonical scope metadata mismatch: {label}")
        controls = payload.get("pytest_scope_controls", {})
        if not (
            isinstance(controls, dict)
            and controls.get("plugin_autoload_disabled") is True
            and controls.get("command_addopts_cleared") is True
            and controls.get("exact_case_file_per_invocation") is True
        ):
            errors.append(f"pytest scope controls are incomplete: {label}")
        identities = payload.get("tool_identities")
        validate_tool_identities(identities, label, errors)
        if isinstance(identities, dict):
            tool_snapshots.add(json.dumps(identities, sort_keys=True))
        package_identities = payload.get("package_identities")
        validate_package_identities(package_identities, label, errors)
        if isinstance(package_identities, dict):
            package_snapshots.add(json.dumps(package_identities, sort_keys=True))

        cases = payload.get("case_results")
        if not isinstance(cases, list):
            errors.append(f"case_results is not a list: {label}")
            cases = []
        if isinstance(shard_index, int) and shard_index in expected_entries_by_shard:
            expected_entries = expected_entries_by_shard[shard_index]
        else:
            expected_entries = []
        expected_paths = [entry["path"] for entry in expected_entries]
        actual_paths = [case.get("relative_path") for case in cases if isinstance(case, dict)]
        if actual_paths != expected_paths:
            errors.append(f"manifest ordinal shard membership mismatch: {label}")
        if payload.get("discovered_case_count") != len(expected_entries):
            errors.append(f"shard discovered case count mismatch: {label}")
        if len(cases) != len(expected_entries):
            errors.append(f"shard case result count mismatch: {label}")

        local_nodes = {key: 0 for key in NODE_OUTCOME_KEYS}
        local_expected_exclusions: list[dict[str, Any]] = []
        local_expected_upstream_static_skips: list[dict[str, Any]] = []
        local_deselected = 0
        local_passed_cases = 0
        local_startup_failure_events = 0
        local_startup_recovered_cases = 0
        for position, case in enumerate(cases):
            if not isinstance(case, dict):
                errors.append(f"non-object case result: {label}")
                continue
            case_id = str(case.get("case_id", ""))
            if not case_id:
                errors.append(f"case result missing case_id: {label}")
            elif case_id in case_ids:
                duplicate_case_ids.add(case_id)
            else:
                case_ids.add(case_id)
            entry = expected_entries[position] if position < len(expected_entries) else None
            if entry is not None:
                if case_id != canonical_case_id(str(entry["path"])):
                    errors.append(f"case_id does not match canonical path: {case_id}")
                if case.get("manifest_ordinal") != entry["ordinal"]:
                    errors.append(f"case manifest ordinal mismatch: {case_id}")
                if case.get("input_sha256") != entry["content_sha256"]:
                    errors.append(f"case input digest mismatch: {case_id}")
            if case.get("status") != "passed":
                errors.append(f"case did not pass: {case_id}")
            else:
                local_passed_cases += 1
            if case.get("junit_valid") is not True:
                errors.append(f"case lacks valid JUnit evidence: {case_id}")
            if case.get("harness_startup_failed") is not False:
                errors.append(f"case has an unrecovered harness startup failure: {case_id}")
            startup_attempts = case.get("startup_attempts")
            if not isinstance(startup_attempts, list) or not startup_attempts:
                errors.append(f"case lacks startup attempt evidence: {case_id}")
                startup_attempts = []
            startup_failures = [
                attempt
                for attempt in startup_attempts
                if isinstance(attempt, dict)
                and attempt.get("status") == "startup_failed"
            ]
            for attempt in startup_failures:
                if not (
                    attempt.get("component") in {
                        "scratchbird_server",
                        "scratchbird_listener",
                    }
                    and bool(attempt.get("reason"))
                    and "returncode" in attempt
                    and isinstance(attempt.get("stdout_tail"), str)
                    and isinstance(attempt.get("stderr_tail"), str)
                ):
                    errors.append(
                        f"case has incomplete startup failure diagnostics: {case_id}"
                    )
            if any(not isinstance(attempt, dict) for attempt in startup_attempts):
                errors.append(f"case has malformed startup attempt evidence: {case_id}")
            last_startup_attempt = (
                startup_attempts[-1] if startup_attempts and isinstance(startup_attempts[-1], dict)
                else {}
            )
            if last_startup_attempt.get("status") != "ready":
                errors.append(f"passing case did not finish with ready startup: {case_id}")
            if case.get("startup_retry_count") != max(0, len(startup_attempts) - 1):
                errors.append(f"case startup retry count mismatch: {case_id}")
            local_startup_failure_events += len(startup_failures)
            if startup_failures:
                local_startup_recovered_cases += 1
            deselected = integer(
                case.get("pytest_deselected_count"),
                f"{case_id}.pytest_deselected_count",
                errors,
            )
            local_deselected += deselected
            if deselected:
                errors.append(f"case has pytest deselections: {case_id}")
            nodes = case.get("node_outcomes")
            if not isinstance(nodes, dict):
                errors.append(f"case lacks node outcome evidence: {case_id}")
                nodes = {}
            node_values = {
                key: integer(nodes.get(key), f"{case_id}.node_outcomes.{key}", errors)
                for key in NODE_OUTCOME_KEYS
            }
            for key, value in node_values.items():
                local_nodes[key] += value
            if (
                node_values["passed"]
                + node_values["failed"]
                + node_values["errors"]
                + node_values["skipped"]
                != node_values["collected"]
            ):
                errors.append(f"case node accounting does not decompose collection: {case_id}")
            if (
                node_values["expected_version_or_platform_deselected"]
                + node_values["expected_upstream_static_skipped"]
                + node_values["unexpected_skipped"]
                + node_values["xfailed"]
                != node_values["skipped"]
            ):
                errors.append(f"case skip accounting does not decompose skips: {case_id}")
            if (
                node_values["unexpected_skipped"]
                or node_values["xfailed"]
                or node_values["xpassed"]
            ):
                errors.append(f"case contains unexpected skip/xfail/xpass: {case_id}")
            exclusions = case.get("expected_exclusions")
            if not isinstance(exclusions, list):
                errors.append(f"case exclusion audit is not a list: {case_id}")
                exclusions = []
            if len(exclusions) != node_values["expected_version_or_platform_deselected"]:
                errors.append(f"case exclusion audit count mismatch: {case_id}")
            for exclusion in exclusions:
                if not isinstance(exclusion, dict) or exclusion.get("exclusion_kind") not in {
                    "version",
                    "platform",
                } or re.fullmatch(
                    r"(?:Skipped:\s*)?Not for ([^\s]+)",
                    str(exclusion.get("reason", "")).strip(),
                ) is None:
                    errors.append(f"invalid expected exclusion audit record: {case_id}")
                    continue
                audited = {"case_id": case_id, **exclusion}
                local_expected_exclusions.append(audited)
                aggregate_expected_exclusions.append(audited)
            static_skips = case.get("expected_upstream_static_skips")
            if not isinstance(static_skips, list):
                errors.append(f"case static skip audit is not a list: {case_id}")
                static_skips = []
            if len(static_skips) != node_values["expected_upstream_static_skipped"]:
                errors.append(f"case static skip audit count mismatch: {case_id}")
            for audit in static_skips:
                valid_source_line = (
                    isinstance(audit, dict)
                    and not isinstance(audit.get("source_line"), bool)
                    and isinstance(audit.get("source_line"), int)
                    and audit["source_line"] > 0
                )
                if not (
                    isinstance(audit, dict)
                    and set(audit) == {
                        "case_id",
                        "node_id",
                        "reason",
                        "source_line",
                    }
                    and audit.get("case_id") == case_id
                    and isinstance(audit.get("node_id"), str)
                    and bool(audit["node_id"])
                    and "::" in audit["node_id"]
                    and isinstance(audit.get("reason"), str)
                    and bool(audit["reason"])
                    and valid_source_line
                ):
                    errors.append(f"invalid expected static skip audit record: {case_id}")
                    continue
                local_expected_upstream_static_skips.append(audit)
                aggregate_expected_upstream_static_skips.append(audit)
            if case.get("upstream_static_skip_source_error") != "":
                errors.append(f"case static skip source audit failed: {case_id}")
            for detail_key in ("unexpected_skips", "xfails", "xpasses"):
                if case.get(detail_key) != []:
                    errors.append(f"case has non-empty {detail_key} audit: {case_id}")

        reported_nodes = payload.get("node_outcomes")
        if reported_nodes != local_nodes:
            errors.append(f"shard node outcomes do not equal case sums: {label}")
        if payload.get("pytest_deselected_count") != local_deselected:
            errors.append(f"shard deselection total mismatch: {label}")
        if payload.get("expected_exclusions") != local_expected_exclusions:
            errors.append(f"shard exclusion audit does not equal case records: {label}")
        if payload.get(
            "expected_upstream_static_skips"
        ) != local_expected_upstream_static_skips:
            errors.append(f"shard static skip audit does not equal case records: {label}")
        if payload.get("expected_upstream_static_skipped") != local_nodes[
            "expected_upstream_static_skipped"
        ]:
            errors.append(f"shard static skip count mismatch: {label}")
        if payload.get("passed_case_count") != local_passed_cases:
            errors.append(f"shard passed case count mismatch: {label}")
        if payload.get("failed_case_count") != 0 or payload.get("skipped_case_count") != 0:
            errors.append(f"shard reports failed or skipped source cases: {label}")
        if (
            payload.get("semantic_failed_case_count") != 0
            or payload.get("harness_failed_case_count") != 0
            or payload.get("scope_policy_failed_case_count") != 0
        ):
            errors.append(f"shard failure-class partition is not all zero: {label}")
        if payload.get("startup_failure_event_count") != local_startup_failure_events:
            errors.append(f"shard startup failure event count mismatch: {label}")
        if payload.get("startup_recovered_case_count") != local_startup_recovered_cases:
            errors.append(f"shard startup recovery count mismatch: {label}")
        startup_policy = payload.get("startup_policy", {})
        if not (
            isinstance(startup_policy, dict)
            and startup_policy.get(
                "semantic_failure_count_excludes_harness_startup_failures"
            ) is True
        ):
            errors.append(f"shard lacks startup/semantic partition policy: {label}")
    if duplicate_case_ids:
        errors.append(
            "duplicate case ids across shards: " + ",".join(sorted(duplicate_case_ids))
        )
    if len(case_ids) != full_count:
        errors.append(
            f"case result coverage mismatch: expected={full_count} actual={len(case_ids)}"
        )

    case_outcomes = {
        key: sum(
            integer(payload.get(f"{key}_case_count"), f"{path}.{key}_case_count", errors)
            for path, payload in payloads
        )
        for key in ("passed", "failed", "skipped")
    }
    node_outcomes = {
        key: sum(
            integer(
                payload.get("node_outcomes", {}).get(key)
                if isinstance(payload.get("node_outcomes"), dict)
                else None,
                f"{path}.node_outcomes.{key}",
                errors,
            )
            for path, payload in payloads
        )
        for key in NODE_OUTCOME_KEYS
    }
    if len(tool_snapshots) != 1:
        errors.append("tool identity evidence differs across shards")
    if len(package_snapshots) != 1:
        errors.append("package identity evidence differs across shards")
    if case_outcomes != {"passed": canonical_count, "failed": 0, "skipped": 0}:
        errors.append(f"aggregate source-case outcomes are not all passed: {case_outcomes}")
    if (
        node_outcomes["unexpected_skipped"]
        or node_outcomes["xfailed"]
        or node_outcomes["xpassed"]
    ):
        errors.append("aggregate contains unexpected skip/xfail/xpass")
    if len(aggregate_expected_exclusions) != node_outcomes[
        "expected_version_or_platform_deselected"
    ]:
        errors.append("aggregate expected exclusion audit count mismatch")
    if len(aggregate_expected_upstream_static_skips) != node_outcomes[
        "expected_upstream_static_skipped"
    ]:
        errors.append("aggregate expected static skip audit count mismatch")

    status = "passed" if not errors else "failed"
    return {
        "schema_version": "scratchbird_firebird_qa_shard_aggregate_v1",
        "gate": "reference_firebird_qa_shard_aggregate",
        "status": status,
        "scope_complete": status == "passed",
        "shard_count": shard_count,
        "shard_indices": sorted(by_index),
        "full_discovered_case_count": full_count,
        "covered_case_count": len(case_ids),
        "full_discovery_manifest_sha256": manifest_hash,
        "canonical_scope_file": canonical_scope.get("scope_file", "injected_test_scope"),
        "canonical_manifest_file": canonical_scope.get(
            "manifest_path", "injected_test_manifest"
        ),
        "case_outcomes": case_outcomes,
        "node_outcomes": node_outcomes,
        "expected_exclusions": aggregate_expected_exclusions,
        "expected_upstream_static_skipped": node_outcomes[
            "expected_upstream_static_skipped"
        ],
        "expected_upstream_static_skips": aggregate_expected_upstream_static_skips,
        "input_evidence_sha256": evidence_hash.hexdigest(),
        "evidence_files": [str(path) for path in ordered_paths],
        "errors": errors,
    }


def main() -> int:
    parser = argparse.ArgumentParser()
    parser.add_argument("--evidence-file", action="append", default=[], type=Path)
    parser.add_argument("--evidence-glob", action="append", default=[])
    parser.add_argument("--output-file", required=True, type=Path)
    args = parser.parse_args()
    paths = list(args.evidence_file)
    for pattern in args.evidence_glob:
        paths.extend(Path(item) for item in glob.glob(pattern))
    payload = aggregate_evidence(paths)
    args.output_file.parent.mkdir(parents=True, exist_ok=True)
    args.output_file.write_text(
        json.dumps(payload, indent=2, sort_keys=True) + "\n", encoding="utf-8"
    )
    print(
        f"reference_firebird_qa_shard_aggregate={payload['status']} "
        f"covered={payload['covered_case_count']}/{payload['full_discovered_case_count']}"
    )
    return 0 if payload["status"] == "passed" else 1


if __name__ == "__main__":
    raise SystemExit(main())
