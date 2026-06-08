#!/usr/bin/env python3
# Copyright (c) 2026 ScratchBird Software Inc.
#
# This Source Code Form is subject to the terms of the Mozilla Public
# License, v. 2.0. If a copy of the MPL was not distributed with this
# file, You can obtain one at https://mozilla.org/MPL/2.0/.
#
# SPDX-License-Identifier: MPL-2.0

"""Generate FUNCTION_SEMANTIC_ORACLE_MATRIX.csv for the SBsql Surface-to-SBLR execution_plan.

Inputs (repo-local, no network):
  public_input_snapshot
  public_contract_snapshot
  public_contract_snapshot
  public_contract_snapshot
  public_contract_snapshot

Output:
  project/tests/sbsql_parser_worker/fixtures/surface_to_sblr/artifacts/FUNCTION_SEMANTIC_ORACLE_MATRIX.csv

For every function/operator/variable surface in the canonical SBsql ledger,
this generator records the oracle semantic contract that P2 implementation
slices (SBSFC-010..SBSFC-016) must satisfy. Oracle values are sourced
exclusively from canonical authority YAML (no implementation-derived values).

Match policy:

1. Match an explicit `surface_id` in `builtin-window-registry.yaml`. Window
   functions are descriptor-bound and may share leaf names with scalar or
   aggregate forms, so only explicit surface-id authority can promote them to
   full oracle status.

2. Match an exact `builtin_id` in `builtin-special-form-registry.yaml`.
   Special forms are not ordinary scalar builtins, but they are canonical
   expression-runtime surfaces with their own SBLR binding and engine
   entrypoint authority.

3. Normalize the surface canonical_name (lowercase, strip from first '(')
   and try to match against `canonical_name` in
   `builtin-expression-registry.yaml`. On match, copy the full oracle
   record (volatility, determinism, side_effects, null_behavior,
   collation_charset_rule, timezone_rule, return_type_rule, coercion_rule,
   error_semantics, optimizer_properties, security_policy, donor_rendering,
   syntax_forms, sblr_binding, ast_binding, engine_entrypoint).
   oracle_authority_status = `full_oracle`.

4. Otherwise, try to match against `builtin_id` in
   `builtin-sblr-expression-binding.yaml`. Build a probe builtin_id from the
   normalized name using known package prefixes (sb.scalar, sb.temporal,
   sb.uuid, sb.session, etc.). On match, copy `kind`, `sblr_binding`, and
   `engine_entrypoint`; semantic fields remain `oracle_pending`.
   oracle_authority_status = `binding_only`.

5. Otherwise, all oracle fields are `oracle_pending` and
   oracle_authority_status = `pending_canonical_authority_entry`. P2/P3
   slices may not implement the row until the canonical authority records
   are added and this matrix re-emits with the real oracle values.

This baseline matrix is the read source for SBSFC-002 fixture authorship,
SBSFC-006 SBLR binary round-trip oracle assertions, and SBSFC-010..-016
function implementation acceptance gates. Coordinator override flows through
SBSFC-009B status-change authority.

Architecture invariant compliance: read-only YAML/CSV consumption; no
transaction model touched; no engine, parser worker, server, listener,
storage, or MGA file modified; no WAL surface introduced. The oracle matrix
is a contract-derived expectation record only — it does not introduce
any execution authority. MGA copy-on-write remains the sole transaction
recovery model.
"""

from __future__ import annotations

import argparse
import csv
import re
import sys
from pathlib import Path

import yaml


REGISTRY_CSV = "public_input_snapshot"
EXPR_REGISTRY = "public_contract_snapshot"
SPECIAL_FORM_REGISTRY = "public_contract_snapshot"
BINDING_REGISTRY = "public_contract_snapshot"
WINDOW_REGISTRY = "public_contract_snapshot"
DEFAULT_ARTIFACT_ROOT = "project/tests/sbsql_parser_worker/fixtures/surface_to_sblr/artifacts"
OUTPUT_NAME = "FUNCTION_SEMANTIC_ORACLE_MATRIX.csv"


COLUMNS = [
    "surface_id",
    "canonical_name",
    "normalized_name",
    "surface_kind",
    "status",
    "cluster_scope",
    "sblr_operation_family",
    "oracle_authority_status",
    "matched_builtin_id",
    "matched_canonical_name",
    "argument_descriptor_rule",
    "return_type_rule",
    "coercion_rule",
    "null_behavior",
    "collation_charset_rule",
    "timezone_rule",
    "volatility",
    "determinism",
    "side_effects",
    "foldable",
    "index_eligible",
    "generated_column_eligible",
    "cost_class",
    "security_policy",
    "error_semantics",
    "expected_diagnostics",
    "sblr_binding",
    "ast_binding",
    "engine_entrypoint",
    "donor_rendering",
    "syntax_forms",
    "conformance_cases",
    "notes",
]


PENDING = "oracle_pending"
EXPRESSION_RUNTIME_KINDS = {"function", "operator", "variable"}
BINDING_PACKAGES = (
    "sb.scalar.",
    "sb.temporal.",
    "sb.uuid.",
    "sb.session.",
    "sb.aggregate.",
    "sb.window.",
    "sb.special.",
    "sb.special_form.",
    "sb.operator.",
    "sb.json.",
    "sb.regex.",
)

SBSFC032_SCALAR_UTILITY_ORACLE_OVERRIDES = {
    "SBSQL-23E118659719": {
        "builtin_id": "sb.scalar.atan2d",
        "canonical_name": "atan2d",
        "overloads": [{"signature": "atan2d(y,x)", "argument_rule": "two numeric arguments y,x; SQL null input returns SQL null"}],
        "return_type_rule": "real64 degrees value computed as atan2(y,x) * 180 / pi",
        "coercion_rule": "arguments must be numeric or numeric-compatible scalar values",
        "null_behavior": "null input returns SQL null real64",
        "collation_charset_rule": "not applicable",
        "timezone_rule": "not applicable",
        "volatility": "immutable",
        "determinism": "deterministic",
        "side_effects": "none",
        "sblr_binding": "sblr.expr.scalar_atan2d.v3",
        "ast_binding": "ast.expr.scalar_atan2d",
        "engine_entrypoint": "atan2d",
        "optimizer_properties": {"foldable": False, "index_eligible": False, "generated_column_eligible": True, "cost_class": "cpu_scalar"},
        "security_policy": "pure scalar numeric helper; no catalog, storage, security, or transaction authority",
        "donor_rendering": "parser renders donor spelling and diagnostics through donor alias registry when applicable",
        "error_semantics": "invalid arity or non-numeric arguments refuse with SBSQL.FUNCTION.INVALID_INPUT",
        "syntax_forms": ["function_call"],
        "conformance_cases": ["SBSFC032-atan2d-quadrant"],
    },
    "SBSQL-6DFCF52729AD": {
        "builtin_id": "sb.scalar.collation_for",
        "canonical_name": "collation_for",
        "overloads": [{"signature": "collation_for()", "argument_rule": "bare form is parse/lowering evidence and runtime invalid-input"}],
        "return_type_rule": "character collation name for argument descriptor metadata",
        "coercion_rule": "one scalar value for runtime metadata form",
        "null_behavior": "null input returns SQL null character",
        "collation_charset_rule": "returns argument collation_name or unicode_root when absent",
        "timezone_rule": "not applicable",
        "volatility": "immutable",
        "determinism": "descriptor-backed deterministic",
        "side_effects": "none",
        "sblr_binding": "sblr.expr.scalar_collation_for.v3",
        "ast_binding": "ast.expr.scalar_collation_for",
        "engine_entrypoint": "collation_for",
        "optimizer_properties": {"foldable": False, "index_eligible": False, "generated_column_eligible": False, "cost_class": "cpu_scalar"},
        "security_policy": "reads only already-bound value descriptor metadata",
        "donor_rendering": "parser renders donor spelling and diagnostics through donor alias registry when applicable",
        "error_semantics": "invalid arity refuses with SBSQL.FUNCTION.INVALID_INPUT",
        "syntax_forms": ["function_call"],
        "conformance_cases": ["SBSFC032-collation-for-bare-invalid", "SBSFC032-collation-for-text"],
    },
    "SBSQL-D989C17E1878": "SBSQL-6DFCF52729AD",
    "SBSQL-99C52D5953AD": {
        "builtin_id": "sb.scalar.descriptor_of",
        "canonical_name": "descriptor_of",
        "overloads": [{"signature": "descriptor_of(expr)", "argument_rule": "one scalar expression"}],
        "return_type_rule": "json_document compact descriptor document with descriptor_id payload_kind is_null charset_name collation_name",
        "coercion_rule": "argument is inspected without value coercion",
        "null_behavior": "null argument still reports descriptor metadata with is_null true",
        "collation_charset_rule": "reports argument charset_name and collation_name metadata",
        "timezone_rule": "not applicable",
        "volatility": "immutable",
        "determinism": "descriptor-backed deterministic",
        "side_effects": "none",
        "sblr_binding": "sblr.expr.scalar_descriptor_of.v3",
        "ast_binding": "ast.expr.scalar_descriptor_of",
        "engine_entrypoint": "descriptor_of",
        "optimizer_properties": {"foldable": False, "index_eligible": False, "generated_column_eligible": False, "cost_class": "cpu_scalar"},
        "security_policy": "reads only already-bound value descriptor metadata",
        "donor_rendering": "parser renders donor spelling and diagnostics through donor alias registry when applicable",
        "error_semantics": "invalid arity refuses with SBSQL.FUNCTION.INVALID_INPUT",
        "syntax_forms": ["function_call"],
        "conformance_cases": ["SBSFC032-descriptor-of-bare-invalid", "SBSFC032-descriptor-of-expr"],
    },
    "SBSQL-7A5C9806BEF5": "SBSQL-99C52D5953AD",
    "SBSQL-CC379798CF3D": {
        "builtin_id": "sb.scalar.pg_typeof",
        "canonical_name": "pg_typeof",
        "overloads": [{"signature": "pg_typeof(expr)", "argument_rule": "zero or one scalar expression"}],
        "return_type_rule": "character descriptor_id for argument or unknown for bare form",
        "coercion_rule": "argument is inspected without value coercion",
        "null_behavior": "null argument still reports descriptor id",
        "collation_charset_rule": "implementation default character descriptor",
        "timezone_rule": "not applicable",
        "volatility": "immutable",
        "determinism": "descriptor-backed deterministic",
        "side_effects": "none",
        "sblr_binding": "sblr.expr.scalar_pg_typeof.v3",
        "ast_binding": "ast.expr.scalar_pg_typeof",
        "engine_entrypoint": "pg_typeof",
        "optimizer_properties": {"foldable": False, "index_eligible": False, "generated_column_eligible": False, "cost_class": "cpu_scalar"},
        "security_policy": "reads only already-bound value descriptor metadata",
        "donor_rendering": "parser renders donor spelling and diagnostics through donor alias registry when applicable",
        "error_semantics": "arity greater than 1 refuses with SBSQL.FUNCTION.INVALID_INPUT",
        "syntax_forms": ["function_call"],
        "conformance_cases": ["SBSFC032-pg-typeof-bare-unknown", "SBSFC032-pg-typeof-expr"],
    },
    "SBSQL-3E3527E30D5F": "SBSQL-CC379798CF3D",
    "SBSQL-D6FBF57E26FC": {
        "builtin_id": "sb.scalar.safe_cast",
        "canonical_name": "safe_cast",
        "overloads": [{"signature": "safe_cast(expr AS type)", "argument_rule": "value plus target descriptor text in engine route; SQL syntax lowers through bounded cast route"}],
        "return_type_rule": "target descriptor value or SQL null with target descriptor on data conversion failure",
        "coercion_rule": "target descriptor text must name a supported scalar descriptor",
        "null_behavior": "null input returns SQL null with target descriptor",
        "collation_charset_rule": "target descriptor controls result metadata",
        "timezone_rule": "not applicable",
        "volatility": "immutable",
        "determinism": "deterministic",
        "side_effects": "none",
        "sblr_binding": "sblr.expr.scalar_safe_cast.v3",
        "ast_binding": "ast.expr.scalar_safe_cast",
        "engine_entrypoint": "safe_cast",
        "optimizer_properties": {"foldable": False, "index_eligible": False, "generated_column_eligible": False, "cost_class": "cpu_scalar"},
        "security_policy": "conversion helper does not bypass security policy dependency gates",
        "donor_rendering": "parser renders donor spelling and diagnostics through donor alias registry when applicable",
        "error_semantics": "unknown target descriptor or invalid arity refuses with SBSQL.FUNCTION.INVALID_INPUT; data conversion failure returns SQL null",
        "syntax_forms": ["function_call", "cast_like_function_call"],
        "conformance_cases": ["SBSFC032-safe-cast-bare-invalid", "SBSFC032-safe-cast-expr-as-type"],
    },
    "SBSQL-6A962F180717": "SBSQL-D6FBF57E26FC",
    "SBSQL-78EE8FA84A8F": {
        "builtin_id": "sb.scalar.try_cast",
        "canonical_name": "try_cast",
        "overloads": [{"signature": "try_cast(expr AS type)", "argument_rule": "value plus target descriptor text in engine route; SQL syntax lowers through bounded cast route"}],
        "return_type_rule": "target descriptor value or SQL null with target descriptor on data conversion failure",
        "coercion_rule": "target descriptor text must name a supported scalar descriptor",
        "null_behavior": "null input returns SQL null with target descriptor",
        "collation_charset_rule": "target descriptor controls result metadata",
        "timezone_rule": "not applicable",
        "volatility": "immutable",
        "determinism": "deterministic",
        "side_effects": "none",
        "sblr_binding": "sblr.expr.scalar_try_cast.v3",
        "ast_binding": "ast.expr.scalar_try_cast",
        "engine_entrypoint": "try_cast",
        "optimizer_properties": {"foldable": False, "index_eligible": False, "generated_column_eligible": False, "cost_class": "cpu_scalar"},
        "security_policy": "conversion helper does not bypass security policy dependency gates",
        "donor_rendering": "parser renders donor spelling and diagnostics through donor alias registry when applicable",
        "error_semantics": "unknown target descriptor or invalid arity refuses with SBSQL.FUNCTION.INVALID_INPUT; data conversion failure returns SQL null",
        "syntax_forms": ["function_call", "cast_like_function_call"],
        "conformance_cases": ["SBSFC032-try-cast-bare-invalid", "SBSFC032-try-cast-expr-as-type-null-on-failure"],
    },
    "SBSQL-77A5EAFF0CD5": "SBSQL-78EE8FA84A8F",
    "SBSQL-BC1A67FBC111": {
        "builtin_id": "sb.scalar.similar_to_escape",
        "canonical_name": "similar_to_escape",
        "overloads": [{"signature": "similar_to_escape(text)", "argument_rule": "one text argument"}],
        "return_type_rule": "character text with SIMILAR TO metacharacters escaped by backslash",
        "coercion_rule": "argument converted through scalar text representation",
        "null_behavior": "null input returns SQL null character",
        "collation_charset_rule": "implementation default character descriptor",
        "timezone_rule": "not applicable",
        "volatility": "immutable",
        "determinism": "deterministic",
        "side_effects": "none",
        "sblr_binding": "sblr.expr.scalar_similar_to_escape.v3",
        "ast_binding": "ast.expr.scalar_similar_to_escape",
        "engine_entrypoint": "similar_to_escape",
        "optimizer_properties": {"foldable": False, "index_eligible": False, "generated_column_eligible": True, "cost_class": "cpu_scalar"},
        "security_policy": "pure text helper; no catalog, storage, security, or transaction authority",
        "donor_rendering": "parser renders donor spelling and diagnostics through donor alias registry when applicable",
        "error_semantics": "invalid arity refuses with SBSQL.FUNCTION.INVALID_INPUT",
        "syntax_forms": ["function_call"],
        "conformance_cases": ["SBSFC032-similar-to-escape-bare-invalid", "SBSFC032-similar-to-escape-text"],
    },
    "SBSQL-254D0D3E1F58": "SBSQL-BC1A67FBC111",
    "SBSQL-161D7B3339E9": {
        "builtin_id": "sb.scalar.value_state",
        "canonical_name": "value_state",
        "overloads": [{"signature": "value_state(any)", "argument_rule": "one scalar expression"}],
        "return_type_rule": "character state name for SBLR payload kind and SQL null state",
        "coercion_rule": "argument is inspected without value coercion",
        "null_behavior": "null argument reports sql_null",
        "collation_charset_rule": "implementation default character descriptor",
        "timezone_rule": "not applicable",
        "volatility": "immutable",
        "determinism": "descriptor-backed deterministic",
        "side_effects": "none",
        "sblr_binding": "sblr.expr.scalar_value_state.v3",
        "ast_binding": "ast.expr.scalar_value_state",
        "engine_entrypoint": "value_state",
        "optimizer_properties": {"foldable": False, "index_eligible": False, "generated_column_eligible": False, "cost_class": "cpu_scalar"},
        "security_policy": "reads only already-bound value payload metadata",
        "donor_rendering": "parser renders donor spelling and diagnostics through donor alias registry when applicable",
        "error_semantics": "invalid arity refuses with SBSQL.FUNCTION.INVALID_INPUT",
        "syntax_forms": ["function_call"],
        "conformance_cases": ["SBSFC032-value-state-bare-invalid", "SBSFC032-value-state-any"],
    },
    "SBSQL-A442EACDA177": "SBSQL-161D7B3339E9",
}


def _sbsfc033_oracle_record(
    builtin_id: str,
    canonical_name: str,
    signature: str,
    argument_rule: str,
    return_type_rule: str,
    null_behavior: str,
    sblr_binding: str,
    ast_binding: str,
    engine_entrypoint: str,
    conformance_case: str,
) -> dict:
    return {
        "builtin_id": builtin_id,
        "canonical_name": canonical_name,
        "overloads": [{"signature": signature, "argument_rule": argument_rule}],
        "return_type_rule": return_type_rule,
        "coercion_rule": "literal arguments are interpreted as descriptor/catalog/diagnostic text without parser-side execution",
        "null_behavior": null_behavior,
        "collation_charset_rule": "implementation default character descriptor when result is textual",
        "timezone_rule": "not applicable",
        "volatility": "stable",
        "determinism": "deterministic within SblrExecutionContext",
        "side_effects": "none",
        "sblr_binding": sblr_binding,
        "ast_binding": ast_binding,
        "engine_entrypoint": engine_entrypoint,
        "optimizer_properties": {
            "foldable": False,
            "index_eligible": False,
            "generated_column_eligible": False,
            "cost_class": "context_metadata_scalar",
        },
        "security_policy": (
            "reads only SblrExecutionContext metadata or already-bound compact descriptor metadata; "
            "no parser SQL execution, donor execution, storage lookup, mutation, or transaction-finality authority"
        ),
        "donor_rendering": "parser renders native SBsql spelling and diagnostics through generated registry evidence",
        "error_semantics": "invalid arity refuses with SBSQL.FUNCTION.INVALID_INPUT; unknown catalog UUID/name probes return SQL NULL",
        "syntax_forms": ["function_call"],
        "conformance_cases": [conformance_case],
    }


SBSFC033_CATALOG_DESCRIPTOR_DIAGNOSTIC_ORACLE_OVERRIDES = {
    "SBSQL-06BFD87D6529": _sbsfc033_oracle_record(
        "sb.scalar.catalog_object_owner", "catalog_object_owner", "catalog_object_owner()",
        "zero arguments; current database is the implicit catalog object",
        "uuid owner UUID or SQL null when no owner is present",
        "unknown or null object UUID returns SQL null uuid",
        "sblr.expr.scalar_catalog_object_owner.v3", "ast.expr.scalar_catalog_object_owner",
        "catalog_object_owner", "SBSFC033-catalog-object-owner-bare"),
    "SBSQL-D26A0353E396": _sbsfc033_oracle_record(
        "sb.scalar.catalog_object_owner", "catalog_object_owner", "catalog_object_owner(uuid)",
        "one UUID argument naming a context-backed catalog object",
        "uuid owner UUID or SQL null when no owner is present",
        "unknown or null object UUID returns SQL null uuid",
        "sblr.expr.scalar_catalog_object_owner.v3", "ast.expr.scalar_catalog_object_owner",
        "catalog_object_owner", "SBSFC033-catalog-object-owner-uuid"),
    "SBSQL-1C8329808D20": _sbsfc033_oracle_record(
        "sb.scalar.catalog_object_uuid", "catalog_object_uuid", "catalog_object_uuid()",
        "zero arguments; current database is the implicit catalog object",
        "uuid current database UUID or SQL null when absent",
        "empty database UUID returns SQL null uuid",
        "sblr.expr.scalar_catalog_object_uuid.v3", "ast.expr.scalar_catalog_object_uuid",
        "catalog_object_uuid", "SBSFC033-catalog-object-uuid-bare"),
    "SBSQL-400622501328": _sbsfc033_oracle_record(
        "sb.scalar.catalog_object_uuid", "catalog_object_uuid", "catalog_object_uuid(name[,object_class])",
        "one name argument and optional object-class filter",
        "uuid object UUID or SQL null for unknown name/class",
        "unknown name/class or null input returns SQL null uuid",
        "sblr.expr.scalar_catalog_object_uuid.v3", "ast.expr.scalar_catalog_object_uuid",
        "catalog_object_uuid", "SBSFC033-catalog-object-uuid-name-class"),
    "SBSQL-99BB305208AD": _sbsfc033_oracle_record(
        "sb.scalar.catalog_object_name", "catalog_object_name", "catalog_object_name()",
        "zero arguments; current database is the implicit catalog object",
        "character catalog object name",
        "missing implicit object returns SQL null character",
        "sblr.expr.scalar_catalog_object_name.v3", "ast.expr.scalar_catalog_object_name",
        "catalog_object_name", "SBSFC033-catalog-object-name-bare"),
    "SBSQL-61072961FEDB": _sbsfc033_oracle_record(
        "sb.scalar.catalog_object_name", "catalog_object_name", "catalog_object_name(uuid)",
        "one UUID argument naming a context-backed catalog object",
        "character catalog object name or SQL null for unknown UUID",
        "unknown or null object UUID returns SQL null character",
        "sblr.expr.scalar_catalog_object_name.v3", "ast.expr.scalar_catalog_object_name",
        "catalog_object_name", "SBSFC033-catalog-object-name-uuid"),
    "SBSQL-E1CB2F1D2656": _sbsfc033_oracle_record(
        "sb.scalar.catalog_object_class", "catalog_object_class", "catalog_object_class()",
        "zero arguments; current database is the implicit catalog object",
        "character catalog object class",
        "missing implicit object returns SQL null character",
        "sblr.expr.scalar_catalog_object_class.v3", "ast.expr.scalar_catalog_object_class",
        "catalog_object_class", "SBSFC033-catalog-object-class-bare"),
    "SBSQL-A9F113392815": _sbsfc033_oracle_record(
        "sb.scalar.catalog_object_class", "catalog_object_class", "catalog_object_class(uuid)",
        "one UUID argument naming a context-backed catalog object",
        "character catalog object class or SQL null for unknown UUID",
        "unknown or null object UUID returns SQL null character",
        "sblr.expr.scalar_catalog_object_class.v3", "ast.expr.scalar_catalog_object_class",
        "catalog_object_class", "SBSFC033-catalog-object-class-uuid"),
    "SBSQL-352707AC1CAE": _sbsfc033_oracle_record(
        "sb.scalar.descriptor_snapshot_id", "descriptor_snapshot_id", "descriptor_snapshot_id()",
        "zero arguments",
        "uuid descriptor/security snapshot UUID or SQL null when absent",
        "absent snapshot UUID returns SQL null uuid",
        "sblr.expr.scalar_descriptor_snapshot_id.v3", "ast.expr.scalar_descriptor_snapshot_id",
        "descriptor_snapshot_id", "SBSFC033-descriptor-snapshot-id"),
    "SBSQL-BE412B3728C3": _sbsfc033_oracle_record(
        "sb.scalar.execution_type_descriptor", "ExecutionTypeDescriptor", "ExecutionTypeDescriptor()",
        "zero arguments",
        "json_document compact execution descriptor derived from SblrExecutionContext",
        "returns a descriptor JSON document for every admitted call",
        "sblr.expr.scalar_execution_type_descriptor.v3", "ast.expr.scalar_execution_type_descriptor",
        "execution_type_descriptor", "SBSFC033-execution-type-descriptor"),
    "SBSQL-62343F602D38": _sbsfc033_oracle_record(
        "sb.scalar.column_descriptor", "column_descriptor", "column_descriptor(table_uuid,column_name)",
        "table UUID plus column name for context-backed descriptor probes",
        "json_document compact column descriptor or SQL null for unknown table/column",
        "unknown table/column or null input returns SQL null json_document",
        "sblr.expr.scalar_column_descriptor.v3", "ast.expr.scalar_column_descriptor",
        "column_descriptor", "SBSFC033-column-descriptor-table-column"),
    "SBSQL-67431DA0E42F": _sbsfc033_oracle_record(
        "sb.scalar.column_descriptor", "column_descriptor", "column_descriptor()",
        "zero arguments",
        "json_document descriptor summary for context-backed columns",
        "returns a descriptor summary JSON document for every admitted call",
        "sblr.expr.scalar_column_descriptor.v3", "ast.expr.scalar_column_descriptor",
        "column_descriptor", "SBSFC033-column-descriptor-bare"),
    "SBSQL-95EA30EEFDEB": _sbsfc033_oracle_record(
        "sb.scalar.index_descriptor", "index_descriptor", "index_descriptor()",
        "zero arguments",
        "json_document deterministic unresolved index descriptor summary",
        "returns a descriptor summary JSON document for every admitted bare call",
        "sblr.expr.scalar_index_descriptor.v3", "ast.expr.scalar_index_descriptor",
        "index_descriptor", "SBSFC033-index-descriptor-bare"),
    "SBSQL-A209C2B5CDDD": _sbsfc033_oracle_record(
        "sb.scalar.index_descriptor", "index_descriptor", "index_descriptor(index_uuid)",
        "one UUID argument naming an index descriptor",
        "json_document index descriptor or SQL null for unknown UUID",
        "unknown or null index UUID returns SQL null json_document",
        "sblr.expr.scalar_index_descriptor.v3", "ast.expr.scalar_index_descriptor",
        "index_descriptor", "SBSFC033-index-descriptor-unknown"),
    "SBSQL-780AF496F174": _sbsfc033_oracle_record(
        "sb.scalar.diagnostic_field", "diagnostic_field", "diagnostic_field()",
        "zero arguments; current diagnostic id is the implicit field",
        "character diagnostic id or SQL null when no diagnostic is active",
        "absent diagnostic id returns SQL null character",
        "sblr.expr.scalar_diagnostic_field.v3", "ast.expr.scalar_diagnostic_field",
        "diagnostic_field", "SBSFC033-diagnostic-field-bare"),
    "SBSQL-6A13011127CF": _sbsfc033_oracle_record(
        "sb.scalar.diagnostic_field", "diagnostic_field", "diagnostic_field(name)",
        "one diagnostic field name argument",
        "character diagnostic field text or SQL null for unknown/absent field",
        "unknown field or absent value returns SQL null character",
        "sblr.expr.scalar_diagnostic_field.v3", "ast.expr.scalar_diagnostic_field",
        "diagnostic_field", "SBSFC033-diagnostic-field-name"),
    "SBSQL-78B6ABBE922C": _sbsfc033_oracle_record(
        "sb.scalar.diagnostic_count", "diagnostic_count", "diagnostic_count()",
        "zero arguments",
        "uint64 count of active diagnostic records visible in SblrExecutionContext",
        "no active diagnostic returns uint64 0",
        "sblr.expr.scalar_diagnostic_count.v3", "ast.expr.scalar_diagnostic_count",
        "diagnostic_count", "SBSFC033-diagnostic-count"),
    "SBSQL-0D860B4A13B7": _sbsfc033_oracle_record(
        "sb.scalar.gdscode", "gdscode", "gdscode()",
        "zero arguments",
        "int64 deterministic GDS-code compatibility value for current diagnostic state",
        "no active diagnostic returns int64 0",
        "sblr.expr.scalar_gdscode.v3", "ast.expr.scalar_gdscode",
        "gdscode", "SBSFC033-gdscode"),
    "SBSQL-E00EAE7EDC3C": _sbsfc033_oracle_record(
        "sb.scalar.last_error_position", "last_error_position", "last_error_position()",
        "zero arguments",
        "int64 last error position or SQL null when no position is attached",
        "current implementation returns SQL null int64 when no position metadata exists",
        "sblr.expr.scalar_last_error_position.v3", "ast.expr.scalar_last_error_position",
        "last_error_position", "SBSFC033-last-error-position"),
    "SBSQL-E36B1B028CC2": _sbsfc033_oracle_record(
        "sb.scalar.error_class", "error_class", "error_class()",
        "zero arguments",
        "character canonical diagnostic class derived from current SQLSTATE",
        "SQLSTATE 00000 or absent SQLSTATE maps to successful_completion",
        "sblr.expr.scalar_error_class.v3", "ast.expr.scalar_error_class",
        "error_class", "SBSFC033-error-class"),
}


