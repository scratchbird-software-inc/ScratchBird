#!/usr/bin/env python3
# Copyright (c) 2026 ScratchBird Software Inc.
#
# This Source Code Form is subject to the terms of the Mozilla Public
# License, v. 2.0. If a copy of the MPL was not distributed with this
# file, You can obtain one at https://mozilla.org/MPL/2.0/.
#
# SPDX-License-Identifier: MPL-2.0

"""Generate default-policy authority artifacts from lifecycle audit CSVs."""

from __future__ import annotations

import csv
from pathlib import Path


REQUIRED_DIAGNOSTICS = [
    "POLICY.CATALOG_MISSING",
    "POLICY.FAMILY_MISSING",
    "POLICY.PROFILE_INVALID",
    "POLICY.DEFAULT_PROPERTY_MISSING",
    "POLICY.GENERATION_STALE",
    "POLICY.OVERRIDE_FORBIDDEN",
    "POLICY.CLUSTER_SCOPE_FORBIDDEN",
    "POLICY.FAIL_CLOSED_BOUNDARY",
    "POLICY.AUDIT_REQUIRED",
]

AUTHORITY_INVARIANTS = [
    "policy_catalog_is_authority",
    "mga_visibility_required",
    "wal_not_authority",
    "parser_not_authority",
    "reference_not_authority",
    "uuid_order_not_finality",
]


def repo_root() -> Path:
    return Path(__file__).resolve().parents[3]


def yaml_scalar(value: object) -> str:
    if isinstance(value, bool):
        return "true" if value else "false"
    if isinstance(value, int):
        return str(value)
    text = str(value)
    if text in {"true", "false", "null"} or any(ch in text for ch in ":#[]{}&,*!|>'\"%@`"):
        return '"' + text.replace("\\", "\\\\").replace('"', '\\"') + '"'
    return text


def load_policy_rows(root: Path) -> list[dict[str, str]]:
    path = (
        root
        / "project/tests/database_lifecycle/fixtures/full_database_lifecycle_closure/artifacts/"
        / "DATABASE_LIFECYCLE_DEFAULT_POLICY_AUDIT.csv"
    )
    with path.open(newline="", encoding="utf-8") as handle:
        return list(csv.DictReader(handle))


def property_names(row: dict[str, str]) -> list[str]:
    count = int(row["required_property_count"])
    names = ["profile", "state", "policy_generation", "authority_ref"]
    if row["policy_key"] == "cluster.boundary_fail_closed":
        names = ["cluster_routes", "standalone_boundary", "policy_generation", "authority_ref"]
    elif row["policy_key"] == "replication.cdc_changefeed_boundary":
        names = ["cluster_scope", "changefeed_default", "fail_closed_boundary", "policy_generation"]
    elif row["policy_key"].startswith("security."):
        names = ["authority_model", "audit_requirement", "policy_generation", "authority_ref"]
    elif row["policy_key"].startswith("transaction."):
        names = ["mga_visibility", "admission_boundary", "policy_generation", "authority_ref"]
    elif row["policy_key"].startswith("resource."):
        names = ["seed_source", "signature_policy", "policy_generation", "authority_ref"]
    elif row["policy_key"].startswith("listener.") or row["policy_key"].startswith("server."):
        names = ["local_route_default", "auth_boundary", "policy_generation", "authority_ref"]
    while len(names) < count:
        names.append(f"required_property_{len(names) + 1:02d}")
    return names[:count]


