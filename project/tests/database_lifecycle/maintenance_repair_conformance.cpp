// Copyright (c) 2026 ScratchBird Software Inc.
//
// This Source Code Form is subject to the terms of the Mozilla Public
// License, v. 2.0. If a copy of the MPL was not distributed with this
// file, You can obtain one at https://mozilla.org/MPL/2.0/.
//
// SPDX-License-Identifier: MPL-2.0

#include "database_lifecycle.hpp"
#include "database_lifecycle_test_memory.hpp"
#include "disk_device.hpp"
#include "lifecycle/engine_lifecycle_api.hpp"
#include "maintenance_coordinator.hpp"
#include "manager_control.hpp"
#include "repair_event_ledger.hpp"
#include "repair_history_inspection.hpp"
#include "repair_identity_rules.hpp"
#include "row_data_page.hpp"
#include "row_version.hpp"
#include "sblr_dispatch.hpp"
#include "session_registry.hpp"
#include "startup_state.hpp"
#include "transaction_state.hpp"
#include "uuid.hpp"

#include <array>
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
namespace disk = scratchbird::storage::disk;
namespace page = scratchbird::storage::page;
namespace sblr = scratchbird::engine::sblr;
namespace txn = scratchbird::transaction::mga;
namespace uuid = scratchbird::core::uuid;
namespace sbps = scratchbird::server::sbps;
using scratchbird::core::platform::UuidKind;
using scratchbird::server::HostedEngineState;
using scratchbird::server::ServerBootstrapConfig;
using scratchbird::server::ServerLifecycleArtifacts;
using scratchbird::server::ServerMaintenanceCoordinator;
using scratchbird::server::ServerManagementContext;
using scratchbird::server::ServerManagementRequest;
using scratchbird::server::ServerManagementResponse;
using scratchbird::server::ServerSessionRecord;
using scratchbird::server::ServerSessionRegistry;

void Require(bool condition, std::string_view message) {
  if (!condition) {
    std::cerr << message << '\n';
    std::exit(EXIT_FAILURE);
  }
}

bool Contains(std::string_view haystack, std::string_view needle) {
  return haystack.find(needle) != std::string_view::npos;
}

bool HasDiagnostic(const api::EngineApiResult& result, std::string_view code) {
  for (const auto& diagnostic : result.diagnostics) {
    if (diagnostic.code == code) return true;
  }
  return false;
}

bool HasDiagnostic(const ServerManagementResponse& result, std::string_view code) {
  for (const auto& diagnostic : result.diagnostics) {
    if (diagnostic.code == code) return true;
  }
  return false;
}

bool HasDiagnostic(const sblr::SblrDispatchResult& result, std::string_view code) {
  for (const auto& diagnostic : result.diagnostics) {
    if (diagnostic.code == code) return true;
  }
  for (const auto& diagnostic : result.api_result.diagnostics) {
    if (diagnostic.code == code) return true;
  }
  return false;
}

std::filesystem::path MakeTempDir() {
  std::string tmpl = "/tmp/sb_dblc010_maintenance_repair.XXXXXX";
  std::vector<char> writable(tmpl.begin(), tmpl.end());
  writable.push_back('\0');
  char* made = ::mkdtemp(writable.data());
  Require(made != nullptr, "mkdtemp failed for DBLC-010 maintenance repair test");
  return std::filesystem::path(made);
}

std::string UuidText(const scratchbird::core::platform::TypedUuid& value) {
  return uuid::UuidToString(value.value);
}

struct Fixture {
  std::filesystem::path path;
  std::string database_uuid;
  std::string filespace_uuid;
  scratchbird::core::platform::u32 page_size = 0;
};

Fixture CreateOpenCleanDatabase(const std::filesystem::path& path,
                                std::uint64_t now_millis) {
  db::DatabaseCreateConfig create;
  create.path = path.string();
  create.database_uuid = uuid::GenerateEngineIdentityV7(UuidKind::database, now_millis).value;
  create.filespace_uuid = uuid::GenerateEngineIdentityV7(UuidKind::filespace, now_millis + 1).value;
  create.page_size = 16384;
  create.creation_unix_epoch_millis = now_millis + 2;
  create.allow_minimal_resource_bootstrap = true;
  create.require_resource_seed_pack = false;
  create.allow_overwrite = true;
  const auto created = db::CreateDatabaseFile(create);
  if (!created.ok()) {
    std::cerr << created.diagnostic.diagnostic_code << ":"
              << created.diagnostic.message_key << '\n';
  }
  Require(created.ok(), "DBLC-010 database create failed");

  const auto opened = db::OpenDatabaseFile({path.string(), false, false, false});
  if (!opened.ok()) {
    std::cerr << opened.diagnostic.diagnostic_code << ":"
              << opened.diagnostic.message_key << '\n';
  }
  Require(opened.ok(), "DBLC-010 first open activation failed");
  const auto clean = db::MarkDatabaseCleanShutdown(path.string());
  Require(clean.ok(), "DBLC-010 clean shutdown failed");

  Fixture fixture;
  fixture.path = path;
  fixture.database_uuid = UuidText(create.database_uuid);
  fixture.filespace_uuid = UuidText(create.filespace_uuid);
  fixture.page_size = created.state.header.page_size;
  return fixture;
}

db::StartupStateRecord ReadStartup(const Fixture& fixture) {
  disk::FileDevice device;
  const auto opened = device.Open(fixture.path.string(), disk::FileOpenMode::open_existing_read_only);
  Require(opened.ok(), "startup read open failed");
  const auto startup = db::ReadStartupStatePageBody(&device, fixture.page_size);
  if (!startup.ok()) {
    std::cerr << startup.diagnostic.diagnostic_code << '\n';
  }
  Require(startup.ok(), "startup read failed");
  return startup.state;
}