def _sbsfc044_oracle_record(
    builtin_id: str,
    canonical_name: str,
    signature: str,
    argument_rule: str,
    return_type_rule: str,
    null_behavior: str,
    sblr_binding: str,
    ast_binding: str,
    engine_entrypoint: str,
    conformance_case: str,
) -> dict:
    return {
        "builtin_id": builtin_id,
        "canonical_name": canonical_name,
        "overloads": [{"signature": signature, "argument_rule": argument_rule}],
        "return_type_rule": return_type_rule,
        "coercion_rule": "table_uuid arguments are UUID scalar descriptors; include_indexes is boolean-compatible when present",
        "null_behavior": null_behavior,
        "collation_charset_rule": "not applicable",
        "timezone_rule": "not applicable",
        "volatility": "stable",
        "determinism": "deterministic within the active MGA transaction snapshot",
        "side_effects": "none",
        "sblr_binding": sblr_binding,
        "ast_binding": ast_binding,
        "engine_entrypoint": engine_entrypoint,
        "optimizer_properties": {
            "foldable": False,
            "index_eligible": False,
            "generated_column_eligible": False,
            "cost_class": "catalog_metadata_io",
        },
        "security_policy": (
            "reads engine-owned MGA relation metadata, row-version sidecars, and index sidecars; "
            "no parser SQL execution, donor execution, mutation, WAL/recovery shortcut, SQLite shortcut, or cluster authority"
        ),
        "donor_rendering": "parser renders native SBsql spelling and diagnostics through generated registry evidence",
        "error_semantics": "invalid arity or invalid include_indexes refuses with SBSQL.FUNCTION.INVALID_INPUT; null or unknown table_uuid returns SQL NULL uint64",
        "syntax_forms": ["function_call"],
        "conformance_cases": [conformance_case],
    }


SBSFC044_CATALOG_STATISTICS_ORACLE_OVERRIDES = {
    "SBSQL-92A5BAB00707": _sbsfc044_oracle_record(
        "sb.scalar.relation_row_estimate",
        "relation_row_estimate",
        "relation_row_estimate()",
        "zero arguments; current database catalog is the implicit scope",
        "uint64 visible row estimate summed across visible local MGA relations",
        "not applicable for the zero-argument form",
        "sblr.expr.scalar_relation_row_estimate.v3",
        "ast.expr.scalar_relation_row_estimate",
        "relation_row_estimate",
        "SBSFC044-relation-row-estimate-catalog",
    ),
    "SBSQL-545ECE804256": _sbsfc044_oracle_record(
        "sb.scalar.relation_row_estimate",
        "relation_row_estimate",
        "relation_row_estimate(table_uuid)",
        "one UUID argument naming a relation in the engine-owned MGA relation store",
        "uint64 visible row estimate for the relation",
        "null table_uuid returns SQL NULL uint64; unknown table_uuid returns SQL NULL uint64",
        "sblr.expr.scalar_relation_row_estimate.v3",
        "ast.expr.scalar_relation_row_estimate",
        "relation_row_estimate",
        "SBSFC044-relation-row-estimate-table",
    ),
    "SBSQL-84CF6043F00F": _sbsfc044_oracle_record(
        "sb.scalar.table_size",
        "table_size",
        "table_size()",
        "zero arguments; current database catalog is the implicit scope and includes indexes",
        "uint64 stable byte estimate derived from visible MGA relation metadata and row-version/index sidecars",
        "not applicable for the zero-argument form",
        "sblr.expr.scalar_table_size.v3",
        "ast.expr.scalar_table_size",
        "table_size",
        "SBSFC044-table-size-catalog",
    ),
    "SBSQL-C167CB99BFAD": _sbsfc044_oracle_record(
        "sb.scalar.table_size",
        "table_size",
        "table_size(table_uuid[,include_indexes])",
        "one UUID relation argument plus optional boolean include_indexes flag defaulting true",
        "uint64 stable byte estimate for relation metadata, row versions, and optionally indexes",
        "null table_uuid or null include_indexes returns SQL NULL uint64; unknown table_uuid returns SQL NULL uint64",
        "sblr.expr.scalar_table_size.v3",
        "ast.expr.scalar_table_size",
        "table_size",
        "SBSFC044-table-size-table-default",
    ),
}


def _sbsfc046_oracle_record(
    builtin_id: str,
    canonical_name: str,
    signature: str,
    argument_rule: str,
    return_type_rule: str,
    null_behavior: str,
    side_effects: str,
    sblr_binding: str,
    ast_binding: str,
    engine_entrypoint: str,
    conformance_case: str,
) -> dict:
    return {
        "builtin_id": builtin_id,
        "canonical_name": canonical_name,
        "overloads": [{"signature": signature, "argument_rule": argument_rule}],
        "return_type_rule": return_type_rule,
        "coercion_rule": "pid arguments are positive int64-compatible scalars; setting names and values are character-compatible scalars; is_local is boolean-compatible",
        "null_behavior": null_behavior,
        "collation_charset_rule": "text results use the session character descriptor",
        "timezone_rule": "timezone is a session config name and does not consult donor timezone authority",
        "volatility": "volatile",
        "determinism": "deterministic within the supplied SBLR session runtime context except for process-id fallback",
        "side_effects": side_effects,
        "sblr_binding": sblr_binding,
        "ast_binding": ast_binding,
        "engine_entrypoint": engine_entrypoint,
        "optimizer_properties": {
            "foldable": False,
            "index_eligible": False,
            "generated_column_eligible": False,
            "cost_class": "session_context",
        },
        "security_policy": (
            "uses SBLR local session runtime config and backend-control evidence vectors only; "
            "unknown backend pids return false, current-backend termination is blocked, and no parser SQL, donor execution, WAL/recovery shortcut, SQLite shortcut, cluster provider authority, or process kill is used"
        ),
        "donor_rendering": "parser renders native SBsql spelling and diagnostics through generated registry evidence",
        "error_semantics": "invalid arity, invalid pid, invalid boolean is_local, or invalid temp_buffers value refuses with SBSQL.FUNCTION.INVALID_INPUT",
        "syntax_forms": ["function_call"],
        "conformance_cases": [conformance_case],
    }


SBSFC046_SESSION_ADMIN_ORACLE_OVERRIDES = {
    "SBSQL-293E7F73D57E": _sbsfc046_oracle_record(
        "sb.scalar.set_config",
        "set_config",
        "set_config()",
        "zero arguments; family marker route evidence only",
        "character marker naming the local session config route",
        "not applicable for the zero-argument marker form",
        "none beyond route evidence",
        "sblr.expr.scalar_set_config.v3",
        "ast.expr.scalar_set_config",
        "set_config",
        "SBSFC046-set-config-marker",
    ),
    "SBSQL-5F6C7C137D61": _sbsfc046_oracle_record(
        "sb.scalar.set_config_name_value_is_local",
        "set_config",
        "set_config(name,value,is_local)",
        "name and value are character-compatible, is_local is boolean-compatible",
        "character applied value",
        "SQL NULL name, value, or is_local returns SQL NULL character",
        "mutates SBLR local session config state",
        "sblr.expr.scalar_set_config.v3",
        "ast.expr.scalar_set_config",
        "set_config",
        "SBSFC046-set-config-apply-timezone",
    ),
    "SBSQL-68FAFF2DC5B5": _sbsfc046_oracle_record(
        "sb.scalar.pid",
        "pid",
        "pid()",
        "zero arguments",
        "uint64 positive backend process identifier",
        "not applicable for the zero-argument form",
        "none",
        "sblr.expr.scalar_pid.v3",
        "ast.expr.scalar_pid",
        "pid",
        "SBSFC046-pid-positive",
    ),
    "SBSQL-8AEA24D52907": _sbsfc046_oracle_record(
        "sb.scalar.temp_buffers",
        "temp_buffers",
        "temp_buffers()",
        "zero arguments",
        "uint64 effective session temp buffer byte setting",
        "not applicable for the zero-argument form",
        "none",
        "sblr.expr.scalar_temp_buffers.v3",
        "ast.expr.scalar_temp_buffers",
        "temp_buffers",
        "SBSFC046-temp-buffers-default",
    ),
    "SBSQL-A9980035E562": _sbsfc046_oracle_record(
        "sb.scalar.pg_cancel_backend",
        "pg_cancel_backend",
        "pg_cancel_backend()",
        "zero arguments; marker route evidence only",
        "boolean false",
        "not applicable for the zero-argument marker form",
        "appends local backend-control marker evidence",
        "sblr.expr.scalar_pg_cancel_backend.v3",
        "ast.expr.scalar_pg_cancel_backend",
        "pg_cancel_backend",
        "SBSFC046-pg-cancel-backend-marker",
    ),
    "SBSQL-448BCEA87184": _sbsfc046_oracle_record(
        "sb.scalar.pg_cancel_backend_pid",
        "pg_cancel_backend",
        "pg_cancel_backend(pid)",
        "one positive int64-compatible pid argument",
        "boolean request result",
        "SQL NULL pid returns SQL NULL boolean",
        "appends local backend-control cancellation evidence for current or registered pids",
        "sblr.expr.scalar_pg_cancel_backend.v3",
        "ast.expr.scalar_pg_cancel_backend",
        "pg_cancel_backend",
        "SBSFC046-pg-cancel-backend-unknown",
    ),
    "SBSQL-33BC7C3DE12B": _sbsfc046_oracle_record(
        "sb.scalar.pg_terminate_backend",
        "pg_terminate_backend",
        "pg_terminate_backend()",
        "zero arguments; marker route evidence only",
        "boolean false",
        "not applicable for the zero-argument marker form",
        "appends local backend-control marker evidence",
        "sblr.expr.scalar_pg_terminate_backend.v3",
        "ast.expr.scalar_pg_terminate_backend",
        "pg_terminate_backend",
        "SBSFC046-pg-terminate-backend-marker",
    ),
    "SBSQL-B81698E45C53": _sbsfc046_oracle_record(
        "sb.scalar.pg_terminate_backend_pid",
        "pg_terminate_backend",
        "pg_terminate_backend(pid)",
        "one positive int64-compatible pid argument",
        "boolean request result",
        "SQL NULL pid returns SQL NULL boolean",
        "appends local backend-control termination evidence for registered pids and blocks current-backend termination",
        "sblr.expr.scalar_pg_terminate_backend.v3",
        "ast.expr.scalar_pg_terminate_backend",
        "pg_terminate_backend",
        "SBSFC046-pg-terminate-backend-unknown",
    ),
}


def _sbsfc047_oracle_record(
    builtin_id: str,
    canonical_name: str,
    signature: str,
    argument_rule: str,
    return_type_rule: str,
    null_behavior: str,
    side_effects: str,
    sblr_binding: str,
    ast_binding: str,
    engine_entrypoint: str,
    conformance_case: str,
) -> dict:
    return {
        "builtin_id": builtin_id,
        "canonical_name": canonical_name,
        "overloads": [{"signature": signature, "argument_rule": argument_rule}],
        "return_type_rule": return_type_rule,
        "coercion_rule": "advisory lock keys are int64-compatible scalar values",
        "null_behavior": null_behavior,
        "collation_charset_rule": "text marker results use the session character descriptor",
        "timezone_rule": "not applicable",
        "volatility": "volatile",
        "determinism": "deterministic within the supplied SBLR session runtime advisory-lock state",
        "side_effects": side_effects,
        "sblr_binding": sblr_binding,
        "ast_binding": ast_binding,
        "engine_entrypoint": engine_entrypoint,
        "optimizer_properties": {
            "foldable": False,
            "index_eligible": False,
            "generated_column_eligible": False,
            "cost_class": "session_context",
        },
        "security_policy": (
            "uses bounded SBLR local session advisory-lock state and evidence vectors only; "
            "current-session ownership is reentrant, try-lock conflicts with another bounded owner return false, "
            "pg_advisory_lock(key) is bounded and nonblocking, and no parser SQL, donor execution, WAL/recovery shortcut, SQLite shortcut, cluster provider authority, external lock manager, or transaction finality change is used"
        ),
        "donor_rendering": "parser renders native SBsql spelling and diagnostics through generated registry evidence",
        "error_semantics": "invalid arity, invalid advisory lock key, or missing session runtime state refuses with SBSQL.FUNCTION.INVALID_INPUT",
        "syntax_forms": ["function_call"],
        "conformance_cases": [conformance_case],
    }


SBSFC047_ADVISORY_LOCK_ORACLE_OVERRIDES = {
    "SBSQL-0FE0C54DA428": _sbsfc047_oracle_record(
        "sb.scalar.pg_advisory_lock",
        "pg_advisory_lock",
        "pg_advisory_lock()",
        "zero arguments; marker route evidence only",
        "character marker naming the local session advisory lock route",
        "not applicable for the zero-argument marker form",
        "appends local advisory-lock marker evidence",
        "sblr.expr.scalar_pg_advisory_lock.v3",
        "ast.expr.scalar_pg_advisory_lock",
        "pg_advisory_lock",
        "SBSFC047-pg-advisory-lock-marker",
    ),
    "SBSQL-9F181B57B169": _sbsfc047_oracle_record(
        "sb.scalar.pg_advisory_lock_key",
        "pg_advisory_lock",
        "pg_advisory_lock(key)",
        "one int64-compatible advisory lock key",
        "boolean true when acquired or reentrant in bounded local runtime, false if bounded state already marks another owner",
        "SQL NULL key returns SQL NULL boolean",
        "mutates SBLR local session advisory-lock state and appends acquisition evidence",
        "sblr.expr.scalar_pg_advisory_lock.v3",
        "ast.expr.scalar_pg_advisory_lock",
        "pg_advisory_lock",
        "SBSFC047-pg-advisory-lock-acquire",
    ),
    "SBSQL-5B10D7D3D141": _sbsfc047_oracle_record(
        "sb.scalar.pg_try_advisory_lock",
        "pg_try_advisory_lock",
        "pg_try_advisory_lock()",
        "zero arguments; marker route evidence only",
        "boolean false marker",
        "not applicable for the zero-argument marker form",
        "appends local advisory-lock marker evidence",
        "sblr.expr.scalar_pg_try_advisory_lock.v3",
        "ast.expr.scalar_pg_try_advisory_lock",
        "pg_try_advisory_lock",
        "SBSFC047-pg-try-advisory-lock-marker",
    ),
    "SBSQL-5EEA3058CDC1": _sbsfc047_oracle_record(
        "sb.scalar.pg_try_advisory_lock_key",
        "pg_try_advisory_lock",
        "pg_try_advisory_lock(key)",
        "one int64-compatible advisory lock key",
        "boolean true when acquired or already owned by the current SBLR session, false when bounded state marks another owner",
        "SQL NULL key returns SQL NULL boolean",
        "mutates SBLR local session advisory-lock state when acquired and appends acquisition or other-owner evidence",
        "sblr.expr.scalar_pg_try_advisory_lock.v3",
        "ast.expr.scalar_pg_try_advisory_lock",
        "pg_try_advisory_lock",
        "SBSFC047-pg-try-advisory-lock-acquire",
    ),
}


def _sbsfc048_oracle_record(
    builtin_id: str,
    canonical_name: str,
    signature: str,
    argument_rule: str,
    return_type_rule: str,
    null_behavior: str,
    side_effects: str,
    sblr_binding: str,
    ast_binding: str,
    engine_entrypoint: str,
    conformance_case: str,
) -> dict:
    return {
        "builtin_id": builtin_id,
        "canonical_name": canonical_name,
        "overloads": [{"signature": signature, "argument_rule": argument_rule}],
        "return_type_rule": return_type_rule,
        "coercion_rule": "advisory lock keys are int64-compatible scalar values",
        "null_behavior": null_behavior,
        "collation_charset_rule": "text marker results use the session character descriptor",
        "timezone_rule": "not applicable",
        "volatility": "volatile",
        "determinism": "deterministic within the supplied SBLR local advisory-lock runtime state",
        "side_effects": side_effects,
        "sblr_binding": sblr_binding,
        "ast_binding": ast_binding,
        "engine_entrypoint": engine_entrypoint,
        "optimizer_properties": {
            "foldable": False,
            "index_eligible": False,
            "generated_column_eligible": False,
            "cost_class": "session_context",
        },
        "security_policy": (
            "uses bounded SBLR local advisory-lock state and evidence vectors only; "
            "session unlock affects only local session advisory locks, transaction-scope locks use separate transaction-token state, "
            "and no parser SQL, donor execution, WAL/recovery shortcut, SQLite shortcut, cluster provider authority, external lock manager, or transaction finality change is used"
        ),
        "donor_rendering": "parser renders native SBsql spelling and diagnostics through generated registry evidence",
        "error_semantics": "invalid arity, invalid advisory lock key, missing session state, or missing transaction context refuses with SBSQL.FUNCTION.INVALID_INPUT",
        "syntax_forms": ["function_call"],
        "conformance_cases": [conformance_case],
    }


SBSFC048_ADVISORY_LOCK_RELEASE_ORACLE_OVERRIDES = {
    "SBSQL-0BD07FFF239C": _sbsfc048_oracle_record(
        "sb.scalar.pg_advisory_unlock",
        "pg_advisory_unlock",
        "pg_advisory_unlock()",
        "zero arguments; marker route evidence only",
        "boolean false marker",
        "not applicable for the zero-argument marker form",
        "appends local session advisory-lock unlock marker evidence",
        "sblr.expr.scalar_pg_advisory_unlock.v3",
        "ast.expr.scalar_pg_advisory_unlock",
        "pg_advisory_unlock",
        "SBSFC048-pg-advisory-unlock-marker",
    ),
    "SBSQL-B1E36E69C90C": _sbsfc048_oracle_record(
        "sb.scalar.pg_advisory_unlock_key",
        "pg_advisory_unlock",
        "pg_advisory_unlock(key)",
        "one int64-compatible advisory lock key",
        "boolean true when the current SBLR session releases an owned lock, false when absent or owned by another bounded session",
        "SQL NULL key returns SQL NULL boolean",
        "decrements or removes SBLR local session advisory-lock state and appends release evidence",
        "sblr.expr.scalar_pg_advisory_unlock.v3",
        "ast.expr.scalar_pg_advisory_unlock",
        "pg_advisory_unlock",
        "SBSFC048-pg-advisory-unlock-release-final",
    ),
    "SBSQL-CFBF100BB5D1": _sbsfc048_oracle_record(
        "sb.scalar.pg_advisory_xact_lock",
        "pg_advisory_xact_lock",
        "pg_advisory_xact_lock()",
        "zero arguments; marker route evidence only",
        "character marker naming the local transaction advisory lock route",
        "not applicable for the zero-argument marker form",
        "appends local transaction advisory-lock marker evidence",
        "sblr.expr.scalar_pg_advisory_xact_lock.v3",
        "ast.expr.scalar_pg_advisory_xact_lock",
        "pg_advisory_xact_lock",
        "SBSFC048-pg-advisory-xact-lock-marker",
    ),
    "SBSQL-3956A1AD000E": _sbsfc048_oracle_record(
        "sb.scalar.pg_advisory_xact_lock_key",
        "pg_advisory_xact_lock",
        "pg_advisory_xact_lock(key)",
        "one int64-compatible advisory lock key plus SBLR transaction context",
        "boolean true when acquired or reentrant for the current SBLR transaction token, false when bounded state marks another transaction owner",
        "SQL NULL key returns SQL NULL boolean",
        "mutates separate SBLR local transaction advisory-lock state and appends acquisition evidence",
        "sblr.expr.scalar_pg_advisory_xact_lock.v3",
        "ast.expr.scalar_pg_advisory_xact_lock",
        "pg_advisory_xact_lock",
        "SBSFC048-pg-advisory-xact-lock-acquire",
    ),
}


def _sbsql_miss_gate_011_lock_oracle_record(
    builtin_id: str,
    canonical_name: str,
    signature: str,
    argument_rule: str,
    return_type_rule: str,
    null_behavior: str,
    side_effects: str,
    sblr_binding: str,
    ast_binding: str,
    engine_entrypoint: str,
    conformance_case: str,
) -> dict:
    return {
        "builtin_id": builtin_id,
        "canonical_name": canonical_name,
        "overloads": [{"signature": signature, "argument_rule": argument_rule}],
        "return_type_rule": return_type_rule,
        "coercion_rule": (
            "lock names are text-compatible scalar values; timeout arguments are "
            "int64-compatible bounded second counts"
        ),
        "null_behavior": null_behavior,
        "collation_charset_rule": "lock names use exact bounded UTF-8 scalar bytes; no collation comparison is authority",
        "timezone_rule": "not applicable",
        "volatility": "volatile",
        "determinism": "deterministic within the supplied SBLR local session advisory-lock runtime state",
        "side_effects": side_effects,
        "sblr_binding": sblr_binding,
        "ast_binding": ast_binding,
        "engine_entrypoint": engine_entrypoint,
        "optimizer_properties": {
            "foldable": False,
            "index_eligible": False,
            "generated_column_eligible": False,
            "cost_class": "session_context",
        },
        "security_policy": (
            "uses bounded SBLR local session advisory-lock state and evidence vectors only; "
            "returns donor-compatible int64/NULL result shape; no parser SQL execution, donor "
            "execution, WAL/recovery shortcut, SQLite shortcut, table/row/page lock authority, "
            "external lock manager, cluster provider behavior, MGA visibility change, cleanup "
            "horizon pin, or transaction finality change is used"
        ),
        "donor_rendering": "parser renders native SBsql spelling and donor-compatible integer/NULL result shape through generated registry evidence",
        "error_semantics": "invalid arity, invalid timeout, or missing session runtime state refuses with SBSQL.FUNCTION.INVALID_INPUT",
        "syntax_forms": ["function_call"],
        "conformance_cases": [conformance_case],
    }


