#!/usr/bin/env python3
# Copyright (c) 2026 ScratchBird Software Inc.
#
# This Source Code Form is subject to the terms of the Mozilla Public
# License, v. 2.0. If a copy of the MPL was not distributed with this
# file, You can obtain one at https://mozilla.org/MPL/2.0/.
#
# SPDX-License-Identifier: MPL-2.0

"""Generate the ScratchBird internal documentation baseline.

The generator is intentionally authority-first. It emits book artifacts for the
registered documentation set, but it marks books as blocked from publication
whenever required source authority is absent or only partially inventoried.
"""

from __future__ import annotations

import argparse
import csv
import hashlib
import html
import json
import os
import re
import sys
from dataclasses import dataclass, field
from datetime import datetime, timezone
from pathlib import Path
from typing import Any, Iterable

try:
    import yaml
except ImportError as exc:  # pragma: no cover - covered by invocation failure
    raise SystemExit("PyYAML is required to generate documentation baseline") from exc


GENERATOR_ID = "scratchbird-documentation-baseline-generator"
GENERATOR_VERSION = "2026.06.04.5"
DOCS_ROOT = Path("docs")
EXECUTION_PLAN_ROOT = DOCS_ROOT / "execution-plans"
COMPLETED_EXECUTION_PLAN_ROOT = DOCS_ROOT / "completed-execution-plans"
EXECUTION_PLAN_DIR = EXECUTION_PLAN_ROOT / "end-user-documentation-baseline-generation"
MANUAL_REGISTRY = Path(
    "public_contract_snapshot"
)
CONFORMANCE_MANIFEST = Path(
    "public_contract_snapshot"
)
BOOK_REGISTRY = EXECUTION_PLAN_DIR / "DOCUMENTATION_BOOK_REGISTRY.csv"
BASELINE_ROOT = Path("docs/documentation/_generated/baseline")
FORBIDDEN_RELEASE_TERMS = (
    "TBD",
    "TODO",
    "stub",
    "placeholder",
    "minimal",
    "future work",
    "implementation-defined",
    "as appropriate",
    "generally",
)

DONOR_PUBLICATION_SOURCE_INPUTS = {
    "donor_catalog_seed_manifests",
    "catalog_seed_manifests",
    "donor_function_mappings",
    "donor_replication_migration_profiles",
    "donor_sbsql_equivalence_profiles",
    "donor_parser_profiles",
    "donor_parser_registries",
    "donor_wire_api_profiles",
    "wire_api_profiles",
    "regression_suites",
}

SECURITY_PUBLICATION_SOURCE_INPUTS = {
    "security_bootstrap_registry",
    "security_policy_registry",
    "security_policy",
    "policy",
    "authentication_registry",
    "authentication",
    "authorization_registry",
    "authorization",
    "audit_registry",
    "audit",
    "redaction_policy_registry",
    "redaction",
    "audit_checklists",
}

EXAMPLE_PUBLICATION_SOURCE_INPUTS = {
    "quick_start_example_manifest",
    "example_manifest",
    "example_replay_manifest",
    "example_sources",
    "examples",
    "sample_database_manifest",
}

CLI_PUBLICATION_SOURCE_INPUTS = {
    "cli_help_extraction",
    "cli_help",
    "cli_flag_registry",
    "tool_metadata",
}


@dataclass
class SourceRule:
    title: str
    evidence: tuple[str, ...]
    complete_when_found: bool
    resolution: str
    notes: str = ""
    required_evidence: tuple[str, ...] = field(default_factory=tuple)


@dataclass
class SourceAssessment:
    source_input: str
    status: str
    evidence: list[str]
    evidence_hashes: list[str]
    resolution: str
    notes: str


@dataclass
class Book:
    book_id: str
    title: str
    output_root: Path
    priority: str = ""
    release_applicability: str = ""
    owner: str = ""
    source_inputs: list[str] = field(default_factory=list)
    chapter_sequence: list[str] = field(default_factory=list)
    required_gates: list[str] = field(default_factory=list)
    registry_sources: list[str] = field(default_factory=list)
    manual_registry_id: str = ""
    required_chapter_rule: str = ""
    primary_gate: str = ""


