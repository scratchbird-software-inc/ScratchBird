// Copyright (c) 2026 ScratchBird Software Inc.
//
// This Source Code Form is subject to the terms of the Mozilla Public
// License, v. 2.0. If a copy of the MPL was not distributed with this
// file, You can obtain one at https://mozilla.org/MPL/2.0/.
//
// SPDX-License-Identifier: MPL-2.0

#include "page_manager.hpp"

#include "metric_contracts.hpp"

#include <chrono>
#include <limits>
#include <string>
#include <utility>
#include <vector>

namespace scratchbird::storage::page {
namespace {

using scratchbird::core::platform::DiagnosticArgument;
using scratchbird::core::platform::MakeDiagnostic;
using scratchbird::core::platform::Severity;
using scratchbird::core::platform::StatusCode;
using scratchbird::core::platform::Subsystem;
using scratchbird::core::platform::UuidKind;
using scratchbird::core::uuid::IsEngineIdentityUuid;
using scratchbird::core::memory::DefaultMemoryManager;
using scratchbird::core::memory::MemoryCategory;
using scratchbird::core::memory::MemoryLifetime;
using scratchbird::core::memory::MemoryTag;
using scratchbird::core::memory::PageBufferRequest;
using scratchbird::storage::disk::ClassifyPageHeader;
using scratchbird::storage::disk::PageClassificationKind;
using scratchbird::storage::disk::PageClassificationKindName;
using scratchbird::storage::disk::IsSupportedDatabasePageSize;
using scratchbird::storage::disk::PageTypeName;
using scratchbird::storage::disk::ParsePageHeader;
using scratchbird::storage::disk::SerializePageHeader;
using scratchbird::storage::disk::ValidatePageHeader;

using Clock = std::chrono::steady_clock;

double ElapsedMicros(Clock::time_point start) {
  return static_cast<double>(std::chrono::duration_cast<std::chrono::microseconds>(Clock::now() - start).count());
}

Status PageManagerOkStatus() {
  return {StatusCode::ok, Severity::info, Subsystem::storage_page};
}

Status PageManagerErrorStatus() {
  return {StatusCode::platform_required_feature_missing, Severity::error, Subsystem::storage_page};
}

Status PageManagerWarningStatus() {
  return {StatusCode::platform_required_feature_missing, Severity::warning, Subsystem::storage_page};
}

bool IsTypedEngineIdentity(const TypedUuid& uuid, UuidKind kind) {
  return uuid.kind == kind && uuid.valid() && IsEngineIdentityUuid(uuid.value);
}

bool IsExpectedSkeletonBodyUnavailable(const PageSkeletonClassification& classification) {
  return classification.diagnostic.diagnostic_code == "SB-PAGE-SKELETON-BODY-PARSER-UNAVAILABLE";
}

bool AddWouldOverflow(u64 lhs, u64 rhs) {
  return lhs > std::numeric_limits<u64>::max() - rhs;
}

bool MulWouldOverflow(u64 lhs, u64 rhs) {
  return rhs != 0 && lhs > std::numeric_limits<u64>::max() / rhs;
}

std::string UuidLabel(const TypedUuid& uuid) {
  return scratchbird::core::uuid::UuidToString(uuid.value);
}

std::string PageFamilyLabel(PageType page_type) {
  const auto lookup = LookupPageFamily(page_type);
  return PageFamilyName(lookup.descriptor.family);
}

void RecordAllocationFailureMetric(const PageManagerContext& context,
                                   PageType page_type,
                                   const std::string& error_class) {
  (void)scratchbird::core::metrics::RecordPageAllocationFailure(error_class,
                                                                UuidLabel(context.database_uuid),
                                                                UuidLabel(context.filespace_uuid),
                                                                {},
                                                                PageFamilyLabel(page_type),
                                                                PageTypeName(page_type));
}

void RecordAllocationLatencyMetric(const PageManagerContext& context,
                                   PageType page_type,
                                   double latency_microseconds) {
  (void)scratchbird::core::metrics::ObservePageAllocationLatency(latency_microseconds,
                                                                 UuidLabel(context.database_uuid),
                                                                 UuidLabel(context.filespace_uuid),
                                                                 {},
                                                                 PageFamilyLabel(page_type),
                                                                 PageTypeName(page_type));
}

ManagedPageHeaderResult PageManagerError(std::string diagnostic_code,
                                         std::string message_key,
                                         std::string detail = {}) {
  ManagedPageHeaderResult result;
  result.status = PageManagerErrorStatus();
  result.diagnostic = MakePageManagerDiagnostic(result.status,
                                                std::move(diagnostic_code),
                                                std::move(message_key),
                                                std::move(detail));
  return result;
}

ManagedPageHeaderResult ValidateContext(const PageManagerContext& context) {
  if (!IsSupportedDatabasePageSize(context.page_size)) {
    return PageManagerError("SB-PAGE-MANAGER-PAGE-SIZE-INVALID",
                            "storage.page_manager.page_size_invalid",
                            std::to_string(context.page_size));
  }
  if (!IsTypedEngineIdentity(context.database_uuid, UuidKind::database)) {
    return PageManagerError("SB-PAGE-MANAGER-DATABASE-UUID-MUST-BE-V7",
                            "storage.page_manager.database_uuid_must_be_v7");
  }
  if (!IsTypedEngineIdentity(context.filespace_uuid, UuidKind::filespace)) {
    return PageManagerError("SB-PAGE-MANAGER-FILESPACE-UUID-MUST-BE-V7",
                            "storage.page_manager.filespace_uuid_must_be_v7");
  }

  ManagedPageHeaderResult result;
  result.status = PageManagerOkStatus();
  return result;
}

}  // namespace

u64 PageOffset(u32 page_size, u64 page_number) {
  const auto checked = CheckedPageOffset(page_size, page_number);
  return checked.ok() ? checked.offset : 0;
}

PageOffsetResult CheckedPageOffset(u32 page_size, u64 page_number) {
  if (!IsSupportedDatabasePageSize(page_size)) {
    PageOffsetResult result;
    result.status = PageManagerErrorStatus();
    result.diagnostic = MakePageManagerDiagnostic(
        result.status,
        "SB-PAGE-MANAGER-PAGE-SIZE-INVALID",
        "storage.page_manager.page_size_invalid",
        std::to_string(page_size));
    return result;
  }
  const u64 page_size_u64 = static_cast<u64>(page_size);
  if (MulWouldOverflow(page_size_u64, page_number)) {
    PageOffsetResult result;
    result.status = PageManagerErrorStatus();
    result.diagnostic = MakePageManagerDiagnostic(
        result.status,
        "SB-PAGE-MANAGER-PAGE-OFFSET-OVERFLOW",
        "storage.page_manager.page_offset_overflow",
        std::to_string(page_size) + ":" + std::to_string(page_number));
    return result;
  }

  PageOffsetResult result;
  result.status = PageManagerOkStatus();
  result.offset = page_size_u64 * page_number;
  return result;
}

PageOffsetResult CheckedPageBodyOffset(u32 page_size,
                                       u64 page_number,
                                       u64 in_page_offset) {
  auto checked = CheckedPageOffset(page_size, page_number);
  if (!checked.ok()) {
    return checked;
  }
  if (in_page_offset >= page_size) {
    PageOffsetResult result;
    result.status = PageManagerErrorStatus();
    result.diagnostic = MakePageManagerDiagnostic(
        result.status,
        "SB-PAGE-MANAGER-IN-PAGE-OFFSET-INVALID",
        "storage.page_manager.in_page_offset_invalid",
        std::to_string(in_page_offset));
    return result;
  }
  if (AddWouldOverflow(checked.offset, in_page_offset)) {
    PageOffsetResult result;
    result.status = PageManagerErrorStatus();
    result.diagnostic = MakePageManagerDiagnostic(
        result.status,
        "SB-PAGE-MANAGER-PAGE-OFFSET-OVERFLOW",
        "storage.page_manager.page_offset_overflow",
        std::to_string(page_size) + ":" + std::to_string(page_number) +
            ":" + std::to_string(in_page_offset));
    return result;
  }
  checked.offset += in_page_offset;
  return checked;
}

const char* ManagedPageQuarantineReasonName(ManagedPageQuarantineReason reason) {
  switch (reason) {
    case ManagedPageQuarantineReason::none: return "none";
    case ManagedPageQuarantineReason::invalid_magic: return "invalid_magic";
    case ManagedPageQuarantineReason::invalid_header: return "invalid_header";
    case ManagedPageQuarantineReason::checksum_mismatch: return "checksum_mismatch";
    case ManagedPageQuarantineReason::unknown_unsafe: return "unknown_unsafe";
    case ManagedPageQuarantineReason::cluster_authority_required: return "cluster_authority_required";
    case ManagedPageQuarantineReason::decryption_required: return "decryption_required";
  }
  return "unknown";
}

ManagedPageHeaderResult BuildManagedPageHeader(const ManagedPageHeaderRequest& request) {
  const auto context_result = ValidateContext(request.context);
  if (!context_result.ok()) {
    return context_result;
  }
  if (!IsTypedEngineIdentity(request.page_uuid, UuidKind::page)) {
    return PageManagerError("SB-PAGE-MANAGER-PAGE-UUID-MUST-BE-V7",
                            "storage.page_manager.page_uuid_must_be_v7");
  }
  if (request.page_type == PageType::unknown) {
    return PageManagerError("SB-PAGE-MANAGER-PAGE-TYPE-UNKNOWN",
                            "storage.page_manager.page_type_unknown");
  }

  PageHeader header;
  header.page_size = request.context.page_size;
  header.page_type = request.page_type;
  header.database_uuid = request.context.database_uuid.value;
  header.filespace_uuid = request.context.filespace_uuid.value;
  header.page_uuid = request.page_uuid.value;
  header.page_number = request.page_number;
  header.page_generation = request.page_generation;
  header.flags = request.flags;

  const auto validated = ValidatePageHeader(header);
  if (!validated.ok()) {
    ManagedPageHeaderResult result;
    result.status = validated.status;
    result.diagnostic = validated.diagnostic;
    return result;
  }

  const auto serialized = SerializePageHeader(validated.header);
  if (!serialized.ok()) {
    ManagedPageHeaderResult result;
    result.status = serialized.status;
    result.diagnostic = serialized.diagnostic;
    return result;
  }

  const auto classification = ClassifyPageSkeleton(ClassifyPageHeader(serialized.serialized));
  if (!classification.ok() && !IsExpectedSkeletonBodyUnavailable(classification)) {
    ManagedPageHeaderResult result;
    result.status = classification.status;
    result.diagnostic = classification.diagnostic;
    result.header = validated.header;
    result.serialized = serialized.serialized;
    result.classification = classification;
    return result;
  }

  ManagedPageHeaderResult result;
  result.status = PageManagerOkStatus();
  result.header = validated.header;
  result.serialized = serialized.serialized;
  result.classification = classification;
  return result;
}

ManagedPageHeaderResult ValidateManagedPageHeader(const PageManagerContext& context,
                                                  const SerializedPageHeader& serialized) {
  const auto context_result = ValidateContext(context);
  if (!context_result.ok()) {
    return context_result;
  }

  const auto parsed = ParsePageHeader(serialized);
  if (!parsed.ok()) {
    ManagedPageHeaderResult result;
    result.status = parsed.status;
    result.diagnostic = parsed.diagnostic;
    return result;
  }

  if (parsed.header.page_size != context.page_size) {
    return PageManagerError("SB-PAGE-MANAGER-PAGE-SIZE-MISMATCH",
                            "storage.page_manager.page_size_mismatch",
                            std::to_string(parsed.header.page_size));
  }
  if (parsed.header.database_uuid != context.database_uuid.value) {
    return PageManagerError("SB-PAGE-MANAGER-DATABASE-UUID-MISMATCH",
                            "storage.page_manager.database_uuid_mismatch");
  }
  if (parsed.header.filespace_uuid != context.filespace_uuid.value) {
    return PageManagerError("SB-PAGE-MANAGER-FILESPACE-UUID-MISMATCH",
                            "storage.page_manager.filespace_uuid_mismatch");
  }

  const auto classification = ClassifyPageSkeleton(ClassifyPageHeader(serialized));
  if (!classification.ok() && !IsExpectedSkeletonBodyUnavailable(classification)) {
    ManagedPageHeaderResult result;
    result.status = classification.status;
    result.diagnostic = classification.diagnostic;
    result.header = parsed.header;
    result.serialized = serialized;
    result.classification = classification;
    return result;
  }

  ManagedPageHeaderResult result;
  result.status = PageManagerOkStatus();
  result.header = parsed.header;
  result.serialized = serialized;
  result.classification = classification;
  return result;
}

ManagedPageBufferResult AllocateManagedPageBuffer(const PageManagerContext& context,
                                                  PageType page_type,
                                                  std::string purpose) {
  const auto metric_start = Clock::now();
  const auto context_result = ValidateContext(context);
  if (!context_result.ok()) {
    ManagedPageBufferResult result;
    result.status = context_result.status;
    result.diagnostic = context_result.diagnostic;
    return result;
  }
  if (page_type == PageType::unknown) {
    RecordAllocationFailureMetric(context, page_type, "page_type_unknown");
    ManagedPageBufferResult result;
    result.status = PageManagerErrorStatus();
    result.diagnostic = MakePageManagerDiagnostic(result.status,
                                                  "SB-PAGE-MANAGER-PAGE-BUFFER-TYPE-UNKNOWN",
                                                  "storage.page_manager.page_buffer_type_unknown");
    return result;
  }

  PageBufferRequest request;
  request.page_size = context.page_size;
  request.page_count = 1;
  request.tag = MemoryTag{Subsystem::storage_page,
                          std::move(purpose),
                          MemoryCategory::page_buffer,
                          MemoryLifetime::page_buffer,
                          "storage.page.manager",
                          "page_buffer"};

  auto allocated = DefaultMemoryManager().AllocateScopedPageBuffer(request);
  RecordAllocationLatencyMetric(context, page_type, ElapsedMicros(metric_start));
  ManagedPageBufferResult result;
  result.status = allocated.status;
  result.diagnostic = allocated.diagnostic;
  if (allocated.ok()) {
    result.buffer = std::move(allocated.buffer);
  } else {
    RecordAllocationFailureMetric(context,
                                  page_type,
                                  allocated.diagnostic.diagnostic_code.empty() ? "allocation_failed"
                                                                               : allocated.diagnostic.diagnostic_code);
  }
  return result;
}

ManagedPageQuarantineResult QuarantineManagedPageIfUnsafe(ManagedPageQuarantineLedger* ledger,
                                                          const PageManagerContext& context,
                                                          const SerializedPageHeader& serialized) {
  ManagedPageQuarantineResult result;
  const auto context_result = ValidateContext(context);
  if (!context_result.ok()) {
    result.status = context_result.status;
    result.diagnostic = context_result.diagnostic;
    return result;
  }

  const auto classification = ClassifyPageHeader(serialized);
  const auto parsed = ParsePageHeader(serialized);

  ManagedPageQuarantineEvidenceRecord evidence;
  evidence.sequence = ledger == nullptr ? 0 : ledger->next_evidence_sequence++;
  evidence.database_uuid = context.database_uuid;
  evidence.filespace_uuid = context.filespace_uuid;
  evidence.classification = PageClassificationKindName(classification.kind);
  evidence.diagnostic_code = classification.diagnostic.diagnostic_code;

  if (parsed.ok()) {
    evidence.page_uuid = TypedUuid{UuidKind::page, parsed.header.page_uuid};
    evidence.page_type = parsed.header.page_type;
    evidence.page_number = parsed.header.page_number;
    evidence.page_generation = parsed.header.page_generation;
  }

  switch (classification.kind) {
    case PageClassificationKind::supported_local:
    case PageClassificationKind::reserved_local:
    case PageClassificationKind::unknown_safe:
      evidence.reason = ManagedPageQuarantineReason::none;
      evidence.quarantined = false;
      result.status = PageManagerOkStatus();
      result.quarantined = false;
      result.diagnostic = MakePageManagerDiagnostic(result.status,
                                                   "SB-PAGE-MANAGER-PAGE-SAFE",
                                                   "storage.page_manager.page_safe",
                                                   evidence.classification);
      break;
    case PageClassificationKind::cluster_only:
      evidence.reason = ManagedPageQuarantineReason::cluster_authority_required;
      evidence.quarantined = true;
      result.status = PageManagerWarningStatus();
      result.quarantined = true;
      result.diagnostic = MakePageManagerDiagnostic(result.status,
                                                   "SB-PAGE-MANAGER-CLUSTER-MAPPING-UNAVAILABLE",
                                                   "storage.page_manager.cluster_mapping_unavailable",
                                                   evidence.classification);
      break;
    case PageClassificationKind::encrypted_or_opaque:
      if (context.decryption_available) {
        evidence.reason = ManagedPageQuarantineReason::none;
        evidence.quarantined = false;
        result.status = PageManagerOkStatus();
        result.quarantined = false;
        result.diagnostic = MakePageManagerDiagnostic(result.status,
                                                     "SB-PAGE-MANAGER-ENCRYPTED-PAGE-DECRYPTABLE",
                                                     "storage.page_manager.encrypted_page_decryptable",
                                                     evidence.classification);
      } else {
        evidence.reason = ManagedPageQuarantineReason::decryption_required;
        evidence.quarantined = true;
        result.status = PageManagerWarningStatus();
        result.quarantined = true;
        result.diagnostic = MakePageManagerDiagnostic(result.status,
                                                     "SB-PAGE-MANAGER-ENCRYPTED-PAGE-QUARANTINE",
                                                     "storage.page_manager.encrypted_page_quarantine",
                                                     evidence.classification);
      }
      break;
    case PageClassificationKind::invalid_magic:
      evidence.reason = ManagedPageQuarantineReason::invalid_magic;
      evidence.quarantined = true;
      result.status = PageManagerErrorStatus();
      result.quarantined = true;
      result.diagnostic = MakePageManagerDiagnostic(result.status,
                                                   "SB-PAGE-MANAGER-INVALID-MAGIC-QUARANTINE",
                                                   "storage.page_manager.invalid_magic_quarantine",
                                                   evidence.classification);
      break;
    case PageClassificationKind::checksum_mismatch:
      evidence.reason = ManagedPageQuarantineReason::checksum_mismatch;
      evidence.quarantined = true;
      result.status = PageManagerErrorStatus();
      result.quarantined = true;
      result.diagnostic = MakePageManagerDiagnostic(result.status,
                                                   "SB-PAGE-MANAGER-CHECKSUM-QUARANTINE",
                                                   "storage.page_manager.checksum_quarantine",
                                                   evidence.classification);
      break;
    case PageClassificationKind::invalid_header:
      evidence.reason = classification.diagnostic.diagnostic_code == "SB-STORAGE-PAGE-UNKNOWN-UNSAFE"
                            ? ManagedPageQuarantineReason::unknown_unsafe
                            : ManagedPageQuarantineReason::invalid_header;
      evidence.quarantined = true;
      result.status = PageManagerErrorStatus();
      result.quarantined = true;
      result.diagnostic = MakePageManagerDiagnostic(result.status,
                                                   "SB-PAGE-MANAGER-INVALID-HEADER-QUARANTINE",
                                                   "storage.page_manager.invalid_header_quarantine",
                                                   evidence.classification);
      break;
  }

  result.evidence = evidence;
  if (ledger != nullptr) {
    ledger->evidence.push_back(evidence);
  }
  return result;
}

DiagnosticRecord MakePageManagerDiagnostic(Status status,
                                           std::string diagnostic_code,
                                           std::string message_key,
                                           std::string detail) {
  std::vector<DiagnosticArgument> arguments;
  if (!detail.empty()) {
    arguments.push_back({"detail", detail});
  }

  return MakeDiagnostic(status.code,
                        status.severity,
                        status.subsystem,
                        std::move(diagnostic_code),
                        std::move(message_key),
                        std::move(arguments),
                        {},
                        "storage.page.manager");
}

}  // namespace scratchbird::storage::page