SBSQL_MISS_GATE_011_LOCK_ORACLE_OVERRIDES = {
    "SBSQL-6E2D0E0B0110": _sbsql_miss_gate_011_lock_oracle_record(
        "sb.scalar.get_lock",
        "get_lock",
        "get_lock(name,timeout)",
        "one text-compatible lock name plus one int64-compatible bounded timeout in seconds",
        "int64 1 when acquired or reentrant, 0 when timeout/other-owner, SQL NULL on SQL NULL argument",
        "SQL NULL name or timeout returns SQL NULL int64",
        "mutates bounded SBLR local session advisory-lock state when acquired and appends acquisition evidence",
        "sblr.expr.scalar_get_lock.v3",
        "ast.expr.scalar_get_lock",
        "get_lock",
        "SBSQL-MISS-011-get-lock-acquire-release",
    ),
    "SBSQL-6E2D0E0B0111": _sbsql_miss_gate_011_lock_oracle_record(
        "sb.scalar.release_lock",
        "release_lock",
        "release_lock(name)",
        "one text-compatible lock name",
        "int64 1 when released/decremented/final, 0 when not owned or not found, SQL NULL on SQL NULL name",
        "SQL NULL name returns SQL NULL int64",
        "decrements or removes bounded SBLR local session advisory-lock state and appends release evidence",
        "sblr.expr.scalar_release_lock.v3",
        "ast.expr.scalar_release_lock",
        "release_lock",
        "SBSQL-MISS-011-get-lock-acquire-release",
    ),
}


def _sbsfc049_oracle_record(
    builtin_id: str,
    canonical_name: str,
    signature: str,
    argument_rule: str,
    return_type_rule: str,
    null_behavior: str,
    side_effects: str,
    sblr_binding: str,
    ast_binding: str,
    engine_entrypoint: str,
    conformance_case: str,
) -> dict:
    return {
        "builtin_id": builtin_id,
        "canonical_name": canonical_name,
        "overloads": [{"signature": signature, "argument_rule": argument_rule}],
        "return_type_rule": return_type_rule,
        "coercion_rule": "text arguments are UTF-8 character scalar values; normalization form is NFC, NFD, NFKC, or NFKD",
        "null_behavior": null_behavior,
        "collation_charset_rule": "Unicode text is normalized or classified by ICU codepoint semantics and returned with the session character descriptor",
        "timezone_rule": "not applicable",
        "volatility": "immutable",
        "determinism": "deterministic for the supplied ICU data version and input text",
        "side_effects": side_effects,
        "sblr_binding": sblr_binding,
        "ast_binding": ast_binding,
        "engine_entrypoint": engine_entrypoint,
        "optimizer_properties": {
            "foldable": True,
            "index_eligible": False,
            "generated_column_eligible": True,
            "cost_class": "unicode_text",
        },
        "security_policy": (
            "executes inside the SBLR data-scalar runtime through ICU Unicode normalization/classification APIs; "
            "no parser SQL, donor execution, WAL/recovery shortcut, SQLite shortcut, cluster provider authority, or external service is used"
        ),
        "donor_rendering": "parser renders native SBsql spelling and diagnostics through generated registry evidence",
        "error_semantics": "invalid arity, invalid UTF-8 text, or unsupported normalization form refuses with SBSQL.FUNCTION.INVALID_INPUT",
        "syntax_forms": ["function_call"],
        "conformance_cases": [conformance_case],
    }


SBSFC049_UNICODE_TEXT_ORACLE_OVERRIDES = {
    "SBSQL-2D99FCFA36F6": _sbsfc049_oracle_record(
        "sb.scalar.unicode_normalize",
        "unicode_normalize",
        "unicode_normalize()",
        "zero arguments; marker route evidence only; the same builtin accepts text with default NFC in the runtime",
        "character marker naming the ICU-backed Unicode normalization route",
        "not applicable for the zero-argument marker form; SQL NULL text returns SQL NULL character in the text form",
        "none",
        "sblr.expr.scalar_unicode_normalize.v3",
        "ast.expr.scalar_unicode_normalize",
        "unicode_normalize",
        "SBSFC049-unicode-normalize-marker",
    ),
    "SBSQL-8BA02A81E9E1": _sbsfc049_oracle_record(
        "sb.scalar.normalize",
        "normalize",
        "normalize()",
        "zero arguments; marker route evidence only",
        "character marker naming the ICU-backed Unicode normalization route",
        "not applicable for the zero-argument marker form",
        "none",
        "sblr.expr.scalar_normalize.v3",
        "ast.expr.scalar_normalize",
        "normalize",
        "SBSFC049-normalize-marker",
    ),
    "SBSQL-F74DE55660A7": _sbsfc049_oracle_record(
        "sb.scalar.normalize_text_form",
        "normalize",
        "normalize(text[,NFC|NFD|NFKC|NFKD])",
        "one UTF-8 text argument plus optional Unicode normalization form",
        "character text normalized by ICU",
        "SQL NULL text returns SQL NULL character; SQL NULL form returns SQL NULL character",
        "none",
        "sblr.expr.scalar_normalize.v3",
        "ast.expr.scalar_normalize",
        "normalize",
        "SBSFC049-normalize-nfc-compose",
    ),
    "SBSQL-F1F44C6A450C": _sbsfc049_oracle_record(
        "sb.scalar.is_alpha",
        "is_alpha",
        "is_alpha()",
        "zero arguments marker plus one UTF-8 text argument in the runtime",
        "boolean false marker or boolean true when every input codepoint is alphabetic",
        "not applicable for the zero-argument marker form; SQL NULL text returns SQL NULL boolean",
        "none",
        "sblr.expr.scalar_is_alpha.v3",
        "ast.expr.scalar_is_alpha",
        "is_alpha",
        "SBSFC049-is-alpha-marker",
    ),
}

def _sbsfc052_oracle_record(
    builtin_id: str,
    canonical_name: str,
    signature: str,
    argument_rule: str,
    return_type_rule: str,
    null_behavior: str,
    sblr_binding: str,
    ast_binding: str,
    engine_entrypoint: str,
    conformance_case: str,
) -> dict:
    return {
        "builtin_id": builtin_id,
        "canonical_name": canonical_name,
        "overloads": [{"signature": signature, "argument_rule": argument_rule}],
        "return_type_rule": return_type_rule,
        "coercion_rule": "JSON document inputs are bounded UTF-8 document scalars; scalar inputs are converted through the existing JSON literal conversion rules",
        "null_behavior": null_behavior,
        "collation_charset_rule": "JSON text is emitted as UTF-8 json_document with deterministic key/order preservation for supplied arguments",
        "timezone_rule": "not applicable",
        "volatility": "immutable",
        "determinism": "deterministic for supplied JSON/scalar inputs",
        "side_effects": "none",
        "sblr_binding": sblr_binding,
        "ast_binding": ast_binding,
        "engine_entrypoint": engine_entrypoint,
        "optimizer_properties": {
            "foldable": True,
            "index_eligible": False,
            "generated_column_eligible": True,
            "cost_class": "json_document_scalar",
        },
        "security_policy": (
            "executes inside the SBLR nosql.document runtime using bounded in-core JSON helpers; "
            "no parser SQL, donor execution, storage scan, WAL/recovery shortcut, cluster provider authority, or external service is used"
        ),
        "donor_rendering": "parser renders native SBsql spelling and diagnostics through generated registry evidence",
        "error_semantics": "invalid arity, invalid pretty flag, or malformed text-array json_object input refuses with SBSQL.FUNCTION.INVALID_INPUT",
        "syntax_forms": ["function_call"],
        "conformance_cases": [conformance_case],
    }


SBSFC052_JSON_DOCUMENT_ORACLE_OVERRIDES = {
    "SBSQL-E4C08DADB61A": _sbsfc052_oracle_record(
        "sb.json.table",
        "JSON_TABLE",
        "JSON_TABLE",
        "document plus optional JSON path, column descriptors, and passing descriptors",
        "json_document descriptor payload describing the JSON_TABLE route",
        "SQL NULL document returns SQL NULL json_document",
        "sblr.expr.json_table.v3",
        "ast.expr.json_table",
        "json_table",
        "SBSFC052-json-table",
    ),
    "SBSQL-433AC9801679": _sbsfc052_oracle_record(
        "sb.json.table",
        "JSON_TABLE",
        "JSON_TABLE(doc,path[PASSING...]COLUMNS(...)[ONERROR])",
        "document, path, and bounded descriptor arguments",
        "json_document descriptor payload with counted COLUMNS and PASSING metadata",
        "SQL NULL document or path returns SQL NULL json_document",
        "sblr.expr.json_table.v3",
        "ast.expr.json_table",
        "json_table",
        "SBSFC052-json-table-columns-onerror",
    ),
    "SBSQL-22963E18DC40": _sbsfc052_oracle_record(
        "sb.json.table",
        "JSON_TABLE",
        "JSON_TABLE(document,jsonpathCOLUMNS(...))",
        "document and SQL/JSON path descriptor",
        "json_document descriptor payload with zero or more bounded column descriptors",
        "SQL NULL document or path returns SQL NULL json_document",
        "sblr.expr.json_table.v3",
        "ast.expr.json_table",
        "json_table",
        "SBSFC052-json-table-document-jsonpath",
    ),
    "SBSQL-2866302407B6": _sbsfc052_oracle_record(
        "sb.json.array_to_json",
        "array_to_json",
        "array_to_json",
        "one JSON array/document, array-like descriptor, or scalar argument",
        "json_document array",
        "SQL NULL input returns SQL NULL json_document",
        "sblr.expr.json_array_to_json.v3",
        "ast.expr.json_array_to_json",
        "array_to_json",
        "SBSFC052-array-to-json",
    ),
    "SBSQL-579AE2ED91B2": _sbsfc052_oracle_record(
        "sb.json.array_to_json",
        "array_to_json",
        "array_to_json(array[,pretty])",
        "array/document argument plus optional boolean-compatible pretty flag",
        "json_document array, optionally pretty rendered",
        "SQL NULL input returns SQL NULL json_document; SQL NULL pretty flag refuses as invalid input",
        "sblr.expr.json_array_to_json.v3",
        "ast.expr.json_array_to_json",
        "array_to_json",
        "SBSFC052-array-to-json-pretty",
    ),
    "SBSQL-4DBBCD45F15C": _sbsfc052_oracle_record(
        "sb.json.object_text_array",
        "json_object",
        "json_object(text[][,text[]])",
        "one even-length text array or matching key and value text arrays",
        "json_document object",
        "SQL NULL text-array input returns SQL NULL json_document",
        "sblr.expr.json_object_text_array.v3",
        "ast.expr.json_object_text_array",
        "json_object_text_array",
        "SBSFC052-json-object-text-arrays",
    ),
    "SBSQL-5F35CBE51FA4": _sbsfc052_oracle_record(
        "sb.json.jsonb_agg",
        "jsonb_agg",
        "jsonb_agg",
        "zero or more scalar harness arguments",
        "json_document array with SQL NULL arguments represented as JSON null",
        "zero supplied rows in the scalar harness returns an empty JSON array",
        "sblr.expr.jsonb_agg.v3",
        "ast.expr.jsonb_agg",
        "jsonb_agg",
        "SBSFC052-jsonb-agg",
    ),
    "SBSQL-F9F64D586108": _sbsfc052_oracle_record(
        "sb.json.jsonb_agg",
        "jsonb_agg",
        "jsonb_agg(expr)",
        "one expression argument in the scalar harness",
        "json_document one-element array",
        "SQL NULL expression is represented as JSON null",
        "sblr.expr.jsonb_agg.v3",
        "ast.expr.jsonb_agg",
        "jsonb_agg",
        "SBSFC052-jsonb-agg-expr",
    ),
    "SBSQL-EA3286F7FED5": _sbsfc052_oracle_record(
        "sb.json.row_to_json",
        "row_to_json",
        "row_to_json",
        "one JSON object row document or named scalar field arguments",
        "json_document object",
        "SQL NULL row input returns SQL NULL json_document",
        "sblr.expr.json_row_to_json.v3",
        "ast.expr.json_row_to_json",
        "row_to_json",
        "SBSFC052-row-to-json",
    ),
    "SBSQL-1E99FF5633C4": _sbsfc052_oracle_record(
        "sb.json.row_to_json",
        "row_to_json",
        "row_to_json(row[,pretty])",
        "row/object argument plus optional boolean-compatible pretty flag",
        "json_document object, optionally pretty rendered",
        "SQL NULL row input returns SQL NULL json_document; SQL NULL pretty flag refuses as invalid input",
        "sblr.expr.json_row_to_json.v3",
        "ast.expr.json_row_to_json",
        "row_to_json",
        "SBSFC052-row-to-json-pretty",
    ),
}


def _sbsfc053_oracle_record(
    function_id: str,
    canonical_name: str,
    signature: str,
    argument_rule: str,
    return_type_rule: str,
    sblr_binding: str,
    engine_entrypoint: str,
    proof: str,
) -> dict:
    return {
        "builtin_id": function_id,
        "canonical_name": canonical_name,
        "overloads": [{"signature": signature, "argument_rule": argument_rule}],
        "return_type_rule": return_type_rule,
        "coercion_rule": (
            "rowset/table_value arguments use bounded JSON descriptors; set-returning helpers "
            "emit array JSON payloads; scalar arguments are converted through SBLR scalar encodings"
        ),
        "null_behavior": "SQL NULL descriptor input returns SQL NULL for descriptor-dependent routes; SQL NULL scalar elements are represented as JSON null where rows or arrays are constructed",
        "collation_charset_rule": "descriptor and array payloads are deterministic UTF-8 JSON text; no collation-sensitive comparison except exact multiset token matching",
        "timezone_rule": "not applicable",
        "volatility": "immutable",
        "determinism": "deterministic for supplied descriptor/scalar inputs",
        "side_effects": "none",
        "sblr_binding": sblr_binding,
        "ast_binding": f"ast.expr.{engine_entrypoint}",
        "engine_entrypoint": engine_entrypoint,
        "optimizer_properties": {
            "foldable": False,
            "index_eligible": False,
            "generated_column_eligible": False,
            "cost_class": "rowset_descriptor",
        },
        "security_policy": (
            "executes inside the SBLR rowset.table runtime using bounded in-core descriptor helpers; "
            "no parser SQL, donor execution, storage scan, cursor lifecycle authority, WAL/recovery shortcut, cluster provider authority, or external service is used"
        ),
        "donor_rendering": "parser renders native SBsql spelling and diagnostics through generated registry evidence",
        "error_semantics": "invalid arity, malformed descriptor, scalar non-array unnest, step-zero generate_series, or non-array multiset input refuses with SBSQL.FUNCTION.INVALID_INPUT",
        "syntax_forms": ["function_call", "table_function_call"],
        "conformance_cases": [proof],
    }


SBSFC053_ROWSET_TABLE_VALUE_ORACLE_OVERRIDES = {
    "SBSQL-957EA3F617A2": _sbsfc053_oracle_record("sb.rowset.rowset", "rowset", "rowset()", "zero or one optional row-shape descriptor", "rowset JSON descriptor with zero rows", "sblr.expr.rowset_descriptor.v3", "rowset", "SBSFC053-rowset-marker"),
    "SBSQL-441883FA4E87": _sbsfc053_oracle_record("sb.rowset.new", "rowset_new", "rowset_new([row_shape])", "optional row-shape descriptor", "rowset JSON descriptor with zero rows", "sblr.expr.rowset_new.v3", "rowset_new", "SBSFC053-rowset-new-empty"),
    "SBSQL-A3DADE5255A6": _sbsfc053_oracle_record("sb.rowset.new", "rowset_new(<row_shape>)", "rowset_new(<row_shape>)", "one row-shape descriptor", "rowset JSON descriptor with declared shape and zero rows", "sblr.expr.rowset_new.v3", "rowset_new", "SBSFC053-rowset-new-shape"),
    "SBSQL-4C3F8279098E": _sbsfc053_oracle_record("sb.rowset.append", "rowset_append", "rowset_append(rowset,expr...)", "rowset descriptor and one or more row values", "rowset JSON descriptor with one appended row", "sblr.expr.rowset_append.v3", "rowset_append", "SBSFC053-rowset-append-marker"),
    "SBSQL-1AFC18FA8618": _sbsfc053_oracle_record("sb.rowset.append", "rowset_append(rowset,expr[,expr...])", "rowset_append(rowset,expr[,expr...])", "rowset descriptor and one or more row values", "rowset JSON descriptor with one appended row", "sblr.expr.rowset_append.v3", "rowset_append", "SBSFC053-rowset-append-exprs"),
    "SBSQL-098E28A1F45B": _sbsfc053_oracle_record("sb.rowset.size", "rowset_size", "rowset_size(rowset)", "one rowset descriptor", "int64 descriptor row count", "sblr.expr.rowset_size.v3", "rowset_size", "SBSFC053-rowset-size-marker"),
    "SBSQL-50C1BBB6018E": _sbsfc053_oracle_record("sb.rowset.size", "rowset_size(rowset)", "rowset_size(rowset)", "one rowset descriptor", "int64 descriptor row count", "sblr.expr.rowset_size.v3", "rowset_size", "SBSFC053-rowset-size-rowset"),
    "SBSQL-054E4DC54266": _sbsfc053_oracle_record("sb.rowset.to_array", "rowset_to_array", "rowset_to_array(rowset)", "one rowset descriptor", "array JSON containing descriptor rows", "sblr.expr.rowset_to_array.v3", "rowset_to_array", "SBSFC053-rowset-to-array-marker"),
    "SBSQL-94F61E4D245C": _sbsfc053_oracle_record("sb.rowset.to_array", "rowset_to_array(rowset)", "rowset_to_array(rowset)", "one rowset descriptor", "array JSON containing descriptor rows", "sblr.expr.rowset_to_array.v3", "rowset_to_array", "SBSFC053-rowset-to-array-rowset"),
    "SBSQL-415E89D3266D": _sbsfc053_oracle_record("sb.table_value.value", "table_value", "table_value()", "zero or one optional row-shape descriptor", "table_value JSON descriptor with zero rows", "sblr.expr.table_value_descriptor.v3", "table_value", "SBSFC053-table-value-marker"),
    "SBSQL-425230445B2C": _sbsfc053_oracle_record("sb.table_value.new", "table_value_new", "table_value_new([row_shape])", "optional row-shape descriptor", "table_value JSON descriptor with zero rows", "sblr.expr.table_value_new.v3", "table_value_new", "SBSFC053-table-value-new-empty"),
    "SBSQL-8467B84B58DF": _sbsfc053_oracle_record("sb.table_value.new", "table_value_new(<row_shape>)", "table_value_new(<row_shape>)", "one row-shape descriptor", "table_value JSON descriptor with declared shape and zero rows", "sblr.expr.table_value_new.v3", "table_value_new", "SBSFC053-table-value-new-shape"),
    "SBSQL-BB65E97117E9": _sbsfc053_oracle_record("sb.table_value.append", "table_value_append", "table_value_append(table_value,row)", "table_value descriptor and one row payload", "table_value JSON descriptor with one appended row", "sblr.expr.table_value_append.v3", "table_value_append", "SBSFC053-table-value-append-marker"),
    "SBSQL-24E967F07B8A": _sbsfc053_oracle_record("sb.table_value.append", "table_value_append(tv,row)", "table_value_append(tv,row)", "table_value descriptor and one row payload", "table_value JSON descriptor with one appended row", "sblr.expr.table_value_append.v3", "table_value_append", "SBSFC053-table-value-append-row"),
    "SBSQL-3278282AF7A1": _sbsfc053_oracle_record("sb.setof.generic", "setof(T,...,ordinalitybigint)", "setof(T,...,ordinality bigint)", "zero or more scalar values", "setof JSON descriptor with columns, one row, and row_count", "sblr.expr.setof_generic.v3", "setof_generic", "SBSFC053-setof-generic"),
    "SBSQL-618842668D61": _sbsfc053_oracle_record("sb.setof.key_text_value_text", "setof(keytext,valuetext)", "setof(key text,value text)", "non-null text key and nullable text value", "setof JSON descriptor containing one key/value text row", "sblr.expr.setof_key_text_value_text.v3", "setof_key_text_value_text", "SBSFC053-setof-key-text-value-text"),
    "SBSQL-DC6373538835": _sbsfc053_oracle_record("sb.setof.key_text_value_document", "setof(keytext,valuedocument)", "setof(key text,value document)", "non-null text key and nullable document value", "setof JSON descriptor containing one key/document row", "sblr.expr.setof_key_text_value_document.v3", "setof_key_text_value_document", "SBSFC053-setof-key-text-value-document"),
    "SBSQL-0D038FF22DA8": _sbsfc053_oracle_record("sb.rowset.unnest", "unnest", "unnest(array)", "one array descriptor", "array JSON preserving input elements", "sblr.expr.unnest_array.v3", "unnest", "SBSFC053-unnest-marker"),
    "SBSQL-E11E27B45C94": _sbsfc053_oracle_record("sb.rowset.unnest", "unnest(array)", "unnest(array)", "one array descriptor", "array JSON preserving input elements", "sblr.expr.unnest_array.v3", "unnest", "SBSFC053-unnest-array"),
    "SBSQL-8A1E3E863769": _sbsfc053_oracle_record("sb.rowset.generate_series", "generate_series", "generate_series(start,stop[,step])", "int64 start, stop, and optional nonzero int64 step", "bounded integer array JSON", "sblr.expr.generate_series.v3", "generate_series", "SBSFC053-generate-series-marker"),
    "SBSQL-38EE3D5E1400": _sbsfc053_oracle_record("sb.rowset.generate_series", "generate_series(start,stop[,step])", "generate_series(start,stop[,step])", "int64 start, stop, and optional nonzero int64 step", "bounded integer array JSON", "sblr.expr.generate_series.v3", "generate_series", "SBSFC053-generate-series-start-stop-step"),
    "SBSQL-01B057BBC0EA": _sbsfc053_oracle_record("sb.multiset.element", "element(multiset<T>)", "element(multiset<T>)", "one singleton array-backed multiset", "single JSON-backed element or SQL NULL for empty multiset; non-singleton multiset refuses with invalid input", "sblr.expr.multiset_element.v3", "multiset_element", "SBSFC053-multiset-element"),
    "SBSQL-4B19CB6607C3": _sbsfc053_oracle_record("sb.multiset.fusion", "fusion(multiset<T>)", "fusion(multiset<T>[,...])", "one or more array-backed multisets", "array JSON with fused multiset elements", "sblr.expr.multiset_fusion.v3", "multiset_fusion", "SBSFC053-multiset-fusion"),
    "SBSQL-6B9810626A72": _sbsfc053_oracle_record("sb.multiset.intersection", "intersection(multiset<T>)", "intersection(multiset<T>[,...])", "one or more array-backed multisets", "array JSON with common multiset elements", "sblr.expr.multiset_intersection.v3", "multiset_intersection", "SBSFC053-multiset-intersection"),
}


def _sbsfc054_oracle_record(
    function_id: str,
    canonical_name: str,
    signature: str,
    argument_rule: str,
    return_type_rule: str,
    sblr_binding: str,
    engine_entrypoint: str,
    proof: str,
) -> dict:
    return {
        "builtin_id": function_id,
        "canonical_name": canonical_name,
        "overloads": [{"signature": signature, "argument_rule": argument_rule}],
        "return_type_rule": return_type_rule,
        "coercion_rule": (
            "cursor, stream, rowset, table_value, and locator arguments use bounded JSON "
            "execution-handle descriptors; optional max_rows arguments are int64 descriptors"
        ),
        "null_behavior": "SQL NULL descriptor input returns SQL NULL for descriptor conversion routes; cursor_active(NULL) returns false",
        "collation_charset_rule": "descriptor payloads are deterministic UTF-8 JSON text; handle attribute values use exact text tokens",
        "timezone_rule": "not applicable",
        "volatility": "stable",
        "determinism": "deterministic for supplied descriptor/scalar inputs",
        "side_effects": "bounded descriptor-only state transition for close/open helpers; no persistent mutation",
        "sblr_binding": sblr_binding,
        "ast_binding": f"ast.expr.{engine_entrypoint}",
        "engine_entrypoint": engine_entrypoint,
        "optimizer_properties": {
            "foldable": False,
            "index_eligible": False,
            "generated_column_eligible": False,
            "cost_class": "execution_handle_descriptor",
        },
        "security_policy": (
            "executes inside the SBLR cursor.stream runtime using bounded in-core descriptor helpers; "
            "no parser SQL execution, donor execution, storage scan, cursor backend, WAL/recovery shortcut, cluster provider authority, or external service is used"
        ),
        "donor_rendering": "parser renders native SBsql spelling and diagnostics through generated registry evidence",
        "error_semantics": "invalid arity, malformed handle descriptor, wrong descriptor kind, or negative max_rows refuses with SBSQL.FUNCTION.INVALID_INPUT",
        "syntax_forms": ["function_call"],
        "conformance_cases": [proof],
    }


