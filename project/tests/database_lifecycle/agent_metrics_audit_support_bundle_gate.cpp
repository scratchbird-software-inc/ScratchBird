// Copyright (c) 2026 ScratchBird Software Inc.
//
// This Source Code Form is subject to the terms of the Mozilla Public
// License, v. 2.0. If a copy of the MPL was not distributed with this
// file, You can obtain one at https://mozilla.org/MPL/2.0/.
//
// SPDX-License-Identifier: MPL-2.0

#include "agents/agent_durable_catalog_store_api.hpp"
#include "diagnostics/diagnostic_rendering.hpp"
#include "management/support_bundle_api.hpp"
#include "manager_runtime.hpp"
#include "manager_support_bundle.hpp"
#include "observability/agent_observability_api.hpp"
#include "observability/metrics_api.hpp"
#include "agent_commercial_evidence.hpp"
#include "agent_durable_catalog.hpp"
#include "database_lifecycle.hpp"
#include "listener_diagnostics.hpp"
#include "listener_metrics.hpp"
#include "local_transaction_store.hpp"
#include "transaction_inventory.hpp"
#include "uuid.hpp"

#include <cstdlib>
#include <filesystem>
#include <fstream>
#include <iostream>
#include <iterator>
#include <map>
#include <string>
#include <string_view>
#include <unistd.h>
#include <utility>
#include <vector>

