// Copyright (c) 2026 ScratchBird Software Inc.
//
// This Source Code Form is subject to the terms of the Mozilla Public
// License, v. 2.0. If a copy of the MPL was not distributed with this
// file, You can obtain one at https://mozilla.org/MPL/2.0/.
//
// SPDX-License-Identifier: MPL-2.0

#include "agents/agent_action_dispatch_store_api.hpp"
#include "agents/agent_durable_catalog_store_api.hpp"
#include "transaction/transaction_api.hpp"

#include "agent_runtime.hpp"
#include "database_lifecycle.hpp"
#include "local_transaction_store.hpp"
#include "memory.hpp"
#include "transaction_inventory.hpp"
#include "uuid.hpp"
#include "agent_package_provenance_test_support.hpp"

// SEARCH_KEY: AEIC_AGENT_CRASH_RESTART_CONCURRENCY_TESTS
// SEARCH_KEY: AEIC_AGENT_SECURITY_NEGATIVE_REDACTION_TESTS
// Store-backed action dispatch regression for evidence rollback/commit
// visibility, stale approval/cancellation conflict refusal, and policy
// generation/security-context race refusal before provider execution.

#include <chrono>
#include <cstdlib>
#include <filesystem>
#include <iostream>
#include <string>
#include <utility>
#include <vector>

namespace {

namespace agents = scratchbird::core::agents;
namespace api = scratchbird::engine::internal_api;
namespace db = scratchbird::storage::database;
namespace memory = scratchbird::core::memory;
namespace mga = scratchbird::transaction::mga;
namespace uuid = scratchbird::core::uuid;
using scratchbird::core::platform::UuidKind;

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
  policy.policy_name = "agent_action_dispatch_store_gate";
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
      "agent_action_dispatch_store_gate");
  Require(configured.ok(), "agent action dispatch store memory fixture failed");
  Require(configured.fixture_mode,
          "agent action dispatch store memory fixture mode missing");
}

struct TestDatabase {
  std::filesystem::path path;
  std::string database_uuid;
  std::string transaction_uuid;
  std::uint64_t local_transaction_id = 0;
};

