#!/usr/bin/env python3
# Copyright (c) 2026 ScratchBird Software Inc.
#
# This Source Code Form is subject to the terms of the Mozilla Public
# License, v. 2.0. If a copy of the MPL was not distributed with this
# file, You can obtain one at https://mozilla.org/MPL/2.0/.
#
# SPDX-License-Identifier: MPL-2.0

"""CEIC-003 MGA authority boundary regression gate.

SEARCH_KEY: CEIC_003_MGA_AUTHORITY_BOUNDARY_GATE
"""

from __future__ import annotations

import argparse
import csv
import pathlib
import re
import sys
from dataclasses import dataclass


EXECUTION_PLAN = pathlib.Path(
    "project/tests/release_evidence/consolidated_enterprise_public_evidence"
)


@dataclass(frozen=True)
class TextRequirement:
    label: str
    path: pathlib.Path
    required: tuple[str, ...]


SPEC_REQUIREMENTS = (
    TextRequirement(
        "canonical MGA/WAL authority",
        pathlib.Path("public_contract_snapshot"),
        (
            "ScratchBird MGA is the controlling transaction, visibility, versioning, recovery, cleanup, retention, archive, and cluster reconciliation model.",
            "Authoritative write-ahead-log recovery MUST NOT be inferred from any file.",
        ),
    ),
    TextRequirement(
        "single-node finality decision",
        pathlib.Path("public_contract_snapshot"),
        (
            "The durable transaction inventory page family is the authoritative single-node transaction finality structure.",
            "Parser state, reference syntax, CRUD text events, wall-clock order, UUID order, and file timestamps are forbidden as transaction finality authority.",
            "WAL/redo/undo is not authoritative recovery for ScratchBird Alpha.",
        ),
    ),
    TextRequirement(
        "MGA transaction model",
        pathlib.Path("public_contract_snapshot"),
        (
            "No implementation may replace this chain with PostgreSQL-style WAL authority, InnoDB-style redo/undo authority, latest-state replication authority, parser SQL authority, or optional write-after delta authority.",
            "Reference terms such as WAL, binlog, redo, undo, flashback, temporal history, logical replication, group replication, or certification are compatibility surfaces.",
        ),
    ),
    TextRequirement(
        "inventory visibility structures",
        pathlib.Path("public_contract_snapshot"),
        (
            "ScratchBird transaction visibility is determined from MGA transaction inventory, transaction snapshots, record-version metadata, page/filespace state, and policy state.",
            "It MUST NOT depend on an authoritative write-ahead log as the source of truth for transaction visibility or recovery.",
            "The in-memory table MUST be a cache of durable transaction inventory state, not a replacement authority.",
        ),
    ),
)

EXECUTION_PLAN_REQUIREMENTS = (
    TextRequirement(
        "execution_plan authority rules",
        EXECUTION_PLAN / "README.md",
        (
            "MGA transaction inventory remains transaction finality and visibility",
            "Memory evidence, metrics, support bundles, benchmarks, indexes, optimizer",
            "authorization, parser, reference, WAL, or recovery authority",
            "Reference engines may provide comparison artifacts only.",
            "WAL must not be introduced as Alpha recovery or transaction finality",
        ),
    ),
    TextRequirement(
        "cross-subsystem interface contracts",
        EXECUTION_PLAN / "INTERFACE_CONTRACTS.md",
        (
            "It must not use memory feedback as transaction",
            "Provider booleans are not accepted as final authority.",
            "They may not become optimizer plan authority, index generation",
            "Support bundles may collect memory, index, optimizer, and agent evidence only as",
            "authoritative transaction finality claims",
        ),
    ),
)