template <typename Mutator>
void MutateStartup(const Fixture& fixture, Mutator mutator) {
  disk::FileDevice device;
  const auto opened = device.Open(fixture.path.string(), disk::FileOpenMode::open_existing);
  Require(opened.ok(), "startup mutation open failed");
  auto startup = db::ReadStartupStatePageBody(&device, fixture.page_size);
  Require(startup.ok(), "startup mutation read failed");
  mutator(&startup.state);
  const auto written = db::WriteStartupStatePageBody(&device, startup.state);
  Require(written.ok(), "startup mutation write failed");
  const auto synced = device.Sync();
  Require(synced.ok(), "startup mutation sync failed");
}

db::DatabaseLifecycleOperationConfig OperationConfig(const Fixture& fixture) {
  db::DatabaseLifecycleOperationConfig config;
  config.path = fixture.path.string();
  config.operation_uuid = "dblc010-operation";
  config.actor_uuid = "dblc010-actor";
  config.write_evidence = true;
  return config;
}

db::DatabaseLifecycleRepairConfig RepairConfig(const Fixture& fixture,
                                               std::string_view plan,
                                               bool admitted) {
  db::DatabaseLifecycleRepairConfig config;
  config.path = fixture.path.string();
  config.operation_uuid = "dblc010-repair";
  config.actor_uuid = "dblc010-actor";
  config.repair_plan_id = std::string(plan);
  config.expected_database_uuid = fixture.database_uuid;
  config.expected_filespace_uuid = fixture.filespace_uuid;
  config.repair_admission_proven = admitted;
  config.allow_mutation = admitted;
  return config;
}

scratchbird::core::platform::TypedUuid RepairUuid(UuidKind kind,
                                                  std::uint64_t offset) {
  const auto generated =
      uuid::GenerateEngineIdentityV7(kind, 1779300010000ull + offset);
  return generated.ok() ? generated.value
                        : scratchbird::core::platform::TypedUuid{};
}

struct RepairEvidenceFixture {
  scratchbird::core::platform::TypedUuid database_uuid =
      RepairUuid(UuidKind::database, 1);
  scratchbird::core::platform::TypedUuid operation_uuid =
      RepairUuid(UuidKind::object, 2);
  scratchbird::core::platform::TypedUuid finding_uuid =
      RepairUuid(UuidKind::object, 3);
  scratchbird::core::platform::TypedUuid page_uuid =
      RepairUuid(UuidKind::page, 4);
  scratchbird::core::platform::TypedUuid object_uuid =
      RepairUuid(UuidKind::object, 5);
  scratchbird::core::platform::TypedUuid row_uuid =
      RepairUuid(UuidKind::row, 6);
  scratchbird::core::platform::TypedUuid version_one_uuid =
      RepairUuid(UuidKind::row, 7);
  scratchbird::core::platform::TypedUuid version_two_uuid =
      RepairUuid(UuidKind::row, 8);
  scratchbird::core::platform::TypedUuid transaction_one_uuid =
      RepairUuid(UuidKind::transaction, 9);
  scratchbird::core::platform::TypedUuid transaction_two_uuid =
      RepairUuid(UuidKind::transaction, 10);
  scratchbird::core::platform::u64 page_number = 410;
};

db::RepairEventRecord RepairEventFor(const RepairEvidenceFixture& fixture,
                                     db::RepairEventPhase phase,
                                     scratchbird::core::platform::u64 sequence,
                                     scratchbird::core::platform::u64 previous_digest,
                                     std::string reason_code) {
  db::RepairEventRecord event;
  event.sequence = sequence;
  event.ledger_epoch = 17;
  event.phase = phase;
  event.database_uuid = fixture.database_uuid;
  event.operation_uuid = fixture.operation_uuid;
  event.finding_uuid = fixture.finding_uuid;
  event.page_uuid = fixture.page_uuid;
  event.object_uuid = fixture.object_uuid;
  event.row_uuid = fixture.row_uuid;
  event.version_uuid = fixture.version_two_uuid;
  event.transaction_uuid = fixture.transaction_two_uuid;
  event.local_transaction_id = 2;
  event.page_number = fixture.page_number;
  event.page_generation = 11;
  event.page_type = disk::PageType::row_data;
  event.observed_header_checksum = 0x4100ull + sequence;
  event.observed_body_checksum_low64 = 0x4200ull + sequence;
  event.observed_body_checksum_high64 = 0x4300ull + sequence;
  event.previous_event_digest = previous_digest;
  event.reason_code = std::move(reason_code);
  event.stable_detail = "maintenance_repair_conformance";
  return event;
}

db::RepairAccessRequest RepairAccessFor(const RepairEvidenceFixture& fixture,
                                        db::RepairAccessIntent intent) {
  db::RepairAccessRequest request;
  request.intent = intent;
  request.operation_uuid = fixture.operation_uuid;
  request.finding_uuid = fixture.finding_uuid;
  request.page_uuid = fixture.page_uuid;
  request.page_number = fixture.page_number;
  request.durable_mga_inventory_authority = true;
  return request;
}

db::RepairEventLedger AppendRepairEventSequence(
    const std::filesystem::path& ledger_path,
    const RepairEvidenceFixture& fixture,
    std::vector<std::pair<db::RepairEventPhase, std::string>> phases) {
  std::filesystem::remove(ledger_path);
  scratchbird::core::platform::u64 previous_digest = 0;
  for (std::size_t index = 0; index < phases.size(); ++index) {
    const auto appended = db::AppendRepairEventToLedger(
        ledger_path.string(),
        RepairEventFor(fixture,
                       phases[index].first,
                       static_cast<scratchbird::core::platform::u64>(index + 1),
                       previous_digest,
                       phases[index].second));
    if (!appended.ok()) {
      std::cerr << appended.diagnostic.diagnostic_code << ':'
                << appended.diagnostic.message_key << '\n';
    }
    Require(appended.ok(), "repair evidence event append failed");
    previous_digest = appended.event.event_digest;
  }
  const auto loaded = db::LoadRepairEventLedger(ledger_path.string());
  Require(loaded.ok(), "repair evidence ledger did not reload");
  Require(loaded.ledger.verified_append_only,
          "repair evidence ledger chain did not verify");
  return loaded.ledger;
}

