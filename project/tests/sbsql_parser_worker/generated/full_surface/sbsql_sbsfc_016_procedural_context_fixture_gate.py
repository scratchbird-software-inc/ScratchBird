#!/usr/bin/env python3
# Copyright (c) 2026 ScratchBird Software Inc.
#
# This Source Code Form is subject to the terms of the Mozilla Public
# License, v. 2.0. If a copy of the MPL was not distributed with this
# file, You can obtain one at https://mozilla.org/MPL/2.0/.
#
# SPDX-License-Identifier: MPL-2.0

"""SBSFC-016 procedural context scalar fixture gate.

Validates the deterministic context-backed and fixed-policy scalar slice. This
gate is static: it reads the fixture CSV, canonical builtin registry, seed
registry, and dispatch source. It does not execute SQL text, use reference backends,
touch server cursor state, or read cluster/storage/recovery state.
"""

from __future__ import annotations

import argparse
import csv
import json
import re
import sys
from pathlib import Path
from typing import Any


SURFACE_REGISTRY = "public_input_snapshot"
BUILTIN_EXPRESSION_REGISTRY = "public_contract_snapshot"
SEED_REGISTRY = "project/src/engine/functions/registry/function_seed_registry.cpp"
DISPATCH_SOURCE = "project/src/engine/functions/families/data_scalar_functions_06_system_session_catalog.inc"
FIXTURES = "project/tests/sbsql_parser_worker/generated/full_surface/SBSFC_016_PROCEDURAL_CONTEXT_FIXTURES.csv"

REQUIRED_COLUMNS = [
    "fixture_id",
    "surface_id",
    "function_id",
    "canonical_builtin_id",
    "case_kind",
    "evaluation_mode",
    "arguments_json",
    "expected_result_value",
    "expected_result_descriptor",
    "expected_diagnostic_code",
    "oracle_authority_ref",
    "notes",
]