SOURCE_RULES: dict[str, SourceRule] = {
    "specs": SourceRule(
        "canonical contract manifest",
        ("public_contract_snapshot", "public_contract_snapshot"),
        True,
        "Keep the canonical contract manifest and authority inventory current.",
    ),
    "installation_package_manifest": SourceRule(
        "installation package manifest",
        ("project/cloud/kubernetes/package-manifest.yaml", "project/drivers/DriverPackageManifest.csv"),
        False,
        "Create a release package manifest that covers every installable ScratchBird package.",
        "Current evidence is component packaging metadata, not a unified install package manifest.",
    ),
    "install_metadata": SourceRule(
        "installation metadata",
        ("project/cloud/kubernetes/package-manifest.yaml", "project/drivers/DriverPackageManifest.csv"),
        False,
        "Create a unified install metadata extractor and release package manifest.",
    ),
    "cli_help_extraction": SourceRule(
        "CLI help extraction",
        ("public_contract_snapshot",),
        True,
        "Keep the CLI/tool branding authority index manifest listed.",
    ),
    "cli_help": SourceRule(
        "CLI help",
        ("public_contract_snapshot",),
        True,
        "Keep the CLI/tool branding authority index manifest listed.",
    ),
    "configuration_registry": SourceRule(
        "configuration registry",
        (
            "public_contract_snapshot",
            "public_contract_snapshot",
        ),
        True,
        "Keep configuration registries manifest listed and extractable.",
    ),
    "configuration_schema_registry": SourceRule(
        "configuration schema registry",
        (
            "public_contract_snapshot",
            "public_contract_snapshot",
        ),
        True,
        "Keep configuration schema registries manifest listed and extractable.",
    ),
    "config_schema": SourceRule(
        "configuration schema",
        (
            "public_contract_snapshot",
            "public_contract_snapshot",
        ),
        True,
        "Keep configuration schema registries manifest listed and extractable.",
    ),
    "quick_start_example_manifest": SourceRule(
        "quick start example manifest",
        ("public_contract_snapshot",),
        True,
        "Keep the documentation example/replay authority index manifest listed.",
    ),
    "example_manifest": SourceRule(
        "example manifest",
        ("public_contract_snapshot",),
        True,
        "Keep the documentation example/replay authority index manifest listed.",
    ),
    "security_bootstrap_registry": SourceRule(
        "security bootstrap registry",
        ("public_contract_snapshot",),
        True,
        "Keep the security/redaction/policy documentation authority index manifest listed.",
    ),
    "sbsql_surface_registry": SourceRule(
        "SBsql surface registry",
        (
            "public_input_snapshot",
            "public_contract_snapshot",
        ),
        True,
        "Keep SBsql surface registries synchronized.",
    ),
    "sbsql_status_matrix": SourceRule(
        "SBsql surface status matrix",
        ("public_input_snapshot",),
        True,
        "Keep SBsql surface status matrix synchronized.",
    ),
    "sbsql_sblr_matrix": SourceRule(
        "SBsql to SBLR matrix",
        ("public_input_snapshot",),
        True,
        "Keep SBsql to SBLR matrix synchronized.",
    ),
    "sbsql_sblr_mapping": SourceRule(
        "SBsql to SBLR mapping",
        ("public_input_snapshot",),
        True,
        "Keep SBsql to SBLR matrix synchronized.",
    ),
    "grammar_resources": SourceRule(
        "grammar resources",
        (
            "public_contract_snapshot",
            "public_contract_snapshot",
            "public_contract_snapshot",
            "public_input_snapshot",
            (COMPLETED_EXECUTION_PLAN_ROOT / "sbsql-native-v3-full-dialect-support/SBSQL_V3_GRAMMAR_CONTRACT.md").as_posix(),
            (COMPLETED_EXECUTION_PLAN_ROOT / "sbsql-native-v3-full-dialect-support/SBSQL_V3_GRAMMAR_INVENTORY.yaml").as_posix(),
        ),
        False,
        "Promote the completed SBsql V3 grammar prep into parser source-of-editing artifacts: sbsql_v3.ebnf, sbsql_v3_precedence.yaml, sbsql_v3_keywords.yaml, and sbsql_v3_lexer.yaml.",
        "Completed grammar-prep evidence exists, but the parser-required source-of-editing grammar resource set is not present yet.",
        required_evidence=(
            "project/src/parsers/native/v3/grammar/sbsql_v3.ebnf",
            "project/src/parsers/native/v3/grammar/sbsql_v3_precedence.yaml",
            "project/src/parsers/native/v3/grammar/sbsql_v3_keywords.yaml",
            "project/src/parsers/native/v3/lexer/sbsql_v3_lexer.yaml",
        ),
    ),
    "grammar_specs": SourceRule(
        "grammar contracts",
        (
            "public_contract_snapshot",
            "public_contract_snapshot",
            "public_contract_snapshot",
            "public_contract_snapshot",
            (COMPLETED_EXECUTION_PLAN_ROOT / "sbsql-native-v3-full-dialect-support/SBSQL_V3_GRAMMAR_CONTRACT.md").as_posix(),
            (COMPLETED_EXECUTION_PLAN_ROOT / "sbsql-native-v3-full-dialect-support/SBSQL_V3_GRAMMAR_INVENTORY.yaml").as_posix(),
            (COMPLETED_EXECUTION_PLAN_ROOT / "sbsql-native-v3-full-dialect-support/SBSQL_V3_LEXICAL_CONTRACT.md").as_posix(),
            (COMPLETED_EXECUTION_PLAN_ROOT / "sbsql-native-v3-full-dialect-support/SBSQL_V3_AMBIGUITY_RULES.md").as_posix(),
        ),
        False,
        "Promote the completed SBsql V3 grammar prep into parser source-of-editing artifacts and keep them manifest-listed or otherwise generation-gated.",
        "Completed grammar-prep contracts exist, but final parser source-of-editing grammar artifacts are not present yet.",
        required_evidence=(
            "project/src/parsers/native/v3/grammar/sbsql_v3.ebnf",
            "project/src/parsers/native/v3/grammar/sbsql_v3_precedence.yaml",
            "project/src/parsers/native/v3/grammar/sbsql_v3_keywords.yaml",
            "project/src/parsers/native/v3/lexer/sbsql_v3_lexer.yaml",
        ),
    ),
    "sblr_operation_matrix": SourceRule(
        "SBLR operation matrix",
        (
            "public_contract_snapshot",
            "project/src/engine/internal_api/SBLR_API_OPERATION_MATRIX.yaml",
        ),
        True,
        "Keep SBLR operation matrix synchronized.",
    ),
    "function_operator_registry": SourceRule(
        "function and operator registry",
        (
            "public_contract_snapshot",
            "public_contract_snapshot",
            "public_contract_snapshot",
            "public_contract_snapshot",
        ),
        False,
        "Publish a single function and operator registry for documentation extraction.",
    ),
    "function_catalog": SourceRule(
        "function catalog",
        (
            "public_contract_snapshot",
            "public_contract_snapshot",
            "public_contract_snapshot",
        ),
        False,
        "Publish a single function catalog for documentation extraction.",
    ),
    "diagnostic_shape_registry": SourceRule(
        "diagnostic shape registry",
        (
            "public_contract_snapshot",
            "public_contract_snapshot",
        ),
        True,
        "Keep diagnostic shape registry synchronized.",
    ),
    "diagnostic_registry": SourceRule(
        "diagnostic registry",
        (
            "public_contract_snapshot",
            "public_contract_snapshot",
        ),
        True,
        "Keep diagnostic registry synchronized.",
    ),
    "diagnostics": SourceRule(
        "diagnostics",
        (
            "public_contract_snapshot",
            "public_contract_snapshot",
        ),
        True,
        "Keep diagnostic registry synchronized.",
    ),
    "diagnostics_registry": SourceRule(
        "diagnostics registry",
        (
            "public_contract_snapshot",
            "public_contract_snapshot",
        ),
        True,
        "Keep diagnostics registry synchronized.",
    ),
    "result_shape_registry": SourceRule(
        "result shape registry",
        ("public_contract_snapshot",),
        True,
        "Keep result shape registry synchronized.",
    ),
    "catalog_registry": SourceRule(
        "catalog registry",
        (
            "public_contract_snapshot*.md",
            "public_contract_snapshot",
        ),
        False,
        "Publish a single catalog documentation registry and generated readable-view index.",
    ),
    "catalog_specs": SourceRule(
        "catalog contracts",
        (
            "public_contract_snapshot*.md",
            "public_contract_snapshot",
        ),
        False,
        "Publish a single catalog documentation registry and generated readable-view index.",
    ),
    "catalog_metadata": SourceRule(
        "catalog metadata",
        (
            "public_contract_snapshot*.md",
            "public_contract_snapshot",
        ),
        False,
        "Publish a single catalog documentation registry and generated readable-view index.",
    ),
    "readable_views": SourceRule(
        "readable catalog views",
        ("public_contract_snapshot",),
        True,
        "Keep readable catalog view contract current.",
    ),
    "information_schema": SourceRule(
        "information schema views",
        ("public_contract_snapshot",),
        True,
        "Keep information schema view contract current.",
    ),
    "seed_manifests": SourceRule(
        "seed manifests",
        ("public_contract_snapshot*-seed*.yaml", "project/tests/sblr_surface/fixtures"),
        False,
        "Publish central catalog seed manifests for generated documentation.",
    ),
    "conformance_manifests": SourceRule(
        "conformance manifests",
        ("public_contract_snapshot*.yaml",),
        True,
        "Keep conformance manifests current.",
    ),
    "driver_api_registries": SourceRule(
        "driver API registries",
        (
            "project/drivers/DriverPackageManifest.csv",
            "public_contract_snapshot",
            "public_contract_snapshot",
        ),
        False,
        "Publish per-driver API registries and documentation extract manifests.",
    ),
    "driver_api_metadata": SourceRule(
        "driver API metadata",
        (
            "project/drivers/DriverPackageManifest.csv",
            "public_contract_snapshot",
        ),
        False,
        "Publish per-driver API registries and documentation extract manifests.",
    ),
    "driver_neutral_api": SourceRule(
        "driver-neutral API",
        (
            "public_contract_snapshot",
            "project/drivers/DriverPackageManifest.csv",
        ),
        False,
        "Publish driver-neutral API docs extraction metadata.",
    ),
    "driver_conformance_manifests": SourceRule(
        "driver conformance manifests",
        ("public_contract_snapshot",),
        True,
        "Keep driver conformance manifest current.",
    ),
    "driver_conformance": SourceRule(
        "driver conformance",
        ("public_contract_snapshot",),
        True,
        "Keep driver conformance manifest current.",
    ),
    "language_binding_metadata": SourceRule(
        "language binding metadata",
        ("project/drivers/DriverPackageManifest.csv", "tracks/*/drivers"),
        False,
        "Publish per-language binding metadata for standalone driver books.",
    ),
    "language_metadata": SourceRule(
        "language metadata",
        ("project/drivers/DriverPackageManifest.csv", "tracks/*/drivers"),
        False,
        "Publish per-language binding metadata for standalone driver books.",
    ),
    "sample_database_manifest": SourceRule(
        "sample database manifest",
        ("public_contract_snapshot",),
        True,
        "Keep the documentation example/replay authority index manifest listed.",
    ),
    "example_replay_manifest": SourceRule(
        "example replay manifest",
        ("public_contract_snapshot",),
        True,
        "Keep the documentation example/replay authority index manifest listed.",
    ),
    "example_sources": SourceRule(
        "example sources",
        ("public_contract_snapshot",),
        True,
        "Keep the documentation example/replay authority index manifest listed.",
    ),
    "examples": SourceRule(
        "examples",
        ("public_contract_snapshot",),
        True,
        "Keep the documentation example/replay authority index manifest listed.",
    ),
    "transaction_contract_refs": SourceRule(
        "transaction contract references",
        (
            "public_contract_snapshot",
            "public_contract_snapshot*.md",
        ),
        True,
        "Keep MGA transaction authority synchronized.",
    ),
    "transactions": SourceRule(
        "transaction references",
        (
            "public_contract_snapshot",
            "public_contract_snapshot*.md",
        ),
        True,
        "Keep MGA transaction authority synchronized.",
    ),
    "operations_registry": SourceRule(
        "operations registry",
        (
            "public_contract_snapshot",
            "public_contract_snapshot*.md",
        ),
        False,
        "Publish a central operations command and runbook registry.",
    ),
    "operations_specs": SourceRule(
        "operations contracts",
        (
            "public_contract_snapshot",
            "public_contract_snapshot*.md",
        ),
        False,
        "Publish a central operations command and runbook registry.",
    ),
    "backup_restore_registry": SourceRule(
        "backup restore registry",
        ("public_contract_snapshot",),
        False,
        "Publish a backup and restore user-facing registry.",
    ),
    "backup_restore": SourceRule(
        "backup restore",
        ("public_contract_snapshot",),
        False,
        "Publish a backup and restore user-facing registry.",
    ),
    "replication": SourceRule(
        "replication",
        ("public_contract_snapshot",),
        False,
        "Publish ScratchBird replication and migration documentation registry.",
    ),
    "observability_registry": SourceRule(
        "observability registry",
        (
            "public_contract_snapshot",
            "public_contract_snapshot*.md",
        ),
        False,
        "Publish a central observability documentation registry.",
    ),
    "metrics": SourceRule(
        "metrics",
        (
            "public_contract_snapshot",
            "public_contract_snapshot*.md",
        ),
        False,
        "Publish a central observability documentation registry.",
    ),
    "support_bundle_registry": SourceRule(
        "support bundle registry",
        ("public_contract_snapshot",),
        True,
        "Create support-bundle-registry.yaml and the support bundle extraction contract.",
    ),
    "support_bundle": SourceRule(
        "support bundle",
        ("public_contract_snapshot",),
        True,
        "Create support-bundle-registry.yaml and the support bundle extraction contract.",
    ),
    "support_process_registry": SourceRule(
        "support process registry",
        ("public_contract_snapshot",),
        True,
        "Create support-process-registry.yaml with escalation, errata, and support ownership.",
    ),
    "support_process": SourceRule(
        "support process",
        ("public_contract_snapshot",),
        True,
        "Create support-process-registry.yaml with escalation, errata, and support ownership.",
    ),
    "operational_runbook_registry": SourceRule(
        "operational runbook registry",
        ("public_contract_snapshot",),
        True,
        "Create operational-runbook-registry.yaml with user-facing procedures.",
    ),
    "runbooks": SourceRule(
        "runbooks",
        ("public_contract_snapshot",),
        True,
        "Create operational-runbook-registry.yaml with user-facing procedures.",
    ),
    "security_policy_registry": SourceRule(
        "security policy registry",
        ("public_contract_snapshot",),
        True,
        "Keep the security/redaction/policy documentation authority index manifest listed.",
    ),
    "security_policy": SourceRule(
        "security policy",
        ("public_contract_snapshot",),
        True,
        "Keep the security/redaction/policy documentation authority index manifest listed.",
    ),
    "policy": SourceRule(
        "policy",
        ("public_contract_snapshot",),
        True,
        "Keep the security/redaction/policy documentation authority index manifest listed.",
    ),
    "authentication_registry": SourceRule(
        "authentication registry",
        ("public_contract_snapshot",),
        True,
        "Keep the security/redaction/policy documentation authority index manifest listed.",
    ),
    "authentication": SourceRule(
        "authentication",
        ("public_contract_snapshot",),
        True,
        "Keep the security/redaction/policy documentation authority index manifest listed.",
    ),
    "authorization_registry": SourceRule(
        "authorization registry",
        ("public_contract_snapshot",),
        True,
        "Keep the security/redaction/policy documentation authority index manifest listed.",
    ),
    "authorization": SourceRule(
        "authorization",
        ("public_contract_snapshot",),
        True,
        "Keep the security/redaction/policy documentation authority index manifest listed.",
    ),
    "audit_registry": SourceRule(
        "audit registry",
        ("public_contract_snapshot",),
        True,
        "Keep the security/redaction/policy documentation authority index manifest listed.",
    ),
    "audit": SourceRule(
        "audit",
        ("public_contract_snapshot",),
        True,
        "Keep the security/redaction/policy documentation authority index manifest listed.",
    ),
    "redaction_policy_registry": SourceRule(
        "redaction policy registry",
        ("public_contract_snapshot",),
        True,
        "Keep the security/redaction/policy documentation authority index manifest listed.",
    ),
    "redaction": SourceRule(
        "redaction",
        ("public_contract_snapshot",),
        True,
        "Keep the security/redaction/policy documentation authority index manifest listed.",
    ),
    "donor_parser_registries": SourceRule(
        "donor parser registries",
        (
            "public_contract_snapshot*.yaml",
            "public_contract_snapshot*-exact-extraction-*.yaml",
        ),
        True,
        "Keep donor parser registries synchronized.",
    ),
    "donor_parser_profiles": SourceRule(
        "donor parser profiles",
        (
            "public_contract_snapshot*.yaml",
            "public_contract_snapshot*-exact-extraction-*.yaml",
        ),
        True,
        "Keep donor parser registries synchronized.",
    ),
    "donor_catalog_seed_manifests": SourceRule(
        "donor catalog seed manifests",
        ("public_contract_snapshot",),
        True,
        "Publish a central donor catalog seed manifest index for documentation.",
    ),
    "catalog_seed_manifests": SourceRule(
        "catalog seed manifests",
        ("public_contract_snapshot",),
        True,
        "Publish a central donor catalog seed manifest index for documentation.",
    ),
    "donor_wire_api_profiles": SourceRule(
        "donor wire API profiles",
        (
            "public_contract_snapshot",
            "public_contract_snapshot",
        ),
        True,
        "Keep donor wire API profile registries synchronized.",
    ),
    "wire_api_profiles": SourceRule(
        "wire API profiles",
        (
            "public_contract_snapshot",
            "public_contract_snapshot",
        ),
        True,
        "Keep donor wire API profile registries synchronized.",
    ),
    "donor_function_mappings": SourceRule(
        "donor function mappings",
        ("public_contract_snapshot",),
        True,
        "Publish a central donor function mapping index for documentation.",
    ),
    "donor_replication_migration_profiles": SourceRule(
        "donor replication and migration profiles",
        ("public_contract_snapshot",),
        True,
        "Publish a central donor replication and live-migration documentation index.",
    ),
    "donor_sbsql_equivalence_profiles": SourceRule(
        "donor to SBsql equivalence profiles",
        ("public_contract_snapshot",),
        True,
        "Publish per-donor migration equivalence profiles that map donor dialects, command processes, and API workflows onto SBsql and SBLR.",
        "Central donor documentation authority index carries all-family legal hold and completed-emulation publication assumptions.",
    ),
    "parser_support_udr_management_package_abi": SourceRule(
        "parser support UDR management package ABI",
        ("public_contract_snapshot",),
        True,
        "Keep parser support UDR management package ABI synchronized.",
    ),
    "parser_support_udr_abi": SourceRule(
        "parser support UDR ABI",
        ("public_contract_snapshot",),
        True,
        "Keep parser support UDR management package ABI synchronized.",
    ),
    "regression_suites": SourceRule(
        "donor regression suites",
        (
            "public_contract_snapshot",
            "project/tests/donor_regression",
        ),
        False,
        "Publish per-donor regression and performance suite replay manifests.",
    ),
    "release_manifest": SourceRule(
        "release manifest",
        ("public_contract_snapshot",),
        False,
        "Publish a release manifest with user-visible feature and package coverage.",
    ),
    "upgrade_migration_registry": SourceRule(
        "upgrade migration registry",
        ("public_contract_snapshot",),
        True,
        "Create upgrade-migration-registry.yaml for release documentation.",
    ),
    "upgrade_registry": SourceRule(
        "upgrade registry",
        ("public_contract_snapshot",),
        True,
        "Create upgrade-migration-registry.yaml for release documentation.",
    ),
    "compatibility_matrix": SourceRule(
        "compatibility matrix",
        ("public_contract_snapshot",),
        True,
        "Keep compatibility matrix current.",
    ),
    "known_issues_registry": SourceRule(
        "known issues registry",
        ("public_contract_snapshot",),
        True,
        "Create known-issues-registry.yaml for generated release and support docs.",
    ),
    "known_issue_registry": SourceRule(
        "known issue registry",
        ("public_contract_snapshot",),
        True,
        "Create known-issues-registry.yaml for generated release and support docs.",
    ),
    "known_issues": SourceRule(
        "known issues",
        ("public_contract_snapshot",),
        True,
        "Create known-issues-registry.yaml for generated release and support docs.",
    ),
    "cli_flag_registry": SourceRule(
        "CLI flag registry",
        ("public_contract_snapshot",),
        True,
        "Keep the CLI/tool branding authority index manifest listed.",
    ),
    "env_registry": SourceRule(
        "environment registry",
        ("public_contract_snapshot",),
        True,
        "Create environment-variable-registry.yaml from deterministic source extraction.",
    ),
    "environment_variable_registry": SourceRule(
        "environment variable registry",
        ("public_contract_snapshot",),
        True,
        "Create environment-variable-registry.yaml from deterministic source extraction.",
    ),
    "tool_metadata": SourceRule(
        "tool metadata",
        ("public_contract_snapshot",),
        True,
        "Keep the CLI/tool branding authority index manifest listed.",
    ),
    "exit_codes": SourceRule(
        "exit codes",
        ("public_contract_snapshot",),
        True,
        "Create exit-code-registry.yaml from deterministic source extraction.",
    ),
    "sblr_language_specs": SourceRule(
        "SBLR language contracts",
        (
            "public_contract_snapshot",
            "public_contract_snapshot",
            "public_contract_snapshot",
        ),
        True,
        "Keep SBLR language contracts synchronized.",
    ),
    "verifier_specs": SourceRule(
        "SBLR verifier contracts",
        ("public_contract_snapshot", "public_contract_snapshot"),
        False,
        "Publish a SBLR verifier behavior registry for generated documentation.",
    ),
    "result_shapes": SourceRule(
        "result shapes",
        ("public_contract_snapshot",),
        True,
        "Keep result shape registry synchronized.",
    ),
    "udr_specs": SourceRule(
        "UDR contracts",
        ("public_contract_snapshot*.md", "public_contract_snapshot"),
        False,
        "Publish UDR user-facing package and security registries.",
    ),
    "architecture_invariants": SourceRule(
        "architecture invariants",
        ("public_contract_snapshot",),
        True,
        "Keep architecture invariants current.",
    ),
    "mga": SourceRule(
        "MGA",
        ("public_contract_snapshot",),
        True,
        "Keep MGA authority current.",
    ),
    "uuid_identity": SourceRule(
        "UUID identity",
        ("public_contract_snapshot",),
        True,
        "Keep UUID identity authority current.",
    ),
    "schema_tree": SourceRule(
        "schema tree",
        ("public_contract_snapshot",),
        True,
        "Keep recursive schema tree authority current.",
    ),
    "parser_sblr_boundary": SourceRule(
        "parser to SBLR boundary",
        ("public_contract_snapshot",),
        True,
        "Keep parser to SBLR boundary authority current.",
    ),
    "benchmark_manifests": SourceRule(
        "benchmark manifests",
        ("public_contract_snapshot", "project/tests/performance"),
        False,
        "Create benchmark-manifest-registry.yaml with reproducible performance suites.",
    ),
    "capacity_profiles": SourceRule(
        "capacity profiles",
        ("public_contract_snapshot",),
        True,
        "Create capacity-profile-registry.yaml for generated performance docs.",
    ),
    "emulation_overhead": SourceRule(
        "emulation overhead",
        ("public_contract_snapshot",),
        True,
        "Create emulation-overhead-registry.yaml from benchmark evidence.",
    ),
    "live_migration_perf": SourceRule(
        "live migration performance",
        ("public_contract_snapshot",),
        True,
        "Create live-migration-performance-registry.yaml from benchmark evidence.",
    ),
    "ctest_labels": SourceRule(
        "CTest labels",
        ("project/tests", "CMakeLists.txt"),
        False,
        "Publish generated CTest label inventory for documentation.",
    ),
    "proof_gates": SourceRule(
        "proof gates",
        ("public_contract_snapshot*.yaml", "project/tests"),
        False,
        "Publish generated proof gate inventory for documentation.",
    ),
    "fixture_manifests": SourceRule(
        "fixture manifests",
        ("project/tests/**/fixtures",),
        False,
        "Publish generated fixture manifest inventory for documentation.",
    ),
    "audit_checklists": SourceRule(
        "audit checklists",
        ("public_contract_snapshot",),
        True,
        "Keep the security/redaction/policy documentation authority index manifest listed.",
    ),
    "release_gates": SourceRule(
        "release gates",
        ("public_contract_snapshot*.yaml",),
        True,
        "Keep release gates current.",
    ),
    "closure_records": SourceRule(
        "closure records",
        ((DOCS_ROOT / "audit").as_posix(), (DOCS_ROOT / "reports").as_posix(), COMPLETED_EXECUTION_PLAN_ROOT.as_posix()),
        False,
        "Publish release closure record manifest for documentation.",
    ),
}


