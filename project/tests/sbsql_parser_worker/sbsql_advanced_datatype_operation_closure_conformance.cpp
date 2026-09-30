// Copyright (c) 2026 ScratchBird Software Inc.
//
// This Source Code Form is subject to the terms of the Mozilla Public
// License, v. 2.0. If a copy of the MPL was not distributed with this
// file, You can obtain one at https://mozilla.org/MPL/2.0/.
//
// SPDX-License-Identifier: MPL-2.0

#include "../support/binary_uuid_fixture.hpp"
#include "../support/catalog_column_binding_fixture.hpp"
#include "../support/engine_statement_fixture.hpp"
#include "crud_support/crud_store.hpp"
#include "catalog/datatype_bootstrap_identity.hpp"
#include "datatype_catalog_manifest.hpp"
#include "database_lifecycle.hpp"
#include "ddl/create_api.hpp"
#include "descriptor_value_runtime.hpp"
#include "memory.hpp"
#include "mga_relation_store/mga_relation_store.hpp"
#include "dml/mga_relation_read_view.hpp"
#include "query/expression_api.hpp"
#include "sblr_dispatch.hpp"
#include "sblr_opcode_registry.hpp"
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
namespace dt = scratchbird::core::datatypes;
namespace exec = scratchbird::engine::executor;
namespace sblr = scratchbird::engine::sblr;
namespace uuid = scratchbird::core::uuid;
using scratchbird::core::platform::UuidKind;

constexpr std::string_view kSchemaUuid = "019f0000-0000-7000-8000-000000080801";
constexpr std::string_view kTableUuid = "019f0000-0000-7000-8000-000000080802";
constexpr auto kInlineIndexUuid = scratchbird::tests::FixtureUuidLiteral("019f0000-0000-7000-8000-000000080803");
constexpr std::string_view kSblrProducerUuid = "019f0000-0000-7000-8000-000000080821";
constexpr std::string_view kSblrRegistryUuid = "019f0000-0000-7000-8000-000000080822";

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
                            ".sb.api_events.v2",
                            ".sb.catalog_object_events",
                            ".sb.crud_events",
                            ".sb.domain_events",
                            ".sb.name_events",
                            ".sb.name_events.v2",
                            ".sb.txn_publish",
                            ".sb.mga_event_sequence_allocator",
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

api::EngineRequestContext CreateFixtureDatabase(const std::filesystem::path& path) {
  db::DatabaseCreateConfig create;
  create.path = path.string();
  create.database_uuid =
      uuid::GenerateEngineIdentityV7(UuidKind::database, 1779810600000).value;
  create.filespace_uuid =
      uuid::GenerateEngineIdentityV7(UuidKind::filespace, 1779810600001).value;
  create.page_size = 16384;
  create.creation_unix_epoch_millis = 1779810600002;
  scratchbird::tests::ConfigureCredentialedFixtureBootstrap(create);
  create.allow_overwrite = true;
  const auto created = db::CreateDatabaseFile(create);
  if (!created.ok()) {
    std::cerr << created.diagnostic.diagnostic_code << ':'
              << created.diagnostic.message_key << '\n';
  }
  Require(created.ok(), "advanced datatype closure database create failed");
  return scratchbird::tests::BootstrapFixtureOwnerContext(create);
}

api::EngineRequestContext EngineContext(const std::filesystem::path& path,
                                        api::EngineRequestContext context) {
  context.request_id = "sbsql-advanced-datatype-operation-closure";
  context.database_path = path.string();
  context.current_schema_uuid = scratchbird::tests::FixtureUuidLiteral("019f0000-0000-7000-8000-000000080801");
  scratchbird::tests::UseBootstrapDatatypeCohort(context);
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
  request.target_object.uuid = scratchbird::tests::FixtureUuidLiteral("019f0000-0000-7000-8000-000000080801");
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

api::EngineDescriptor BoundScalarDescriptor(
    const std::string_view type,
    const unsigned ordinal,
    std::string encoded_descriptor = "nullability=nullable") {
  const auto type_id = dt::CanonicalTypeIdFromStableName(std::string(type));
  const auto manifest = dt::LoadCurrentCoreDatatypeCatalogManifest();
  Require(manifest.ok(), "bound consumer fixture datatype catalog unavailable");
  const auto row = dt::LookupDatatypeCatalogRow(manifest.manifest, type_id);
  Require(row.ok() && row.manifest.descriptor_rows.size() == 1,
          "bound consumer fixture datatype absent from catalog");
  const auto& datatype = row.manifest.descriptor_rows.front();
  const auto identity = dt::LookupDatatypeTypeCodecIdentityV1(
      api::kBootstrapDatatypeCatalogUuid,
      api::kBootstrapDatatypeCatalogGeneration,
      api::kBootstrapDatatypeRegistryGeneration,
      datatype.descriptor_uuid.value, datatype.descriptor_epoch);
  Require(identity.ok,
          "bound consumer fixture datatype identity is not admitted");
  auto descriptor = Descriptor(std::string(type));
  descriptor.descriptor_uuid = scratchbird::tests::FixtureUuid(2086, ordinal);
  descriptor.type_uuid = identity.row.type_uuid;
  descriptor.datatype_descriptor_uuid = datatype.descriptor_uuid.value;
  descriptor.datatype_descriptor_generation = datatype.descriptor_epoch;
  descriptor.encoded_descriptor = std::move(encoded_descriptor);
  return descriptor;
}

scratchbird::engine::ExecutionTypeDescriptor CoreExecutionDescriptor(
    const dt::CanonicalTypeId type_id,
    const bool nullable) {
  const auto manifest = dt::LoadCurrentCoreDatatypeCatalogManifest();
  Require(manifest.ok(), "set fixture datatype catalog unavailable");
  const auto row = dt::LookupDatatypeCatalogRow(manifest.manifest, type_id);
  Require(row.ok() && row.manifest.descriptor_rows.size() == 1,
          "set fixture datatype descriptor unavailable");
  dt::CatalogExecutionTypeMetadata metadata;
  metadata.descriptor_uuid = row.manifest.descriptor_rows.front().descriptor_uuid;
  metadata.descriptor_epoch = row.manifest.descriptor_rows.front().descriptor_epoch;
  const auto built =
      dt::LookupExecutionTypeDescriptorFromCatalog(type_id, metadata);
  Require(built.ok(), "set fixture execution descriptor unavailable");
  auto descriptor = built.descriptor;
  descriptor.nullable_allowed = nullable;
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
    if (evidence.evidence_kind == kind && (std::holds_alternative<std::string>(evidence.evidence_id) && std::get<std::string>(evidence.evidence_id) == id)) { return true; }
  }
  return false;
}

