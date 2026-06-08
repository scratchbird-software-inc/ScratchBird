// Copyright (c) 2026 ScratchBird Software Inc.
//
// This Source Code Form is subject to the terms of the Mozilla Public
// License, v. 2.0. If a copy of the MPL was not distributed with this
// file, You can obtain one at https://mozilla.org/MPL/2.0/.
//
// SPDX-License-Identifier: MPL-2.0

#pragma once

// SB-INDEX-METAPAGE-CLOSURE-ANCHOR

#include "index_family_registry.hpp"
#include "page_body_integrity.hpp"

#include <string>
#include <vector>

namespace scratchbird::core::index {

using scratchbird::core::platform::byte;
using scratchbird::core::platform::u32;
using scratchbird::core::platform::u64;
using scratchbird::storage::page::PageBodyChecksumProfile;

struct IndexMetapageControl {
  TypedUuid index_uuid;
  IndexFamily family = IndexFamily::unknown;
  u64 root_page_number = 0;
  u64 resource_epoch = 0;
  u64 mutation_epoch = 0;
  u64 root_generation = 0;
  u64 page_count = 0;
  u64 tuple_count_estimate = 0;
  u32 layout_version = 1;
  u32 flags = 0;
  std::string semantic_profile_id = "sb_native_default";
  bool durable_metadata_present = false;
  u32 metadata_format_version = 2;
  u32 minimum_reader_format_version = 1;
  PageBodyChecksumProfile checksum_profile = PageBodyChecksumProfile::strong;
  u64 identity_hash = 0;
  u64 descriptor_hash = 0;
  u64 route_capability_hash = 0;
  u64 provider_evidence_hash = 0;
  u32 provider_evidence_count = 0;
  u32 family_validator_version = 1;
  bool format_compatible = false;
  bool identity_bound = false;
  bool descriptor_hash_bound = false;
  bool route_capability_bound = false;
  bool provider_evidence_hash_bound = false;
  bool family_validator_required = false;
  bool family_validator_passed = false;
  u64 metadata_checksum_low64 = 0;
  u64 metadata_checksum_high64 = 0;
};

struct IndexMetapageResult {
  Status status;
  IndexMetapageControl control;
  std::vector<byte> serialized;
  DiagnosticRecord diagnostic;

  bool ok() const { return status.ok(); }
};

IndexMetapageResult BuildIndexMetapageControl(const IndexMetapageControl& control);
IndexMetapageResult ParseIndexMetapageControl(const std::vector<byte>& serialized);
IndexMetapageControl PopulateIndexMetapageDurableMetadata(
    const IndexMetapageControl& control);
IndexMetapageResult ValidateIndexMetapageDurableMetadata(
    const IndexMetapageControl& control);
DiagnosticRecord MakeIndexMetapageDiagnostic(Status status,
                                             std::string diagnostic_code,
                                             std::string message_key,
                                             std::string detail = {});

}  // namespace scratchbird::core::index