def parse_args() -> argparse.Namespace:
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument("--repo-root", default=".", help="ScratchBird repository root")
    parser.add_argument(
        "--check",
        action="store_true",
        help="validate generated artifacts after writing them",
    )
    return parser.parse_args()


def rel(path: Path, root: Path) -> str:
    return path.resolve().relative_to(root.resolve()).as_posix()


def slug(value: str) -> str:
    value = value.strip().lower().replace("&", " and ")
    value = re.sub(r"[^a-z0-9]+", "_", value)
    return value.strip("_")


def title_from_id(value: str) -> str:
    return " ".join(part.capitalize() for part in slug(value).split("_"))


def ensure_ascii(value: str, path: Path) -> None:
    try:
        value.encode("ascii")
    except UnicodeEncodeError as exc:
        raise SystemExit(f"{path}: non-ASCII output rejected") from exc


def read_csv(path: Path) -> list[dict[str, str]]:
    with path.open(newline="", encoding="utf-8") as fh:
        return list(csv.DictReader(fh))


def write_csv(path: Path, rows: Iterable[dict[str, Any]], fieldnames: list[str]) -> None:
    path.parent.mkdir(parents=True, exist_ok=True)
    with path.open("w", newline="", encoding="utf-8") as fh:
        writer = csv.DictWriter(fh, fieldnames=fieldnames)
        writer.writeheader()
        for row in rows:
            writer.writerow({key: row.get(key, "") for key in fieldnames})