EXPECTED_CASES = {
    "SBSFC016-session-id-context": ("SBSQL-9A39831606E7", "sb.session.session_id", "019e1600-0000-7000-8000-000000000016", "uuid", ""),
    "SBSFC016-current-session-id-alias": ("SBSQL-33F4A211B147", "sb.session.session_id", "019e1600-0000-7000-8000-000000000016", "uuid", ""),
    "SBSFC016-current-session-uuid-alias": ("SBSQL-E95EA6CDDBE9", "sb.session.session_id", "019e1600-0000-7000-8000-000000000016", "uuid", ""),
    "SBSFC016-transaction-id-context": ("SBSQL-D197325B8209", "sb.session.transaction_id", "16016", "uint64", ""),
    "SBSFC016-current-transaction-id-alias": ("SBSQL-34E0B31299B8", "sb.session.transaction_id", "16016", "uint64", ""),
    "SBSFC016-transaction-uuid-context": ("SBSQL-F64197E1A6B0", "sb.session.transaction_uuid", "019e1600-0000-7000-8000-0000000000aa", "uuid", ""),
    "SBSFC016-current-statement-uuid-context": ("SBSQL-EB9DE727DEA0", "sb.session.current_statement_uuid", "019e1600-0000-7000-8000-0000000000bb", "uuid", ""),
    "SBSFC016-row-count-context": ("SBSQL-CCE5CAC9861B", "sb.fn.diagnostic.row_count", "7", "uint64", ""),
    "SBSFC016-session-user-context": ("SBSQL-235DF6445F0F", "sb.session.session_user", "019e1600-0000-7000-8000-000000000001", "uuid", ""),
    "SBSFC016-system-user-context": ("SBSQL-30C0DA11E783", "sb.session.system_user", "019e1600-0000-7000-8000-000000000001", "uuid", ""),
    "SBSFC016-user-context": ("SBSQL-F8F43A1BBC67", "sb.session.user", "019e1600-0000-7000-8000-000000000001", "uuid", ""),
    "SBSFC016-current-server-context": ("SBSQL-004C26692ACD", "sb.session.current_server", "019e1600-0000-7000-8000-0000000000dd", "uuid", ""),
    "SBSFC016-server-version-fixed-policy": ("SBSQL-5708DF455B5D", "sb.scalar.server_version", "ScratchBird 0.1.0", "character", ""),
    "SBSFC016-server-version-num-fixed-policy": ("SBSQL-A898D257AD44", "sb.scalar.server_version_num", "100", "uint64", ""),
    "SBSFC016-current-setting-timezone-fixed-policy": ("SBSQL-D1967545874D", "sb.scalar.current_setting_timezone", "UTC", "character", ""),
    "SBSFC016-current-timezone-alias": ("SBSQL-B8F6BB0CDCFA", "sb.scalar.current_setting_timezone", "UTC", "character", ""),
    "SBSFC016-current-setting-bare-known-timezone": ("SBSQL-208B00F367F7", "sb.scalar.current_setting", "UTC", "character", ""),
    "SBSFC016-current-setting-known-timezone": ("SBSQL-F38121AC8F2C", "sb.scalar.current_setting", "UTC", "character", ""),
    "SBSFC016-current-setting-unknown-refusal": ("SBSQL-F38121AC8F2C", "sb.scalar.current_setting", "", "character", "SB_DIAG_FUNCTION_INVALID_INPUT"),
    "SBSFC016-current-setting-var-exact-refusal": ("SBSQL-4D5E8F8A2B64", "sb.scalar.current_setting", "", "character", "SB_DIAG_FUNCTION_INVALID_INPUT"),
    "SBSFC016-current-setting-autocommit-exact-refusal": ("SBSQL-52B83E6B1FF1", "sb.scalar.current_setting", "", "character", "SB_DIAG_FUNCTION_INVALID_INPUT"),
    "SBSFC016-current-setting-unknown-missing-ok-null": ("SBSQL-F38121AC8F2C", "sb.scalar.current_setting", "", "character", ""),
    "SBSFC016-array-max-dimension": ("SBSQL-F824C47A36A5", "sb.scalar.array_max_dimension", "6", "uint64", ""),
    "SBSFC016-array-max-element-count": ("SBSQL-BCAC432F4C75", "sb.scalar.array_max_element_count", "1048576", "uint64", ""),
    "SBSFC016-case-when-max-branches": ("SBSQL-6C698B54A7CB", "sb.scalar.case_when_max_branches", "1024", "uint64", ""),
    "SBSFC016-cte-max-count-per-statement": ("SBSQL-8EAA8898DBEB", "sb.scalar.cte_max_count_per_statement", "1024", "uint64", ""),
    "SBSFC016-nested-subquery-max-depth": ("SBSQL-2CF7F4318343", "sb.scalar.nested_subquery_max_depth", "256", "uint64", ""),
    "SBSFC016-recursive-cte-max-depth": ("SBSQL-8A442BFCB429", "sb.scalar.recursive_cte_max_depth", "1024", "uint64", ""),
    "SBSFC016-result-set-max-columns": ("SBSQL-189B5E58953A", "sb.scalar.result_set_max_columns", "4096", "uint64", ""),
    "SBSFC016-union-max-arms": ("SBSQL-7998B79486A5", "sb.scalar.union_max_arms", "1024", "uint64", ""),
    "SBSFC016-numeric-division-by-zero": ("SBSQL-0A96CD6009B0", "sb.scalar.numeric_division_by_zero", "error", "character", ""),
    "SBSFC016-localized-label-max-length-bytes": ("SBSQL-1076274FA39A", "sb.scalar.localized_label_max_length_bytes", "1024", "uint64", ""),
    "SBSFC016-default-schema-resolution": ("SBSQL-1DD19CC14460", "sb.scalar.default_schema_resolution", "session_search_path_ambiguous_context_refusal", "character", ""),
    "SBSFC016-result-set-max-rows-in-response": ("SBSQL-38F7E6CE50C4", "sb.scalar.result_set_max_rows_in_response", "0", "uint64", ""),
    "SBSFC016-identifier-max-length-bytes": ("SBSQL-3B0F02549442", "sb.scalar.identifier_max_length_bytes", "255", "uint64", ""),
    "SBSFC016-statement-timeout": ("SBSQL-566F25C6A1DC", "sb.scalar.statement_timeout", "0", "uint64", ""),
    "SBSFC016-statement-timeout-default": ("SBSQL-63F49DEC1864", "sb.scalar.statement_timeout_default", "0", "uint64", ""),
    "SBSFC016-statement-timeout-ms": ("SBSQL-48F297900CCF", "sb.scalar.statement_timeout_ms", "0", "uint64", ""),
    "SBSFC016-client-min-messages-default": ("SBSQL-61AA83D5AF3B", "sb.scalar.client_min_messages_default", "NOTICE", "character", ""),
    "SBSFC016-numeric-overflow-behavior": ("SBSQL-62293C2E8E6C", "sb.scalar.numeric_overflow_behavior", "error", "character", ""),
    "SBSFC016-null-ordering-default-for-asc": ("SBSQL-6A71D5F290FE", "sb.scalar.null_ordering_default_for_asc", "NULLS LAST", "character", ""),
    "SBSFC016-statement-max-length-bytes": ("SBSQL-6DFFFC9C7143", "sb.scalar.statement_max_length_bytes", "33554432", "uint64", ""),
    "SBSFC016-null-concat-returns-null": ("SBSQL-76AE668F5B32", "sb.scalar.null_concat_returns_null", "1", "boolean", ""),
    "SBSFC016-recursion-max-depth": ("SBSQL-885A9826C368", "sb.scalar.recursion_max_depth", "1024", "uint64", ""),
    "SBSFC016-parameter-marker-max-count": ("SBSQL-8F4843478BD9", "sb.scalar.parameter_marker_max_count", "262144", "uint64", ""),
    "SBSFC016-delimited-identifier-max-length-bytes": ("SBSQL-94568295C808", "sb.scalar.delimited_identifier_max_length_bytes", "255", "uint64", ""),
    "SBSFC016-case-resolution-for-quoted-identifiers": ("SBSQL-A0D8F5B44E98", "sb.scalar.case_resolution_for_quoted_identifiers", "exact_spelling_match_not_identity", "character", ""),
    "SBSFC016-temporal-default-precision": ("SBSQL-A727EFD5293F", "sb.scalar.temporal_default_precision", "6", "uint64", ""),
    "SBSFC016-string-truncation-behavior": ("SBSQL-AB0BCEEA8CAD", "sb.scalar.string_truncation_behavior", "error", "character", ""),
    "SBSFC016-timezone-resolution": ("SBSQL-ADF52458FA79", "sb.scalar.timezone_resolution", "session_timezone_explicit_zone_conversion", "character", ""),
    "SBSFC016-null-in-aggregate-skipped": ("SBSQL-B2ACB81B5943", "sb.scalar.null_in_aggregate_skipped", "1", "boolean", ""),
    "SBSFC016-null-ordering-default-for-desc": ("SBSQL-C8724AE0B69E", "sb.scalar.null_ordering_default_for_desc", "NULLS FIRST", "character", ""),
    "SBSFC016-interval-default-precision": ("SBSQL-C9B937A0E6A4", "sb.scalar.interval_default_precision", "qualifier_driven", "character", ""),
    "SBSFC016-name-resolution": ("SBSQL-CB465EA2790A", "sb.scalar.name_resolution", "uuid_bound_at_bind_time", "character", ""),
    "SBSFC016-recursive-schema-path-separator": ("SBSQL-D231D788EACF", "sb.scalar.recursive_schema_path_separator", ".", "character", ""),
    "SBSFC016-identifier-max-length-chars": ("SBSQL-DBEE94C525A5", "sb.scalar.identifier_max_length_chars", "63", "uint64", ""),
    "SBSFC016-lock-timeout": ("SBSQL-04636D3CDEF5", "sb.scalar.lock_timeout", "0", "uint64", ""),
    "SBSFC016-lock-timeout-default": ("SBSQL-0080B3C2968B", "sb.scalar.lock_timeout_default", "0", "uint64", ""),
    "SBSFC016-lock-timeout-ms": ("SBSQL-E6D97E527535", "sb.scalar.lock_timeout_ms", "0", "uint64", ""),
    "SBSFC016-idle-in-transaction-session-timeout": ("SBSQL-678A3FA8960F", "sb.scalar.idle_in_transaction_session_timeout", "0", "uint64", ""),
    "SBSFC016-idle-in-transaction-timeout-default": ("SBSQL-EF6F8A3F935F", "sb.scalar.idle_in_transaction_timeout_default", "0", "uint64", ""),
    "SBSFC016-idle-in-transaction-session-timeout-ms": ("SBSQL-ED07CA49F7D2", "sb.scalar.idle_in_transaction_session_timeout_ms", "0", "uint64", ""),
    "SBSFC016-transaction-timeout": ("SBSQL-902AB93C9666", "sb.scalar.transaction_timeout", "0", "uint64", ""),
    "SBSFC016-transaction-timeout-default": ("SBSQL-9842FF657243", "sb.scalar.transaction_timeout_default", "0", "uint64", ""),
    "SBSFC016-null-in-unique-constraint": ("SBSQL-EF1565632553", "sb.scalar.null_in_unique_constraint", "multiple_nulls_allowed", "character", ""),
    "SBSFC016-qualified-name-max-segments": ("SBSQL-FFCEFC0CEF1E", "sb.scalar.qualified_name_max_segments", "16", "uint64", ""),
    "SBSFC016-empty-string-equals-null": ("SBSQL-F53140C3E231", "sb.scalar.empty_string_equals_null", "0", "boolean", ""),
    "SBSFC016-count-distinct-includes-null": ("SBSQL-E37283B0B5BD", "sb.scalar.count_distinct_includes_null", "0", "boolean", ""),
    "SBSFC016-operation-evidence-required": ("SBSQL-00A225C7BC09", "sb.scalar.operation_evidence_required", "1", "boolean", ""),
    "SBSFC016-decision-proof-required": ("SBSQL-5E9BF65CABA9", "sb.scalar.decision_proof_required", "1", "boolean", ""),
    "SBSFC016-current-capability-set": ("SBSQL-EA1055FD778D", "sb.scalar.current_capability_set", "public_noncluster_alpha", "character", ""),
    "SBSFC016-current-engine-version": ("SBSQL-96AC6D338681", "sb.scalar.current_engine_version", "ScratchBird 0.1.0", "character", ""),
    "SBSFC016-application-name-context": ("SBSQL-C69205E8B7F5", "sb.scalar.application_name", "sbsql_conformance", "character", ""),
    "SBSFC016-current-locale": ("SBSQL-52A87230C51F", "sb.scalar.current_locale", "en-US", "character", ""),
    "SBSFC016-client-protocol": ("SBSQL-9F79AF739250", "sb.scalar.client_protocol", "019e1600-0000-7000-8000-0000000000ee", "uuid", ""),
    "SBSFC016-private-profile-active": ("SBSQL-8EF55DAAC17F", "sb.scalar.private_profile_active", "0", "boolean", ""),
    "SBSFC016-built-in-function-shadow-rule": ("SBSQL-C41110D1C709", "sb.scalar.built_in_function_shadow_rule", "deny_builtin_shadowing", "character", ""),
    "SBSFC016-current-isolation-level": ("SBSQL-3D23D96C580E", "sb.scalar.current_isolation_level", "snapshot", "character", ""),
    "SBSFC016-mga-isolation-profile-context": ("SBSQL-0D3AF5689337", "sb.scalar.mga_isolation_profile", "snapshot_transaction", "character", ""),
    "SBSFC016-tx-read-only": ("SBSQL-33ABD23561B2", "sb.scalar.tx_read_only", "0", "boolean", ""),
    "SBSFC016-read-only-session": ("SBSQL-6E6DF1F1D3F5", "sb.scalar.read_only_session", "0", "boolean", ""),
    "SBSFC016-request-key-required": ("SBSQL-48630666FF4B", "sb.scalar.request_key_required", "1", "boolean", ""),
    "SBSFC016-sbsql-v3": ("SBSQL-493876D2FFDB", "sb.scalar.sbsql_v3", "sbsql.v3", "character", ""),
    "SBSFC016-sqlstate": ("SBSQL-95EFD6F591B0", "sb.scalar.sqlstate", "00000", "character", ""),
    "SBSFC016-sqlcode": ("SBSQL-B5B0AC8A7813", "sb.scalar.sqlcode", "0", "int64", ""),
    "SBSFC016-sqlerrm": ("SBSQL-9487E723F6DB", "sb.scalar.sqlerrm", "OK", "character", ""),
    "SBSFC016-not-found-default": ("SBSQL-26096EC6FBAF", "sb.scalar.not_found", "0", "boolean", ""),
    "SBSFC016-signal-diagnostic": ("SBSQL-B7E4638E5F7C", "sb.scalar.signal", "", "character", "SB_DIAG_PROCEDURAL_SIGNAL"),
    "SBSFC016-raise-diagnostic": ("SBSQL-22A8D96173B3", "sb.scalar.raise", "", "character", "SB_DIAG_PROCEDURAL_RAISE"),
    "SBSFC016-resignal-diagnostic": ("SBSQL-7390531C3071", "sb.scalar.resignal", "", "character", "SB_DIAG_PROCEDURAL_RESIGNAL"),
    "SBSFC016-deprecated-keyword": ("SBSQL-14D576F7019D", "sb.scalar.deprecated_keyword", "keyword_class.deprecated", "character", ""),
    "SBSFC016-reference-contextual-keyword": ("SBSQL-21B3D26B555C", "sb.scalar.reference_contextual_keyword", "keyword_class.reference_contextual", "character", ""),
    "SBSFC016-reserved-native-keyword": ("SBSQL-7036F89856D2", "sb.scalar.reserved_native_keyword", "keyword_class.reserved_native", "character", ""),
    "SBSFC016-contextual-native-keyword": ("SBSQL-FA8B706E49D0", "sb.scalar.contextual_native_keyword", "keyword_class.contextual_native", "character", ""),
    "SBSFC016-reference-reserved-keyword": ("SBSQL-813817A7EDFD", "sb.scalar.reference_reserved_keyword", "keyword_class.reference_reserved", "character", ""),
    "SBSFC016-meta-command-keyword": ("SBSQL-C1E6BF629293", "sb.scalar.meta_command_keyword", "keyword_class.meta_command", "character", ""),
    "SBSFC016-private-only-keyword": ("SBSQL-92C51F4C0F42", "sb.scalar.private_only_keyword", "keyword_class.private_only", "character", ""),
    "SBSFC016-refusal-only-keyword": ("SBSQL-2150B810CBA5", "sb.scalar.refusal_only_keyword", "keyword_class.refusal_only", "character", ""),
    "SBSFC016-statement-terminator": ("SBSQL-D8C32B223686", "sb.scalar.statement_terminator", "lexeme.statement_terminator", "character", ""),
    "SBSFC016-comment-line": ("SBSQL-9FB7E7E0066C", "sb.scalar.comment_line", "lexeme.comment_line", "character", ""),
    "SBSFC016-comment-block": ("SBSQL-BB0BB989E8B2", "sb.scalar.comment_block", "lexeme.comment_block", "character", ""),
    "SBSFC016-current-request-uuid": ("SBSQL-80864EB79EEB", "sb.scalar.current_request_uuid", "019e1600-0000-7000-8000-0000000000bb", "uuid", ""),
    "SBSFC016-current-dialect-version": ("SBSQL-5CDBD2168B18", "sb.scalar.current_dialect_version", "sbsql.v3", "character", ""),
    "SBSFC016-cardinality-violation": ("SBSQL-26BCBFF1FEED", "sb.scalar.cardinality_violation", "21000", "character", ""),
    "SBSFC016-currency": ("SBSQL-0032FE107FA7", "sb.scalar.currency", "datatype.currency", "character", ""),
    "SBSFC016-client-min-messages": ("SBSQL-0382B5F327AA", "sb.scalar.client_min_messages", "NOTICE", "character", ""),
    "SBSFC016-merge-action": ("SBSQL-03FC991DBA65", "sb.scalar.merge_action", "dml.merge_action", "character", ""),
    "SBSFC016-colocation": ("SBSQL-0D36181A09C4", "sb.scalar.colocation", "physical_layout.colocation", "character", ""),
    "SBSFC016-identifier-bare": ("SBSQL-1FF40A008189", "sb.scalar.identifier_bare", "identifier_class.bare", "character", ""),
    "SBSFC016-search-path": ("SBSQL-2477F077886D", "sb.scalar.search_path", "users.public", "character", ""),
    "SBSFC016-sbsql-psql": ("SBSQL-27CC3A88C9CA", "sb.scalar.sbsql_psql", "sbsql.psql", "character", ""),
    "SBSFC016-sql-variant": ("SBSQL-416D56ED915F", "sb.scalar.sql_variant", "datatype.sql_variant", "character", ""),
    "SBSFC016-random-seed": ("SBSQL-460F18A49506", "sb.scalar.random_seed", "0", "uint64", ""),
    "SBSFC016-recursion-limit": ("SBSQL-461B56193B9B", "sb.scalar.recursion_limit", "1024", "uint64", ""),
    "SBSFC016-performance": ("SBSQL-6DE72BA55723", "sb.scalar.performance", "management.performance", "character", ""),
    "SBSFC016-parser-only": ("SBSQL-72FA26BECAC5", "sb.scalar.parser_only", "scope.parser_only", "character", ""),
    "SBSFC016-deprecated": ("SBSQL-73BA9BC6005B", "sb.scalar.deprecated", "keyword_class.deprecated", "character", ""),
    "SBSFC016-filesystem": ("SBSQL-7D66C2236A2B", "sb.scalar.filesystem", "scope.filesystem_public_refused", "character", ""),
    "SBSFC016-refuse": ("SBSQL-80B16048C80F", "sb.scalar.refuse", "decision.refuse", "character", ""),
    "SBSFC016-metrics": ("SBSQL-823D51652BD0", "sb.scalar.metrics", "management.metrics", "character", ""),
    "SBSFC016-catalog-read": ("SBSQL-85D62227A882", "sb.scalar.catalog_read", "authority.catalog_read", "character", ""),
    "SBSFC016-reference-log-compatibility": ("SBSQL-87F8BED4D9EE", "sb.scalar.reference_log_compatibility", "compatibility.reference_log_non_authority", "character", ""),
    "SBSFC016-fail-closed": ("SBSQL-92C226CF5A7A", "sb.scalar.fail_closed", "decision.fail_closed", "character", ""),
    "SBSFC016-requires-new-function": ("SBSQL-980131EAA57E", "sb.scalar.requires_new_function", "implementation.requires_new_function", "character", ""),
    "SBSFC016-random-seed-control": ("SBSQL-A069D1ED14C0", "sb.scalar.random_seed_control", "policy.random_seed_control", "character", ""),
    "SBSFC016-evidence": ("SBSQL-B777E985366D", "sb.scalar.evidence", "evidence.required", "character", ""),
    "SBSFC016-private-profile-read": ("SBSQL-B845A701EF3C", "sb.scalar.private_profile_read", "authority.private_profile_read", "character", ""),
    "SBSFC016-evidence-chain-uuid": ("SBSQL-C9883DF74D82", "sb.scalar.evidence_chain_uuid", "019e1600-0000-7000-8000-0000000000bb", "uuid", ""),
    "SBSFC016-parameter-marker": ("SBSQL-CE7F2EE0D34E", "sb.scalar.parameter_marker", "token.parameter_marker", "character", ""),
    "SBSFC016-security": ("SBSQL-D437EC74B872", "sb.scalar.security", "management.security", "character", ""),
    "SBSFC016-localized-label": ("SBSQL-DC3ADB63538F", "sb.scalar.localized_label", "label.localized", "character", ""),
    "SBSFC016-policy-blocked": ("SBSQL-E302317C73E2", "sb.scalar.policy_blocked", "decision.policy_blocked", "character", ""),
    "SBSFC016-notice": ("SBSQL-E9EC607BA6D8", "sb.scalar.notice", "NOTICE", "character", ""),
    "SBSFC016-dictionary-encoded": ("SBSQL-F1C822127E64", "sb.scalar.dictionary_encoded", "encoding.dictionary", "character", ""),
    "SBSFC016-unresolved": ("SBSQL-06DAC31C3A89", "sb.scalar.unresolved", "decision.unresolved", "character", ""),
    "SBSFC016-public": ("SBSQL-0FEC8BF7CD54", "sb.scalar.public", "schema.public", "character", ""),
    "SBSFC016-tablegroup": ("SBSQL-112E5B307AE1", "sb.scalar.tablegroup", "physical_layout.tablegroup", "character", ""),
    "SBSFC016-none": ("SBSQL-131758ECDAEC", "sb.scalar.none", "value.none", "character", ""),
    "SBSFC016-descriptor": ("SBSQL-15828625DD4A", "sb.scalar.descriptor", "metadata.descriptor", "character", ""),
    "SBSFC016-engine": ("SBSQL-18E2D41E637B", "sb.scalar.engine", "authority.engine", "character", ""),
    "SBSFC016-catalog": ("SBSQL-3456DC1DC1E6", "sb.scalar.catalog", "authority.catalog", "character", ""),
    "SBSFC016-unsupported": ("SBSQL-456D4BF70496", "sb.scalar.unsupported", "decision.unsupported", "character", ""),
    "SBSFC016-mergetree": ("SBSQL-46D0BFB6ED61", "sb.scalar.mergetree", "physical_layout.mergetree", "character", ""),
    "SBSFC016-refused": ("SBSQL-6AA173CC38F1", "sb.scalar.refused", "decision.refused", "character", ""),
    "SBSFC016-innodb": ("SBSQL-70117DFD73D9", "sb.scalar.innodb", "reference_storage.innodb", "character", ""),
    "SBSFC016-hnsw": ("SBSQL-902E2EF680C8", "sb.scalar.hnsw", "index_method.hnsw", "character", ""),
    "SBSFC016-sessions": ("SBSQL-98F7C17D42D9", "sb.scalar.sessions", "management.sessions", "character", ""),
    "SBSFC016-asof": ("SBSQL-9B1E3F7A4C5F", "sb.scalar.asof", "temporal.asof", "character", ""),
    "SBSFC016-post-event": ("SBSQL-A19CEADCF8F5", "sb.scalar.post_event", "event.post", "character", ""),
    "SBSFC016-regional": ("SBSQL-A770228DEC74", "sb.scalar.regional", "locality.regional", "character", ""),
    "SBSFC016-ivf-flat": ("SBSQL-B6E1C5B44AAC", "sb.scalar.ivf_flat", "index_method.ivf_flat", "character", ""),
    "SBSFC016-tsquery": ("SBSQL-BB09AA368A93", "sb.scalar.tsquery", "datatype.tsquery", "character", ""),
    "SBSFC016-client-address": ("SBSQL-C549A9F3CF89", "sb.scalar.client_address", "session.client_address", "character", ""),
    "SBSFC016-txn": ("SBSQL-CD402A856EE1", "sb.scalar.txn", "transaction.alias", "character", ""),
    "SBSFC016-hierarchyid": ("SBSQL-D7D779AE16BC", "sb.scalar.hierarchyid", "datatype.hierarchyid", "character", ""),
    "SBSFC016-locality": ("SBSQL-E217738F653C", "sb.scalar.locality", "physical_layout.locality", "character", ""),
    "SBSFC016-unknown": ("SBSQL-FD4C6500CBC7", "sb.scalar.unknown", "value.unknown", "character", ""),
    "SBSFC016-sortop": ("SBSQL-FE93CA721F82", "sb.scalar.sortop", "operator.sortop", "character", ""),
    "SBSFC016-customer-id": ("SBSQL-0E3B7964D1F9", "sb.scalar.customer_id", "fixture.identifier.customer_id", "character", ""),
    "SBSFC016-customers": ("SBSQL-5EF1F41AA6DE", "sb.scalar.customers", "fixture.identifier.customers", "character", ""),
    "SBSFC016-sep": ("SBSQL-64EA78126DC3", "sb.scalar.sep", "fixture.identifier.sep", "character", ""),
    "SBSFC016-part": ("SBSQL-976C8ECD388C", "sb.scalar.part", "fixture.identifier.part", "character", ""),
    "SBSFC016-value": ("SBSQL-9EF349862A64", "sb.scalar.value", "fixture.identifier.value", "character", ""),
    "SBSFC016-expr": ("SBSQL-C6DE029570B1", "sb.scalar.expr", "fixture.identifier.expr", "character", ""),
    "SBSFC016-private-surface-refused": ("SBSQL-132BACF604F3", "sb.scalar.private_surface_refused", "diagnostic.private_surface_refused", "character", ""),
    "SBSFC016-no-request": ("SBSQL-2E1DD952FDAF", "sb.scalar.no_request", "diagnostic.no_request", "character", ""),
    "SBSFC016-no-statement": ("SBSQL-38565DFB4027", "sb.scalar.no_statement", "diagnostic.no_statement", "character", ""),
    "SBSFC016-unsupported-refused-by-design": ("SBSQL-3AA8E65D9971", "sb.scalar.unsupported_refused_by_design", "decision.unsupported_refused_by_design", "character", ""),
    "SBSFC016-operator-overload-unresolved": ("SBSQL-406B1DB66B8A", "sb.scalar.operator_overload_unresolved", "diagnostic.operator_overload_unresolved", "character", ""),
    "SBSFC016-no-transaction": ("SBSQL-4762703600F0", "sb.scalar.no_transaction", "diagnostic.no_transaction", "character", ""),
    "SBSFC016-requires-function-authoring": ("SBSQL-6ED46344E647", "sb.scalar.requires_function_authoring", "implementation.requires_function_authoring", "character", ""),
    "SBSFC016-event-trigger-authority-unavailable": ("SBSQL-7A84E49081E9", "sb.scalar.event_trigger_authority_unavailable", "authority.event_trigger_unavailable", "character", ""),
    "SBSFC016-capability-required": ("SBSQL-7F00278E0723", "sb.scalar.capability_required", "diagnostic.capability_required", "character", ""),
    "SBSFC016-psql-case-not-found": ("SBSQL-882F0BF84BCC", "sb.scalar.psql_case_not_found", "psql.case_not_found", "character", ""),
    "SBSFC016-syntax-parser-only": ("SBSQL-95878BFEDF43", "sb.scalar.syntax_parser_only", "syntax.parser_only", "character", ""),
    "SBSFC016-reference-only-rewrite": ("SBSQL-A371FE7C3BAA", "sb.scalar.reference_only_rewrite", "reference.rewrite_only", "character", ""),
    "SBSFC016-object-resolution-failed": ("SBSQL-A57396612A09", "sb.scalar.object_resolution_failed", "diagnostic.object_resolution_failed", "character", ""),
    "SBSFC016-error-diagnostic-uuid": ("SBSQL-B8E49C049ECB", "sb.scalar.error_diagnostic_uuid", "019e1600-0000-7000-8000-0000000000cc", "uuid", ""),
    "SBSFC016-transaction": ("SBSQL-91F466E96DE4", "sb.scalar.transaction", "fixture.identifier.transaction", "character", ""),
    "SBSFC016-context-ambiguous": ("SBSQL-BB49C3D09E24", "sb.scalar.context_ambiguous", "diagnostic.context_ambiguous", "character", ""),
    "SBSFC016-policy-blocked-diagnostic": ("SBSQL-CE3790BA0486", "sb.scalar.policy_blocked_diagnostic", "decision.policy_blocked", "character", ""),
    "SBSFC016-diag-sqlstate": ("SBSQL-CB2705E35D88", "sb.scalar.diag_sqlstate", "00000", "character", ""),
    "SBSFC016-canonical-function-idempotency-requirement": ("SBSQL-D2A2D11E9991", "sb.scalar.canonical_function_idempotency_requirement", "metadata.idempotency_requirement", "character", ""),
    "SBSFC016-deprecation-warning": ("SBSQL-D4C7802D088A", "sb.scalar.deprecation_warning", "warning.deprecation", "character", ""),
    "SBSFC016-syntax-unsupported": ("SBSQL-E01305A870F7", "sb.scalar.syntax_unsupported", "syntax.unsupported", "character", ""),
    "SBSFC016-dynamic-sql-untrusted": ("SBSQL-E41591430FF1", "sb.scalar.dynamic_sql_untrusted", "sblr.dynamic_sql.untrusted", "character", ""),
    "SBSFC016-udr-admission-denied": ("SBSQL-F4443D288A35", "sb.scalar.udr_admission_denied", "sblr.udr.admission_denied", "character", ""),
    "SBSFC016-statement": ("SBSQL-279F22AD9A6E", "sb.scalar.statement", "fixture.identifier.statement", "character", ""),
    "SBSFC016-table": ("SBSQL-4555F205B04B", "sb.scalar.table", "fixture.identifier.table", "character", ""),
    "SBSFC016-a": ("SBSQL-4A468A1D2EF5", "sb.scalar.a", "fixture.identifier.a", "character", ""),
    "SBSFC016-x": ("SBSQL-4E297DAEBA5B", "sb.scalar.x", "fixture.identifier.x", "character", ""),
    "SBSFC016-t": ("SBSQL-ADBBF56B1E71", "sb.scalar.t", "fixture.identifier.t", "character", ""),
    "SBSFC016-name": ("SBSQL-C643D29F39E1", "sb.scalar.name", "fixture.identifier.name", "character", ""),
    "SBSFC016-customer": ("SBSQL-B4D68D87A882", "sb.scalar.customer", "fixture.identifier.customer", "character", ""),
    "SBSFC016-session": ("SBSQL-F81E6A7D24D3", "sb.scalar.session", "fixture.identifier.session", "character", ""),
    "SBSFC016-contextual-keyword-alter": ("SBSQL-6B594AD9CCAB", "sb.scalar.alter", "keyword.contextual.alter", "character", ""),
    "SBSFC016-contextual-keyword-as": ("SBSQL-0F2568295099", "sb.scalar.as", "keyword.contextual.as", "character", ""),
    "SBSFC016-contextual-keyword-begin": ("SBSQL-054E79ED816E", "sb.scalar.begin", "keyword.contextual.begin", "character", ""),
    "SBSFC016-contextual-keyword-create": ("SBSQL-52AD0BC41D43", "sb.scalar.create", "keyword.contextual.create", "character", ""),
    "SBSFC016-contextual-keyword-cross": ("SBSQL-3E7F642D05FF", "sb.scalar.cross", "keyword.contextual.cross", "character", ""),
    "SBSFC016-contextual-keyword-distinct": ("SBSQL-AA7E9547F110", "sb.scalar.distinct", "keyword.contextual.distinct", "character", ""),
    "SBSFC016-contextual-keyword-drop": ("SBSQL-D94ED65E33DB", "sb.scalar.drop", "keyword.contextual.drop", "character", ""),
    "SBSFC016-contextual-keyword-else": ("SBSQL-6FA47CF68727", "sb.scalar.else", "keyword.contextual.else", "character", ""),
    "SBSFC016-contextual-keyword-end": ("SBSQL-DAFA3EAF3212", "sb.scalar.end", "keyword.contextual.end", "character", ""),
    "SBSFC016-contextual-keyword-events": ("SBSQL-09C5727E3E1B", "sb.scalar.events", "keyword.contextual.events", "character", ""),
    "SBSFC016-contextual-keyword-exists": ("SBSQL-169A3A38AFD4", "sb.scalar.exists", "keyword.contextual.exists", "character", ""),
    "SBSFC016-contextual-keyword-full": ("SBSQL-003C1703274A", "sb.scalar.full", "keyword.contextual.full", "character", ""),
    "SBSFC016-contextual-keyword-index": ("SBSQL-DE3A507DF9C4", "sb.scalar.index", "keyword.contextual.index", "character", ""),
    "SBSFC016-contextual-keyword-inner": ("SBSQL-D7351B1C962A", "sb.scalar.inner", "keyword.contextual.inner", "character", ""),
    "SBSFC016-contextual-keyword-is": ("SBSQL-E2F9B4091D9B", "sb.scalar.is", "keyword.contextual.is", "character", ""),
    "SBSFC016-contextual-keyword-lateral": ("SBSQL-999C058E0160", "sb.scalar.lateral", "keyword.contextual.lateral", "character", ""),
    "SBSFC016-contextual-keyword-natural": ("SBSQL-ACA52E0AA80F", "sb.scalar.natural", "keyword.contextual.natural", "character", ""),
    "SBSFC016-contextual-keyword-on": ("SBSQL-A1491C2B87CE", "sb.scalar.on", "keyword.contextual.on", "character", ""),
    "SBSFC016-contextual-keyword-outer": ("SBSQL-11D5D2852576", "sb.scalar.outer", "keyword.contextual.outer", "character", ""),
    "SBSFC016-contextual-keyword-sequence": ("SBSQL-907D2DCB4E7A", "sb.scalar.sequence", "keyword.contextual.sequence", "character", ""),
    "SBSFC016-contextual-keyword-show": ("SBSQL-94CD1F009C96", "sb.scalar.show", "keyword.contextual.show", "character", ""),
    "SBSFC016-contextual-keyword-similar": ("SBSQL-05EAA3104951", "sb.scalar.similar", "keyword.contextual.similar", "character", ""),
    "SBSFC016-contextual-keyword-then": ("SBSQL-0B5204549537", "sb.scalar.then", "keyword.contextual.then", "character", ""),
    "SBSFC016-contextual-keyword-upsert": ("SBSQL-540CB1A2A056", "sb.scalar.upsert", "keyword.contextual.upsert", "character", ""),
    "SBSFC016-contextual-keyword-using": ("SBSQL-00E6C0F7766E", "sb.scalar.using", "keyword.contextual.using", "character", ""),
    "SBSFC016-contextual-keyword-view": ("SBSQL-AE7A4534FAF9", "sb.scalar.view", "keyword.contextual.view", "character", ""),
    "SBSFC016-contextual-keyword-wait": ("SBSQL-A5E97200C0D9", "sb.scalar.wait", "keyword.contextual.wait", "character", ""),
    "SBSFC016-contextual-keyword-when": ("SBSQL-3666F7699C5E", "sb.scalar.when", "keyword.contextual.when", "character", ""),
    "SBSFC016-contextual-keyword-with": ("SBSQL-CC9235354362", "sb.scalar.with", "keyword.contextual.with", "character", ""),
    "SBSFC016-stmt-null": ("SBSQL-853009230194", "sb.scalar.stmt_null", "statement.null", "character", ""),
}