txn::TransactionIdentity RepairTransactionIdentity(
    scratchbird::core::platform::TypedUuid transaction_uuid,
    scratchbird::core::platform::u64 local_id) {
  txn::TransactionIdentity identity;
  identity.local_id = txn::MakeLocalTransactionId(local_id);
  identity.transaction_uuid = transaction_uuid;
  identity.scope = txn::TransactionScope::local_node;
  return identity;
}

txn::RowVersionMetadata RepairMetadata(
    const RepairEvidenceFixture& fixture,
    scratchbird::core::platform::TypedUuid transaction_uuid,
    scratchbird::core::platform::u64 local_id,
    scratchbird::core::platform::u64 sequence,
    txn::RowVersionState row_state,
    txn::TransactionState transaction_state) {
  txn::RowVersionMetadata metadata;
  metadata.identity.row.row_uuid = fixture.row_uuid;
  metadata.identity.creator_transaction =
      RepairTransactionIdentity(transaction_uuid, local_id);
  metadata.identity.version_sequence = sequence;
  metadata.state = row_state;
  metadata.creator_transaction_state = transaction_state;
  metadata.payload_present =
      row_state != txn::RowVersionState::rolled_back &&
      row_state != txn::RowVersionState::delete_marker;
  return metadata;
}

page::RowDataRecord RepairRowFor(
    const RepairEvidenceFixture& fixture,
    scratchbird::core::platform::TypedUuid transaction_uuid,
    scratchbird::core::platform::u64 local_id,
    scratchbird::core::platform::u32 version_sequence) {
  page::RowDataRecord row;
  row.row_uuid = fixture.row_uuid;
  row.transaction_uuid = transaction_uuid;
  row.local_transaction_id = local_id;
  row.row_version = version_sequence;
  row.stable_slot_id = version_sequence;
  return row;
}

page::RepairIdentityRequest RepairExactIdentityRequest(
    const RepairEvidenceFixture& fixture,
    scratchbird::core::platform::u64 repair_event_digest) {
  page::RepairIdentityRequest request;
  request.action = page::RepairIdentityAction::exact_relocation;
  request.original_row =
      RepairRowFor(fixture, fixture.transaction_one_uuid, 1, 1);
  request.candidate_row = request.original_row;
  request.candidate_row.stable_slot_id = 9;
  request.original_metadata =
      RepairMetadata(fixture,
                     fixture.transaction_one_uuid,
                     1,
                     1,
                     txn::RowVersionState::committed,
                     txn::TransactionState::committed);
  request.candidate_metadata = request.original_metadata;
  request.original_version_uuid = fixture.version_one_uuid;
  request.candidate_version_uuid = fixture.version_one_uuid;
  request.repair_event_digest = repair_event_digest;
  return request;
}

page::RepairIdentityRequest RepairSalvagePromotionRequest(
    const RepairEvidenceFixture& fixture,
    scratchbird::core::platform::u64 repair_event_digest) {
  auto request = RepairExactIdentityRequest(fixture, repair_event_digest);
  request.action = page::RepairIdentityAction::salvage_promote_with_authority;
  request.candidate_row =
      RepairRowFor(fixture, fixture.transaction_two_uuid, 2, 2);
  request.candidate_metadata =
      RepairMetadata(fixture,
                     fixture.transaction_two_uuid,
                     2,
                     2,
                     txn::RowVersionState::uncommitted,
                     txn::TransactionState::active);
  request.candidate_metadata.chain.previous_version_sequence = 1;
  request.candidate_metadata.chain.previous_version_uuid =
      fixture.version_one_uuid;
  request.original_metadata.chain.next_version_sequence = 2;
  request.original_metadata.chain.next_version_uuid = fixture.version_two_uuid;
  request.candidate_version_uuid = fixture.version_two_uuid;
  request.logical_payload_changed = true;
  request.authoritative_payload_proof = true;
  request.salvage_uncertain = true;
  request.salvage_restore_required = true;
  request.salvage_payload_promoted_to_committed = true;
  return request;
}

db::RepairHistoryInspectionRequest RepairHistoryRequest(
    const RepairEvidenceFixture& fixture,
    const db::RepairEventLedger& ledger) {
  db::RepairHistoryInspectionRequest request;
  request.durable_mga_inventory_authority = true;
  request.repair_events = ledger.events;

  db::RepairOrdinaryVersionRecord first;
  first.metadata = RepairMetadata(fixture,
                                  fixture.transaction_one_uuid,
                                  1,
                                  1,
                                  txn::RowVersionState::committed,
                                  txn::TransactionState::committed);
  first.metadata.chain.next_version_sequence = 2;
  first.metadata.chain.next_version_uuid = fixture.version_two_uuid;
  first.version_uuid = fixture.version_one_uuid;
  first.page_uuid = fixture.page_uuid;
  first.page_number = fixture.page_number;
  request.ordinary_versions.push_back(first);

  db::RepairArchiveEntry archive;
  archive.row_uuid = fixture.row_uuid;
  archive.version_uuid = fixture.version_one_uuid;
  archive.page_uuid = fixture.page_uuid;
  archive.object_uuid = fixture.object_uuid;
  archive.page_number = fixture.page_number;
  archive.version_sequence = 1;
  archive.local_transaction_id = 1;
  archive.archive_manifest_digest = "maintenance_repair_archive_digest";
  archive.payload_present = false;
  request.archive_entries.push_back(archive);

  db::RepairSalvageEvidence salvage;
  salvage.finding_uuid = fixture.finding_uuid;
  salvage.page_uuid = fixture.page_uuid;
  salvage.row_uuid = fixture.row_uuid;
  salvage.version_uuid = fixture.version_two_uuid;
  salvage.page_number = fixture.page_number;
  salvage.salvage_class = "uncertain_review_only";
  salvage.uncertain = true;
  request.salvage_evidence.push_back(salvage);

  db::RepairDiagnosticEvidence diagnostic;
  diagnostic.row_uuid = fixture.row_uuid;
  diagnostic.page_uuid = fixture.page_uuid;
  diagnostic.page_number = fixture.page_number;
  diagnostic.diagnostic_code = "SB-REPAIR-HISTORY-DATA-LOSS-POSSIBLE";
  diagnostic.message_key = "repair.history.data_loss_possible";
  diagnostic.detail = "archive_payload_absent";
  request.diagnostics.push_back(diagnostic);
  return request;
}