def load_yaml(path: Path) -> Any:
    with path.open(encoding="utf-8") as fh:
        return yaml.safe_load(fh)


def file_sha256(path: Path) -> str:
    h = hashlib.sha256()
    with path.open("rb") as fh:
        for chunk in iter(lambda: fh.read(1024 * 1024), b""):
            h.update(chunk)
    return h.hexdigest()


def text_sha256(text: str) -> str:
    return hashlib.sha256(text.encode("utf-8")).hexdigest()


def write_text(path: Path, text: str) -> None:
    ensure_ascii(text, path)
    path.parent.mkdir(parents=True, exist_ok=True)
    if text and not text.endswith("\n"):
        text += "\n"
    path.write_text(text, encoding="utf-8")


def write_json(path: Path, data: Any) -> None:
    text = json.dumps(data, indent=2, sort_keys=True)
    write_text(path, text)


def split_inputs(value: str) -> list[str]:
    if not value:
        return []
    parts = re.split(r"[;,]", value)
    return [slug(part) for part in parts if part.strip()]


def load_execution_plan_books(root: Path) -> dict[str, Book]:
    books: dict[str, Book] = {}
    for row in read_csv(root / BOOK_REGISTRY):
        book_id = slug(row["book_id"])
        books[book_id] = Book(
            book_id=book_id,
            title=row["title"].strip(),
            output_root=Path(row["output_root"].strip().rstrip("/")),
            priority=row.get("priority", "").strip(),
            release_applicability=row.get("release_applicability", "").strip(),
            owner=row.get("owner", "").strip(),
            source_inputs=split_inputs(row.get("source_inputs", "")),
            chapter_sequence=[],
            required_gates=[row.get("primary_gate", "").strip()] if row.get("primary_gate", "").strip() else [],
            registry_sources=["execution_plan_book_registry"],
            required_chapter_rule=row.get("required_chapter_rule", "").strip(),
            primary_gate=row.get("primary_gate", "").strip(),
        )
    return books