SBSFC054_CURSOR_STREAM_HANDLE_ORACLE_OVERRIDES = {
    "SBSQL-14BCC57267D0": _sbsfc054_oracle_record("sb.cursor.lifetime_class", "cursor_lifetime_class(cursor)", "cursor_lifetime_class(cursor)", "one cursor descriptor", "character lifetime token", "sblr.expr.cursor_lifetime_class.v3", "cursor_lifetime_class", "SBSFC054-cursor-lifetime-class-arg"),
    "SBSQL-163833F6642E": _sbsfc054_oracle_record("sb.cursor.open", "cursor_open(<select>)", "cursor_open(<select>)", "optional select descriptor", "cursor JSON descriptor in open state", "sblr.expr.cursor_open.v3", "cursor_open", "SBSFC054-cursor-open-select"),
    "SBSQL-21E6F6488A64": _sbsfc054_oracle_record("sb.cursor.to_rowset", "cursor_to_rowset", "cursor_to_rowset(cursor[,max_rows])", "cursor descriptor and optional non-negative max_rows", "rowset JSON descriptor containing cursor rows", "sblr.expr.cursor_to_rowset.v3", "cursor_to_rowset", "SBSFC054-cursor-to-rowset-marker"),
    "SBSQL-38FDC3F10237": _sbsfc054_oracle_record("sb.cursor.close", "cursor_close(cursor)", "cursor_close(cursor)", "one cursor descriptor", "cursor JSON descriptor in closed state", "sblr.expr.cursor_close.v3", "cursor_close", "SBSFC054-cursor-close-arg"),
    "SBSQL-60054AA2660F": _sbsfc054_oracle_record("sb.cursor.lifetime_class", "cursor_lifetime_class", "cursor_lifetime_class([cursor])", "optional cursor descriptor", "character lifetime token", "sblr.expr.cursor_lifetime_class.v3", "cursor_lifetime_class", "SBSFC054-cursor-lifetime-class-marker"),
    "SBSQL-6C87F2E4972C": _sbsfc054_oracle_record("sb.cursor.open", "cursor_open", "cursor_open([select])", "optional select descriptor", "cursor JSON descriptor in open state", "sblr.expr.cursor_open.v3", "cursor_open", "SBSFC054-cursor-open-marker"),
    "SBSQL-892AE352BD3A": _sbsfc054_oracle_record("sb.cursor.rowset_to_cursor", "rowset_to_cursor(rowset)", "rowset_to_cursor(rowset)", "one rowset descriptor", "cursor JSON descriptor backed by rowset rows", "sblr.expr.rowset_to_cursor.v3", "rowset_to_cursor", "SBSFC054-rowset-to-cursor-arg"),
    "SBSQL-8BE380B5BA73": _sbsfc054_oracle_record("sb.stream.close", "stream_close", "stream_close([stream])", "optional stream descriptor", "stream JSON descriptor in closed state", "sblr.expr.stream_close.v3", "stream_close", "SBSFC054-stream-close-marker"),
    "SBSQL-92B7E4FF1332": _sbsfc054_oracle_record("sb.cursor.state", "cursor_state", "cursor_state([cursor])", "optional cursor descriptor", "character state token", "sblr.expr.cursor_state.v3", "cursor_state", "SBSFC054-cursor-state-marker"),
    "SBSQL-9CD08935260C": _sbsfc054_oracle_record("sb.cursor.position", "cursor_position", "cursor_position([cursor])", "optional cursor descriptor", "int64 cursor position", "sblr.expr.cursor_position.v3", "cursor_position", "SBSFC054-cursor-position-marker"),
    "SBSQL-A1FC30F481BC": _sbsfc054_oracle_record("sb.cursor.table_value_to_cursor", "table_value_to_cursor(tv)", "table_value_to_cursor(tv)", "one table_value descriptor", "cursor JSON descriptor backed by table_value rows", "sblr.expr.table_value_to_cursor.v3", "table_value_to_cursor", "SBSFC054-table-value-to-cursor-arg"),
    "SBSQL-A2E0FE1E034D": _sbsfc054_oracle_record("sb.cursor.close", "cursor_close", "cursor_close([cursor])", "optional cursor descriptor", "cursor JSON descriptor in closed state", "sblr.expr.cursor_close.v3", "cursor_close", "SBSFC054-cursor-close-marker"),
    "SBSQL-A339D846AD19": _sbsfc054_oracle_record("sb.cursor.current_row_locator", "current_row_locator(cursor)", "current_row_locator(cursor)", "one cursor descriptor", "locator JSON descriptor for current cursor row", "sblr.expr.current_row_locator.v3", "current_row_locator", "SBSFC054-current-row-locator-cursor"),
    "SBSQL-A99EC7329DF0": _sbsfc054_oracle_record("sb.cursor.to_rowset", "cursor_to_rowset(cursor[,max_rows])", "cursor_to_rowset(cursor[,max_rows])", "cursor descriptor and optional non-negative max_rows", "rowset JSON descriptor containing cursor rows", "sblr.expr.cursor_to_rowset.v3", "cursor_to_rowset", "SBSFC054-cursor-to-rowset-args"),
    "SBSQL-AE071DCD88A8": _sbsfc054_oracle_record("sb.cursor.scrollability", "cursor_scrollability(cursor)", "cursor_scrollability(cursor)", "one cursor descriptor", "character scrollability token", "sblr.expr.cursor_scrollability.v3", "cursor_scrollability", "SBSFC054-cursor-scrollability-arg"),
    "SBSQL-B062E4E23477": _sbsfc054_oracle_record("sb.handle.kind", "handle_kind", "handle_kind([handle])", "optional handle descriptor", "character handle-kind token", "sblr.expr.handle_kind.v3", "handle_kind", "SBSFC054-handle-kind-marker"),
    "SBSQL-B9BA63C166A2": _sbsfc054_oracle_record("sb.stream.to_rowset", "stream_to_rowset(stream[,max_rows])", "stream_to_rowset(stream[,max_rows])", "stream descriptor and optional non-negative max_rows", "rowset JSON descriptor containing stream rows", "sblr.expr.stream_to_rowset.v3", "stream_to_rowset", "SBSFC054-stream-to-rowset-args"),
    "SBSQL-C3E53B267C4B": _sbsfc054_oracle_record("sb.cursor.holdability", "cursor_holdability", "cursor_holdability([cursor])", "optional cursor descriptor", "character holdability token", "sblr.expr.cursor_holdability.v3", "cursor_holdability", "SBSFC054-cursor-holdability-marker"),
    "SBSQL-C4EB99EF9F6F": _sbsfc054_oracle_record("sb.cursor.position", "cursor_position(cursor)", "cursor_position(cursor)", "one cursor descriptor", "int64 cursor position", "sblr.expr.cursor_position.v3", "cursor_position", "SBSFC054-cursor-position-arg"),
    "SBSQL-C682B85033B8": _sbsfc054_oracle_record("sb.cursor.scrollability", "cursor_scrollability", "cursor_scrollability([cursor])", "optional cursor descriptor", "character scrollability token", "sblr.expr.cursor_scrollability.v3", "cursor_scrollability", "SBSFC054-cursor-scrollability-marker"),
    "SBSQL-CD315B828601": _sbsfc054_oracle_record("sb.cursor.table_value_to_cursor", "table_value_to_cursor", "table_value_to_cursor(tv)", "one table_value descriptor", "cursor JSON descriptor backed by table_value rows", "sblr.expr.table_value_to_cursor.v3", "table_value_to_cursor", "SBSFC054-table-value-to-cursor-marker"),
    "SBSQL-CF69DD85814A": _sbsfc054_oracle_record("sb.cursor.rowset_to_cursor", "rowset_to_cursor", "rowset_to_cursor(rowset)", "one rowset descriptor", "cursor JSON descriptor backed by rowset rows", "sblr.expr.rowset_to_cursor.v3", "rowset_to_cursor", "SBSFC054-rowset-to-cursor-marker"),
    "SBSQL-D059810BF5A0": _sbsfc054_oracle_record("sb.handle.kind", "handle_kind(handle)", "handle_kind(handle)", "one handle descriptor", "character handle-kind token", "sblr.expr.handle_kind.v3", "handle_kind", "SBSFC054-handle-kind-arg"),
    "SBSQL-D7858961F2DA": _sbsfc054_oracle_record("sb.cursor.active", "cursor_active", "cursor_active([name_or_cursor])", "optional cursor name or descriptor", "boolean active state", "sblr.expr.cursor_active.v3", "cursor_active", "SBSFC054-cursor-active-marker"),
    "SBSQL-DCB17997FCA9": _sbsfc054_oracle_record("sb.stream.close", "stream_close(stream)", "stream_close(stream)", "one stream descriptor", "stream JSON descriptor in closed state", "sblr.expr.stream_close.v3", "stream_close", "SBSFC054-stream-close-arg"),
    "SBSQL-EFC58ACD7975": _sbsfc054_oracle_record("sb.cursor.active", "cursor_active(name)", "cursor_active(name)", "one cursor name or descriptor", "boolean active state", "sblr.expr.cursor_active.v3", "cursor_active", "SBSFC054-cursor-active-name"),
    "SBSQL-F0E216005A4E": _sbsfc054_oracle_record("sb.cursor.holdability", "cursor_holdability(cursor)", "cursor_holdability(cursor)", "one cursor descriptor", "character holdability token", "sblr.expr.cursor_holdability.v3", "cursor_holdability", "SBSFC054-cursor-holdability-arg"),
    "SBSQL-F15435ED32F1": _sbsfc054_oracle_record("sb.stream.to_rowset", "stream_to_rowset", "stream_to_rowset(stream[,max_rows])", "stream descriptor and optional non-negative max_rows", "rowset JSON descriptor containing stream rows", "sblr.expr.stream_to_rowset.v3", "stream_to_rowset", "SBSFC054-stream-to-rowset-marker"),
    "SBSQL-FCD7942CBB69": _sbsfc054_oracle_record("sb.cursor.state", "cursor_state(cursor)", "cursor_state(cursor)", "one cursor descriptor", "character state token", "sblr.expr.cursor_state.v3", "cursor_state", "SBSFC054-cursor-state-arg"),
}


def _sbsfc055_oracle_record(builtin_id: str,
                             canonical_name: str,
                             signature: str,
                             argument_rule: str,
                             return_type_rule: str,
                             sblr_binding: str,
                             engine_entrypoint: str,
                             proof: str) -> dict:
    return {
        "builtin_id": builtin_id,
        "canonical_name": canonical_name,
        "overloads": [{"signature": signature, "argument_rule": argument_rule}],
        "return_type_rule": return_type_rule,
        "coercion_rule": "LOB locator arguments are bounded JSON descriptors; data accepts binary or text scalar values; offsets and lengths are int64",
        "null_behavior": "SQL NULL locator input refuses for required-locator operations and returns empty marker values for marker forms",
        "collation_charset_rule": "text LOB payloads use UTF-8 character descriptor; binary payloads use binary descriptor",
        "timezone_rule": "not applicable",
        "volatility": "stable_statement",
        "determinism": "deterministic for supplied descriptors",
        "side_effects": "bounded descriptor-only LOB state transition; no persistent mutation",
        "sblr_binding": sblr_binding,
        "ast_binding": f"ast.expr.{engine_entrypoint}",
        "engine_entrypoint": engine_entrypoint,
        "optimizer_properties": {
            "foldable": False,
            "index_eligible": False,
            "generated_column_eligible": False,
            "cost_class": "lob_locator_descriptor",
        },
        "security_policy": (
            "executes inside the SBLR lob.locator runtime using bounded in-core descriptor helpers; "
            "no parser SQL execution, donor execution, storage finality, WAL/recovery shortcut, cluster provider authority, or external service is used"
        ),
        "donor_rendering": "parser renders native SBsql spelling and diagnostics through generated registry evidence",
        "error_semantics": "invalid arity, malformed locator descriptor, non-positive offset, negative length, or unsupported open mode refuses with SBSQL.FUNCTION.INVALID_INPUT",
        "syntax_forms": ["function_call"],
        "conformance_cases": [proof],
    }


SBSFC055_LOB_LOCATOR_ORACLE_OVERRIDES = {
    "SBSQL-15EB156297E9": _sbsfc055_oracle_record("sb.lob.locator_to_binary", "lob_locator_to_binary", "lob_locator_to_binary", "optional locator descriptor", "binary locator payload or empty binary marker", "sblr.expr.lob_locator_to_binary.v3", "lob_locator_to_binary", "SBSFC055-lob-locator-to-binary-marker"),
    "SBSQL-176685E96193": _sbsfc055_oracle_record("sb.locator.validity", "locator_validity(locator)", "locator_validity(locator)", "one locator descriptor", "boolean locator validity", "sblr.expr.locator_validity.v3", "locator_validity", "SBSFC055-locator-validity-arg"),
    "SBSQL-1EE6C3D7F2EE": _sbsfc055_oracle_record("sb.lob.write", "lob_write", "lob_write", "locator descriptor, 1-based offset, and data", "LOB locator descriptor with bytes written at offset", "sblr.expr.lob_write.v3", "lob_write", "SBSFC055-lob-write-marker"),
    "SBSQL-2E2F4913A42F": _sbsfc055_oracle_record("sb.lob.close", "lob_close(locator)", "lob_close(locator)", "one locator descriptor", "LOB locator descriptor in closed state", "sblr.expr.lob_close.v3", "lob_close", "SBSFC055-lob-close-arg"),
    "SBSQL-317A464A74B3": _sbsfc055_oracle_record("sb.lob.size", "lob_size", "lob_size", "optional locator descriptor", "int64 byte count", "sblr.expr.lob_size.v3", "lob_size", "SBSFC055-lob-size-marker"),
    "SBSQL-41A19A07C09B": _sbsfc055_oracle_record("sb.lob.open", "lob_open(locator,mode)", "lob_open(locator,mode)", "locator descriptor and mode token", "LOB locator descriptor in requested open mode", "sblr.expr.lob_open.v3", "lob_open", "SBSFC055-lob-open-arg"),
    "SBSQL-4B3B3D4FB26A": _sbsfc055_oracle_record("sb.lob.append", "lob_append", "lob_append", "data or locator descriptor plus data", "LOB locator descriptor with appended bytes", "sblr.expr.lob_append.v3", "lob_append", "SBSFC055-lob-append-marker"),
    "SBSQL-531A760C8C66": _sbsfc055_oracle_record("sb.locator.current_row", "current_row_locator", "current_row_locator", "no arguments", "current row locator descriptor", "sblr.expr.current_row_locator_marker.v3", "current_row_locator", "SBSFC055-current-row-locator-marker"),
    "SBSQL-7B6B59743B35": _sbsfc055_oracle_record("sb.locator.validity", "locator_validity", "locator_validity", "optional locator descriptor", "boolean locator validity", "sblr.expr.locator_validity.v3", "locator_validity", "SBSFC055-locator-validity-marker"),
    "SBSQL-7CF4F4150D85": _sbsfc055_oracle_record("sb.lob.truncate", "lob_truncate", "lob_truncate", "locator descriptor and non-negative length", "LOB locator descriptor truncated to length", "sblr.expr.lob_truncate.v3", "lob_truncate", "SBSFC055-lob-truncate-marker"),
    "SBSQL-891992A5F310": _sbsfc055_oracle_record("sb.lob.open", "lob_open", "lob_open", "optional locator descriptor and mode token", "default open LOB locator descriptor", "sblr.expr.lob_open.v3", "lob_open", "SBSFC055-lob-open-marker"),
    "SBSQL-96DE0F2265B6": _sbsfc055_oracle_record("sb.lob.locator_to_binary", "lob_locator_to_binary(locator)", "lob_locator_to_binary(locator)", "one locator descriptor", "binary locator payload", "sblr.expr.lob_locator_to_binary.v3", "lob_locator_to_binary", "SBSFC055-lob-locator-to-binary-arg"),
    "SBSQL-9C16F6BF7072": _sbsfc055_oracle_record("sb.lob.truncate", "lob_truncate(locator,length)", "lob_truncate(locator,length)", "locator descriptor and non-negative length", "LOB locator descriptor truncated to length", "sblr.expr.lob_truncate.v3", "lob_truncate", "SBSFC055-lob-truncate-arg"),
    "SBSQL-A9177A9A947C": _sbsfc055_oracle_record("sb.lob.write", "lob_write(locator,offset,data)", "lob_write(locator,offset,data)", "locator descriptor, 1-based offset, and data", "LOB locator descriptor with bytes written at offset", "sblr.expr.lob_write.v3", "lob_write", "SBSFC055-lob-write-arg"),
    "SBSQL-C2F659F4DFC0": _sbsfc055_oracle_record("sb.lob.read", "lob_read", "lob_read", "locator descriptor, 1-based offset, and non-negative length", "binary or text slice from locator payload", "sblr.expr.lob_read.v3", "lob_read", "SBSFC055-lob-read-marker"),
    "SBSQL-C62D69D167C8": _sbsfc055_oracle_record("sb.lob.size", "lob_size(locator)", "lob_size(locator)", "one locator descriptor", "int64 byte count", "sblr.expr.lob_size.v3", "lob_size", "SBSFC055-lob-size-arg"),
    "SBSQL-D2E48C4160ED": _sbsfc055_oracle_record("sb.lob.create", "lob_create(class,[media])", "lob_create(class,[media])", "optional class and media type", "new LOB locator descriptor", "sblr.expr.lob_create.v3", "lob_create", "SBSFC055-lob-create-arg"),
    "SBSQL-D5E167A4984B": _sbsfc055_oracle_record("sb.lob.append", "lob_append(locator,data)", "lob_append(locator,data)", "locator descriptor plus data", "LOB locator descriptor with appended bytes", "sblr.expr.lob_append.v3", "lob_append", "SBSFC055-lob-append-arg"),
    "SBSQL-D89EF3B31969": _sbsfc055_oracle_record("sb.lob.create", "lob_create", "lob_create", "optional class and media type", "new default LOB locator descriptor", "sblr.expr.lob_create.v3", "lob_create", "SBSFC055-lob-create-marker"),
    "SBSQL-DA9087A02218": _sbsfc055_oracle_record("sb.lob.locator_to_text", "lob_locator_to_text(locator)", "lob_locator_to_text(locator)", "one locator descriptor", "text locator payload", "sblr.expr.lob_locator_to_text.v3", "lob_locator_to_text", "SBSFC055-lob-locator-to-text-arg"),
    "SBSQL-DF89DE098501": _sbsfc055_oracle_record("sb.lob.close", "lob_close", "lob_close", "optional locator descriptor", "default closed LOB locator descriptor", "sblr.expr.lob_close.v3", "lob_close", "SBSFC055-lob-close-marker"),
    "SBSQL-E7CBEEE4AAC6": _sbsfc055_oracle_record("sb.lob.locator_to_text", "lob_locator_to_text", "lob_locator_to_text", "optional locator descriptor", "empty text marker or text locator payload", "sblr.expr.lob_locator_to_text.v3", "lob_locator_to_text", "SBSFC055-lob-locator-to-text-marker"),
    "SBSQL-F2A363288372": _sbsfc055_oracle_record("sb.locator.locator", "locator", "locator", "no arguments", "generic locator descriptor", "sblr.expr.locator_descriptor.v3", "locator", "SBSFC055-locator-marker"),
    "SBSQL-FC06D12DAC16": _sbsfc055_oracle_record("sb.lob.read", "lob_read(locator,offset,length)", "lob_read(locator,offset,length)", "locator descriptor, 1-based offset, and non-negative length", "binary or text slice from locator payload", "sblr.expr.lob_read.v3", "lob_read", "SBSFC055-lob-read-arg"),
}


def _sbsfc056_oracle_record(
    builtin_id: str,
    signature: str,
    argument_rule: str,
    return_type_rule: str,
    sblr_binding: str,
    engine_entrypoint: str,
    proof: str,
) -> dict[str, object]:
    return {
        "builtin_id": builtin_id,
        "canonical_name": signature,
        "overloads": [{"signature": signature, "argument_rule": argument_rule}],
        "return_type_rule": return_type_rule,
        "coercion_rule": "bounded SBsql expression-runtime descriptor coercion only",
        "null_behavior": "NULL inputs preserve SQL NULL where the surface semantics require it",
        "collation_charset_rule": "UTF-8/unicode_root for returned character descriptors",
        "timezone_rule": "at_time_zone returns a bounded timestamp_tz descriptor; other rows are not timezone-sensitive",
        "volatility": "stable_statement",
        "determinism": "deterministic for supplied descriptors",
        "side_effects": "none; descriptor-only expression runtime surface",
        "sblr_binding": sblr_binding,
        "ast_binding": f"ast.expr.{engine_entrypoint}",
        "engine_entrypoint": engine_entrypoint,
        "optimizer_properties": {
            "foldable": False,
            "index_eligible": False,
            "generated_column_eligible": False,
            "cost_class": "native_surface_descriptor",
        },
        "security_policy": (
            "executes inside the SBLR surface.scalar runtime using bounded in-core descriptor helpers; "
            "no parser SQL execution, donor execution, storage finality, WAL/recovery shortcut, cluster provider authority, or external service is used"
        ),
        "donor_rendering": "parser renders native SBsql spelling and diagnostics through generated registry evidence",
        "error_semantics": "invalid arity or malformed bounded descriptor refuses with SBSQL.FUNCTION.INVALID_INPUT",
        "syntax_forms": ["function_call"],
        "conformance_cases": [proof],
    }


SBSFC056_NATIVE_SURFACE_ORACLE_OVERRIDES = {
    "SBSQL-DF502F8DF4FA": _sbsfc056_oracle_record("sb.scalar.accept", "Accept", "zero arguments or one feature descriptor", "character marker or boolean feature acceptance", "sblr.expr.native_surface.accept.v3", "accept", "SBSFC056-accept-marker"),
    "SBSQL-8CBB8186C7CC": _sbsfc056_oracle_record("sb.scalar.close", "CLOSE", "no arguments", "character keyword marker", "sblr.expr.native_surface.close.v3", "close", "SBSFC056-close-marker"),
    "SBSQL-755DD39EA853": _sbsfc056_oracle_record("sb.scalar.future_version", "FUTURE_VERSION", "no arguments", "character syntax marker", "sblr.expr.native_surface.future_version.v3", "future_version", "SBSFC056-future-version-marker"),
    "SBSQL-B30BB888C751": _sbsfc056_oracle_record("sb.scalar.gap", "GAP", "no arguments", "character surface marker", "sblr.expr.native_surface.gap.v3", "gap", "SBSFC056-gap-marker"),
    "SBSQL-CD2216F125FB": _sbsfc056_oracle_record("sb.scalar.immutable", "IMMUTABLE", "no arguments", "character volatility marker", "sblr.expr.native_surface.immutable.v3", "immutable", "SBSFC056-immutable-marker"),
    "SBSQL-14EDC2636B45": _sbsfc056_oracle_record("sb.scalar.match_recognize", "MATCH_RECOGNIZE", "optional pattern descriptor", "match_recognize JSON descriptor", "sblr.expr.native_surface.match_recognize.v3", "match_recognize", "SBSFC056-match-recognize-marker"),
    "SBSQL-C4027F6E6C8A": _sbsfc056_oracle_record("sb.scalar.open", "OPEN", "no arguments", "character keyword marker", "sblr.expr.native_surface.open.v3", "open", "SBSFC056-open-marker"),
    "SBSQL-67B876B5339F": _sbsfc056_oracle_record("sb.scalar.reserved", "RESERVED", "no arguments", "character syntax marker", "sblr.expr.native_surface.reserved.v3", "reserved", "SBSFC056-reserved-marker"),
    "SBSQL-4AF1FA4C5BBC": _sbsfc056_oracle_record("sb.scalar.sbsql_syntax_future_version", "SBSQL.SYNTAX_FUTURE_VERSION", "no arguments", "character SBsql syntax marker", "sblr.expr.native_surface.sbsql_syntax_future_version.v3", "sbsql_syntax_future_version", "SBSFC056-syntax-future-marker"),
    "SBSQL-4975481A1AB7": _sbsfc056_oracle_record("sb.scalar.sbsql_syntax_reserved", "SBSQL.SYNTAX_RESERVED", "no arguments", "character SBsql syntax marker", "sblr.expr.native_surface.sbsql_syntax_reserved.v3", "sbsql_syntax_reserved", "SBSFC056-syntax-reserved-marker"),
    "SBSQL-8893D25F387F": _sbsfc056_oracle_record("sb.scalar.stable", "STABLE", "no arguments", "character volatility marker", "sblr.expr.native_surface.stable.v3", "stable", "SBSFC056-stable-marker"),
    "SBSQL-ABD89A468ECA": _sbsfc056_oracle_record("sb.scalar.treat", "TREAT", "no arguments", "character special-form marker", "sblr.expr.native_surface.treat.v3", "treat", "SBSFC056-treat-marker"),
    "SBSQL-504CEBDC6FE1": _sbsfc056_oracle_record("sb.scalar.treat_typed", "TREAT(exprASsubtype)", "expression and subtype descriptor", "input value with bounded subtype descriptor", "sblr.expr.native_surface.treat_typed.v3", "treat_typed", "SBSFC056-treat-typed"),
    "SBSQL-2E11730BB92B": _sbsfc056_oracle_record("sb.scalar.volatile", "VOLATILE", "no arguments", "character volatility marker", "sblr.expr.native_surface.volatile.v3", "volatile", "SBSFC056-volatile-marker"),
    "SBSQL-12CD234538AF": _sbsfc056_oracle_record("sb.scalar.accept_sql2016_timeseries", "accept(SQL:2016 fits time-series)", "feature descriptor", "boolean accepted marker", "sblr.expr.native_surface.accept_sql2016_timeseries.v3", "accept_sql2016_timeseries", "SBSFC056-accept-sql2016-timeseries"),
    "SBSQL-A23C7082573D": _sbsfc056_oracle_record("sb.aggregate.any_value", "any_value", "zero or one expression", "marker or first bounded value", "sblr.expr.native_surface.any_value.v3", "any_value", "SBSFC056-any-value-marker"),
    "SBSQL-76EC89319569": _sbsfc056_oracle_record("sb.aggregate.any_value_expr", "any_value(expr)", "one expression", "first bounded value", "sblr.expr.native_surface.any_value_expr.v3", "any_value_expr", "SBSFC056-any-value-expr"),
    "SBSQL-6C877B4376DE": _sbsfc056_oracle_record("sb.scalar.at_time_zone", "at_time_zone", "timestamp and timezone descriptor", "timestamp_tz character payload", "sblr.expr.native_surface.at_time_zone.v3", "at_time_zone", "SBSFC056-at-time-zone"),
    "SBSQL-E17CFDACCB8E": _sbsfc056_oracle_record("sb.scalar.bit_string", "bit_string", "zero or one 0/1 bit text", "bit_string descriptor", "sblr.expr.native_surface.bit_string.v3", "bit_string", "SBSFC056-bit-string"),
    "SBSQL-4E24AE0D0EDE": _sbsfc056_oracle_record("sb.scalar.bulk_exceptions", "bulk_exceptions", "optional JSON array descriptor", "json_document exception array", "sblr.expr.native_surface.bulk_exceptions.v3", "bulk_exceptions", "SBSFC056-bulk-exceptions"),
    "SBSQL-D03ED69E33B7": _sbsfc056_oracle_record("sb.aggregate.collect", "collect", "zero or more expressions", "json_document array", "sblr.expr.native_surface.collect.v3", "collect", "SBSFC056-collect-marker"),
    "SBSQL-A1B94C83C5F1": _sbsfc056_oracle_record("sb.aggregate.collect_expr", "collect(expr)", "one expression", "json_document array containing expression", "sblr.expr.native_surface.collect_expr.v3", "collect_expr", "SBSFC056-collect-expr"),
    "SBSQL-5550BDA0A76C": _sbsfc056_oracle_record("sb.scalar.domain_stack", "domain_stack", "zero or one value", "json_document domain stack", "sblr.expr.native_surface.domain_stack.v3", "domain_stack", "SBSFC056-domain-stack-marker"),
    "SBSQL-1906412209C9": _sbsfc056_oracle_record("sb.scalar.domain_stack_value", "domain_stack(value)", "one value", "json_document domain stack", "sblr.expr.native_surface.domain_stack_value.v3", "domain_stack_value", "SBSFC056-domain-stack-value"),
    "SBSQL-8F66D89149F5": _sbsfc056_oracle_record("sb.scalar.donor_only", "donor_only", "no arguments", "character surface marker", "sblr.expr.native_surface.donor_only.v3", "donor_only", "SBSFC056-donor-only"),
    "SBSQL-F785EAF383DE": _sbsfc056_oracle_record("sb.scalar.donor_rewrite", "donor_rewrite", "no arguments", "character surface marker", "sblr.expr.native_surface.donor_rewrite.v3", "donor_rewrite", "SBSFC056-donor-rewrite"),
    "SBSQL-FD0DF4067008": _sbsfc056_oracle_record("sb.multiset.element", "element", "singleton array-backed multiset", "json_document singleton value", "sblr.expr.multiset_element.v3", "multiset_element", "SBSFC056-element"),
    "SBSQL-1A8470FC95E7": _sbsfc056_oracle_record("sb.expr.match_recognize.v1", "expr.match_recognize.v1", "optional pattern descriptor", "match_recognize JSON descriptor", "sblr.expr.native_surface.expr_match_recognize_v1.v3", "expr_match_recognize_v1", "SBSFC056-expr-match-recognize"),
    "SBSQL-9F6F909938A0": _sbsfc056_oracle_record("sb.multiset.fusion", "fusion", "array-backed multisets", "json_document fused array", "sblr.expr.multiset_fusion.v3", "multiset_fusion", "SBSFC056-fusion"),
    "SBSQL-DB32CA47B7B5": _sbsfc056_oracle_record("sb.type.integer", "integer", "zero or one integer value", "type_descriptor or int64 value", "sblr.expr.native_surface.integer.v3", "integer", "SBSFC056-integer"),
    "SBSQL-9C90F3645C34": _sbsfc056_oracle_record("sb.multiset.intersection", "intersection", "array-backed multisets", "json_document intersection array", "sblr.expr.multiset_intersection.v3", "multiset_intersection", "SBSFC056-intersection"),
    "SBSQL-AC8794BE30FE": _sbsfc056_oracle_record("sb.scalar.native_future", "native_future", "no arguments", "character status marker", "sblr.expr.native_surface.native_future.v3", "native_future", "SBSFC056-native-future"),
    "SBSQL-3F50B9923297": _sbsfc056_oracle_record("sb.scalar.native_now", "native_now", "no arguments", "character status marker", "sblr.expr.native_surface.native_now.v3", "native_now", "SBSFC056-native-now"),
    "SBSQL-83969495B383": _sbsfc056_oracle_record("sb.scalar.nvl", "nvl(a,b)", "two expressions", "first expression unless NULL, otherwise fallback", "sblr.expr.native_surface.nvl.v3", "nvl", "SBSFC056-nvl"),
    "SBSQL-75F997655797": _sbsfc056_oracle_record("sb.scalar.private_only", "private_only", "no arguments", "character surface marker", "sblr.expr.native_surface.private_only.v3", "private_only", "SBSFC056-private-only"),
    "SBSQL-F94025D79003": _sbsfc056_oracle_record("sb.scalar.tabular", "tabular", "no arguments", "json_document tabular descriptor", "sblr.expr.native_surface.tabular.v3", "tabular", "SBSFC056-tabular"),
    "SBSQL-FB4A06130103": _sbsfc056_oracle_record("sb.scalar.void", "void", "no arguments", "SQL NULL void descriptor", "sblr.expr.native_surface.void.v3", "void", "SBSFC056-void"),
}


