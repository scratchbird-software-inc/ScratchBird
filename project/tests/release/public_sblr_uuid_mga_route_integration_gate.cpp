// Copyright (c) 2026 ScratchBird Software Inc.
//
// This Source Code Form is subject to the terms of the Mozilla Public
// License, v. 2.0. If a copy of the MPL was not distributed with this
// file, You can obtain one at https://mozilla.org/MPL/2.0/.
//
// SPDX-License-Identifier: MPL-2.0

#include "agent_policy_recommendation_application.hpp"
#include "api_types.hpp"
#include "catalog/datatype_index_optimizer_admission_api.hpp"
#include "database_lifecycle.hpp"
#include "hash_digest.hpp"
#include "index_route_capability.hpp"
#include "local_transaction_store.hpp"
#include "memory.hpp"
#include "query/plan_api.hpp"
#include "security/authorization_api.hpp"
#include "sblr_dispatch.hpp"
#include "sblr_engine_envelope.hpp"
#include "sblr_opcode_registry.hpp"
#include "sblr_transaction_begin_runtime.hpp"
#include "sblr_transaction_commit_runtime.hpp"
#include "transaction/transaction_api.hpp"
#include "transaction_inventory.hpp"
#include "uuid.hpp"

#include <algorithm>
#include <cstdint>
#include <cstdlib>
#include <filesystem>
#include <iostream>
#include <string>
#include <string_view>
#include <vector>