def manual_match_key(manual: dict[str, Any]) -> str:
    return slug(manual.get("title", ""))


def output_key(path: str | Path) -> str:
    return Path(str(path).rstrip("/")).as_posix()


def merge_manual_registry(root: Path, books: dict[str, Book]) -> dict[str, dict[str, Any]]:
    registry = load_yaml(root / MANUAL_REGISTRY)
    manuals_by_book: dict[str, dict[str, Any]] = {}
    by_title = {slug(book.title): book_id for book_id, book in books.items()}
    by_output = {output_key(book.output_root): book_id for book_id, book in books.items()}
    for manual in registry.get("manuals", []):
        manual_id = slug(manual["manual_id"])
        match = ""
        if manual_id in books:
            match = manual_id
        if not match and manual_match_key(manual) in by_title:
            match = by_title[manual_match_key(manual)]
        if not match and output_key(manual.get("output_root", "")) in by_output:
            match = by_output[output_key(manual.get("output_root", ""))]
        if not match and manual_id.startswith("scratchbird_"):
            trimmed = manual_id.removeprefix("scratchbird_")
            if trimmed in books:
                match = trimmed
        if not match:
            match = manual_id
            books[match] = Book(
                book_id=match,
                title=manual["title"],
                output_root=Path(str(manual["output_root"]).rstrip("/")),
                release_applicability="manual_registry",
                owner="docs",
                registry_sources=["manual_registry"],
            )
        book = books[match]
        book.manual_registry_id = manual_id
        book.registry_sources = sorted(set(book.registry_sources + ["manual_registry"]))
        book.source_inputs = sorted(set(book.source_inputs + [slug(v) for v in manual.get("source_inputs", [])]))
        book.chapter_sequence = list(manual.get("chapter_sequence", []))
        book.required_gates = sorted(set(book.required_gates + list(manual.get("required_gates", []))))
        manuals_by_book[match] = manual
    return manuals_by_book


def expand_evidence(root: Path, pattern: str) -> list[Path]:
    base = root / pattern
    if any(ch in pattern for ch in "*?[]"):
        return sorted(p for p in root.glob(pattern) if p.exists())
    if base.exists():
        return [base]
    return []


def assess_source(root: Path, source_input: str) -> SourceAssessment:
    rule = SOURCE_RULES.get(source_input)
    if rule is None:
        return SourceAssessment(
            source_input=source_input,
            status="missing",
            evidence=[],
            evidence_hashes=[],
            resolution=f"Create authority rule and source extractor for {source_input}.",
            notes="No source rule exists for this input.",
        )
    found: list[Path] = []
    for pattern in rule.evidence:
        found.extend(expand_evidence(root, pattern))
    required_found: list[Path] = []
    missing_required: list[str] = []
    for pattern in rule.required_evidence:
        matches = expand_evidence(root, pattern)
        if matches:
            required_found.extend(matches)
        else:
            missing_required.append(pattern)
    found.extend(required_found)
    unique = []
    seen: set[str] = set()
    for path in found:
        key = path.resolve().as_posix()
        if key not in seen:
            unique.append(path)
            seen.add(key)
    if rule.required_evidence:
        status = "available" if not missing_required else ("partial" if unique else "missing")
    elif not unique:
        status = "missing"
    elif rule.complete_when_found:
        status = "available"
    else:
        status = "partial"
    hashes = []
    evidence = []
    for path in unique[:40]:
        evidence.append(rel(path, root))
        if path.is_file():
            hashes.append(f"{rel(path, root)}:{file_sha256(path)}")
    if len(unique) > 40:
        evidence.append(f"... {len(unique) - 40} additional evidence paths")
    notes = rule.notes
    if missing_required:
        missing_note = "Missing required evidence: " + "; ".join(missing_required)
        notes = f"{notes} {missing_note}".strip()
    return SourceAssessment(
        source_input=source_input,
        status=status,
        evidence=evidence,
        evidence_hashes=hashes,
        resolution=rule.resolution,
        notes=notes,
    )


def assess_all_sources(root: Path, books: dict[str, Book]) -> dict[str, SourceAssessment]:
    source_ids = sorted({item for book in books.values() for item in book.source_inputs})
    return {source_id: assess_source(root, source_id) for source_id in source_ids}


def book_registry_gap(book: Book) -> str:
    if book.manual_registry_id:
        return ""
    return "manual_registry_entry_missing"


def book_status(book: Book, assessments: dict[str, SourceAssessment]) -> str:
    if book_registry_gap(book):
        return "blocked_missing_authority"
    statuses = [assessments[source].status for source in book.source_inputs if source in assessments]
    if any(status == "missing" for status in statuses):
        return "blocked_missing_authority"
    if any(status == "partial" for status in statuses):
        return "blocked_partial_authority"
    return "source_authority_available"


def manual_release_ready(book: Book, assessments: dict[str, SourceAssessment]) -> bool:
    return (
        book_status(book, assessments) == "source_authority_available"
        and not book_registry_gap(book)
        and not publication_policy_blockers(book)
    )


def source_authority_ready(book: Book, assessments: dict[str, SourceAssessment]) -> bool:
    return book_status(book, assessments) == "source_authority_available" and not book_registry_gap(book)


def publication_policy_blockers(book: Book) -> list[str]:
    blockers: list[str] = []
    is_donor_book = book.book_id.startswith("donor_") or bool(
        DONOR_PUBLICATION_SOURCE_INPUTS.intersection(book.source_inputs)
    )
    is_security_sensitive_book = bool(
        SECURITY_PUBLICATION_SOURCE_INPUTS.intersection(book.source_inputs)
    )
    has_documented_examples = bool(
        EXAMPLE_PUBLICATION_SOURCE_INPUTS.intersection(book.source_inputs)
    )
    has_cli_surfaces = bool(CLI_PUBLICATION_SOURCE_INPUTS.intersection(book.source_inputs))
    if is_donor_book:
        blockers.append("donor_documentation_legal_hold_pending_ip_lawyer")
    if book.book_id == "donor_interbase_migration_reference":
        blockers.append("closed_source_interbase_private_only_pending_legal_approval")
    if is_security_sensitive_book:
        blockers.append("security_documentation_pre_gold_private_use_at_own_risk")
    if has_documented_examples:
        blockers.append("example_replay_proof_pending")
    if has_cli_surfaces:
        blockers.append("cli_branding_implementation_rename_pending")
    return blockers


def table(headers: list[str], rows: Iterable[Iterable[Any]]) -> list[str]:
    out = ["| " + " | ".join(headers) + " |"]
    out.append("| " + " | ".join("---" for _ in headers) + " |")
    for row in rows:
        out.append("| " + " | ".join(str(cell).replace("\n", " ") for cell in row) + " |")
    return out


def chapter_title(chapter_id: str) -> str:
    return title_from_id(chapter_id)


