// Copyright (c) 2026 ScratchBird Software Inc.
//
// This Source Code Form is subject to the terms of the Mozilla Public
// License, v. 2.0. If a copy of the MPL was not distributed with this
// file, You can obtain one at https://mozilla.org/MPL/2.0/.
//
// SPDX-License-Identifier: MPL-2.0

#include "dml/insert_batch.hpp"

#include <cstdint>
#include <cstdlib>
#include <iostream>
#include <string>
#include <string_view>
#include <utility>
#include <vector>

namespace {

namespace api = scratchbird::engine::internal_api;

constexpr std::uint64_t kLocalTransactionId = 6102;

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

api::EngineRequestContext Context(
    std::string request_id,
    std::string table_uuid,
    std::string principal = "principal-ipar-p6-02",
    std::string session = "session-ipar-p6-02",
    std::string role = "role-ipar-p6-02",
    std::string group = "group-ipar-p6-02",
    std::uint64_t catalog_epoch = 701,
    std::uint64_t security_epoch = 801,
    std::uint64_t policy_epoch = 901) {
  api::EngineRequestContext context;
  context.request_id = std::move(request_id);
  context.database_uuid.canonical = "database-ipar-p6-02";
  context.principal_uuid.canonical = std::move(principal);
  context.session_uuid.canonical = std::move(session);
  context.current_role_uuid.canonical = std::move(role);
  context.transaction_uuid.canonical = "transaction-ipar-p6-02";
  context.local_transaction_id = kLocalTransactionId;
  context.snapshot_visible_through_local_transaction_id = kLocalTransactionId;
  context.catalog_generation_id = catalog_epoch;
  context.security_epoch = security_epoch;
  context.resource_epoch = policy_epoch;
  context.name_resolution_epoch = 1001;
  context.security_context_present = true;

  context.authorization_context.present = true;
  context.authorization_context.authority_uuid.canonical =
      "security-authority-ipar-p6-02";
  context.authorization_context.principal_uuid = context.principal_uuid;
  context.authorization_context.catalog_generation_id = catalog_epoch;
  context.authorization_context.security_epoch = security_epoch;
  context.authorization_context.policy_epoch = policy_epoch;
  context.authorization_context.effective_subjects.push_back(
      Subject(context.principal_uuid.canonical, "principal"));
  context.authorization_context.effective_subjects.push_back(
      Subject(std::move(group), "group"));

  api::EngineMaterializedAuthorizationGrant grant;
  grant.grant_uuid.canonical = "grant-ipar-p6-02-insert";
  grant.subject_uuid =
      context.authorization_context.effective_subjects.back().subject_uuid;
  grant.subject_kind = "group";
  grant.target_uuid.canonical = table_uuid;
  grant.right = "INSERT";
  grant.security_epoch = security_epoch;
  context.authorization_context.grants.push_back(std::move(grant));

  api::EngineMaterializedAuthorizationPolicy policy;
  policy.policy_uuid.canonical = "policy-ipar-p6-02-runtime";
  policy.subject_uuid =
      context.authorization_context.effective_subjects.back().subject_uuid;
  policy.subject_kind = "group";
  policy.target_uuid.canonical = std::move(table_uuid);
  policy.right = "INSERT";
  policy.policy_kind = "rls_filter";
  policy.requires_runtime_recheck = true;
  policy.policy_epoch = policy_epoch;
  policy.canonical_policy_envelope = "sblr_predicate:ipar_p6_02_visible";
  context.authorization_context.policies.push_back(std::move(policy));
  context.authorization_context.evidence_tags.push_back("ipar_p6_02_pressure");
  return context;
}

api::CrudTableRecord Table(std::string table_uuid) {
  api::CrudTableRecord table;
  table.creator_tx = kLocalTransactionId;
  table.table_uuid = std::move(table_uuid);
  table.default_name = "ipar_memory_pressure_trim";
  table.columns.push_back(
      {"id", "canonical=int64;primary_key=true;not_null=true"});
  table.columns.push_back(
      {"payload", "canonical=character;default=literal:empty"});
  return table;
}

api::CrudIndexRecord Index(const std::string& table_uuid) {
  api::CrudIndexRecord index;
  index.creator_tx = kLocalTransactionId;
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
  state.transactions[kLocalTransactionId] = "active";
  state.tables.push_back(table);
  return state;
}

api::EngineInsertRowsRequest InsertRequest(
    const api::CrudTableRecord& table,
    api::EngineRequestContext context,
    std::vector<std::string> options = {}) {
  api::EngineInsertRowsRequest request;
  request.context = std::move(context);
  request.target_table.uuid.canonical = table.table_uuid;
  request.target_schema.uuid.canonical = "schema-ipar-p6-02";
  request.target_object.uuid.canonical = table.table_uuid;
  request.bound_object_identity.object_uuid = request.target_table.uuid;
  request.bound_object_identity.catalog_generation_id =
      request.context.catalog_generation_id;
  request.bound_object_identity.security_epoch = request.context.security_epoch;
  request.bound_object_identity.resource_epoch = request.context.resource_epoch;
  request.estimated_row_count = 1;
  request.input_rows.push_back({});
  request.option_envelopes = std::move(options);
  return request;
}

api::InsertBatchContext Begin(const api::CrudTableRecord& table,
                              const api::EngineInsertRowsRequest& request) {
  const auto state = State(table);
  const std::vector<api::CrudIndexRecord> indexes{Index(table.table_uuid)};
  return api::BeginInsertBatchContext(request, state, table, indexes);
}

std::vector<std::string> ExpectedAuthorityOptions(
    const api::InsertBatchContext& context) {
  return {
      "prepared_descriptor.expected_cache_key=" +
          context.prepared_descriptor_cache_key,
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

std::vector<std::string> PressureOptions(std::uint64_t target_entries) {
  return {"prepared_descriptor.cache_limit=8",
          "prepared_descriptor.memory_pressure=true",
          "prepared_descriptor.pressure_reason=ipar_p6_02_test_pressure",
          "prepared_descriptor.trim_target_entries=" +
              std::to_string(target_entries)};
}

void AppendPressureOptions(std::vector<std::string>* options,
                           std::uint64_t target_entries) {
  auto pressure = PressureOptions(target_entries);
  options->insert(options->end(), pressure.begin(), pressure.end());
}

api::EngineApiResult EvidenceResult(const api::InsertBatchContext& context) {
  api::EngineApiResult result;
  api::AddInsertBatchEvidenceToResult(context, &result);
  return result;
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

std::string EvidenceText(const std::vector<api::EngineEvidenceReference>& evidence,
                         std::string_view kind) {
  for (const auto& entry : evidence) {
    if (entry.evidence_kind == kind) {
      return entry.evidence_id;
    }
  }
  return {};
}

std::uint64_t EvidenceU64(
    const std::vector<api::EngineEvidenceReference>& evidence,
    std::string_view kind) {
  const std::string value = EvidenceText(evidence, kind);
  return value.empty() ? 0 : static_cast<std::uint64_t>(std::stoull(value));
}

bool HasDiagnosticDetail(const api::InsertBatchContext& context,
                         std::string_view detail) {
  for (const auto& diagnostic : context.diagnostics) {
    if (diagnostic.code == "SB_ENGINE_API_INVALID_REQUEST" &&
        diagnostic.detail == detail) {
      return true;
    }
  }
  return false;
}

void RequireRefusal(const api::InsertBatchContext& context,
                    std::string_view reason) {
  Require(!context.accepted,
          "IPAR-P6-02 stale prepared descriptor was accepted");
  Require(context.prepared_descriptor_authority_refused,
          "IPAR-P6-02 stale descriptor did not mark authority refusal");
  Require(context.prepared_descriptor_refusal_reason == reason,
          "IPAR-P6-02 stale descriptor refusal reason mismatch");
  Require(HasDiagnosticDetail(
              context,
              std::string("dml.insert_rows:prepared_descriptor_authority_refused:") +
                  std::string(reason)),
          "IPAR-P6-02 stale descriptor diagnostic missing");

  const auto result = EvidenceResult(context);
  Require(HasEvidence(result.evidence,
                      "prepared_descriptor_authority_refusal",
                      reason),
          "IPAR-P6-02 authority refusal evidence missing");
  Require(HasEvidence(result.evidence,
                      "prepared_descriptor_refused_before_execution",
                      "true"),
          "IPAR-P6-02 before-execution refusal evidence missing");
  Require(HasEvidence(result.evidence,
                      "prepared_descriptor_authority_after_trim",
                      "refused_before_execution"),
          "IPAR-P6-02 trim/refusal authority evidence missing");
}

void ValidateMemoryPressureTrimAndAuthority() {
  const auto table_a = Table("table-ipar-p6-02-a");
  const auto table_b = Table("table-ipar-p6-02-b");
  const auto table_c = Table("table-ipar-p6-02-c");

  const auto first_a = Begin(
      table_a,
      InsertRequest(table_a,
                    Context("ipar-p6-02-a", table_a.table_uuid),
                    {"prepared_descriptor.cache_limit=8"}));
  Require(first_a.accepted, "IPAR-P6-02 first descriptor A refused");

  const auto first_b = Begin(
      table_b,
      InsertRequest(table_b,
                    Context("ipar-p6-02-b", table_b.table_uuid),
                    {"prepared_descriptor.cache_limit=8"}));
  Require(first_b.accepted, "IPAR-P6-02 first descriptor B refused");

  const auto first_c = Begin(
      table_c,
      InsertRequest(table_c,
                    Context("ipar-p6-02-c", table_c.table_uuid),
                    {"prepared_descriptor.cache_limit=8"}));
  Require(first_c.accepted, "IPAR-P6-02 first descriptor C refused");
  Require(first_c.prepared_descriptor_cache_size >= 3,
          "IPAR-P6-02 baseline cache did not contain hot descriptors");

  auto retained_options = ExpectedAuthorityOptions(first_b);
  AppendPressureOptions(&retained_options, 1);
  const auto retained = Begin(
      table_b,
      InsertRequest(table_b,
                    Context("ipar-p6-02-b-retained", table_b.table_uuid),
                    retained_options));
  Require(retained.accepted,
          "IPAR-P6-02 retained descriptor was refused under pressure");
  Require(retained.prepared_descriptor_cache_hit,
          "IPAR-P6-02 pressure trim did not retain current descriptor");
  Require(retained.prepared_descriptor_generation ==
              first_b.prepared_descriptor_generation,
          "IPAR-P6-02 retained descriptor generation changed");
  Require(retained.prepared_descriptor_principal_uuid ==
              first_b.prepared_descriptor_principal_uuid,
          "IPAR-P6-02 retained descriptor principal drifted");
  Require(retained.prepared_descriptor_session_uuid ==
              first_b.prepared_descriptor_session_uuid,
          "IPAR-P6-02 retained descriptor session drifted");
  Require(retained.prepared_descriptor_catalog_epoch ==
              first_b.prepared_descriptor_catalog_epoch,
          "IPAR-P6-02 retained descriptor catalog epoch drifted");
  Require(retained.prepared_descriptor_security_epoch ==
              first_b.prepared_descriptor_security_epoch,
          "IPAR-P6-02 retained descriptor security epoch drifted");
  Require(retained.prepared_descriptor_policy_epoch ==
              first_b.prepared_descriptor_policy_epoch,
          "IPAR-P6-02 retained descriptor policy epoch drifted");
  Require(retained.prepared_descriptor_memory_pressure_detected,
          "IPAR-P6-02 pressure was not detected");
  Require(retained.prepared_descriptor_trim_requested,
          "IPAR-P6-02 trim request was not recorded");
  Require(retained.prepared_descriptor_backoff_active,
          "IPAR-P6-02 cache backoff was not active");
  Require(retained.prepared_descriptor_trim_entries_before >= 3,
          "IPAR-P6-02 trim before-count did not capture hot cache");
  Require(retained.prepared_descriptor_trim_entries_after == 1,
          "IPAR-P6-02 trim after-count did not reach target");
  Require(retained.prepared_descriptor_trim_evictions >= 2,
          "IPAR-P6-02 trim did not evict hot descriptors");
  Require(retained.prepared_descriptor_cache_size == 1,
          "IPAR-P6-02 cache size did not shrink after trim");
  Require(retained.prepared_descriptor_effective_cache_limit == 1,
          "IPAR-P6-02 effective cache limit did not back off");
  Require(retained.prepared_descriptor_authority_after_trim ==
              "retained_and_revalidated",
          "IPAR-P6-02 retained descriptor was not revalidated after trim");

  const auto retained_evidence = EvidenceResult(retained);
  Require(HasEvidence(retained_evidence.evidence,
                      "prepared_descriptor_memory_pressure",
                      "ipar_p6_02_test_pressure"),
          "IPAR-P6-02 pressure detection evidence missing");
  Require(EvidenceU64(retained_evidence.evidence,
                      "prepared_descriptor_trim_target_entries") == 1,
          "IPAR-P6-02 trim target evidence mismatch");
  Require(EvidenceU64(retained_evidence.evidence,
                      "prepared_descriptor_effective_cache_limit") == 1,
          "IPAR-P6-02 effective limit evidence mismatch");
  Require(HasEvidence(retained_evidence.evidence,
                      "prepared_descriptor_authority_after_trim",
                      "retained_and_revalidated"),
          "IPAR-P6-02 retained authority evidence missing");

  auto stale_generation_options = ExpectedAuthorityOptions(first_a);
  AppendPressureOptions(&stale_generation_options, 1);
  const auto stale_generation = Begin(
      table_a,
      InsertRequest(table_a,
                    Context("ipar-p6-02-stale-generation",
                            table_a.table_uuid),
                    stale_generation_options));
  RequireRefusal(stale_generation, "evicted_or_rebound");
  Require(stale_generation.prepared_descriptor_memory_pressure_detected,
          "IPAR-P6-02 stale generation did not run under pressure");

  auto stale_key_options = PressureOptions(1);
  stale_key_options.push_back("prepared_descriptor.expected_cache_key=" +
                              first_b.prepared_descriptor_cache_key);
  const auto stale_key = Begin(
      table_a,
      InsertRequest(table_a,
                    Context("ipar-p6-02-stale-key", table_a.table_uuid),
                    stale_key_options));
  RequireRefusal(stale_key, "stale_descriptor_key");

  auto cross_session_options = ExpectedAuthorityOptions(first_b);
  AppendPressureOptions(&cross_session_options, 1);
  const auto cross_session = Begin(
      table_b,
      InsertRequest(table_b,
                    Context("ipar-p6-02-cross-session",
                            table_b.table_uuid,
                            "principal-ipar-p6-02",
                            "session-ipar-p6-02-other"),
                    cross_session_options));
  RequireRefusal(cross_session, "cross_session");
}

}  // namespace

int main() {
  ValidateMemoryPressureTrimAndAuthority();
  return EXIT_SUCCESS;
}
