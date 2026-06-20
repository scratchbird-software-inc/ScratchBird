// Copyright (c) 2026 ScratchBird Software Inc.
//
// This Source Code Form is subject to the terms of the Mozilla Public
// License, v. 2.0. If a copy of the MPL was not distributed with this
// file, You can obtain one at https://mozilla.org/MPL/2.0/.
//
// SPDX-License-Identifier: MPL-2.0

#include "crud_support/crud_store.hpp"
#include "database_lifecycle.hpp"
#include "ddl/create_api.hpp"
#include "descriptor_value_runtime.hpp"
#include "mga_relation_store/mga_relation_store.hpp"
#include "query/expression_api.hpp"
#include "sblr_dispatch.hpp"
#include "sblr_operator_runtime.hpp"
#include "transaction/transaction_api.hpp"
#include "uuid.hpp"

#include <chrono>
#include <cmath>
#include <cstdlib>
#include <filesystem>
#include <iostream>
#include <string>
#include <string_view>
#include <utility>
#include <vector>

namespace {

namespace api = scratchbird::engine::internal_api;
namespace db = scratchbird::storage::database;
namespace exec = scratchbird::engine::executor;
namespace sblr = scratchbird::engine::sblr;
namespace uuid = scratchbird::core::uuid;
using scratchbird::core::platform::UuidKind;

constexpr std::string_view kSchemaUuid = "019f0000-0000-7000-8000-000000080801";
constexpr std::string_view kTableUuid = "019f0000-0000-7000-8000-000000080802";
constexpr std::string_view kInlineIndexUuid = "019f0000-0000-7000-8000-000000080803";

void Require(bool condition, std::string_view message) {
  if (!condition) {
    std::cerr << message << '\n';
    std::exit(EXIT_FAILURE);
  }
}

std::uint64_t CurrentUnixMillis() {
  return static_cast<std::uint64_t>(
      std::chrono::duration_cast<std::chrono::milliseconds>(
          std::chrono::system_clock::now().time_since_epoch())
          .count());
}

std::filesystem::path TestDatabasePath() {
  return std::filesystem::temp_directory_path() /
         ("sbsql_advanced_datatype_operation_closure_" +
          std::to_string(CurrentUnixMillis()) + ".sbdb");
}

void RemoveDatabaseArtifacts(const std::filesystem::path& path) {
  std::error_code ignored;
  std::filesystem::remove(path, ignored);
  for (const auto suffix : {".sb.api_events",
                            ".sb.crud_events",
                            ".sb.domain_events",
                            ".sb.name_events",
                            ".sb.transaction_inventory",
                            ".sb.mga_row_versions",
                            ".sb.mga_relation_metadata",
                            ".sb.mga_index_entries",
                            ".sb.mga_relation_descriptors",
                            ".sb.mga_large_values",
                            ".sb.mga_savepoints",
                            ".dirty.manifest",
                            ".recovery.evidence",
                            ".sb.owner.lock"}) {
    std::filesystem::remove(path.string() + suffix, ignored);
  }
}

std::string CreateMinimalDatabase(const std::filesystem::path& path) {
  db::DatabaseCreateConfig create;
  create.path = path.string();
  create.database_uuid =
      uuid::GenerateEngineIdentityV7(UuidKind::database, 1779810600000).value;
  create.filespace_uuid =
      uuid::GenerateEngineIdentityV7(UuidKind::filespace, 1779810600001).value;
  create.page_size = 16384;
  create.creation_unix_epoch_millis = 1779810600002;
  create.allow_minimal_resource_bootstrap = true;
  create.require_resource_seed_pack = false;
  create.allow_overwrite = true;
  const auto created = db::CreateDatabaseFile(create);
  if (!created.ok()) {
    std::cerr << created.diagnostic.diagnostic_code << ':'
              << created.diagnostic.message_key << '\n';
  }
  Require(created.ok(), "advanced datatype closure database create failed");
  return uuid::UuidToString(create.database_uuid.value);
}

api::EngineRequestContext EngineContext(const std::filesystem::path& path,
                                        const std::string& database_uuid) {
  api::EngineRequestContext context;
  context.request_id = "sbsql-advanced-datatype-operation-closure";
  context.database_path = path.string();
  context.database_uuid.canonical = database_uuid;
  context.session_uuid.canonical = "019f0000-0000-7000-8000-000000080811";
  context.principal_uuid.canonical = "019f0000-0000-7000-8000-000000080812";
  context.current_schema_uuid.canonical = std::string(kSchemaUuid);
  context.security_context_present = true;
  context.catalog_generation_id = 1;
  context.security_epoch = 1;
  context.resource_epoch = 1;
  context.name_resolution_epoch = 1;
  context.trace_tags.push_back("right:CATALOG_MUTATE");
  context.trace_tags.push_back("advanced_datatype_operation_closure");
  return context;
}

api::EngineRequestContext BeginTransaction(api::EngineRequestContext context) {
  api::EngineBeginTransactionRequest begin;
  begin.context = context;
  begin.isolation_level = "read_committed";
  const auto result = api::EngineBeginTransaction(begin);
  Require(result.ok, "transaction.begin failed for advanced datatype closure");
  context.local_transaction_id = result.local_transaction_id;
  context.transaction_uuid = result.transaction_uuid;
  return context;
}

api::EngineLocalizedName Name(std::string value);
std::string FirstDetail(const api::EngineApiResult& result);

void CreateSchema(const api::EngineRequestContext& context) {
  api::EngineCreateSchemaRequest request;
  request.context = context;
  request.target_object.uuid.canonical = std::string(kSchemaUuid);
  request.target_object.object_kind = "schema";
  request.localized_names.push_back(Name("cbq008_schema"));
  const auto result = api::EngineCreateSchema(request);
  if (!result.ok) { std::cerr << FirstDetail(result) << '\n'; }
  Require(result.ok, "create schema failed for advanced datatype closure");
}

api::EngineDescriptor Descriptor(std::string type) {
  api::EngineDescriptor descriptor;
  descriptor.descriptor_kind = "scalar";
  descriptor.canonical_type_name = std::move(type);
  descriptor.encoded_descriptor = "canonical_type=" + descriptor.canonical_type_name;
  return descriptor;
}

api::EngineTypedValue TypedValue(std::string type, std::string encoded) {
  api::EngineTypedValue value;
  value.descriptor = Descriptor(std::move(type));
  value.encoded_value = std::move(encoded);
  value.is_null = false;
  return value;
}

bool HasEvidence(const api::EngineApiResult& result,
                 std::string_view kind,
                 std::string_view id) {
  for (const auto& evidence : result.evidence) {
    if (evidence.evidence_kind == kind && evidence.evidence_id == id) { return true; }
  }
  return false;
}

std::string FirstDetail(const api::EngineApiResult& result) {
  return result.diagnostics.empty() ? std::string{} : result.diagnostics.front().detail;
}

void AddOptionOperand(sblr::SblrOperationEnvelope* envelope,
                      std::string name,
                      std::string value) {
  sblr::SblrOperand operand;
  operand.type = "option";
  operand.name = std::move(name);
  operand.value = std::move(value);
  envelope->operands.push_back(std::move(operand));
}

sblr::SblrOperationEnvelope QueryEnvelope(std::string operation_id,
                                          std::string opcode,
                                          std::string trace_key) {
  auto envelope = sblr::MakeSblrEnvelope(std::move(operation_id),
                                         std::move(opcode),
                                         std::move(trace_key));
  envelope.requires_security_context = true;
  envelope.requires_transaction_context = false;
  envelope.requires_cluster_authority = false;
  envelope.contains_sql_text = false;
  envelope.parser_resolved_names_to_uuids = true;
  return envelope;
}

void PrintDispatchDiagnostics(const sblr::SblrDispatchResult& result) {
  for (const auto& diagnostic : result.diagnostics) {
    std::cerr << diagnostic.code << ':' << diagnostic.message << '\n';
  }
  for (const auto& diagnostic : result.api_result.diagnostics) {
    std::cerr << diagnostic.code << ':' << diagnostic.detail << '\n';
  }
}

void RequireNumericOperations(const api::EngineRequestContext& context) {
  const std::vector<std::string> operations = {
      "canonicalize", "add", "sub", "mul", "div", "cmp"};
  const std::vector<std::string> rounding_modes = {
      "half_even", "half-up", "toward_zero"};
  for (const auto& operation : operations) {
    for (const auto& rounding : rounding_modes) {
      api::EngineApplyNumericOperationRequest request;
      request.context = context;
      request.numeric_operation = operation;
      request.rounding_mode = rounding;
      request.precision = 38;
      request.scale = 2;
      request.left_value = TypedValue("decimal", "10");
      request.right_value = TypedValue("decimal", "2");
      request.descriptors.push_back(Descriptor("decimal"));
      const auto result = api::EngineApplyNumericOperation(request);
      if (!result.ok) { std::cerr << operation << '/' << rounding << ':' << FirstDetail(result) << '\n'; }
      Require(result.ok, "supported numeric operation or rounding mode refused");
      Require(HasEvidence(result, "datatype_numeric_operation",
                          operation == "sub" ? "subtract" :
                          operation == "mul" ? "multiply" :
                          operation == "div" ? "divide" :
                          operation == "cmp" ? "compare" : operation),
              "numeric operation evidence drifted");
    }
  }

  api::EngineApplyNumericOperationRequest bad_operation;
  bad_operation.context = context;
  bad_operation.numeric_operation = "modulo";
  bad_operation.left_value = TypedValue("decimal", "10");
  bad_operation.right_value = TypedValue("decimal", "2");
  auto rejected = api::EngineApplyNumericOperation(bad_operation);
  Require(!rejected.ok && FirstDetail(rejected) == "query.apply_numeric_operation:numeric_operation_unsupported:modulo",
          "unsupported numeric operation diagnostic drifted");

  api::EngineApplyNumericOperationRequest bad_rounding;
  bad_rounding.context = context;
  bad_rounding.numeric_operation = "add";
  bad_rounding.rounding_mode = "stochastic";
  bad_rounding.left_value = TypedValue("decimal", "10");
  bad_rounding.right_value = TypedValue("decimal", "2");
  rejected = api::EngineApplyNumericOperation(bad_rounding);
  Require(!rejected.ok && FirstDetail(rejected) == "query.apply_numeric_operation:numeric_rounding_mode_unsupported:stochastic",
          "unsupported numeric rounding diagnostic drifted");
}

struct AdvancedCase {
  std::string type_name;
  std::string operation;
  std::string index;
  std::string family;
  std::string descriptor_profile;
  std::uint32_t vector_dimension = 0;
};

void RequireAdvancedFamilies(const api::EngineRequestContext& context) {
  const std::vector<AdvancedCase> cases = {
      {"point", "operator.spatial.contains", "rtree", "spatial", "format=wkb;srid=4326", 0},
      {"point", "spatial.distance", "geohash", "spatial", "format=wkb;srid=4326", 0},
      {"dense_vector", "operator.vector.distance", "hnsw", "vector", "dimension=3;element_type=real32", 3},
      {"dense_vector", "vector_nearest_neighbor", "ivf_flat", "vector", "dimension=3;element_type=real32", 3},
      {"token_stream", "search.tokenize", "inverted", "search", "language=en;tokenizer=unicode_v1", 0},
      {"token_stream", "operator.search.rank", "inverted", "search", "language=en;tokenizer=unicode_v1", 0},
      {"graph_path", "graph.traverse", "adjacency", "graph", "direction=directed;schema_uuid=019f0000-0000-7000-8000-000000080801", 0},
      {"graph_path", "graph.traverse", "graph_adjacency", "graph", "direction=directed;schema_uuid=019f0000-0000-7000-8000-000000080801", 0},
      {"graph_path", "operator.graph.path_match", "adjacency", "graph", "direction=directed;schema_uuid=019f0000-0000-7000-8000-000000080801", 0},
      {"time_series_value", "time_series.append_point", "time-partition", "time_series", "timestamp_type=timestamp;value_type=real64", 0},
      {"time_series_value", "time_series.aggregate_window", "time_partition", "time_series", "timestamp_type=timestamp;value_type=real64", 0},
  };
  for (const auto& item : cases) {
    api::EngineEvaluateAdvancedDatatypeFamilyRequest request;
    request.context = context;
    request.descriptor = Descriptor(item.type_name);
    request.operation_kind = item.operation;
    request.index_kind = item.index;
    request.descriptor_profile = item.descriptor_profile;
    request.vector_dimension = item.vector_dimension;
    const auto result = api::EngineEvaluateAdvancedDatatypeFamily(request);
    if (!result.ok) { std::cerr << item.type_name << ':' << item.operation << ':' << item.index << ':' << FirstDetail(result) << '\n'; }
    Require(result.ok, "supported advanced datatype family operation/index refused");
    Require(result.family == item.family, "advanced datatype family evidence drifted");
    Require(result.operation_supported && result.index_supported,
            "advanced datatype operation/index support flags drifted");
    Require(result.optimizer_admitted, "advanced datatype optimizer admission drifted");
  }

  api::EngineEvaluateAdvancedDatatypeFamilyRequest bad_operation;
  bad_operation.context = context;
  bad_operation.descriptor = Descriptor("point");
  bad_operation.operation_kind = "teleport";
  auto rejected = api::EngineEvaluateAdvancedDatatypeFamily(bad_operation);
  Require(!rejected.ok && FirstDetail(rejected) == "query.evaluate_advanced_datatype_family:advanced_operation_unsupported:teleport",
          "unsupported advanced operation diagnostic drifted");

  api::EngineEvaluateAdvancedDatatypeFamilyRequest bad_index;
  bad_index.context = context;
  bad_index.descriptor = Descriptor("point");
  bad_index.operation_kind = "contains";
  bad_index.index_kind = "mystery";
  rejected = api::EngineEvaluateAdvancedDatatypeFamily(bad_index);
  Require(!rejected.ok && FirstDetail(rejected) == "query.evaluate_advanced_datatype_family:advanced_index_unsupported:mystery",
          "unsupported advanced index diagnostic drifted");
}

void RequireSblrApiDispatchRoutes(const api::EngineRequestContext& context) {
  auto numeric = QueryEnvelope("query.apply_numeric_operation",
                               "SBLR_QUERY_APPLY_NUMERIC_OPERATION",
                               "trace.cbq008.query.apply_numeric_operation");
  AddOptionOperand(&numeric, "numeric_operation", "add");
  AddOptionOperand(&numeric, "rounding_mode", "half_even");
  AddOptionOperand(&numeric, "precision", "38");
  AddOptionOperand(&numeric, "scale", "2");
  AddOptionOperand(&numeric, "left_type", "decimal");
  AddOptionOperand(&numeric, "left_value", "10");
  AddOptionOperand(&numeric, "right_type", "decimal");
  AddOptionOperand(&numeric, "right_value", "2");
  auto dispatched = sblr::DispatchSblrOperation({context, numeric, api::EngineApiRequest{}});
  if (!dispatched.api_result.ok) { PrintDispatchDiagnostics(dispatched); }
  Require(dispatched.envelope_validated, "numeric SBLR/API envelope did not validate");
  Require(dispatched.accepted && dispatched.dispatched_to_api,
          "numeric SBLR/API route did not dispatch to the internal API");
  Require(dispatched.api_result.ok &&
              dispatched.api_result.operation_id == "query.apply_numeric_operation",
          "numeric SBLR/API route failed");
  Require(HasEvidence(dispatched.api_result, "datatype_numeric_operation", "add"),
          "numeric SBLR/API route evidence drifted");

  auto advanced = QueryEnvelope("query.evaluate_advanced_datatype_family",
                                "SBLR_QUERY_EVALUATE_ADVANCED_DATATYPE_FAMILY",
                                "trace.cbq008.query.evaluate_advanced_datatype_family");
  AddOptionOperand(&advanced, "descriptor_type", "graph_path");
  AddOptionOperand(&advanced, "operation_kind", "graph.traverse");
  AddOptionOperand(&advanced, "index_kind", "graph_adjacency");
  AddOptionOperand(&advanced,
                   "descriptor_profile",
                   "direction=directed;schema_uuid=019f0000-0000-7000-8000-000000080801");
  dispatched = sblr::DispatchSblrOperation({context, advanced, api::EngineApiRequest{}});
  if (!dispatched.api_result.ok) { PrintDispatchDiagnostics(dispatched); }
  Require(dispatched.envelope_validated, "advanced datatype SBLR/API envelope did not validate");
  Require(dispatched.accepted && dispatched.dispatched_to_api,
          "advanced datatype SBLR/API route did not dispatch to the internal API");
  Require(dispatched.api_result.ok &&
              dispatched.api_result.operation_id == "query.evaluate_advanced_datatype_family",
          "advanced datatype SBLR/API route failed");
  Require(HasEvidence(dispatched.api_result, "advanced_family", "graph"),
          "advanced datatype SBLR/API family evidence drifted");
  Require(HasEvidence(dispatched.api_result, "advanced_index", "adjacency"),
          "advanced datatype SBLR/API adjacency evidence drifted");
}

sblr::SblrExecutionContext SblrContext() {
  sblr::SblrExecutionContext context;
  context.database_uuid = "CBQ-008-ADVANCED-DATATYPE-OPERATION-CLOSURE-db";
  context.transaction_uuid = "CBQ-008-ADVANCED-DATATYPE-OPERATION-CLOSURE-tx";
  context.transaction_context_present = true;
  return context;
}

sblr::SblrValue SblrText(std::string descriptor, std::string text) {
  sblr::SblrValue value;
  value.descriptor_id = std::move(descriptor);
  value.payload_kind = sblr::SblrValuePayloadKind::text;
  value.is_null = false;
  value.encoded_value = std::move(text);
  value.text_value = value.encoded_value;
  return value;
}

sblr::SblrValue SblrInt(std::int64_t input) {
  sblr::SblrValue value;
  value.descriptor_id = "int64";
  value.payload_kind = sblr::SblrValuePayloadKind::signed_integer;
  value.is_null = false;
  value.has_int64_value = true;
  value.int64_value = input;
  value.encoded_value = std::to_string(input);
  value.text_value = value.encoded_value;
  return value;
}

void RequireOkScalar(const sblr::SblrResult& result, std::string_view message) {
  if (!result.ok()) {
    for (const auto& diagnostic : result.diagnostics) {
      std::cerr << diagnostic.diagnostic_id << ':' << diagnostic.detail << '\n';
    }
  }
  Require(result.ok() && result.scalar_values.size() == 1 &&
              !result.mutation_attempted && !result.mutation_committed,
          message);
}

void RequireSblrCollectionVectorSpecializedOperators() {
  const auto context = SblrContext();
  const auto array = SblrText("array", "[1,2,3]");
  const auto array_two = SblrText("array", "[3,4]");
  const auto item_two = SblrInt(2);
  const std::vector<std::string> collection_ids = {
      "op_array_subscript", "operator.collection.subscript",
      "op_collection_contains", "operator.collection.contains", "op_array_contains",
      "op_collection_overlap", "operator.collection.overlap",
      "op_collection_concat", "operator.collection.concat",
      "op_collection_cardinality", "operator.collection.cardinality"};
  for (const auto& operator_id : collection_ids) {
    const sblr::SblrValue right =
        operator_id.find("subscript") != std::string::npos || operator_id == "op_array_subscript"
            ? item_two
            : (operator_id.find("concat") != std::string::npos ||
                       operator_id.find("overlap") != std::string::npos
                   ? array_two
                   : SblrInt(2));
    const auto result = sblr::EvaluateSblrCollectionOperator(operator_id, array, right, context);
    RequireOkScalar(result, "registered collection operator refused");
  }
  const auto cardinality = sblr::EvaluateSblrCollectionOperator("operator.collection.cardinality", array, SblrInt(0), context);
  RequireOkScalar(cardinality, "collection cardinality refused");
  Require(cardinality.scalar_values.front().has_uint64_value &&
              cardinality.scalar_values.front().uint64_value == 3,
          "collection cardinality result drifted");

  const auto vector_left = SblrText("dense_vector", "[1,2,3]");
  const auto vector_right = SblrText("dense_vector", "[4,5,6]");
  const std::vector<std::string> vector_ids = {
      "op_vector_distance", "operator.vector.distance",
      "op_vector_squared_distance", "operator.vector.squared_distance",
      "op_vector_dot", "operator.vector.dot",
      "op_vector_inner_product", "operator.vector.inner_product",
      "op_vector_cosine_similarity", "operator.vector.cosine_similarity",
      "op_vector_cosine_distance", "operator.vector.cosine_distance"};
  for (const auto& operator_id : vector_ids) {
    RequireOkScalar(sblr::EvaluateSblrVectorOperator(operator_id, vector_left, vector_right, context),
                    "registered vector operator refused");
  }

  const std::vector<std::pair<std::string, std::pair<sblr::SblrValue, sblr::SblrValue>>> specialized = {
      {"op_spatial_contains", {SblrText("point", "[1,2]"), SblrText("point", "[1,2]")}},
      {"operator.spatial.contains", {SblrText("point", "[1,2]"), SblrText("point", "[1,2]")}},
      {"op_spatial_intersects", {SblrText("point", "[1,2]"), SblrText("point", "[1,2]")}},
      {"operator.spatial.intersects", {SblrText("point", "[1,2]"), SblrText("point", "[1,2]")}},
      {"op_spatial_distance", {SblrText("point", "[0,0]"), SblrText("point", "[3,4]")}},
      {"operator.spatial.distance", {SblrText("point", "[0,0]"), SblrText("point", "[3,4]")}},
      {"op_search_match", {SblrText("text", "alpha beta beta"), SblrText("text", "beta")}},
      {"operator.search.match", {SblrText("text", "alpha beta beta"), SblrText("text", "beta")}},
      {"op_search_rank", {SblrText("text", "alpha beta beta"), SblrText("text", "beta")}},
      {"operator.search.rank", {SblrText("text", "alpha beta beta"), SblrText("text", "beta")}},
      {"op_graph_path_match", {SblrText("graph_path", "[\"a\",\"b\"]"), SblrText("text", "b")}},
      {"operator.graph.path_match", {SblrText("graph_path", "[\"a\",\"b\"]"), SblrText("text", "b")}},
  };
  for (const auto& [operator_id, operands] : specialized) {
    RequireOkScalar(sblr::EvaluateSblrSpecializedOperatorBridge(operator_id, operands.first, operands.second, context),
                    "registered specialized operator refused");
  }

  const auto invalid = sblr::EvaluateSblrVectorOperator("operator.vector.teleport", vector_left, vector_right, context);
  Require(!invalid.ok() && !invalid.diagnostics.empty() &&
              invalid.diagnostics.front().diagnostic_id == "SB_DIAG_OPERATOR_INVALID_INPUT",
          "invalid vector operator diagnostic drifted");
}

api::EngineLocalizedName Name(std::string text) {
  return {"en", "primary", "", std::move(text), true};
}

api::EngineColumnDefinition Column(std::string name, std::string type, std::uint32_t ordinal) {
  api::EngineColumnDefinition column;
  column.requested_column_uuid.canonical =
      "019f0000-0000-7000-8000-000000081" + std::to_string(ordinal + 100);
  column.names.push_back(Name(std::move(name)));
  column.descriptor = Descriptor(std::move(type));
  column.ordinal = ordinal;
  column.nullable = true;
  return column;
}

api::EngineIndexDefinition Index(std::string uuid_text,
                                 std::string name,
                                 std::string index_kind,
                                 std::vector<std::string> keys) {
  api::EngineIndexDefinition index;
  index.requested_index_uuid.canonical = std::move(uuid_text);
  index.names.push_back(Name(std::move(name)));
  index.index_kind = std::move(index_kind);
  index.key_envelopes = std::move(keys);
  return index;
}

api::EngineCreateTableResult CreateTable(const api::EngineRequestContext& context,
                                         std::vector<api::EngineIndexDefinition> inline_indexes = {}) {
  api::EngineCreateTableRequest request;
  request.context = context;
  request.target_schema.uuid.canonical = std::string(kSchemaUuid);
  request.target_schema.object_kind = "schema";
  request.requested_table_uuid.canonical = std::string(kTableUuid);
  request.table_names.push_back(Name("cbq008_table"));
  request.table_columns.push_back(Column("id", "int64", 0));
  request.table_columns.push_back(Column("geom", "point", 1));
  request.table_columns.push_back(Column("embedding", "dense_vector", 2));
  request.table_columns.push_back(Column("body", "text", 3));
  request.table_columns.push_back(Column("observed_at", "timestamp", 4));
  request.table_columns.push_back(Column("graph_path", "graph_path", 5));
  request.table_columns.push_back(Column("secret_payload", "opaque_extension", 6));
  request.table_indexes = std::move(inline_indexes);
  return api::EngineCreateTable(request);
}

api::EngineCreateIndexResult CreateIndex(const api::EngineRequestContext& context,
                                         api::EngineIndexDefinition index) {
  api::EngineCreateIndexRequest request;
  request.context = context;
  request.target_object.uuid.canonical = std::string(kTableUuid);
  request.target_object.object_kind = "table";
  request.indexes.push_back(std::move(index));
  return api::EngineCreateIndex(request);
}

void RequireIndexFamilyPersisted(const api::EngineRequestContext& context,
                                 std::string_view index_uuid,
                                 std::string_view family) {
  const auto loaded = api::LoadMgaRelationStoreState(context);
  Require(loaded.ok, "MGA relation metadata load failed");
  const api::CrudState state = api::BuildCrudCompatibilityStateFromMga(loaded.state);
  for (const auto& index : state.indexes) {
    if (index.index_uuid == index_uuid) {
      Require(index.family == family, "persisted index family drifted");
      return;
    }
  }
  Require(false, "persisted index metadata not found");
}

void RequireAdvancedIndexDDL(const api::EngineRequestContext& context) {
  const auto table = CreateTable(context, {Index(std::string(kInlineIndexUuid), "inline_geom_idx", "rtree", {"geom"})});
  if (!table.ok) { std::cerr << FirstDetail(table) << '\n'; }
  Require(table.ok, "create table with inline advanced index failed");
  Require(HasEvidence(table, "inline_index_create", kInlineIndexUuid),
          "inline index create evidence missing");
  RequireIndexFamilyPersisted(context, kInlineIndexUuid, "rtree");

  const std::vector<std::pair<api::EngineIndexDefinition, std::string>> positive_indexes = {
      {Index("019f0000-0000-7000-8000-000000080821", "embedding_hnsw_idx", "hnsw", {"embedding"}), "vector_hnsw"},
      {Index("019f0000-0000-7000-8000-000000080822", "embedding_ivfflat_idx", "ivfflat", {"embedding"}), "vector_ivf"},
      {Index("019f0000-0000-7000-8000-000000080823", "body_inverted_idx", "inverted", {"body"}), "inverted"},
      {Index("019f0000-0000-7000-8000-000000080824", "observed_time_partition_idx", "time_partition", {"observed_at"}), "columnar_zone"},
      {Index("019f0000-0000-7000-8000-000000080825", "body_expression_idx", "expression", {"lower:body"}), "expression"},
      {Index("019f0000-0000-7000-8000-000000080826", "id_partial_idx", "partial", {"id", "where_eq:id=42"}), "partial"},
      {Index("019f0000-0000-7000-8000-000000080827", "graph_adjacency_idx", "adjacency", {"graph_path"}), "graph_adjacency"},
      {Index("019f0000-0000-7000-8000-000000080828", "graph_adjacency_profile_idx", "graph_adjacency", {"graph_path"}), "graph_adjacency"},
      {Index("019f0000-0000-7000-8000-000000080834", "full_partial_idx", "partial", {"id"}), "partial"},
      {Index("019f0000-0000-7000-8000-000000080836", "policy_blocked_idx", "policy_blocked", {"id"}), "policy_blocked"},
  };
  for (const auto& [definition, expected_family] : positive_indexes) {
    const std::string uuid_text = definition.requested_index_uuid.canonical;
    const auto result = CreateIndex(context, definition);
    if (!result.ok) { std::cerr << expected_family << ':' << FirstDetail(result) << '\n'; }
    Require(result.ok, "supported advanced create-index family failed");
    Require(HasEvidence(result, "index_family", expected_family),
            "create-index family evidence drifted");
    RequireIndexFamilyPersisted(context, uuid_text, expected_family);
  }

  auto rejected = CreateIndex(context, Index("019f0000-0000-7000-8000-000000080831", "bad_profile_idx", "mystery", {"id"}));
  Require(!rejected.ok && FirstDetail(rejected) == "ddl.create_index:unsupported_index_profile",
          "unsupported index profile diagnostic drifted");

  rejected = CreateIndex(context, Index("019f0000-0000-7000-8000-000000080832", "missing_key_idx", "hnsw", {}));
  Require(!rejected.ok && FirstDetail(rejected) == "ddl.create_index:at_least_one_key_envelope_required",
          "missing key envelope diagnostic drifted");

  rejected = CreateIndex(context, Index("019f0000-0000-7000-8000-000000080833", "bad_expression_idx", "expression", {"substr:body"}));
  Require(!rejected.ok && FirstDetail(rejected) == "ddl.create_index:unsupported_expression_index_envelope",
          "unsupported expression index diagnostic drifted");

  rejected = CreateIndex(context, Index("019f0000-0000-7000-8000-000000080835", "opaque_idx", "btree", {"secret_payload"}));
  Require(!rejected.ok && FirstDetail(rejected) == "ddl.create_index:opaque_column_index_denied",
          "opaque column index diagnostic drifted");
}

void RequireDescriptorRuntimeDatatypeSlice() {
  exec::DescriptorRuntimeDiagnostic diagnostic;
  auto result = exec::EvaluateDescriptorExpression(exec::DescriptorExpressionOperator::kReal64Add,
                                                   exec::EncodeReal64Value(1.5),
                                                   exec::EncodeReal64Value(2.25),
                                                   &diagnostic);
  Require(diagnostic.ok && result.descriptor.canonical_type_name == "real64" &&
              std::fabs(std::stod(result.encoded_value) - 3.75) < 0.000001,
          "descriptor real64 expression failed");

  result = exec::EvaluateDescriptorExpression(exec::DescriptorExpressionOperator::kTextConcat,
                                              exec::EncodeTextValue("alpha"),
                                              exec::EncodeTextValue("beta"),
                                              &diagnostic);
  Require(diagnostic.ok && result.encoded_value == "alphabeta",
          "descriptor text concat expression failed");

  result = exec::EvaluateDescriptorExpression(exec::DescriptorExpressionOperator::kInt64Divide,
                                              exec::EncodeInt64Value(3),
                                              exec::EncodeInt64Value(0),
                                              &diagnostic);
  Require(!diagnostic.ok && diagnostic.diagnostic_code == "SB_EXECUTOR_DIVIDE_BY_ZERO",
          "descriptor divide-by-zero diagnostic drifted");

  result = exec::CastDescriptorValue(exec::EncodeTextValue("42"),
                                     exec::MakeExecutorDescriptor("int64"),
                                     &diagnostic);
  Require(diagnostic.ok && result.encoded_value == "42" &&
              result.descriptor.canonical_type_name == "int64",
          "descriptor text to int64 cast failed");

  result = exec::ExtractDescriptorField(exec::EncodeTextValue("hello"), "length", &diagnostic);
  Require(diagnostic.ok && result.descriptor.canonical_type_name == "uint64" &&
              result.encoded_value == "5",
          "descriptor text length extract failed");

  result = exec::ExtractDescriptorField(exec::MakeExecutorValue(exec::MakeExecutorDescriptor("binary"), "abcd", false),
                                        "octet_length",
                                        &diagnostic);
  Require(diagnostic.ok && result.descriptor.canonical_type_name == "uint64" &&
              result.encoded_value == "4",
          "descriptor binary octet length extract failed");

  result = exec::ExtractDescriptorField(
      exec::MakeExecutorValue(exec::MakeExecutorDescriptor("uuid"),
                              "550e8400-e29b-41d4-a716-446655440000",
                              false),
      "version",
      &diagnostic);
  Require(diagnostic.ok && result.descriptor.canonical_type_name == "uint8" &&
              result.encoded_value == "4",
          "descriptor uuid version extract failed");
}

}  // namespace

int main() {
  api::EngineRequestContext api_context;
  api_context.request_id = "sbsql-advanced-datatype-operation-api";
  api_context.security_context_present = true;

  RequireNumericOperations(api_context);
  RequireAdvancedFamilies(api_context);
  RequireSblrApiDispatchRoutes(api_context);
  RequireSblrCollectionVectorSpecializedOperators();
  RequireDescriptorRuntimeDatatypeSlice();

  const auto path = TestDatabasePath();
  RemoveDatabaseArtifacts(path);
  const auto database_uuid = CreateMinimalDatabase(path);
  const auto context = BeginTransaction(EngineContext(path, database_uuid));
  CreateSchema(context);
  RequireAdvancedIndexDDL(context);
  return EXIT_SUCCESS;
}