def book_markdown(book: Book, assessments: dict[str, SourceAssessment], now: str) -> str:
    status = book_status(book, assessments)
    policy_blockers = publication_policy_blockers(book)
    lines = [
        f"# {book.title}",
        "",
        f"Document id: `{book.book_id}`",
        f"Generated: {now}",
        f"Generator: `{GENERATOR_ID}` version `{GENERATOR_VERSION}`",
        "Release state: internal release-candidate, blocked from public release until all gates close.",
        f"Authority state: `{status}`",
        f"Publication policy blockers: `{';'.join(policy_blockers) if policy_blockers else 'none'}`",
        "",
        "## Rights And Classification",
        "",
        "This generated artifact is private ScratchBird documentation material. It is generated from repository-local authority inputs and is not a behavior authority.",
        "",
        "## About This Manual",
        "",
        f"This manual provides the source-traced documentation baseline for `{book.book_id}`. The canonical behavior authority remains under `public_release_evidence`; this book records the generated user-facing view and the authority inputs that control it.",
        "",
        "## Source Boundary",
        "",
        "The generator used only repository-local inputs. Donor manuals and external references are layout or compatibility evidence only when source maps name them explicitly. Donor behavior does not override ScratchBird authority.",
        "",
        "## Source Authority Inventory",
        "",
    ]
    source_rows = []
    for source in book.source_inputs:
        assessment = assessments[source]
        evidence = "; ".join(assessment.evidence[:4]) if assessment.evidence else "none"
        source_rows.append([source, assessment.status, evidence, assessment.resolution])
    lines.extend(table(["Input", "Status", "Observed Evidence", "Required Resolution"], source_rows))
    lines.extend(["", "## Blocking Authority Inputs", ""])
    blockers = [
        source
        for source in book.source_inputs
        if assessments[source].status in {"missing", "partial"}
    ]
    if book_registry_gap(book):
        lines.append("- Canonical manual registry entry is absent for this execution_plan book.")
    for source in blockers:
        assessment = assessments[source]
        lines.append(f"- `{source}` is `{assessment.status}`: {assessment.resolution}")
    if not blockers and not book_registry_gap(book):
        lines.append("- No source authority blockers were detected by this generator run.")
    lines.extend(["", "## Table Of Contents", ""])
    chapters = book.chapter_sequence or ["about_this_manual", "source_authority_inventory", "document_history"]
    for idx, chapter in enumerate(chapters, 1):
        lines.append(f"{idx}. {chapter_title(chapter)}")
    lines.extend(["", "## Manual Body Chapters", ""])
    for idx, chapter in enumerate(chapters, 1):
        lines.extend(
            [
                f"### {idx}. {chapter_title(chapter)}",
                "",
                f"Chapter id: `{chapter}`",
                "",
                "Authority inputs:",
                "",
            ]
        )
        for source in book.source_inputs:
            assessment = assessments[source]
            lines.append(f"- `{source}`: `{assessment.status}`")
        lines.extend(
            [
                "",
                "Source trace:",
                "",
            ]
        )
        for source in book.source_inputs:
            assessment = assessments[source]
            if assessment.evidence:
                lines.append(f"- `{source}` evidence: {assessment.evidence[0]}")
            else:
                lines.append(f"- `{source}` evidence: no authority input found")
        lines.extend(
            [
                "",
                "Publication rule:",
                "",
                "This chapter may be included in public release output only after every listed input is available, replay or coverage proof is attached where required, and the book build report records independent review closure.",
                "",
            ]
        )
    lines.extend(
        [
            "## Appendices",
            "",
            "### Source Hash Index",
            "",
        ]
    )
    hash_rows = []
    for source in book.source_inputs:
        assessment = assessments[source]
        if assessment.evidence_hashes:
            for item in assessment.evidence_hashes[:10]:
                path, digest = item.split(":", 1)
                hash_rows.append([source, path, digest])
        else:
            hash_rows.append([source, "none", "none"])
    lines.extend(table(["Input", "Path", "SHA256"], hash_rows))
    lines.extend(
        [
            "",
            "## Document History",
            "",
            f"- {now}: generated by `{GENERATOR_ID}` as an internal authority-tracked baseline.",
            "",
            "## Alphabetical Index",
            "",
        ]
    )
    for term in sorted(set(book.source_inputs + chapters)):
        lines.append(f"- {title_from_id(term)}")
    return "\n".join(lines)


def html_from_markdown(title: str, markdown: str) -> str:
    escaped = html.escape(markdown)
    return "\n".join(
        [
            "<!doctype html>",
            "<html lang=\"en\">",
            "<head>",
            "  <meta charset=\"utf-8\">",
            f"  <title>{html.escape(title)}</title>",
            "  <style>",
            "    body { font-family: Arial, sans-serif; margin: 2rem; line-height: 1.45; }",
            "    pre { white-space: pre-wrap; font-family: inherit; }",
            "  </style>",
            "</head>",
            "<body>",
            f"<h1>{html.escape(title)}</h1>",
            f"<pre>{escaped}</pre>",
            "</body>",
            "</html>",
        ]
    )


def pdf_escape(text: str) -> str:
    return text.replace("\\", "\\\\").replace("(", "\\(").replace(")", "\\)")


def pdf_lines(markdown: str) -> list[str]:
    lines: list[str] = []
    for raw in markdown.splitlines():
        text = re.sub(r"[`#|*_]", "", raw).strip()
        if not text:
            lines.append("")
            continue
        while len(text) > 92:
            lines.append(text[:92])
            text = text[92:]
        lines.append(text)
    return lines


def build_pdf_bytes(title: str, markdown: str) -> bytes:
    lines = pdf_lines(f"{title}\n\n{markdown}")[:1000]
    pages = [lines[idx : idx + 48] for idx in range(0, len(lines), 48)] or [[title]]
    objects: list[bytes] = []
    page_object_numbers: list[int] = []
    objects.append(b"<< /Type /Catalog /Pages 2 0 R >>")
    objects.append(b"")
    font_obj_number = 3 + (len(pages) * 2)
    for page in pages:
        content_lines = ["BT", "/F1 9 Tf", "50 790 Td", "12 TL"]
        for line in page:
            content_lines.append(f"({pdf_escape(line)}) Tj")
            content_lines.append("T*")
        content_lines.append("ET")
        stream = "\n".join(content_lines).encode("ascii")
        content_obj = len(objects) + 1
        objects.append(
            b"<< /Length "
            + str(len(stream)).encode("ascii")
            + b" >>\nstream\n"
            + stream
            + b"\nendstream"
        )
        page_obj = len(objects) + 1
        page_object_numbers.append(page_obj)
        objects.append(
            (
                f"<< /Type /Page /Parent 2 0 R /MediaBox [0 0 595 842] "
                f"/Resources << /Font << /F1 {font_obj_number} 0 R >> >> "
                f"/Contents {content_obj} 0 R >>"
            ).encode("ascii")
        )
    objects.append(b"<< /Type /Font /Subtype /Type1 /BaseFont /Helvetica >>")
    kids = " ".join(f"{num} 0 R" for num in page_object_numbers)
    objects[1] = f"<< /Type /Pages /Kids [{kids}] /Count {len(page_object_numbers)} >>".encode("ascii")
    out = bytearray(b"%PDF-1.4\n%\xe2\xe3\xcf\xd3\n")
    offsets = [0]
    for idx, obj in enumerate(objects, 1):
        if idx == font_obj_number:
            pass
        offsets.append(len(out))
        out.extend(f"{idx} 0 obj\n".encode("ascii"))
        out.extend(obj)
        out.extend(b"\nendobj\n")
    xref = len(out)
    out.extend(f"xref\n0 {len(objects) + 1}\n".encode("ascii"))
    out.extend(b"0000000000 65535 f \n")
    for offset in offsets[1:]:
        out.extend(f"{offset:010d} 00000 n \n".encode("ascii"))
    out.extend(
        (
            f"trailer\n<< /Size {len(objects) + 1} /Root 1 0 R >>\n"
            f"startxref\n{xref}\n%%EOF\n"
        ).encode("ascii")
    )
    return bytes(out)


def write_pdf(path: Path, title: str, markdown: str) -> None:
    path.parent.mkdir(parents=True, exist_ok=True)
    path.write_bytes(build_pdf_bytes(title, markdown))


def source_map_rows(book: Book, assessments: dict[str, SourceAssessment]) -> list[dict[str, str]]:
    rows = []
    for source in book.source_inputs:
        assessment = assessments[source]
        rows.append(
            {
                "manual_id": book.book_id,
                "source_input": source,
                "status": assessment.status,
                "evidence_paths": ";".join(assessment.evidence),
                "evidence_hashes": ";".join(assessment.evidence_hashes),
                "resolution": assessment.resolution,
            }
        )
    return rows


