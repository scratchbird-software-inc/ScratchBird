#!/usr/bin/env python3
# Copyright (c) 2026 ScratchBird Software Inc.
#
# This Source Code Form is subject to the terms of the Mozilla Public
# License, v. 2.0. If a copy of the MPL was not distributed with this
# file, You can obtain one at https://mozilla.org/MPL/2.0/.
#
# SPDX-License-Identifier: MPL-2.0

"""Generate the public SBsql language resource pack.

The generated pack is product resource data. Lexical resources are derived from
ScratchBird-owned SBsql registry artifacts and the fixed system-object baseline
in this generator. Clause and phrase topology metadata includes deterministic
summaries derived from Universal Dependencies PUD treebank structure. Raw
treebank data is not vendored into the public pack. The generator never reads
draft documentation and never uses network translation services.
"""

from __future__ import annotations

import argparse
import csv
import hashlib
import importlib.util
import json
import re
import shutil
import sys
from dataclasses import dataclass
from pathlib import Path
from typing import Any


DEFAULT_SEED_PACK = "project/resources/seed-packs/initial-resource-pack"
DEFAULT_PACK_REL = "resources/i18n/sbsql-language-resource-pack"
REGISTRY_HPP = "project/src/parsers/sbsql_worker/registry/generated/sbsql_generated_registry.hpp"
REGISTRY_CPP = "project/src/parsers/sbsql_worker/registry/generated/sbsql_generated_registry.cpp"
REGISTRY_MANIFEST = "project/src/parsers/sbsql_worker/registry/generated/sbsql_generated_registry.manifest"
RELEASE_DECLARATION = "project/tests/sbsql_parser_worker/fixtures/surface_to_sblr/artifacts/SBSQL_SURFACE_RELEASE_DECLARATION.csv"
STRICT_LEDGER = "project/tests/sbsql_parser_worker/fixtures/surface_to_sblr/artifacts/STRICT_ROW_COVERAGE_LEDGER.csv"
DRIVER_SURFACE_MANIFEST = "project/drivers/language/sbsql_language_surface_manifest.json"
PUBLIC_DIAGNOSTIC_MATRIX_GENERATOR = "project/tools/release/public_diagnostic_matrix_generator.py"
SBLR_ENVELOPE_HPP = "project/include/scratchbird/engine/sblr_envelope.hpp"

PACK_SCHEMA_VERSION = "sbsql.language_resource_pack.v1"
MANIFEST_SCHEMA_VERSION = "sbsql.language_resource_pack_manifest.v1"
SIGNATURE_SCHEMA_VERSION = "sbsql.language_resource_pack_signature.v1"
RESOURCE_IDENTITY = "sbsql.common_resource_pack.v1"
DIALECT_PROFILE_UUID = "sbsql.v3"
TOPOLOGY_PROFILE_UUID = "topology.sbsql.canonical.v1"
GENERATOR_ID = "project/tools/sb_parser_gen/generate_sbsql_language_resource_pack.py"
PUBLIC_SIGNING_KEY_ID = "scratchbird.public-source-review.language-resource.sha256-transcript.v1"
I18N_VERSION = "2026-06-12"

REGISTRY_FIELDS = [
    "surface_id",
    "fixed_uuid_v7",
    "canonical_name",
    "surface_kind",
    "family",
    "source_status",
    "cluster_scope",
    "canonical_spec",
    "sblr_operation_family",
    "parser_packet",
    "engine_packet",
    "owner_lane",
    "batch_id",
    "ctest_label",
    "parser_handler_key",
    "udr_handler_key",
    "lowering_handler_key",
    "server_admission_key",
    "engine_rule_key",
    "diagnostic_key",
    "oracle_key",
    "validation_fixture_id",
    "final_acceptance_rule",
    "closure_action",
]

LANGUAGE_PROFILES = [
    {
        "exact_tag": "en-US",
        "profile_uuid": "sbsql.language.en-US.canonical-recovery.v1",
        "display_name": "English (United States)",
        "release_channel": "release_supported",
        "support_state": "release_supported",
        "translation_source": "canonical_scratchbird_english_source",
        "native_review_state": "source_authority_reviewed",
        "fallback_parent_uuid": "",
    },
    {
        "exact_tag": "en-CA",
        "profile_uuid": "sbsql.language.en-CA.canonical-recovery.v1",
        "display_name": "English (Canada)",
        "release_channel": "release_supported",
        "support_state": "release_supported",
        "translation_source": "canonical_scratchbird_english_source_canadian_profile",
        "native_review_state": "source_authority_reviewed",
        "fallback_parent_uuid": "sbsql.language.en-US.canonical-recovery.v1",
    },
    {
        "exact_tag": "fr-FR",
        "profile_uuid": "sbsql.language.fr-FR.machine-beta.v1",
        "display_name": "French (France)",
        "release_channel": "beta",
        "support_state": "fully_populated_native_review_required",
        "translation_source": "scratchbird_deterministic_glossary_with_reference_ui_term_anchor_review",
        "native_review_state": "native_technical_review_required_before_release_support",
        "fallback_parent_uuid": "sbsql.language.en-US.canonical-recovery.v1",
    },
    {
        "exact_tag": "fr-CA",
        "profile_uuid": "sbsql.language.fr-CA.machine-beta.v1",
        "display_name": "French (Canada)",
        "release_channel": "beta",
        "support_state": "fully_populated_native_review_required",
        "translation_source": "scratchbird_deterministic_glossary_with_reference_ui_term_anchor_review",
        "native_review_state": "native_technical_review_required_before_release_support",
        "fallback_parent_uuid": "sbsql.language.fr-FR.machine-beta.v1",
    },
    {
        "exact_tag": "de-DE",
        "profile_uuid": "sbsql.language.de-DE.machine-beta.v1",
        "display_name": "German (Germany)",
        "release_channel": "beta",
        "support_state": "fully_populated_native_review_required",
        "translation_source": "scratchbird_deterministic_glossary_with_reference_ui_term_anchor_review",
        "native_review_state": "native_technical_review_required_before_release_support",
        "fallback_parent_uuid": "sbsql.language.en-US.canonical-recovery.v1",
    },
    {
        "exact_tag": "it-IT",
        "profile_uuid": "sbsql.language.it-IT.machine-beta.v1",
        "display_name": "Italian (Italy)",
        "release_channel": "beta",
        "support_state": "fully_populated_native_review_required",
        "translation_source": "scratchbird_deterministic_glossary_with_reference_ui_term_anchor_review",
        "native_review_state": "native_technical_review_required_before_release_support",
        "fallback_parent_uuid": "sbsql.language.en-US.canonical-recovery.v1",
    },
    {
        "exact_tag": "es-ES",
        "profile_uuid": "sbsql.language.es-ES.machine-beta.v1",
        "display_name": "Spanish (Spain)",
        "release_channel": "beta",
        "support_state": "fully_populated_native_review_required",
        "translation_source": "scratchbird_deterministic_glossary_with_reference_ui_term_anchor_review",
        "native_review_state": "native_technical_review_required_before_release_support",
        "fallback_parent_uuid": "sbsql.language.en-US.canonical-recovery.v1",
    },
]

SOURCE_AUTHORITY_REVIEWED_PROFILES = {
    profile["exact_tag"]
    for profile in LANGUAGE_PROFILES
    if profile["native_review_state"] == "source_authority_reviewed"
}

UD_PUD_TOPOLOGY_SOURCES = {
    "en": {
        "treebank": "UD_English-PUD",
        "treebank_file": "en_pud-ud-test.conllu",
        "license": "CC BY-SA 3.0",
        "sentences": 1000,
        "tokens": 21180,
        "source_url": "https://github.com/UniversalDependencies/UD_English-PUD",
    },
    "fr": {
        "treebank": "UD_French-PUD",
        "treebank_file": "fr_pud-ud-test.conllu",
        "license": "CC BY-SA 3.0",
        "sentences": 1000,
        "tokens": 24726,
        "source_url": "https://github.com/UniversalDependencies/UD_French-PUD",
    },
    "de": {
        "treebank": "UD_German-PUD",
        "treebank_file": "de_pud-ud-test.conllu",
        "license": "CC BY-SA 3.0",
        "sentences": 1000,
        "tokens": 21332,
        "source_url": "https://github.com/UniversalDependencies/UD_German-PUD",
    },
    "it": {
        "treebank": "UD_Italian-PUD",
        "treebank_file": "it_pud-ud-test.conllu",
        "license": "CC BY-SA 3.0",
        "sentences": 1000,
        "tokens": 23732,
        "source_url": "https://github.com/UniversalDependencies/UD_Italian-PUD",
    },
    "es": {
        "treebank": "UD_Spanish-PUD",
        "treebank_file": "es_pud-ud-test.conllu",
        "license": "CC BY-SA 3.0",
        "sentences": 1000,
        "tokens": 23283,
        "source_url": "https://github.com/UniversalDependencies/UD_Spanish-PUD",
    },
}

UD_TOPOLOGY_METRICS = {
    "en": {
        "dominant_predicate_argument_order": "subject_predicate_object",
        "adposition_order": "prepositions_before_nominal_head",
        "nominal_adjective_order": "adjective_before_noun",
        "determiner_order": "determiner_before_noun",
        "dependency_direction_ratios": {
            "amod_before_head": 0.9897,
            "case_before_head": 0.9562,
            "det_before_head": 1.0,
            "nsubj_before_predicate": 0.9669,
            "obj_before_predicate": 0.0228,
            "obl_before_predicate": 0.1944,
        },
    },
    "fr": {
        "dominant_predicate_argument_order": "subject_predicate_object",
        "adposition_order": "prepositions_before_nominal_head",
        "nominal_adjective_order": "mixed_postnominal_preferred",
        "determiner_order": "determiner_before_noun",
        "dependency_direction_ratios": {
            "amod_before_head": 0.3061,
            "case_before_head": 1.0,
            "det_before_head": 1.0,
            "nsubj_before_predicate": 0.9605,
            "obj_before_predicate": 0.1187,
            "obl_before_predicate": 0.1919,
        },
    },
    "de": {
        "dominant_predicate_argument_order": "verb_second_mixed_object_order",
        "adposition_order": "prepositions_before_nominal_head",
        "nominal_adjective_order": "adjective_before_noun",
        "determiner_order": "determiner_before_noun",
        "dependency_direction_ratios": {
            "amod_before_head": 0.9973,
            "case_before_head": 0.9951,
            "det_before_head": 0.999,
            "nsubj_before_predicate": 0.8378,
            "obj_before_predicate": 0.5857,
            "obl_before_predicate": 0.6561,
        },
    },
    "it": {
        "dominant_predicate_argument_order": "subject_predicate_object",
        "adposition_order": "prepositions_before_nominal_head",
        "nominal_adjective_order": "mixed_postnominal_preferred",
        "determiner_order": "determiner_before_noun",
        "dependency_direction_ratios": {
            "amod_before_head": 0.3104,
            "case_before_head": 0.9991,
            "det_before_head": 1.0,
            "nsubj_before_predicate": 0.8902,
            "obj_before_predicate": 0.0636,
            "obl_before_predicate": 0.2066,
        },
    },
    "es": {
        "dominant_predicate_argument_order": "subject_predicate_object",
        "adposition_order": "prepositions_before_nominal_head",
        "nominal_adjective_order": "mixed_postnominal_preferred",
        "determiner_order": "determiner_before_noun",
        "dependency_direction_ratios": {
            "amod_before_head": 0.3043,
            "case_before_head": 0.9995,
            "det_before_head": 1.0,
            "nsubj_before_predicate": 0.8827,
            "obj_before_predicate": 0.092,
            "obl_before_predicate": 0.1879,
        },
    },
}

PROFILE_TO_UD_LANGUAGE = {
    "en-US": "en",
    "en-CA": "en",
    "fr-FR": "fr",
    "fr-CA": "fr",
    "de-DE": "de",
    "it-IT": "it",
    "es-ES": "es",
}

POSTNOMINAL_MODIFIER_PROFILES = {"fr-FR", "fr-CA", "it-IT", "es-ES"}

NOMINAL_HEAD_TOKENS = {
    "action", "address", "agent", "alias", "argument", "array", "artifact", "attribute",
    "audit", "authorization", "backup", "binary", "block", "body", "branch", "bridge",
    "buffer", "capability", "cardinality", "case", "catalog", "character", "checkpoint",
    "class", "clause", "client", "cluster", "collation", "column", "comment", "compression",
    "condition", "connection", "constraint", "constructor", "context", "copy", "cursor",
    "data", "database", "date", "descriptor", "diagnostic", "dialect", "domain", "edge",
    "element", "engine", "event", "exception", "expression", "extension", "field",
    "filespace", "filter", "flag", "form", "function", "generator", "geometry", "graph",
    "group", "handle", "identifier", "index", "instruction", "interval", "item", "job",
    "key", "keyword", "label", "language", "length", "level", "list", "literal", "locator",
    "lock", "log", "management", "map", "mask", "message", "method", "metric", "mode",
    "modifier", "name", "namespace", "node", "object", "operation", "operator", "option",
    "order", "package", "parser", "partition", "path", "pattern", "pipeline", "point",
    "policy", "position", "privilege", "procedure", "processor", "profile", "projection",
    "proxy", "query", "range", "record", "reference", "registry", "resource", "result",
    "role", "routine", "route", "row", "rowset", "schema", "search", "security", "sequence",
    "server", "session", "setting", "shape", "snapshot", "source", "statement", "stream",
    "string", "subtype", "table", "target", "tenant", "text", "time", "timestamp",
    "token", "transaction", "trigger", "type", "user", "value", "variable", "vector",
    "version", "view", "window", "wrapper", "zone",
}

NON_NOMINAL_PREHEAD_TOKENS = {
    "abs", "add", "alter", "and", "append", "begin", "call", "cancel", "cast", "close",
    "coalesce", "collect", "commit", "contains", "copy", "create", "declare", "delete",
    "disconnect", "distinct", "drop", "execute", "exists", "explain", "extract", "fetch",
    "for", "from", "generate", "get", "grant", "has", "in", "insert", "into", "is", "join",
    "like", "make", "merge", "move", "not", "on", "open", "or", "over", "raise", "read",
    "replace", "restore", "revoke", "rollback", "search", "select", "set", "show", "split",
    "start", "to", "transform", "truncate", "union", "unnest", "unpivot", "update", "use",
    "when", "where", "with", "write",
}

STRUCTURAL_PHRASE_TEMPLATES = [
    {
        "template_id": "sbsql.query.select.basic",
        "logical_form": "select_query",
        "canonical_slots": ["action", "projection", "source", "condition", "modifier"],
        "slot_required": ["action", "projection"],
        "slot_order_by_profile": {
            "en-US": ["action", "projection", "source", "condition", "modifier"],
            "en-CA": ["action", "projection", "source", "condition", "modifier"],
            "fr-FR": ["action", "projection", "source", "condition", "modifier"],
            "fr-CA": ["action", "projection", "source", "condition", "modifier"],
            "de-DE": ["action", "projection", "source", "condition", "modifier"],
            "it-IT": ["action", "projection", "source", "condition", "modifier"],
            "es-ES": ["action", "projection", "source", "condition", "modifier"],
        },
        "slot_markers_by_profile": {
            "en-US": {"action": "select", "source": "from", "condition": "where"},
            "en-CA": {"action": "select", "source": "from", "condition": "where"},
            "fr-FR": {"action": "sélectionner", "source": "depuis", "condition": "où"},
            "fr-CA": {"action": "sélectionner", "source": "depuis", "condition": "où"},
            "de-DE": {"action": "auswählen", "source": "von", "condition": "wo"},
            "it-IT": {"action": "seleziona", "source": "da", "condition": "dove"},
            "es-ES": {"action": "seleccionar", "source": "desde", "condition": "donde"},
        },
    },
    {
        "template_id": "sbsql.command.transaction.basic",
        "logical_form": "transaction_control",
        "canonical_slots": ["action", "transaction_target", "modifier"],
        "slot_required": ["action"],
        "slot_order_by_profile": {
            "en-US": ["action", "transaction_target", "modifier"],
            "en-CA": ["action", "transaction_target", "modifier"],
            "fr-FR": ["action", "transaction_target", "modifier"],
            "fr-CA": ["action", "transaction_target", "modifier"],
            "de-DE": ["action", "transaction_target", "modifier"],
            "it-IT": ["action", "transaction_target", "modifier"],
            "es-ES": ["action", "transaction_target", "modifier"],
        },
        "slot_markers_by_profile": {
            "en-US": {"transaction_target": "transaction"},
            "en-CA": {"transaction_target": "transaction"},
            "fr-FR": {"transaction_target": "transaction"},
            "fr-CA": {"transaction_target": "transaction"},
            "de-DE": {"transaction_target": "Transaktion"},
            "it-IT": {"transaction_target": "transazione"},
            "es-ES": {"transaction_target": "transacción"},
        },
    },
    {
        "template_id": "sbsql.object.nominal.headed",
        "logical_form": "catalog_object_name",
        "canonical_slots": ["modifier", "nominal_head"],
        "slot_required": ["nominal_head"],
        "slot_order_by_profile": {
            "en-US": ["modifier", "nominal_head"],
            "en-CA": ["modifier", "nominal_head"],
            "fr-FR": ["nominal_head", "modifier"],
            "fr-CA": ["nominal_head", "modifier"],
            "de-DE": ["modifier", "nominal_head"],
            "it-IT": ["nominal_head", "modifier"],
            "es-ES": ["nominal_head", "modifier"],
        },
        "slot_markers_by_profile": {},
    },
]