FIXED_POLICY_LIMIT_CASES = {
    "SBSFC016-array-max-dimension",
    "SBSFC016-array-max-element-count",
    "SBSFC016-case-when-max-branches",
    "SBSFC016-cte-max-count-per-statement",
    "SBSFC016-nested-subquery-max-depth",
    "SBSFC016-recursive-cte-max-depth",
    "SBSFC016-result-set-max-columns",
    "SBSFC016-union-max-arms",
}

FIXED_POLICY_LANGUAGE_PROPERTY_CASES = {
    "SBSFC016-numeric-division-by-zero",
    "SBSFC016-localized-label-max-length-bytes",
    "SBSFC016-default-schema-resolution",
    "SBSFC016-result-set-max-rows-in-response",
    "SBSFC016-identifier-max-length-bytes",
    "SBSFC016-statement-timeout-ms",
    "SBSFC016-client-min-messages-default",
    "SBSFC016-numeric-overflow-behavior",
    "SBSFC016-null-ordering-default-for-asc",
    "SBSFC016-statement-max-length-bytes",
    "SBSFC016-null-concat-returns-null",
    "SBSFC016-recursion-max-depth",
    "SBSFC016-parameter-marker-max-count",
    "SBSFC016-delimited-identifier-max-length-bytes",
    "SBSFC016-case-resolution-for-quoted-identifiers",
    "SBSFC016-temporal-default-precision",
    "SBSFC016-string-truncation-behavior",
    "SBSFC016-timezone-resolution",
    "SBSFC016-null-in-aggregate-skipped",
    "SBSFC016-null-ordering-default-for-desc",
    "SBSFC016-interval-default-precision",
    "SBSFC016-name-resolution",
    "SBSFC016-recursive-schema-path-separator",
    "SBSFC016-identifier-max-length-chars",
    "SBSFC016-lock-timeout-ms",
    "SBSFC016-idle-in-transaction-session-timeout-ms",
    "SBSFC016-null-in-unique-constraint",
    "SBSFC016-qualified-name-max-segments",
    "SBSFC016-empty-string-equals-null",
    "SBSFC016-count-distinct-includes-null",
}

