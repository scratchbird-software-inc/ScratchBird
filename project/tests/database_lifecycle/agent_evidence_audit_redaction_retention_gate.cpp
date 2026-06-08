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
#include "observability/agent_evidence_retention_api.hpp"
#include "agent_commercial_evidence.hpp"
#include "agent_durable_catalog.hpp"
#include "database_lifecycle.hpp"
#include "local_transaction_store.hpp"
#include "transaction_inventory.hpp"
#include "uuid.hpp"

#include <cstdlib>
#include <filesystem>
#include <iostream>
#include <map>
#include <string>
#include <string_view>
#include <utility>
#include <vector>

namespace {

namespace api = scratchbird::engine::internal_api;
namespace agents = scratchbird::core::agents;
namespace db = scratchbird::storage::database;
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

bool Contains(std::string_view haystack, std::string_view needle) {
  return haystack.find(needle) != std::string_view::npos;
}

std::string Id(platform::UuidKind kind, platform::u64 seed) {
  static std::map<std::pair<int, platform::u64>, std::string> generated_ids;
  const auto key = std::make_pair(static_cast<int>(kind), seed);
  const auto found = generated_ids.find(key);
  if (found != generated_ids.end()) { return found->second; }
  const auto generated = uuid::GenerateEngineIdentityV7(kind, 1916017000000ull + seed);
  Require(generated.ok(), "fixture UUID generation failed");
  const auto [inserted, _] =
      generated_ids.emplace(key, uuid::UuidToString(generated.value.value));
  return inserted->second;
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

TestDatabase CreateActiveDatabase(const char* basename,
                                  platform::u64 timestamp_base) {
  const auto path = std::filesystem::temp_directory_path() / basename;
  CleanupDatabase(path);
  const auto database_uuid = uuid::GenerateEngineIdentityV7(
      platform::UuidKind::database, timestamp_base + 1);
  const auto filespace_uuid = uuid::GenerateEngineIdentityV7(
      platform::UuidKind::filespace, timestamp_base + 2);
  Require(database_uuid.ok(), "database UUID generation failed");
  Require(filespace_uuid.ok(), "filespace UUID generation failed");

  db::DatabaseCreateConfig create;
  create.path = path.string();
  create.database_uuid = database_uuid.value;
  create.filespace_uuid = filespace_uuid.value;
  create.page_size = 16384;
  create.creation_unix_epoch_millis = timestamp_base + 3;
  create.allow_minimal_resource_bootstrap = true;
  create.require_resource_seed_pack = false;
  create.allow_overwrite = true;
  Require(db::CreateDatabaseFile(create).ok(),
          "retention durable catalog database creation failed");

  auto inventory = mga::MakeEmptyLocalTransactionInventory();
  const auto transaction_uuid = uuid::GenerateEngineIdentityV7(
      platform::UuidKind::transaction, timestamp_base + 4);
  Require(transaction_uuid.ok(), "transaction UUID generation failed");
  auto begun = mga::BeginLocalTransaction(std::move(inventory),
                                          transaction_uuid.value,
                                          timestamp_base + 5);
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

bool HasDiagnostic(const api::EngineApiResult& result, std::string_view code) {
  for (const auto& diagnostic : result.diagnostics) {
    if (diagnostic.code == code || Contains(diagnostic.detail, code)) { return true; }
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

bool UnsafeValue(std::string_view value) {
  return Contains(value, "/tmp/") ||
         Contains(value, "cleartext") ||
         Contains(value, "secret-token") ||
         Contains(value, "raw-principal") ||
         Contains(value, "protected_payload") ||
         Contains(value, "agent.page_allocation_manager.local") ||
         Contains(value, "policy.page_allocation.default") ||
         Contains(value, "scope.database");
}

void RequireUuidFieldAuthority(const api::EngineApiResult& result) {
  for (const auto& row : result.result_shape.rows) {
    for (const auto& field : row.fields) {
      if (field.first.size() >= 5 &&
          field.first.substr(field.first.size() - 5) == "_uuid" &&
          !field.second.encoded_value.empty() &&
          field.second.encoded_value.rfind("<redacted", 0) != 0) {
        Require(!Contains(field.second.encoded_value, "agent."),
                "synthetic agent reference leaked in UUID field");
        Require(!Contains(field.second.encoded_value, "policy."),
                "synthetic policy reference leaked in UUID field");
        Require(!Contains(field.second.encoded_value, "scope."),
                "synthetic scope reference leaked in UUID field");
        const auto kind = field.first == "filespace_uuid" ? platform::UuidKind::filespace
                          : field.first == "actor_uuid"  ? platform::UuidKind::principal
                                                         : platform::UuidKind::object;
        Require(uuid::ParseDurableEngineIdentityUuid(kind, field.second.encoded_value).ok(),
                "UUID field did not contain a typed durable engine UUID");
      }
      Require(!UnsafeValue(field.second.encoded_value),
              "unsafe value leaked in evidence retention result payload");
    }
  }
  for (const auto& diagnostic : result.diagnostics) {
    Require(!UnsafeValue(diagnostic.detail), "unsafe value leaked in diagnostic detail");
  }
}

api::EngineRequestContext Context() {
  api::EngineRequestContext context;
  context.security_context_present = true;
  context.database_uuid.canonical = Id(platform::UuidKind::database, 1);
  context.node_uuid.canonical = Id(platform::UuidKind::object, 2);
  context.session_uuid.canonical = Id(platform::UuidKind::object, 3);
  context.principal_uuid.canonical = Id(platform::UuidKind::principal, 4);
  context.transaction_uuid.canonical = Id(platform::UuidKind::transaction, 5);
  context.trace_tags = {
      "right:OBS_AGENT_EVIDENCE_READ",
      "right:OBS_AGENT_STATE_READ",
      "right:OBS_CONFIG_INSPECT",
      "right:SUPPORT_BUNDLE_EXPORT"};
  return context;
}

api::EngineRequestContext DurableContext(const TestDatabase& database,
                                         bool evidence_right = true) {
  auto context = Context();
  context.request_id = "pfar016a-evidence-retention-durable-agent-catalog";
  context.database_path = database.path.string();
  context.database_uuid.canonical = database.database_uuid;
  context.transaction_uuid.canonical = database.transaction_uuid;
  context.local_transaction_id = database.local_transaction_id;
  context.snapshot_visible_through_local_transaction_id =
      database.local_transaction_id;
  context.trace_tags = {"right:OBS_AGENT_STATE_READ",
                        "right:OBS_CONFIG_INSPECT",
                        "right:SUPPORT_BUNDLE_EXPORT"};
  if (evidence_right) {
    context.trace_tags.push_back("right:OBS_AGENT_EVIDENCE_READ");
  }
  return context;
}

api::EngineAgentEvidenceAuditRetentionRecord EvidenceRecord() {
  api::EngineAgentEvidenceAuditRetentionRecord record;
  record.source_surface = "engine_api";
  record.agent_type_id = "page_allocation_manager";
  record.agent_uuid = Id(platform::UuidKind::object, 10);
  record.filespace_uuid = Id(platform::UuidKind::filespace, 11);
  record.policy_uuid = Id(platform::UuidKind::object, 12);
  record.evidence_uuid = Id(platform::UuidKind::object, 13);
  record.action_uuid = Id(platform::UuidKind::object, 14);
  record.actor_uuid = Id(platform::UuidKind::principal, 15);
  record.evidence_kind = "agent_action_evidence";
  record.result_state = "success";
  record.diagnostic_code = "AGENT.PAGE_PREALLOCATION.COMPLETED";
  record.retention_class = "agent_evidence_400_day";
  record.retention_policy_ref = "agent.evidence.default_retention.v1";
  record.retention_deadline = "2027-06-25T00:00:00Z";
  record.policy_generation = "42";
  record.reason_text = "operator reason includes secret-token";
  record.policy_body = "policy allow page preallocation";
  record.physical_path = "/tmp/protected/runtime.sbdb";
  record.raw_principal = "raw-principal-token";
  record.raw_evidence_body = "protected_payload password=cleartext";
  record.support_bundle_payload = "support payload secret-token";
  record.legal_hold = true;
  record.actor_visible = true;
  record.policy_body_visible = true;
  return record;
}

agents::DurableAgentCatalogImage DurableEvidenceCatalog(bool tamper_bad = false) {
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
  action.idempotency_key = "pfar016a-retention-action";
  action.dry_run = false;
  action.inputs["metric_digest"] = "sha256:pfar016a-durable-metric";

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
  build.input_evidence_digest = "sha256:pfar016a-input";
  build.input_metric_digest = "sha256:pfar016a-durable-metric";
  build.policy_generation = instance.policy_generation;
  build.scope_uuids = {authority.scope_uuid};
  build.decision_payload = "durable retention evidence";
  build.result_state = "success";
  build.diagnostic_code = "AGENT.PAGE_PREALLOCATION.COMPLETED";
  build.retention_class = "agent_evidence_400_day";
  build.outcome_verification_evidence_uuid = Id(platform::UuidKind::object, 116);
  build.storage_linkage_digest = "sha256:pfar016a-storage-linkage";
  build.created_at_microseconds = 1916017000200ull;
  image.evidence.push_back(agents::BuildCommercialAgentEvidence(build));
  if (tamper_bad) {
    image.evidence.back().tamper_digest = "tampered";
  } else {
    Require(agents::ValidateCommercialAgentEvidence(image.evidence.back()).status.ok,
            "commercial evidence fixture did not validate");
  }
  return image;
}

void SeedDurableEvidenceCatalog(const api::EngineRequestContext& context,
                                bool tamper_bad = false) {
  api::AgentDurableCatalogStoreRequest seed;
  seed.context = context;
  seed.image = DurableEvidenceCatalog(tamper_bad);
  seed.evidence_uuid = Id(platform::UuidKind::object, 117);
  seed.production_live_path = true;
  seed.fsync_or_checkpoint_evidence = true;
  const auto persisted = api::PersistAgentDurableCatalogImage(seed);
  Require(persisted.ok,
          "retention durable catalog seed failed: " + persisted.diagnostic.detail);
}

void TestUserRedactionAndRetentionDecision() {
  api::EngineEvaluateAgentEvidenceRetentionRequest request;
  request.context = Context();
  request.records.push_back(EvidenceRecord());

  const auto result = api::EngineEvaluateAgentEvidenceRetention(request);
  Require(result.ok, "evidence retention API refused valid user-visible evidence");
  Require(result.evidence_before_success_enforced && result.retention_decision_recorded &&
              result.redaction_applied,
          "evidence retention API did not mark required enforcement flags");
  Require(HasRowField(result, "agent_uuid", request.records.front().agent_uuid),
          "generated agent UUID was not passed through");
  Require(HasRowField(result, "filespace_uuid", request.records.front().filespace_uuid),
          "generated filespace UUID was not passed through");
  Require(HasRowField(result, "actor_uuid", "<redacted:actor_uuid>"),
          "user-visible evidence did not redact actor UUID");
  Require(HasRowField(result, "reason_text", "<redacted:reason_text>"),
          "user-visible evidence did not redact reason text");
  Require(HasRowField(result, "policy_body", "<redacted:policy_body>"),
          "user-visible evidence did not redact policy body");
  Require(HasRowField(result, "physical_path", "<redacted>"),
          "user-visible evidence did not redact physical path");
  Require(HasRowField(result, "raw_evidence_body", "<redacted:evidence_body>"),
          "user-visible evidence did not redact raw evidence body");
  Require(HasRowField(result, "support_bundle_payload", "<redacted:support_bundle>"),
          "user-visible evidence did not redact support-bundle payload");
  Require(HasRowField(result, "hold_state", "legal_hold"),
          "legal hold was not represented in retention decision");
  Require(HasRowField(result, "purge_eligibility", "held"),
          "legal hold did not block purge eligibility");
  Require(HasEvidence(result,
                      "agent_evidence_retention_decision",
                      request.records.front().evidence_uuid),
          "retention decision evidence marker missing");
  RequireUuidFieldAuthority(result);

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

void TestAdminSafeVisibility() {
  api::EngineEvaluateAgentEvidenceRetentionRequest request;
  request.context = Context();
  request.admin_view = true;
  request.records.push_back(EvidenceRecord());
  request.records.front().reason_text = "approved by security operator";
  request.records.front().policy_body = "safe policy summary";
  request.records.front().legal_hold = false;
  request.records.front().retention_deadline_expired = true;

  const auto result = api::EngineEvaluateAgentEvidenceRetention(request);
  Require(result.ok, "evidence retention API refused valid admin evidence");
  Require(result.purge_eligible, "expired admin evidence was not marked purge eligible");
  Require(HasRowField(result, "actor_uuid", request.records.front().actor_uuid),
          "admin-visible evidence did not expose policy-permitted actor UUID");
  Require(HasRowField(result, "reason_text", "approved by security operator"),
          "admin-visible evidence did not expose safe reason text");
  Require(HasRowField(result, "policy_body", "safe policy summary"),
          "admin-visible evidence did not expose safe policy summary");
  Require(HasRowField(result, "physical_path", "<redacted>"),
          "admin-visible evidence leaked physical path");
  Require(HasRowField(result, "raw_principal", "<redacted:principal>"),
          "admin-visible evidence leaked raw principal");
  RequireUuidFieldAuthority(result);
}

void TestEvidenceBeforeSuccess() {
  api::EngineEvaluateAgentEvidenceRetentionRequest refused_request;
  refused_request.context = Context();
  refused_request.records.push_back(EvidenceRecord());
  refused_request.records.front().evidence_write_available = false;
  const auto refused = api::EngineEvaluateAgentEvidenceRetention(refused_request);
  Require(!refused.ok, "success was reported without retained evidence");
  Require(HasDiagnostic(refused, "AGENT.EVIDENCE.BEFORE_SUCCESS_REQUIRED"),
          "evidence-before-success refusal diagnostic drifted");
  Require(refused.result_shape.rows.empty(),
          "evidence-before-success refusal mutated result rows");

  api::EngineEvaluateAgentEvidenceRetentionRequest pending_request = refused_request;
  pending_request.records.front().evidence_recoverable = true;
  const auto pending = api::EngineEvaluateAgentEvidenceRetention(pending_request);
  Require(pending.ok, "recoverable missing evidence did not return pending_evidence");
  Require(pending.pending_evidence, "recoverable missing evidence flag was not set");
  Require(HasRowField(pending, "result_state", "pending_evidence"),
          "recoverable missing evidence did not expose pending_evidence state");
  Require(HasDiagnostic(pending, "AGENT.EVIDENCE.PENDING_EVIDENCE"),
          "pending evidence diagnostic drifted");
  RequireUuidFieldAuthority(pending);
}

void TestExactRefusals() {
  api::EngineEvaluateAgentEvidenceRetentionRequest missing_policy;
  missing_policy.context = Context();
  missing_policy.records.push_back(EvidenceRecord());
  missing_policy.records.front().retention_policy_ref.clear();
  const auto no_policy = api::EngineEvaluateAgentEvidenceRetention(missing_policy);
  Require(!no_policy.ok, "missing retention policy was accepted");
  Require(HasDiagnostic(no_policy, "AGENT.EVIDENCE.RETENTION_POLICY_REQUIRED"),
          "missing retention policy diagnostic drifted");
  Require(no_policy.result_shape.rows.empty(), "missing retention policy mutated rows");

  api::EngineEvaluateAgentEvidenceRetentionRequest invalid_policy = missing_policy;
  invalid_policy.records.front().retention_policy_ref = "agent.evidence.default_retention.v1";
  invalid_policy.records.front().retention_policy_valid = false;
  const auto bad_policy = api::EngineEvaluateAgentEvidenceRetention(invalid_policy);
  Require(!bad_policy.ok, "invalid retention policy was accepted");
  Require(HasDiagnostic(bad_policy, "AGENT.EVIDENCE.RETENTION_POLICY_INVALID"),
          "invalid retention policy diagnostic drifted");

  api::EngineEvaluateAgentEvidenceRetentionRequest malformed_uuid;
  malformed_uuid.context = Context();
  malformed_uuid.records.push_back(EvidenceRecord());
  malformed_uuid.records.front().policy_uuid = "not-a-uuid";
  const auto malformed = api::EngineEvaluateAgentEvidenceRetention(malformed_uuid);
  Require(!malformed.ok, "malformed UUID was accepted");
  Require(HasDiagnostic(malformed, "AGENT.EVIDENCE.INVALID_CATALOG_UUID"),
          "malformed UUID diagnostic drifted");

  api::EngineEvaluateAgentEvidenceRetentionRequest synthetic_uuid;
  synthetic_uuid.context = Context();
  synthetic_uuid.records.push_back(EvidenceRecord());
  synthetic_uuid.records.front().agent_uuid = "agent.page_allocation_manager.local";
  const auto synthetic = api::EngineEvaluateAgentEvidenceRetention(synthetic_uuid);
  Require(!synthetic.ok, "synthetic UUID reference was accepted");
  Require(HasDiagnostic(synthetic, "AGENT.EVIDENCE.INVALID_CATALOG_UUID"),
          "synthetic UUID diagnostic drifted");

  api::EngineEvaluateAgentEvidenceRetentionRequest cluster;
  cluster.context = Context();
  cluster.records.push_back(EvidenceRecord());
  cluster.records.front().cluster_scoped = true;
  const auto no_cluster = api::EngineEvaluateAgentEvidenceRetention(cluster);
  Require(!no_cluster.ok, "cluster-scoped evidence bypassed provider boundary");
  Require(HasDiagnostic(no_cluster, "SBLR.CLUSTER.SUPPORT_NOT_ENABLED"),
          "cluster no-provider diagnostic drifted");

  api::EngineEvaluateAgentEvidenceRetentionRequest missing_security;
  missing_security.context = Context();
  missing_security.context.security_context_present = false;
  missing_security.records.push_back(EvidenceRecord());
  const auto denied = api::EngineEvaluateAgentEvidenceRetention(missing_security);
  Require(!denied.ok, "missing security context was accepted");
  Require(HasDiagnostic(denied, "SB_ENGINE_API_SECURITY_CONTEXT_REQUIRED"),
          "missing security context diagnostic drifted");
}

void TestSupportBundleRetentionMetadata() {
  const auto evidence = EvidenceRecord();
  api::EnginePrepareSupportBundleRequest request;
  request.context = Context();
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
  source.payload_digest = "sha256:retention";
  source.retention_class = evidence.retention_class;
  source.retention_policy_ref = evidence.retention_policy_ref;
  source.retention_deadline = evidence.retention_deadline;
  source.legal_hold = true;
  source.physical_path = evidence.physical_path;
  source.unsafe_payload = evidence.support_bundle_payload;
  request.agent_runtime_evidence.push_back(source);

  const auto prepared = api::EnginePrepareSupportBundle(request);
  Require(prepared.ok, "support bundle refused retained evidence metadata");
  Require(HasRowField(prepared, "retention_class", evidence.retention_class),
          "support bundle omitted evidence retention class");
  Require(HasRowField(prepared, "retention_policy_ref", evidence.retention_policy_ref),
          "support bundle omitted evidence retention policy");
  Require(HasRowField(prepared, "legal_hold", "true"),
          "support bundle omitted evidence legal hold");
  Require(HasRowField(prepared, "physical_path", "<redacted>"),
          "support bundle leaked physical path");
  Require(HasRowField(prepared, "unsafe_payload", "<redacted>"),
          "support bundle leaked unsafe payload");
  RequireUuidFieldAuthority(prepared);
}

void TestProductionRetentionReadsDurableCatalog() {
  const auto database = CreateActiveDatabase(
      "scratchbird_pfar016a_retention_durable_catalog.sbdb",
      1916017000300ull);
  const auto context = DurableContext(database, true);
  SeedDurableEvidenceCatalog(context);

  api::EngineEvaluateAgentEvidenceRetentionRequest request;
  request.context = context;
  request.admin_view = false;
  request.sysarch_view = false;
  request.option_envelopes.push_back("agent_evidence_retention_production_live:true");
  request.option_envelopes.push_back("agent_durable_catalog_store_required:true");
  request.option_envelopes.push_back("allow_caller_agent_evidence_retention_records:false");

  const auto result = api::EngineEvaluateAgentEvidenceRetention(request);
  Require(result.ok, "production retention refused durable catalog evidence");
  Require(HasRowField(result, "source_surface", "durable_agent_catalog_store"),
          "production retention did not derive rows from durable catalog store");
  Require(HasRowField(result, "result_state", "success"),
          "production retention omitted durable evidence result");
  Require(HasRowField(result, "actor_uuid", Id(platform::UuidKind::principal, 113)),
          "production retention did not derive privileged actor visibility from rights");
  Require(HasEvidence(result, "agent_evidence_retention_durable_catalog"),
          "durable catalog retention evidence marker missing");
  Require(HasEvidence(result, "agent_evidence_retention_tamper_chain"),
          "tamper-chain retention evidence marker missing");
  RequireUuidFieldAuthority(result);

  api::EngineEvaluateAgentEvidenceRetentionRequest forged = request;
  forged.records.push_back(EvidenceRecord());
  forged.records.front().legal_hold = true;
  const auto forged_refused =
      api::EngineEvaluateAgentEvidenceRetention(forged);
  Require(!forged_refused.ok,
          "production retention accepted caller-forged retention state");
  Require(HasDiagnostic(forged_refused,
                        "agent_evidence_retention_caller_records_forbidden"),
          "caller-forged retention diagnostic drifted");

  CleanupDatabase(database.path);
}

void TestProductionRetentionRedactsWithoutEvidenceRight() {
  const auto database = CreateActiveDatabase(
      "scratchbird_pfar016a_retention_redaction.sbdb",
      1916017000400ull);
  const auto seed_context = DurableContext(database, true);
  SeedDurableEvidenceCatalog(seed_context);

  api::EngineEvaluateAgentEvidenceRetentionRequest request;
  request.context = DurableContext(database, false);
  request.admin_view = true;
  request.option_envelopes.push_back("agent_evidence_retention_production_live:true");
  request.option_envelopes.push_back("agent_durable_catalog_store_required:true");
  request.option_envelopes.push_back("allow_caller_agent_evidence_retention_records:false");

  const auto result = api::EngineEvaluateAgentEvidenceRetention(request);
  Require(result.ok, "production retention refused redacted durable view");
  Require(HasRowField(result, "actor_uuid", "<redacted:actor_uuid>"),
          "production retention trusted caller admin_view instead of rights");
  RequireUuidFieldAuthority(result);

  CleanupDatabase(database.path);
}

void TestProductionRetentionRejectsTamperMismatch() {
  const auto database = CreateActiveDatabase(
      "scratchbird_pfar016a_retention_tamper.sbdb",
      1916017000500ull);
  const auto context = DurableContext(database, true);
  SeedDurableEvidenceCatalog(context, true);

  api::EngineEvaluateAgentEvidenceRetentionRequest request;
  request.context = context;
  request.option_envelopes.push_back("agent_evidence_retention_production_live:true");
  request.option_envelopes.push_back("agent_durable_catalog_store_required:true");
  request.option_envelopes.push_back("allow_caller_agent_evidence_retention_records:false");

  const auto result = api::EngineEvaluateAgentEvidenceRetention(request);
  Require(!result.ok, "production retention accepted tampered evidence");
  Require(HasDiagnostic(result, "AGENT.EVIDENCE.TAMPER_INVALID"),
          "tamper mismatch diagnostic drifted");

  CleanupDatabase(database.path);
}

}  // namespace

int main() {
  TestUserRedactionAndRetentionDecision();
  TestAdminSafeVisibility();
  TestEvidenceBeforeSuccess();
  TestExactRefusals();
  TestSupportBundleRetentionMetadata();
  TestProductionRetentionReadsDurableCatalog();
  TestProductionRetentionRedactsWithoutEvidenceRight();
  TestProductionRetentionRejectsTamperMismatch();
  return EXIT_SUCCESS;
}