api::EngineRequestContext EngineContext(const Fixture& fixture) {
  api::EngineRequestContext context;
  context.trust_mode = api::EngineTrustMode::server_isolated;
  context.request_id = "dblc010-engine-request";
  context.database_path = fixture.path.string();
  context.database_uuid.canonical = fixture.database_uuid;
  context.principal_uuid.canonical = "019e1080-a100-7000-8000-000000000101";
  context.session_uuid.canonical = "019e1080-a100-7000-8000-000000000102";
  context.security_context_present = true;
  context.catalog_generation_id = 1;
  context.security_epoch = 1;
  context.resource_epoch = 1;
  context.name_resolution_epoch = 1;
  return context;
}

sblr::SblrOperationEnvelope LifecycleEnvelope(std::string operation_id,
                                              std::string opcode) {
  auto envelope = sblr::MakeSblrEnvelope(std::move(operation_id),
                                         std::move(opcode),
                                         "trace.dblc010.lifecycle.repair");
  envelope.requires_security_context = true;
  envelope.requires_transaction_context = false;
  envelope.requires_cluster_authority = false;
  envelope.contains_sql_text = false;
  envelope.parser_resolved_names_to_uuids = true;
  return envelope;
}

sblr::SblrDispatchResult DispatchLifecycleSblr(
    const Fixture& fixture,
    std::string operation_id,
    std::string opcode,
    std::vector<std::string> options = {}) {
  api::EngineApiRequest api_request;
  api_request.option_envelopes = std::move(options);
  const sblr::SblrDispatchRequest request{
      EngineContext(fixture),
      LifecycleEnvelope(std::move(operation_id), std::move(opcode)),
      std::move(api_request)};
  return sblr::DispatchSblrOperation(request);
}

void RequireSblrLifecycleSuccess(const Fixture& fixture,
                                 std::string operation_id,
                                 std::string opcode,
                                 std::vector<std::string> options = {}) {
  const std::string expected_operation = operation_id;
  const auto result = DispatchLifecycleSblr(fixture,
                                           std::move(operation_id),
                                           std::move(opcode),
                                           std::move(options));
  Require(result.envelope_validated, "SBLR lifecycle envelope did not validate");
  Require(result.accepted, "SBLR lifecycle dispatch did not accept operation");
  Require(result.dispatched_to_api, "SBLR lifecycle dispatch did not reach API");
  if (!result.api_result.ok) {
    for (const auto& diagnostic : result.api_result.diagnostics) {
      std::cerr << diagnostic.code << ':' << diagnostic.detail << '\n';
    }
  }
  Require(result.api_result.ok, "SBLR lifecycle API operation failed");
  Require(result.api_result.operation_id == expected_operation,
          "SBLR lifecycle API operation id mismatch");
}

void TestStorageMaintenanceRepair(const Fixture& fixture) {
  const auto entered = db::EnterDatabaseRestrictedOpenMode(OperationConfig(fixture));
  Require(entered.ok(), "restricted-open entry failed");
  Require(entered.state.phase == db::DatabaseLifecyclePhase::restricted_open,
          "restricted-open phase mismatch");
  auto startup = ReadStartup(fixture);
  Require(startup.write_admission_fenced, "restricted-open did not fence write admission");
  Require(startup.durable_lifecycle_phase == db::StartupLifecycleDurablePhase::restricted_open_entered,
          "restricted-open durable phase missing");
  Require(startup.last_lifecycle_local_transaction_id != 0,
          "restricted-open did not record MGA lifecycle transaction");

  const auto ordinary_open = db::OpenDatabaseFile({fixture.path.string(), false, false, false});
  Require(!ordinary_open.ok(), "ordinary open succeeded during restricted-open");
  Require(ordinary_open.diagnostic.diagnostic_code == "SB-DB-LIFECYCLE-RESTRICTED-OPEN-REQUIRED",
          "ordinary open did not report restricted-open required");

  const auto verified = db::VerifyDatabaseLifecycle(OperationConfig(fixture));
  Require(verified.ok(), "verify failed in restricted-open");
  startup = ReadStartup(fixture);
  Require(startup.write_admission_fenced, "verify cleared restricted-open write fence");
  Require(startup.durable_lifecycle_phase == db::StartupLifecycleDurablePhase::verify_completed,
          "verify durable phase missing");

  const auto refused = db::RepairDatabaseLifecycle(RepairConfig(fixture, "", false));
  Require(!refused.ok(), "repair without plan/admission succeeded");
  Require(refused.diagnostic.diagnostic_code == "ENGINE.DBLC_REPAIR_REFUSED",
          "repair refusal did not use DBLC diagnostic");

  const auto repaired = db::RepairDatabaseLifecycle(
      RepairConfig(fixture, "clear_verified_write_fence", true));
  Require(repaired.ok(), "accepted repair plan failed");
  startup = ReadStartup(fixture);
  Require(!startup.write_admission_fenced, "repair did not clear verified write fence");
  Require(startup.durable_lifecycle_phase == db::StartupLifecycleDurablePhase::repair_completed,
          "repair durable phase missing");
  Require(startup.last_lifecycle_local_transaction_id != 0,
          "repair did not record MGA lifecycle transaction");

  const auto open_after_repair = db::OpenDatabaseFile({fixture.path.string(), false, false, false});
  Require(open_after_repair.ok(), "ordinary open after accepted repair failed");
  const auto clean = db::MarkDatabaseCleanShutdown(fixture.path.string());
  Require(clean.ok(), "clean shutdown after repair failed");

  const auto maintenance = db::EnterDatabaseMaintenanceMode(OperationConfig(fixture));
  Require(maintenance.ok(), "maintenance entry failed");
  startup = ReadStartup(fixture);
  Require(startup.write_admission_fenced, "maintenance entry did not fence writes");
  Require(startup.durable_lifecycle_phase == db::StartupLifecycleDurablePhase::maintenance_entered,
          "maintenance durable phase missing");
  const auto ordinary_during_maintenance =
      db::OpenDatabaseFile({fixture.path.string(), false, false, false});
  Require(!ordinary_during_maintenance.ok(),
          "ordinary open succeeded during maintenance");
  Require(ordinary_during_maintenance.diagnostic.diagnostic_code ==
              "SB-DB-LIFECYCLE-RESTRICTED-OPEN-REQUIRED",
          "ordinary maintenance open did not report restricted-open required");
  const auto exited = db::ExitDatabaseMaintenanceMode(OperationConfig(fixture));
  Require(exited.ok(), "maintenance exit failed");
  startup = ReadStartup(fixture);
  Require(!startup.write_admission_fenced, "maintenance exit did not clear write fence");
  Require(startup.durable_lifecycle_phase == db::StartupLifecycleDurablePhase::maintenance_exited,
          "maintenance exit durable phase missing");
}