void Cleanup(const std::filesystem::path& path) {
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

api::EngineRequestContext BeginTransactionContext(const TestDatabase& database,
                                                  std::string request_id);
void CommitContext(api::EngineRequestContext context);
void RollbackContext(api::EngineRequestContext context);

TestDatabase CreateActiveDatabase(const char* basename,
                                  std::uint64_t timestamp_base) {
  (void)timestamp_base;
  const std::uint64_t current_base = CurrentTestBaseMillis();
  const std::filesystem::path path =
      std::filesystem::temp_directory_path() / basename;
  Cleanup(path);

  const auto database_uuid = uuid::GenerateEngineIdentityV7(
      UuidKind::database, current_base + 1);
  const auto filespace_uuid = uuid::GenerateEngineIdentityV7(
      UuidKind::filespace, current_base + 2);
  Require(database_uuid.ok(), "database UUID generation failed");
  Require(filespace_uuid.ok(), "filespace UUID generation failed");

  db::DatabaseCreateConfig create;
  create.path = path.string();
  create.database_uuid = database_uuid.value;
  create.filespace_uuid = filespace_uuid.value;
  create.page_size = 16384;
  create.creation_unix_epoch_millis = current_base + 3;
  create.allow_minimal_resource_bootstrap = true;
  create.require_resource_seed_pack = false;
  create.allow_overwrite = true;
  Require(db::CreateDatabaseFile(create).ok(), "database creation failed");

  auto inventory = mga::MakeEmptyLocalTransactionInventory();
  const auto transaction_uuid = uuid::GenerateEngineIdentityV7(
      UuidKind::transaction, current_base + 4);
  Require(transaction_uuid.ok(), "transaction UUID generation failed");
  auto begun = mga::BeginLocalTransaction(std::move(inventory),
                                          transaction_uuid.value,
                                          current_base + 5);
  Require(begun.ok(), "local transaction begin failed");
  Require(db::PersistLocalTransactionInventoryToDatabase(path.string(),
                                                         begun.inventory)
              .ok(),
          "local transaction inventory persist failed");

  TestDatabase result;
  result.path = path;
  result.database_uuid = uuid::UuidToString(database_uuid.value.value);
  result.transaction_uuid = uuid::UuidToString(transaction_uuid.value.value);
  result.local_transaction_id = begun.entry.identity.local_id.value;
  return result;
}

api::EngineRequestContext Context(const TestDatabase& database) {
  api::EngineRequestContext context;
  context.request_id = "aeic-action-dispatch-store";
  context.database_path = database.path.string();
  context.database_uuid.canonical = database.database_uuid;
  context.transaction_uuid.canonical = database.transaction_uuid;
  context.local_transaction_id = database.local_transaction_id;
  context.snapshot_visible_through_local_transaction_id =
      database.local_transaction_id;
  context.security_context_present = true;
  context.principal_uuid.canonical =
      "018f0000-0000-7000-8000-00000000ce10";
  context.session_uuid.canonical =
      "018f0000-0000-7000-8000-00000000ce14";
  context.catalog_generation_id = 1;
  context.security_epoch = 1;
  context.resource_epoch = 1;
  context.name_resolution_epoch = 1;
  context.transaction_isolation_level = "read_committed";
  return context;
}

api::EngineRequestContext BeginTransactionContext(const TestDatabase& database,
                                                  std::string request_id) {
  api::EngineBeginTransactionRequest begin;
  begin.context = Context(database);
  begin.context.request_id = std::move(request_id);
  begin.context.transaction_uuid.canonical.clear();
  begin.context.local_transaction_id = 0;
  begin.context.snapshot_visible_through_local_transaction_id = 0;
  begin.isolation_level = "read_committed";
  begin.transaction_policy_profile.encoded_profiles.push_back("fail_closed:true");
  begin.transaction_policy_profile.encoded_profiles.push_back(
      "transaction_read_only:false");
  begin.transaction_policy_profile.encoded_profiles.push_back(
      "transaction_read_mode:read_write");
  const auto begun = api::EngineBeginTransaction(begin);
  std::string begin_detail;
  if (!begun.diagnostics.empty()) {
    begin_detail = begun.diagnostics.front().code + ":" +
                   begun.diagnostics.front().detail;
  }
  Require(begun.ok && begun.local_transaction_id != 0,
          "engine transaction begin failed for action dispatch store gate: " +
              begin.context.request_id + ":" + begin_detail);
  auto context = begin.context;
  context.transaction_uuid = begun.transaction_uuid;
  context.local_transaction_id = begun.local_transaction_id;
  context.snapshot_visible_through_local_transaction_id =
      begun.snapshot_visible_through_local_transaction_id;
  context.transaction_isolation_level = "read_committed";
  return context;
}

void CommitContext(api::EngineRequestContext context) {
  api::EngineCommitTransactionRequest commit;
  commit.context = std::move(context);
  const auto committed = api::EngineCommitTransaction(commit);
  std::string detail;
  if (!committed.diagnostics.empty()) {
    detail = committed.diagnostics.front().code + ":" +
             committed.diagnostics.front().detail;
  }
  Require(committed.ok,
          "engine transaction commit failed for action dispatch store gate: " +
              detail);
}

void RollbackContext(api::EngineRequestContext context) {
  api::EngineRollbackTransactionRequest rollback;
  rollback.context = std::move(context);
  const auto rolled_back = api::EngineRollbackTransaction(rollback);
  Require(rolled_back.ok,
          "engine transaction rollback failed for action dispatch store gate");
}

agents::AgentActionAuthorityProvenance SealedAuthority();
agents::AgentActionRequest Action(std::string uuid,
                                  std::string idempotency_key);

agents::DurableAgentCatalogImage CatalogImage() {
  agents::DurableAgentCatalogImage image;
  agents::AgentInstanceRecord instance;
  instance.instance_uuid = "018f0000-0000-7000-8000-00000000ce11";
  instance.agent_type_id = "page_allocation_manager";
  instance.policy_uuid = "018f0000-0000-7000-8000-00000000ce12";
  instance.scope = "database/filespace/page_family/page_type";
  instance.state = agents::AgentLifecycleState::registered;
  instance.run_generation = 1;
  instance.policy_generation = 7;
  instance.instance_generation = 1;
  image.instances.push_back(instance);
  return image;
}

agents::DurableAgentCatalogImage CatalogImageWithPendingActionAndPolicy() {
  auto image = CatalogImage();
  const auto descriptor = agents::FindAgentType("page_allocation_manager");
  Require(descriptor.has_value(), "page allocation descriptor missing");
  auto policy = agents::BaselinePolicyForAgent(*descriptor);
  policy.policy_uuid = image.instances.front().policy_uuid;
  policy.policy_name = "AEIC-041 page allocation policy";
  policy.scope = image.instances.front().scope;
  policy.activation = agents::AgentActivationProfile::live_action;
  policy.allow_live_action = true;
  policy.require_manual_approval = true;
  policy.require_dry_run_before_live = false;
  policy.policy_generation = image.instances.front().policy_generation;
  image.policies.push_back(policy);

  agents::AgentPolicyAttachmentRecord attachment;
  attachment.attachment_uuid = "018f0000-0000-7000-8000-00000000ce71";
  attachment.agent_type_id = "page_allocation_manager";
  attachment.policy_family = policy.policy_family;
  attachment.policy_uuid = policy.policy_uuid;
  attachment.scope = policy.scope;
  attachment.policy_generation = policy.policy_generation;
  attachment.attachment_generation = policy.policy_generation;
  attachment.baseline = false;
  attachment.active = true;
  attachment.valid = true;
  attachment.diagnostic_code = "SB_AGENT_POLICY_ATTACHMENT.AEIC041";
  attachment.evidence_uuid = "018f0000-0000-7000-8000-00000000ce72";
  image.attachments.push_back(std::move(attachment));

  auto pending_action = Action("018f0000-0000-7000-8000-00000000ce73",
                               "idem-aeic041-pending-action");
  pending_action.inputs["expected_policy_generation"] =
      std::to_string(policy.policy_generation);

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
  pending.evidence_uuid = "018f0000-0000-7000-8000-00000000ce74";
  pending.diagnostic_code = "SB_AGENT_ACTION_DISPATCH.PENDING_INTENT";
  pending.generation = 1;
  image.actions.push_back(std::move(pending));
  return image;
}

void SeedCatalog(const api::EngineRequestContext& context) {
  api::AgentDurableCatalogStoreRequest seed;
  seed.context = context;
  seed.image = CatalogImage();
  seed.evidence_uuid = "018f0000-0000-7000-8000-00000000ce13";
  seed.production_live_path = true;
  seed.fsync_or_checkpoint_evidence = true;
  Require(api::PersistAgentDurableCatalogImage(seed).ok,
          "initial action-dispatch catalog seed failed");
}

void SeedCatalog(const api::EngineRequestContext& context,
                 agents::DurableAgentCatalogImage image,
                 std::string evidence_uuid) {
  api::AgentDurableCatalogStoreRequest seed;
  seed.context = context;
  seed.image = std::move(image);
  seed.evidence_uuid = std::move(evidence_uuid);
  seed.production_live_path = true;
  seed.fsync_or_checkpoint_evidence = true;
  Require(api::PersistAgentDurableCatalogImage(seed).ok,
          "initial action-dispatch catalog seed failed");
}

agents::AgentActionAuthorityProvenance SealedAuthority() {
  agents::AgentActionAuthorityProvenance authority;
  authority.source = agents::AgentActionAuthoritySource::sealed_internal_bootstrap;
  authority.principal_uuid = "018f0000-0000-7000-8000-00000000ce20";
  authority.scope_uuid = "018f0000-0000-7000-8000-00000000ce21";
  authority.provenance_evidence_uuid =
      "018f0000-0000-7000-8000-00000000ce22";
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
  context.wall_now_microseconds = 1760000000001000ull;
  return context;
}

std::vector<agents::AgentObservedMetricSnapshot> ObservedMetricSnapshots(
    const agents::AgentActionAuthorityProvenance& authority) {
  const auto descriptor = agents::FindAgentType("page_allocation_manager");
  Require(descriptor.has_value(), "page allocation descriptor missing");
  std::vector<agents::AgentObservedMetricSnapshot> snapshots;
  int ordinal = 0;
  for (const auto& dependency : descriptor->metric_dependencies) {
    agents::AgentObservedMetricSnapshot snapshot;
    snapshot.metric_family = dependency.metric_family;
    snapshot.namespace_path = dependency.namespace_prefix + ".observed";
    snapshot.generation = 200 + ordinal;
    snapshot.observed_wall_microseconds = 1760000000000000ull;
    snapshot.scope_uuid = authority.scope_uuid;
    snapshot.digest = "sha256:" + dependency.metric_family;
    snapshot.source_quality = agents::AgentMetricSourceQuality::trusted;
    snapshot.present = true;
    snapshot.trusted = true;
    snapshot.schema_compatible = true;
    snapshot.trust_provenance = "engine_metric_registry";
    snapshot.evidence_uuid = "metric-evidence-store-" + std::to_string(ordinal);
    snapshot.snapshot_id = "metric-snapshot-store-" + std::to_string(ordinal);
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
    source_a.attestation_digest = "attestation:" + source_a.metric_family +
                                  ":" + source_a.source_id;
    source_a.evidence_uuid += ":source-a";
    source_a.snapshot_id += ":source-a";
    snapshots.push_back(std::move(source_a));

    auto source_b = snapshot;
    source_b.source_id = "source-b";
    source_b.source_sequence = snapshot.generation * 2 + 2;
    source_b.previous_source_sequence = source_b.source_sequence - 1;
    source_b.attestation_key_id = "metric-key:" + source_b.source_id;
    source_b.attestation_digest = "attestation:" + source_b.metric_family +
                                  ":" + source_b.source_id;
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
  Require(descriptor.has_value(), "page allocation descriptor missing");
  agents::AgentMetricSnapshotEvaluationOptions options;
  options.expected_scope_uuid = authority.scope_uuid;
  const auto evaluation = agents::EvaluateAgentObservedMetricSnapshots(
      *descriptor,
      MetricContext(authority),
      ObservedMetricSnapshots(authority),
      options);
  Require(evaluation.accepted,
          "metric digest helper rejected observed snapshots: " +
              evaluation.status.diagnostic_code);
  return evaluation.input_digest;
}

agents::AgentActionRequest Action(std::string uuid,
                                  std::string idempotency_key) {
  const auto authority = SealedAuthority();
  agents::AgentActionRequest action;
  action.action_uuid = std::move(uuid);
  action.agent_type_id = "page_allocation_manager";
  action.instance_uuid = "018f0000-0000-7000-8000-00000000ce11";
  action.actuator_id = "page_manager";
  action.operation_id = "preallocate_page_family";
  action.idempotency_key = std::move(idempotency_key);
  action.dry_run = false;
  action.inputs["evidence_uuid"] = "018f0000-0000-7000-8000-00000000ce30";
  action.inputs["metric_digest"] = ObservedMetricDigestForAction(authority);
  action.inputs["safety_envelope_version"] = "1";
  action.inputs["safety_evidence_uuid"] = "018f0000-0000-7000-8000-00000000ce31";
  action.inputs["policy_evidence_uuid"] = "018f0000-0000-7000-8000-00000000ce32";
  action.inputs["rollout_mode"] = "live";
  action.inputs["rollout_state"] = "active";
  action.inputs["rollout_evidence_uuid"] = "018f0000-0000-7000-8000-00000000ce33";
  action.inputs["failure_threshold"] = "3";
  action.inputs["observed_failures"] = "0";
  action.inputs["retry_limit"] = "2";
  action.inputs["retry_count"] = "0";
  action.inputs["rate_limit_key"] = "page-preallocate";
  action.inputs["rate_limit_per_window"] = "4";
  action.inputs["action_count_in_window"] = "1";
  action.inputs["rate_limit_evidence_uuid"] = "018f0000-0000-7000-8000-00000000ce34";
  action.inputs["blast_radius_units"] = "1";
  action.inputs["max_blast_radius_units"] = "3";
  action.inputs["blast_radius_evidence_uuid"] = "018f0000-0000-7000-8000-00000000ce35";
  action.inputs["backup_check_required"] = "true";
  action.inputs["checkpoint_check_required"] = "true";
  action.inputs["storage_check_required"] = "true";
  action.inputs["transaction_check_required"] = "true";
  action.inputs["backup_evidence_uuid"] = "018f0000-0000-7000-8000-00000000ce36";
  action.inputs["checkpoint_evidence_uuid"] = "018f0000-0000-7000-8000-00000000ce37";
  action.inputs["storage_check_evidence_uuid"] = "018f0000-0000-7000-8000-00000000ce38";
  action.inputs["transaction_evidence_uuid"] = "018f0000-0000-7000-8000-00000000ce39";
  action.inputs["compensation_required"] = "true";
  action.inputs["rollback_required"] = "true";
  action.inputs["compensation_plan_evidence_uuid"] = "018f0000-0000-7000-8000-00000000ce3a";
  action.inputs["rollback_plan_evidence_uuid"] = "018f0000-0000-7000-8000-00000000ce3b";
  action.inputs["authority_claims"] = "agent_evidence";
  return action;
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
  provider.handler_provenance = "store_gate_real_subsystem_handler";
  provider.handler_evidence_uuid = "018f0000-0000-7000-8000-00000000ce32";
  provider.idempotent = true;
  provider.supports_retry = true;
  provider.supports_rollback_compensation = true;
  provider.requires_outcome_verification = true;
  provider.required_evidence_fields = {"evidence_uuid", "metric_digest"};
  provider.package_provenance =
      agent_test_support::PageProviderPackageProvenance(
          "018f0000-0000-7000-8000-00000000ce8");

  agents::AgentActuatorProviderRegistry registry;
  const auto registered = registry.Register(
      std::move(provider),
      [dispatch_count](const agents::AgentActuatorProviderRequest& request) {
        if (dispatch_count != nullptr && !request.dry_run) { ++(*dispatch_count); }
        if (!request.dry_run) {
          Require(request.execution_context.engine_owned_registry,
                  "provider did not receive engine-owned registry proof");
          Require(request.execution_context.durable_catalog_store_context,
                  "provider did not receive durable catalog store context");
          Require(request.execution_context.engine_request_context_present,
                  "provider did not receive engine request context");
          Require(!request.execution_context.database_uuid.empty(),
                  "provider execution context missing database UUID");
          Require(!request.execution_context.transaction_uuid.empty(),
                  "provider execution context missing transaction UUID");
          Require(request.execution_context.local_transaction_id != 0,
                  "provider execution context missing local transaction id");
        }
        agents::AgentActuatorProviderResult result;
        result.dry_run = request.dry_run;
        result.dispatched = !request.dry_run;
        result.mutation_attempted = !request.dry_run;
        result.outcome_verified = request.dry_run ||
                                  (request.subsystem_reported_success &&
                                   request.intended_state_observed);
        result.compensation_required = !result.outcome_verified;
        result.compensation_attempted = !result.outcome_verified;
        if (!result.outcome_verified) {
          result.compensation_executor_id =
              "storage.page.preallocate_page_family.compensator";
          result.compensation_evidence_uuid =
              "018f0000-0000-7000-8000-00000000ce34";
        }
        result.verification_evidence_uuid =
            "018f0000-0000-7000-8000-00000000ce31";
        result.status = result.outcome_verified
                            ? agents::AgentRuntimeStatus{
                                  true, "SB_AGENT_ACTION.OUTCOME_VERIFIED",
                                  request.action.action_uuid}
                            : agents::AgentError(
                                  "SB_AGENT_ACTION.OUTCOME_UNVERIFIED_COMPENSATION_REQUIRED",
                                  request.action.action_uuid);
        return result;
      });
  Require(registered.ok, "real provider registration failed");
  return registry;
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
  proof.handler_provenance = "store_gate_real_subsystem_handler";
  proof.handler_evidence_uuid = "018f0000-0000-7000-8000-00000000ce32";
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

api::EngineOwnedAgentActuatorRegistry SealRegistry(
    const api::EngineRequestContext& context,
    agents::AgentActuatorProviderRegistry registry) {
  auto sealed = api::BuildEngineOwnedAgentActuatorRegistry(
      context, std::move(registry), PageRouteProofs());
  Require(sealed.status.ok, "engine-owned registry sealing failed: " +
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

void TestStoreBackedLiveDispatchPersistsAction() {
  const auto database = CreateActiveDatabase(
      "scratchbird_aeic_agent_action_dispatch_store.sbdb",
      1760000000000ull);
  const auto context = Context(database);
  SeedCatalog(context);

  int dispatch_count = 0;
  auto registry = SealRegistry(context, RealRegistry(&dispatch_count));
  const auto first = api::DispatchAgentActionWithDurableCatalogStore(
      StoreDispatchRequest(
          context, &registry,
          Action("018f0000-0000-7000-8000-00000000ce40",
                 "idem-store-live")));
  Require(first.dispatch.status.ok, "store-backed live dispatch failed: " +
                                       first.dispatch.status.diagnostic_code +
                                       ":" + first.dispatch.status.detail);
  Require(first.loaded_from_store, "dispatch did not load from durable store");
  Require(first.persisted_to_store, "dispatch did not persist to durable store");
  Require(first.engine_owned_registry_validated,
          "engine-owned provider registry was not validated");
  Require(first.pending_action_intent_persisted_before_dispatch,
          "pending action intent was not persisted before provider dispatch");
  Require(first.dispatch.provider_dispatched,
          "store-backed dispatch did not call real provider");
  Require(dispatch_count == 1, "unexpected provider dispatch count");

  const auto loaded = api::LoadAgentDurableCatalogImage(context, true);
  Require(loaded.ok, "catalog reload after action dispatch failed");
  Require(loaded.image.actions.size() == 1,
          "persisted action record missing after dispatch");
  Require(loaded.image.evidence.size() == 1,
          "commercial evidence missing after dispatch");
  Require(loaded.image.actions.front().outcome_verified,
          "persisted action outcome was not verified");
  Require(loaded.image.actions.front().actuator_provider_id ==
              "page_manager:preallocate_page_family",
          "persisted action provider mismatch");
  Require(loaded.image.resource_reservations.size() == 1,
          "resource reservation record missing after dispatch");
  Require(loaded.image.resource_reservations.front().state ==
              agents::DurableAgentResourceReservationState::released,
          "resource reservation was not released after dispatch");

  const auto duplicate = api::DispatchAgentActionWithDurableCatalogStore(
      StoreDispatchRequest(
          context, &registry,
          Action("018f0000-0000-7000-8000-00000000ce41",
                 "idem-store-live")));
  Require(duplicate.dispatch.status.ok &&
              duplicate.dispatch.duplicate_idempotency_key,
          "store-backed idempotency replay failed");
  Require(dispatch_count == 1, "idempotent replay dispatched a second time");

  const auto replay_loaded = api::LoadAgentDurableCatalogImage(context, true);
  Require(replay_loaded.ok, "catalog reload after idempotency replay failed");
  Require(replay_loaded.image.actions.size() == 1,
          "idempotency replay wrote a duplicate action");
  Require(replay_loaded.image.resource_reservations.size() == 2,
          "idempotency replay did not record/release resource reservation");

  Cleanup(database.path);
}

void TestDefaultRegistryCannotSimulateLiveMutation() {
  const auto database = CreateActiveDatabase(
      "scratchbird_aeic_agent_action_default_registry.sbdb",
      1760000000100ull);
  const auto context = Context(database);
  SeedCatalog(context);

  const auto result = api::DispatchAgentActionWithDurableCatalogStore(
      StoreDispatchRequest(
          context, nullptr,
          Action("018f0000-0000-7000-8000-00000000ce50",
                 "idem-default-registry")));
  Require(!result.dispatch.status.ok,
          "default registry simulated a live action successfully");
  Require(result.dispatch.status.diagnostic_code ==
              "SB_AGENT_ACTION_STORE.ENGINE_OWNED_REGISTRY_REQUIRED",
          "default registry refusal diagnostic changed: " +
              result.dispatch.status.diagnostic_code);
  Require(!result.dispatch.provider_dispatched,
          "default registry reported provider dispatch");
  Require(!result.persisted_to_store,
          "missing engine-owned registry should fail before store mutation");

  const auto loaded = api::LoadAgentDurableCatalogImage(context, true);
  Require(loaded.ok, "catalog reload after refused action failed");
  Require(loaded.image.actions.empty(),
          "simulated-provider refusal wrote a durable action record");
  Require(loaded.image.resource_reservations.empty(),
          "missing engine-owned registry leaked a resource reservation");

  Cleanup(database.path);
}

void TestCallerSuppliedRegistryRefusedWithoutEngineProvenance() {
  const auto database = CreateActiveDatabase(
      "scratchbird_aeic_agent_action_caller_registry.sbdb",
      1760000000150ull);
  const auto context = Context(database);
  SeedCatalog(context);

  int dispatch_count = 0;
  auto registry = SealRegistry(context, RealRegistry(&dispatch_count));
  auto request = StoreDispatchRequest(
      context, &registry,
      Action("018f0000-0000-7000-8000-00000000ce55",
             "idem-caller-registry"));
  api::EngineOwnedAgentActuatorRegistry unsealed_registry;
  request.engine_registry = &unsealed_registry;
  const auto result = api::DispatchAgentActionWithDurableCatalogStore(request);
  Require(!result.dispatch.status.ok &&
              result.dispatch.status.diagnostic_code ==
                  "SB_AGENT_ACTION_STORE.REGISTRY_PROVENANCE_REQUIRED",
          "unsealed provider registry reached production dispatch");
  Require(dispatch_count == 0,
          "unsealed provider registry executed before provenance gate");

  const auto loaded = api::LoadAgentDurableCatalogImage(context, true);
  Require(loaded.ok, "catalog reload after caller registry refusal failed");
  Require(loaded.image.actions.empty(),
          "unsealed registry refusal wrote a durable action");
  Require(loaded.image.resource_reservations.empty(),
          "unsealed registry refusal leaked a resource reservation");

  Cleanup(database.path);
}

void TestProductionDispatchRequiresSecurityContextBeforeMutation() {
  const auto database = CreateActiveDatabase(
      "scratchbird_aeic_agent_action_security_context.sbdb",
      1760000000160ull);
  const auto context = Context(database);
  SeedCatalog(context);

  int dispatch_count = 0;
  auto registry = SealRegistry(context, RealRegistry(&dispatch_count));
  auto request = StoreDispatchRequest(
      context, &registry,
      Action("018f0000-0000-7000-8000-00000000ce56",
             "idem-security-context"));
  request.context.security_context_present = false;
  const auto missing_context =
      api::DispatchAgentActionWithDurableCatalogStore(request);
  Require(!missing_context.dispatch.status.ok &&
              missing_context.dispatch.status.diagnostic_code ==
                  "SB_AGENT_ACTION_STORE.ENGINE_REQUEST_CONTEXT_REQUIRED",
          "store dispatch accepted missing security context");
  Require(dispatch_count == 0,
          "store dispatch reached provider without security context");
  Require(!missing_context.loaded_from_store &&
              !missing_context.resource_reservation_acquired &&
              !missing_context.pending_action_intent_persisted_before_dispatch &&
              !missing_context.persisted_to_store,
          "store dispatch mutated state before security-context refusal");

  auto missing_transaction = StoreDispatchRequest(
      context, &registry,
      Action("018f0000-0000-7000-8000-00000000ce57",
             "idem-missing-local-transaction"));
  missing_transaction.context.local_transaction_id = 0;
  const auto missing_transaction_result =
      api::DispatchAgentActionWithDurableCatalogStore(missing_transaction);
  Require(!missing_transaction_result.dispatch.status.ok &&
              missing_transaction_result.dispatch.status.diagnostic_code ==
                  "SB_AGENT_ACTION_STORE.ENGINE_REQUEST_CONTEXT_REQUIRED",
          "store dispatch accepted missing MGA transaction context");
  Require(dispatch_count == 0,
          "store dispatch reached provider without MGA transaction context");

  const auto loaded = api::LoadAgentDurableCatalogImage(context, true);
  Require(loaded.ok, "catalog reload after security refusal failed");
  Require(loaded.image.actions.empty(),
          "security-context refusal wrote a durable action");
  Require(loaded.image.resource_reservations.empty(),
          "security-context refusal leaked a resource reservation");

  Cleanup(database.path);
}

void TestEngineRegistryRequiresRouteProof() {
  const auto database = CreateActiveDatabase(
      "scratchbird_aeic_agent_action_route_proof.sbdb",
      1760000000175ull);
  const auto context = Context(database);
  SeedCatalog(context);

  int dispatch_count = 0;
  agents::AgentProductionRouteProofInputs missing_proofs;
  const auto sealed = api::BuildEngineOwnedAgentActuatorRegistry(
      context, RealRegistry(&dispatch_count), std::move(missing_proofs));
  Require(!sealed.status.ok &&
              sealed.status.diagnostic_code ==
                  "SB_AGENT_ACTION_STORE.ROUTE_PROOF_REQUIRED",
          "engine-owned registry accepted live provider without route proof");
  Require(dispatch_count == 0,
          "route-proof registry build executed provider unexpectedly");

  Cleanup(database.path);
}

void TestStoreBackedDispatchRequiresStrictMetricsBeforeReservation() {
  const auto database = CreateActiveDatabase(
      "scratchbird_aeic_agent_action_strict_metrics.sbdb",
      1760000000180ull);
  const auto context = Context(database);
  SeedCatalog(context);

  int dispatch_count = 0;
  auto registry = SealRegistry(context, RealRegistry(&dispatch_count));

  auto missing_request = StoreDispatchRequest(
      context, &registry,
      Action("018f0000-0000-7000-8000-00000000ce56",
             "idem-missing-metrics"));
  missing_request.observed_metric_snapshots.clear();
  const auto missing =
      api::DispatchAgentActionWithDurableCatalogStore(missing_request);
  Require(!missing.dispatch.status.ok &&
              missing.dispatch.status.diagnostic_code ==
                  "SB_AGENT_METRIC_SNAPSHOT.MISSING",
          "store-backed dispatch accepted missing observed metrics");
  Require(!missing.loaded_from_store && !missing.resource_reservation_acquired,
          "missing metric refusal reached durable store/reservation path");
  Require(dispatch_count == 0,
          "missing metric refusal reached provider dispatch");

  auto forged_request = StoreDispatchRequest(
      context, &registry,
      Action("018f0000-0000-7000-8000-00000000ce57",
             "idem-forged-metric-digest"));
  forged_request.action.inputs["metric_digest"] = "sha256:forged-digest";
  const auto forged =
      api::DispatchAgentActionWithDurableCatalogStore(forged_request);
  Require(!forged.dispatch.status.ok &&
              forged.dispatch.status.diagnostic_code ==
                  "SB_AGENT_COMMERCIAL_EVIDENCE.METRIC_DIGEST_MISMATCH",
          "store-backed dispatch accepted a forged metric digest");
  Require(!forged.loaded_from_store && !forged.resource_reservation_acquired,
          "forged metric refusal reached durable store/reservation path");
  Require(dispatch_count == 0,
          "forged metric refusal reached provider dispatch");

  const auto loaded = api::LoadAgentDurableCatalogImage(context, true);
  Require(loaded.ok, "catalog reload after metric refusal failed");
  Require(loaded.image.actions.empty(),
          "metric refusal wrote a durable action");
  Require(loaded.image.resource_reservations.empty(),
          "metric refusal leaked a durable resource reservation");

  Cleanup(database.path);
}

void TestPendingActionIntentReplaysAfterCrash() {
  const auto database = CreateActiveDatabase(
      "scratchbird_aeic_agent_action_pending_replay.sbdb",
      1760000000185ull);
  const auto context = Context(database);
  SeedCatalog(context);

  auto loaded = api::LoadAgentDurableCatalogImage(context, true);
  Require(loaded.ok, "catalog load before pending replay seed failed");
  const auto pending_action = Action(
      "018f0000-0000-7000-8000-00000000ce58",
      "idem-pending-replay");

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
      "018f0000-0000-7000-8000-00000000ce59";
  pending.diagnostic_code = "SB_AGENT_ACTION_DISPATCH.PENDING_INTENT";
  pending.generation = 1;
  loaded.image.actions.push_back(std::move(pending));

  api::AgentDurableCatalogStoreRequest persist_pending;
  persist_pending.context = context;
  persist_pending.image = loaded.image;
  persist_pending.evidence_uuid =
      "018f0000-0000-7000-8000-00000000ce5a";
  persist_pending.production_live_path = true;
  persist_pending.fsync_or_checkpoint_evidence = true;
  auto persisted_pending =
      api::PersistAgentDurableCatalogImage(persist_pending);
  Require(persisted_pending.ok, "pending action intent persist failed");

  auto recovered = persisted_pending.image;
  const auto recovery = agents::RecoverDurableAgentCatalogAfterCrash(
      &recovered,
      1760000000003000ull,
      "018f0000-0000-7000-8000-00000000ce5b");
  Require(recovery.ok, "pending action recovery failed: " +
                           recovery.diagnostic_code);

  api::AgentDurableCatalogStoreRequest persist_replay;
  persist_replay.context = context;
  persist_replay.image = recovered;
  persist_replay.evidence_uuid =
      "018f0000-0000-7000-8000-00000000ce5c";
  persist_replay.production_live_path = true;
  persist_replay.fsync_or_checkpoint_evidence = true;
  auto persisted_replay =
      api::PersistAgentDurableCatalogImage(persist_replay);
  Require(persisted_replay.ok, "replay-pending action persist failed");

  const auto replay_loaded = api::LoadAgentDurableCatalogImage(context, true);
  Require(replay_loaded.ok, "catalog reload after pending replay failed");
  Require(replay_loaded.image.actions.size() == 1,
          "pending replay changed action cardinality");
  Require(replay_loaded.image.actions.front().state ==
              agents::DurableAgentActionState::replay_pending,
          "pending action intent did not replay after crash");
  bool saw_action_replay_history = false;
  for (const auto& history : replay_loaded.image.retained_history) {
    saw_action_replay_history =
        saw_action_replay_history ||
        history.event_kind == "action_replay_pending";
  }
  Require(saw_action_replay_history,
          "pending action replay history was not retained");

  Cleanup(database.path);
}

void TestActionEvidenceRollbackAndCommitVisibility() {
  const auto database = CreateActiveDatabase(
      "scratchbird_aeic_agent_action_evidence_visibility.sbdb",
      1760000000190ull);
  auto seed_context = Context(database);
  SeedCatalog(seed_context);
  CommitContext(seed_context);

  int rollback_dispatch_count = 0;
  auto rollback_context =
      BeginTransactionContext(database, "aeic-action-evidence-rollback");
  auto rollback_registry =
      SealRegistry(rollback_context, RealRegistry(&rollback_dispatch_count));
  auto rollback_action = Action(
      "018f0000-0000-7000-8000-00000000ce80",
      "idem-aeic041-evidence-rollback");
  rollback_action.inputs["expected_policy_generation"] = "7";
  const auto rollback_result = api::DispatchAgentActionWithDurableCatalogStore(
      StoreDispatchRequest(rollback_context,
                           &rollback_registry,
                           rollback_action));
  Require(rollback_result.dispatch.status.ok,
          "rollback-scope action dispatch failed: " +
              rollback_result.dispatch.status.diagnostic_code);
  Require(rollback_result.dispatch.commercial_evidence_written_before_action_record,
          "rollback-scope dispatch did not write evidence before action record");
  Require(rollback_dispatch_count == 1,
          "rollback-scope dispatch did not call provider exactly once");
  auto own_load = api::LoadAgentDurableCatalogImage(rollback_context, true);
  Require(own_load.ok, "own transaction catalog load after dispatch failed");
  Require(own_load.image.actions.size() == 1 &&
              own_load.image.evidence.size() == 1,
          "own transaction did not observe action/evidence writes");
  RollbackContext(rollback_context);

  auto verify_rollback_context =
      BeginTransactionContext(database, "aeic-action-evidence-rollback-load");
  auto rolled_back = api::LoadAgentDurableCatalogImage(verify_rollback_context,
                                                       true);
  Require(rolled_back.ok, "catalog load after rollback failed");
  Require(rolled_back.image.actions.empty(),
          "rolled-back action remained visible");
  Require(rolled_back.image.evidence.empty(),
          "rolled-back evidence remained visible");
  Require(rolled_back.image.resource_reservations.empty(),
          "rolled-back resource reservation remained visible");
  CommitContext(verify_rollback_context);

  int commit_dispatch_count = 0;
  auto commit_context =
      BeginTransactionContext(database, "aeic-action-evidence-commit");
  auto commit_registry =
      SealRegistry(commit_context, RealRegistry(&commit_dispatch_count));
  auto commit_action = Action(
      "018f0000-0000-7000-8000-00000000ce81",
      "idem-aeic041-evidence-commit");
  commit_action.inputs["expected_policy_generation"] = "7";
  const auto commit_result = api::DispatchAgentActionWithDurableCatalogStore(
      StoreDispatchRequest(commit_context, &commit_registry, commit_action));
  Require(commit_result.dispatch.status.ok,
          "commit-scope action dispatch failed: " +
              commit_result.dispatch.status.diagnostic_code);
  CommitContext(commit_context);

  auto verify_commit_context =
      BeginTransactionContext(database, "aeic-action-evidence-commit-load");
  auto committed = api::LoadAgentDurableCatalogImage(verify_commit_context,
                                                     true);
  Require(committed.ok, "catalog load after commit failed");
  Require(committed.image.actions.size() == 1,
          "committed action was not visible");
  Require(committed.image.evidence.size() == 1,
          "committed commercial evidence was not visible");
  Require(committed.image.actions.front().state ==
              agents::DurableAgentActionState::completed,
          "committed action was not completed");
  Require(!committed.image.resource_reservations.empty() &&
              committed.image.resource_reservations.front().state ==
                  agents::DurableAgentResourceReservationState::released,
          "committed resource reservation was not released");
  CommitContext(verify_commit_context);

  Cleanup(database.path);
}

void TestApprovalCancellationAndPolicyGenerationRaces() {
  const auto database = CreateActiveDatabase(
      "scratchbird_aeic_agent_action_race_windows.sbdb",
      1760000000195ull);
  auto seed_context = Context(database);
  SeedCatalog(seed_context,
              CatalogImageWithPendingActionAndPolicy(),
              "018f0000-0000-7000-8000-00000000ce75");
  CommitContext(seed_context);

  auto approval_a_context =
      BeginTransactionContext(database, "aeic-approval-race-a");
  auto approval_b_context =
      BeginTransactionContext(database, "aeic-approval-race-b");
  auto approval_a = api::LoadAgentDurableCatalogImage(approval_a_context, true);
  auto approval_b = api::LoadAgentDurableCatalogImage(approval_b_context, true);
  Require(approval_a.ok && approval_b.ok, "approval race preloads failed");

  agents::DurableAgentApprovalRequest approve;
  approve.approval_uuid = "018f0000-0000-7000-8000-00000000ce76";
  approve.action_uuid = "018f0000-0000-7000-8000-00000000ce73";
  approve.principal_uuid = "018f0000-0000-7000-8000-00000000ce77";
  approve.evidence_uuid = "018f0000-0000-7000-8000-00000000ce78";
  approve.approved_at_microseconds = 1760000000195001ull;
  approve.approved = true;
  auto approval_status =
      agents::RecordDurableAgentApproval(&approval_a.image, approve);
  Require(approval_status.ok, "approval race primary approval failed");
  api::AgentDurableCatalogStoreRequest persist_approval_a;
  persist_approval_a.context = approval_a_context;
  persist_approval_a.image = approval_a.image;
  persist_approval_a.evidence_uuid = approve.evidence_uuid;
  persist_approval_a.production_live_path = true;
  persist_approval_a.fsync_or_checkpoint_evidence = true;
  auto persisted_approval_a =
      api::PersistAgentDurableCatalogImage(persist_approval_a);
  Require(persisted_approval_a.ok, "approval race primary persist failed");
  CommitContext(approval_a_context);

  auto deny = approve;
  deny.approval_uuid = "018f0000-0000-7000-8000-00000000ce79";
  deny.principal_uuid = "018f0000-0000-7000-8000-00000000ce7a";
  deny.evidence_uuid = "018f0000-0000-7000-8000-00000000ce7b";
  deny.approved = false;
  auto deny_status =
      agents::RecordDurableAgentApproval(&approval_b.image, deny);
  Require(deny_status.ok,
          "stale approval image should be internally consistent before store conflict");
  api::AgentDurableCatalogStoreRequest persist_approval_b;
  persist_approval_b.context = approval_b_context;
  persist_approval_b.image = approval_b.image;
  persist_approval_b.evidence_uuid = deny.evidence_uuid;
  persist_approval_b.production_live_path = true;
  persist_approval_b.fsync_or_checkpoint_evidence = true;
  auto persisted_approval_b =
      api::PersistAgentDurableCatalogImage(persist_approval_b);
  Require(!persisted_approval_b.ok &&
              persisted_approval_b.diagnostic.detail.find(
                  "catalog_root_digest_stale_write_refused") !=
                  std::string::npos,
          "stale conflicting approval was not refused by catalog root guard: " +
              std::string(persisted_approval_b.ok ? "ok" : "refused") + ":" +
              persisted_approval_b.diagnostic.detail);
  RollbackContext(approval_b_context);

  auto cancellation_a_context =
      BeginTransactionContext(database, "aeic-cancel-race-a");
  auto cancellation_b_context =
      BeginTransactionContext(database, "aeic-cancel-race-b");
  auto cancellation_a =
      api::LoadAgentDurableCatalogImage(cancellation_a_context, true);
  auto cancellation_b =
      api::LoadAgentDurableCatalogImage(cancellation_b_context, true);
  Require(cancellation_a.ok && cancellation_b.ok,
          "cancellation race preloads failed");

  agents::DurableAgentActionCancellationRequest cancel;
  cancel.action_uuid = "018f0000-0000-7000-8000-00000000ce73";
  cancel.principal_uuid = "018f0000-0000-7000-8000-00000000ce7c";
  cancel.evidence_uuid = "018f0000-0000-7000-8000-00000000ce7d";
  cancel.reason = "operator_cancelled_after_approval";
  cancel.cancelled_at_microseconds = 1760000000195002ull;
  cancel.operator_approved = true;
  auto cancel_status =
      agents::CancelDurableAgentAction(&cancellation_a.image, cancel);
  Require(cancel_status.ok, "action cancellation failed");
  api::AgentDurableCatalogStoreRequest persist_cancel_a;
  persist_cancel_a.context = cancellation_a_context;
  persist_cancel_a.image = cancellation_a.image;
  persist_cancel_a.evidence_uuid = cancel.evidence_uuid;
  persist_cancel_a.production_live_path = true;
  persist_cancel_a.fsync_or_checkpoint_evidence = true;
  auto persisted_cancel_a =
      api::PersistAgentDurableCatalogImage(persist_cancel_a);
  Require(persisted_cancel_a.ok, "action cancellation persist failed");
  CommitContext(cancellation_a_context);

  auto stale_cancel = cancel;
  stale_cancel.evidence_uuid = "018f0000-0000-7000-8000-00000000ce7e";
  stale_cancel.cancelled_at_microseconds = 1760000000195003ull;
  auto stale_cancel_status =
      agents::CancelDurableAgentAction(&cancellation_b.image, stale_cancel);
  Require(stale_cancel_status.ok,
          "stale cancellation image should be internally cancellable");
  api::AgentDurableCatalogStoreRequest persist_cancel_b;
  persist_cancel_b.context = cancellation_b_context;
  persist_cancel_b.image = cancellation_b.image;
  persist_cancel_b.evidence_uuid = stale_cancel.evidence_uuid;
  persist_cancel_b.production_live_path = true;
  persist_cancel_b.fsync_or_checkpoint_evidence = true;
  auto persisted_cancel_b =
      api::PersistAgentDurableCatalogImage(persist_cancel_b);
  Require(!persisted_cancel_b.ok &&
              persisted_cancel_b.diagnostic.detail.find(
                  "catalog_root_digest_stale_write_refused") !=
                  std::string::npos,
          "stale cancellation write was not refused by catalog root guard");
  RollbackContext(cancellation_b_context);

  auto policy_context =
      BeginTransactionContext(database, "aeic-policy-generation-update");
  auto policy_image = api::LoadAgentDurableCatalogImage(policy_context, true);
  Require(policy_image.ok, "policy generation image load failed");
  auto updated_policy = policy_image.image.policies.front();
  updated_policy.policy_generation = 8;
  updated_policy.require_manual_approval = false;
  agents::DurableAgentPolicyUpdateRequest policy_update;
  policy_update.policy = updated_policy;
  policy_update.agent_type_id = "page_allocation_manager";
  policy_update.principal_uuid = "018f0000-0000-7000-8000-00000000ce7f";
  policy_update.evidence_uuid = "018f0000-0000-7000-8000-00000000ce82";
  policy_update.expected_previous_generation = 7;
  policy_update.updated_at_microseconds = 1760000000195004ull;
  policy_update.operator_approved = true;
  auto policy_status =
      agents::ApplyDurableAgentPolicyUpdate(&policy_image.image,
                                            policy_update);
  Require(policy_status.ok, "policy generation update failed: " +
                                policy_status.diagnostic_code);
  api::AgentDurableCatalogStoreRequest persist_policy;
  persist_policy.context = policy_context;
  persist_policy.image = policy_image.image;
  persist_policy.evidence_uuid = policy_update.evidence_uuid;
  persist_policy.production_live_path = true;
  persist_policy.fsync_or_checkpoint_evidence = true;
  auto persisted_policy = api::PersistAgentDurableCatalogImage(persist_policy);
  Require(persisted_policy.ok, "policy generation update persist failed");
  CommitContext(policy_context);

  int stale_policy_dispatch_count = 0;
  auto dispatch_context =
      BeginTransactionContext(database, "aeic-policy-generation-dispatch");
  auto registry =
      SealRegistry(dispatch_context, RealRegistry(&stale_policy_dispatch_count));
  auto stale_policy_action = Action(
      "018f0000-0000-7000-8000-00000000ce83",
      "idem-aeic041-stale-policy");
  stale_policy_action.inputs["expected_policy_generation"] = "7";
  const auto stale_policy_result =
      api::DispatchAgentActionWithDurableCatalogStore(
          StoreDispatchRequest(dispatch_context,
                               &registry,
                               stale_policy_action));
  Require(!stale_policy_result.dispatch.status.ok &&
              stale_policy_result.dispatch.status.diagnostic_code ==
                  "SB_AGENT_ACTION.POLICY_GENERATION_CHANGED",
          "stale policy-generation action was not refused");
  Require(stale_policy_dispatch_count == 0,
          "stale policy-generation action reached provider dispatch");
  Require(!stale_policy_result.resource_reservation_acquired &&
              !stale_policy_result.pending_action_intent_persisted_before_dispatch &&
              !stale_policy_result.persisted_to_store,
          "stale policy-generation refusal mutated durable action state");
  RollbackContext(dispatch_context);

  auto verify_context =
      BeginTransactionContext(database, "aeic-race-window-verify");
  const auto verified = api::LoadAgentDurableCatalogImage(verify_context, true);
  Require(verified.ok, "race-window final catalog load failed");
  Require(verified.image.approvals.size() == 1 &&
              verified.image.approvals.front().approved,
          "approval race did not preserve exactly one approved record");
  Require(verified.image.actions.size() == 1 &&
              verified.image.actions.front().state ==
                  agents::DurableAgentActionState::cancelled,
          "cancellation race did not preserve cancelled action");
  Require(verified.image.instances.front().policy_generation == 8,
          "policy generation update did not reach instance");
  bool saw_approval = false;
  bool saw_cancel = false;
  bool saw_policy = false;
  for (const auto& history : verified.image.retained_history) {
    saw_approval =
        saw_approval || history.event_kind == "action_approval_recorded";
    saw_cancel = saw_cancel || history.event_kind == "action_cancelled";
    saw_policy =
        saw_policy || history.event_kind == "policy_generation_updated";
  }
  Require(saw_approval, "approval race history missing");
  Require(saw_cancel, "cancellation race history missing");
  Require(saw_policy, "policy generation race history missing");
  CommitContext(verify_context);

  Cleanup(database.path);
}

void TestStoreBackedDispatchRequiresCheckpointEvidence() {
  const auto database = CreateActiveDatabase(
      "scratchbird_aeic_agent_action_checkpoint.sbdb",
      1760000000200ull);
  const auto context = Context(database);
  SeedCatalog(context);

  int dispatch_count = 0;
  auto registry = SealRegistry(context, RealRegistry(&dispatch_count));
  auto request = StoreDispatchRequest(
      context, &registry,
      Action("018f0000-0000-7000-8000-00000000ce60",
             "idem-missing-checkpoint"));
  request.fsync_or_checkpoint_evidence = false;
  const auto result = api::DispatchAgentActionWithDurableCatalogStore(request);
  Require(!result.dispatch.status.ok &&
              result.dispatch.status.diagnostic_code ==
                  "SB_AGENT_ACTION_STORE.RESOURCE_RESERVATION_PERSIST_FAILED",
          "store-backed dispatch accepted missing checkpoint evidence");

  const auto loaded = api::LoadAgentDurableCatalogImage(context, true);
  Require(loaded.ok, "catalog reload after checkpoint refusal failed");
  Require(loaded.image.actions.empty(),
          "failed checkpoint persist mutated durable catalog");
  Require(loaded.image.resource_reservations.empty(),
          "failed checkpoint persist leaked resource reservation");

  Cleanup(database.path);
}

}  // namespace

int main() {
  ConfigureMemoryFixture();
  TestStoreBackedLiveDispatchPersistsAction();
  TestDefaultRegistryCannotSimulateLiveMutation();
  TestCallerSuppliedRegistryRefusedWithoutEngineProvenance();
  TestProductionDispatchRequiresSecurityContextBeforeMutation();
  TestEngineRegistryRequiresRouteProof();
  TestStoreBackedDispatchRequiresStrictMetricsBeforeReservation();
  TestPendingActionIntentReplaysAfterCrash();
  TestActionEvidenceRollbackAndCommitVisibility();
  TestApprovalCancellationAndPolicyGenerationRaces();
  TestStoreBackedDispatchRequiresCheckpointEvidence();
  return EXIT_SUCCESS;
}