def property_values(row: dict[str, str]) -> dict[str, str]:
    values: dict[str, str] = {}
    for name in property_names(row):
        if name == "profile":
            values[name] = row["profile"]
        elif name == "state":
            values[name] = row["state"]
        elif name == "policy_generation":
            values[name] = "1"
        elif name == "authority_ref":
            values[name] = "DEFAULT-POLICY-CATALOG-CREATE-DATABASE"
        elif name == "cluster_routes":
            values[name] = "deny"
        elif name == "standalone_boundary":
            values[name] = "fail_closed"
        elif name == "cluster_scope":
            values[name] = "cluster_only"
        elif name == "changefeed_default":
            values[name] = "disabled"
        elif name == "fail_closed_boundary":
            values[name] = "required"
        elif name == "mga_visibility":
            values[name] = "required"
        elif name == "admission_boundary":
            values[name] = "engine_owned"
        elif name == "authority_model":
            values[name] = "engine_owned"
        elif name == "audit_requirement":
            values[name] = "redacted_canonical"
        elif name == "seed_source":
            values[name] = "tx1"
        elif name == "signature_policy":
            values[name] = "record_provenance"
        elif name == "local_route_default":
            values[name] = "disabled_until_configured"
        elif name == "auth_boundary":
            values[name] = "required"
        else:
            values[name] = "required"
    return values


def diagnostics_for(row: dict[str, str]) -> list[str]:
    diagnostics = [
        "POLICY.FAMILY_MISSING",
        "POLICY.PROFILE_INVALID",
        "POLICY.DEFAULT_PROPERTY_MISSING",
        "POLICY.GENERATION_STALE",
        "POLICY.AUDIT_REQUIRED",
    ]
    if row["override_class"] == "no_override":
        diagnostics.append("POLICY.OVERRIDE_FORBIDDEN")
    if row["override_class"] == "cluster_only" or row["state"] == "fail_closed":
        diagnostics.extend(["POLICY.CLUSTER_SCOPE_FORBIDDEN", "POLICY.FAIL_CLOSED_BOUNDARY"])
    return diagnostics


def write_packet(root: Path, rows: list[dict[str, str]]) -> None:
    path = root / "public_input_snapshot"
    lines = [
        "# Default Policy Catalog",
        "",
        "Search key: `DEFAULT-POLICY-CATALOG-CREATE-DATABASE`",
        "",
        "This packet is the single authority for create-database default policy seed rows.",
        "The catalog owns policy family defaults, tx1 seed requirements, override classes,",
        "diagnostic coverage, cache invalidation expectations, and fail-closed boundaries.",
        "MGA visibility determines catalog visibility; WAL, cache, checkpoint, parser, reference,",
        "and UUID ordering are evidence inputs, not policy finality authorities.",
        "",
        "## Default policy family table",
        "",
        "| Policy key | Profile and state | Required properties |",
        "| --- | --- | --- |",
    ]
    for row in rows:
        properties = ", ".join(f"{key}={value}" for key, value in property_values(row).items())
        lines.append(
            f"| `{row['policy_key']}` | `{row['profile']}` / `{row['state']}` | {properties} |"
        )
    lines.extend(
        [
            "",
            "## Default policy override-class map",
            "",
            "| Policy key | Override class |",
            "| --- | --- |",
        ]
    )
    for row in rows:
        lines.append(f"| `{row['policy_key']}` | `{row['override_class']}` |")
    lines.extend(
        [
            "",
            "## Authority invariants",
            "",
            "- `policy_catalog_is_authority`",
            "- `mga_visibility_required`",
            "- `wal_not_authority`",
            "- `parser_not_authority`",
            "- `reference_not_authority`",
            "- `uuid_order_not_finality`",
        ]
    )
    path.write_text("\n".join(lines) + "\n", encoding="utf-8")