SOURCE_REQUIREMENTS = (
    TextRequirement(
        "MGA inventory implementation anchor",
        pathlib.Path("project/src/transaction/mga/transaction_inventory.hpp"),
        (
            "struct LocalTransactionInventory",
            "CommitLocalTransaction",
            "RollbackLocalTransaction",
            "CompactLocalTransactionInventory",
        ),
    ),
    TextRequirement(
        "MGA manager implementation anchor",
        pathlib.Path("project/src/transaction/mga/transaction_manager.hpp"),
        (
            "class LocalTransactionManager",
            "LocalTransactionManagerResult Commit",
            "LocalTransactionManagerResult Rollback",
            "TransactionSnapshotResult Snapshot",
        ),
    ),
    TextRequirement(
        "memory accounting non-authority",
        pathlib.Path("project/src/core/memory/sharded_memory_accounting_ledger.cpp"),
        (
            "memory_accounting_event_only_not_transaction_finality_visibility_recovery_parser_reference_benchmark_or_support_bundle_authority",
        ),
    ),
    TextRequirement(
        "memory support-bundle non-authority",
        pathlib.Path("project/src/core/memory/memory_support_bundle.cpp"),
        (
            "memory_support_bundle.authority_scope=evidence_only_not_transaction_finality_visibility_security_recovery_parser_reference_wal_or_benchmark_authority",
        ),
    ),
    TextRequirement(
        "metrics evidence non-authority",
        pathlib.Path("project/src/transaction/mga/optimizer_mga_pressure_metrics.cpp"),
        (
            "metric_visibility_or_finality_authority",
            "metric_recovery_authority",
            "benchmark_authority",
            "optimizer.mga_pressure.advisory_only=true",
            "optimizer.mga_pressure.finality_authority=false",
            "optimizer.mga_pressure.visibility_authority=false",
            "optimizer.mga_pressure.recovery_authority=false",
        ),
    ),
    TextRequirement(
        "candidate indexes require engine rechecks",
        pathlib.Path("project/src/core/index/candidate_set.cpp"),
        (
            "exact_recheck.required=true",
            "mga_visibility_recheck.required=true",
            "security_authorization_recheck.required=true",
            "candidate_set_finality_authority=false",
            "parser_or_reference_authority=false",
            "wal_recovery_or_finality_authority=false",
        ),
    ),
    TextRequirement(
        "index recheck policy preserves visibility authority",
        pathlib.Path("project/src/core/index/index_recheck.cpp"),
        (
            "policy.require_mga_visibility",
            "policy.require_predicate_match",
            "policy.require_security_visibility",
            "candidate.mga_visible",
            "candidate.security_visible",
        ),
    ),
    TextRequirement(
        "benchmark evidence is observational only",
        pathlib.Path("project/src/core/index/index_family_benchmark_gate.cpp"),
        (
            "benchmark_clean_admissible",
            "observational_only",
            "parser_authority",
            "reference_authority",
            "transaction_finality_authority",
            "visibility_authority",
            "recovery_authority",
        ),
    ),
    TextRequirement(
        "optimizer runtime payloads are candidate-only",
        pathlib.Path("project/src/engine/optimizer/runtime_filter_pushdown.cpp"),
        (
            "runtime_filter.candidate_rows_only=true",
            "runtime_filter.exact_recheck_required=true",
            "runtime_filter.mga_visibility_recheck_required=true",
            "runtime_filter.security_recheck_required=true",
            "parser_or_reference_finality_or_visibility_authority=false",
            "write_ahead_log_finality_or_visibility_authority=false",
        ),
    ),
    TextRequirement(
        "optimizer runtime feedback is advisory",
        pathlib.Path("project/src/engine/optimizer/optimizer_feedback.cpp"),
        (
            "runtime_feedback_persistence.authority_scope=advisory_only_not_transaction_finality_visibility_security_recovery_parser_or_reference_authority",
            "runtime_feedback_persistence.invalidatable=true",
        ),
    ),
    TextRequirement(
        "optimizer support bundle non-authority",
        pathlib.Path("project/src/engine/internal_api/observability/optimizer_metric_support_bundle.cpp"),
        (
            "optimizer.metric_bundle.advisory_only=true",
            "optimizer.metric_bundle.finality_authority=false",
            "optimizer.metric_bundle.visibility_authority=false",
            "optimizer.metric_bundle.recovery_authority=false",
            "optimizer.metric_bundle.wal_redo_authority=false",
        ),
    ),
    TextRequirement(
        "agent recommendation non-authority",
        pathlib.Path("project/src/core/agents/agent_optimizer_recommendation.cpp"),
        (
            "input.transaction_authority",
            "input.visibility_authority",
            "input.finality_authority",
            "input.recovery_authority",
            "agent_optimizer_recommendation.visibility_authority=false",
            "agent_optimizer_recommendation.finality_authority=false",
            "agent recommendations cannot provide engine authority",
        ),
    ),
    TextRequirement(
        "parser manager cannot claim finality",
        pathlib.Path("project/src/core/agents/agents/parser_interface_manager.cpp"),
        (
            "parser_execution_authority",
            "parser_finality_authority",
            "parser manager cannot accept parser execution/finality authority",
        ),
    ),
    TextRequirement(
        "reference-emulated indexes are mapping only",
        pathlib.Path("project/src/core/index/reference_emulated_index_mapping.cpp"),
        (
            "reference_emulated.candidate_only=true",
            "reference_emulated.final_rows_authorized=false",
            "reference_emulated.parser_authority=false",
            "reference_emulated.reference_authority=false",
            "reference_emulated.visibility_authority=false",
            "reference_emulated.transaction_finality_authority=false",
            "reference_emulated.log_finality_authority=false",
            "reference_emulated.mga_visibility_recheck.required=true",
        ),
    ),
    TextRequirement(
        "dirty manifest rejects WAL authority",
        pathlib.Path("project/src/storage/database/database_dirty_manifest.cpp"),
        (
            "RECOVERY.MANIFEST_WAL_CONFUSION_FORBIDDEN",
            "dirty manifest is classification evidence only and is never redo authority",
            "dirty manifest must not contain WAL, LSN, or write-ahead redo authority",
        ),
    ),
    TextRequirement(
        "transaction restore refuses WAL authority",
        pathlib.Path("project/src/transaction/mga/transaction_evidence.cpp"),
        (
            "SB-MGA-WAL-NOT-AUTHORITY",
            "transaction.evidence.wal_not_authority",
            "restore classification uses MGA inventory and lineage evidence",
        ),
    ),
)