def _sbsfc057_oracle_record(
    builtin_id: str,
    signature: str,
    argument_rule: str,
    return_type_rule: str,
    sblr_binding: str,
    engine_entrypoint: str,
    proof: str,
    dependency_unavailable: bool = False,
) -> dict[str, object]:
    return {
        "builtin_id": builtin_id,
        "canonical_name": signature,
        "overloads": [{"signature": signature, "argument_rule": argument_rule}],
        "return_type_rule": return_type_rule,
        "coercion_rule": "bounded text/binary/uint64 argument coercion through SBLR scalar values only",
        "null_behavior": "NULL inputs preserve SQL NULL for value-returning helpers; provider-gated rows fail closed before data disclosure",
        "collation_charset_rule": "UTF-8/unicode_root for returned character descriptors; binary payloads are byte-stable",
        "timezone_rule": "not timezone-sensitive",
        "volatility": "volatile_value for random-byte/uuid helpers; stable_statement for deterministic crypto descriptors",
        "determinism": "deterministic under fixture random/uuid overrides; random helpers use OpenSSL RAND otherwise",
        "side_effects": "none; no mutation or transaction finality change",
        "sblr_binding": sblr_binding,
        "ast_binding": f"ast.expr.crypto_hash.{engine_entrypoint}",
        "engine_entrypoint": engine_entrypoint,
        "optimizer_properties": {
            "foldable": False,
            "index_eligible": False,
            "generated_column_eligible": False,
            "cost_class": "crypto_scalar",
        },
        "security_policy": (
            "executes inside the SBLR crypto.hash runtime using OpenSSL EVP/RAND/HMAC/scrypt where locally available, "
            "in-core xxhash64, deterministic ScratchBird armor/dearmor, and bounded ScratchBird PGP envelopes that do not claim OpenPGP compatibility; "
            "no parser SQL execution, donor execution, storage finality, WAL/recovery shortcut, cluster provider authority, or external service is used"
        ),
        "donor_rendering": "parser renders native SBsql spelling and diagnostics through generated registry evidence",
        "error_semantics": (
            "provider-gated row refuses with SBSQL.FUNCTION.DEPENDENCY_UNAVAILABLE"
            if dependency_unavailable
            else "invalid arity, malformed armor/envelope, unsupported algorithm, or out-of-budget request refuses with SBSQL.FUNCTION.INVALID_INPUT"
        ),
        "syntax_forms": ["function_call"],
        "conformance_cases": [proof],
    }


SBSFC057_CRYPTO_HASH_ORACLE_OVERRIDES = {
    "SBSQL-697D0080DA8E": _sbsfc057_oracle_record("sb.crypto.argon2", "argon2", "provider-gated password/hash inputs", "dependency-unavailable diagnostic", "sblr.expr.crypto_hash.argon2.v3", "argon2", "SBSFC057-argon2-provider-refuses", True),
    "SBSQL-F2657259D869": _sbsfc057_oracle_record("sb.crypto.armor", "armor", "one text or binary value", "deterministic ScratchBird ASCII armor text", "sblr.expr.crypto_hash.armor.v3", "armor", "SBSFC057-armor-text"),
    "SBSQL-A475C1402E1D": _sbsfc057_oracle_record("sb.crypto.armor_binary", "armor(binary)", "one binary value", "deterministic ScratchBird ASCII armor text", "sblr.expr.crypto_hash.armor_binary.v3", "armor_binary", "SBSFC057-armor-binary"),
    "SBSQL-DEAE160F496C": _sbsfc057_oracle_record("sb.crypto.bcrypt", "bcrypt", "provider-gated password/hash inputs", "dependency-unavailable diagnostic", "sblr.expr.crypto_hash.bcrypt.v3", "bcrypt", "SBSFC057-bcrypt-provider-refuses", True),
    "SBSQL-333906CB50B9": _sbsfc057_oracle_record("sb.crypto.blake2b", "blake2b", "zero or one text/binary value", "OpenSSL EVP BLAKE2b-512 hex text", "sblr.expr.crypto_hash.blake2b.v3", "blake2b", "SBSFC057-blake2b-digest"),
    "SBSQL-EDA08840598E": _sbsfc057_oracle_record("sb.crypto.blake3", "blake3", "provider-gated text/binary input", "dependency-unavailable diagnostic", "sblr.expr.crypto_hash.blake3.v3", "blake3", "SBSFC057-blake3-provider-refuses", True),
    "SBSQL-38360AA175AA": _sbsfc057_oracle_record("sb.crypto.crypt", "crypt", "provider-gated password/hash inputs", "dependency-unavailable diagnostic", "sblr.expr.crypto_hash.crypt.v3", "crypt", "SBSFC057-crypt-provider-refuses", True),
    "SBSQL-46DCE02DBE41": _sbsfc057_oracle_record("sb.crypto.crypt_password_salt", "crypt(password,salt)", "password and salt", "dependency-unavailable diagnostic", "sblr.expr.crypto_hash.crypt_password_salt.v3", "crypt_password_salt", "SBSFC057-crypt-password-salt-provider-refuses", True),
    "SBSQL-0253C933634F": _sbsfc057_oracle_record("sb.crypto.dearmor", "dearmor", "one ScratchBird armor/base64 text", "binary payload bytes", "sblr.expr.crypto_hash.dearmor.v3", "dearmor", "SBSFC057-dearmor-armor"),
    "SBSQL-DFF3ADE173F5": _sbsfc057_oracle_record("sb.crypto.dearmor_text", "dearmor(text)", "one ScratchBird armor/base64 text", "binary payload bytes", "sblr.expr.crypto_hash.dearmor_text.v3", "dearmor_text", "SBSFC057-dearmor-base64"),
    "SBSQL-98AC558CD662": _sbsfc057_oracle_record("sb.crypto.gen_random_bytes", "gen_random_bytes", "optional bounded byte count", "binary random bytes", "sblr.expr.crypto_hash.gen_random_bytes.v3", "gen_random_bytes", "SBSFC057-gen-random-bytes-default"),
    "SBSQL-7612CF167F37": _sbsfc057_oracle_record("sb.crypto.gen_random_bytes_n", "gen_random_bytes(n)", "bounded byte count", "binary random bytes", "sblr.expr.crypto_hash.gen_random_bytes_n.v3", "gen_random_bytes_n", "SBSFC057-gen-random-bytes-n"),
    "SBSQL-F609742097AE": _sbsfc057_oracle_record("sb.crypto.gen_random_uuid", "gen_random_uuid", "no arguments", "uuid text", "sblr.expr.crypto_hash.gen_random_uuid.v3", "gen_random_uuid", "SBSFC057-gen-random-uuid"),
    "SBSQL-5F23337B660A": _sbsfc057_oracle_record("sb.crypto.gen_salt", "gen_salt", "zero or two bounded salt arguments", "deterministic bf salt descriptor", "sblr.expr.crypto_hash.gen_salt.v3", "gen_salt", "SBSFC057-gen-salt-default"),
    "SBSQL-058471F02F03": _sbsfc057_oracle_record("sb.crypto.gen_salt_algo", "gen_salt(algo[,rounds])", "algorithm and optional rounds", "deterministic md5/bf salt descriptor", "sblr.expr.crypto_hash.gen_salt_algo.v3", "gen_salt_algo", "SBSFC057-gen-salt-md5"),
    "SBSQL-6144531FD80E": _sbsfc057_oracle_record("sb.crypto.hmac", "hmac", "zero or three value/key/algorithm arguments", "marker or OpenSSL HMAC hex text", "sblr.expr.crypto_hash.hmac.v3", "hmac", "SBSFC057-hmac-marker"),
    "SBSQL-D0FF02C4CDDE": _sbsfc057_oracle_record("sb.crypto.hmac_value_key_algo", "hmac(text|binary,key,algo)", "value, key, and algorithm", "OpenSSL HMAC hex text", "sblr.expr.crypto_hash.hmac_value_key_algo.v3", "hmac_value_key_algo", "SBSFC057-hmac-sha256"),
    "SBSQL-AF4E7BFEFDD1": _sbsfc057_oracle_record("sb.crypto.pgcrypto", "pgcrypto", "no arguments", "compatibility envelope marker", "sblr.expr.crypto_hash.pgcrypto.v3", "pgcrypto", "SBSFC057-pgcrypto-marker"),
    "SBSQL-6358314B6883": _sbsfc057_oracle_record("sb.crypto.pgp_pub_decrypt", "pgp_pub_decrypt", "ScratchBird pub envelope and key", "decrypted character payload", "sblr.expr.crypto_hash.pgp_pub_decrypt.v3", "pgp_pub_decrypt", "SBSFC057-pgp-pub-decrypt"),
    "SBSQL-2854D8B0790B": _sbsfc057_oracle_record("sb.crypto.pgp_pub_encrypt", "pgp_pub_encrypt", "value and public key descriptor", "bounded ScratchBird pub envelope text", "sblr.expr.crypto_hash.pgp_pub_encrypt.v3", "pgp_pub_encrypt", "SBSFC057-pgp-pub-encrypt"),
    "SBSQL-6DBE85C4B814": _sbsfc057_oracle_record("sb.crypto.pgp_sym_decrypt", "pgp_sym_decrypt", "ScratchBird sym envelope and key", "decrypted character payload", "sblr.expr.crypto_hash.pgp_sym_decrypt.v3", "pgp_sym_decrypt", "SBSFC057-pgp-sym-decrypt"),
    "SBSQL-C98EC981ACD7": _sbsfc057_oracle_record("sb.crypto.pgp_sym_encrypt", "pgp_sym_encrypt", "value and symmetric key", "bounded ScratchBird sym envelope text", "sblr.expr.crypto_hash.pgp_sym_encrypt.v3", "pgp_sym_encrypt", "SBSFC057-pgp-sym-encrypt"),
    "SBSQL-C8996122850A": _sbsfc057_oracle_record("sb.crypto.scrypt", "scrypt", "password, salt, and optional bounded parameters", "OpenSSL EVP_PBE_scrypt hex text", "sblr.expr.crypto_hash.scrypt.v3", "scrypt", "SBSFC057-scrypt"),
    "SBSQL-BD3080D87EA5": _sbsfc057_oracle_record("sb.crypto.sha3_256", "sha3_256", "zero or one text/binary value", "OpenSSL SHA3-256 hex text", "sblr.expr.crypto_hash.sha3_256.v3", "sha3_256", "SBSFC057-sha3-256"),
    "SBSQL-51BB6328126C": _sbsfc057_oracle_record("sb.crypto.sha3_512", "sha3_512", "zero or one text/binary value", "OpenSSL SHA3-512 hex text", "sblr.expr.crypto_hash.sha3_512.v3", "sha3_512", "SBSFC057-sha3-512"),
    "SBSQL-4AD05EF7474D": _sbsfc057_oracle_record("sb.crypto.xxhash64", "xxhash64", "zero or one text/binary value", "uint64 xxhash64 value", "sblr.expr.crypto_hash.xxhash64.v3", "xxhash64", "SBSFC057-xxhash64"),
    "SBSQL-B75400EDF4FB": _sbsfc057_oracle_record("sb.crypto.xxhash64_value_seed", "xxhash64(text|binary[,seed])", "text/binary value and optional uint64 seed", "uint64 xxhash64 value", "sblr.expr.crypto_hash.xxhash64_value_seed.v3", "xxhash64_value_seed", "SBSFC057-xxhash64-seed"),
}


def _sbsfc058_oracle_record(
    builtin_id: str,
    signature: str,
    argument_rule: str,
    return_type_rule: str,
    sblr_binding: str,
    engine_entrypoint: str,
    proof: str,
    volatility: str = "stable_statement",
    side_effects: str = "none; no mutation or transaction finality change",
) -> dict[str, object]:
    return {
        "builtin_id": builtin_id,
        "canonical_name": signature,
        "overloads": [{"signature": signature, "argument_rule": argument_rule}],
        "return_type_rule": return_type_rule,
        "coercion_rule": "bounded descriptor-to-SBLR scalar coercion for the fixture arguments only",
        "null_behavior": "SQL NULL propagates through the SBLR scalar runtime for nullable value arguments",
        "collation_charset_rule": "UTF-8/unicode_root for character descriptors; regex rows use the bounded C++ regex route",
        "timezone_rule": "numeric offset or UTC/GMT only for timezone rows; other rows are not timezone-sensitive",
        "volatility": volatility,
        "determinism": "deterministic for the SBSFC-058 fixture inputs except sequence advancement state",
        "side_effects": side_effects,
        "sblr_binding": sblr_binding,
        "ast_binding": f"ast.expr.sbsfc058.{engine_entrypoint}",
        "engine_entrypoint": engine_entrypoint,
        "optimizer_properties": {
            "foldable": False,
            "index_eligible": False,
            "generated_column_eligible": False,
            "cost_class": "cpu_scalar",
        },
        "security_policy": (
            "executes inside the SBLR expression runtime using in-core scalar helpers and the existing SBLR sequence runtime; "
            "no parser SQL execution, donor execution, storage finality, WAL/recovery shortcut, cluster provider authority, or external service is used"
        ),
        "donor_rendering": "parser renders native SBsql spelling and diagnostics through generated registry evidence",
        "error_semantics": "invalid arity, malformed regex/temporal/numeric text, unsupported timezone, or sequence current-state errors use canonical SBLR diagnostics",
        "syntax_forms": ["function_call"],
        "conformance_cases": [proof],
    }