namespace {

namespace agents = scratchbird::core::agents;
namespace api = scratchbird::engine::internal_api;
namespace db = scratchbird::storage::database;
namespace idx = scratchbird::core::index;
namespace memory = scratchbird::core::memory;
namespace sblr = scratchbird::engine::sblr;
namespace txn = scratchbird::transaction::mga;
namespace uuid = scratchbird::core::uuid;

using scratchbird::core::platform::TypedUuid;
using scratchbird::core::platform::UuidKind;
using scratchbird::core::platform::u64;

constexpr u64 kBaseMillis = 1771200000000ull;
constexpr scratchbird::core::platform::u32 kPageSize = 16384;

struct CleanupDir {
  std::filesystem::path root;
  ~CleanupDir() {
    if (root.empty()) return;
    std::error_code ignored;
    std::filesystem::remove_all(root, ignored);
  }
};

struct Fixture {
  std::filesystem::path root;
  std::filesystem::path database_path;
  TypedUuid database_uuid;
  TypedUuid filespace_uuid;
  TypedUuid relation_uuid;
  TypedUuid descriptor_uuid;
  TypedUuid index_uuid;
  TypedUuid principal_uuid;
  TypedUuid session_uuid;
  TypedUuid policy_uuid;
};

bool Expect(bool condition, std::string_view message) {
  if (condition) return true;
  std::cerr << message << '\n';
  return false;
}

bool Contains(std::string_view value, std::string_view needle) {
  return value.find(needle) != std::string_view::npos;
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

bool HasEvidenceKind(const api::EngineApiResult& result,
                     std::string_view kind) {
  for (const auto& evidence : result.evidence) {
    if (evidence.evidence_kind == kind) return true;
  }
  return false;
}

bool HasDiagnostic(const api::EngineApiResult& result,
                   std::string_view code) {
  for (const auto& diagnostic : result.diagnostics) {
    if (diagnostic.code == code) return true;
  }
  return false;
}

bool DispatchHasDiagnostic(const sblr::SblrDispatchResult& result,
                           std::string_view code) {
  for (const auto& diagnostic : result.diagnostics) {
    if (diagnostic.code == code) return true;
  }
  return false;
}

std::string DiagnosticText(const api::EngineApiResult& result) {
  if (result.diagnostics.empty()) return {};
  const auto& diagnostic = result.diagnostics.front();
  return diagnostic.code + ":" + diagnostic.message_key + ":" +
         diagnostic.detail;
}

bool ExpectApiOk(const api::EngineApiResult& result,
                 std::string_view message) {
  if (result.ok) return true;
  std::cerr << message << ": " << DiagnosticText(result) << '\n';
  return false;
}

bool ExpectDispatchOk(const sblr::SblrDispatchResult& result,
                      std::string_view message) {
  if (result.accepted && result.envelope_validated && result.dispatched_to_api &&
      result.api_result.ok) {
    return true;
  }
  std::cerr << message << ": " << DiagnosticText(result.api_result) << '\n';
  return false;
}

TypedUuid MakeUuid(UuidKind kind, u64 offset) {
  const auto generated =
      uuid::GenerateEngineIdentityV7(kind, kBaseMillis + offset);
  return generated.ok() ? generated.value : TypedUuid{};
}

std::string UuidText(const TypedUuid& typed_uuid) {
  return typed_uuid.valid() ? uuid::UuidToString(typed_uuid.value)
                            : std::string{};
}

api::EngineDescriptor Descriptor(std::string canonical_type_name,
                                 const TypedUuid& descriptor_uuid = {}) {
  api::EngineDescriptor descriptor;
  descriptor.descriptor_uuid.canonical = UuidText(descriptor_uuid);
  descriptor.descriptor_kind = "scalar";
  descriptor.canonical_type_name = std::move(canonical_type_name);
  descriptor.encoded_descriptor = "canonical=" + descriptor.canonical_type_name;
  return descriptor;
}

api::EngineTypedValue TypedValue(std::string canonical_type_name,
                                 std::string encoded_value) {
  api::EngineTypedValue value;
  value.descriptor = Descriptor(std::move(canonical_type_name));
  value.encoded_value = std::move(encoded_value);
  return value;
}

void AddGrant(api::EngineRequestContext* context,
              const TypedUuid& target_uuid,
              std::string right,
              u64 offset) {
  api::EngineMaterializedAuthorizationGrant grant;
  grant.grant_uuid.canonical = UuidText(MakeUuid(UuidKind::object, offset));
  grant.subject_uuid = context->principal_uuid;
  grant.subject_kind = "principal";
  grant.target_uuid.canonical = UuidText(target_uuid);
  grant.right = std::move(right);
  grant.security_epoch = context->security_epoch;
  context->authorization_context.grants.push_back(std::move(grant));
}

api::EngineRequestContext Context(const Fixture& fixture,
                                  std::string request_id) {
  api::EngineRequestContext context;
  context.trust_mode = api::EngineTrustMode::server_isolated;
  context.request_id = std::move(request_id);
  context.database_path = fixture.database_path.string();
  context.database_uuid.canonical = UuidText(fixture.database_uuid);
  context.node_uuid.canonical = UuidText(MakeUuid(UuidKind::object, 10));
  context.principal_uuid.canonical = UuidText(fixture.principal_uuid);
  context.session_uuid.canonical = UuidText(fixture.session_uuid);
  context.statement_uuid.canonical = UuidText(MakeUuid(UuidKind::object, 11));
  context.statement_metadata_snapshot_uuid.canonical =
      UuidText(MakeUuid(UuidKind::object, 13));
  context.catalog_epoch_uuid.canonical =
      UuidText(MakeUuid(UuidKind::object, 14));
  context.statement_metadata_snapshot_engine_owned = true;
  context.security_context_present = true;
  context.identifier_profile_uuid = "sbsql_v3";
  context.language_context.language_tag = "en";
  context.language_context.default_language_tag = "en";
  context.catalog_generation_id = 7;
  context.security_epoch = 11;
  context.resource_epoch = 13;
  context.name_resolution_epoch = 17;
  context.authorization_context.present = true;
  context.authorization_context.authority_uuid.canonical =
      UuidText(MakeUuid(UuidKind::object, 12));
  context.authorization_context.principal_uuid = context.principal_uuid;
  context.authorization_context.security_epoch = context.security_epoch;
  context.authorization_context.policy_epoch = 19;
  context.authorization_context.catalog_generation_id =
      context.catalog_generation_id;
  context.authorization_context.effective_subjects.push_back(
      {context.principal_uuid, "principal"});
  context.authorization_context.evidence_tags.push_back(
      "public_sblr_uuid_mga_route_integration_gate");
  context.optimizer_capability_snapshot_uuid.canonical =
      UuidText(MakeUuid(UuidKind::object, 15));
  context.optimizer_resource_snapshot_uuid.canonical =
      UuidText(MakeUuid(UuidKind::object, 16));
  context.optimizer_route_snapshot_uuid.canonical =
      UuidText(MakeUuid(UuidKind::object, 17));
  context.optimizer_route_epoch = 23;
  context.optimizer_route_generation = 29;
  context.optimizer_memory_budget_bytes = 8 * 1024 * 1024;
  context.optimizer_maximum_candidate_count = 4096;
  context.optimizer_maximum_memo_groups = 512;
  context.optimizer_maximum_search_steps = 16384;
  context.optimizer_maximum_planning_time_ns = 10'000'000;
  context.current_monotonic_ns = "31000000";
  AddGrant(&context, fixture.relation_uuid, "OBS_INDEX_PROFILE_READ", 20);
  AddGrant(&context, fixture.relation_uuid, "OBS_AGENT_STATE_READ", 21);
  return context;
}

sblr::SblrOperationEnvelope Envelope(std::string operation_id,
                                     std::string opcode,
                                     std::string trace_key) {
  const auto* registry_entry = sblr::LookupSblrOperation(operation_id);
  if (registry_entry == nullptr) {
    std::cerr << "public route operation is absent from the canonical SBLR "
                 "registry: "
              << operation_id << '\n';
    std::exit(EXIT_FAILURE);
  }
  if (registry_entry->opcode != opcode) {
    std::cerr << "public route opcode mnemonic drifted for " << operation_id
              << ": expected " << registry_entry->opcode << " got " << opcode
              << '\n';
    std::exit(EXIT_FAILURE);
  }
  auto envelope = sblr::MakeSblrEnvelope(std::move(operation_id),
                                         std::move(opcode),
                                         std::move(trace_key));
  envelope.opcode_code = registry_entry->code;
  envelope.result_shape = registry_entry->result_contract;
  envelope.diagnostic_shape = "diagnostic_vector";
  envelope.parser_package_uuid =
      UuidText(MakeUuid(UuidKind::object, 40));
  envelope.registry_snapshot_uuid =
      UuidText(MakeUuid(UuidKind::object, 41));
  envelope.contains_sql_text = false;
  envelope.parser_resolved_names_to_uuids = true;
  envelope.requires_security_context = true;
  return envelope;
}

sblr::SblrDispatchResult Dispatch(const api::EngineRequestContext& context,
                                  sblr::SblrOperationEnvelope envelope,
                                  api::EngineApiRequest api_request = {}) {
  api_request.context = context;
  api_request.operation_id = envelope.operation_id;
  sblr::SblrDispatchRequest request;
  request.context = context;
  request.envelope = std::move(envelope);
  request.api_request = std::move(api_request);
  return sblr::DispatchSblrOperation(request);
}

memory::AllocationPolicy MemoryPolicy() {
  memory::AllocationPolicy policy;
  policy.policy_name = "public_sblr_uuid_mga_route_integration_gate";
  policy.hard_limit_bytes = 32ull * 1024ull * 1024ull;
  policy.soft_limit_bytes = 24ull * 1024ull * 1024ull;
  policy.per_context_limit_bytes = 16ull * 1024ull * 1024ull;
  policy.page_buffer_pool_limit_bytes = 8ull * 1024ull * 1024ull;
  policy.track_allocations = true;
  policy.zero_memory_on_release = true;
  return policy;
}

bool ConfigureMemoryFixture() {
  const auto configured = memory::ConfigureDefaultMemoryManagerForFixture(
      MemoryPolicy(), "public_sblr_uuid_mga_route_integration_gate");
  return Expect(configured.ok(), "memory fixture configuration failed") &&
         Expect(configured.fixture_mode,
                "memory fixture must not use production lazy defaults");
}

Fixture CreateFixture(const std::filesystem::path& root) {
  std::filesystem::create_directories(root);
  Fixture fixture;
  fixture.root = root;
  fixture.database_path = root / "pcr006_route_integration.sbdb";
  fixture.database_uuid = MakeUuid(UuidKind::database, 1);
  fixture.filespace_uuid = MakeUuid(UuidKind::filespace, 2);
  fixture.relation_uuid = MakeUuid(UuidKind::object, 3);
  fixture.descriptor_uuid = MakeUuid(UuidKind::object, 4);
  fixture.index_uuid = MakeUuid(UuidKind::object, 5);
  fixture.principal_uuid = MakeUuid(UuidKind::principal, 6);
  fixture.session_uuid = MakeUuid(UuidKind::object, 7);
  fixture.policy_uuid = MakeUuid(UuidKind::object, 8);

  db::DatabaseCreateConfig create;
  create.path = fixture.database_path.string();
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

std::string EncodeHex(std::string_view value) {
  static constexpr char kHex[] = "0123456789abcdef";
  std::string encoded;
  encoded.reserve(value.size() * 2);
  for (const unsigned char ch : value) {
    encoded.push_back(kHex[ch >> 4]);
    encoded.push_back(kHex[ch & 0x0f]);
  }
  return encoded;
}

void AppendLittleEndianU64(std::vector<std::uint8_t>* output,
                           std::uint64_t value) {
  for (unsigned byte = 0; byte < 8; ++byte) {
    output->push_back(
        static_cast<std::uint8_t>((value >> (byte * 8)) & 0xffu));
  }
}

void FinalizeProductionOperands(sblr::SblrOperationEnvelope* envelope) {
  std::uint32_t ordinal = 1;
  for (auto& operand : envelope->operands) {
    if (!operand.name.empty() &&
        std::all_of(operand.name.begin(), operand.name.end(),
                    [](const unsigned char ch) {
                      return ch >= '0' && ch <= '9';
                    })) {
      operand.name = "slot_" + operand.name;
    }
    const auto value = std::move(operand.value);
    operand.value.clear();
    operand.value_kind = sblr::SblrValueKind::literal_typed;
    operand.value_body.assign(16, 0);
    operand.value_body.front() = 0x73;
    AppendLittleEndianU64(&operand.value_body, value.size());
    operand.value_body.insert(operand.value_body.end(), value.begin(),
                              value.end());
    operand.ordinal = ordinal++;
  }
}

sblr::SblrOperationEnvelope QueryExecuteEnvelope(
    const api::EngineRequestContext& context) {
  constexpr std::string_view kInt64TypeUuid =
      "019d0000-0000-7000-8000-00000000d711";
  auto envelope =
      Envelope("query.execute", "SBLR_QUERY_EXECUTE", "pcr006.query");
  envelope.requires_transaction_context = true;

  const std::string descriptor_uuid =
      UuidText(MakeUuid(UuidKind::object, 50));
  envelope.operands = {
      {"uint16", "relational_wire_version", "2"},
      {"uuid", "relational_bound_sblr_tree_uuid",
       UuidText(MakeUuid(UuidKind::object, 51))},
      {"uuid", "relational_catalog_epoch_uuid",
       context.catalog_epoch_uuid.canonical},
      {"uuid", "relational_security_context_uuid",
       context.authorization_context.authority_uuid.canonical},
      {"uuid", "relational_statement_uuid", context.statement_uuid.canonical},
      {"uuid", "relational_owning_transaction_uuid",
       context.transaction_uuid.canonical},
      {"uuid", "relational_statement_snapshot_uuid",
       context.statement_snapshot_uuid.canonical},
      {"uuid", "relational_statement_metadata_snapshot_uuid",
       context.statement_metadata_snapshot_uuid.canonical},
      {"uint64", "relational_local_transaction_id",
       std::to_string(context.local_transaction_id)},
      {"uint64", "relational_snapshot_visible_through_local_transaction_id",
       std::to_string(
           context.snapshot_visible_through_local_transaction_id)},
      {"uint32", "relational_root_node_id", "1"},
      {"relational_descriptor_v1", "1",
       descriptor_uuid + "|" + std::string(kInt64TypeUuid) +
           "|1|-|-|-|-|-"},
      {"relational_expression_v1", "1", "1|-|1|-|-|1|-|3432"},
      {"relational_values_row_v1", "1", "1"},
      {"relational_output_v1", "1", "1|1|1|1|0|" + EncodeHex("id")},
      {"relational_node_v1", "1", "13|0|-|1|1"},
      {"relational_node_binding_v1", "1",
       EncodeHex("values.literal-table.v1") + "|1|-|-|-"},
  };
  FinalizeProductionOperands(&envelope);
  return envelope;
}

bool ProveSblrEnvelopeAuthority() {
  bool ok = true;
  auto accepted = Envelope("engine.op.txn_begin",
                           "SBLR_TXN_BEGIN",
                           "pcr006.accepted");
  accepted.requires_transaction_context = false;
  sblr::SblrTransactionBeginOptionsV1 options;
  options.isolation_profile_uuid[0] = 1;
  options.isolation_profile_generation = 1;
  options.transaction_policy_snapshot_uuid[0] = 2;
  options.transaction_policy_generation = 1;
  options.read_mode = 1;
  options.authority_scope = 1;
  options.wait_policy = 1;
  sblr::SblrOperand operand;
  operand.ordinal = 1;
  operand.type = "transaction.begin_options";
  operand.name = "options";
  operand.value_kind = sblr::SblrValueKind::transaction_begin_options;
  operand.value_body = sblr::EncodeSblrTransactionBeginOptionsV1(&options);
  accepted.operands.push_back(std::move(operand));
  ok = Expect(sblr::ValidateSblrEnvelope(accepted).ok,
              "valid SBLR envelope was rejected") &&
       ok;

  auto sql = accepted;
  sql.contains_sql_text = true;
  const auto sql_result = sblr::ValidateSblrEnvelope(sql);
  ok = Expect(!sql_result.ok, "SBLR envelope accepted SQL text") && ok;
  bool sql_diag = false;
  for (const auto& diagnostic : sql_result.diagnostics) {
    sql_diag = sql_diag ||
               diagnostic.code ==
                   "SBLR.OPERATION.DUPLICATE_INGRESS_AUTHORITY";
  }
  ok = Expect(sql_diag, "SBLR SQL-text refusal diagnostic missing") && ok;

  auto identity_mismatch = accepted;
  ++identity_mismatch.opcode_code;
  const auto mismatch_result = sblr::ValidateSblrEnvelope(identity_mismatch);
  ok = Expect(!mismatch_result.ok,
              "SBLR envelope accepted mismatched public opcode identity") &&
       ok;
  bool mismatch_diag = false;
  for (const auto& diagnostic : mismatch_result.diagnostics) {
    mismatch_diag = mismatch_diag ||
                    diagnostic.code ==
                        "SBLR.OPERATION.OPCODE_IDENTITY_MISMATCH";
  }
  ok = Expect(mismatch_diag,
              "SBLR public opcode-identity refusal diagnostic missing") &&
       ok;
  return ok;
}

struct RouteTransactionEvidence {
  scratchbird::core::hash::Digest256 begin_admission_sha256{};
};

bool BeginRouteTransaction(Fixture const& fixture,
                           api::EngineRequestContext* context,
                           RouteTransactionEvidence* evidence) {
  auto envelope =
      Envelope("engine.op.txn_begin", "SBLR_TXN_BEGIN", "pcr006.begin");
  envelope.requires_transaction_context = false;

  sblr::SblrTransactionBeginOptionsV1 options;
  options.isolation_profile_uuid[0] = 1;
  options.isolation_profile_generation = 1;
  options.transaction_policy_snapshot_uuid[0] = 2;
  options.transaction_policy_generation = 1;
  options.read_mode = 1;
  options.authority_scope = 1;
  options.wait_policy = 1;
  auto body = sblr::EncodeSblrTransactionBeginOptionsV1(&options);
  if (!Expect(!body.empty(),
              "canonical transaction-begin options failed to encode")) {
    return false;
  }
  const auto admission_sha =
      scratchbird::core::hash::ComputeSha256Digest(body);
  if (!Expect(admission_sha.ok(),
              "canonical transaction-begin evidence hash failed")) {
    return false;
  }
  evidence->begin_admission_sha256 = admission_sha.digest;

  sblr::SblrOperand operand;
  operand.ordinal = 1;
  operand.type = "transaction.begin_options";
  operand.name = "options";
  operand.value_kind = sblr::SblrValueKind::transaction_begin_options;
  operand.value_body = std::move(body);
  envelope.operands.push_back(std::move(operand));

  const auto admitted = Dispatch(*context, std::move(envelope));
  if (!ExpectDispatchOk(admitted,
                        "canonical SBLR transaction-begin admission failed") ||
      !Expect(admitted.api_result.local_transaction_id == 0 &&
                  admitted.api_result.transaction_uuid.canonical.empty(),
              "transaction-begin admission published engine MGA state")) {
    return false;
  }

  api::EngineBeginTransactionRequest begin;
  begin.context = *context;
  begin.operation_id = "transaction.begin";
  begin.isolation_level = "read_committed";
  const auto begun = api::EngineBeginTransaction(begin);
  if (!ExpectApiOk(begun,
                   "engine-owned transaction begin failed after admission") ||
      !Expect(!begun.transaction_uuid.canonical.empty() &&
                  begun.local_transaction_id != 0,
              "engine-owned begin did not publish an MGA identity") ||
      !Expect(HasEvidence(begun, "mga_authority",
                          "durable_transaction_inventory") &&
                  HasEvidence(begun, "transaction_admission",
                              "engine_mga_admitted"),
              "engine-owned begin did not report durable MGA authority")) {
    return false;
  }
  context->transaction_uuid = begun.transaction_uuid;
  context->local_transaction_id = begun.local_transaction_id;
  context->snapshot_visible_through_local_transaction_id =
      begun.snapshot_visible_through_local_transaction_id;
  context->transaction_isolation_level = begun.isolation_level;

  api::EnginePublishStatementSnapshotRequest publish;
  publish.context = *context;
  const auto published = api::EnginePublishStatementSnapshot(publish);
  if (!ExpectApiOk(published,
                   "engine-owned statement snapshot publication failed")) {
    return false;
  }
  context->statement_snapshot_uuid = published.statement_snapshot_uuid;
  context->snapshot_visible_through_local_transaction_id =
      published.snapshot_vector.visible_committed_high_watermark;
  (void)fixture;
  return true;
}

bool ProveRoutePlanning(const Fixture& fixture,
                        const api::EngineRequestContext& context) {
  (void)fixture;
  auto parser_authority = QueryExecuteEnvelope(context);
  parser_authority.contains_sql_text = true;
  const auto parser_refused = Dispatch(context, std::move(parser_authority));
  bool ok = Expect(!parser_refused.accepted &&
                       !parser_refused.dispatched_to_api,
                   "optimizer route did not reject parser SQL execution authority") &&
            Expect(DispatchHasDiagnostic(
                       parser_refused,
                       "SBLR.OPERATION.DUPLICATE_INGRESS_AUTHORITY"),
                   "optimizer route did not preserve parser SQL refusal") ;

  auto transaction_authority = QueryExecuteEnvelope(context);
  transaction_authority.requires_transaction_context = true;
  auto transactionless = context;
  transactionless.transaction_uuid.canonical.clear();
  transactionless.local_transaction_id = 0;
  const auto transaction_refused =
      Dispatch(transactionless, std::move(transaction_authority));
  ok = Expect(!transaction_refused.accepted &&
                  !transaction_refused.dispatched_to_api,
              "optimizer route did not reject parser transaction authority") &&
       ok;

  const auto executed = sblr::DispatchSblrOperation(
      {context, QueryExecuteEnvelope(context), api::EngineApiRequest{},
       std::nullopt});
  return ExpectDispatchOk(executed,
                          "canonical query.execute descriptor DAG failed") &&
         Expect(executed.logical_graph_populated &&
                    executed.optimizer_admitted && executed.optimizer_selected &&
                    executed.physical_dag_published &&
                    executed.physical_dag_executed &&
                    executed.runtime_actuals_attached &&
                    executed.canonical_result_published,
                "query.execute did not complete the canonical QOW route") &&
         Expect(executed.api_result.result_shape.rows.size() == 1 &&
                    executed.api_result.result_shape.rows.front().fields.size() ==
                        1 &&
                    executed.api_result.result_shape.rows.front()
                            .fields.front()
                            .first == "id" &&
                    executed.api_result.result_shape.rows.front()
                            .fields.front()
                            .second.descriptor.canonical_type_name == "int64" &&
                    executed.api_result.result_shape.rows.front()
                            .fields.front()
                            .second.encoded_value == "42",
                "query.execute did not publish the independent one-row int64 "
                "result") &&
         ok;
}

bool ProveSecurityAuthorization(const Fixture& fixture,
                                const api::EngineRequestContext& context) {
  api::EngineAuthorizeRequest request;
  request.context = context;
  request.operation_id = "security.authorize";
  request.target_object.uuid.canonical = UuidText(fixture.relation_uuid);
  request.target_object.object_kind = "table";
  request.required_right = "OBS_INDEX_PROFILE_READ";
  const auto authorized = api::EngineAuthorize(request);
  bool ok = ExpectApiOk(authorized, "security authorization failed");
  ok = Expect(authorized.authorized &&
                  HasEvidence(authorized, "authorization_authority",
                          "materialized_authorization_context"),
              "authorization did not use materialized engine context") &&
       ok;

  api::EngineAuthorizeRequest cluster_request;
  cluster_request.context = context;
  cluster_request.operation_id = "security.authorize";
  cluster_request.target_object.uuid.canonical = UuidText(fixture.relation_uuid);
  cluster_request.target_object.object_kind = "cluster_route";
  cluster_request.required_right = "OBS_CLUSTER_HEALTH_INSPECT";
  cluster_request.require_cluster_authority = true;
  const auto refused = api::EngineAuthorize(cluster_request);
  ok = Expect(!refused.ok && refused.cluster_authority_required &&
                  HasDiagnostic(refused,
                                "SECURITY.CLUSTER.AUTHORITY_REQUIRED"),
              "cluster authorization did not fail closed without provider") &&
       ok;
  return ok;
}

bool ProveIndexDatatypeAndAgentBoundaries(const Fixture& fixture) {
  bool ok = true;
  const auto* route = idx::FindBuiltinIndexRouteCapabilityState(
      idx::IndexRouteKind::sql_select, idx::IndexFamily::btree);
  ok = Expect(route != nullptr && route->route_complete(),
              "btree sql_select route capability is not complete") &&
       ok;
  ok = Expect(route != nullptr && route->requires_mga_recheck &&
                  route->requires_security_recheck,
              "index route does not require MGA and security rechecks") &&
       ok;

  api::EngineDatatypeIndexOptimizerAdmissionRequest datatype_request;
  datatype_request.type_group = "scalar";
  datatype_request.descriptor = Descriptor("int64", fixture.descriptor_uuid);
  datatype_request.support_path = "scalar_family:canonical_descriptor:btree";
  datatype_request.index_stats_status = "validated";
  datatype_request.reference_label = "postgres_bigint";
  const auto datatype = api::EvaluateDatatypeIndexOptimizerAdmission(
      datatype_request);
  ok = Expect(datatype.ok && datatype.index_admitted &&
                  datatype.statistics_admitted,
              "datatype/index/optimizer admission failed") &&
       ok;
  ok = Expect(datatype.optimizer_uses_canonical_descriptor &&
                  datatype.canonical_descriptor_used == "int64",
              "datatype/index/optimizer admission used reference label authority") &&
       ok;

  agents::AgentPolicyRecommendationApplicationRequest agent_request;
  agent_request.recommendation_uuid = UuidText(MakeUuid(UuidKind::object, 60));
  agent_request.evidence_uuid = UuidText(MakeUuid(UuidKind::object, 61));
  agent_request.policy_family = "memory_governor_policy";
  agent_request.scope_uuid = UuidText(fixture.database_uuid);
  agent_request.metric_digest = "sha256:pcr006-memory-metric";
  agent_request.proposed_field_name = "emergency_reserve_percent";
  agent_request.proposed_field_value = "25";
  agent_request.policy_generation = 3;
  agent_request.observed_policy_generation = 3;
  agent_request.durable_catalog_state = true;
  agent_request.strict_metric_snapshot = true;
  agent_request.metric_trusted = true;
  agent_request.metric_fresh = true;
  const auto agent = agents::EvaluateAgentPolicyRecommendationApplication(
      agent_request);
  ok = Expect(agent.ok && agent.recommendation_record_created,
              "agent recommendation was not accepted as pending-review evidence") &&
       ok;
  ok = Expect(agent.auto_apply_blocked,
              "agent recommendation unexpectedly allowed auto-apply") &&
       ok;
  return ok;
}

bool CommitRouteTransaction(api::EngineRequestContext* context,
                            const RouteTransactionEvidence& evidence) {
  auto envelope = Envelope("engine.op.txn_commit",
                           "SBLR_TXN_COMMIT",
                           "pcr006.commit");
  envelope.requires_transaction_context = true;

  const auto parsed_transaction =
      uuid::ParseUuid(context->transaction_uuid.canonical);
  if (!Expect(parsed_transaction.ok(),
              "transaction UUID is not canonical before commit")) {
    return false;
  }
  sblr::SblrTransactionCommitOptionsV1 options;
  std::copy(parsed_transaction.value.bytes.begin(),
            parsed_transaction.value.bytes.end(),
            options.transaction_uuid.begin());
  options.local_transaction_id = context->local_transaction_id;
  options.admitted_handle_evidence_sha256 = evidence.begin_admission_sha256;
  options.commit_mode = 1;
  options.authority_scope = 1;
  options.wait_policy = 1;
  auto body = sblr::EncodeSblrTransactionCommitOptionsV1(&options);
  if (!Expect(!body.empty(),
              "canonical transaction-commit options failed to encode")) {
    return false;
  }
  sblr::SblrOperand operand;
  operand.ordinal = 1;
  operand.type = "transaction.commit.options";
  operand.name = "options";
  operand.value_kind = sblr::SblrValueKind::transaction_commit_options;
  operand.value_body = std::move(body);
  envelope.operands.push_back(std::move(operand));

  const auto admitted = Dispatch(*context, std::move(envelope));
  if (!ExpectDispatchOk(admitted,
                        "canonical SBLR transaction-commit admission failed")) {
    return false;
  }

  api::EngineCommitTransactionRequest commit;
  commit.context = *context;
  commit.operation_id = "transaction.commit";
  const auto committed = api::EngineCommitTransaction(commit);
  const bool ok =
      ExpectApiOk(committed,
                  "engine-owned transaction commit failed after admission") &&
      Expect(committed.engine_finality_known &&
                 committed.commit_finality_state ==
                     "committed_by_engine_inventory" &&
                 HasEvidence(committed, "mga_authority",
                             "durable_transaction_inventory"),
             "engine-owned commit did not publish durable MGA finality");
  context->local_transaction_id = 0;
  context->transaction_uuid.canonical.clear();
  context->statement_snapshot_uuid.canonical.clear();
  return ok;
}

bool ProveMGAInventoryFinality(const Fixture& fixture) {
  const auto loaded =
      db::LoadLocalTransactionInventoryFromDatabase(
          fixture.database_path.string());
  bool ok = Expect(loaded.ok(), "could not load transaction inventory") &&
            Expect(!loaded.inventory.entries.empty(),
                   "transaction inventory contained no entries");
  bool committed = false;
  for (const auto& entry : loaded.inventory.entries) {
    committed = committed ||
                (entry.state == txn::TransactionState::committed &&
                 entry.identity.local_id.value == 1 &&
                 entry.identity.transaction_uuid.valid());
  }
  ok = Expect(committed,
              "committed transaction was not recorded in MGA inventory") &&
       ok;
  return ok;
}

bool ProveClusterSblrFailsClosed(const api::EngineRequestContext& context) {
  auto envelope = Envelope("cluster.inspect_state",
                           "SBLR_CLUSTER_INSPECT_STATE",
                           "pcr006.cluster");
  envelope.requires_cluster_authority = true;
  const auto refused = Dispatch(context, std::move(envelope));
  return Expect(refused.accepted && refused.dispatched_to_api,
                "cluster SBLR route did not reach cluster provider boundary") &&
         Expect(!refused.api_result.ok,
                "cluster SBLR route succeeded without provider") &&
         Expect(DispatchHasDiagnostic(refused,
                                      "SBLR.CLUSTER.SUPPORT_NOT_ENABLED") ||
                    DispatchHasDiagnostic(
                        refused,
                        "SBLR.CLUSTER.HANDSHAKE.STUB_COMPILE_LINK_ONLY"),
                "cluster SBLR route did not fail closed at provider boundary");
}

}  // namespace

int main(int argc, char** argv) {
  if (argc != 2) {
    std::cerr << "usage: public_sblr_uuid_mga_route_integration_gate <work-dir>\n";
    return 2;
  }

  CleanupDir cleanup{std::filesystem::path(argv[1])};
  bool ok = ConfigureMemoryFixture();
  const Fixture fixture = CreateFixture(cleanup.root);
  api::EngineRequestContext context =
      Context(fixture, "public-sblr-uuid-mga-route-integration");
  RouteTransactionEvidence transaction_evidence;

  ok = ProveSblrEnvelopeAuthority() && ok;
  ok = BeginRouteTransaction(fixture, &context, &transaction_evidence) && ok;
  ok = ProveRoutePlanning(fixture, context) && ok;
  ok = ProveSecurityAuthorization(fixture, context) && ok;
  ok = ProveIndexDatatypeAndAgentBoundaries(fixture) && ok;
  ok = CommitRouteTransaction(&context, transaction_evidence) && ok;
  ok = ProveMGAInventoryFinality(fixture) && ok;
  ok = ProveClusterSblrFailsClosed(context) && ok;

  if (!ok) return 1;
  std::cout << "public_sblr_uuid_mga_route_integration_gate=passed\n";
  return 0;
}