bool HasEvidence(const api::EngineApiResult& result,
                 std::string_view kind, const api::EngineUuid& id) {
  for (const auto& evidence : result.evidence) {
    const auto* identity = std::get_if<api::EngineUuid>(&evidence.evidence_id);
    if (evidence.evidence_kind == kind && identity && *identity == id) return true;
  }
  return false;
}

std::string FirstDetail(const api::EngineApiResult& result) {
  return result.diagnostics.empty() ? std::string{} : result.diagnostics.front().detail;
}

const std::vector<std::uint8_t>& CanonicalOptionDescriptorIdentity() {
  static const std::vector<std::uint8_t> descriptor_identity = [] {
    const auto manifest = dt::LoadCurrentCoreDatatypeCatalogManifest();
    Require(manifest.ok(), "canonical datatype manifest did not load");
    const auto row =
        dt::LookupDatatypeCatalogRow(manifest.manifest,
                                     dt::CanonicalTypeId::character);
    Require(row.ok() && row.manifest.descriptor_rows.size() == 1,
            "canonical character datatype row did not resolve");
    const auto& descriptor = row.manifest.descriptor_rows.front().descriptor_uuid;
    Require(descriptor.valid(), "canonical character descriptor UUID was nil");
    return std::vector<std::uint8_t>(descriptor.value.bytes.begin(),
                                     descriptor.value.bytes.end());
  }();
  return descriptor_identity;
}

void AppendLittleU64(std::vector<std::uint8_t>* output,
                     std::uint64_t value) {
  for (unsigned byte = 0; byte < 8; ++byte) {
    output->push_back(
        static_cast<std::uint8_t>((value >> (byte * 8)) & 0xffu));
  }
}

void AddOptionOperand(sblr::SblrOperationEnvelope* envelope,
                      std::string name,
                      std::string value) {
  sblr::SblrOperand operand;
  operand.ordinal = static_cast<std::uint32_t>(envelope->operands.size() + 1);
  operand.type = "option";
  operand.name = std::move(name);
  operand.value_kind = sblr::SblrValueKind::literal_typed;
  operand.value_body = CanonicalOptionDescriptorIdentity();
  AppendLittleU64(&operand.value_body, value.size());
  operand.value_body.insert(operand.value_body.end(), value.begin(), value.end());
  envelope->operands.push_back(std::move(operand));
}

