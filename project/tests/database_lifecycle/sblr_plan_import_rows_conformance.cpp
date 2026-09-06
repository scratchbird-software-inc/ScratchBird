// Copyright (c) 2026 ScratchBird Software Inc.
//
// This Source Code Form is subject to the terms of the Mozilla Public
// License, v. 2.0. If a copy of the MPL was not distributed with this
// file, You can obtain one at https://mozilla.org/MPL/2.0/.
//
// SPDX-License-Identifier: MPL-2.0

// SBLR-DML-PLAN-IMPORT-ROWS-ZERO-GREY-V1

#include "database_lifecycle.hpp"
#include "dml/import_api.hpp"
#include "mga_relation_store/mga_relation_store.hpp"
#include "sblr_dispatch.hpp"
#include "sblr_engine_envelope.hpp"
#include "sblr_executor_availability_registry.hpp"
#include "storage/database/local_transaction_store.hpp"
#include "transaction/transaction_api.hpp"
#include "transaction_inventory.hpp"
#include "uuid.hpp"

#include <algorithm>
#include <array>
#include <chrono>
#include <cstdint>
#include <cstdlib>
#include <filesystem>
#include <fstream>
#include <functional>
#include <iostream>
#include <iterator>
#include <map>
#include <string>
#include <string_view>
#include <utility>
#include <vector>

namespace {

namespace api = scratchbird::engine::internal_api;
namespace codec = scratchbird::engine::sblr;
namespace db = scratchbird::storage::database;
namespace mga = scratchbird::transaction::mga;
namespace platform = scratchbird::core::platform;
namespace uuid = scratchbird::core::uuid;

constexpr std::string_view kOperationId = "dml.plan_import_rows";
constexpr std::string_view kBinderTag =
    "private_dml_plan_import_rows_binder";
constexpr std::string_view kConsumerTag =
    "private_dml_plan_import_rows_consumer";

[[noreturn]] void Fail(std::string_view message) {
  std::cerr << message << '\n';
  std::exit(EXIT_FAILURE);
}

void Require(bool condition, std::string_view message) {
  if (!condition) Fail(message);
}

template <typename TResult>
void RequireOk(const TResult& result, std::string_view message) {
  if (!result.ok) {
    if (!result.diagnostics.empty()) {
      std::cerr << result.diagnostics.front().code << ':'
                << result.diagnostics.front().detail << '\n';
    }
    Fail(message);
  }
}

std::uint64_t NowMillis() {
  return static_cast<std::uint64_t>(
      std::chrono::duration_cast<std::chrono::milliseconds>(
          std::chrono::system_clock::now().time_since_epoch())
          .count());
}

std::uint64_t NextUuidMillis() {
  static std::uint64_t sequence = 0;
  return 1900000000000ull + sequence++;
}

platform::TypedUuid NewUuid(platform::UuidKind kind) {
  const auto millis = NextUuidMillis();
  if (uuid::UuidKindAllowsDurableIdentity(kind)) {
    const auto issued = uuid::GenerateEngineIdentityV7(kind, millis);
    Require(issued.ok(), "plan-import test durable UUID issuance failed");
    return issued.value;
  }
  const auto compatible = uuid::GenerateCompatibilityUnixTimeV7(millis);
  Require(compatible.ok(), "plan-import test compatibility UUID issuance failed");
  const auto issued = uuid::MakeTypedUuid(kind, compatible.value);
  Require(issued.ok(), "plan-import test typed UUID issuance failed");
  return issued.value;
}

std::string NewUuidText(platform::UuidKind kind) {
  return uuid::UuidToString(NewUuid(kind).value);
}

codec::PlanImportRowsUuidV1 UuidBytes(std::string_view text) {
  const auto parsed = uuid::ParseUuid(std::string(text));
  Require(parsed.ok() && !uuid::IsNilUuid(parsed.value),
          "plan-import test UUID was not canonical");
  return parsed.value.bytes;
}

std::string UuidText(const codec::PlanImportRowsUuidV1& bytes) {
  platform::Uuid value;
  value.bytes = bytes;
  return uuid::UuidToString(value);
}

template <std::size_t N>
bool Nonzero(const std::array<std::uint8_t, N>& bytes) {
  return std::any_of(bytes.begin(), bytes.end(),
                     [](std::uint8_t byte) { return byte != 0; });
}

using DirectoryImage =
    std::map<std::string, std::vector<std::uint8_t>>;

DirectoryImage ReadDirectoryImage(const std::filesystem::path& root) {
  DirectoryImage image;
  for (const auto& entry : std::filesystem::recursive_directory_iterator(root)) {
    if (!entry.is_regular_file()) continue;
    std::ifstream input(entry.path(), std::ios::binary);
    Require(static_cast<bool>(input), "plan-import durable file open failed");
    std::vector<std::uint8_t> bytes{
        std::istreambuf_iterator<char>(input),
        std::istreambuf_iterator<char>()};
    Require(!input.bad(), "plan-import durable file read failed");
    image.emplace(
        std::filesystem::relative(entry.path(), root).generic_string(),
        std::move(bytes));
  }
  return image;
}

struct Fixture {
  std::filesystem::path root;
  std::filesystem::path database_path;
  std::string database_uuid;
  std::string filespace_uuid;
  std::string schema_uuid;
  std::string principal_uuid;
  std::string session_uuid;
  std::string table_uuid;
  std::uint64_t ended_local_transaction_id = 0;
  api::EngineUuid ended_transaction_uuid;
  api::EngineRequestContext planning_context;
  api::MgaRelationStorageDescriptor relation_descriptor;