SYSTEM_OBJECT_SOURCES = [
    ("database", "database", "storage_object", "database", "databases", "sys.database", "catalog_root", "Database file and database identity visible to SQL users."),
    ("filespace", "filespace", "storage_object", "filespace", "filespaces", "sys.storage.filespace", "storage_path", "Named storage allocation domain inside a database."),
    ("schema", "schema", "catalog_object", "schema", "schemas", "sys.catalog.schema", "schema_namespace", "General SQL schema namespace."),
    ("user_schema", "schema", "catalog_object", "user schema", "user schemas", "sys.catalog.user_schema", "schema_namespace", "Schema owned by or associated with a user. Keep distinct from the security identity named user."),
    ("system_schema", "schema", "catalog_object", "system schema", "system schemas", "sys.catalog.system_schema", "schema_namespace", "ScratchBird-owned system schema namespace."),
    ("table", "table", "catalog_object", "table", "tables", "sys.catalog.table", "relation", "Persistent relational table."),
    ("temporary_table", "table", "catalog_object", "temporary table", "temporary tables", "sys.catalog.temporary_table", "relation", "Transaction-scoped or session-scoped temporary table family."),
    ("global_temporary_table", "table", "catalog_object", "global temporary table", "global temporary tables", "sys.catalog.global_temporary_table", "relation", "Shared metadata temporary table whose data never commits and remains transaction isolated."),
    ("private_temporary_table", "table", "catalog_object", "private temporary table", "private temporary tables", "sys.catalog.private_temporary_table", "relation", "Temporary table with temporary metadata and transaction-isolated noncommitting data."),
    ("view", "view", "catalog_object", "view", "views", "sys.catalog.view", "relation", "Stored query projection."),
    ("materialized_view", "view", "catalog_object", "materialized view", "materialized views", "sys.catalog.materialized_view", "relation", "Stored query projection with materialized state."),
    ("sequence", "sequence", "catalog_object", "sequence", "sequences", "sys.catalog.sequence", "generator", "Ordered value generator."),
    ("generator", "generator", "catalog_object", "generator", "generators", "sys.catalog.generator", "generator", "Generator-compatible value source."),
    ("domain", "domain", "catalog_object", "domain", "domains", "sys.catalog.domain", "datatype", "Named datatype with constraints and policy."),
    ("datatype", "datatype", "catalog_object", "datatype", "datatypes", "sys.catalog.datatype", "datatype", "SQL datatype."),
    ("enum", "datatype", "catalog_object", "enumeration type", "enumeration types", "sys.catalog.enum", "datatype", "Datatype with fixed named values."),
    ("range", "datatype", "catalog_object", "range type", "range types", "sys.catalog.range", "datatype", "Datatype representing a bounded range."),
    ("composite_type", "datatype", "catalog_object", "composite type", "composite types", "sys.catalog.composite_type", "datatype", "Datatype composed from fields."),
    ("function", "function", "catalog_object", "function", "functions", "sys.catalog.function", "routine", "Scalar or table function."),
    ("aggregate_function", "function", "catalog_object", "aggregate function", "aggregate functions", "sys.catalog.aggregate_function", "routine", "Function that aggregates rows."),
    ("procedure", "procedure", "catalog_object", "procedure", "procedures", "sys.catalog.procedure", "routine", "Executable stored procedure."),
    ("trigger", "trigger", "catalog_object", "trigger", "triggers", "sys.catalog.trigger", "routine", "Event-driven catalog object."),
    ("package", "package", "catalog_object", "package", "packages", "sys.catalog.package", "package", "Named package of routines or resources."),
    ("udr_package", "package", "catalog_object", "UDR package", "UDR packages", "sys.catalog.udr_package", "package", "User-defined routine package."),
    ("parser_package", "package", "catalog_object", "parser package", "parser packages", "sys.catalog.parser_package", "package", "Parser extension package."),
    ("index", "index", "catalog_object", "index", "indexes", "sys.catalog.index", "index", "Index over table or expression data."),
    ("index_family", "index", "catalog_object", "index family", "index families", "sys.catalog.index_family", "index", "Index implementation or policy family."),
    ("constraint", "constraint", "catalog_object", "constraint", "constraints", "sys.catalog.constraint", "constraint", "General table or domain constraint."),
    ("primary_key", "constraint", "catalog_object", "primary key", "primary keys", "sys.catalog.primary_key", "constraint", "Primary key constraint."),
    ("foreign_key", "constraint", "catalog_object", "foreign key", "foreign keys", "sys.catalog.foreign_key", "constraint", "Foreign key constraint."),
    ("unique_constraint", "constraint", "catalog_object", "unique constraint", "unique constraints", "sys.catalog.unique_constraint", "constraint", "Uniqueness constraint."),
    ("check_constraint", "constraint", "catalog_object", "check constraint", "check constraints", "sys.catalog.check_constraint", "constraint", "Predicate check constraint."),
    ("exclusion_constraint", "constraint", "catalog_object", "exclusion constraint", "exclusion constraints", "sys.catalog.exclusion_constraint", "constraint", "Exclusion relationship constraint."),
    ("collation", "collation", "resource_object", "collation", "collations", "sys.resource.collation", "language_resource", "Text comparison and ordering resource."),
    ("character_set", "charset", "resource_object", "character set", "character sets", "sys.resource.charset", "language_resource", "Character encoding resource."),
    ("role", "role", "security_object", "role", "roles", "sys.security.role", "security_identity", "Authorization role."),
    ("user", "user", "security_object", "user", "users", "sys.security.user", "security_identity", "Security principal representing a user. Do not conflate with user schema."),
    ("group", "group", "security_object", "group", "groups", "sys.security.group", "security_identity", "Security group."),
    ("grant", "grant", "security_object", "grant", "grants", "sys.security.grant", "security_policy", "Authorization grant."),
    ("policy", "policy", "security_object", "policy", "policies", "sys.security.policy", "security_policy", "General policy object."),
    ("row_policy", "policy", "security_object", "row policy", "row policies", "sys.security.row_policy", "security_policy", "Row-level policy."),
    ("masking_policy", "policy", "security_object", "masking policy", "masking policies", "sys.security.masking_policy", "security_policy", "Value masking policy."),
    ("audit_policy", "policy", "security_object", "audit policy", "audit policies", "sys.security.audit_policy", "security_policy", "Audit recording policy."),
    ("tenant", "tenant", "security_object", "tenant", "tenants", "sys.security.tenant", "security_identity", "Tenant boundary identity."),
    ("resource_profile", "resource_profile", "resource_object", "resource profile", "resource profiles", "sys.resource.profile", "language_resource", "Named runtime resource policy profile."),
    ("language_profile", "language_profile", "resource_object", "language profile", "language profiles", "sys.resource.language_profile", "language_resource", "Language resource profile."),
    ("dialect_profile", "dialect_profile", "resource_object", "dialect profile", "dialect profiles", "sys.resource.dialect_profile", "language_resource", "Dialect resource profile."),
    ("topology_profile", "topology_profile", "resource_object", "topology profile", "topology profiles", "sys.resource.topology_profile", "language_resource", "Clause and phrase ordering profile."),
    ("resource_pack", "resource_pack", "resource_object", "resource pack", "resource packs", "sys.resource.pack", "language_resource", "Versioned resource bundle."),
    ("descriptor", "descriptor", "descriptor_object", "descriptor", "descriptors", "sys.descriptor", "descriptor", "Descriptor visible through metadata APIs."),
    ("catalog_descriptor", "descriptor", "descriptor_object", "catalog descriptor", "catalog descriptors", "sys.descriptor.catalog", "descriptor", "Catalog metadata descriptor."),
    ("execution_descriptor", "descriptor", "descriptor_object", "execution descriptor", "execution descriptors", "sys.descriptor.execution", "descriptor", "Execution metadata descriptor."),
    ("transaction", "transaction", "transaction_object", "transaction", "transactions", "sys.transaction", "transaction", "MGA transaction identity and state."),
    ("cursor", "cursor", "execution_object", "cursor", "cursors", "sys.execution.cursor", "execution", "Cursor over a result stream."),
    ("prepared_statement", "prepared_statement", "execution_object", "prepared statement", "prepared statements", "sys.execution.prepared_statement", "execution", "Prepared statement handle."),
    ("result_set", "result_set", "execution_object", "result set", "result sets", "sys.execution.result_set", "execution", "Query result stream or materialized result."),
    ("support_bundle", "support_bundle", "support_object", "support bundle", "support bundles", "sys.support.bundle", "support", "Redacted support evidence bundle."),
    ("management_operation", "management_operation", "management_object", "management operation", "management operations", "sys.management.operation", "management", "Administrative operation exposed to SQL tooling."),
    ("server_route", "server_route", "route_object", "server route", "server routes", "sys.route.server", "route", "Server command route."),
    ("listener_route", "listener_route", "route_object", "listener route", "listener routes", "sys.route.listener", "route", "Listener command route."),
    ("parser_route", "parser_route", "route_object", "parser route", "parser routes", "sys.route.parser", "route", "Parser worker route."),
    ("storage_object", "storage_object", "storage_object", "storage object", "storage objects", "sys.storage.object", "storage", "Physical or logical storage object."),
    ("backup_object", "backup_object", "archive_object", "backup object", "backup objects", "sys.archive.backup", "archive", "Backup operation or artifact."),
    ("restore_object", "restore_object", "archive_object", "restore object", "restore objects", "sys.archive.restore", "archive", "Restore operation or artifact."),
    ("archive_object", "archive_object", "archive_object", "archive object", "archive objects", "sys.archive.object", "archive", "Archive operation or artifact."),
    ("snapshot", "snapshot", "transaction_object", "snapshot", "snapshots", "sys.transaction.snapshot", "transaction", "Transaction visibility snapshot."),
    ("shadow", "shadow", "storage_object", "shadow", "shadows", "sys.storage.shadow", "storage", "Shadow storage file or object."),
    ("agent", "agent", "agent_object", "agent", "agents", "sys.agent", "agent", "Managed background agent."),
    ("job", "job", "agent_object", "job", "jobs", "sys.agent.job", "agent", "Managed background job."),
    ("metric", "metric", "observability_object", "metric", "metrics", "sys.observability.metric", "observability", "Runtime metric."),
    ("event", "event", "observability_object", "event", "events", "sys.observability.event", "observability", "Runtime event."),
    ("diagnostic", "diagnostic", "observability_object", "diagnostic", "diagnostics", "sys.observability.diagnostic", "observability", "Diagnostic message or code."),
]

INVARIANT_TOKENS = {
    "a", "b", "g1", "g2", "m", "n", "t", "tv", "x", "y", "z",
    "adbc", "ai", "api", "ascii", "bcrypt", "blob", "bool", "bytea", "cdc",
    "char", "clob", "crc32", "cypher", "db2", "ddl", "def", "dml", "dn", "expr",
    "gcd", "gis", "h2", "h3", "json", "jsonb", "jsonpath", "kv", "ldap", "lob", "lpad", "ltrim",
    "md5", "mga", "mssql", "mysql", "nosql", "odbc", "oid", "olap", "os",
    "pg", "pk", "psql", "regr", "rpad", "rtrim", "sb", "sbsql", "sblr", "sha1",
    "sha224", "sha256", "sha384", "sha512", "sha3", "sql", "srid", "st",
    "udr", "uint128", "uri", "url", "uuid", "v1", "v3", "v4", "varchar",
    "wkb", "wkt", "xml", "xmlelement", "xmlforest", "xmlpi", "xmlroot",
    "xmlserialize", "xpath", "xxhash64",
}

