// Copyright (c) 2026 ScratchBird Software Inc.
//
// This Source Code Form is subject to the terms of the Mozilla Public
// License, v. 2.0. If a copy of the MPL was not distributed with this
// file, You can obtain one at https://mozilla.org/MPL/2.0/.
//
// SPDX-License-Identifier: MPL-2.0

#pragma once

// SB-PAGE-LAYOUT-ANCHOR
#include "page_header.hpp"
#include "page_registry.hpp"
#include "runtime_platform.hpp"

#include <string>
#include <vector>

namespace scratchbird::storage::page {

using scratchbird::core::platform::DiagnosticRecord;
using scratchbird::core::platform::Status;
using scratchbird::core::platform::u16;
using scratchbird::core::platform::u32;
using scratchbird::storage::disk::PageType;

enum class PageBodyGrowthDirection : u16 {
  forward,
  split_fixed_variable,
  append_segments,
  opaque,
  unknown
};

struct PageLayoutDescriptor {
  PageType page_type = PageType::unknown;
  PageFamily family = PageFamily::unknown;
  std::string stable_name;
  u32 body_header_bytes = 0;
  u32 minimum_page_size = 0;
  u32 slot_entry_bytes = 0;
  u32 minimum_free_bytes = 0;
  PageBodyGrowthDirection growth = PageBodyGrowthDirection::unknown;
  bool has_row_slots = false;
  bool has_overflow_links = false;
  bool supports_variable_payload = false;
  bool supports_checksummed_body = true;
  bool supports_dense_internal_row_ordinals = false;
};

struct PageLayoutCapacity {
  PageLayoutDescriptor descriptor;
  u32 page_size = 0;
  u32 body_bytes = 0;
  u32 usable_payload_bytes = 0;
  u32 approximate_minimum_rows = 0;
};

struct PageLayoutResult {
  Status status;
  PageLayoutDescriptor descriptor;
  PageLayoutCapacity capacity;
  DiagnosticRecord diagnostic;

  bool ok() const {
    return status.ok();
  }
};

const char* PageBodyGrowthDirectionName(PageBodyGrowthDirection growth);
const std::vector<PageLayoutDescriptor>& BuiltinPageLayoutRegistry();
PageLayoutResult LookupPageLayout(PageType page_type);
PageLayoutResult ComputePageLayoutCapacity(PageType page_type, u32 page_size);
DiagnosticRecord MakePageLayoutDiagnostic(Status status,
                                          std::string diagnostic_code,
                                          std::string message_key,
                                          std::string detail = {});

}  // namespace scratchbird::storage::page
