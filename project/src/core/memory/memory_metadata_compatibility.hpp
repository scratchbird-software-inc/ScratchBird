// Copyright (c) 2026 ScratchBird Software Inc.
//
// This Source Code Form is subject to the terms of the Mozilla Public
// License, v. 2.0. If a copy of the MPL was not distributed with this
// file, You can obtain one at https://mozilla.org/MPL/2.0/.
//
// SPDX-License-Identifier: MPL-2.0

#pragma once

#include "runtime_platform.hpp"

#include <string>
#include <vector>

namespace scratchbird::core::memory {

using scratchbird::core::platform::DiagnosticRecord;
using scratchbird::core::platform::Status;
using scratchbird::core::platform::u64;

// MMCH_MEMORY_METADATA_OPEN_UPGRADE_COMPATIBILITY
enum class MemoryMetadataDomain {
  memory_policy,
  temp_workspace_manifest,
  page_cache_metadata
};

enum class MemoryMetadataOpenAction {
  open_current,
  upgrade_from_supported_legacy,
  fail_closed
};

struct MemoryMetadataOpenPolicy {
  MemoryMetadataDomain expected_domain = MemoryMetadataDomain::memory_policy;
  u64 current_format_version = 2;
  u64 oldest_supported_version = 1;
  bool allow_legacy_upgrade = true;
  bool require_authoritative_base_input = true;
  bool require_payload_checksum = true;
  bool protected_material_must_be_redacted = true;
};

struct MemoryMetadataRecord {
  MemoryMetadataDomain domain = MemoryMetadataDomain::memory_policy;
  u64 format_version = 0;
  std::string metadata_id;
  std::string schema_digest;
  bool authoritative_base_input_present = false;
  bool parser_or_client_authority = false;
  bool donor_authority = false;
  bool wal_authority = false;
  bool recovery_authority_claimed = false;
  bool payload_checksum_present = false;
  bool ambiguous_metadata = false;
  bool protected_material_redacted = true;
};

struct MemoryMetadataOpenResult {
  Status status;
  bool fail_closed = false;
  MemoryMetadataOpenAction action = MemoryMetadataOpenAction::fail_closed;
  u64 upgraded_format_version = 0;
  DiagnosticRecord diagnostic;
  std::vector<std::string> evidence;

  bool ok() const {
    return status.ok() && !fail_closed;
  }
};

const char* MemoryMetadataDomainName(MemoryMetadataDomain domain);
const char* MemoryMetadataOpenActionName(MemoryMetadataOpenAction action);
MemoryMetadataOpenResult ValidateMemoryMetadataOpen(
    const MemoryMetadataOpenPolicy& policy,
    const MemoryMetadataRecord& record);

}  // namespace scratchbird::core::memory
