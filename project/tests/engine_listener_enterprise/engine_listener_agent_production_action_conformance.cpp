// Copyright (c) 2026 ScratchBird Software Inc.
//
// This Source Code Form is subject to the terms of the Mozilla Public
// License, v. 2.0. If a copy of the MPL was not distributed with this
// file, You can obtain one at https://mozilla.org/MPL/2.0/.
//
// SPDX-License-Identifier: MPL-2.0

#include "agents/agent_action_dispatch_store_api.hpp"
#include "agents/agent_durable_catalog_store_api.hpp"
#include "agents/agent_support_bundle_triage_route_api.hpp"
#include "transaction/transaction_api.hpp"

#include "agent_package_provenance.hpp"
#include "agent_production_classification.hpp"
#include "agent_replay_quarantine.hpp"
#include "agents/support_bundle_triage_agent.hpp"
#include "database_lifecycle.hpp"
#include "local_transaction_store.hpp"
#include "memory.hpp"
#include "transaction_inventory.hpp"
#include "uuid.hpp"

#include <chrono>
#include <cstdlib>
#include <filesystem>
#include <fstream>
#include <iostream>
#include <set>
#include <string>
#include <utility>
#include <vector>

namespace {

namespace agents = scratchbird::core::agents;
namespace api = scratchbird::engine::internal_api;
namespace db = scratchbird::storage::database;
namespace impl = scratchbird::core::agents::implemented_agents;
namespace memory = scratchbird::core::memory;
namespace mga = scratchbird::transaction::mga;
namespace platform = scratchbird::core::platform;
namespace uuid = scratchbird::core::uuid;

[[noreturn]] void Fail(const std::string& message) {
  std::cerr << message << '\n';
  std::exit(EXIT_FAILURE);
}

void Require(bool condition, const std::string& message) {
  if (!condition) { Fail(message); }
}

std::uint64_t CurrentTestBaseMillis() {
  const auto now = std::chrono::system_clock::now().time_since_epoch();
  const auto millis =
      std::chrono::duration_cast<std::chrono::milliseconds>(now).count();
  const auto current = static_cast<std::uint64_t>(millis);
  return current > 10000 ? current - 10000 : current;
}

memory::AllocationPolicy MemoryPolicy() {
  auto policy = memory::DefaultLocalEngineMemoryPolicy();
  policy.policy_name = "engine_listener_agent_production_action_conformance";
  policy.hard_limit_bytes = 16 * 1024 * 1024;
  policy.soft_limit_bytes = 16 * 1024 * 1024;
  policy.per_context_limit_bytes = 16 * 1024 * 1024;
  policy.page_buffer_pool_limit_bytes = 16 * 1024 * 1024;
  policy.track_allocations = true;
  policy.zero_memory_on_release = true;
  return policy;
}

void ConfigureMemoryFixture() {
  const auto configured = memory::ConfigureDefaultMemoryManagerForFixture(
      MemoryPolicy(),
      "engine_listener_agent_production_action_conformance");
  Require(configured.ok(), "ELER-042 memory fixture failed");
  Require(configured.fixture_mode, "ELER-042 memory fixture mode missing");
}

struct ProofRow {
  std::string feature_id;
  std::string scenario;
  std::string result;
  std::string evidence;
};

std::vector<ProofRow> g_rows;

void AddRow(std::string scenario, std::string result, std::string evidence) {
  g_rows.push_back({"ELER-042", std::move(scenario), std::move(result),
                    std::move(evidence)});
}

std::string CsvEscape(const std::string& value) {
  bool quote = false;
  for (const char ch : value) {
    quote = quote || ch == '"' || ch == ',' || ch == '\n' || ch == '\r';
  }
  if (!quote) { return value; }
  std::string out = "\"";
  for (const char ch : value) {
    if (ch == '"') { out += "\"\""; }
    else { out += ch; }
  }
  out += '"';
  return out;
}

void WriteMatrix(const std::filesystem::path& output_path) {
  if (output_path.empty()) { return; }
  std::filesystem::create_directories(output_path.parent_path());
  std::ofstream out(output_path);
  Require(out.good(), "ELER-042 proof matrix could not be opened");
  out << "feature_id,scenario,result,evidence\n";
  for (const auto& row : g_rows) {
    out << CsvEscape(row.feature_id) << ','
        << CsvEscape(row.scenario) << ','
        << CsvEscape(row.result) << ','
        << CsvEscape(row.evidence) << '\n';
  }
}

struct TestDatabase {
  std::filesystem::path path;
  std::string database_uuid;
};

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

TestDatabase CreateDatabase(const char* basename, std::uint64_t timestamp_base) {
  (void)timestamp_base;
  const std::uint64_t current_base = CurrentTestBaseMillis();
  const std::filesystem::path path =
      std::filesystem::temp_directory_path() / basename;
  CleanupDatabase(path);

  const auto database_uuid =
      uuid::GenerateEngineIdentityV7(platform::UuidKind::database,
                                     current_base + 1);
  const auto filespace_uuid =
      uuid::GenerateEngineIdentityV7(platform::UuidKind::filespace,
                                     current_base + 2);
  Require(database_uuid.ok(), "ELER-042 database UUID generation failed");
  Require(filespace_uuid.ok(), "ELER-042 filespace UUID generation failed");

  db::DatabaseCreateConfig create;
  create.path = path.string();
  create.database_uuid = database_uuid.value;
  create.filespace_uuid = filespace_uuid.value;
  create.page_size = 16384;
  create.creation_unix_epoch_millis = current_base + 3;
  create.allow_minimal_resource_bootstrap = true;
  create.require_resource_seed_pack = false;
  create.allow_overwrite = true;
  const auto created = db::CreateDatabaseFile(create);
  Require(created.ok(), "ELER-042 database creation failed: " +
                            created.diagnostic.diagnostic_code);

  TestDatabase result;
  result.path = path;
  result.database_uuid = uuid::UuidToString(database_uuid.value.value);
  return result;
}

api::EngineRequestContext BootstrapContext(const TestDatabase& database,
                                           std::uint64_t timestamp_base) {
  (void)timestamp_base;
  const std::uint64_t current_base = CurrentTestBaseMillis();
  auto inventory = mga::MakeEmptyLocalTransactionInventory();
  const auto transaction_uuid =
      uuid::GenerateEngineIdentityV7(platform::UuidKind::transaction,
                                     current_base + 10);
  Require(transaction_uuid.ok(), "ELER-042 bootstrap transaction UUID failed");
  auto begun = mga::BeginLocalTransaction(std::move(inventory),
                                          transaction_uuid.value,
                                          current_base + 11);
  Require(begun.ok(), "ELER-042 bootstrap transaction begin failed");
  Require(db::PersistLocalTransactionInventoryToDatabase(database.path.string(),
                                                         begun.inventory)
              .ok(),
          "ELER-042 bootstrap transaction inventory persist failed");

  api::EngineRequestContext context;
  context.request_id = "eler042-bootstrap";
  context.database_path = database.path.string();
  context.database_uuid.canonical = database.database_uuid;
  context.transaction_uuid.canonical =
      uuid::UuidToString(transaction_uuid.value.value);
  context.local_transaction_id = begun.entry.identity.local_id.value;
  context.snapshot_visible_through_local_transaction_id =
      context.local_transaction_id;
  context.security_context_present = true;
  context.principal_uuid.canonical =
      agents::DeterministicAgentRuntimePrincipalUuidFromKey("eler042-principal");
  context.session_uuid.canonical =
      agents::DeterministicAgentRuntimeObjectUuidFromKey("eler042-session");
  context.catalog_generation_id = 1;
  context.security_epoch = 42;
  context.resource_epoch = 1;
  context.name_resolution_epoch = 1;
  return context;
}

api::EngineRequestContext BeginTransaction(const TestDatabase& database,
                                           std::string request_id) {
  api::EngineBeginTransactionRequest begin;
  begin.context.database_path = database.path.string();
  begin.context.database_uuid.canonical = database.database_uuid;
  begin.context.request_id = std::move(request_id);
  begin.context.security_context_present = true;
  begin.context.principal_uuid.canonical =
      agents::DeterministicAgentRuntimePrincipalUuidFromKey("eler042-principal");
  begin.context.session_uuid.canonical =
      agents::DeterministicAgentRuntimeObjectUuidFromKey("eler042-session");
  begin.context.catalog_generation_id = 1;
  begin.context.security_epoch = 42;
  begin.context.resource_epoch = 1;
  begin.context.name_resolution_epoch = 1;
  begin.transaction_policy_profile.encoded_profiles.push_back("fail_closed:true");
  begin.transaction_policy_profile.encoded_profiles.push_back(
      "transaction_read_only:false");
  begin.transaction_policy_profile.encoded_profiles.push_back(
      "transaction_read_mode:read_write");
  const auto begun = api::EngineBeginTransaction(begin);
  std::string detail;
  if (!begun.diagnostics.empty()) {
    detail = begun.diagnostics.front().code + ":" +
             begun.diagnostics.front().detail;
  }
  Require(begun.ok && begun.local_transaction_id != 0,
          "ELER-042 engine transaction begin failed: " + detail);
  auto context = begin.context;
  context.transaction_uuid = begun.transaction_uuid;
  context.local_transaction_id = begun.local_transaction_id;
  context.snapshot_visible_through_local_transaction_id =
      begun.snapshot_visible_through_local_transaction_id;
  return context;
}

void Commit(api::EngineRequestContext context) {
  api::EngineCommitTransactionRequest commit;
  commit.context = std::move(context);
  const auto committed = api::EngineCommitTransaction(commit);
  std::string detail;
  if (!committed.diagnostics.empty()) {
    detail = committed.diagnostics.front().code + ":" +
             committed.diagnostics.front().detail;
  }
  Require(committed.ok, "ELER-042 engine transaction commit failed: " + detail);
}

std::string DigestHex(char value) {
  return "sha256:" + std::string(64, value);
}

agents::AgentPackageProvenanceBundle PageProviderPackage() {
  agents::AgentPackageProvenanceBundle bundle;
  bundle.policy.policy_id = "eler042-agent-package-policy";
  bundle.policy.policy_generation = 1;
  bundle.policy.allowed_signer_identities = {"scratchbird-release-signing"};
  bundle.policy.allowed_signer_key_ids = {"release-key-v1"};
  bundle.policy.allowed_sandbox_profiles = {"agent-bounded-local"};
  bundle.policy.minimum_versions = {
      {agents::AgentPackageSubjectKind::plugin, "", 100},
      {agents::AgentPackageSubjectKind::actuator_provider, "", 100},
      {agents::AgentPackageSubjectKind::agent_binary, "", 100}};
  const std::vector<std::pair<agents::AgentPackageSubjectKind, std::string>>
      subjects = {
          {agents::AgentPackageSubjectKind::plugin,
           "page_allocation_manager.plugin"},
          {agents::AgentPackageSubjectKind::actuator_provider,
           "page_manager.preallocate_page_family.provider"},
          {agents::AgentPackageSubjectKind::agent_binary,
           "page_allocation_manager.agent_binary"}};
  int index = 0;
  for (const auto& [kind, subject_id] : subjects) {
    agents::AgentPackageProvenanceRecord record;
    record.subject_kind = kind;
    record.subject_id = subject_id;
    record.package_uuid =
        agents::DeterministicAgentRuntimeObjectUuidFromKey(
            "eler042-package-" + std::to_string(index));
    record.package_version = "1.0." + std::to_string(index);
    record.package_version_ordinal = 100;
    record.package_digest = DigestHex(static_cast<char>('1' + index));
    record.signature_algorithm = "ed25519";
    record.signature_digest = DigestHex(static_cast<char>('4' + index));
    record.signature_verified = true;
    record.signature_evidence_uuid =
        agents::DeterministicAgentRuntimeObjectUuidFromKey(
            "eler042-signature-" + std::to_string(index));
    record.signer_identity = "scratchbird-release-signing";
    record.signer_key_id = "release-key-v1";
    record.signer_policy_id = bundle.policy.policy_id;
    record.signer_allowed_by_policy = true;
    record.sbom_present = true;
    record.sbom_format = "spdx-2.3";
    record.sbom_digest = DigestHex(static_cast<char>('7' + index));
    record.sbom_evidence_uuid =
        agents::DeterministicAgentRuntimeObjectUuidFromKey(
            "eler042-sbom-" + std::to_string(index));
    record.sandbox_profile_id = "agent-bounded-local";
    record.sandbox_profile_digest = DigestHex(static_cast<char>('a' + index));
    record.sandbox_evidence_uuid =
        agents::DeterministicAgentRuntimeObjectUuidFromKey(
            "eler042-sandbox-" + std::to_string(index));
    record.revocation_status =
        agents::AgentPackageRevocationStatus::not_revoked;
    record.revocation_checked = true;
    record.revocation_generation = 10 + index;
    record.revocation_evidence_uuid =
        agents::DeterministicAgentRuntimeObjectUuidFromKey(
            "eler042-revocation-" + std::to_string(index));
    record.production_package = true;
    record.provenance_evidence_uuid =
        agents::DeterministicAgentRuntimeObjectUuidFromKey(
            "eler042-provenance-" + std::to_string(index));
    agents::FinalizeAgentPackageProvenanceDigest(&record);
    bundle.records.push_back(std::move(record));
    ++index;
  }
  return bundle;
}

agents::AgentActionAuthorityProvenance SealedAuthority() {
  agents::AgentActionAuthorityProvenance authority;
  authority.source = agents::AgentActionAuthoritySource::sealed_internal_bootstrap;
  authority.principal_uuid =
      agents::DeterministicAgentRuntimePrincipalUuidFromKey("eler042-agent");
  authority.scope_uuid =
      agents::DeterministicAgentRuntimeDatabaseUuidFromKey("eler042-scope");
  authority.provenance_evidence_uuid =
      agents::DeterministicAgentRuntimeObjectUuidFromKey(
          "eler042-authority-evidence");
  authority.rights = {"OBS_AGENT_CONTROL"};
  authority.sealed_bootstrap_authority = true;
  return authority;
}

agents::AgentRuntimeContext MetricContext(
    const agents::AgentActionAuthorityProvenance& authority) {
  agents::AgentRuntimeContext context;
  context.security_context_present = true;
  context.private_features_available = true;
  context.standalone_edition = true;
  context.database_uuid = authority.scope_uuid;
  context.principal_uuid = authority.principal_uuid;
  context.wall_now_microseconds = 1761000000001000ull;
  return context;
}

std::vector<agents::AgentObservedMetricSnapshot> ObservedMetricSnapshots(
    const agents::AgentActionAuthorityProvenance& authority) {
  const auto descriptor = agents::FindAgentType("page_allocation_manager");
  Require(descriptor.has_value(), "ELER-042 page allocation descriptor missing");
  std::vector<agents::AgentObservedMetricSnapshot> snapshots;
  int ordinal = 0;
  for (const auto& dependency : descriptor->metric_dependencies) {
    agents::AgentObservedMetricSnapshot snapshot;
    snapshot.metric_family = dependency.metric_family;
    snapshot.namespace_path = dependency.namespace_prefix + ".observed";
    snapshot.generation = 420 + ordinal;
    snapshot.observed_wall_microseconds = 1761000000000000ull;
    snapshot.scope_uuid = authority.scope_uuid;
    snapshot.digest = "sha256:eler042-" + dependency.metric_family;
    snapshot.source_quality = agents::AgentMetricSourceQuality::trusted;
    snapshot.present = true;
    snapshot.trusted = true;
    snapshot.schema_compatible = true;
    snapshot.trust_provenance = "engine_metric_registry";
    snapshot.evidence_uuid =
        agents::DeterministicAgentRuntimeObjectUuidFromKey(
            "eler042-metric-evidence-" + std::to_string(ordinal));
    snapshot.snapshot_id = "eler042-metric-snapshot-" + std::to_string(ordinal);
    snapshot.value_digest = snapshot.digest;
    snapshot.schema_digest = "schema:" + snapshot.metric_family + ":" +
                             std::to_string(snapshot.generation);
    snapshot.attestation_verified = true;
    snapshot.redacted = true;
    snapshot.protected_material_present = false;
    snapshot.provenance_record = snapshot.trust_provenance + ":" +
                                 snapshot.metric_family;
    snapshot.authority_claims = {"metric_evidence"};

    auto source_a = snapshot;
    source_a.source_id = "source-a";
    source_a.source_sequence = snapshot.generation * 2 + 1;
    source_a.previous_source_sequence = source_a.source_sequence - 1;
    source_a.attestation_key_id = "metric-key:" + source_a.source_id;
    source_a.attestation_digest =
        "attestation:" + source_a.metric_family + ":" + source_a.source_id;
    source_a.evidence_uuid += ":source-a";
    source_a.snapshot_id += ":source-a";
    snapshots.push_back(std::move(source_a));

    auto source_b = snapshot;
    source_b.source_id = "source-b";
    source_b.source_sequence = snapshot.generation * 2 + 2;
    source_b.previous_source_sequence = source_b.source_sequence - 1;
    source_b.attestation_key_id = "metric-key:" + source_b.source_id;
    source_b.attestation_digest =
        "attestation:" + source_b.metric_family + ":" + source_b.source_id;
    source_b.evidence_uuid += ":source-b";
    source_b.snapshot_id += ":source-b";
    snapshots.push_back(std::move(source_b));
    ++ordinal;
  }
  return snapshots;
}

std::string ObservedMetricDigestForAction(
    const agents::AgentActionAuthorityProvenance& authority) {
  const auto descriptor = agents::FindAgentType("page_allocation_manager");
  Require(descriptor.has_value(), "ELER-042 page allocation descriptor missing");
  agents::AgentMetricSnapshotEvaluationOptions options;
  options.expected_scope_uuid = authority.scope_uuid;
  const auto evaluation = agents::EvaluateAgentObservedMetricSnapshots(
      *descriptor,
      MetricContext(authority),
      ObservedMetricSnapshots(authority),
      options);
  Require(evaluation.accepted,
          "ELER-042 metric digest helper rejected observed snapshots: " +
              evaluation.status.diagnostic_code);
  return evaluation.input_digest;
}

agents::AgentActionRequest PageAction(std::string uuid,
                                      std::string idempotency_key) {
  const auto authority = SealedAuthority();
  agents::AgentActionRequest action;
  action.action_uuid = std::move(uuid);
  action.agent_type_id = "page_allocation_manager";
  action.instance_uuid =
      agents::DeterministicAgentRuntimeObjectUuidFromKey(
          "eler042-page-agent-instance");
  action.actuator_id = "page_manager";
  action.operation_id = "preallocate_page_family";
  action.idempotency_key = std::move(idempotency_key);
  action.dry_run = false;
  action.inputs["evidence_uuid"] =
      agents::DeterministicAgentRuntimeObjectUuidFromKey(
          "eler042-action-input-evidence");
  action.inputs["metric_digest"] = ObservedMetricDigestForAction(authority);
  action.inputs["safety_envelope_version"] = "1";
  action.inputs["safety_evidence_uuid"] =
      agents::DeterministicAgentRuntimeObjectUuidFromKey("eler042-safety");
  action.inputs["policy_evidence_uuid"] =
      agents::DeterministicAgentRuntimeObjectUuidFromKey("eler042-policy");
  action.inputs["rollout_mode"] = "live";
  action.inputs["rollout_state"] = "active";
  action.inputs["rollout_evidence_uuid"] =
      agents::DeterministicAgentRuntimeObjectUuidFromKey("eler042-rollout");
  action.inputs["failure_threshold"] = "3";
  action.inputs["observed_failures"] = "0";
  action.inputs["retry_limit"] = "2";
  action.inputs["retry_count"] = "0";
  action.inputs["rate_limit_key"] = "page-preallocate";
  action.inputs["rate_limit_per_window"] = "4";
  action.inputs["action_count_in_window"] = "1";
  action.inputs["rate_limit_evidence_uuid"] =
      agents::DeterministicAgentRuntimeObjectUuidFromKey("eler042-rate-limit");
  action.inputs["blast_radius_units"] = "1";
  action.inputs["max_blast_radius_units"] = "3";
  action.inputs["blast_radius_evidence_uuid"] =
      agents::DeterministicAgentRuntimeObjectUuidFromKey("eler042-blast-radius");
  action.inputs["backup_check_required"] = "true";
  action.inputs["checkpoint_check_required"] = "true";
  action.inputs["storage_check_required"] = "true";
  action.inputs["transaction_check_required"] = "true";
  action.inputs["backup_evidence_uuid"] =
      agents::DeterministicAgentRuntimeObjectUuidFromKey("eler042-backup");
  action.inputs["checkpoint_evidence_uuid"] =
      agents::DeterministicAgentRuntimeObjectUuidFromKey("eler042-checkpoint");
  action.inputs["storage_check_evidence_uuid"] =
      agents::DeterministicAgentRuntimeObjectUuidFromKey("eler042-storage-check");
  action.inputs["transaction_evidence_uuid"] =
      agents::DeterministicAgentRuntimeObjectUuidFromKey("eler042-transaction");
  action.inputs["compensation_required"] = "true";
  action.inputs["rollback_required"] = "true";
  action.inputs["compensation_plan_evidence_uuid"] =
      agents::DeterministicAgentRuntimeObjectUuidFromKey("eler042-comp-plan");
  action.inputs["rollback_plan_evidence_uuid"] =
      agents::DeterministicAgentRuntimeObjectUuidFromKey("eler042-rollback-plan");
  action.inputs["authority_claims"] = "agent_evidence";
  return action;
}

agents::DurableAgentCatalogImage CatalogImage() {
  const auto descriptor = agents::FindAgentType("page_allocation_manager");
  Require(descriptor.has_value(), "ELER-042 page allocation descriptor missing");

  agents::DurableAgentCatalogImage image;
  agents::AgentInstanceRecord instance;
  instance.instance_uuid =
      agents::DeterministicAgentRuntimeObjectUuidFromKey(
          "eler042-page-agent-instance");
  instance.agent_type_id = "page_allocation_manager";
  instance.policy_uuid =
      agents::DeterministicAgentRuntimeObjectUuidFromKey("eler042-page-policy");
  instance.scope = "database/filespace/page_family/page_type";
  instance.state = agents::AgentLifecycleState::registered;
  instance.run_generation = 1;
  instance.policy_generation = 7;
  instance.instance_generation = 1;
  image.instances.push_back(instance);

  auto policy = agents::BaselinePolicyForAgent(*descriptor);
  policy.policy_uuid = instance.policy_uuid;
  policy.policy_name = "ELER-042 page allocation production policy";
  policy.scope = instance.scope;
  policy.activation = agents::AgentActivationProfile::live_action;
  policy.enabled = true;
  policy.allow_live_action = true;
  policy.require_manual_approval = true;
  policy.require_dry_run_before_live = false;
  policy.policy_generation = instance.policy_generation;
  image.policies.push_back(policy);

  agents::AgentPolicyAttachmentRecord attachment;
  attachment.attachment_uuid =
      agents::DeterministicAgentRuntimeObjectUuidFromKey(
          "eler042-page-policy-attachment");
  attachment.agent_type_id = instance.agent_type_id;
  attachment.policy_family = policy.policy_family;
  attachment.policy_uuid = policy.policy_uuid;
  attachment.scope = policy.scope;
  attachment.policy_generation = policy.policy_generation;
  attachment.attachment_generation = policy.policy_generation;
  attachment.baseline = false;
  attachment.active = true;
  attachment.valid = true;
  attachment.diagnostic_code = "SB_AGENT_POLICY_ATTACHMENT.ELER042";
  attachment.evidence_uuid =
      agents::DeterministicAgentRuntimeObjectUuidFromKey(
          "eler042-policy-attachment-evidence");
  image.attachments.push_back(std::move(attachment));
  return image;
}

void PersistCatalog(const api::EngineRequestContext& context,
                    agents::DurableAgentCatalogImage image,
                    const std::string& evidence_uuid) {
  api::AgentDurableCatalogStoreRequest request;
  request.context = context;
  request.image = std::move(image);
  request.evidence_uuid = evidence_uuid;
  request.production_live_path = true;
  request.fsync_or_checkpoint_evidence = true;
  const auto persisted = api::PersistAgentDurableCatalogImage(request);
  Require(persisted.ok, "ELER-042 catalog persist failed: " +
                            persisted.diagnostic.detail);
}

agents::AgentProductionRouteProofInputs PageRouteProofs() {
  agents::AgentProductionRouteProofInputs inputs;
  agents::AgentProductionRouteProofInputs::RouteProof proof;
  proof.agent_type_id = "page_allocation_manager";
  proof.action_id = "preallocate_page_family";
  proof.provider_id = "page_manager:preallocate_page_family";
  proof.actuator_id = "page_manager";
  proof.authority_domain = agents::AgentActuatorAuthorityDomain::page;
  proof.subsystem_handler_id = "storage.page.preallocate_page_family";
  proof.handler_provenance = "eler042_storage_page_preallocation_route";
  proof.handler_evidence_uuid =
      agents::DeterministicAgentRuntimeObjectUuidFromKey(
          "ELER-042|page_allocation_manager|preallocate_page_family");
  proof.live_route_available = true;
  proof.real_subsystem_handler = true;
  proof.idempotent = true;
  proof.supports_retry = true;
  proof.supports_rollback_compensation = true;
  proof.requires_outcome_verification = true;
  proof.physical_mutation_route = true;
  inputs.route_proofs.push_back(std::move(proof));
  return inputs;
}

agents::AgentActuatorProviderRegistry RealRegistry(int* dispatch_count) {
  agents::AgentActuatorProviderDescriptor provider;
  provider.provider_id = "page_manager:preallocate_page_family";
  provider.owning_agent = "page_allocation_manager";
  provider.actuator_id = "page_manager";
  provider.operation_id = "preallocate_page_family";
  provider.authority_domain = agents::AgentActuatorAuthorityDomain::page;
  provider.supports_dry_run = true;
  provider.live_route_available = true;
  provider.real_subsystem_handler = true;
  provider.subsystem_handler_id = "storage.page.preallocate_page_family";
  provider.handler_provenance = "eler042_storage_page_preallocation_route";
  provider.handler_evidence_uuid =
      agents::DeterministicAgentRuntimeObjectUuidFromKey(
          "ELER-042|page_allocation_manager|preallocate_page_family");
  provider.idempotent = true;
  provider.supports_retry = true;
  provider.supports_rollback_compensation = true;
  provider.requires_outcome_verification = true;
  provider.required_evidence_fields = {"evidence_uuid", "metric_digest"};
  provider.package_provenance = PageProviderPackage();

  agents::AgentActuatorProviderRegistry registry;
  const auto registered = registry.Register(
      std::move(provider),
      [dispatch_count](const agents::AgentActuatorProviderRequest& request) {
        if (dispatch_count != nullptr && !request.dry_run) {
          ++(*dispatch_count);
        }
        Require(request.execution_context.engine_owned_registry,
                "ELER-042 provider missed engine-owned registry proof");
        Require(request.execution_context.durable_catalog_store_context,
                "ELER-042 provider missed durable catalog store proof");
        Require(request.execution_context.engine_request_context_present,
                "ELER-042 provider missed engine request context proof");
        Require(request.execution_context.fsync_or_checkpoint_evidence,
                "ELER-042 provider missed checkpoint/fsync proof");

        agents::AgentActuatorProviderResult result;
        result.dry_run = request.dry_run;
        result.dispatched = !request.dry_run;
        result.mutation_attempted = !request.dry_run;
        result.outcome_verified = request.dry_run ||
                                  (request.subsystem_reported_success &&
                                   request.intended_state_observed);
        result.compensation_required = !result.outcome_verified;
        result.compensation_attempted = !result.outcome_verified;
        result.verification_evidence_uuid =
            agents::DeterministicAgentRuntimeObjectUuidFromKey(
                "eler042-provider-verification|" + request.action.action_uuid);
        if (!result.outcome_verified) {
          result.compensation_executor_id =
              "storage.page.preallocate_page_family.compensator";
          result.compensation_evidence_uuid =
              agents::DeterministicAgentRuntimeObjectUuidFromKey(
                  "eler042-provider-compensation|" + request.action.action_uuid);
          result.status = agents::AgentError(
              "SB_AGENT_ACTION.OUTCOME_UNVERIFIED_COMPENSATION_REQUIRED",
              request.action.action_uuid);
          return result;
        }
        result.status = {true, "SB_AGENT_ACTION.OUTCOME_VERIFIED",
                         request.action.action_uuid};
        return result;
      });
  Require(registered.ok, "ELER-042 real provider registration failed: " +
                             registered.diagnostic_code);
  return registry;
}

api::EngineOwnedAgentActuatorRegistry SealRegistry(
    const api::EngineRequestContext& context,
    agents::AgentActuatorProviderRegistry registry) {
  auto sealed = api::BuildEngineOwnedAgentActuatorRegistry(
      context, std::move(registry), PageRouteProofs());
  Require(sealed.status.ok, "ELER-042 engine-owned registry seal failed: " +
                                sealed.status.diagnostic_code);
  return std::move(sealed.registry);
}

api::AgentActionDispatchStoreRequest StoreDispatchRequest(
    const api::EngineRequestContext& context,
    const api::EngineOwnedAgentActuatorRegistry* registry,
    agents::AgentActionRequest action) {
  auto authority = SealedAuthority();
  api::AgentActionDispatchStoreRequest request;
  request.context = context;
  request.action = std::move(action);
  request.authority = authority;
  request.engine_registry = registry;
  request.metric_context = MetricContext(authority);
  request.metric_snapshot_options.expected_scope_uuid = authority.scope_uuid;
  request.observed_metric_snapshots = ObservedMetricSnapshots(authority);
  request.production_live_path = true;
  request.fsync_or_checkpoint_evidence = true;
  request.subsystem_reported_success = true;
  request.intended_state_observed = true;
  return request;
}

const agents::DurableAgentActionRecord* FindAction(
    const agents::DurableAgentCatalogImage& image,
    const std::string& action_uuid) {
  for (const auto& action : image.actions) {
    if (action.action_uuid == action_uuid) { return &action; }
  }
  return nullptr;
}

agents::AgentReplayDigestCapture CaptureReplay(
    const agents::DurableAgentCatalogImage& image,
    const std::string& action_uuid,
    const agents::AgentPackageProvenanceBundle& package) {
  agents::AgentReplayDigestCaptureRequest request;
  request.catalog = &image;
  request.action_uuid = action_uuid;
  request.security_epoch = 42;
  request.package_provenance = package;
  const auto captured = agents::CaptureAgentReplayDigests(request);
  Require(captured.status.ok, "ELER-042 replay digest capture failed: " +
                                  captured.status.diagnostic_code);
  return captured.capture;
}

void TestProductionExposureMatrix() {
  const auto route_proofs = PageRouteProofs();
  const auto status =
      agents::ValidateAgentProductionExposureMatrix(route_proofs);
  Require(status.ok, "ELER-042 production exposure matrix rejected route proof: " +
                         status.diagnostic_code);

  const auto records =
      agents::ClassifyAllCanonicalAgentProductionExposures(route_proofs);
  Require(!records.empty(), "ELER-042 canonical agent exposure matrix is empty");
  std::size_t live_agent_count = 0;
  std::size_t live_action_count = 0;
  std::size_t cluster_stub_count = 0;
  bool saw_page_live = false;
  for (const auto& record : records) {
    Require(!record.action_contract_implies_live_route,
            "ELER-042 action contract implied live route for " +
                record.agent_type_id);
    if (record.cluster_provider_authority_required &&
        !record.cluster_provider_authority_available) {
      ++cluster_stub_count;
      Require(record.exposure != agents::AgentProductionExposureClass::live_action,
              "ELER-042 cluster stub exposed live action for " +
                  record.agent_type_id);
    }
    if (record.exposure == agents::AgentProductionExposureClass::live_action) {
      ++live_agent_count;
      Require(record.production_live_route_available &&
                  record.real_subsystem_route_proven,
              "ELER-042 live agent lacks real route proof: " +
                  record.agent_type_id);
    }
    for (const auto& action : record.actions) {
      Require(!action.action_contract_implies_live_route,
              "ELER-042 action contract implied live route for " +
                  record.agent_type_id + ":" + action.action_id);
      if (!action.live_route_available) { continue; }
      ++live_action_count;
      saw_page_live = saw_page_live ||
                      (action.agent_type_id == "page_allocation_manager" &&
                       action.action_id == "preallocate_page_family" &&
                       action.exposure ==
                           agents::AgentProductionExposureClass::live_action);
      Require(action.live_route_available &&
                  action.route_evidence_kind ==
                      "eler042_storage_page_preallocation_route",
              "ELER-042 live action lost route evidence: " +
                  action.agent_type_id + ":" + action.action_id);
    }
  }
  Require(live_agent_count == 1 && live_action_count == 1 && saw_page_live,
          "ELER-042 expected exactly one proven local production-live route");
  Require(cluster_stub_count != 0,
          "ELER-042 cluster provider stub boundary was not represented");

  auto cluster_input = route_proofs;
  agents::AgentProductionRouteProofInputs::RouteProof cluster_proof;
  cluster_proof.agent_type_id = "cluster_scheduler_manager";
  cluster_proof.action_id = "route_cluster_job";
  cluster_proof.provider_id = "cluster_scheduler:route_cluster_job";
  cluster_proof.actuator_id = "scheduler";
  cluster_proof.authority_domain =
      agents::AgentActuatorAuthorityDomain::cluster_provider;
  cluster_proof.subsystem_handler_id = "cluster.scheduler.route_cluster_job";
  cluster_proof.handler_provenance = "external_cluster_provider";
  cluster_proof.handler_evidence_uuid =
      agents::DeterministicAgentRuntimeObjectUuidFromKey(
          "eler042-cluster-proof");
  cluster_proof.live_route_available = true;
  cluster_proof.real_subsystem_handler = true;
  cluster_proof.idempotent = true;
  cluster_proof.supports_retry = true;
  cluster_proof.supports_rollback_compensation = true;
  cluster_proof.requires_outcome_verification = true;
  cluster_proof.external_cluster_provider = true;
  cluster_input.route_proofs.push_back(std::move(cluster_proof));
  const auto cluster_status =
      agents::ValidateAgentProductionExposureMatrix(cluster_input);
  Require(!cluster_status.ok,
          "ELER-042 cluster route proof was accepted without provider authority");
  AddRow("production_exposure_matrix", "pass",
         "live_routes=1;cluster_stub_boundary=present;cluster_admission_requires_provider_authority");
}

void TestStoreBackedDispatchReplayAndRecovery() {
  const auto database = CreateDatabase(
      "scratchbird_eler042_agent_production_action.sbdb",
      1761000000000ull);
  auto seed_context = BootstrapContext(database, 1761000000000ull);
  PersistCatalog(seed_context,
                 CatalogImage(),
                 agents::DeterministicAgentRuntimeObjectUuidFromKey(
                     "eler042-seed-catalog"));
  Commit(seed_context);

  int dispatch_count = 0;
  auto dispatch_context = BeginTransaction(database, "eler042-dispatch");
  auto registry = SealRegistry(dispatch_context, RealRegistry(&dispatch_count));

  const auto live_action = PageAction(
      agents::DeterministicAgentRuntimeObjectUuidFromKey("eler042-live-action"),
      "eler042-live-idempotency");
  const auto live = api::DispatchAgentActionWithDurableCatalogStore(
      StoreDispatchRequest(dispatch_context, &registry, live_action));
  Require(live.dispatch.status.ok,
          "ELER-042 live dispatch failed: " +
              live.dispatch.status.diagnostic_code);
  Require(live.loaded_from_store && live.persisted_to_store &&
              live.engine_owned_registry_validated,
          "ELER-042 live dispatch did not use durable engine-owned store path");
  Require(live.resource_reservation_persisted_before_dispatch &&
              live.pending_action_intent_persisted_before_dispatch,
          "ELER-042 live dispatch did not persist reservation and intent first");
  Require(live.dispatch.provider_dispatched &&
              live.dispatch.durable_record_written &&
              live.dispatch.commercial_evidence_written_before_action_record &&
              live.dispatch.package_provenance_validated &&
              live.dispatch.outcome_verified,
          "ELER-042 live dispatch did not prove provider/evidence/outcome");

  auto failed_action = PageAction(
      agents::DeterministicAgentRuntimeObjectUuidFromKey("eler042-failed-action"),
      "eler042-failed-idempotency");
  auto failed_request =
      StoreDispatchRequest(dispatch_context, &registry, failed_action);
  failed_request.subsystem_reported_success = false;
  failed_request.intended_state_observed = false;
  const auto failed = api::DispatchAgentActionWithDurableCatalogStore(
      failed_request);
  Require(!failed.dispatch.status.ok &&
              failed.dispatch.durable_record_written &&
              failed.dispatch.compensation_required &&
              failed.dispatch.quarantined_or_replay_pending &&
              failed.persisted_to_store,
          "ELER-042 failed action did not persist compensation/quarantine proof");
  Require(dispatch_count == 2,
          "ELER-042 provider dispatch count did not include both live attempts");
  Commit(dispatch_context);

  auto verify_context = BeginTransaction(database, "eler042-verify");
  auto loaded = api::LoadAgentDurableCatalogImage(verify_context, true);
  Require(loaded.ok, "ELER-042 catalog reload after dispatch failed: " +
                         loaded.diagnostic.detail);
  Require(loaded.image.actions.size() == 2,
          "ELER-042 dispatch records did not survive commit/reload");
  Require(loaded.image.evidence.size() == 2,
          "ELER-042 commercial evidence did not survive commit/reload");
  Require(loaded.image.resource_reservations.size() == 2,
          "ELER-042 resource reservations did not survive commit/reload");
  const auto* live_record = FindAction(loaded.image, live_action.action_uuid);
  const auto* failed_record = FindAction(loaded.image, failed_action.action_uuid);
  Require(live_record != nullptr && failed_record != nullptr,
          "ELER-042 reloaded catalog missed action records");
  Require(live_record->state == agents::DurableAgentActionState::completed &&
              live_record->outcome_verified,
          "ELER-042 live action did not reload as completed/verified");
  Require(failed_record->state == agents::DurableAgentActionState::quarantined &&
              failed_record->compensation_required &&
              failed_record->compensation_attempted,
          "ELER-042 failed action did not reload as compensated/quarantined");

  const auto encoded =
      agents::SerializeDurableAgentCatalogImage(loaded.image);
  const auto decoded = agents::ValidateDurableAgentCatalogImage(encoded, true);
  Require(decoded.status.ok &&
              decoded.image.authority.catalog_root_digest ==
                  loaded.image.authority.catalog_root_digest,
          "ELER-042 serialized catalog image failed production validation");

  const auto package = PageProviderPackage();
  auto replay = agents::AgentReplayControlRequest{};
  replay.catalog = &loaded.image;
  replay.action_uuid = failed_action.action_uuid;
  replay.operation = agents::AgentReplayOperationKind::record_compensation;
  replay.capture = CaptureReplay(loaded.image, failed_action.action_uuid, package);
  replay.package_provenance = package;
  replay.evidence_uuid =
      agents::DeterministicAgentRuntimeObjectUuidFromKey("eler042-replay");
  replay.compensation_evidence_uuid =
      agents::DeterministicAgentRuntimeObjectUuidFromKey(
          "eler042-replay-compensation");
  replay.now_microseconds = 1761000000005000ull;
  replay.max_retry_count = 3;
  replay.retry_after_microseconds = 5000;
  const auto replayed = agents::ApplyAgentReplayControl(replay);
  Require(replayed.status.ok && replayed.replay_record_written &&
              replayed.compensation_recorded,
          "ELER-042 compensation replay record failed: " +
              replayed.status.diagnostic_code);
  PersistCatalog(verify_context, loaded.image, replay.evidence_uuid);
  Commit(verify_context);

  auto duplicate_context = BeginTransaction(database, "eler042-duplicate");
  auto duplicate_registry =
      SealRegistry(duplicate_context, RealRegistry(&dispatch_count));
  const auto duplicate = api::DispatchAgentActionWithDurableCatalogStore(
      StoreDispatchRequest(
          duplicate_context,
          &duplicate_registry,
          PageAction(agents::DeterministicAgentRuntimeObjectUuidFromKey(
                         "eler042-live-duplicate"),
                     live_action.idempotency_key)));
  Require(duplicate.dispatch.status.ok &&
              duplicate.dispatch.duplicate_idempotency_key,
          "ELER-042 idempotent dispatch replay failed");
  Require(dispatch_count == 2,
          "ELER-042 idempotent replay called provider again");
  Commit(duplicate_context);

  auto recovery_context = BeginTransaction(database, "eler042-crash-recovery");
  auto recovery_loaded =
      api::LoadAgentDurableCatalogImage(recovery_context, true);
  Require(recovery_loaded.ok,
          "ELER-042 catalog load before crash recovery failed");
  const auto pending_action = PageAction(
      agents::DeterministicAgentRuntimeObjectUuidFromKey("eler042-pending"),
      "eler042-pending-idempotency");
  agents::DurableAgentActionRecord pending;
  pending.action_uuid = pending_action.action_uuid;
  pending.instance_uuid = pending_action.instance_uuid;
  pending.owner_uuid = SealedAuthority().principal_uuid;
  pending.operation_id = pending_action.operation_id;
  pending.actuator_provider_id =
      pending_action.actuator_id + ":" + pending_action.operation_id;
  pending.state = agents::DurableAgentActionState::pending;
  pending.idempotency_key = pending_action.idempotency_key;
  pending.input_evidence_digest =
      agents::AgentActionInputEvidenceDigest(pending_action);
  pending.evidence_uuid =
      agents::DeterministicAgentRuntimeObjectUuidFromKey(
          "eler042-pending-intent");
  pending.diagnostic_code = "SB_AGENT_ACTION_DISPATCH.PENDING_INTENT";
  pending.generation = 1;
  recovery_loaded.image.actions.push_back(std::move(pending));
  const auto pending_refresh =
      agents::RefreshDurableAgentCatalogAuthorityDigest(
          &recovery_loaded.image,
          agents::DeterministicAgentRuntimeObjectUuidFromKey(
              "eler042-pending-refresh"));
  Require(pending_refresh.ok,
          "ELER-042 pending action refresh failed: " +
              pending_refresh.diagnostic_code);
  PersistCatalog(recovery_context,
                 recovery_loaded.image,
                 agents::DeterministicAgentRuntimeObjectUuidFromKey(
                     "eler042-pending-persisted"));

  auto lease_loaded =
      api::LoadAgentDurableCatalogImage(recovery_context, true);
  Require(lease_loaded.ok,
          "ELER-042 catalog reload before lease acquire failed: " +
              lease_loaded.diagnostic.detail);
  agents::DurableLeaseRequest lease;
  lease.lease_uuid =
      agents::DeterministicAgentRuntimeObjectUuidFromKey("eler042-lease");
  lease.instance_uuid = pending_action.instance_uuid;
  lease.owner_uuid = SealedAuthority().principal_uuid;
  lease.now_microseconds = 1761000000007000ull;
  lease.lease_duration_microseconds = 1000000;
  lease.evidence_uuid =
      agents::DeterministicAgentRuntimeObjectUuidFromKey(
          "eler042-lease-evidence");
  const auto leased =
      agents::AcquireDurableAgentLease(&lease_loaded.image, lease);
  Require(leased.ok, "ELER-042 pending lease acquire failed: " +
                         leased.diagnostic_code);
  PersistCatalog(recovery_context,
                 lease_loaded.image,
                 agents::DeterministicAgentRuntimeObjectUuidFromKey(
                     "eler042-lease-persisted"));

  auto recovery_ready =
      api::LoadAgentDurableCatalogImage(recovery_context, true);
  Require(recovery_ready.ok,
          "ELER-042 catalog reload before crash recovery failed: " +
              recovery_ready.diagnostic.detail);
  auto recovered = recovery_ready.image;
  const auto recovery = agents::RecoverDurableAgentCatalogAfterCrash(
      &recovered,
      1761000000008000ull,
      agents::DeterministicAgentRuntimeObjectUuidFromKey(
          "eler042-crash-recovery"));
  Require(recovery.ok, "ELER-042 crash recovery failed: " +
                           recovery.diagnostic_code);
  PersistCatalog(recovery_context,
                 recovered,
                 agents::DeterministicAgentRuntimeObjectUuidFromKey(
                     "eler042-recovery-persisted"));
  const auto recovered_load =
      api::LoadAgentDurableCatalogImage(recovery_context, true);
  Require(recovered_load.ok,
          "ELER-042 recovered catalog reload failed: " +
              recovered_load.diagnostic.detail);
  const auto* recovered_pending =
      FindAction(recovered_load.image, pending_action.action_uuid);
  Require(recovered_pending != nullptr &&
              recovered_pending->state ==
                  agents::DurableAgentActionState::replay_pending,
          "ELER-042 pending action did not become replay_pending");
  bool saw_replay_lease = false;
  bool saw_replay_history = false;
  for (const auto& recovered_lease : recovered_load.image.leases) {
    saw_replay_lease = saw_replay_lease ||
                       recovered_lease.state ==
                           agents::DurableAgentLeaseState::replay_pending;
  }
  for (const auto& history : recovered_load.image.retained_history) {
    saw_replay_history = saw_replay_history ||
                         history.event_kind == "action_replay_pending" ||
                         history.event_kind == "lease_replay_pending";
  }
  Require(saw_replay_lease && saw_replay_history,
          "ELER-042 crash recovery did not retain replay evidence");
  Commit(recovery_context);

  CleanupDatabase(database.path);
  AddRow("store_backed_dispatch_replay_recovery", "pass",
         "live_dispatch=verified;unverified_action=compensated;replay_record=durable;pending_action_and_lease=replay_pending_after_recovery");
}

void TestUnsafeAuthorityFailsBeforeMutation() {
  const auto database = CreateDatabase(
      "scratchbird_eler042_agent_authority_refusal.sbdb",
      1761000001000ull);
  auto seed_context = BootstrapContext(database, 1761000001000ull);
  PersistCatalog(seed_context,
                 CatalogImage(),
                 agents::DeterministicAgentRuntimeObjectUuidFromKey(
                     "eler042-authority-seed"));
  Commit(seed_context);

  auto context = BeginTransaction(database, "eler042-authority-refusal");
  int dispatch_count = 0;
  auto registry = SealRegistry(context, RealRegistry(&dispatch_count));
  auto request = StoreDispatchRequest(
      context,
      &registry,
      PageAction(agents::DeterministicAgentRuntimeObjectUuidFromKey(
                     "eler042-parser-authority-action"),
                 "eler042-parser-authority"));
  request.authority.parser_authority = true;
  const auto refused =
      api::DispatchAgentActionWithDurableCatalogStore(request);
  Require(!refused.dispatch.status.ok &&
              refused.dispatch.status.diagnostic_code ==
                  "SB_AGENT_ACTION_AUTHORITY.UNTRUSTED_SOURCE",
          "ELER-042 parser authority was not refused");
  Require(!refused.loaded_from_store &&
              !refused.resource_reservation_acquired &&
              !refused.pending_action_intent_persisted_before_dispatch &&
              !refused.persisted_to_store,
          "ELER-042 unsafe authority reached durable mutation path");
  Require(dispatch_count == 0,
          "ELER-042 unsafe authority reached provider execution");

  const auto loaded = api::LoadAgentDurableCatalogImage(context, true);
  Require(loaded.ok, "ELER-042 catalog load after authority refusal failed");
  Require(loaded.image.actions.empty() &&
              loaded.image.resource_reservations.empty() &&
              loaded.image.evidence.empty(),
          "ELER-042 authority refusal left durable action artifacts");

  auto sidecar_image = loaded.image;
  sidecar_image.authority.sidecar_storage = true;
  const auto sidecar =
      agents::ValidateDurableAgentCatalogForProduction(sidecar_image);
  Require(!sidecar.ok &&
              sidecar.diagnostic_code ==
                  "SB_AGENT_CATALOG.SIDECAR_OR_MEMORY_AUTHORITY_REFUSED",
          "ELER-042 sidecar durable catalog authority was accepted");
  Commit(context);
  CleanupDatabase(database.path);
  AddRow("unsafe_authority_ordering", "pass",
         "parser_authority_refused_before_catalog_load_or_mutation;sidecar_catalog_refused");
}

api::EnginePrepareSupportBundleRequest SupportRequest(
    const TestDatabase& database) {
  api::EnginePrepareSupportBundleRequest request;
  request.context.trust_mode = api::EngineTrustMode::server_isolated;
  request.context.security_context_present = true;
  request.context.database_uuid.canonical = database.database_uuid;
  request.context.principal_uuid.canonical =
      agents::DeterministicAgentRuntimePrincipalUuidFromKey(
          "eler042-support-principal");
  request.context.database_path = database.path.string();
  request.option_envelopes.push_back("engine_authorized_support_export:true");
  return request;
}

void TestSupportBundleRoute() {
  const auto database = CreateDatabase(
      "scratchbird_eler042_agent_support_bundle.sbdb",
      1761000002000ull);

  impl::SupportBundleTriageSnapshot snapshot;
  snapshot.completeness_ratio_per_mille = 950;
  snapshot.agent_actions_total = 4;
  snapshot.evidence_catalog_authoritative = true;
  snapshot.tamper_evidence_valid = true;
  snapshot.redaction_policy_valid = true;
  snapshot.protected_material_present = true;
  snapshot.support_bundle_sink_available = true;
  const auto triage = impl::EvaluateSupportBundleTriage(snapshot);
  Require(triage.ok() &&
              triage.decision ==
                  impl::SupportBundleTriageDecisionKind::prepare_redacted_bundle,
          "ELER-042 support triage did not prepare redacted bundle");

  api::AgentSupportBundleTriageRouteRequest route;
  route.triage_result = triage;
  route.support_request = SupportRequest(database);
  route.agent_uuid =
      agents::DeterministicAgentRuntimeObjectUuidFromKey("eler042-support-agent");
  route.evidence_uuid =
      agents::DeterministicAgentRuntimeObjectUuidFromKey(
          "eler042-support-evidence");
  route.durable_evidence_store_authority = true;
  route.tamper_chain_verified = true;
  route.redaction_profile_authoritative = true;
  route.support_export_authorized_by_engine = true;
  const auto prepared = api::ApplySupportBundleTriageAgentRoute(route);
  Require(prepared.ok && prepared.support_bundle_prepared &&
              prepared.protected_material_suppressed &&
              prepared.support_result.redaction_applied &&
              prepared.support_result.forbidden_fields_absent &&
              prepared.support_result.flush_required_before_export &&
              prepared.support_result.agent_runtime_evidence_collected,
          "ELER-042 support bundle triage did not prepare safe bundle");
  Require(prepared.support_result.support_bundle_json.find("secret") ==
              std::string::npos &&
              prepared.support_result.support_bundle_json.find("password") ==
                  std::string::npos,
          "ELER-042 support bundle leaked protected material keywords");

  route.sidecar_authority = true;
  const auto sidecar = api::ApplySupportBundleTriageAgentRoute(route);
  Require(!sidecar.ok &&
              sidecar.diagnostic_code ==
                  "SB_AGENT_SUPPORT_TRIAGE_ROUTE.UNSAFE_AUTHORITY",
          "ELER-042 support bundle route accepted sidecar authority");
  route.sidecar_authority = false;
  route.support_request.option_envelopes.clear();
  const auto unauth = api::ApplySupportBundleTriageAgentRoute(route);
  Require(!unauth.ok &&
              unauth.diagnostic_code ==
                  "SB_AGENT_SUPPORT_TRIAGE_ROUTE.ENGINE_AUTH_REQUIRED",
          "ELER-042 support bundle route accepted missing engine authorization");

  CleanupDatabase(database.path);
  AddRow("support_bundle_route", "pass",
         "redacted_bundle_prepared;agent_runtime_evidence_collected;sidecar_and_unauthorized_routes_fail_closed");
}

}  // namespace

int main(int argc, char** argv) {
  ConfigureMemoryFixture();
  const std::filesystem::path output_path =
      argc > 1 ? std::filesystem::path(argv[1]) : std::filesystem::path{};
  TestProductionExposureMatrix();
  TestStoreBackedDispatchReplayAndRecovery();
  TestUnsafeAuthorityFailsBeforeMutation();
  TestSupportBundleRoute();
  WriteMatrix(output_path);
  return EXIT_SUCCESS;
}
