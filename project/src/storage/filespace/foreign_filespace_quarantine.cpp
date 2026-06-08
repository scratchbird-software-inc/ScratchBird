// Copyright (c) 2026 ScratchBird Software Inc.
//
// This Source Code Form is subject to the terms of the Mozilla Public
// License, v. 2.0. If a copy of the MPL was not distributed with this
// file, You can obtain one at https://mozilla.org/MPL/2.0/.
//
// SPDX-License-Identifier: MPL-2.0

#include "foreign_filespace_quarantine.hpp"

#include "filespace_header.hpp"
#include "metric_producer.hpp"
#include "uuid.hpp"

#include <utility>

namespace scratchbird::storage::filespace {
namespace {

using scratchbird::core::platform::Severity;
using scratchbird::core::platform::Status;
using scratchbird::core::platform::StatusCode;
using scratchbird::core::platform::Subsystem;
using scratchbird::core::platform::UuidKind;
using scratchbird::core::uuid::IsEngineIdentityUuid;
using scratchbird::core::uuid::UuidToString;

Status QuarantineOkStatus() {
  return {StatusCode::ok, Severity::info, Subsystem::storage_disk};
}

Status QuarantineErrorStatus() {
  return {StatusCode::platform_required_feature_missing, Severity::error, Subsystem::storage_disk};
}

bool IsTypedEngineIdentity(const TypedUuid& uuid, UuidKind kind) {
  return uuid.kind == kind && uuid.valid() && IsEngineIdentityUuid(uuid.value);
}

FilespaceDescriptor* FindMutable(FilespaceRegistry* registry, const TypedUuid& filespace_uuid) {
  if (registry == nullptr) {
    return nullptr;
  }
  for (FilespaceDescriptor& descriptor : registry->filespaces) {
    if (descriptor.filespace_uuid.value == filespace_uuid.value) {
      return &descriptor;
    }
  }
  return nullptr;
}

const FilespaceDescriptor* Find(const FilespaceRegistry& registry, const TypedUuid& filespace_uuid) {
  for (const FilespaceDescriptor& descriptor : registry.filespaces) {
    if (descriptor.filespace_uuid.value == filespace_uuid.value) {
      return &descriptor;
    }
  }
  return nullptr;
}

void EmitMetric(const char* operation,
                const char* result,
                const char* reason,
                const ForeignFilespaceQuarantineRequest& request) {
  (void)scratchbird::core::metrics::IncrementCounter(
      "sb_foreign_filespace_quarantine_total",
      scratchbird::core::metrics::Labels({
          {"component", "storage.filespace.foreign"},
          {"operation", operation},
          {"result", result},
          {"reason", reason},
          {"database_uuid", UuidToString(request.database_uuid.value)},
          {"filespace_uuid", UuidToString(request.filespace_uuid.value)},
      }),
      1.0,
      "storage_filespace");
}

ForeignFilespaceQuarantineResult Error(std::string code,
                                       std::string key,
                                       const ForeignFilespaceQuarantineRequest& request,
                                       std::string detail = {}) {
  if (request.database_uuid.valid() && request.filespace_uuid.valid()) {
    EmitMetric("foreign_quarantine", "error", code.c_str(), request);
  }
  ForeignFilespaceQuarantineResult result;
  result.status = QuarantineErrorStatus();
  result.diagnostic = MakeFilespaceDiagnostic(result.status,
                                              std::move(code),
                                              std::move(key),
                                              std::move(detail));
  return result;
}

ForeignFilespaceQuarantineResult Ok(FilespaceDescriptor descriptor,
                                    FilespaceEvidenceRecord evidence,
                                    bool inspection_passed,
                                    bool release_allowed,
                                    bool quarantine_fence_active,
                                    bool durable_state_changed) {
  ForeignFilespaceQuarantineResult result;
  result.status = QuarantineOkStatus();
  result.descriptor = std::move(descriptor);
  result.evidence = std::move(evidence);
  result.inspection_passed = inspection_passed;
  result.release_allowed = release_allowed;
  result.quarantine_fence_active = quarantine_fence_active;
  result.durable_state_changed = durable_state_changed;
  result.cache_invalidation_required = durable_state_changed;
  return result;
}

FilespaceEvidenceRecord Evidence(FilespaceRegistry* registry,
                                 FilespaceOperation operation,
                                 const ForeignFilespaceQuarantineRequest& request,
                                 const FilespaceDescriptor& before,
                                 const FilespaceDescriptor& after,
                                 const char* diagnostic_code) {
  FilespaceEvidenceRecord evidence;
  evidence.sequence = registry == nullptr ? 0 : registry->next_evidence_sequence++;
  evidence.operation = operation;
  evidence.filespace_uuid = request.filespace_uuid;
  evidence.previous_state = before.state;
  evidence.new_state = after.state;
  evidence.previous_role = before.role;
  evidence.new_role = after.role;
  evidence.reason = request.operation_uuid;
  evidence.diagnostic_code = diagnostic_code;
  evidence.durable_state_changed = true;
  if (registry != nullptr) {
    registry->evidence.push_back(evidence);
  }
  return evidence;
}

PhysicalFilespaceHeaderResult ReadAndValidatePhysicalHeader(
    const ForeignFilespaceQuarantineRequest& request) {
  auto header = ReadPhysicalFilespaceHeader(request.path);
  if (!header.ok()) {
    return header;
  }
  PhysicalFilespaceHeader expected;
  expected.database_uuid = request.database_uuid;
  expected.filespace_uuid = request.filespace_uuid;
  expected.role = header.header.role;
  expected.state = header.header.state;
  expected.page_size = request.page_size;
  expected.format_version = header.header.format_version;
  expected.checksum_profile = header.header.checksum_profile;
  expected.encryption_profile = header.header.encryption_profile;
  expected.compatibility_flags = header.header.compatibility_flags;
  expected.attach_policy_flags = header.header.attach_policy_flags;
  expected.physical_filespace_id = header.header.physical_filespace_id;
  expected.total_pages = header.header.total_pages;
  expected.free_pages = header.header.free_pages;
  expected.preallocated_pages = header.header.preallocated_pages;
  expected.allocation_root_page = header.header.allocation_root_page;
  expected.header_generation = header.header.header_generation;
  expected.writer_identity_uuid = header.header.writer_identity_uuid;
  return ValidatePhysicalFilespaceHeader(expected, header.header);
}

}  // namespace

ForeignFilespaceQuarantineResult ImportForeignFilespaceIntoQuarantine(
    FilespaceRegistry* registry,
    const ForeignFilespaceQuarantineRequest& request) {
  if (registry == nullptr) {
    return Error("SB-FOREIGN-FILESPACE-REGISTRY-NULL",
                 "storage.filespace.foreign.registry_null",
                 request);
  }
  if (!IsTypedEngineIdentity(request.database_uuid, UuidKind::database)) {
    return Error("SB-FOREIGN-FILESPACE-DATABASE-UUID-MUST-BE-V7",
                 "storage.filespace.foreign.database_uuid_must_be_v7",
                 request);
  }
  if (!IsTypedEngineIdentity(request.filespace_uuid, UuidKind::filespace)) {
    return Error("SB-FOREIGN-FILESPACE-FILESPACE-UUID-MUST-BE-V7",
                 "storage.filespace.foreign.filespace_uuid_must_be_v7",
                 request);
  }
  if (request.path.empty()) {
    return Error("SB-FOREIGN-FILESPACE-PATH-REQUIRED",
                 "storage.filespace.foreign.path_required",
                 request);
  }
  if (Find(*registry, request.filespace_uuid) != nullptr) {
    return Error("SB-FOREIGN-FILESPACE-DUPLICATE-IDENTITY",
                 "storage.filespace.foreign.duplicate_identity",
                 request);
  }
  PhysicalFilespaceHeaderResult physical_header;
  const bool physical_header_available = request.physical_header_required;
  if (physical_header_available) {
    physical_header = ReadAndValidatePhysicalHeader(request);
    if (!physical_header.ok()) {
      return Error("SB-FOREIGN-FILESPACE-PHYSICAL-HEADER-INVALID",
                   "storage.filespace.foreign.physical_header_invalid",
                   request,
                   physical_header.diagnostic.diagnostic_code);
    }
  }

  FilespaceDescriptor before;
  FilespaceDescriptor after;
  after.database_uuid = request.database_uuid;
  after.filespace_uuid = request.filespace_uuid;
  after.path = request.path;
  after.role = FilespaceRole::import_candidate;
  after.state = FilespaceState::quarantine;
  after.page_size = request.page_size;
  after.read_only = true;
  after.active = false;
  after.generation = 1;
  if (physical_header_available) {
    after.physical_filespace_id = physical_header.header.physical_filespace_id;
    after.total_pages = physical_header.header.total_pages;
    after.free_pages = physical_header.header.free_pages;
    after.preallocated_pages = physical_header.header.preallocated_pages;
    after.allocation_root_page = physical_header.header.allocation_root_page;
    after.header_generation = physical_header.header.header_generation;
    after.writer_identity_uuid = physical_header.header.writer_identity_uuid;
  }
  registry->filespaces.push_back(after);
  const auto evidence = Evidence(registry,
                                 FilespaceOperation::attach_filespace,
                                 request,
                                 before,
                                 after,
                                 "SB-FOREIGN-FILESPACE-QUARANTINED");
  EmitMetric("import_quarantine", "ok", "quarantined", request);
  return Ok(after, evidence, false, false, true, true);
}

ForeignFilespaceQuarantineResult InspectForeignFilespaceQuarantine(
    const FilespaceRegistry& registry,
    const ForeignFilespaceQuarantineRequest& request) {
  const FilespaceDescriptor* descriptor = Find(registry, request.filespace_uuid);
  if (descriptor == nullptr) {
    return Error("SB-FOREIGN-FILESPACE-NOT-FOUND",
                 "storage.filespace.foreign.not_found",
                 request);
  }
  if (descriptor->state != FilespaceState::quarantine ||
      descriptor->role != FilespaceRole::import_candidate) {
    return Error("SB-FOREIGN-FILESPACE-NOT-QUARANTINED",
                 "storage.filespace.foreign.not_quarantined",
                 request);
  }
  if (request.inspector_uuid.empty()) {
    return Error("SB-FOREIGN-FILESPACE-INSPECTOR-REQUIRED",
                 "storage.filespace.foreign.inspector_required",
                 request);
  }
  const auto header = ReadAndValidatePhysicalHeader(request);
  if (!header.ok()) {
    return Error("SB-FOREIGN-FILESPACE-INSPECTION-FAILED",
                 "storage.filespace.foreign.inspection_failed",
                 request,
                 header.diagnostic.diagnostic_code);
  }
  EmitMetric("inspect_quarantine", "ok", "inspection_passed", request);
  FilespaceEvidenceRecord evidence;
  evidence.filespace_uuid = request.filespace_uuid;
  evidence.operation = FilespaceOperation::verify_filespace;
  evidence.previous_state = descriptor->state;
  evidence.new_state = descriptor->state;
  evidence.previous_role = descriptor->role;
  evidence.new_role = descriptor->role;
  evidence.reason = request.operation_uuid;
  evidence.diagnostic_code = "SB-FOREIGN-FILESPACE-INSPECTION-PASSED";
  return Ok(*descriptor, evidence, true, true, true, false);
}

ForeignFilespaceQuarantineResult ReleaseForeignFilespaceQuarantine(
    FilespaceRegistry* registry,
    const ForeignFilespaceQuarantineRequest& request) {
  FilespaceDescriptor* descriptor = FindMutable(registry, request.filespace_uuid);
  if (descriptor == nullptr) {
    return Error("SB-FOREIGN-FILESPACE-NOT-FOUND",
                 "storage.filespace.foreign.not_found",
                 request);
  }
  if (descriptor->state != FilespaceState::quarantine ||
      descriptor->role != FilespaceRole::import_candidate) {
    return Error("SB-FOREIGN-FILESPACE-NOT-QUARANTINED",
                 "storage.filespace.foreign.not_quarantined",
                 request);
  }
  if (!request.header_inspection_passed || !request.release_authorized ||
      request.release_authority_uuid.empty()) {
    return Error("SB-FOREIGN-FILESPACE-RELEASE-AUTHORITY-REQUIRED",
                 "storage.filespace.foreign.release_authority_required",
                 request);
  }
  const auto header = ReadAndValidatePhysicalHeader(request);
  if (!header.ok()) {
    return Error("SB-FOREIGN-FILESPACE-RELEASE-HEADER-INVALID",
                 "storage.filespace.foreign.release_header_invalid",
                 request,
                 header.diagnostic.diagnostic_code);
  }

  const FilespaceDescriptor before = *descriptor;
  descriptor->state = FilespaceState::detached;
  descriptor->read_only = true;
  descriptor->active = false;
  ++descriptor->generation;
  const auto evidence = Evidence(registry,
                                 FilespaceOperation::verify_filespace,
                                 request,
                                 before,
                                 *descriptor,
                                 "SB-FOREIGN-FILESPACE-RELEASED");
  EmitMetric("release_quarantine", "ok", "released", request);
  return Ok(*descriptor, evidence, true, true, false, true);
}

}  // namespace scratchbird::storage::filespace