SQL_GLOSSARY = {
    "fr-FR": {
        "abs": "absolu", "acceleration": "accélération", "action": "action", "add": "ajouter",
        "advisory": "consultatif", "agent": "agent", "agg": "agrégation", "aggregate": "agrégat",
        "alias": "alias", "alter": "modifier", "and": "et", "any": "nimporte lequel",
        "append": "ajouter", "approx": "approximatif", "archive": "archive", "arg": "argument",
        "args": "arguments", "array": "tableau", "artifact": "artefact", "attribute": "attribut",
        "audit": "audit", "authorization": "autorisation", "backup": "sauvegarde",
        "begin": "débuter", "binary": "binaire", "bit": "bit", "block": "bloc", "body": "corps",
        "boolean": "booléen", "branch": "branche", "bridge": "pont", "buffer": "tampon",
        "build": "construire", "bulk": "en masse", "by": "par", "bytes": "octets",
        "call": "appel", "cancel": "annuler", "capability": "capacité", "cardinality": "cardinalité",
        "case": "cas", "cast": "conversion", "catalog": "catalogue", "ceiling": "plafond",
        "character": "caractère", "chars": "caractères", "checkpoint": "point de contrôle",
        "class": "classe", "clause": "clause", "client": "client", "close": "fermer",
        "cluster": "grappe", "coalesce": "coalescer", "collation": "collation",
        "collect": "collecter", "column": "colonne", "columns": "colonnes", "comment": "commentaire",
        "commit": "valider", "composite": "composite", "compression": "compression",
        "concat": "concaténer", "conflict": "conflit", "connection": "connexion",
        "constraint": "contrainte", "constructor": "constructeur", "contains": "contient",
        "context": "contexte", "copy": "copier", "count": "compte", "create": "créer",
        "current": "actuel", "cursor": "curseur", "cypher": "cypher", "data": "données",
        "database": "base de données", "date": "date", "day": "jour", "declare": "déclarer",
        "default": "défaut", "delete": "supprimer", "descriptor": "descripteur",
        "diagnostic": "diagnostic", "dialect": "dialecte", "digit": "chiffre",
        "disconnect": "déconnecter", "distinct": "distinct", "distance": "distance",
        "doc": "document", "document": "document", "domain": "domaine", "double": "double",
        "drop": "supprimer", "edge": "arête", "element": "élément", "empty": "vide",
        "engine": "moteur", "enum": "énumération", "equals": "égal", "event": "événement",
        "exception": "exception", "execute": "exécuter", "execution": "exécution",
        "exists": "existe", "explain": "expliquer", "expression": "expression",
        "extension": "extension", "extract": "extraire", "field": "champ", "filespace": "espace de fichiers",
        "filter": "filtre", "flags": "indicateurs", "float": "flottant", "floor": "plancher",
        "for": "pour", "foreign": "étrangère", "form": "forme", "format": "format",
        "from": "depuis", "fulltext": "texte intégral", "function": "fonction",
        "generate": "générer", "generator": "générateur", "geom": "géométrie",
        "geometry": "géométrie", "get": "obtenir", "global": "globale", "grant": "autorisation",
        "grantee": "bénéficiaire", "graph": "graphe", "group": "groupe", "handle": "descripteur",
        "has": "possède", "historical": "historique", "id": "identifiant", "identifier": "identifiant",
        "in": "dans", "index": "index", "inner": "interne", "insert": "insérer",
        "integer": "entier", "intersection": "intersection", "interval": "intervalle",
        "into": "dans", "is": "est", "iso": "iso", "item": "élément", "job": "tâche",
        "key": "clé", "keyword": "mot-clé", "label": "étiquette", "lang": "langue",
        "last": "dernier", "lead": "avance", "legacy": "ancien", "length": "longueur",
        "level": "niveau", "like": "comme", "list": "liste", "literal": "littéral",
        "locality": "localité", "locator": "localisateur", "lock": "verrou", "log": "journal",
        "lower": "minuscule", "make": "créer", "management": "gestion", "map": "carte",
        "masking": "masquage", "match": "correspondance", "materialized": "matérialisée",
        "max": "maximum", "merge": "fusionner", "message": "message", "meta": "méta",
        "method": "méthode", "metric": "métrique", "min": "minimum", "mode": "mode",
        "modifier": "modificateur", "months": "mois", "move": "déplacer", "multiset": "multi-ensemble",
        "name": "nom", "named": "nommé", "namespace": "espace de noms", "namespaces": "espaces de noms",
        "native": "natif", "nextval": "valeur suivante", "node": "nœud", "not": "non",
        "null": "nul", "number": "nombre", "numeric": "numérique", "object": "objet",
        "octet": "octet", "of": "de", "offset": "décalage", "on": "sur", "only": "seulement",
        "open": "ouvrir", "op": "opération", "operation": "opération", "operator": "opérateur",
        "option": "option", "options": "options", "or": "ou", "order": "ordre",
        "orderby": "ordre par", "orderbyexpr": "expression ordre par", "over": "sur",
        "overflow": "dépassement", "overload": "surcharge", "package": "paquet",
        "paging": "pagination", "parser": "analyseur", "part": "partie", "partition": "partition",
        "passing": "passage", "path": "chemin", "pattern": "motif", "percentile": "percentile",
        "ping": "ping", "pipeline": "pipeline", "pivot": "pivot", "point": "point",
        "policy": "politique", "pop": "population", "position": "position", "power": "puissance",
        "prepared": "préparée", "previous": "précédente", "primary": "primaire",
        "private": "privée", "privilege": "privilège", "procedure": "procédure",
        "processor": "processeur", "profile": "profil", "projection": "projection",
        "proxy": "mandataire", "query": "requête", "quote": "guillemet", "quoted": "entre guillemets",
        "raise": "lever", "random": "aléatoire", "range": "plage", "rank": "rang",
        "read": "lecture", "record": "enregistrement", "recursive": "récursif",
        "ref": "référence", "reference": "référence", "referencing": "référencement",
        "regexp": "expression régulière", "regex": "expression régulière", "replace": "remplacer",
        "required": "requis", "resignal": "resignaliser", "resource": "ressource",
        "result": "résultat", "restore": "restaurer", "revoke": "révoquer", "reverse": "inverser",
        "role": "rôle", "rollback": "annuler", "routine": "routine", "route": "route",
        "row": "ligne", "rowcount": "nombre de lignes", "rowset": "jeu de lignes",
        "samp": "échantillon", "savepoint": "point de sauvegarde", "scalar": "scalaire",
        "schema": "schéma", "search": "recherche", "security": "sécurité", "select": "sélectionner",
        "sep": "séparateur", "sequence": "séquence", "serialize": "sérialiser", "server": "serveur",
        "servername": "nom serveur", "session": "session", "set": "définir", "setting": "paramètre",
        "shadow": "ombre", "shape": "forme", "shard": "fragment", "show": "afficher", "sign": "signe",
        "similarity": "similarité", "size": "taille", "snapshot": "instantané",
        "source": "source", "special": "spécial", "spec": "spécification", "split": "scinder",
        "start": "début", "state": "état", "statement": "instruction", "stddev": "écart type",
        "stream": "flux", "strictly": "strictement", "string": "chaîne", "substr": "sous-chaîne",
        "substring": "sous-chaîne", "subtype": "sous-type", "subvector": "sous-vecteur",
        "sum": "somme", "support": "assistance", "syntax": "syntaxe", "system": "système",
        "table": "table", "tabular": "tabulaire", "target": "cible", "temp": "temporaire",
        "temporal": "temporel", "temporary": "temporaire", "tenant": "locataire",
        "text": "texte", "time": "heure", "timeout": "délai", "timestamp": "horodatage",
        "timezone": "fuseau horaire", "to": "vers", "trailing": "final", "transaction": "transaction",
        "transform": "transformer", "trigger": "déclencheur", "trunc": "tronquer",
        "type": "type", "typeof": "type de", "unbounded": "non borné", "union": "union",
        "unique": "unique", "unnest": "déplier", "unpivot": "dé-pivot", "unsupported": "non pris en charge",
        "update": "mettre à jour", "upper": "majuscule", "use": "utiliser", "user": "utilisateur",
        "value": "valeur", "values": "valeurs", "var": "variance", "variable": "variable",
        "variance": "variance", "vector": "vecteur", "verb": "verbe", "version": "version",
        "view": "vue", "visibility": "visibilité", "when": "quand", "where": "où",
        "window": "fenêtre", "with": "avec", "withingroup": "dans le groupe", "word": "mot",
        "wrapper": "enveloppe", "write": "écrire", "year": "année", "zone": "zone",
    },
    "de-DE": {
        "action": "Aktion", "add": "hinzufügen", "agent": "Agent", "agg": "Aggregation",
        "aggregate": "Aggregat", "alias": "Alias", "alter": "ändern", "and": "und",
        "append": "anhängen", "approx": "ungefähr", "archive": "Archiv", "arg": "Argument",
        "args": "Argumente", "array": "Array", "artifact": "Artefakt", "attribute": "Attribut",
        "audit": "Audit", "authorization": "Autorisierung", "backup": "Sicherung",
        "begin": "beginnen", "binary": "binär", "bit": "Bit", "block": "Block", "body": "Rumpf",
        "boolean": "boolesch", "bridge": "Brücke", "buffer": "Puffer", "build": "erstellen",
        "by": "nach", "bytes": "Bytes", "call": "Aufruf", "cancel": "abbrechen",
        "capability": "Fähigkeit", "cardinality": "Kardinalität", "case": "Fall",
        "cast": "Umwandlung", "catalog": "Katalog", "character": "Zeichen", "chars": "Zeichen",
        "checkpoint": "Kontrollpunkt", "class": "Klasse", "clause": "Klausel", "client": "Client",
        "close": "schließen", "cluster": "Cluster", "coalesce": "zusammenführen",
        "collation": "Sortierung", "collect": "sammeln", "column": "Spalte", "columns": "Spalten",
        "comment": "Kommentar", "commit": "festschreiben", "composite": "zusammengesetzt",
        "compression": "Komprimierung", "concat": "verketten", "conflict": "Konflikt",
        "connection": "Verbindung", "constraint": "Einschränkung", "constructor": "Konstruktor",
        "contains": "enthält", "context": "Kontext", "copy": "kopieren", "count": "Anzahl",
        "create": "erstellen", "current": "aktuell", "cursor": "Cursor", "data": "Daten",
        "database": "Datenbank", "date": "Datum", "day": "Tag", "declare": "deklarieren",
        "default": "Standard", "delete": "löschen", "descriptor": "Deskriptor",
        "diagnostic": "Diagnose", "dialect": "Dialekt", "digit": "Ziffer",
        "disconnect": "trennen", "distinct": "eindeutig", "distance": "Abstand",
        "doc": "Dokument", "document": "Dokument", "domain": "Domäne", "double": "Double",
        "drop": "entfernen", "edge": "Kante", "element": "Element", "empty": "leer",
        "engine": "Engine", "enum": "Aufzählung", "equals": "gleich", "event": "Ereignis",
        "exception": "Ausnahme", "execute": "ausführen", "execution": "Ausführung",
        "exists": "existiert", "explain": "erklären", "expression": "Ausdruck",
        "extension": "Erweiterung", "extract": "extrahieren", "field": "Feld",
        "filespace": "Dateibereich", "filter": "Filter", "flags": "Flags", "float": "Gleitkomma",
        "floor": "Abrunden", "for": "für", "foreign": "fremd", "form": "Form", "format": "Format",
        "from": "von", "fulltext": "Volltext", "function": "Funktion", "generate": "generieren",
        "generator": "Generator", "geometry": "Geometrie", "get": "holen", "global": "global",
        "grant": "Berechtigung", "grantee": "Empfänger", "graph": "Graph", "group": "Gruppe",
        "handle": "Handle", "historical": "historisch", "id": "Kennung", "identifier": "Bezeichner",
        "in": "in", "index": "Index", "inner": "inner", "insert": "einfügen", "integer": "Ganzzahl",
        "intersection": "Schnittmenge", "interval": "Intervall", "into": "in", "is": "ist",
        "item": "Element", "job": "Auftrag", "key": "Schlüssel", "keyword": "Schlüsselwort",
        "label": "Bezeichnung", "lang": "Sprache", "last": "letzte", "lead": "Vorlauf",
        "legacy": "Altbestand", "length": "Länge", "level": "Ebene", "like": "wie",
        "list": "Liste", "literal": "Literal", "locator": "Lokator", "lock": "Sperre",
        "log": "Protokoll", "lower": "klein", "make": "erstellen", "management": "Verwaltung",
        "map": "Abbildung", "masking": "Maskierung", "match": "Übereinstimmung",
        "materialized": "materialisiert", "max": "Maximum", "merge": "zusammenführen",
        "message": "Nachricht", "method": "Methode", "metric": "Metrik", "min": "Minimum",
        "mode": "Modus", "modifier": "Modifikator", "months": "Monate", "move": "verschieben",
        "name": "Name", "named": "benannt", "namespace": "Namensraum", "namespaces": "Namensräume",
        "native": "nativ", "node": "Knoten", "not": "nicht", "null": "Null",
        "number": "Zahl", "numeric": "numerisch", "object": "Objekt", "octet": "Oktett",
        "of": "von", "offset": "Versatz", "on": "auf", "only": "nur", "open": "öffnen",
        "op": "Operation", "operation": "Operation", "operator": "Operator", "option": "Option",
        "options": "Optionen", "or": "oder", "order": "Reihenfolge", "over": "über",
        "overflow": "Überlauf", "package": "Paket", "paging": "Seitennummerierung",
        "parser": "Parser", "part": "Teil", "partition": "Partition", "path": "Pfad",
        "pattern": "Muster", "percentile": "Perzentil", "pipeline": "Pipeline", "pivot": "Pivot",
        "point": "Punkt", "policy": "Richtlinie", "position": "Position", "power": "Potenz",
        "prepared": "vorbereitet", "previous": "vorherig", "primary": "primär",
        "private": "privat", "privilege": "Privileg", "procedure": "Prozedur",
        "processor": "Prozessor", "profile": "Profil", "projection": "Projektion",
        "proxy": "Proxy", "query": "Abfrage", "quote": "Anführungszeichen", "quoted": "zitiert",
        "raise": "auslösen", "random": "zufällig", "range": "Bereich", "rank": "Rang",
        "read": "lesen", "record": "Datensatz", "recursive": "rekursiv", "ref": "Referenz",
        "reference": "Referenz", "regexp": "regulärer Ausdruck", "regex": "regulärer Ausdruck",
        "replace": "ersetzen", "required": "erforderlich", "resource": "Ressource",
        "result": "Ergebnis", "restore": "wiederherstellen", "revoke": "entziehen",
        "reverse": "umkehren", "role": "Rolle", "rollback": "zurückrollen", "routine": "Routine",
        "route": "Route", "row": "Zeile", "rowcount": "Zeilenzahl", "rowset": "Zeilensatz",
        "savepoint": "Sicherpunkt", "scalar": "Skalar", "schema": "Schema", "search": "Suche",
        "security": "Sicherheit", "select": "auswählen", "sequence": "Sequenz",
        "serialize": "serialisieren", "server": "Server", "session": "Sitzung", "set": "setzen",
        "setting": "Einstellung", "shadow": "Schatten", "shape": "Form", "shard": "Shard",
        "show": "anzeigen",
        "sign": "Vorzeichen", "similarity": "Ähnlichkeit", "size": "Größe", "snapshot": "Momentaufnahme",
        "source": "Quelle", "special": "speziell", "spec": "Spezifikation", "split": "teilen",
        "start": "Start", "state": "Status", "statement": "Anweisung", "stddev": "Standardabweichung",
        "stream": "Stream", "strictly": "streng", "string": "Zeichenkette", "substr": "Teilzeichenkette",
        "substring": "Teilzeichenkette", "subtype": "Untertyp", "subvector": "Untervektor",
        "sum": "Summe", "support": "Support", "syntax": "Syntax", "system": "System",
        "table": "Tabelle", "tabular": "tabellarisch", "target": "Ziel", "temp": "temporär",
        "temporal": "zeitlich", "temporary": "temporär", "tenant": "Mandant", "text": "Text",
        "time": "Zeit", "timeout": "Zeitlimit", "timestamp": "Zeitstempel", "timezone": "Zeitzone",
        "to": "nach", "transaction": "Transaktion", "transform": "transformieren",
        "trigger": "Trigger", "trunc": "abschneiden", "type": "Typ", "typeof": "Typ von",
        "union": "Vereinigung", "unique": "eindeutig", "update": "aktualisieren",
        "upper": "groß", "use": "verwenden", "user": "Benutzer", "value": "Wert",
        "values": "Werte", "var": "Varianz", "variable": "Variable", "variance": "Varianz",
        "vector": "Vektor", "verb": "Verb", "version": "Version", "view": "Ansicht",
        "visibility": "Sichtbarkeit", "when": "wenn", "where": "wo", "window": "Fenster",
        "with": "mit", "word": "Wort", "wrapper": "Wrapper", "write": "schreiben",
        "year": "Jahr", "zone": "Zone",
    },
    "it-IT": {
        "select": "selezionare", "from": "da", "where": "dove", "insert": "inserire",
        "update": "aggiornare", "delete": "eliminare", "create": "creare", "alter": "modificare",
        "drop": "rimuovere", "table": "tabella", "schema": "schema", "user": "utente",
    "role": "ruolo", "grant": "autorizzazione", "function": "funzione", "procedure": "procedura",
    "statement": "istruzione", "temporary": "temporanea", "global": "globale", "private": "privata",
        "cursor": "cursore", "transaction": "transazione", "diagnostic": "diagnostica",
        "show": "mostra",
    },
    "es-ES": {
        "select": "seleccionar", "from": "desde", "where": "donde", "insert": "insertar",
        "update": "actualizar", "delete": "eliminar", "create": "crear", "alter": "modificar",
        "drop": "descartar", "table": "tabla", "schema": "esquema", "user": "usuario",
    "role": "rol", "grant": "permiso", "function": "funcion", "procedure": "procedimiento",
    "statement": "sentencia", "temporary": "temporal", "global": "global", "private": "privada",
        "cursor": "cursor", "transaction": "transaccion", "diagnostic": "diagnostico",
        "show": "mostrar",
    },
}

SQL_GLOSSARY["fr-CA"] = {
    **SQL_GLOSSARY["fr-FR"],
    "email": "courriel",
    "file": "fichier",
    "files": "fichiers",
    "locality": "emplacement",
    "support": "soutien",
}

for _tag, _terms in {
    "fr-FR": {
        "assignment": "affectation", "backend": "arrière-plan", "deprecation": "dépréciation",
        "desc": "décroissant", "else": "sinon", "fetch": "extraire", "full": "complet",
        "hint": "indice", "immutable": "immuable", "join": "jointure", "limit": "limite",
        "long": "long", "marker": "marqueur", "outer": "externe", "parameter": "paramètre",
        "refuse": "refuser", "right": "droite", "signal": "signal", "status": "statut",
        "strategy": "stratégie", "truncate": "tronquer", "variant": "variante",
        "warning": "avertissement", "within": "dans",
    },
    "fr-CA": {
        "assignment": "affectation", "backend": "arrière-plan", "deprecation": "dépréciation",
        "desc": "décroissant", "else": "sinon", "fetch": "extraire", "full": "complet",
        "hint": "indice", "immutable": "immuable", "join": "jointure", "limit": "limite",
        "long": "long", "marker": "marqueur", "outer": "externe", "parameter": "paramètre",
        "refuse": "refuser", "right": "droite", "signal": "signal", "status": "statut",
        "strategy": "stratégie", "truncate": "tronquer", "variant": "variante",
        "warning": "avertissement", "within": "dans",
    },
    "de-DE": {
        "assignment": "Zuweisung", "backend": "Backend", "deprecation": "Veraltung",
        "desc": "absteigend", "else": "sonst", "fetch": "abrufen", "full": "voll",
        "hint": "Hinweis", "immutable": "unveränderlich", "join": "Verbund", "limit": "Grenze",
        "long": "lang", "marker": "Markierung", "outer": "außen", "parameter": "Parameter",
        "refuse": "verweigern", "right": "rechts", "signal": "Signal", "status": "Status",
        "strategy": "Strategie", "truncate": "kürzen", "variant": "Variante",
        "warning": "Warnung", "within": "innerhalb",
    },
    "it-IT": {
        "assignment": "assegnazione", "backend": "backend", "deprecation": "deprecazione",
        "desc": "decrescente", "else": "altrimenti", "fetch": "preleva", "full": "completo",
        "hint": "suggerimento", "immutable": "immutabile", "join": "join", "limit": "limite",
        "long": "lungo", "marker": "marcatore", "outer": "esterna", "parameter": "parametro",
        "refuse": "rifiuta", "right": "destra", "signal": "segnale", "status": "stato",
        "strategy": "strategia", "truncate": "tronca", "variant": "variante",
        "warning": "avviso", "within": "entro",
    },
    "es-ES": {
        "assignment": "asignación", "backend": "backend", "deprecation": "obsolescencia",
        "desc": "descendente", "else": "sino", "fetch": "obtener", "full": "completo",
        "hint": "indicio", "immutable": "inmutable", "join": "unión", "limit": "límite",
        "long": "largo", "marker": "marcador", "outer": "externa", "parameter": "parámetro",
        "refuse": "rechazar", "right": "derecha", "signal": "señal", "status": "estado",
        "strategy": "estrategia", "truncate": "truncar", "variant": "variante",
        "warning": "advertencia", "within": "dentro",
    },
}.items():
    SQL_GLOSSARY[_tag].update(_terms)

