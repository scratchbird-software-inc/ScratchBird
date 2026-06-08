// Copyright (c) 2026 ScratchBird Software Inc.
//
// This Source Code Form is subject to the terms of the Mozilla Public
// License, v. 2.0. If a copy of the MPL was not distributed with this
// file, You can obtain one at https://mozilla.org/MPL/2.0/.
//
// SPDX-License-Identifier: MPL-2.0

#include "compression_policy.hpp"
#include "ddl/create_api.hpp"
#include "dml/insert_api.hpp"
#include "dml/merge_api.hpp"
#include "dml/select_api.hpp"
#include "lifecycle/engine_lifecycle_api.hpp"
#include "nosql/nosql_physical_provider_contract.hpp"
#include "nosql/nosql_provider_generation_store.hpp"
#include "online_maintenance_progress.hpp"
#include "snapshot_safe_result_cache.hpp"
#include "streaming_cursor_manager.hpp"
#include "transaction/transaction_api.hpp"
#include "vector_maintenance_jobs.hpp"
#include "vector_training_recall_lifecycle.hpp"

#include <algorithm>
#include <cstdlib>
#include <filesystem>
#include <fstream>
#include <iostream>
#include <string>
#include <string_view>
#include <unistd.h>
#include <vector>

namespace {

namespace agents = scratchbird::core::agents;
namespace api = scratchbird::engine::internal_api;
namespace exec = scratchbird::engine::executor;
namespace idx = scratchbird::core::index;
namespace wire = scratchbird::wire;

#ifndef SB_ORH121_SEED_PACK_ROOT
#define SB_ORH121_SEED_PACK_ROOT "project/resources/seed-packs/initial-resource-pack"
#endif

constexpr const char* kDatabaseUuid = "019f2200-0000-7000-8000-000000121001";
constexpr const char* kSchemaUuid = "019f2200-0000-7000-8000-000000121101";
constexpr const char* kTableUuid = "019f2200-0000-7000-8000-000000121102";
constexpr const char* kSeedRowUuid = "019f2200-0000-7000-8000-000000121201";
constexpr const char* kInsertedRowUuid = "019f2200-0000-7000-8000-000000121202";

[[noreturn]] void Fail(const std::string& message) {
  std::cerr << "ORH-121 gate failure: " << message << '\n';
  std::exit(EXIT_FAILURE);
}

void Require(bool condition, const std::string& message) {
  if (!condition) {
    Fail(message);
  }
}

bool Has(const std::vector<std::string>& values, std::string_view expected) {
  return std::find(values.begin(), values.end(), expected) != values.end();
}

bool HasPrefix(const std::vector<std::string>& values,
               std::string_view prefix) {
  return std::any_of(values.begin(), values.end(), [&](const auto& value) {
    return value.rfind(prefix, 0) == 0;
  });
}

bool HasEvidence(const api::EngineApiResult& result,
                 std::string_view kind,
                 std::string_view id = {}) {
  for (const auto& evidence : result.evidence) {
    if (evidence.evidence_kind == kind &&
        (id.empty() || evidence.evidence_id == id)) {
      return true;
    }
  }
  return false;
}

bool HasDiagnostic(const api::EngineApiResult& result,
                   std::string_view token) {
  for (const auto& diagnostic : result.diagnostics) {
    if (diagnostic.code.find(token) != std::string::npos ||
        diagnostic.detail.find(token) != std::string::npos) {
      return true;
    }
  }
  return false;
}

std::filesystem::path MakeTempDir(std::string_view label) {
  std::string tmpl = "/tmp/sb_orh121_" + std::string(label) + ".XXXXXX";
  std::vector<char> writable(tmpl.begin(), tmpl.end());
  writable.push_back('\0');
  char* made = ::mkdtemp(writable.data());
  return made == nullptr ? std::filesystem::path{} : std::filesystem::path(made);
}

std::string ObjectUuid(const std::string& key) {
  return agents::DeterministicAgentRuntimeObjectUuidFromKey("orh121|" + key);
}

std::string DatabaseUuid() {
  return agents::DeterministicAgentRuntimeDatabaseUuidFromKey("orh121|db");
}

api::EngineLocalizedName Name(std::string value) {
  return {"en", "primary", value, value, true};
}

api::EngineTypedValue TextValue(std::string value) {
  api::EngineTypedValue typed;
  typed.descriptor.descriptor_kind = "scalar";
  typed.descriptor.canonical_type_name = "text";
  typed.descriptor.encoded_descriptor = "type=text";
  typed.encoded_value = std::move(value);
  return typed;
}

api::EngineColumnDefinition Column(std::uint32_t ordinal, std::string name) {
  api::EngineColumnDefinition column;
  column.ordinal = ordinal;
  column.requested_column_uuid.canonical =
      "019f2200-0000-7000-8000-00000012130" + std::to_string(ordinal);
  column.names.push_back(Name(std::move(name)));
  column.descriptor.descriptor_uuid.canonical =
      "019f2200-0000-7000-8000-00000012140" + std::to_string(ordinal);
  column.descriptor.descriptor_kind = "scalar";
  column.descriptor.canonical_type_name = "text";
  column.descriptor.encoded_descriptor = "type=text";
  return column;
}

api::EngineRowValue Row(std::string row_uuid,
                        std::string id,
                        std::string note) {
  api::EngineRowValue row;
  row.requested_row_uuid.canonical = std::move(row_uuid);
  row.fields.push_back({"id", TextValue(std::move(id))});
  row.fields.push_back({"note", TextValue(std::move(note))});
  return row;
}

std::string FieldValue(const api::EngineApiResult& result,
                       std::string_view field,
                       std::size_t row_index = 0) {
  if (row_index >= result.result_shape.rows.size()) {
    return {};
  }
  for (const auto& [name, value] : result.result_shape.rows[row_index].fields) {
    if (name == field) {
      return value.encoded_value;
    }
  }
  return {};
}

std::uint64_t EvidenceU64(const api::EngineApiResult& result,
                          std::string_view kind) {
  for (const auto& evidence : result.evidence) {
    if (evidence.evidence_kind != kind) {
      continue;
    }
    try {
      return static_cast<std::uint64_t>(std::stoull(evidence.evidence_id));
    } catch (...) {
      return 0;
    }
  }
  return 0;
}

api::EngineRequestContext BaseContext(const std::filesystem::path& database_path,
                                      std::string_view session_suffix = "001") {
  api::EngineRequestContext context;
  context.trust_mode = api::EngineTrustMode::server_isolated;
  context.request_id = "orh121-fault-injection";
  context.database_path = database_path.string();
  context.database_uuid.canonical = kDatabaseUuid;
  context.principal_uuid.canonical = "019f2200-0000-7000-8000-000000121002";
  context.session_uuid.canonical =
      std::string("019f2200-0000-7000-8000-000000121") +
      std::string(session_suffix);
  context.security_context_present = true;
  context.catalog_generation_id = 121;
  context.security_epoch = 121;
  context.resource_epoch = 121;
  context.name_resolution_epoch = 121;
  context.trace_tags.push_back("ORH-121");
  return context;
}

api::EngineRequestContext BeginTransaction(
    const std::filesystem::path& database_path,
    std::string_view session_suffix) {
  api::EngineBeginTransactionRequest request;
  request.context = BaseContext(database_path, session_suffix);
  const auto begin = api::EngineBeginTransaction(request);
  Require(begin.ok, "transaction begin failed");
  auto context = BaseContext(database_path, session_suffix);
  context.local_transaction_id = begin.local_transaction_id;
  context.transaction_uuid = begin.transaction_uuid;
  context.snapshot_visible_through_local_transaction_id =
      begin.snapshot_visible_through_local_transaction_id != 0
          ? begin.snapshot_visible_through_local_transaction_id
          : EvidenceU64(begin, "snapshot_visible_through_local_transaction_id");
  return context;
}

void Commit(const api::EngineRequestContext& context) {
  api::EngineCommitTransactionRequest request;
  request.context = context;
  const auto commit = api::EngineCommitTransaction(request);
  Require(commit.ok, "commit failed");
}

void Rollback(const api::EngineRequestContext& context) {
  api::EngineRollbackTransactionRequest request;
  request.context = context;
  const auto rollback = api::EngineRollbackTransaction(request);
  Require(rollback.ok, "rollback failed");
}

void CreateLifecycleSchemaAndTable(const std::filesystem::path& database_path) {
  api::EngineCreateLifecycleRequest create;
  create.context = BaseContext(database_path);
  create.option_envelopes.push_back(std::string("resource_seed_pack_root:") +
                                    SB_ORH121_SEED_PACK_ROOT);
  const auto created = api::EngineCreateLifecycle(create);
  Require(created.ok, "lifecycle create failed");

  auto context = BeginTransaction(database_path, "101");
  api::EngineCreateSchemaRequest schema_request;
  schema_request.context = context;
  schema_request.target_object.uuid.canonical = kSchemaUuid;
  schema_request.target_object.object_kind = "schema";
  schema_request.localized_names.push_back(Name("orh121_schema"));
  const auto schema = api::EngineCreateSchema(schema_request);
  Require(schema.ok, "schema create failed");

  api::EngineCreateTableRequest table_request;
  table_request.context = context;
  table_request.target_schema.uuid.canonical = kSchemaUuid;
  table_request.target_schema.object_kind = "schema";
  table_request.requested_table_uuid.canonical = kTableUuid;
  table_request.target_object.uuid.canonical = kTableUuid;
  table_request.target_object.object_kind = "table";
  table_request.table_names.push_back(Name("orh121_merge_target"));
  table_request.table_columns.push_back(Column(0, "id"));
  table_request.table_columns.push_back(Column(1, "note"));
  const auto table = api::EngineCreateTable(table_request);
  Require(table.ok, "table create failed");
  Commit(context);
}

void ProvePartialMergeBatchRollbackReopenRecovery() {
  const auto work = MakeTempDir("merge");
  Require(!work.empty(), "failed to create MERGE temp directory");
  const auto database_path = work / "orh121_merge.sbdb";
  CreateLifecycleSchemaAndTable(database_path);

  auto seed_tx = BeginTransaction(database_path, "201");
  api::EngineInsertRowsRequest insert;
  insert.context = seed_tx;
  insert.target_table.uuid.canonical = kTableUuid;
  insert.target_table.object_kind = "table";
  insert.input_rows.push_back(Row(kSeedRowUuid, "1", "seed"));
  const auto inserted = api::EngineInsertRows(insert);
  Require(inserted.ok, "seed insert failed");
  Commit(seed_tx);

  auto interrupted_tx = BeginTransaction(database_path, "202");
  api::EngineMergeRowsRequest merge;
  merge.context = interrupted_tx;
  merge.target_table.uuid.canonical = kTableUuid;
  merge.target_table.object_kind = "table";
  merge.match_predicate.predicate_kind = "row_uuid_match";
  merge.input_rows.push_back(Row(kSeedRowUuid, "1", "interrupted-update"));
  merge.input_rows.push_back(Row(kInsertedRowUuid, "2", "not-inserted"));
  merge.update_assignments.push_back({"note", TextValue("interrupted-update")});
  merge.diagnostic_options.push_back(
      "orh121.fault_injection.partial_merge_batch.after_update_batch");
  const auto interrupted = api::EngineMergeRows(merge);
  Require(!interrupted.ok, "partial MERGE batch fault was not injected");
  Require(HasDiagnostic(
              interrupted,
              "fault_injection.partial_merge_batch.after_update_batch"),
          "partial MERGE fault diagnostic mismatch");
  Require(HasEvidence(interrupted, "merge_action", "update_batch"),
          "partial MERGE did not execute a real update batch before fault");
  Require(HasEvidence(interrupted,
                      "merge_fault_injection_recovery_required",
                      "rollback_reopen"),
          "partial MERGE recovery requirement evidence missing");
  Require(HasEvidence(interrupted,
                      "merge_fault_injection_mga_authority",
                      "engine_transaction_inventory"),
          "partial MERGE MGA authority evidence missing");
  Rollback(interrupted_tx);

  api::EngineOpenLifecycleRequest reopen;
  reopen.context = BaseContext(database_path, "203");
  const auto reopened = api::EngineOpenLifecycle(reopen);
  Require(reopened.ok, "database did not reopen after partial MERGE rollback");

  auto reader = BeginTransaction(database_path, "204");
  api::EngineSelectRowsRequest verify_rollback;
  verify_rollback.context = reader;
  verify_rollback.source_object.uuid.canonical = kTableUuid;
  verify_rollback.source_object.object_kind = "table";
  verify_rollback.select_predicate.predicate_kind = "row_uuid_match";
  verify_rollback.select_predicate.canonical_predicate_envelope = kSeedRowUuid;
  verify_rollback.select_projection.canonical_projection_envelopes = {"note"};
  const auto before_recovery_update = api::EngineSelectRows(verify_rollback);
  Require(before_recovery_update.ok,
          "post-reopen SELECT after partial MERGE rollback failed");
  Require(before_recovery_update.visible_count == 1,
          "rolled-back partial MERGE changed visible row count");
  Require(FieldValue(before_recovery_update, "note") == "seed",
          "interrupted MERGE update survived rollback/reopen");
  api::EngineMergeRowsRequest probe;
  probe.context = reader;
  probe.target_table.uuid.canonical = kTableUuid;
  probe.target_table.object_kind = "table";
  probe.match_predicate.predicate_kind = "row_uuid_match";
  probe.update_when_matched = true;
  probe.insert_when_not_matched = false;
  probe.input_rows.push_back(Row(kSeedRowUuid, "1", "after-reopen"));
  probe.update_assignments.push_back({"note", TextValue("after-reopen")});
  const auto visibility = api::EngineMergeRows(probe);
  Require(visibility.ok, "post-reopen MERGE visibility probe failed");
  Require(visibility.matched_count == 1,
          "rolled-back partial MERGE hid the committed seed row");
  Require(visibility.updated_count == 1,
          "post-reopen committed seed row was not updateable");
  Require(FieldValue(visibility, "note") == "after-reopen",
          "post-reopen MERGE returned stale interrupted value");
  Require(HasEvidence(visibility, "mga_visibility_recheck", "required"),
          "post-reopen MERGE lost MGA recheck evidence");
  Require(HasEvidence(visibility, "security_recheck", "required"),
          "post-reopen MERGE lost security recheck evidence");
  Commit(reader);
}

wire::StreamingCursorState CursorState() {
  wire::StreamingCursorState state;
  state.cursor_id = "019f2200-0000-7000-8000-000000121301";
  state.plan_result_contract_hash = "fnv1a64:orh121-result-contract";
  state.catalog_epoch = 121;
  state.descriptor_epoch = 122;
  state.transaction_snapshot_class = "mga_statement_snapshot";
  state.transaction_uuid = "019f2200-0000-7000-8000-000000121302";
  state.local_transaction_id = 12101;
  state.snapshot_visible_through_local_transaction_id = 12100;
  state.security_epoch = 123;
  state.redaction_epoch = 124;
  state.route_kind = "embedded";
  state.frame_sequence = 1;
  state.expiry_deadline_unix_millis = 10000;
  state.client_credit.frame_credit = 2;
  state.client_credit.row_credit = 16;
  state.client_credit.byte_credit = 4096;
  state.client_credit.backpressure_active = false;
  return state;
}

void ProveCursorInterruptionFailsClosedAfterRestart() {
  wire::StreamingCursorManager manager;
  const auto opened =
      manager.OpenCursor({.state = CursorState(), .now_unix_millis = 100});
  Require(opened.ok(), "cursor open failed");
  Require(Has(opened.evidence,
              "cursor_mga_visibility_or_finality_authority=false"),
          "cursor non-authority evidence missing");
  auto binding = wire::StreamingCursorBindingFromState(opened.state);
  const auto delivered =
      manager.RecordFrameDelivery({.expected = binding,
                                   .row_count = 4,
                                   .byte_count = 512,
                                   .now_unix_millis = 101});
  Require(delivered.ok(), "cursor frame delivery failed before interruption");
  binding.frame_sequence = delivered.state.frame_sequence;

  wire::StreamingCursorManager restarted_manager;
  const auto after_restart =
      restarted_manager.ValidateFetch({.expected = binding,
                                        .now_unix_millis = 102});
  Require(!after_restart.ok(), "interrupted cursor survived restart");
  Require(after_restart.diagnostic.diagnostic_code ==
              "SB_ORH_STREAMING_CURSOR.NOT_FOUND",
          "interrupted cursor diagnostic mismatch");
  Require(Has(after_restart.evidence, "fail_closed=true"),
          "interrupted cursor did not fail closed");
}

idx::CompressionPolicyRequest CompressionDictionaryRequest() {
  auto request =
      idx::DefaultCompressionPolicyRequest(idx::CompressionFamily::kDocumentShape);
  request.uncompressed_bytes = 32 * 1024;
  request.estimated_compressed_bytes = 8 * 1024;
  request.cost.cpu_cost = 2;
  request.cost.io_savings = 30;
  request.cost.cache_density_gain = 16;
  request.cost.update_frequency_penalty = 1;
  request.cost.read_hotness = 10;
  request.cost.write_hotness = 1;
  request.measured_feedback.present = true;
  request.measured_feedback.compress_ns_per_byte = 1.0;
  request.measured_feedback.decompress_ns_per_byte = 1.0;
  request.measured_feedback.observed_compression_ratio = 0.25;
  request.measured_feedback.cache_hit_improvement = 0.20;
  request.measured_feedback.write_amplification_change = -0.05;
  request.measured_feedback.update_rewrite_cost = 1.0;
  request.measured_feedback.dictionary_miss_rate = 0.0;
  request.measured_feedback.fallback_rate = 0.0;
  request.measured_feedback.sample_count = 2048;
  request.measured_feedback.age_ms = 100;
  request.dictionary.required = true;
  request.dictionary.present = true;
  request.dictionary.reuse_observed = true;
  request.dictionary.observed_generation = 121;
  request.dictionary.current_generation = 121;
  request.dictionary.training_reason = "orh121_publish";
  request.dictionary.retraining_reason = "not_required";
  request.dictionary.stale_dictionary_fallback = true;
  return request;
}

agents::OnlineMaintenanceRecord RecoveredCheckpoint(
    const std::filesystem::path& checkpoint_path,
    const agents::OnlineMaintenanceRecord& record) {
  const auto persisted = agents::PersistOnlineMaintenanceCheckpointFile(
      checkpoint_path.string(), record);
  Require(persisted.ok, "online maintenance checkpoint persist failed");
  const auto loaded =
      agents::LoadOnlineMaintenanceCheckpointFile(checkpoint_path.string());
  Require(loaded.ok(), "online maintenance checkpoint did not reload");
  return loaded.record;
}

void ProveCompressionDictionaryPublishRecovery() {
  const auto decision = idx::EvaluateCompressionPolicy(
      CompressionDictionaryRequest());
  Require(decision.accepted, "compression dictionary policy was not accepted");
  Require(Has(decision.evidence, "compression_dictionary_reuse_observed=true"),
          "compression dictionary reuse evidence missing");

  agents::OnlineMaintenanceStateStore store;
  agents::OnlineMaintenanceStartRequest start;
  start.kind = agents::OnlineMaintenanceOperationKind::generation_publish;
  start.operation_uuid = ObjectUuid("compression_dictionary_publish");
  start.database_uuid = DatabaseUuid();
  start.target_uuid = ObjectUuid("compression_dictionary");
  start.stage = "compression_dictionary_publish_started";
  start.total_units = 4;
  start.now_microseconds = 1000;
  start.engine_mga_authoritative = true;
  start.durable_checkpoint_persisted = true;
  start.support_bundle_sink_available = true;
  start.observability_sink_available = true;
  const auto started = agents::StartOnlineMaintenanceOperation(&store, start);
  Require(started.ok(), "compression dictionary publish start failed");

  agents::OnlineMaintenanceProgressRequest progress;
  progress.operation_uuid = start.operation_uuid;
  progress.stage = "dictionary_bytes_written_publish_ready";
  progress.completed_units = 4;
  progress.total_units = 4;
  progress.now_microseconds = 2000;
  progress.durable_checkpoint_persisted = true;
  progress.checkpoint_payload = "dictionary_generation=121";
  const auto ready = agents::RecordOnlineMaintenanceProgress(&store, progress);
  Require(ready.ok(), "compression dictionary publish progress failed");
  Require(ready.snapshot.phase ==
              agents::OnlineMaintenancePhase::publish_ready,
          "compression dictionary did not reach publish_ready");

  const auto work = MakeTempDir("compression");
  Require(!work.empty(), "failed to create compression temp directory");
  const auto checkpoint = work / "dictionary.checkpoint";
  const auto recovered_record = RecoveredCheckpoint(checkpoint, ready.record);
  agents::OnlineMaintenanceStateStore recovered_store;
  const auto recovered = agents::RecoverOnlineMaintenanceOperation(
      &recovered_store,
      {.checkpoint = recovered_record,
       .now_microseconds = 3000,
       .engine_mga_authoritative = true,
       .support_bundle_sink_available = true,
       .observability_sink_available = true});
  Require(recovered.ok(), "compression dictionary checkpoint did not recover");
  Require(recovered.decision ==
              agents::OnlineMaintenanceDecision::recovered_resumable,
          "compression dictionary recovery did not become resumable");

  const auto resumed = agents::ResumeOnlineMaintenanceOperation(
      &recovered_store,
      {.operation_uuid = start.operation_uuid,
       .now_microseconds = 4000,
       .engine_mga_authoritative = true,
       .durable_checkpoint_persisted = true,
       .support_bundle_sink_available = true,
       .observability_sink_available = true});
  Require(resumed.ok(), "compression dictionary resume failed");
  progress.now_microseconds = 5000;
  const auto ready_again =
      agents::RecordOnlineMaintenanceProgress(&recovered_store, progress);
  Require(ready_again.ok(), "compression dictionary recovered progress failed");
  const auto published = agents::PublishOnlineMaintenanceOperation(
      &recovered_store,
      {.operation_uuid = start.operation_uuid,
       .now_microseconds = 6000,
       .engine_mga_authoritative = true,
       .durable_publication_fence_persisted = true,
       .authoritative_generation_validated = true,
       .no_partial_visibility = true,
       .support_bundle_sink_available = true,
       .observability_sink_available = true});
  Require(published.ok(), "compression dictionary publish after recovery failed");
  Require(published.snapshot.published_visible,
          "compression dictionary publish was not visible after recovery");

  auto unsafe = ready.record;
  unsafe.snapshot.partial_publish_visible = true;
  const auto unsafe_recovered = agents::RecoverOnlineMaintenanceOperation(
      &recovered_store,
      {.checkpoint = unsafe,
       .now_microseconds = 7000,
       .engine_mga_authoritative = true,
       .support_bundle_sink_available = true,
       .observability_sink_available = true});
  Require(!unsafe_recovered.ok(),
          "unsafe partial compression dictionary publish recovered");
  Require(unsafe_recovered.snapshot.diagnostic_code ==
              agents::kOnlineMaintenanceUnsafeRestartRefused,
          "unsafe compression dictionary diagnostic mismatch");
}

api::EngineNoSqlPhysicalProviderContract ProviderContract(
    const api::EngineRequestContext& context,
    const api::EngineNoSqlProviderGenerationMetadata& generation,
    std::uint64_t required_generation) {
  api::EngineNoSqlPhysicalProviderContract contract;
  contract.family = api::EngineNoSqlProviderFamily::kDocument;
  contract.provider_id = generation.provider_id;
  contract.provider_generation.required = true;
  contract.provider_generation.proof_present = true;
  contract.provider_generation.visible_to_snapshot = true;
  contract.provider_generation.publish_state_bound = true;
  contract.provider_generation.validation_state_bound = true;
  contract.provider_generation.backup_restore_repair_metadata_bound = true;
  contract.provider_generation.support_bundle_evidence_bound = true;
  contract.provider_generation.required_generation = required_generation;
  contract.provider_generation.available_generation = generation.generation_id;
  contract.provider_generation.descriptor_epoch = context.resource_epoch;
  contract.provider_generation.security_epoch = context.security_epoch;
  contract.provider_generation.redaction_epoch = context.security_epoch;
  contract.provider_generation.catalog_epoch = context.catalog_generation_id;
  contract.provider_generation.generation_uuid = generation.generation_uuid;
  contract.provider_generation.provider_id = generation.provider_id;
  contract.provider_generation.database_uuid = context.database_uuid.canonical;
  contract.provider_generation.collection_uuid = generation.collection_uuid;
  contract.provider_generation.publish_state = "published";
  contract.provider_generation.validation_state = "validated";
  contract.provider_generation.backup_metadata_ref = generation.backup_metadata_ref;
  contract.provider_generation.restore_metadata_ref =
      generation.restore_metadata_ref;
  contract.provider_generation.repair_metadata_ref = generation.repair_metadata_ref;
  contract.provider_generation.support_bundle_evidence_id =
      generation.support_bundle_evidence_id;
  return contract;
}

void ProveNoSqlProviderPublishRecovery() {
  const auto work = MakeTempDir("nosql");
  Require(!work.empty(), "failed to create NoSQL temp directory");
  const auto db_path = work / "orh121_nosql.sbdb";
  auto context = BaseContext(db_path, "501");
  const std::string provider_id = "orh121.document_path_provider";
  const std::string collection_uuid = ObjectUuid("nosql_collection");
  api::CleanupNoSqlProviderGenerations(context, true);
  auto gen1 = api::MakeDocumentProviderGenerationMetadata(
      context, provider_id, collection_uuid, 1);
  const auto published1 =
      api::PublishNoSqlProviderGeneration(context, gen1);
  Require(published1.ok, "NoSQL provider generation 1 publish failed");

  const auto generation_file =
      std::filesystem::path(context.database_path + ".sb.nosql_provider_generations");
  {
    std::ofstream out(generation_file, std::ios::binary | std::ios::app);
    out << "SBNOSQLPG1\tGENERATION\tfamily=document|provider_id="
        << provider_id << "|generation_id=2";
  }
  api::CleanupNoSqlProviderGenerations(context, false);
  const auto loaded = api::LoadNoSqlProviderGeneration(
      context,
      api::EngineNoSqlProviderFamily::kDocument,
      provider_id,
      collection_uuid);
  Require(loaded.ok, "NoSQL provider generation did not reopen after partial append");
  Require(loaded.metadata.generation_id == 1,
          "partial NoSQL provider publish became visible");

  auto stale_contract = ProviderContract(context, loaded.metadata, 2);
  const auto stale = api::ValidateNoSqlProviderGeneration(context, stale_contract);
  Require(!stale.ok, "stale NoSQL provider generation was accepted");
  Require(stale.diagnostic.code ==
              api::kNoSqlProviderGenerationStale ||
              HasPrefix(stale.evidence, "provider_generation_refusal="),
          "stale NoSQL provider diagnostic missing");
  Require(Has(stale.evidence, "provider_generation_fail_closed=true"),
          "stale NoSQL provider validation did not fail closed");

  auto gen2 = api::MakeDocumentProviderGenerationMetadata(
      context, provider_id, collection_uuid, 2);
  api::EngineNoSqlProviderGenerationRepairRequest repair;
  repair.family = api::EngineNoSqlProviderFamily::kDocument;
  repair.provider_id = provider_id;
  repair.collection_uuid = collection_uuid;
  repair.repair_admitted = true;
  repair.authoritative_source_generations.push_back(gen2);
  const auto repaired = api::RepairNoSqlProviderGeneration(context, repair);
  Require(repaired.ok, "NoSQL provider generation repair failed");
  Require(Has(repaired.evidence,
              "provider_generation_repair_source=authoritative"),
          "NoSQL provider repair source evidence missing");
  api::CleanupNoSqlProviderGenerations(context, false);
  const auto loaded_repaired = api::LoadNoSqlProviderGeneration(
      context,
      api::EngineNoSqlProviderFamily::kDocument,
      provider_id,
      collection_uuid);
  Require(loaded_repaired.ok && loaded_repaired.metadata.generation_id == 2,
          "NoSQL provider repaired generation did not reopen");
}

idx::VectorTrainingRecallLifecycleDecision VectorLifecycleDecision() {
  auto profile =
      idx::DefaultVectorTrainingRecallLifecycleProfile(
          idx::IndexVectorAlgorithm::hnsw);
  profile.drift.deleted_vector_count = 400;
  profile.drift.live_vector_count = 600;
  profile.drift.tombstone_ratio = 0.40;
  profile.drift.max_tombstone_ratio = 0.20;
  profile.drift.hnsw_graph_age_generations = 12;
  profile.drift.max_hnsw_graph_age_generations = 8;
  profile.drift.p95_latency_microseconds = 6000;
  profile.drift.policy_p95_latency_microseconds = 2000;
  return idx::EvaluateVectorTrainingRecallLifecycle(profile);
}

void ProveVectorJobPublishRecovery() {
  const auto decision = VectorLifecycleDecision();
  Require(decision.accepted, "vector lifecycle decision was not accepted");
  Require(decision.action ==
              idx::VectorTrainingRecallLifecycleAction::kScheduleRebuild,
          "vector lifecycle did not schedule rebuild");

  agents::VectorMaintenanceJobStore store;
  agents::VectorMaintenanceJobRequest request;
  request.job_uuid = ObjectUuid("vector_publish_job");
  request.database_uuid = DatabaseUuid();
  request.target_collection_uuid = ObjectUuid("vector_collection");
  request.target_index_uuid = ObjectUuid("vector_index");
  request.provider_generation = 121;
  request.old_training_generation = 120;
  request.new_training_generation = 121;
  request.action_kind = agents::VectorMaintenanceActionKind::rebuild;
  request.lifecycle_decision = decision;
  request.total_units = 8;
  request.now_microseconds = 1000;
  const auto created = agents::CreateVectorMaintenanceJob(&store, request);
  Require(created.ok(), "vector maintenance job create failed");
  const auto progressed = agents::RecordVectorMaintenanceProgress(
      &store, request.job_uuid, 4, 8, "scan_vectors", 2000);
  Require(progressed.ok(), "vector maintenance progress failed");
  const auto cancelled = agents::CancelVectorMaintenanceJob(
      &store, request.job_uuid, "orh121_crash_interrupt", 2500);
  Require(cancelled.ok(), "vector maintenance interrupt checkpoint failed");

  const auto work = MakeTempDir("vector");
  Require(!work.empty(), "failed to create vector temp directory");
  const auto checkpoint = work / "vector.checkpoint";
  const auto recovered_record =
      RecoveredCheckpoint(checkpoint, cancelled.record.progress);
  agents::OnlineMaintenanceStateStore recovered_progress;
  const auto recovered = agents::RecoverOnlineMaintenanceOperation(
      &recovered_progress,
      {.checkpoint = recovered_record,
       .now_microseconds = 3000,
       .engine_mga_authoritative = true,
       .support_bundle_sink_available = true,
       .observability_sink_available = true});
  Require(recovered.ok(), "vector checkpoint did not recover");
  Require(recovered.decision ==
              agents::OnlineMaintenanceDecision::recovered_resumable,
          "vector recovery did not become resumable");

  const auto resumed =
      agents::ResumeVectorMaintenanceJob(&store, request.job_uuid, 4000);
  Require(resumed.ok(), "vector maintenance resume failed");
  const auto ready =
      agents::MarkVectorMaintenanceValidationReady(&store, request.job_uuid, 5000);
  Require(ready.ok(), "vector maintenance validation-ready failed");
  const auto published =
      agents::PublishVectorMaintenanceAfterValidation(&store, request.job_uuid, 6000);
  Require(published.ok(), "vector maintenance publish failed");
  Require(published.record.publish_state ==
              agents::VectorMaintenancePublishState::published,
          "vector maintenance publish state mismatch");
  Require(published.record.progress.snapshot.published_visible,
          "vector maintenance publish was not visible");
  Require(Has(published.record.evidence, "ann_visibility_authority=false"),
          "vector publish claimed ANN visibility authority");
  Require(Has(published.record.evidence, "ann_finality_authority=false"),
          "vector publish claimed ANN finality authority");
}

exec::SnapshotSafeCacheKey SnapshotCacheKey() {
  exec::SnapshotSafeCacheKey key;
  key.normalized_operation = "orh121.snapshot.cache";
  key.safe_parameter_digest = "params:orh121";
  key.catalog_epoch = 121;
  key.statistics_epoch = 122;
  key.security_epoch = 123;
  key.redaction_epoch = 124;
  key.mga_visibility_snapshot_class = "mga_statement_snapshot";
  key.provider_generation = 125;
  key.result_contract_identity = "contract:orh121";
  key.result_contract_hash = "hash:orh121";
  key.route_compatibility = "embedded";
  key.dialect_compatibility = "sbsql_v3";
  return key;
}

exec::SnapshotSafeCacheStoreRequest SnapshotStoreRequest() {
  exec::SnapshotSafeCacheStoreRequest request;
  request.entry.key = SnapshotCacheKey();
  request.entry.payload_kind = exec::SnapshotSafeCachePayloadKind::kSmallFinalResult;
  request.entry.row_count = 1;
  request.entry.cached_result_digest = "result:old";
  request.entry.cached_mga_security_digest = "mga-security:old";
  request.small_final_result = true;
  return request;
}

exec::SnapshotSafeCacheLookupRequest SnapshotLookupRequest() {
  exec::SnapshotSafeCacheLookupRequest request;
  request.key = SnapshotCacheKey();
  request.payload_kind = exec::SnapshotSafeCachePayloadKind::kSmallFinalResult;
  request.row_count = 1;
  request.small_final_result = true;
  request.recomputed_result_digest = "result:old";
  request.recomputed_mga_security_digest = "mga-security:old";
  return request;
}

void ProveCacheInvalidationAfterReopenRecovery() {
  exec::SnapshotSafeResultCache cache;
  const auto stored = cache.Store(SnapshotStoreRequest());
  Require(stored.accepted && stored.action == exec::SnapshotSafeCacheAction::kStore,
          "snapshot cache store failed");

  exec::SnapshotSafeResultCache reopened_cache;
  const auto miss = reopened_cache.Lookup(SnapshotLookupRequest());
  Require(miss.accepted &&
              miss.action == exec::SnapshotSafeCacheAction::kMissRecompute,
          "snapshot cache did not miss/recompute after reopen");
  Require(Has(miss.evidence, "snapshot_cache_miss_recompute=true"),
          "snapshot cache reopen miss evidence missing");
  Require(Has(miss.evidence, "cache_recovery_authority=false"),
          "snapshot cache recovery non-authority evidence missing");

  reopened_cache.Store(SnapshotStoreRequest());
  auto changed = SnapshotLookupRequest();
  changed.recomputed_result_digest = "result:new-after-recovery";
  const auto invalidated = reopened_cache.Lookup(changed);
  Require(invalidated.accepted &&
              invalidated.action ==
                  exec::SnapshotSafeCacheAction::kInvalidateRecompute,
          "snapshot cache stale entry was not invalidated");
  Require(Has(invalidated.evidence, "snapshot_cache_invalidated=true"),
          "snapshot cache invalidation evidence missing");

  auto authority = SnapshotLookupRequest();
  authority.recovery_authority_cached = true;
  const auto refused = reopened_cache.Lookup(authority);
  Require(!refused.accepted &&
              refused.diagnostic_code ==
                  "EXECUTOR.SNAPSHOT_RESULT_CACHE.AUTHORITY_REFUSED",
          "snapshot cache authority refusal diagnostic mismatch");
}

}  // namespace

int main() {
  ProvePartialMergeBatchRollbackReopenRecovery();
  ProveCursorInterruptionFailsClosedAfterRestart();
  ProveCompressionDictionaryPublishRecovery();
  ProveNoSqlProviderPublishRecovery();
  ProveVectorJobPublishRecovery();
  ProveCacheInvalidationAfterReopenRecovery();
  std::cout << "optimizer_runtime_hot_path_orh_121_gate=passed\n";
  return EXIT_SUCCESS;
}
