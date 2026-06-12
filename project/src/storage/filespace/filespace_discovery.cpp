// Copyright (c) 2026 ScratchBird Software Inc.
//
// This Source Code Form is subject to the terms of the Mozilla Public
// License, v. 2.0. If a copy of the MPL was not distributed with this
// file, You can obtain one at https://mozilla.org/MPL/2.0/.
//
// SPDX-License-Identifier: MPL-2.0

#include "filespace_discovery.hpp"

#include "filespace_header.hpp"
#include "foreign_filespace_quarantine.hpp"

#include <algorithm>
#include <filesystem>
#include <string>
#include <utility>
#include <vector>

namespace scratchbird::storage::filespace {
namespace {

using scratchbird::core::platform::Severity;
using scratchbird::core::platform::Status;
using scratchbird::core::platform::StatusCode;
using scratchbird::core::platform::Subsystem;

Status DiscoveryOkStatus() {
  return {StatusCode::ok, Severity::info, Subsystem::storage_disk};
}

Status DiscoveryErrorStatus() {
  return {StatusCode::platform_required_feature_missing, Severity::error, Subsystem::storage_disk};
}

bool SameUuid(const TypedUuid& left, const TypedUuid& right) {
  return left.valid() && right.valid() && left.value == right.value;
}

bool SamePath(const std::string& left, const std::string& right) {
  return !left.empty() && !right.empty() && left == right;
}

bool SameWriterIdentity(const TypedUuid& left, const TypedUuid& right) {
  if (!left.valid() || !right.valid()) {
    return true;
  }
  return left.value == right.value;
}

const char* DiagnosticCode(FilespaceDiscoveryClassification classification) {
  switch (classification) {
    case FilespaceDiscoveryClassification::ok:
      return "SB-FILESPACE-DISCOVERY-OK";
    case FilespaceDiscoveryClassification::missing:
      return "SB-FILESPACE-DISCOVERY-MISSING";
    case FilespaceDiscoveryClassification::wrong_database:
      return "SB-FILESPACE-DISCOVERY-WRONG-DATABASE";
    case FilespaceDiscoveryClassification::wrong_filespace:
      return "SB-FILESPACE-DISCOVERY-WRONG-FILESPACE";
    case FilespaceDiscoveryClassification::duplicate_identity:
      return "SB-FILESPACE-DISCOVERY-DUPLICATE-IDENTITY";
    case FilespaceDiscoveryClassification::stale_header:
      return "SB-FILESPACE-DISCOVERY-STALE-HEADER";
    case FilespaceDiscoveryClassification::replaced_header:
      return "SB-FILESPACE-DISCOVERY-REPLACED-HEADER";
    case FilespaceDiscoveryClassification::foreign_orphan:
      return "SB-FILESPACE-DISCOVERY-FOREIGN-ORPHAN";
    case FilespaceDiscoveryClassification::quarantined_candidate:
      return "SB-FILESPACE-DISCOVERY-QUARANTINED";
  }
  return "SB-FILESPACE-DISCOVERY-UNKNOWN";
}

const char* MessageKey(FilespaceDiscoveryClassification classification) {
  switch (classification) {
    case FilespaceDiscoveryClassification::ok:
      return "storage.filespace.discovery.ok";
    case FilespaceDiscoveryClassification::missing:
      return "storage.filespace.discovery.missing";
    case FilespaceDiscoveryClassification::wrong_database:
      return "storage.filespace.discovery.wrong_database";
    case FilespaceDiscoveryClassification::wrong_filespace:
      return "storage.filespace.discovery.wrong_filespace";
    case FilespaceDiscoveryClassification::duplicate_identity:
      return "storage.filespace.discovery.duplicate_identity";
    case FilespaceDiscoveryClassification::stale_header:
      return "storage.filespace.discovery.stale_header";
    case FilespaceDiscoveryClassification::replaced_header:
      return "storage.filespace.discovery.replaced_header";
    case FilespaceDiscoveryClassification::foreign_orphan:
      return "storage.filespace.discovery.foreign_orphan";
    case FilespaceDiscoveryClassification::quarantined_candidate:
      return "storage.filespace.discovery.quarantined";
  }
  return "storage.filespace.discovery.unknown";
}

const char* RecommendedAction(FilespaceDiscoveryClassification classification) {
  switch (classification) {
    case FilespaceDiscoveryClassification::ok:
      return "allow_normal_access";
    case FilespaceDiscoveryClassification::missing:
      return "restore_or_reattach_after_verify";
    case FilespaceDiscoveryClassification::wrong_database:
      return "quarantine_and_operator_review";
    case FilespaceDiscoveryClassification::wrong_filespace:
      return "quarantine_and_verify_expected_identity";
    case FilespaceDiscoveryClassification::duplicate_identity:
      return "quarantine_duplicates_until_one_authoritative_identity_remains";
    case FilespaceDiscoveryClassification::stale_header:
      return "fence_and_refresh_manifest_from_authoritative_header";
    case FilespaceDiscoveryClassification::replaced_header:
      return "operator_review_before_replacement_acceptance";
    case FilespaceDiscoveryClassification::foreign_orphan:
      return "import_into_foreign_filespace_quarantine";
    case FilespaceDiscoveryClassification::quarantined_candidate:
      return "inspect_quarantine_before_release";
  }
  return "operator_review_required";
}

bool BlocksNormalAccess(FilespaceDiscoveryClassification classification) {
  return classification != FilespaceDiscoveryClassification::ok;
}

bool NeedsQuarantine(FilespaceDiscoveryClassification classification,
                     const FilespaceDiscoveryRequest& request) {
  switch (classification) {
    case FilespaceDiscoveryClassification::wrong_database:
    case FilespaceDiscoveryClassification::wrong_filespace:
    case FilespaceDiscoveryClassification::duplicate_identity:
    case FilespaceDiscoveryClassification::foreign_orphan:
      return request.quarantine_unmatched_observed;
    case FilespaceDiscoveryClassification::quarantined_candidate:
      return true;
    case FilespaceDiscoveryClassification::ok:
    case FilespaceDiscoveryClassification::missing:
    case FilespaceDiscoveryClassification::stale_header:
    case FilespaceDiscoveryClassification::replaced_header:
      return false;
  }
  return false;
}

FilespaceDiscoveryRow MakeRow(FilespaceDiscoveryClassification classification,
                              const FilespaceDiscoveryRequest& request,
                              const FilespaceDescriptor* expected,
                              const FilespaceDiscoveryCandidate* observed) {
  FilespaceDiscoveryRow row;
  row.classification = classification;
  if (expected != nullptr) {
    row.expected_database_uuid = expected->database_uuid;
    row.expected_filespace_uuid = expected->filespace_uuid;
    row.expected_path = expected->path;
  } else {
    row.expected_database_uuid = request.database_uuid;
  }
  if (observed != nullptr) {
    row.observed_database_uuid = observed->database_uuid;
    row.observed_filespace_uuid = observed->filespace_uuid;
    row.observed_path = observed->path;
  }
  row.recommended_action = RecommendedAction(classification);
  row.normal_access_allowed = !BlocksNormalAccess(classification);
  row.quarantine_required = NeedsQuarantine(classification, request);
  row.release_requires_authority =
      row.quarantine_required ||
              classification == FilespaceDiscoveryClassification::quarantined_candidate
          ? request.release_requires_authority
          : false;
  row.operator_review_required =
      BlocksNormalAccess(classification) && request.require_operator_review_for_anomalies;
  row.cache_invalidation_required =
      classification == FilespaceDiscoveryClassification::stale_header ||
      classification == FilespaceDiscoveryClassification::replaced_header ||
      classification == FilespaceDiscoveryClassification::duplicate_identity;
  row.diagnostic = MakeFilespaceDiagnostic(DiscoveryOkStatus(),
                                           DiagnosticCode(classification),
                                           MessageKey(classification),
                                           row.recommended_action);
  return row;
}

std::vector<std::size_t> ObservedWithUuid(const FilespaceDiscoveryRequest& request,
                                          const TypedUuid& filespace_uuid) {
  std::vector<std::size_t> matches;
  for (std::size_t index = 0; index < request.observed.size(); ++index) {
    if (SameUuid(request.observed[index].filespace_uuid, filespace_uuid)) {
      matches.push_back(index);
    }
  }
  return matches;
}

const FilespaceDiscoveryCandidate* ObservedWithPath(const FilespaceDiscoveryRequest& request,
                                                    const std::string& path) {
  for (const FilespaceDiscoveryCandidate& observed : request.observed) {
    if (SamePath(observed.path, path)) {
      return &observed;
    }
  }
  return nullptr;
}

bool ExpectedClaimsObserved(const FilespaceDiscoveryRequest& request,
                            const FilespaceDiscoveryCandidate& observed) {
  return std::any_of(request.expected.begin(), request.expected.end(),
                     [&](const FilespaceDescriptor& expected) {
                       return SameUuid(expected.filespace_uuid, observed.filespace_uuid) ||
                              SamePath(expected.path, observed.path);
                     });
}

const FilespaceDescriptor* ExpectedWithPath(
    const std::vector<FilespaceDescriptor>& expected,
    const std::string& path) {
  for (const FilespaceDescriptor& descriptor : expected) {
    if (SamePath(descriptor.path, path)) {
      return &descriptor;
    }
  }
  return nullptr;
}

FilespaceDiscoveryCandidate CandidateFromHeader(
    const std::string& path,
    const PhysicalFilespaceHeader& header) {
  FilespaceDiscoveryCandidate candidate;
  candidate.database_uuid = header.database_uuid;
  candidate.filespace_uuid = header.filespace_uuid;
  candidate.path = path;
  candidate.role = header.role;
  candidate.state = header.state;
  candidate.page_size = header.page_size;
  candidate.physical_filespace_id = header.physical_filespace_id;
  candidate.header_generation = header.header_generation;
  candidate.writer_identity_uuid = header.writer_identity_uuid;
  candidate.physical_header_present = true;
  return candidate;
}

FilespaceDiscoveryCandidate MissingHeaderCandidate(
    const std::string& path,
    const FilespaceDescriptor* expected) {
  FilespaceDiscoveryCandidate candidate;
  if (expected != nullptr) {
    candidate.database_uuid = expected->database_uuid;
    candidate.filespace_uuid = expected->filespace_uuid;
    candidate.role = expected->role;
    candidate.state = expected->state;
    candidate.page_size = expected->page_size;
    candidate.physical_filespace_id = expected->physical_filespace_id;
    candidate.writer_identity_uuid = expected->writer_identity_uuid;
  }
  candidate.path = path;
  candidate.header_generation = 0;
  candidate.physical_header_present = false;
  return candidate;
}

std::vector<std::string> ScanPathsForRequest(
    const FilespaceDiscoveryFilesystemScanRequest& request) {
  std::vector<std::string> paths;
  for (const std::string& path : request.observed_paths) {
    if (!path.empty() &&
        std::find(paths.begin(), paths.end(), path) == paths.end()) {
      paths.push_back(path);
    }
  }
  if (paths.empty()) {
    for (const FilespaceDescriptor& expected : request.expected) {
      if (!expected.path.empty() &&
          std::find(paths.begin(), paths.end(), expected.path) == paths.end()) {
        paths.push_back(expected.path);
      }
    }
  }
  return paths;
}

FilespaceDiscoveryClassification ClassifyExpected(
    const FilespaceDiscoveryRequest& request,
    const FilespaceDescriptor& expected,
    const FilespaceDiscoveryCandidate** observed_out) {
  *observed_out = nullptr;
  const std::vector<std::size_t> uuid_matches = ObservedWithUuid(request, expected.filespace_uuid);
  if (uuid_matches.size() > 1) {
    *observed_out = &request.observed[uuid_matches.front()];
    return FilespaceDiscoveryClassification::duplicate_identity;
  }
  if (uuid_matches.size() == 1) {
    const FilespaceDiscoveryCandidate& observed = request.observed[uuid_matches.front()];
    *observed_out = &observed;
    if (!SameUuid(expected.database_uuid, observed.database_uuid)) {
      return FilespaceDiscoveryClassification::wrong_database;
    }
    if (observed.state == FilespaceState::quarantine ||
        expected.state == FilespaceState::quarantine) {
      return FilespaceDiscoveryClassification::quarantined_candidate;
    }
    if (!observed.physical_header_present ||
        observed.header_generation < expected.header_generation) {
      return FilespaceDiscoveryClassification::stale_header;
    }
    if (observed.header_generation > expected.header_generation ||
        observed.physical_filespace_id != expected.physical_filespace_id ||
        !SameWriterIdentity(expected.writer_identity_uuid, observed.writer_identity_uuid)) {
      return FilespaceDiscoveryClassification::replaced_header;
    }
    return FilespaceDiscoveryClassification::ok;
  }

  const FilespaceDiscoveryCandidate* observed = ObservedWithPath(request, expected.path);
  if (observed == nullptr) {
    return FilespaceDiscoveryClassification::missing;
  }
  *observed_out = observed;
  if (!SameUuid(expected.database_uuid, observed->database_uuid)) {
    return FilespaceDiscoveryClassification::wrong_database;
  }
  return FilespaceDiscoveryClassification::wrong_filespace;
}

void AddRow(FilespaceDiscoveryResult* result, FilespaceDiscoveryRow row) {
  if (row.classification != FilespaceDiscoveryClassification::ok) {
    ++result->anomaly_count;
  }
  result->quarantine_required = result->quarantine_required || row.quarantine_required;
  result->operator_review_required =
      result->operator_review_required || row.operator_review_required;
  result->cache_invalidation_required =
      result->cache_invalidation_required || row.cache_invalidation_required;
  result->rows.push_back(std::move(row));
}

const FilespaceDiscoveryCandidate* ObservedForRow(
    const FilespaceDiscoveryRequest& request,
    const FilespaceDiscoveryRow& row) {
  for (const FilespaceDiscoveryCandidate& observed : request.observed) {
    if (row.observed_filespace_uuid.valid() &&
        SameUuid(observed.filespace_uuid, row.observed_filespace_uuid) &&
        (row.observed_path.empty() || SamePath(observed.path, row.observed_path))) {
      return &observed;
    }
  }
  for (const FilespaceDiscoveryCandidate& observed : request.observed) {
    if (SamePath(observed.path, row.observed_path)) {
      return &observed;
    }
  }
  return nullptr;
}

FilespaceDescriptor* MutableDescriptorForRow(
    FilespaceRegistry* registry,
    const FilespaceDiscoveryRow& row) {
  if (registry == nullptr) {
    return nullptr;
  }
  const TypedUuid filespace_uuid = row.expected_filespace_uuid.valid()
                                       ? row.expected_filespace_uuid
                                       : row.observed_filespace_uuid;
  if (!filespace_uuid.valid()) {
    return nullptr;
  }
  for (FilespaceDescriptor& descriptor : registry->filespaces) {
    if (descriptor.filespace_uuid.value == filespace_uuid.value) {
      return &descriptor;
    }
  }
  return nullptr;
}

bool RowEligibleForPhysicalCleanup(const FilespaceDiscoveryRow& row) {
  return row.classification == FilespaceDiscoveryClassification::quarantined_candidate ||
         row.classification == FilespaceDiscoveryClassification::foreign_orphan;
}

FilespaceDiscoveryExecutionResult ExecutionError(
    FilespaceDiscoveryExecutionResult result,
    std::string diagnostic_code,
    std::string message_key,
    std::string detail = {}) {
  result.status = DiscoveryErrorStatus();
  result.diagnostic = MakeFilespaceDiagnostic(result.status,
                                              std::move(diagnostic_code),
                                              std::move(message_key),
                                              std::move(detail));
  return result;
}

std::string ExecutionOperationUuid(const FilespaceDiscoveryExecutionRequest& request) {
  return request.operation_uuid.empty() ? "filespace.discovery.execution" : request.operation_uuid;
}

ForeignFilespaceQuarantineRequest ForeignQuarantineRequestForRow(
    const FilespaceDiscoveryExecutionRequest& request,
    const FilespaceDiscoveryRow& row,
    const FilespaceDiscoveryCandidate* observed) {
  ForeignFilespaceQuarantineRequest quarantine_request;
  quarantine_request.database_uuid = row.observed_database_uuid.valid()
                                         ? row.observed_database_uuid
                                         : request.discovery.database_uuid;
  quarantine_request.filespace_uuid = row.observed_filespace_uuid;
  quarantine_request.path = row.observed_path;
  quarantine_request.page_size =
      observed == nullptr
          ? static_cast<u32>(scratchbird::storage::disk::PageSizeProfile::profile_16k)
          : observed->page_size;
  quarantine_request.operation_uuid = ExecutionOperationUuid(request);
  quarantine_request.inspector_uuid = request.inspector_uuid;
  quarantine_request.release_authority_uuid = request.release_authority_uuid;
  quarantine_request.physical_header_required =
      request.physical_header_required_for_quarantine;
  quarantine_request.header_inspection_passed = request.header_inspection_passed;
  quarantine_request.release_authorized = request.release_authorized;
  return quarantine_request;
}

FilespaceOperationRequest LifecycleQuarantineRequestForRow(
    const FilespaceDiscoveryExecutionRequest& request,
    const FilespaceDiscoveryRow& row,
    const FilespaceDiscoveryCandidate* observed) {
  FilespaceOperationRequest quarantine_request;
  quarantine_request.operation = FilespaceOperation::quarantine_filespace;
  quarantine_request.database_uuid = row.expected_database_uuid.valid()
                                         ? row.expected_database_uuid
                                         : request.discovery.database_uuid;
  quarantine_request.filespace_uuid = row.expected_filespace_uuid;
  quarantine_request.path = row.expected_path.empty() ? row.observed_path : row.expected_path;
  quarantine_request.role = observed == nullptr ? FilespaceRole::secondary_data : observed->role;
  quarantine_request.page_size =
      observed == nullptr
          ? static_cast<u32>(scratchbird::storage::disk::PageSizeProfile::profile_16k)
          : observed->page_size;
  quarantine_request.physical_filespace_id =
      observed == nullptr ? 0 : observed->physical_filespace_id;
  quarantine_request.header_generation = observed == nullptr ? 0 : observed->header_generation;
  quarantine_request.writer_identity_uuid =
      observed == nullptr ? TypedUuid{} : observed->writer_identity_uuid;
  quarantine_request.reason = ExecutionOperationUuid(request);
  quarantine_request.policy.require_no_active_pins_for_quarantine = true;
  return quarantine_request;
}

FilespaceOperationRequest LifecyclePhysicalCleanupRequestForDescriptor(
    const FilespaceDiscoveryExecutionRequest& request,
    const FilespaceDescriptor& descriptor) {
  FilespaceOperationRequest cleanup_request;
  cleanup_request.operation = FilespaceOperation::delete_physical_filespace;
  cleanup_request.database_uuid = descriptor.database_uuid;
  cleanup_request.filespace_uuid = descriptor.filespace_uuid;
  cleanup_request.path = descriptor.path;
  cleanup_request.role = descriptor.role;
  cleanup_request.page_size = descriptor.page_size;
  cleanup_request.physical_filespace_id = descriptor.physical_filespace_id;
  cleanup_request.total_pages = descriptor.total_pages;
  cleanup_request.free_pages = descriptor.free_pages;
  cleanup_request.preallocated_pages = descriptor.preallocated_pages;
  cleanup_request.allocation_root_page = descriptor.allocation_root_page;
  cleanup_request.header_generation = descriptor.header_generation;
  cleanup_request.writer_identity_uuid = descriptor.writer_identity_uuid;
  cleanup_request.reason = ExecutionOperationUuid(request);
  cleanup_request.policy.require_no_active_pins_for_delete_physical = true;
  cleanup_request.policy.allow_physical_filespace_delete =
      request.allow_physical_filespace_delete;
  cleanup_request.policy.physical_delete_retention_satisfied =
      request.physical_delete_retention_satisfied;
  cleanup_request.policy.physical_delete_legal_hold_clear =
      request.physical_delete_legal_hold_clear;
  cleanup_request.policy.physical_delete_cleanup_horizon_authoritative =
      request.physical_delete_cleanup_horizon_authoritative;
  return cleanup_request;
}

}  // namespace

const char* FilespaceDiscoveryClassificationName(
    FilespaceDiscoveryClassification classification) {
  switch (classification) {
    case FilespaceDiscoveryClassification::ok: return "ok";
    case FilespaceDiscoveryClassification::missing: return "missing";
    case FilespaceDiscoveryClassification::wrong_database: return "wrong_database";
    case FilespaceDiscoveryClassification::wrong_filespace: return "wrong_filespace";
    case FilespaceDiscoveryClassification::duplicate_identity: return "duplicate_identity";
    case FilespaceDiscoveryClassification::stale_header: return "stale_header";
    case FilespaceDiscoveryClassification::replaced_header: return "replaced_header";
    case FilespaceDiscoveryClassification::foreign_orphan: return "foreign_orphan";
    case FilespaceDiscoveryClassification::quarantined_candidate:
      return "quarantined_candidate";
  }
  return "unknown";
}

FilespaceDiscoveryResult DiscoverFilespaceAnomalies(
    const FilespaceDiscoveryRequest& request) {
  FilespaceDiscoveryResult result;
  result.status = DiscoveryOkStatus();

  for (const FilespaceDescriptor& expected : request.expected) {
    const FilespaceDiscoveryCandidate* observed = nullptr;
    const auto classification = ClassifyExpected(request, expected, &observed);
    AddRow(&result, MakeRow(classification, request, &expected, observed));
  }

  for (const FilespaceDiscoveryCandidate& observed : request.observed) {
    if (ExpectedClaimsObserved(request, observed)) {
      continue;
    }
    AddRow(&result,
           MakeRow(FilespaceDiscoveryClassification::foreign_orphan,
                   request,
                   nullptr,
                   &observed));
  }

  result.diagnostic = MakeFilespaceDiagnostic(
      result.status,
      result.anomaly_count == 0 ? "SB-FILESPACE-DISCOVERY-CLEAN"
                                : "SB-FILESPACE-DISCOVERY-ANOMALIES",
      result.anomaly_count == 0 ? "storage.filespace.discovery.clean"
                                : "storage.filespace.discovery.anomalies",
      std::to_string(result.anomaly_count));
  return result;
}

FilespaceDiscoveryExecutionResult ExecuteFilespaceDiscoveryActions(
    FilespaceRegistry* registry,
    const FilespaceDiscoveryExecutionRequest& request) {
  FilespaceDiscoveryExecutionResult result;
  result.discovery = DiscoverFilespaceAnomalies(request.discovery);
  result.status = result.discovery.status;
  result.diagnostic = result.discovery.diagnostic;
  result.cache_invalidation_required = result.discovery.cache_invalidation_required;

  if (!result.discovery.ok()) {
    return result;
  }
  if (!request.execute_quarantine_actions && !request.execute_release_actions &&
      !request.execute_physical_cleanup_actions) {
    return result;
  }
  if (request.execute_release_actions && request.execute_physical_cleanup_actions) {
    return ExecutionError(std::move(result),
                          "SB-FILESPACE-DISCOVERY-RELEASE-CLEANUP-CONFLICT",
                          "storage.filespace.discovery.release_cleanup_conflict");
  }
  if (registry == nullptr) {
    return ExecutionError(std::move(result),
                          "SB-FILESPACE-DISCOVERY-EXECUTION-REGISTRY-NULL",
                          "storage.filespace.discovery.execution_registry_null");
  }

  for (const FilespaceDiscoveryRow& row : result.discovery.rows) {
    const FilespaceDiscoveryCandidate* observed = ObservedForRow(request.discovery, row);

    if (request.execute_quarantine_actions && row.quarantine_required &&
        row.classification != FilespaceDiscoveryClassification::quarantined_candidate) {
      if (row.classification == FilespaceDiscoveryClassification::foreign_orphan) {
        auto quarantine_request =
            ForeignQuarantineRequestForRow(request, row, observed);
        const auto quarantined =
            ImportForeignFilespaceIntoQuarantine(registry, quarantine_request);
        if (!quarantined.ok()) {
          result.status = quarantined.status;
          result.diagnostic = quarantined.diagnostic;
          return result;
        }
      } else if (row.expected_filespace_uuid.valid()) {
        auto quarantine_request =
            LifecycleQuarantineRequestForRow(request, row, observed);
        const auto quarantined =
            ApplyFilespaceOperation(registry, quarantine_request);
        if (!quarantined.ok()) {
          result.status = quarantined.status;
          result.diagnostic = quarantined.diagnostic;
          return result;
        }
      } else {
        continue;
      }
      ++result.quarantine_execution_count;
      result.cleanup_or_quarantine_executed = true;
      result.durable_state_changed = true;
      result.cache_invalidation_required = true;
    }

    if (request.execute_physical_cleanup_actions &&
        RowEligibleForPhysicalCleanup(row)) {
      FilespaceDescriptor* descriptor = MutableDescriptorForRow(registry, row);
      if (descriptor == nullptr) {
        return ExecutionError(
            std::move(result),
            "SB-FILESPACE-DISCOVERY-PHYSICAL-CLEANUP-NOT-QUARANTINED",
            "storage.filespace.discovery.physical_cleanup_not_quarantined",
            row.observed_path.empty() ? row.expected_path : row.observed_path);
      }
      auto cleanup_request =
          LifecyclePhysicalCleanupRequestForDescriptor(request, *descriptor);
      const auto cleanup = ApplyFilespaceOperation(registry, cleanup_request);
      if (!cleanup.ok()) {
        result.status = cleanup.status;
        result.diagnostic = cleanup.diagnostic;
        return result;
      }
      ++result.physical_cleanup_execution_count;
      result.physical_cleanup_executed = true;
      result.physical_file_removed =
          result.physical_file_removed || cleanup.physical_file_removed;
      result.cleanup_or_quarantine_executed = true;
      result.durable_state_changed = true;
      result.cache_invalidation_required = true;
    }

    if (request.execute_release_actions &&
        row.classification == FilespaceDiscoveryClassification::quarantined_candidate) {
      auto release_request = ForeignQuarantineRequestForRow(request, row, observed);
      if (!release_request.filespace_uuid.valid()) {
        release_request.filespace_uuid = row.expected_filespace_uuid;
      }
      if (release_request.path.empty()) {
        release_request.path = row.expected_path;
      }
      if (!release_request.database_uuid.valid()) {
        release_request.database_uuid = row.expected_database_uuid.valid()
                                            ? row.expected_database_uuid
                                            : request.discovery.database_uuid;
      }
      const auto released =
          ReleaseForeignFilespaceQuarantine(registry, release_request);
      if (!released.ok()) {
        result.status = released.status;
        result.diagnostic = released.diagnostic;
        return result;
      }
      ++result.release_execution_count;
      result.release_executed = true;
      result.durable_state_changed = true;
      result.cache_invalidation_required = true;
    }
  }

  result.status = DiscoveryOkStatus();
  result.discovery.durable_state_changed = result.durable_state_changed;
  result.discovery.cache_invalidation_required = result.cache_invalidation_required;
  result.diagnostic = MakeFilespaceDiagnostic(
      result.status,
      "SB-FILESPACE-DISCOVERY-EXECUTION-COMPLETED",
      "storage.filespace.discovery.execution_completed",
      "quarantined=" + std::to_string(result.quarantine_execution_count) +
          ";released=" + std::to_string(result.release_execution_count) +
          ";physical_cleanup=" +
          std::to_string(result.physical_cleanup_execution_count));
  return result;
}

FilespaceDiscoveryFilesystemScanResult DiscoverFilespaceAnomaliesFromFilesystem(
    const FilespaceDiscoveryFilesystemScanRequest& request) {
  FilespaceDiscoveryFilesystemScanResult result;
  result.status = DiscoveryOkStatus();
  result.runtime_filesystem_scan_executed = true;
  result.durable_state_changed = false;
  result.cleanup_or_quarantine_executed = false;

  if (!request.database_uuid.valid()) {
    result.status = DiscoveryErrorStatus();
    result.diagnostic = MakeFilespaceDiagnostic(
        result.status,
        "SB-FILESPACE-DISCOVERY-RUNTIME-SCAN-DATABASE-UUID-INVALID",
        "storage.filespace.discovery.runtime_scan_database_uuid_invalid");
    return result;
  }

  FilespaceDiscoveryRequest classifier_request;
  classifier_request.database_uuid = request.database_uuid;
  classifier_request.expected = request.expected;
  classifier_request.quarantine_unmatched_observed =
      request.quarantine_unmatched_observed;
  classifier_request.release_requires_authority =
      request.release_requires_authority;
  classifier_request.require_operator_review_for_anomalies =
      request.require_operator_review_for_anomalies;

  for (const std::string& path : ScanPathsForRequest(request)) {
    ++result.scanned_path_count;
    std::error_code error;
    if (!std::filesystem::exists(path, error) ||
        error ||
        !std::filesystem::is_regular_file(path, error) ||
        error) {
      ++result.missing_path_count;
      continue;
    }

    const auto header = ReadPhysicalFilespaceHeader(path);
    if (header.ok()) {
      ++result.observed_header_count;
      classifier_request.observed.push_back(CandidateFromHeader(path, header.header));
      continue;
    }

    ++result.unreadable_header_count;
    classifier_request.observed.push_back(
        MissingHeaderCandidate(path, ExpectedWithPath(request.expected, path)));
  }

  result.discovery = DiscoverFilespaceAnomalies(classifier_request);
  result.observed = classifier_request.observed;
  result.status = result.discovery.status;
  result.discovery.durable_state_changed = false;
  result.diagnostic = MakeFilespaceDiagnostic(
      result.status,
      result.discovery.ok() ? "SB-FILESPACE-DISCOVERY-RUNTIME-SCAN-COMPLETED"
                            : "SB-FILESPACE-DISCOVERY-RUNTIME-SCAN-FAILED",
      result.discovery.ok() ? "storage.filespace.discovery.runtime_scan_completed"
                            : "storage.filespace.discovery.runtime_scan_failed",
      "scanned=" + std::to_string(result.scanned_path_count) +
          ";observed_headers=" + std::to_string(result.observed_header_count) +
          ";missing_paths=" + std::to_string(result.missing_path_count) +
          ";unreadable_headers=" + std::to_string(result.unreadable_header_count));
  return result;
}

}  // namespace scratchbird::storage::filespace