void TestEngineLifecycleApi(const Fixture& fixture, const Fixture& corrupt_fixture) {
  api::EngineVerifyLifecycleRequest verify;
  verify.context = EngineContext(fixture);
  const auto verified = api::EngineVerifyLifecycle(verify);
  Require(verified.ok, "engine verify rejected valid database");

  api::EngineRepairLifecycleRequest refused;
  refused.context = EngineContext(fixture);
  const auto refused_result = api::EngineRepairLifecycle(refused);
  Require(!refused_result.ok, "engine repair without plan succeeded");
  Require(HasDiagnostic(refused_result, "ENGINE.DBLC_REPAIR_REFUSED"),
          "engine repair refusal missing DBLC diagnostic");

  api::EngineEnterRestrictedOpenLifecycleRequest enter_restricted;
  enter_restricted.context = EngineContext(fixture);
  const auto restricted = api::EngineEnterRestrictedOpenLifecycle(enter_restricted);
  Require(restricted.ok, "engine restricted-open entry failed");

  api::EngineRepairLifecycleRequest repair;
  repair.context = EngineContext(fixture);
  repair.option_envelopes.push_back("repair_plan_id:clear_verified_write_fence");
  repair.option_envelopes.push_back("expected_filespace_uuid:" + fixture.filespace_uuid);
  repair.option_envelopes.push_back("repair_admission_proven:true");
  repair.option_envelopes.push_back("allow_repair:true");
  const auto repaired = api::EngineRepairLifecycle(repair);
  Require(repaired.ok, "engine accepted repair plan failed");

  api::EngineVerifyLifecycleRequest corrupt_verify;
  corrupt_verify.context = EngineContext(corrupt_fixture);
  const auto corrupt_result = api::EngineVerifyLifecycle(corrupt_verify);
  Require(!corrupt_result.ok, "engine verify accepted corrupted startup identity");
  Require(HasDiagnostic(corrupt_result, "ENGINE.DBLC_VERIFY_FAILED"),
          "engine verify failure missing DBLC diagnostic");

  api::EngineVerifyLifecycleRequest missing_security;
  missing_security.context = EngineContext(fixture);
  missing_security.context.security_context_present = false;
  const auto denied = api::EngineVerifyLifecycle(missing_security);
  Require(!denied.ok, "engine verify admitted missing security context");
  Require(HasDiagnostic(denied, "SB_ENGINE_API_SECURITY_CONTEXT_REQUIRED"),
          "engine verify missing-security diagnostic mismatch");
}

void TestSblrLifecycleRoute(const Fixture& fixture) {
  RequireSblrLifecycleSuccess(fixture,
                              "lifecycle.enter_maintenance",
                              "SBLR_LIFECYCLE_ENTER_MAINTENANCE");
  RequireSblrLifecycleSuccess(fixture,
                              "lifecycle.verify_database",
                              "SBLR_LIFECYCLE_VERIFY_DATABASE");

  const auto repair_refused = DispatchLifecycleSblr(
      fixture,
      "lifecycle.repair_database",
      "SBLR_LIFECYCLE_REPAIR_DATABASE");
  Require(repair_refused.envelope_validated, "SBLR repair-refusal envelope did not validate");
  Require(repair_refused.accepted, "SBLR repair-refusal dispatch did not accept operation");
  Require(repair_refused.dispatched_to_api, "SBLR repair-refusal did not reach API");
  Require(!repair_refused.api_result.ok, "SBLR repair without admission unexpectedly succeeded");
  Require(HasDiagnostic(repair_refused, "ENGINE.DBLC_REPAIR_REFUSED"),
          "SBLR repair refusal missing exact DBLC diagnostic");

  RequireSblrLifecycleSuccess(fixture,
                              "lifecycle.exit_maintenance",
                              "SBLR_LIFECYCLE_EXIT_MAINTENANCE");
}

