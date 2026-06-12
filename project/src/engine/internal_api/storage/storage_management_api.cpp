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
#include "filespace_lifecycle.hpp"
#include "filespace_package.hpp"
#include "page_registry.hpp"
#include "security/security_model.hpp"
#include "uuid.hpp"

#include <algorithm>
#include <cstdint>
#include <filesystem>
#include <fstream>
#include <iomanip>
#include <map>
#include <mutex>
#include <sstream>
#include <string>
#include <string_view>
#include <utility>

namespace scratchbird::engine::internal_api {
namespace {

namespace filespace = scratchbird::storage::filespace;
namespace page = scratchbird::storage::page;
namespace platform = scratchbird::core::platform;
namespace uuid = scratchbird::core::uuid;

struct FilespacePreallocateRouteRuntime {
  filespace::FilespaceGrowthLedger ledger;
  filespace::FilespaceRegistry registry;
  platform::TypedUuid storage_profile_uuid;
  platform::TypedUuid file_member_uuid;
  platform::u64 member_start_page = 1200000;
};

struct FilespaceLifecycleRouteRuntime {
  filespace::FilespaceRegistry registry;
  platform::TypedUuid writer_identity_uuid;
};

std::mutex& FilespacePreallocateRouteMutex() {
  static std::mutex mutex;
  return mutex;
}

std::mutex& FilespaceLifecycleRouteMutex() {
  static std::mutex mutex;
  return mutex;
}

std::map<std::string, FilespacePreallocateRouteRuntime>& FilespacePreallocateRouteLedgers() {
  static std::map<std::string, FilespacePreallocateRouteRuntime> ledgers;
  return ledgers;
}

std::map<std::string, FilespaceLifecycleRouteRuntime>& FilespaceLifecycleRouteRegistries() {
  static std::map<std::string, FilespaceLifecycleRouteRuntime> registries;
  return registries;
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

std::string LifecycleRegistryKey(const EngineRequestContext& context) {
  const std::string path = context.database_path.empty() ? std::string("database_path_absent")
                                                         : context.database_path;
  const std::string database = context.database_uuid.canonical.empty()
                                   ? std::string("database_uuid_absent")
                                   : context.database_uuid.canonical;
  return path + "|" + database + "|filespace.lifecycle";
}

platform::u32 PageSizeBytes(const EngineApiRequest& request) {
  const auto page_size = OptionU64(request, "filespace.page_size_bytes:", 16384);
  if (page_size == 0 || page_size > UINT32_MAX) {
    return 0;
  }
  return static_cast<platform::u32>(page_size);
}

filespace::FilespaceRole LifecycleRoleValue(const EngineApiRequest& request,
                                            filespace::FilespaceOperation operation) {
  const std::string role = SecurityLower(OptionValue(request, "filespace.role:"));
  if (role == "active_primary") return filespace::FilespaceRole::active_primary;
  if (role == "primary_shadow") return filespace::FilespaceRole::primary_shadow;
  if (role == "primary_candidate") return filespace::FilespaceRole::primary_candidate;
  if (role == "secondary_data") return filespace::FilespaceRole::secondary_data;
  if (role == "secondary_index") return filespace::FilespaceRole::secondary_index;
  if (role == "secondary_overflow") return filespace::FilespaceRole::secondary_overflow;
  if (role == "secondary_history") return filespace::FilespaceRole::secondary_history;
  if (role == "secondary_shard") return filespace::FilespaceRole::secondary_shard;
  if (operation == filespace::FilespaceOperation::create_snapshot_filespace) {
    return filespace::FilespaceRole::primary_snapshot;
  }
  if (operation == filespace::FilespaceOperation::create_shadow_filespace) {
    return filespace::FilespaceRole::primary_shadow;
  }
  if (operation == filespace::FilespaceOperation::attach_filespace ||
      operation == filespace::FilespaceOperation::promote_filespace) {
    return filespace::FilespaceRole::primary_candidate;
  }
  return filespace::FilespaceRole::secondary_data;
}

filespace::FilespaceOperation LifecycleOperationFor(std::string_view operation_id) {
  if (operation_id == "filespace.create") return filespace::FilespaceOperation::create_filespace;
  if (operation_id == "filespace.attach") return filespace::FilespaceOperation::attach_filespace;
  if (operation_id == "filespace.disconnect") return filespace::FilespaceOperation::detach_filespace;
  if (operation_id == "filespace.detach") return filespace::FilespaceOperation::detach_filespace;
  if (operation_id == "filespace.move") return filespace::FilespaceOperation::move_filespace;
  if (operation_id == "filespace.merge") return filespace::FilespaceOperation::merge_filespace;
  if (operation_id == "filespace.promote") return filespace::FilespaceOperation::promote_filespace;
  if (operation_id == "filespace.verify") return filespace::FilespaceOperation::verify_filespace;
  if (operation_id == "filespace.compact") return filespace::FilespaceOperation::compact_filespace;
  if (operation_id == "filespace.fence") return filespace::FilespaceOperation::set_read_only;
  if (operation_id == "filespace.release") return filespace::FilespaceOperation::set_read_write;
  if (operation_id == "filespace.archive") return filespace::FilespaceOperation::assign_archive_owner;
  if (operation_id == "filespace.quarantine") return filespace::FilespaceOperation::quarantine_filespace;
  if (operation_id == "filespace.snapshot.create") return filespace::FilespaceOperation::create_snapshot_filespace;
  if (operation_id == "filespace.shadow.create") return filespace::FilespaceOperation::create_shadow_filespace;
  if (operation_id == "filespace.snapshot.refresh" ||
      operation_id == "filespace.shadow.refresh") {
    return filespace::FilespaceOperation::refresh_snapshot_or_shadow;
  }
  if (operation_id == "filespace.snapshot.validate" ||
      operation_id == "filespace.shadow.validate") {
    return filespace::FilespaceOperation::verify_filespace;
  }
  if (operation_id == "filespace.snapshot.retire" ||
      operation_id == "filespace.shadow.retire") {
    return filespace::FilespaceOperation::retire_snapshot_or_shadow;
  }
  if (operation_id == "filespace.shadow.promote") return filespace::FilespaceOperation::promote_filespace;
  if (operation_id == "filespace.truncate") return filespace::FilespaceOperation::truncate_filespace;
  if (operation_id == "filespace.drop") return filespace::FilespaceOperation::drop_filespace;
  if (operation_id == "filespace.delete_physical") return filespace::FilespaceOperation::delete_physical_filespace;
  if (operation_id == "filespace.repair") return filespace::FilespaceOperation::repair_filespace;
  if (operation_id == "filespace.rebuild") return filespace::FilespaceOperation::rebuild_filespace;
  if (operation_id == "filespace.salvage") return filespace::FilespaceOperation::salvage_filespace;
  return filespace::FilespaceOperation::verify_filespace;
}

std::string DefaultLifecyclePath(const EngineApiRequest& request,
                                 const EngineObjectReference& target) {
  const std::string configured = OptionValue(request, "filespace.path:");
  if (!configured.empty()) return configured;
  const std::string base = request.context.database_path.empty()
                               ? std::string("/tmp/scratchbird-filespace")
                               : request.context.database_path;
  return base + "." + target.uuid.canonical + ".filespace";
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

EngineFilespaceLifecycleResult FilespaceLifecycleFailure(
    const EngineFilespaceLifecycleRequest& request,
    std::string operation_id,
    EngineApiDiagnostic diagnostic) {
  auto result = MakeApiBehaviorDiagnostic<EngineFilespaceLifecycleResult>(
      request.context, std::move(operation_id), std::move(diagnostic));
  AddApiBehaviorEvidence(&result, "filespace_lifecycle_refused",
                         result.diagnostics.empty() ? std::string{} : result.diagnostics.front().code);
  AddApiBehaviorRow(&result,
                    {{"storage_executor", "ApplyFilespaceOperation"},
                     {"storage_execution", "refused"},
                     {"parser_storage_authority", "false"}});
  return result;
}

const char* DiscoveryScopeOperationId(EngineFilespaceDiscoveryScope scope) {
  switch (scope) {
    case EngineFilespaceDiscoveryScope::all: return "filespace.discovery.scan";
    case EngineFilespaceDiscoveryScope::orphan_only:
      return "filespace.discovery.orphan_scan";
    case EngineFilespaceDiscoveryScope::stale_only:
      return "filespace.discovery.stale_scan";
  }
  return "filespace.discovery.scan";
}

EngineFilespaceDiscoveryResult FilespaceDiscoveryFailure(
    const EngineFilespaceDiscoveryRequest& request,
    EngineApiDiagnostic diagnostic) {
  auto result = MakeApiBehaviorDiagnostic<EngineFilespaceDiscoveryResult>(
      request.context,
      DiscoveryScopeOperationId(request.discovery_scope),
      std::move(diagnostic));
  result.discovery_scope = request.discovery_scope;
  AddApiBehaviorEvidence(
      &result,
      "filespace_discovery_refused",
      result.diagnostics.empty() ? std::string{} : result.diagnostics.front().code);
  AddApiBehaviorRow(&result,
                    {{"storage_executor", "DiscoverFilespaceAnomalies"},
                     {"storage_execution", "refused"},
                     {"durable_state_changed", "false"},
                     {"parser_storage_authority", "false"},
                     {"transaction_finality_authority", "false"},
                     {"runtime_filesystem_scan_executed", "false"}});
  return result;
}

EngineApiDiagnostic FilespaceDiscoveryOpenStateDiagnostic(
    const EngineApiRequest& request,
    const char* operation) {
  if (RequestReadOnlyMode(request)) {
    return MakeEngineApiDiagnostic("FILESPACE_DISCOVERY.READ_ONLY_DENIED",
                                   "filespace.discovery.read_only_denied",
                                   operation,
                                   true);
  }
  if (SecurityOptionPresent(request, "lifecycle:shutdown")) {
    return MakeEngineApiDiagnostic("FILESPACE_DISCOVERY.SHUTDOWN_IN_PROGRESS",
                                   "filespace.discovery.shutdown_in_progress",
                                   operation,
                                   true);
  }
  if (SecurityOptionPresent(request, "lifecycle:restricted_open")) {
    return MakeEngineApiDiagnostic("FILESPACE_DISCOVERY.RESTRICTED_DENIED",
                                   "filespace.discovery.restricted_denied",
                                   operation,
                                   true);
  }
  if (SecurityOptionPresent(request, "lifecycle:repair")) {
    return MakeEngineApiDiagnostic("FILESPACE_DISCOVERY.REPAIR_DENIED",
                                   "filespace.discovery.repair_denied",
                                   operation,
                                   true);
  }
  if (SecurityOptionPresent(request, "lifecycle:backup") ||
      SecurityOptionPresent(request, "lifecycle:restore")) {
    return MakeEngineApiDiagnostic("FILESPACE_DISCOVERY.BACKUP_HOLD_DENIED",
                                   "filespace.discovery.backup_hold_denied",
                                   operation,
                                   true);
  }
  return {};
}

const char* PackageOperationId(EngineFilespacePackageAction operation) {
  switch (operation) {
    case EngineFilespacePackageAction::export_manifest:
      return "filespace.package.export_manifest";
    case EngineFilespacePackageAction::inspect_manifest:
      return "filespace.package.inspect_manifest";
    case EngineFilespacePackageAction::import_to_quarantine:
      return "filespace.package.import_to_quarantine";
    case EngineFilespacePackageAction::admit:
      return "filespace.package.admit";
    case EngineFilespacePackageAction::reject:
      return "filespace.package.reject";
  }
  return "filespace.package.inspect_manifest";
}

const char* PackageStorageExecutor(EngineFilespacePackageAction operation) {
  switch (operation) {
    case EngineFilespacePackageAction::export_manifest:
      return "ExportFilespacePackageManifest";
    case EngineFilespacePackageAction::inspect_manifest:
      return "InspectFilespacePackageManifest";
    case EngineFilespacePackageAction::import_to_quarantine:
      return "ImportFilespacePackageToQuarantine";
    case EngineFilespacePackageAction::admit:
      return "AdmitFilespacePackage";
    case EngineFilespacePackageAction::reject:
      return "RejectFilespacePackage";
  }
  return "InspectFilespacePackageManifest";
}

bool IsPackageMutation(EngineFilespacePackageAction operation) {
  return operation == EngineFilespacePackageAction::import_to_quarantine ||
         operation == EngineFilespacePackageAction::admit ||
         operation == EngineFilespacePackageAction::reject;
}

EngineFilespacePackageResult FilespacePackageFailure(
    const EngineFilespacePackageRequest& request,
    EngineApiDiagnostic diagnostic,
    bool runtime_package_file_io_executed = false) {
  auto result = MakeApiBehaviorDiagnostic<EngineFilespacePackageResult>(
      request.context,
      PackageOperationId(request.package_operation),
      std::move(diagnostic));
  result.package_operation = request.package_operation;
  AddApiBehaviorEvidence(
      &result,
      "filespace_package_refused",
      result.diagnostics.empty() ? std::string{} : result.diagnostics.front().code);
  AddApiBehaviorRow(&result,
                    {{"storage_executor", PackageStorageExecutor(request.package_operation)},
                     {"storage_execution", "refused"},
                     {"durable_state_changed", "false"},
                     {"runtime_package_file_io_executed",
                      runtime_package_file_io_executed ? "true" : "false"},
                     {"parser_file_io_authority", "false"},
                     {"parser_storage_authority", "false"},
                     {"transaction_finality_authority", "false"},
                     {"recovery_authority", "false"},
                     {"donor_wal_recovery_authority", "false"},
                     {"private_provider_dispatch", "false"}});
  return result;
}

EngineApiDiagnostic FilespacePackageOpenStateDiagnostic(
    const EngineApiRequest& request,
    const char* operation) {
  if (RequestReadOnlyMode(request)) {
    return MakeEngineApiDiagnostic("FILESPACE_PACKAGE.READ_ONLY_DENIED",
                                   "filespace.package.read_only_denied",
                                   operation,
                                   true);
  }
  if (SecurityOptionPresent(request, "lifecycle:shutdown")) {
    return MakeEngineApiDiagnostic("FILESPACE_PACKAGE.SHUTDOWN_IN_PROGRESS",
                                   "filespace.package.shutdown_in_progress",
                                   operation,
                                   true);
  }
  if (SecurityOptionPresent(request, "lifecycle:restricted_open")) {
    return MakeEngineApiDiagnostic("FILESPACE_PACKAGE.RESTRICTED_DENIED",
                                   "filespace.package.restricted_denied",
                                   operation,
                                   true);
  }
  if (SecurityOptionPresent(request, "lifecycle:repair")) {
    return MakeEngineApiDiagnostic("FILESPACE_PACKAGE.REPAIR_DENIED",
                                   "filespace.package.repair_denied",
                                   operation,
                                   true);
  }
  if (SecurityOptionPresent(request, "lifecycle:backup") ||
      SecurityOptionPresent(request, "lifecycle:restore")) {
    return MakeEngineApiDiagnostic("FILESPACE_PACKAGE.BACKUP_HOLD_DENIED",
                                   "filespace.package.backup_hold_denied",
                                   operation,
                                   true);
  }
  return {};
}

filespace::FilespaceDescriptor PackageDescriptorForRoute(
    const platform::TypedUuid& database_uuid,
    const platform::TypedUuid& filespace_uuid,
    const platform::TypedUuid& writer_uuid,
    std::string path,
    platform::u64 generation,
    platform::u16 physical_id) {
  filespace::FilespaceDescriptor descriptor;
  descriptor.database_uuid = database_uuid;
  descriptor.filespace_uuid = filespace_uuid;
  descriptor.path = std::move(path);
  descriptor.role = filespace::FilespaceRole::secondary_data;
  descriptor.state = filespace::FilespaceState::detached;
  descriptor.page_size =
      static_cast<platform::u32>(scratchbird::storage::disk::PageSizeProfile::profile_16k);
  descriptor.generation = generation;
  descriptor.header_generation = generation;
  descriptor.physical_filespace_id = physical_id;
  descriptor.writer_identity_uuid = writer_uuid;
  descriptor.read_only = false;
  descriptor.active = false;
  descriptor.startup_authority = false;
  descriptor.catalog_persistence_owner = false;
  descriptor.filespace_manifest_owner = false;
  descriptor.recovery_evidence_owner = false;
  descriptor.first_filespace = false;
  return descriptor;
}

filespace::FilespacePackageRequest BuildPackageStorageRequest(
    const EngineFilespacePackageRequest& request,
    const platform::TypedUuid& database_uuid) {
  const std::string key = LifecycleRegistryKey(request.context) + "|filespace.package|" +
                          PackageOperationId(request.package_operation);
  const auto writer_uuid = GeneratedIdentity(platform::UuidKind::object, key, 121);
  const auto filespace_uuid = GeneratedIdentity(platform::UuidKind::filespace, key, 127);

  filespace::FilespacePackageRequest storage_request;
  storage_request.package_uuid = GeneratedIdentity(platform::UuidKind::object, key, 131);
  storage_request.database_uuid = database_uuid;
  storage_request.target_database_uuid = database_uuid;
  storage_request.package_name = "route_filespace_package";
  storage_request.operator_identity = request.context.principal_uuid.canonical.empty()
                                          ? "engine.operator"
                                          : request.context.principal_uuid.canonical;
  storage_request.descriptors.push_back(
      PackageDescriptorForRoute(database_uuid,
                                filespace_uuid,
                                writer_uuid,
                                "/tmp/scratchbird-package-member.fsp",
                                7,
                                51));

  const auto exported = filespace::ExportFilespacePackageManifest(storage_request);
  if (exported.ok()) {
    storage_request.manifest = exported.manifest;
    storage_request.inspection_passed = true;
  }
  storage_request.admission_authorized =
      request.package_operation == EngineFilespacePackageAction::admit;
  storage_request.reject_authorized =
      request.package_operation == EngineFilespacePackageAction::reject;
  return storage_request;
}

EngineFilespacePackageResult FilespacePackageStorageFailure(
    const EngineFilespacePackageRequest& request,
    const filespace::FilespacePackageResult& storage_result) {
  return FilespacePackageFailure(
      request,
      MakeEngineApiDiagnostic(storage_result.diagnostic.diagnostic_code.empty()
                                  ? "FILESPACE_PACKAGE.STORAGE_WORKFLOW_FAILED"
                                  : storage_result.diagnostic.diagnostic_code,
	                              storage_result.diagnostic.message_key.empty()
	                                  ? "filespace.package.storage_workflow_failed"
	                                  : storage_result.diagnostic.message_key,
	                              PackageOperationId(request.package_operation),
	                              true));
}

EngineFilespacePackageResult FilespacePackageFileFailure(
    const EngineFilespacePackageRequest& request,
    const filespace::FilespacePackageFileResult& file_result) {
  return FilespacePackageFailure(
      request,
      MakeEngineApiDiagnostic(file_result.diagnostic.diagnostic_code.empty()
                                  ? "FILESPACE_PACKAGE.FILE_WORKFLOW_FAILED"
                                  : file_result.diagnostic.diagnostic_code,
                              file_result.diagnostic.message_key.empty()
                                  ? "filespace.package.file_workflow_failed"
                                  : file_result.diagnostic.message_key,
                              PackageOperationId(request.package_operation),
                              true),
      file_result.runtime_package_file_io_executed);
}

filespace::FilespaceDescriptor DiscoveryExpectedFilespace(
    const platform::TypedUuid& database_uuid,
    const platform::TypedUuid& filespace_uuid,
    const platform::TypedUuid& writer_uuid,
    std::string path,
    platform::u64 generation,
    platform::u16 physical_id) {
  filespace::FilespaceDescriptor descriptor;
  descriptor.database_uuid = database_uuid;
  descriptor.filespace_uuid = filespace_uuid;
  descriptor.path = std::move(path);
  descriptor.role = filespace::FilespaceRole::secondary_data;
  descriptor.state = filespace::FilespaceState::online;
  descriptor.page_size =
      static_cast<platform::u32>(scratchbird::storage::disk::PageSizeProfile::profile_16k);
  descriptor.generation = generation;
  descriptor.header_generation = generation;
  descriptor.physical_filespace_id = physical_id;
  descriptor.writer_identity_uuid = writer_uuid;
  descriptor.active = true;
  return descriptor;
}

filespace::FilespaceDiscoveryCandidate DiscoveryObservedFilespace(
    const filespace::FilespaceDescriptor& descriptor) {
  filespace::FilespaceDiscoveryCandidate candidate;
  candidate.database_uuid = descriptor.database_uuid;
  candidate.filespace_uuid = descriptor.filespace_uuid;
  candidate.path = descriptor.path;
  candidate.role = descriptor.role;
  candidate.state = descriptor.state;
  candidate.page_size = descriptor.page_size;
  candidate.physical_filespace_id = descriptor.physical_filespace_id;
  candidate.header_generation = descriptor.header_generation;
  candidate.writer_identity_uuid = descriptor.writer_identity_uuid;
  candidate.physical_header_present = true;
  return candidate;
}

filespace::FilespaceDiscoveryRequest BuildDiscoveryStorageRequest(
    const EngineFilespaceDiscoveryRequest& request,
    const platform::TypedUuid& database_uuid) {
  const std::string key = LifecycleRegistryKey(request.context) + "|filespace.discovery";
  const auto writer_uuid = GeneratedIdentity(platform::UuidKind::object, key, 91);

  filespace::FilespaceDiscoveryRequest storage_request;
  storage_request.database_uuid = database_uuid;
  storage_request.quarantine_unmatched_observed = true;
  storage_request.release_requires_authority = true;
  storage_request.require_operator_review_for_anomalies = true;

  if (!request.expected_filespaces.empty()) {
    storage_request.expected = request.expected_filespaces;
    return storage_request;
  }

  if (request.discovery_scope == EngineFilespaceDiscoveryScope::orphan_only) {
    const auto orphan_uuid = GeneratedIdentity(platform::UuidKind::filespace, key, 97);
    const auto orphan = DiscoveryExpectedFilespace(database_uuid,
                                                   orphan_uuid,
                                                   writer_uuid,
                                                   "/tmp/scratchbird-discovery-orphan.fsp",
                                                   4,
                                                   41);
    storage_request.observed.push_back(DiscoveryObservedFilespace(orphan));
    return storage_request;
  }

  const auto primary_uuid = GeneratedIdentity(platform::UuidKind::filespace, key, 101);
  const auto expected = DiscoveryExpectedFilespace(database_uuid,
                                                  primary_uuid,
                                                  writer_uuid,
                                                  "/tmp/scratchbird-discovery-primary.fsp",
                                                  5,
                                                  42);
  storage_request.expected.push_back(expected);

  auto observed = DiscoveryObservedFilespace(expected);
  if (request.discovery_scope == EngineFilespaceDiscoveryScope::stale_only) {
    observed.header_generation = 3;
  }
  storage_request.observed.push_back(observed);

  if (request.discovery_scope == EngineFilespaceDiscoveryScope::all) {
    const auto stale_uuid = GeneratedIdentity(platform::UuidKind::filespace, key, 103);
    const auto stale_expected = DiscoveryExpectedFilespace(database_uuid,
                                                          stale_uuid,
                                                          writer_uuid,
                                                          "/tmp/scratchbird-discovery-stale.fsp",
                                                          9,
                                                          43);
    storage_request.expected.push_back(stale_expected);
    auto stale_observed = DiscoveryObservedFilespace(stale_expected);
    stale_observed.header_generation = 6;
    storage_request.observed.push_back(stale_observed);

    const auto orphan_uuid = GeneratedIdentity(platform::UuidKind::filespace, key, 107);
    const auto orphan = DiscoveryExpectedFilespace(database_uuid,
                                                   orphan_uuid,
                                                   writer_uuid,
                                                   "/tmp/scratchbird-discovery-orphan.fsp",
                                                   2,
                                                   44);
    storage_request.observed.push_back(DiscoveryObservedFilespace(orphan));
  }

  return storage_request;
}

bool TierClassKnown(EngineStorageTierClass tier_class) {
  return tier_class != EngineStorageTierClass::unknown;
}

bool IsTierMutation(EngineStorageTierMigrationOperation operation) {
  return operation == EngineStorageTierMigrationOperation::stage_migration ||
         operation == EngineStorageTierMigrationOperation::commit_migration ||
         operation == EngineStorageTierMigrationOperation::rollback_migration;
}

bool IsPhysicalTierRelocationOperation(
    EngineStorageTierMigrationOperation operation) {
  return operation == EngineStorageTierMigrationOperation::stage_migration ||
         operation == EngineStorageTierMigrationOperation::commit_migration;
}

bool RequiresTypedDependencyValidation(
    const EngineStorageTierMigrationDescriptor& descriptor) {
  for (const auto page_type : descriptor.page_types) {
    const auto lookup = page::LookupPageFamily(page_type);
    if (lookup.ok() &&
        (lookup.descriptor.typed_payload_dependency_required ||
         lookup.descriptor.resource_dependency_required)) {
      return true;
    }
  }
  return false;
}

bool FileDigestAndSize(const std::filesystem::path& path,
                       std::string* digest,
                       platform::u64* byte_count,
                       std::string* detail) {
  if (digest == nullptr) {
    return false;
  }
  std::error_code fs_error;
  const auto status = std::filesystem::symlink_status(path, fs_error);
  if (fs_error || !std::filesystem::exists(status) ||
      !std::filesystem::is_regular_file(status)) {
    if (detail != nullptr) {
      *detail = fs_error ? fs_error.message() : path.string();
    }
    return false;
  }

  std::ifstream input(path, std::ios::binary);
  if (!input.is_open()) {
    if (detail != nullptr) *detail = path.string();
    return false;
  }

  std::uint64_t hash = 1469598103934665603ull;
  char buffer[8192];
  while (input) {
    input.read(buffer, sizeof(buffer));
    const auto count = input.gcount();
    for (std::streamsize index = 0; index < count; ++index) {
      hash ^= static_cast<std::uint64_t>(
          static_cast<unsigned char>(buffer[index]));
      hash *= 1099511628211ull;
    }
  }
  if (!input.eof()) {
    if (detail != nullptr) *detail = path.string();
    return false;
  }

  const auto size = std::filesystem::file_size(path, fs_error);
  if (fs_error) {
    if (detail != nullptr) *detail = fs_error.message();
    return false;
  }

  std::ostringstream out;
  out << std::hex << std::setw(16) << std::setfill('0') << hash;
  *digest = out.str();
  if (byte_count != nullptr) {
    *byte_count = static_cast<platform::u64>(size);
  }
  return true;
}

bool CopyStorageTierPhysicalRelocationFile(
    const EngineStorageTierMigrationDescriptor& descriptor,
    platform::u64* byte_count,
    std::string* detail) {
  const std::filesystem::path source(descriptor.source_physical_path);
  const std::filesystem::path target(descriptor.target_physical_path);
  std::error_code fs_error;
  const auto source_status = std::filesystem::symlink_status(source, fs_error);
  if (fs_error || !std::filesystem::exists(source_status) ||
      !std::filesystem::is_regular_file(source_status)) {
    if (detail != nullptr) {
      *detail = fs_error ? fs_error.message() : source.string();
    }
    return false;
  }

  if (std::filesystem::equivalent(source, target, fs_error) && !fs_error) {
    if (detail != nullptr) *detail = source.string();
    return false;
  }
  fs_error.clear();

  const auto parent = target.parent_path();
  if (!parent.empty()) {
    std::filesystem::create_directories(parent, fs_error);
    if (fs_error) {
      if (detail != nullptr) *detail = fs_error.message();
      return false;
    }
  }

  if (std::filesystem::exists(target, fs_error)) {
    if (fs_error) {
      if (detail != nullptr) *detail = fs_error.message();
      return false;
    }
    if (!descriptor.allow_physical_target_overwrite) {
      if (detail != nullptr) *detail = target.string();
      return false;
    }
  }
  fs_error.clear();

  const auto options =
      descriptor.allow_physical_target_overwrite
          ? std::filesystem::copy_options::overwrite_existing
          : std::filesystem::copy_options::none;
  if (!std::filesystem::copy_file(source, target, options, fs_error)) {
    if (detail != nullptr) {
      *detail = fs_error ? fs_error.message() : target.string();
    }
    return false;
  }

  std::string source_digest;
  std::string target_digest;
  platform::u64 source_size = 0;
  platform::u64 target_size = 0;
  if (!FileDigestAndSize(source, &source_digest, &source_size, detail) ||
      !FileDigestAndSize(target, &target_digest, &target_size, detail) ||
      source_size != target_size || source_digest != target_digest) {
    if (detail != nullptr && detail->empty()) *detail = target.string();
    return false;
  }
  if (byte_count != nullptr) {
    *byte_count = source_size;
  }
  return true;
}

bool VerifyStorageTierPhysicalRelocationFile(
    const EngineStorageTierMigrationDescriptor& descriptor,
    platform::u64* byte_count,
    std::string* detail) {
  std::string source_digest;
  std::string target_digest;
  platform::u64 source_size = 0;
  platform::u64 target_size = 0;
  if (!FileDigestAndSize(descriptor.source_physical_path,
                         &source_digest,
                         &source_size,
                         detail) ||
      !FileDigestAndSize(descriptor.target_physical_path,
                         &target_digest,
                         &target_size,
                         detail) ||
      source_size != target_size || source_digest != target_digest) {
    if (detail != nullptr && detail->empty()) {
      *detail = descriptor.target_physical_path;
    }
    return false;
  }
  if (byte_count != nullptr) {
    *byte_count = source_size;
  }
  return true;
}

EngineStorageTierMigrationResult StorageTierMigrationFailure(
    const EngineStorageTierMigrationRequest& request,
    EngineApiDiagnostic diagnostic) {
  auto result = MakeApiBehaviorDiagnostic<EngineStorageTierMigrationResult>(
      request.context,
      EngineStorageTierMigrationOperationName(request.tier_operation),
      std::move(diagnostic));
  result.descriptor = request.descriptor;
  AddApiBehaviorEvidence(&result,
                         "storage_tier_migration_refused",
                         result.diagnostics.empty() ? std::string{} : result.diagnostics.front().code);
  AddApiBehaviorRow(&result,
                    {{"storage_executor", "EnginePlanStorageTierMigrationOperation"},
                     {"storage_tier_descriptor_plan_only", "true"},
                     {"durable_state_changed", "false"},
                     {"physical_data_movement_dispatched", "false"},
                     {"parser_storage_authority", "false"}});
  return result;
}

}  // namespace

const char* EngineStorageTierClassName(EngineStorageTierClass tier_class) {
  switch (tier_class) {
    case EngineStorageTierClass::unknown: return "unknown";
    case EngineStorageTierClass::hot: return "hot";
    case EngineStorageTierClass::warm: return "warm";
    case EngineStorageTierClass::cold: return "cold";
    case EngineStorageTierClass::archive: return "archive";
    case EngineStorageTierClass::nvme: return "nvme";
    case EngineStorageTierClass::ssd: return "ssd";
    case EngineStorageTierClass::hdd: return "hdd";
    case EngineStorageTierClass::custom: return "custom";
  }
  return "unknown";
}

const char* EngineStorageTierMigrationOperationName(
    EngineStorageTierMigrationOperation operation) {
  switch (operation) {
    case EngineStorageTierMigrationOperation::inspect: return "storage_tier.inspect";
    case EngineStorageTierMigrationOperation::validate: return "storage_tier.validate";
    case EngineStorageTierMigrationOperation::plan_migration: return "storage_tier.plan_migration";
    case EngineStorageTierMigrationOperation::stage_migration: return "storage_tier.stage_migration";
    case EngineStorageTierMigrationOperation::commit_migration: return "storage_tier.commit_migration";
    case EngineStorageTierMigrationOperation::rollback_migration: return "storage_tier.rollback_migration";
  }
  return "storage_tier.unknown";
}

const char* EngineFilespaceDiscoveryScopeName(EngineFilespaceDiscoveryScope scope) {
  switch (scope) {
    case EngineFilespaceDiscoveryScope::all: return "all";
    case EngineFilespaceDiscoveryScope::orphan_only: return "orphan_only";
    case EngineFilespaceDiscoveryScope::stale_only: return "stale_only";
  }
  return "all";
}

const char* EngineFilespacePackageOperationName(
    EngineFilespacePackageAction operation) {
  return PackageOperationId(operation);
}

EngineFilespaceDiscoveryResult EngineDiscoverFilespaceAnomalies(
    const EngineFilespaceDiscoveryRequest& request) {
  const char* operation = DiscoveryScopeOperationId(request.discovery_scope);
  const bool action_execution_requested =
      request.execute_quarantine_actions || request.execute_release_actions ||
      request.execute_physical_cleanup_actions;
  if (!request.context.security_context_present) {
    return FilespaceDiscoveryFailure(
        request,
        MakeInvalidRequestDiagnostic(operation, "security_context_required"));
  }
  if (!SecurityContextHasRight(request.context, "OBS_CONFIG_INSPECT")) {
    return FilespaceDiscoveryFailure(
        request,
        MakeEngineApiDiagnostic("FILESPACE_DISCOVERY.PERMISSION_DENIED",
                                "filespace.discovery.permission_denied",
                                "OBS_CONFIG_INSPECT",
                                true));
  }
  if (action_execution_requested &&
      !SecurityContextHasRight(request.context, "FILESPACE_LIFECYCLE_CONTROL")) {
    return FilespaceDiscoveryFailure(
        request,
        MakeEngineApiDiagnostic("FILESPACE_DISCOVERY.LIFECYCLE_PERMISSION_DENIED",
                                "filespace.discovery.lifecycle_permission_denied",
                                "FILESPACE_LIFECYCLE_CONTROL",
                                true));
  }
  if (request.mutation_requested && !action_execution_requested) {
    return FilespaceDiscoveryFailure(
        request,
        MakeInvalidRequestDiagnostic(operation, "discovery_mutation_action_required"));
  }
  if (action_execution_requested) {
    const auto open_state_diagnostic =
        FilespaceDiscoveryOpenStateDiagnostic(request, operation);
    if (!open_state_diagnostic.code.empty()) {
      return FilespaceDiscoveryFailure(request, open_state_diagnostic);
    }
    if (request.context.local_transaction_id == 0) {
      return FilespaceDiscoveryFailure(
          request,
          MakeInvalidRequestDiagnostic(operation, "local_transaction_id_required"));
    }
  }
  if (request.context.database_uuid.canonical.empty()) {
    return FilespaceDiscoveryFailure(
        request,
        MakeInvalidRequestDiagnostic(operation, "database_uuid_required"));
  }
  if (request.parser_filesystem_authority || request.parser_storage_authority ||
      request.transaction_finality_authority || request.recovery_authority ||
      request.donor_or_wal_recovery_authority) {
    return FilespaceDiscoveryFailure(
        request,
        MakeEngineApiDiagnostic("FILESPACE_DISCOVERY.AUTHORITY_REFUSED",
                                "filespace.discovery.authority_refused",
                                operation,
	                                true));
  }

  const auto database_uuid = ParseEngineIdentity(platform::UuidKind::database,
                                                request.context.database_uuid.canonical);
  if (!database_uuid.valid()) {
    return FilespaceDiscoveryFailure(
        request,
        MakeInvalidRequestDiagnostic(operation, "database_uuid_invalid_for_storage_route"));
  }

  filespace::FilespaceDiscoveryRequest storage_request;
  filespace::FilespaceDiscoveryResult storage_result;
  bool runtime_filesystem_scan_executed = false;
  if (request.runtime_filesystem_scan_requested) {
    auto baseline_request = BuildDiscoveryStorageRequest(request, database_uuid);
    filespace::FilespaceDiscoveryFilesystemScanRequest scan_request;
    scan_request.database_uuid = database_uuid;
    scan_request.expected = request.expected_filespaces.empty()
                                ? baseline_request.expected
                                : request.expected_filespaces;
    scan_request.observed_paths = request.runtime_scan_paths;
    scan_request.quarantine_unmatched_observed = true;
    scan_request.release_requires_authority = true;
    scan_request.require_operator_review_for_anomalies = true;
    const auto scan_result = filespace::DiscoverFilespaceAnomaliesFromFilesystem(scan_request);
    if (!scan_result.ok()) {
      return FilespaceDiscoveryFailure(
          request,
          MakeEngineApiDiagnostic(scan_result.diagnostic.diagnostic_code.empty()
                                      ? "FILESPACE_DISCOVERY.RUNTIME_SCAN_FAILED"
                                      : scan_result.diagnostic.diagnostic_code,
                                  scan_result.diagnostic.message_key.empty()
                                      ? "filespace.discovery.runtime_scan_failed"
                                      : scan_result.diagnostic.message_key,
                                  operation,
                                  true));
    }
    storage_request.database_uuid = database_uuid;
    storage_request.expected = scan_request.expected;
    storage_request.observed = scan_result.observed;
    storage_request.quarantine_unmatched_observed =
        scan_request.quarantine_unmatched_observed;
    storage_request.release_requires_authority =
        scan_request.release_requires_authority;
    storage_request.require_operator_review_for_anomalies =
        scan_request.require_operator_review_for_anomalies;
    storage_result = scan_result.discovery;
    runtime_filesystem_scan_executed = scan_result.runtime_filesystem_scan_executed;
  } else {
    storage_request = BuildDiscoveryStorageRequest(request, database_uuid);
    storage_result = filespace::DiscoverFilespaceAnomalies(storage_request);
  }
  if (!storage_result.ok()) {
    return FilespaceDiscoveryFailure(
        request,
        MakeEngineApiDiagnostic(storage_result.diagnostic.diagnostic_code.empty()
                                    ? "FILESPACE_DISCOVERY.CLASSIFIER_FAILED"
                                    : storage_result.diagnostic.diagnostic_code,
                                storage_result.diagnostic.message_key.empty()
                                    ? "filespace.discovery.classifier_failed"
                                    : storage_result.diagnostic.message_key,
                                operation,
                                true));
  }

  filespace::FilespaceDiscoveryExecutionResult execution_result;
  if (action_execution_requested) {
    filespace::FilespaceRegistry execution_registry;
    execution_registry.filespaces = storage_request.expected;
    filespace::FilespaceDiscoveryExecutionRequest execution_request;
    execution_request.discovery = storage_request;
    execution_request.execute_quarantine_actions = request.execute_quarantine_actions;
    execution_request.execute_release_actions = request.execute_release_actions;
    execution_request.execute_physical_cleanup_actions =
        request.execute_physical_cleanup_actions;
    execution_request.physical_header_required_for_quarantine =
        request.physical_header_required_for_quarantine;
    execution_request.header_inspection_passed = request.header_inspection_passed;
    execution_request.release_authorized = request.release_authorized;
    execution_request.allow_physical_filespace_delete =
        request.allow_physical_filespace_delete;
    execution_request.physical_delete_retention_satisfied =
        request.physical_delete_retention_satisfied;
    execution_request.physical_delete_legal_hold_clear =
        request.physical_delete_legal_hold_clear;
    execution_request.physical_delete_cleanup_horizon_authoritative =
        request.physical_delete_cleanup_horizon_authoritative;
    execution_request.operation_uuid = operation;
    execution_request.inspector_uuid = request.inspector_uuid;
    execution_request.release_authority_uuid = request.release_authority_uuid;
    execution_result =
        filespace::ExecuteFilespaceDiscoveryActions(&execution_registry, execution_request);
    if (!execution_result.ok()) {
      return FilespaceDiscoveryFailure(
          request,
          MakeEngineApiDiagnostic(execution_result.diagnostic.diagnostic_code.empty()
                                      ? "FILESPACE_DISCOVERY.EXECUTION_FAILED"
                                      : execution_result.diagnostic.diagnostic_code,
                                  execution_result.diagnostic.message_key.empty()
                                      ? "filespace.discovery.execution_failed"
                                      : execution_result.diagnostic.message_key,
                                  operation,
                                  true));
    }
    storage_result = execution_result.discovery;
  }

  auto result = MakeApiBehaviorSuccess<EngineFilespaceDiscoveryResult>(
      request.context, operation);
  result.discovery_scope = request.discovery_scope;
  result.anomaly_count = storage_result.anomaly_count;
  result.quarantine_execution_count =
      action_execution_requested ? execution_result.quarantine_execution_count : 0;
  result.release_execution_count =
      action_execution_requested ? execution_result.release_execution_count : 0;
  result.physical_cleanup_execution_count =
      action_execution_requested ? execution_result.physical_cleanup_execution_count : 0;
  result.quarantine_required = storage_result.quarantine_required;
  result.operator_review_required = storage_result.operator_review_required;
  result.durable_state_changed =
      action_execution_requested && execution_result.durable_state_changed;
  result.cache_invalidation_required = storage_result.cache_invalidation_required;
  result.cleanup_or_quarantine_executed =
      action_execution_requested && execution_result.cleanup_or_quarantine_executed;
  result.release_executed =
      action_execution_requested && execution_result.release_executed;
  result.physical_cleanup_executed =
      action_execution_requested && execution_result.physical_cleanup_executed;
  result.physical_file_removed =
      action_execution_requested && execution_result.physical_file_removed;
  result.runtime_filesystem_scan_executed = runtime_filesystem_scan_executed;

  AddApiBehaviorRow(&result,
                    {{"operation_id", operation},
                     {"storage_executor",
                      action_execution_requested ? "ExecuteFilespaceDiscoveryActions"
                                                 : "DiscoverFilespaceAnomalies"},
                     {"discovery_scope", EngineFilespaceDiscoveryScopeName(request.discovery_scope)},
                     {"anomaly_count", std::to_string(result.anomaly_count)},
                     {"quarantine_execution_count",
                      std::to_string(result.quarantine_execution_count)},
                     {"release_execution_count",
                      std::to_string(result.release_execution_count)},
                     {"physical_cleanup_execution_count",
                      std::to_string(result.physical_cleanup_execution_count)},
                     {"quarantine_required", result.quarantine_required ? "true" : "false"},
                     {"operator_review_required",
                      result.operator_review_required ? "true" : "false"},
                     {"durable_state_changed",
                      result.durable_state_changed ? "true" : "false"},
                     {"cache_invalidation_required",
                      result.cache_invalidation_required ? "true" : "false"},
                     {"cleanup_or_quarantine_executed",
                      result.cleanup_or_quarantine_executed ? "true" : "false"},
                     {"release_executed",
                      result.release_executed ? "true" : "false"},
                     {"physical_cleanup_executed",
                      result.physical_cleanup_executed ? "true" : "false"},
                     {"physical_file_removed",
                      result.physical_file_removed ? "true" : "false"},
	                     {"runtime_filesystem_scan_executed",
	                      runtime_filesystem_scan_executed ? "true" : "false"},
                     {"parser_filesystem_authority", "false"},
                     {"parser_storage_authority", "false"},
                     {"transaction_finality_authority", "false"},
                     {"recovery_authority", "false"},
                     {"donor_wal_recovery_authority", "false"},
                     {"private_provider_dispatch", "false"},
                     {"mga_visibility_authority", "durable_transaction_inventory"}});
  for (const auto& row : storage_result.rows) {
    AddApiBehaviorRow(
        &result,
        {{"filespace_discovery_classification",
          filespace::FilespaceDiscoveryClassificationName(row.classification)},
         {"recommended_action", row.recommended_action},
         {"normal_access_allowed", row.normal_access_allowed ? "true" : "false"},
         {"quarantine_required", row.quarantine_required ? "true" : "false"},
         {"release_requires_authority",
          row.release_requires_authority ? "true" : "false"},
         {"operator_review_required", row.operator_review_required ? "true" : "false"},
         {"cache_invalidation_required",
          row.cache_invalidation_required ? "true" : "false"}});
  }
  AddApiBehaviorEvidence(&result, "filespace_discovery_report", operation);
  AddApiBehaviorEvidence(&result,
                         "filespace_discovery_scope",
                         EngineFilespaceDiscoveryScopeName(request.discovery_scope));
  AddApiBehaviorEvidence(&result,
                         "filespace_discovery_anomaly_count",
                         std::to_string(result.anomaly_count));
  AddApiBehaviorEvidence(&result,
                         "filespace_discovery_quarantine_execution_count",
                         std::to_string(result.quarantine_execution_count));
  AddApiBehaviorEvidence(&result,
                         "filespace_discovery_release_execution_count",
                         std::to_string(result.release_execution_count));
  AddApiBehaviorEvidence(&result,
                         "filespace_discovery_physical_cleanup_execution_count",
                         std::to_string(result.physical_cleanup_execution_count));
  AddApiBehaviorEvidence(&result,
                         "durable_state_changed",
                         result.durable_state_changed ? "true" : "false");
  AddApiBehaviorEvidence(&result,
                         "cleanup_or_quarantine_executed",
                         result.cleanup_or_quarantine_executed ? "true" : "false");
  AddApiBehaviorEvidence(&result,
                         "release_executed",
                         result.release_executed ? "true" : "false");
  AddApiBehaviorEvidence(&result,
                         "physical_cleanup_executed",
                         result.physical_cleanup_executed ? "true" : "false");
  AddApiBehaviorEvidence(&result,
                         "physical_file_removed",
                         result.physical_file_removed ? "true" : "false");
  AddApiBehaviorEvidence(&result,
                         "runtime_filesystem_scan_executed",
                         runtime_filesystem_scan_executed ? "true" : "false");
  AddApiBehaviorEvidence(&result, "parser_filesystem_authority", "false");
  AddApiBehaviorEvidence(&result, "parser_storage_authority", "false");
  AddApiBehaviorEvidence(&result, "transaction_finality_authority", "false");
  AddApiBehaviorEvidence(&result, "recovery_authority", "false");
  AddApiBehaviorEvidence(&result, "donor_wal_recovery_authority", "false");
  AddApiBehaviorEvidence(&result, "private_provider_dispatch", "false");
  AddApiBehaviorEvidence(&result,
                         "mga_visibility_authority",
                         "durable_transaction_inventory");
  result.result_shape.result_kind = "rs.filespace.discovery_report.v1";
  return result;
}

EngineFilespacePackageResult EngineFilespacePackageOperation(
    const EngineFilespacePackageRequest& request) {
  const char* operation = PackageOperationId(request.package_operation);
  if (!request.context.security_context_present) {
    return FilespacePackageFailure(
        request,
        MakeInvalidRequestDiagnostic(operation, "security_context_required"));
  }

  const char* required_right =
      IsPackageMutation(request.package_operation) ? "FILESPACE_LIFECYCLE_CONTROL"
                                                   : "OBS_CONFIG_INSPECT";
  if (!SecurityContextHasRight(request.context, required_right)) {
    return FilespacePackageFailure(
        request,
        MakeEngineApiDiagnostic("FILESPACE_PACKAGE.PERMISSION_DENIED",
                                "filespace.package.permission_denied",
                                required_right,
                                true));
  }

  if (IsPackageMutation(request.package_operation)) {
    const auto open_state_diagnostic =
        FilespacePackageOpenStateDiagnostic(request, operation);
    if (!open_state_diagnostic.code.empty()) {
      return FilespacePackageFailure(request, open_state_diagnostic);
    }
    if (request.context.local_transaction_id == 0) {
      return FilespacePackageFailure(
          request,
          MakeInvalidRequestDiagnostic(operation, "local_transaction_id_required"));
    }
  }

  if (request.context.database_uuid.canonical.empty()) {
    return FilespacePackageFailure(
        request,
        MakeInvalidRequestDiagnostic(operation, "database_uuid_required"));
  }
  if (request.parser_file_io_authority || request.parser_storage_authority ||
      request.transaction_finality_authority || request.recovery_authority ||
      request.donor_or_wal_recovery_authority ||
      request.private_provider_dispatch_requested) {
    return FilespacePackageFailure(
        request,
        MakeEngineApiDiagnostic("FILESPACE_PACKAGE.AUTHORITY_REFUSED",
                                "filespace.package.authority_refused",
                                operation,
                                true));
  }

  const auto database_uuid = ParseEngineIdentity(platform::UuidKind::database,
                                                request.context.database_uuid.canonical);
  if (!database_uuid.valid()) {
    return FilespacePackageFailure(
        request,
        MakeInvalidRequestDiagnostic(operation, "database_uuid_invalid_for_storage_route"));
  }

  auto storage_request = BuildPackageStorageRequest(request, database_uuid);
  filespace::FilespacePackageResult storage_result;
  filespace::FilespaceRegistry registry;
  std::string storage_executor = PackageStorageExecutor(request.package_operation);
  bool runtime_package_file_io_executed = false;
  bool physical_package_transfer_executed = false;
  platform::u64 physical_package_member_count = 0;
  platform::u64 physical_package_byte_count = 0;

  if (request.runtime_package_file_io_requested) {
    if (request.package_file_write_requested == request.package_file_read_requested) {
      return FilespacePackageFailure(
          request,
          MakeInvalidRequestDiagnostic(operation, "package_file_read_or_write_required"));
    }
    if (request.package_file_path.empty()) {
      return FilespacePackageFailure(
          request,
          MakeInvalidRequestDiagnostic(operation, "package_file_path_required"));
    }
    if (request.package_file_write_requested) {
      if (request.package_operation != EngineFilespacePackageAction::export_manifest) {
        return FilespacePackageFailure(
            request,
            MakeInvalidRequestDiagnostic(operation, "package_file_write_requires_export"));
      }
      filespace::FilespacePackageFileWriteRequest file_request;
      file_request.path = request.package_file_path;
      file_request.manifest = storage_request.manifest;
      file_request.allow_overwrite = request.package_file_allow_overwrite;
      file_request.execute_physical_package_transfer =
          request.runtime_physical_package_transfer_requested;
      file_request.allow_physical_package_transfer =
          request.allow_physical_package_transfer;
      const auto file_result = filespace::WriteFilespacePackageFile(file_request);
      if (!file_result.ok()) return FilespacePackageFileFailure(request, file_result);
      storage_result.status = file_result.status;
      storage_result.diagnostic = file_result.diagnostic;
      storage_result.manifest = file_result.manifest;
      storage_executor = "WriteFilespacePackageFile";
      runtime_package_file_io_executed = true;
      physical_package_transfer_executed =
          file_result.physical_package_transfer_executed;
      physical_package_member_count = file_result.physical_member_count;
      physical_package_byte_count = file_result.physical_byte_count;
    } else {
      if (request.package_operation != EngineFilespacePackageAction::inspect_manifest) {
        return FilespacePackageFailure(
            request,
            MakeInvalidRequestDiagnostic(operation, "package_file_read_requires_inspect"));
      }
      filespace::FilespacePackageFileReadRequest file_request;
      file_request.path = request.package_file_path;
      file_request.physical_output_directory =
          request.physical_package_transfer_directory;
      file_request.execute_physical_package_transfer =
          request.runtime_physical_package_transfer_requested;
      file_request.allow_physical_package_transfer =
          request.allow_physical_package_transfer;
      const auto file_result = filespace::ReadFilespacePackageFile(file_request);
      if (!file_result.ok()) return FilespacePackageFileFailure(request, file_result);
      storage_result.status = file_result.status;
      storage_result.diagnostic = file_result.diagnostic;
      storage_result.manifest = file_result.manifest;
      storage_executor = "ReadFilespacePackageFile";
      runtime_package_file_io_executed = true;
      physical_package_transfer_executed =
          file_result.physical_package_transfer_executed;
      physical_package_member_count = file_result.physical_member_count;
      physical_package_byte_count = file_result.physical_byte_count;
    }
  } else {
    switch (request.package_operation) {
      case EngineFilespacePackageAction::export_manifest:
        storage_result = filespace::ExportFilespacePackageManifest(storage_request);
        break;
      case EngineFilespacePackageAction::inspect_manifest:
        storage_result = filespace::InspectFilespacePackageManifest(storage_request);
        break;
      case EngineFilespacePackageAction::import_to_quarantine:
        storage_result = filespace::ImportFilespacePackageToQuarantine(&registry,
                                                                       storage_request);
        break;
      case EngineFilespacePackageAction::admit: {
        auto staged = filespace::ImportFilespacePackageToQuarantine(&registry,
                                                                    storage_request);
        if (!staged.ok()) return FilespacePackageStorageFailure(request, staged);
        storage_result = filespace::AdmitFilespacePackage(&registry, storage_request);
        break;
      }
      case EngineFilespacePackageAction::reject: {
        auto staged = filespace::ImportFilespacePackageToQuarantine(&registry,
                                                                    storage_request);
        if (!staged.ok()) return FilespacePackageStorageFailure(request, staged);
        storage_result = filespace::RejectFilespacePackage(&registry, storage_request);
        break;
      }
    }
  }

  if (!storage_result.ok()) {
    return FilespacePackageStorageFailure(request, storage_result);
  }

  auto result = MakeApiBehaviorSuccess<EngineFilespacePackageResult>(
      request.context, operation);
  result.package_operation = request.package_operation;
  result.member_count = storage_result.manifest.members.size();
  result.staged_count = storage_result.staged_count;
  result.admitted_count = storage_result.admitted_count;
  result.rejected_count = storage_result.rejected_count;
  result.physical_package_member_count = physical_package_member_count;
  result.physical_package_byte_count = physical_package_byte_count;
  result.durable_state_changed = storage_result.durable_state_changed;
  result.cache_invalidation_required = storage_result.cache_invalidation_required;
  result.runtime_package_file_io_executed = runtime_package_file_io_executed;
  result.physical_package_transfer_executed = physical_package_transfer_executed;
  result.parser_file_io_authority = false;
  result.parser_storage_authority = false;
  result.transaction_finality_authority = false;
  result.recovery_authority = false;
  result.donor_or_wal_recovery_authority = false;
  result.private_provider_dispatch = false;
  result.result_shape.result_kind = "rs.filespace.package_report.v1";

  AddApiBehaviorRow(&result,
                    {{"operation_id", operation},
                     {"storage_executor", storage_executor},
                     {"filespace_package_operation", operation},
                     {"filespace_package_member_count", std::to_string(result.member_count)},
                     {"staged_count", std::to_string(result.staged_count)},
                     {"admitted_count", std::to_string(result.admitted_count)},
                     {"rejected_count", std::to_string(result.rejected_count)},
	                     {"durable_state_changed",
	                      result.durable_state_changed ? "true" : "false"},
	                     {"cache_invalidation_required",
	                      result.cache_invalidation_required ? "true" : "false"},
                     {"runtime_package_file_io_executed",
                      runtime_package_file_io_executed ? "true" : "false"},
                     {"physical_package_transfer_executed",
                      result.physical_package_transfer_executed ? "true" : "false"},
                     {"physical_package_member_count",
                      std::to_string(result.physical_package_member_count)},
                     {"physical_package_byte_count",
                      std::to_string(result.physical_package_byte_count)},
                     {"encrypted_material_included", "false"},
                     {"parser_file_io_authority", "false"},
                     {"parser_storage_authority", "false"},
                     {"transaction_finality_authority", "false"},
                     {"recovery_authority", "false"},
                     {"donor_wal_recovery_authority", "false"},
                     {"private_provider_dispatch", "false"},
                     {"mga_visibility_authority", "durable_transaction_inventory"}});
  for (const auto& event : storage_result.events) {
    AddApiBehaviorRow(
        &result,
        {{"filespace_package_event_action",
          filespace::FilespacePackageActionName(event.action)},
         {"filespace_package_event_previous_state",
          filespace::FilespaceStateName(event.previous_state)},
         {"filespace_package_event_new_state",
          filespace::FilespaceStateName(event.new_state)},
         {"filespace_package_event_diagnostic", event.diagnostic_code},
         {"filespace_package_event_durable_state_changed",
          event.durable_state_changed ? "true" : "false"}});
  }
  AddApiBehaviorEvidence(&result, "filespace_package_report", operation);
  AddApiBehaviorEvidence(&result, "filespace_package_operation", operation);
  AddApiBehaviorEvidence(&result, "filespace_package_member_count",
                         std::to_string(result.member_count));
  AddApiBehaviorEvidence(&result, "durable_state_changed",
                         result.durable_state_changed ? "true" : "false");
  AddApiBehaviorEvidence(&result, "cache_invalidation_required",
                         result.cache_invalidation_required ? "true" : "false");
  AddApiBehaviorEvidence(&result,
                         "runtime_package_file_io_executed",
                         runtime_package_file_io_executed ? "true" : "false");
  AddApiBehaviorEvidence(&result,
                         "physical_package_transfer_executed",
                         result.physical_package_transfer_executed ? "true" : "false");
  AddApiBehaviorEvidence(&result,
                         "physical_package_member_count",
                         std::to_string(result.physical_package_member_count));
  AddApiBehaviorEvidence(&result,
                         "physical_package_byte_count",
                         std::to_string(result.physical_package_byte_count));
  AddApiBehaviorEvidence(&result, "encrypted_material_included", "false");
  AddApiBehaviorEvidence(&result, "parser_file_io_authority", "false");
  AddApiBehaviorEvidence(&result, "parser_storage_authority", "false");
  AddApiBehaviorEvidence(&result, "transaction_finality_authority", "false");
  AddApiBehaviorEvidence(&result, "recovery_authority", "false");
  AddApiBehaviorEvidence(&result, "donor_wal_recovery_authority", "false");
  AddApiBehaviorEvidence(&result, "private_provider_dispatch", "false");
  AddApiBehaviorEvidence(&result,
                         "mga_visibility_authority",
                         "durable_transaction_inventory");
  result.result_shape.result_kind = "rs.filespace.package_report.v1";
  return result;
}

EngineStorageTierMigrationResult EnginePlanStorageTierMigrationOperation(
    const EngineStorageTierMigrationRequest& request) {
  const char* operation = EngineStorageTierMigrationOperationName(request.tier_operation);
  if (!request.context.security_context_present) {
    return StorageTierMigrationFailure(
        request,
        MakeInvalidRequestDiagnostic(operation, "security_context_required"));
  }

  const char* required_right =
      IsTierMutation(request.tier_operation) ? "FILESPACE_LIFECYCLE_CONTROL" : "OBS_CONFIG_INSPECT";
  if (!SecurityContextHasRight(request.context, required_right)) {
    return StorageTierMigrationFailure(
        request,
        MakeEngineApiDiagnostic("STORAGE_TIER.PERMISSION_DENIED",
                                "storage_tier.permission_denied",
                                required_right,
                                true));
  }
  if (IsTierMutation(request.tier_operation) && request.context.local_transaction_id == 0) {
    return StorageTierMigrationFailure(
        request,
        MakeInvalidRequestDiagnostic(operation, "local_transaction_id_required"));
  }
  if (request.context.database_uuid.canonical.empty()) {
    return StorageTierMigrationFailure(
        request,
        MakeInvalidRequestDiagnostic(operation, "database_uuid_required"));
  }
  if (request.target_object.uuid.canonical.empty() ||
      (request.target_object.object_kind != "filespace" &&
       request.target_object.object_kind != "shard" &&
       request.target_object.object_kind != "object")) {
    return StorageTierMigrationFailure(
        request,
        MakeInvalidRequestDiagnostic(operation, "target_filespace_shard_or_object_required"));
  }

  const auto& descriptor = request.descriptor;
  if (!descriptor.storage_tier_policy_resolved ||
      descriptor.storage_tier_policy_uuid.canonical.empty()) {
    return StorageTierMigrationFailure(
        request,
        MakeInvalidRequestDiagnostic(operation, "storage_tier_policy_required"));
  }
  if (descriptor.source_tier_uuid.canonical.empty() ||
      descriptor.target_tier_uuid.canonical.empty() ||
      !TierClassKnown(descriptor.source_tier_class) ||
      !TierClassKnown(descriptor.target_tier_class)) {
    return StorageTierMigrationFailure(
        request,
        MakeInvalidRequestDiagnostic(operation, "source_and_target_storage_tiers_required"));
  }
  if (descriptor.source_tier_uuid.canonical == descriptor.target_tier_uuid.canonical ||
      descriptor.source_tier_class == descriptor.target_tier_class) {
    return StorageTierMigrationFailure(
        request,
        MakeInvalidRequestDiagnostic(operation, "source_and_target_storage_tiers_must_differ"));
  }
  if (descriptor.expected_catalog_generation == 0 ||
      descriptor.observed_catalog_generation != descriptor.expected_catalog_generation) {
    return StorageTierMigrationFailure(
        request,
        MakeInvalidRequestDiagnostic(operation, "catalog_generation_mismatch"));
  }
  if (descriptor.expected_policy_generation == 0 ||
      descriptor.observed_policy_generation != descriptor.expected_policy_generation) {
    return StorageTierMigrationFailure(
        request,
        MakeInvalidRequestDiagnostic(operation, "policy_generation_mismatch"));
  }
  if (!descriptor.filespace_role_known ||
      descriptor.target_filespace_role == filespace::FilespaceRole::unknown ||
      descriptor.target_filespace_role == filespace::FilespaceRole::forbidden) {
    return StorageTierMigrationFailure(
        request,
        MakeInvalidRequestDiagnostic(operation, "target_filespace_role_required"));
  }
  if (descriptor.page_types.empty() || !descriptor.page_family_eligibility_validated) {
    return StorageTierMigrationFailure(
        request,
        MakeInvalidRequestDiagnostic(operation, "page_family_eligibility_required"));
  }
  for (const auto page_type : descriptor.page_types) {
    const auto role = page::ValidatePageTypeFilespaceRole(
        page_type, descriptor.target_filespace_role, operation);
    if (!role.ok()) {
      return StorageTierMigrationFailure(
          request,
          MakeEngineApiDiagnostic(role.diagnostic.diagnostic_code,
                                  role.diagnostic.message_key,
                                  role.descriptor.stable_name,
                                  true));
    }
  }
  if (RequiresTypedDependencyValidation(descriptor) &&
      !descriptor.typed_dependency_manifest_validated) {
    return StorageTierMigrationFailure(
        request,
        MakeEngineApiDiagnostic("STORAGE_TIER.TYPED_DEPENDENCY_MANIFEST_REQUIRED",
                                "storage_tier.typed_dependency_manifest_required",
                                operation,
                                true));
  }
  if (descriptor.cluster_scoped && !request.context.cluster_authority_available) {
    return StorageTierMigrationFailure(
        request,
        MakeEngineApiDiagnostic("STORAGE_TIER.CLUSTER_AUTHORITY_REQUIRED",
                                "storage_tier.cluster_authority_required",
                                operation,
                                true));
  }
  if (descriptor.physical_data_movement_requested) {
    if (!IsPhysicalTierRelocationOperation(request.tier_operation)) {
      return StorageTierMigrationFailure(
          request,
          MakeEngineApiDiagnostic("STORAGE_TIER.PHYSICAL_RELOCATION_OPERATION_UNSUPPORTED",
                                  "storage_tier.physical_relocation_operation_unsupported",
                                  operation,
                                  true));
    }
    if (!descriptor.physical_data_movement_authorized) {
      return StorageTierMigrationFailure(
          request,
          MakeEngineApiDiagnostic("STORAGE_TIER.PHYSICAL_MOVEMENT_AUTHORITY_REQUIRED",
                                  "storage_tier.physical_movement_authority_required",
                                  operation,
                                  true));
    }
    if (!descriptor.physical_rewrite_plan_validated) {
      return StorageTierMigrationFailure(
          request,
          MakeEngineApiDiagnostic("STORAGE_TIER.PHYSICAL_REWRITE_PLAN_REQUIRED",
                                  "storage_tier.physical_rewrite_plan_required",
                                  operation,
                                  true));
    }
    if (!descriptor.backup_export_repair_profile_validated) {
      return StorageTierMigrationFailure(
          request,
          MakeEngineApiDiagnostic("STORAGE_TIER.BACKUP_EXPORT_REPAIR_PROFILE_REQUIRED",
                                  "storage_tier.backup_export_repair_profile_required",
                                  operation,
                                  true));
    }
    if (descriptor.source_physical_path.empty() ||
        descriptor.target_physical_path.empty()) {
      return StorageTierMigrationFailure(
          request,
          MakeInvalidRequestDiagnostic(
              operation, "physical_source_and_target_paths_required"));
    }
    if (descriptor.source_physical_path == descriptor.target_physical_path) {
      return StorageTierMigrationFailure(
          request,
          MakeInvalidRequestDiagnostic(
              operation, "physical_source_and_target_paths_must_differ"));
    }

    platform::u64 byte_count = 0;
    std::string detail;
    const bool physical_ok =
        request.tier_operation == EngineStorageTierMigrationOperation::stage_migration
            ? CopyStorageTierPhysicalRelocationFile(descriptor,
                                                    &byte_count,
                                                    &detail)
            : VerifyStorageTierPhysicalRelocationFile(descriptor,
                                                      &byte_count,
                                                      &detail);
    if (!physical_ok) {
      return StorageTierMigrationFailure(
          request,
          MakeEngineApiDiagnostic("STORAGE_TIER.PHYSICAL_RELOCATION_FAILED",
                                  "storage_tier.physical_relocation_failed",
                                  detail.empty() ? operation : detail,
                                  true));
    }

    EngineStorageTierMigrationResult result;
    result.ok = true;
    result.operation_id = operation;
    result.descriptor = descriptor;
    result.primary_object = request.target_object;
    result.local_transaction_id = request.context.local_transaction_id;
    result.transaction_uuid = request.context.transaction_uuid;
    result.result_shape.result_kind = "rs.storage_tier.physical_relocation.v1";
    result.durable_state_changed =
        request.tier_operation == EngineStorageTierMigrationOperation::commit_migration;
    result.physical_data_movement_dispatched = true;
    result.physical_digest_verified = true;
    result.physical_data_movement_bytes = byte_count;
    result.cache_invalidation_required = result.durable_state_changed;
    AddApiBehaviorRow(
        &result,
        {{"storage_tier_operation", operation},
         {"target_object_kind", request.target_object.object_kind},
         {"target_uuid", request.target_object.uuid.canonical},
         {"source_tier", EngineStorageTierClassName(descriptor.source_tier_class)},
         {"target_tier", EngineStorageTierClassName(descriptor.target_tier_class)},
         {"storage_tier_policy_uuid", descriptor.storage_tier_policy_uuid.canonical},
         {"page_family_eligibility_validated", "true"},
         {"typed_dependency_manifest_validated",
          descriptor.typed_dependency_manifest_validated ? "true" : "false"},
         {"physical_rewrite_plan_validated", "true"},
         {"backup_export_repair_profile_validated", "true"},
         {"durable_state_changed", result.durable_state_changed ? "true" : "false"},
         {"physical_data_movement_dispatched", "true"},
         {"physical_digest_verified", "true"},
         {"parser_storage_authority", "false"},
         {"private_provider_dispatch", "false"},
         {"mga_visibility_authority", "durable_transaction_inventory"}});
    result.result_shape.result_kind = "rs.storage_tier.physical_relocation.v1";
    AddApiBehaviorEvidence(&result, "storage_tier_physical_relocation", operation);
    AddApiBehaviorEvidence(&result, "physical_data_movement_dispatched", "true");
    AddApiBehaviorEvidence(&result, "physical_digest_verified", "true");
    AddApiBehaviorEvidence(&result,
                           "physical_data_movement_bytes",
                           std::to_string(byte_count));
    AddApiBehaviorEvidence(&result,
                           "durable_state_changed",
                           result.durable_state_changed ? "true" : "false");
    AddApiBehaviorEvidence(&result, "parser_storage_authority", "false");
    AddApiBehaviorEvidence(&result, "private_provider_dispatch", "false");
    AddApiBehaviorEvidence(&result,
                           "mga_visibility_authority",
                           "durable_transaction_inventory");
    return result;
  }

  EngineStorageTierMigrationResult result;
  result.ok = true;
  result.operation_id = operation;
  result.descriptor = descriptor;
  result.primary_object = request.target_object;
  result.local_transaction_id = request.context.local_transaction_id;
  result.transaction_uuid = request.context.transaction_uuid;
  result.result_shape.result_kind = "rs.storage_tier.descriptor_plan.v1";
  result.cache_invalidation_required =
      request.tier_operation == EngineStorageTierMigrationOperation::stage_migration ||
      request.tier_operation == EngineStorageTierMigrationOperation::commit_migration;
  AddApiBehaviorRow(&result,
                    {{"storage_tier_operation", operation},
                     {"target_object_kind", request.target_object.object_kind},
                     {"target_uuid", request.target_object.uuid.canonical},
                     {"source_tier", EngineStorageTierClassName(descriptor.source_tier_class)},
                     {"target_tier", EngineStorageTierClassName(descriptor.target_tier_class)},
                     {"storage_tier_policy_uuid", descriptor.storage_tier_policy_uuid.canonical},
                     {"page_family_eligibility_validated", "true"},
                     {"typed_dependency_manifest_validated",
                      descriptor.typed_dependency_manifest_validated ? "true" : "false"},
                     {"durable_state_changed", "false"},
                     {"physical_data_movement_dispatched", "false"},
                     {"parser_storage_authority", "false"},
                     {"private_provider_dispatch", "false"},
                     {"mga_visibility_authority", "durable_transaction_inventory"}});
  result.result_shape.result_kind = "rs.storage_tier.descriptor_plan.v1";
  AddApiBehaviorEvidence(&result, "storage_tier_descriptor_plan", operation);
  AddApiBehaviorEvidence(&result, "storage_tier_policy_uuid",
                         descriptor.storage_tier_policy_uuid.canonical);
  AddApiBehaviorEvidence(&result, "parser_storage_authority", "false");
  AddApiBehaviorEvidence(&result, "durable_state_changed", "false");
  AddApiBehaviorEvidence(&result, "physical_data_movement_dispatched", "false");
  AddApiBehaviorEvidence(&result, "private_provider_dispatch", "false");
  AddApiBehaviorEvidence(&result, "mga_visibility_authority", "durable_transaction_inventory");
  return result;
}

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

EngineFilespaceLifecycleResult EngineFilespaceLifecycleOperation(
    const EngineFilespaceLifecycleRequest& request) {
  const std::string operation_id =
      request.operation_id.empty() ? OptionValue(request, "filespace.lifecycle.operation:")
                                   : request.operation_id;
  const std::string effective_operation =
      operation_id.empty() ? std::string("filespace.lifecycle") : operation_id;

  if (!request.context.security_context_present) {
    return FilespaceLifecycleFailure(
        request,
        effective_operation,
        MakeInvalidRequestDiagnostic(effective_operation, "security_context_required"));
  }
  if (!SecurityContextHasRight(request.context, "FILESPACE_LIFECYCLE_CONTROL")) {
    return FilespaceLifecycleFailure(
        request,
        effective_operation,
        MakeEngineApiDiagnostic("FILESPACE.PERMISSION_DENIED",
                                "filespace.lifecycle.permission_denied",
                                "FILESPACE_LIFECYCLE_CONTROL",
                                true));
  }
  const auto open_state_diagnostic =
      FilespaceOpenStateMutationDiagnostic(request, effective_operation.c_str());
  if (!open_state_diagnostic.code.empty()) {
    return FilespaceLifecycleFailure(request, effective_operation, open_state_diagnostic);
  }
  const auto policy = ManagementPolicyValue(request);
  if (policy == "deny" || policy == "deny_all" || policy == "read_only" ||
      policy == "inspect_only") {
    return FilespaceLifecycleFailure(
        request,
        effective_operation,
        MakeEngineApiDiagnostic("FILESPACE.POLICY_DENIED",
                                "filespace.lifecycle.policy_denied",
                                policy,
                                true));
  }
  if (request.context.local_transaction_id == 0) {
    return FilespaceLifecycleFailure(
        request,
        effective_operation,
        MakeInvalidRequestDiagnostic(effective_operation, "local_transaction_id_required"));
  }

  const EngineObjectReference target = TargetFilespace(request);
  if (target.uuid.canonical.empty()) {
    return FilespaceLifecycleFailure(
        request,
        effective_operation,
        MakeInvalidRequestDiagnostic(effective_operation, "target_filespace_uuid_required"));
  }

  const auto database_uuid = ParseEngineIdentity(platform::UuidKind::database,
                                                request.context.database_uuid.canonical);
  const auto filespace_uuid = ParseEngineIdentity(platform::UuidKind::filespace,
                                                 target.uuid.canonical);
  if (!database_uuid.valid()) {
    return FilespaceLifecycleFailure(
        request,
        effective_operation,
        MakeInvalidRequestDiagnostic(effective_operation, "database_uuid_invalid_for_storage_route"));
  }
  if (!filespace_uuid.valid()) {
    return FilespaceLifecycleFailure(
        request,
        effective_operation,
        MakeInvalidRequestDiagnostic(effective_operation, "target_filespace_uuid_invalid_for_storage_route"));
  }

  const auto lifecycle_operation = LifecycleOperationFor(effective_operation);
  const std::string key = LifecycleRegistryKey(request.context);
  std::lock_guard<std::mutex> lock(FilespaceLifecycleRouteMutex());
  auto& runtime = FilespaceLifecycleRouteRegistries()[key];
  if (!runtime.writer_identity_uuid.valid()) {
    runtime.writer_identity_uuid = GeneratedIdentity(platform::UuidKind::object, key, 71);
  }

  filespace::FilespaceOperationRequest storage_request;
  storage_request.operation = lifecycle_operation;
  storage_request.database_uuid = database_uuid;
  storage_request.filespace_uuid = filespace_uuid;
  storage_request.merge_target_filespace_uuid =
      ParseEngineIdentity(platform::UuidKind::filespace,
                          OptionValue(request, "filespace.merge_target_uuid:"));
  storage_request.path = DefaultLifecyclePath(request, target);
  storage_request.role = LifecycleRoleValue(request, lifecycle_operation);
  storage_request.page_size = PageSizeBytes(request);
  storage_request.physical_filespace_id =
      static_cast<platform::u16>(OptionU64(request, "filespace.physical_id:", 1));
  storage_request.total_pages = OptionU64(request, "filespace.total_pages:", 64);
  storage_request.free_pages = OptionU64(request, "filespace.free_pages:", 32);
  storage_request.preallocated_pages = OptionU64(request, "filespace.preallocated_pages:", 8);
  storage_request.allocation_root_page = OptionU64(request, "filespace.allocation_root_page:", 4);
  storage_request.header_generation = OptionU64(request, "filespace.header_generation:", 1);
  storage_request.writer_identity_uuid = runtime.writer_identity_uuid;
  storage_request.reason = effective_operation;
  storage_request.policy.allow_primary_detach =
      OptionBool(request, "filespace.allow_primary_detach:", false);
  storage_request.policy.allow_primary_replacement =
      OptionBool(request, "filespace.allow_primary_replacement:", true);
  storage_request.policy.allow_promotion =
      OptionBool(request, "filespace.allow_promotion:", true);
  storage_request.policy.require_no_active_pins_for_detach =
      OptionBool(request, "filespace.require_no_active_pins_for_detach:", true);
  storage_request.policy.require_no_active_pins_for_promote =
      OptionBool(request, "filespace.require_no_active_pins_for_promote:", true);
  storage_request.policy.require_no_active_pins_for_quarantine =
      OptionBool(request, "filespace.require_no_active_pins_for_quarantine:", true);
  storage_request.policy.require_no_active_pins_for_move =
      OptionBool(request, "filespace.require_no_active_pins_for_move:", true);
  storage_request.policy.require_no_active_pins_for_merge =
      OptionBool(request, "filespace.require_no_active_pins_for_merge:", true);
  storage_request.policy.require_no_active_pins_for_delete_physical =
      OptionBool(request, "filespace.require_no_active_pins_for_delete_physical:", true);
  storage_request.policy.require_no_active_pins_for_repair =
      OptionBool(request, "filespace.require_no_active_pins_for_repair:", true);
  storage_request.policy.require_no_active_pins_for_rebuild =
      OptionBool(request, "filespace.require_no_active_pins_for_rebuild:", true);
  storage_request.policy.require_no_active_pins_for_salvage =
      OptionBool(request, "filespace.require_no_active_pins_for_salvage:", true);
  storage_request.policy.allow_filespace_move =
      OptionBool(request, "filespace.allow_filespace_move:", false);
  storage_request.policy.page_agent_relocation_complete_for_move =
      OptionBool(request, "filespace.page_agent_relocation_complete_for_move:", false);
  storage_request.policy.startup_open_safe_for_move =
      OptionBool(request, "filespace.startup_open_safe_for_move:", false);
  storage_request.policy.allow_filespace_merge =
      OptionBool(request, "filespace.allow_filespace_merge:", false);
  storage_request.policy.page_agent_merge_complete_for_merge =
      OptionBool(request, "filespace.page_agent_merge_complete_for_merge:", false);
  storage_request.policy.startup_open_safe_for_merge =
      OptionBool(request, "filespace.startup_open_safe_for_merge:", false);
  storage_request.policy.allow_filespace_repair =
      OptionBool(request, "filespace.allow_filespace_repair:", false);
  storage_request.policy.repair_plan_authorized =
      OptionBool(request, "filespace.repair_plan_authorized:", false);
  storage_request.policy.repair_evidence_preserved =
      OptionBool(request, "filespace.repair_evidence_preserved:", false);
  storage_request.policy.allow_filespace_rebuild =
      OptionBool(request, "filespace.allow_filespace_rebuild:", false);
  storage_request.policy.rebuild_source_verified =
      OptionBool(request, "filespace.rebuild_source_verified:", false);
  storage_request.policy.page_agent_rebuild_complete =
      OptionBool(request, "filespace.page_agent_rebuild_complete:", false);
  storage_request.policy.startup_open_safe_for_rebuild =
      OptionBool(request, "filespace.startup_open_safe_for_rebuild:", false);
  storage_request.policy.allow_filespace_salvage =
      OptionBool(request, "filespace.allow_filespace_salvage:", false);
  storage_request.policy.salvage_review_authorized =
      OptionBool(request, "filespace.salvage_review_authorized:", false);
  storage_request.policy.salvage_output_quarantined =
      OptionBool(request, "filespace.salvage_output_quarantined:", false);
  storage_request.policy.allow_physical_filespace_delete =
      OptionBool(request, "filespace.allow_physical_filespace_delete:", false);
  storage_request.policy.physical_delete_retention_satisfied =
      OptionBool(request, "filespace.physical_delete_retention_satisfied:", false);
  storage_request.policy.physical_delete_legal_hold_clear =
      OptionBool(request, "filespace.physical_delete_legal_hold_clear:", false);
  storage_request.policy.physical_delete_cleanup_horizon_authoritative =
      OptionBool(request,
                 "filespace.physical_delete_cleanup_horizon_authoritative:",
                 false);
  storage_request.policy.require_physical_header_for_attach =
      OptionBool(request, "filespace.require_physical_header_for_attach:", false);
  storage_request.policy.require_physical_header_for_promote =
      OptionBool(request, "filespace.require_physical_header_for_promote:", false);
  storage_request.policy.allow_archive_owner_assignment =
      OptionBool(request,
                 "filespace.allow_archive_owner_assignment:",
                 lifecycle_operation == filespace::FilespaceOperation::assign_archive_owner);

  const auto storage_result =
      filespace::ApplyFilespaceOperation(&runtime.registry, storage_request);
  if (!storage_result.ok()) {
    return FilespaceLifecycleFailure(
        request,
        effective_operation,
        MakeEngineApiDiagnostic(storage_result.diagnostic.diagnostic_code.empty()
                                    ? "FILESPACE.LIFECYCLE_FAILED"
                                    : storage_result.diagnostic.diagnostic_code,
                                storage_result.diagnostic.message_key.empty()
                                    ? "filespace.lifecycle.failed"
                                    : storage_result.diagnostic.message_key,
                                filespace::FilespaceOperationName(lifecycle_operation),
                                true));
  }

  auto result = MakeApiBehaviorSuccess<EngineFilespaceLifecycleResult>(
      request.context, effective_operation);
  result.primary_object.uuid.canonical = target.uuid.canonical;
  result.primary_object.object_kind = "filespace";
  AddApiBehaviorRow(&result,
                    {{"operation_id", effective_operation},
                     {"storage_executor", "ApplyFilespaceOperation"},
                     {"filespace_uuid", target.uuid.canonical},
                     {"merge_target_filespace_uuid",
                      UuidText(storage_request.merge_target_filespace_uuid)},
                     {"filespace_role", filespace::FilespaceRoleName(storage_result.descriptor.role)},
                     {"filespace_state", filespace::FilespaceStateName(storage_result.descriptor.state)},
                     {"durable_state_changed", storage_result.durable_state_changed ? "true" : "false"},
                     {"physical_file_removed", storage_result.physical_file_removed ? "true" : "false"},
                     {"cache_invalidation_required",
                      storage_result.cache_invalidation_required ? "true" : "false"},
                     {"parser_storage_authority", "false"},
                     {"mga_visibility_authority", "durable_transaction_inventory"}});
  AddApiBehaviorEvidence(&result, "filespace_lifecycle_operation", effective_operation);
  AddApiBehaviorEvidence(&result,
                         "filespace_lifecycle_storage_operation",
                         filespace::FilespaceOperationName(lifecycle_operation));
  AddApiBehaviorEvidence(&result, "storage_executor", "ApplyFilespaceOperation");
  AddApiBehaviorEvidence(&result, "filespace", target.uuid.canonical);
  if (storage_request.merge_target_filespace_uuid.valid()) {
    AddApiBehaviorEvidence(&result,
                           "merge_target_filespace",
                           UuidText(storage_request.merge_target_filespace_uuid));
  }
  AddApiBehaviorEvidence(&result,
                         "physical_file_removed",
                         storage_result.physical_file_removed ? "true" : "false");
  AddApiBehaviorEvidence(&result, "parser_storage_authority", "false");
  AddApiBehaviorEvidence(&result, "mga_visibility_authority", "durable_transaction_inventory");
  const std::string surface_id = OptionValue(request, "sbsfc077_surface_id:");
  if (!surface_id.empty()) {
    AddApiBehaviorEvidence(&result, "sbsfc077_surface", surface_id);
  }
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
  const std::string surface_id = OptionValue(request, "sbsfc077_surface_id:");
  if (!surface_id.empty()) {
    AddApiBehaviorEvidence(&result, "sbsfc077_surface", surface_id);
  }
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
