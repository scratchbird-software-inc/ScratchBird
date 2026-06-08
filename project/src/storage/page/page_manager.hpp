// Copyright (c) 2026 ScratchBird Software Inc.
//
// This Source Code Form is subject to the terms of the Mozilla Public
// License, v. 2.0. If a copy of the MPL was not distributed with this
// file, You can obtain one at https://mozilla.org/MPL/2.0/.
//
// SPDX-License-Identifier: MPL-2.0

#pragma once

// SB-PAGE-MANAGER-ANCHOR
#include "page_header.hpp"
#include "page_skeleton.hpp"
#include "runtime_platform.hpp"
#include "uuid.hpp"
#include "memory.hpp"

#include <string>
#include <vector>

namespace scratchbird::storage::page {

using scratchbird::core::platform::DiagnosticRecord;
using scratchbird::core::platform::Status;
using scratchbird::core::platform::TypedUuid;
using scratchbird::core::platform::u32;
using scratchbird::core::platform::u64;
using scratchbird::storage::disk::PageHeader;
using scratchbird::storage::disk::PageType;
using scratchbird::storage::disk::SerializedPageHeader;
using scratchbird::core::memory::ScopedPageBuffer;

struct PageManagerContext {
  u32 page_size = static_cast<u32>(scratchbird::storage::disk::PageSizeProfile::profile_16k);
  TypedUuid database_uuid;
  TypedUuid filespace_uuid;
  bool cluster_authority_active = false;
  bool decryption_available = false;
};

struct ManagedPageHeaderRequest {
  PageManagerContext context;
  PageType page_type = PageType::unknown;
  TypedUuid page_uuid;
  u64 page_number = 0;
  u64 page_generation = 1;
  u64 flags = 0;
};

struct ManagedPageHeaderResult {
  Status status;
  PageHeader header;
  SerializedPageHeader serialized{};
  PageSkeletonClassification classification;
  DiagnosticRecord diagnostic;

  bool ok() const {
    return status.ok();
  }
};

struct ManagedPageBufferResult {
  Status status;
  ScopedPageBuffer buffer;
  DiagnosticRecord diagnostic;

  bool ok() const {
    return status.ok() && buffer.valid();
  }
};

struct PageOffsetResult {
  Status status;
  u64 offset = 0;
  DiagnosticRecord diagnostic;

  bool ok() const {
    return status.ok();
  }
};

enum class ManagedPageQuarantineReason : u32 {
  none,
  invalid_magic,
  invalid_header,
  checksum_mismatch,
  unknown_unsafe,
  cluster_authority_required,
  decryption_required
};

struct ManagedPageQuarantineEvidenceRecord {
  u64 sequence = 0;
  ManagedPageQuarantineReason reason = ManagedPageQuarantineReason::none;
  TypedUuid database_uuid;
  TypedUuid filespace_uuid;
  TypedUuid page_uuid;
  PageType page_type = PageType::unknown;
  u64 page_number = 0;
  u64 page_generation = 0;
  std::string classification;
  std::string diagnostic_code;
  bool quarantined = false;
};

struct ManagedPageQuarantineLedger {
  std::vector<ManagedPageQuarantineEvidenceRecord> evidence;
  u64 next_evidence_sequence = 1;
};

struct ManagedPageQuarantineResult {
  Status status;
  bool quarantined = false;
  ManagedPageQuarantineEvidenceRecord evidence;
  DiagnosticRecord diagnostic;

  bool ok() const {
    return status.ok() && !quarantined;
  }
};

u64 PageOffset(u32 page_size, u64 page_number);
PageOffsetResult CheckedPageOffset(u32 page_size, u64 page_number);
PageOffsetResult CheckedPageBodyOffset(u32 page_size,
                                       u64 page_number,
                                       u64 in_page_offset);
ManagedPageHeaderResult BuildManagedPageHeader(const ManagedPageHeaderRequest& request);
ManagedPageHeaderResult ValidateManagedPageHeader(const PageManagerContext& context,
                                                  const SerializedPageHeader& serialized);
ManagedPageBufferResult AllocateManagedPageBuffer(const PageManagerContext& context,
                                                  PageType page_type,
                                                  std::string purpose);
const char* ManagedPageQuarantineReasonName(ManagedPageQuarantineReason reason);
ManagedPageQuarantineResult QuarantineManagedPageIfUnsafe(ManagedPageQuarantineLedger* ledger,
                                                          const PageManagerContext& context,
                                                          const SerializedPageHeader& serialized);
DiagnosticRecord MakePageManagerDiagnostic(Status status,
                                           std::string diagnostic_code,
                                           std::string message_key,
                                           std::string detail = {});

}  // namespace scratchbird::storage::page
