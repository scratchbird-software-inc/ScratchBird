// Copyright (c) 2026 ScratchBird Software Inc.
//
// This Source Code Form is subject to the terms of the Mozilla Public
// License, v. 2.0. If a copy of the MPL was not distributed with this
// file, You can obtain one at https://mozilla.org/MPL/2.0/.
//
// SPDX-License-Identifier: MPL-2.0

#pragma once

#include "page_registry.hpp"

#include <cstdint>
#include <string>
#include <vector>

namespace scratchbird::storage::page {

// MDF-009-FINAL-DEFERRED-RESERVED-PAGE-FAMILY-BODIES
// PAGE-REGISTRY-STATUS-MATRIX-IMPLEMENTED

struct ReservedPageFamilyBodyProfile {
  bool cluster_authority_admitted = false;
  bool protected_material_authority_admitted = false;
};

struct ReservedPageFamilyBodyResult {
  bool ok = false;
  std::string diagnostic_code;
  PageFamilyDescriptor descriptor;
  std::vector<std::uint8_t> body;
  std::vector<std::uint8_t> payload;
  std::uint64_t digest = 0;
  std::vector<std::string> evidence;
};

std::uint64_t ComputeReservedPageFamilyDigest(const std::vector<std::uint8_t>& bytes);

ReservedPageFamilyBodyResult BuildReservedPageFamilyBody(
    scratchbird::storage::disk::PageType page_type,
    const std::vector<std::uint8_t>& payload,
    const ReservedPageFamilyBodyProfile& profile);

ReservedPageFamilyBodyResult ParseReservedPageFamilyBody(
    scratchbird::storage::disk::PageType expected_page_type,
    const std::vector<std::uint8_t>& body,
    const ReservedPageFamilyBodyProfile& profile);

ReservedPageFamilyBodyResult BuildReservedPageFamilyBackupRecord(
    scratchbird::storage::disk::PageType page_type,
    const std::vector<std::uint8_t>& payload,
    const ReservedPageFamilyBodyProfile& profile);

ReservedPageFamilyBodyResult BuildReservedPageFamilyRepairDecision(
    scratchbird::storage::disk::PageType page_type,
    const std::vector<std::uint8_t>& payload,
    const ReservedPageFamilyBodyProfile& profile);

}  // namespace scratchbird::storage::page