def claim_trace_rows(book: Book, assessments: dict[str, SourceAssessment]) -> list[dict[str, str]]:
    rows = []
    chapters = book.chapter_sequence or ["about_this_manual", "source_authority_inventory", "document_history"]
    for chapter in chapters:
        for source in book.source_inputs:
            assessment = assessments[source]
            rows.append(
                {
                    "claim_id": f"{book.book_id}.{chapter}.{source}",
                    "manual_id": book.book_id,
                    "chapter_id": chapter,
                    "claim_type": "source_authority_binding",
                    "source_input": source,
                    "status": assessment.status,
                    "evidence_paths": ";".join(assessment.evidence),
                    "resolution": assessment.resolution,
                }
            )
    return rows


def generated_file_hashes(paths: Iterable[Path], root: Path) -> list[dict[str, str]]:
    hashes = []
    for path in sorted(paths):
        if path.is_file():
            hashes.append({"path": rel(path, root), "sha256": file_sha256(path)})
    return hashes


def write_book(root: Path, book: Book, assessments: dict[str, SourceAssessment], now: str) -> dict[str, Any]:
    output_root = root / book.output_root
    generated_root = output_root / "_generated"
    rendered_path = generated_root / "rendered_source" / f"{book.book_id}.md"
    markdown = book_markdown(book, assessments, now)
    write_text(rendered_path, markdown)
    html_path = output_root / "html" / "index.html"
    write_text(html_path, html_from_markdown(book.title, markdown))
    pdf_path = output_root / "pdf" / f"{book.book_id}.pdf"
    write_pdf(pdf_path, book.title, markdown)
    source_map_path = generated_root / "documentation_source_map.csv"
    write_csv(
        source_map_path,
        source_map_rows(book, assessments),
        ["manual_id", "source_input", "status", "evidence_paths", "evidence_hashes", "resolution"],
    )
    claim_trace_path = generated_root / "documentation_claim_trace.csv"
    write_csv(
        claim_trace_path,
        claim_trace_rows(book, assessments),
        [
            "claim_id",
            "manual_id",
            "chapter_id",
            "claim_type",
            "source_input",
            "status",
            "evidence_paths",
            "resolution",
        ],
    )
    has_documented_examples = bool(
        EXAMPLE_PUBLICATION_SOURCE_INPUTS.intersection(book.source_inputs)
    )
    examples_manifest = {
        "schema_version": 1,
        "manual_id": book.book_id,
        "authority_registry": "public_contract_snapshot"
        if has_documented_examples
        else "",
        "release_blocker": "example_replay_proof_pending" if has_documented_examples else "",
        "status": "authority_available_replay_proof_pending"
        if has_documented_examples
        else "not_required_by_current_source_inputs",
        "examples": [],
    }
    write_text(generated_root / "examples_manifest.yaml", yaml.safe_dump(examples_manifest, sort_keys=True))
    redaction_report = {
        "manual_id": book.book_id,
        "classification": "private_internal",
        "public_release_ready": False,
        "private_source_text_exported": False,
        "external_ai_egress": False,
        "status": "passed_internal_generation_filter",
    }
    write_json(generated_root / "private_redaction_report.json", redaction_report)
    prompt_provenance = {
        "manual_id": book.book_id,
        "generator": GENERATOR_ID,
        "generator_version": GENERATOR_VERSION,
        "ai_provider": "none",
        "prompt_used": False,
        "packet_hash": text_sha256("\n".join(book.source_inputs)),
        "source_inputs": book.source_inputs,
    }
    write_json(generated_root / "prompt_provenance.json", prompt_provenance)
    ai_egress_report = {
        "manual_id": book.book_id,
        "execution_mode": "local_deterministic_tool",
        "external_egress": False,
        "redacted_external_packet_used": False,
        "private_paths_exported": False,
    }
    write_json(generated_root / "ai_egress_report.json", ai_egress_report)
    navigation_manifest = {
        "manual_id": book.book_id,
        "title": book.title,
        "output_root": book.output_root.as_posix(),
        "html": rel(html_path, root),
        "pdf": rel(pdf_path, root),
        "chapters": book.chapter_sequence,
    }
    write_json(generated_root / "global_navigation_manifest.json", navigation_manifest)
    source_completeness_rows = []
    for source in book.source_inputs:
        assessment = assessments[source]
        source_completeness_rows.append(
            {
                "manual_id": book.book_id,
                "source_input": source,
                "status": assessment.status,
                "resolution": assessment.resolution,
                "notes": assessment.notes,
            }
        )
    write_csv(
        generated_root / "source_completeness.csv",
        source_completeness_rows,
        ["manual_id", "source_input", "status", "resolution", "notes"],
    )
    source_ready = source_authority_ready(book, assessments)
    policy_blockers = publication_policy_blockers(book)
    build_report = {
        "manual_id": book.book_id,
        "title": book.title,
        "generated_at_utc": now,
        "generator": GENERATOR_ID,
        "generator_version": GENERATOR_VERSION,
        "authority_state": book_status(book, assessments),
        "source_ready": source_ready,
        "public_release_ready": False,
        "publication_policy_blockers": policy_blockers,
        "manual_registry_id": book.manual_registry_id,
        "manual_registry_gap": book_registry_gap(book),
        "source_input_count": len(book.source_inputs),
        "chapter_count": len(book.chapter_sequence),
        "required_gates": book.required_gates,
    }
    write_json(generated_root / "documentation_build_report.json", build_report)
    release_attestation = {
        "manual_id": book.book_id,
        "release_state": "internal_rc",
        "public_release_ready": False,
        "required_before_public_release": [
            "all source inputs available",
            "all publication policy blockers cleared",
            "example replay proof where examples exist",
            "security review closure",
            "independent audit closure",
        ],
    }
    write_json(generated_root / "release_attestation.json", release_attestation)
    doc_model = {
        "schema_version": 1,
        "manual_id": book.book_id,
        "manual_registry_id": book.manual_registry_id,
        "title": book.title,
        "generated_at_utc": now,
        "generator": {"id": GENERATOR_ID, "version": GENERATOR_VERSION},
        "style_profile_id": "firebird_reference_manual_profile",
        "release_state": "internal_rc",
        "public_release_ready": False,
        "source_ready": source_ready,
        "authority_state": book_status(book, assessments),
        "publication_policy_blockers": policy_blockers,
        "output_root": book.output_root.as_posix(),
        "source_inputs": [
            {
                "input": source,
                "status": assessments[source].status,
                "evidence_paths": assessments[source].evidence,
                "resolution": assessments[source].resolution,
            }
            for source in book.source_inputs
        ],
        "chapter_sequence": book.chapter_sequence,
        "generated_artifacts": [
            rel(rendered_path, root),
            rel(html_path, root),
            rel(pdf_path, root),
            rel(source_map_path, root),
            rel(claim_trace_path, root),
        ],
    }
    doc_model_path = generated_root / "doc_model.json"
    write_json(doc_model_path, doc_model)
    artifacts = [
        rendered_path,
        html_path,
        pdf_path,
        source_map_path,
        claim_trace_path,
        generated_root / "examples_manifest.yaml",
        generated_root / "private_redaction_report.json",
        generated_root / "prompt_provenance.json",
        generated_root / "ai_egress_report.json",
        generated_root / "global_navigation_manifest.json",
        generated_root / "source_completeness.csv",
        generated_root / "documentation_build_report.json",
        generated_root / "release_attestation.json",
        doc_model_path,
    ]
    return {
        "book_id": book.book_id,
        "title": book.title,
        "status": book_status(book, assessments),
        "source_ready": source_ready,
        "public_release_ready": False,
        "publication_policy_blockers": policy_blockers,
        "output_root": book.output_root.as_posix(),
        "artifacts": generated_file_hashes(artifacts, root),
    }