  ~Fixture() {
    std::error_code ignored;
    if (!root.empty()) std::filesystem::remove_all(root, ignored);
  }
};

api::EngineRequestContext BaseContext(const Fixture& fixture,
                                      std::string request_id) {
  api::EngineRequestContext context;
  context.trust_mode = api::EngineTrustMode::server_isolated;
  context.request_id = std::move(request_id);
  context.database_path = fixture.database_path.string();
  context.database_uuid.canonical = fixture.database_uuid;
  context.default_root_uuid.canonical = fixture.filespace_uuid;
  context.current_schema_uuid.canonical = fixture.schema_uuid;
  context.principal_uuid.canonical = fixture.principal_uuid;
  context.session_uuid.canonical = fixture.session_uuid;
  context.security_context_present = true;
  context.catalog_generation_id = 1;
  context.security_epoch = 1;
  context.resource_epoch = 1;
  context.name_resolution_epoch = 1;
  context.identifier_profile_uuid = "sbsql_v3";
  context.language_context.language_tag = "en";
  context.language_context.default_language_tag = "en";
  return context;
}

api::EngineRequestContext Begin(const Fixture& fixture,
                                std::string request_id) {
  api::EngineBeginTransactionRequest request;
  request.context = BaseContext(fixture, std::move(request_id));
  request.isolation_level = "repeatable_read";
  const auto begun = api::EngineBeginTransaction(request);
  RequireOk(begun, "plan-import transaction begin failed");
  auto context = request.context;
  context.local_transaction_id = begun.local_transaction_id;
  context.transaction_uuid = begun.transaction_uuid;
  context.snapshot_visible_through_local_transaction_id =
      begun.snapshot_visible_through_local_transaction_id;
  context.transaction_isolation_level = begun.isolation_level;
  return context;
}

void Commit(const api::EngineRequestContext& context) {
  api::EngineCommitTransactionRequest request;
  request.context = context;
  RequireOk(api::EngineCommitTransaction(request),
            "plan-import transaction commit failed");
}

void Rollback(const api::EngineRequestContext& context) {
  api::EngineRollbackTransactionRequest request;
  request.context = context;
  RequireOk(api::EngineRollbackTransaction(request),
            "plan-import transaction rollback failed");
}

api::EngineRequestContext AttachStatementReceipt(
    const Fixture& fixture,
    api::EngineRequestContext context) {
  context.statement_uuid.canonical =
      NewUuidText(platform::UuidKind::object);
  context.statement_snapshot_uuid.canonical.clear();
  api::EnginePublishStatementSnapshotRequest publish;
  publish.context = context;
  const auto snapshot = api::EnginePublishStatementSnapshot(publish);
  RequireOk(snapshot, "plan-import statement snapshot publication failed");
  context.statement_snapshot_uuid = snapshot.statement_snapshot_uuid;
  context.statement_snapshot_generation =
      snapshot.snapshot_vector
          .publication_inventory_next_local_transaction_id;
  context.snapshot_visible_through_local_transaction_id =
      snapshot.snapshot_vector.visible_committed_high_watermark;
  context.statement_receipt_uuid.canonical =
      NewUuidText(platform::UuidKind::object);
  context.statement_metadata_snapshot_uuid.canonical =
      NewUuidText(platform::UuidKind::object);
  context.statement_metadata_snapshot_engine_owned = true;
  context.statement_metadata_snapshot_visible_through_local_transaction_id =
      snapshot.snapshot_vector.visible_committed_high_watermark;
  context.statement_metadata_snapshot_active_excluded_local_transaction_ids =
      snapshot.snapshot_vector.active_excluded_local_transaction_ids;
  context.statement_metadata_snapshot_in_doubt_excluded_local_transaction_ids =
      snapshot.snapshot_vector.in_doubt_excluded_local_transaction_ids;
  context.transaction_policy_snapshot_uuid.canonical =
      NewUuidText(platform::UuidKind::object);
  context.transaction_policy_snapshot_generation = 1;
  context.resource_admission_uuid.canonical =
      NewUuidText(platform::UuidKind::object);

  auto& authorization = context.authorization_context;
  authorization.present = true;
  authorization.authority_uuid.canonical =
      NewUuidText(platform::UuidKind::object);
  authorization.security_context_generation = 1;
  authorization.principal_uuid = context.principal_uuid;
  authorization.security_epoch = context.security_epoch;
  authorization.policy_epoch = 1;
  authorization.catalog_generation_id = context.catalog_generation_id;
  api::EngineAuthorizationSubject subject;
  subject.subject_uuid = context.principal_uuid;
  subject.subject_kind = "principal";
  authorization.effective_subjects.push_back(subject);
  api::EngineMaterializedAuthorizationGrant grant;
  grant.grant_uuid.canonical =
      NewUuidText(platform::UuidKind::object);
  grant.subject_uuid = context.principal_uuid;
  grant.subject_kind = "principal";
  grant.target_uuid.canonical = fixture.table_uuid;
  grant.right = "INSERT";
  grant.security_epoch = context.security_epoch;
  authorization.grants.push_back(std::move(grant));
  return context;
}

api::SblrExecutorAvailabilityRowIdentity PlanImportAvailabilityIdentity() {
  api::SblrExecutorAvailabilityRowIdentity identity;
  identity.executor_id = api::kSblrDmlPlanImportRowsExecutorId;
  identity.opcode_code = api::kSblrDmlPlanImportRowsOpcodeCode;
  identity.opcode_version = api::kSblrDmlPlanImportRowsOpcodeVersion;
  identity.operand_descriptor_id =
      api::kSblrDmlPlanImportRowsOperandDescriptorId;
  identity.result_descriptor_id =
      api::kSblrDmlPlanImportRowsResultDescriptorId;
  identity.result_descriptor_version =
      api::kSblrDmlPlanImportRowsResultDescriptorVersion;
  return identity;
}

Fixture MakeFixture() {
  Fixture fixture;
  fixture.root = std::filesystem::temp_directory_path() /
                 ("scratchbird_plan_import_rows_" +
                  std::to_string(NowMillis()));
  std::filesystem::create_directories(fixture.root);
  fixture.database_path = fixture.root / "plan_import_rows.sbdb";

  db::DatabaseCreateConfig create;
  create.path = fixture.database_path.string();
  create.database_uuid = NewUuid(platform::UuidKind::database);
  create.filespace_uuid = NewUuid(platform::UuidKind::filespace);
  create.creation_unix_epoch_millis = NowMillis();
  create.require_resource_seed_pack = false;
  create.allow_minimal_resource_bootstrap = true;
  create.allow_overwrite = true;
  const auto created = db::CreateDatabaseFile(create);
  if (!created.ok()) {
    std::cerr << created.diagnostic.diagnostic_code << ':'
              << created.diagnostic.message_key << '\n';
  }
  Require(created.ok(), "plan-import database creation failed");

  fixture.database_uuid = uuid::UuidToString(create.database_uuid.value);
  fixture.filespace_uuid = uuid::UuidToString(create.filespace_uuid.value);
  fixture.schema_uuid = NewUuidText(platform::UuidKind::schema);
  fixture.principal_uuid = NewUuidText(platform::UuidKind::principal);
  fixture.session_uuid = NewUuidText(platform::UuidKind::session);
  fixture.table_uuid = NewUuidText(platform::UuidKind::object);

  auto metadata = Begin(fixture, "plan-import-metadata");
  api::CrudTableRecord table;
  table.creator_tx = metadata.local_transaction_id;
  table.table_uuid = fixture.table_uuid;
  table.default_name = "plan_import_target";
  table.columns.push_back({"payload", "canonical=character"});
  Require(!api::AppendMgaTableMetadata(metadata, table).error,
          "plan-import table metadata append failed");
  Require(!api::EnsureMgaRelationStorageDescriptor(
               metadata, table, {}, &fixture.relation_descriptor)
               .error,
          "plan-import relation descriptor persistence failed");
  Commit(metadata);

  auto ended = Begin(fixture, "plan-import-ended-transaction");
  fixture.ended_local_transaction_id = ended.local_transaction_id;
  fixture.ended_transaction_uuid = ended.transaction_uuid;
  Commit(ended);

  fixture.planning_context = AttachStatementReceipt(
      fixture, Begin(fixture, "plan-import-active-transaction"));
  const auto bootstrapped = api::LoadSblrExecutorAvailabilitySnapshot(
      fixture.planning_context, PlanImportAvailabilityIdentity());
  Require(bootstrapped.ok && bootstrapped.snapshot.installed &&
              bootstrapped.snapshot.generation != 0,
          "plan-import executor availability setup failed");
  const auto current = api::LoadCurrentSblrExecutorAvailabilitySnapshot(
      fixture.planning_context, PlanImportAvailabilityIdentity());
  Require(current.ok &&
              current.snapshot.snapshot_uuid ==
                  bootstrapped.snapshot.snapshot_uuid &&
              current.snapshot.generation == bootstrapped.snapshot.generation,
          "plan-import read-only availability lookup drifted");
  return fixture;
}

api::EngineRequestContext WithOnlyTraceTag(
    api::EngineRequestContext context,
    std::string_view tag) {
  context.trace_tags.clear();
  context.trace_tags.emplace_back(tag);
  return context;
}

api::EngineCreateImportRowsPlanDescriptorRequestV1 FactoryRequest(
    const Fixture& fixture,
    const api::EngineRequestContext& binder_context) {
  api::EngineCreateImportRowsPlanDescriptorRequestV1 request;
  request.context = binder_context;
  request.structural_occurrence_id = 1;
  request.target_table_uuid.canonical = fixture.table_uuid;
  request.source_kind = codec::PlanImportRowsSourceKindV1::csv_stream;
  request.source_fingerprint_present = false;
  request.format_family = codec::PlanImportRowsFormatFamilyV1::csv;
  request.reject_mode = codec::PlanImportRowsRejectModeV1::fail_fast;
  request.reject_payload_policy =
      codec::PlanImportRowsRejectPayloadPolicyV1::diagnostic_only;
  request.resume_policy = codec::PlanImportRowsResumePolicyV1::fail_closed;
  return request;
}

api::EnginePlanImportRowsRequest PlanRequest(
    const api::EngineRequestContext& consumer_context,
    const codec::PlanImportRowsDescriptorRefV1& descriptor_ref) {
  api::EnginePlanImportRowsRequest request;
  request.context = consumer_context;
  request.operation_id = std::string(kOperationId);
  request.descriptor_ref = descriptor_ref;
  return request;
}

std::function<bool()> CountingCancellationProbe(std::size_t* count,
                                                bool requested) {
  return [count, requested] {
    ++*count;
    return requested;
  };
}

std::function<bool()> CancellationAtProbe(std::size_t* count,
                                          std::size_t requested_probe) {
  return [count, requested_probe] {
    ++*count;
    return *count == requested_probe;
  };
}

void RequireRefusal(const api::EnginePlanImportRowsResult& result,
                    std::string_view code,
                    std::string_view message) {
  if (result.ok || result.diagnostics.size() != 1 ||
      result.diagnostics.front().code != code) {
    if (!result.diagnostics.empty()) {
      std::cerr << "expected=" << code << ";actual="
                << result.diagnostics.front().code << ':'
                << result.diagnostics.front().detail << '\n';
    }
    Fail(message);
  }
  Require(!result.surface_accepted && !result.planning_only &&
              !result.execution_requires_execute_import_rows &&
              !result.row_execution_completed &&
              !result.row_persistence_claimed &&
              result.normalized_insert_mode_code == 0 &&
              result.normalized_source_kind_code == 0 &&
              result.normalized_format_family_code == 0 &&
              result.mapped_column_count == 0 &&
              result.validated_request_descriptor_uuid.canonical.empty() &&
              result.validated_request_descriptor_generation == 0 &&
              !Nonzero(result.validated_request_projection_sha256) &&
              result.accepted_executor_evidence.exact_bytes.empty() &&
              result.evidence.empty(),
          "plan-import refusal published success extensions or evidence");
}

void RequireSuccessContract(
    const api::EnginePlanImportRowsResult& result,
    const api::EngineRequestContext& context,
    const codec::PlanImportRowsDescriptorRefV1& descriptor_ref) {
  Require(result.ok && result.diagnostics.empty(),
          "plan-import planning result was not successful");
  Require(result.surface_accepted && result.planning_only &&
              result.execution_requires_execute_import_rows &&
              !result.row_execution_completed &&
              !result.row_persistence_claimed,
          "plan-import five boolean result fields drifted");
  Require(result.normalized_insert_mode_code ==
              static_cast<std::uint16_t>(
                  codec::PlanImportRowsInsertModeV1::copy_import) &&
              result.normalized_source_kind_code ==
                  static_cast<std::uint16_t>(
                      codec::PlanImportRowsSourceKindV1::csv_stream) &&
              result.normalized_format_family_code ==
                  static_cast<std::uint16_t>(
                      codec::PlanImportRowsFormatFamilyV1::csv) &&
              result.normalized_insert_mode == "copy_import" &&
              result.normalized_source_kind == "csv_stream" &&
              result.normalized_format_family == "csv",
          "plan-import normalized enum result fields drifted");
  Require(result.mapped_column_count == 0 &&
              result.validated_request_descriptor_uuid.canonical ==
                  UuidText(descriptor_ref.descriptor_uuid) &&
              result.validated_request_descriptor_generation ==
                  descriptor_ref.descriptor_generation &&
              Nonzero(result.validated_request_projection_sha256),
          "plan-import descriptor result fields drifted");

  const auto& evidence = result.accepted_executor_evidence;
  Require(Nonzero(evidence.evidence_uuid) &&
              evidence.evidence_generation != 0 &&
              evidence.request_descriptor_uuid == descriptor_ref.descriptor_uuid &&
              evidence.request_descriptor_generation ==
                  descriptor_ref.descriptor_generation &&
              evidence.request_projection_sha256 ==
                  result.validated_request_projection_sha256 &&
              evidence.executor_availability_generation != 0 &&
              evidence.transaction_uuid ==
                  UuidBytes(context.transaction_uuid.canonical) &&
              evidence.local_transaction_id == context.local_transaction_id &&
              evidence.mga_snapshot_uuid ==
                  UuidBytes(context.statement_snapshot_uuid.canonical) &&
              evidence.mga_snapshot_generation ==
                  context.statement_snapshot_generation &&
              evidence.completed_validation_bits ==
                  codec::kPlanImportRowsAcceptedValidationBitsV1 &&
              Nonzero(evidence.evidence_sha256) &&
              evidence.exact_bytes.size() ==
                  codec::kPlanImportRowsExecutorEvidenceBytesV1,
          "plan-import accepted IPEV binding drifted");
  codec::PlanImportRowsCodecDiagnosticV1 diagnostic;
  std::vector<std::uint8_t> encoded;
  Require(codec::EncodePlanImportRowsExecutorEvidenceV1(
              evidence, &encoded, &diagnostic) &&
              encoded == evidence.exact_bytes,
          "plan-import accepted IPEV was not canonical byte identity");
}

codec::SblrOperationEnvelope PlanEnvelope(
    const codec::PlanImportRowsDescriptorRefV1& descriptor_ref) {
  auto envelope = codec::MakeSblrEnvelope(
      std::string(kOperationId), "SBLR_DML_PLAN_IMPORT_ROWS",
      "SBLR-PLAN-IMPORT-ROWS-CONFORMANCE");
  envelope.opcode_code = codec::kPlanImportRowsOpcodeCodeV1;
  envelope.operation_version_major = 1;
  envelope.operation_version_minor = 0;
  envelope.result_shape = "import_plan_result";
  envelope.diagnostic_shape = "diagnostic_vector";
  envelope.parser_package_uuid =
      NewUuidText(platform::UuidKind::object);
  envelope.registry_snapshot_uuid =
      NewUuidText(platform::UuidKind::object);
  envelope.requires_security_context = true;
  envelope.requires_transaction_context = true;
  envelope.contains_sql_text = false;
  envelope.parser_resolved_names_to_uuids = true;
  codec::PlanImportRowsCodecDiagnosticV1 diagnostic;
  std::vector<std::uint8_t> encoded_ref;
  Require(codec::EncodePlanImportRowsDescriptorRefV1(
              descriptor_ref, &encoded_ref, &diagnostic),
          "plan-import dispatch descriptor reference encoding failed");
  codec::SblrOperand operand;
  operand.type = "import_rows_plan_descriptor";
  operand.name = "request";
  operand.ordinal = 1;
  operand.value_kind = codec::SblrValueKind::descriptor_ref;
  operand.value_body = std::move(encoded_ref);
  envelope.operands.push_back(std::move(operand));
  return envelope;
}

void RequireInventoryEqual(
    const mga::LocalTransactionInventory& before,
    const mga::LocalTransactionInventory& after) {
  Require(before.next_local_transaction_id == after.next_local_transaction_id &&
              before.entries.size() == after.entries.size(),
          "plan-import changed the transaction inventory shape");
  for (std::size_t index = 0; index < before.entries.size(); ++index) {
    const auto& left = before.entries[index];
    const auto& right = after.entries[index];
    Require(left.identity.local_id.value == right.identity.local_id.value &&
                left.identity.transaction_uuid.kind ==
                    right.identity.transaction_uuid.kind &&
                left.identity.transaction_uuid.value ==
                    right.identity.transaction_uuid.value &&
                left.identity.scope == right.identity.scope &&
                left.state == right.state &&
                left.begin_unix_epoch_millis ==
                    right.begin_unix_epoch_millis &&
                left.final_unix_epoch_millis ==
                    right.final_unix_epoch_millis &&
                left.begin_visible_through_local_transaction_id ==
                    right.begin_visible_through_local_transaction_id &&
                left.evidence_record_required ==
                    right.evidence_record_required &&
                left.evidence_record_written ==
                    right.evidence_record_written &&
                left.rollback_only == right.rollback_only,
            "plan-import changed a transaction inventory row");
  }
}

void RequireRelationStateEqual(const api::MgaRelationStoreState& before,
                               const api::MgaRelationStoreState& after) {
  const auto& left = before.relation_metadata;
  const auto& right = after.relation_metadata;
  Require(before.row_versions.size() == after.row_versions.size() &&
              before.index_entries.size() == after.index_entries.size() &&
              before.max_row_event_sequence == after.max_row_event_sequence &&
              before.max_index_event_sequence ==
                  after.max_index_event_sequence &&
              left.transactions == right.transactions &&
              left.tables.size() == right.tables.size() &&
              left.indexes.size() == right.indexes.size() &&
              left.large_values.size() == right.large_values.size() &&
              left.sealed_relation_descriptor_snapshots.size() ==
                  right.sealed_relation_descriptor_snapshots.size() &&
              left.max_transaction_id == right.max_transaction_id &&
              left.max_sequence == right.max_sequence &&
              left.max_index_sequence == right.max_index_sequence &&
              left.max_event_sequence == right.max_event_sequence &&
              left.savepoints == right.savepoints,
          "plan-import changed durable relation/catalog state");
}

}  // namespace

