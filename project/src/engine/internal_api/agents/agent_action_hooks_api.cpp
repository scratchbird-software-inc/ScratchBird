// Copyright (c) 2026 ScratchBird Software Inc.
//
// This Source Code Form is subject to the terms of the Mozilla Public
// License, v. 2.0. If a copy of the MPL was not distributed with this
// file, You can obtain one at https://mozilla.org/MPL/2.0/.
//
// SPDX-License-Identifier: MPL-2.0

#include "agents/agent_action_hooks_api.hpp"

#include "agents/agent_durable_catalog_store_api.hpp"
#include "agent_metric_runtime.hpp"
#include "agent_runtime.hpp"
#include "api_diagnostics.hpp"
#include "behavior_support/api_behavior_store.hpp"
#include "filespace_growth.hpp"
#include "filespace_header.hpp"
#include "metric_contracts.hpp"
#include "page_allocation_lifecycle.hpp"
#include "security/security_model.hpp"
#include "uuid.hpp"

#include <algorithm>
#include <cstdint>
#include <filesystem>
#include <map>
#include <mutex>
#include <string>
#include <utility>
#include <vector>

namespace scratchbird::engine::internal_api {
namespace {

namespace filespace = scratchbird::storage::filespace;
namespace page = scratchbird::storage::page;
namespace platform = scratchbird::core::platform;
namespace uuid = scratchbird::core::uuid;

bool Empty(const EngineUuid& uuid) {
  return uuid.canonical.empty();
}

bool Empty(const EngineObjectReference& object) {
  return object.uuid.canonical.empty();
}

using scratchbird::core::agents::AgentActionClass;
using scratchbird::core::agents::AgentActionClassName;
using scratchbird::core::agents::AgentActionRequest;
using scratchbird::core::agents::AgentActionResultClass;
using scratchbird::core::agents::AgentActionResultClassName;
using scratchbird::core::agents::AgentActivationProfile;
using scratchbird::core::agents::AgentActivationProfileName;
using scratchbird::core::agents::AgentAuthorityClass;
using scratchbird::core::agents::AgentRuntimeContext;
using scratchbird::core::agents::BaselinePolicyForAgent;
using scratchbird::core::agents::EffectiveActivationForLifecycle;
using scratchbird::core::agents::EvaluateAgentAction;
using scratchbird::core::agents::EvaluateAgentFeatureAvailability;
using scratchbird::core::agents::FindAgentType;
using scratchbird::core::agents::AgentMetricRuntimeMode;
using scratchbird::core::agents::AgentMetricSnapshotEvaluationOptions;
using scratchbird::core::agents::AgentMetricSourceQuality;
using scratchbird::core::agents::AgentObservedMetricSnapshot;
using scratchbird::core::agents::EvaluateAgentObservedMetricSnapshots;
using scratchbird::core::agents::RecordAgentRuntimeMetric;
using scratchbird::core::agents::ResolveAgentMetricDependencies;
using scratchbird::core::agents::ValidateAgentPolicy;
using scratchbird::core::agents::ValidateAgentSecurity;
using scratchbird::core::agents::AcquireDurableAgentResourceReservation;
using scratchbird::core::agents::ReleaseDurableAgentResourceReservation;
using scratchbird::core::agents::DurableAgentResourceReservationRequest;
using scratchbird::core::agents::DurableAgentResourceReservationState;
using scratchbird::core::agents::DeterministicAgentRuntimeObjectUuidFromKey;

struct PagePreallocationRouteRuntime {
  page::PageAllocationLedger ledger;
  platform::u64 next_start_page = 400000;
};

struct FilespaceGrowthRouteRuntime {
  filespace::FilespaceGrowthLedger ledger;
  filespace::FilespaceRegistry registry;
  platform::TypedUuid storage_profile_uuid;
  platform::TypedUuid file_member_uuid;
  platform::u64 member_start_page = 800000;
  std::string physical_member_path;
};

std::mutex& AgentStorageRouteMutex() {
  static std::mutex mutex;
  return mutex;
}

std::map<std::string, PagePreallocationRouteRuntime>& PagePreallocationRouteLedgers() {
  static std::map<std::string, PagePreallocationRouteRuntime> ledgers;
  return ledgers;
}

std::map<std::string, FilespaceGrowthRouteRuntime>& FilespaceGrowthRouteLedgers() {
  static std::map<std::string, FilespaceGrowthRouteRuntime> ledgers;
  return ledgers;
}

platform::u64 StableSeed(const std::string& value, platform::u64 salt);

std::string LedgerKey(const EngineRequestContext& context,
                      const EngineObjectReference& filespace) {
  const std::string database = context.database_uuid.canonical.empty()
                                   ? std::string("database_uuid_absent")
                                   : context.database_uuid.canonical;
  const std::string path = context.database_path.empty() ? std::string("database_path_absent")
                                                         : context.database_path;
  return path + "|" + database + "|" + filespace.uuid.canonical;
}

std::string FilespaceGrowthPhysicalMemberPath(const EngineRequestContext& context,
                                              const EngineObjectReference& filespace,
                                              const std::string& key) {
  std::filesystem::path base;
  if (!context.database_path.empty()) {
    base = std::filesystem::path(context.database_path).parent_path();
  }
  if (base.empty()) {
    base = std::filesystem::temp_directory_path() / "scratchbird_agent_filespace_growth";
  }
  std::error_code ignored;
  std::filesystem::create_directories(base, ignored);
  const auto seed = StableSeed(key + "|" + filespace.uuid.canonical, 59);
  return (base / ("scratchbird_filespace_growth_" + std::to_string(seed) + ".sbfs")).string();
}

platform::u64 StableSeed(const std::string& value, platform::u64 salt) {
  platform::u64 hash = 1469598103934665603ull ^ salt;
  for (const unsigned char c : value) {
    hash ^= static_cast<platform::u64>(c);
    hash *= 1099511628211ull;
  }
  return 1900000000000ull + (hash % 1000000000ull);
}

platform::TypedUuid GeneratedIdentity(platform::UuidKind kind,
                                      const std::string& key,
                                      platform::u64 salt) {
  const auto generated = uuid::GenerateEngineIdentityV7(kind, StableSeed(key, salt));
  return generated.ok() ? generated.value : platform::TypedUuid{};
}

bool IsEngineIdentity(const platform::TypedUuid& typed, platform::UuidKind kind) {
  return typed.kind == kind && typed.valid() && uuid::IsEngineIdentityUuid(typed.value);
}

platform::TypedUuid ParseEngineIdentity(platform::UuidKind kind, const std::string& text) {
  const auto parsed = uuid::ParseTypedUuid(kind, text);
  if (!parsed.ok() || !IsEngineIdentity(parsed.value, kind)) {
    return {};
  }
  return parsed.value;
}

std::string UuidText(const platform::TypedUuid& typed) {
  return typed.valid() ? uuid::UuidToString(typed.value) : std::string{};
}

platform::u64 ParseU64(const std::string& value, platform::u64 fallback) {
  if (value.empty()) {
    return fallback;
  }
  try {
    return static_cast<platform::u64>(std::stoull(value));
  } catch (...) {
    return fallback;
  }
}

platform::u64 OptionU64(const EngineApiRequest& request,
                        const std::string& prefix,
                        platform::u64 fallback) {
  return ParseU64(SecurityOptionValue(request, prefix), fallback);
}

bool IsKnownPageFamilyForPreallocation(const std::string& family) {
  return family == "data" || family == "index" || family == "blob" ||
         family == "overflow" || family == "toast" || family == "catalog" ||
         family == "metrics" || family == "transaction_inventory";
}

platform::TypedUuid PolicyUuidForStorageRoute(const EngineAgentActionHookRequest& request,
                                              const std::string& key) {
  const auto parsed = ParseEngineIdentity(platform::UuidKind::object,
                                          request.policy_snapshot_uuid.canonical);
  return parsed.valid() ? parsed : GeneratedIdentity(platform::UuidKind::object, key, 17);
}

platform::u32 FilespacePageSizeBytes(const EngineAgentActionHookRequest& request) {
  const auto page_size = OptionU64(request, "filespace.page_size_bytes:", 16384);
  if (page_size == 0 || page_size > UINT32_MAX) {
    return 0;
  }
  return static_cast<platform::u32>(page_size);
}

platform::u64 RequestedGrowthPages(const EngineRequestFilespaceGrowthRequest& request,
                                   platform::u32 page_size) {
  if (request.requested_pages != 0) {
    return request.requested_pages;
  }
  if (page_size == 0 || request.requested_bytes == 0) {
    return 0;
  }
  return (request.requested_bytes + page_size - 1) / page_size;
}

EngineApiDiagnostic DiagnosticFromPagePreallocation(
    const page::PageAllocationResult& storage) {
  std::string detail = storage.diagnostic.message_key;
  for (const auto& argument : storage.diagnostic.arguments) {
    if (argument.key == "detail" && !argument.value.empty()) {
      detail += ":" + argument.value;
    }
  }
  return MakeEngineApiDiagnostic(
      storage.diagnostic.diagnostic_code.empty()
          ? "SB-STORAGE-PAGE-PREALLOCATION-ROUTE-FAILED"
          : storage.diagnostic.diagnostic_code,
      storage.diagnostic.message_key.empty()
          ? "storage.page_allocation.preallocation_route_failed"
          : storage.diagnostic.message_key,
      std::move(detail),
      true);
}

EngineApiDiagnostic DiagnosticFromFilespaceGrowth(
    const filespace::FilespacePhysicalGrowthResult& storage) {
  std::string detail = storage.diagnostic.message_key;
  for (const auto& argument : storage.diagnostic.arguments) {
    if (argument.key == "detail" && !argument.value.empty()) {
      detail += ":" + argument.value;
    }
  }
  return MakeEngineApiDiagnostic(
      storage.diagnostic.diagnostic_code.empty()
          ? "SB-STORAGE-FILESPACE-GROWTH-ROUTE-FAILED"
          : storage.diagnostic.diagnostic_code,
      storage.diagnostic.message_key.empty()
          ? "storage.filespace.growth_route_failed"
          : storage.diagnostic.message_key,
      std::move(detail),
      true);
}

EngineObjectReference TargetForEvidence(const EngineAgentActionHookRequest& request) {
  if (!Empty(request.target_index)) {
    return request.target_index;
  }
  if (!Empty(request.target_filespace)) {
    return request.target_filespace;
  }
  return request.target_object;
}

std::string OptionValue(const EngineApiRequest& request, const std::string& prefix) {
  return SecurityOptionValue(request, prefix);
}

bool OptionPresent(const EngineApiRequest& request, const std::string& value) {
  return SecurityOptionPresent(request, value);
}

AgentMetricSourceQuality MetricSourceQualityFromText(std::string value) {
  value = SecurityLower(std::move(value));
  if (value == "trusted") { return AgentMetricSourceQuality::trusted; }
  if (value == "cluster_confirmed") {
    return AgentMetricSourceQuality::cluster_confirmed;
  }
  return AgentMetricSourceQuality::unknown;
}

bool StrictObservedMetricSnapshotEvidencePresent(
    const EngineAgentActionHookRequest& request) {
  const auto digest = OptionValue(request, "agent_metric_snapshot_digest:");
  const auto snapshot_id = OptionValue(request, "agent_metric_snapshot_id:");
  const auto evidence_uuid =
      OptionValue(request, "agent_metric_snapshot_evidence_uuid:");
  const auto source_quality =
      SecurityLower(OptionValue(request, "agent_metric_snapshot_source_quality:"));
  return SecurityOptionBool(request, "agent_metric_snapshot_observed:", false) &&
         SecurityOptionBool(request, "agent_metric_snapshot_trusted:", false) &&
         !digest.empty() && !snapshot_id.empty() && !evidence_uuid.empty() &&
         (source_quality == "trusted" ||
          source_quality == "cluster_confirmed");
}

AgentRuntimeContext AgentContextFromHookRequest(const EngineAgentActionHookRequest& request) {
  AgentRuntimeContext context;
  context.security_context_present = request.context.security_context_present;
  context.cluster_authority_available = request.context.cluster_authority_available;
  context.cluster_time_majority_available =
      SecurityOptionBool(request, "cluster_time_majority:", request.context.cluster_authority_available);
  context.private_features_available = SecurityOptionBool(request, "private_features:", true);
  context.standalone_edition = SecurityOptionBool(request, "standalone_edition:", !request.context.cluster_authority_available);
  context.shutdown_requested = OptionPresent(request, "lifecycle:shutdown");
  context.read_only_mode = OptionPresent(request, "lifecycle:read_only");
  context.maintenance_mode = OptionPresent(request, "lifecycle:maintenance");
  context.restricted_open_mode = OptionPresent(request, "lifecycle:restricted_open");
  context.repair_mode = OptionPresent(request, "lifecycle:repair");
  context.backup_hold_mode = OptionPresent(request, "lifecycle:backup") ||
                             OptionPresent(request, "lifecycle:restore");
  context.archive_hold_mode = OptionPresent(request, "lifecycle:archive_hold");
  context.principal_uuid = request.context.principal_uuid.canonical;
  context.database_uuid = request.context.database_uuid.canonical;
  context.cluster_uuid = request.context.cluster_uuid.canonical;
  for (const auto& tag : request.context.trace_tags) {
    if (tag.rfind("right:", 0) == 0) { context.rights.push_back(tag.substr(6)); }
    if (tag.rfind("group:", 0) == 0) { context.groups.push_back(tag.substr(6)); }
    context.trace_tags.push_back(tag);
  }
  if (SecurityContextHasTag(request.context, "security.bootstrap")) { context.trace_tags.push_back("security.bootstrap"); }
  const auto wall = OptionValue(request, "wall_now_us:");
  const auto mono = OptionValue(request, "monotonic_now_us:");
  const auto cluster = OptionValue(request, "cluster_now_us:");
  try {
    if (!wall.empty()) { context.wall_now_microseconds = static_cast<std::uint64_t>(std::stoull(wall)); }
    if (!mono.empty()) { context.monotonic_now_microseconds = static_cast<std::uint64_t>(std::stoull(mono)); }
    if (!cluster.empty()) { context.cluster_now_microseconds = static_cast<std::uint64_t>(std::stoull(cluster)); }
  } catch (...) {
    context.wall_now_microseconds = 0;
    context.monotonic_now_microseconds = 0;
    context.cluster_now_microseconds = 0;
  }
  return context;
}

bool DurableResourceReservationRequiredForHook(
    const EngineAgentActionHookRequest& request) {
  return !request.dry_run &&
         (SecurityOptionBool(request, "agent_action_hook_production_live:", false) ||
          SecurityOptionBool(request,
                             "agent_durable_resource_reservation_required:",
                             false));
}

bool DurableCatalogStoreContextAvailableForHook(
    const EngineAgentActionHookRequest& request) {
  return !request.context.database_path.empty() &&
         !request.context.transaction_uuid.canonical.empty() &&
         request.context.local_transaction_id != 0;
}

bool DurableCatalogStoreCheckpointEvidencePresentForHook(
    const EngineAgentActionHookRequest& request) {
  return SecurityOptionBool(
             request,
             "agent_durable_catalog_fsync_or_checkpoint_evidence:",
             false) ||
         SecurityOptionPresent(
             request,
             "agent_durable_catalog_checkpoint_evidence:true") ||
         SecurityOptionPresent(request,
                               "agent_durable_catalog_fsync_evidence:true");
}

DurableAgentResourceReservationRequest DurableResourceReservationForHook(
    const EngineAgentActionHookRequest& request,
    const std::string& operation_id,
    const std::string& normalized_action,
    const AgentRuntimeContext& context) {
  DurableAgentResourceReservationRequest reservation;
  const auto explicit_key =
      OptionValue(request, "agent_resource_reservation_key:");
  reservation.reservation_key =
      explicit_key.empty()
          ? "agent_hook:" + request.agent_type + ":" + normalized_action +
                ":" + request.context.request_id
          : explicit_key;
  reservation.reservation_uuid =
      DeterministicAgentRuntimeObjectUuidFromKey(
          "agent_hook_resource_reservation|" + reservation.reservation_key);
  reservation.owner_scope = context.principal_uuid.empty()
                                ? request.context.principal_uuid.canonical
                                : context.principal_uuid;
  reservation.agent_type_id = request.agent_type;
  reservation.operation_id = operation_id;
  reservation.now_microseconds = context.wall_now_microseconds == 0
                                     ? 1
                                     : context.wall_now_microseconds;
  reservation.memory_bytes = OptionU64(
      request, "agent_resource_reservation_memory_bytes:", 4096);
  reservation.worker_slots = OptionU64(
      request, "agent_resource_reservation_worker_slots:", 1);
  reservation.overhead_microseconds = OptionU64(
      request, "agent_resource_reservation_overhead_us:", 1000);
  reservation.max_active_reservations = OptionU64(
      request, "agent_resource_reservation_max_active:", 1024);
  reservation.max_memory_bytes = OptionU64(
      request, "agent_resource_reservation_max_memory_bytes:",
      64 * 1024 * 1024);
  reservation.max_worker_slots = OptionU64(
      request, "agent_resource_reservation_max_worker_slots:", 8);
  reservation.max_overhead_microseconds = OptionU64(
      request, "agent_resource_reservation_max_overhead_us:",
      10 * 1000 * 1000);
  reservation.evidence_uuid =
      DeterministicAgentRuntimeObjectUuidFromKey(
          "agent_hook_resource_reservation_evidence|" +
          reservation.reservation_uuid);
  return reservation;
}

std::vector<AgentObservedMetricSnapshot> ObservedMetricSnapshotsFromHookRequest(
    const EngineAgentActionHookRequest& request,
    const scratchbird::core::agents::AgentTypeDescriptor& descriptor,
    const AgentRuntimeContext& context) {
  std::vector<AgentObservedMetricSnapshot> snapshots;
  if (!StrictObservedMetricSnapshotEvidencePresent(request)) {
    return snapshots;
  }
  const auto digest = OptionValue(request, "agent_metric_snapshot_digest:");
  const auto snapshot_id = OptionValue(request, "agent_metric_snapshot_id:");
  const auto evidence_uuid =
      OptionValue(request, "agent_metric_snapshot_evidence_uuid:");
  const auto trust_provenance =
      OptionValue(request, "agent_metric_snapshot_trust_provenance:").empty()
          ? "engine_metric_registry"
          : OptionValue(request, "agent_metric_snapshot_trust_provenance:");
  const auto source_quality = MetricSourceQualityFromText(
      OptionValue(request, "agent_metric_snapshot_source_quality:"));
  const auto schema_digest =
      OptionValue(request, "agent_metric_snapshot_schema_digest:");
  const auto value_digest =
      OptionValue(request, "agent_metric_snapshot_value_digest:");
  const auto source_id =
      OptionValue(request, "agent_metric_snapshot_source_id:");
  const auto attestation_key_id =
      OptionValue(request, "agent_metric_snapshot_attestation_key_id:");
  const auto attestation_digest =
      OptionValue(request, "agent_metric_snapshot_attestation_digest:");
  const auto provenance_record =
      OptionValue(request, "agent_metric_snapshot_provenance_record:");
  const platform::u64 source_sequence =
      OptionU64(request, "agent_metric_snapshot_source_sequence:", 1);
  const platform::u64 source_count =
      std::max<platform::u64>(
          1,
          std::min<platform::u64>(
              8,
              OptionU64(request, "agent_metric_snapshot_source_count:", 1)));
  const auto scope_uuid =
      OptionValue(request, "agent_metric_snapshot_scope_uuid:").empty()
          ? context.database_uuid
          : OptionValue(request, "agent_metric_snapshot_scope_uuid:");
  platform::u64 generation = request.context.resource_epoch == 0
                                 ? 1
                                 : request.context.resource_epoch;
  if (const auto value = OptionValue(request, "agent_metric_snapshot_generation:");
      !value.empty()) {
    try { generation = static_cast<platform::u64>(std::stoull(value)); } catch (...) {}
  }
  platform::u64 observed_wall = context.wall_now_microseconds == 0
                                    ? 1
                                    : context.wall_now_microseconds;
  if (const auto value =
          OptionValue(request, "agent_metric_snapshot_observed_wall_us:");
      !value.empty()) {
    try { observed_wall = static_cast<platform::u64>(std::stoull(value)); } catch (...) {}
  }

  for (const auto& dependency : descriptor.metric_dependencies) {
    for (platform::u64 source_index = 0; source_index < source_count; ++source_index) {
      const auto source_suffix = std::to_string(source_index + 1);
      AgentObservedMetricSnapshot snapshot;
      snapshot.metric_family = dependency.metric_family;
      snapshot.namespace_path = dependency.namespace_prefix.empty()
                                    ? dependency.metric_family
                                    : dependency.namespace_prefix + ".observed";
      snapshot.source_id =
          source_id.empty() ? std::string{} : source_id + ":" + source_suffix;
      snapshot.generation = generation;
      snapshot.source_sequence = source_sequence + source_index;
      snapshot.observed_wall_microseconds = observed_wall;
      snapshot.scope_uuid = scope_uuid;
      snapshot.digest = digest + ":" + dependency.metric_family + ":" + source_suffix;
      snapshot.value_digest =
          value_digest.empty() ? digest + ":" + dependency.metric_family
                               : value_digest + ":" + dependency.metric_family;
      snapshot.schema_digest =
          schema_digest.empty() ? std::string{} : schema_digest + ":" + dependency.metric_family;
      snapshot.source_quality = source_quality;
      snapshot.present = SecurityOptionBool(request,
                                            "agent_metric_snapshot_observed:",
                                            false);
      snapshot.trusted = SecurityOptionBool(request,
                                           "agent_metric_snapshot_trusted:",
                                           false);
      snapshot.schema_compatible =
          !SecurityOptionPresent(request,
                                 "agent_metric_snapshot_schema_incompatible:true");
      snapshot.attestation_verified =
          SecurityOptionBool(request, "agent_metric_snapshot_attestation_verified:", false);
      snapshot.redacted =
          SecurityOptionBool(request, "agent_metric_snapshot_redacted:", false);
      snapshot.protected_material_present =
          SecurityOptionBool(request, "agent_metric_snapshot_protected_material_present:", false);
      snapshot.external_provider_attested =
          SecurityOptionBool(request, "agent_metric_snapshot_external_provider_attested:", false);
      snapshot.trust_provenance = trust_provenance;
      snapshot.provenance_record = provenance_record;
      snapshot.attestation_key_id = attestation_key_id;
      snapshot.attestation_digest =
          attestation_digest.empty()
              ? std::string{}
              : attestation_digest + ":" + dependency.metric_family + ":" + source_suffix;
      snapshot.evidence_uuid =
          evidence_uuid + ":" + dependency.metric_family + ":" + source_suffix;
      snapshot.snapshot_id =
          snapshot_id + ":" + dependency.metric_family + ":" + source_suffix;
      snapshots.push_back(std::move(snapshot));
    }
  }
  return snapshots;
}

scratchbird::core::agents::AgentLifecycleMode LifecycleModeFromHookRequest(
    const EngineAgentActionHookRequest& request) {
  using scratchbird::core::agents::AgentLifecycleMode;
  if (OptionPresent(request, "lifecycle:database_create")) { return AgentLifecycleMode::database_create; }
  if (OptionPresent(request, "lifecycle:database_open")) { return AgentLifecycleMode::database_open; }
  if (OptionPresent(request, "lifecycle:database_close")) { return AgentLifecycleMode::database_close; }
  if (OptionPresent(request, "lifecycle:backup")) { return AgentLifecycleMode::backup; }
  if (OptionPresent(request, "lifecycle:restore")) { return AgentLifecycleMode::restore; }
  if (OptionPresent(request, "lifecycle:shutdown")) { return AgentLifecycleMode::shutdown; }
  if (OptionPresent(request, "lifecycle:crash_recovery")) { return AgentLifecycleMode::crash_recovery; }
  if (OptionPresent(request, "lifecycle:restricted_open")) { return AgentLifecycleMode::restricted_open; }
  if (OptionPresent(request, "lifecycle:read_only")) { return AgentLifecycleMode::read_only; }
  if (OptionPresent(request, "lifecycle:maintenance")) { return AgentLifecycleMode::maintenance; }
  if (OptionPresent(request, "lifecycle:repair")) { return AgentLifecycleMode::repair; }
  if (OptionPresent(request, "lifecycle:archive_hold")) { return AgentLifecycleMode::archive_hold; }
  return AgentLifecycleMode::normal;
}

AgentActionClass ActionClassForRequest(const EngineAgentActionHookRequest& request,
                                       AgentAuthorityClass authority) {
  const auto value = OptionValue(request, "agent_action_class:");
  if (request.dry_run || value == "dry_run") { return AgentActionClass::dry_run; }
  if (value == "recommendation") { return AgentActionClass::recommendation; }
  if (value == "direct_bounded_action") { return AgentActionClass::direct_bounded_action; }
  if (value == "override_action") { return AgentActionClass::override_action; }
  if (value == "manual_approval_required") { return AgentActionClass::manual_approval_required; }
  if (authority == AgentAuthorityClass::direct_bounded_action) { return AgentActionClass::direct_bounded_action; }
  if (authority == AgentAuthorityClass::request_action) { return AgentActionClass::request_action; }
  if (authority == AgentAuthorityClass::recommend_only) { return AgentActionClass::recommendation; }
  return AgentActionClass::none;
}

std::string HookActionUuid(const EngineAgentActionHookRequest& request,
                           const std::string& normalized_action) {
  const auto explicit_uuid = OptionValue(request, "action_uuid:");
  if (!explicit_uuid.empty()) {
    return UuidText(ParseEngineIdentity(platform::UuidKind::object, explicit_uuid));
  }
  const std::string key = !request.context.request_id.empty()
      ? request.context.request_id + "|agent_action|" + normalized_action
      : request.agent_uuid.canonical + "|agent_action|" + normalized_action;
  return UuidText(GeneratedIdentity(platform::UuidKind::object, key, 19));
}

std::string HookIdempotencyKey(const EngineAgentActionHookRequest& request,
                               const std::string& normalized_action) {
  const auto explicit_key = OptionValue(request, "idempotency_key:");
  if (!explicit_key.empty()) { return explicit_key; }
  if (!request.cooldown_key.empty()) { return request.cooldown_key + ":" + normalized_action; }
  return request.agent_uuid.canonical + ":" + request.policy_snapshot_uuid.canonical + ":" + normalized_action;
}

EngineApiDiagnostic HookRefusalDiagnostic(const std::string& operation_id,
                                          const std::string& reason) {
  if (reason == "agent_management_read_only_denied") {
    return MakeEngineApiDiagnostic("AGENT.MANAGEMENT.READ_ONLY_DENIED",
                                   "agent.action_hook.open_state_denied",
                                   reason,
                                   true);
  }
  if (reason == "agent_management_shutdown_in_progress") {
    return MakeEngineApiDiagnostic("AGENT.MANAGEMENT.SHUTDOWN_IN_PROGRESS",
                                   "agent.action_hook.open_state_denied",
                                   reason,
                                   true);
  }
  if (reason == "agent_management_restricted_denied") {
    return MakeEngineApiDiagnostic("AGENT.MANAGEMENT.RESTRICTED_DENIED",
                                   "agent.action_hook.open_state_denied",
                                   reason,
                                   true);
  }
  if (reason == "agent_management_repair_denied") {
    return MakeEngineApiDiagnostic("AGENT.MANAGEMENT.REPAIR_DENIED",
                                   "agent.action_hook.open_state_denied",
                                   reason,
                                   true);
  }
  if (reason == "agent_management_maintenance_denied") {
    return MakeEngineApiDiagnostic("AGENT.MANAGEMENT.MAINTENANCE_DENIED",
                                   "agent.action_hook.open_state_denied",
                                   reason,
                                   true);
  }
  if (reason == "agent_management_backup_hold_denied") {
    return MakeEngineApiDiagnostic("AGENT.MANAGEMENT.BACKUP_HOLD_DENIED",
                                   "agent.action_hook.open_state_denied",
                                   reason,
                                   true);
  }
  if (reason == "agent_management_archive_hold_denied") {
    return MakeEngineApiDiagnostic("AGENT.MANAGEMENT.ARCHIVE_HOLD_DENIED",
                                   "agent.action_hook.open_state_denied",
                                   reason,
                                   true);
  }
  return MakeInvalidRequestDiagnostic(operation_id, reason);
}

template <typename TResult>
TResult HookFailure(const EngineAgentActionHookRequest& request,
                    const std::string& operation_id,
                    const std::string& normalized_action,
                    const std::string& reason,
                    bool deferred = false) {
  TResult result;
  result.ok = false;
  result.operation_id = operation_id;
  result.action_accepted = false;
  result.action_deferred = deferred;
  result.dry_run = request.dry_run;
  result.refusal_reason = reason;
  result.normalized_action = normalized_action;
  result.primary_object = TargetForEvidence(request);
  result.transaction_uuid = request.context.transaction_uuid;
  result.local_transaction_id = request.context.local_transaction_id;
  result.diagnostics.push_back(HookRefusalDiagnostic(operation_id, reason));
  result.evidence.push_back({"agent_hook_refused", reason});
  if (!request.agent_type.empty()) {
    result.evidence.push_back({"agent_type", request.agent_type});
    (void)scratchbird::core::metrics::RecordAgentAction(request.agent_type, normalized_action, deferred ? "deferred" : "refused");
  }
  return result;
}

std::string ValidateRuntimeDecision(const EngineAgentActionHookRequest& request,
                                    const std::string& normalized_action,
                                    std::vector<std::pair<std::string, std::string>>* runtime_rows) {
  const auto descriptor = FindAgentType(request.agent_type);
  if (!descriptor.has_value()) { return "agent_type_unknown:" + request.agent_type; }
  const auto context = AgentContextFromHookRequest(request);
  const auto feature = EvaluateAgentFeatureAvailability(*descriptor, context);
  if (feature != scratchbird::core::agents::AgentFeatureAvailability::available) {
    return "agent_feature_unavailable:" + std::string(scratchbird::core::agents::AgentFeatureAvailabilityName(feature));
  }
  const auto security = ValidateAgentSecurity(context, *descriptor, request.dry_run ? "inspect" : "control");
  if (!security.ok) { return security.diagnostic_code + ":" + security.detail; }
  if (!request.dry_run) {
    if (!StrictObservedMetricSnapshotEvidencePresent(request)) {
      return "SB_AGENT_METRIC_SNAPSHOT.PRODUCTION_OBSERVED_SNAPSHOT_REQUIRED:" +
             descriptor->type_id;
    }
    AgentMetricSnapshotEvaluationOptions metric_options;
    metric_options.mode = AgentMetricRuntimeMode::production_strict;
    metric_options.expected_scope_uuid = context.database_uuid;
    const auto metric_evaluation = EvaluateAgentObservedMetricSnapshots(
        *descriptor,
        context,
        ObservedMetricSnapshotsFromHookRequest(request, *descriptor, context),
        metric_options);
    if (!metric_evaluation.accepted) {
      return metric_evaluation.status.diagnostic_code + ":" +
             metric_evaluation.status.detail;
    }
  }
  const auto metrics = ResolveAgentMetricDependencies(*descriptor, context);
  if (!metrics.ok) { return metrics.diagnostic_code + ":" + metrics.detail; }
  auto policy = BaselinePolicyForAgent(*descriptor);
  policy.policy_uuid = request.policy_snapshot_uuid.canonical;
  policy.activation = request.dry_run ? AgentActivationProfile::dry_run : descriptor->default_activation;
  policy.activation = EffectiveActivationForLifecycle(policy.activation, LifecycleModeFromHookRequest(request));
  if (!request.dry_run && request.policy_authorized) {
    policy.activation = AgentActivationProfile::live_action;
    policy.allow_live_action = true;
    policy.require_manual_approval = false;
    policy.require_dry_run_before_live = false;
  }
  const auto policy_status = ValidateAgentPolicy(policy, *descriptor);
  if (!policy_status.ok) { return policy_status.diagnostic_code + ":" + policy_status.detail; }
  AgentActionRequest action;
  action.action_uuid = HookActionUuid(request, normalized_action);
  action.agent_type_id = descriptor->type_id;
  action.instance_uuid = request.agent_uuid.canonical;
  action.action_class = ActionClassForRequest(request, descriptor->authority);
  action.actuator_id = descriptor->type_id;
  action.operation_id = normalized_action;
  action.idempotency_key = HookIdempotencyKey(request, normalized_action);
  action.dry_run = request.dry_run || policy.activation == AgentActivationProfile::dry_run;
  action.manual_approval_present = request.policy_authorized;
  action.inputs["action_class"] = request.action_class;
  action.inputs["page_family"] = request.page_family;
  action.inputs["page_type"] = request.page_type;
  action.inputs["target_uuid"] = TargetForEvidence(request).uuid.canonical;
  const auto decision = EvaluateAgentAction(context, *descriptor, policy, action);
  (void)RecordAgentRuntimeMetric(*descriptor, decision, 0);
  runtime_rows->push_back({"runtime_action_class", AgentActionClassName(action.action_class)});
  runtime_rows->push_back({"runtime_action_result", AgentActionResultClassName(decision.result_class)});
  runtime_rows->push_back({"runtime_diagnostic_code", decision.diagnostic_code});
  runtime_rows->push_back({"runtime_evidence_uuid", decision.evidence_uuid});
  runtime_rows->push_back({"effective_activation", AgentActivationProfileName(policy.activation)});
  if (decision.result_class == AgentActionResultClass::failed_closed ||
      decision.result_class == AgentActionResultClass::refused ||
      decision.result_class == AgentActionResultClass::approval_required ||
      decision.result_class == AgentActionResultClass::quarantined) {
    return decision.diagnostic_code + ":" + decision.detail;
  }
  if (!request.dry_run && decision.result_class == AgentActionResultClass::dry_run_only) {
    return "agent_runtime_dry_run_only_for_live_hook:" + decision.detail;
  }
  return {};
}

std::string ValidateCommon(const EngineAgentActionHookRequest& request) {
  // SEARCH_KEY: PFAR_014_AGENT_ACTION_HOOK_OPEN_MODE_AUTHORIZATION
  if (request.context.local_transaction_id == 0) { return "local_transaction_id_required"; }
  if (!request.context.security_context_present) { return "security_context_required"; }
  if (!request.dry_run &&
      (request.context.read_only_mode || OptionPresent(request, "lifecycle:read_only"))) {
    return "agent_management_read_only_denied";
  }
  if (!request.dry_run && OptionPresent(request, "lifecycle:shutdown")) {
    return "agent_management_shutdown_in_progress";
  }
  if (!request.dry_run && OptionPresent(request, "lifecycle:restricted_open")) {
    return "agent_management_restricted_denied";
  }
  if (!request.dry_run && OptionPresent(request, "lifecycle:repair")) {
    return "agent_management_repair_denied";
  }
  if (!request.dry_run && OptionPresent(request, "lifecycle:maintenance")) {
    return "agent_management_maintenance_denied";
  }
  if (!request.dry_run &&
      (OptionPresent(request, "lifecycle:backup") ||
       OptionPresent(request, "lifecycle:restore"))) {
    return "agent_management_backup_hold_denied";
  }
  if (!request.dry_run && OptionPresent(request, "lifecycle:archive_hold")) {
    return "agent_management_archive_hold_denied";
  }
  if (!request.localized_names.empty()) { return "localized_names_not_allowed_engine_boundary"; }
  if (request.agent_type.empty()) { return "agent_type_required"; }
  if (request.action_class.empty()) { return "action_class_required"; }
  if (Empty(request.agent_uuid)) { return "agent_uuid_required"; }
  if (Empty(request.policy_snapshot_uuid)) { return "policy_snapshot_uuid_required"; }
  const auto explicit_action_uuid = OptionValue(request, "action_uuid:");
  if (!explicit_action_uuid.empty() &&
      !ParseEngineIdentity(platform::UuidKind::object, explicit_action_uuid).valid()) {
    return "action_uuid_invalid";
  }
  if (!request.policy_authorized) { return "policy_authorization_required"; }
  if (!request.evidence_sink_available) { return "evidence_sink_required"; }
  if (!request.metrics_fresh) { return "metric_freshness_required"; }
  if (request.cooldown_active) { return "cooldown_active"; }
  if (request.manual_override_active) { return "manual_operator_override_active"; }
  if (request.lifecycle_fence_active) { return "lifecycle_fence_active"; }
  if (request.safety_fence_result.empty()) { return "safety_fence_result_required"; }
  if (request.safety_fence_result != "passed" && request.safety_fence_result != "safe" &&
      request.safety_fence_result != "not_required") {
    return "safety_fence_not_passed:" + request.safety_fence_result;
  }
  return {};
}

EngineApiDiagnostic PersistHookEvidence(const EngineAgentActionHookRequest& request,
                                        const std::string& operation_id,
                                        const std::string& normalized_action) {
  const auto target = TargetForEvidence(request);
  const auto event = std::string("SBAGENTHOOK1\t") + operation_id + "\t" + request.agent_type + "\t" +
                     normalized_action + "\t" + target.uuid.canonical + "\t" + request.policy_snapshot_uuid.canonical;
  return AppendApiBehaviorEvent(request.context, event);
}

template <typename TResult>
TResult HookSuccess(const EngineAgentActionHookRequest& request,
                    const std::string& operation_id,
                    const std::string& normalized_action,
                    std::vector<std::pair<std::string, std::string>> runtime_rows) {
  if (!request.dry_run) {
    const auto evidence = PersistHookEvidence(request, operation_id, normalized_action);
    if (evidence.error) {
      return MakeApiBehaviorDiagnostic<TResult>(request.context, operation_id, evidence);
    }
  }
  auto result = MakeApiBehaviorSuccess<TResult>(request.context, operation_id);
  result.action_accepted = true;
  result.action_deferred = false;
  result.dry_run = request.dry_run;
  result.normalized_action = normalized_action;
  result.primary_object = TargetForEvidence(request);
  AddApiBehaviorEvidence(&result, "agent_hook", operation_id);
  AddApiBehaviorEvidence(&result, "policy_snapshot_uuid", request.policy_snapshot_uuid.canonical);
  AddApiBehaviorRow(&result,
                    {{"agent_type", request.agent_type},
                     {"action", normalized_action},
                     {"action_class", request.action_class},
                     {"dry_run", request.dry_run ? "true" : "false"},
                     {"target_uuid", result.primary_object.uuid.canonical},
                     {"target_kind", result.primary_object.object_kind},
                     {"policy_snapshot_uuid", request.policy_snapshot_uuid.canonical},
                     {"safety_fence_result", request.safety_fence_result}});
  if (!runtime_rows.empty()) {
    AddApiBehaviorRow(&result, runtime_rows);
  }
  (void)scratchbird::core::metrics::RecordAgentAction(request.agent_type,
                                                     normalized_action,
                                                     request.dry_run ? "dry_run" : "accepted");
  return result;
}

void AddDryRunStorageRouteRows(EngineApiResult* result,
                               const std::string& executor_name,
                               const std::string& mutation_flag) {
  AddApiBehaviorRow(result,
                    {{"storage_executor", executor_name},
                     {"storage_execution", "dry_run"},
                     {mutation_flag, "false"}});
}

template <typename TResult>
TResult StorageRouteFailure(const EngineAgentActionHookRequest& request,
                            const std::string& operation_id,
                            const std::string& normalized_action,
                            EngineApiDiagnostic diagnostic) {
  TResult result = MakeApiBehaviorDiagnostic<TResult>(request.context, operation_id, std::move(diagnostic));
  result.action_accepted = false;
  result.action_deferred = false;
  result.dry_run = request.dry_run;
  result.refusal_reason = result.diagnostics.empty() ? std::string{} : result.diagnostics.front().code;
  result.normalized_action = normalized_action;
  result.primary_object = TargetForEvidence(request);
  result.transaction_uuid = request.context.transaction_uuid;
  result.local_transaction_id = request.context.local_transaction_id;
  result.evidence.push_back({"agent_hook_refused", result.refusal_reason});
  return result;
}

template <typename TResult>
TResult DurableResourceReservationHookFailure(
    const EngineAgentActionHookRequest& request,
    const std::string& operation_id,
    const std::string& normalized_action,
    const std::string& code,
    const std::string& detail) {
  return HookFailure<TResult>(
      request,
      operation_id,
      normalized_action,
      code + (detail.empty() ? std::string{} : ":" + detail));
}

template <typename TResult, typename TRequest, typename TRoute>
TResult RunHookWithDurableResourceReservationIfRequired(
    const TRequest& request,
    const std::string& operation_id,
    const std::string& normalized_action,
    std::vector<std::pair<std::string, std::string>> runtime_rows,
    TRoute route) {
  if (!DurableResourceReservationRequiredForHook(request)) {
    return route(request, std::move(runtime_rows));
  }
  if (!DurableCatalogStoreContextAvailableForHook(request)) {
    return DurableResourceReservationHookFailure<TResult>(
        request,
        operation_id,
        normalized_action,
        "SB_AGENT_HOOK_RESOURCE_RESERVATION.DURABLE_CONTEXT_REQUIRED",
        request.agent_type);
  }

  auto loaded = LoadAgentDurableCatalogImage(request.context, true);
  if (!loaded.ok) {
    return DurableResourceReservationHookFailure<TResult>(
        request,
        operation_id,
        normalized_action,
        "SB_AGENT_HOOK_RESOURCE_RESERVATION.LOAD_FAILED",
        loaded.diagnostic.detail.empty() ? loaded.diagnostic.code
                                         : loaded.diagnostic.detail);
  }
  auto catalog = loaded.image;
  const auto context = AgentContextFromHookRequest(request);
  const auto reservation = DurableResourceReservationForHook(
      request, operation_id, normalized_action, context);
  const auto acquired = AcquireDurableAgentResourceReservation(
      &catalog, reservation);
  if (!acquired.ok) {
    return DurableResourceReservationHookFailure<TResult>(
        request,
        operation_id,
        normalized_action,
        acquired.diagnostic_code,
        acquired.detail);
  }

  AgentDurableCatalogStoreRequest acquire_store;
  acquire_store.context = request.context;
  acquire_store.image = catalog;
  acquire_store.evidence_uuid = reservation.evidence_uuid;
  acquire_store.production_live_path = true;
  acquire_store.fsync_or_checkpoint_evidence =
      DurableCatalogStoreCheckpointEvidencePresentForHook(request);
  auto persisted = PersistAgentDurableCatalogImage(acquire_store);
  if (!persisted.ok) {
    return DurableResourceReservationHookFailure<TResult>(
        request,
        operation_id,
        normalized_action,
        "SB_AGENT_HOOK_RESOURCE_RESERVATION.PERSIST_FAILED",
        persisted.diagnostic.detail.empty() ? persisted.diagnostic.code
                                            : persisted.diagnostic.detail);
  }
  catalog = persisted.image;

  auto result = route(request, std::move(runtime_rows));
  const auto released = ReleaseDurableAgentResourceReservation(
      &catalog,
      reservation.reservation_uuid,
      result.evidence.empty() ? reservation.evidence_uuid
                              : result.evidence.front().evidence_id,
      reservation.now_microseconds + 1,
      result.ok ? DurableAgentResourceReservationState::released
                : DurableAgentResourceReservationState::cancelled);
  if (!released.ok) {
    return DurableResourceReservationHookFailure<TResult>(
        request,
        operation_id,
        normalized_action,
        released.diagnostic_code,
        released.detail);
  }

  AgentDurableCatalogStoreRequest release_store;
  release_store.context = request.context;
  release_store.image = std::move(catalog);
  release_store.evidence_uuid = released.detail.empty()
                                    ? reservation.evidence_uuid
                                    : released.detail;
  release_store.production_live_path = true;
  release_store.fsync_or_checkpoint_evidence =
      DurableCatalogStoreCheckpointEvidencePresentForHook(request);
  persisted = PersistAgentDurableCatalogImage(release_store);
  if (!persisted.ok) {
    return DurableResourceReservationHookFailure<TResult>(
        request,
        operation_id,
        normalized_action,
        "SB_AGENT_HOOK_RESOURCE_RESERVATION.RELEASE_PERSIST_FAILED",
        persisted.diagnostic.detail.empty() ? persisted.diagnostic.code
                                            : persisted.diagnostic.detail);
  }
  AddApiBehaviorEvidence(&result,
                         "agent_durable_resource_reservation",
                         reservation.reservation_uuid);
  AddApiBehaviorRow(&result,
                    {{"durable_resource_reservation", "true"},
                     {"durable_resource_reservation_released", "true"},
                     {"durable_resource_reservation_uuid",
                      reservation.reservation_uuid}});
  return result;
}

EngineRequestPagePreallocationResult RunPagePreallocationRoute(
    const EngineRequestPagePreallocationRequest& request,
    std::vector<std::pair<std::string, std::string>> runtime_rows) {
  constexpr const char* kOperation = "agents.request_page_preallocation";
  constexpr const char* kAction = "page_preallocation_request";
  if (request.dry_run) {
    auto result = HookSuccess<EngineRequestPagePreallocationResult>(
        request, kOperation, kAction, std::move(runtime_rows));
    AddDryRunStorageRouteRows(&result, "PreallocatePageFamilyPool", "page_preallocation_ledger_mutated");
    return result;
  }
  if (!IsKnownPageFamilyForPreallocation(request.page_family)) {
    return HookFailure<EngineRequestPagePreallocationResult>(
        request, kOperation, kAction, "page_family_unknown:" + request.page_family);
  }

  const std::string key = LedgerKey(request.context, request.target_filespace);
  const auto database_uuid = ParseEngineIdentity(platform::UuidKind::database,
                                                request.context.database_uuid.canonical);
  const auto filespace_uuid = ParseEngineIdentity(platform::UuidKind::filespace,
                                                 request.target_filespace.uuid.canonical);
  const auto transaction_uuid = ParseEngineIdentity(platform::UuidKind::transaction,
                                                   request.context.transaction_uuid.canonical);
  if (!database_uuid.valid()) {
    return HookFailure<EngineRequestPagePreallocationResult>(
        request, kOperation, kAction, "database_uuid_invalid_for_storage_route");
  }
  if (!filespace_uuid.valid()) {
    return HookFailure<EngineRequestPagePreallocationResult>(
        request, kOperation, kAction, "target_filespace_uuid_invalid_for_storage_route");
  }
  if (!transaction_uuid.valid()) {
    return HookFailure<EngineRequestPagePreallocationResult>(
        request, kOperation, kAction, "transaction_uuid_invalid_for_storage_route");
  }

  const std::lock_guard<std::mutex> guard(AgentStorageRouteMutex());
  auto& runtime = PagePreallocationRouteLedgers()[key];
  page::PageAllocationLedger candidate = runtime.ledger;
  candidate.database_uuid = database_uuid;
  candidate.filespace_uuid = filespace_uuid;
  candidate.free_extents.push_back({runtime.next_start_page, request.requested_pages});

  page::PagePreallocationRequest storage_request;
  storage_request.database_uuid = database_uuid;
  storage_request.filespace_uuid = filespace_uuid;
  storage_request.policy_uuid = PolicyUuidForStorageRoute(request, key);
  storage_request.capacity_evidence_uuid = GeneratedIdentity(platform::UuidKind::object, key, 23);
  storage_request.creator_transaction_uuid = transaction_uuid;
  storage_request.creator_local_transaction_id = request.context.local_transaction_id;
  storage_request.page_family = request.page_family;
  storage_request.page_count = request.requested_pages;
  storage_request.page_generation = std::max<platform::u64>(1, request.context.resource_epoch);
  storage_request.engine_authoritative = true;
  storage_request.capacity_evidence_accepted = true;
  storage_request.durability_fence_satisfied = true;

  const auto storage = page::PreallocatePageFamilyPool(&candidate, storage_request);
  if (!storage.ok()) {
    return StorageRouteFailure<EngineRequestPagePreallocationResult>(
        request, kOperation, kAction, DiagnosticFromPagePreallocation(storage));
  }
  runtime.ledger = std::move(candidate);
  runtime.next_start_page += request.requested_pages + 16;

  auto result = HookSuccess<EngineRequestPagePreallocationResult>(
      request, kOperation, kAction, std::move(runtime_rows));
  AddApiBehaviorEvidence(&result, "page_preallocation", UuidText(storage.allocation.allocation_uuid));
  AddApiBehaviorEvidence(&result, "storage_executor", "PreallocatePageFamilyPool");
  AddApiBehaviorRow(&result,
                    {{"storage_executor", "PreallocatePageFamilyPool"},
                     {"storage_execution", "completed"},
                     {"page_preallocation_ledger_mutated", "true"},
                     {"page_preallocation_allocation_uuid", UuidText(storage.allocation.allocation_uuid)},
                     {"page_preallocation_state", page::PageAllocationLifecycleStateName(storage.allocation.state)},
                     {"page_preallocation_diagnostic", storage.evidence.diagnostic_code},
                     {"page_preallocation_evidence_action", storage.evidence.action},
                     {"page_preallocation_evidence_sequence", std::to_string(storage.evidence.sequence)},
                     {"page_preallocation_start_page", std::to_string(storage.evidence.start_page)},
                     {"page_preallocation_page_count", std::to_string(storage.evidence.page_count)},
                     {"page_preallocation_durable_state_changed",
                      storage.evidence.durable_state_changed ? "true" : "false"},
                     {"page_preallocation_capacity_evidence_accepted",
                      storage.evidence.capacity_evidence_accepted ? "true" : "false"}});
  return result;
}

void EnsureFilespaceGrowthRuntime(FilespaceGrowthRouteRuntime* runtime,
                                  const platform::TypedUuid& database_uuid,
                                  const platform::TypedUuid& filespace_uuid,
                                  const std::string& key,
                                  platform::u32 page_size,
                                  std::string physical_member_path) {
  if (runtime == nullptr) {
    return;
  }
  if (!runtime->storage_profile_uuid.valid()) {
    runtime->storage_profile_uuid = GeneratedIdentity(platform::UuidKind::object, key, 31);
    runtime->file_member_uuid = GeneratedIdentity(platform::UuidKind::object, key, 37);
  }
  runtime->physical_member_path = std::move(physical_member_path);
  auto found = std::find_if(runtime->registry.filespaces.begin(),
                            runtime->registry.filespaces.end(),
                            [&](const filespace::FilespaceDescriptor& descriptor) {
                              return descriptor.filespace_uuid.value == filespace_uuid.value;
                            });
  if (found == runtime->registry.filespaces.end()) {
    filespace::FilespaceDescriptor descriptor;
    descriptor.database_uuid = database_uuid;
    descriptor.filespace_uuid = filespace_uuid;
    descriptor.role = filespace::FilespaceRole::secondary_data;
    descriptor.state = filespace::FilespaceState::online;
    descriptor.path = runtime->physical_member_path;
    descriptor.page_size = page_size;
    descriptor.generation = 1;
    descriptor.active = true;
    descriptor.read_only = false;
    runtime->registry.filespaces.push_back(descriptor);
  } else {
    found->database_uuid = database_uuid;
    found->path = runtime->physical_member_path;
    found->page_size = page_size;
    found->active = true;
    found->read_only = false;
    if (found->state == filespace::FilespaceState::absent) {
      found->state = filespace::FilespaceState::online;
    }
  }
}

filespace::PhysicalFilespaceWriteResult EnsureFilespaceGrowthPhysicalMember(
    FilespaceGrowthRouteRuntime* runtime,
    const platform::TypedUuid& database_uuid,
    const platform::TypedUuid& filespace_uuid,
    platform::u32 page_size,
    platform::u64 current_pages,
    platform::u64 preallocated_pages) {
  filespace::PhysicalFilespaceWriteResult result;
  if (runtime == nullptr || runtime->physical_member_path.empty()) {
    result.status = {platform::StatusCode::platform_required_feature_missing,
                     platform::Severity::error,
                     platform::Subsystem::storage_disk};
    result.diagnostic = filespace::MakePhysicalFilespaceHeaderDiagnostic(
        result.status,
        "SB-FILESPACE-HEADER-PATH-REQUIRED",
        "storage.filespace.header.path_required",
        "agent filespace growth route has no physical member path");
    return result;
  }
  if (std::filesystem::exists(runtime->physical_member_path)) {
    result.status = {platform::StatusCode::ok,
                     platform::Severity::info,
                     platform::Subsystem::storage_disk};
    return result;
  }
  if (preallocated_pages > UINT64_MAX - current_pages) {
    result.status = {platform::StatusCode::platform_required_feature_missing,
                     platform::Severity::error,
                     platform::Subsystem::storage_disk};
    result.diagnostic = filespace::MakePhysicalFilespaceHeaderDiagnostic(
        result.status,
        "SB-FILESPACE-HEADER-CAPACITY-OVERFLOW",
        "storage.filespace.header.capacity_overflow",
        "agent filespace growth route initial capacity overflowed");
    return result;
  }

  filespace::PhysicalFilespaceHeader header;
  header.database_uuid = database_uuid;
  header.filespace_uuid = filespace_uuid;
  header.role = filespace::FilespaceRole::secondary_data;
  header.state = filespace::FilespaceState::online;
  header.page_size = page_size;
  header.physical_filespace_id = 1;
  header.total_pages = current_pages + preallocated_pages;
  header.free_pages = 0;
  header.preallocated_pages = preallocated_pages;
  header.allocation_root_page = 1;
  header.header_generation = 1;
  header.writer_identity_uuid = runtime->storage_profile_uuid;
  header.creation_operation_uuid =
      UuidText(GeneratedIdentity(platform::UuidKind::object,
                                 runtime->physical_member_path +
                                     "|agent_filespace_growth_route",
                                 61));
  return filespace::WritePhysicalFilespaceHeader(runtime->physical_member_path, header, false);
}

EngineRequestFilespaceGrowthResult RunFilespaceGrowthRoute(
    const EngineRequestFilespaceGrowthRequest& request,
    std::vector<std::pair<std::string, std::string>> runtime_rows) {
  constexpr const char* kOperation = "agents.request_filespace_growth";
  constexpr const char* kAction = "filespace_growth_request";
  if (request.dry_run) {
    auto result = HookSuccess<EngineRequestFilespaceGrowthResult>(
        request, kOperation, kAction, std::move(runtime_rows));
    AddDryRunStorageRouteRows(&result, "ExecuteFilespacePhysicalGrowth", "filespace_growth_ledger_mutated");
    return result;
  }

  const std::string key = LedgerKey(request.context, request.target_filespace);
  const auto database_uuid = ParseEngineIdentity(platform::UuidKind::database,
                                                request.context.database_uuid.canonical);
  const auto filespace_uuid = ParseEngineIdentity(platform::UuidKind::filespace,
                                                 request.target_filespace.uuid.canonical);
  const auto transaction_uuid = ParseEngineIdentity(platform::UuidKind::transaction,
                                                   request.context.transaction_uuid.canonical);
  if (!database_uuid.valid()) {
    return HookFailure<EngineRequestFilespaceGrowthResult>(
        request, kOperation, kAction, "database_uuid_invalid_for_storage_route");
  }
  if (!filespace_uuid.valid()) {
    return HookFailure<EngineRequestFilespaceGrowthResult>(
        request, kOperation, kAction, "target_filespace_uuid_invalid_for_storage_route");
  }
  if (!transaction_uuid.valid()) {
    return HookFailure<EngineRequestFilespaceGrowthResult>(
        request, kOperation, kAction, "transaction_uuid_invalid_for_storage_route");
  }

  const auto page_size = FilespacePageSizeBytes(request);
  const auto requested_pages = RequestedGrowthPages(request, page_size);
  if (requested_pages == 0) {
    return HookFailure<EngineRequestFilespaceGrowthResult>(
        request, kOperation, kAction, "requested_growth_pages_required_for_storage_route");
  }

  const std::lock_guard<std::mutex> guard(AgentStorageRouteMutex());
  auto& runtime = FilespaceGrowthRouteLedgers()[key];
  const std::string physical_member_path =
      FilespaceGrowthPhysicalMemberPath(request.context, request.target_filespace, key);
  EnsureFilespaceGrowthRuntime(&runtime,
                               database_uuid,
                               filespace_uuid,
                               key,
                               page_size,
                               physical_member_path);
  filespace::FilespaceGrowthLedger candidate = runtime.ledger;

  platform::u64 current_pages = OptionU64(request, "filespace.current_pages:", 64);
  platform::u64 preallocated_pages = OptionU64(request, "filespace.preallocated_pages:", 0);
  const auto existing_window = std::find_if(
      candidate.member_capacity_windows.begin(),
      candidate.member_capacity_windows.end(),
      [&](const filespace::FilespaceMemberCapacityWindow& window) {
        return window.filespace_uuid.value == filespace_uuid.value &&
               window.file_member_uuid.value == runtime.file_member_uuid.value;
      });
  if (existing_window != candidate.member_capacity_windows.end()) {
    current_pages = existing_window->logical_page_count;
    preallocated_pages = existing_window->preallocated_page_count;
  }
  const platform::u64 maximum_pages = OptionU64(
      request,
      "filespace.maximum_pages:",
      current_pages + preallocated_pages + requested_pages + 1024);

  const auto physical_member = EnsureFilespaceGrowthPhysicalMember(&runtime,
                                                                   database_uuid,
                                                                   filespace_uuid,
                                                                   page_size,
                                                                   current_pages,
                                                                   preallocated_pages);
  if (!physical_member.ok()) {
    return HookFailure<EngineRequestFilespaceGrowthResult>(
        request,
        kOperation,
        kAction,
        physical_member.diagnostic.diagnostic_code.empty()
            ? "filespace_growth_physical_member_init_failed"
            : physical_member.diagnostic.diagnostic_code);
  }

  filespace::FilespacePhysicalGrowthRequest storage_request;
  storage_request.request_uuid = GeneratedIdentity(platform::UuidKind::object,
                                                   key + "|" + request.context.request_id,
                                                   41 + candidate.next_evidence_sequence);
  storage_request.database_uuid = database_uuid;
  storage_request.filespace_uuid = filespace_uuid;
  storage_request.policy_uuid = PolicyUuidForStorageRoute(request, key);
  storage_request.storage_profile_uuid = runtime.storage_profile_uuid;
  storage_request.requested_growth_pages = requested_pages;
  storage_request.page_size_bytes = page_size;
  storage_request.policy_generation = std::max<platform::u64>(1, request.context.resource_epoch);
  storage_request.observed_policy_generation = storage_request.policy_generation;
  storage_request.catalog_generation = std::max<platform::u64>(1, request.context.catalog_generation_id);
  storage_request.observed_catalog_generation = storage_request.catalog_generation;
  storage_request.caller_mode = filespace::FilespacePhysicalGrowthCallerMode::filespace_capacity_manager;
  storage_request.authorization.obs_agent_control_right = true;
  storage_request.authorization.filespace_lifecycle_right = true;
  storage_request.authorization.action_approval = true;
  storage_request.evidence_store_present = request.evidence_sink_available;
  storage_request.evidence_before_success = true;
  storage_request.policy_expand_allowed = request.policy_authorized;
  storage_request.engine_owned_authority = true;
  storage_request.reserve_growth_as_preallocated =
      SecurityOptionBool(request, "filespace.reserve_growth_as_preallocated:", true);
  storage_request.reason = "agent_route_filespace_growth";
  storage_request.member_capacity.present = true;
  storage_request.member_capacity.explicit_capacity_context = true;
  storage_request.member_capacity.file_member_uuid = runtime.file_member_uuid;
  storage_request.member_capacity.start_page_number = runtime.member_start_page;
  storage_request.member_capacity.current_page_count = current_pages;
  storage_request.member_capacity.preallocated_page_count = preallocated_pages;
  storage_request.member_capacity.maximum_page_count = maximum_pages;
  storage_request.member_capacity.physical_path = runtime.physical_member_path;
  storage_request.member_capacity.online = true;
  storage_request.member_capacity.writable = true;
  storage_request.transaction_context.present = true;
  storage_request.transaction_context.transaction_uuid = transaction_uuid;
  storage_request.transaction_context.transaction_number = request.context.local_transaction_id;
  storage_request.transaction_context.durable_inventory_admitted = true;
  storage_request.transaction_context.write_intent = true;
  storage_request.transaction_context.durability_fence_satisfied = true;

  const auto storage = filespace::ExecuteFilespacePhysicalGrowth(
      &candidate, runtime.registry, storage_request);
  if (!storage.ok()) {
    return StorageRouteFailure<EngineRequestFilespaceGrowthResult>(
        request, kOperation, kAction, DiagnosticFromFilespaceGrowth(storage));
  }
  runtime.ledger = std::move(candidate);

  auto result = HookSuccess<EngineRequestFilespaceGrowthResult>(
      request, kOperation, kAction, std::move(runtime_rows));
  AddApiBehaviorEvidence(&result, "filespace_growth", UuidText(storage.operation.growth_operation_id));
  AddApiBehaviorEvidence(&result, "storage_executor", "ExecuteFilespacePhysicalGrowth");
  AddApiBehaviorRow(&result,
                    {{"storage_executor", "ExecuteFilespacePhysicalGrowth"},
                     {"storage_execution", "completed"},
                     {"filespace_growth_ledger_mutated", "true"},
                     {"filespace_growth_operation_uuid", UuidText(storage.operation.growth_operation_id)},
                     {"filespace_growth_state", filespace::FilespacePhysicalGrowthStateName(storage.operation.state)},
                     {"filespace_growth_diagnostic", storage.diagnostic.diagnostic_code},
                     {"filespace_growth_evidence_action", storage.evidence.action},
                     {"filespace_growth_evidence_sequence", std::to_string(storage.evidence.sequence)},
                     {"filespace_growth_requested_pages", std::to_string(storage.operation.requested_growth_pages)},
                     {"filespace_growth_grown_pages", std::to_string(storage.operation.grown_page_count)},
                     {"filespace_growth_bytes_grown", std::to_string(storage.operation.bytes_grown)},
                     {"filespace_growth_physical_extension_completed",
                      storage.evidence.physical_extension_completed ? "true" : "false"},
                     {"filespace_growth_physical_extension_synced",
                      storage.evidence.physical_extension_synced ? "true" : "false"},
                     {"filespace_growth_physical_header_updated",
                      storage.evidence.physical_header_updated ? "true" : "false"},
                     {"filespace_growth_metadata_after_physical_extension",
                      storage.evidence.metadata_commit_after_physical_extension ? "true" : "false"},
                     {"filespace_growth_durable_state_changed",
                      storage.evidence.durable_state_changed ? "true" : "false"},
                     {"filespace_growth_cache_invalidation_required",
                      storage.cache_invalidation_required ? "true" : "false"},
                     {"filespace_growth_reserved_as_preallocated",
                      storage.operation.reserve_growth_as_preallocated ? "true" : "false"},
                     {"filespace_growth_page_allocation_authority_bypassed",
                      storage.page_allocation_authority_bypassed ? "true" : "false"}});
  return result;
}

template <typename TResult, typename TRequest>
TResult ValidateAndAccept(const TRequest& request,
                          const std::string& operation_id,
                          const std::string& normalized_action,
                          std::string hook_specific_error) {
  if (!hook_specific_error.empty()) {
    return HookFailure<TResult>(request, operation_id, normalized_action, hook_specific_error);
  }
  const auto common_error = ValidateCommon(request);
  if (!common_error.empty()) {
    return HookFailure<TResult>(request, operation_id, normalized_action, common_error, common_error == "cooldown_active");
  }
  std::vector<std::pair<std::string, std::string>> runtime_rows;
  const auto runtime_error = ValidateRuntimeDecision(request, normalized_action, &runtime_rows);
  if (!runtime_error.empty()) {
    return HookFailure<TResult>(request, operation_id, normalized_action, runtime_error);
  }
  return RunHookWithDurableResourceReservationIfRequired<TResult>(
      request,
      operation_id,
      normalized_action,
      std::move(runtime_rows),
      [&](const TRequest& routed_request,
          std::vector<std::pair<std::string, std::string>> routed_rows) {
        return HookSuccess<TResult>(
            routed_request, operation_id, normalized_action, std::move(routed_rows));
      });
}

template <typename TResult, typename TRequest, typename TRoute>
TResult ValidateAndRoute(const TRequest& request,
                         const std::string& operation_id,
                         const std::string& normalized_action,
                         std::string hook_specific_error,
                         TRoute route) {
  if (!hook_specific_error.empty()) {
    return HookFailure<TResult>(request, operation_id, normalized_action, hook_specific_error);
  }
  const auto common_error = ValidateCommon(request);
  if (!common_error.empty()) {
    return HookFailure<TResult>(request, operation_id, normalized_action, common_error, common_error == "cooldown_active");
  }
  std::vector<std::pair<std::string, std::string>> runtime_rows;
  const auto runtime_error = ValidateRuntimeDecision(request, normalized_action, &runtime_rows);
  if (!runtime_error.empty()) {
    return HookFailure<TResult>(request, operation_id, normalized_action, runtime_error);
  }
  return RunHookWithDurableResourceReservationIfRequired<TResult>(
      request,
      operation_id,
      normalized_action,
      std::move(runtime_rows),
      route);
}

std::string RequirePageTarget(const EngineAgentActionHookRequest& request) {
  if (Empty(request.target_filespace)) { return "target_filespace_uuid_required"; }
  if (request.page_family.empty()) { return "page_family_required"; }
  if (request.page_type.empty()) { return "page_type_required"; }
  if (request.requested_pages == 0) { return "requested_pages_required"; }
  return {};
}

std::string RequireFilespaceTarget(const EngineAgentActionHookRequest& request, bool require_capacity) {
  if (Empty(request.target_filespace)) { return "target_filespace_uuid_required"; }
  if (require_capacity && request.requested_bytes == 0 && request.requested_pages == 0) {
    return "requested_bytes_or_pages_required";
  }
  return {};
}

std::string RequireIndexTarget(const EngineAgentActionHookRequest& request) {
  if (Empty(request.target_index)) { return "target_index_uuid_required"; }
  return {};
}

}  // namespace

// SEARCH_KEY: SB_AGENT_ACTION_HOOK_PAGE_PREALLOCATION_REQUEST
EngineRequestPagePreallocationResult EngineRequestPagePreallocation(const EngineRequestPagePreallocationRequest& request) {
  return ValidateAndRoute<EngineRequestPagePreallocationResult>(
      request,
      "agents.request_page_preallocation",
      "page_preallocation_request",
      RequirePageTarget(request),
      RunPagePreallocationRoute);
}

EngineRequestPageRelocationResult EngineRequestPageRelocation(const EngineRequestPageRelocationRequest& request) {
  return ValidateAndAccept<EngineRequestPageRelocationResult>(
      request,
      "agents.request_page_relocation",
      "page_relocation_request",
      RequirePageTarget(request));
}

// SEARCH_KEY: SB_AGENT_ACTION_HOOK_FILESPACE_GROWTH_REQUEST
EngineRequestFilespaceGrowthResult EngineRequestFilespaceGrowth(const EngineRequestFilespaceGrowthRequest& request) {
  return ValidateAndRoute<EngineRequestFilespaceGrowthResult>(
      request,
      "agents.request_filespace_growth",
      "filespace_growth_request",
      RequireFilespaceTarget(request, true),
      RunFilespaceGrowthRoute);
}

EngineNotifyFilespaceShrinkReadinessResult EngineNotifyFilespaceShrinkReadiness(
    const EngineNotifyFilespaceShrinkReadinessRequest& request) {
  return ValidateAndAccept<EngineNotifyFilespaceShrinkReadinessResult>(
      request,
      "agents.notify_filespace_shrink_readiness",
      "filespace_shrink_readiness_notification",
      RequireFilespaceTarget(request, false));
}

EngineRequestIndexDeltaMergeResult EngineRequestIndexDeltaMerge(const EngineRequestIndexDeltaMergeRequest& request) {
  return ValidateAndAccept<EngineRequestIndexDeltaMergeResult>(
      request,
      "agents.request_index_delta_merge",
      "index_delta_merge_request",
      RequireIndexTarget(request));
}

EngineRequestIndexRebuildOrShadowBuildResult EngineRequestIndexRebuildOrShadowBuild(
    const EngineRequestIndexRebuildOrShadowBuildRequest& request) {
  return ValidateAndAccept<EngineRequestIndexRebuildOrShadowBuildResult>(
      request,
      "agents.request_index_rebuild_or_shadow_build",
      request.shadow_build ? "index_shadow_build_request" : "index_rebuild_request",
      RequireIndexTarget(request));
}

}  // namespace scratchbird::engine::internal_api