SQL_GLOSSARY["it-IT"].update({
    "abs": "assoluto", "acceleration": "accelerazione", "action": "azione", "add": "aggiungi",
    "advisory": "consultivo", "agent": "agente", "agg": "aggregazione", "aggregate": "aggregato",
    "alias": "alias", "and": "e", "any": "qualsiasi", "append": "aggiungi", "approx": "approssimato",
    "archive": "archivio", "arg": "argomento", "args": "argomenti", "array": "array",
    "artifact": "artefatto", "attribute": "attributo", "audit": "audit", "authorization": "autorizzazione",
    "backup": "backup", "begin": "inizia", "binary": "binario", "bit": "bit", "block": "blocco",
    "body": "corpo", "boolean": "booleano", "bridge": "ponte", "buffer": "buffer",
    "build": "costruisci", "bulk": "massivo", "by": "per", "bytes": "byte", "call": "chiamata",
    "cancel": "annulla", "capability": "capacità", "cardinality": "cardinalità", "case": "caso",
    "cast": "conversione", "catalog": "catalogo", "ceiling": "soffitto", "character": "carattere",
    "chars": "caratteri", "checkpoint": "punto di controllo", "class": "classe", "clause": "clausola",
    "client": "client", "close": "chiudi", "cluster": "cluster", "coalesce": "coalesci",
    "collation": "collazione", "collect": "raccogli", "column": "colonna", "columns": "colonne",
    "comment": "commento", "commit": "conferma", "composite": "composto", "compression": "compressione",
    "concat": "concatena", "conflict": "conflitto", "connection": "connessione", "constraint": "vincolo",
    "constructor": "costruttore", "contains": "contiene", "context": "contesto", "copy": "copia",
    "count": "conteggio", "current": "corrente", "cypher": "cypher", "data": "dati",
    "database": "database", "date": "data", "day": "giorno", "declare": "dichiara",
    "default": "predefinito", "descriptor": "descrittore", "dialect": "dialetto",
    "digit": "cifra", "disconnect": "disconnetti", "distinct": "distinto", "distance": "distanza",
    "doc": "documento", "document": "documento", "domain": "dominio", "double": "doppio",
    "edge": "arco", "element": "elemento", "empty": "vuoto", "engine": "motore",
    "enum": "enumerazione", "equals": "uguale", "event": "evento", "exception": "eccezione",
    "execute": "esegui", "execution": "esecuzione", "exists": "esiste", "explain": "spiega",
    "expression": "espressione", "extension": "estensione", "extract": "estrai",
    "field": "campo", "filespace": "spazio file", "filter": "filtro", "flags": "flag",
    "float": "virgola mobile", "floor": "pavimento", "for": "per", "foreign": "esterna",
    "form": "forma", "format": "formato", "fulltext": "testo completo", "generate": "genera",
    "generator": "generatore", "geom": "geometria", "geometry": "geometria", "get": "ottieni",
    "grantee": "beneficiario", "graph": "grafo", "group": "gruppo", "handle": "handle",
    "has": "ha", "historical": "storico", "id": "identificatore", "identifier": "identificatore",
    "in": "in", "index": "indice", "inner": "interno", "integer": "intero",
    "intersection": "intersezione", "interval": "intervallo", "into": "in", "is": "è",
    "item": "elemento", "job": "lavoro", "key": "chiave", "keyword": "parola chiave",
    "label": "etichetta", "lang": "lingua", "last": "ultimo", "lead": "anticipo",
    "legacy": "legacy", "length": "lunghezza", "level": "livello", "like": "come",
    "list": "lista", "literal": "letterale", "locality": "località", "locator": "localizzatore",
    "lock": "blocco", "log": "registro", "lower": "minuscolo", "make": "crea",
    "management": "gestione", "map": "mappa", "masking": "mascheramento", "match": "corrispondenza",
    "materialized": "materializzata", "max": "massimo", "merge": "unisci", "message": "messaggio",
    "meta": "meta", "method": "metodo", "metric": "metrica", "min": "minimo",
    "mode": "modalità", "modifier": "modificatore", "months": "mesi", "move": "sposta",
    "multiset": "multiinsieme", "name": "nome", "named": "nominato", "namespace": "spazio dei nomi",
    "namespaces": "spazi dei nomi", "native": "nativo", "node": "nodo", "not": "non",
    "null": "nullo", "number": "numero", "numeric": "numerico", "object": "oggetto",
    "octet": "ottetto", "of": "di", "offset": "scostamento", "on": "su", "only": "solo",
    "open": "apri", "op": "operazione", "operation": "operazione", "operator": "operatore",
    "option": "opzione", "options": "opzioni", "or": "o", "order": "ordine",
    "orderby": "ordina per", "orderbyexpr": "espressione ordina per", "over": "su",
    "overflow": "traboccamento", "overload": "sovraccarico", "package": "pacchetto",
    "paging": "paginazione", "parser": "analizzatore", "part": "parte", "partition": "partizione",
    "passing": "passaggio", "path": "percorso", "pattern": "modello", "percentile": "percentile",
    "pipeline": "pipeline", "pivot": "pivot", "point": "punto", "policy": "criterio",
    "pop": "popolazione", "position": "posizione", "power": "potenza", "prepared": "preparata",
    "previous": "precedente", "primary": "primaria", "privilege": "privilegio",
    "processor": "processore", "profile": "profilo", "projection": "proiezione",
    "proxy": "proxy", "query": "query", "quote": "virgolette", "quoted": "tra virgolette",
    "raise": "solleva", "random": "casuale", "range": "intervallo", "rank": "rango",
    "read": "lettura", "record": "record", "recursive": "ricorsivo", "ref": "riferimento",
    "reference": "riferimento", "referencing": "riferimento", "regexp": "espressione regolare",
    "regex": "espressione regolare", "replace": "sostituisci", "required": "richiesto",
    "resource": "risorsa", "result": "risultato", "restore": "ripristina", "revoke": "revoca",
    "reverse": "inverso", "rollback": "rollback", "routine": "routine", "route": "percorso",
    "row": "riga", "rowcount": "conteggio righe", "rowset": "insieme righe",
    "samp": "campione", "savepoint": "punto di salvataggio", "scalar": "scalare",
    "search": "ricerca", "security": "sicurezza", "sep": "separatore", "sequence": "sequenza",
    "serialize": "serializza", "server": "server", "servername": "nome server",
    "session": "sessione", "set": "imposta", "setting": "impostazione", "shadow": "ombra",
    "shape": "forma", "shard": "frammento", "sign": "segno", "similarity": "similarità",
    "size": "dimensione", "snapshot": "istantanea", "source": "sorgente", "special": "speciale",
    "spec": "specifica", "split": "dividi", "start": "inizio", "state": "stato",
    "stddev": "deviazione standard", "stream": "flusso", "strictly": "strettamente",
    "string": "stringa", "substr": "sottostringa", "substring": "sottostringa",
    "subtype": "sottotipo", "subvector": "sottovettore", "sum": "somma", "support": "supporto",
    "syntax": "sintassi", "system": "sistema", "tabular": "tabellare", "target": "destinazione",
    "temp": "temporanea", "temporal": "temporale", "tenant": "tenant", "text": "testo",
    "time": "ora", "timeout": "timeout", "timestamp": "marcatura temporale",
    "timezone": "fuso orario", "to": "a", "transform": "trasforma", "trunc": "tronca",
    "type": "tipo", "typeof": "tipo di", "union": "unione", "unique": "unico",
    "unnest": "espandi", "unpivot": "de-pivot", "unsupported": "non supportato",
    "upper": "maiuscolo", "use": "usa", "value": "valore", "values": "valori",
    "var": "varianza", "variable": "variabile", "variance": "varianza", "vector": "vettore",
    "verb": "verbo", "version": "versione", "view": "vista", "visibility": "visibilità",
    "when": "quando", "window": "finestra", "with": "con", "withingroup": "nel gruppo",
    "word": "parola", "wrapper": "wrapper", "write": "scrivi", "year": "anno", "zone": "zona",
})

SQL_GLOSSARY["es-ES"].update({
    "abs": "absoluto", "acceleration": "aceleración", "action": "acción", "add": "agregar",
    "advisory": "consultivo", "agent": "agente", "agg": "agregación", "aggregate": "agregado",
    "alias": "alias", "and": "y", "any": "cualquiera", "append": "añadir", "approx": "aproximado",
    "archive": "archivo", "arg": "argumento", "args": "argumentos", "array": "arreglo",
    "artifact": "artefacto", "attribute": "atributo", "audit": "auditoría", "authorization": "autorización",
    "backup": "copia de seguridad", "begin": "iniciar", "binary": "binario", "bit": "bit",
    "block": "bloque", "body": "cuerpo", "boolean": "booleano", "bridge": "puente",
    "buffer": "búfer", "build": "construir", "bulk": "masivo", "by": "por", "bytes": "bytes",
    "call": "llamada", "cancel": "cancelar", "capability": "capacidad", "cardinality": "cardinalidad",
    "case": "caso", "cast": "conversión", "catalog": "catálogo", "ceiling": "techo",
    "character": "carácter", "chars": "caracteres", "checkpoint": "punto de control",
    "class": "clase", "clause": "cláusula", "client": "cliente", "close": "cerrar",
    "cluster": "clúster", "coalesce": "coalescer", "collation": "intercalación",
    "collect": "recoger", "column": "columna", "columns": "columnas", "comment": "comentario",
    "commit": "confirmar", "composite": "compuesto", "compression": "compresión",
    "concat": "concatenar", "conflict": "conflicto", "connection": "conexión",
    "constraint": "restricción", "constructor": "constructor", "contains": "contiene",
    "context": "contexto", "copy": "copiar", "count": "conteo", "current": "actual",
    "cypher": "cypher", "data": "datos", "database": "base de datos", "date": "fecha",
    "day": "día", "declare": "declarar", "default": "predeterminado", "descriptor": "descriptor",
    "dialect": "dialecto", "digit": "dígito", "disconnect": "desconectar", "distinct": "distinto",
    "distance": "distancia", "doc": "documento", "document": "documento", "domain": "dominio",
    "double": "doble", "edge": "arista", "element": "elemento", "empty": "vacío",
    "engine": "motor", "enum": "enumeración", "equals": "igual", "event": "evento",
    "exception": "excepción", "execute": "ejecutar", "execution": "ejecución",
    "exists": "existe", "explain": "explicar", "expression": "expresión", "extension": "extensión",
    "extract": "extraer", "field": "campo", "filespace": "espacio de archivos", "filter": "filtro",
    "flags": "indicadores", "float": "flotante", "floor": "piso", "for": "para",
    "foreign": "externa", "form": "forma", "format": "formato", "fulltext": "texto completo",
    "generate": "generar", "generator": "generador", "geom": "geometría", "geometry": "geometría",
    "get": "obtener", "grantee": "beneficiario", "graph": "grafo", "group": "grupo",
    "handle": "manejador", "has": "tiene", "historical": "histórico", "id": "identificador",
    "identifier": "identificador", "in": "en", "index": "índice", "inner": "interno",
    "integer": "entero", "intersection": "intersección", "interval": "intervalo",
    "into": "en", "is": "es", "item": "elemento", "job": "trabajo", "key": "clave",
    "keyword": "palabra clave", "label": "etiqueta", "lang": "idioma", "last": "último",
    "lead": "adelanto", "legacy": "heredado", "length": "longitud", "level": "nivel",
    "like": "como", "list": "lista", "literal": "literal", "locality": "localidad",
    "locator": "localizador", "lock": "bloqueo", "log": "registro", "lower": "minúscula",
    "make": "crear", "management": "administración", "map": "mapa", "masking": "enmascaramiento",
    "match": "coincidencia", "materialized": "materializada", "max": "máximo",
    "merge": "fusionar", "message": "mensaje", "meta": "meta", "method": "método",
    "metric": "métrica", "min": "mínimo", "mode": "modo", "modifier": "modificador",
    "months": "meses", "move": "mover", "multiset": "multiconjunto", "name": "nombre",
    "named": "nombrado", "namespace": "espacio de nombres", "namespaces": "espacios de nombres",
    "native": "nativo", "node": "nodo", "not": "no", "null": "nulo", "number": "número",
    "numeric": "numérico", "object": "objeto", "octet": "octeto", "of": "de",
    "offset": "desplazamiento", "on": "en", "only": "solo", "open": "abrir",
    "op": "operación", "operation": "operación", "operator": "operador", "option": "opción",
    "options": "opciones", "or": "o", "order": "orden", "orderby": "ordenar por",
    "orderbyexpr": "expresión ordenar por", "over": "sobre", "overflow": "desbordamiento",
    "overload": "sobrecarga", "package": "paquete", "paging": "paginación", "parser": "analizador",
    "part": "parte", "partition": "partición", "passing": "paso", "path": "ruta",
    "pattern": "patrón", "percentile": "percentil", "pipeline": "canalización",
    "pivot": "pivote", "point": "punto", "policy": "política", "pop": "población",
    "position": "posición", "power": "potencia", "prepared": "preparada", "previous": "anterior",
    "primary": "primaria", "privilege": "privilegio", "processor": "procesador",
    "profile": "perfil", "projection": "proyección", "proxy": "proxy", "query": "consulta",
    "quote": "comilla", "quoted": "entre comillas", "raise": "lanzar", "random": "aleatorio",
    "range": "rango", "rank": "rango", "read": "lectura", "record": "registro",
    "recursive": "recursivo", "ref": "referencia", "reference": "referencia",
    "referencing": "referenciando", "regexp": "expresión regular", "regex": "expresión regular",
    "replace": "reemplazar", "required": "requerido", "resource": "recurso", "result": "resultado",
    "restore": "restaurar", "revoke": "revocar", "reverse": "inverso", "rollback": "revertir",
    "routine": "rutina", "route": "ruta", "row": "fila", "rowcount": "conteo de filas",
    "rowset": "conjunto de filas", "samp": "muestra", "savepoint": "punto de guardado",
    "scalar": "escalar", "search": "búsqueda", "security": "seguridad", "sep": "separador",
    "sequence": "secuencia", "serialize": "serializar", "server": "servidor",
    "servername": "nombre del servidor", "session": "sesión", "set": "establecer",
    "setting": "configuración", "shadow": "sombra", "shape": "forma", "shard": "fragmento",
    "sign": "signo", "similarity": "similitud", "size": "tamaño", "snapshot": "instantánea",
    "source": "origen", "special": "especial", "spec": "especificación", "split": "dividir",
    "start": "inicio", "state": "estado", "stddev": "desviación estándar", "stream": "flujo",
    "strictly": "estrictamente", "string": "cadena", "substr": "subcadena",
    "substring": "subcadena", "subtype": "subtipo", "subvector": "subvector",
    "sum": "suma", "support": "soporte", "syntax": "sintaxis", "system": "sistema",
    "tabular": "tabular", "target": "destino", "temp": "temporal", "temporal": "temporal",
    "tenant": "inquilino", "text": "texto", "time": "hora", "timeout": "tiempo de espera",
    "timestamp": "marca de tiempo", "timezone": "zona horaria", "to": "a",
    "transform": "transformar", "trunc": "truncar", "type": "tipo", "typeof": "tipo de",
    "union": "unión", "unique": "único", "unnest": "expandir", "unpivot": "despivotar",
    "unsupported": "no compatible", "upper": "mayúscula", "use": "usar", "value": "valor",
    "values": "valores", "var": "varianza", "variable": "variable", "variance": "varianza",
    "vector": "vector", "verb": "verbo", "version": "versión", "view": "vista",
    "visibility": "visibilidad", "when": "cuando", "window": "ventana", "with": "con",
    "withingroup": "dentro del grupo", "word": "palabra", "wrapper": "envoltorio",
    "write": "escribir", "year": "año", "zone": "zona",
})