int main() {
  auto fixture = MakeFixture();
  const auto binder_context =
      WithOnlyTraceTag(fixture.planning_context, kBinderTag);
  const auto consumer_context =
      WithOnlyTraceTag(fixture.planning_context, kConsumerTag);

  const auto descriptor_before = api::LoadMgaRelationStorageDescriptor(
      consumer_context, fixture.table_uuid);
  Require(descriptor_before.ok,
          "plan-import current relation descriptor baseline load failed");

  const auto availability_before_evidence =
      api::LoadCurrentSblrExecutorAvailabilitySnapshot(
          fixture.planning_context, PlanImportAvailabilityIdentity());
  Require(availability_before_evidence.ok &&
              availability_before_evidence.snapshot.installed,
          "plan-import evidence-precedence availability load failed");
  const auto evidence_bound =
      api::CreateAndPublishEngineBoundImportRowsPlanDescriptorV1(
          FactoryRequest(fixture, binder_context));
  Require(evidence_bound.ok,
          "plan-import evidence-precedence descriptor bind failed");

  auto executor_admin = fixture.planning_context;
  executor_admin.trace_tags = {"right:SBLR_EXECUTOR_AVAILABILITY_ADMIN"};
  api::SblrExecutorAvailabilitySetRequest revoke;
  revoke.database_uuid = fixture.database_uuid;
  revoke.expected_snapshot_uuid =
      availability_before_evidence.snapshot.snapshot_uuid;
  revoke.expected_generation =
      availability_before_evidence.snapshot.generation;
  revoke.exact_row_identity = PlanImportAvailabilityIdentity();
  revoke.requested_state = api::SblrExecutorAvailabilityState::revoked;
  revoke.reason_code = "test.plan_import.evidence_missing";
  const auto revoked = api::SetSblrExecutorAvailability(executor_admin, revoke);
  Require(revoked.ok && !revoked.snapshot.installed,
          "plan-import executor revocation failed");

  std::size_t evidence_missing_probes = 0;
  auto evidence_missing =
      PlanRequest(consumer_context, evidence_bound.descriptor_ref);
  evidence_missing.context.query_cancellation_requested =
      CountingCancellationProbe(&evidence_missing_probes, false);
  RequireRefusal(
      api::EnginePlanImportRows(evidence_missing),
      "SBLR.OPCODE.EXECUTOR_EVIDENCE_MISSING",
      "plan-import missing executor evidence precedence drifted");
  Require(evidence_missing_probes == 5,
          "plan-import evidence refusal cancellation probe count drifted");
  const auto evidence_release =
      api::ReleaseEngineBoundImportRowsPlanDescriptorsV1(binder_context);
  Require(evidence_release.ok && evidence_release.released_row_count == 1,
          "plan-import evidence descriptor release failed");

  api::SblrExecutorAvailabilitySetRequest reinstall;
  reinstall.database_uuid = fixture.database_uuid;
  reinstall.expected_snapshot_uuid = revoked.snapshot.snapshot_uuid;
  reinstall.expected_generation = revoked.snapshot.generation;
  reinstall.exact_row_identity = PlanImportAvailabilityIdentity();
  reinstall.requested_state = api::SblrExecutorAvailabilityState::installed;
  reinstall.reason_code = "test.plan_import.evidence_restore";
  const auto reinstalled =
      api::SetSblrExecutorAvailability(executor_admin, reinstall);
  Require(reinstalled.ok && reinstalled.snapshot.installed,
          "plan-import executor reinstall failed");

  auto unsupported_profile = FactoryRequest(fixture, binder_context);
  unsupported_profile.format_family =
      codec::PlanImportRowsFormatFamilyV1::jsonl;
  const auto unsupported_bound =
      api::CreateAndPublishEngineBoundImportRowsPlanDescriptorV1(
          unsupported_profile);
  Require(unsupported_bound.ok,
          "recognized unsupported plan-import profile did not bind");
  std::size_t unsupported_probes = 0;
  auto unsupported_request =
      PlanRequest(consumer_context, unsupported_bound.descriptor_ref);
  unsupported_request.context.query_cancellation_requested =
      CountingCancellationProbe(&unsupported_probes, false);
  RequireRefusal(api::EnginePlanImportRows(unsupported_request),
                 "SBLR.OPERATION_UNSUPPORTED",
                 "recognized plan-import profile was not refused last");
  Require(unsupported_probes == 5,
          "plan-import unsupported refusal cancellation probe count drifted");
  const auto unsupported_release =
      api::ReleaseEngineBoundImportRowsPlanDescriptorsV1(binder_context);
  Require(unsupported_release.ok &&
              unsupported_release.released_row_count == 1,
          "unsupported plan-import profile release failed");

  auto unsupported_mapping = FactoryRequest(fixture, binder_context);
  api::EngineImportRowsColumnMappingDemandV1 mapping;
  mapping.source_field_ordinal = 0;
  mapping.required = true;
  mapping.target_column_uuid =
      descriptor_before.descriptor.columns.front().column_uuid;
  unsupported_mapping.mappings.push_back(mapping);
  const auto unsupported_result =
      api::CreateAndPublishEngineBoundImportRowsPlanDescriptorV1(
          unsupported_mapping);
  Require(!unsupported_result.ok &&
              unsupported_result.diagnostic.code ==
                  "SBLR.OPERATION_UNSUPPORTED" &&
              !Nonzero(unsupported_result.descriptor_ref.descriptor_uuid) &&
              unsupported_result.descriptor_ref.descriptor_generation == 0,
          "plan-import nonzero production IMAP did not fail closed");
  const auto empty_release =
      api::ReleaseEngineBoundImportRowsPlanDescriptorsV1(binder_context);
  Require(empty_release.ok && empty_release.released_row_count == 0,
          "unsupported plan-import demand published a registry row");

  const auto state_before = api::LoadMgaRelationStoreState(consumer_context);
  Require(state_before.ok && state_before.state.row_versions.empty(),
          "plan-import precondition did not have an empty target row store");
  const auto inventory_before =
      db::LoadLocalTransactionInventoryFromDatabase(
          fixture.database_path.string());
  Require(inventory_before.ok(),
          "plan-import transaction inventory baseline load failed");
  const auto durable_before = ReadDirectoryImage(fixture.root);

  const auto bound = api::CreateAndPublishEngineBoundImportRowsPlanDescriptorV1(
      FactoryRequest(fixture, binder_context));
  if (!bound.ok) {
    std::cerr << bound.diagnostic.code << ':' << bound.diagnostic.detail << '\n';
  }
  Require(bound.ok && Nonzero(bound.descriptor_ref.descriptor_uuid) &&
              bound.descriptor_ref.descriptor_generation != 0,
          "zero-IMAP plan-import binder factory failed");

  const auto request = PlanRequest(consumer_context, bound.descriptor_ref);
  const auto first = api::EnginePlanImportRows(request);
  RequireSuccessContract(first, consumer_context, bound.descriptor_ref);
  const auto repeated = api::EnginePlanImportRows(request);
  RequireSuccessContract(repeated, consumer_context, bound.descriptor_ref);
  Require(first.accepted_executor_evidence.exact_bytes ==
              repeated.accepted_executor_evidence.exact_bytes,
          "repeated plan-import live revalidation changed accepted IPEV");

  std::size_t successful_probes = 0;
  auto observed_success = request;
  observed_success.context.query_cancellation_requested =
      CountingCancellationProbe(&successful_probes, false);
  RequireSuccessContract(api::EnginePlanImportRows(observed_success),
                         consumer_context, bound.descriptor_ref);
  Require(successful_probes == 6,
          "plan-import did not observe all six cancellation checkpoints");

  codec::SblrDispatchRequest dispatch;
  dispatch.context = consumer_context;
  dispatch.envelope = PlanEnvelope(bound.descriptor_ref);
  dispatch.standalone_package_root = true;
  const auto dispatched = codec::DispatchSblrOperation(std::move(dispatch));
  Require(dispatched.envelope_validated && dispatched.accepted &&
              dispatched.dispatched_to_api && dispatched.api_result.ok &&
              dispatched.plan_import_rows_result.has_value() &&
              dispatched.canonical_result_bytes.empty(),
          "standalone plan-import dispatch did not preserve typed result");
  RequireSuccessContract(*dispatched.plan_import_rows_result,
                         consumer_context, bound.descriptor_ref);

  codec::SblrDispatchRequest inline_dispatch;
  inline_dispatch.context = consumer_context;
  inline_dispatch.envelope = PlanEnvelope(bound.descriptor_ref);
  const auto inline_refused =
      codec::DispatchSblrOperation(std::move(inline_dispatch));
  Require(inline_refused.envelope_validated &&
              inline_refused.plan_import_rows_result.has_value(),
          "inline plan-import dispatch did not reach typed refusal");
  RequireRefusal(*inline_refused.plan_import_rows_result,
                 "SBLR.OPERAND_INVALID",
                 "inline plan-import dispatch was not refused");

  const auto require_higher_precedence = [](
      api::EnginePlanImportRowsRequest candidate,
      std::string_view code,
      std::size_t expected_probes,
      std::string_view message) {
    std::size_t probes = 0;
    candidate.context.query_cancellation_requested =
        CountingCancellationProbe(&probes, true);
    RequireRefusal(api::EnginePlanImportRows(candidate), code, message);
    Require(
        probes == expected_probes,
        "plan-import cancellation observation count drifted at a higher-priority refusal");
  };

  auto opcode_invalid = request;
  opcode_invalid.operation_id = "dml.update_rows";
  opcode_invalid.descriptor_ref = {};
  opcode_invalid.context.authorization_context.grants.clear();
  opcode_invalid.context.local_transaction_id = 0;
  opcode_invalid.context.transaction_uuid.canonical.clear();
  require_higher_precedence(opcode_invalid, "SBLR.OPCODE_INVALID", 0,
                            "plan-import opcode precedence drifted");

  auto operand_invalid = opcode_invalid;
  operand_invalid.operation_id = std::string(kOperationId);
  require_higher_precedence(operand_invalid, "SBLR.OPERAND_INVALID", 0,
                            "plan-import operand precedence drifted");

  auto security_denied = request;
  security_denied.context.authorization_context.grants.clear();
  security_denied.context.local_transaction_id = 0;
  security_denied.context.transaction_uuid.canonical.clear();
  ++security_denied.context.statement_snapshot_generation;
  require_higher_precedence(security_denied, "SECURITY.ACCESS_DENIED", 4,
                            "plan-import security precedence drifted");

  auto transaction_absent = request;
  transaction_absent.context.local_transaction_id = 0;
  transaction_absent.context.transaction_uuid.canonical.clear();
  ++transaction_absent.context.statement_snapshot_generation;
  require_higher_precedence(
      transaction_absent, "MGA.TRANSACTION_INVALID", 4,
      "absent plan-import transaction was not TRANSACTION_INVALID");

  auto transaction_malformed = request;
  transaction_malformed.context.transaction_uuid.canonical = "malformed";
  require_higher_precedence(
      transaction_malformed, "MGA.TRANSACTION_INVALID", 4,
      "malformed plan-import transaction was not TRANSACTION_INVALID");

  auto transaction_ended = request;
  transaction_ended.context.local_transaction_id =
      fixture.ended_local_transaction_id;
  transaction_ended.context.transaction_uuid = fixture.ended_transaction_uuid;
  require_higher_precedence(
      transaction_ended, "MGA.TRANSACTION_INVALID", 4,
      "ended plan-import transaction was not TRANSACTION_INVALID");

  auto stale_authority = request;
  ++stale_authority.context.statement_snapshot_generation;
  require_higher_precedence(
      stale_authority, "MGA.AUTHORITY_MISMATCH", 4,
      "active stale plan-import authority was not AUTHORITY_MISMATCH");

  auto stale_receipt = request;
  stale_receipt.context.statement_receipt_uuid.canonical =
      NewUuidText(platform::UuidKind::object);
  require_higher_precedence(
      stale_receipt, "MGA.AUTHORITY_MISMATCH", 4,
      "stale plan-import receipt was not AUTHORITY_MISMATCH");

  auto cluster_refused = request;
  cluster_refused.context.route_fence_present = true;
  require_higher_precedence(
      cluster_refused,
      "CLUSTER.GATEWAY_CLUSTER_FALLTHROUGH_FORBIDDEN",
      4,
      "plan-import cluster precedence drifted");

  for (std::size_t checkpoint = 1; checkpoint <= 6; ++checkpoint) {
    auto cancelled = request;
    std::size_t cancellation_probes = 0;
    cancelled.context.query_cancellation_requested =
        CancellationAtProbe(&cancellation_probes, checkpoint);
    RequireRefusal(
        api::EnginePlanImportRows(cancelled), "PROCESS.CANCELLED",
        "plan-import cancellation checkpoint was not observed");
    const std::size_t expected_probes = checkpoint <= 4 ? 4 : checkpoint;
    Require(cancellation_probes == expected_probes,
            "plan-import cancellation checkpoint count drifted");
  }

  const auto released =
      api::ReleaseEngineBoundImportRowsPlanDescriptorsV1(binder_context);
  Require(released.ok && released.released_row_count == 1,
          "plan-import receipt release count drifted");
  RequireRefusal(api::EnginePlanImportRows(request),
                 "SBLR.OPERAND_INVALID",
                 "released plan-import descriptor remained resolvable");

  const auto state_after = api::LoadMgaRelationStoreState(consumer_context);
  Require(state_after.ok,
          "plan-import postcondition relation state load failed");
  RequireRelationStateEqual(state_before.state, state_after.state);
  const auto inventory_after =
      db::LoadLocalTransactionInventoryFromDatabase(
          fixture.database_path.string());
  Require(inventory_after.ok(),
          "plan-import postcondition transaction inventory load failed");
  RequireInventoryEqual(inventory_before.inventory, inventory_after.inventory);
  const auto descriptor_after = api::LoadMgaRelationStorageDescriptor(
      consumer_context, fixture.table_uuid);
  Require(descriptor_after.ok &&
              api::SerializeMgaRelationStorageDescriptor(
                  descriptor_before.descriptor) ==
                  api::SerializeMgaRelationStorageDescriptor(
                      descriptor_after.descriptor),
          "plan-import changed the current catalog relation descriptor");
  Require(ReadDirectoryImage(fixture.root) == durable_before,
          "plan-import planning changed durable database or registry bytes");
  Require(consumer_context.catalog_generation_id ==
              fixture.planning_context.catalog_generation_id,
          "plan-import planning changed catalog generation authority");

  Rollback(fixture.planning_context);
  std::cout << "sblr_plan_import_rows_conformance: PASS\n";
  return EXIT_SUCCESS;
}
