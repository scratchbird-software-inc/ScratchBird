#!/usr/bin/env python3
# Copyright (c) 2026 ScratchBird Software Inc.
#
# This Source Code Form is subject to the terms of the Mozilla Public
# License, v. 2.0. If a copy of the MPL was not distributed with this
# file, You can obtain one at https://mozilla.org/MPL/2.0/.
#
# SPDX-License-Identifier: MPL-2.0

"""Generate FAILURE_TRIAGE_TAXONOMY.csv for the SBsql Surface-to-SBLR execution_plan.

Output:
  project/tests/sbsql_parser_worker/fixtures/surface_to_sblr/artifacts/FAILURE_TRIAGE_TAXONOMY.csv

Standard categories for every failure class the execution_plan recognizes:
parser, binder, sblr, server, engine, fixture, oracle, auth, mga,
catalog, driver, generation, cluster_profile, agent_stall. Each row
carries the owner lane that must triage the failure, the diagnostic code
prefixes that match the failure class, the required evidence to record
in FAILURE_INVENTORY.csv, the standard remediation action, the
escalation path, and a stable `failure_id_prefix` used by
sb_execution_plan_agent_controls.py and other gates to namespace inventory rows.

Architecture invariant compliance: read-only CSV generation; no
transaction model touched; no engine, parser worker, server, listener,
storage, or MGA file modified; no WAL surface introduced. MGA copy-on-write
remains the sole transaction recovery model.
"""

from __future__ import annotations

import argparse
import csv
import sys
from pathlib import Path


DEFAULT_ARTIFACT_ROOT = "project/tests/sbsql_parser_worker/fixtures/surface_to_sblr/artifacts"
OUTPUT_NAME = "FAILURE_TRIAGE_TAXONOMY.csv"


COLUMNS = [
    "failure_category",
    "owner_lane",
    "diagnostic_code_prefix",
    "required_evidence",
    "standard_action",
    "escalation_path",
    "failure_id_prefix",
    "notes",
]


