#include "database_lifecycle_test_memory.hpp"
#include "../support/database_fixture_cleanup.hpp"
#include "../support/engine_evidence_fixture.hpp"
#include "../support/component_authorization_fixture.hpp"
#include "../support/durable_authorization_fixture.hpp"
#include "../support/metric_projection_fixture.hpp"
#include "behavior_support/api_behavior_record_codec.hpp"
#include "../../src/wire/public_result_packet.hpp"
// Copyright (c) 2026 ScratchBird Software Inc.
//
// This Source Code Form is subject to the terms of the Mozilla Public
// License, v. 2.0. If a copy of the MPL was not distributed with this
// file, You can obtain one at https://mozilla.org/MPL/2.0/.
//
// SPDX-License-Identifier: MPL-2.0

#include "agents/agent_durable_catalog_store_api.hpp"
#include "server_engine_bridge/legacy_diagnostic_projection.hpp"
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
#include <algorithm>
#include <filesystem>
#include <fstream>
#include <iostream>
#include <iterator>
#include <map>
#include <string>
#include <stdexcept>
#include <string_view>
#include <unistd.h>
#include <utility>
#include <vector>

namespace {

namespace api = scratchbird::engine::internal_api;
namespace rendering = scratchbird::server::legacy_rendering;
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
  platform::u64 resource_epoch = 0;
};

[[noreturn]] void Fail(std::string_view message) {
  throw std::runtime_error(std::string(message));
}

void Require(bool condition, std::string_view message) {
  if (!condition) { Fail(message); }
}

std::string IdentityBytes(const platform::Uuid& id) {
  return {reinterpret_cast<const char*>(id.bytes.data()), id.bytes.size()};
}
platform::Uuid NativeIdentity(std::string_view bytes) {
  Require(bytes.size() == 16, "UUID carrier must contain exactly 16 bytes");
  platform::Uuid id;
  std::copy_n(reinterpret_cast<const std::uint8_t*>(bytes.data()), 16, id.bytes.begin());
  Require(uuid::IsEngineIdentityUuid(id), "UUID carrier must be a valid engine identity");
  return id;
}

std::string IdentityJsonBytes(std::string_view bytes) {
  (void)NativeIdentity(bytes);
  std::string result = "[";
  for (const unsigned char byte : bytes) {
    if (result.size() > 1) result += ',';
    result += std::to_string(byte);
  }
  return result + ']';
}

std::string Id(platform::UuidKind kind, platform::u64 seed) {
  static std::map<std::pair<int, platform::u64>, std::string> generated_ids;
  const auto key = std::make_pair(static_cast<int>(kind), seed);
  const auto found = generated_ids.find(key);
  if (found != generated_ids.end()) { return found->second; }
  const auto generated = uuid::GenerateEngineIdentityV7(kind, 1915017000000ull + seed);
  Require(generated.ok(), "fixture UUID generation failed");
  const auto [inserted, _] =
      generated_ids.emplace(key, IdentityBytes(generated.value.value));
  return inserted->second;
}

std::filesystem::path MakeTempDir() {
  std::string tmpl = (std::filesystem::temp_directory_path() /
                      "sb_pfar016_obs.XXXXXX").string();
  std::vector<char> writable(tmpl.begin(), tmpl.end());
  writable.push_back('\0');
  char* made = ::mkdtemp(writable.data());
  Require(made != nullptr, "mkdtemp failed for PFAR-016 gate");
  return std::filesystem::path(made);
}

struct OwnedTempDir {
  OwnedTempDir() = default;
  OwnedTempDir(const OwnedTempDir&) = delete;
  OwnedTempDir& operator=(const OwnedTempDir&) = delete;
  std::filesystem::path path = MakeTempDir();
  void Cleanup() {
    std::filesystem::remove_all(path);
    path.clear();
  }
  ~OwnedTempDir() {
    if (path.empty()) return;
    std::error_code error;
    std::filesystem::remove_all(path, error);
    if (error) std::cerr << "agent observability fixture cleanup failed: " << error.message() << '\n';
  }
};