for _tag, _terms in {
    "fr-FR": {
        "accepted": "accepté", "actual": "réel", "already": "déjà", "ambiguous": "ambigu",
        "approval": "approbation", "asan": "ASan", "authentication": "authentification", "authority": "autorité",
        "available": "disponible", "candidate": "candidat", "checksum": "somme de contrôle",
        "classified": "classé", "closed": "fermé", "configuration": "configuration", "could": "a pu", "denied": "refusée",
        "detected": "détecté", "did": "a", "duplicate": "doublon", "external": "externe",
        "failed": "échoué", "failure": "échec", "file": "fichier", "forbidden": "interdit",
        "forbids": "interdit", "found": "trouvé", "hash": "hachage", "invalid": "invalide", "journal": "journal", "keyring": "trousseau de clés",
        "lifecycle": "cycle de vie", "lossiness": "perte", "malformed": "mal formé",
        "manifest": "manifeste", "match": "correspondance", "mismatch": "incompatibilité", "missing": "manquant",
        "opened": "ouvert", "pack": "paquet", "permission": "autorisation", "policy": "politique", "profile": "profil", "ready": "prêt", "redacted": "masqué",
        "renderable": "rendu possible", "rendered": "rendu", "requested": "demandé",
        "required": "requis", "requires": "requiert", "secret": "secret", "store": "magasin",
        "tsan": "TSan", "unavailable": "indisponible", "unsafe": "non sûr", "validated": "validé",
        "validation": "validation", "was": "a été",
    },
    "fr-CA": {
        "accepted": "accepté", "actual": "réel", "already": "déjà", "ambiguous": "ambigu",
        "approval": "approbation", "asan": "ASan", "authentication": "authentification", "authority": "autorité",
        "available": "disponible", "candidate": "candidat", "checksum": "somme de contrôle",
        "classified": "classé", "closed": "fermé", "configuration": "configuration", "could": "a pu", "denied": "refusée",
        "detected": "détecté", "did": "a", "duplicate": "doublon", "external": "externe",
        "failed": "échoué", "failure": "échec", "file": "fichier", "forbidden": "interdit",
        "forbids": "interdit", "found": "trouvé", "hash": "hachage", "invalid": "invalide", "journal": "journal", "keyring": "trousseau de clés",
        "lifecycle": "cycle de vie", "lossiness": "perte", "malformed": "mal formé",
        "manifest": "manifeste", "match": "correspondance", "mismatch": "incompatibilité", "missing": "manquant",
        "opened": "ouvert", "pack": "paquet", "permission": "autorisation", "policy": "politique", "profile": "profil", "ready": "prêt", "redacted": "caviardé",
        "renderable": "rendu possible", "rendered": "rendu", "requested": "demandé",
        "required": "requis", "requires": "requiert", "secret": "secret", "store": "magasin",
        "tsan": "TSan", "unavailable": "indisponible", "unsafe": "non sûr", "validated": "validé",
        "validation": "validation", "was": "a été",
    },
    "de-DE": {
        "accepted": "akzeptiert", "actual": "tatsächlich", "already": "bereits", "ambiguous": "mehrdeutig",
        "approval": "Freigabe", "asan": "ASan", "authentication": "Authentifizierung", "authority": "Autorität",
        "available": "verfügbar", "candidate": "Kandidat", "checksum": "Prüfsumme",
        "classified": "klassifiziert", "closed": "geschlossen", "configuration": "Konfiguration", "could": "konnte", "denied": "verweigert",
        "detected": "erkannt", "did": "hat", "duplicate": "Duplikat", "external": "extern",
        "failed": "fehlgeschlagen", "failure": "Fehler", "file": "Datei", "forbidden": "verboten",
        "forbids": "verbietet", "found": "gefunden", "hash": "Hash", "invalid": "ungültig", "journal": "Journal", "keyring": "Schlüsselbund",
        "lifecycle": "Lebenszyklus", "lossiness": "Verlustigkeit", "malformed": "fehlerhaft",
        "manifest": "Manifest", "match": "Übereinstimmung", "mismatch": "Nichtübereinstimmung", "missing": "fehlend",
        "opened": "geöffnet", "pack": "Paket", "permission": "Berechtigung", "policy": "Richtlinie", "profile": "Profil", "ready": "bereit", "redacted": "redigiert",
        "renderable": "darstellbar", "rendered": "gerendert", "requested": "angefordert",
        "required": "erforderlich", "requires": "erfordert", "secret": "Geheimnis", "store": "Speicher",
        "tsan": "TSan", "unavailable": "nicht verfügbar", "unsafe": "unsicher", "validated": "validiert",
        "validation": "Validierung", "was": "wurde",
    },
    "it-IT": {
        "accepted": "accettato", "actual": "effettivo", "already": "già", "ambiguous": "ambiguo",
        "approval": "approvazione", "asan": "ASan", "authentication": "autenticazione", "authority": "autorità",
        "available": "disponibile", "candidate": "candidato", "checksum": "checksum",
        "classified": "classificata", "closed": "chiuso", "configuration": "configurazione", "could": "poteva", "denied": "negata",
        "detected": "rilevato", "did": "ha", "duplicate": "duplicato", "external": "esterno",
        "failed": "fallito", "failure": "errore", "file": "file", "forbidden": "vietato",
        "forbids": "vieta", "found": "trovato", "hash": "hash", "invalid": "non valido", "journal": "giornale", "keyring": "portachiavi",
        "lifecycle": "ciclo di vita", "lossiness": "perdita", "malformed": "malformato",
        "manifest": "manifesto", "match": "corrispondenza", "mismatch": "mancata corrispondenza", "missing": "mancante",
        "opened": "aperto", "pack": "pacchetto", "permission": "permesso", "policy": "criterio", "profile": "profilo", "ready": "pronto", "redacted": "oscurato",
        "renderable": "renderizzabile", "rendered": "renderizzato", "requested": "richiesto",
        "required": "richiesto", "requires": "richiede", "secret": "segreto", "store": "archivio",
        "tsan": "TSan", "unavailable": "non disponibile", "unsafe": "non sicuro", "validated": "convalidato",
        "validation": "convalida", "was": "è stato",
    },
    "es-ES": {
        "accepted": "aceptado", "actual": "real", "already": "ya", "ambiguous": "ambiguo",
        "approval": "aprobación", "asan": "ASan", "authentication": "autenticación", "authority": "autoridad",
        "available": "disponible", "candidate": "candidato", "checksum": "suma de comprobación",
        "classified": "clasificado", "closed": "cerrado", "configuration": "configuración", "could": "pudo", "denied": "denegada",
        "detected": "detectado", "did": "hizo", "duplicate": "duplicado", "external": "externo",
        "failed": "fallido", "failure": "fallo", "file": "archivo", "forbidden": "prohibido",
        "forbids": "prohíbe", "found": "encontrado", "hash": "hash", "invalid": "no válido", "journal": "diario", "keyring": "llavero",
        "lifecycle": "ciclo de vida", "lossiness": "pérdida", "malformed": "mal formado",
        "manifest": "manifiesto", "match": "coincidencia", "mismatch": "discordancia", "missing": "faltante",
        "opened": "abierto", "pack": "paquete", "permission": "permiso", "policy": "política", "profile": "perfil", "ready": "listo", "redacted": "redactado",
        "renderable": "representable", "rendered": "representado", "requested": "solicitado",
        "required": "requerido", "requires": "requiere", "secret": "secreto", "store": "almacén",
        "tsan": "TSan", "unavailable": "no disponible", "unsafe": "inseguro", "validated": "validado",
        "validation": "validación", "was": "fue",
    },
}.items():
    SQL_GLOSSARY[_tag].update(_terms)

DIAGNOSTIC_TRANSLATIONS = {
    "fr-FR": {
        "SBSQL.LANG_RESOURCE.FALLBACK_TO_CANONICAL_ENGLISH": "Profil linguistique préféré non compatible; passage au SBsql anglais canonique.",
        "SBSQL.LANG_RESOURCE.FAIL_CLOSED_ON_PROFILE_MISMATCH": "Incompatibilité du profil linguistique; opération refusée en mode fermé.",
        "SBSQL.LANG_RESOURCE.RENDERER_LOSSINESS_CLASSIFIED": "La perte du rendu a été classée avant la sortie.",
        "SBSQL.LANG_RESOURCE.RENDERER_NOT_RENDERABLE": "Le flux SBLR demandé ne peut pas être rendu dans la langue préférée.",
    },
    "fr-CA": {
        "SBSQL.LANG_RESOURCE.FALLBACK_TO_CANONICAL_ENGLISH": "Profil linguistique préféré non compatible; passage au SBsql anglais canonique.",
        "SBSQL.LANG_RESOURCE.FAIL_CLOSED_ON_PROFILE_MISMATCH": "Incompatibilité du profil linguistique; opération refusée en mode fermé.",
        "SBSQL.LANG_RESOURCE.RENDERER_LOSSINESS_CLASSIFIED": "La perte du rendu a été classée avant la sortie.",
        "SBSQL.LANG_RESOURCE.RENDERER_NOT_RENDERABLE": "Le flux SBLR demandé ne peut pas être rendu dans la langue préférée.",
    },
    "de-DE": {
        "SBSQL.LANG_RESOURCE.FALLBACK_TO_CANONICAL_ENGLISH": "Bevorzugtes Sprachprofil nicht passend; Rückfall auf kanonisches englisches SBsql.",
        "SBSQL.LANG_RESOURCE.FAIL_CLOSED_ON_PROFILE_MISMATCH": "Sprachprofil stimmt nicht überein; Operation wurde fail-closed verweigert.",
        "SBSQL.LANG_RESOURCE.RENDERER_LOSSINESS_CLASSIFIED": "Der Wiedergabeverlust wurde vor der Ausgabe klassifiziert.",
        "SBSQL.LANG_RESOURCE.RENDERER_NOT_RENDERABLE": "Der angeforderte SBLR-Stream kann in der bevorzugten Sprache nicht gerendert werden.",
    },
    "it-IT": {
        "SBSQL.LANG_RESOURCE.FALLBACK_TO_CANONICAL_ENGLISH": "Profilo linguistico preferito non corrispondente; passaggio a SBsql inglese canonico.",
        "SBSQL.LANG_RESOURCE.FAIL_CLOSED_ON_PROFILE_MISMATCH": "Profilo linguistico non corrispondente; operazione rifiutata in fail-closed.",
        "SBSQL.LANG_RESOURCE.RENDERER_LOSSINESS_CLASSIFIED": "La perdita del renderer è stata classificata prima dell'output.",
        "SBSQL.LANG_RESOURCE.RENDERER_NOT_RENDERABLE": "Il flusso SBLR richiesto non può essere reso nella lingua preferita.",
    },
    "es-ES": {
        "SBSQL.LANG_RESOURCE.FALLBACK_TO_CANONICAL_ENGLISH": "El perfil de idioma preferido no coincide; se usa SBsql inglés canónico.",
        "SBSQL.LANG_RESOURCE.FAIL_CLOSED_ON_PROFILE_MISMATCH": "El perfil de idioma no coincide; la operación se rechazó en modo cerrado.",
        "SBSQL.LANG_RESOURCE.RENDERER_LOSSINESS_CLASSIFIED": "La pérdida del renderizador se clasificó antes de la salida.",
        "SBSQL.LANG_RESOURCE.RENDERER_NOT_RENDERABLE": "El flujo SBLR solicitado no se puede representar en el idioma preferido.",
    },
}

MESSAGE_PHRASE_TRANSLATIONS = {
    "Authentication failed.": {
        "fr-FR": "Échec de l'authentification.",
        "fr-CA": "Échec de l'authentification.",
        "de-DE": "Authentifizierung fehlgeschlagen.",
        "it-IT": "Autenticazione fallita.",
        "es-ES": "La autenticación falló.",
    },
    "Authorization forbidden.": {
        "fr-FR": "Autorisation interdite.",
        "fr-CA": "Autorisation interdite.",
        "de-DE": "Autorisierung verboten.",
        "it-IT": "Autorizzazione vietata.",
        "es-ES": "Autorización prohibida.",
    },
    "Resource pack is missing.": {
        "fr-FR": "Le pack de ressources est manquant.",
        "fr-CA": "Le pack de ressources est manquant.",
        "de-DE": "Ressourcenpaket fehlt.",
        "it-IT": "Pacchetto risorse mancante.",
        "es-ES": "Falta el paquete de recursos.",
    },
    "Manifest hash mismatch.": {
        "fr-FR": "Discordance du hachage du manifeste.",
        "fr-CA": "Discordance du hachage du manifeste.",
        "de-DE": "Manifest-Hash stimmt nicht überein.",
        "it-IT": "Hash del manifesto non corrispondente.",
        "es-ES": "Discordancia del hash del manifiesto.",
    },
    "Invalid configuration value.": {
        "fr-FR": "Valeur de configuration non valide.",
        "fr-CA": "Valeur de configuration non valide.",
        "de-DE": "Ungültiger Konfigurationswert.",
        "it-IT": "Valore di configurazione non valido.",
        "es-ES": "Valor de configuración no válido.",
    },
    "Server is not ready.": {
        "fr-FR": "Le serveur n'est pas prêt.",
        "fr-CA": "Le serveur n'est pas prêt.",
        "de-DE": "Der Server ist nicht bereit.",
        "it-IT": "Il server non è pronto.",
        "es-ES": "El servidor no está listo.",
    },
    "Transaction conflict detected.": {
        "fr-FR": "Conflit de transaction détecté.",
        "fr-CA": "Conflit de transaction détecté.",
        "de-DE": "Transaktionskonflikt erkannt.",
        "it-IT": "Conflitto di transazione rilevato.",
        "es-ES": "Conflicto de transacción detectado.",
    },
    "Database file could not be opened.": {
        "fr-FR": "Le fichier de base de données n'a pas pu être ouvert.",
        "fr-CA": "Le fichier de base de données n'a pas pu être ouvert.",
        "de-DE": "Die Datenbankdatei konnte nicht geöffnet werden.",
        "it-IT": "Impossibile aprire il file del database.",
        "es-ES": "No se pudo abrir el archivo de base de datos.",
    },
    "Permission is required.": {
        "fr-FR": "Une autorisation est requise.",
        "fr-CA": "Une autorisation est requise.",
        "de-DE": "Eine Berechtigung ist erforderlich.",
        "it-IT": "È richiesta un'autorizzazione.",
        "es-ES": "Se requiere permiso.",
    },
    "Unsupported operation.": {
        "fr-FR": "Opération non prise en charge.",
        "fr-CA": "Opération non prise en charge.",
        "de-DE": "Nicht unterstützte Operation.",
        "it-IT": "Operazione non supportata.",
        "es-ES": "Operación no compatible.",
    },
    "Schema object was not found.": {
        "fr-FR": "L'objet du schéma est introuvable.",
        "fr-CA": "L'objet du schéma est introuvable.",
        "de-DE": "Schemaobjekt wurde nicht gefunden.",
        "it-IT": "Oggetto dello schema non trovato.",
        "es-ES": "No se encontró el objeto del esquema.",
    },
    "SBLR stream validation failed.": {
        "fr-FR": "La validation du flux SBLR a échoué.",
        "fr-CA": "La validation du flux SBLR a échoué.",
        "de-DE": "SBLR-Stream-Validierung fehlgeschlagen.",
        "it-IT": "Convalida del flusso SBLR non riuscita.",
        "es-ES": "Error de validación del flujo SBLR.",
    },
    "UUID descriptor is invalid.": {
        "fr-FR": "Le descripteur UUID est invalide.",
        "fr-CA": "Le descripteur UUID est invalide.",
        "de-DE": "UUID-Deskriptor ist ungültig.",
        "it-IT": "Il descrittore UUID non è valido.",
        "es-ES": "El descriptor UUID no es válido.",
    },
    "Language profile is not supported.": {
        "fr-FR": "Le profil linguistique n'est pas pris en charge.",
        "fr-CA": "Le profil linguistique n'est pas pris en charge.",
        "de-DE": "Sprachprofil wird nicht unterstützt.",
        "it-IT": "Il profilo linguistico non è supportato.",
        "es-ES": "El perfil de idioma no es compatible.",
    },
    "Diagnostic message redacted by policy.": {
        "fr-FR": "Message de diagnostic masqué par la politique.",
        "fr-CA": "Message de diagnostic caviardé par la politique.",
        "de-DE": "Diagnosemeldung durch Richtlinie redigiert.",
        "it-IT": "Messaggio diagnostico oscurato dal criterio.",
        "es-ES": "Mensaje de diagnóstico redactado por la política.",
    },
}


@dataclass(frozen=True)
class GeneratedFile:
    rel_path: str
    data: bytes


def fail(message: str) -> None:
    print(message, file=sys.stderr)
    raise SystemExit(1)


def canonical_json_bytes(payload: Any) -> bytes:
    return (json.dumps(payload, ensure_ascii=False, sort_keys=True, separators=(",", ":")) + "\n").encode("utf-8")


def canonical_jsonl_bytes(rows: list[dict[str, Any]]) -> bytes:
    return b"".join(canonical_json_bytes(row) for row in rows)


def stable_hash(*parts: str, length: int = 16) -> str:
    digest = hashlib.sha256("\x1f".join(parts).encode("utf-8")).hexdigest()
    return digest[:length]


def stable_id(prefix: str, *parts: str) -> str:
    return f"{prefix}.{stable_hash(*parts)}"


def sha256_bytes(data: bytes) -> str:
    return "sha256:" + hashlib.sha256(data).hexdigest()


def fnv1a64_bytes(data: bytes) -> str:
    value = 0xCBF29CE484222325
    for byte in data:
        value ^= byte
        value = (value * 0x100000001B3) & 0xFFFFFFFFFFFFFFFF
    return f"fnv1a64:{value:016x}"


def safe_token(value: str) -> str:
    token = re.sub(r"[^A-Za-z0-9]+", "_", value.strip()).strip("_").lower()
    return token or "unnamed"


def split_words(value: str) -> list[str]:
    spaced = re.sub(r"([a-z])([A-Z])", r"\1 \2", value)
    spaced = re.sub(r"[^A-Za-z0-9]+", " ", spaced)
    return [word.lower() for word in spaced.split() if word]