sblr::SblrOperationEnvelope QueryEnvelope(std::string operation_id,
                                          std::string trace_key) {
  const auto* registry_entry = sblr::LookupSblrOperation(operation_id);
  Require(registry_entry != nullptr && registry_entry->code != 0,
          "canonical SBLR operation identity did not resolve");
  auto envelope = sblr::MakeSblrEnvelope(std::move(operation_id),
                                         registry_entry->opcode,
                                         std::move(trace_key));
  envelope.opcode_code = registry_entry->code;
  envelope.result_shape = registry_entry->result_contract;
  envelope.diagnostic_shape = "diagnostic_vector";
  envelope.parser_package_uuid = scratchbird::tests::FixtureUuidLiteral("019f0000-0000-7000-8000-000000080821");
  envelope.registry_snapshot_uuid = scratchbird::tests::FixtureUuidLiteral("019f0000-0000-7000-8000-000000080822");
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
      request.descriptors.push_back(
          Descriptor(operation == "cmp" ? "boolean" : "decimal"));
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

void RequireNullConsumerContracts(const api::EngineRequestContext& context) {
  const auto decimal = BoundScalarDescriptor(
      "decimal", 1, "precision=38;scale=2;nullability=nullable");
  const auto boolean = BoundScalarDescriptor("boolean", 2);
  api::EngineTypedValue null_decimal;
  null_decimal.descriptor = decimal;
  null_decimal.setState(api::EngineValueState::sql_null);
  api::EngineTypedValue decimal_one;
  decimal_one.descriptor = decimal;
  decimal_one.encoded_value = "1.00";

  api::EngineApplyNumericOperationRequest numeric;
  numeric.context = context;
  numeric.numeric_operation = "cmp";
  numeric.precision = 38;
  numeric.scale = 2;
  numeric.left_value = null_decimal;
  numeric.right_value = decimal_one;
  numeric.descriptors.push_back(boolean);
  const auto compared = api::EngineApplyNumericOperation(numeric);
  Require(compared.ok &&
              compared.value.state == api::EngineValueState::sql_null &&
              compared.value.is_null && compared.value.encoded_value.empty() &&
              compared.value.binary_value.empty() &&
              compared.value.descriptor == boolean,
          "NULL numeric comparison did not publish a bound Boolean NULL");

  numeric.descriptors.front() = decimal;
  Require(!api::EngineApplyNumericOperation(numeric).ok,
          "numeric comparison accepted a non-Boolean result descriptor");
  numeric.descriptors.front() = boolean;
  numeric.left_value.is_null = false;
  const auto malformed_comparison = api::EngineApplyNumericOperation(numeric);
  Require(!malformed_comparison.ok &&
              !malformed_comparison.diagnostics.empty() &&
              malformed_comparison.diagnostics.front().code ==
                  "DATATYPE.NULL_STATE.INVALID",
          "numeric comparison lost the canonical malformed NULL diagnostic");

  const auto date = BoundScalarDescriptor("date", 3);
  const auto int32 = BoundScalarDescriptor("int32", 4);
  api::EngineExtractValueRequest extract;
  extract.context = context;
  extract.field = "year";
  extract.input_value.descriptor = date;
  extract.input_value.encoded_value = "2026-09-30";
  extract.descriptors.push_back(int32);
  const auto present_extract = api::EngineExtractValue(extract);
  Require(present_extract.ok && present_extract.value.descriptor == int32 &&
              present_extract.value.state == api::EngineValueState::value &&
              present_extract.value.encoded_value == "2026",
          "present extraction did not preserve its bound result descriptor");
  extract.input_value.encoded_value.clear();
  extract.input_value.setState(api::EngineValueState::sql_null);
  const auto null_extract = api::EngineExtractValue(extract);
  Require(null_extract.ok && null_extract.value.descriptor == int32 &&
              null_extract.value.state == api::EngineValueState::sql_null &&
              null_extract.value.is_null &&
              null_extract.value.encoded_value.empty() &&
              null_extract.value.binary_value.empty(),
          "NULL extraction did not preserve its bound result descriptor");
  extract.input_value.binary_value.push_back(0);
  const auto dirty_extract = api::EngineExtractValue(extract);
  Require(!dirty_extract.ok && !dirty_extract.diagnostics.empty() &&
              dirty_extract.diagnostics.front().code ==
                  "DATATYPE.NULL_STATE.INVALID",
          "NULL extraction accepted a substitute binary payload");

  const auto character = BoundScalarDescriptor("character", 5);
  const auto character_execution =
      CoreExecutionDescriptor(dt::CanonicalTypeId::character, true);
  dt::DatatypeSetDescriptor set_descriptor;
  set_descriptor.element_type_id = dt::CanonicalTypeId::character;
  set_descriptor.element_descriptor = character_execution;
  set_descriptor.allow_null_elements = true;
  dt::DatatypeOperationValue printable_null{
      dt::CanonicalTypeId::character, "<NULL>", false};
  printable_null.descriptor = character_execution;
  dt::DatatypeOperationValue sql_null{
      dt::CanonicalTypeId::character, {}, true};
  sql_null.descriptor = character_execution;
  const auto encoded_set =
      dt::EncodeSetValue(set_descriptor, {printable_null, sql_null});
  Require(encoded_set.ok(), "engine set fixture did not encode");

  api::EngineSetOperationRequest membership;
  membership.context = context;
  membership.set_operation = "membership";
  membership.descriptors.push_back(character);
  membership.allow_null_elements = true;
  membership.left_set = TypedValue("set_value", encoded_set.encoded_set);
  membership.right_set_or_value.descriptor = character;
  membership.right_set_or_value.setState(api::EngineValueState::sql_null);
  const auto null_membership = api::EngineSetOperation(membership);
  Require(null_membership.ok &&
              null_membership.value.encoded_value == "true" &&
              !null_membership.value.is_null,
          "engine set membership lost descriptor-bound SQL NULL");

  auto dirty_null_membership = membership;
  dirty_null_membership.right_set_or_value.encoded_value = "payload";
  const auto dirty_null_set_result =
      api::EngineSetOperation(dirty_null_membership);
  Require(!dirty_null_set_result.ok &&
              !dirty_null_set_result.diagnostics.empty() &&
              dirty_null_set_result.diagnostics.front().code ==
                  "DATATYPE.NULL_STATE.INVALID",
          "engine set lost canonical dirty SQL NULL diagnostic precedence");
  auto dirty_null_left = membership;
  dirty_null_left.left_set.setState(api::EngineValueState::sql_null);
  const auto dirty_null_left_result = api::EngineSetOperation(dirty_null_left);
  Require(!dirty_null_left_result.ok &&
              !dirty_null_left_result.diagnostics.empty() &&
              dirty_null_left_result.diagnostics.front().code ==
                  "DATATYPE.NULL_STATE.INVALID",
          "engine set lost dirty left SQL NULL diagnostic precedence");

  membership.right_set_or_value = TypedValue("character", "<NULL>");
  membership.right_set_or_value.descriptor = character;
  const auto printable_membership = api::EngineSetOperation(membership);
  Require(printable_membership.ok &&
              printable_membership.value.encoded_value == "true",
          "engine set membership collapsed present <NULL> payload");

  auto wrong_left_type = membership;
  wrong_left_type.left_set.descriptor = character;
  const auto wrong_left_result = api::EngineSetOperation(wrong_left_type);
  Require(!wrong_left_result.ok,
          "engine set membership accepted SBSET2 bytes in a non-set carrier");

  auto wrong_right_set_type = membership;
  wrong_right_set_type.set_operation = "equals";
  wrong_right_set_type.right_set_or_value = wrong_right_set_type.left_set;
  wrong_right_set_type.right_set_or_value.descriptor = character;
  const auto wrong_right_result =
      api::EngineSetOperation(wrong_right_set_type);
  Require(!wrong_right_result.ok,
          "engine set equality accepted SBSET2 bytes in a non-set carrier");

  auto equality_request = membership;
  equality_request.set_operation = "equals";
  equality_request.right_set_or_value = equality_request.left_set;
  const auto equality_result = api::EngineSetOperation(equality_request);
  Require(equality_result.ok && equality_result.value.encoded_value == "true",
          "engine set equality did not populate the right encoded set");

  const auto binary = BoundScalarDescriptor(
      "binary", 7, "nullability=non_null");
  const auto binary_execution =
      CoreExecutionDescriptor(dt::CanonicalTypeId::binary, false);
  dt::DatatypeSetDescriptor binary_set_descriptor;
  binary_set_descriptor.element_type_id = dt::CanonicalTypeId::binary;
  binary_set_descriptor.element_descriptor = binary_execution;
  dt::DatatypeOperationValue binary_item{
      dt::CanonicalTypeId::binary,
      std::string{"\0\xff", 2},
      false};
  binary_item.descriptor = binary_execution;
  const auto encoded_binary_set =
      dt::EncodeSetValue(binary_set_descriptor, {binary_item});
  Require(encoded_binary_set.ok(), "binary set fixture did not encode");
  api::EngineSetOperationRequest binary_membership;
  binary_membership.context = context;
  binary_membership.set_operation = "membership";
  binary_membership.descriptors.push_back(binary);
  binary_membership.left_set =
      TypedValue("set_value", encoded_binary_set.encoded_set);
  binary_membership.right_set_or_value.descriptor = binary;
  binary_membership.right_set_or_value.binary_value = {0x00, 0xff};
  const auto binary_membership_result =
      api::EngineSetOperation(binary_membership);
  Require(binary_membership_result.ok &&
              binary_membership_result.value.encoded_value == "true",
          "engine set membership did not preserve native binary payload bytes");

  api::EngineSetOperationRequest cardinality_request;
  cardinality_request.context = context;
  cardinality_request.set_operation = "cardinality";
  cardinality_request.descriptors.push_back(character);
  cardinality_request.allow_null_elements = true;
  cardinality_request.left_set = membership.left_set;
  const auto set_cardinality = api::EngineSetOperation(cardinality_request);
  Require(set_cardinality.ok &&
              set_cardinality.value.encoded_value == "2",
          "engine unary set cardinality required a synthetic right operand");

  const auto nonnull_character = BoundScalarDescriptor(
      "character", 6, "nullability=non_null");
  membership.descriptors.front() = nonnull_character;
  membership.right_set_or_value.descriptor = nonnull_character;
  membership.right_set_or_value.setState(api::EngineValueState::sql_null);
  const auto forbidden_null_membership = api::EngineSetOperation(membership);
  Require(!forbidden_null_membership.ok &&
              !forbidden_null_membership.diagnostics.empty() &&
              forbidden_null_membership.diagnostics.front().code ==
                  "DATATYPE.NULL_NOT_ADMITTED",
          "engine set accepted nullable profile under non-null descriptor");
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

void RequireSblrDescriptorAuthorityBoundary(
    const api::EngineRequestContext& context) {
  auto numeric = QueryEnvelope("engine.op.query_apply_numeric_operation",
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
  Require(!dispatched.envelope_validated && !dispatched.accepted &&
              !dispatched.dispatched_to_api &&
              !dispatched.diagnostics.empty() &&
              dispatched.diagnostics.front().code == "SBLR.OPERAND_INVALID",
          "parser-authored numeric options bypassed the engine-bound descriptor");

  auto advanced = QueryEnvelope(
      "engine.op.query_evaluate_advanced_datatype_family",
                                "trace.cbq008.query.evaluate_advanced_datatype_family");
  AddOptionOperand(&advanced, "descriptor_type", "graph_path");
  AddOptionOperand(&advanced, "operation_kind", "graph.traverse");
  AddOptionOperand(&advanced, "index_kind", "graph_adjacency");
  AddOptionOperand(&advanced,
                   "descriptor_profile",
                   "direction=directed;schema_uuid=019f0000-0000-7000-8000-000000080801");
  dispatched = sblr::DispatchSblrOperation({context, advanced, api::EngineApiRequest{}});
  Require(!dispatched.envelope_validated && !dispatched.accepted &&
              !dispatched.dispatched_to_api &&
              !dispatched.diagnostics.empty() &&
              dispatched.diagnostics.front().code == "SBLR.OPERAND_INVALID",
          "parser-authored advanced datatype options bypassed the engine-bound "
          "descriptor");
}

sblr::SblrExecutionContext SblrContext() {
  sblr::SblrExecutionContext context;
  context.database_uuid = scratchbird::tests::FixtureUuid(1208, 4101);
  context.transaction_uuid = scratchbird::tests::FixtureUuid(1208, 4102);
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

sblr::SblrValue SblrSet(std::string encoded) {
  sblr::SblrValue value;
  value.descriptor_id = "set_value";
  value.payload_kind = sblr::SblrValuePayloadKind::descriptor_payload;
  value.is_null = false;
  value.encoded_value = std::move(encoded);
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

  const auto present_null_set = SblrSet(
      "SBSET2;element=character;descriptor=none;ordered=0;nulls=0;duplicates=0;items=V3c4e554c4c3e");
  const auto present_null_membership = sblr::EvaluateSblrCollectionOperator(
      "operator.collection.contains", present_null_set,
      SblrText("character", "<NULL>"), context);
  RequireOkScalar(present_null_membership,
                  "canonical SBSET2 present <NULL> membership refused");
  Require(present_null_membership.scalar_values.front().encoded_value == "TRUE",
          "canonical SBSET2 present <NULL> membership was not preserved");

  const auto empty_character_set = SblrSet(
      "SBSET2;element=character;descriptor=none;ordered=0;nulls=0;duplicates=0;items=V");
  const auto empty_character_membership =
      sblr::EvaluateSblrCollectionOperator(
          "operator.collection.contains", empty_character_set,
          SblrText("character", ""), context);
  RequireOkScalar(empty_character_membership,
                  "canonical empty character set element was refused");
  Require(empty_character_membership.scalar_values.front().encoded_value ==
              "TRUE",
          "canonical V token did not preserve an empty character value");

  const auto escaped_character_membership =
      sblr::EvaluateSblrCollectionOperator(
          "operator.collection.contains",
          SblrSet("SBSET2;element=character;descriptor=none;ordered=0;nulls=0;duplicates=0;items=V225c753030363122"),
          SblrText("character", R"json("\u0061")json"), context);
  RequireOkScalar(escaped_character_membership,
                  "SBSET2 escaped character membership refused");
  Require(escaped_character_membership.scalar_values.front().encoded_value ==
              "TRUE",
          "SBSET2 membership normalized character bytes through JSON");

  sblr::SblrValue null_candidate;
  null_candidate.descriptor_id = "character";
  null_candidate.is_null = true;
  const auto null_set_membership = sblr::EvaluateSblrCollectionOperator(
      "operator.collection.contains", present_null_set, null_candidate, context);
  Require(!null_set_membership.ok() &&
              !null_set_membership.diagnostics.empty() &&
              null_set_membership.diagnostics.front().diagnostic_id ==
                  "DATATYPE.CONTEXT_REQUIRED",
          "SBLR set membership invented authority for SQL NULL");

  auto dirty_null_candidate = null_candidate;
  dirty_null_candidate.encoded_value = present_null_set.encoded_value;
  const auto dirty_null_membership =
      sblr::EvaluateSblrCollectionOperator(
          "operator.collection.contains", present_null_set,
          dirty_null_candidate, context);
  Require(!dirty_null_membership.ok() &&
              !dirty_null_membership.diagnostics.empty() &&
              dirty_null_membership.diagnostics.front().diagnostic_id ==
                  "DATATYPE.CONTEXT_REQUIRED",
          "SBLR inspected dirty SQL NULL before resolving set authority");

  sblr::SblrValue null_set;
  null_set.descriptor_id = "set_value";
  null_set.is_null = true;
  const auto null_set_cardinality =
      sblr::EvaluateSblrCollectionOperator(
          "operator.collection.cardinality", null_set, SblrInt(0), context);
  Require(!null_set_cardinality.ok() &&
              !null_set_cardinality.diagnostics.empty() &&
              null_set_cardinality.diagnostics.front().diagnostic_id ==
                  "DATATYPE.CONTEXT_REQUIRED",
          "SBLR propagated descriptor-free SQL NULL set state");
  auto dirty_null_set = null_set;
  dirty_null_set.encoded_value = present_null_set.encoded_value;
  const auto dirty_null_set_cardinality =
      sblr::EvaluateSblrCollectionOperator(
          "operator.collection.cardinality", dirty_null_set, SblrInt(0),
          context);
  Require(!dirty_null_set_cardinality.ok() &&
              !dirty_null_set_cardinality.diagnostics.empty() &&
              dirty_null_set_cardinality.diagnostics.front().diagnostic_id ==
                  "DATATYPE.CONTEXT_REQUIRED",
          "SBLR inspected dirty SQL NULL set before resolving authority");

  const auto integer_set_membership = sblr::EvaluateSblrCollectionOperator(
      "operator.collection.contains",
      SblrSet("SBSET2;element=character;descriptor=none;ordered=0;nulls=0;duplicates=0;items=V31"),
      SblrInt(1), context);
  Require(!integer_set_membership.ok() &&
              !integer_set_membership.diagnostics.empty() &&
              integer_set_membership.diagnostics.front().diagnostic_id ==
                  "DATATYPE.DESCRIPTOR.INVALID",
          "SBLR matched a non-character scalar against a character set");

  const auto invalid_utf8_membership =
      sblr::EvaluateSblrCollectionOperator(
          "operator.collection.contains", present_null_set,
          SblrText("character", std::string(1, static_cast<char>(0xff))),
          context);
  Require(!invalid_utf8_membership.ok() &&
              !invalid_utf8_membership.diagnostics.empty() &&
              invalid_utf8_membership.diagnostics.front().diagnostic_id ==
                  "DATATYPE.DESCRIPTOR.INVALID",
          "SBLR character set membership accepted invalid UTF-8 bytes");

  auto shadow_character = SblrText("character", "<NULL>");
  shadow_character.uuid_value.bytes[0] = 1;
  const auto shadow_character_membership =
      sblr::EvaluateSblrCollectionOperator(
          "operator.collection.contains", present_null_set,
          shadow_character, context);
  Require(!shadow_character_membership.ok() &&
              !shadow_character_membership.diagnostics.empty() &&
              shadow_character_membership.diagnostics.front().diagnostic_id ==
                  "DATATYPE.DESCRIPTOR.INVALID",
          "SBLR character membership accepted a UUID shadow carrier");

  const auto disguised_set = sblr::EvaluateSblrCollectionOperator(
      "operator.collection.cardinality",
      SblrText("character", present_null_set.encoded_value), SblrInt(0),
      context);
  Require(!disguised_set.ok() && !disguised_set.diagnostics.empty() &&
              disguised_set.diagnostics.front().diagnostic_id ==
                  "SBLR.OPERAND_INVALID",
          "SBLR reclassified SBSET2 bytes from an arbitrary descriptor");

  const auto wrong_payload_set = sblr::EvaluateSblrCollectionOperator(
      "operator.collection.cardinality",
      SblrText("set_value", present_null_set.encoded_value), SblrInt(0),
      context);
  Require(!wrong_payload_set.ok() &&
              !wrong_payload_set.diagnostics.empty() &&
              wrong_payload_set.diagnostics.front().diagnostic_id ==
                  "SBLR.OPERAND_INVALID",
          "SBLR accepted a set frame through the text payload carrier");

  auto shadow_set = present_null_set;
  shadow_set.charset_name = "UTF8";
  shadow_set.uuid_array_value.push_back({});
  const auto shadow_set_result = sblr::EvaluateSblrCollectionOperator(
      "operator.collection.cardinality", shadow_set, SblrInt(0), context);
  Require(!shadow_set_result.ok() &&
              !shadow_set_result.diagnostics.empty() &&
              shadow_set_result.diagnostics.front().diagnostic_id ==
                  "SBLR.OPERAND_INVALID",
          "SBLR accepted set bytes with shadow carrier metadata");

  const auto unordered_subscript = sblr::EvaluateSblrCollectionOperator(
      "operator.collection.subscript", present_null_set, SblrInt(1), context);
  Require(!unordered_subscript.ok() &&
              !unordered_subscript.diagnostics.empty() &&
              unordered_subscript.diagnostics.front().diagnostic_id ==
                  "SBLR.OPERAND_INVALID",
          "SBLR assigned an ordinal to an unordered SBSET2 value");

  for (const auto operation : {"operator.collection.overlap",
                               "operator.collection.concat"}) {
    const auto mixed = sblr::EvaluateSblrCollectionOperator(
        operation, present_null_set, array, context);
    Require(!mixed.ok() && !mixed.diagnostics.empty() &&
                mixed.diagnostics.front().diagnostic_id ==
                    "SBLR.OPERAND_INVALID",
            "SBLR implicitly converted a mixed set/array operation");
  }

  std::string invalid_utf8_frame =
      "SBSET2;element=character;descriptor=none;ordered=0;nulls=0;duplicates=0;items=V";
  invalid_utf8_frame += "ff";
  const auto invalid_utf8_set = sblr::EvaluateSblrCollectionOperator(
      "operator.collection.cardinality", SblrSet(invalid_utf8_frame),
      SblrInt(0), context);
  Require(!invalid_utf8_set.ok() && !invalid_utf8_set.diagnostics.empty() &&
              invalid_utf8_set.diagnostics.front().diagnostic_id ==
                  "SBLR.OPERAND_INVALID",
          "SBLR character set accepted invalid UTF-8 bytes");

  const std::vector<std::string> invalid_set_frames = {
      "SBSET1;element=character;ordered=0;nulls=0;duplicates=0;items=3c4e554c4c3e",
      "SBSET2;element=character;descriptor=none;ordered=0;nulls=1;duplicates=0;items=N",
      "SBSET2;element=character;descriptor=none;ordered=0;nulls=0;duplicates=0;items=V3c4E554c4c3e",
      "SBSET2;element=character;descriptor=none;ordered=0;nulls=0;duplicates=0;items=V3c4e554c4c3e,",
  };
  for (const auto& frame : invalid_set_frames) {
    const auto invalid_set = sblr::EvaluateSblrCollectionOperator(
        "operator.collection.cardinality", SblrSet(frame),
        SblrInt(0), context);
    Require(!invalid_set.ok() && !invalid_set.diagnostics.empty() &&
                invalid_set.diagnostics.front().diagnostic_id ==
                    "SBLR.OPERAND_INVALID",
            "SBLR accepted a legacy, nullable, or noncanonical set frame");
  }

  const auto concatenated_set = sblr::EvaluateSblrCollectionOperator(
      "operator.collection.concat", present_null_set, present_null_set, context);
  RequireOkScalar(concatenated_set, "canonical SBSET2 concat refused");
  Require(concatenated_set.scalar_values.front().encoded_value ==
              present_null_set.encoded_value,
          "SBLR set emitter did not preserve canonical SBSET2 deduplication");

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

api::EngineColumnDefinition Column(const api::EngineRequestContext& context,
                                  std::string name, std::string type, std::uint32_t ordinal) {
  api::EngineColumnDefinition column;
  column.requested_column_uuid =
      scratchbird::tests::FixtureUuid(1385, ordinal + 1);
  column.names.push_back(Name(std::move(name)));
  column.descriptor = Descriptor(std::move(type));
  if (column.descriptor.canonical_type_name == "text") {
    column.descriptor.encoded_descriptor = "type=text";
  }
  column.ordinal = ordinal;
  column.nullable = true;
  scratchbird::tests::BindFixtureColumnDatatype(context,
      dt::CanonicalTypeIdFromStableName(column.descriptor.canonical_type_name), column);
  return column;
}

api::EngineIndexDefinition Index(api::EngineUuid index_uuid,
                                 std::string name,
                                 std::string index_kind,
                                 std::vector<std::string> keys) {
  api::EngineIndexDefinition index;
  index.requested_index_uuid = index_uuid;
  index.names.push_back(Name(std::move(name)));
  index.index_kind = std::move(index_kind);
  index.key_envelopes = std::move(keys);
  return index;
}

api::EngineCreateTableResult CreateTable(const api::EngineRequestContext& context,
                                         std::vector<api::EngineIndexDefinition> inline_indexes = {}) {
  api::EngineCreateTableRequest request;
  request.context = context;
  request.target_schema.uuid = scratchbird::tests::FixtureUuidLiteral("019f0000-0000-7000-8000-000000080801");
  request.target_schema.object_kind = "schema";
  request.requested_table_uuid = scratchbird::tests::FixtureUuidLiteral("019f0000-0000-7000-8000-000000080802");
  request.table_names.push_back(Name("cbq008_table"));
  request.table_columns.push_back(Column(context, "id", "int64", 0));
  request.table_columns.push_back(Column(context, "geom", "point", 1));
  request.table_columns.push_back(Column(context, "embedding", "dense_vector", 2));
  request.table_columns.push_back(Column(context, "body", "text", 3));
  request.table_columns.push_back(Column(context, "observed_at", "timestamp", 4));
  request.table_columns.push_back(Column(context, "graph_path", "graph_path", 5));
  request.table_columns.push_back(Column(context, "secret_payload", "opaque_extension", 6));
  request.table_indexes = std::move(inline_indexes);
  return api::EngineCreateTable(request);
}

api::EngineCreateIndexResult CreateIndex(const api::EngineRequestContext& context,
                                         api::EngineIndexDefinition index) {
  api::EngineCreateIndexRequest request;
  request.context = context;
  request.target_object.uuid = scratchbird::tests::FixtureUuidLiteral("019f0000-0000-7000-8000-000000080802");
  request.target_object.object_kind = "table";
  request.indexes.push_back(std::move(index));
  return api::EngineCreateIndex(request);
}

void RequireIndexFamilyPersisted(const api::EngineRequestContext& context,
                                 const api::EngineUuid& index_uuid,
                                 std::string_view family) {
  const auto loaded = api::LoadMgaRelationStoreState(context);
  Require(loaded.ok, "MGA relation metadata load failed");
  const api::MgaRelationReadView state = api::BuildMgaRelationReadView(loaded.state);
  for (const auto& index : state.indexes) {
    if (index.index_uuid == index_uuid) {
      Require(index.family == family, "persisted index family drifted");
      return;
    }
  }
  Require(false, "persisted index metadata not found");
}

void RequireAdvancedIndexDDL(const api::EngineRequestContext& context) {
  auto missing_filespace = context;
  missing_filespace.default_root_uuid = {};
  const auto missing = CreateTable(missing_filespace, {});
  Require(!missing.ok && FirstDetail(missing) ==
              "ddl.create_table:bound_primary_filespace_identity_required",
          "missing filespace identity must refuse before table publication instead of terminating");
  const auto table = CreateTable(context, {Index(scratchbird::tests::FixtureUuidLiteral("019f0000-0000-7000-8000-000000080803"), "inline_geom_idx", "rtree", {"geom"})});
  if (!table.ok) { std::cerr << FirstDetail(table) << '\n'; }
  Require(table.ok, "create table with inline advanced index failed");
  Require(HasEvidence(table, "inline_index_create", kInlineIndexUuid),
          "inline index create evidence missing");
  RequireIndexFamilyPersisted(context, scratchbird::tests::FixtureUuidLiteral("019f0000-0000-7000-8000-000000080803"), "rtree");

  const std::vector<std::pair<api::EngineIndexDefinition, std::string>> positive_indexes = {
      {Index(scratchbird::tests::FixtureUuidLiteral("019f0000-0000-7000-8000-000000080821"), "embedding_hnsw_idx", "hnsw", {"embedding"}), "vector_hnsw"},
      {Index(scratchbird::tests::FixtureUuidLiteral("019f0000-0000-7000-8000-000000080822"), "embedding_ivfflat_idx", "ivfflat", {"embedding"}), "vector_ivf"},
      {Index(scratchbird::tests::FixtureUuidLiteral("019f0000-0000-7000-8000-000000080823"), "body_inverted_idx", "inverted", {"body"}), "inverted"},
      {Index(scratchbird::tests::FixtureUuidLiteral("019f0000-0000-7000-8000-000000080824"), "observed_time_partition_idx", "time_partition", {"observed_at"}), "columnar_zone"},
      {Index(scratchbird::tests::FixtureUuidLiteral("019f0000-0000-7000-8000-000000080825"), "body_expression_idx", "expression", {"lower:body"}), "expression"},
      {Index(scratchbird::tests::FixtureUuidLiteral("019f0000-0000-7000-8000-000000080826"), "id_partial_idx", "partial", {"id", "where_eq:id=42"}), "partial"},
      {Index(scratchbird::tests::FixtureUuidLiteral("019f0000-0000-7000-8000-000000080827"), "graph_adjacency_idx", "adjacency", {"graph_path"}), "graph_adjacency"},
      {Index(scratchbird::tests::FixtureUuidLiteral("019f0000-0000-7000-8000-000000080828"), "graph_adjacency_profile_idx", "graph_adjacency", {"graph_path"}), "graph_adjacency"},
      {Index(scratchbird::tests::FixtureUuidLiteral("019f0000-0000-7000-8000-000000080834"), "full_partial_idx", "partial", {"id"}), "partial"},
      {Index(scratchbird::tests::FixtureUuidLiteral("019f0000-0000-7000-8000-000000080836"), "policy_blocked_idx", "policy_blocked", {"id"}), "policy_blocked"},
  };
  for (const auto& [definition, expected_family] : positive_indexes) {
    const auto index_uuid = definition.requested_index_uuid;
    const auto result = CreateIndex(context, definition);
    if (!result.ok) { std::cerr << expected_family << ':' << FirstDetail(result) << '\n'; }
    Require(result.ok, "supported advanced create-index family failed");
    Require(HasEvidence(result, "index_family", expected_family),
            "create-index family evidence drifted");
    RequireIndexFamilyPersisted(context, index_uuid, expected_family);
  }

  auto rejected = CreateIndex(context, Index(scratchbird::tests::FixtureUuidLiteral("019f0000-0000-7000-8000-000000080831"), "bad_profile_idx", "mystery", {"id"}));
  Require(!rejected.ok && FirstDetail(rejected) == "ddl.create_index:unsupported_index_profile",
          "unsupported index profile diagnostic drifted");

  rejected = CreateIndex(context, Index(scratchbird::tests::FixtureUuidLiteral("019f0000-0000-7000-8000-000000080832"), "missing_key_idx", "hnsw", {}));
  Require(!rejected.ok && FirstDetail(rejected) == "ddl.create_index:at_least_one_key_envelope_required",
          "missing key envelope diagnostic drifted");

  rejected = CreateIndex(context, Index(scratchbird::tests::FixtureUuidLiteral("019f0000-0000-7000-8000-000000080833"), "bad_expression_idx", "expression", {"substr:body"}));
  Require(!rejected.ok && FirstDetail(rejected) == "ddl.create_index:unsupported_expression_index_envelope",
          "unsupported expression index diagnostic drifted");

  rejected = CreateIndex(context, Index(scratchbird::tests::FixtureUuidLiteral("019f0000-0000-7000-8000-000000080835"), "opaque_idx", "btree", {"secret_payload"}));
  if (rejected.ok || FirstDetail(rejected) != "ddl.create_index:opaque_column_index_denied")
    std::cerr << "opaque index ok=" << rejected.ok << " diagnostic=" << FirstDetail(rejected) << '\n';
  Require(!rejected.ok && FirstDetail(rejected) == "ddl.create_index:opaque_column_index_denied",
          "opaque column index diagnostic drifted");

  const std::vector<std::pair<std::string, std::vector<std::string>>> opaque_profiles = {
      {"btree", {"id", "include:secret_payload"}},
      {"partial", {"id", "where_eq:secret_payload=42"}},
      {"partial", {"id", "where_mod_eq:secret_payload:2=0"}},
      {"expression", {"lower:secret_payload"}},
      {"btree", {"desc:secret_payload"}},
  };
  for (std::size_t i = 0; i < opaque_profiles.size(); ++i) {
    const auto index_uuid = scratchbird::tests::FixtureUuid(1386, i + 1);
    const auto& [family, keys] = opaque_profiles[i];
    const auto result = CreateIndex(context, Index(index_uuid,
        "opaque_indirect_" + std::to_string(i), family, keys));
    if (result.ok || FirstDetail(result) != "ddl.create_index:opaque_column_index_denied")
      std::cerr << "opaque profile=" << i << " diagnostic=" << FirstDetail(result) << '\n';
    Require(!result.ok && FirstDetail(result) == "ddl.create_index:opaque_column_index_denied",
            "indirect opaque column index was not refused");
    const auto loaded = api::LoadMgaRelationStoreState(context);
    Require(loaded.ok, "MGA metadata load after refused index failed");
    const auto state = api::BuildMgaRelationReadView(loaded.state);
    Require(state.indexes.size() == positive_indexes.size() + 1,
            "refused opaque index changed persisted index count");
    for (const auto& index : state.indexes)
      Require(index.index_uuid != index_uuid, "refused opaque index retained metadata");
  }
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

  const auto require_bounded_signed_integer =
      [&](const std::string& type,
          const std::string& minimum,
          const std::string& maximum,
          const std::string& underflow,
          const std::string& overflow) {
        const auto descriptor = exec::MakeExecutorDescriptor(type);
        for (const auto& accepted : {minimum, maximum}) {
          const auto value =
              exec::MakeExecutorValue(descriptor, accepted, false);
          const auto decoded = exec::DecodeInt64Value(value);
          Require(decoded.ok() && std::to_string(decoded.value) == accepted,
                  "bounded signed integer endpoint decode failed");

          exec::DescriptorBatch batch;
          batch.columns.push_back({"value", descriptor, false, 0});
          batch.rows.push_back({{value}});
          Require(exec::ValidateDescriptorBatch(batch).ok,
                  "bounded signed integer endpoint batch validation failed");

          const auto cast = exec::CastDescriptorValue(
              exec::EncodeTextValue(accepted), descriptor, &diagnostic);
          Require(diagnostic.ok && cast.encoded_value == accepted,
                  "bounded signed integer endpoint cast failed");
        }

        for (const auto& refused : {underflow, overflow}) {
          const auto value =
              exec::MakeExecutorValue(descriptor, refused, false);
          Require(!exec::DecodeInt64Value(value).ok(),
                  "bounded signed integer overflow decode was accepted");

          exec::DescriptorBatch batch;
          batch.columns.push_back({"value", descriptor, false, 0});
          batch.rows.push_back({{value}});
          const auto batch_diagnostic = exec::ValidateDescriptorBatch(batch);
          Require(!batch_diagnostic.ok &&
                      batch_diagnostic.diagnostic_code ==
                          "SB_EXECUTOR_INT64_DECODE_FAILED",
                  "bounded signed integer overflow batch was accepted");

          const auto cast = exec::CastDescriptorValue(
              exec::EncodeTextValue(refused), descriptor, &diagnostic);
          Require(!diagnostic.ok && cast.descriptor.canonical_type_name.empty(),
                  "bounded signed integer overflow cast was accepted");
        }
      };

  require_bounded_signed_integer("int8", "-128", "127", "-129", "128");
  require_bounded_signed_integer(
      "int16", "-32768", "32767", "-32769", "32768");
  require_bounded_signed_integer(
      "smallint", "-32768", "32767", "-32769", "32768");
  require_bounded_signed_integer("int32",
                                 "-2147483648",
                                 "2147483647",
                                 "-2147483649",
                                 "2147483648");
  require_bounded_signed_integer("integer",
                                 "-2147483648",
                                 "2147483647",
                                 "-2147483649",
                                 "2147483648");
  require_bounded_signed_integer("int64",
                                 "-9223372036854775808",
                                 "9223372036854775807",
                                 "-9223372036854775809",
                                 "9223372036854775808");
  require_bounded_signed_integer("bigint",
                                 "-9223372036854775808",
                                 "9223372036854775807",
                                 "-9223372036854775809",
                                 "9223372036854775808");

  exec::DescriptorBatch empty_integer_batch;
  empty_integer_batch.columns.push_back(
      {"integer_value", exec::MakeExecutorDescriptor("integer"), true, 0});
  Require(exec::ValidateDescriptorBatch(empty_integer_batch).ok,
          "empty ordinary integer descriptor batch was rejected");

  const auto int64_two_hundred = exec::MakeExecutorValue(
      exec::MakeExecutorDescriptor("int64"), "200", false);
  result = exec::CastDescriptorValue(
      int64_two_hundred, exec::MakeExecutorDescriptor("int8"), &diagnostic);
  Require(!diagnostic.ok && result.descriptor.canonical_type_name.empty(),
          "narrow signed integer cast accepted out-of-range source value");

  result = exec::ExtractDescriptorField(exec::EncodeTextValue("hello"), "length", &diagnostic);
  Require(diagnostic.ok && result.descriptor.canonical_type_name == "uint64" &&
              result.encoded_value == "5",
          "descriptor text length extract failed");

  auto native_binary = exec::MakeExecutorValue(exec::MakeExecutorDescriptor("binary"), {}, false);
  native_binary.binary_value = {'a', 'b', 'c', 'd'};
  result = exec::ExtractDescriptorField(native_binary,
                                        "octet_length",
                                        &diagnostic);
  Require(diagnostic.ok && result.descriptor.canonical_type_name == "uint64" &&
              result.encoded_value == "4",
          "descriptor binary octet length extract failed");

  auto native_uuid = exec::MakeExecutorValue(exec::MakeExecutorDescriptor("uuid"), {}, false);
  constexpr auto uuid_data = scratchbird::tests::FixtureUuidLiteral("550e8400-e29b-41d4-a716-446655440000");
  native_uuid.binary_value.assign(uuid_data.bytes.begin(), uuid_data.bytes.end());
  result = exec::ExtractDescriptorField(native_uuid,
      "version",
      &diagnostic);
  Require(diagnostic.ok && result.descriptor.canonical_type_name == "uint8" &&
              result.encoded_value == "4",
          "descriptor uuid version extract failed");
}

}  // namespace

int main(int argc, char** argv) {
  auto memory_policy = scratchbird::core::memory::DefaultLocalEngineMemoryPolicy();
  memory_policy.policy_name = "advanced_datatype_fixture";
  Require(scratchbird::core::memory::ConfigureDefaultMemoryManagerForFixture(
      memory_policy, "advanced_datatype_fixture").ok(), "fixture memory configuration failed");
  if (argc == 2 &&
      std::string_view(argv[1]) ==
          "--bounded-signed-integer-descriptor-only") {
    RequireDescriptorRuntimeDatatypeSlice();
    std::cout << "bounded_signed_integer_descriptor_execution=passed\n";
    return EXIT_SUCCESS;
  }

  api::EngineRequestContext api_context;
  api_context.request_id = "sbsql-advanced-datatype-operation-api";
  api_context.security_context_present = true;

  RequireNumericOperations(api_context);
  RequireNullConsumerContracts(api_context);
  RequireAdvancedFamilies(api_context);
  RequireSblrDescriptorAuthorityBoundary(api_context);
  RequireSblrCollectionVectorSpecializedOperators();
  RequireDescriptorRuntimeDatatypeSlice();

  const auto path = TestDatabasePath();
  RemoveDatabaseArtifacts(path);
  const auto context = BeginTransaction(EngineContext(path, CreateFixtureDatabase(path)));
  CreateSchema(context);
  RequireAdvancedIndexDDL(context);
  RemoveDatabaseArtifacts(path);
  return EXIT_SUCCESS;
}