METADATA_POLICY_CASES = {
    "SBSFC016-operation-evidence-required",
    "SBSFC016-decision-proof-required",
    "SBSFC016-current-capability-set",
    "SBSFC016-current-engine-version",
    "SBSFC016-application-name-context",
    "SBSFC016-current-locale",
    "SBSFC016-client-protocol",
    "SBSFC016-private-profile-active",
    "SBSFC016-built-in-function-shadow-rule",
    "SBSFC016-current-isolation-level",
    "SBSFC016-mga-isolation-profile-context",
    "SBSFC016-tx-read-only",
    "SBSFC016-read-only-session",
    "SBSFC016-request-key-required",
    "SBSFC016-sbsql-v3",
    "SBSFC016-sqlstate",
    "SBSFC016-sqlcode",
    "SBSFC016-sqlerrm",
    "SBSFC016-not-found-default",
    "SBSFC016-deprecated-keyword",
    "SBSFC016-reference-contextual-keyword",
    "SBSFC016-reserved-native-keyword",
    "SBSFC016-contextual-native-keyword",
    "SBSFC016-reference-reserved-keyword",
    "SBSFC016-meta-command-keyword",
    "SBSFC016-private-only-keyword",
    "SBSFC016-refusal-only-keyword",
    "SBSFC016-statement-terminator",
    "SBSFC016-comment-line",
    "SBSFC016-comment-block",
    "SBSFC016-current-request-uuid",
    "SBSFC016-current-dialect-version",
    "SBSFC016-cardinality-violation",
    "SBSFC016-currency",
    "SBSFC016-client-min-messages",
    "SBSFC016-merge-action",
    "SBSFC016-colocation",
    "SBSFC016-identifier-bare",
    "SBSFC016-search-path",
    "SBSFC016-sbsql-psql",
    "SBSFC016-sql-variant",
    "SBSFC016-random-seed",
    "SBSFC016-recursion-limit",
    "SBSFC016-performance",
    "SBSFC016-parser-only",
    "SBSFC016-deprecated",
    "SBSFC016-filesystem",
    "SBSFC016-refuse",
    "SBSFC016-metrics",
    "SBSFC016-catalog-read",
    "SBSFC016-reference-log-compatibility",
    "SBSFC016-fail-closed",
    "SBSFC016-requires-new-function",
    "SBSFC016-random-seed-control",
    "SBSFC016-evidence",
    "SBSFC016-private-profile-read",
    "SBSFC016-evidence-chain-uuid",
    "SBSFC016-parameter-marker",
    "SBSFC016-security",
    "SBSFC016-localized-label",
    "SBSFC016-policy-blocked",
    "SBSFC016-notice",
    "SBSFC016-dictionary-encoded",
    "SBSFC016-unresolved",
    "SBSFC016-public",
    "SBSFC016-tablegroup",
    "SBSFC016-none",
    "SBSFC016-descriptor",
    "SBSFC016-engine",
    "SBSFC016-catalog",
    "SBSFC016-unsupported",
    "SBSFC016-mergetree",
    "SBSFC016-refused",
    "SBSFC016-innodb",
    "SBSFC016-hnsw",
    "SBSFC016-sessions",
    "SBSFC016-asof",
    "SBSFC016-post-event",
    "SBSFC016-regional",
    "SBSFC016-ivf-flat",
    "SBSFC016-tsquery",
    "SBSFC016-client-address",
    "SBSFC016-txn",
    "SBSFC016-hierarchyid",
    "SBSFC016-locality",
    "SBSFC016-unknown",
    "SBSFC016-sortop",
    "SBSFC016-customer-id",
    "SBSFC016-customers",
    "SBSFC016-sep",
    "SBSFC016-part",
    "SBSFC016-value",
    "SBSFC016-expr",
    "SBSFC016-private-surface-refused",
    "SBSFC016-no-request",
    "SBSFC016-no-statement",
    "SBSFC016-unsupported-refused-by-design",
    "SBSFC016-operator-overload-unresolved",
    "SBSFC016-no-transaction",
    "SBSFC016-requires-function-authoring",
    "SBSFC016-event-trigger-authority-unavailable",
    "SBSFC016-capability-required",
    "SBSFC016-psql-case-not-found",
    "SBSFC016-syntax-parser-only",
    "SBSFC016-reference-only-rewrite",
    "SBSFC016-object-resolution-failed",
    "SBSFC016-error-diagnostic-uuid",
    "SBSFC016-transaction",
    "SBSFC016-context-ambiguous",
    "SBSFC016-policy-blocked-diagnostic",
    "SBSFC016-diag-sqlstate",
    "SBSFC016-canonical-function-idempotency-requirement",
    "SBSFC016-deprecation-warning",
    "SBSFC016-syntax-unsupported",
    "SBSFC016-dynamic-sql-untrusted",
    "SBSFC016-udr-admission-denied",
    "SBSFC016-statement",
    "SBSFC016-table",
    "SBSFC016-a",
    "SBSFC016-x",
    "SBSFC016-t",
    "SBSFC016-name",
    "SBSFC016-customer",
    "SBSFC016-session",
    "SBSFC016-contextual-keyword-alter",
    "SBSFC016-contextual-keyword-as",
    "SBSFC016-contextual-keyword-begin",
    "SBSFC016-contextual-keyword-create",
    "SBSFC016-contextual-keyword-cross",
    "SBSFC016-contextual-keyword-distinct",
    "SBSFC016-contextual-keyword-drop",
    "SBSFC016-contextual-keyword-else",
    "SBSFC016-contextual-keyword-end",
    "SBSFC016-contextual-keyword-events",
    "SBSFC016-contextual-keyword-exists",
    "SBSFC016-contextual-keyword-full",
    "SBSFC016-contextual-keyword-index",
    "SBSFC016-contextual-keyword-inner",
    "SBSFC016-contextual-keyword-is",
    "SBSFC016-contextual-keyword-lateral",
    "SBSFC016-contextual-keyword-natural",
    "SBSFC016-contextual-keyword-on",
    "SBSFC016-contextual-keyword-outer",
    "SBSFC016-contextual-keyword-sequence",
    "SBSFC016-contextual-keyword-show",
    "SBSFC016-contextual-keyword-similar",
    "SBSFC016-contextual-keyword-then",
    "SBSFC016-contextual-keyword-upsert",
    "SBSFC016-contextual-keyword-using",
    "SBSFC016-contextual-keyword-view",
    "SBSFC016-contextual-keyword-wait",
    "SBSFC016-contextual-keyword-when",
    "SBSFC016-contextual-keyword-with",
    "SBSFC016-stmt-null",
}