def canonical_full_name(canonical_name: str, surface_kind: str) -> str:
    if re.fullmatch(r"[A-Z][A-Z0-9_]*", canonical_name):
        return canonical_name
    words = split_words(canonical_name)
    if surface_kind == "grammar_production" and words and words[-1] == "stmt":
        words[-1] = "statement"
    return " ".join(words) or canonical_name


def keyword_tokens(canonical_name: str) -> list[str]:
    if re.fullmatch(r"[A-Z][A-Z0-9_]*", canonical_name):
        return canonical_name.split("_")
    return [word.upper() for word in split_words(canonical_name) if word.isalpha() and len(word) > 1 and word == word.upper()]


DISPLAY_TOKEN_OVERRIDES = {
    "ai": "AI",
    "asan": "ASan",
    "dbbt": "DBBT",
    "mga": "MGA",
    "sblr": "SBLR",
    "sql": "SQL",
    "sqlstate": "SQLSTATE",
    "tsan": "TSan",
    "ubsan": "UBSan",
    "uuid": "UUID",
}


ONLINE_TRANSLATION_VERIFICATION_PHRASES = (
    "Authentication failed.",
    "Authorization forbidden.",
    "Resource pack is missing.",
    "Manifest hash mismatch.",
    "Invalid configuration value.",
    "Server is not ready.",
    "Transaction conflict detected.",
    "Database file could not be opened.",
    "Permission is required.",
    "Unsupported operation.",
    "Schema object was not found.",
    "SBLR stream validation failed.",
    "UUID descriptor is invalid.",
    "Language profile is not supported.",
    "Diagnostic message redacted by policy.",
)


def load_public_diagnostic_matrix_rows(repo_root: Path) -> list[dict[str, str]]:
    path = repo_root / PUBLIC_DIAGNOSTIC_MATRIX_GENERATOR
    spec = importlib.util.spec_from_file_location("sb_public_diagnostic_matrix_generator", path)
    if spec is None or spec.loader is None:
        fail(f"cannot load public diagnostic matrix generator: {path}")
    module = importlib.util.module_from_spec(spec)
    sys.modules[spec.name] = module
    spec.loader.exec_module(module)
    rows = getattr(module, "MATRIX_ROWS", None)
    if not isinstance(rows, tuple):
        fail("public diagnostic matrix generator did not expose MATRIX_ROWS")
    return [dict(row) for row in rows]


def display_message_word(word: str) -> str:
    lowered = word.lower()
    if lowered in DISPLAY_TOKEN_OVERRIDES:
        return DISPLAY_TOKEN_OVERRIDES[lowered]
    return lowered


def message_key_to_template(message_key: str) -> str:
    words = [display_message_word(word) for word in split_words(message_key)]
    if not words:
        return "Diagnostic message."
    text = " ".join(words)
    return text[:1].upper() + text[1:] + "."


def diagnostic_severity(row: dict[str, str]) -> str:
    status = row.get("compatibility_status", "")
    if status in {"diagnostic_only_stable", "native_runner_required"}:
        return "info"
    if status == "unsupported_stable":
        return "warning"
    return "error"


def extract_sblr_envelope_diagnostics(repo_root: Path) -> list[dict[str, str]]:
    text = (repo_root / SBLR_ENVELOPE_HPP).read_text(encoding="utf-8")
    pattern = re.compile(
        r'decoded\.diagnostic_code\s*=\s*"(?P<code>[^"]+)";\s*'
        r'decoded\.message_key\s*=\s*"(?P<message_key>[^"]+)";',
        re.MULTILINE,
    )
    rows: dict[str, dict[str, str]] = {}
    for match in pattern.finditer(text):
        code = match.group("code")
        rows.setdefault(
            code,
            {
                "diagnostic_id": "sblr.envelope." + safe_token(code),
                "area": "sblr_envelope",
                "code": code,
                "message_key": match.group("message_key"),
                "redaction_class": "public_code_only",
                "compatibility_status": "fail_closed_stable",
                "source_path": SBLR_ENVELOPE_HPP,
                "public_test_path": "project/tests/release/public_sblr_uuid_mga_route_integration_gate.cpp",
            },
        )
    return sorted(rows.values(), key=lambda row: row["code"])


def parse_cpp_string_literal(text: str, start: int) -> tuple[bytes, int]:
    if start >= len(text) or text[start] != '"':
        fail("internal parser expected C++ string literal")
    output = bytearray()
    i = start + 1
    while i < len(text):
        ch = text[i]
        if ch == '"':
            return bytes(output), i + 1
        if ch != "\\":
            output.extend(ch.encode("utf-8"))
            i += 1
            continue
        i += 1
        if i >= len(text):
            fail("unterminated C++ string escape")
        esc = text[i]
        if esc == "x":
            match = re.match(r"x([0-9A-Fa-f]{2})", text[i:])
            if not match:
                fail("invalid C++ hex string escape")
            output.append(int(match.group(1), 16))
            i += 3
            continue
        escapes = {
            "\\": ord("\\"),
            '"': ord('"'),
            "n": ord("\n"),
            "r": ord("\r"),
            "t": ord("\t"),
            "0": 0,
        }
        output.append(escapes.get(esc, ord(esc)))
        i += 1
    fail("unterminated C++ string literal")


def parse_cpp_registry_row(row_text: str) -> list[str]:
    fields: list[str] = []
    i = 0
    while i < len(row_text):
        while i < len(row_text) and row_text[i].isspace():
            i += 1
        if i < len(row_text) and row_text[i] == ",":
            i += 1
            continue
        if i >= len(row_text):
            break
        parts: list[bytes] = []
        while i < len(row_text):
            while i < len(row_text) and row_text[i].isspace():
                i += 1
            if i >= len(row_text) or row_text[i] != '"':
                break
            part, i = parse_cpp_string_literal(row_text, i)
            parts.append(part)
        if not parts:
            fail(f"invalid generated registry row near: {row_text[i:i + 80]}")
        fields.append(b"".join(parts).decode("utf-8", errors="replace"))
        while i < len(row_text) and row_text[i].isspace():
            i += 1
        if i < len(row_text) and row_text[i] == ",":
            i += 1
            continue
        if i < len(row_text):
            fail(f"unexpected generated registry row suffix near: {row_text[i:i + 80]}")
    return fields


def split_cpp_registry_rows(array_text: str) -> list[str]:
    rows: list[str] = []
    current: list[str] = []
    depth = 0
    in_string = False
    escaped = False
    for ch in array_text:
        if in_string:
            current.append(ch)
            if escaped:
                escaped = False
            elif ch == "\\":
                escaped = True
            elif ch == '"':
                in_string = False
            continue
        if ch == '"':
            in_string = True
            current.append(ch)
            continue
        if ch == "{":
            depth += 1
            if depth == 1:
                current = []
            else:
                current.append(ch)
            continue
        if ch == "}":
            if depth == 1:
                rows.append("".join(current))
                current = []
                depth = 0
            else:
                current.append(ch)
                depth -= 1
            continue
        if depth >= 1:
            current.append(ch)
    return rows


def parse_registry_cpp(registry_cpp: Path, registry_hpp: Path) -> tuple[list[dict[str, str]], int]:
    hpp_text = registry_hpp.read_text(encoding="utf-8")
    match = re.search(r"kGeneratedSurfaceRegistryRowCount\s*=\s*(\d+)", hpp_text)
    if not match:
        fail(f"registry row-count constant missing: {registry_hpp}")
    expected = int(match.group(1))

    cpp_text = registry_cpp.read_text(encoding="utf-8")
    start = cpp_text.index("kRows{{") + len("kRows{{")
    end = cpp_text.index("}};", start)
    row_texts = split_cpp_registry_rows(cpp_text[start:end])
    rows: list[dict[str, str]] = []
    for row_text in row_texts:
        values = parse_cpp_registry_row(row_text)
        if len(values) != len(REGISTRY_FIELDS):
            fail(f"registry row field count mismatch: {len(values)}")
        rows.append(dict(zip(REGISTRY_FIELDS, values)))
    if len(rows) != expected:
        fail(f"registry row count mismatch: parsed={len(rows)} expected={expected}")
    ids = [row["surface_id"] for row in rows]
    if len(ids) != len(set(ids)):
        fail("duplicate surface_id in generated registry")
    return sorted(rows, key=lambda row: row["surface_id"]), expected


def read_key_value_manifest(path: Path) -> dict[str, str]:
    result: dict[str, str] = {}
    for line in path.read_text(encoding="utf-8").splitlines():
        if not line.strip() or "=" not in line:
            continue
        key, value = line.split("=", 1)
        result[key.strip()] = value.strip()
    return result


def read_csv_rows(path: Path) -> list[dict[str, str]]:
    with path.open(newline="", encoding="utf-8") as handle:
        return list(csv.DictReader(handle))


def load_release_maps(repo_root: Path) -> tuple[dict[str, dict[str, str]], dict[str, dict[str, str]]]:
    release_rows = read_csv_rows(repo_root / RELEASE_DECLARATION)
    ledger_rows = read_csv_rows(repo_root / STRICT_LEDGER)
    return (
        {row["surface_id"]: row for row in release_rows},
        {row["surface_id"]: row for row in ledger_rows},
    )


def build_system_object_registry() -> dict[str, Any]:
    entries: list[dict[str, Any]] = []
    for key, object_kind, object_class, full_name, plural, path, collision_group, context in SYSTEM_OBJECT_SOURCES:
        entries.append(
            {
                "case_policy": "lowercase" if full_name.lower() == full_name else "fixed",
                "canonical_full_name": full_name,
                "canonical_short_name": full_name if " " not in full_name else full_name.split()[-1],
                "collision_group": collision_group,
                "do_not_translate": False,
                "entry_id": stable_id("sys_object", key),
                "object_class": object_class,
                "object_kind": object_kind,
                "plural_full_name": plural,
                "source_authority": "ScratchBird generated system SQL object baseline",
                "source_key": key,
                "system_path": path,
                "translation_context": context,
            }
        )
    return {
        "schema_version": "sbsql.system_object_name_registry.v1",
        "source_authority": "ScratchBird SQL-visible system object baseline",
        "generated_by": GENERATOR_ID,
        "entries": sorted(entries, key=lambda row: row["entry_id"]),
    }


def build_dialect_baseline(
    registry_rows: list[dict[str, str]],
    release_by_id: dict[str, dict[str, str]],
    ledger_by_id: dict[str, dict[str, str]],
    registry_manifest: dict[str, str],
    registry_count: int,
) -> dict[str, Any]:
    surfaces: list[dict[str, Any]] = []
    for row in registry_rows:
        release = release_by_id.get(row["surface_id"], {})
        ledger = ledger_by_id.get(row["surface_id"], {})
        full_name = canonical_full_name(row["canonical_name"], row["surface_kind"])
        surfaces.append(
            {
                "batch_id": row["batch_id"],
                "canonical_example": release.get("fixture_refs", "").split(";", 1)[0],
                "canonical_full_name": full_name,
                "canonical_name": row["canonical_name"],
                "cluster_scope": row["cluster_scope"],
                "ctest_label": row["ctest_label"],
                "diagnostic_key": row["diagnostic_key"],
                "engine_rule_key": row["engine_rule_key"],
                "family": row["family"],
                "fixed_uuid_v7": row["fixed_uuid_v7"],
                "keyword_tokens": keyword_tokens(row["canonical_name"]),
                "lowering_handler_key": row["lowering_handler_key"],
                "nontranslatable_tokens": [
                    row["surface_id"],
                    row["fixed_uuid_v7"],
                    row["sblr_operation_family"],
                    row["parser_handler_key"],
                    row["lowering_handler_key"],
                    row["server_admission_key"],
                    row["engine_rule_key"],
                ],
                "parser_handler_key": row["parser_handler_key"],
                "release_status": release.get("release_status", "registry_only"),
                "sblr_operation_family": row["sblr_operation_family"],
                "server_admission_key": row["server_admission_key"],
                "source_status": row["source_status"],
                "support_state": "cluster_profile_gated" if row["cluster_scope"] == "cluster_private" else "release_supported",
                "surface_id": row["surface_id"],
                "surface_kind": row["surface_kind"],
                "translation_context": (
                    f"SBsql {row['surface_kind']} in the {row['family']} family. "
                    f"Server admission remains {row['server_admission_key']} and translation must not alter SBLR identity."
                ),
                "translation_units": [
                    {
                        "case_policy": "fixed" if full_name == full_name.upper() else "lowercase",
                        "grammar_role": "keyword" if full_name == full_name.upper() else "command phrase",
                        "source_text": full_name,
                        "unit_id": stable_id("translation_unit", row["surface_id"], full_name),
                    }
                ],
                "validation_fixture_id": row["validation_fixture_id"],
                "evidence_complete": ledger.get("evidence_complete", ""),
            }
        )
    return {
        "schema_version": "sbsql.dialect_baseline.v1",
        "dialect_profile_uuid": DIALECT_PROFILE_UUID,
        "generated_by": GENERATOR_ID,
        "registry": {
            "cpp_row_count": registry_count,
            "manifest_surface_count": int(registry_manifest.get("surface_count", "0")),
            "native_now": int(registry_manifest.get("native_now", "0")),
            "native_future": int(registry_manifest.get("native_future", "0")),
            "cluster_private": int(registry_manifest.get("cluster_private", "0")),
            "source_files": [REGISTRY_HPP, REGISTRY_CPP, REGISTRY_MANIFEST],
        },
        "surfaces": surfaces,
    }


def build_source_corpus(system_registry: dict[str, Any], dialect: dict[str, Any]) -> list[dict[str, Any]]:
    rows: list[dict[str, Any]] = []
    for entry in system_registry["entries"]:
        rows.append(
            {
                "case_policy": entry["case_policy"],
                "do_not_translate": entry["do_not_translate"],
                "example_source": entry["system_path"],
                "fallback_policy": "canonical_english_required_when_missing",
                "grammar_role": "noun",
                "record_id": stable_id("corpus", "system_object", entry["entry_id"]),
                "review_required": False,
                "source_family": "system_object",
                "source_id": entry["entry_id"],
                "source_text": entry["canonical_full_name"],
                "source_text_full": entry["canonical_full_name"],
                "translation_context": entry["translation_context"],
            }
        )
    for surface in dialect["surfaces"]:
        text = surface["canonical_full_name"]
        rows.append(
            {
                "case_policy": "fixed" if text == text.upper() else "lowercase",
                "do_not_translate": False,
                "example_source": surface.get("canonical_example", ""),
                "fallback_policy": "canonical_english_required_when_missing",
                "grammar_role": "keyword" if text == text.upper() else "command phrase",
                "record_id": stable_id("corpus", "sbsql_surface", surface["surface_id"]),
                "review_required": False,
                "source_family": "sbsql_surface",
                "source_id": surface["surface_id"],
                "source_text": text,
                "source_text_full": text,
                "translation_context": surface["translation_context"],
            }
        )
    for code, text in (
        ("SBSQL.LANG_RESOURCE.FALLBACK_TO_CANONICAL_ENGLISH", "Fell back to canonical English SBsql because the preferred language profile did not match."),
        ("SBSQL.LANG_RESOURCE.FAIL_CLOSED_ON_PROFILE_MISMATCH", "Language profile mismatch; operation failed closed."),
        ("SBSQL.LANG_RESOURCE.RENDERER_LOSSINESS_CLASSIFIED", "Renderer lossiness was classified before output."),
        ("SBSQL.LANG_RESOURCE.RENDERER_NOT_RENDERABLE", "The requested SBLR stream is not renderable in the preferred language."),
    ):
        rows.append(
            {
                "case_policy": "sentence",
                "do_not_translate": False,
                "example_source": code,
                "fallback_policy": "canonical_english_required_when_missing",
                "grammar_role": "diagnostic",
                "record_id": stable_id("corpus", "diagnostic", code),
                "review_required": False,
                "source_family": "diagnostic",
                "source_id": code,
                "source_text": text,
                "source_text_full": text,
                "translation_context": "Diagnostic message template. Preserve placeholders and do not disclose hidden object names.",
            }
        )
    return sorted(rows, key=lambda row: row["record_id"])


def apply_phrase_topology(
    source_words: list[str],
    translated_words: list[str],
    exact_tag: str,
) -> tuple[list[str], str | None]:
    if exact_tag not in POSTNOMINAL_MODIFIER_PROFILES:
        return translated_words, None
    if len(source_words) < 2 or len(source_words) != len(translated_words):
        return translated_words, None
    if source_words[-1] not in NOMINAL_HEAD_TOKENS:
        return translated_words, None
    if any(word in NON_NOMINAL_PREHEAD_TOKENS for word in source_words[:-1]):
        return translated_words, None
    return [translated_words[-1], *translated_words[:-1]], "head_initial_nominal_phrase"