SBSFC058_EXPRESSION_RUNTIME_ORACLE_OVERRIDES = {
    "SBSQL-0FF0127F4755": _sbsfc058_oracle_record("sb.scalar.position_regex", "POSITION_REGEX([START|AFTER]patternINstring[OCCURRENCEn][FLAGflags])", "pattern, string, optional occurrence, flags, and mode", "int64 one-based position or 0", "sblr.expr.regex_position.v3", "position_regex", "SBSFC058-position-regex"),
    "SBSQL-190A9409E3F9": _sbsfc058_oracle_record("sb.temporal.age", "age(timestamp[,timestamp])", "timestamp and optional comparison timestamp", "interval text", "sblr.expr.temporal_age.v3", "age", "SBSFC058-age-signature"),
    "SBSQL-1B190CAA8EB4": _sbsfc058_oracle_record("sb.temporal.epoch", "epoch", "one ISO-like temporal value", "int64 Unix epoch seconds", "sblr.expr.temporal_epoch.v3", "epoch", "SBSFC058-epoch"),
    "SBSQL-20441CF0D96A": _sbsfc058_oracle_record("sb.temporal.date_add", "date_add", "date or timestamp and ISO-like interval", "date or timestamp text", "sblr.expr.temporal_date_add.v3", "date_add", "SBSFC058-date-add-bare"),
    "SBSQL-270091D8A5BB": _sbsfc058_oracle_record("sb.scalar.currval", "currval", "sequence name", "int64 current sequence value", "sblr.expr.sequence_currval.v3", "sequence_currval", "SBSFC058-currval-bare", "volatile_value", "reads existing SBLR sequence runtime state; no transaction finality change"),
    "SBSQL-2B4C5FFFF451": _sbsfc058_oracle_record("sb.temporal.timezone", "timezone(zone,timestamp)", "UTC/GMT or numeric offset zone and timestamp", "timestamp_tz text", "sblr.expr.temporal_timezone.v3", "timezone", "SBSFC058-timezone-signature"),
    "SBSQL-2D1538908FB4": _sbsfc058_oracle_record("sb.temporal.age", "age", "timestamp and optional comparison timestamp", "interval text", "sblr.expr.temporal_age.v3", "age", "SBSFC058-age-bare"),
    "SBSQL-3053BF29742E": _sbsfc058_oracle_record("sb.scalar.regexp_split_to_table", "regexp_split_to_table", "string and pattern", "array descriptor containing split text values", "sblr.expr.regex_split_to_array.v3", "regexp_split_to_table", "SBSFC058-regexp-split-to-table-bare"),
    "SBSQL-308BCDB4E875": _sbsfc058_oracle_record("sb.temporal.day_name", "day_name(date[,locale])", "date and optional English locale", "character weekday name", "sblr.expr.temporal_day_name.v3", "day_name", "SBSFC058-day-name"),
    "SBSQL-352D7A25CBF2": _sbsfc058_oracle_record("sb.temporal.age_in_years", "age_in_years", "timestamp and optional comparison timestamp", "int64 complete years", "sblr.expr.temporal_age_in_years.v3", "age_in_years", "SBSFC058-age-in-years"),
    "SBSQL-3D0D5DEFD6B5": _sbsfc058_oracle_record("sb.temporal.next_day", "next_day", "date and English weekday", "date text", "sblr.expr.temporal_next_day.v3", "next_day", "SBSFC058-next-day-bare"),
    "SBSQL-3DF9A31D7101": _sbsfc058_oracle_record("sb.temporal.date_bin", "date_bin", "positive day/time stride, source timestamp, and origin timestamp", "timestamp text", "sblr.expr.temporal_date_bin.v3", "date_bin", "SBSFC058-date-bin-bare"),
    "SBSQL-4064D6205441": _sbsfc058_oracle_record("sb.scalar.gen_id", "gen_id(generator_name,increment)", "sequence name and nonzero increment", "int64 generated sequence value", "sblr.expr.sequence_gen_id.v3", "sequence_gen_id", "SBSFC058-gen-id", "volatile_value", "advances SBLR sequence runtime state; no transaction finality change"),
    "SBSQL-5159B04F9783": _sbsfc058_oracle_record("sb.scalar.currval", "currval(sequence_name)", "sequence name", "int64 current sequence value", "sblr.expr.sequence_currval.v3", "sequence_currval", "SBSFC058-currval-signature", "volatile_value", "reads existing SBLR sequence runtime state; no transaction finality change"),
    "SBSQL-54956088D143": _sbsfc058_oracle_record("sb.scalar.nextval", "nextval", "sequence name", "int64 next sequence value", "sblr.expr.sequence_nextval.v3", "sequence_nextval", "SBSFC058-nextval-bare", "volatile_value", "advances SBLR sequence runtime state; no transaction finality change"),
    "SBSQL-5ECBB4B91523": _sbsfc058_oracle_record("sb.temporal.date_bin", "date_bin(stride,source,origin)", "positive day/time stride, source timestamp, and origin timestamp", "timestamp text", "sblr.expr.temporal_date_bin.v3", "date_bin", "SBSFC058-date-bin-signature"),
    "SBSQL-5F642BC24E5F": _sbsfc058_oracle_record("sb.scalar.occurrences_regex", "OCCURRENCES_REGEX(patternINstring[FLAGflags])", "pattern, string, and optional flags", "int64 match count", "sblr.expr.regex_count.v3", "occurrences_regex", "SBSFC058-occurrences-regex-signature"),
    "SBSQL-6421F1CDC60B": _sbsfc058_oracle_record("sb.temporal.timezone", "timezone", "UTC/GMT or numeric offset zone and timestamp", "timestamp_tz text", "sblr.expr.temporal_timezone.v3", "timezone", "SBSFC058-timezone-bare"),
    "SBSQL-6A44A39395D3": _sbsfc058_oracle_record("sb.temporal.age_in_days", "age_in_days", "timestamp and optional comparison timestamp", "int64 complete days", "sblr.expr.temporal_age_in_days.v3", "age_in_days", "SBSFC058-age-in-days"),
    "SBSQL-76BE7E8C82E9": _sbsfc058_oracle_record("sb.scalar.translate_regex", "TRANSLATE_REGEX(patternINstringWITHreplacement[OCCURRENCEn|ALL][FLAGflags])", "pattern, string, replacement, optional occurrence/ALL, and flags", "character text", "sblr.expr.regex_replace.v3", "translate_regex", "SBSFC058-translate-regex-signature"),
    "SBSQL-7C1E9B3A101C": _sbsfc058_oracle_record("sb.scalar.to_char", "to_char(temporal|numeric,format)", "temporal or numeric value and format", "character text", "sblr.expr.scalar_to_char.v3", "to_char", "SBSFC058-to-char-signature"),
    "SBSQL-7DE0B158322A": _sbsfc058_oracle_record("sb.scalar.regexp_matches", "regexp_matches", "string, pattern, and optional flags", "array descriptor containing captures", "sblr.expr.regex_match_array.v3", "regexp_matches", "SBSFC058-regexp-matches-bare"),
    "SBSQL-819EAB680D05": _sbsfc058_oracle_record("sb.temporal.from_unixtime", "from_unixtime", "int64 Unix epoch seconds", "timestamp_tz text", "sblr.expr.temporal_from_unixtime.v3", "from_unixtime", "SBSFC058-from-unixtime-bare"),
    "SBSQL-822B9F2AD5C5": _sbsfc058_oracle_record("sb.scalar.lastval", "lastval", "no arguments", "int64 last identity value or SQL NULL", "sblr.expr.sequence_lastval.v3", "sequence_lastval", "SBSFC058-lastval", "volatile_value", "reads existing SBLR identity runtime state; no transaction finality change"),
    "SBSQL-83DF52C71DB1": _sbsfc058_oracle_record("sb.temporal.date_add", "date_add(date|timestamp,interval)", "date or timestamp and ISO-like interval", "date or timestamp text", "sblr.expr.temporal_date_add.v3", "date_add", "SBSFC058-date-add-signature"),
    "SBSQL-8457926FEA71": _sbsfc058_oracle_record("sb.scalar.to_date", "to_date(text,format)", "text and format", "date text", "sblr.expr.scalar_to_date.v3", "to_date", "SBSFC058-to-date-signature"),
    "SBSQL-88E2E2EB0657": _sbsfc058_oracle_record("sb.scalar.setval", "setval(sequence_name,n[,is_called])", "sequence name, value, and optional is_called boolean", "int64 assigned sequence value", "sblr.expr.sequence_setval.v3", "sequence_setval", "SBSFC058-setval-signature", "volatile_value", "sets SBLR sequence runtime state; no transaction finality change"),
    "SBSQL-89C364139695": _sbsfc058_oracle_record("sb.temporal.age_in_months", "age_in_months", "timestamp and optional comparison timestamp", "int64 complete months", "sblr.expr.temporal_age_in_months.v3", "age_in_months", "SBSFC058-age-in-months"),
    "SBSQL-924B70AB5641": _sbsfc058_oracle_record("sb.temporal.month_name", "month_name", "date and optional English locale", "character month name", "sblr.expr.temporal_month_name.v3", "month_name", "SBSFC058-month-name"),
    "SBSQL-94D34F1E05AF": _sbsfc058_oracle_record("sb.temporal.date_diff", "date_diff", "part, start timestamp, and end timestamp", "int64 difference", "sblr.expr.temporal_date_diff.v3", "date_diff", "SBSFC058-date-diff-bare"),
    "SBSQL-96A61FEA2B25": _sbsfc058_oracle_record("sb.scalar.setval", "setval", "sequence name, value, and optional is_called boolean", "int64 assigned sequence value", "sblr.expr.sequence_setval.v3", "sequence_setval", "SBSFC058-setval-bare", "volatile_value", "sets SBLR sequence runtime state; no transaction finality change"),
    "SBSQL-9AFD6269AD11": _sbsfc058_oracle_record("sb.scalar.to_number", "to_number(text,format)", "text and numeric format", "real64 numeric value", "sblr.expr.scalar_to_number.v3", "to_number", "SBSFC058-to-number"),
    "SBSQL-9B20B713B248": _sbsfc058_oracle_record("sb.scalar.to_timestamp", "to_timestamp", "text and timestamp format", "timestamp text", "sblr.expr.scalar_to_timestamp.v3", "to_timestamp", "SBSFC058-to-timestamp-bare"),
    "SBSQL-9E51B994A03C": _sbsfc058_oracle_record("sb.scalar.occurrences_regex", "OCCURRENCES_REGEX", "pattern, string, and optional flags", "int64 match count", "sblr.expr.regex_count.v3", "occurrences_regex", "SBSFC058-occurrences-regex-bare"),
    "SBSQL-A1FB79D234A0": _sbsfc058_oracle_record("sb.temporal.months_between", "months_between", "two date values", "real64 month delta", "sblr.expr.temporal_months_between.v3", "months_between", "SBSFC058-months-between-bare"),
    "SBSQL-A48CE29C1EF8": _sbsfc058_oracle_record("sb.temporal.next_day", "next_day(date,dow)", "date and English weekday", "date text", "sblr.expr.temporal_next_day.v3", "next_day", "SBSFC058-next-day-signature"),
    "SBSQL-A66313AFAD59": _sbsfc058_oracle_record("sb.temporal.months_between", "months_between(date,date)", "two date values", "real64 month delta", "sblr.expr.temporal_months_between.v3", "months_between", "SBSFC058-months-between-signature"),
    "SBSQL-AB367C935012": _sbsfc058_oracle_record("sb.scalar.substring_regex", "SUBSTRING_REGEX", "pattern, string, optional occurrence, group, and flags", "character text or SQL NULL", "sblr.expr.regex_substr.v3", "substring_regex", "SBSFC058-substring-regex-bare"),
    "SBSQL-B18EB4D81617": _sbsfc058_oracle_record("sb.temporal.date_diff", "date_diff(part,start,end)", "part, start timestamp, and end timestamp", "int64 difference", "sblr.expr.temporal_date_diff.v3", "date_diff", "SBSFC058-date-diff-signature"),
    "SBSQL-B4E283C07390": _sbsfc058_oracle_record("sb.scalar.to_date", "to_date", "text and format", "date text", "sblr.expr.scalar_to_date.v3", "to_date", "SBSFC058-to-date-bare"),
    "SBSQL-B684AF9349FE": _sbsfc058_oracle_record("sb.scalar.regexp_split_to_table", "regexp_split_to_table(text,pattern)", "string and pattern", "array descriptor containing split text values", "sblr.expr.regex_split_to_array.v3", "regexp_split_to_table", "SBSFC058-regexp-split-to-table-signature"),
    "SBSQL-BF05630BC377": _sbsfc058_oracle_record("sb.temporal.make_interval", "make_interval([years[,months[,...]]])", "up to seven int64 interval components", "interval text", "sblr.expr.temporal_make_interval.v3", "make_interval", "SBSFC058-make-interval-signature"),
    "SBSQL-C4FAF4EDAF96": _sbsfc058_oracle_record("sb.scalar.nextval", "nextval(sequence_name)", "sequence name", "int64 next sequence value", "sblr.expr.sequence_nextval.v3", "sequence_nextval", "SBSFC058-nextval-signature", "volatile_value", "advances SBLR sequence runtime state; no transaction finality change"),
    "SBSQL-CDD43912803F": _sbsfc058_oracle_record("sb.scalar.to_timestamp", "to_timestamp(text,format)", "text and timestamp format", "timestamp text", "sblr.expr.scalar_to_timestamp.v3", "to_timestamp", "SBSFC058-to-timestamp-signature"),
    "SBSQL-D13340E9CF67": _sbsfc058_oracle_record("sb.scalar.regexp_matches", "regexp_matches(string,pattern[,flags])", "string, pattern, and optional flags", "array descriptor containing captures", "sblr.expr.regex_match_array.v3", "regexp_matches", "SBSFC058-regexp-matches-signature"),
    "SBSQL-D14DD562957B": _sbsfc058_oracle_record("sb.scalar.translate_regex", "TRANSLATE_REGEX", "pattern, string, replacement, optional occurrence/ALL, and flags", "character text", "sblr.expr.regex_replace.v3", "translate_regex", "SBSFC058-translate-regex-bare"),
    "SBSQL-D9E42211F7B8": _sbsfc058_oracle_record("sb.scalar.to_char", "to_char", "temporal or numeric value and format", "character text", "sblr.expr.scalar_to_char.v3", "to_char", "SBSFC058-to-char-bare"),
    "SBSQL-E4A673B0FADE": _sbsfc058_oracle_record("sb.temporal.make_interval", "make_interval", "up to seven int64 interval components", "interval text", "sblr.expr.temporal_make_interval.v3", "make_interval", "SBSFC058-make-interval-bare"),
    "SBSQL-E6D846B78614": _sbsfc058_oracle_record("sb.temporal.from_unixtime", "from_unixtime(bigint)", "int64 Unix epoch seconds", "timestamp_tz text", "sblr.expr.temporal_from_unixtime.v3", "from_unixtime", "SBSFC058-from-unixtime-signature"),
    "SBSQL-EA11A0912D91": _sbsfc058_oracle_record("sb.scalar.substring_regex", "SUBSTRING_REGEX(patternINstring[OCCURRENCEn][GROUPg][FLAGflags])", "pattern, string, optional occurrence, group, and flags", "character text or SQL NULL", "sblr.expr.regex_substr.v3", "substring_regex", "SBSFC058-substring-regex-signature"),
}


def _sbsfc059_oracle_record(
    builtin_id: str,
    signature: str,
    argument_rule: str,
    return_type_rule: str,
    sblr_binding: str,
    engine_entrypoint: str,
    proof: str,
    volatility: str = "stable_statement",
    side_effects: str = "none; no mutation or transaction finality change",
) -> dict[str, object]:
    return {
        "builtin_id": builtin_id,
        "canonical_name": signature,
        "overloads": [{"signature": signature, "argument_rule": argument_rule}],
        "return_type_rule": return_type_rule,
        "coercion_rule": "bounded descriptor-to-SBLR scalar coercion for the SBSFC-059 fixture arguments only",
        "null_behavior": "SQL NULL propagates through the SBLR scalar runtime for nullable value arguments",
        "collation_charset_rule": "UTF-8/unicode_root for character descriptors; regex rows use the bounded C++ regex route",
        "timezone_rule": "UTC epoch arithmetic only for epoch/date_sub fixture rows; other rows are not timezone-sensitive",
        "volatility": volatility,
        "determinism": "deterministic for the SBSFC-059 fixture inputs except sequence advancement state",
        "side_effects": side_effects,
        "sblr_binding": sblr_binding,
        "ast_binding": f"ast.expr.sbsfc059.{engine_entrypoint}",
        "engine_entrypoint": engine_entrypoint,
        "optimizer_properties": {
            "foldable": False,
            "index_eligible": False,
            "generated_column_eligible": False,
            "cost_class": "cpu_scalar",
        },
        "security_policy": (
            "executes inside the SBLR expression runtime using in-core scalar helpers and the existing SBLR sequence runtime; "
            "no parser SQL execution, donor execution, storage finality, WAL/recovery shortcut, cluster provider authority, or external service is used"
        ),
        "donor_rendering": "parser renders native SBsql spelling and diagnostics through generated registry evidence",
        "error_semantics": "invalid arity, malformed regex/temporal text, unsupported locale, or sequence increment errors use canonical SBLR diagnostics",
        "syntax_forms": ["function_call"],
        "conformance_cases": [proof],
    }


SBSFC059_EXPRESSION_RUNTIME_CLEANUP_ORACLE_OVERRIDES = {
    "SBSQL-EBE863C39BD6": _sbsfc059_oracle_record("sb.temporal.month_name", "month_name(date[,locale])", "date and optional English/C locale", "character month name", "sblr.expr.temporal_month_name.v3", "month_name", "SBSFC059-month-name-signature"),
    "SBSQL-ED8C540CF5B1": _sbsfc059_oracle_record("sb.temporal.date_sub", "date_sub(date|timestamp,interval)", "date or timestamp and ISO-like interval", "date or timestamp text", "sblr.expr.temporal_date_sub.v3", "date_sub", "SBSFC059-date-sub-signature"),
    "SBSQL-EF179A79677B": _sbsfc059_oracle_record("sb.temporal.epoch", "epoch(timestamp)", "one ISO-like timestamp value", "int64 Unix epoch seconds", "sblr.expr.temporal_epoch.v3", "epoch", "SBSFC059-epoch-signature"),
    "SBSQL-F0999F1E0637": _sbsfc059_oracle_record("sb.scalar.gen_id", "gen_id", "sequence name and nonzero increment", "int64 generated sequence value", "sblr.expr.sequence_gen_id.v3", "sequence_gen_id", "SBSFC059-gen-id-bare", "volatile_value", "advances SBLR sequence runtime state; no transaction finality change"),
    "SBSQL-F6C685816805": _sbsfc059_oracle_record("sb.temporal.day_name", "day_name", "date and optional English/C locale", "character weekday name", "sblr.expr.temporal_day_name.v3", "day_name", "SBSFC059-day-name-bare"),
    "SBSQL-FB4C56854614": _sbsfc059_oracle_record("sb.temporal.date_sub", "date_sub", "date or timestamp and ISO-like interval", "date or timestamp text", "sblr.expr.temporal_date_sub.v3", "date_sub", "SBSFC059-date-sub-bare"),
    "SBSQL-FEC27B990B29": _sbsfc059_oracle_record("sb.scalar.position_regex", "POSITION_REGEX", "pattern, string, optional occurrence, flags, and mode", "int64 one-based position or 0", "sblr.expr.regex_position.v3", "position_regex", "SBSFC059-position-regex-bare"),
}

SBSFC034_TEXT_TRIGRAM_BIT_STRING_ORACLE_OVERRIDES = {
    "SBSQL-D7C03289F1AA": {
        "builtin_id": "sb.scalar.bit_string_position",
        "canonical_name": "bit_string_position",
        "overloads": [{"signature": "bit_string_position(bit_string,bit_string)", "argument_rule": "bit-string needle and bit-string haystack"}],
        "return_type_rule": "int64 one-based bit position or 0 when not found",
        "coercion_rule": "both arguments must be bit_string binary descriptors",
        "null_behavior": "null input returns SQL null int64",
        "collation_charset_rule": "not applicable",
        "timezone_rule": "not applicable",
        "volatility": "immutable",
        "determinism": "deterministic",
        "side_effects": "none",
        "sblr_binding": "sblr.expr.scalar_bit_string_position.v3",
        "ast_binding": "ast.expr.scalar_bit_string_position",
        "engine_entrypoint": "bit_string_position",
        "optimizer_properties": {"foldable": False, "index_eligible": False, "generated_column_eligible": True, "cost_class": "cpu_scalar"},
        "security_policy": "pure bit-string helper; no catalog, storage, security, transaction, donor, plugin, or cluster authority",
        "donor_rendering": "parser renders native SBsql spelling and diagnostics through generated registry evidence",
        "error_semantics": "invalid arity or mixed descriptors refuse with SBSQL.FUNCTION.INVALID_INPUT",
        "syntax_forms": ["function_call"],
        "conformance_cases": ["SBSFC034-bit-string-position"],
    },
    "SBSQL-4CE572F88F79": {
        "builtin_id": "sb.scalar.bit_string_substring",
        "canonical_name": "bit_string_substring",
        "overloads": [{"signature": "bit_string_substring(bit_string,start[,length])", "argument_rule": "bit-string source, one-based int64 start, and optional int64 length"}],
        "return_type_rule": "bit_string slice using the existing bit-string-aware substring route",
        "coercion_rule": "source must be bit_string and start/length must be int64 values",
        "null_behavior": "null input returns SQL null bit_string",
        "collation_charset_rule": "not applicable",
        "timezone_rule": "not applicable",
        "volatility": "immutable",
        "determinism": "deterministic",
        "side_effects": "none",
        "sblr_binding": "sblr.expr.scalar_bit_string_substring.v3",
        "ast_binding": "ast.expr.scalar_bit_string_substring",
        "engine_entrypoint": "bit_string_substring",
        "optimizer_properties": {"foldable": False, "index_eligible": False, "generated_column_eligible": True, "cost_class": "cpu_scalar"},
        "security_policy": "pure bit-string helper; no catalog, storage, security, transaction, donor, plugin, or cluster authority",
        "donor_rendering": "parser renders native SBsql spelling and diagnostics through generated registry evidence",
        "error_semantics": "invalid arity, non-int64 bounds, or invalid bounds refuse with SBSQL.FUNCTION.INVALID_INPUT",
        "syntax_forms": ["function_call"],
        "conformance_cases": ["SBSFC034-bit-string-substring"],
    },
    "SBSQL-38CE72403078": {
        "builtin_id": "sb.scalar.show_trgm",
        "canonical_name": "show_trgm",
        "overloads": [{"signature": "show_trgm(text)", "argument_rule": "one text argument; bare form is admitted and refuses invalid input at runtime"}],
        "return_type_rule": "array descriptor containing deterministic JSON text array of unique trigrams",
        "coercion_rule": "argument converted through scalar text representation",
        "null_behavior": "null input returns SQL null array",
        "collation_charset_rule": "implementation default array descriptor with JSON text payload",
        "timezone_rule": "not applicable",
        "volatility": "immutable",
        "determinism": "deterministic",
        "side_effects": "none",
        "sblr_binding": "sblr.expr.scalar_show_trgm.v3",
        "ast_binding": "ast.expr.scalar_show_trgm",
        "engine_entrypoint": "show_trgm",
        "optimizer_properties": {"foldable": False, "index_eligible": False, "generated_column_eligible": True, "cost_class": "cpu_scalar"},
        "security_policy": "pure trigram helper; no donor/plugin call, catalog lookup, storage lookup, transaction finality, WAL/recovery, or cluster behavior",
        "donor_rendering": "parser renders native SBsql spelling and diagnostics through generated registry evidence",
        "error_semantics": "invalid arity refuses with SBSQL.FUNCTION.INVALID_INPUT",
        "syntax_forms": ["function_call"],
        "conformance_cases": ["SBSFC034-show-trgm-bare-invalid", "SBSFC034-show-trgm-text", "SBSFC034-show-trgm-null"],
    },
    "SBSQL-A2A770275F65": "SBSQL-38CE72403078",
    "SBSQL-F01F3112C706": {
        "builtin_id": "sb.scalar.pg_trgm",
        "canonical_name": "pg_trgm",
        "overloads": [{"signature": "pg_trgm()", "argument_rule": "zero arguments"}],
        "return_type_rule": "json_document deterministic native pg_trgm capability metadata",
        "coercion_rule": "not applicable",
        "null_behavior": "not applicable",
        "collation_charset_rule": "implementation default json_document descriptor",
        "timezone_rule": "not applicable",
        "volatility": "immutable",
        "determinism": "deterministic",
        "side_effects": "none",
        "sblr_binding": "sblr.expr.scalar_pg_trgm.v3",
        "ast_binding": "ast.expr.scalar_pg_trgm",
        "engine_entrypoint": "pg_trgm",
        "optimizer_properties": {"foldable": False, "index_eligible": False, "generated_column_eligible": False, "cost_class": "metadata_scalar"},
        "security_policy": "fixed capability metadata route; no donor/plugin call, catalog lookup, storage lookup, transaction finality, WAL/recovery, or cluster behavior",
        "donor_rendering": "parser renders native SBsql spelling and diagnostics through generated registry evidence",
        "error_semantics": "nonzero arity refuses with SBSQL.FUNCTION.INVALID_INPUT",
        "syntax_forms": ["function_call"],
        "conformance_cases": ["SBSFC034-pg-trgm-capability"],
    },
}


def _sbsfc035_oracle_record(
    name: str,
    signature: str,
    argument_rule: str,
    return_type_rule: str,
    null_behavior: str,
    conformance_cases: list[str],
) -> dict:
    return {
        "builtin_id": f"sb.scalar.{name}",
        "canonical_name": name,
        "overloads": [{"signature": signature, "argument_rule": argument_rule}],
        "return_type_rule": return_type_rule,
        "coercion_rule": (
            "range arguments accept deterministic bracket text like [lower,upper) "
            "or flat JSON descriptors with lower, upper, lower_inc, upper_inc, and empty fields; "
            "element comparison uses numeric ordering when both sides parse as finite numbers, otherwise lexical text ordering"
        ),
        "null_behavior": null_behavior,
        "collation_charset_rule": "implementation default character descriptor for textual bounds; boolean helpers return boolean descriptor",
        "timezone_rule": "not applicable",
        "volatility": "immutable",
        "determinism": "deterministic",
        "side_effects": "none",
        "sblr_binding": f"sblr.expr.scalar_{name}.v3",
        "ast_binding": f"ast.expr.scalar_{name}",
        "engine_entrypoint": name,
        "optimizer_properties": {
            "foldable": False,
            "index_eligible": False,
            "generated_column_eligible": True,
            "cost_class": "cpu_scalar",
        },
        "security_policy": (
            "pure range scalar helper; accepts deterministic textual and flat-json range descriptors; "
            "no parser SQL execution, catalog/storage lookup, donor/plugin call, transaction finality, WAL/recovery, or cluster behavior"
        ),
        "donor_rendering": "parser renders native SBsql spelling and diagnostics through generated registry evidence",
        "error_semantics": "invalid arity or malformed descriptor refuses with SBSQL.FUNCTION.INVALID_INPUT",
        "syntax_forms": ["function_call"],
        "conformance_cases": conformance_cases,
    }


SBSFC035_RANGE_SCALAR_HELPER_ORACLE_OVERRIDES = {
    "SBSQL-DBA02300598B": _sbsfc035_oracle_record(
        "range_contains",
        "range_contains(range,range)",
        "two deterministic range descriptors",
        "boolean true when the right range is empty or wholly contained by the left range",
        "SQL null input returns SQL null boolean",
        ["SBSFC035-range-contains-true", "SBSFC035-range-contains-empty"],
    ),
    "SBSQL-33FC6A422D2A": _sbsfc035_oracle_record(
        "range_contains_element",
        "range_contains_element(range,element)",
        "range descriptor and scalar element",
        "boolean true when the element falls within the range bounds",
        "SQL null input returns SQL null boolean",
        ["SBSFC035-range-contains-element", "SBSFC035-range-json-contains-element"],
    ),
    "SBSQL-E67AFDCD6017": _sbsfc035_oracle_record(
        "range_lower",
        "range_lower(range)",
        "one deterministic range descriptor",
        "character textual lower bound or SQL null when the range is empty or unbounded below",
        "SQL null input returns SQL null character",
        ["SBSFC035-range-lower", "SBSFC035-range-lower-empty-null"],
    ),
    "SBSQL-23C8E30D9502": _sbsfc035_oracle_record(
        "range_lower_inc",
        "range_lower_inc(range)",
        "one deterministic range descriptor",
        "boolean true when the lower bound is present and inclusive",
        "SQL null input returns SQL null boolean",
        ["SBSFC035-range-lower-inc"],
    ),
    "SBSQL-68F1659E06B7": _sbsfc035_oracle_record(
        "range_overlaps",
        "range_overlaps(range,range)",
        "two deterministic range descriptors",
        "boolean true when non-empty ranges share at least one included point",
        "SQL null input returns SQL null boolean",
        ["SBSFC035-range-overlaps-boundary", "SBSFC035-range-overlaps-invalid"],
    ),
    "SBSQL-98C59707CA44": _sbsfc035_oracle_record(
        "range_strictly_left",
        "range_strictly_left(range,range)",
        "two deterministic range descriptors",
        "boolean true when the left range ends before the right range begins without overlap",
        "SQL null input returns SQL null boolean",
        ["SBSFC035-range-strictly-left"],
    ),
    "SBSQL-866D51FDCD73": _sbsfc035_oracle_record(
        "range_strictly_right",
        "range_strictly_right(range,range)",
        "two deterministic range descriptors",
        "boolean true when the left range begins after the right range ends without overlap",
        "SQL null input returns SQL null boolean",
        ["SBSFC035-range-strictly-right"],
    ),
    "SBSQL-7547BF6B8187": _sbsfc035_oracle_record(
        "range_upper",
        "range_upper(range)",
        "one deterministic range descriptor",
        "character textual upper bound or SQL null when the range is empty or unbounded above",
        "SQL null input returns SQL null character",
        ["SBSFC035-range-upper"],
    ),
    "SBSQL-A383B9185803": _sbsfc035_oracle_record(
        "range_upper_inc",
        "range_upper_inc(range)",
        "one deterministic range descriptor",
        "boolean true when the upper bound is present and inclusive",
        "SQL null input returns SQL null boolean",
        ["SBSFC035-range-upper-inc", "SBSFC035-range-upper-inc-null"],
    ),
}


def _sbsfc036_oracle_record(function_id: str, conformance_cases: list[str]) -> dict:
    name = function_id.rsplit(".", 1)[-1]
    return {
        "builtin_id": function_id,
        "canonical_name": function_id,
        "return_type_rule": (
            "spatial geometry scalar helper returns deterministic bounded geometry, character, "
            "boolean, int64, real64, bytea, or json_document descriptor according to function id"
        ),
        "overloads": [
            {
                "signature": function_id,
                "argument_rule": (
                    "bounded in-core spatial scalar helper arguments: WKT geometry descriptors, "
                    "POINT/LINESTRING/POLYGON/GEOMETRYCOLLECTION text, narrow GeoJSON text, "
                    "point WKB hex text, numeric SRID/tolerance/distance, or point coordinates"
                ),
            }
        ],
        "coercion_rule": (
            "accepts only deterministic bounded textual geometry descriptors, narrow GeoJSON, "
            "point WKB hex, and scalar numeric helper arguments; no donor or catalog coercion"
        ),
        "null_behavior": "SQL null input returns SQL null using the target descriptor where applicable",
        "collation_charset_rule": "implementation default character descriptor for textual geometry and GeoJSON output; boolean/numeric helpers use native scalar descriptors",
        "timezone_rule": "not applicable",
        "volatility": "immutable",
        "determinism": "deterministic",
        "side_effects": "none",
        "sblr_binding": f"sblr.expr.scalar_{name}.v3",
        "ast_binding": f"ast.expr.scalar_{name}",
        "engine_entrypoint": name,
        "optimizer_properties": {
            "foldable": False,
            "index_eligible": False,
            "generated_column_eligible": True,
            "cost_class": "cpu_scalar",
        },
        "security_policy": (
            "pure bounded spatial geometry scalar helper; deterministic in-core WKT, POINT, "
            "LINESTRING, POLYGON, GEOMETRYCOLLECTION, narrow GeoJSON, and point WKB hex route; "
            "no parser SQL execution, donor/plugin call, storage/catalog lookup, transaction "
            "finality, WAL/recovery, or cluster behavior"
        ),
        "donor_rendering": "parser renders native SBsql spelling and diagnostics through generated registry evidence",
        "error_semantics": (
            "invalid arity, malformed geometry, or unsupported WKB refuses with "
            "SBSQL.FUNCTION.INVALID_INPUT"
        ),
        "syntax_forms": ["function_call"],
        "conformance_cases": conformance_cases,
    }


