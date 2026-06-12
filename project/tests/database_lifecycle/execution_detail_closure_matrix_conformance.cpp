// Copyright (c) 2026 ScratchBird Software Inc.
//
// This Source Code Form is subject to the terms of the Mozilla Public
// License, v. 2.0. If a copy of the MPL was not distributed with this
// file, You can obtain one at https://mozilla.org/MPL/2.0/.
//
// SPDX-License-Identifier: MPL-2.0

#include "scratchbird/engine/value.hpp"

#include <cstdint>
#include <cstdlib>
#include <iostream>
#include <string>
#include <string_view>
#include <vector>

namespace {

namespace engine = scratchbird::engine;

[[noreturn]] void Fail(std::string_view message) {
  std::cerr << message << '\n';
  std::exit(EXIT_FAILURE);
}

void Require(bool condition, std::string_view message) {
  if (!condition) {
    Fail(message);
  }
}

engine::Uuid Uuid(std::uint16_t seed) {
  engine::Uuid uuid;
  uuid.bytes[0] = static_cast<std::uint8_t>(seed & 0xffu);
  uuid.bytes[1] = static_cast<std::uint8_t>((seed >> 8) & 0xffu);
  for (std::size_t index = 2; index < 16; ++index) {
    uuid.bytes[index] =
        static_cast<std::uint8_t>((seed + (index * 37u)) & 0xffu);
  }
  if (engine::ExecutionDataPacketUuidIsNil(uuid)) {
    uuid.bytes[0] = 1;
  }
  return uuid;
}

std::string KeyToken(std::string_view value) {
  std::string token;
  bool last_was_separator = false;
  for (const char ch : value) {
    const bool alnum = (ch >= 'a' && ch <= 'z') ||
                       (ch >= 'A' && ch <= 'Z') ||
                       (ch >= '0' && ch <= '9');
    if (alnum) {
      if (last_was_separator && !token.empty()) {
        token.push_back('_');
      }
      token.push_back(ch >= 'A' && ch <= 'Z'
                          ? static_cast<char>(ch - 'A' + 'a')
                          : ch);
      last_was_separator = false;
    } else {
      last_was_separator = true;
    }
  }
  if (token.empty()) {
    return "row";
  }
  return token;
}

struct MatrixDefinition {
  engine::ExecutionDetailClosureMatrixKind kind;
  std::string name;
  engine::ExecutionDetailClosureRowScope scope;
  std::vector<std::string> subjects;
};

std::vector<engine::ExecutionDetailClosureSurfaceKind> RequiredSurfaces(
    engine::ExecutionDetailClosureMatrixKind kind) {
  using Kind = engine::ExecutionDetailClosureSurfaceKind;
  switch (kind) {
    case engine::ExecutionDetailClosureMatrixKind::canonical_type_family:
      return {Kind::descriptor, Kind::domain, Kind::operation,
              Kind::storage_encoding, Kind::diagnostic, Kind::documentation};
    case engine::ExecutionDetailClosureMatrixKind::reference_type_coverage:
      return {Kind::descriptor, Kind::domain, Kind::literal_bind, Kind::cast,
              Kind::operation, Kind::diagnostic, Kind::conformance,
              Kind::documentation};
    case engine::ExecutionDetailClosureMatrixKind::operation_registry:
      return {Kind::operation, Kind::diagnostic, Kind::conformance,
              Kind::implementation_trace};
    case engine::ExecutionDetailClosureMatrixKind::cast_registry:
      return {Kind::cast, Kind::diagnostic, Kind::conformance,
              Kind::implementation_trace};
    case engine::ExecutionDetailClosureMatrixKind::aggregate_window:
      return {Kind::aggregate_window, Kind::storage_encoding, Kind::diagnostic,
              Kind::conformance, Kind::implementation_trace};
    case engine::ExecutionDetailClosureMatrixKind::index_family_compatibility:
      return {Kind::index, Kind::diagnostic, Kind::conformance,
              Kind::implementation_trace};
    case engine::ExecutionDetailClosureMatrixKind::statistics_selectivity:
      return {Kind::statistics, Kind::metrics, Kind::diagnostic,
              Kind::conformance, Kind::implementation_trace};
    case engine::ExecutionDetailClosureMatrixKind::driver_metadata:
      return {Kind::driver_metadata, Kind::wire_rendering, Kind::diagnostic,
              Kind::conformance, Kind::implementation_trace};
    case engine::ExecutionDetailClosureMatrixKind::
        backup_replication_transport:
      return {Kind::backup_restore, Kind::replication, Kind::cluster_transport,
              Kind::diagnostic, Kind::conformance, Kind::implementation_trace};
    case engine::ExecutionDetailClosureMatrixKind::system_catalog:
      return {Kind::catalog, Kind::security, Kind::diagnostic,
              Kind::conformance, Kind::implementation_trace};
    case engine::ExecutionDetailClosureMatrixKind::conformance_corpus:
      return {Kind::conformance, Kind::diagnostic, Kind::metrics,
              Kind::implementation_trace};
  }
  return {};
}

std::vector<MatrixDefinition> MatrixDefinitions() {
  using Kind = engine::ExecutionDetailClosureMatrixKind;
  using Scope = engine::ExecutionDetailClosureRowScope;
  return {
      {Kind::canonical_type_family,
       "canonical_type_family_inventory",
       Scope::native,
       {"Boolean",
        "Signed integer",
        "Unsigned integer",
        "Exact numeric",
        "Decimal floating",
        "Approximate numeric",
        "Money/currency",
        "Character text",
        "Binary string",
        "Bit string",
        "Temporal",
        "Interval/duration",
        "UUID/object identity",
        "Network address",
        "Enum",
        "Set",
        "Array/list",
        "Map/dictionary",
        "Row/composite",
        "Variant/tagged union",
        "Range/multirange",
        "Document",
        "Full-text/search",
        "Spatial",
        "Vector/similarity",
        "Graph",
        "Time-series",
        "Columnar/OLAP",
        "Aggregate state",
        "Approximate/sketch",
        "External locator",
        "System pseudo-type",
        "Opaque extension"}},
      {Kind::reference_type_coverage,
       "reference_type_coverage_matrix",
       Scope::reference,
       {"PostgreSQL boolean",
        "PostgreSQL smallint integer bigint",
        "PostgreSQL numeric decimal",
        "PostgreSQL real double precision",
        "PostgreSQL money",
        "PostgreSQL char varchar text",
        "PostgreSQL bytea",
        "PostgreSQL bit varbit",
        "PostgreSQL date time timetz timestamp timestamptz interval",
        "PostgreSQL uuid",
        "PostgreSQL inet cidr macaddr macaddr8",
        "PostgreSQL json jsonb xml hstore",
        "PostgreSQL arrays and composite types",
        "PostgreSQL enum range multirange",
        "PostgreSQL tsvector tsquery",
        "PostgreSQL geometric types",
        "PostgreSQL oid tid xid cid pg_lsn reg",
        "Firebird integer family",
        "Firebird decimal numeric decfloat",
        "Firebird float double precision",
        "Firebird char varchar",
        "Firebird blob subtypes",
        "Firebird date time timestamp time-zone variants",
        "Firebird boolean",
        "Firebird arrays",
        "Firebird domains",
        "Firebird DB_KEY blob IDs transaction metadata",
        "MySQL integer and unsigned families",
        "MySQL numeric and approximate numeric",
        "MySQL bit bool boolean",
        "MySQL date time datetime timestamp year",
        "MySQL text families",
        "MySQL binary and blob families",
        "MySQL enum set",
        "MySQL json",
        "MySQL geometry geography",
        "Oracle number decimal numeric",
        "Oracle binary_float binary_double",
        "Oracle character and LOB text",
        "Oracle raw long raw blob",
        "Oracle temporal timestamp variants",
        "Oracle interval families",
        "Oracle rowid urowid",
        "Oracle bfile",
        "Oracle xmltype JSON",
        "Oracle object varray nested table",
        "Oracle ref cursor anydata",
        "SQL Server integer families bit",
        "SQL Server numeric money float real",
        "SQL Server temporal datetimeoffset",
        "SQL Server character text families",
        "SQL Server binary varbinary image",
        "SQL Server uniqueidentifier rowversion timestamp",
        "SQL Server xml JSON hierarchyid geometry geography sql_variant",
        "DB2 numeric families",
        "DB2 character LOB graphic families",
        "DB2 binary blob families",
        "DB2 date time timestamp",
        "DB2 XML rowid distinct structured arrays",
        "SQLite storage classes",
        "SQLite affinity date time JSON rowid",
        "Cassandra scalar descriptors",
        "Cassandra temporal network counter descriptors",
        "Cassandra collection tuple UDT vector",
        "MongoDB scalar BSON values",
        "MongoDB opaque and unsupported legacy values",
        "MongoDB numeric timestamp ordering pseudo-values",
        "ClickHouse wide integer families",
        "ClickHouse numeric float decimal bool",
        "ClickHouse string UUID network",
        "ClickHouse temporal",
        "ClickHouse compound columnar wrappers",
        "ClickHouse aggregate variant dynamic JSON object",
        "DuckDB scalar integers bool",
        "DuckDB decimal float double",
        "DuckDB varchar blob bit",
        "DuckDB temporal interval",
        "DuckDB uuid enum list array map struct union JSON",
        "OpenSearch text keyword wildcard",
        "OpenSearch numeric boolean",
        "OpenSearch date ip binary",
        "OpenSearch object nested flattened",
        "OpenSearch geo vector rank completion percolator",
        "Redis collection values",
        "Redis bitmap hyperloglog geospatial vector search modules",
        "Neo4j scalar values",
        "Neo4j temporal spatial values",
        "Neo4j graph structures",
        "InfluxDB measurement tag field timestamp",
        "InfluxDB scalar fields and duration values",
        "Milvus scalar document container values",
        "Milvus vector values"}},
      {Kind::operation_registry,
       "operation_registry_seed_matrix",
       Scope::engine,
       {"Boolean",
        "Equality/comparison",
        "Numeric arithmetic",
        "Temporal arithmetic",
        "Text",
        "Binary/bit",
        "Containers",
        "Compound element",
        "Document",
        "Range",
        "Spatial",
        "Vector",
        "Full-text",
        "Graph",
        "Time-series",
        "Locator",
        "Opaque"}},
      {Kind::cast_registry,
       "cast_registry_seed_matrix",
       Scope::engine,
       {"Numeric widening/narrowing",
        "Numeric/text",
        "Temporal/text",
        "UUID/object-id/text",
        "Binary/text",
        "JSON/document/text",
        "Domain/base",
        "Reference compatibility",
        "Opaque bridge",
        "Protected values"}},
      {Kind::aggregate_window,
       "aggregate_window_seed_matrix",
       Scope::engine,
       {"Count",
        "Numeric",
        "Boolean/bit",
        "Text/binary",
        "Collection/document",
        "Approximate/sketch",
        "Vector/spatial",
        "Time-series",
        "Window ranking",
        "Window value"}},
      {Kind::index_family_compatibility,
       "index_family_compatibility_matrix",
       Scope::index,
       {"Boolean",
        "Integer/numeric",
        "Floating",
        "Money/currency",
        "Text",
        "Binary/bit",
        "Temporal/interval",
        "UUID/object-id",
        "Network",
        "Enum/set",
        "Array/list/map",
        "Row/composite",
        "Variant",
        "Range/multirange",
        "Document",
        "Full-text/search",
        "Spatial",
        "Vector",
        "Graph",
        "Time-series",
        "Aggregate/sketch",
        "Locator/pseudo/opaque"}},
      {Kind::statistics_selectivity,
       "statistics_selectivity_seed_matrix",
       Scope::optimizer,
       {"Scalar orderable",
        "Text",
        "Temporal",
        "Document",
        "Spatial",
        "Vector",
        "Full-text",
        "Graph",
        "Time-series",
        "Columnar",
        "Opaque"}},
      {Kind::driver_metadata,
       "driver_metadata_seed_matrix",
       Scope::driver,
       {"Type name",
        "Type code",
        "Precision/scale",
        "Display size",
        "Nullable",
        "Signed",
        "Searchable",
        "Case-sensitive",
        "Currency",
        "Auto-increment/generated",
        "Literal prefix/suffix",
        "Array/compound metadata",
        "Unsupported/degraded status"}},
      {Kind::backup_replication_transport,
       "backup_replication_transport_seed_matrix",
       Scope::cross_subsystem,
       {"Scalar native/domain",
        "LOB/oversized",
        "Compound/document",
        "Spatial/vector/full-text",
        "Aggregate/sketch",
        "Locator/external",
        "Opaque/UDR bridge",
        "Protected material"}},
      {Kind::system_catalog,
       "system_catalog_seed_contracts",
       Scope::catalog,
       {"sys.catalog.type_descriptor",
        "sys.catalog.domain_descriptor",
        "sys.catalog.domain_element",
        "sys.catalog.reference_type_mapping",
        "sys.catalog.type_capability",
        "sys.catalog.operation_family",
        "sys.catalog.operation_descriptor",
        "sys.catalog.cast_rule",
        "sys.catalog.aggregate_descriptor",
        "sys.catalog.window_descriptor",
        "sys.catalog.overload_set",
        "sys.catalog.comparison_contract",
        "sys.catalog.canonicalization_profile",
        "sys.catalog.index_compatibility",
        "sys.catalog.type_statistics",
        "sys.catalog.driver_type_metadata",
        "sys.catalog.backup_restore_type_profile",
        "sys.catalog.replication_type_profile",
        "sys.catalog.cluster_type_transport_profile",
        "sys.catalog.type_conformance_case"}},
      {Kind::conformance_corpus,
       "conformance_corpus_seed_matrix",
       Scope::conformance,
       {"Descriptor creation",
        "Literal/bind typing",
        "Casts",
        "Operations",
        "Aggregates/windows",
        "Indexes",
        "Statistics",
        "Optimizer",
        "Driver metadata",
        "Backup/restore",
        "Replication/transport",
        "Diagnostics"}}};
}

std::vector<std::string> RequiredDiagnostics() {
  return {"EDC.MATRIX_RECORD_MISSING",
          "EDC.MATRIX_ROW_MISSING",
          "EDC.MATRIX_ROW_IDENTITY_INCOMPLETE",
          "EDC.SURFACE_RECORD_MISSING",
          "EDC.ROW_SURFACE_COVERAGE_INCOMPLETE",
          "EDC.SEED_INVENTORY_INCOMPLETE",
          "EDC.SUPERSESSION_RECORD_MISSING",
          "EDC.CLOSURE_CLAIM_MISSING",
          "EDC.CLAIM_EVIDENCE_INCOMPLETE",
          "EDC.CONFORMANCE_CASE_MISSING",
          "EDC.IMPLEMENTATION_TRACE_MISSING",
          "EDC.DEFERRED_OR_UNSUPPORTED_DIAGNOSTIC_MISSING",
          "EDC.NATIVE_OR_BETTER_UNPROVEN"};
}

std::vector<std::string> RequiredMetrics() {
  return {"sys.metrics.execution_detail_closure.matrix_count",
          "sys.metrics.execution_detail_closure.row_count",
          "sys.metrics.execution_detail_closure.surface_count",
          "sys.metrics.execution_detail_closure.matrix_record_missing_total",
          "sys.metrics.execution_detail_closure.matrix_row_missing_total",
          "sys.metrics.execution_detail_closure.row_identity_incomplete_total",
          "sys.metrics.execution_detail_closure.surface_missing_total",
          "sys.metrics.execution_detail_closure.surface_incomplete_total",
          "sys.metrics.execution_detail_closure.seed_inventory_incomplete_total",
          "sys.metrics.execution_detail_closure.supersession_missing_total",
          "sys.metrics.execution_detail_closure.claim_evidence_incomplete_total",
          "sys.metrics.execution_detail_closure.conformance_case_missing_total",
          "sys.metrics.execution_detail_closure.implementation_trace_missing_total",
          "sys.metrics.execution_detail_closure.native_or_better_unproven_total"};
}

void AddRow(engine::ExecutionDetailClosureRegistry& registry,
            const engine::ExecutionDetailClosureMatrixRecord& matrix,
            engine::ExecutionDetailClosureRowScope scope,
            std::string_view subject,
            std::uint16_t& seed,
            engine::ExecutionDetailClosureCompletionState state =
                engine::ExecutionDetailClosureCompletionState::complete) {
  const auto token = KeyToken(subject);
  engine::ExecutionDetailClosureRowRecord row;
  row.matrix_row_uuid = Uuid(seed++);
  row.matrix_uuid = matrix.matrix_uuid;
  row.row_search_key = std::string(matrix.matrix_search_key) + "." + token;
  row.row_subject = std::string(subject);
  row.row_scope = scope;
  row.representation_or_status =
      scope == engine::ExecutionDetailClosureRowScope::native
          ? "native_or_better"
          : "complete";
  row.controlling_spec_path =
      "docs/specifications/chapters/data-representation/datatypes/"
      "appendix-execution-detail-closure-matrices.md";
  row.controlling_search_key = matrix.matrix_search_key;
  row.required_surface_set_hash = "required." + row.row_search_key;
  row.completed_surface_set_hash = "completed." + row.row_search_key;
  row.completion_state = state;
  row.diagnostic_code = "EDC.ROW.OK";
  row.conformance_case_set_hash = "conformance." + row.row_search_key;
  row.implementation_trace_uuid = Uuid(seed++);
  row.row_hash = "row." + row.row_search_key;
  const auto row_uuid = row.matrix_row_uuid;
  registry.rows.push_back(row);

  for (const auto surface_kind : RequiredSurfaces(matrix.matrix_kind)) {
    engine::ExecutionDetailClosureSurfaceRecord surface;
    surface.surface_uuid = Uuid(seed++);
    surface.matrix_row_uuid = row_uuid;
    surface.surface_kind = surface_kind;
    surface.surface_status =
        engine::ExecutionDetailClosureSurfaceStatus::complete;
    surface.owning_spec_path =
        "docs/specifications/chapters/data-representation/datatypes/"
        "appendix-execution-detail-closure-matrices.md";
    surface.owning_search_key = std::string(matrix.matrix_search_key);
    surface.evidence_hash = "evidence." + token;
    surface.failure_diagnostic_code = "EDC.SURFACE_RECORD_MISSING";
    surface.surface_hash = "surface." + token;
    registry.surfaces.push_back(surface);
  }

  engine::ExecutionDetailClosureClaimRecord claim;
  claim.closure_claim_uuid = Uuid(seed++);
  claim.matrix_row_uuid = row_uuid;
  claim.claim_scope = engine::ExecutionDetailClosureClaimScope::release_ready;
  claim.claim_status = engine::ExecutionDetailClosureClaimStatus::go;
  claim.required_evidence_hash = "evidence." + row.row_search_key;
  claim.provided_evidence_hash = claim.required_evidence_hash;
  claim.decision_diagnostic_code = "EDC.CLAIM.ACCEPTED";
  claim.claim_hash = "claim." + row.row_search_key;
  registry.claims.push_back(claim);

  if (scope == engine::ExecutionDetailClosureRowScope::native &&
      subject == std::string_view("Boolean")) {
    engine::ExecutionDetailClosureClaimRecord native_claim = claim;
    native_claim.closure_claim_uuid = Uuid(seed++);
    native_claim.claim_scope =
        engine::ExecutionDetailClosureClaimScope::native_or_better;
    native_claim.claim_hash = "native.claim." + row.row_search_key;
    registry.claims.push_back(native_claim);
  }
}

engine::ExecutionDetailClosureRegistry ValidRegistry() {
  engine::ExecutionDetailClosureRegistry registry;
  std::uint16_t seed = 1;
  registry.registry_uuid = Uuid(seed++);
  registry.registry_epoch = 1;
  registry.registry_name = "execution_detail_closure_matrix_registry";
  registry.detail_closure_search_key = "EDC-DETAIL-CLOSURE-MATRICES";
  registry.diagnostic_codes = RequiredDiagnostics();
  registry.local_metric_names = RequiredMetrics();

  for (const auto& definition : MatrixDefinitions()) {
    engine::ExecutionDetailClosureMatrixRecord matrix;
    matrix.matrix_uuid = Uuid(seed++);
    matrix.matrix_name = definition.name;
    matrix.matrix_search_key = std::string(
        engine::ExecutionDetailClosureMatrixKindSearchKey(definition.kind));
    matrix.matrix_kind = definition.kind;
    matrix.controlling_appendix_set_hash = "appendix-set." + definition.name;
    matrix.minimum_inventory_hash = "minimum-inventory." + definition.name;
    matrix.required_seed_subjects = definition.subjects;
    matrix.coverage_status = engine::ExecutionDetailClosureCoverageStatus::complete;
    matrix.conformance_manifest_hash = "manifest." + definition.name;
    matrix.implementation_trace_uuid = Uuid(seed++);
    matrix.matrix_hash = "matrix." + definition.name;
    registry.matrices.push_back(matrix);

    for (const auto& subject : definition.subjects) {
      AddRow(registry, registry.matrices.back(), definition.scope, subject,
             seed);
    }
  }
  return registry;
}

void RequireStatus(const engine::ExecutionDetailClosureRegistry& registry,
                   engine::ExecutionDetailClosureStatus expected,
                   std::string_view message) {
  const auto result = engine::ValidateExecutionDetailClosureRegistry(registry);
  Require(!result.ok(), message);
  if (result.status != expected) {
    std::cerr << "expected="
              << engine::ExecutionDetailClosureStatusName(expected)
              << " actual="
              << engine::ExecutionDetailClosureStatusName(result.status)
              << '\n';
    Fail("EDC closure status mismatch");
  }
}

void TestValidRegistryCoversAllEdcGates() {
  const auto registry = ValidRegistry();
  const auto result = engine::ValidateExecutionDetailClosureRegistry(registry);
  Require(result.ok(), "EDC rejected a complete closure registry");
  Require(registry.matrices.size() == 11,
          "EDC-GATE-009 did not register every matrix section");
  Require(registry.rows.size() > 200,
          "EDC-GATE-010 did not expand the appendix seed subjects");
  Require(!registry.surfaces.empty(),
          "EDC-GATE-011 did not register closure surfaces");
  Require(engine::ExecutionDetailClosureStatusName(
              engine::ExecutionDetailClosureStatus::ok) == "ok",
          "EDC status names are not stable");
}

void TestMatrixAndSeedFailures() {
  auto registry = ValidRegistry();
  registry.matrices.pop_back();
  RequireStatus(registry, engine::ExecutionDetailClosureStatus::matrix_record_missing,
                "EDC accepted missing conformance corpus matrix");

  registry = ValidRegistry();
  registry.rows.erase(registry.rows.begin());
  RequireStatus(registry,
                engine::ExecutionDetailClosureStatus::seed_inventory_incomplete,
                "EDC accepted missing canonical seed row");

  registry = ValidRegistry();
  registry.matrices[0].matrix_search_key = "EDC-WRONG";
  RequireStatus(registry,
                engine::ExecutionDetailClosureStatus::matrix_identity_incomplete,
                "EDC accepted wrong matrix search key");
}

void TestSurfaceAndClaimFailures() {
  auto registry = ValidRegistry();
  registry.surfaces.erase(registry.surfaces.begin());
  RequireStatus(registry, engine::ExecutionDetailClosureStatus::surface_record_missing,
                "EDC accepted row missing a required surface");

  registry = ValidRegistry();
  registry.surfaces[0].surface_status =
      engine::ExecutionDetailClosureSurfaceStatus::partial;
  RequireStatus(registry,
                engine::ExecutionDetailClosureStatus::
                    row_surface_coverage_incomplete,
                "EDC accepted incomplete closure surface");

  registry = ValidRegistry();
  registry.claims[0].provided_evidence_hash = "mismatch";
  RequireStatus(registry,
                engine::ExecutionDetailClosureStatus::claim_evidence_incomplete,
                "EDC accepted claim with incomplete evidence");

  registry = ValidRegistry();
  registry.rows[0].implementation_trace_uuid = {};
  RequireStatus(registry,
                engine::ExecutionDetailClosureStatus::implementation_trace_missing,
                "EDC accepted release row without implementation trace");
}

void TestDiagnosticNativeSupersessionAndMetricsFailures() {
  auto registry = ValidRegistry();
  registry.rows[0].completion_state =
      engine::ExecutionDetailClosureCompletionState::deferred;
  registry.rows[0].diagnostic_code.clear();
  RequireStatus(registry,
                engine::ExecutionDetailClosureStatus::
                    deferred_or_unsupported_diagnostic_missing,
                "EDC accepted deferred row without deterministic diagnostic");

  registry = ValidRegistry();
  engine::ExecutionDetailClosureClaimRecord native_claim;
  native_claim.closure_claim_uuid = Uuid(60000);
  native_claim.matrix_row_uuid = registry.rows[40].matrix_row_uuid;
  native_claim.claim_scope =
      engine::ExecutionDetailClosureClaimScope::native_or_better;
  native_claim.claim_status = engine::ExecutionDetailClosureClaimStatus::go;
  native_claim.required_evidence_hash = "native.evidence";
  native_claim.provided_evidence_hash = native_claim.required_evidence_hash;
  native_claim.decision_diagnostic_code = "EDC.CLAIM.ACCEPTED";
  native_claim.claim_hash = "native.unproven";
  registry.claims.push_back(native_claim);
  RequireStatus(registry,
                engine::ExecutionDetailClosureStatus::native_or_better_unproven,
                "EDC accepted native-or-better claim without documentation");

  registry = ValidRegistry();
  const auto matrix = registry.matrices[0];
  std::uint16_t seed = 61000;
  AddRow(registry, matrix, engine::ExecutionDetailClosureRowScope::native,
         "Superseded old row", seed,
         engine::ExecutionDetailClosureCompletionState::superseded);
  registry.claims.pop_back();
  RequireStatus(registry,
                engine::ExecutionDetailClosureStatus::
                    supersession_record_missing,
                "EDC accepted superseded row without supersession record");

  registry = ValidRegistry();
  registry.diagnostic_codes.pop_back();
  RequireStatus(registry,
                engine::ExecutionDetailClosureStatus::diagnostic_vector_missing,
                "EDC accepted missing diagnostic vector code");

  registry = ValidRegistry();
  registry.local_metric_names.pop_back();
  RequireStatus(registry,
                engine::ExecutionDetailClosureStatus::local_metric_missing,
                "EDC accepted missing execution detail closure metric");
}

}  // namespace

int main() {
  TestValidRegistryCoversAllEdcGates();
  TestMatrixAndSeedFailures();
  TestSurfaceAndClaimFailures();
  TestDiagnosticNativeSupersessionAndMetricsFailures();
  std::cout << "execution_detail_closure_matrix_conformance=passed\n";
  return EXIT_SUCCESS;
}