def write_registry(root: Path, rows: list[dict[str, str]]) -> None:
    path = root / "public_contract_snapshot"
    lines = [
        "schema_version: 1",
        "registry_id: default-policy-catalog",
        "search_key: DEFAULT-POLICY-CATALOG-CREATE-DATABASE",
        "source_of_truth: public_input_snapshot",
        f"policy_family_count: {len(rows)}",
        "universal_seed_requirements:",
        "  tx1_seed_required: true",
        "  policy_generation: 1",
        "  uuid_source: fresh_uuidv7",
        "  created_txn: tx1",
        "authority_invariants:",
    ]
    lines.extend(f"  - {item}" for item in AUTHORITY_INVARIANTS)
    lines.append("diagnostic_codes_required:")
    lines.extend(f"  - {code}" for code in REQUIRED_DIAGNOSTICS)
    lines.append("policies:")
    for row in rows:
        lines.extend(
            [
                f"- policy_key: {yaml_scalar(row['policy_key'])}",
                f"  default_profile: {yaml_scalar(row['profile'])}",
                f"  state: {yaml_scalar(row['state'])}",
                f"  override_class: {yaml_scalar(row['override_class'])}",
                "  required_properties:",
            ]
        )
        for key, value in property_values(row).items():
            lines.append(f"    {key}: {yaml_scalar(value)}")
        lines.extend(
            [
                "  tx1_seed:",
                "    required: true",
                "    policy_generation: 1",
                "    uuid_source: fresh_uuidv7",
                "    created_txn: tx1",
                "  diagnostics:",
            ]
        )
        lines.extend(f"    - {code}" for code in diagnostics_for(row))
        metrics = "sys.metrics.policy.lookup.*"
        if row["override_class"] == "no_override":
            metrics = "sys.metrics.policy.bootstrap.*"
        lines.extend(
            [
                f"  metrics: {metrics}",
                f"  cache_requirements: {row['policy_key'].replace('.', '_')}_cache",
                "  authority_invariants:",
            ]
        )
        lines.extend(f"    - {item}" for item in AUTHORITY_INVARIANTS)
    path.write_text("\n".join(lines) + "\n", encoding="utf-8")


def write_conformance(root: Path, rows: list[dict[str, str]]) -> None:
    path = root / "public_contract_snapshot"
    lines = [
        "schema_version: 1",
        "manifest_id: default-policy-catalog-conformance",
        "source_of_truth:",
        "  - public_input_snapshot",
        "  - public_contract_snapshot",
        "gates:",
        "- id: database_lifecycle_default_policy_registry",
        "  kind: ctest",
        "  proves: registry matches Markdown packet and audit CSV",
        "- id: database_lifecycle_policy_diagnostic_registry",
        "  kind: ctest",
        "  proves: POLICY diagnostics have codes and message-vector shapes",
        "- id: database_lifecycle_policy_override_fixtures",
        "  kind: ctest",
        "  proves: override classes and fail-closed boundaries are represented",
        "policy_families:",
    ]
    lines.extend(f"  - {yaml_scalar(row['policy_key'])}" for row in rows)
    path.write_text("\n".join(lines) + "\n", encoding="utf-8")


def merge_manifest(root: Path) -> None:
    path = root / "public_contract_snapshot"
    text = path.read_text(encoding="utf-8")
    authority_entries = [
        "- implementation_inputs/default_policy_catalog.md",
        "- implementation_inputs/engine_lifecycle.md",
        "- conformance_manifests/default-policy-catalog-conformance.yaml",
    ]
    registry_entry = "- registries/default-policy-catalog.yaml"
    if authority_entries[0] not in text:
        marker = "- implementation_inputs/sbmn_manager.md\n"
        text = text.replace(marker, marker + "\n".join(authority_entries) + "\n")
    for entry in authority_entries[1:]:
        if entry not in text:
            text = text.replace("registry_files:\n", f"{entry}\nregistry_files:\n")
    if registry_entry not in text:
        text = text.replace("registry_files:\n", f"registry_files:\n{registry_entry}\n")
    path.write_text(text, encoding="utf-8")


def append_policy_diagnostic_codes(root: Path) -> None:
    path = root / "public_contract_snapshot"
    text = path.read_text(encoding="utf-8")
    existing = {line.split(":", 1)[1].strip() for line in text.splitlines() if line.strip().startswith("- code:")}
    additions: list[str] = []
    for code in REQUIRED_DIAGNOSTICS:
        if code in existing:
            continue
        shape = "policy_" + code.split(".", 1)[1].lower() + "_shape"
        additions.extend(
            [
                f"- code: {code}",
                "  class: POLICY",
                "  severity: error",
                "  retryable: false",
                "  owner: database-lifecycle-policy",
                "  audit_policy: required",
                f"  shape: {shape}",
                "  authority: public_contract_snapshot",
            ]
        )
    if additions:
        path.write_text(text.rstrip() + "\n" + "\n".join(additions) + "\n", encoding="utf-8")