SBSFC036_SPATIAL_GEOMETRY_SCALAR_ROW_EVIDENCE = {
    "SBSQL-007BD17BDF55": {"function_id": "sb.scalar.st_x", "proof": "SBSFC036-st-x"},
    "SBSQL-01C6BF2303B1": {"function_id": "sb.scalar.st_makepoint", "proof": "SBSFC036-st-makepoint"},
    "SBSQL-064CC33574E2": {"function_id": "sb.scalar.st_crosses_g1_g2", "proof": "SBSFC036-st-crosses-signature"},
    "SBSQL-14816C2A7E33": {"function_id": "sb.scalar.st_simplify", "proof": "SBSFC036-st-simplify"},
    "SBSQL-14CD6B2AA8E3": {"function_id": "sb.scalar.st_geometrytype_geometry", "proof": "SBSFC036-st-geometrytype"},
    "SBSQL-16705FD6AD8C": {"function_id": "sb.scalar.st_geogfromtext", "proof": "SBSFC036-st-geogfromtext"},
    "SBSQL-1817177FD841": {"function_id": "sb.scalar.st_contains_g1_g2", "proof": "SBSFC036-st-contains-signature"},
    "SBSQL-19B4EE69BD6A": {"function_id": "sb.scalar.geom_extent_geometry", "proof": "SBSFC036-geom-extent"},
    "SBSQL-19C49EFCE56D": {"function_id": "sb.scalar.st_y", "proof": "SBSFC036-st-y"},
    "SBSQL-20DB96AF4D98": {"function_id": "sb.scalar.st_crosses", "proof": "SBSFC036-st-crosses"},
    "SBSQL-2ED82920C391": {"function_id": "sb.scalar.st_disjoint", "proof": "SBSFC036-st-disjoint"},
    "SBSQL-34B32A5FF887": {"function_id": "sb.scalar.st_transform_geometry_target_srid", "proof": "SBSFC036-st-transform"},
    "SBSQL-37FDE5D4CA38": {"function_id": "sb.scalar.st_numpoints_geometry", "proof": "SBSFC036-st-numpoints-signature"},
    "SBSQL-39EC9401DD7F": {"function_id": "sb.scalar.st_asbinary", "proof": "SBSFC036-st-asbinary"},
    "SBSQL-3B96D65453D5": {"function_id": "sb.scalar.st_simplify_geometry_tolerance", "proof": "SBSFC036-st-simplify-signature"},
    "SBSQL-3E576350E9B0": {"function_id": "sb.scalar.st_assvg_geometry", "proof": "SBSFC036-st-assvg-signature"},
    "SBSQL-3F84A2CEBD71": {"function_id": "sb.scalar.geom_union_geometry", "proof": "SBSFC036-geom-union"},
    "SBSQL-4016AFBC31B8": {"function_id": "sb.scalar.st_perimeter_geometry", "proof": "SBSFC036-st-perimeter-signature"},
    "SBSQL-48C47B22CD64": {"function_id": "sb.scalar.st_envelope", "proof": "SBSFC036-st-envelope"},
    "SBSQL-4976BE206EC9": {"function_id": "sb.scalar.st_distance", "proof": "SBSFC036-st-distance"},
    "SBSQL-53EF6CC1B84B": {"function_id": "sb.scalar.st_x_point", "proof": "SBSFC036-st-x-point"},
    "SBSQL-549915041FC5": {"function_id": "sb.scalar.st_distance_g1_g2", "proof": "SBSFC036-st-distance-signature"},
    "SBSQL-56C21F337176": {"function_id": "sb.scalar.st_envelope_geometry", "proof": "SBSFC036-st-envelope-signature"},
    "SBSQL-577953487165": {"function_id": "sb.scalar.st_astext", "proof": "SBSFC036-st-astext"},
    "SBSQL-581DB27EE2F3": {"function_id": "sb.scalar.st_astext_geometry", "proof": "SBSFC036-st-astext-signature"},
    "SBSQL-5C008F9218F5": {"function_id": "sb.scalar.st_asbinary_geometry", "proof": "SBSFC036-st-asbinary-signature"},
    "SBSQL-610EB642822F": {"function_id": "sb.scalar.st_touches_g1_g2", "proof": "SBSFC036-st-touches"},
    "SBSQL-63555A174F42": {"function_id": "sb.scalar.st_setsrid_geometry_srid", "proof": "SBSFC036-st-setsrid"},
    "SBSQL-65C7DA8D048B": {"function_id": "sb.scalar.st_perimeter", "proof": "SBSFC036-st-perimeter"},
    "SBSQL-6B3AC153575D": {"function_id": "sb.scalar.st_assvg", "proof": "SBSFC036-st-assvg"},
    "SBSQL-6CC46392ADAC": {"function_id": "sb.scalar.st_buffer_geometry_distance", "proof": "SBSFC036-st-buffer-signature"},
    "SBSQL-6E042C3D9DA7": {"function_id": "sb.scalar.st_npoints", "proof": "SBSFC036-st-npoints"},
    "SBSQL-6EE712BB2CA2": {"function_id": "sb.scalar.st_numpoints", "proof": "SBSFC036-st-numpoints"},
    "SBSQL-71E8B706D3F5": {"function_id": "sb.scalar.st_overlaps", "proof": "SBSFC036-st-overlaps"},
    "SBSQL-7387E0B53393": {"function_id": "sb.scalar.st_intersects", "proof": "SBSFC036-st-intersects"},
    "SBSQL-774E359ADAF4": {"function_id": "sb.scalar.st_geomfromwkb_wkb_srid", "proof": "SBSFC036-st-geomfromwkb-signature"},
    "SBSQL-779284739DB2": {"function_id": "sb.scalar.st_equals_g1_g2", "proof": "SBSFC036-st-equals"},
    "SBSQL-78846923611D": {"function_id": "sb.scalar.st_geomfromwkb", "proof": "SBSFC036-st-geomfromwkb"},
    "SBSQL-7B1A7ED9A65B": {"function_id": "sb.scalar.st_makepolygon_linestring_holesarray", "proof": "SBSFC036-st-makepolygon"},
    "SBSQL-7C81986B79D9": {"function_id": "sb.scalar.st_srid_geometry", "proof": "SBSFC036-st-srid"},
    "SBSQL-7E7F908D3782": {"function_id": "sb.scalar.st_geomfromgeojson_text", "proof": "SBSFC036-st-geomfromgeojson"},
    "SBSQL-7F1AA7BC1C1B": {"function_id": "sb.scalar.st_centroid", "proof": "SBSFC036-st-centroid"},
    "SBSQL-81134D15580F": {"function_id": "sb.scalar.st_contains", "proof": "SBSFC036-st-contains"},
    "SBSQL-8126547CB199": {"function_id": "sb.scalar.st_intersection", "proof": "SBSFC036-st-intersection"},
    "SBSQL-82098A2E3A54": {"function_id": "sb.scalar.st_area_geometry", "proof": "SBSFC036-st-area"},
    "SBSQL-83D324A5BD04": {"function_id": "sb.scalar.st_within_g1_g2", "proof": "SBSFC036-st-within"},
    "SBSQL-8A2191CBD1FF": {"function_id": "sb.scalar.st_buffer", "proof": "SBSFC036-st-buffer"},
    "SBSQL-918FBB8B8F9A": {"function_id": "sb.scalar.geom_collect", "proof": "SBSFC036-geom-collect"},
    "SBSQL-9589706D80E8": {"function_id": "sb.scalar.st_asgeojson_geometry_maxdecimaldigits", "proof": "SBSFC036-st-asgeojson"},
    "SBSQL-9639CB5F0B9A": {"function_id": "sb.scalar.st_intersects_g1_g2", "proof": "SBSFC036-st-intersects-signature"},
}


SBSFC036_SPATIAL_GEOMETRY_SCALAR_ORACLE_OVERRIDES = {
    surface_id: _sbsfc036_oracle_record(
        evidence["function_id"],
        [evidence["proof"], "SBSFC036-st-x-invalid"],
    )
    for surface_id, evidence in SBSFC036_SPATIAL_GEOMETRY_SCALAR_ROW_EVIDENCE.items()
}


def _sbsfc037_oracle_record(function_id: str, conformance_cases: list[str]) -> dict:
    name = function_id.rsplit(".", 1)[-1]
    return {
        "builtin_id": function_id,
        "canonical_name": function_id,
        "return_type_rule": (
            "XML/multimodel scalar helper returns bounded xml_document, xml fragment, "
            "boolean, or json_document descriptor according to function id"
        ),
        "overloads": [
            {
                "signature": function_id,
                "argument_rule": (
                    "bounded in-core XML scalar helper arguments: XML/text fragments, "
                    "name-like aliases, path tokens, declaration controls, and scalar "
                    "content values only"
                ),
            }
        ],
        "coercion_rule": (
            "accepts deterministic bounded textual XML fragments and scalar values; "
            "escapes non-XML descriptors and rejects unsafe XML names, comments, PI "
            "content, declaration values, arity, or byte limits"
        ),
        "null_behavior": "SQL null input propagates to SQL null for document/boolean helpers where applicable",
        "collation_charset_rule": "UTF-8 text semantics for XML/JSON descriptor output; no collation-sensitive comparison",
        "timezone_rule": "not applicable",
        "volatility": "immutable",
        "determinism": "deterministic",
        "side_effects": "none",
        "sblr_binding": f"sblr.expr.xml_{name}.v3",
        "ast_binding": f"ast.expr.xml_{name}",
        "engine_entrypoint": f"xml_{name}",
        "optimizer_properties": {
            "foldable": False,
            "index_eligible": False,
            "generated_column_eligible": True,
            "cost_class": "cpu_scalar",
        },
        "security_policy": (
            "pure bounded XML/multimodel scalar helper; deterministic in-core XML text "
            "construction/inspection only; no parser SQL execution, donor/plugin XML "
            "engine, storage/catalog lookup, transaction finality, WAL/recovery, or "
            "cluster behavior"
        ),
        "donor_rendering": "parser renders native SBsql XML spelling and diagnostics through generated registry evidence",
        "error_semantics": (
            "invalid arity, unsafe XML name/content, malformed XML control value, or "
            "bounded-size violation refuses with SBSQL.FUNCTION.INVALID_INPUT"
        ),
        "syntax_forms": ["function_call", "xml_special_form"],
        "conformance_cases": conformance_cases,
    }


SBSFC037_XML_MULTIMODEL_SCALAR_ROW_EVIDENCE = {
    "SBSQL-0C16676374C8": {"function_id": "sb.xml.forest", "proof": "SBSFC037-xmlforest"},
    "SBSQL-6C89436D2254": {"function_id": "sb.xml.forest", "proof": "SBSFC037-xmlforest-as"},
    "SBSQL-2BBA1DA50B23": {"function_id": "sb.xml.cast", "proof": "SBSFC037-xmlcast"},
    "SBSQL-0C8A8486F751": {"function_id": "sb.xml.cast", "proof": "SBSFC037-xmlcast-as"},
    "SBSQL-104DD993AED4": {"function_id": "sb.xml.exists", "proof": "SBSFC037-xmlexists"},
    "SBSQL-EEA4907830CB": {"function_id": "sb.xml.exists", "proof": "SBSFC037-xmlexists-passing"},
    "SBSQL-1FD7CBD0921F": {"function_id": "sb.xml.attributes", "proof": "SBSFC037-xmlattributes"},
    "SBSQL-E2022718464C": {"function_id": "sb.xml.attributes", "proof": "SBSFC037-xmlattributes-as"},
    "SBSQL-934D2E7C0508": {"function_id": "sb.xml.concat", "proof": "SBSFC037-xmlconcat"},
    "SBSQL-2B38A69D425B": {"function_id": "sb.xml.concat", "proof": "SBSFC037-xmlconcat-list"},
    "SBSQL-4F494D9A6610": {"function_id": "sb.xml.comment", "proof": "SBSFC037-xmlcomment"},
    "SBSQL-7881C81BBBE8": {"function_id": "sb.xml.comment", "proof": "SBSFC037-xmlcomment-text"},
    "SBSQL-DC75730A32EA": {"function_id": "sb.xml.pi", "proof": "SBSFC037-xmlpi"},
    "SBSQL-51E09D00A979": {"function_id": "sb.xml.pi", "proof": "SBSFC037-xmlpi-name"},
    "SBSQL-52CC2FA7719D": {"function_id": "sb.xml.root", "proof": "SBSFC037-xmlroot"},
    "SBSQL-A31D3F4A9E77": {"function_id": "sb.xml.root", "proof": "SBSFC037-xmlroot-version"},
    "SBSQL-54EBF8EDE58A": {"function_id": "sb.xml.element", "proof": "SBSFC037-xmlelement-name"},
    "SBSQL-5702FA6BF536": {"function_id": "sb.xml.agg", "proof": "SBSFC037-xmlagg"},
    "SBSQL-94785A48EF57": {"function_id": "sb.xml.agg", "proof": "SBSFC037-xmlagg-order"},
    "SBSQL-F0C5F1661298": {"function_id": "sb.xml.table", "proof": "SBSFC037-xmltable"},
    "SBSQL-796CAD6CD56E": {"function_id": "sb.xml.table", "proof": "SBSFC037-xmltable-columns"},
}


SBSFC037_XML_MULTIMODEL_SCALAR_ORACLE_OVERRIDES = {
    surface_id: _sbsfc037_oracle_record(
        evidence["function_id"],
        [evidence["proof"], "SBSFC037-xmlcomment-invalid"],
    )
    for surface_id, evidence in SBSFC037_XML_MULTIMODEL_SCALAR_ROW_EVIDENCE.items()
}

SBSFC038_SPATIAL_TAIL_SCALAR_ROW_EVIDENCE = {
    "SBSQL-9689873CEFCA": {"function_id": "sb.scalar.st_setsrid_geometry_srid", "proof": "SBSFC038-st-setsrid"},
    "SBSQL-A01836D957A0": {"function_id": "sb.scalar.st_dwithin", "proof": "SBSFC038-st-dwithin-signature"},
    "SBSQL-A0BCD0E4C3DC": {"function_id": "sb.scalar.st_m", "proof": "SBSFC038-st-m"},
    "SBSQL-A57555BEE95E": {"function_id": "sb.scalar.st_overlaps", "proof": "SBSFC038-st-overlaps-signature"},
    "SBSQL-A5BDCC976DD0": {"function_id": "sb.scalar.st_difference", "proof": "SBSFC038-st-difference-signature"},
    "SBSQL-A5D10A16CCFA": {"function_id": "sb.scalar.st_z", "proof": "SBSFC038-st-z"},
    "SBSQL-A8D99D74565F": {"function_id": "sb.scalar.st_area_geometry", "proof": "SBSFC038-st-area"},
    "SBSQL-AD4F92702329": {"function_id": "sb.scalar.st_asmvtgeom", "proof": "SBSFC038-st-asmvtgeom"},
    "SBSQL-AEFECB9626BB": {"function_id": "sb.scalar.st_difference", "proof": "SBSFC038-st-difference"},
    "SBSQL-B1718AA4E4B6": {"function_id": "sb.scalar.st_length", "proof": "SBSFC038-st-length"},
    "SBSQL-B26EC3DF7AFB": {"function_id": "sb.scalar.geom_union_geometry", "proof": "SBSFC038-geom-union"},
    "SBSQL-B288AFD4ECE5": {"function_id": "sb.scalar.geom_collect", "proof": "SBSFC038-geom-collect-geometry"},
    "SBSQL-B5825D1638CA": {"function_id": "sb.scalar.st_makepoint", "proof": "SBSFC038-st-makepoint-xyzm"},
    "SBSQL-BA4115A6DBA5": {"function_id": "sb.scalar.st_equals_g1_g2", "proof": "SBSFC038-st-equals"},
    "SBSQL-BD9DD4BBECA7": {"function_id": "sb.scalar.st_intersection", "proof": "SBSFC038-st-intersection-signature"},
    "SBSQL-C03FDC7E09D0": {"function_id": "sb.scalar.st_centroid", "proof": "SBSFC038-st-centroid-geometry"},
    "SBSQL-C44E7F61A475": {"function_id": "sb.scalar.st_geometrytype_geometry", "proof": "SBSFC038-st-geometrytype"},
    "SBSQL-C557FC25C1DF": {"function_id": "sb.scalar.st_geomfromgeojson_text", "proof": "SBSFC038-st-geomfromgeojson"},
    "SBSQL-C5B5E28021D3": {"function_id": "sb.scalar.st_makeline", "proof": "SBSFC038-st-makeline-signature"},
    "SBSQL-C6D14CCCA2D1": {"function_id": "sb.scalar.st_geomfromtext", "proof": "SBSFC038-st-geomfromtext-signature"},
    "SBSQL-CBD9B6358B34": {"function_id": "sb.scalar.geom_extent_geometry", "proof": "SBSFC038-geom-extent"},
    "SBSQL-CBE14326BD0B": {"function_id": "sb.scalar.st_symdifference", "proof": "SBSFC038-st-symdifference-signature"},
    "SBSQL-CF31B52FAA1F": {"function_id": "sb.scalar.st_asgeojson_geometry_maxdecimaldigits", "proof": "SBSFC038-st-asgeojson"},
    "SBSQL-CFE56EE1BAC3": {"function_id": "sb.scalar.st_dwithin", "proof": "SBSFC038-st-dwithin"},
    "SBSQL-D3C5EA9765BE": {"function_id": "sb.scalar.st_touches_g1_g2", "proof": "SBSFC038-st-touches"},
    "SBSQL-D5BEA7309046": {"function_id": "sb.scalar.st_transform_geometry_target_srid", "proof": "SBSFC038-st-transform"},
    "SBSQL-DB22C5B8D6E6": {"function_id": "sb.scalar.st_covers", "proof": "SBSFC038-st-covers-signature"},
    "SBSQL-E211ACCD957F": {"function_id": "sb.scalar.st_srid_geometry", "proof": "SBSFC038-st-srid"},
    "SBSQL-E43632706687": {"function_id": "sb.scalar.st_disjoint", "proof": "SBSFC038-st-disjoint-signature"},
    "SBSQL-E4EB3BEDAA0A": {"function_id": "sb.scalar.st_convexhull", "proof": "SBSFC038-st-convexhull-geometry"},
    "SBSQL-E73C186D5991": {"function_id": "sb.scalar.st_length", "proof": "SBSFC038-st-length-geometry"},
    "SBSQL-E8E12B064114": {"function_id": "sb.scalar.st_convexhull", "proof": "SBSFC038-st-convexhull"},
    "SBSQL-F053EEAC95CD": {"function_id": "sb.scalar.st_npoints", "proof": "SBSFC038-st-npoints-geometry"},
    "SBSQL-F1B58755A174": {"function_id": "sb.scalar.st_makeline", "proof": "SBSFC038-st-makeline"},
    "SBSQL-F21F901FC2AF": {"function_id": "sb.scalar.st_makepolygon_linestring_holesarray", "proof": "SBSFC038-st-makepolygon"},
    "SBSQL-F3C89846D91C": {"function_id": "sb.scalar.st_geomfromtext", "proof": "SBSFC038-st-geomfromtext"},
    "SBSQL-F4AE1FA62237": {"function_id": "sb.scalar.st_within_g1_g2", "proof": "SBSFC038-st-within"},
    "SBSQL-F763191B3241": {"function_id": "sb.scalar.st_symdifference", "proof": "SBSFC038-st-symdifference"},
    "SBSQL-F7D5231CA0E4": {"function_id": "sb.scalar.st_covers", "proof": "SBSFC038-st-covers"},
    "SBSQL-F8050BCAF06D": {"function_id": "sb.scalar.st_union", "proof": "SBSFC038-st-union"},
    "SBSQL-F985930BDD2F": {"function_id": "sb.scalar.st_geogfromtext", "proof": "SBSFC038-st-geogfromtext-wkt"},
    "SBSQL-FB46F964CAA5": {"function_id": "sb.scalar.st_union", "proof": "SBSFC038-st-union-signature"},
    "SBSQL-FF57FEDF9747": {"function_id": "sb.scalar.st_asmvtgeom", "proof": "SBSFC038-st-asmvtgeom-signature"},
}

SBSFC038_SPATIAL_TAIL_SCALAR_ORACLE_OVERRIDES = {
    surface_id: _sbsfc036_oracle_record(
        evidence["function_id"],
        [evidence["proof"], "SBSFC038-st-geomfromtext-invalid"],
    )
    for surface_id, evidence in SBSFC038_SPATIAL_TAIL_SCALAR_ROW_EVIDENCE.items()
}


def _sbsfc039_oracle_record(function_id: str, conformance_cases: list[str]) -> dict:
    name = function_id.rsplit(".", 1)[-1]
    return {
        "builtin_id": function_id,
        "canonical_name": function_id,
        "return_type_rule": (
            "XML document/query scalar helper returns bounded xml_document, xml fragment, "
            "or character descriptor according to function id"
        ),
        "overloads": [
            {
                "signature": function_id,
                "argument_rule": (
                    "bounded in-core XML document/query helper arguments: XML/text "
                    "fragments, document/content/sequence mode tokens, namespace or "
                    "attribute name/value pairs, path tokens, and target type metadata only"
                ),
            }
        ],
        "coercion_rule": (
            "accepts deterministic bounded textual XML fragments and scalar values; "
            "escapes text/attribute/namespace output and rejects unsafe names, malformed "
            "XML document text, arity, entity, DTD, or byte-limit violations"
        ),
        "null_behavior": "SQL null input propagates to SQL null for document/query/serialize helpers where applicable",
        "collation_charset_rule": "UTF-8 text semantics for XML descriptor output; no collation-sensitive comparison",
        "timezone_rule": "not applicable",
        "volatility": "immutable",
        "determinism": "deterministic",
        "side_effects": "none",
        "sblr_binding": f"sblr.expr.xml_{name}.v3",
        "ast_binding": f"ast.expr.xml_{name}",
        "engine_entrypoint": f"xml_{name}",
        "optimizer_properties": {
            "foldable": False,
            "index_eligible": False,
            "generated_column_eligible": True,
            "cost_class": "cpu_scalar",
        },
        "security_policy": (
            "pure bounded XML document/query scalar helper; deterministic in-core XML "
            "normalization, escaping, namespace/attribute descriptor, serialize, validate, "
            "and narrow tag-path query behavior only; no parser SQL execution, donor/plugin "
            "XML engine, storage/catalog lookup, transaction finality, WAL/recovery, or "
            "cluster behavior"
        ),
        "donor_rendering": "parser renders native SBsql XML document/query spelling and diagnostics through generated registry evidence",
        "error_semantics": (
            "invalid arity, unsafe XML name/content/entity, malformed XML document text, "
            "DTD/entity declaration, or bounded-size violation refuses with SBSQL.FUNCTION.INVALID_INPUT"
        ),
        "syntax_forms": ["function_call", "xml_special_form"],
        "conformance_cases": conformance_cases,
    }


SBSFC039_XML_DOCUMENT_QUERY_SCALAR_ROW_EVIDENCE = {
    "SBSQL-253585ABE51D": {"function_id": "sb.xml.document", "proof": "SBSFC039-xmldocument-bare"},
    "SBSQL-5753A90D2A1C": {"function_id": "sb.xml.document", "proof": "SBSFC039-xmldocument-expr"},
    "SBSQL-9D96355276FC": {"function_id": "sb.xml.namespaces", "proof": "SBSFC039-xmlnamespaces-bare"},
    "SBSQL-4F9AE84DDF5A": {"function_id": "sb.xml.namespaces", "proof": "SBSFC039-xmlnamespaces-decl"},
    "SBSQL-965B96256EB3": {"function_id": "sb.xml.parse", "proof": "SBSFC039-xmlparse-bare"},
    "SBSQL-F48761720168": {"function_id": "sb.xml.parse", "proof": "SBSFC039-xmlparse-doc"},
    "SBSQL-B9BD61883168": {"function_id": "sb.xml.query", "proof": "SBSFC039-xmlquery-bare"},
    "SBSQL-04FE00443530": {"function_id": "sb.xml.query", "proof": "SBSFC039-xmlquery-path"},
    "SBSQL-24C067DA97B0": {"function_id": "sb.xml.serialize", "proof": "SBSFC039-xmlserialize-bare"},
    "SBSQL-C9809EF23816": {"function_id": "sb.xml.serialize", "proof": "SBSFC039-xmlserialize-doc"},
    "SBSQL-82BBA556D880": {"function_id": "sb.xml.text", "proof": "SBSFC039-xmltext-bare"},
    "SBSQL-D53A57E7DD0B": {"function_id": "sb.xml.text", "proof": "SBSFC039-xmltext-text"},
    "SBSQL-666EAE033CFC": {"function_id": "sb.xml.validate", "proof": "SBSFC039-xmlvalidate-bare"},
    "SBSQL-B4880446510E": {"function_id": "sb.xml.validate", "proof": "SBSFC039-xmlvalidate-doc"},
    "SBSQL-663D565ADA02": {"function_id": "sb.xml.document", "proof": "SBSFC039-xml-alias"},
    "SBSQL-5F496C39F6E8": {"function_id": "sb.xml.attrs", "proof": "SBSFC039-xml-attrs"},
    "SBSQL-2ABE2825F6A1": {"function_id": "sb.xml.ns", "proof": "SBSFC039-xml-ns"},
}

SBSFC039_XML_DOCUMENT_QUERY_SCALAR_ORACLE_OVERRIDES = {
    surface_id: _sbsfc039_oracle_record(
        evidence["function_id"],
        [evidence["proof"], "SBSFC039-xmldocument-invalid", "SBSFC039-xml-attrs-invalid"],
    )
    for surface_id, evidence in SBSFC039_XML_DOCUMENT_QUERY_SCALAR_ROW_EVIDENCE.items()
}


