// Copyright (c) 2026 ScratchBird Software Inc.
//
// This Source Code Form is subject to the terms of the Mozilla Public
// License, v. 2.0. If a copy of the MPL was not distributed with this
// file, You can obtain one at https://mozilla.org/MPL/2.0/.
//
// SPDX-License-Identifier: MPL-2.0

#pragma once

// SB-FOREIGN-FILESPACE-QUARANTINE-ANCHOR
#include "filespace_lifecycle.hpp"

#include <string>

namespace scratchbird::storage::filespace {

struct ForeignFilespaceQuarantineRequest {
  TypedUuid database_uuid;
  TypedUuid filespace_uuid;
  std::string path;
  u32 page_size = static_cast<u32>(scratchbird::storage::disk::PageSizeProfile::profile_16k);
  std::string operation_uuid;
  std::string inspector_uuid;
  std::string release_authority_uuid;
  bool physical_header_required = true;
  bool header_inspection_passed = false;
  bool release_authorized = false;
};

struct ForeignFilespaceQuarantineResult {
  Status status;
  DiagnosticRecord diagnostic;
  FilespaceDescriptor descriptor;
  FilespaceEvidenceRecord evidence;
  bool quarantine_fence_active = true;
  bool inspection_passed = false;
  bool release_allowed = false;
  bool durable_state_changed = false;
  bool cache_invalidation_required = false;

  bool ok() const { return status.ok(); }
};

ForeignFilespaceQuarantineResult ImportForeignFilespaceIntoQuarantine(
    FilespaceRegistry* registry,
    const ForeignFilespaceQuarantineRequest& request);
ForeignFilespaceQuarantineResult InspectForeignFilespaceQuarantine(
    const FilespaceRegistry& registry,
    const ForeignFilespaceQuarantineRequest& request);
ForeignFilespaceQuarantineResult ReleaseForeignFilespaceQuarantine(
    FilespaceRegistry* registry,
    const ForeignFilespaceQuarantineRequest& request);

}  // namespace scratchbird::storage::filespace