void CleanupDatabase(const std::filesystem::path& path) {
  scratchbird::tests::RemoveDatabaseFixtureArtifacts(path);
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
  // Canonical TEXT catalog columns require the real resource catalog, not the
  // deliberately epoch-free minimal-bootstrap fixture.
  create.resource_seed_pack_root = (std::filesystem::path(__FILE__).parent_path().parent_path().parent_path() /
      "resources/seed-packs/initial-resource-pack").string();
  create.require_resource_seed_pack = true;
  create.allow_overwrite = true;
  create.bootstrap_principal_name = "agent_observability_owner";
  create.bootstrap_credential_fingerprint =
      "local-password-pbkdf2-sha256:v1:iterations=600000:"
      "salt=0123456789abcdef0123456789abcdef:"
      "verifier=0358b60b6875c81e17d3e0ab67f8b785f49d4146547c79da401f21dc641c2c16";
  create.require_bootstrap_principal = true;
  create.allow_uncredentialed_bootstrap = false;
  const auto created = db::CreateDatabaseFile(create);
  Require(created.ok(), "durable support-bundle database creation failed: " +
      created.diagnostic.diagnostic_code + ":" + created.diagnostic.message_key);

  auto initial_inventory = db::LoadLocalTransactionInventoryFromDatabase(path.string());
  Require(initial_inventory.ok() && initial_inventory.inventory.publication_base.has_value(),
          "lifecycle-published transaction inventory unavailable");
  auto inventory = std::move(initial_inventory.inventory);
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
  database.database_uuid = IdentityBytes(database_uuid.value.value);
  database.transaction_uuid = IdentityBytes(transaction_uuid.value.value);
  database.local_transaction_id = begun.entry.identity.local_id.value;
  database.resource_epoch = created.state.resource_seed_catalog.resource_epoch;
  Require(database.resource_epoch != 0, "created resource catalog epoch unavailable");
  return database;
}

std::string ReadFile(const std::filesystem::path& path) {
  std::ifstream in(path, std::ios::binary);
  std::string text((std::istreambuf_iterator<char>(in)), std::istreambuf_iterator<char>());
  return text;
}