sbps::Frame ManagementFrame(const std::array<std::uint8_t, 16>& session_uuid,
                            std::string_view operation_key,
                            std::string_view mode = {}) {
  ServerManagementRequest request;
  request.operation_key = std::string(operation_key);
  request.mode = std::string(mode);
  sbps::Frame frame;
  frame.header.message_type = static_cast<std::uint16_t>(sbps::MessageType::kManagementRequest);
  frame.header.payload_schema_id = sbps::kSchemaManagementRequestV1;
  frame.header.request_uuid = sbps::MakeUuidV7Bytes();
  frame.header.session_uuid = session_uuid;
  frame.payload = scratchbird::server::EncodeServerManagementRequestForTest(request);
  return frame;
}

ServerSessionRegistry RegistryWithPrincipal(const Fixture& fixture,
                                            std::string_view principal,
                                            std::array<std::uint8_t, 16>* session_uuid) {
  ServerSessionRegistry registry;
  ServerSessionRecord session;
  session.session_uuid = sbps::MakeUuidV7Bytes();
  session.principal_claim = std::string(principal);
  session.database_path = fixture.path.string();
  session.database_uuid = fixture.database_uuid;
  session.effective_user_uuid = sbps::MakeUuidV7Bytes();
  if (principal == "admin") {
    session.engine_authorization_trace_tags = {
        "right:OBS_MANAGEMENT_CONTROL",
        "right:OBS_MANAGEMENT_INSPECT",
        "right:OBS_CONFIG_CONTROL",
        "right:SUPPORT_EXPORT"};
  } else if (principal == "auditor") {
    session.engine_authorization_trace_tags = {
        "right:OBS_CONFIG_INSPECT",
        "right:OBS_METRICS_READ_ALL"};
  }
  *session_uuid = session.session_uuid;
  registry.sessions_by_uuid[scratchbird::server::UuidBytesToText(session.session_uuid)] = session;
  return registry;
}

ServerManagementContext ManagementContext(ServerBootstrapConfig* config,
                                          ServerLifecycleArtifacts* artifacts,
                                          HostedEngineState* engine_state,
                                          ServerSessionRegistry* registry,
                                          ServerMaintenanceCoordinator* coordinator) {
  ServerManagementContext context;
  context.config = config;
  context.artifacts = artifacts;
  context.engine_state = engine_state;
  context.session_registry = registry;
  context.maintenance_coordinator = coordinator;
  return context;
}

void TestManagementRoute(const Fixture& fixture) {
  ServerBootstrapConfig config;
  config.database_default_path = fixture.path;
  ServerLifecycleArtifacts artifacts;
  artifacts.generation = 7;
  artifacts.state = "dblc010-test";
  HostedEngineState engine_state;
  engine_state.engine_context_active = true;
  ServerMaintenanceCoordinator coordinator =
      scratchbird::server::BuildMaintenanceCoordinator(config, artifacts);

  std::array<std::uint8_t, 16> admin_uuid{};
  auto registry = RegistryWithPrincipal(fixture, "admin", &admin_uuid);
  auto context = ManagementContext(&config, &artifacts, &engine_state, &registry, &coordinator);

  auto enter = scratchbird::server::HandleServerManagementRequest(
      context, ManagementFrame(admin_uuid, "enter_restricted_open"));
  Require(enter.accepted && !enter.error, "management restricted-open entry failed");
  const std::string enter_payload(enter.payload.begin(), enter.payload.end());
  Require(Contains(enter_payload, "restricted_open_enabled"),
          "management restricted-open payload missing outcome");
  Require(coordinator.restricted_open_mode, "coordinator did not enter restricted-open");
  Require(!scratchbird::server::MaintenanceAllowsAttach(coordinator),
          "restricted-open did not fence ordinary attach");

  auto verify = scratchbird::server::HandleServerManagementRequest(
      context, ManagementFrame(admin_uuid, "verify_database"));
  Require(verify.accepted && !verify.error, "management verify failed");

  auto repair_refused = scratchbird::server::HandleServerManagementRequest(
      context, ManagementFrame(admin_uuid, "repair_database"));
  Require(repair_refused.error, "management repair without plan succeeded");
  Require(HasDiagnostic(repair_refused, "ENGINE.DBLC_REPAIR_REFUSED"),
          "management repair refusal missing DBLC diagnostic");

  std::string repair_mode = "repair_plan_id:clear_verified_write_fence;";
  repair_mode += "expected_database_uuid:" + fixture.database_uuid + ";";
  repair_mode += "expected_filespace_uuid:" + fixture.filespace_uuid + ";";
  repair_mode += "repair_admission_proven:true;allow_repair:true";
  auto repair = scratchbird::server::HandleServerManagementRequest(
      context, ManagementFrame(admin_uuid, "repair_database", repair_mode));
  Require(repair.accepted && !repair.error, "management accepted repair failed");
  Require(!coordinator.restricted_open_mode, "repair did not clear restricted-open coordinator state");

  std::array<std::uint8_t, 16> auditor_uuid{};
  auto auditor_registry = RegistryWithPrincipal(fixture, "auditor", &auditor_uuid);
  auto auditor_context =
      ManagementContext(&config, &artifacts, &engine_state, &auditor_registry, &coordinator);
  auto denied = scratchbird::server::HandleServerManagementRequest(
      auditor_context, ManagementFrame(auditor_uuid, "verify_database"));
  Require(denied.error, "auditor unexpectedly admitted to verify management control");
  Require(HasDiagnostic(denied, "SECURITY.ACCESS_DENIED"),
          "auditor denial diagnostic mismatch");
}

