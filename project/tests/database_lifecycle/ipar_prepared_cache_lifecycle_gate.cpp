// Copyright (c) 2026 ScratchBird Software Inc.
//
// This Source Code Form is subject to the terms of the Mozilla Public
// License, v. 2.0. If a copy of the MPL was not distributed with this
// file, You can obtain one at https://mozilla.org/MPL/2.0/.
//
// SPDX-License-Identifier: MPL-2.0

#include "database_lifecycle.hpp"
#include "dml/insert_batch.hpp"
#include "domain_support/domain_store.hpp"
#include "mga_relation_store/mga_relation_store.hpp"
#include "transaction/transaction_api.hpp"
#include "uuid.hpp"

#include <chrono>
#include <cstdlib>
#include <filesystem>
#include <iostream>
#include <string>
#include <string_view>
#include <utility>
#include <unistd.h>
#include <vector>

namespace {

namespace api = scratchbird::engine::internal_api;
namespace db = scratchbird::storage::database;
namespace uuid = scratchbird::core::uuid;
using scratchbird::core::platform::UuidKind;

constexpr const char* kLiveSchemaUuid = "019f3000-0000-7000-8000-000000000001";
constexpr const char* kLiveTableUuid = "019f3000-0000-7000-8000-000000000101";
constexpr const char* kLiveIndexUuid = "019f3000-0000-7000-8000-000000000201";
constexpr const char* kLiveDomainUuid = "019f3000-0000-7000-8000-000000000301";
constexpr const char* kLivePrincipalUuid = "019f3000-0000-7000-8000-000000000401";
constexpr const char* kLiveGroupUuid = "019f3000-0000-7000-8000-000000000402";

[[noreturn]] void Fail(std::string_view message) {
  std::cerr << message << '\n';
  std::exit(EXIT_FAILURE);
}

void Require(bool condition, std::string_view message) {
  if (!condition) {
    Fail(message);
  }
}

api::EngineAuthorizationSubject Subject(std::string uuid, std::string kind) {
  api::EngineAuthorizationSubject subject;
  subject.subject_uuid.canonical = std::move(uuid);
  subject.subject_kind = std::move(kind);
  return subject;
}

api::EngineRequestContext Context(std::string request_id,
                                  std::string principal = "principal-ipar-cache",
                                  std::string session = "session-ipar-cache",
                                  std::string role = "role-ipar-cache",
                                  std::string group = "group-ipar-cache",
                                  std::uint64_t catalog_epoch = 101,
                                  std::uint64_t security_epoch = 201,
                                  std::uint64_t policy_epoch = 301) {
  api::EngineRequestContext context;
  context.request_id = std::move(request_id);
  context.database_uuid.canonical = "database-ipar-cache";
  context.principal_uuid.canonical = std::move(principal);
  context.session_uuid.canonical = std::move(session);
  context.current_role_uuid.canonical = std::move(role);
  context.transaction_uuid.canonical = "transaction-ipar-cache";
  context.local_transaction_id = 77;
  context.snapshot_visible_through_local_transaction_id = 77;
  context.catalog_generation_id = catalog_epoch;
  context.security_epoch = security_epoch;
  context.resource_epoch = policy_epoch;
  context.name_resolution_epoch = 401;
  context.security_context_present = true;

  context.authorization_context.present = true;
  context.authorization_context.authority_uuid.canonical = "security-authority-ipar-cache";
  context.authorization_context.principal_uuid = context.principal_uuid;
  context.authorization_context.catalog_generation_id = catalog_epoch;
  context.authorization_context.security_epoch = security_epoch;
  context.authorization_context.policy_epoch = policy_epoch;
  context.authorization_context.effective_subjects.push_back(
      Subject(context.principal_uuid.canonical, "principal"));
  context.authorization_context.effective_subjects.push_back(
      Subject(std::move(group), "group"));
  api::EngineMaterializedAuthorizationGrant grant;
  grant.grant_uuid.canonical = "grant-ipar-cache-insert";
  grant.subject_uuid = context.authorization_context.effective_subjects.back().subject_uuid;
  grant.subject_kind = "group";
  grant.target_uuid.canonical = "table-ipar-cache";
  grant.right = "INSERT";
  grant.security_epoch = security_epoch;
  context.authorization_context.grants.push_back(std::move(grant));
  api::EngineMaterializedAuthorizationPolicy policy;
  policy.policy_uuid.canonical = "policy-ipar-cache-rls";
  policy.subject_uuid = context.authorization_context.effective_subjects.back().subject_uuid;
  policy.subject_kind = "group";
  policy.target_uuid.canonical = "table-ipar-cache";
  policy.right = "INSERT";
  policy.policy_kind = "rls_filter";
  policy.requires_runtime_recheck = true;
  policy.policy_epoch = policy_epoch;
  policy.canonical_policy_envelope = "sblr_predicate:tenant_visible";
  context.authorization_context.policies.push_back(std::move(policy));
  context.authorization_context.evidence_tags.push_back("group_chain_depth=1");
  return context;
}

api::CrudTableRecord Table(std::string table_uuid = "table-ipar-cache") {
  api::CrudTableRecord table;
  table.creator_tx = 77;
  table.table_uuid = std::move(table_uuid);
  table.default_name = "ipar_cache_table";
  table.columns.push_back({"id", "canonical=int64;primary_key=true;not_null=true;check=gte:0;domain_uuid=domain-ipar-int"});
  table.columns.push_back({"payload", "canonical=character;default=literal:empty;check=length_lte:256"});
  return table;
}

api::CrudIndexRecord Index(const std::string& table_uuid) {
  api::CrudIndexRecord index;
  index.creator_tx = 77;
  index.index_uuid = table_uuid + "-idx-id";
  index.table_uuid = table_uuid;
  index.column_name = "id";
  index.family = api::kCrudIndexFamilyBtree;
  index.profile = api::kCrudIndexProfileRowStoreScalarBtreeV1;
  index.unique = true;
  index.key_envelopes.push_back("unique");
  return index;
}

api::CrudState State(const api::CrudTableRecord& table) {
  api::CrudState state;
  state.transactions[77] = "active";
  state.tables.push_back(table);
  return state;
}

api::EngineInsertRowsRequest InsertRequest(api::EngineRequestContext context,
                                           std::vector<std::string> options = {}) {
  api::EngineInsertRowsRequest request;
  request.context = std::move(context);
  request.target_table.uuid.canonical = "table-ipar-cache";
  request.target_schema.uuid.canonical = "schema-ipar-cache";
  request.target_object.uuid.canonical = "table-ipar-cache";
  request.bound_object_identity.object_uuid = request.target_table.uuid;
  request.bound_object_identity.catalog_generation_id = request.context.catalog_generation_id;
  request.bound_object_identity.security_epoch = request.context.security_epoch;
  request.bound_object_identity.resource_epoch = request.context.resource_epoch;
  request.estimated_row_count = 1;
  request.input_rows.push_back({});
  request.option_envelopes = std::move(options);
  return request;
}

std::vector<std::string> ExpectedAuthorityOptions(const api::InsertBatchContext& context) {
  return {
      "prepared_descriptor.expected_cache_key=" + context.prepared_descriptor_cache_key,
      "prepared_descriptor.expected_generation=" +
          std::to_string(context.prepared_descriptor_generation),
      "prepared_descriptor.expected_principal_uuid=" +
          context.prepared_descriptor_principal_uuid,
      "prepared_descriptor.expected_role_uuid=" +
          context.prepared_descriptor_role_uuid,
      "prepared_descriptor.expected_session_uuid=" +
          context.prepared_descriptor_session_uuid,
      "prepared_descriptor.expected_catalog_epoch=" +
          std::to_string(context.prepared_descriptor_catalog_epoch),
      "prepared_descriptor.expected_security_epoch=" +
          std::to_string(context.prepared_descriptor_security_epoch),
      "prepared_descriptor.expected_policy_epoch=" +
          std::to_string(context.prepared_descriptor_policy_epoch),
      "prepared_descriptor.expected_authorization_digest=" +
          context.prepared_descriptor_authorization_digest};
}

bool HasEvidence(const std::vector<api::EngineEvidenceReference>& evidence,
                 std::string_view kind,
                 std::string_view id) {
  for (const auto& entry : evidence) {
    if (entry.evidence_kind == kind && entry.evidence_id == id) {
      return true;
    }
  }
  return false;
}

bool EvidenceContains(const api::EngineApiResult& result,
                      std::string_view kind,
                      std::string_view needle = {}) {
  for (const auto& entry : result.evidence) {
    if (entry.evidence_kind != kind) {
      continue;
    }
    if (needle.empty() ||
        entry.evidence_id.find(needle) != std::string::npos) {
      return true;
    }
  }
  return false;
}

template <typename TResult>
bool HasDiagnostic(const TResult& result, std::string_view code) {
  for (const auto& diagnostic : result.diagnostics) {
    if (diagnostic.code == code) {
      return true;
    }
  }
  return false;
}

api::EngineTypedValue Value(std::string encoded) {
  api::EngineTypedValue value;
  value.encoded_value = std::move(encoded);
  return value;
}

api::EngineTypedValue TextValue(std::string encoded, bool is_null = false) {
  api::EngineTypedValue value;
  value.descriptor.descriptor_kind = "scalar";
  value.descriptor.canonical_type_name = "character";
  value.descriptor.encoded_descriptor = "canonical=character";
  value.encoded_value = std::move(encoded);
  value.is_null = is_null;
  value.state = is_null ? api::EngineValueState::sql_null
                        : api::EngineValueState::value;
  return value;
}

std::uint64_t CurrentUnixMillis() {
  return static_cast<std::uint64_t>(
      std::chrono::duration_cast<std::chrono::milliseconds>(
          std::chrono::system_clock::now().time_since_epoch())
          .count());
}

std::filesystem::path MakeLiveTempPath() {
  return std::filesystem::temp_directory_path() /
         ("sb_ipar_prepared_validator_" +
          std::to_string(CurrentUnixMillis()) + "_" +
          std::to_string(static_cast<long long>(getpid())) + ".sbdb");
}

std::string CreateLiveDatabase(const std::filesystem::path& path) {
  db::DatabaseCreateConfig create;
  create.path = path.string();
  create.database_uuid =
      uuid::GenerateEngineIdentityV7(UuidKind::database, 1779800300000).value;
  create.filespace_uuid =
      uuid::GenerateEngineIdentityV7(UuidKind::filespace, 1779800300001).value;
  create.page_size = 16384;
  create.creation_unix_epoch_millis = 1779800300002;
  create.allow_minimal_resource_bootstrap = true;
  create.require_resource_seed_pack = false;
  create.allow_overwrite = true;
  const auto created = db::CreateDatabaseFile(create);
  if (!created.ok()) {
    std::cerr << created.diagnostic.diagnostic_code << ":"
              << created.diagnostic.message_key << '\n';
  }
  Require(created.ok(), "IPAR prepared validator database create failed");
  return uuid::UuidToString(create.database_uuid.value);
}

api::EngineRequestContext LiveBaseContext(const std::filesystem::path& path,
                                          const std::string& database_uuid,
                                          std::string request_id,
                                          std::string session_uuid) {
  api::EngineRequestContext context;
  context.trust_mode = api::EngineTrustMode::server_isolated;
  context.request_id = std::move(request_id);
  context.database_path = path.string();
  context.database_uuid.canonical = database_uuid;
  context.principal_uuid.canonical = kLivePrincipalUuid;
  context.session_uuid.canonical = std::move(session_uuid);
  context.current_schema_uuid.canonical = kLiveSchemaUuid;
  context.default_root_uuid.canonical =
      "019f3000-0000-7000-8000-000000000403";
  context.current_role_uuid.canonical =
      "019f3000-0000-7000-8000-000000000404";
  context.security_context_present = true;
  context.catalog_generation_id = 1;
  context.security_epoch = 1;
  context.resource_epoch = 1;
  context.name_resolution_epoch = 1;
  context.authorization_context.present = true;
  context.authorization_context.authority_uuid.canonical =
      "019f3000-0000-7000-8000-000000000405";
  context.authorization_context.principal_uuid = context.principal_uuid;
  context.authorization_context.security_epoch = context.security_epoch;
  context.authorization_context.policy_epoch = context.resource_epoch;
  context.authorization_context.catalog_generation_id =
      context.catalog_generation_id;
  context.authorization_context.effective_subjects.push_back(
      Subject(kLivePrincipalUuid, "principal"));
  context.authorization_context.effective_subjects.push_back(
      Subject(kLiveGroupUuid, "group"));

  api::EngineMaterializedAuthorizationGrant grant;
  grant.grant_uuid.canonical = "019f3000-0000-7000-8000-000000000501";
  grant.subject_uuid.canonical = kLiveGroupUuid;
  grant.subject_kind = "group";
  grant.target_uuid.canonical = kLiveTableUuid;
  grant.right = "INSERT";
  grant.security_epoch = context.security_epoch;
  context.authorization_context.grants.push_back(std::move(grant));

  api::EngineMaterializedAuthorizationPolicy policy;
  policy.policy_uuid.canonical = "019f3000-0000-7000-8000-000000000502";
  policy.subject_uuid.canonical = kLiveGroupUuid;
  policy.subject_kind = "group";
  policy.target_uuid.canonical = kLiveTableUuid;
  policy.right = "INSERT";
  policy.policy_kind = "rls_filter";
  policy.requires_runtime_recheck = true;
  policy.policy_epoch = context.resource_epoch;
  policy.canonical_policy_envelope =
      "sblr_predicate:column_equals:tenant:tenant_a";
  context.authorization_context.policies.push_back(std::move(policy));
  return context;
}

api::EngineRequestContext BeginLiveTransaction(const std::filesystem::path& path,
                                               const std::string& database_uuid,
                                               std::string request_id,
                                               std::string session_uuid) {
  api::EngineBeginTransactionRequest request;
  request.context = LiveBaseContext(path,
                                    database_uuid,
                                    std::move(request_id),
                                    std::move(session_uuid));
  request.isolation_level = "read_committed";
  const auto begun = api::EngineBeginTransaction(request);
  if (!begun.ok) {
    for (const auto& diagnostic : begun.diagnostics) {
      std::cerr << diagnostic.code << ":" << diagnostic.detail << '\n';
    }
  }
  Require(begun.ok, "IPAR prepared validator transaction begin failed");
  auto context = request.context;
  context.local_transaction_id = begun.local_transaction_id;
  context.transaction_uuid = begun.transaction_uuid;
  context.snapshot_visible_through_local_transaction_id =
      begun.snapshot_visible_through_local_transaction_id;
  context.transaction_isolation_level = begun.isolation_level;
  return context;
}

void CommitLiveTransaction(const api::EngineRequestContext& context) {
  api::EngineCommitTransactionRequest request;
  request.context = context;
  const auto committed = api::EngineCommitTransaction(request);
  if (!committed.ok) {
    for (const auto& diagnostic : committed.diagnostics) {
      std::cerr << diagnostic.code << ":" << diagnostic.detail << '\n';
    }
  }
  Require(committed.ok, "IPAR prepared validator transaction commit failed");
}

api::CrudTableRecord LiveTable() {
  api::CrudTableRecord table;
  table.table_uuid = kLiveTableUuid;
  table.default_name = "ipar_prepared_validator";
  table.columns.push_back(
      {"id",
       std::string("canonical=int64;primary_key=true;not_null=true;") +
           "check=gte:0;domain_uuid=" + kLiveDomainUuid});
  table.columns.push_back(
      {"payload", "canonical=character;default=literal:empty;check=length_lte:16"});
  table.columns.push_back({"tenant", "canonical=character;not_null=true"});
  return table;
}

api::CrudIndexRecord LiveUniqueIndex() {
  api::CrudIndexRecord index;
  index.index_uuid = kLiveIndexUuid;
  index.table_uuid = kLiveTableUuid;
  index.default_name = "ipar_prepared_validator_pk";
  index.column_name = "id";
  index.key_envelopes.push_back("id");
  index.family = api::kCrudIndexFamilyBtree;
  index.profile = api::kCrudIndexProfileRowStoreScalarBtreeV1;
  index.unique = true;
  return index;
}

api::DomainRecord LiveDomain(std::uint64_t creator_tx) {
  api::DomainRecord record;
  record.creator_tx = creator_tx;
  record.domain_uuid = kLiveDomainUuid;
  record.catalog_row_uuid = "019f3000-0000-7000-8000-000000000302";
  record.schema_uuid = kLiveSchemaUuid;
  record.default_name = "ipar_non_negative_int";
  record.base_descriptor_uuid = "descriptor-int64";
  record.base_descriptor_kind = "scalar";
  record.base_canonical_type_name = "int64";
  record.base_encoded_descriptor = "canonical=int64";
  record.nullable = false;
  record.check_constraint_envelope = "gte:0";
  record.validation_hook_status = "builtin";
  return record;
}

void SeedLiveValidatorMetadata(const api::EngineRequestContext& context) {
  const auto domain_status =
      api::AppendDomainEvent(context, api::MakeDomainCreateEvent(
                                          LiveDomain(context.local_transaction_id)));
  Require(!domain_status.error, "IPAR prepared validator domain seed failed");
  Require(!api::AppendMgaTableMetadata(context, LiveTable()).error,
          "IPAR prepared validator table seed failed");
  Require(!api::AppendMgaIndexMetadata(context, LiveUniqueIndex()).error,
          "IPAR prepared validator index seed failed");
}

api::EngineRowValue LiveRow(std::string row_uuid,
                            std::vector<std::pair<std::string, api::EngineTypedValue>> fields) {
  api::EngineRowValue row;
  row.requested_row_uuid.canonical = std::move(row_uuid);
  row.fields = std::move(fields);
  return row;
}

api::EngineInsertRowsResult LiveInsert(const api::EngineRequestContext& context,
                                       api::EngineRowValue row) {
  api::EngineInsertRowsRequest request;
  request.context = context;
  request.target_schema.uuid.canonical = kLiveSchemaUuid;
  request.target_table.uuid.canonical = kLiveTableUuid;
  request.target_table.object_kind = "table";
  request.target_object.uuid.canonical = kLiveTableUuid;
  request.target_object.object_kind = "table";
  request.bound_object_identity.object_uuid = request.target_table.uuid;
  request.bound_object_identity.catalog_generation_id =
      context.catalog_generation_id;
  request.bound_object_identity.security_epoch = context.security_epoch;
  request.bound_object_identity.resource_epoch = context.resource_epoch;
  request.estimated_row_count = 1;
  request.input_rows.push_back(std::move(row));
  return api::EngineInsertRows(request);
}

void RequireRefusal(const api::InsertBatchContext& context,
                    std::string_view reason) {
  Require(!context.accepted, "IPAR prepared descriptor stale handle was accepted");
  Require(context.prepared_descriptor_authority_refused,
          "IPAR prepared descriptor did not mark authority refusal");
  Require(context.prepared_descriptor_refusal_reason == reason,
          "IPAR prepared descriptor refusal reason mismatch");
  api::EngineApiResult result;
  api::AddInsertBatchEvidenceToResult(context, &result);
  Require(HasEvidence(result.evidence, "prepared_descriptor_authority_refusal", reason),
          "IPAR prepared descriptor refusal evidence missing");
  Require(HasEvidence(result.evidence, "prepared_descriptor_refused_before_execution", "true"),
          "IPAR prepared descriptor did not prove before-execution refusal");
}

api::InsertBatchContext Begin(const api::EngineInsertRowsRequest& request,
                              const api::CrudTableRecord& table) {
  const auto state = State(table);
  const std::vector<api::CrudIndexRecord> indexes{Index(table.table_uuid)};
  return api::BeginInsertBatchContext(request, state, table, indexes);
}

void ValidateSameGroupChainReusesAuthorizationDescriptor() {
  const auto table = Table();
  const auto request = InsertRequest(Context("ipar-prepared-cache-reuse"));
  const auto first = Begin(request, table);
  Require(first.accepted, "IPAR prepared descriptor first bind refused");
  Require(!first.prepared_descriptor_cache_hit,
          "IPAR prepared descriptor first bind unexpectedly hit cache");

  const auto second = Begin(request, table);
  Require(second.accepted, "IPAR prepared descriptor second bind refused");
  Require(second.prepared_descriptor_cache_hit,
          "IPAR prepared descriptor did not reuse same user/role/group chain");
  Require(first.prepared_descriptor_authorization_digest ==
              second.prepared_descriptor_authorization_digest,
          "IPAR prepared descriptor authorization digest drifted for same group chain");
  Require(first.row_encoder_plan.plan_id == second.row_encoder_plan.plan_id,
          "IPAR row encoder plan was not reused with prepared descriptor");
  Require(first.row_encoder_plan.row_shape_signature ==
              second.row_encoder_plan.row_shape_signature,
          "IPAR row encoder shape signature drifted on cache reuse");
  Require(first.row_encoder_plan.validator_signature ==
              second.row_encoder_plan.validator_signature,
          "IPAR row validator signature drifted on cache reuse");
  Require(first.row_encoder_plan.column_count == 2,
          "IPAR row encoder did not bind expected column count");
  Require(first.row_encoder_plan.default_validator_count == 1,
          "IPAR row encoder did not bind default validator count");
  Require(first.row_encoder_plan.domain_validator_count == 1,
          "IPAR row encoder did not bind domain validator count");
  Require(first.row_encoder_plan.check_validator_count == 2,
          "IPAR row encoder did not bind check validator count");
  Require(first.row_encoder_plan.not_null_validator_count == 1,
          "IPAR row encoder did not bind not-null validator count");
  Require(first.row_encoder_plan.unique_validator_count == 1,
          "IPAR row encoder did not bind unique validator count");
  Require(first.row_encoder_plan.runtime_policy_recheck_count == 1,
          "IPAR row encoder did not bind runtime security recheck count");
  Require(first.row_encoder_plan.unsupported_sblr_validators_fail_closed,
          "IPAR row encoder did not mark unsupported SBLR validators fail-closed");

  api::EngineRowValue row;
  row.fields.push_back({"payload", Value("payload-first")});
  row.fields.push_back({"id", Value("42")});
  const auto prepared = api::PrepareInsertRowForBatch(request,
                                                     row,
                                                     first.row_template,
                                                     first.row_encoder_plan);
  Require(prepared.values.size() == 2,
          "IPAR row encoder prepared unexpected value count");
  Require(prepared.values[0].first == "id" &&
              prepared.values[1].first == "payload",
          "IPAR row encoder did not use cached descriptor column order");

  api::EngineApiResult first_evidence;
  api::AddInsertBatchEvidenceToResult(first, &first_evidence);
  Require(HasEvidence(first_evidence.evidence,
                      "insert_row_encoder_descriptor_state",
                      "compiled"),
          "IPAR row encoder compile evidence missing");
  Require(HasEvidence(first_evidence.evidence,
                      "insert_validator_runtime_policy_recheck_count",
                      "1"),
          "IPAR row encoder security policy evidence missing");

  api::EngineApiResult second_evidence;
  api::AddInsertBatchEvidenceToResult(second, &second_evidence);
  Require(HasEvidence(second_evidence.evidence,
                      "insert_row_encoder_descriptor_state",
                      "reused"),
          "IPAR row encoder reuse evidence missing");
}

void ValidateRowEncoderInvalidatesOnShapeChange() {
  const auto table = Table();
  const auto request = InsertRequest(Context("ipar-row-encoder-shape-base"));
  const auto first = Begin(request, table);
  Require(first.accepted, "IPAR row encoder shape base refused");

  auto changed_table = table;
  changed_table.columns.push_back({"shape_extra", "canonical=int64;default=literal:7"});
  const auto changed = Begin(request, changed_table);
  Require(changed.accepted, "IPAR row encoder shape change refused");
  Require(!changed.prepared_descriptor_cache_hit,
          "IPAR row encoder shape change incorrectly hit descriptor cache");
  Require(first.row_encoder_plan.plan_id != changed.row_encoder_plan.plan_id,
          "IPAR row encoder plan did not change after table shape changed");
  Require(first.row_encoder_plan.row_shape_signature !=
              changed.row_encoder_plan.row_shape_signature,
          "IPAR row encoder shape signature did not change after table shape changed");
}

void ValidateEpochAndAuthorityRefusals() {
  const auto table = Table();
  const auto base = Begin(InsertRequest(Context("ipar-prepared-cache-base")), table);
  Require(base.accepted, "IPAR prepared descriptor base context refused");

  auto stale_security_options = ExpectedAuthorityOptions(base);
  auto stale_security = InsertRequest(
      Context("ipar-prepared-cache-stale-security",
              "principal-ipar-cache",
              "session-ipar-cache",
              "role-ipar-cache",
              "group-ipar-cache",
              base.prepared_descriptor_catalog_epoch,
              base.prepared_descriptor_security_epoch + 1,
              base.prepared_descriptor_policy_epoch),
      stale_security_options);
  RequireRefusal(Begin(stale_security, table), "stale_security_epoch");

  auto cross_session = InsertRequest(
      Context("ipar-prepared-cache-cross-session",
              "principal-ipar-cache",
              "session-ipar-cache-other"),
      ExpectedAuthorityOptions(base));
  RequireRefusal(Begin(cross_session, table), "cross_session");

  auto cross_user = InsertRequest(
      Context("ipar-prepared-cache-cross-user",
              "principal-ipar-cache-other",
              "session-ipar-cache"),
      ExpectedAuthorityOptions(base));
  RequireRefusal(Begin(cross_user, table), "cross_user");

  auto changed_group = InsertRequest(
      Context("ipar-prepared-cache-group-change",
              "principal-ipar-cache",
              "session-ipar-cache",
              "role-ipar-cache",
              "group-ipar-cache-other"),
      ExpectedAuthorityOptions(base));
  RequireRefusal(Begin(changed_group, table), "authorization_context_changed");

  auto lease_options = ExpectedAuthorityOptions(base);
  lease_options.push_back("prepared_descriptor.lease_expires_at_epoch=10");
  lease_options.push_back("prepared_descriptor.current_lease_epoch=11");
  auto lease_expired =
      InsertRequest(Context("ipar-prepared-cache-lease-expired"), lease_options);
  RequireRefusal(Begin(lease_expired, table), "lease_expired");
}

void ValidateEvictionGenerationRefusal() {
  const auto table_a = Table("table-ipar-cache-evict-a");
  auto request_a = InsertRequest(Context("ipar-prepared-cache-evict-a"),
                                 {"prepared_descriptor.cache_limit=1"});
  request_a.target_table.uuid.canonical = table_a.table_uuid;
  request_a.target_object.uuid.canonical = table_a.table_uuid;
  request_a.bound_object_identity.object_uuid = request_a.target_table.uuid;
  const auto first_a = Begin(request_a, table_a);
  Require(first_a.accepted, "IPAR eviction first descriptor refused");

  const auto table_b = Table("table-ipar-cache-evict-b");
  auto request_b = InsertRequest(Context("ipar-prepared-cache-evict-b"),
                                 {"prepared_descriptor.cache_limit=1"});
  request_b.target_table.uuid.canonical = table_b.table_uuid;
  request_b.target_object.uuid.canonical = table_b.table_uuid;
  request_b.bound_object_identity.object_uuid = request_b.target_table.uuid;
  const auto first_b = Begin(request_b, table_b);
  Require(first_b.accepted, "IPAR eviction second descriptor refused");
  Require(first_b.prepared_descriptor_eviction_count >
              first_a.prepared_descriptor_eviction_count,
          "IPAR descriptor cache eviction counter did not advance");

  auto rebound_options = ExpectedAuthorityOptions(first_a);
  rebound_options.push_back("prepared_descriptor.cache_limit=1");
  auto rebound_request =
      InsertRequest(Context("ipar-prepared-cache-evict-a-rebound"), rebound_options);
  rebound_request.target_table.uuid.canonical = table_a.table_uuid;
  rebound_request.target_object.uuid.canonical = table_a.table_uuid;
  rebound_request.bound_object_identity.object_uuid = rebound_request.target_table.uuid;
  RequireRefusal(Begin(rebound_request, table_a), "evicted_or_rebound");
}

void ValidatePreparedDescriptorExecutesLiveValidators() {
  const auto path = MakeLiveTempPath();
  const auto database_uuid = CreateLiveDatabase(path);

  auto setup = BeginLiveTransaction(path,
                                    database_uuid,
                                    "ipar-prepared-validator-setup",
                                    "019f3000-0000-7000-8000-000000000601");
  SeedLiveValidatorMetadata(setup);
  CommitLiveTransaction(setup);

  auto writer = BeginLiveTransaction(path,
                                     database_uuid,
                                     "ipar-prepared-validator-writer",
                                     "019f3000-0000-7000-8000-000000000602");

  const auto first = LiveInsert(
      writer,
      LiveRow("019f3000-0000-7000-8000-000000000701",
              {{"id", TextValue("42")},
               {"tenant", TextValue("tenant_a")}}));
  if (!first.ok) {
    for (const auto& diagnostic : first.diagnostics) {
      std::cerr << diagnostic.code << ":" << diagnostic.detail << '\n';
    }
  }
  Require(first.ok, "IPAR prepared validator first live insert failed");
  Require(EvidenceContains(first, "insert_row_encoder_descriptor_state", "compiled"),
          "IPAR live first insert did not compile prepared descriptor");
  Require(EvidenceContains(first, "constraint_default", "payload"),
          "IPAR live default validator evidence missing");
  Require(EvidenceContains(first, "domain_validation", kLiveDomainUuid),
          "IPAR live domain validator evidence missing");
  Require(EvidenceContains(first, "domain_check", kLiveDomainUuid),
          "IPAR live domain check evidence missing");
  Require(EvidenceContains(first, "constraint_not_null", "id"),
          "IPAR live not-null validator evidence missing");
  Require(EvidenceContains(first, "constraint_check", "id"),
          "IPAR live check validator evidence missing");
  Require(EvidenceContains(first, "constraint_key_support", kLiveIndexUuid),
          "IPAR live unique validator evidence missing");
  Require(EvidenceContains(first, "insert_runtime_security_recheck", "rls=filter"),
          "IPAR live RLS runtime recheck evidence missing");
  bool saw_payload_default = false;
  for (const auto& [field, typed] : first.result_shape.rows.front().fields) {
    if (field == "payload" && typed.encoded_value == "empty") {
      saw_payload_default = true;
    }
  }
  Require(saw_payload_default, "IPAR live default was not materialized");

  const auto second = LiveInsert(
      writer,
      LiveRow("019f3000-0000-7000-8000-000000000702",
              {{"id", TextValue("43")},
               {"payload", TextValue("short")},
               {"tenant", TextValue("tenant_a")}}));
  Require(second.ok, "IPAR prepared validator second live insert failed");
  Require(EvidenceContains(second, "insert_row_encoder_descriptor_state", "reused"),
          "IPAR live second insert did not reuse prepared descriptor");
  Require(EvidenceContains(second, "insert_memory_arena_reuse_claim",
                           "prepared_descriptor_cache_reuse"),
          "IPAR live second insert did not publish prepared reuse memory evidence");
  Require(EvidenceContains(second, "insert_memory_arena_reuse_physical_arena_claimed",
                           "true"),
          "IPAR live second insert did not publish physical arena reuse evidence");
  Require(EvidenceContains(second, "insert_memory_arena_grant_state", "granted"),
          "IPAR live second insert did not grant query memory arena scratch");
  Require(EvidenceContains(second, "insert_memory_arena_release_state", "released"),
          "IPAR live second insert did not release query memory arena scratch");
  Require(EvidenceContains(second, "insert_memory_arena_reset_state", "reset"),
          "IPAR live second insert did not reset query memory arena scratch");
  Require(EvidenceContains(second, "insert_memory_arena_fail_closed", "false"),
          "IPAR live second insert fail-closed the query memory arena");
  Require(EvidenceContains(second, "insert_memory_arena_leak_count", "0"),
          "IPAR live second insert leaked query memory arena scratch");
  Require(EvidenceContains(second, "constraint_check", "payload"),
          "IPAR live reused descriptor did not execute payload check");
  Require(EvidenceContains(second, "insert_runtime_security_recheck", "rls=filter"),
          "IPAR live reused descriptor skipped RLS runtime recheck");

  const auto domain_refusal = LiveInsert(
      writer,
      LiveRow("019f3000-0000-7000-8000-000000000703",
              {{"id", TextValue("-1")},
               {"tenant", TextValue("tenant_a")}}));
  Require(!domain_refusal.ok,
          "IPAR live negative domain value was admitted");
  Require(HasDiagnostic(domain_refusal, "SBSQL_DOMAIN_CHECK_VIOLATION"),
          "IPAR live domain refusal diagnostic mismatch");

  const auto not_null_refusal = LiveInsert(
      writer,
      LiveRow("019f3000-0000-7000-8000-000000000704",
              {{"id", TextValue("44")},
               {"payload", TextValue("short")}}));
  Require(!not_null_refusal.ok,
          "IPAR live missing tenant was admitted");
  Require(HasDiagnostic(not_null_refusal, "CLI.CONSTRAINT_NOT_NULL_VIOLATION"),
          "IPAR live not-null refusal diagnostic mismatch");

  const auto check_refusal = LiveInsert(
      writer,
      LiveRow("019f3000-0000-7000-8000-000000000705",
              {{"id", TextValue("45")},
               {"payload", TextValue("payload-too-long-for-check")},
               {"tenant", TextValue("tenant_a")}}));
  Require(!check_refusal.ok,
          "IPAR live check violation was admitted");
  Require(HasDiagnostic(check_refusal, "CLI.CONSTRAINT_CHECK_VIOLATION"),
          "IPAR live check refusal diagnostic mismatch");

  const auto duplicate_refusal = LiveInsert(
      writer,
      LiveRow("019f3000-0000-7000-8000-000000000706",
              {{"id", TextValue("42")},
               {"tenant", TextValue("tenant_a")}}));
  Require(!duplicate_refusal.ok,
          "IPAR live duplicate unique value was admitted");
  Require(HasDiagnostic(duplicate_refusal, "CLI.CONSTRAINT_PRIMARY_KEY_VIOLATION"),
          "IPAR live unique refusal diagnostic mismatch");

  const auto rls_refusal = LiveInsert(
      writer,
      LiveRow("019f3000-0000-7000-8000-000000000707",
              {{"id", TextValue("46")},
               {"payload", TextValue("short")},
               {"tenant", TextValue("tenant_b")}}));
  Require(!rls_refusal.ok,
          "IPAR live RLS-denied row was admitted");
  Require(HasDiagnostic(rls_refusal, "SECURITY.RLS.DENIED"),
          "IPAR live RLS refusal diagnostic mismatch");
  Require(EvidenceContains(rls_refusal, "insert_runtime_security_policy_result",
                           "column_equals:tenant:deny"),
          "IPAR live RLS refusal evidence missing");

  CommitLiveTransaction(writer);

  std::error_code ignored;
  std::filesystem::remove(path, ignored);
  std::filesystem::remove(path.string() + ".sb.mga", ignored);
  std::filesystem::remove(path.string() + ".sb.behavior", ignored);
}

}  // namespace

int main() {
  ValidateSameGroupChainReusesAuthorizationDescriptor();
  ValidateRowEncoderInvalidatesOnShapeChange();
  ValidateEpochAndAuthorityRefusals();
  ValidateEvictionGenerationRefusal();
  ValidatePreparedDescriptorExecutesLiveValidators();
  return EXIT_SUCCESS;
}