def _sbsfc040_regr_oracle_record(function_id: str, conformance_case: str) -> dict:
    name = function_id.rsplit(".", 1)[-1]
    return {
        "builtin_id": function_id,
        "canonical_name": name,
        "return_type_rule": (
            "real64 regression aggregate over non-NULL numeric y/x input pairs; "
            "NULL when the regression denominator is undefined"
        ),
        "overloads": [
            {
                "signature": f"{name}(y,x)",
                "argument_rule": "two nullable numeric expressions y and x; rows with either NULL input are ignored",
            }
        ],
        "coercion_rule": "numeric descriptor coercion only for both y and x",
        "null_behavior": (
            "rows with NULL y or NULL x are ignored; empty/all-NULL groups return NULL real64 except "
            "sum-of-squares forms return zero for singleton valid-pair groups"
        ),
        "collation_charset_rule": "not applicable",
        "timezone_rule": "not applicable",
        "volatility": "immutable",
        "determinism": "deterministic for stable numeric input pairs",
        "side_effects": "none",
        "sblr_binding": f"sblr.expr.aggregate_{name}.v3",
        "ast_binding": f"ast.expr.aggregate_{name}",
        "engine_entrypoint": f"aggregate_{name}",
        "optimizer_properties": {
            "foldable": False,
            "index_eligible": False,
            "generated_column_eligible": False,
            "cost_class": "aggregate_state",
        },
        "security_policy": "none unless source expressions read session/security/system metadata",
        "donor_rendering": "parser renders donor spelling and diagnostics through donor alias registry",
        "error_semantics": (
            "missing y/x input pair refuses with SB_DIAG_AGGREGATE_CORR_PAIR_REQUIRED; "
            "non-numeric inputs refuse with SB_DIAG_AGGREGATE_NUMERIC_INPUT_REQUIRED"
        ),
        "syntax_forms": ["aggregate_function_call"],
        "conformance_cases": [conformance_case],
    }


SBSFC040_REGR_AGGREGATE_ROW_EVIDENCE = {
    "SBSQL-7102C019D2CF": {"function_id": "sb.aggregate.regr_avgx", "proof": "SBSFC040-regr-avgx-name"},
    "SBSQL-54324247868A": {"function_id": "sb.aggregate.regr_avgx", "proof": "SBSFC040-regr-avgx-y-x-signature"},
    "SBSQL-DF6313DE4B56": {"function_id": "sb.aggregate.regr_avgy", "proof": "SBSFC040-regr-avgy-name"},
    "SBSQL-189983EF2867": {"function_id": "sb.aggregate.regr_avgy", "proof": "SBSFC040-regr-avgy-y-x-signature"},
    "SBSQL-431925B5EC67": {"function_id": "sb.aggregate.regr_intercept", "proof": "SBSFC040-regr-intercept-name"},
    "SBSQL-8F9FD6E0E1B0": {"function_id": "sb.aggregate.regr_intercept", "proof": "SBSFC040-regr-intercept-y-x-signature"},
    "SBSQL-794AAFE26F38": {"function_id": "sb.aggregate.regr_r2", "proof": "SBSFC040-regr-r2-name"},
    "SBSQL-BE43021856AE": {"function_id": "sb.aggregate.regr_r2", "proof": "SBSFC040-regr-r2-y-x-signature"},
    "SBSQL-559DFA580089": {"function_id": "sb.aggregate.regr_slope", "proof": "SBSFC040-regr-slope-name"},
    "SBSQL-BB7BA14B2666": {"function_id": "sb.aggregate.regr_slope", "proof": "SBSFC040-regr-slope-y-x-signature"},
    "SBSQL-C77EA68C577B": {"function_id": "sb.aggregate.regr_sxx", "proof": "SBSFC040-regr-sxx-name"},
    "SBSQL-D291129F3FD3": {"function_id": "sb.aggregate.regr_sxx", "proof": "SBSFC040-regr-sxx-y-x-signature"},
    "SBSQL-61641209CF6B": {"function_id": "sb.aggregate.regr_sxy", "proof": "SBSFC040-regr-sxy-name"},
    "SBSQL-1F514A240E49": {"function_id": "sb.aggregate.regr_sxy", "proof": "SBSFC040-regr-sxy-y-x-signature"},
    "SBSQL-1D81FEFFF22A": {"function_id": "sb.aggregate.regr_syy", "proof": "SBSFC040-regr-syy-name"},
    "SBSQL-9C9BD835BEAF": {"function_id": "sb.aggregate.regr_syy", "proof": "SBSFC040-regr-syy-y-x-signature"},
}

SBSFC040_REGR_AGGREGATE_ORACLE_OVERRIDES = {
    surface_id: _sbsfc040_regr_oracle_record(evidence["function_id"], evidence["proof"])
    for surface_id, evidence in SBSFC040_REGR_AGGREGATE_ROW_EVIDENCE.items()
}


def fail(message: str) -> None:
    print(message, file=sys.stderr)
    raise SystemExit(1)


def read_csv(path: Path) -> list[dict[str, str]]:
    if not path.is_file():
        fail(f"required CSV missing: {path}")
    with path.open(newline="", encoding="utf-8") as handle:
        return list(csv.DictReader(handle))


def read_yaml(path: Path) -> dict:
    if not path.is_file():
        fail(f"required YAML missing: {path}")
    with path.open(encoding="utf-8") as handle:
        return yaml.safe_load(handle)


def normalize_name(canonical_name: str) -> str:
    name = canonical_name.strip().lower()
    paren = name.find("(")
    if paren >= 0:
        name = name[:paren]
    # Recognize canonical `sb.<package>.<name>` builtin id format and pull
    # the leaf name out (e.g., `sb.scalar.abs` -> `abs`). This makes the
    # SBsql surface registry rows whose canonical_name is the engine-anchored
    # builtin id match against the builtin-expression-registry.yaml entries
    # whose canonical_name is the leaf form.
    if name.startswith("sb.") and "." in name[3:]:
        leaf = name.rsplit(".", 1)[-1]
        if leaf:
            name = leaf
    # Remove non-identifier trailing chars (e.g., punctuation marks)
    stripped = re.sub(r"[^a-z0-9_]+$", "", name)
    # Replace internal punctuation that is not legal in identifiers
    stripped = re.sub(r"[^a-z0-9_]", "", stripped)
    # Fall back to original (pre-normalization) name when normalization
    # would otherwise yield empty (punctuation-only operators such as `@`).
    if not stripped:
        return canonical_name.strip().lower()
    return stripped


def normalize_signature(signature: str) -> str:
    return re.sub(r"\s+", "", signature.strip().lower())


def join_list(value) -> str:
    if value is None:
        return ""
    if isinstance(value, list):
        return ";".join(str(v) for v in value)
    return str(value)


def optimizer_field(record: dict, key: str) -> str:
    props = record.get("optimizer_properties") or {}
    if not isinstance(props, dict):
        return PENDING
    if key not in props:
        return PENDING
    return str(props[key])


def expected_diagnostics_default() -> str:
    return "SBSQL.BINDING.*;SBLR.ENVELOPE.*;SBLR.OPCODE.*;SECURITY.*;CATALOG.NAME.*"


def classify(
    surface: dict[str, str],
    expr_by_surface_id: dict[str, dict],
    expr_by_signature: dict[str, dict],
    expr_by_name: dict[str, list[dict]],
    special_by_id: dict[str, dict],
    window_by_surface_id: dict[str, dict],
    binding_by_id: dict[str, dict],
) -> dict[str, str]:
    canonical_name = surface["canonical_name"]
    norm = normalize_name(canonical_name)

    override = SBSFC032_SCALAR_UTILITY_ORACLE_OVERRIDES.get(surface["surface_id"])
    if isinstance(override, str):
        override = SBSFC032_SCALAR_UTILITY_ORACLE_OVERRIDES[override]
    if override is not None:
        return _full_oracle_row(
            surface,
            norm,
            override,
            "SBSFC-032 scalar utility/conversion row-evidence override",
        )

    override = SBSFC033_CATALOG_DESCRIPTOR_DIAGNOSTIC_ORACLE_OVERRIDES.get(surface["surface_id"])
    if override is not None:
        return _full_oracle_row(
            surface,
            norm,
            override,
            "SBSFC-033 catalog/descriptor/diagnostic row-evidence override",
        )

    override = SBSFC044_CATALOG_STATISTICS_ORACLE_OVERRIDES.get(surface["surface_id"])
    if override is not None:
        return _full_oracle_row(
            surface,
            norm,
            override,
            "SBSFC-044 catalog statistics row-evidence override",
        )

    override = SBSFC046_SESSION_ADMIN_ORACLE_OVERRIDES.get(surface["surface_id"])
    if override is not None:
        return _full_oracle_row(
            surface,
            norm,
            override,
            "SBSFC-046 session-admin row-evidence override",
        )

    override = SBSFC047_ADVISORY_LOCK_ORACLE_OVERRIDES.get(surface["surface_id"])
    if override is not None:
        return _full_oracle_row(
            surface,
            norm,
            override,
            "SBSFC-047 advisory-lock row-evidence override",
        )

    override = SBSFC048_ADVISORY_LOCK_RELEASE_ORACLE_OVERRIDES.get(surface["surface_id"])
    if override is not None:
        return _full_oracle_row(
            surface,
            norm,
            override,
            "SBSFC-048 advisory-lock release and xact-lock row-evidence override",
        )

    override = SBSQL_MISS_GATE_011_LOCK_ORACLE_OVERRIDES.get(surface["surface_id"])
    if override is not None:
        return _full_oracle_row(
            surface,
            norm,
            override,
            "SBSQL-MISS-GATE-011 named-lock scalar row-evidence override",
        )

    override = SBSFC049_UNICODE_TEXT_ORACLE_OVERRIDES.get(surface["surface_id"])
    if override is not None:
        return _full_oracle_row(
            surface,
            norm,
            override,
            "SBSFC-049 Unicode text row-evidence override",
        )

    override = SBSFC052_JSON_DOCUMENT_ORACLE_OVERRIDES.get(surface["surface_id"])
    if override is not None:
        return _full_oracle_row(
            surface,
            norm,
            override,
            "SBSFC-052 JSON/document construction-table-aggregate row-evidence override",
        )

    override = SBSFC053_ROWSET_TABLE_VALUE_ORACLE_OVERRIDES.get(surface["surface_id"])
    if override is not None:
        return _full_oracle_row(
            surface,
            norm,
            override,
            "SBSFC-053 rowset/table-value/set-returning row-evidence override",
        )

    override = SBSFC054_CURSOR_STREAM_HANDLE_ORACLE_OVERRIDES.get(surface["surface_id"])
    if override is not None:
        return _full_oracle_row(
            surface,
            norm,
            override,
            "SBSFC-054 cursor/stream/handle row-evidence override",
        )

    override = SBSFC055_LOB_LOCATOR_ORACLE_OVERRIDES.get(surface["surface_id"])
    if override is not None:
        return _full_oracle_row(
            surface,
            norm,
            override,
            "SBSFC-055 LOB/locator row-evidence override",
        )

    override = SBSFC056_NATIVE_SURFACE_ORACLE_OVERRIDES.get(surface["surface_id"])
    if override is not None:
        return _full_oracle_row(
            surface,
            norm,
            override,
            "SBSFC-056 native surface scalar/descriptor row-evidence override",
        )

    override = SBSFC057_CRYPTO_HASH_ORACLE_OVERRIDES.get(surface["surface_id"])
    if override is not None:
        return _full_oracle_row(
            surface,
            norm,
            override,
            "SBSFC-057 crypto/hash and pgcrypto row-evidence override",
        )

    override = SBSFC058_EXPRESSION_RUNTIME_ORACLE_OVERRIDES.get(surface["surface_id"])
    if override is not None:
        return _full_oracle_row(
            surface,
            norm,
            override,
            "SBSFC-058 expression runtime function row-evidence override",
        )

    override = SBSFC059_EXPRESSION_RUNTIME_CLEANUP_ORACLE_OVERRIDES.get(surface["surface_id"])
    if override is not None:
        return _full_oracle_row(
            surface,
            norm,
            override,
            "SBSFC-059 expression runtime function cleanup row-evidence override",
        )

    override = SBSFC034_TEXT_TRIGRAM_BIT_STRING_ORACLE_OVERRIDES.get(surface["surface_id"])
    if isinstance(override, str):
        override = SBSFC034_TEXT_TRIGRAM_BIT_STRING_ORACLE_OVERRIDES[override]
    if override is not None:
        return _full_oracle_row(
            surface,
            norm,
            override,
            "SBSFC-034 text/trigram/bit-string row-evidence override",
        )

    override = SBSFC035_RANGE_SCALAR_HELPER_ORACLE_OVERRIDES.get(surface["surface_id"])
    if override is not None:
        return _full_oracle_row(
            surface,
            norm,
            override,
            "SBSFC-035 range scalar helper row-evidence override",
        )

    override = SBSFC036_SPATIAL_GEOMETRY_SCALAR_ORACLE_OVERRIDES.get(surface["surface_id"])
    if override is not None:
        return _full_oracle_row(
            surface,
            norm,
            override,
            "SBSFC-036 spatial geometry scalar helper row-evidence override",
        )

    override = SBSFC037_XML_MULTIMODEL_SCALAR_ORACLE_OVERRIDES.get(surface["surface_id"])
    if override is not None:
        return _full_oracle_row(
            surface,
            norm,
            override,
            "SBSFC-037 XML/multimodel scalar helper row-evidence override",
        )

    override = SBSFC038_SPATIAL_TAIL_SCALAR_ORACLE_OVERRIDES.get(surface["surface_id"])
    if override is not None:
        return _full_oracle_row(
            surface,
            norm,
            override,
            "SBSFC-038 spatial tail scalar helper row-evidence override",
        )

    override = SBSFC039_XML_DOCUMENT_QUERY_SCALAR_ORACLE_OVERRIDES.get(surface["surface_id"])
    if override is not None:
        return _full_oracle_row(
            surface,
            norm,
            override,
            "SBSFC-039 XML document/query scalar helper row-evidence override",
        )

    override = SBSFC040_REGR_AGGREGATE_ORACLE_OVERRIDES.get(surface["surface_id"])
    if override is not None:
        return _full_oracle_row(
            surface,
            norm,
            override,
            "SBSFC-040 statistical regression aggregate row-evidence override",
        )

    window = window_by_surface_id.get(surface["surface_id"])
    if window is not None:
        return _full_oracle_row(
            surface,
            norm,
            window,
            "builtin-window-registry.yaml via explicit surface_ids",
        )

    expr = expr_by_surface_id.get(surface["surface_id"])
    if expr is not None:
        return _full_oracle_row(
            surface,
            norm,
            expr,
            "builtin-expression-registry.yaml via explicit surface_ids",
        )

    surface_signature = normalize_signature(canonical_name)
    expr = expr_by_signature.get(surface_signature)
    if expr is not None:
        return _full_oracle_row(
            surface,
            norm,
            expr,
            "builtin-expression-registry.yaml via overload signature",
        )

    expr_matches = expr_by_name.get(norm, [])
    if len(expr_matches) == 1:
        expr = expr_matches[0]
        return _full_oracle_row(
            surface,
            norm,
            expr,
            "builtin-expression-registry.yaml after unambiguous normalize-and-strip-parens",
        )
    if len(expr_matches) > 1:
        return _ambiguous_name_row(surface, norm, [r.get("builtin_id", "") for r in expr_matches])

    special = special_by_id.get(canonical_name)
    if special is not None:
        return _full_oracle_row(
            surface,
            norm,
            special,
            "builtin-special-form-registry.yaml via builtin_id",
        )

    matched_binding = None
    matched_builtin_id = ""
    for prefix in BINDING_PACKAGES:
        probe = f"{prefix}{norm}"
        if probe in binding_by_id:
            matched_binding = binding_by_id[probe]
            matched_builtin_id = probe
            break
    if matched_binding is not None:
        return _binding_only_row(surface, norm, matched_builtin_id, matched_binding)

    return _pending_row(surface, norm)


def _full_oracle_row(
    surface: dict[str, str],
    norm: str,
    expr: dict,
    authority_source: str,
) -> dict[str, str]:
    overload = (expr.get("overloads") or [{}])[0] if expr.get("overloads") else {}
    return {
        "normalized_name": norm,
        "oracle_authority_status": "full_oracle",
        "matched_builtin_id": expr.get("builtin_id", ""),
        "matched_canonical_name": expr.get("canonical_name", ""),
        "argument_descriptor_rule": str(overload.get("argument_rule", PENDING)),
        "return_type_rule": str(expr.get("return_type_rule", PENDING)),
        "coercion_rule": str(expr.get("coercion_rule", PENDING)),
        "null_behavior": str(expr.get("null_behavior", PENDING)),
        "collation_charset_rule": str(expr.get("collation_charset_rule", PENDING)),
        "timezone_rule": str(expr.get("timezone_rule", PENDING)),
        "volatility": str(expr.get("volatility", PENDING)),
        "determinism": str(expr.get("determinism", PENDING)),
        "side_effects": str(expr.get("side_effects", PENDING)),
        "foldable": optimizer_field(expr, "foldable"),
        "index_eligible": optimizer_field(expr, "index_eligible"),
        "generated_column_eligible": optimizer_field(expr, "generated_column_eligible"),
        "cost_class": optimizer_field(expr, "cost_class"),
        "security_policy": str(expr.get("security_policy", PENDING)),
        "error_semantics": str(expr.get("error_semantics", PENDING)),
        "expected_diagnostics": expected_diagnostics_default(),
        "sblr_binding": str(expr.get("sblr_binding", PENDING)),
        "ast_binding": str(expr.get("ast_binding", PENDING)),
        "engine_entrypoint": str(expr.get("engine_entrypoint", PENDING)),
        "donor_rendering": str(expr.get("donor_rendering", PENDING)),
        "syntax_forms": join_list(expr.get("syntax_forms")),
        "conformance_cases": join_list(expr.get("conformance_cases")),
        "notes": f"oracle sourced from {authority_source}.",
    }


def _binding_only_row(
    surface: dict[str, str],
    norm: str,
    builtin_id: str,
    binding: dict,
) -> dict[str, str]:
    return {
        "normalized_name": norm,
        "oracle_authority_status": "binding_only",
        "matched_builtin_id": builtin_id,
        "matched_canonical_name": "",
        "argument_descriptor_rule": PENDING,
        "return_type_rule": PENDING,
        "coercion_rule": PENDING,
        "null_behavior": PENDING,
        "collation_charset_rule": PENDING,
        "timezone_rule": PENDING,
        "volatility": PENDING,
        "determinism": PENDING,
        "side_effects": PENDING,
        "foldable": PENDING,
        "index_eligible": PENDING,
        "generated_column_eligible": PENDING,
        "cost_class": PENDING,
        "security_policy": PENDING,
        "error_semantics": PENDING,
        "expected_diagnostics": expected_diagnostics_default(),
        "sblr_binding": str(binding.get("sblr_binding", PENDING)),
        "ast_binding": PENDING,
        "engine_entrypoint": str(binding.get("engine_entrypoint", PENDING)),
        "donor_rendering": PENDING,
        "syntax_forms": "",
        "conformance_cases": "",
        "notes": "sblr_binding and engine_entrypoint sourced from builtin-sblr-expression-binding.yaml; semantic oracle fields pending canonical builtin-expression-registry entry.",
    }


def _pending_row(surface: dict[str, str], norm: str) -> dict[str, str]:
    return {
        "normalized_name": norm,
        "oracle_authority_status": "pending_canonical_authority_entry",
        "matched_builtin_id": "",
        "matched_canonical_name": "",
        "argument_descriptor_rule": PENDING,
        "return_type_rule": PENDING,
        "coercion_rule": PENDING,
        "null_behavior": PENDING,
        "collation_charset_rule": PENDING,
        "timezone_rule": PENDING,
        "volatility": PENDING,
        "determinism": PENDING,
        "side_effects": PENDING,
        "foldable": PENDING,
        "index_eligible": PENDING,
        "generated_column_eligible": PENDING,
        "cost_class": PENDING,
        "security_policy": PENDING,
        "error_semantics": PENDING,
        "expected_diagnostics": expected_diagnostics_default(),
        "sblr_binding": PENDING,
        "ast_binding": PENDING,
        "engine_entrypoint": PENDING,
        "donor_rendering": PENDING,
        "syntax_forms": "",
        "conformance_cases": "",
        "notes": "no canonical builtin-expression-registry or builtin-sblr-expression-binding entry matched; row blocked from P2 implementation until canonical authority records the oracle. Coordinator must add canonical entries (or route the surface to remove_by_spec_change in NATIVE_FUTURE_PROMOTION_MATRIX) before any P2 slice may implement.",
    }


def _ambiguous_name_row(surface: dict[str, str], norm: str, builtin_ids: list[str]) -> dict[str, str]:
    row = _pending_row(surface, norm)
    row["oracle_authority_status"] = "ambiguous_name_match_requires_surface_or_overload_authority"
    row["notes"] = (
        "normalized-name oracle match is ambiguous across builtin ids "
        f"{';'.join(sorted(b for b in builtin_ids if b))}; add explicit surface_ids "
        "or overload signature authority before implementation."
    )
    return row


def main() -> int:
    parser = argparse.ArgumentParser()
    parser.add_argument("--repo-root", required=True)
    parser.add_argument("--artifact-root", default=DEFAULT_ARTIFACT_ROOT)
    args = parser.parse_args()
    root = Path(args.repo_root)
    artifact_root = Path(args.artifact_root)
    if not artifact_root.is_absolute():
        artifact_root = root / artifact_root

    surfaces = read_csv(root / REGISTRY_CSV)
    expr_data = read_yaml(root / EXPR_REGISTRY)
    special_data = read_yaml(root / SPECIAL_FORM_REGISTRY)
    binding_data = read_yaml(root / BINDING_REGISTRY)
    window_data = read_yaml(root / WINDOW_REGISTRY)

    expr_by_surface_id: dict[str, dict] = {}
    expr_by_signature: dict[str, dict] = {}
    expr_by_name: dict[str, list[dict]] = {}
    for record in expr_data.get("records", []):
        for surface_id in record.get("surface_ids", []) or []:
            if surface_id in expr_by_surface_id:
                fail(
                    "duplicate builtin-expression-registry surface_id authority: "
                    f"{surface_id}"
                )
            expr_by_surface_id[surface_id] = record
        for overload in record.get("overloads", []) or []:
            signature = overload.get("signature", "")
            if signature:
                normalized_signature = normalize_signature(signature)
                if normalized_signature in expr_by_signature:
                    fail(
                        "duplicate builtin-expression-registry overload signature authority: "
                        f"{signature}"
                    )
                expr_by_signature[normalized_signature] = record
        name = record.get("canonical_name", "")
        if name:
            expr_by_name.setdefault(normalize_name(name), []).append(record)

    binding_by_id: dict[str, dict] = {}
    for record in binding_data.get("records", []):
        bid = record.get("builtin_id", "")
        if bid:
            binding_by_id[bid] = record

    special_by_id: dict[str, dict] = {}
    for record in special_data.get("records", []):
        bid = record.get("builtin_id", "")
        if bid:
            special_by_id[bid] = record

    window_by_surface_id: dict[str, dict] = {}
    for record in window_data.get("records", []):
        for surface_id in record.get("surface_ids", []) or []:
            if surface_id in window_by_surface_id:
                fail(
                    "duplicate builtin-window-registry surface_id authority: "
                    f"{surface_id}"
                )
            window_by_surface_id[surface_id] = record

    expression_runtime_surfaces = [
        s for s in surfaces if s["surface_kind"] in EXPRESSION_RUNTIME_KINDS
    ]
    if not expression_runtime_surfaces:
        fail("no function/operator/variable surfaces found in SBSQL_SURFACE_REGISTRY.csv")

    output_rows: list[dict[str, str]] = []
    authority_counts: dict[str, int] = {}
    kind_counts: dict[str, int] = {}

    for surface in sorted(expression_runtime_surfaces, key=lambda r: r["surface_id"]):
        classification = classify(
            surface,
            expr_by_surface_id,
            expr_by_signature,
            expr_by_name,
            special_by_id,
            window_by_surface_id,
            binding_by_id,
        )
        ledger_row = {
            "surface_id": surface["surface_id"],
            "canonical_name": surface["canonical_name"],
            "surface_kind": surface["surface_kind"],
            "status": surface["status"],
            "cluster_scope": surface["cluster_scope"],
            "sblr_operation_family": surface["sblr_operation_family"],
        }
        ledger_row.update(classification)
        output_rows.append(ledger_row)

        status_key = classification["oracle_authority_status"]
        authority_counts[status_key] = authority_counts.get(status_key, 0) + 1
        kind_counts[surface["surface_kind"]] = kind_counts.get(surface["surface_kind"], 0) + 1

    output_path = artifact_root / OUTPUT_NAME
    output_path.parent.mkdir(parents=True, exist_ok=True)
    with output_path.open("w", newline="", encoding="utf-8") as handle:
        writer = csv.DictWriter(handle, fieldnames=COLUMNS, lineterminator="\n")
        writer.writeheader()
        writer.writerows(output_rows)

    print(
        "function_semantic_oracle_matrix=generated "
        f"rows={len(output_rows)} "
        + " ".join(f"{k}={v}" for k, v in sorted(kind_counts.items()))
        + " "
        + " ".join(f"{k}={v}" for k, v in sorted(authority_counts.items()))
    )
    return 0


if __name__ == "__main__":
    raise SystemExit(main())