ROWS: list[dict[str, str]] = [
    {
        "failure_category": "parser",
        "owner_lane": "parser_statement_worker",
        "diagnostic_code_prefix": "SBSQL.SYNTAX.*;SBSQL.LEXER.*;SBSQL.CST.*",
        "required_evidence": "source_input;tokens;cst_snapshot;ast_snapshot;surface_id;diagnostic_message_vector",
        "standard_action": "fix_row_identifiable_parser_path_or_emit_exact_refusal_with_canonical_message_vector",
        "escalation_path": "parser_statement_worker -> lowering_worker (if grammar/parser binding implicated) -> coordinator",
        "failure_id_prefix": "fail-parser",
        "notes": "Parser-side failures including CST/AST construction, surface id resolution, and lexer/binding mode switching.",
    },
    {
        "failure_category": "binder",
        "owner_lane": "parser_statement_worker",
        "diagnostic_code_prefix": "SBSQL.BINDING.*;CATALOG.NAME.NOT_FOUND_OR_NOT_VISIBLE;CATALOG.RESOLUTION.*",
        "required_evidence": "bound_ast_snapshot;uuid_set;descriptors;rights_required;transaction_effect;result_contract;diagnostic_contract",
        "standard_action": "fix_binder_metadata_or_uuid_resolver_or_descriptor_authority",
        "escalation_path": "parser_statement_worker -> catalog_bootstrap_upgrade_worker (if catalog UUID resolution implicated) -> coordinator",
        "failure_id_prefix": "fail-binder",
        "notes": "Bound-AST binding failures including UUID resolution, descriptor lookup, rights inference, and result-contract assignment.",
    },
    {
        "failure_category": "sblr",
        "owner_lane": "lowering_worker",
        "diagnostic_code_prefix": "SBLR.ENVELOPE.*;SBLR.OPCODE.*;SBLR.VERIFIER.*;SBLR.OPERATION.*",
        "required_evidence": "sblr_execution_envelope_snapshot;verifier_output;canonical_function_or_api_operation_id;canonical_container_bytes_md5",
        "standard_action": "fix_lowering_serializer_verifier_or_canonical_operation_binding_no_family_only_routing",
        "escalation_path": "lowering_worker -> engine_runtime_worker (if dispatch implicated) -> coordinator",
        "failure_id_prefix": "fail-sblr",
        "notes": "SBLR envelope/verifier/serialization/dispatch failures. Family-only routing is never acceptable; the canonical function or API operation id must resolve.",
    },
    {
        "failure_category": "server",
        "owner_lane": "server_udr_worker",
        "diagnostic_code_prefix": "SERVER.ADMISSION.*;SBPS.*;SERVER.RESOURCE.*;SERVER.SECURITY.*",
        "required_evidence": "admission_request_payload;admission_response;message_vector;auth_context;resource_budget_used",
        "standard_action": "fix_sbps_admission_route_resource_governance_transaction_handoff_or_result_bridge",
        "escalation_path": "server_udr_worker -> authenticated_route_worker (if auth implicated) -> engine_runtime_worker -> coordinator",
        "failure_id_prefix": "fail-server",
        "notes": "Server-side admission failures across SBPS, security authority enforcement, resource budgets, and parser-support UDR routing.",
    },
    {
        "failure_category": "engine",
        "owner_lane": "engine_runtime_worker",
        "diagnostic_code_prefix": "ENGINE.EXEC.*;ENGINE.API.*;SBLR.DISPATCH.*;ENGINE.STATE.*",
        "required_evidence": "engine_api_operation_dispatched;execution_result_envelope_snapshot;message_vector_set;mga_transaction_id",
        "standard_action": "fix_engine_behavior_or_emit_exact_refusal_with_canonical_diagnostic_no_sql_text_dispatch",
        "escalation_path": "engine_runtime_worker -> transaction_worker (if MGA implicated) -> coordinator",
        "failure_id_prefix": "fail-engine",
        "notes": "Engine-side execution failures including dispatch, result envelope emission, internal API behavior, and SBLR runtime semantics. Engine must never dispatch on SQL text or family-only keys.",
    },
    {
        "failure_category": "fixture",
        "owner_lane": "conformance_worker",
        "diagnostic_code_prefix": "FIXTURE.EXPECTED.*;FIXTURE.ACTUAL.*;FIXTURE.MISSING.*;FIXTURE.AUTHORING.*",
        "required_evidence": "fixture_path;ctest_label;expected_oracle_row;actual_run_output;diff",
        "standard_action": "fix_fixture_oracle_or_implementation_to_match_canonical_authority_no_implementation_derived_oracle",
        "escalation_path": "conformance_worker -> semantic_oracle_worker (if oracle implicated) -> implementation_owner_lane -> coordinator",
        "failure_id_prefix": "fail-fixture",
        "notes": "Fixture-level mismatches. Oracle rows must be canonical-authority-derived; fixture failures may reflect implementation drift, missing fixture authoring, or stale oracle values.",
    },
    {
        "failure_category": "oracle",
        "owner_lane": "semantic_oracle_worker",
        "diagnostic_code_prefix": "ORACLE.MISMATCH.*;ORACLE.PENDING.*;ORACLE.DRIFT.*;ORACLE.AUTHORITY_MISSING.*",
        "required_evidence": "oracle_row_authority_status;expected_descriptor_cast_null_error_volatility_determinism;canonical_spec_reference",
        "standard_action": "fix_oracle_authority_or_record_expected_value_with_canonical_spec_reference_no_implementation_derived_oracle",
        "escalation_path": "semantic_oracle_worker -> coordinator (for canonical authority extension or remove_by_spec_change decision)",
        "failure_id_prefix": "fail-oracle",
        "notes": "Oracle authority failures. Includes oracle_pending rows blocking P2 implementation and canonical-authority drift.",
    },
    {
        "failure_category": "auth",
        "owner_lane": "authenticated_route_worker",
        "diagnostic_code_prefix": "SECURITY.AUTHENTICATION.*;SECURITY.AUTHORIZATION.*;SECURITY.AUDIT.*;SECURITY.POLICY.*",
        "required_evidence": "credential_profile_used;auth_policy_applied;policy_decision;session_uuid;redaction_evidence;audit_event_emitted",
        "standard_action": "fix_engine_owned_auth_authz_redaction_or_route_fixture_no_bypass_routes",
        "escalation_path": "authenticated_route_worker -> server_udr_worker (if SBPS implicated) -> coordinator",
        "failure_id_prefix": "fail-auth",
        "notes": "Authentication/authorization failures. Engine remains the only auth/authz authority; parser/listener/driver layers cannot grant rights.",
    },
    {
        "failure_category": "mga",
        "owner_lane": "transaction_worker",
        "diagnostic_code_prefix": "MGA.VISIBILITY.*;MGA.TRANSACTION.*;MGA.RECOVERY.*;MGA.HORIZON.*;MGA.CONFLICT.*",
        "required_evidence": "transaction_uuid;snapshot_state;commit_or_rollback_decision;visibility_outcome;cleanup_horizon;recovery_classification",
        "standard_action": "fix_mga_behavior_under_copy_on_write_authority_no_wal_no_redo_log_recovery_path",
        "escalation_path": "transaction_worker -> engine_runtime_worker -> coordinator",
        "failure_id_prefix": "fail-mga",
        "notes": "MGA copy-on-write transaction/visibility/recovery failures. WAL/redo-log behavior is never an acceptable repair; the project's architecture invariant forbids WAL.",
    },
    {
        "failure_category": "catalog",
        "owner_lane": "catalog_bootstrap_upgrade_worker",
        "diagnostic_code_prefix": "CATALOG.LIFECYCLE.*;CATALOG.UUID.*;CATALOG.SCHEMA.*;CATALOG.DEPENDENCY.*;CATALOG.NAME.*",
        "required_evidence": "catalog_object_uuid;descriptor_state;name_resolver_state;bootstrap_or_upgrade_transaction_id;dependency_set",
        "standard_action": "fix_catalog_seed_resolver_projection_or_upgrade_path_preserving_uuid_identity",
        "escalation_path": "catalog_bootstrap_upgrade_worker -> transaction_worker (if MGA bootstrap implicated) -> coordinator",
        "failure_id_prefix": "fail-catalog",
        "notes": "Catalog UUID/descriptor/name resolution and bootstrap/upgrade failures. UUID identity must be preserved across all transitions.",
    },
    {
        "failure_category": "driver",
        "owner_lane": "driver_route_worker",
        "diagnostic_code_prefix": "DRIVER.PROTOCOL.*;DRIVER.TLS.*;DRIVER.METADATA.*;DRIVER.POOL.*;DRIVER.RECONNECT.*",
        "required_evidence": "driver_config;sbwp_tls_handshake_evidence;auth_request_response;rendered_result;reconnect_finality_evidence",
        "standard_action": "fix_driver_route_or_server_compatibility_preserving_reconnect_finality_and_no_hidden_replay",
        "escalation_path": "driver_route_worker -> server_udr_worker (if server-side compatibility implicated) -> coordinator",
        "failure_id_prefix": "fail-driver",
        "notes": "Driver/client route failures including SBWP/TLS, auth bootstrap, metadata, pooling, and reconnect finality.",
    },
    {
        "failure_category": "generation",
        "owner_lane": "repro_budget_worker",
        "diagnostic_code_prefix": "GEN.DETERMINISM.*;GEN.NETWORK.*;GEN.DRIFT.*;GEN.ROW_COUNT.*;GEN.INPUT_MISSING.*",
        "required_evidence": "input_checksums;generated_output_checksums;regeneration_command;no_network_evidence",
        "standard_action": "fix_generator_determinism_or_input_drift_or_remove_network_dependency",
        "escalation_path": "repro_budget_worker -> coverage_gate_worker -> coordinator",
        "failure_id_prefix": "fail-gen",
        "notes": "Generator/reproducibility failures. Generators must be deterministic and repo-local-only; non-determinism is a execution_plan blocker.",
    },
    {
        "failure_category": "cluster_profile",
        "owner_lane": "cluster_profile_worker",
        "diagnostic_code_prefix": "SBSQL.CLUSTER.AUTHORITY_REQUIRED;SBSQL.SURFACE.PARSER_PRIVATE_REFUSED;SBSQL.SURFACE.NOT_ADMITTED",
        "required_evidence": "public_fail_closed_diagnostic;private_profile_evidence_ref;route_proof_no_cluster_execution_path_in_public_build",
        "standard_action": "fix_public_cluster_refusal_or_private_profile_gate_no_cluster_details_leaked",
        "escalation_path": "cluster_profile_worker -> coordinator (private cluster authority lives outside the public execution_plan)",
        "failure_id_prefix": "fail-cluster",
        "notes": "Cluster-private public-fail-closed and private-profile gate failures. Public builds must never enter cluster execution paths.",
    },
    {
        "failure_category": "agent_stall",
        "owner_lane": "coordinator",
        "diagnostic_code_prefix": "AGENT.HEARTBEAT.STALE;AGENT.WRITE_SCOPE.CONFLICT;AGENT.STATE.UNKNOWN",
        "required_evidence": "agent_id;last_heartbeat_utc;assigned_slice;owned_write_scope;stall_duration_minutes",
        "standard_action": "mark_agent_stalled_record_failure_inventory_row_reassign_per_agent_heartbeat_recovery_plan",
        "escalation_path": "coordinator (replacement agent must not revert unrelated edits)",
        "failure_id_prefix": "stall",
        "notes": "Agent heartbeat staleness or write-scope conflict. Used by sb_execution_plan_agent_controls.py detect-stalls subcommand; failure_id_prefix `stall` matches existing inventory records.",
    },
]


def fail(message: str) -> None:
    print(message, file=sys.stderr)
    raise SystemExit(1)


def main() -> int:
    parser = argparse.ArgumentParser()
    parser.add_argument("--repo-root", required=True)
    parser.add_argument("--artifact-root", default=DEFAULT_ARTIFACT_ROOT)
    args = parser.parse_args()
    root = Path(args.repo_root)
    artifact_root = Path(args.artifact_root)
    if not artifact_root.is_absolute():
        artifact_root = root / artifact_root

    output_path = artifact_root / OUTPUT_NAME
    output_path.parent.mkdir(parents=True, exist_ok=True)
    with output_path.open("w", newline="", encoding="utf-8") as handle:
        writer = csv.DictWriter(handle, fieldnames=COLUMNS, lineterminator="\n")
        writer.writeheader()
        writer.writerows(ROWS)

    print(f"failure_triage_taxonomy=generated categories={len(ROWS)}")
    return 0


if __name__ == "__main__":
    raise SystemExit(main())