SELECTED_SOURCE_SCAN_PATHS = tuple(requirement.path for requirement in SOURCE_REQUIREMENTS[2:])

FORBIDDEN_SUBJECTS = (
    "memory",
    "metric",
    "metrics",
    "index",
    "optimizer",
    "payload",
    "support bundle",
    "support_bundle",
    "agent",
    "benchmark",
    "parser",
    "reference",
    "wal",
    "write_ahead_log",
    "write-ahead log",
    "provider",
    "client",
    "cache",
    "descriptor",
    "feedback",
    "candidate_set",
    "runtime_filter",
)
AUTHORITY_WORDS = ("finality", "visibility", "recovery")
CLAIM_WORDS = ("authority", "authoritative", "source of truth", "owns", "own ")
ALLOWED_NEGATORS = (
    "=false",
    " false",
    '"false"',
    "not ",
    "cannot",
    "must not",
    "do_not_use",
    "_not_",
    "evidence_only",
    "advisory_only",
    "observational_only",
    "candidate_only",
    "refus",
    "forbidden",
    "required",
    "if (",
    "||",
    "&&",
    "!=",
)


def read_text(repo_root: pathlib.Path, rel: pathlib.Path) -> str:
    path = repo_root / rel
    if not path.exists():
        raise FileNotFoundError(str(rel))
    return path.read_text(encoding="utf-8")


def compact(text: str) -> str:
    return " ".join(text.split())


def validate_text_requirements(
    repo_root: pathlib.Path,
    requirements: tuple[TextRequirement, ...],
) -> list[str]:
    errors: list[str] = []
    for requirement in requirements:
        try:
            text = read_text(repo_root, requirement.path)
        except FileNotFoundError as exc:
            errors.append(f"{requirement.label}: missing {exc}")
            continue
        haystack = compact(text)
        for phrase in requirement.required:
            if compact(phrase) not in haystack:
                errors.append(f"{requirement.label}: {requirement.path} missing phrase: {phrase}")
    return errors