def build_topology_profile(profile: dict[str, str]) -> dict[str, Any]:
    exact_tag = profile["exact_tag"]
    ud_language = PROFILE_TO_UD_LANGUAGE[exact_tag]
    metrics = UD_TOPOLOGY_METRICS[ud_language]
    source = UD_PUD_TOPOLOGY_SOURCES[ud_language]
    nominal_order = STRUCTURAL_PHRASE_TEMPLATES[2]["slot_order_by_profile"][exact_tag]
    return {
        "adposition_order": metrics["adposition_order"],
        "clause_slot_policy": "ud_topology_and_context_slot_binding",
        "dependency_direction_ratios": metrics["dependency_direction_ratios"],
        "determiner_order": metrics["determiner_order"],
        "dominant_predicate_argument_order": metrics["dominant_predicate_argument_order"],
        "english_fallback_when_stream_not_preferred_language": True,
        "exact_tag": exact_tag,
        "locale_topology_profile_uuid": stable_id("locale_topology", exact_tag, ud_language),
        "nominal_adjective_order": metrics["nominal_adjective_order"],
        "nominal_phrase_slot_order": nominal_order,
        "profile_uuid": profile["profile_uuid"],
        "sbsql_clause_slot_order": STRUCTURAL_PHRASE_TEMPLATES[0]["slot_order_by_profile"][exact_tag],
        "topology_profile_uuid": TOPOLOGY_PROFILE_UUID,
        "topology_source": {
            "derived_metrics_only": True,
            "license": source["license"],
            "raw_treebank_material_included": False,
            "sentences": source["sentences"],
            "source_url": source["source_url"],
            "tokens": source["tokens"],
            "treebank": source["treebank"],
            "treebank_file": source["treebank_file"],
            "ud_language": ud_language,
        },
        "uuid_resolution_stage": "after_canonical_stream_normalization",
    }


def translate_text(row: dict[str, Any], exact_tag: str) -> tuple[str, str, list[str], str | None]:
    text = row["source_text"]
    if exact_tag in SOURCE_AUTHORITY_REVIEWED_PROFILES:
        return text, "canonical_source", [], None
    if row["source_family"] == "diagnostic":
        translated = DIAGNOSTIC_TRANSLATIONS[exact_tag][row["source_id"]]
        return translated, "localized_deterministic", [], None
    if row.get("do_not_translate") is True:
        return text, "technical_source_preserved", split_words(text), None

    glossary = SQL_GLOSSARY[exact_tag]
    words = split_words(text)
    if not words:
        return text, "technical_source_preserved", [], None

    translated_words: list[str] = []
    preserved_tokens: list[str] = []
    translated_count = 0
    for word in words:
        if word in glossary:
            translated_words.append(glossary[word])
            translated_count += 1
            continue
        if word in INVARIANT_TOKENS or any(ch.isdigit() for ch in word):
            translated_words.append(word)
            preserved_tokens.append(word)
            continue
        translated_words.append(word)
        preserved_tokens.append(word)

    topology_transform = None
    if row.get("topology_transform_allowed", True) is not False:
        translated_words, topology_transform = apply_phrase_topology(words, translated_words, exact_tag)
    translated = " ".join(translated_words)
    if translated_count == 0:
        return translated, "technical_source_preserved", sorted(set(preserved_tokens)), topology_transform
    if preserved_tokens:
        return translated, "localized_with_technical_source_preserved", sorted(set(preserved_tokens)), topology_transform
    return translated, "localized_deterministic", [], topology_transform


def translate_message_template(template: str, exact_tag: str) -> dict[str, Any]:
    phrase_translations = MESSAGE_PHRASE_TRANSLATIONS.get(template, {})
    if exact_tag in SOURCE_AUTHORITY_REVIEWED_PROFILES:
        return {
            "exact_tag": exact_tag,
            "localized_template": template,
            "translation_status": "canonical_source",
        }
    if exact_tag in phrase_translations:
        return {
            "exact_tag": exact_tag,
            "localized_template": phrase_translations[exact_tag],
            "translation_status": "localized_phrase_seed",
        }
    translated, status, preserved_tokens, topology_transform = translate_text(
        {
            "case_policy": "sentence",
            "do_not_translate": False,
            "source_family": "database_message",
            "source_id": stable_id("message_source", template),
            "source_text": template,
            "topology_transform_allowed": False,
        },
        exact_tag,
    )
    payload: dict[str, Any] = {
        "exact_tag": exact_tag,
        "localized_template": translated,
        "translation_status": status,
    }
    if preserved_tokens:
        payload["source_tokens_preserved_by_policy"] = preserved_tokens
    if topology_transform:
        payload["topology_transform"] = topology_transform
    return payload


def build_database_message_catalog(repo_root: Path) -> dict[str, Any]:
    matrix_rows = load_public_diagnostic_matrix_rows(repo_root)
    message_rows: list[dict[str, Any]] = []
    seen_codes: set[str] = set()
    for row in matrix_rows:
        template = message_key_to_template(row["message_key"])
        seen_codes.add(row["code"])
        message_rows.append(
            {
                "message_id": stable_id("database_message", row["code"], row["message_key"]),
                "source_family": "public_diagnostic_matrix",
                "diagnostic_id": row["diagnostic_id"],
                "area": row["area"],
                "diagnostic_code": row["code"],
                "message_key": row["message_key"],
                "severity": diagnostic_severity(row),
                "redaction_class": row["redaction_class"],
                "compatibility_status": row["compatibility_status"],
                "canonical_template": template,
                "source_path": row["source_path"],
                "public_test_path": row["public_test_path"],
                "translations": [
                    translate_message_template(template, profile["exact_tag"])
                    for profile in LANGUAGE_PROFILES
                ],
            }
        )
    for row in extract_sblr_envelope_diagnostics(repo_root):
        if row["code"] in seen_codes:
            continue
        template = message_key_to_template(row["message_key"])
        message_rows.append(
            {
                "message_id": stable_id("database_message", row["code"], row["message_key"]),
                "source_family": "sblr_envelope_codec",
                "diagnostic_id": row["diagnostic_id"],
                "area": row["area"],
                "diagnostic_code": row["code"],
                "message_key": row["message_key"],
                "severity": "error",
                "redaction_class": row["redaction_class"],
                "compatibility_status": row["compatibility_status"],
                "canonical_template": template,
                "source_path": row["source_path"],
                "public_test_path": row["public_test_path"],
                "translations": [
                    translate_message_template(template, profile["exact_tag"])
                    for profile in LANGUAGE_PROFILES
                ],
            }
        )
    message_rows.sort(key=lambda row: (row["area"], row["diagnostic_code"]))
    return {
        "schema_version": "sbsql.database_message_catalog.v1",
        "generated_by": GENERATOR_ID,
        "source_files": [PUBLIC_DIAGNOSTIC_MATRIX_GENERATOR, SBLR_ENVELOPE_HPP],
        "profile_count": len(LANGUAGE_PROFILES),
        "profiles": [profile["exact_tag"] for profile in LANGUAGE_PROFILES],
        "message_count": len(message_rows),
        "translation_policy": {
            "canonical_english_profiles": sorted(SOURCE_AUTHORITY_REVIEWED_PROFILES),
            "non_english_profiles": [
                profile["exact_tag"]
                for profile in LANGUAGE_PROFILES
                if profile["exact_tag"] not in SOURCE_AUTHORITY_REVIEWED_PROFILES
            ],
            "no_english_fallback_rows_allowed": True,
            "sentence_topology_reordering_allowed": False,
            "placeholders_preserved_by_policy": True,
        },
        "messages": message_rows,
    }


def build_online_translation_verification_corpus() -> dict[str, Any]:
    cases: list[dict[str, Any]] = []
    for index, phrase in enumerate(ONLINE_TRANSLATION_VERIFICATION_PHRASES, start=1):
        translations = [
            translate_message_template(phrase, profile["exact_tag"])
            for profile in LANGUAGE_PROFILES
        ]
        cases.append(
            {
                "case_id": f"SBSQL-ONLINE-TRANS-{index:03d}",
                "source_language": "en",
                "source_text": phrase,
                "provider_query_text": phrase,
                "verification_policy": "compare_external_online_translation_with_scratchbird_seed_translation",
                "network_required": True,
                "external_reference_text_not_vendored": True,
                "translations": translations,
            }
        )
    return {
        "schema_version": "sbsql.online_translation_verification_corpus.v1",
        "generated_by": GENERATOR_ID,
        "profiles": [profile["exact_tag"] for profile in LANGUAGE_PROFILES],
        "provider_policy": {
            "release_pack_generation_uses_network": False,
            "online_reference_checks_are_optional_external_verification": True,
            "raw_online_responses_not_required_in_public_repo": True,
            "supported_language_pairs": {
                "fr-FR": "en|fr",
                "fr-CA": "en|fr",
                "de-DE": "en|de",
                "it-IT": "en|it",
                "es-ES": "en|es",
            },
        },
        "case_count": len(cases),
        "cases": cases,
    }


def build_language_profile(profile: dict[str, str], corpus_rows: list[dict[str, Any]]) -> dict[str, Any]:
    translations: list[dict[str, Any]] = []
    status_counts: dict[str, int] = {}
    preserved_token_counts: dict[str, int] = {}
    topology_transform_counts: dict[str, int] = {}
    for row in corpus_rows:
        translated, status, preserved_tokens, topology_transform = translate_text(row, profile["exact_tag"])
        status_counts[status] = status_counts.get(status, 0) + 1
        for token in preserved_tokens:
            preserved_token_counts[token] = preserved_token_counts.get(token, 0) + 1
        if topology_transform:
            topology_transform_counts[topology_transform] = topology_transform_counts.get(topology_transform, 0) + 1
        translations.append(
            {
                "case_policy": row["case_policy"],
                "fallback_policy": row["fallback_policy"],
                "localized_text": translated,
                "record_id": row["record_id"],
                "review_required": profile["exact_tag"] not in SOURCE_AUTHORITY_REVIEWED_PROFILES,
                "source_family": row["source_family"],
                "source_id": row["source_id"],
                "source_text": row["source_text"],
                "translation_status": status,
            }
        )
        if preserved_tokens:
            translations[-1]["source_tokens_preserved_by_policy"] = preserved_tokens
        if topology_transform:
            translations[-1]["topology_transform"] = topology_transform
    return {
        "schema_version": "sbsql.language_profile.v1",
        "display_name": profile["display_name"],
        "exact_tag": profile["exact_tag"],
        "fallback_parent_uuid": profile["fallback_parent_uuid"],
        "language_resource_authority": {
            "canonical_english_fallback_profile_uuid": "sbsql.language.en-US.canonical-recovery.v1",
            "local_sblr_is_untrusted_until_server_revalidation": True,
            "server_revalidates_sblr_uuid_descriptor_policy_and_mga": True,
        },
        "native_review_state": profile["native_review_state"],
        "profile_uuid": profile["profile_uuid"],
        "release_channel": profile["release_channel"],
        "support_state": profile["support_state"],
        "fallback_translation_count": status_counts.get("english_fallback_machine_seed", 0),
        "source_tokens_preserved_by_policy_count": sum(preserved_token_counts.values()),
        "source_tokens_preserved_by_policy_top": [
            {"token": token, "count": count}
            for token, count in sorted(preserved_token_counts.items(), key=lambda item: (-item[1], item[0]))[:50]
        ],
        "translation_count": len(translations),
        "translation_status_counts": dict(sorted(status_counts.items())),
        "translation_source": profile["translation_source"],
        "topology_transform_counts": dict(sorted(topology_transform_counts.items())),
        "translations": translations,
    }


def schema(name: str, required: list[str]) -> dict[str, Any]:
    return {
        "$schema": "https://json-schema.org/draft/2020-12/schema",
        "title": name,
        "type": "object",
        "required": required,
        "additionalProperties": True,
    }


def build_auxiliary_resources(
    repo_root: Path,
    registry_rows: list[dict[str, str]],
    dialect: dict[str, Any],
    corpus_rows: list[dict[str, Any]],
    language_profiles: list[dict[str, Any]],
) -> dict[str, Any]:
    surface_ids = [row["surface_id"] for row in registry_rows]
    topology = {
        "schema_version": "sbsql.topology_profiles.v1",
        "topology_profile_uuid": TOPOLOGY_PROFILE_UUID,
        "framework": {
            "name": "Universal Dependencies",
            "guidelines": "UD v2 dependency relations between content words with function words attached to content heads",
            "derived_metrics_only": True,
            "raw_treebank_material_included": False,
        },
        "normalization_stage": "stream_analysis_before_uuid_resolution",
        "canonical_order": ["action", "projection", "source", "condition", "modifier"],
        "structural_templates": STRUCTURAL_PHRASE_TEMPLATES,
        "profiles": [build_topology_profile(profile) for profile in LANGUAGE_PROFILES],
    }
    phrases = {
        "schema_version": "sbsql.phrase_table.v1",
        "phrase_count": len(dialect["surfaces"]),
        "structural_template_count": len(STRUCTURAL_PHRASE_TEMPLATES),
        "structural_templates": STRUCTURAL_PHRASE_TEMPLATES,
        "phrases": [
            {
                "phrase_id": stable_id("phrase", surface["surface_id"], surface["canonical_name"]),
                "surface_id": surface["surface_id"],
                "canonical_text": surface["canonical_full_name"],
                "slot_id": stable_id("slot", surface["surface_id"], surface["family"]),
                "translation_unit_ids": [unit["unit_id"] for unit in surface["translation_units"]],
            }
            for surface in dialect["surfaces"]
        ],
    }
    predictive = {
        "schema_version": "sbsql.predictive_grammar.v1",
        "max_table_entries": len(surface_ids),
        "no_database_object_names": True,
        "no_uuid_values": True,
        "server_revalidation_required": True,
        "states": [
            {
                "state_id": stable_id("predictive", row["surface_id"], row["canonical_name"]),
                "surface_id": row["surface_id"],
                "family": row["family"],
                "completion_class": "cluster_profile_gated" if row["cluster_scope"] == "cluster_private" else "grammar_completion",
            }
            for row in registry_rows
        ],
    }
    diagnostics = {
        "schema_version": "sbsql.diagnostic_messages.v1",
        "redaction_policy": "no_query_text_no_hidden_identifier_no_local_path",
        "messages": [
            {
                "diagnostic_code": row["source_id"],
                "message_id": stable_id("message", row["source_id"]),
                "redaction_class": "public_no_hidden_object_disclosure",
                "severity": "warning" if "FALLBACK" in row["source_id"] else "error",
                "template": row["source_text"],
            }
            for row in corpus_rows
            if row["source_family"] == "diagnostic"
        ],
    }
    database_messages = build_database_message_catalog(repo_root)
    online_translation_verification = build_online_translation_verification_corpus()
    rendering = {
        "schema_version": "sbsql.rendering_templates.v1",
        "renderer_lossiness_classes": [
            "lossless_canonical",
            "canonical_equivalent",
            "preferred_language_partial",
            "canonical_english_fallback",
            "not_renderable",
        ],
        "source_reconstruction_forbidden": True,
        "templates": [
            {
                "exact_tag": profile["exact_tag"],
                "profile_uuid": profile["profile_uuid"],
                "renderer_id": stable_id("renderer", profile["profile_uuid"]),
                "fallback_profile_uuid": "sbsql.language.en-US.canonical-recovery.v1",
                "round_trip_target": "canonical_element_stream",
                "server_revalidation_required": True,
            }
            for profile in LANGUAGE_PROFILES
        ],
    }
    unicode = {
        "schema_version": "sbsql.unicode_policy.v1",
        "normalization": "preserve_source_bytes_canonicalize_before_uuid_resolution_only_when_profile_allows",
        "unsafe_cases_fail_closed": [
            "malformed_utf8",
            "orphan_combining_mark",
            "bidi_control",
            "mixed_script_confusable",
            "mirrored_punctuation_ambiguity",
        ],
        "hidden_object_disclosure_allowed": False,
    }
    resolver = {
        "schema_version": "sbsql.resolver_policy.v1",
        "authorized_schema_path_resolution_required": True,
        "authorized_uuid_to_path_required": True,
        "hidden_or_missing_objects": "no_disclosure_diagnostic",
        "resource_epoch_sensitive": True,
        "server_authority": "server_filters_schema_object_path_and_uuid_resolution_by_auth_policy",
    }
    conformance = {
        "schema_version": "sbsql.language_resource_conformance_corpus.v1",
        "profiles": [profile["exact_tag"] for profile in LANGUAGE_PROFILES],
        "coverage": {
            "registry_surface_count": len(surface_ids),
            "translation_source_rows": len(corpus_rows),
            "language_profile_count": len(language_profiles),
        },
        "cases": [
            {"case_id": "LRP-CONF-001", "class": "exact_profile_admission", "expected": "all_exact_tags_admitted"},
            {"case_id": "LRP-CONF-002", "class": "english_fallback", "expected": "preferred_profile_fails_then_canonical_english"},
            {"case_id": "LRP-CONF-003", "class": "renderer_round_trip", "expected": "canonical_element_stream_unchanged"},
            {"case_id": "LRP-CONF-004", "class": "predictive_privacy", "expected": "no_uuid_or_hidden_object_names"},
            {"case_id": "LRP-CONF-005", "class": "corruption_oracle", "expected": "missing_hash_signature_duplicate_or_extra_file_refused"},
        ],
    }
    provenance = {
        "schema_version": "sbsql.language_resource_provenance.v1",
        "generated_by": GENERATOR_ID,
        "license": "MPL-2.0",
        "third_party_source_material_included": False,
        "third_party_derived_topology_metadata": {
            "included": True,
            "source_family": "Universal Dependencies PUD",
            "raw_treebank_material_included": False,
            "sources": UD_PUD_TOPOLOGY_SOURCES,
        },
        "native_review": [
            {
                "exact_tag": profile["exact_tag"],
                "profile_uuid": profile["profile_uuid"],
                "native_review_state": profile["native_review_state"],
                "release_channel": profile["release_channel"],
            }
            for profile in LANGUAGE_PROFILES
        ],
        "source_files": [
            REGISTRY_HPP,
            REGISTRY_CPP,
            REGISTRY_MANIFEST,
            RELEASE_DECLARATION,
            STRICT_LEDGER,
            PUBLIC_DIAGNOSTIC_MATRIX_GENERATOR,
            SBLR_ENVELOPE_HPP,
        ],
    }
    dialect_profile = {
        "schema_version": "sbsql.dialect_profile.v1",
        "dialect_profile_uuid": DIALECT_PROFILE_UUID,
        "baseline_path": "resources/canonical/sbsql-dialect-baseline.json",
        "surface_count": len(surface_ids),
        "canonical_english_fallback": "en-US",
    }
    return {
        "topology/topology-profiles.json": topology,
        "dialects/sbsql-v3-dialect-profile.json": dialect_profile,
        "phrases/phrase-table.json": phrases,
        "predictive/predictive-grammar.json": predictive,
        "diagnostics/diagnostic-messages.json": diagnostics,
        "diagnostics/database-message-catalog.json": database_messages,
        "rendering/rendering-templates.json": rendering,
        "unicode/unicode-policy.json": unicode,
        "resolver/resolver-policy.json": resolver,
        "conformance/conformance-corpus.json": conformance,
        "conformance/online-translation-verification-corpus.json": online_translation_verification,
        "provenance/provenance.json": provenance,
        "provenance/native-review-status.json": {"schema_version": "sbsql.native_review_status.v1", "profiles": provenance["native_review"]},
    }