PROCEDURAL_DIAGNOSTIC_CASES = {
    "SBSFC016-signal-diagnostic",
    "SBSFC016-raise-diagnostic",
    "SBSFC016-resignal-diagnostic",
}


def fail(message: str) -> None:
    print(message, file=sys.stderr)
    raise SystemExit(1)


def read_csv(path: Path) -> list[dict[str, str]]:
    if not path.is_file():
        fail(f"required CSV missing: {path}")
    with path.open(newline="", encoding="utf-8") as handle:
        return list(csv.DictReader(handle))


def read_text(path: Path) -> str:
    if not path.is_file():
        fail(f"required source missing: {path}")
    return path.read_text(encoding="utf-8")


def builtin_ids(text: str) -> set[str]:
    return set(re.findall(r"^\s*-\s*builtin_id:\s*([A-Za-z0-9_.-]+)\s*$", text, re.MULTILINE))


def surface_ids(rows: list[dict[str, str]]) -> set[str]:
    return {row["surface_id"] for row in rows}


def validate_arguments(row: dict[str, str]) -> None:
    try:
        parsed = json.loads(row["arguments_json"])
    except json.JSONDecodeError as exc:
        fail(f"{row['fixture_id']}: arguments_json is not valid JSON: {exc}")
    if not isinstance(parsed, list):
        fail(f"{row['fixture_id']}: arguments_json must be a list")
    if row["fixture_id"] == "SBSFC016-current-setting-unknown-missing-ok-null":
        if parsed != [{"text_value": "not_a_setting"}, {"bool_value": True}]:
            fail(f"{row['fixture_id']}: missing_ok fixture must use exact boolean true payload")
    literal_refusals = {
        "SBSFC016-current-setting-var-exact-refusal": "var",
        "SBSFC016-current-setting-autocommit-exact-refusal": "autocommit",
    }
    expected_setting = literal_refusals.get(row["fixture_id"])
    if expected_setting is not None and parsed != [{"text_value": expected_setting}]:
        fail(f"{row['fixture_id']}: literal refusal fixture must use exact {expected_setting!r} payload")


