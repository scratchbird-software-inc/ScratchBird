// Copyright (c) 2026 ScratchBird Software Inc.
//
// This Source Code Form is subject to the terms of the Mozilla Public
// License, v. 2.0. If a copy of the MPL was not distributed with this
// file, You can obtain one at https://mozilla.org/MPL/2.0/.
//
// SPDX-License-Identifier: MPL-2.0

#include "dml/update_immutable_authority_provider.hpp"
#include "dml/delete_security_authority_provider.hpp"
#include "dml/update_policy_catalog_authority_provider.hpp"
#include "dml/update_resource_authority_provider.hpp"
#include "database_lifecycle.hpp"
#include "local_transaction_store.hpp"
#include "physical_mga_cow_store.hpp"
#include "security/security_principal_lifecycle.hpp"
#include "uuid.hpp"
#include "../common/single_tu_allocation_fault.hpp"

#include <algorithm>
#include <array>
#include <cstdint>
#include <cstdlib>
#include <filesystem>
#include <iostream>
#include <sstream>
#include <string>
#include <string_view>
#include <thread>
#include <unistd.h>
#include <utility>
#include <vector>

namespace {

namespace engine_api = scratchbird::engine::internal_api;
namespace db = scratchbird::storage::database;
namespace mga = scratchbird::transaction::mga;
namespace uuid = scratchbird::core::uuid;
using scratchbird::core::platform::UuidKind;

constexpr std::string_view kDatabaseUuid =
    "019d5100-0000-7000-8000-000000000001";
constexpr std::string_view kPrincipalUuid =
    "019d5100-0000-7000-8000-000000000002";
constexpr std::string_view kSecurityContextUuid =
    "019d5100-0000-7000-8000-000000000003";
constexpr std::string_view kRelationUuid =
    "019d5100-0000-7000-8000-000000000010";
constexpr std::string_view kRelationOccurrenceUuid =
    "019d5100-0000-7000-8000-000000000011";
constexpr std::string_view kEmptyRelationUuid =
    "019d5100-0000-7000-8000-000000000012";
constexpr std::string_view kEmptyRelationOccurrenceUuid =
    "019d5100-0000-7000-8000-000000000013";
constexpr std::string_view kOtherRelationUuid =
    "019d5100-0000-7000-8000-000000000014";
constexpr std::string_view kPolicyUsingA =
    "019d5100-0000-7000-8000-000000000020";
constexpr std::string_view kPolicyUsingB =
    "019d5100-0000-7000-8000-000000000021";
constexpr std::string_view kPolicyCheck =
    "019d5100-0000-7000-8000-000000000022";
constexpr std::string_view kPolicyOther =
    "019d5100-0000-7000-8000-000000000023";
constexpr std::string_view kEffectiveUsing =
    "019d5100-0000-7000-8000-000000000030";
constexpr std::string_view kEffectiveCheck =
    "019d5100-0000-7000-8000-000000000031";
constexpr std::string_view kExpressionUsing =
    "019d5100-0000-7000-8000-000000000040";
constexpr std::string_view kExpressionCheck =
    "019d5100-0000-7000-8000-000000000041";
constexpr std::string_view kConstraintA =
    "019d5100-0000-7000-8000-000000000050";
constexpr std::string_view kConstraintB =
    "019d5100-0000-7000-8000-000000000051";
constexpr std::string_view kConstraintC =
    "019d5100-0000-7000-8000-000000000052";
constexpr std::string_view kConstraintExpressionA =
    "019d5100-0000-7000-8000-000000000060";
constexpr std::string_view kConstraintExpressionB =
    "019d5100-0000-7000-8000-000000000061";
constexpr std::string_view kConstraintExpressionC =
    "019d5100-0000-7000-8000-000000000062";
constexpr std::string_view kReservationA =
    "019d5100-0000-7000-8000-000000000070";
constexpr std::string_view kReservationB =
    "019d5100-0000-7000-8000-000000000071";
constexpr std::string_view kReservationC =
    "019d5100-0000-7000-8000-000000000072";
constexpr std::string_view kTriggerA =
    "019d5100-0000-7000-8000-000000000080";
constexpr std::string_view kTriggerB =
    "019d5100-0000-7000-8000-000000000081";
constexpr std::string_view kTriggerC =
    "019d5100-0000-7000-8000-000000000082";

[[noreturn]] void Fail(std::string_view message) {
  std::cerr << message << '\n';
  std::exit(EXIT_FAILURE);
}

void Require(bool condition, std::string_view message) {
  if (!condition) Fail(message);
}

bool Nonzero(const engine_api::EngineDmlUpdateSha256V1& value) {
  for (const auto byte : value) {
    if (byte != 0) return true;
  }
  return false;
}

std::string CanonicalUuid(std::uint64_t suffix) {
  std::ostringstream out;
  out << "019d5100-0000-7000-8000-" << std::hex;
  out.width(12);
  out.fill('0');
  out << suffix;
  return out.str();
}

engine_api::EngineDmlUpdateSha256V1 FilledSha(std::uint8_t value) {
  engine_api::EngineDmlUpdateSha256V1 result{};
  result.fill(value);
  return result;
}

engine_api::EngineDmlUpdateSha256V1 HexSha(std::string_view text) {
  Require(text.size() == 64, "invalid test SHA-256 hexadecimal width");
  engine_api::EngineDmlUpdateSha256V1 result{};
  auto nibble = [](char value) -> std::uint8_t {
    if (value >= '0' && value <= '9') {
      return static_cast<std::uint8_t>(value - '0');
    }
    if (value >= 'a' && value <= 'f') {
      return static_cast<std::uint8_t>(10 + value - 'a');
    }
    Fail("invalid test SHA-256 hexadecimal digit");
  };
  for (std::size_t i = 0; i < result.size(); ++i) {
    result[i] = static_cast<std::uint8_t>((nibble(text[i * 2]) << 4) |
                                          nibble(text[i * 2 + 1]));
  }
  return result;
}

class TemporaryDirectory final {
 public:
  TemporaryDirectory() {
    std::string pattern = "/tmp/sb_update_authority_provider.XXXXXX";
    std::vector<char> writable(pattern.begin(), pattern.end());
    writable.push_back('\0');
    char* made = ::mkdtemp(writable.data());
    Require(made != nullptr,
            "mkdtemp failed for update authority provider test");
    path_ = made;
  }

  ~TemporaryDirectory() { std::filesystem::remove_all(path_); }

  const std::filesystem::path& path() const { return path_; }