def validate_claim_boundary_matrix(repo_root: pathlib.Path) -> list[str]:
    path = repo_root / EXECUTION_PLAN / "CLAIM_BOUNDARY_MATRIX.csv"
    required = {
        "memory": ("not transaction finality visibility", "recovery authority"),
        "metrics": ("do not authorize rows commits recovery or security",),
        "indexes": ("exact mga security recheck",),
        "optimizer": ("cannot become row visibility transaction finality reference or parser authority",),
        "agents": ("cannot become transaction recovery security optimizer plan or index finality authority",),
        "support_bundles": ("observability artifacts only",),
        "reference_comparisons": ("comparison only",),
    }
    errors: list[str] = []
    with path.open(newline="", encoding="utf-8") as handle:
        rows = {row["claim_surface"]: row for row in csv.DictReader(handle)}
    for surface, phrases in required.items():
        row = rows.get(surface)
        if row is None:
            errors.append(f"CLAIM_BOUNDARY_MATRIX.csv missing {surface}")
            continue
        text = compact(" ".join(row.values()).lower())
        for phrase in phrases:
            if phrase not in text:
                errors.append(f"CLAIM_BOUNDARY_MATRIX.csv {surface} row missing: {phrase}")
    return errors


def forbidden_claim(line: str) -> bool:
    lowered = line.lower()
    if "authority=false" in lowered:
        return False
    if not any(subject in lowered for subject in FORBIDDEN_SUBJECTS):
        return False
    if not any(word in lowered for word in AUTHORITY_WORDS):
        return False
    if not any(word in lowered for word in CLAIM_WORDS):
        return False
    if any(negator in lowered for negator in ALLOWED_NEGATORS):
        return False
    if re.search(r"\bauthority\s*=\s*true\b", lowered):
        return True
    if "source of truth" in lowered or "authoritative" in lowered:
        return True
    if re.search(r"\bowns?\b", lowered):
        return True
    if re.search(r"\bis\b.*\bauthority\b", lowered):
        return True
    if re.search(r"\b[a-z_]+\.[a-z_]*authority[a-z_]*\)\s*\{?$", lowered):
        return False
    return True


def validate_forbidden_claim_scanner(repo_root: pathlib.Path) -> list[str]:
    errors: list[str] = []
    bad_fixtures = (
        "optimizer payload finality authority=true",
        "reference comparison is visibility authority",
        "support_bundle recovery authority source of truth",
        "memory metric owns transaction finality authority",
        "parser WAL visibility authoritative",
        "metric feedback replaces MGA transaction finality authority",
        "support bundle is the only recovery authority",
    )
    good_fixtures = (
        "optimizer payload finality_authority=false",
        "support bundle is observability only and not transaction finality authority",
        "benchmark evidence is advisory_only_not_transaction_finality_visibility_authority",
        "MGA transaction inventory remains transaction finality authority",
        "candidate_set requires mga_visibility_recheck.required=true",
    )
    for fixture in bad_fixtures:
        if not forbidden_claim(fixture):
            errors.append(f"forbidden-claim detector failed to reject fixture: {fixture}")
    for fixture in good_fixtures:
        if forbidden_claim(fixture):
            errors.append(f"forbidden-claim detector rejected allowed fixture: {fixture}")

    for rel in SELECTED_SOURCE_SCAN_PATHS:
        text = read_text(repo_root, rel)
        for line_no, line in enumerate(text.splitlines(), start=1):
            if forbidden_claim(line):
                errors.append(f"{rel}:{line_no} appears to grant non-MGA authority: {line.strip()}")
    return errors


def run(repo_root: pathlib.Path) -> list[str]:
    errors: list[str] = []
    errors.extend(validate_text_requirements(repo_root, SPEC_REQUIREMENTS))
    errors.extend(validate_text_requirements(repo_root, EXECUTION_PLAN_REQUIREMENTS))
    errors.extend(validate_text_requirements(repo_root, SOURCE_REQUIREMENTS))
    errors.extend(validate_claim_boundary_matrix(repo_root))
    errors.extend(validate_forbidden_claim_scanner(repo_root))
    return errors


def parse_args(argv: list[str]) -> argparse.Namespace:
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument(
        "--repo-root",
        type=pathlib.Path,
        default=pathlib.Path.cwd(),
        help="ScratchBird" "-Private repository root",
    )
    return parser.parse_args(argv)


def main(argv: list[str]) -> int:
    args = parse_args(argv)
    errors = run(args.repo_root.resolve())
    if errors:
        for error in errors:
            print(f"ceic_003_mga_authority_boundary_gate=fail:{error}", file=sys.stderr)
        return 1
    print("ceic_003_mga_authority_boundary_gate=pass")
    return 0


if __name__ == "__main__":
    raise SystemExit(main(sys.argv[1:]))