namespace {

namespace api = scratchbird::engine::internal_api;
namespace agents = scratchbird::core::agents;
namespace db = scratchbird::storage::database;
namespace listener = scratchbird::listener;
namespace manager = scratchbird::manager::node;
namespace mga = scratchbird::transaction::mga;
namespace platform = scratchbird::core::platform;
namespace uuid = scratchbird::core::uuid;

struct TestDatabase {
  std::filesystem::path path;
  std::string database_uuid;
  std::string transaction_uuid;
  platform::u64 local_transaction_id = 0;
};

[[noreturn]] void Fail(std::string_view message) {
  std::cerr << message << '\n';
  std::exit(EXIT_FAILURE);
}

void Require(bool condition, std::string_view message) {
  if (!condition) { Fail(message); }
}

std::string Id(platform::UuidKind kind, platform::u64 seed) {
  static std::map<std::pair<int, platform::u64>, std::string> generated_ids;
  const auto key = std::make_pair(static_cast<int>(kind), seed);
  const auto found = generated_ids.find(key);
  if (found != generated_ids.end()) { return found->second; }
  const auto generated = uuid::GenerateEngineIdentityV7(kind, 1915017000000ull + seed);
  Require(generated.ok(), "fixture UUID generation failed");
  const auto [inserted, _] =
      generated_ids.emplace(key, uuid::UuidToString(generated.value.value));
  return inserted->second;
}

std::filesystem::path MakeTempDir() {
  std::string tmpl = "/tmp/sb_pfar016_obs.XXXXXX";
  std::vector<char> writable(tmpl.begin(), tmpl.end());
  writable.push_back('\0');
  char* made = ::mkdtemp(writable.data());
  Require(made != nullptr, "mkdtemp failed for PFAR-016 gate");
  return std::filesystem::path(made);
}

void CleanupDatabase(const std::filesystem::path& path) {
  std::error_code ignored;
  std::filesystem::remove(path, ignored);
  for (const char* suffix : {".dirty.manifest",
                             ".sb.mga_event_sequence_allocator",
                             ".sb.mga_index_entries",
                             ".sb.mga_large_values",
                             ".sb.mga_relation_descriptors",
                             ".sb.mga_relation_metadata",
                             ".sb.mga_row_versions",
                             ".sb.mga_savepoints",
                             ".sb.mga_secondary_index_delta_ledger"}) {
    std::filesystem::remove(path.string() + suffix, ignored);
  }
}

TestDatabase CreateActiveDatabase(const std::filesystem::path& temp_dir) {
  const auto path = temp_dir / "support-bundle-durable-agent-catalog.sbdb";
  CleanupDatabase(path);
  const auto database_uuid = uuid::GenerateEngineIdentityV7(
      platform::UuidKind::database, 1915017000101ull);
  const auto filespace_uuid = uuid::GenerateEngineIdentityV7(
      platform::UuidKind::filespace, 1915017000102ull);
  Require(database_uuid.ok(), "database UUID generation failed");
  Require(filespace_uuid.ok(), "filespace UUID generation failed");

  db::DatabaseCreateConfig create;
  create.path = path.string();
  create.database_uuid = database_uuid.value;
  create.filespace_uuid = filespace_uuid.value;
  create.page_size = 16384;
  create.creation_unix_epoch_millis = 1915017000103ull;
  create.allow_minimal_resource_bootstrap = true;
  create.require_resource_seed_pack = false;
  create.allow_overwrite = true;
  Require(db::CreateDatabaseFile(create).ok(),
          "durable support-bundle database creation failed");

  auto inventory = mga::MakeEmptyLocalTransactionInventory();
  const auto transaction_uuid = uuid::GenerateEngineIdentityV7(
      platform::UuidKind::transaction, 1915017000104ull);
  Require(transaction_uuid.ok(), "transaction UUID generation failed");
  auto begun = mga::BeginLocalTransaction(std::move(inventory),
                                          transaction_uuid.value,
                                          1915017000105ull);
  Require(begun.ok(), "local transaction begin failed");
  Require(db::PersistLocalTransactionInventoryToDatabase(path.string(),
                                                         begun.inventory)
              .ok(),
          "local transaction inventory persist failed");

  TestDatabase database;
  database.path = path;
  database.database_uuid = uuid::UuidToString(database_uuid.value.value);
  database.transaction_uuid = uuid::UuidToString(transaction_uuid.value.value);
  database.local_transaction_id = begun.entry.identity.local_id.value;
  return database;
}

std::string ReadFile(const std::filesystem::path& path) {
  std::ifstream in(path, std::ios::binary);
  std::string text((std::istreambuf_iterator<char>(in)), std::istreambuf_iterator<char>());
  return text;
}

bool Contains(std::string_view haystack, std::string_view needle) {
  return haystack.find(needle) != std::string_view::npos;
}

bool UnsafeValue(std::string_view value) {
  return Contains(value, "/tmp/") ||
         Contains(value, "cleartext") ||
         Contains(value, "secret-token") ||
         Contains(value, "raw-principal") ||
         Contains(value, "agent.page_allocation_manager.local") ||
         Contains(value, "policy.page_allocation.default") ||
         Contains(value, "scope.database");
}

std::string Field(const api::EngineRowValue& row, std::string_view name) {
  for (const auto& field : row.fields) {
    if (field.first == name) { return field.second.encoded_value; }
  }
  return {};
}

bool HasRowField(const api::EngineApiResult& result,
                 std::string_view field_name,
                 std::string_view value) {
  for (const auto& row : result.result_shape.rows) {
    if (Field(row, field_name) == value) { return true; }
  }
  return false;
}

bool HasEvidence(const api::EngineApiResult& result,
                 std::string_view kind,
                 std::string_view id = {}) {
  for (const auto& evidence : result.evidence) {
    if (evidence.evidence_kind == kind && (id.empty() || evidence.evidence_id == id)) {
      return true;
    }
  }
  return false;
}

bool HasDiagnostic(const api::EngineApiResult& result, std::string_view code) {
  for (const auto& diagnostic : result.diagnostics) {
    if (diagnostic.code == code || Contains(diagnostic.detail, code)) { return true; }
  }
  return false;
}

void RequireNoUnsafeResultPayload(const api::EngineApiResult& result) {
  for (const auto& row : result.result_shape.rows) {
    for (const auto& field : row.fields) {
      if (field.first.size() >= 5 &&
          field.first.substr(field.first.size() - 5) == "_uuid") {
        Require(!Contains(field.second.encoded_value, "agent."),
                "synthetic agent reference leaked in UUID field");
        Require(!Contains(field.second.encoded_value, "policy."),
                "synthetic policy reference leaked in UUID field");
        Require(!Contains(field.second.encoded_value, "scope."),
                "synthetic scope reference leaked in UUID field");
      }
      Require(!UnsafeValue(field.second.encoded_value),
              "unsafe value leaked in engine result payload");
    }
  }
  for (const auto& diagnostic : result.diagnostics) {
    Require(!UnsafeValue(diagnostic.detail), "unsafe value leaked in diagnostic detail");
  }
}

api::EngineRequestContext Context(const std::filesystem::path& temp_dir) {
  api::EngineRequestContext context;
  context.security_context_present = true;
  context.database_path = (temp_dir / "runtime.sbdb").string();
  context.database_uuid.canonical = Id(platform::UuidKind::database, 1);
  context.node_uuid.canonical = Id(platform::UuidKind::object, 2);
  context.session_uuid.canonical = Id(platform::UuidKind::object, 3);
  context.principal_uuid.canonical = Id(platform::UuidKind::principal, 4);
  context.transaction_uuid.canonical = Id(platform::UuidKind::transaction, 5);
  context.trace_tags = {
      "right:OBS_METRICS_READ_FAMILY",
      "right:OBS_AGENT_EVIDENCE_READ",
      "right:OBS_AGENT_STATE_READ",
      "right:OBS_CONFIG_INSPECT"};
  return context;
}

api::EngineRequestContext DurableContext(const TestDatabase& database) {
  api::EngineRequestContext context;
  context.request_id = "pfar016-support-bundle-durable-agent-catalog";
  context.security_context_present = true;
  context.database_path = database.path.string();
  context.database_uuid.canonical = database.database_uuid;
  context.transaction_uuid.canonical = database.transaction_uuid;
  context.local_transaction_id = database.local_transaction_id;
  context.snapshot_visible_through_local_transaction_id =
      database.local_transaction_id;
  context.node_uuid.canonical = Id(platform::UuidKind::object, 102);
  context.session_uuid.canonical = Id(platform::UuidKind::object, 103);
  context.principal_uuid.canonical = Id(platform::UuidKind::principal, 104);
  context.trace_tags = {
      "right:OBS_METRICS_READ_FAMILY",
      "right:OBS_AGENT_EVIDENCE_READ",
      "right:OBS_AGENT_STATE_READ",
      "right:OBS_CONFIG_INSPECT"};
  return context;
}

api::EngineAgentRuntimeEvidenceRecord EvidenceRecord() {
  api::EngineAgentRuntimeEvidenceRecord record;
  record.source_surface = "engine_api";
  record.agent_type_id = "page_allocation_manager";
  record.agent_uuid = Id(platform::UuidKind::object, 10);
  record.filespace_uuid = Id(platform::UuidKind::filespace, 11);
  record.policy_uuid = Id(platform::UuidKind::object, 12);
  record.evidence_uuid = Id(platform::UuidKind::object, 13);
  record.action_id = "request_page_preallocation";
  record.evidence_kind = "page_preallocation";
  record.result_state = "success";
  record.diagnostic_code = "AGENT.PAGE_PREALLOCATION.COMPLETED";
  record.payload_digest = "sha256:pfar016";
  record.redaction_class = "summary";
  record.physical_path = "/tmp/protected/runtime.sbdb";
  record.raw_principal = "raw-principal-token";
  record.unsafe_payload = "password=cleartext token=secret-token";
  record.payload_redacted = true;
  return record;
}

agents::DurableAgentCatalogImage DurableCatalogWithCommercialEvidence() {
  agents::DurableAgentCatalogImage image;

  agents::AgentInstanceRecord instance;
  instance.instance_uuid = Id(platform::UuidKind::object, 110);
  instance.agent_type_id = "page_allocation_manager";
  instance.policy_uuid = Id(platform::UuidKind::object, 111);
  instance.scope = "database/filespace/page_family/page_type";
  instance.state = agents::AgentLifecycleState::running;
  instance.policy_generation = 42;
  instance.instance_generation = 7;
  image.instances.push_back(instance);

  agents::AgentActionRequest action;
  action.action_uuid = Id(platform::UuidKind::object, 112);
  action.agent_type_id = instance.agent_type_id;
  action.instance_uuid = instance.instance_uuid;
  action.actuator_id = "page_manager";
  action.operation_id = "preallocate_page_family";
  action.idempotency_key = "pfar016-support-bundle-durable-action";
  action.dry_run = false;
  action.inputs["metric_digest"] = "sha256:pfar016-durable-metric";

  agents::AgentActionAuthorityProvenance authority;
  authority.source = agents::AgentActionAuthoritySource::sealed_internal_bootstrap;
  authority.principal_uuid = Id(platform::UuidKind::principal, 113);
  authority.scope_uuid = Id(platform::UuidKind::database, 114);
  authority.provenance_evidence_uuid = Id(platform::UuidKind::object, 115);
  authority.rights = {"OBS_AGENT_CONTROL", "OBS_AGENT_EVIDENCE_READ"};
  authority.sealed_bootstrap_authority = true;

  agents::CommercialAgentEvidenceBuildRequest build;
  build.action = action;
  build.authority = authority;
  build.provider_id = "page_manager:preallocate_page_family";
  build.input_evidence_digest = "sha256:pfar016-input";
  build.input_metric_digest = "sha256:pfar016-durable-metric";
  build.policy_generation = instance.policy_generation;
  build.scope_uuids = {authority.scope_uuid};
  build.decision_payload = "durable support bundle evidence";
  build.result_state = "success";
  build.diagnostic_code = "AGENT.PAGE_PREALLOCATION.COMPLETED";
  build.redaction_class = "standard";
  build.retention_class = "agent_evidence_400_day";
  build.outcome_verification_evidence_uuid = Id(platform::UuidKind::object, 116);
  build.storage_linkage_digest = "sha256:pfar016-storage-linkage";
  build.created_at_microseconds = 1915017000200ull;
  image.evidence.push_back(agents::BuildCommercialAgentEvidence(build));
  Require(agents::ValidateCommercialAgentEvidence(image.evidence.back()).status.ok,
          "commercial evidence fixture did not validate");

  agents::DurableAgentActionRecord action_record;
  action_record.action_uuid = action.action_uuid;
  action_record.instance_uuid = instance.instance_uuid;
  action_record.owner_uuid = authority.principal_uuid;
  action_record.operation_id = action.operation_id;
  action_record.actuator_provider_id = "page_manager:preallocate_page_family";
  action_record.state = agents::DurableAgentActionState::completed;
  action_record.idempotency_key = action.idempotency_key;
  action_record.input_evidence_digest = build.input_evidence_digest;
  action_record.evidence_uuid = image.evidence.back().evidence_uuid;
  action_record.verification_evidence_uuid =
      build.outcome_verification_evidence_uuid;
  action_record.diagnostic_code = build.diagnostic_code;
  action_record.generation = 1;
  action_record.outcome_verified = true;
  image.actions.push_back(action_record);

  agents::DurableAgentLeaseRecord lease;
  lease.lease_uuid = Id(platform::UuidKind::object, 117);
  lease.instance_uuid = instance.instance_uuid;
  lease.owner_uuid = authority.principal_uuid;
  lease.state = agents::DurableAgentLeaseState::acquired;
  lease.heartbeat_generation = 2;
  lease.evidence_uuid = Id(platform::UuidKind::object, 118);
  image.leases.push_back(lease);

  agents::DurableAgentResourceReservationRecord reservation;
  reservation.reservation_uuid = Id(platform::UuidKind::object, 119);
  reservation.reservation_key = "pfar016/resource/reservation";
  reservation.owner_scope = "support-bundle-agent-runtime";
  reservation.agent_type_id = instance.agent_type_id;
  reservation.operation_id = action.operation_id;
  reservation.state = agents::DurableAgentResourceReservationState::released;
  reservation.memory_bytes = 4096;
  reservation.worker_slots = 1;
  reservation.overhead_microseconds = 250;
  reservation.evidence_uuid = Id(platform::UuidKind::object, 120);
  reservation.release_evidence_uuid = Id(platform::UuidKind::object, 121);
  reservation.release_reason = "completed";
  image.resource_reservations.push_back(reservation);

  return image;
}

void SeedDurableCatalog(const api::EngineRequestContext& context) {
  api::AgentDurableCatalogStoreRequest seed;
  seed.context = context;
  seed.image = DurableCatalogWithCommercialEvidence();
  seed.evidence_uuid = Id(platform::UuidKind::object, 122);
  seed.production_live_path = true;
  seed.fsync_or_checkpoint_evidence = true;
  const auto persisted = api::PersistAgentDurableCatalogImage(seed);
  Require(persisted.ok,
          "support bundle durable catalog seed failed: " +
              persisted.diagnostic.detail);
}

void TestEngineCollectorAndMetrics(const std::filesystem::path& temp_dir) {
  api::EngineCollectAgentRuntimeObservabilityRequest request;
  request.context = Context(temp_dir);
  request.records.push_back(EvidenceRecord());

  const auto result = api::EngineCollectAgentRuntimeObservability(request);
  Require(result.ok, "agent observability collector refused valid evidence");
  Require(result.metrics_recorded && result.audit_recorded &&
              result.diagnostics_rendered && result.support_bundle_ready &&
              result.redaction_applied,
          "agent observability collector did not mark all collector families");
  Require(HasRowField(result, "agent_uuid", request.records.front().agent_uuid),
          "agent observability collector did not expose generated agent UUID");
  Require(HasRowField(result, "physical_path", "<redacted>"),
          "agent observability collector did not redact physical path");
  Require(HasEvidence(result, "agent_observability_metric", "page_allocation_manager"),
          "agent observability metric evidence missing");
  Require(HasEvidence(result, "manager_surface", "support_bundle_agent_observability"),
          "manager support-bundle evidence missing");
  RequireNoUnsafeResultPayload(result);

  api::EngineSysMetricsCurrentRequest metrics;
  metrics.context = request.context;
  metrics.option_envelopes.push_back("family:sb_agent_page_allocation_requests_total");
  const auto current = api::EngineSysMetricsCurrent(metrics);
  Require(current.ok, "sys metrics current refused after agent evidence collection");
  Require(HasRowField(current, "metric", "sb_agent_page_allocation_requests_total"),
          "agent page allocation metric was not recorded");
  RequireNoUnsafeResultPayload(current);

  api::EngineParserPackageRenderOptions render;
  render.parser_package_uuid = Id(platform::UuidKind::object, 20);
  render.parser_package_version = "sbsql.v3";
  render.client_dialect = "sbsql";
  render.correlation_uuid = Id(platform::UuidKind::object, 21);
  render.request_uuid = Id(platform::UuidKind::object, 22);
  render.session_uuid = request.context.session_uuid.canonical;
  render.database_uuid = request.context.database_uuid.canonical;
  render.transaction_uuid = request.context.transaction_uuid.canonical;
  const auto envelope = api::RenderEngineApiResultForParserPackage(result, std::move(render));
  std::vector<std::string> errors;
  Require(api::ValidateEngineRenderedResultEnvelope(envelope, &errors),
          "parser/client rendered envelope failed validation");
  Require(!envelope.parser_finality_authority && !envelope.donor_finality_authority,
          "parser/client envelope claimed finality authority");
  for (const auto& row : envelope.rows) {
    for (const auto& field : row.fields) {
      Require(!UnsafeValue(field.encoded_value), "parser/client envelope leaked unsafe value");
    }
  }
}

void TestSupportBundleAndManagerCollectors(const std::filesystem::path& temp_dir) {
  const auto evidence = EvidenceRecord();
  api::EnginePrepareSupportBundleRequest request;
  request.context = Context(temp_dir);
  request.option_envelopes.push_back("engine_authorized_support_export:true");
  api::EngineSupportBundleAgentEvidenceSource source;
  source.agent_type_id = evidence.agent_type_id;
  source.agent_uuid = evidence.agent_uuid;
  source.filespace_uuid = evidence.filespace_uuid;
  source.policy_uuid = evidence.policy_uuid;
  source.evidence_uuid = evidence.evidence_uuid;
  source.evidence_kind = evidence.evidence_kind;
  source.result_state = evidence.result_state;
  source.diagnostic_code = evidence.diagnostic_code;
  source.payload_digest = evidence.payload_digest;
  source.physical_path = evidence.physical_path;
  source.unsafe_payload = evidence.unsafe_payload;
  source.payload_redacted = true;
  request.agent_runtime_evidence.push_back(source);

  const auto prepared = api::EnginePrepareSupportBundle(request);
  Require(prepared.ok, "support bundle API refused agent runtime evidence");
  Require(prepared.agent_runtime_evidence_collected,
          "support bundle API did not collect agent runtime evidence");
  Require(HasEvidence(prepared, "support_bundle_agent_runtime_evidence", "redacted"),
          "support bundle API missing agent runtime evidence marker");
  Require(HasRowField(prepared, "bundle_record_kind", "agent_runtime_evidence"),
          "support bundle API missing agent runtime row");
  RequireNoUnsafeResultPayload(prepared);

  api::EnginePrepareSupportBundleRequest invalid = request;
  invalid.agent_runtime_evidence.front().agent_uuid = "agent.page_allocation_manager.local";
  const auto refused = api::EnginePrepareSupportBundle(invalid);
  Require(!refused.ok, "support bundle API accepted synthetic UUID reference");
  Require(HasDiagnostic(refused, "AGENT.OBSERVABILITY.INVALID_CATALOG_UUID"),
          "support bundle API did not emit exact synthetic UUID diagnostic");

  api::EnginePrepareSupportBundleRequest malformed = request;
  malformed.agent_runtime_evidence.front().evidence_uuid = "not-a-uuid";
  const auto malformed_refused = api::EnginePrepareSupportBundle(malformed);
  Require(!malformed_refused.ok, "support bundle API accepted malformed UUID text");
  Require(HasDiagnostic(malformed_refused, "AGENT.OBSERVABILITY.INVALID_CATALOG_UUID"),
          "support bundle API malformed UUID diagnostic mismatch");

  manager::ManagerConfig config;
  config.native_bind = "/tmp/protected/native.sock";
  config.dbbt_keyring_path = "/tmp/protected/keyring";
  config.mcp_secret_ref = "secret-token";
  config.restart_executable = "/tmp/protected/sb_server";
  manager::SupportBundleInputs inputs;
  inputs.bundle_dir = temp_dir / "manager-bundle";
  inputs.scope = "local_node";
  inputs.redaction_profile = "server.support_bundle.default_redaction.v1";
  inputs.status_json = "{\"state\":\"ready\"}";
  inputs.metrics_json = "{\"metric\":\"sb_agent_page_allocation_requests_total\"}";
  inputs.agent_observability_json =
      "{\"agent_uuid\":\"" + evidence.agent_uuid +
      "\",\"unsafe\":\"password=cleartext token=secret-token\",\"path\":\"/tmp/protected/runtime.sbdb\"}";
  std::string error_code;
  Require(manager::GenerateManagerSupportBundle(config, inputs, &error_code),
          "manager support bundle generation failed");
  const auto agent_bundle = ReadFile(inputs.bundle_dir / "agent-observability.json");
  Require(Contains(agent_bundle, evidence.agent_uuid),
          "manager support bundle omitted generated agent UUID");
  Require(!UnsafeValue(agent_bundle), "manager support bundle leaked unsafe agent evidence");
  const auto manifest = ReadFile(inputs.bundle_dir / "manifest.txt");
  Require(Contains(manifest, "local_path_policy=redacted"),
          "manager support bundle did not declare path redaction");
}

void TestProductionSupportBundleReadsDurableAgentCatalog(
    const std::filesystem::path& temp_dir) {
  const auto database = CreateActiveDatabase(temp_dir);
  const auto context = DurableContext(database);
  SeedDurableCatalog(context);

  api::EnginePrepareSupportBundleRequest request;
  request.context = context;
  request.option_envelopes.push_back("engine_authorized_support_export:true");
  request.option_envelopes.push_back("agent_support_bundle_production_live:true");
  request.option_envelopes.push_back("agent_durable_catalog_store_required:true");
  request.option_envelopes.push_back("allow_caller_agent_runtime_evidence:false");

  const auto prepared = api::EnginePrepareSupportBundle(request);
  Require(prepared.ok, "production support bundle refused durable catalog");
  Require(prepared.agent_runtime_evidence_collected,
          "production support bundle did not collect durable agent evidence");
  Require(HasRowField(prepared,
                      "agent_runtime_evidence_source",
                      "durable_agent_catalog_store"),
          "support bundle did not identify durable catalog evidence source");
  Require(HasRowField(prepared,
                      "bundle_record_kind",
                      "agent_durable_catalog_summary"),
          "durable catalog summary row missing");
  Require(HasRowField(prepared,
                      "bundle_record_kind",
                      "agent_durable_evidence"),
          "durable commercial evidence row missing");
  Require(HasRowField(prepared, "tamper_valid", "true"),
          "durable evidence tamper chain was not validated");
  Require(HasRowField(prepared,
                      "bundle_record_kind",
                      "agent_durable_action"),
          "durable action row missing");
  Require(HasRowField(prepared,
                      "bundle_record_kind",
                      "agent_durable_lease"),
          "durable lease row missing");
  Require(HasRowField(prepared,
                      "bundle_record_kind",
                      "agent_durable_resource_reservation"),
          "durable resource reservation row missing");
  Require(HasEvidence(prepared, "support_bundle_agent_durable_catalog"),
          "durable catalog support-bundle evidence marker missing");
  Require(HasEvidence(prepared, "support_bundle_agent_evidence_tamper_chain"),
          "tamper-chain support-bundle evidence marker missing");
  RequireNoUnsafeResultPayload(prepared);

  api::EngineCollectAgentRuntimeObservabilityRequest observability;
  observability.context = context;
  observability.option_envelopes.push_back("agent_observability_production_live:true");
  observability.option_envelopes.push_back("agent_durable_catalog_store_required:true");
  observability.option_envelopes.push_back("allow_caller_agent_runtime_evidence:false");
  const auto collected = api::EngineCollectAgentRuntimeObservability(observability);
  Require(collected.ok,
          "production agent observability refused durable catalog records");
  Require(HasEvidence(collected, "agent_observability_durable_catalog"),
          "durable catalog observability evidence marker missing");
  Require(HasEvidence(collected, "agent_observability_tamper_chain"),
          "tamper-chain observability evidence marker missing");
  Require(HasRowField(collected,
                      "source_surface",
                      "durable_agent_catalog_store"),
          "observability did not derive rows from durable catalog store");
  RequireNoUnsafeResultPayload(collected);

  api::EnginePrepareSupportBundleRequest caller_supplied = request;
  const auto evidence = EvidenceRecord();
  api::EngineSupportBundleAgentEvidenceSource source;
  source.agent_type_id = evidence.agent_type_id;
  source.agent_uuid = evidence.agent_uuid;
  source.filespace_uuid = evidence.filespace_uuid;
  source.policy_uuid = evidence.policy_uuid;
  source.evidence_uuid = evidence.evidence_uuid;
  source.evidence_kind = evidence.evidence_kind;
  source.result_state = evidence.result_state;
  source.diagnostic_code = evidence.diagnostic_code;
  source.payload_digest = evidence.payload_digest;
  source.payload_redacted = true;
  caller_supplied.agent_runtime_evidence.push_back(source);
  const auto refused = api::EnginePrepareSupportBundle(caller_supplied);
  Require(!refused.ok,
          "production support bundle accepted caller-supplied agent evidence");
  Require(HasDiagnostic(refused,
                        "OPS.SUPPORT_BUNDLE.CALLER_AGENT_EVIDENCE_FORBIDDEN"),
          "caller-supplied production evidence refusal diagnostic drifted");

  api::EngineCollectAgentRuntimeObservabilityRequest forged_observability =
      observability;
  forged_observability.records.push_back(EvidenceRecord());
  const auto forged_refused =
      api::EngineCollectAgentRuntimeObservability(forged_observability);
  Require(!forged_refused.ok,
          "production observability accepted caller-supplied records");
  Require(HasDiagnostic(forged_refused,
                        "agent_observability_caller_records_forbidden"),
          "caller-supplied observability refusal diagnostic drifted");

  CleanupDatabase(database.path);
}

void TestProductionSupportBundleRequiresDurableCatalog(
    const std::filesystem::path& temp_dir) {
  const auto database = CreateActiveDatabase(temp_dir);
  const auto context = DurableContext(database);

  api::EnginePrepareSupportBundleRequest request;
  request.context = context;
  request.option_envelopes.push_back("engine_authorized_support_export:true");
  request.option_envelopes.push_back("agent_support_bundle_production_live:true");
  request.option_envelopes.push_back("agent_durable_catalog_store_required:true");
  request.option_envelopes.push_back("allow_caller_agent_runtime_evidence:false");
  const auto refused = api::EnginePrepareSupportBundle(request);
  Require(!refused.ok,
          "production support bundle accepted missing durable catalog");
  Require(HasDiagnostic(refused,
                        "OPS.SUPPORT_BUNDLE.DURABLE_AGENT_CATALOG_REQUIRED"),
          "missing durable catalog diagnostic drifted");

  CleanupDatabase(database.path);
}

void TestListenerCollectors() {
  listener::ListenerMetrics metrics;
  metrics.RecordAgentRuntimeEvidence("success", "AGENT.PAGE_PREALLOCATION.COMPLETED");
  const auto json = metrics.ToJson();
  Require(Contains(json, "agent_runtime_evidence_total"),
          "listener metrics did not accept agent runtime evidence");
  Require(Contains(json, "agent_runtime_result_success"),
          "listener metrics did not expose exact agent result state");
  Require(!UnsafeValue(json), "listener metrics leaked unsafe value");

  const auto diagnostic = listener::MakeDiagnostic("AGENT.PAGE_PREALLOCATION.COMPLETED",
                                                   "info",
                                                   "agent runtime evidence accepted",
                                                   "sb_listener");
  const auto vector = listener::MessageVectorSetJson(listener::MakeMessageVectorSet({diagnostic}));
  Require(Contains(vector, "AGENT.PAGE_PREALLOCATION.COMPLETED"),
          "listener diagnostic vector omitted agent diagnostic code");
  Require(!UnsafeValue(vector), "listener diagnostic vector leaked unsafe value");
}

void TestNegativeSecurityAndUuid(const std::filesystem::path& temp_dir) {
  api::EngineCollectAgentRuntimeObservabilityRequest missing_security;
  missing_security.context = Context(temp_dir);
  missing_security.context.security_context_present = false;
  missing_security.records.push_back(EvidenceRecord());
  const auto denied = api::EngineCollectAgentRuntimeObservability(missing_security);
  Require(!denied.ok, "agent observability collector accepted missing security context");
  Require(HasDiagnostic(denied, "SB_ENGINE_API_SECURITY_CONTEXT_REQUIRED"),
          "agent observability collector security diagnostic drifted");

  api::EngineCollectAgentRuntimeObservabilityRequest fake_uuid;
  fake_uuid.context = Context(temp_dir);
  fake_uuid.records.push_back(EvidenceRecord());
  fake_uuid.records.front().policy_uuid = "policy.page_allocation.default";
  const auto refused = api::EngineCollectAgentRuntimeObservability(fake_uuid);
  Require(!refused.ok, "agent observability collector accepted synthetic UUID reference");
  Require(HasDiagnostic(refused, "AGENT.OBSERVABILITY.INVALID_CATALOG_UUID"),
          "agent observability collector synthetic UUID diagnostic drifted");

  api::EngineCollectAgentRuntimeObservabilityRequest malformed_uuid;
  malformed_uuid.context = Context(temp_dir);
  malformed_uuid.records.push_back(EvidenceRecord());
  malformed_uuid.records.front().filespace_uuid = "not-a-uuid";
  const auto malformed = api::EngineCollectAgentRuntimeObservability(malformed_uuid);
  Require(!malformed.ok, "agent observability collector accepted malformed UUID text");
  Require(HasDiagnostic(malformed, "AGENT.OBSERVABILITY.INVALID_CATALOG_UUID"),
          "agent observability collector malformed UUID diagnostic drifted");
}

}  // namespace

int main() {
  const auto temp_dir = MakeTempDir();
  TestEngineCollectorAndMetrics(temp_dir);
  TestSupportBundleAndManagerCollectors(temp_dir);
  TestProductionSupportBundleReadsDurableAgentCatalog(temp_dir);
  TestProductionSupportBundleRequiresDurableCatalog(temp_dir);
  TestListenerCollectors();
  TestNegativeSecurityAndUuid(temp_dir);
  std::filesystem::remove_all(temp_dir);
  return EXIT_SUCCESS;
}