 private:
  std::filesystem::path path_;
};

struct DurableDatabaseFixture {
  std::filesystem::path path;
  std::string database_uuid;
  std::string principal_uuid;
  std::uint32_t page_size = 16384;
};

DurableDatabaseFixture CreateDurableDatabase(
    const std::filesystem::path& path) {
  const auto database_uuid = uuid::ParseDurableEngineIdentityUuid(
      UuidKind::database, std::string(kDatabaseUuid));
  const auto filespace_uuid = uuid::GenerateDurableEngineIdentityV7(
      UuidKind::filespace, 1788200000001ull);
  Require(database_uuid.ok() && filespace_uuid.ok(),
          "durable database fixture identity generation failed");

  db::DatabaseCreateConfig create;
  create.path = path.string();
  create.database_uuid = database_uuid.value;
  create.filespace_uuid = filespace_uuid.value;
  create.page_size = 16384;
  create.creation_unix_epoch_millis = 1788200000002ull;
  create.allow_minimal_resource_bootstrap = true;
  create.require_resource_seed_pack = false;
  create.bootstrap_principal_name = "typed_update_authority_admin";
  create.bootstrap_credential_fingerprint =
      "local-password-pbkdf2-sha256:v1:iterations=600000:"
      "salt=0123456789abcdef0123456789abcdef:"
      "verifier=4ce03aa5a5657aaf221192635ed9c63a"
      "cdb76d78a0994ec6e6ab55286e29e6a5";
  create.require_bootstrap_principal = true;
  create.allow_uncredentialed_bootstrap = false;
  const auto created = db::CreateDatabaseFile(create);
  if (!created.ok()) {
    std::cerr << created.diagnostic.diagnostic_code << ':'
              << created.diagnostic.message_key << '\n';
  }
  Require(created.ok(), "durable database fixture create failed");
  Require(db::OpenDatabaseFile({path.string(), false, false, false}).ok(),
          "durable database fixture first open failed");
  Require(db::MarkDatabaseCleanShutdown(path.string()).ok(),
          "durable database fixture clean marker failed");
  const auto bootstrap = db::ReadDatabaseBootstrapSecurityCatalog(path.string());
  Require(bootstrap.ok() && bootstrap.state.present &&
              bootstrap.state.principal_uuid.valid() &&
              bootstrap.state.security_context_generation != 0,
          "durable database bootstrap security catalog unavailable");

  DurableDatabaseFixture fixture;
  fixture.path = path;
  fixture.database_uuid = std::string(kDatabaseUuid);
  fixture.principal_uuid = uuid::UuidToString(
      bootstrap.state.principal_uuid.value);
  fixture.page_size = create.page_size;
  return fixture;
}

engine_api::EngineRequestContext BeginTransaction(
    const DurableDatabaseFixture& fixture, std::uint64_t identity_time) {
  const auto loaded =
      db::LoadLocalTransactionInventoryFromDatabase(fixture.path.string());
  Require(loaded.ok(), "durable transaction inventory load failed");
  const auto transaction_uuid = uuid::GenerateDurableEngineIdentityV7(
      UuidKind::transaction, identity_time);
  Require(transaction_uuid.ok(),
          "durable transaction identity generation failed");
  const auto begun = mga::BeginLocalTransaction(
      loaded.inventory, transaction_uuid.value, identity_time + 1);
  Require(begun.ok(), "durable transaction begin failed");
  Require(db::PersistLocalTransactionInventoryToDatabase(
              fixture.path.string(), begun.inventory)
              .ok(),
          "durable active transaction persistence failed");

  engine_api::EngineRequestContext context;
  context.trust_mode = engine_api::EngineTrustMode::embedded_in_process;
  context.database_path = fixture.path.string();
  context.database_uuid.canonical = fixture.database_uuid;
  context.database_page_size_bytes = fixture.page_size;
  context.local_transaction_id = begun.entry.identity.local_id.value;
  context.transaction_uuid.canonical =
      uuid::UuidToString(begun.entry.identity.transaction_uuid.value);
  context.principal_uuid.canonical = fixture.principal_uuid;
  context.security_context_present = true;
  context.snapshot_visible_through_local_transaction_id =
      begun.entry.begin_visible_through_local_transaction_id;
  return context;
}

void FinalizeTransaction(const engine_api::EngineRequestContext& context,
                         db::PhysicalMgaCowFinalizeDecision decision,
                         std::uint64_t final_time) {
  db::PhysicalMgaCowFinalizeRequest finalize;
  finalize.database_path = context.database_path;
  finalize.local_transaction_id =
      mga::MakeLocalTransactionId(context.local_transaction_id);
  finalize.decision = decision;
  finalize.final_unix_epoch_millis = final_time;
  Require(db::FinalizePhysicalMgaCowTransaction(finalize).ok(),
          "durable transaction finality failed");
}

engine_api::EngineRequestContext SecurityContext(
    const engine_api::EngineRequestContext& transaction,
    std::uint64_t receipt_suffix) {
  engine_api::EngineRequestContext context = transaction;
  context.statement_snapshot_uuid.canonical =
      CanonicalUuid(0x1800 + receipt_suffix);
  context.statement_receipt_uuid.canonical = CanonicalUuid(receipt_suffix);
  context.statement_metadata_snapshot_engine_owned = true;
  context.statement_metadata_snapshot_uuid.canonical = CanonicalUuid(0x2004);
  context.catalog_generation_id = 41;
  context.trace_tags.push_back("security.fixture_trace_authority");
  context.trace_tags.push_back("right:POLICY_ADMIN");
  return context;
}

engine_api::EngineSecurityRowPolicyNativeAuthorityV1 NativePolicyAuthority(
    std::uint64_t identity_suffix, std::uint8_t phase,
    std::string_view effective_policy_uuid,
    std::string_view effective_expression_uuid) {
  engine_api::EngineSecurityRowPolicyNativeAuthorityV1 authority;
  authority.present = true;
  authority.policy_version_uuid = CanonicalUuid(0x6000 + identity_suffix);
  authority.target_relation_generation = 7;
  authority.phase = phase;
  authority.effective_policy_uuid = std::string(effective_policy_uuid);
  authority.effective_policy_generation = 1;
  authority.effective_expression_uuid =
      std::string(effective_expression_uuid);
  authority.effective_expression_generation = 3;
  authority.effective_expression_evidence_sha256 =
      FilledSha(phase == 1 ? 0xb1 : 0xb2);
  authority.source_expression_uuid =
      CanonicalUuid(0x7000 + identity_suffix);
  authority.source_expression_generation = 4;
  authority.source_expression_evidence_sha256 =
      FilledSha(static_cast<std::uint8_t>(0xc0 + identity_suffix));
  return authority;
}

std::uint64_t PutPolicy(
                        const engine_api::EngineRequestContext& transaction,
                        std::uint64_t receipt_suffix,
                        std::string_view policy_uuid,
                        std::string_view target_relation_uuid,
                        engine_api::EngineSecurityRowPolicyNativeAuthorityV1
                            native_authority) {
  engine_api::EngineSecurityPutRowPolicyRequest request;
  request.context = SecurityContext(transaction, receipt_suffix);
  request.policy_uuid = std::string(policy_uuid);
  request.target_object_uuid = std::string(target_relation_uuid);
  request.target_object_kind = "relation";
  request.policy_effect = "engine_effective_expression";
  request.predicate_envelope = "redacted_fixture_policy";
  request.option_envelopes.push_back("row_security_authority:engine");
  request.native_authority = std::move(native_authority);
  const auto result = engine_api::EngineSecurityPutRowPolicy(request);
  if (!result.ok) {
    for (const auto& diagnostic : result.diagnostics) {
      std::cerr << diagnostic.code << ':' << diagnostic.detail << '\n';
    }
  }
  Require(result.ok && result.policy_persisted && result.policy_generation != 0,
          "durable row-policy seed failed");
  return result.policy_generation;
}

engine_api::EngineSecurityPrincipalLifecycleState LoadSecurityState(
    const engine_api::EngineRequestContext& context) {
  const auto loaded = engine_api::LoadSecurityPrincipalLifecycleState(
      context);
  Require(loaded.ok && loaded.state.security_generation != 0 &&
              loaded.state.policy_generation != 0 &&
              loaded.state.security_context_generation != 0,
          "durable security lifecycle state was not observable");
  return loaded.state;
}

engine_api::EngineMaterializedAuthorizationPolicy MaterializedPolicy(
    const engine_api::EngineSecurityRowPolicyRecord& durable,
    std::string_view principal_uuid,
    std::uint64_t global_policy_epoch) {
  engine_api::EngineMaterializedAuthorizationPolicy policy;
  policy.policy_uuid.canonical = durable.policy_uuid;
  policy.subject_uuid.canonical = std::string(principal_uuid);
  policy.subject_kind = "principal";
  policy.target_uuid.canonical = durable.target_object_uuid;
  policy.right = "UPDATE";
  policy.policy_kind = "row_policy";
  policy.source_policy_generation = durable.policy_generation;
  policy.policy_epoch = global_policy_epoch;
  policy.update_policy_phase = durable.update_policy_phase;
  policy.effective_policy_uuid.canonical = durable.effective_policy_uuid;
  policy.effective_policy_generation =
      durable.effective_policy_generation;
  policy.effective_expression_uuid.canonical =
      durable.effective_expression_uuid;
  policy.effective_expression_generation =
      durable.effective_expression_generation;
  policy.effective_expression_evidence_sha256 =
      durable.effective_expression_evidence_sha256;
  return policy;
}

engine_api::EngineRequestContext ProviderContext(
    const engine_api::EngineRequestContext& transaction,
    const engine_api::EngineSecurityPrincipalLifecycleState& state,
    const std::vector<engine_api::EngineMaterializedAuthorizationPolicy>&
        policies,
    std::uint64_t receipt_suffix = 0x2000) {
  engine_api::EngineRequestContext context = transaction;
  context.trust_mode = engine_api::EngineTrustMode::server_isolated;
  context.statement_snapshot_uuid.canonical = CanonicalUuid(0x2002);
  context.statement_receipt_uuid.canonical = CanonicalUuid(receipt_suffix);
  context.statement_metadata_snapshot_engine_owned = true;
  context.statement_metadata_snapshot_uuid.canonical = CanonicalUuid(0x2004);
  context.snapshot_visible_through_local_transaction_id = 1000;
  context.statement_metadata_snapshot_visible_through_local_transaction_id =
      1000;
  context.catalog_generation_id = 41;
  context.security_epoch = state.security_generation;
  context.security_context_present = true;
  context.trace_tags.push_back("private_dml_update_rows_binder");
  context.authorization_context.present = true;
  context.authorization_context.authority_uuid.canonical =
      std::string(kSecurityContextUuid);
  context.authorization_context.principal_uuid.canonical =
      transaction.principal_uuid.canonical;
  context.authorization_context.security_context_generation =
      state.security_context_generation;
  context.authorization_context.security_epoch = state.security_generation;
  context.authorization_context.policy_epoch = state.policy_generation;
  context.authorization_context.catalog_generation_id =
      context.catalog_generation_id;
  context.authorization_context.policies = policies;
  return context;
}

const engine_api::EngineSecurityRowPolicyRecord& FindPolicy(
    const engine_api::EngineSecurityPrincipalLifecycleState& state,
    std::string_view policy_uuid) {
  const auto found = std::find_if(
      state.row_policies.begin(), state.row_policies.end(),
      [&](const auto& row) { return row.policy_uuid == policy_uuid; });
  Require(found != state.row_policies.end(),
          "durable native row-policy record was not found");
  return *found;
}

engine_api::EngineDmlUpdatePolicyCatalogCaptureResultV1
CapturePolicyAuthority(
    const engine_api::EngineRequestContext& context,
    std::string_view relation_uuid,
    std::string_view relation_occurrence_uuid,
    std::uint64_t structural_occurrence_id,
    std::uint64_t descriptor_suffix) {
  engine_api::EngineDmlUpdatePolicyCatalogCaptureRequestV1 capture;
  capture.context = context;
  capture.authenticated_statement_receipt_uuid =
      context.statement_receipt_uuid.canonical;
  capture.structural_occurrence_id = structural_occurrence_id;
  capture.relation_occurrence.relation_uuid = std::string(relation_uuid);
  capture.relation_occurrence.relation_generation =
      relation_uuid == kRelationUuid ? 7 : 1;
  capture.relation_occurrence.relation_occurrence_uuid =
      std::string(relation_occurrence_uuid);
  capture.relation_occurrence.relation_occurrence_generation = 1;
  capture.catalog_snapshot_uuid =
      context.statement_metadata_snapshot_uuid.canonical;
  capture.catalog_generation = context.catalog_generation_id;
  capture.descriptor_uuid = CanonicalUuid(descriptor_suffix);
  capture.descriptor_generation = 1;
  const auto captured =
      engine_api::CaptureDmlUpdatePolicyCatalogAuthorityV1(capture);
  if (!captured.ok) {
    std::cerr << captured.diagnostic.code << ':'
              << captured.diagnostic.message_key << ':'
              << captured.diagnostic.detail << '\n';
  }
  Require(captured.ok,
          "durable policy catalog authority capture was refused");
  return captured;
}

engine_api::EngineDmlUpdateConstraintAuthoritySourceV1 ConstraintSource(
    std::string_view constraint_uuid, std::string_view expression_uuid,
    std::string_view reservation_uuid, std::uint64_t execution_order,
    engine_api::EngineDmlUpdateConstraintClassV1 constraint_class,
    std::uint8_t hash_byte) {
  engine_api::EngineDmlUpdateConstraintAuthoritySourceV1 source;
  source.catalog_snapshot_uuid = CanonicalUuid(0x2004);
  source.catalog_generation = 41;
  source.target_relation_uuid = std::string(kRelationUuid);
  source.target_relation_generation = 7;
  source.manager_execution_order_present = true;
  source.manager_execution_order = execution_order;
  source.constraint_class = constraint_class;
  source.timing =
      engine_api::EngineDmlUpdateConstraintTimingV1::immediate_row;
  source.reservation_mode =
      engine_api::EngineDmlUpdateReservationModeV1::row_reservation;
  source.constraint_uuid = std::string(constraint_uuid);
  source.constraint_generation = 4;
  source.expression_uuid = std::string(expression_uuid);
  source.expression_generation = 5;
  source.reservation_profile_uuid = std::string(reservation_uuid);
  source.reservation_profile_generation = 6;
  source.dependency_set_sha256 = FilledSha(hash_byte);
  return source;
}

engine_api::EngineDmlUpdateTriggerAuthoritySourceV1 TriggerSource(
    std::string_view trigger_uuid,
    engine_api::EngineDmlUpdateTriggerTimingV1 timing,
    std::uint64_t firing_order, std::uint64_t identity_suffix,
    std::uint64_t security_generation) {
  engine_api::EngineDmlUpdateTriggerAuthoritySourceV1 source;
  source.catalog_snapshot_uuid = CanonicalUuid(0x2004);
  source.catalog_generation = 41;
  source.target_relation_uuid = std::string(kRelationUuid);
  source.target_relation_generation = 7;
  source.security_generation = security_generation;
  source.firing_order_present = true;
  source.firing_order = firing_order;
  source.timing = timing;
  source.security_mode =
      engine_api::EngineDmlUpdateTriggerSecurityModeV1::definer;
  source.trigger_uuid = std::string(trigger_uuid);
  source.trigger_generation = 8;
  source.body_sblr_uuid = CanonicalUuid(0x3000 + identity_suffix);
  source.body_sblr_generation = 9;
  source.execution_security_context_uuid =
      CanonicalUuid(0x4000 + identity_suffix);
  source.execution_security_generation = 10;
  source.recursion_profile_uuid = CanonicalUuid(0x5000 + identity_suffix);
  source.recursion_profile_generation = 11;
  source.maximum_depth = 12;
  source.dependency_set_sha256 =
      FilledSha(static_cast<std::uint8_t>(0xc0 + identity_suffix));
  return source;
}

engine_api::EngineDmlUpdateImmutableAuthorityFreezeRequestV1 PopulatedRequest(
    const engine_api::EngineRequestContext& context,
    const engine_api::EngineSecurityPrincipalLifecycleState& state,
    const engine_api::EngineDmlUpdatePolicyCatalogCaptureResultV1& captured) {
  engine_api::EngineDmlUpdateImmutableAuthorityFreezeRequestV1 request;
  request.context = context;
  request.authenticated_statement_receipt_uuid =
      request.context.statement_receipt_uuid.canonical;
  request.structural_occurrence_id = 17;
  request.relation_occurrence.relation_uuid = std::string(kRelationUuid);
  request.relation_occurrence.relation_generation = 7;
  request.relation_occurrence.relation_occurrence_uuid =
      std::string(kRelationOccurrenceUuid);
  request.relation_occurrence.relation_occurrence_generation = 1;
  request.catalog_snapshot_uuid =
      request.context.statement_metadata_snapshot_uuid.canonical;
  request.catalog_generation = request.context.catalog_generation_id;
  request.security_policy_snapshot_authority =
      captured.security_policy_snapshot;
  request.row_policies = captured.immutable_policy_sources;
  request.constraints = {
      ConstraintSource(kConstraintC, kConstraintExpressionC, kReservationC,
                       20,
                       engine_api::EngineDmlUpdateConstraintClassV1::check,
                       0xd3),
      ConstraintSource(kConstraintB, kConstraintExpressionB, kReservationB,
                       10,
                       engine_api::EngineDmlUpdateConstraintClassV1::unique,
                       0xd2),
      ConstraintSource(kConstraintA, kConstraintExpressionA, kReservationA,
                       10,
                       engine_api::EngineDmlUpdateConstraintClassV1::not_null,
                       0xd1),
  };
  request.triggers = {
      TriggerSource(kTriggerC,
                    engine_api::EngineDmlUpdateTriggerTimingV1::before_row,
                    50, 3, state.security_generation),
      TriggerSource(
          kTriggerB,
          engine_api::EngineDmlUpdateTriggerTimingV1::before_statement, 20, 2,
          state.security_generation),
      TriggerSource(
          kTriggerA,
          engine_api::EngineDmlUpdateTriggerTimingV1::before_statement, 20, 1,
          state.security_generation),
  };

  engine_api::EngineDmlUpdateRowPolicyAuthoritySourceV1 invisible_policy;
  invisible_policy.visible = false;
  request.row_policies.push_back(invisible_policy);
  engine_api::EngineDmlUpdateConstraintAuthoritySourceV1 deleted_constraint;
  deleted_constraint.deleted = true;
  request.constraints.push_back(deleted_constraint);
  engine_api::EngineDmlUpdateTriggerAuthoritySourceV1 invisible_trigger;
  invisible_trigger.visible = false;
  request.triggers.push_back(invisible_trigger);
  return request;
}

void RequireDiagnostic(const engine_api::EngineApiDiagnostic& diagnostic,
                       std::string_view expected_code,
                       std::string_view message) {
  if (!diagnostic.error || diagnostic.code != expected_code) {
    std::cerr << diagnostic.code << ':' << diagnostic.message_key << ':'
              << diagnostic.detail << '\n';
    Fail(message);
  }
}

void TestEmptyAuthoritySets(
    const engine_api::EngineRequestContext& context,
    const engine_api::EngineSecurityPrincipalLifecycleState& state) {
  const auto captured = CapturePolicyAuthority(
      context, kEmptyRelationUuid, kEmptyRelationOccurrenceUuid, 18, 0x9001);
  engine_api::EngineDmlUpdateImmutableAuthorityFreezeRequestV1 request;
  request.context = context;
  request.authenticated_statement_receipt_uuid =
      request.context.statement_receipt_uuid.canonical;
  request.structural_occurrence_id = 18;
  request.relation_occurrence.relation_uuid =
      std::string(kEmptyRelationUuid);
  request.relation_occurrence.relation_generation = 1;
  request.relation_occurrence.relation_occurrence_uuid =
      std::string(kEmptyRelationOccurrenceUuid);
  request.relation_occurrence.relation_occurrence_generation = 1;
  request.catalog_snapshot_uuid =
      request.context.statement_metadata_snapshot_uuid.canonical;
  request.catalog_generation = request.context.catalog_generation_id;
  request.security_policy_snapshot_authority =
      captured.security_policy_snapshot;
  request.row_policies = captured.immutable_policy_sources;

  const auto frozen = engine_api::FreezeDmlUpdateImmutableAuthorityV1(request);
  Require(frozen.ok, "empty immutable authority freeze was refused");
  const auto& snapshot = frozen.snapshot;
  Require(snapshot.security_policy_snapshot.snapshot_generation == 1 &&
              snapshot.security_policy_snapshot.admitted_policy_rows.empty(),
          "empty freeze did not issue an observed durable security snapshot");
  Require(snapshot.row_policy_set.records.empty() &&
              snapshot.constraint_set.records.empty() &&
              snapshot.trigger_set.records.empty(),
          "empty freeze invented authority records");
  Require(snapshot.row_policy_set.set_generation == 1 &&
              snapshot.constraint_set.set_generation == 1 &&
              snapshot.trigger_set.set_generation == 1 &&
              snapshot.row_policy_set.set_uuid !=
                  snapshot.constraint_set.set_uuid &&
              snapshot.row_policy_set.set_uuid != snapshot.trigger_set.set_uuid &&
              snapshot.constraint_set.set_uuid != snapshot.trigger_set.set_uuid,
          "empty freeze did not issue distinct fresh set identities");
  Require(
      snapshot.row_policy_set.vector_sha256 ==
              HexSha("5098911e2e6a965cad2c3c70c9016109cfb215c8e02bf9f247cb9d509779e6ee") &&
          snapshot.constraint_set.vector_sha256 ==
              HexSha("a9c68812c054e26b4bee8f2682dc5b213d137665f00f109be36ed266c027112a") &&
          snapshot.trigger_set.vector_sha256 ==
              HexSha("785b35322120f590260539297f99a8e1199d66970ccc560dc3cd4eeed9b941ff"),
      "empty vector hashes do not match independent Core-domain oracles");
}

void TestOrderingCollapseAndRevalidation(
    const engine_api::EngineRequestContext& context,
    const engine_api::EngineSecurityPrincipalLifecycleState& state,
    const engine_api::EngineDmlUpdatePolicyCatalogCaptureResultV1& captured) {
  const auto request = PopulatedRequest(context, state, captured);
  const auto frozen = engine_api::FreezeDmlUpdateImmutableAuthorityV1(request);
  Require(frozen.ok, "populated immutable authority freeze was refused");
  const auto& snapshot = frozen.snapshot;

  Require(snapshot.security_policy_snapshot.security_context_uuid ==
              kSecurityContextUuid &&
              snapshot.security_policy_snapshot.snapshot_uuid !=
                  kSecurityContextUuid &&
              snapshot.security_policy_snapshot.admitted_policy_rows.size() ==
                  3 &&
              snapshot.security_policy_snapshot.admitted_policy_rows[0]
                      .policy_uuid == kPolicyUsingA &&
              snapshot.security_policy_snapshot.admitted_policy_rows[1]
                      .policy_uuid == kPolicyUsingB &&
              snapshot.security_policy_snapshot.admitted_policy_rows[2]
                      .policy_uuid == kPolicyCheck,
          "security lifecycle authority did not issue a distinct durable snapshot");
  Require(snapshot.row_policy_set.records.size() == 2 &&
              snapshot.row_policy_set.records[0].policy_ordinal == 1 &&
              snapshot.row_policy_set.records[0].phase ==
                  engine_api::EngineDmlUpdateRowPolicyPhaseV1::using_filter &&
              snapshot.row_policy_set.records[1].policy_ordinal == 2 &&
              snapshot.row_policy_set.records[1].phase ==
                  engine_api::EngineDmlUpdateRowPolicyPhaseV1::with_check &&
              snapshot.row_policy_set.records[0].effective_policy_uuid ==
                  kEffectiveUsing &&
              snapshot.row_policy_set.records[1].effective_policy_uuid ==
                  kEffectiveCheck &&
              snapshot.row_policy_set.records[0].security_snapshot_uuid ==
                  snapshot.security_policy_snapshot.snapshot_uuid &&
              snapshot.row_policy_set.records[1].security_snapshot_uuid ==
                  snapshot.security_policy_snapshot.snapshot_uuid &&
              snapshot.row_policy_set.records[0]
                      .source_policy_catalog_vector_sha256 ==
                  captured.source_policy_vector.identity.vector_sha256 &&
              snapshot.row_policy_set.records[1]
                      .source_policy_catalog_vector_sha256 ==
                  captured.source_policy_vector.identity.vector_sha256,
          "row-policy authority was not collapsed and ordered USING before WITH CHECK");

  Require(snapshot.constraint_set.records.size() == 3 &&
              snapshot.constraint_set.records[0].constraint_uuid ==
                  kConstraintA &&
              snapshot.constraint_set.records[1].constraint_uuid ==
                  kConstraintB &&
              snapshot.constraint_set.records[2].constraint_uuid ==
                  kConstraintC &&
              snapshot.constraint_set.records[0].constraint_ordinal == 1 &&
              snapshot.constraint_set.records[2].constraint_ordinal == 3,
          "constraint manager ordering and UUID tie-break were not preserved");
  Require(snapshot.trigger_set.records.size() == 3 &&
              snapshot.trigger_set.records[0].trigger_uuid == kTriggerA &&
              snapshot.trigger_set.records[1].trigger_uuid == kTriggerB &&
              snapshot.trigger_set.records[2].trigger_uuid == kTriggerC &&
              snapshot.trigger_set.records[0].trigger_ordinal == 1 &&
              snapshot.trigger_set.records[2].trigger_ordinal == 3,
          "trigger timing, firing order, and UUID ordering were not preserved");
  for (const auto& record : snapshot.row_policy_set.records) {
    Require(Nonzero(record.record_evidence_sha256),
            "row-policy record evidence hash is zero");
  }
  for (const auto& record : snapshot.constraint_set.records) {
    Require(Nonzero(record.record_evidence_sha256),
            "constraint record evidence hash is zero");
  }
  for (const auto& record : snapshot.trigger_set.records) {
    Require(Nonzero(record.record_evidence_sha256),
            "trigger record evidence hash is zero");
  }

  const auto repeated =
      engine_api::FreezeDmlUpdateImmutableAuthorityV1(request);
  Require(repeated.ok &&
              repeated.snapshot.row_policy_set.set_uuid !=
                  snapshot.row_policy_set.set_uuid &&
              repeated.snapshot.constraint_set.set_uuid !=
                  snapshot.constraint_set.set_uuid &&
              repeated.snapshot.trigger_set.set_uuid !=
                  snapshot.trigger_set.set_uuid &&
              repeated.snapshot.security_policy_snapshot.snapshot_uuid ==
                  snapshot.security_policy_snapshot.snapshot_uuid,
          "repeated freeze did not preserve the preissued security snapshot or reused a set identity");
  Require(repeated.snapshot.constraint_set.vector_sha256 ==
                  snapshot.constraint_set.vector_sha256 &&
              repeated.snapshot.trigger_set.vector_sha256 ==
                  snapshot.trigger_set.vector_sha256 &&
              repeated.snapshot.row_policy_set.vector_sha256 ==
                  snapshot.row_policy_set.vector_sha256,
          "stable typed authority source hashes drifted across freezes");

  engine_api::EngineDmlUpdateImmutableAuthorityRevalidateRequestV1 revalidate;
  revalidate.current = request;
  revalidate.current.context.trace_tags.clear();
  revalidate.current.context.trace_tags.push_back(
      "private_dml_update_rows_consumer");
  revalidate.admitted = snapshot;
  Require(engine_api::RevalidateDmlUpdateImmutableAuthorityV1(revalidate).ok,
          "unchanged immutable authority snapshot failed revalidation");

  auto binder_revalidate = revalidate;
  binder_revalidate.current.context.trace_tags.clear();
  binder_revalidate.current.context.trace_tags.push_back(
      "private_dml_update_rows_binder");
  const auto binder_refused =
      engine_api::RevalidateDmlUpdateImmutableAuthorityV1(binder_revalidate);
  RequireDiagnostic(binder_refused.diagnostic,
                    engine_api::kDmlUpdateAuthorityDiagnosticAccessDenied,
                    "binder capability was accepted for revalidation");

  auto changed_generation = revalidate;
  ++changed_generation.current.constraints[0].constraint_generation;
  const auto generation_refused =
      engine_api::RevalidateDmlUpdateImmutableAuthorityV1(changed_generation);
  RequireDiagnostic(generation_refused.diagnostic,
                    engine_api::kDmlUpdateAuthorityDiagnosticStale,
                    "changed source generation was accepted");

  auto cross_receipt = revalidate;
  cross_receipt.current.authenticated_statement_receipt_uuid =
      CanonicalUuid(0x2100);
  cross_receipt.current.context.statement_receipt_uuid.canonical =
      cross_receipt.current.authenticated_statement_receipt_uuid;
  const auto receipt_refused =
      engine_api::RevalidateDmlUpdateImmutableAuthorityV1(cross_receipt);
  RequireDiagnostic(receipt_refused.diagnostic,
                    engine_api::kDmlUpdateAuthorityDiagnosticStale,
                    "cross-receipt authority snapshot was accepted");

  auto cross_occurrence = revalidate;
  cross_occurrence.current.relation_occurrence.relation_occurrence_uuid =
      CanonicalUuid(0x2101);
  const auto occurrence_refused =
      engine_api::RevalidateDmlUpdateImmutableAuthorityV1(cross_occurrence);
  RequireDiagnostic(occurrence_refused.diagnostic,
                    engine_api::kDmlUpdateAuthorityDiagnosticStale,
                    "cross-occurrence authority snapshot was accepted");

  auto cross_relation = revalidate;
  cross_relation.current.relation_occurrence.relation_uuid =
      std::string(kEmptyRelationUuid);
  cross_relation.current.relation_occurrence.relation_occurrence_uuid =
      std::string(kEmptyRelationOccurrenceUuid);
  const auto relation_refused =
      engine_api::RevalidateDmlUpdateImmutableAuthorityV1(cross_relation);
  RequireDiagnostic(relation_refused.diagnostic,
                    engine_api::kDmlUpdateAuthorityDiagnosticStale,
                    "cross-relation authority snapshot was accepted");

  auto forged_set_generation = revalidate;
  forged_set_generation.admitted.row_policy_set.set_generation = 2;
  const auto set_generation_refused =
      engine_api::RevalidateDmlUpdateImmutableAuthorityV1(
          forged_set_generation);
  RequireDiagnostic(set_generation_refused.diagnostic,
                    engine_api::kDmlUpdateAuthorityDiagnosticStale,
                    "forged authority set generation was accepted");
}

void TestRefusalCases(
    const engine_api::EngineRequestContext& context,
    const engine_api::EngineSecurityPrincipalLifecycleState& state,
    const engine_api::EngineDmlUpdatePolicyCatalogCaptureResultV1& captured) {
  const auto request = PopulatedRequest(context, state, captured);

  auto missing_typed_authority = request;
  missing_typed_authority.constraints[0].reservation_profile_uuid.clear();
  const auto missing_refused = engine_api::FreezeDmlUpdateImmutableAuthorityV1(
      missing_typed_authority);
  RequireDiagnostic(missing_refused.diagnostic,
                    engine_api::kDmlUpdateAuthorityDiagnosticInvalid,
                    "missing typed constraint authority was accepted");

  auto autonomous_request = request;
  autonomous_request.autonomous_transaction = true;
  const auto autonomous_refused =
      engine_api::FreezeDmlUpdateImmutableAuthorityV1(autonomous_request);
  RequireDiagnostic(autonomous_refused.diagnostic,
                    engine_api::kDmlUpdateAuthorityDiagnosticUnsupported,
                    "autonomous UPDATE authority was accepted");

  auto autonomous_trigger = request;
  autonomous_trigger.triggers[0].autonomous = true;
  const auto trigger_refused =
      engine_api::FreezeDmlUpdateImmutableAuthorityV1(autonomous_trigger);
  RequireDiagnostic(trigger_refused.diagnostic,
                    engine_api::kDmlUpdateAuthorityDiagnosticUnsupported,
                    "autonomous trigger authority was accepted");

  auto lifecycle_mismatch = request;
  ++lifecycle_mismatch.context.authorization_context.policies[0]
        .source_policy_generation;
  const auto lifecycle_refused =
      engine_api::FreezeDmlUpdateImmutableAuthorityV1(lifecycle_mismatch);
  Require(!lifecycle_refused.ok && lifecycle_refused.diagnostic.error,
          "durable/materialized policy generation mismatch was accepted");

  auto cross_phase = request;
  cross_phase.context.authorization_context.policies[0]
      .update_policy_phase = 2;
  const auto phase_refused =
      engine_api::FreezeDmlUpdateImmutableAuthorityV1(cross_phase);
  Require(!phase_refused.ok && phase_refused.diagnostic.error,
          "cross-phase materialized policy authority was accepted");

  auto missing_evidence = request;
  missing_evidence.context.authorization_context.policies[0]
      .effective_expression_evidence_sha256.fill(0);
  const auto evidence_refused =
      engine_api::FreezeDmlUpdateImmutableAuthorityV1(missing_evidence);
  Require(!evidence_refused.ok && evidence_refused.diagnostic.error,
          "materialized policy with missing native evidence was accepted");

  auto conflicting_identity = request;
  conflicting_identity.context.authorization_context.policies[0]
      .effective_policy_uuid.canonical = CanonicalUuid(0x9f01);
  const auto identity_refused =
      engine_api::FreezeDmlUpdateImmutableAuthorityV1(conflicting_identity);
  Require(!identity_refused.ok && identity_refused.diagnostic.error,
          "conflicting effective policy identity was accepted");

  auto source_mismatch = request;
  ++source_mismatch.row_policies[0].source_policy_generation;
  const auto source_refused =
      engine_api::FreezeDmlUpdateImmutableAuthorityV1(source_mismatch);
  RequireDiagnostic(source_refused.diagnostic,
                    engine_api::kDmlUpdateAuthorityDiagnosticStale,
                    "policy source outside durable admitted identities was accepted");
}

engine_api::EngineDmlUpdateResourceReleaseCodeV1 ReleaseWithAllocationDenied(
    engine_api::EngineDmlUpdateResourceGovernorV1& governor,
    const engine_api::EngineRequestContext& context,
    const engine_api::EngineDmlUpdateResourceHandleV1& handle,
    engine_api::EngineDmlUpdateResourceReleaseV1 reason) {
  allocation_attempts = 0;
  allocations_before_failure = 0;
  const auto result = governor.ReleaseNoAlloc(context, handle, reason);
  allocations_before_failure = -1;
  Require(allocation_attempts == 0, "terminal resource release attempted allocation");
  return result;
}

void TestResourceNoAllocationCompletion(const engine_api::EngineRequestContext& context) {
  using Governor = engine_api::EngineDmlUpdateResourceGovernorV1;
  using Reason = engine_api::EngineDmlUpdateResourceReleaseV1;
  using Code = engine_api::EngineDmlUpdateResourceReleaseCodeV1;
  Governor governor, other;
  Require(!governor.Configure({4, 16, 32, 2, 64, 64, 128, 4}).error, "scalar release policy failed");
  auto retained_context = context;
  unsigned probes = 0;
  retained_context.query_cancellation_requested = [&] { ++probes; return false; };
  const auto a = governor.Capture(retained_context);
  const auto b = governor.Capture(retained_context);
  Require(a.ok && b.ok, "scalar release fixture failed capture");
  auto foreign = retained_context;
  foreign.statement_receipt_uuid.canonical = CanonicalUuid(0xa005);
  Require(ReleaseWithAllocationDenied(governor, foreign, a.handle, Reason::abandoned_before_publication) == Code::owner_mismatch &&
              ReleaseWithAllocationDenied(other, retained_context, a.handle, Reason::abandoned_before_publication) == Code::owner_mismatch &&
              ReleaseWithAllocationDenied(governor, retained_context, {}, Reason::abandoned_before_publication) == Code::owner_mismatch &&
              ReleaseWithAllocationDenied(governor, retained_context, a.handle, static_cast<Reason>(255)) == Code::invalid_disposition &&
              ReleaseWithAllocationDenied(governor, retained_context, a.handle, Reason::published) == Code::phase_mismatch,
          "scalar release refusal checks failed");
  Require(governor.Observe().reserved_canonical_bytes == 128, "refusal returned resource capacity");
  Require(!governor.PublishDescriptor(retained_context, a.handle).error &&
              !governor.Cancel(retained_context, a.handle).error, "scalar abort fixture failed");
  const auto probes_before = probes;
  Require(ReleaseWithAllocationDenied(governor, retained_context, a.handle, Reason::abandoned_before_publication) == Code::phase_mismatch &&
              ReleaseWithAllocationDenied(governor, retained_context, a.handle, Reason::aborted) == Code::released &&
              ReleaseWithAllocationDenied(governor, retained_context, a.handle, Reason::aborted) == Code::already_released &&
              ReleaseWithAllocationDenied(governor, retained_context, a.handle, Reason::published) == Code::conflicting_disposition,
          "scalar aborted lifecycle failed");
  Require(probes == probes_before && governor.Observe().reserved_canonical_bytes == 64,
          "scalar release invoked callback or returned unrelated capacity");
  Require(!governor.PublishDescriptor(retained_context, b.handle).error, "scalar publish fixture failed");
  std::array<Code, 8> results{};
  std::vector<std::thread> threads;
  for (std::size_t i = 0; i < results.size(); ++i)
    threads.emplace_back([&, i] {
      results[i] = ReleaseWithAllocationDenied(governor, retained_context, b.handle, Reason::published);
    });
  for (auto& t : threads) t.join();
  Require(std::count(results.begin(), results.end(), Code::released) == 1 &&
              std::count(results.begin(), results.end(), Code::already_released) == 7 &&
              governor.Observe().active_grants == 0 && governor.Observe().released_grants == 2 &&
              governor.Observe().reserved_canonical_bytes == 0,
          "concurrent scalar releases were not exactly once");
  const auto abandoned = governor.Capture(retained_context);
  Require(abandoned.ok && ReleaseWithAllocationDenied(governor, retained_context, abandoned.handle,
              Reason::abandoned_before_publication) == Code::released, "scalar abandonment failed");
  Require(!governor.Configure({4, 16, 32, 2, 64, 64, 128, 4}).error &&
              ReleaseWithAllocationDenied(governor, retained_context, a.handle, Reason::aborted) == Code::already_released,
          "policy replacement broke retained terminal disposition");
  const auto shutdown_grant = governor.Capture(retained_context);
  Require(shutdown_grant.ok, "shutdown resource fixture failed");
  governor.StopAdmission();
  Require(!governor.Capture(retained_context).ok &&
              governor.Revalidate(retained_context, shutdown_grant.handle, *shutdown_grant.handle.carrier()).error &&
              governor.Configure({4, 16, 32, 2, 64, 64, 128, 4}).error,
          "stopped runtime admitted resource use or reopened policy");
  Require(ReleaseWithAllocationDenied(governor, retained_context, shutdown_grant.handle,
              Reason::abandoned_before_publication) == Code::released,
          "stopped runtime prevented resource-only cleanup");
  std::cout << "resource terminal release/refusals/concurrent retry: zero allocation attempts\n";
}

engine_api::EngineDmlUpdateResourcePublicationCodeV1 CompleteWithAllocationDenied(
    const engine_api::EngineDmlUpdateResourcePublicationV1& publication,
    engine_api::EngineDmlUpdateResourcePublicationOutcomeV1 outcome) {
  allocation_attempts = 0;
  allocations_before_failure = 0;
  const auto result = publication.CompleteNoAlloc(outcome);
  allocations_before_failure = -1;
  Require(allocation_attempts == 0, "resource publication attempted allocation");
  return result;
}

void TestResourcePreparedPublication(engine_api::EngineRequestContext context) {
  using Governor = engine_api::EngineDmlUpdateResourceGovernorV1;
  using Outcome = engine_api::EngineDmlUpdateResourcePublicationOutcomeV1;
  using Code = engine_api::EngineDmlUpdateResourcePublicationCodeV1;
  using Release = engine_api::EngineDmlUpdateResourceReleaseV1;
  using ReleaseCode = engine_api::EngineDmlUpdateResourceReleaseCodeV1;
  Governor governor, foreign;
  Require(!governor.Configure({4, 16, 32, 2, 64, 64, 128, 4}).error, "publication policy failed");
  unsigned probes = 0;
  bool cancelled = false;
  context.query_cancellation_requested = [&] { ++probes; return cancelled; };
  auto grant = governor.Capture(context);
  Require(grant.ok && !foreign.PrepareDescriptorPublication(context, grant.handle).ok &&
              !governor.PrepareDescriptorPublication(context, {}).ok, "publication ownership refusal failed");
  auto wrong_context = context;
  wrong_context.resource_epoch++;
  Require(!governor.PrepareDescriptorPublication(wrong_context, grant.handle).ok,
          "publication accepted altered receipt context");

  auto prepared = governor.PrepareDescriptorPublication(context, grant.handle);
  Require(prepared.ok && prepared.publication.valid(), "publication preparation failed");
  Require(!governor.PrepareDescriptorPublication(context, grant.handle).ok &&
              governor.Revalidate(context, grant.handle, *grant.handle.carrier()).error,
          "pending publication admitted another preparation or execution");
  for (auto reason : {Release::abandoned_before_publication, Release::published, Release::aborted})
    Require(ReleaseWithAllocationDenied(governor, context, grant.handle, reason) == ReleaseCode::phase_mismatch,
            "uncertain publication returned capacity");
  Require(CompleteWithAllocationDenied({}, Outcome::published) == Code::invalid_handle &&
              CompleteWithAllocationDenied(prepared.publication, static_cast<Outcome>(255)) == Code::invalid_disposition,
          "invalid publication completion accepted");
  auto retained = prepared.publication;
  prepared.publication = {};
  Require(governor.Observe().reserved_canonical_bytes == 64 &&
              ReleaseWithAllocationDenied(governor, context, grant.handle, Release::abandoned_before_publication) ==
                  ReleaseCode::phase_mismatch, "ticket destruction resolved an uncertain publication");
  const auto probes_before = probes;
  Require(CompleteWithAllocationDenied(retained, Outcome::not_published) == Code::completed &&
              CompleteWithAllocationDenied(retained, Outcome::not_published) == Code::already_completed &&
              CompleteWithAllocationDenied(retained, Outcome::published) == Code::conflicting_disposition &&
              probes == probes_before && governor.Observe().reserved_canonical_bytes == 64,
          "negative publication decision probed, released, or changed its outcome");

  // A resolved non-write may be prepared again. An old ticket must not change
  // the new window, even with a conflicting completion request.
  prepared = governor.PrepareDescriptorPublication(context, grant.handle);
  Require(prepared.ok && CompleteWithAllocationDenied(retained, Outcome::published) == Code::conflicting_disposition &&
              ReleaseWithAllocationDenied(governor, context, grant.handle, Release::abandoned_before_publication) ==
                  ReleaseCode::phase_mismatch, "old ticket changed a replacement publication window");
  cancelled = true;
  Require(!governor.Cancel(context, grant.handle).error, "pending cancellation failed");
  governor.StopAdmission();
  const auto stopped_probes = probes;
  std::array<Code, 8> results{};
  std::vector<std::thread> threads;
  for (std::size_t i = 0; i < results.size(); ++i)
    threads.emplace_back([&, i] { results[i] = CompleteWithAllocationDenied(prepared.publication, Outcome::published); });
  for (auto& t : threads) t.join();
  Require(std::count(results.begin(), results.end(), Code::completed) == 1 &&
              std::count(results.begin(), results.end(), Code::already_completed) == 7 && probes == stopped_probes &&
              governor.Observe().reserved_canonical_bytes == 64,
          "post-write completion was not exactly once or consulted revoked live authority");
  Require(ReleaseWithAllocationDenied(governor, context, grant.handle, Release::abandoned_before_publication) ==
              ReleaseCode::phase_mismatch &&
              ReleaseWithAllocationDenied(governor, context, grant.handle, Release::published) == ReleaseCode::released &&
              CompleteWithAllocationDenied(prepared.publication, Outcome::published) == Code::already_completed,
          "bound publication permitted abandonment or broke terminal retry");

  // Persistent allocation denial through unwinding at every preparation
  // allocation: no failed attempt may strand a pending-publication pin.
  Governor fault_governor;
  cancelled = false;
  Require(!fault_governor.Configure({4, 16, 32, 2, 64, 64, 128, 4}).error, "fault policy failed");
  std::size_t failures = 0;
  bool reached_success = false;
  for (std::ptrdiff_t failure = 0; failure < 1024; ++failure) {
    auto captured = fault_governor.Capture(context);
    Require(captured.ok, "allocation sweep capture failed");
    engine_api::EngineDmlUpdateResourcePublicationPreparationV1 attempt;
    bool threw = false;
    allocation_attempts = 0;
    allocations_before_failure = failure;
    try { attempt = fault_governor.PrepareDescriptorPublication(context, captured.handle); }
    catch (const std::bad_alloc&) { threw = true; }
    allocations_before_failure = -1;
    Require(fault_governor.Observe().active_grants == 1 &&
                fault_governor.Observe().reserved_canonical_bytes == 64,
            "publication allocation failure changed resource accounting");
    if (attempt.ok) {
      Require(!threw && CompleteWithAllocationDenied(attempt.publication, Outcome::not_published) == Code::completed,
              "allocation sweep success failed resolution");
      reached_success = true;
    } else {
      Require(!attempt.publication.valid(), "failed preparation returned a publication ticket");
      ++failures;
    }
    Require(ReleaseWithAllocationDenied(fault_governor, context, captured.handle, Release::abandoned_before_publication) ==
                ReleaseCode::released, "failed preparation stranded a publication pin");
    if (reached_success) break;
  }
  Require(reached_success && failures > 0 && fault_governor.Observe().active_grants == 0,
          "publication preparation allocation sweep did not complete");

  auto raced_grant = fault_governor.Capture(context);
  Require(raced_grant.ok, "conflicting publication capture failed");
  auto raced = fault_governor.PrepareDescriptorPublication(context, raced_grant.handle);
  Require(raced.ok, "conflicting publication preparation failed");
  threads.clear();
  for (std::size_t i = 0; i < results.size(); ++i)
    threads.emplace_back([&, i] {
      results[i] = CompleteWithAllocationDenied(raced.publication,
          i % 2 ? Outcome::published : Outcome::not_published);
    });
  for (auto& t : threads) t.join();
  Require(std::count(results.begin(), results.end(), Code::completed) == 1 &&
              std::count(results.begin(), results.end(), Code::already_completed) == 3 &&
              std::count(results.begin(), results.end(), Code::conflicting_disposition) == 4,
          "concurrent conflicting decisions did not preserve one outcome");
  const auto winner = std::find(results.begin(), results.end(), Code::completed) - results.begin();
  Require(ReleaseWithAllocationDenied(fault_governor, context, raced_grant.handle,
              winner % 2 ? Release::aborted : Release::abandoned_before_publication) == ReleaseCode::released,
          "conflicting publication race corrupted the winning phase");

  {
    Governor unresolved;
    Require(!unresolved.Configure({4, 16, 32, 2, 64, 64, 128, 4}).error, "unresolved policy failed");
    const auto captured = unresolved.Capture(context);
    Require(captured.ok, "unresolved capture failed");
    {
      const auto lost = unresolved.PrepareDescriptorPublication(context, captured.handle);
      Require(lost.ok, "unresolved preparation failed");
    }
    Require(unresolved.Observe().active_grants == 1 && unresolved.Observe().reserved_canonical_bytes == 64 &&
                ReleaseWithAllocationDenied(unresolved, context, captured.handle,
                    Release::abandoned_before_publication) == ReleaseCode::phase_mismatch,
            "dropping all publication tickets silently resolved an uncertain write");
    // This deliberately lost process-local ticket cannot be reconciled by this
    // component. Instance destruction is not a durable transaction decision.
  }
  std::cout << "resource publication: " << failures << " preparation allocation failures; zero-allocation completion passed\n";
}

void TestResourceGovernorLifecycle(engine_api::EngineRequestContext context) {
  using Governor = engine_api::EngineDmlUpdateResourceGovernorV1;
  using Release = engine_api::EngineDmlUpdateResourceReleaseV1;
  namespace wire = scratchbird::wire;
  context.statement_uuid.canonical = CanonicalUuid(0xa001);
  context.session_uuid.canonical = CanonicalUuid(0xa002);
  context.resource_admission_uuid.canonical = CanonicalUuid(0xa003);
  context.resource_epoch = 77;
  TestResourceNoAllocationCompletion(context);
  TestResourcePreparedPublication(context);
  Governor governor;
  Require(!governor.Capture(context).ok, "unconfigured resource governor admitted work");
  engine_api::EngineDmlUpdateResourcePolicyV1 policy{4, 16, 32, 2, 64, 64, 128, 4};
  Require(governor.Configure({}).error, "zero/unbounded resource policy accepted");
  Require(!governor.Configure(policy).error, "resource policy installation failed");
  auto bad_policy = policy;
  bad_policy.maximum_total_canonical_value_bytes = wire::kTypedUpdateMaximumCanonicalValueBytes + 1;
  Require(governor.Configure(bad_policy).error && governor.Observe().policy_generation == 1,
          "invalid policy changed the live governor generation");
  const auto first = governor.Capture(context);
  const auto second = governor.Capture(context);
  Require(first.ok && second.ok, "live resource grants failed capture");
  const auto& carrier = *first.handle.carrier();
  Require(carrier.exact_bytes.size() == 208 && carrier.resource_budget_generation == 1 &&
              carrier.resource_budget_generation != context.resource_epoch &&
              second.handle.carrier()->grant_receipt_generation > carrier.grant_receipt_generation &&
              second.handle.carrier()->resource_budget_uuid != carrier.resource_budget_uuid &&
              second.handle.carrier()->cancellation_token_uuid != carrier.cancellation_token_uuid &&
              second.handle.carrier()->grant_receipt_uuid != carrier.grant_receipt_uuid,
          "DUBR identities/generations did not come from live governor issuance");
  const auto txn = uuid::ParseUuid(context.transaction_uuid.canonical);
  Require(std::equal(txn.value.bytes.begin(), txn.value.bytes.end(), carrier.exact_bytes.begin() + 56),
          "DUBR transaction UUID was not binary16");
  Require(governor.Observe().active_grants == 2 && governor.Observe().reserved_canonical_bytes == 128 &&
              !governor.Capture(context).ok && governor.Observe().active_grants == 2,
          "aggregate quota did not debit/refuse without leaking a handle");
  Require(governor.Configure(policy).error, "policy changed underneath active grants");
  Require(!governor.Revalidate(context, first.handle, carrier).error, "live DUBR failed revalidation");
  for (unsigned field = 0; field < 5; ++field) {
    auto changed = carrier;
    if (field == 0) changed.maximum_assignments += 1;
    if (field == 1) changed.evidence_sha256[0] ^= 1;
    if (field == 2) changed.grant_receipt_generation += 1;
    if (field == 3) changed.exact_bytes[72] ^= 1;
    if (field == 4) {
      changed.maximum_candidate_rows -= 1;
      std::vector<std::uint8_t> encoded;
      wire::TypedUpdateCarrierError error;
      Require(wire::EncodeTypedUpdateResourceBudget(changed, &encoded, &error) &&
                  wire::DecodeAndValidateTypedUpdateResourceBudget(encoded, &changed, &error),
              "tampered-but-self-consistent DUBR fixture failed");
    }
    Require(governor.Revalidate(context, first.handle, changed).error,
            "modified DUBR substituted for retained resource authority");
  }
  auto changed = context;
  changed.statement_receipt_uuid.canonical = CanonicalUuid(0xa004);
  Require(governor.Revalidate(changed, first.handle, carrier).error &&
              governor.Cancel(changed, first.handle).error &&
              governor.Release(changed, first.handle, Release::abandoned_before_publication).error,
          "cross-receipt resource operation accepted");
  changed = context;
  changed.statement_metadata_snapshot_active_excluded_local_transaction_ids.push_back(9000);
  Require(governor.Revalidate(changed, first.handle, carrier).error,
          "resource handle ignored metadata exclusion changes");
  Governor other;
  Require(other.Revalidate(context, first.handle, carrier).error &&
              other.Release(context, first.handle, Release::abandoned_before_publication).error,
          "resource handle crossed governor ownership");
  Require(!governor.PublishDescriptor(context, first.handle).error,
          "resource grant did not enter published-descriptor phase");
  Require(governor.Release(context, first.handle, Release::abandoned_before_publication).error,
          "published descriptor was released as abandoned");
  Require(!governor.Cancel(context, first.handle).error &&
              governor.Revalidate(context, first.handle, carrier).code == "PROCESS.CANCELLED" &&
              governor.Observe().reserved_canonical_bytes == 128,
          "cancellation released capacity before terminal cleanup");
  auto bad_cancelled = carrier;
  bad_cancelled.evidence_sha256[0] ^= 1;
  Require(governor.Revalidate(context, first.handle, bad_cancelled).code == "RESOURCE.BUDGET_EXCEEDED",
          "cancellation masked malformed resource authority");
  Require(!governor.Release(context, first.handle, Release::aborted).error &&
              !governor.Release(context, first.handle, Release::aborted).error &&
              governor.Observe().released_grants == 1 && governor.Observe().reserved_canonical_bytes == 64,
          "terminal abort did not release exactly once");
  Require(governor.Release(context, first.handle, Release::published).error &&
              governor.Revalidate(context, first.handle, carrier).error,
          "released grant was reused or given a conflicting disposition");
  Require(governor.Release(context, second.handle, Release::published).error,
          "unpublished grant accepted a terminal published disposition");
  Require(!governor.PublishDescriptor(context, second.handle).error &&
              !governor.Release(context, second.handle, Release::published).error &&
              governor.Observe().reserved_canonical_bytes == 0,
          "terminal published release did not drain the governor");
  Require(!governor.Configure(policy).error && governor.Observe().policy_generation == 2,
          "drained policy replacement did not advance governor generation");
  std::array<engine_api::EngineDmlUpdateResourceCaptureV1, 8> concurrent;
  std::vector<std::thread> threads;
  for (std::size_t i = 0; i < concurrent.size(); ++i)
    threads.emplace_back([&, i] { concurrent[i] = governor.Capture(context); });
  for (auto& thread : threads) thread.join();
  Require(std::count_if(concurrent.begin(), concurrent.end(), [](const auto& c) { return c.ok; }) == 2 &&
              governor.Observe().reserved_canonical_bytes == 128,
          "concurrent captures exceeded governor capacity");
  for (const auto& captured : concurrent) if (captured.ok)
    Require(!governor.Release(context, captured.handle, Release::abandoned_before_publication).error,
            "concurrent abandoned grant did not release");
  bool cancel = false;
  auto probe_context = context;
  probe_context.query_cancellation_requested = [&cancel] { return cancel; };
  const auto probed = governor.Capture(probe_context);
  Require(probed.ok, "engine cancellation-bound grant failed capture");
  cancel = true;
  auto replaced_probe = probe_context;
  replaced_probe.query_cancellation_requested = [] { return false; };
  Require(governor.Revalidate(replaced_probe, probed.handle, *probed.handle.carrier()).code == "PROCESS.CANCELLED",
          "replacement probe cleared retained cancellation authority");
  cancel = false;
  Require(governor.Revalidate(probe_context, probed.handle, *probed.handle.carrier()).code == "PROCESS.CANCELLED",
          "observed cancellation was not sticky");
  Require(!governor.Release(probe_context, probed.handle, Release::abandoned_before_publication).error,
          "cancelled unpublished grant did not release");
  cancel = true;
  Require(!governor.Capture(probe_context).ok && governor.Observe().active_grants == 0,
          "already-cancelled request created a resource grant");
  for (unsigned field = 0; field < 5; ++field) {
    auto refused = context;
    if (field == 0) refused.read_only_mode = true;
    if (field == 1) refused.cluster_transaction_active = true;
    if (field == 2) refused.route_fence_present = true;
    if (field == 3) refused.resource_admission_uuid = {};
    if (field == 4) refused.local_transaction_id += 999;
    Require(!governor.Capture(refused).ok, "unsupported/stale resource context admitted");
  }
  const auto retiring = governor.Capture(context);
  Require(retiring.ok, "final resource fixture failed capture");
  FinalizeTransaction(context, db::PhysicalMgaCowFinalizeDecision::rollback, 1788200004000ull);
  Require(governor.Revalidate(context, retiring.handle, *retiring.handle.carrier()).error &&
              !governor.Capture(context).ok,
          "resource governor admitted a finalized MGA transaction");
  Require(ReleaseWithAllocationDenied(governor, context, retiring.handle, Release::abandoned_before_publication) ==
              engine_api::EngineDmlUpdateResourceReleaseCodeV1::released &&
              governor.Observe().active_grants == 0 && governor.Observe().reserved_canonical_bytes == 0,
          "finalized transaction prevented resource-only cleanup");
}

void TestIndependentDeleteSecurityAuthority() {
  TemporaryDirectory work;
  const auto database = CreateDurableDatabase(work.path() / "delete-security.sbdb");
  auto seed = BeginTransaction(database, 1788205000000ull);
  for (const auto right : {"DELETE", "UPDATE"}) {
    engine_api::EngineSecurityGrantPrivilegeRequest grant;
    grant.context = SecurityContext(seed, 0x5010);
    grant.context.trace_tags.push_back("right:SEC_GRANT_ADMIN");
    grant.grant_uuid = CanonicalUuid(right == std::string_view("DELETE") ? 0x5020 : 0x5021);
    grant.grantee_uuid = database.principal_uuid;
    grant.target_object_uuid = std::string(kEmptyRelationUuid);
    grant.target_object_kind = "table";
    grant.privilege = right;
    const auto granted = engine_api::EngineSecurityGrantPrivilege(grant);
    Require(granted.ok && granted.privilege_granted, "DELETE fixture durable privilege grant failed");
  }
  {
    engine_api::EngineSecurityGrantPrivilegeRequest update_only;
    update_only.context = SecurityContext(seed, 0x5011);
    update_only.context.trace_tags.push_back("right:SEC_GRANT_ADMIN");
    update_only.grant_uuid = CanonicalUuid(0x5022);
    update_only.grantee_uuid = database.principal_uuid;
    update_only.target_object_uuid = std::string(kOtherRelationUuid);
    update_only.target_object_kind = "table";
    update_only.privilege = "UPDATE";
    Require(engine_api::EngineSecurityGrantPrivilege(update_only).ok,
            "DELETE fixture UPDATE-only target grant failed");
  }
  FinalizeTransaction(seed, db::PhysicalMgaCowFinalizeDecision::commit, 1788205001000ull);
  auto transaction = BeginTransaction(database, 1788205002000ull);
  const auto state = LoadSecurityState(transaction);
  auto context = ProviderContext(transaction, state, {}, 0x5030);
  context.session_uuid.canonical = CanonicalUuid(0x5031);
  context.trace_tags = {"private_dml_delete_rows_binder"};
  context.authorization_context.effective_subjects.push_back({context.principal_uuid, "principal"});
  for (const auto& source : state.grants) {
    if (source.revoked || source.grantee_uuid != database.principal_uuid ||
        source.target_object_uuid != kEmptyRelationUuid) continue;
    engine_api::EngineMaterializedAuthorizationGrant grant;
    grant.grant_uuid.canonical = source.grant_uuid;
    grant.subject_uuid.canonical = source.grantee_uuid;
    grant.subject_kind = source.grantee_kind;
    grant.target_uuid.canonical = source.target_object_uuid;
    grant.right = source.privilege;
    grant.security_epoch = context.security_epoch;
    grant.deny = source.grant_effect == "deny";
    context.authorization_context.grants.push_back(std::move(grant));
  }
  const auto capture = [&](const auto& owner) {
    return engine_api::CaptureDmlDeleteSecurityAuthorityV1(owner, std::string(kEmptyRelationUuid));
  };
  auto captured = capture(context);
  if (!captured.ok) std::cerr << captured.diagnostic.code << ':' << captured.diagnostic.detail << '\n';
  Require(captured.ok && captured.handle.valid() && !captured.matched_grant_uuids.empty() &&
              captured.snapshot.admitted_policy_rows.empty(), "independent DELETE privilege capture failed");
  auto consumer = context;
  consumer.trace_tags = {"private_dml_delete_rows_consumer"};
  Require(!engine_api::RevalidateDmlDeleteSecurityAuthorityV1(consumer, captured).error,
          "unchanged DELETE privilege did not revalidate");
  auto recovery_context = context;
  recovery_context.trace_tags = {"private_dml_delete_rows_recovery"};
  Require(!engine_api::RevalidateRecoveredDmlDeleteSecurityProjectionV1(
              recovery_context, captured.snapshot, captured.matched_grant_uuids).error,
          "actual recovered DELETE security source did not revalidate");
  Require(engine_api::RevalidateRecoveredDmlDeleteSecurityProjectionV1(
              consumer, captured.snapshot, captured.matched_grant_uuids).error,
          "consumer used recovery projection as a live security provider");
  auto wrong_projection = captured.snapshot;
  ++wrong_projection.policy_generation;
  Require(engine_api::RevalidateRecoveredDmlDeleteSecurityProjectionV1(
              recovery_context, wrong_projection, captured.matched_grant_uuids).error,
          "recovered security accepted changed policy catalog generation");
  Require(engine_api::RevalidateRecoveredDmlDeleteSecurityProjectionV1(
              recovery_context, captured.snapshot, {}).error,
          "recovered security accepted missing actual DELETE grants");
  auto changed = context;
  for (auto& grant : changed.authorization_context.grants)
    grant.target_uuid.canonical = std::string(kOtherRelationUuid);
  Require(!engine_api::CaptureDmlDeleteSecurityAuthorityV1(changed,
              std::string(kOtherRelationUuid)).ok,
          "materialized DELETE grant substituted for durable UPDATE-only privilege");
  changed = context;
  std::erase_if(changed.authorization_context.grants, [](const auto& grant) { return grant.right == "DELETE"; });
  Require(!capture(changed).ok, "UPDATE privilege substituted for DELETE");
  changed = context;
  for (auto& grant : changed.authorization_context.grants)
    if (grant.right == "DELETE") grant.deny = true;
  Require(!capture(changed).ok, "materialized DELETE deny was ignored");
  for (const auto tag : {"private_dml_update_rows_binder", "private_dml_update_rows_consumer",
                         "private_dml_delete_rows_consumer", "private_dml_delete_rows_recovery"}) {
    changed = context; changed.trace_tags = {tag};
    Require(!capture(changed).ok, "foreign operation or phase acquired DELETE security authority");
  }
  for (unsigned mutation = 0; mutation != 8; ++mutation) {
    changed = consumer;
    switch (mutation) {
      case 0: changed.database_path += ".other"; break;
      case 1: changed.session_uuid.canonical = CanonicalUuid(0x5090); break;
      case 2: changed.statement_receipt_uuid.canonical = CanonicalUuid(0x5090); break;
      case 3: ++changed.local_transaction_id; break;
      case 4: ++changed.authorization_context.security_context_generation; break;
      case 5: ++changed.authorization_context.policy_epoch; break;
      case 6: changed.authorization_context.grants.clear(); break;
      case 7: changed.read_only_mode = true; break;
    }
    Require(engine_api::RevalidateDmlDeleteSecurityAuthorityV1(changed, captured).error,
            "changed DELETE security owner or privilege reused handle");
  }
  auto forged = captured;
  forged.handle = {};
  Require(engine_api::RevalidateDmlDeleteSecurityAuthorityV1(consumer, forged).error,
          "snapshot bytes manufactured DELETE security handle");
  forged = captured; ++forged.snapshot.security_generation;
  Require(engine_api::RevalidateDmlDeleteSecurityAuthorityV1(consumer, forged).error,
          "modified DELETE snapshot projection was admitted");
  changed = context;
  engine_api::EngineMaterializedAuthorizationPolicy policy;
  policy.target_uuid.canonical = std::string(kEmptyRelationUuid);
  policy.right = "UPDATE";
  changed.authorization_context.policies.push_back(policy);
  Require(!capture(changed).ok, "UPDATE policy substituted for absent DELETE USING authority");
  FinalizeTransaction(transaction, db::PhysicalMgaCowFinalizeDecision::rollback, 1788205003000ull);
  auto policy_transaction = BeginTransaction(database, 1788205004000ull);
  PutPolicy(policy_transaction, 0x5012, kPolicyUsingA, kEmptyRelationUuid,
            NativePolicyAuthority(1, 1, kEffectiveUsing, kExpressionUsing));
  FinalizeTransaction(policy_transaction, db::PhysicalMgaCowFinalizeDecision::commit, 1788205005000ull);
  auto after_policy = BeginTransaction(database, 1788205006000ull);
  const auto policy_state = LoadSecurityState(after_policy);
  auto hidden_policy = ProviderContext(after_policy, policy_state, {}, 0x5032);
  hidden_policy.session_uuid = context.session_uuid;
  hidden_policy.trace_tags = {"private_dml_delete_rows_binder"};
  hidden_policy.authorization_context.effective_subjects = context.authorization_context.effective_subjects;
  hidden_policy.authorization_context.grants = context.authorization_context.grants;
  for (auto& grant : hidden_policy.authorization_context.grants) grant.security_epoch = hidden_policy.security_epoch;
  const auto hidden = capture(hidden_policy);
  Require(!hidden.ok && hidden.diagnostic.code == "SBLR.OPERATION_UNSUPPORTED",
          "omitting a durable UPDATE row policy manufactured empty DELETE USING authority");
  FinalizeTransaction(after_policy, db::PhysicalMgaCowFinalizeDecision::rollback, 1788205007000ull);
  std::cout << "independent_DELETE_security_authority=passed\n";
}

}  // namespace