void TestRepairEvidenceAuthority(const std::filesystem::path& temp_dir) {
  const RepairEvidenceFixture fixture;
  const auto ledger_path = temp_dir / "dblc010_repair_evidence.sbrel";
  const auto ledger = AppendRepairEventSequence(
      ledger_path,
      fixture,
      {{db::RepairEventPhase::finding_recorded, "damaged_page_finding"},
       {db::RepairEventPhase::scan_admission, "repair_scan_admitted"},
       {db::RepairEventPhase::mutation_admission, "repair_mutation_admitted"},
       {db::RepairEventPhase::page_quarantined, "page_quarantined"}});
  Require(ledger.events.size() == 4,
          "repair evidence ledger did not retain ordered events");
  const auto mutation_digest = ledger.events[2].event_digest;

  const auto scan_access = db::AdmitRepairAccessFromLedger(
      ledger, RepairAccessFor(fixture, db::RepairAccessIntent::repair_scan));
  Require(scan_access.ok() && scan_access.scan_allowed,
          "repair scan was not admitted from durable scan event");
  const auto mutation_access = db::AdmitRepairAccessFromLedger(
      ledger, RepairAccessFor(fixture, db::RepairAccessIntent::repair_mutation));
  Require(mutation_access.ok() && mutation_access.mutation_allowed,
          "repair mutation was not admitted from durable mutation event");
  const auto normal_access = db::AdmitRepairAccessFromLedger(
      ledger, RepairAccessFor(fixture, db::RepairAccessIntent::normal_access));
  Require(!normal_access.ok(),
          "ordinary page access was admitted while page was quarantined");
  Require(normal_access.diagnostic.diagnostic_code ==
              "SB-REPAIR-ACCESS-PAGE-QUARANTINED",
          "quarantined page access diagnostic mismatch");

  auto sequence_gap = RepairEventFor(fixture,
                                     db::RepairEventPhase::scan_admission,
                                     ledger.last_sequence + 2,
                                     ledger.last_event_digest,
                                     "sequence_gap");
  const auto sequence_result =
      db::AppendRepairEventToLedger(ledger_path.string(), sequence_gap);
  Require(!sequence_result.ok(),
          "repair evidence ledger admitted sequence gap");
  Require(sequence_result.diagnostic.diagnostic_code ==
              "SB-REPAIR-EVENT-LEDGER-CHAIN-INVALID",
          "repair evidence sequence-gap diagnostic mismatch");

  auto finality_authority = RepairEventFor(
      fixture,
      db::RepairEventPhase::scan_admission,
      ledger.last_sequence + 1,
      ledger.last_event_digest,
      "finality_authority_refused");
  finality_authority.authority
      .repair_evidence_is_transaction_finality_authority = true;
  const auto finality_result =
      db::AppendRepairEventToLedger(ledger_path.string(), finality_authority);
  Require(!finality_result.ok(),
          "repair event admitted transaction-finality authority drift");
  Require(finality_result.diagnostic.diagnostic_code ==
              "SB-REPAIR-EVENT-AUTHORITY-REFUSED",
          "repair event authority-refusal diagnostic mismatch");
  const auto unchanged = db::LoadRepairEventLedger(ledger_path.string());
  Require(unchanged.ok() && unchanged.ledger.events.size() == ledger.events.size(),
          "refused repair events mutated append-only ledger");

  db::RepairEventRetentionRequest retention;
  retention.ledger = ledger;
  retention.now_epoch_millis = 1000;
  retention.retention_deadline_epoch_millis = 999;
  retention.durable_retention_policy_loaded = true;
  retention.purge_requested = true;
  const auto purge_allowed = db::EvaluateRepairEventRetention(retention);
  Require(purge_allowed.ok() && purge_allowed.purge_allowed,
          "repair retention did not allow purge after policy deadline");
  auto legal_hold = retention;
  legal_hold.legal_hold_active = true;
  const auto legal = db::EvaluateRepairEventRetention(legal_hold);
  Require(legal.ok() && legal.purge_blocked && legal.legal_hold_blocker,
          "repair retention did not expose legal-hold blocker");
  Require(!legal.repair_evidence_is_transaction_authority,
          "repair retention elevated repair evidence to transaction authority");
  auto no_policy = retention;
  no_policy.durable_retention_policy_loaded = false;
  const auto missing_policy = db::EvaluateRepairEventRetention(no_policy);
  Require(!missing_policy.ok() &&
              missing_policy.diagnostic.diagnostic_code ==
                  "SB-REPAIR-RETENTION-POLICY-REQUIRED",
          "repair retention without durable policy did not fail closed");
  auto retention_drift = retention;
  retention_drift.repair_evidence_is_transaction_authority = true;
  const auto retention_refused =
      db::EvaluateRepairEventRetention(retention_drift);
  Require(!retention_refused.ok() &&
              retention_refused.diagnostic.diagnostic_code ==
                  "SB-REPAIR-RETENTION-AUTHORITY-REFUSED",
          "repair retention admitted repair evidence authority drift");

  const auto started_ledger = AppendRepairEventSequence(
      temp_dir / "dblc010_repair_crash_started.sbrel",
      fixture,
      {{db::RepairEventPhase::finding_recorded, "crash_finding"},
       {db::RepairEventPhase::scan_admission, "crash_scan"},
       {db::RepairEventPhase::mutation_admission, "crash_mutation"},
       {db::RepairEventPhase::crash_resume_started, "crash_resume_started"}});
  db::RepairCrashResumeRequest crash;
  crash.ledger = started_ledger;
  crash.durable_mga_inventory_authority = true;
  const auto crash_started = db::EvaluateRepairCrashResumeFromLedger(crash);
  Require(crash_started.ok() && crash_started.resume_required &&
              crash_started.replay_required && !crash_started.completed,
          "repair crash-resume start did not require replay");
  Require(!crash_started.repair_evidence_is_recovery_authority,
          "repair crash-resume elevated repair evidence to recovery authority");
  const auto completed_ledger = AppendRepairEventSequence(
      temp_dir / "dblc010_repair_crash_completed.sbrel",
      fixture,
      {{db::RepairEventPhase::finding_recorded, "crash_done_finding"},
       {db::RepairEventPhase::scan_admission, "crash_done_scan"},
       {db::RepairEventPhase::mutation_admission, "crash_done_mutation"},
       {db::RepairEventPhase::crash_resume_started, "crash_done_started"},
       {db::RepairEventPhase::crash_resume_replay_admitted,
        "crash_done_replay"},
       {db::RepairEventPhase::crash_resume_completed,
        "crash_done_completed"}});
  crash.ledger = completed_ledger;
  const auto crash_completed = db::EvaluateRepairCrashResumeFromLedger(crash);
  Require(crash_completed.ok() && !crash_completed.resume_required &&
              !crash_completed.replay_required && crash_completed.completed,
          "repair crash-resume completion did not close replay requirement");

  const auto inspected =
      db::InspectRepairHistory(RepairHistoryRequest(fixture, ledger));
  Require(inspected.ok(), "repair history inspection failed");
  Require(inspected.ordinary_version_count == 1,
          "repair history ordinary-version count mismatch");
  Require(inspected.archive_entry_count == 1,
          "repair history archive-entry count mismatch");
  Require(inspected.repair_event_count == 4,
          "repair history repair-event count mismatch");
  Require(inspected.salvage_evidence_count == 1,
          "repair history salvage count mismatch");
  Require(inspected.diagnostic_count == 1,
          "repair history diagnostic count mismatch");
  Require(inspected.quarantine_present,
          "repair history did not expose quarantine");
  Require(inspected.data_loss_possible && inspected.restore_required,
          "repair history did not expose restore-required data loss assessment");
  Require(!inspected.repair_evidence_is_transaction_authority,
          "repair history elevated repair evidence to transaction authority");

  auto history_drift = RepairHistoryRequest(fixture, ledger);
  history_drift.repair_evidence_is_transaction_authority = true;
  const auto drift_result = db::InspectRepairHistory(history_drift);
  Require(!drift_result.ok() &&
              drift_result.diagnostic.diagnostic_code ==
                  "SB-REPAIR-HISTORY-AUTHORITY-REFUSED",
          "repair history admitted repair evidence authority drift");
  auto salvage_promotion = RepairHistoryRequest(fixture, ledger);
  salvage_promotion.salvage_evidence.front().payload_promoted_to_committed =
      true;
  const auto salvage_result = db::InspectRepairHistory(salvage_promotion);
  Require(!salvage_result.ok() &&
              salvage_result.diagnostic.diagnostic_code ==
                  "SB-REPAIR-HISTORY-SALVAGE-INVALID",
          "repair history admitted direct salvage-to-committed promotion");

  auto review = RepairExactIdentityRequest(fixture, 0);
  review.action = page::RepairIdentityAction::salvage_review;
  review.repair_event_persisted_before_mutation = false;
  review.salvage_uncertain = true;
  review.salvage_restore_required = true;
  const auto review_decision = page::EvaluateRepairIdentityRule(review);
  Require(review_decision.ok() && !review_decision.mutation_allowed,
          "salvage review did not remain non-mutating evidence");
  Require(review_decision.salvage_remains_evidence &&
              review_decision.restore_required,
          "salvage review did not expose restore-required evidence");

  auto no_proof = RepairSalvagePromotionRequest(fixture, mutation_digest);
  no_proof.authoritative_payload_proof = false;
  const auto proof_refused = page::EvaluateRepairIdentityRule(no_proof);
  Require(!proof_refused.ok() &&
              proof_refused.diagnostic.diagnostic_code ==
                  "SB-REPAIR-IDENTITY-SALVAGE-PROOF-REQUIRED",
          "salvage promotion without proof did not fail closed");
  const auto promoted = page::EvaluateRepairIdentityRule(
      RepairSalvagePromotionRequest(fixture, mutation_digest));
  Require(promoted.ok() && promoted.mutation_allowed &&
              promoted.logical_correction_created_new_version,
          "authorized salvage promotion did not route through new MGA version");
  Require(promoted.salvage_remains_evidence &&
              !promoted.repair_evidence_is_transaction_authority,
          "authorized salvage promotion made salvage evidence authoritative");

  auto identity_drift = RepairExactIdentityRequest(fixture, mutation_digest);
  identity_drift.repair_evidence_is_transaction_authority = true;
  const auto identity_refused = page::EvaluateRepairIdentityRule(identity_drift);
  Require(!identity_refused.ok() &&
              identity_refused.diagnostic.diagnostic_code ==
                  "SB-REPAIR-IDENTITY-AUTHORITY-REFUSED",
          "repair identity rules admitted transaction-authority drift");
}

}  // namespace

int main() {
  scratchbird::tests::database_lifecycle::ConfigureLifecycleMemoryFixture(
      "database_lifecycle_maintenance_repair_conformance");
  const auto temp_dir = MakeTempDir();
  const auto fixture = CreateOpenCleanDatabase(temp_dir / "dblc010.sbdb", 1779300001000);
  const auto corrupt_fixture = CreateOpenCleanDatabase(temp_dir / "dblc010_corrupt.sbdb", 1779300002000);
  MutateStartup(corrupt_fixture, [](db::StartupStateRecord* state) {
    state->database_uuid = uuid::GenerateEngineIdentityV7(UuidKind::database, 1779300003000).value;
  });

  TestStorageMaintenanceRepair(fixture);
  TestEngineLifecycleApi(fixture, corrupt_fixture);
  TestSblrLifecycleRoute(fixture);
  TestManagementRoute(fixture);
  TestRepairEvidenceAuthority(temp_dir);

  std::filesystem::remove_all(temp_dir);
  return EXIT_SUCCESS;
}