def write_diagnostic_shapes(root: Path) -> None:
    path = root / "public_contract_snapshot"
    lines = [
        "registry: diagnostic-shape-registry",
        "owner: database-lifecycle-policy",
        "shapes:",
    ]
    for code in REQUIRED_DIAGNOSTICS:
        suffix = code.split(".", 1)[1].lower()
        lines.extend(
            [
                f"- diagnostic_shape_id: policy_{suffix}_shape",
                f"  canonical_message_key: policy.{suffix}",
                "  message_vector_mapping:",
                "    code: diagnostic.code",
                "    family: policy.family",
                "    profile: policy.profile",
                "    generation: policy.generation",
            ]
        )
    path.write_text("\n".join(lines) + "\n", encoding="utf-8")


def write_lifecycle_specs(root: Path) -> None:
    packet = root / "public_input_snapshot"
    packet.write_text(
        "\n".join(
            [
                "# Engine Lifecycle",
                "",
                "Search key: `DBLC-001-CANONICAL-LIFECYCLE-SPEC-CLOSURE`",
                "",
                "The canonical lifecycle sequence is create tx1, first-open tx2, open/reopen, attach,",
                "transaction admission, detach, maintenance, restricted-open, diagnostic, verify, repair,",
                "graceful shutdown, force shutdown, final clean shutdown transaction, and drop.",
                "Each phase emits a message vector, performs cache invalidation when policy generation",
                "changes, preserves MGA visibility, records WAL evidence without treating WAL as policy",
                "authority, keeps cluster fail-closed behavior explicit, and keeps manager, listener,",
                "parser, and process association responsibilities separated.",
            ]
        )
        + "\n",
        encoding="utf-8",
    )
    chapters = {
        "04-storage-mga-and-transactions.md": (
            "Storage lifecycle closure records tx1, tx2, final shutdown, MGA, WAL, and filespace "
            "rules under `DBLC-001-CANONICAL-LIFECYCLE-SPEC-CLOSURE`."
        ),
        "08-security-auth-and-audit.md": (
            "Security lifecycle closure records authentication, authorization, policy generation, "
            "and engine-owned protected material under `DBLC-001-CANONICAL-LIFECYCLE-SPEC-CLOSURE`."
        ),
        "09-server-listener-ipc-and-config.md": (
            "Server lifecycle closure records manager, listener, parser, shutdown, and process "
            "association boundaries under `DBLC-001-CANONICAL-LIFECYCLE-SPEC-CLOSURE`."
        ),
        "12-resources-locales-timezones-collations.md": (
            "Resource lifecycle closure records timezone, charset, collation, tx1, and first-open "
            "resource seed requirements under `DBLC-001-CANONICAL-LIFECYCLE-SPEC-CLOSURE`."
        ),
        "13-operations-deployment-and-observability.md": (
            "Operations lifecycle closure records maintenance, restricted open, verify, repair, "
            "metrics, and diagnostic evidence under `DBLC-001-CANONICAL-LIFECYCLE-SPEC-CLOSURE`."
        ),
    }
    chapter_root = root / "public_contract_snapshot"
    chapter_root.mkdir(parents=True, exist_ok=True)
    for name, body in chapters.items():
        (chapter_root / name).write_text(f"# {name[:-3]}\n\n{body}\n", encoding="utf-8")


def main() -> None:
    root = repo_root()
    rows = load_policy_rows(root)
    (root / "public_contract_snapshot").mkdir(parents=True, exist_ok=True)
    (root / "public_contract_snapshot").mkdir(parents=True, exist_ok=True)
    (root / "public_contract_snapshot").mkdir(parents=True, exist_ok=True)
    write_packet(root, rows)
    write_registry(root, rows)
    write_conformance(root, rows)
    merge_manifest(root)
    append_policy_diagnostic_codes(root)
    write_diagnostic_shapes(root)
    write_lifecycle_specs(root)
    print(f"Generated default-policy authority for {len(rows)} policy families.")


if __name__ == "__main__":
    main()