def main() -> int:
    parser = argparse.ArgumentParser()
    parser.add_argument("--repo-root", default=".")
    args = parser.parse_args()
    root = Path(args.repo_root).resolve()

    fixtures = read_csv(root / FIXTURES)
    if not fixtures:
        fail("SBSFC-016 fixture CSV is empty")
    if list(fixtures[0].keys()) != REQUIRED_COLUMNS:
        fail(f"SBSFC-016 fixture columns drifted: {list(fixtures[0].keys())}")

    by_case = {row["fixture_id"]: row for row in fixtures}
    if set(by_case) != set(EXPECTED_CASES):
        fail(f"SBSFC-016 fixture set drifted: {sorted(set(by_case) ^ set(EXPECTED_CASES))}")

    surfaces = surface_ids(read_csv(root / SURFACE_REGISTRY))
    builtins = builtin_ids(read_text(root / BUILTIN_EXPRESSION_REGISTRY))
    seed_text = read_text(root / SEED_REGISTRY)
    dispatch_text = read_text(root / DISPATCH_SOURCE)

    for fixture_id, (surface_id, function_id, expected_value, descriptor, diagnostic) in EXPECTED_CASES.items():
        row = by_case[fixture_id]
        if row["surface_id"] != surface_id:
            fail(f"{fixture_id}: expected surface {surface_id}, got {row['surface_id']}")
        if surface_id not in surfaces:
            fail(f"{fixture_id}: surface {surface_id} missing from surface registry")
        if row["function_id"] != function_id or row["canonical_builtin_id"] != function_id:
            fail(f"{fixture_id}: function/canonical id mismatch")
        uses_builtin_oracle = "builtin-expression-registry.yaml" in row["oracle_authority_ref"]
        uses_surface_oracle = "SBSQL_SURFACE_REGISTRY.csv" in row["oracle_authority_ref"]
        if uses_builtin_oracle and function_id not in builtins:
            fail(f"{fixture_id}: builtin id {function_id} missing from builtin-expression-registry")
        if not uses_builtin_oracle and not uses_surface_oracle:
            fail(f"{fixture_id}: oracle authority must include builtin-expression-registry.yaml or SBSQL_SURFACE_REGISTRY.csv")
        if f'"{function_id}"' not in seed_text:
            fail(f"{fixture_id}: seed registry entry missing for {function_id}")
        dispatch_key = function_id.split(".")[-1]
        if dispatch_key not in dispatch_text:
            fail(f"{fixture_id}: dispatch source does not mention {dispatch_key}")
        if row["expected_result_value"] != expected_value:
            fail(f"{fixture_id}: expected value {expected_value!r}, got {row['expected_result_value']!r}")
        if row["expected_result_descriptor"] != descriptor:
            fail(f"{fixture_id}: expected descriptor {descriptor}, got {row['expected_result_descriptor']}")
        if row["expected_diagnostic_code"] != diagnostic:
            fail(f"{fixture_id}: expected diagnostic {diagnostic}, got {row['expected_diagnostic_code']}")
        validate_arguments(row)

    for fixture_id in FIXED_POLICY_LIMIT_CASES | FIXED_POLICY_LANGUAGE_PROPERTY_CASES:
        row = by_case[fixture_id]
        if row["case_kind"] != "positive_fixed_policy":
            fail(f"{fixture_id}: fixed policy row must be positive_fixed_policy")
        if row["evaluation_mode"] != "scalar":
            fail(f"{fixture_id}: fixed policy row must use scalar evaluation")
        if json.loads(row["arguments_json"]) != []:
            fail(f"{fixture_id}: fixed policy row must be nullary")
        if row["expected_diagnostic_code"]:
            fail(f"{fixture_id}: fixed policy row must not expect a diagnostic")
        dispatch_key = row["function_id"].split(".")[-1]
        if f"data_scalar_functions_06_system_session_catalog.inc#{dispatch_key}" not in row["oracle_authority_ref"]:
            fail(f"{fixture_id}: fixed policy row must cite dispatch source")

    for fixture_id in METADATA_POLICY_CASES:
        row = by_case[fixture_id]
        if row["case_kind"] not in {"positive_metadata_policy", "positive_context_metadata"}:
            fail(f"{fixture_id}: metadata row must use metadata case kind")
        if row["evaluation_mode"] != "scalar":
            fail(f"{fixture_id}: metadata row must use scalar evaluation")
        if json.loads(row["arguments_json"]) != []:
            fail(f"{fixture_id}: metadata row must be nullary")
        if row["expected_diagnostic_code"]:
            fail(f"{fixture_id}: metadata row must not expect a diagnostic")
        dispatch_key = row["function_id"].split(".")[-1]
        if f"data_scalar_functions_06_system_session_catalog.inc#{dispatch_key}" not in row["oracle_authority_ref"]:
            fail(f"{fixture_id}: metadata row must cite dispatch source")

    for fixture_id in PROCEDURAL_DIAGNOSTIC_CASES:
        row = by_case[fixture_id]
        if row["case_kind"] != "procedural_diagnostic":
            fail(f"{fixture_id}: procedural diagnostic row must use procedural_diagnostic case kind")
        if row["evaluation_mode"] != "scalar":
            fail(f"{fixture_id}: procedural diagnostic row must use scalar evaluation")
        if json.loads(row["arguments_json"]) != []:
            fail(f"{fixture_id}: procedural diagnostic row must be nullary")
        if not row["expected_diagnostic_code"].startswith("SB_DIAG_PROCEDURAL_"):
            fail(f"{fixture_id}: procedural diagnostic row must expect procedural diagnostic")
        dispatch_key = row["function_id"].split(".")[-1]
        if f"data_scalar_functions_06_system_session_catalog.inc#{dispatch_key}" not in row["oracle_authority_ref"]:
            fail(f"{fixture_id}: procedural diagnostic row must cite dispatch source")

    for fixture_id in FIXED_POLICY_LIMIT_CASES:
        row = by_case[fixture_id]
        if row["expected_result_descriptor"] != "uint64":
            fail(f"{fixture_id}: fixed policy limit row must return uint64")

    if "current_setting setting is unknown" not in dispatch_text:
        fail("current_setting exact unknown-setting refusal detail is missing")
    if "MakeNullValue(\"character\")" not in dispatch_text:
        fail("current_setting missing_ok null behavior is not statically visible")

    return 0


if __name__ == "__main__":
    raise SystemExit(main())
