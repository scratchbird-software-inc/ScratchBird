// Copyright (c) 2026 ScratchBird Software Inc.
//
// This Source Code Form is subject to the terms of the Mozilla Public
// License, v. 2.0. If a copy of the MPL was not distributed with this
// file, You can obtain one at https://mozilla.org/MPL/2.0/.
//
// SPDX-License-Identifier: MPL-2.0

#include "database_lifecycle.hpp"
#include "datatype_operations.hpp"
#include "domain_support/domain_store.hpp"
#include "memory.hpp"
#include "query/expression_api.hpp"
#include "sbl_numeric.hpp"
#include "sblr_domain_runtime.hpp"
#include "transaction/transaction_api.hpp"
#include "uuid.hpp"

#include <filesystem>
#include <iostream>
#include <string>
#include <string_view>
#include <vector>

namespace {

namespace api = scratchbird::engine::internal_api;
namespace db = scratchbird::storage::database;
namespace dt = scratchbird::core::datatypes;
namespace memory = scratchbird::core::memory;
namespace numeric = scratchbird::libraries::sbl_numeric;
namespace sblr = scratchbird::engine::sblr;
namespace uuid = scratchbird::core::uuid;

using scratchbird::core::platform::TypedUuid;
using scratchbird::core::platform::UuidKind;
using scratchbird::core::platform::u64;

constexpr u64 kBaseMillis = 1771200000000ull;
constexpr scratchbird::core::platform::u32 kPageSize = 16384;

struct DatabaseFixture {
  std::filesystem::path root;
  std::filesystem::path path;
  TypedUuid database_uuid;
  TypedUuid filespace_uuid;
};

struct CleanupDir {
  std::filesystem::path root;
  ~CleanupDir() {
    std::error_code ignored;
    std::filesystem::remove_all(root, ignored);
  }
};

bool Expect(bool condition, std::string_view message) {
  if (!condition) {
    std::cerr << message << '\n';
    return false;
  }
  return true;
}

std::string DiagnosticText(const api::EngineApiResult& result) {
  if (result.diagnostics.empty()) {
    return {};
  }
  const auto& diagnostic = result.diagnostics.front();
  return diagnostic.code + ":" + diagnostic.message_key + ":" + diagnostic.detail;
}

bool ExpectApiOk(const api::EngineApiResult& result, std::string_view message) {
  if (result.ok) {
    return true;
  }
  std::cerr << message << ": " << DiagnosticText(result) << '\n';
  return false;
}

bool ExpectDiagnosticDetail(const api::EngineApiResult& result,
                            std::string_view detail,
                            std::string_view message) {
  if (result.ok) {
    std::cerr << message << ": result unexpectedly succeeded\n";
    return false;
  }
  for (const auto& diagnostic : result.diagnostics) {
    if (diagnostic.detail.find(detail) != std::string::npos) {
      return true;
    }
  }
  std::cerr << message << ": diagnostic mismatch: " << DiagnosticText(result) << '\n';
  return false;
}

bool ExpectDiagnosticDetail(const api::EngineApiDiagnostic& diagnostic,
                            std::string_view detail,
                            std::string_view message) {
  if (!diagnostic.error) {
    std::cerr << message << ": diagnostic unexpectedly succeeded\n";
    return false;
  }
  if (diagnostic.detail.find(detail) != std::string::npos) {
    return true;
  }
  std::cerr << message << ": diagnostic mismatch: " << diagnostic.code << ':'
            << diagnostic.message_key << ':' << diagnostic.detail << '\n';
  return false;
}

bool HasEvidence(const api::EngineApiResult& result,
                 std::string_view kind,
                 std::string_view id) {
  for (const auto& evidence : result.evidence) {
    if (evidence.evidence_kind == kind && evidence.evidence_id == id) {
      return true;
    }
  }
  return false;
}

memory::AllocationPolicy MemoryPolicy() {
  memory::AllocationPolicy policy;
  policy.policy_name = "public_domain_psql_exact_numeric_gate";
  policy.hard_limit_bytes = 64ull * 1024ull * 1024ull;
  policy.soft_limit_bytes = 48ull * 1024ull * 1024ull;
  policy.per_context_limit_bytes = 32ull * 1024ull * 1024ull;
  policy.page_buffer_pool_limit_bytes = 16ull * 1024ull * 1024ull;
  policy.track_allocations = true;
  policy.zero_memory_on_release = true;
  return policy;
}

bool ConfigureMemoryFixture() {
  const auto configured =
      memory::ConfigureDefaultMemoryManagerForFixture(
          MemoryPolicy(), "public_domain_psql_exact_numeric_gate");
  return Expect(configured.ok(),
                "PCR-041 memory manager should configure") &&
         Expect(configured.fixture_mode,
                "PCR-041 memory manager should use fixture mode");
}

TypedUuid MakeUuid(UuidKind kind, u64 offset) {
  const auto generated = uuid::GenerateEngineIdentityV7(kind, kBaseMillis + offset);
  return generated.ok() ? generated.value : TypedUuid{};
}

std::string UuidText(TypedUuid typed_uuid) {
  return uuid::UuidToString(typed_uuid.value);
}

std::string UuidText(UuidKind kind, u64 offset) {
  return UuidText(MakeUuid(kind, offset));
}

DatabaseFixture CreateDatabaseFixture(const std::filesystem::path& root) {
  DatabaseFixture fixture;
  fixture.root = root;
  fixture.path = root / "pcr041_domain_numeric.sbdb";
  fixture.database_uuid = MakeUuid(UuidKind::database, 1);
  fixture.filespace_uuid = MakeUuid(UuidKind::filespace, 2);
  std::filesystem::create_directories(root);

  db::DatabaseCreateConfig create;
  create.path = fixture.path.string();
  create.database_uuid = fixture.database_uuid;
  create.filespace_uuid = fixture.filespace_uuid;
  create.page_size = kPageSize;
  create.creation_unix_epoch_millis = kBaseMillis;
  create.require_resource_seed_pack = false;
  create.allow_minimal_resource_bootstrap = true;
  create.allow_overwrite = true;
  const auto created = db::CreateDatabaseFile(create);
  if (!created.ok()) {
    std::cerr << created.diagnostic.diagnostic_code << ':'
              << created.diagnostic.message_key << '\n';
  }
  return fixture;
}

api::EngineRequestContext Context(const DatabaseFixture& fixture,
                                  std::string request_id) {
  api::EngineRequestContext context;
  context.trust_mode = api::EngineTrustMode::server_isolated;
  context.request_id = std::move(request_id);
  context.database_path = fixture.path.string();
  context.database_uuid.canonical = UuidText(fixture.database_uuid);
  context.node_uuid.canonical = UuidText(UuidKind::object, 3);
  context.principal_uuid.canonical = UuidText(UuidKind::principal, 4);
  context.session_uuid.canonical = UuidText(UuidKind::object, 5);
  context.security_context_present = true;
  context.identifier_profile_uuid = "sbsql_v3";
  context.language_context.language_tag = "en";
  context.language_context.default_language_tag = "en";
  context.catalog_generation_id = 1;
  context.security_epoch = 1;
  context.resource_epoch = 1;
  context.name_resolution_epoch = 1;
  return context;
}

bool Begin(api::EngineRequestContext* context) {
  api::EngineBeginTransactionRequest request;
  request.context = *context;
  request.isolation_level = "read_committed";
  const auto begun = api::EngineBeginTransaction(request);
  if (!ExpectApiOk(begun, "PCR-041 transaction begin failed")) {
    return false;
  }
  context->local_transaction_id = begun.local_transaction_id;
  context->transaction_uuid = begun.transaction_uuid;
  context->snapshot_visible_through_local_transaction_id =
      begun.snapshot_visible_through_local_transaction_id;
  context->transaction_isolation_level = begun.isolation_level;
  return true;
}

bool Commit(api::EngineRequestContext* context) {
  api::EngineCommitTransactionRequest request;
  request.context = *context;
  const auto committed = api::EngineCommitTransaction(request);
  if (!ExpectApiOk(committed, "PCR-041 transaction commit failed")) {
    return false;
  }
  context->local_transaction_id = 0;
  context->transaction_uuid.canonical.clear();
  return true;
}

api::EngineDescriptor ScalarDescriptor(std::string canonical_type_name) {
  api::EngineDescriptor descriptor;
  descriptor.descriptor_kind = "scalar";
  descriptor.canonical_type_name = std::move(canonical_type_name);
  descriptor.encoded_descriptor = "canonical=" + descriptor.canonical_type_name;
  return descriptor;
}

api::EngineTypedValue TypedValue(std::string canonical_type_name,
                                 std::string encoded_value) {
  api::EngineTypedValue value;
  value.descriptor = ScalarDescriptor(std::move(canonical_type_name));
  value.encoded_value = std::move(encoded_value);
  value.is_null = false;
  return value;
}

api::DomainRecord Domain(std::uint64_t creator_tx,
                         std::string domain_uuid,
                         std::string base_type,
                         std::string check_envelope,
                         std::string method_binding = {},
                         std::string cast_policy = {}) {
  api::DomainRecord record;
  record.creator_tx = creator_tx;
  record.domain_uuid = std::move(domain_uuid);
  record.catalog_row_uuid = UuidText(UuidKind::object, 100 + creator_tx);
  record.schema_uuid = UuidText(UuidKind::schema, 200 + creator_tx);
  record.default_name = "pcr041_domain_cache";
  record.base_descriptor_uuid = UuidText(UuidKind::object, 300 + creator_tx);
  record.base_descriptor_kind = "scalar";
  record.base_canonical_type_name = std::move(base_type);
  record.base_encoded_descriptor = "canonical=" + record.base_canonical_type_name;
  record.nullable = false;
  record.check_constraint_envelope = std::move(check_envelope);
  record.method_binding_envelope = std::move(method_binding);
  record.cast_policy_envelope = std::move(cast_policy);
  record.validation_hook_status = "sblr_builtin";
  return record;
}

bool ExpectNumericBackend(numeric::NumericType type,
                          numeric::NumericOperation operation,
                          std::string left,
                          std::string right,
                          std::string expected_value,
                          int expected_comparison = 0) {
  numeric::NumericRequest request;
  request.type = type;
  request.operation = operation;
  request.left = {type, std::move(left), false};
  request.right = {type, std::move(right), false};
  request.context.precision = 38;
  request.context.scale = type == numeric::NumericType::decimal ||
                                  type == numeric::NumericType::decimal_float
                              ? 2
                              : 0;
  request.context.allow_special_values = true;
  const auto result = numeric::ApplyNumericOperation(request);
  if (!Expect(result.status == numeric::NumericStatusCode::ok,
              std::string("sbl_numeric operation failed for ") +
                  numeric::NumericTypeName(type))) {
    std::cerr << result.diagnostic_code << '\n';
    return false;
  }
  if (!Expect(result.value.encoded == expected_value,
              std::string("sbl_numeric value mismatch for ") +
                  numeric::NumericTypeName(type))) {
    std::cerr << "expected=" << expected_value << " actual="
              << result.value.encoded << '\n';
    return false;
  }
  if (operation == numeric::NumericOperation::compare) {
    return Expect(result.comparison == expected_comparison,
                  "sbl_numeric comparison mismatch");
  }
  return true;
}

dt::CanonicalTypeId ToDatatype(numeric::NumericType type) {
  switch (type) {
    case numeric::NumericType::decimal:
      return dt::CanonicalTypeId::decimal;
    case numeric::NumericType::decimal_float:
      return dt::CanonicalTypeId::decimal_float;
    case numeric::NumericType::real128:
      return dt::CanonicalTypeId::real128;
    case numeric::NumericType::int128:
      return dt::CanonicalTypeId::int128;
    case numeric::NumericType::uint128:
      return dt::CanonicalTypeId::uint128;
  }
  return dt::CanonicalTypeId::unknown;
}

dt::DatatypeNumericOperationKind ToDatatype(numeric::NumericOperation operation) {
  switch (operation) {
    case numeric::NumericOperation::canonicalize:
      return dt::DatatypeNumericOperationKind::canonicalize;
    case numeric::NumericOperation::add:
      return dt::DatatypeNumericOperationKind::add;
    case numeric::NumericOperation::subtract:
      return dt::DatatypeNumericOperationKind::subtract;
    case numeric::NumericOperation::multiply:
      return dt::DatatypeNumericOperationKind::multiply;
    case numeric::NumericOperation::divide:
      return dt::DatatypeNumericOperationKind::divide;
    case numeric::NumericOperation::compare:
      return dt::DatatypeNumericOperationKind::compare;
  }
  return dt::DatatypeNumericOperationKind::canonicalize;
}

bool ExpectDatatypeNumeric(numeric::NumericType type,
                           numeric::NumericOperation operation,
                           std::string left,
                           std::string right,
                           std::string expected_value,
                           int expected_comparison = 0) {
  dt::DatatypeNumericOperationRequest request;
  request.type_id = ToDatatype(type);
  request.operation = ToDatatype(operation);
  request.left = {request.type_id, std::move(left), false};
  request.right = {request.type_id, std::move(right), false};
  request.context.precision = 38;
  request.context.scale = type == numeric::NumericType::decimal ||
                                  type == numeric::NumericType::decimal_float
                              ? 2
                              : 0;
  request.context.allow_special_values = true;
  const auto result = dt::ApplyNumericOperation(request);
  if (!Expect(result.ok(),
              std::string("datatype numeric operation failed for ") +
                  numeric::NumericTypeName(type))) {
    std::cerr << result.diagnostic.diagnostic_code << '\n';
    return false;
  }
  if (!Expect(result.value.encoded_value == expected_value,
              std::string("datatype numeric value mismatch for ") +
                  numeric::NumericTypeName(type))) {
    std::cerr << "expected=" << expected_value << " actual="
              << result.value.encoded_value << '\n';
    return false;
  }
  if (operation == numeric::NumericOperation::compare) {
    return Expect(result.comparison == expected_comparison,
                  "datatype numeric comparison mismatch");
  }
  return true;
}

bool TestExactNumericSurfaces(const api::EngineRequestContext& context) {
  bool ok = true;
  ok &= ExpectNumericBackend(numeric::NumericType::int128,
                             numeric::NumericOperation::add,
                             "170141183460469231731687303715884105720",
                             "7",
                             "170141183460469231731687303715884105727");
  ok &= ExpectNumericBackend(numeric::NumericType::uint128,
                             numeric::NumericOperation::subtract,
                             "340282366920938463463374607431768211455",
                             "5",
                             "340282366920938463463374607431768211450");
  ok &= ExpectNumericBackend(numeric::NumericType::decimal,
                             numeric::NumericOperation::add,
                             "1.25",
                             "2.75",
                             "4.00");
  ok &= ExpectNumericBackend(numeric::NumericType::decimal_float,
                             numeric::NumericOperation::compare,
                             "4.50",
                             "4.5",
                             "true",
                             0);
  ok &= ExpectNumericBackend(numeric::NumericType::real128,
                             numeric::NumericOperation::add,
                             "1.5",
                             "2.25",
                             "3.75");

  ok &= ExpectDatatypeNumeric(numeric::NumericType::int128,
                              numeric::NumericOperation::add,
                              "170141183460469231731687303715884105720",
                              "7",
                              "170141183460469231731687303715884105727");
  ok &= ExpectDatatypeNumeric(numeric::NumericType::uint128,
                              numeric::NumericOperation::subtract,
                              "340282366920938463463374607431768211455",
                              "5",
                              "340282366920938463463374607431768211450");
  ok &= ExpectDatatypeNumeric(numeric::NumericType::decimal,
                              numeric::NumericOperation::add,
                              "1.25",
                              "2.75",
                              "4.00");
  ok &= ExpectDatatypeNumeric(numeric::NumericType::decimal_float,
                              numeric::NumericOperation::compare,
                              "4.50",
                              "4.5",
                              "true",
                              0);
  ok &= ExpectDatatypeNumeric(numeric::NumericType::real128,
                              numeric::NumericOperation::add,
                              "1.5",
                              "2.25",
                              "3.75");

  api::EngineApplyNumericOperationRequest request;
  request.context = context;
  request.numeric_operation = "multiply";
  request.precision = 38;
  request.scale = 2;
  request.descriptors.push_back(ScalarDescriptor("decimal"));
  request.left_value = TypedValue("decimal", "6.50");
  request.right_value = TypedValue("decimal", "2");
  const auto multiplied = api::EngineApplyNumericOperation(request);
  ok &= ExpectApiOk(multiplied, "engine numeric multiply failed");
  ok &= Expect(multiplied.value.encoded_value == "13.00",
               "engine numeric multiply value mismatch");
  ok &= Expect(HasEvidence(multiplied, "datatype_numeric_operation", "multiply"),
               "engine numeric operation evidence missing");

  request.numeric_operation = "not_a_numeric_operation";
  const auto refused = api::EngineApplyNumericOperation(request);
  ok &= ExpectDiagnosticDetail(refused,
                               "numeric_operation_unsupported:not_a_numeric_operation",
                               "unsupported numeric operation did not fail closed");
  return ok;
}

sblr::SblrExecutionContext SblrContext(const api::EngineRequestContext& context) {
  sblr::SblrExecutionContext out;
  out.database_path = context.database_path;
  out.database_uuid = context.database_uuid.canonical;
  out.node_uuid = context.node_uuid.canonical;
  out.user_uuid = context.principal_uuid.canonical;
  out.session_uuid = context.session_uuid.canonical;
  out.transaction_uuid = context.transaction_uuid.canonical;
  out.local_transaction_id = context.local_transaction_id;
  out.snapshot_visible_through_local_transaction_id =
      context.snapshot_visible_through_local_transaction_id;
  out.transaction_isolation_level = context.transaction_isolation_level;
  out.application_name = "public_domain_psql_exact_numeric_gate";
  out.security_context_present = context.security_context_present;
  out.transaction_context_present = true;
  return out;
}

bool TestDomainSurfaces(DatabaseFixture fixture) {
  bool ok = true;

  auto no_tx_context = Context(fixture, "pcr041-domain-no-tx");
  auto no_tx_domain = Domain(77,
                             UuidText(UuidKind::object, 410),
                             "character",
                             "sblr_predicate:not_empty");
  const auto no_tx_append =
      api::AppendDomainEvent(no_tx_context, api::MakeDomainCreateEvent(no_tx_domain));
  ok &= ExpectDiagnosticDetail(no_tx_append,
                               "local_transaction_id_required",
                               "domain append without transaction did not fail closed");

  auto writer = Context(fixture, "pcr041-domain-writer");
  ok &= Begin(&writer);
  if (!ok) {
    return false;
  }

  auto mismatched = Domain(writer.local_transaction_id + 100,
                           UuidText(UuidKind::object, 411),
                           "character",
                           "sblr_predicate:not_empty");
  const auto mismatched_append =
      api::AppendDomainEvent(writer, api::MakeDomainCreateEvent(mismatched));
  ok &= ExpectDiagnosticDetail(mismatched_append,
                               "domain_creator_tx_mismatch",
                               "domain append accepted mismatched creator transaction");

  const auto text_domain_uuid = UuidText(UuidKind::object, 412);
  const auto numeric_domain_uuid = UuidText(UuidKind::object, 413);
  const auto udr_domain_uuid = UuidText(UuidKind::object, 414);
  const auto no_method_domain_uuid = UuidText(UuidKind::object, 415);

  const auto text_domain = Domain(writer.local_transaction_id,
                                  text_domain_uuid,
                                  "character",
                                  "sblr_predicate:length_gte:3",
                                  "builtin:upper;require_right:DOMAIN_METHOD_EXEC");
  const auto numeric_domain = Domain(writer.local_transaction_id,
                                     numeric_domain_uuid,
                                     "int128",
                                     "sblr_predicate:gte:1000");
  const auto udr_domain = Domain(writer.local_transaction_id,
                                 udr_domain_uuid,
                                 "character",
                                 "sblr_predicate:not_empty",
                                 "udr:package.method;require_right:DOMAIN_METHOD_EXEC");
  const auto no_method_domain = Domain(writer.local_transaction_id,
                                       no_method_domain_uuid,
                                       "character",
                                       "sblr_predicate:not_empty");

  for (const auto& domain : {text_domain, numeric_domain, udr_domain, no_method_domain}) {
    const auto appended =
        api::AppendDomainEvent(writer, api::MakeDomainCreateEvent(domain));
    ok &= Expect(!appended.error, "domain create event append failed");
  }
  auto altered_text_domain = text_domain;
  altered_text_domain.check_constraint_envelope = "sblr_predicate:length_gte:4";
  const auto altered =
      api::AppendDomainEvent(writer, api::MakeDomainAlterEvent(altered_text_domain));
  ok &= Expect(!altered.error, "domain alter event append failed");
  ok &= Expect(api::FindVisibleDomain(writer, text_domain_uuid, writer.local_transaction_id)
                   .has_value(),
               "active transaction domain visibility missing");
  ok &= Commit(&writer);
  if (!ok) {
    return false;
  }

  auto reader = Context(fixture, "pcr041-domain-reader");
  ok &= Begin(&reader);
  if (!ok) {
    return false;
  }

  api::EngineValidateDomainValueRequest validate_text;
  validate_text.context = reader;
  validate_text.domain_descriptor = api::DomainDescriptor(text_domain);
  validate_text.input_value = TypedValue("character", "Alpha");
  const auto text_valid = api::EngineValidateDomainValue(validate_text);
  ok &= ExpectApiOk(text_valid, "domain SBLR predicate validation failed");
  ok &= Expect(text_valid.value.descriptor.descriptor_uuid.canonical == text_domain_uuid,
               "domain validation did not return UUID descriptor authority");
  ok &= Expect(HasEvidence(text_valid, "domain_validation", text_domain_uuid),
               "domain validation evidence missing");
  ok &= Expect(HasEvidence(text_valid, "domain_check", text_domain_uuid),
               "domain check evidence missing");

  validate_text.input_value = TypedValue("character", "Bob");
  const auto text_refused = api::EngineValidateDomainValue(validate_text);
  ok &= ExpectDiagnosticDetail(text_refused,
                               "domain_sblr_predicate_domain_check_length_gte_failed",
                               "short domain value did not fail closed");

  api::EngineValidateDomainValueRequest validate_numeric;
  validate_numeric.context = reader;
  validate_numeric.domain_descriptor = api::DomainDescriptor(numeric_domain);
  validate_numeric.input_value = TypedValue("int128", "170141183460469231731687303715884105727");
  const auto numeric_valid = api::EngineValidateDomainValue(validate_numeric);
  ok &= ExpectApiOk(numeric_valid, "int128 domain validation failed");
  ok &= Expect(numeric_valid.value.encoded_value ==
                   "170141183460469231731687303715884105727",
               "int128 domain validation did not preserve canonical value");

  sblr::SblrDomainRequest sblr_request;
  sblr_request.context = SblrContext(reader);
  sblr_request.domain_uuid = text_domain_uuid;
  sblr_request.value.descriptor_id = "character";
  sblr_request.value.encoded_value = "Bravo";
  sblr_request.value.text_value = "Bravo";
  sblr_request.value.payload_kind = sblr::SblrValuePayloadKind::text;
  sblr_request.value.is_null = false;
  const auto sblr_valid = sblr::ValidateSblrDomainValue(sblr_request);
  ok &= Expect(sblr_valid.ok(), "SBLR domain validation failed");
  ok &= Expect(!sblr_valid.scalar_values.empty() &&
                   sblr_valid.scalar_values.front().descriptor_id ==
                       "domain:" + text_domain_uuid,
               "SBLR domain validation did not return domain descriptor");

  sblr_request.value.encoded_value = "Ace";
  sblr_request.value.text_value = "Ace";
  const auto sblr_refused = sblr::ValidateSblrDomainValue(sblr_request);
  ok &= Expect(!sblr_refused.ok(), "SBLR domain validation accepted bad value");
  ok &= Expect(!sblr_refused.diagnostics.empty() &&
                   sblr_refused.diagnostics.front().detail.find(
                       "domain_sblr_predicate_domain_check_length_gte_failed") !=
                       std::string::npos,
               "SBLR domain validation diagnostic drifted");

  api::EngineInvokeDomainMethodRequest invoke;
  invoke.context = reader;
  invoke.domain_descriptor = api::DomainDescriptor(text_domain);
  invoke.input_value = TypedValue("character", "Alpha");
  invoke.method_name = "upper";
  const auto denied = api::EngineInvokeDomainMethod(invoke);
  ok &= ExpectDiagnosticDetail(denied,
                               "domain_method_right_denied:DOMAIN_METHOD_EXEC",
                               "domain method right denial did not fail closed");

  invoke.context.trace_tags.push_back("right:DOMAIN_METHOD_EXEC");
  const auto upper = api::EngineInvokeDomainMethod(invoke);
  ok &= ExpectApiOk(upper, "domain builtin method invocation failed");
  ok &= Expect(upper.value.encoded_value == "ALPHA",
               "domain builtin method result mismatch");
  ok &= Expect(HasEvidence(upper, "domain_method_builtin", "upper"),
               "domain builtin method evidence missing");

  invoke.domain_descriptor = api::DomainDescriptor(udr_domain);
  invoke.method_name = "package.method";
  const auto udr_refused = api::EngineInvokeDomainMethod(invoke);
  ok &= ExpectDiagnosticDetail(udr_refused,
                               "domain_method_udr_bridge_not_available",
                               "domain UDR bridge did not fail closed");

  invoke.domain_descriptor = api::DomainDescriptor(no_method_domain);
  invoke.method_name = "upper";
  const auto missing_refused = api::EngineInvokeDomainMethod(invoke);
  ok &= ExpectDiagnosticDetail(missing_refused,
                               "domain_method_not_declared",
                               "domain missing method did not fail closed");

  ok &= Commit(&reader);
  return ok;
}

}  // namespace

int main(int argc, char** argv) {
  if (argc != 2) {
    std::cerr << "usage: public_domain_psql_exact_numeric_gate TMP_ROOT\n";
    return 2;
  }

  const std::filesystem::path root = argv[1];
  CleanupDir cleanup{root};
  std::error_code ignored;
  std::filesystem::remove_all(root, ignored);

  if (!ConfigureMemoryFixture()) {
    return 1;
  }

  const auto fixture = CreateDatabaseFixture(root);
  if (!Expect(std::filesystem::exists(fixture.path),
              "PCR-041 database fixture was not created")) {
    return 1;
  }

  auto context = Context(fixture, "pcr041-numeric");
  bool ok = Begin(&context);
  ok &= TestExactNumericSurfaces(context);
  ok &= Commit(&context);
  ok &= TestDomainSurfaces(fixture);
  std::cout << "public_domain_psql_exact_numeric_gate="
            << (ok ? "passed" : "failed") << '\n';
  return ok ? 0 : 1;
}
