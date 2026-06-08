// Copyright (c) 2026 ScratchBird Software Inc.
//
// This Source Code Form is subject to the terms of the Mozilla Public
// License, v. 2.0. If a copy of the MPL was not distributed with this
// file, You can obtain one at https://mozilla.org/MPL/2.0/.
//
// SPDX-License-Identifier: MPL-2.0

#include "storage/storage_management_api.hpp"

#include "api_diagnostics.hpp"
#include "behavior_support/api_behavior_store.hpp"
#include "filespace_growth.hpp"
#include "security/security_model.hpp"
#include "uuid.hpp"

#include <algorithm>
#include <cstdint>
#include <map>
#include <mutex>
#include <string>
#include <utility>

namespace scratchbird::engine::internal_api {
namespace {

namespace filespace = scratchbird::storage::filespace;
namespace platform = scratchbird::core::platform;
namespace uuid = scratchbird::core::uuid;

struct FilespacePreallocateRouteRuntime {
  filespace::FilespaceGrowthLedger ledger;
  filespace::FilespaceRegistry registry;
  platform::TypedUuid storage_profile_uuid;
  platform::TypedUuid file_member_uuid;
  platform::u64 member_start_page = 1200000;
};

std::mutex& FilespacePreallocateRouteMutex() {
  static std::mutex mutex;
  return mutex;
}

std::map<std::string, FilespacePreallocateRouteRuntime>& FilespacePreallocateRouteLedgers() {
  static std::map<std::string, FilespacePreallocateRouteRuntime> ledgers;
  return ledgers;
}

std::string OptionValue(const EngineApiRequest& request, const std::string& prefix) {
  for (const auto& option : request.option_envelopes) {
    if (option.rfind(prefix, 0) == 0) return option.substr(prefix.size());
  }
  return {};
}

platform::u64 ParseU64(std::string value, platform::u64 fallback) {
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
  return ParseU64(OptionValue(request, prefix), fallback);
}

bool OptionBool(const EngineApiRequest& request,
                const std::string& prefix,
                bool fallback) {
  const auto value = OptionValue(request, prefix);
  if (value.empty()) {
    return fallback;
  }
  return value == "true" || value == "1" || value == "yes" || value == "on";
}

bool RequestReadOnlyMode(const EngineApiRequest& request) {
  return request.context.read_only_mode ||
         SecurityOptionPresent(request, "lifecycle:read_only");
}

EngineApiDiagnostic FilespaceOpenStateMutationDiagnostic(const EngineApiRequest& request,
                                                         const char* operation) {
  if (RequestReadOnlyMode(request)) {
    return MakeEngineApiDiagnostic("FILESPACE.READ_ONLY_DENIED",
                                   "filespace.preallocate.read_only_denied",
                                   operation,
                                   true);
  }
  if (SecurityOptionPresent(request, "lifecycle:shutdown")) {
    return MakeEngineApiDiagnostic("FILESPACE.SHUTDOWN_IN_PROGRESS",
                                   "filespace.preallocate.shutdown_in_progress",
                                   operation,
                                   true);
  }
  if (SecurityOptionPresent(request, "lifecycle:restricted_open")) {
    return MakeEngineApiDiagnostic("FILESPACE.RESTRICTED_DENIED",
                                   "filespace.preallocate.restricted_denied",
                                   operation,
                                   true);
  }
  if (SecurityOptionPresent(request, "lifecycle:repair")) {
    return MakeEngineApiDiagnostic("FILESPACE.REPAIR_DENIED",
                                   "filespace.preallocate.repair_denied",
                                   operation,
                                   true);
  }
  if (SecurityOptionPresent(request, "lifecycle:maintenance")) {
    return MakeEngineApiDiagnostic("FILESPACE.MAINTENANCE_DENIED",
                                   "filespace.preallocate.maintenance_denied",
                                   operation,
                                   true);
  }
  if (SecurityOptionPresent(request, "lifecycle:backup") ||
      SecurityOptionPresent(request, "lifecycle:restore")) {
    return MakeEngineApiDiagnostic("FILESPACE.BACKUP_HOLD_DENIED",
                                   "filespace.preallocate.backup_hold_denied",
                                   operation,
                                   true);
  }
  if (SecurityOptionPresent(request, "lifecycle:archive_hold")) {
    return MakeEngineApiDiagnostic("FILESPACE.ARCHIVE_HOLD_DENIED",
                                   "filespace.preallocate.archive_hold_denied",
                                   operation,
                                   true);
  }
  return {};
}

std::string ManagementPolicyValue(const EngineApiRequest& request) {
  auto value = SecurityOptionValue(request, "filespace_management_policy:");
  if (value.empty()) { value = SecurityOptionValue(request, "agent_management_policy:"); }
  if (value.empty()) { value = SecurityOptionValue(request, "policy_profile:"); }
  return SecurityLower(value);
}

std::string LedgerKey(const EngineRequestContext& context,
                      const EngineObjectReference& filespace) {
  const std::string path = context.database_path.empty() ? std::string("database_path_absent")
                                                         : context.database_path;
  const std::string database = context.database_uuid.canonical.empty()
                                   ? std::string("database_uuid_absent")
                                   : context.database_uuid.canonical;
  return path + "|" + database + "|" + filespace.uuid.canonical + "|filespace.preallocate";
}

platform::u64 StableSeed(const std::string& value, platform::u64 salt) {
  platform::u64 hash = 1469598103934665603ull ^ salt;
  for (const unsigned char c : value) {
    hash ^= static_cast<platform::u64>(c);
    hash *= 1099511628211ull;
  }
  return 1910000000000ull + (hash % 1000000000ull);
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

EngineObjectReference TargetFilespace(const EngineApiRequest& request) {
  if (request.target_object.object_kind == "filespace") {
    return request.target_object;
  }
  for (const auto& object : request.related_objects) {
    if (object.object_kind == "filespace") {
      return object;
    }
  }
  EngineObjectReference target;
  target.uuid.canonical = OptionValue(request, "target_filespace_uuid:");
  target.object_kind = "filespace";
  return target;
}

platform::u32 PageSizeBytes(const EngineApiRequest& request) {
  const auto page_size = OptionU64(request, "filespace.page_size_bytes:", 16384);
  if (page_size == 0 || page_size > UINT32_MAX) {
    return 0;
  }
  return static_cast<platform::u32>(page_size);
}

platform::u64 RequestedPreallocationPages(const EngineApiRequest& request,
                                          platform::u32 page_size) {
  const auto requested_pages = OptionU64(request, "requested_pages:", 0);
  if (requested_pages != 0) {
    return requested_pages;
  }
  const auto requested_bytes = OptionU64(request, "requested_bytes:", 0);
  if (requested_bytes == 0 || page_size == 0) {
    return 0;
  }
  return (requested_bytes + page_size - 1) / page_size;
}

platform::TypedUuid OptionalObjectIdentity(const EngineApiRequest& request,
                                           const std::string& prefix,
                                           const std::string& key,
                                           platform::u64 salt) {
  const auto parsed = ParseEngineIdentity(platform::UuidKind::object, OptionValue(request, prefix));
  return parsed.valid() ? parsed : GeneratedIdentity(platform::UuidKind::object, key, salt);
}

void EnsureFilespacePreallocateRuntime(FilespacePreallocateRouteRuntime* runtime,
                                       const platform::TypedUuid& database_uuid,
                                       const platform::TypedUuid& filespace_uuid,
                                       const std::string& key,
                                       platform::u32 page_size) {
  if (runtime == nullptr) {
    return;
  }
  if (!runtime->storage_profile_uuid.valid()) {
    runtime->storage_profile_uuid = GeneratedIdentity(platform::UuidKind::object, key, 31);
    runtime->file_member_uuid = GeneratedIdentity(platform::UuidKind::object, key, 37);
  }
  const auto found = std::find_if(runtime->registry.filespaces.begin(),
                                  runtime->registry.filespaces.end(),
                                  [&](const filespace::FilespaceDescriptor& descriptor) {
                                    return descriptor.filespace_uuid.value == filespace_uuid.value;
                                  });
  if (found != runtime->registry.filespaces.end()) {
    return;
  }
  filespace::FilespaceDescriptor descriptor;
  descriptor.database_uuid = database_uuid;
  descriptor.filespace_uuid = filespace_uuid;
  descriptor.role = filespace::FilespaceRole::secondary_data;
  descriptor.state = filespace::FilespaceState::online;
  descriptor.page_size = page_size;
  descriptor.generation = 1;
  descriptor.read_only = false;
  descriptor.active = true;
  runtime->registry.filespaces.push_back(std::move(descriptor));
}

EngineApiDiagnostic DiagnosticFromPreallocation(
    const filespace::FilespacePreallocationResult& storage) {
  std::string detail = storage.diagnostic.message_key;
  for (const auto& argument : storage.diagnostic.arguments) {
    if (argument.key == "detail" && !argument.value.empty()) {
      detail += ":" + argument.value;
    }
  }
  return MakeEngineApiDiagnostic(
      storage.diagnostic.diagnostic_code.empty()
          ? "filespace_preallocate_route_failed"
          : storage.diagnostic.diagnostic_code,
      storage.diagnostic.message_key.empty()
          ? "storage.filespace.preallocate.route_failed"
          : storage.diagnostic.message_key,
      std::move(detail),
      true);
}

EngineFilespacePreallocateResult FilespacePreallocateFailure(
    const EngineFilespacePreallocateRequest& request,
    EngineApiDiagnostic diagnostic) {
  auto result = MakeApiBehaviorDiagnostic<EngineFilespacePreallocateResult>(
      request.context, "filespace.preallocate", std::move(diagnostic));
  AddApiBehaviorEvidence(&result, "filespace_preallocation_refused",
                         result.diagnostics.empty() ? std::string{} : result.diagnostics.front().code);
  AddApiBehaviorRow(&result,
                    {{"storage_executor", "PreallocateFilespace"},
                     {"storage_execution", "refused"},
                     {"filespace_preallocation_ledger_mutated", "false"}});
  return result;
}

}  // namespace

EngineStorageManagementResult EngineStorageManagementOperation(
    const EngineStorageManagementRequest& request) {
  constexpr const char* kOperation = "storage.manage_operation";
  if (!request.context.security_context_present) {
    return MakeApiBehaviorDiagnostic<EngineStorageManagementResult>(
        request.context,
        kOperation,
        MakeInvalidRequestDiagnostic(kOperation, "security_context_required"));
  }

  const std::string surface_id = OptionValue(request, "sbsfc077_surface_id:");
  const std::string sbsfc080_surface_id = OptionValue(request, "sbsfc080_surface_id:");
  const std::string action = OptionValue(request, "storage_action:").empty()
                                 ? "inspect"
                                 : OptionValue(request, "storage_action:");
  const std::string object_kind = request.target_object.object_kind.empty()
                                      ? "storage_management_surface"
                                      : request.target_object.object_kind;
  const bool control_action = action != "inspect" && action != "show" && action != "read";
  const char* required_right = control_action ? "OBS_CONFIG_CONTROL" : "OBS_CONFIG_INSPECT";
  if (!SecurityContextHasRight(request.context, required_right)) {
    return MakeApiBehaviorDiagnostic<EngineStorageManagementResult>(
        request.context,
        kOperation,
        MakeEngineApiDiagnostic("STORAGE.PERMISSION_DENIED",
                                "storage.management.permission_denied",
                                required_right,
                                true));
  }

  auto result = MakeApiBehaviorSuccess<EngineStorageManagementResult>(
      request.context, kOperation);
  AddApiBehaviorRow(&result,
                    {{"storage_action", action},
                     {"surface_id", surface_id},
                     {"object_kind", object_kind},
                     {"object_uuid", request.target_object.uuid.canonical},
                     {"storage_authority", "engine_open_core_management_api"},
                     {"cluster_provider_dispatch", "false"}});
  AddApiBehaviorEvidence(&result, "storage_management_operation", action);
  if (!surface_id.empty()) {
    AddApiBehaviorEvidence(&result, "sbsfc077_surface", surface_id);
  }
  if (!sbsfc080_surface_id.empty()) {
    const std::string evidence_kind = OptionValue(request, "sbsfc080_runtime_evidence_kind:");
    const std::string evidence_id = OptionValue(request, "sbsfc080_runtime_evidence_id:");
    AddApiBehaviorEvidence(&result,
                           evidence_kind.empty() ? "storage_management_operation" : evidence_kind,
                           evidence_id.empty() ? action : evidence_id);
    AddApiBehaviorEvidence(&result, "sbsfc080_surface", sbsfc080_surface_id);
    AddApiBehaviorEvidence(&result, "parser_executes_sql", "false");
    AddApiBehaviorEvidence(&result, "cluster_provider_dispatch", "false");
    AddApiBehaviorEvidence(&result, "private_cluster_execution", "false");
    AddApiBehaviorEvidence(&result, "wal_recovery_authority", "false");
  }
  AddApiBehaviorEvidence(&result, "parser_storage_authority", "false");
  return result;
}

EngineFilespacePreallocateResult EngineFilespacePreallocate(
    const EngineFilespacePreallocateRequest& request) {
  // SEARCH_KEY: PFAR_014_FILESPACE_MANAGEMENT_AUTHORIZATION_POLICY_GATE
  constexpr const char* kOperation = "filespace.preallocate";
  if (!request.context.security_context_present) {
    return FilespacePreallocateFailure(
        request,
        MakeEngineApiDiagnostic("AGENT.SECURITY_CONTEXT_REQUIRED",
                                "agent.security_context_required",
                                kOperation,
                                true));
  }
  if (!SecurityContextHasRight(request.context, "OBS_AGENT_CONTROL")) {
    return FilespacePreallocateFailure(
        request,
        MakeEngineApiDiagnostic("FILESPACE.PERMISSION_DENIED",
                                "filespace.preallocate.permission_denied",
                                "OBS_AGENT_CONTROL",
                                true));
  }
  if (!SecurityContextHasRight(request.context, "FILESPACE_LIFECYCLE_CONTROL")) {
    return FilespacePreallocateFailure(
        request,
        MakeEngineApiDiagnostic("FILESPACE.PERMISSION_DENIED",
                                "filespace.preallocate.permission_denied",
                                "FILESPACE_LIFECYCLE_CONTROL",
                                true));
  }
  const auto open_state_diagnostic =
      FilespaceOpenStateMutationDiagnostic(request, kOperation);
  if (!open_state_diagnostic.code.empty()) {
    return FilespacePreallocateFailure(request, open_state_diagnostic);
  }
  const auto policy = ManagementPolicyValue(request);
  if (policy == "deny" || policy == "deny_all" || policy == "read_only" ||
      policy == "inspect_only") {
    return FilespacePreallocateFailure(
        request,
        MakeEngineApiDiagnostic("FILESPACE.POLICY_DENIED",
                                "filespace.preallocate.policy_denied",
                                policy,
                                true));
  }
  if (request.context.local_transaction_id == 0) {
    return FilespacePreallocateFailure(
        request, MakeInvalidRequestDiagnostic(kOperation, "local_transaction_id_required"));
  }

  const EngineObjectReference target = TargetFilespace(request);
  if (target.uuid.canonical.empty()) {
    return FilespacePreallocateFailure(
        request, MakeInvalidRequestDiagnostic(kOperation, "target_filespace_uuid_required"));
  }

  const std::string key = LedgerKey(request.context, target);
  const auto database_uuid = ParseEngineIdentity(platform::UuidKind::database,
                                                request.context.database_uuid.canonical);
  const auto filespace_uuid = ParseEngineIdentity(platform::UuidKind::filespace,
                                                 target.uuid.canonical);
  const auto transaction_uuid = ParseEngineIdentity(platform::UuidKind::transaction,
                                                   request.context.transaction_uuid.canonical);
  if (!database_uuid.valid()) {
    return FilespacePreallocateFailure(
        request, MakeInvalidRequestDiagnostic(kOperation, "database_uuid_invalid_for_storage_route"));
  }
  if (!filespace_uuid.valid()) {
    return FilespacePreallocateFailure(
        request, MakeInvalidRequestDiagnostic(kOperation, "target_filespace_uuid_invalid_for_storage_route"));
  }
  if (!transaction_uuid.valid()) {
    return FilespacePreallocateFailure(
        request, MakeInvalidRequestDiagnostic(kOperation, "transaction_uuid_invalid_for_storage_route"));
  }

  const auto page_size = PageSizeBytes(request);
  const auto requested_pages = RequestedPreallocationPages(request, page_size);
  if (requested_pages == 0) {
    return FilespacePreallocateFailure(
        request, MakeInvalidRequestDiagnostic(kOperation, "requested_pages_required"));
  }

  const std::lock_guard<std::mutex> guard(FilespacePreallocateRouteMutex());
  auto& runtime = FilespacePreallocateRouteLedgers()[key];
  EnsureFilespacePreallocateRuntime(&runtime, database_uuid, filespace_uuid, key, page_size);
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

  filespace::FilespacePreallocationRequest storage_request;
  storage_request.request_uuid = GeneratedIdentity(platform::UuidKind::object,
                                                   key + "|" + request.context.request_id,
                                                   41 + candidate.next_evidence_sequence);
  storage_request.database_uuid = database_uuid;
  storage_request.filespace_uuid = filespace_uuid;
  storage_request.policy_uuid = OptionalObjectIdentity(request, "policy_uuid:", key, 43);
  storage_request.storage_profile_uuid = OptionalObjectIdentity(
      request, "storage_profile_uuid:", key, 47);
  if (!storage_request.storage_profile_uuid.valid()) {
    storage_request.storage_profile_uuid = runtime.storage_profile_uuid;
  }
  storage_request.requested_page_count = requested_pages;
  storage_request.page_size_bytes = page_size;
  storage_request.policy_generation = std::max<platform::u64>(
      1, OptionU64(request, "policy_generation:", request.context.resource_epoch));
  storage_request.observed_policy_generation = OptionU64(
      request, "observed_policy_generation:", storage_request.policy_generation);
  storage_request.catalog_generation = std::max<platform::u64>(
      1, OptionU64(request, "catalog_generation:", request.context.catalog_generation_id));
  storage_request.observed_catalog_generation = OptionU64(
      request, "observed_catalog_generation:", storage_request.catalog_generation);
  storage_request.evidence_store_present = OptionBool(request, "evidence_sink_available:", true);
  storage_request.evidence_before_success = true;
  storage_request.reason = "engine_filespace_preallocate_route";
  storage_request.member_capacity.present = true;
  storage_request.member_capacity.explicit_capacity_context = true;
  storage_request.member_capacity.file_member_uuid = runtime.file_member_uuid;
  storage_request.member_capacity.start_page_number = runtime.member_start_page;
  storage_request.member_capacity.current_page_count = current_pages;
  storage_request.member_capacity.preallocated_page_count = preallocated_pages;
  storage_request.member_capacity.maximum_page_count = maximum_pages;
  storage_request.member_capacity.online = true;
  storage_request.member_capacity.writable = true;
  storage_request.transaction_context.present = true;
  storage_request.transaction_context.transaction_uuid = transaction_uuid;
  storage_request.transaction_context.transaction_number = request.context.local_transaction_id;
  storage_request.transaction_context.durable_inventory_admitted = true;
  storage_request.transaction_context.write_intent = true;
  storage_request.transaction_context.durability_fence_satisfied = true;

  const auto storage = filespace::PreallocateFilespace(
      &candidate, runtime.registry, storage_request);
  if (!storage.ok()) {
    return FilespacePreallocateFailure(request, DiagnosticFromPreallocation(storage));
  }
  runtime.ledger = std::move(candidate);

  auto result = MakeApiBehaviorSuccess<EngineFilespacePreallocateResult>(
      request.context, kOperation);
  result.primary_object = target;
  AddApiBehaviorEvidence(&result, "filespace_preallocation",
                         UuidText(storage.operation.preallocation_operation_id));
  AddApiBehaviorEvidence(&result, "storage_executor", "PreallocateFilespace");
  AddApiBehaviorEvidence(&result, "parser_storage_authority", "false");
  AddApiBehaviorRow(&result,
                    {{"storage_executor", "PreallocateFilespace"},
                     {"storage_execution", "completed"},
                     {"filespace_preallocation_ledger_mutated", "true"},
                     {"filespace_preallocation_operation_uuid",
                      UuidText(storage.operation.preallocation_operation_id)},
                     {"filespace_preallocation_state",
                      filespace::FilespacePreallocationStateName(storage.operation.state)},
                     {"filespace_preallocation_diagnostic", storage.diagnostic.diagnostic_code},
                     {"filespace_preallocation_evidence_action", storage.evidence.action},
                     {"filespace_preallocation_evidence_sequence", std::to_string(storage.evidence.sequence)},
                     {"filespace_preallocation_requested_pages",
                      std::to_string(storage.operation.requested_page_count)},
                     {"filespace_preallocation_pages",
                      std::to_string(storage.operation.preallocated_page_count)},
                     {"filespace_preallocation_bytes",
                      std::to_string(storage.operation.bytes_preallocated)},
                     {"filespace_preallocation_start_page",
                      std::to_string(storage.operation.start_page_number)},
                     {"filespace_preallocation_durable_state_changed",
                      storage.evidence.durable_state_changed ? "true" : "false"},
                     {"filespace_preallocation_cache_invalidation_required",
                      storage.cache_invalidation_required ? "true" : "false"}});
  return result;
}

}  // namespace scratchbird::engine::internal_api
