// Copyright (c) 2026 ScratchBird Software Inc.
//
// This Source Code Form is subject to the terms of the Mozilla Public
// License, v. 2.0. If a copy of the MPL was not distributed with this
// file, You can obtain one at https://mozilla.org/MPL/2.0/.
//
// SPDX-License-Identifier: MPL-2.0

#pragma once

// SB-INDEX-PAGE-FAMILY-CLOSURE-ANCHOR

#include "page_header.hpp"
#include "runtime_platform.hpp"
#include "uuid.hpp"

#include <string>
#include <vector>

namespace scratchbird::storage::page {

using scratchbird::core::platform::DiagnosticRecord;
using scratchbird::core::platform::Status;
using scratchbird::core::platform::TypedUuid;
using scratchbird::core::platform::byte;
using scratchbird::core::platform::u16;
using scratchbird::core::platform::u32;
using scratchbird::core::platform::u64;
using scratchbird::storage::disk::PageType;

inline constexpr u32 kIndexPageFamilyHeaderBytes = 128;

enum class IndexPageFamilyKind : u16 {
  btree = 1,
  hash = 2,
  bitmap = 3,
  summary = 4,
  inverted = 5,
  spatial = 6,
  vector = 7,
  graph = 8,
  temporary_work = 9,
  statistics = 10,
  unknown = 0xffffu
};

struct IndexSpecialHeader {
  TypedUuid index_object_uuid;
  TypedUuid family_uuid;
  u64 resource_epoch = 0;
  u64 mutation_epoch = 0;
  u64 logical_page_number = 0;
  u64 right_sibling_page_number = 0;
  u32 layout_version = 1;
  IndexPageFamilyKind family = IndexPageFamilyKind::unknown;
  PageType page_type = PageType::unknown;
};

using IndexPageFamilyHeader = IndexSpecialHeader;

struct IndexSpecialHeaderResult {
  Status status;
  IndexSpecialHeader header;
  std::vector<byte> serialized;
  DiagnosticRecord diagnostic;

  bool ok() const { return status.ok(); }
};

using IndexPageFamilyHeaderResult = IndexSpecialHeaderResult;

const char* IndexPageFamilyKindName(IndexPageFamilyKind family);
IndexPageFamilyKind IndexPageFamilyKindForPageType(PageType page_type);
PageType RootPageTypeForIndexFamilyKind(IndexPageFamilyKind family);
IndexSpecialHeaderResult BuildIndexSpecialHeader(const IndexSpecialHeader& header);
IndexSpecialHeaderResult ParseIndexSpecialHeader(const std::vector<byte>& serialized);
IndexPageFamilyHeaderResult BuildIndexPageFamilyHeader(const IndexPageFamilyHeader& header);
IndexPageFamilyHeaderResult ParseIndexPageFamilyHeader(const std::vector<byte>& serialized);
DiagnosticRecord MakeIndexPageFamilyDiagnostic(Status status,
                                               std::string diagnostic_code,
                                               std::string message_key,
                                               std::string detail = {});

}  // namespace scratchbird::storage::page