def build_files(repo_root: Path) -> tuple[list[GeneratedFile], dict[str, Any], dict[str, Any]]:
    registry_rows, registry_count = parse_registry_cpp(repo_root / REGISTRY_CPP, repo_root / REGISTRY_HPP)
    registry_manifest = read_key_value_manifest(repo_root / REGISTRY_MANIFEST)
    release_by_id, ledger_by_id = load_release_maps(repo_root)
    system_registry = build_system_object_registry()
    dialect = build_dialect_baseline(registry_rows, release_by_id, ledger_by_id, registry_manifest, registry_count)
    corpus_rows = build_source_corpus(system_registry, dialect)
    language_profiles = [build_language_profile(profile, corpus_rows) for profile in LANGUAGE_PROFILES]
    aux = build_auxiliary_resources(repo_root, registry_rows, dialect, corpus_rows, language_profiles)

    files: list[GeneratedFile] = [
        GeneratedFile("resources/canonical/system-object-name-registry.schema.json", canonical_json_bytes(schema("SBsql system object name registry", ["schema_version", "entries"]))),
        GeneratedFile("resources/canonical/system-object-name-registry.json", canonical_json_bytes(system_registry)),
        GeneratedFile("resources/canonical/sbsql-dialect-baseline.schema.json", canonical_json_bytes(schema("SBsql dialect baseline", ["schema_version", "registry", "surfaces"]))),
        GeneratedFile("resources/canonical/sbsql-dialect-baseline.json", canonical_json_bytes(dialect)),
        GeneratedFile("resources/canonical/translation-source-corpus.schema.json", canonical_json_bytes(schema("SBsql translation source corpus row", ["record_id", "source_family", "source_id", "source_text"]))),
        GeneratedFile("resources/canonical/translation-source-corpus.jsonl", canonical_jsonl_bytes(corpus_rows)),
        GeneratedFile(
            "resources/canonical/translation-style-guide.en-US.json",
            canonical_json_bytes(
                {
                    "schema_version": "sbsql.translation_style_guide.v1",
                    "exact_tag": "en-US",
                    "rules": [
                        "Preserve UUIDs, SBLR operation families, protocol identifiers, placeholders, and diagnostic codes.",
                        "Translate object-kind nouns separately from command verbs.",
                        "Keep user schema distinct from user.",
                        "Record uncertainty as native_review_required before release support.",
                        "Do not translate hidden object names or authorization-filtered schema paths.",
                    ],
                }
            ),
        ),
        GeneratedFile("resources/languages/language-profile.schema.json", canonical_json_bytes(schema("SBsql language profile", ["schema_version", "exact_tag", "profile_uuid", "translations"]))),
    ]
    for profile in language_profiles:
        files.append(GeneratedFile(f"resources/languages/{profile['exact_tag']}/language-profile.json", canonical_json_bytes(profile)))
    schema_files = {
        "topology/topology-profile.schema.json": "SBsql topology profile",
        "dialects/dialect-resource.schema.json": "SBsql dialect profile",
        "phrases/phrase-resource.schema.json": "SBsql phrase table",
        "predictive/predictive-resource.schema.json": "SBsql predictive resource",
        "diagnostics/diagnostic-resource.schema.json": "SBsql diagnostic messages",
        "diagnostics/database-message-catalog.schema.json": "SBsql database message catalog",
        "rendering/rendering-resource.schema.json": "SBsql rendering templates",
        "unicode/unicode-policy.schema.json": "SBsql unicode policy",
        "resolver/resolver-resource.schema.json": "SBsql resolver policy",
        "conformance/conformance-corpus.schema.json": "SBsql conformance corpus",
        "conformance/online-translation-verification-corpus.schema.json": "SBsql online translation verification corpus",
        "provenance/provenance.schema.json": "SBsql provenance",
    }
    for rel_path, title in schema_files.items():
        files.append(GeneratedFile(f"resources/{rel_path}", canonical_json_bytes(schema(title, ["schema_version"]))))
    for rel_path, payload in aux.items():
        files.append(GeneratedFile(f"resources/{rel_path}", canonical_json_bytes(payload)))

    resource_hashes = {item.rel_path: sha256_bytes(item.data) for item in files}
    common_resource_hash = sha256_bytes(
        canonical_json_bytes(
            {
                "dialect": resource_hashes["resources/canonical/sbsql-dialect-baseline.json"],
                "system_objects": resource_hashes["resources/canonical/system-object-name-registry.json"],
                "corpus": resource_hashes["resources/canonical/translation-source-corpus.jsonl"],
                "profiles": {
                    profile["exact_tag"]: resource_hashes[f"resources/languages/{profile['exact_tag']}/language-profile.json"]
                    for profile in language_profiles
                },
            }
        )
    )
    manifest = {
        "schema_version": MANIFEST_SCHEMA_VERSION,
        "pack_schema_version": PACK_SCHEMA_VERSION,
        "resource_identity": RESOURCE_IDENTITY,
        "common_resource_hash": common_resource_hash,
        "dialect_profile_uuid": DIALECT_PROFILE_UUID,
        "topology_profile_uuid": TOPOLOGY_PROFILE_UUID,
        "generated_by": GENERATOR_ID,
        "generated_from": [
            REGISTRY_HPP,
            REGISTRY_CPP,
            REGISTRY_MANIFEST,
            RELEASE_DECLARATION,
            STRICT_LEDGER,
            PUBLIC_DIAGNOSTIC_MATRIX_GENERATOR,
            SBLR_ENVELOPE_HPP,
        ],
        "registry_row_count": registry_count,
        "translation_source_row_count": len(corpus_rows),
        "profiles": [
            {
                "exact_tag": profile["exact_tag"],
                "profile_uuid": profile["profile_uuid"],
                "release_channel": profile["release_channel"],
                "support_state": profile["support_state"],
                "native_review_state": profile["native_review_state"],
                "resource_path": f"resources/languages/{profile['exact_tag']}/language-profile.json",
            }
            for profile in LANGUAGE_PROFILES
        ],
        "authority": {
            "local_sblr_uuid_streams_are_untrusted": True,
            "server_revalidates_sblr_uuid_descriptor_authorization_policy_and_mga": True,
            "normalization_before_uuid_resolution": True,
            "canonical_english_fallback_exact_tag": "en-US",
        },
        "files": [
            {"path": item.rel_path, "sha256": resource_hashes[item.rel_path], "size_bytes": len(item.data)}
            for item in sorted(files, key=lambda item: item.rel_path)
        ],
    }
    manifest_bytes = canonical_json_bytes(manifest)
    files.append(GeneratedFile("manifest.sblrp.json", manifest_bytes))
    hash_lines = [
        f"{sha256_bytes(item.data)}  {item.rel_path}\n"
        for item in sorted(files, key=lambda item: item.rel_path)
    ]
    hashes_data = "".join(hash_lines).encode("utf-8")
    files.append(GeneratedFile("hashes.sha256", hashes_data))
    signature_payload = {
        "schema_version": SIGNATURE_SCHEMA_VERSION,
        "algorithm": "scratchbird-public-source-review-sha256-transcript-v1",
        "key_id": PUBLIC_SIGNING_KEY_ID,
        "signature_id": stable_id("signature", sha256_bytes(manifest_bytes), sha256_bytes(hashes_data)),
        "support_scope": "source_review_pack_integrity_not_external_code_signing",
        "signed_hashes_sha256": sha256_bytes(hashes_data),
        "signed_manifest_sha256": sha256_bytes(manifest_bytes),
        "transcript_sha256": sha256_bytes(canonical_json_bytes({"hashes": sha256_bytes(hashes_data), "manifest": sha256_bytes(manifest_bytes), "key_id": PUBLIC_SIGNING_KEY_ID})),
    }
    files.append(GeneratedFile("manifest.sblrp.sig", canonical_json_bytes(signature_payload)))
    return sorted(files, key=lambda item: item.rel_path), manifest, {"system": system_registry, "dialect": dialect, "corpus_rows": corpus_rows}


def ensure_safe_pack_root(pack_root: Path) -> None:
    if pack_root.name != "sbsql-language-resource-pack":
        fail(f"refusing to replace unexpected pack root: {pack_root}")


def write_pack(pack_root: Path, files: list[GeneratedFile]) -> None:
    ensure_safe_pack_root(pack_root)
    if pack_root.exists():
        shutil.rmtree(pack_root)
    for item in files:
        path = pack_root / item.rel_path
        path.parent.mkdir(parents=True, exist_ok=True)
        path.write_bytes(item.data)


def update_seed_manifest(seed_pack_root: Path) -> None:
    manifest_path = seed_pack_root / "RESOURCE_SEED_MANIFEST.csv"
    rows = read_csv_rows(manifest_path)
    by_family = {row["seed_family"]: row for row in rows}
    additions = [
        {
            "seed_family": "sbsql_language_resource_pack",
            "source_pattern": "resources/i18n/sbsql-language-resource-pack/manifest.sblrp.json;resources/i18n/sbsql-language-resource-pack/manifest.sblrp.sig;resources/i18n/sbsql-language-resource-pack/hashes.sha256",
            "required_catalog_rows": "ResourceBundleRecord;LanguageResourceProfileRecord;LanguageResourceArtifactRecord",
            "create_time_action": "validate_signature_hashes_and_load_exact_profiles",
            "status": "specified",
        },
        {
            "seed_family": "sbsql_language_resource_pack_artifacts",
            "source_pattern": "resources/i18n/sbsql-language-resource-pack/resources/**/*.json;resources/i18n/sbsql-language-resource-pack/resources/**/*.jsonl",
            "required_catalog_rows": "LanguageResourceArtifactRecord;LanguageResourceTranslationRecord;LanguageResourcePredictiveRecord",
            "create_time_action": "validate_pack_internal_hashes_and_attach_to_language_profiles",
            "status": "specified",
        },
        {
            "seed_family": "sbsql_language_resource_pack_provenance",
            "source_pattern": "resources/i18n/sbsql-language-resource-pack/resources/provenance/*.json",
            "required_catalog_rows": "ResourceProvenanceRecord;ResourceLicenseRecord;NativeReviewStatusRecord",
            "create_time_action": "retain_generated_resource_provenance",
            "status": "specified",
        },
    ]
    for row in additions:
        by_family[row["seed_family"]] = row
    ordered: list[dict[str, str]] = []
    seen: set[str] = set()
    for row in rows:
        family = row["seed_family"]
        ordered.append(by_family[family])
        seen.add(family)
    ordered.extend(by_family[family] for family in sorted(set(by_family) - seen))
    with manifest_path.open("w", newline="", encoding="utf-8") as handle:
        writer = csv.DictWriter(handle, fieldnames=["seed_family", "source_pattern", "required_catalog_rows", "create_time_action", "status"], lineterminator="\n")
        writer.writeheader()
        writer.writerows(ordered)


def update_seed_artifacts(seed_pack_root: Path) -> None:
    rows: list[dict[str, str]] = []
    resource_root = seed_pack_root / "resources"
    for path in sorted(resource_root.rglob("*")):
        if not path.is_file():
            continue
        rel = path.relative_to(seed_pack_root).as_posix()
        data = path.read_bytes()
        rows.append({"canonical_path": rel, "content_hash": fnv1a64_bytes(data), "content_size_bytes": str(len(data))})
    artifacts_path = seed_pack_root / "RESOURCE_SEED_ARTIFACTS.csv"
    with artifacts_path.open("w", newline="", encoding="utf-8") as handle:
        writer = csv.DictWriter(handle, fieldnames=["canonical_path", "content_hash", "content_size_bytes"], lineterminator="\n")
        writer.writeheader()
        writer.writerows(rows)


def update_i18n_version(seed_pack_root: Path) -> None:
    version_path = seed_pack_root / "resources/i18n/version"
    version_path.parent.mkdir(parents=True, exist_ok=True)
    version_path.write_text(I18N_VERSION + "\n", encoding="utf-8")


def driver_metadata_hash(metadata: dict[str, Any]) -> str:
    hashed = dict(metadata)
    hashed.pop("resource_hash", None)
    data = json.dumps(hashed, ensure_ascii=False, sort_keys=True, separators=(",", ":")).encode("utf-8")
    return sha256_bytes(data)


def update_driver_surface_manifest(repo_root: Path, manifest: dict[str, Any]) -> None:
    path = repo_root / DRIVER_SURFACE_MANIFEST
    payload = json.loads(path.read_text(encoding="utf-8"))
    metadata = payload["common_resource_pack_metadata"]
    metadata.update(
        {
            "resource_identity": RESOURCE_IDENTITY,
            "resource_pack_path": f"{DEFAULT_SEED_PACK}/{DEFAULT_PACK_REL}",
            "resource_pack_manifest_sha256": sha256_bytes(canonical_json_bytes(manifest)),
            "resource_pack_common_resource_hash": manifest["common_resource_hash"],
            "supported_exact_profiles": [profile["exact_tag"] for profile in manifest["profiles"]],
        }
    )
    metadata["resource_hash"] = driver_metadata_hash(metadata)
    path.write_text(json.dumps(payload, ensure_ascii=False, indent=2, sort_keys=True) + "\n", encoding="utf-8")


def main() -> int:
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument("--repo-root", type=Path, default=Path(__file__).resolve().parents[3])
    parser.add_argument("--seed-pack-root", type=Path)
    parser.add_argument("--pack-root", type=Path)
    parser.add_argument("--no-driver-manifest-update", action="store_true")
    args = parser.parse_args()

    repo_root = args.repo_root.resolve()
    seed_pack_root = (args.seed_pack_root if args.seed_pack_root else repo_root / DEFAULT_SEED_PACK).resolve()
    pack_root = (args.pack_root if args.pack_root else seed_pack_root / DEFAULT_PACK_REL).resolve()
    files, manifest, _ = build_files(repo_root)
    write_pack(pack_root, files)
    update_i18n_version(seed_pack_root)
    update_seed_manifest(seed_pack_root)
    update_seed_artifacts(seed_pack_root)
    if not args.no_driver_manifest_update and seed_pack_root == (repo_root / DEFAULT_SEED_PACK).resolve():
        update_driver_surface_manifest(repo_root, manifest)
    print(
        "sbsql_language_resource_pack=generated "
        f"files={len(files)} profiles={len(manifest['profiles'])} "
        f"registry_rows={manifest['registry_row_count']} "
        f"common_resource_hash={manifest['common_resource_hash']}"
    )
    return 0


if __name__ == "__main__":
    raise SystemExit(main())