int main() {
  TestIndependentDeleteSecurityAuthority();
  TemporaryDirectory temporary;
  const auto database_path = temporary.path() / "authority_provider.sdb";
  const auto database = CreateDurableDatabase(database_path);
  engine_api::ResetDmlUpdateImmutableAuthorityProviderForTestV1();

  auto policy_transaction = BeginTransaction(database, 1788200001000ull);
  PutPolicy(policy_transaction, 0x100a, kPolicyOther, kOtherRelationUuid,
            NativePolicyAuthority(4, 1, CanonicalUuid(0x6034),
                                  CanonicalUuid(0x7044)));
  const std::uint64_t using_a_generation =
      PutPolicy(policy_transaction, 0x100b, kPolicyUsingA, kRelationUuid,
                NativePolicyAuthority(1, 1, kEffectiveUsing,
                                      kExpressionUsing));
  const std::uint64_t using_b_generation =
      PutPolicy(policy_transaction, 0x100c, kPolicyUsingB, kRelationUuid,
                NativePolicyAuthority(2, 1, kEffectiveUsing,
                                      kExpressionUsing));
  const std::uint64_t check_generation =
      PutPolicy(policy_transaction, 0x100d, kPolicyCheck, kRelationUuid,
                NativePolicyAuthority(3, 2, kEffectiveCheck,
                                      kExpressionCheck));
  FinalizeTransaction(policy_transaction,
                      db::PhysicalMgaCowFinalizeDecision::commit,
                      1788200002000ull);

  auto provider_transaction = BeginTransaction(database, 1788200003000ull);
  const auto state = LoadSecurityState(provider_transaction);
  Require(FindPolicy(state, kPolicyUsingA).policy_generation ==
                  using_a_generation &&
              FindPolicy(state, kPolicyUsingB).policy_generation ==
                  using_b_generation &&
              FindPolicy(state, kPolicyCheck).policy_generation ==
                  check_generation,
          "committed policy generation identity drifted");
  const std::vector<engine_api::EngineMaterializedAuthorizationPolicy>
      policies = {
          MaterializedPolicy(FindPolicy(state, kPolicyUsingA),
                             database.principal_uuid,
                             state.policy_generation),
          MaterializedPolicy(FindPolicy(state, kPolicyUsingB),
                             database.principal_uuid,
                             state.policy_generation),
          MaterializedPolicy(FindPolicy(state, kPolicyCheck),
                             database.principal_uuid,
                             state.policy_generation),
  };
  const auto context =
      ProviderContext(provider_transaction, state, policies);
  const auto captured = CapturePolicyAuthority(
      context, kRelationUuid, kRelationOccurrenceUuid, 17, 0x9000);

  TestEmptyAuthoritySets(context, state);
  TestOrderingCollapseAndRevalidation(context, state, captured);
  TestRefusalCases(context, state, captured);

  engine_api::ResetEngineSecurityPolicySnapshotAuthorityForTestV1();
  const auto restarted_state = LoadSecurityState(provider_transaction);
  const auto restarted_context = ProviderContext(
      provider_transaction, restarted_state,
      {MaterializedPolicy(FindPolicy(restarted_state, kPolicyUsingA),
                          database.principal_uuid,
                          restarted_state.policy_generation),
       MaterializedPolicy(FindPolicy(restarted_state, kPolicyUsingB),
                          database.principal_uuid,
                          restarted_state.policy_generation),
       MaterializedPolicy(FindPolicy(restarted_state, kPolicyCheck),
                          database.principal_uuid,
                          restarted_state.policy_generation)},
      0x2010);
  const auto restarted_capture = CapturePolicyAuthority(
      restarted_context, kRelationUuid, kRelationOccurrenceUuid, 17,
      0x9010);
  Require(restarted_capture.source_policy_vector.records.size() == 3 &&
              restarted_capture.security_policy_snapshot
                      .security_context_generation ==
                  state.security_context_generation,
          "policy catalog restart did not recover exact native source rows");

  // Runtime configuration epochs and durable security-event generations are
  // independent domains. Match the authenticated context's own epoch fence,
  // while retaining the exact page-backed authorization successor generation.
  auto independent_epochs = restarted_context;
  independent_epochs.security_epoch = restarted_state.security_generation + 100;
  independent_epochs.authorization_context.security_epoch =
      independent_epochs.security_epoch;
  independent_epochs.authorization_context.policy_epoch =
      restarted_state.policy_generation + 200;
  for (auto& policy : independent_epochs.authorization_context.policies) {
    policy.policy_epoch = independent_epochs.authorization_context.policy_epoch;
  }
  const auto independent_snapshot =
      engine_api::IssueEngineSecurityPolicySnapshotAuthorityV1(
          independent_epochs, std::string(kRelationUuid));
  Require(independent_snapshot.ok &&
              independent_snapshot.snapshot.security_generation ==
                  restarted_state.security_generation &&
              independent_snapshot.snapshot.policy_generation ==
                  restarted_state.policy_generation &&
              independent_snapshot.snapshot.admitted_policy_rows.size() == 3,
          "runtime epochs were conflated with durable security event generations");
  Require(!engine_api::RevalidateEngineSecurityPolicySnapshotAuthorityV1(
              independent_epochs, independent_snapshot.snapshot).error,
          "unchanged runtime and durable snapshot identities did not revalidate");
  auto changed_epoch = independent_epochs;
  ++changed_epoch.security_epoch;
  ++changed_epoch.authorization_context.security_epoch;
  Require(engine_api::RevalidateEngineSecurityPolicySnapshotAuthorityV1(
              changed_epoch, independent_snapshot.snapshot).error,
          "changed runtime security epoch reused an issued snapshot");
  changed_epoch = independent_epochs;
  ++changed_epoch.authorization_context.policy_epoch;
  for (auto& policy : changed_epoch.authorization_context.policies) {
    policy.policy_epoch = changed_epoch.authorization_context.policy_epoch;
  }
  Require(engine_api::RevalidateEngineSecurityPolicySnapshotAuthorityV1(
              changed_epoch, independent_snapshot.snapshot).error,
          "changed runtime policy epoch reused an issued snapshot");
  auto inconsistent_epochs = independent_epochs;
  ++inconsistent_epochs.authorization_context.security_epoch;
  Require(!engine_api::IssueEngineSecurityPolicySnapshotAuthorityV1(
              inconsistent_epochs, std::string(kRelationUuid)).ok,
          "mismatched runtime context epoch was admitted");
  inconsistent_epochs = independent_epochs;
  ++inconsistent_epochs.authorization_context.policies.front().policy_epoch;
  Require(!engine_api::IssueEngineSecurityPolicySnapshotAuthorityV1(
              inconsistent_epochs, std::string(kRelationUuid)).ok,
          "mismatched materialized policy epoch was admitted");

  auto stale_context = independent_epochs;
  --stale_context.authorization_context.security_context_generation;
  engine_api::EngineDmlUpdatePolicyCatalogCaptureRequestV1 stale_request;
  stale_request.context = stale_context;
  stale_request.authenticated_statement_receipt_uuid =
      stale_context.statement_receipt_uuid.canonical;
  stale_request.structural_occurrence_id = 17;
  stale_request.relation_occurrence.relation_uuid =
      std::string(kRelationUuid);
  stale_request.relation_occurrence.relation_generation = 7;
  stale_request.relation_occurrence.relation_occurrence_uuid =
      std::string(kRelationOccurrenceUuid);
  stale_request.relation_occurrence.relation_occurrence_generation = 1;
  stale_request.catalog_snapshot_uuid =
      stale_context.statement_metadata_snapshot_uuid.canonical;
  stale_request.catalog_generation = stale_context.catalog_generation_id;
  stale_request.descriptor_uuid = CanonicalUuid(0x9011);
  stale_request.descriptor_generation = 1;
  Require(!engine_api::CaptureDmlUpdatePolicyCatalogAuthorityV1(stale_request)
               .ok,
          "stale security-context generation was admitted after restart");

  TestResourceGovernorLifecycle(context);

  engine_api::ResetDmlUpdateImmutableAuthorityProviderForTestV1();
  std::cout << "sbsql_dml_update_authority_provider_conformance: PASS\n";
  return EXIT_SUCCESS;
}