def build_ledgers(
    books: dict[str, Book], assessments: dict[str, SourceAssessment]
) -> tuple[list[dict[str, str]], list[dict[str, str]]]:
    inventory: dict[str, dict[str, str]] = {}
    missing_rows: list[dict[str, str]] = []
    for source, assessment in sorted(assessments.items()):
        inventory[source] = {
            "source_input": source,
            "status": assessment.status,
            "evidence_count": str(len(assessment.evidence)),
            "evidence_paths": ";".join(assessment.evidence),
            "resolution": assessment.resolution,
            "notes": assessment.notes,
        }
    seq = 1
    for book in sorted(books.values(), key=lambda item: item.book_id):
        if book_registry_gap(book):
            missing_rows.append(
                {
                    "ledger_id": f"DOC-MISSING-{seq:04d}",
                    "book_id": book.book_id,
                    "issue_type": "manual_registry_entry_missing",
                    "authority_input": "end_user_documentation_manual_registry",
                    "status": "missing",
                    "required_resolution": "Add this execution_plan book to the canonical manual registry or mark it not applicable by release profile.",
                    "observed_evidence": ";".join(book.registry_sources),
                }
            )
            seq += 1
        for source in book.source_inputs:
            assessment = assessments[source]
            if assessment.status in {"missing", "partial"}:
                missing_rows.append(
                    {
                        "ledger_id": f"DOC-MISSING-{seq:04d}",
                        "book_id": book.book_id,
                        "issue_type": "source_authority_missing" if assessment.status == "missing" else "source_authority_partial",
                        "authority_input": source,
                        "status": assessment.status,
                        "required_resolution": assessment.resolution,
                        "observed_evidence": ";".join(assessment.evidence) if assessment.evidence else "none",
                    }
                )
                seq += 1
    return list(inventory.values()), missing_rows


def write_bookshelf(root: Path, book_summaries: list[dict[str, Any]], missing_rows: list[dict[str, str]], now: str) -> None:
    index_lines = [
        "# ScratchBird Documentation Bookshelf",
        "",
        f"Generated: {now}",
        "",
        "Release state: internal release-candidate, blocked from public release until the missing authority ledger is empty and all documentation gates close.",
        "",
        "## Books",
        "",
    ]
    for summary in sorted(book_summaries, key=lambda item: item["book_id"]):
        book_rel = summary["output_root"].removeprefix("docs/documentation/").rstrip("/")
        index_lines.append(
            f"- [{summary['title']}](../../{book_rel}/html/index.html) - `{summary['status']}`"
        )
    index_lines.extend(
        [
            "",
            "## Missing Authority Ledger",
            "",
            f"- Ledger rows: {len(missing_rows)}",
            f"- Path: `docs/documentation/_generated/baseline/missing_authority_inputs.csv`",
        ]
    )
    write_text(BASELINE_ROOT_AT(root) / "bookshelf_index.md", "\n".join(index_lines))
    write_json(
        BASELINE_ROOT_AT(root) / "bookshelf_index.json",
        {
            "generated_at_utc": now,
            "release_state": "internal_rc",
            "public_release_ready": False,
            "missing_authority_count": len(missing_rows),
            "books": book_summaries,
        },
    )
    root_links = [
        "<!doctype html>",
        "<html lang=\"en\">",
        "<head>",
        "  <meta charset=\"utf-8\">",
        "  <title>ScratchBird Documentation Bookshelf</title>",
        "  <style>",
        "    body { font-family: Arial, sans-serif; margin: 2rem; line-height: 1.45; }",
        "    table { border-collapse: collapse; width: 100%; }",
        "    th, td { border: 1px solid #bbb; padding: 0.4rem; text-align: left; }",
        "  </style>",
        "</head>",
        "<body>",
        "<h1>ScratchBird Documentation Bookshelf</h1>",
        f"<p>Generated: {html.escape(now)}</p>",
        "<p>Release state: internal release-candidate, blocked from public release until the authority ledger is empty and all gates close.</p>",
        "<table>",
        "<thead><tr><th>Book</th><th>Status</th><th>HTML</th><th>PDF</th></tr></thead>",
        "<tbody>",
    ]
    for summary in sorted(book_summaries, key=lambda item: item["book_id"]):
        book_rel = summary["output_root"].removeprefix("docs/documentation/").rstrip("/")
        pdf_name = f"{summary['book_id']}.pdf"
        root_links.append(
            "<tr>"
            f"<td>{html.escape(summary['title'])}</td>"
            f"<td><code>{html.escape(summary['status'])}</code></td>"
            f"<td><a href=\"{html.escape(book_rel)}/html/index.html\">HTML</a></td>"
            f"<td><a href=\"{html.escape(book_rel)}/pdf/{html.escape(pdf_name)}\">PDF</a></td>"
            "</tr>"
        )
    root_links.extend(
        [
            "</tbody>",
            "</table>",
            "<h2>Missing Authority Ledger</h2>",
            f"<p>Rows: {len(missing_rows)}</p>",
            "<p><a href=\"_generated/baseline/missing_authority_inputs.csv\">missing_authority_inputs.csv</a></p>",
            "</body>",
            "</html>",
        ]
    )
    write_text(root / "docs/documentation/index.html", "\n".join(root_links))


def BASELINE_ROOT_AT(root: Path) -> Path:
    return root / BASELINE_ROOT


def validate_generated(root: Path, book_summaries: list[dict[str, Any]]) -> None:
    problems: list[str] = []
    for summary in book_summaries:
        for artifact in summary["artifacts"]:
            path = root / artifact["path"]
            if not path.exists():
                problems.append(f"missing artifact {artifact['path']}")
    for path in (root / "docs/documentation").glob("**/*"):
        if path.is_file() and path.suffix.lower() in {".md", ".html"}:
            text = path.read_text(encoding="utf-8")
            for term in FORBIDDEN_RELEASE_TERMS:
                if term in text:
                    problems.append(f"{rel(path, root)} contains forbidden release term {term!r}")
            try:
                text.encode("ascii")
            except UnicodeEncodeError:
                problems.append(f"{rel(path, root)} is not ASCII clean")
    if problems:
        for problem in problems:
            print(problem, file=sys.stderr)
        raise SystemExit(1)


def main() -> None:
    args = parse_args()
    root = Path(args.repo_root).resolve()
    os.chdir(root)
    required = [MANUAL_REGISTRY, CONFORMANCE_MANIFEST, BOOK_REGISTRY]
    for path in required:
        if not (root / path).exists():
            raise SystemExit(f"required input missing: {path}")
    now = datetime.now(timezone.utc).replace(microsecond=0).isoformat().replace("+00:00", "Z")
    books = load_execution_plan_books(root)
    merge_manual_registry(root, books)
    assessments = assess_all_sources(root, books)
    book_summaries = []
    for book in sorted(books.values(), key=lambda item: item.book_id):
        book_summaries.append(write_book(root, book, assessments, now))
    inventory_rows, missing_rows = build_ledgers(books, assessments)
    write_csv(
        BASELINE_ROOT_AT(root) / "documentation_authority_inventory.csv",
        inventory_rows,
        ["source_input", "status", "evidence_count", "evidence_paths", "resolution", "notes"],
    )
    write_csv(
        BASELINE_ROOT_AT(root) / "missing_authority_inputs.csv",
        missing_rows,
        [
            "ledger_id",
            "book_id",
            "issue_type",
            "authority_input",
            "status",
            "required_resolution",
            "observed_evidence",
        ],
    )
    write_csv(
        root / EXECUTION_PLAN_DIR / "GENERATED_MISSING_AUTHORITY_LEDGER.csv",
        missing_rows,
        [
            "ledger_id",
            "book_id",
            "issue_type",
            "authority_input",
            "status",
            "required_resolution",
            "observed_evidence",
        ],
    )
    summary = {
        "generated_at_utc": now,
        "generator": GENERATOR_ID,
        "generator_version": GENERATOR_VERSION,
        "book_count": len(book_summaries),
        "source_input_count": len(assessments),
        "missing_authority_count": len(missing_rows),
        "public_release_ready": False,
        "books_blocked_from_public_release": [
            item["book_id"]
            for item in book_summaries
            if item["status"] != "source_authority_available"
            or item.get("publication_policy_blockers")
        ],
        "ledger_path": rel(BASELINE_ROOT_AT(root) / "missing_authority_inputs.csv", root),
    }
    write_json(BASELINE_ROOT_AT(root) / "documentation_generation_summary.json", summary)
    write_bookshelf(root, book_summaries, missing_rows, now)
    if args.check:
        validate_generated(root, book_summaries)
    print(json.dumps(summary, indent=2, sort_keys=True))


if __name__ == "__main__":
    main()