void VerifyAudit(const api::EngineRequestContext& context,
                 const std::vector<api::EngineAgentRuntimeEvidenceRecord>& expected) {
  namespace packet = scratchbird::wire::public_result;
  std::ifstream in(context.database_path + ".sb.api_events.v2", std::ios::binary);
  Require(in.good(), "agent audit was not actually written");
  for (const auto& source : expected) {
    api::ApiBehaviorRecord stored;
    Require(api::ReadApiBehaviorRecord(in, &stored), "agent audit cannot be reopened/decoded");
    Require(stored.operation_id == "observability.agent_runtime.collect" &&
                stored.target_database_uuid == context.database_uuid,
            "agent audit operation/owner mismatch");
    for (const auto& [name, value] :
         {std::pair{"agent_uuid", &source.agent_uuid}, {"filespace_uuid", &source.filespace_uuid},
          {"policy_uuid", &source.policy_uuid}, {"evidence_uuid", &source.evidence_uuid}}) {
      const auto field = packet::Find(stored.payload, name);
      Require(value->empty() ? !field : (field && field->kind == packet::Kind::uuid && field->value == *value),
              "audit identity lost native framing or was fabricated");
    }
    Require(!packet::Find(stored.payload, "physical_path") &&
                !packet::Find(stored.payload, "raw_principal") &&
                !packet::Find(stored.payload, "unsafe_payload"),
            "audit retained protected raw fields");
    const auto outcome = packet::Find(stored.payload, "result_state");
    Require(outcome && outcome->value == source.result_state, "audit changed the source outcome");
  }
  Require(in.peek() == std::char_traits<char>::eof(), "unexpected trailing audit records");
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
    if (field.first == name) {
      if (field.second.descriptor.canonical_type_name == "uuid") {
        if (field.second.state == api::EngineValueState::sql_null) {
          Require(field.second.encoded_value.empty() && field.second.binary_value.empty(),
                  "NULL UUID must not contain a payload");
          return {};
        }
        Require(field.second.encoded_value.empty() && field.second.binary_value.size() == 16,
                "UUID result must use binary16 only");
        return {reinterpret_cast<const char*>(field.second.binary_value.data()), 16};
      }
      return field.second.encoded_value;
    }
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
    if (evidence.evidence_kind == kind && (id.empty() || scratchbird::tests::EvidenceTextEquals(evidence.evidence_id, id))) {
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
      if (field.first.ends_with("_uuid")) {
        Require(field.second.descriptor.canonical_type_name == "uuid",
                "support/collector UUID field is not natively typed: " + field.first);
        const auto value = Field(row, field.first);
        if (!value.empty()) (void)NativeIdentity(value);
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
  context.trust_mode = api::EngineTrustMode::embedded_in_process;
  context.database_path = (temp_dir / "runtime.sbdb").string();
  context.database_uuid = NativeIdentity(Id(platform::UuidKind::database, 1));
  context.node_uuid = NativeIdentity(Id(platform::UuidKind::object, 2));
  context.session_uuid = NativeIdentity(Id(platform::UuidKind::object, 3));
  context.principal_uuid = NativeIdentity(Id(platform::UuidKind::principal, 4));
  context.transaction_uuid = NativeIdentity(Id(platform::UuidKind::transaction, 5));
  scratchbird::tests::MaterializeComponentAuthorization(
      context,
      {"OBS_METRICS_READ_FAMILY", "OBS_AGENT_EVIDENCE_READ",
       "OBS_AGENT_STATE_READ", "OBS_CONFIG_INSPECT"});
  return context;
}

api::EngineRequestContext DurableContext(const TestDatabase& database) {
  api::EngineRequestContext context;
  context.request_id = "pfar016-support-bundle-durable-agent-catalog";
  context.security_context_present = true;
  context.trust_mode = api::EngineTrustMode::embedded_in_process;
  context.database_path = database.path.string();
  context.resource_epoch = database.resource_epoch;
  context.database_uuid = NativeIdentity(database.database_uuid);
  context.transaction_uuid = NativeIdentity(database.transaction_uuid);
  context.local_transaction_id = database.local_transaction_id;
  context.snapshot_visible_through_local_transaction_id =
      database.local_transaction_id;
  context.node_uuid = NativeIdentity(Id(platform::UuidKind::object, 102));
  context.session_uuid = NativeIdentity(Id(platform::UuidKind::object, 103));
  const auto bootstrap = db::ReadDatabaseBootstrapSecurityCatalog(context.database_path);
  Require(bootstrap.ok() && bootstrap.state.present, "durable bootstrap owner unavailable");
  context.principal_uuid = bootstrap.state.principal_uuid.value;
  context.catalog_generation_id = 1;
  scratchbird::tests::MaterializeBootstrapFixtureAuthorization(context);
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
  scratchbird::tests::MetricProjectionFixture fixture(
      request.context.database_uuid, request.context.node_uuid, 114);
  fixture.Admit("sb_agent_actions_total", {{"component", "agent.runtime"},
      {"agent_type", "page_allocation_manager"}, {"action_class", "request_page_preallocation"},
      {"result", "success"}});
  fixture.Admit("sb_agent_page_allocation_requests_total", {{"component", "agent.page_allocation"},
      {"agent_type", "page_allocation_manager"}, {"filespace_uuid", NativeIdentity(Id(platform::UuidKind::filespace, 11))},
      {"page_family", "data"}, {"request_class", "request_page_preallocation"}, {"result", "success"}});

  const auto result = api::EngineCollectAgentRuntimeObservability(request);
  for (const auto& diagnostic : result.diagnostics) {
    if (diagnostic.error) std::cerr << diagnostic.code << ':' << diagnostic.detail << '\n';
  }
  Require(result.ok, "agent observability collector refused valid evidence");
  VerifyAudit(request.context, request.records);
  fixture.ExpectProduced(2);
  fixture.Seal();
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

  rendering::EngineParserPackageRenderOptions render;
  render.parser_package_uuid = NativeIdentity(Id(platform::UuidKind::object, 20));
  render.parser_package_version = "sbsql.v3";
  render.client_dialect = "sbsql";
  render.correlation_uuid = NativeIdentity(Id(platform::UuidKind::object, 21));
  render.request_uuid = NativeIdentity(Id(platform::UuidKind::object, 22));
  render.session_uuid = request.context.session_uuid;
  render.database_uuid = request.context.database_uuid;
  render.transaction_uuid = request.context.transaction_uuid;
  const auto envelope = scratchbird::server_engine_bridge::RenderLegacyEngineResult(result, std::move(render));
  std::vector<std::string> errors;
  Require(rendering::ValidateLegacyRenderedProjectionStructure(envelope, &errors),
          "parser/client rendered envelope failed validation");
  Require(!envelope.parser_finality_authority && !envelope.reference_finality_authority,
          "parser/client envelope claimed finality authority");
  for (const auto& row : envelope.rows) {
    for (const auto& field : row.fields) {
      Require(!UnsafeValue(field.encoded_value), "parser/client envelope leaked unsafe value");
    }
  }
  const auto audit_before = ReadFile(request.context.database_path + ".sb.api_events.v2");
  auto malformed_batch = request;
  malformed_batch.records.push_back(EvidenceRecord());
  malformed_batch.records.back().policy_uuid = uuid::UuidToString(NativeIdentity(Id(platform::UuidKind::object, 12)));
  const auto malformed_result = api::EngineCollectAgentRuntimeObservability(malformed_batch);
  Require(!malformed_result.ok && !malformed_result.audit_recorded &&
              !malformed_result.metrics_recorded && malformed_result.result_shape.rows.empty() &&
              HasDiagnostic(malformed_result, "AGENT.OBSERVABILITY.INVALID_CATALOG_UUID") &&
              ReadFile(request.context.database_path + ".sb.api_events.v2") == audit_before,
          "malformed trailing record allowed leading metric/audit effects");
  auto no_audit_sink = request;
  no_audit_sink.context.database_path = (temp_dir / "missing-parent" / "runtime.sbdb").string();
  const auto audit_refused = api::EngineCollectAgentRuntimeObservability(no_audit_sink);
  Require(!audit_refused.ok && !audit_refused.audit_recorded && !audit_refused.metrics_recorded &&
              !audit_refused.support_bundle_ready && audit_refused.result_shape.rows.empty() &&
              HasDiagnostic(audit_refused, "database_path_unwritable"),
          "unwritable audit sink returned success or emitted metrics");
  auto foreign_node = request;
  foreign_node.context.node_uuid = NativeIdentity(Id(platform::UuidKind::object, 900));
  const auto foreign_result = api::EngineCollectAgentRuntimeObservability(foreign_node);
  Require(!foreign_result.ok && HasDiagnostic(foreign_result, "METRIC.OBSERVATION_SOURCE_UNAVAILABLE") &&
              ReadFile(request.context.database_path + ".sb.api_events.v2") == audit_before,
          "foreign node admitted observations or audit");
  fixture.VerifyReadOnly();
  fixture.VerifyAndDrain();
}

void TestPartialMetricReceipt(const std::filesystem::path& temp_dir) {
  api::EngineCollectAgentRuntimeObservabilityRequest request;
  request.context = Context(temp_dir);
  request.records.push_back(EvidenceRecord());
  scratchbird::tests::MetricProjectionFixture fixture(
      request.context.database_uuid, request.context.node_uuid, 116);
  fixture.Admit("sb_agent_actions_total", {{"component", "agent.runtime"},
      {"agent_type", "page_allocation_manager"}, {"action_class", "request_page_preallocation"},
      {"result", "success"}});
  // The second descriptor is intentionally not admitted. The first accepted
  // counter and actual audit append must survive the refusal in the receipt.
  const auto result = api::EngineCollectAgentRuntimeObservability(request);
  Require(!result.ok && result.audit_recorded && !result.metrics_recorded &&
              !result.support_bundle_ready && result.result_shape.rows.empty() &&
              HasEvidence(result, "agent_metric_observation_admitted", "sb_agent_actions_total") &&
              !HasEvidence(result, "agent_metric_observation_admitted", "sb_agent_page_allocation_requests_total") &&
              HasEvidence(result, "agent_audit_completion", "appended_not_durable_finality"),
          "partial collector failure lost admitted effects or claimed complete success");
  VerifyAudit(request.context, request.records);
  fixture.ExpectProduced(1);
  fixture.Seal();
  fixture.VerifyAndDrain();
}

void TestSupportBundleAndManagerCollectors(const std::filesystem::path& temp_dir) {
  const auto evidence = EvidenceRecord();
  api::EnginePrepareSupportBundleRequest request;
  request.context = Context(temp_dir);
  request.option_envelopes.push_back("engine_authorized_support_export:true");
  api::EngineSupportBundleAgentEvidenceSource source;
  source.agent_type_id = evidence.agent_type_id;
  source.agent_uuid = NativeIdentity(evidence.agent_uuid);
  source.filespace_uuid = NativeIdentity(evidence.filespace_uuid);
  source.policy_uuid = NativeIdentity(evidence.policy_uuid);
  source.evidence_uuid = NativeIdentity(evidence.evidence_uuid);
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

  auto optional_references = request;
  optional_references.agent_runtime_evidence.front().filespace_uuid = {};
  optional_references.agent_runtime_evidence.front().policy_uuid = {};
  optional_references.agent_runtime_evidence.front().evidence_uuid = {};
  const auto absent_references = api::EnginePrepareSupportBundle(optional_references);
  Require(absent_references.ok, "support bundle rejected absent optional identity references");
  RequireNoUnsafeResultPayload(absent_references);

  api::EnginePrepareSupportBundleRequest invalid = request;
  invalid.agent_runtime_evidence.front().agent_uuid = {};
  const auto refused = api::EnginePrepareSupportBundle(invalid);
  Require(!refused.ok, "support bundle API accepted nil UUID reference");
  Require(HasDiagnostic(refused, "AGENT.OBSERVABILITY.INVALID_CATALOG_UUID"),
          "support bundle API did not emit exact synthetic UUID diagnostic");

  api::EnginePrepareSupportBundleRequest malformed = request;
  malformed.agent_runtime_evidence.front().evidence_uuid = NativeIdentity(Id(platform::UuidKind::object, 999));
  malformed.agent_runtime_evidence.front().evidence_uuid.bytes[6] = 0x60;
  const auto malformed_refused = api::EnginePrepareSupportBundle(malformed);
  Require(!malformed_refused.ok, "support bundle API accepted non-v7 UUID identity");
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
      "{\"agent_uuid\":" + IdentityJsonBytes(evidence.agent_uuid) +
      ",\"unsafe\":\"password=cleartext token=secret-token\",\"path\":\"/tmp/protected/runtime.sbdb\"}";
  std::string error_code;
  Require(manager::GenerateManagerSupportBundle(config, inputs, &error_code),
          "manager support bundle generation failed");
  const auto agent_bundle = ReadFile(inputs.bundle_dir / "agent-observability.json");
  Require(Contains(agent_bundle, IdentityJsonBytes(evidence.agent_uuid)),
          "manager support bundle omitted generated agent UUID");
  Require(!UnsafeValue(agent_bundle), "manager support bundle leaked unsafe agent evidence");
  const auto manifest = ReadFile(inputs.bundle_dir / "manifest.txt");
  Require(Contains(manifest, "local_path_policy=redacted"),
          "manager support bundle did not declare path redaction");
}

void TestProductionSupportBundleRequiresDurableCatalog(const api::EngineRequestContext& context);

void TestProductionSupportBundleReadsDurableAgentCatalog(
    const std::filesystem::path& temp_dir) {
  const auto database = CreateActiveDatabase(temp_dir);
  const auto context = DurableContext(database);
  // Prove absence before publication on the same actual node, then prove
  // presence after publication. No second expensive seed bootstrap or queue
  // owner reset is needed, and the missing-catalog assertions are retained.
  TestProductionSupportBundleRequiresDurableCatalog(context);
  SeedDurableCatalog(context);
  scratchbird::tests::MetricProjectionFixture fixture(context.database_uuid, context.node_uuid, 115);
  const auto reopened = api::LoadAgentDurableCatalogImage(context, true);
  Require(reopened.ok, "seeded agent catalog could not be reopened");
  const auto& image = reopened.image;
  for (const auto& record : image.evidence)
    fixture.Admit("sb_agent_actions_total", {{"component", "agent.runtime"},
        {"agent_type", record.agent_type_id}, {"action_class", record.evidence_kind},
        {"result", record.result_state}});
  for (const auto& action : image.actions)
    fixture.Admit("sb_agent_actions_total", {{"component", "agent.runtime"},
        {"agent_type", "page_allocation_manager"}, {"action_class", action.operation_id}, {"result", "success"}});

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
  Require(HasRowField(prepared, "owner_identities_redacted", "true") &&
              HasRowField(prepared, "owner_redacted", "true"),
          "durable bundle did not identify its typed UUID redaction");
  RequireNoUnsafeResultPayload(prepared);

  api::EngineCollectAgentRuntimeObservabilityRequest observability;
  observability.context = context;
  observability.option_envelopes.push_back("agent_observability_production_live:true");
  observability.option_envelopes.push_back("agent_durable_catalog_store_required:true");
  observability.option_envelopes.push_back("allow_caller_agent_runtime_evidence:false");
  const auto collected = api::EngineCollectAgentRuntimeObservability(observability);
  for (const auto& diagnostic : collected.diagnostics)
    if (diagnostic.error) std::cerr << diagnostic.code << ':' << diagnostic.detail << '\n';
  Require(collected.ok,
          "production agent observability refused durable catalog records");
  std::vector<api::EngineAgentRuntimeEvidenceRecord> audited;
  for (const auto& source : image.evidence) {
    api::EngineAgentRuntimeEvidenceRecord record;
    record.agent_uuid = source.instance_uuid;
    record.evidence_uuid = source.evidence_uuid;
    record.result_state = source.result_state;
    audited.push_back(std::move(record));
  }
  for (const auto& source : image.actions) {
    api::EngineAgentRuntimeEvidenceRecord record;
    record.agent_uuid = source.instance_uuid;
    record.evidence_uuid = source.evidence_uuid;
    record.result_state = "success";
    audited.push_back(std::move(record));
  }
  VerifyAudit(context, audited);
  fixture.ExpectProduced(image.evidence.size() + image.actions.size());
  fixture.Seal();
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
  source.agent_uuid = NativeIdentity(evidence.agent_uuid);
  source.filespace_uuid = NativeIdentity(evidence.filespace_uuid);
  source.policy_uuid = NativeIdentity(evidence.policy_uuid);
  source.evidence_uuid = NativeIdentity(evidence.evidence_uuid);
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

  fixture.VerifyReadOnly();
  fixture.VerifyAndDrain();
  CleanupDatabase(database.path);
}

void TestProductionSupportBundleRequiresDurableCatalog(
    const api::EngineRequestContext& context) {
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
  Require(!refused.ok, "agent observability collector accepted nil UUID reference");
  Require(HasDiagnostic(refused, "AGENT.OBSERVABILITY.INVALID_CATALOG_UUID"),
          "agent observability collector synthetic UUID diagnostic drifted");

  api::EngineCollectAgentRuntimeObservabilityRequest malformed_uuid;
  malformed_uuid.context = Context(temp_dir);
  malformed_uuid.records.push_back(EvidenceRecord());
  malformed_uuid.records.front().filespace_uuid = "not-a-uuid";
  const auto malformed = api::EngineCollectAgentRuntimeObservability(malformed_uuid);
  Require(!malformed.ok, "agent observability collector accepted non-v7 UUID identity");
  Require(HasDiagnostic(malformed, "AGENT.OBSERVABILITY.INVALID_CATALOG_UUID"),
          "agent observability collector malformed UUID diagnostic drifted");
}

}  // namespace

static int Run(std::string_view scenario) {
  scratchbird::tests::database_lifecycle::ConfigureLifecycleMemoryFixture("agent-observability-conformance");
  OwnedTempDir owned;
  const auto& temp_dir = owned.path;
  if (scenario == "component") {
    TestEngineCollectorAndMetrics(temp_dir);
    TestSupportBundleAndManagerCollectors(temp_dir);
    TestListenerCollectors();
    TestNegativeSecurityAndUuid(temp_dir);
  } else if (scenario == "durable") {
    TestProductionSupportBundleReadsDurableAgentCatalog(temp_dir);
  } else if (scenario == "partial") {
    TestPartialMetricReceipt(temp_dir);
  } else Fail("expected component, durable or partial scenario");
  owned.Cleanup();
  return EXIT_SUCCESS;
}

int main(int argc, char** argv) {
  try {
    Require(argc == 2, "one explicit scenario is required; CTest runs all scenarios");
    return Run(argv[1]);
  }
  catch (const std::exception& error) {
    std::cerr << error.what() << '\n';
    return EXIT_FAILURE;
  }
}
