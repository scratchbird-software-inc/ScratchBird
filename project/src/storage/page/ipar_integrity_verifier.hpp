// Copyright (c) 2026 ScratchBird Software Inc.
//
// This Source Code Form is subject to the terms of the Mozilla Public
// License, v. 2.0. If a copy of the MPL was not distributed with this
// file, You can obtain one at https://mozilla.org/MPL/2.0/.
//
// SPDX-License-Identifier: MPL-2.0

#pragma once

#include <cstdint>
#include <string>
#include <vector>

namespace scratchbird::storage::page {

struct IparIntegritySubject {
  std::string subject_uuid;
  std::string subject_kind;
  std::uint64_t page_generation = 0;
  bool checksum_valid = true;
  bool catalog_reference_valid = true;
  bool policy_reference_valid = true;
  bool dependency_map_valid = true;
  bool committed_mga_visible = true;
};

struct IparIntegrityVerificationReport {
  bool accepted = false;
  bool clean = false;
  std::uint64_t checked_count = 0;
  std::vector<std::string> diagnostics;
  std::vector<std::string> evidence;
};

IparIntegrityVerificationReport VerifyIparBackgroundIntegrity(
    const std::vector<IparIntegritySubject>& subjects);

}  // namespace scratchbird::storage::page
