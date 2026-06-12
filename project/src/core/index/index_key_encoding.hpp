// Copyright (c) 2026 ScratchBird Software Inc.
//
// This Source Code Form is subject to the terms of the Mozilla Public
// License, v. 2.0. If a copy of the MPL was not distributed with this
// file, You can obtain one at https://mozilla.org/MPL/2.0/.
//
// SPDX-License-Identifier: MPL-2.0

#pragma once

// SB-INDEX-KEY-ENCODING-CLOSURE-ANCHOR
#include "index_family_registry.hpp"

#include <string>
#include <string_view>
#include <vector>

namespace scratchbird::core::index {

using scratchbird::core::platform::byte;
using scratchbird::core::platform::u16;
using scratchbird::core::platform::u32;
using scratchbird::core::platform::u64;

enum class IndexKeyComponentKind : u32 {
  scalar = 1,
  domain = 2,
  collation_key = 3,
  expression = 4,
  vector_element = 5,
  spatial_token = 6,
  text_token = 7,
  donor_raw = 8,
  uuid_v7 = 9
};

enum class IndexKeySortDirection : u16 {
  ascending = 1,
  descending = 2
};

enum class IndexKeyNullPlacement : u16 {
  nulls_first = 1,
  nulls_last = 2,
  donor_profile_default = 3
};

struct IndexKeySemanticProfile {
  std::string profile_id = "sb_native_default";
  bool donor_visible_tiebreak = false;
  bool bytewise_stable = true;
  bool requires_recheck = false;
};

struct IndexKeyEncodingComponent {
  IndexKeyComponentKind kind = IndexKeyComponentKind::scalar;
  u32 ordinal = 0;
  TypedUuid type_descriptor_uuid;
  u64 type_descriptor_epoch = 1;
  TypedUuid collation_uuid;
  IndexKeySortDirection sort_direction = IndexKeySortDirection::ascending;
  IndexKeyNullPlacement null_placement = IndexKeyNullPlacement::nulls_last;
  bool is_null = false;
  bool case_folded = false;
  bool lossy = false;
  TypedUuid uuid_v7_value;
  std::vector<byte> payload;
};

struct IndexKeyEncodingResult {
  Status status;
  std::vector<byte> encoded;
  bool lossy = false;
  bool requires_recheck = false;
  bool uuid_v7_specialized = false;
  std::vector<std::string> evidence;
  DiagnosticRecord diagnostic;

  bool ok() const { return status.ok(); }
};

struct IndexKeyPrefixBoundsResult {
  Status status;
  std::vector<byte> matcher_prefix;
  std::vector<byte> lower_bound;
  std::vector<byte> upper_bound;
  bool upper_bound_unbounded = false;
  bool lossy = false;
  bool requires_recheck = false;
  std::vector<std::string> evidence;
  DiagnosticRecord diagnostic;

  bool ok() const { return status.ok(); }
};

struct IndexKeyCompareResult {
  Status status;
  int comparison = 0;
  DiagnosticRecord diagnostic;

  bool ok() const { return status.ok(); }
};

IndexKeyEncodingResult EncodeIndexKey(const std::vector<IndexKeyEncodingComponent>& components,
                                      const IndexKeySemanticProfile& profile);
IndexKeyPrefixBoundsResult BuildEncodedPrefixMatcher(
    const std::vector<IndexKeyEncodingComponent>& components,
    const IndexKeySemanticProfile& profile);
IndexKeyPrefixBoundsResult BuildEncodedPrefixLowerBound(
    const std::vector<IndexKeyEncodingComponent>& components,
    const IndexKeySemanticProfile& profile);
IndexKeyPrefixBoundsResult BuildEncodedPrefixUpperBound(
    const std::vector<IndexKeyEncodingComponent>& components,
    const IndexKeySemanticProfile& profile);
IndexKeyCompareResult CompareEncodedIndexKeys(const std::vector<byte>& left,
                                              const std::vector<byte>& right);
IndexKeyCompareResult CompareEncodedIndexKeyBytes(std::string_view left,
                                                  std::string_view right);
bool IsOrderPreservingIndexKeyEncoding(std::string_view bytes);
bool IsUnsafeLegacyIndexKeyEncoding(std::string_view bytes);
DiagnosticRecord MakeIndexKeyEncodingDiagnostic(Status status,
                                                std::string diagnostic_code,
                                                std::string message_key,
                                                std::string detail = {});

}  // namespace scratchbird::core::index
