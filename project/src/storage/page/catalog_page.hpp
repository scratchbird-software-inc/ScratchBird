// Copyright (c) 2026 ScratchBird Software Inc.
//
// This Source Code Form is subject to the terms of the Mozilla Public
// License, v. 2.0. If a copy of the MPL was not distributed with this
// file, You can obtain one at https://mozilla.org/MPL/2.0/.
//
// SPDX-License-Identifier: MPL-2.0

#pragma once

// SB-CATALOG-PAGE-BODY-ANCHOR
#include "runtime_platform.hpp"

#include <string>
#include <vector>

namespace scratchbird::storage::page {

using scratchbird::core::platform::DiagnosticRecord;
using scratchbird::core::platform::Status;
using scratchbird::core::platform::byte;
using scratchbird::core::platform::u16;
using scratchbird::core::platform::u32;
using scratchbird::core::platform::u64;

inline constexpr u32 kCatalogPageBodyHeaderBytes = 64;
inline constexpr u16 kCatalogPageBodyFormatMajor = 1;
inline constexpr u16 kCatalogPageBodyFormatMinor = 0;
inline constexpr u16 kCatalogPageBodyFormatMajorMinSupported = 1;
inline constexpr u16 kCatalogPageBodyFormatMajorMaxSupported = 1;
inline constexpr u16 kCatalogPageBodyFormatMinorMaxSupported = 0;

enum class CatalogPageRowKind : u16 {
  // CLUSTER_CATALOG_PAGE_ROWS
  resource_seed_pack = 1,
  resource_seed_artifact = 2,
  resource_family_summary = 3,
  typed_catalog_record = 4,
  policy_seed_pack = 5,
  charset_record = 10,
  charset_alias_record = 11,
  collation_record = 20,
  collation_tailoring_record = 21,
  timezone_record = 30,
  timezone_transition_record = 31,
  timezone_leap_second_record = 32,
  bootstrap_object = 100,
  cluster_catalog_record = 110,
  unknown = 0xffffu
};

struct CatalogPageRow {
  CatalogPageRowKind kind = CatalogPageRowKind::unknown;
  u32 ordinal = 0;
  std::string payload;
};

struct CatalogPageBody {
  u32 page_sequence = 0;
  u64 page_number = 0;
  u64 next_page_number = 0;
  std::vector<CatalogPageRow> rows;
};

struct SerializedCatalogPageBody {
  u64 page_number = 0;
  u64 next_page_number = 0;
  std::vector<byte> body;
};

struct CatalogPageBodyResult {
  Status status;
  CatalogPageBody body;
  SerializedCatalogPageBody serialized;
  DiagnosticRecord diagnostic;

  bool ok() const {
    return status.ok();
  }
};

struct CatalogPageSetResult {
  Status status;
  std::vector<SerializedCatalogPageBody> pages;
  DiagnosticRecord diagnostic;

  bool ok() const {
    return status.ok();
  }
};

const char* CatalogPageRowKindName(CatalogPageRowKind kind);
u64 ComputeCatalogPageBodyChecksum(const std::vector<byte>& body);
CatalogPageSetResult BuildCatalogPageSet(const std::vector<CatalogPageRow>& rows,
                                         u32 page_size,
                                         u64 first_page_number,
                                         u64 overflow_first_page_number);
CatalogPageBodyResult ParseCatalogPageBody(const std::vector<byte>& body, u64 page_number);
DiagnosticRecord MakeCatalogPageDiagnostic(Status status,
                                           std::string diagnostic_code,
                                           std::string message_key,
                                           std::string detail = {});

}  // namespace scratchbird::storage::page
