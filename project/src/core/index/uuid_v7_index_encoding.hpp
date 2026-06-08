// Copyright (c) 2026 ScratchBird Software Inc.
//
// This Source Code Form is subject to the terms of the Mozilla Public
// License, v. 2.0. If a copy of the MPL was not distributed with this
// file, You can obtain one at https://mozilla.org/MPL/2.0/.
//
// SPDX-License-Identifier: MPL-2.0

#pragma once

// SB-UUIDV7-INDEX-ENCODING-ANCHOR
#include "index_family_registry.hpp"
#include "runtime_capabilities.hpp"
#include "uuid.hpp"

#include <string>
#include <vector>

namespace scratchbird::core::index {

using scratchbird::core::platform::byte;
using scratchbird::core::platform::u16;
using scratchbird::core::platform::u32;
using scratchbird::core::platform::u64;
using scratchbird::core::platform::UuidKind;
using scratchbird::core::platform::TypedUuid;

inline constexpr const char* kUuidV7IndexEncodingSearchKey =
    "SB-UUIDV7-INDEX-ENCODING-ANCHOR";

enum class UuidV7TimePruneDecisionKind : u16 {
  scan = 1,
  prune = 2,
  fallback_uncompressed = 3
};

struct UuidV7IndexPageDictionary {
  u16 format_version = 1;
  UuidKind uuid_kind = UuidKind::unknown;
  u64 dictionary_generation = 0;
  u64 base_unix_epoch_millis = 0;
  u64 max_unix_epoch_millis = 0;
  u16 prefix_bytes = 0;
  std::vector<byte> prefix;
  u64 dictionary_checksum = 0;
  bool self_describing = true;
  bool finality_authority = false;
  bool visibility_authority = false;
  std::vector<std::string> evidence;
};

struct UuidV7IndexEncodedPage {
  bool ok = false;
  bool compressed = false;
  bool fallback_to_uncompressed_uuid = true;
  std::string refusal_reason;
  UuidV7IndexPageDictionary dictionary;
  std::vector<byte> serialized;
  std::vector<TypedUuid> decoded_round_trip;
  u64 uncompressed_bytes = 0;
  u64 compressed_bytes = 0;
  u64 bytes_saved = 0;
  std::vector<std::string> evidence;
};

struct UuidV7IndexEncodeRequest {
  std::vector<TypedUuid> uuids;
  UuidKind expected_kind = UuidKind::unknown;
  u64 dictionary_generation = 0;
  bool require_all_v7 = true;
  scratchbird::core::platform::RuntimeCompatibilityDescriptor
      runtime_compatibility;
};

struct UuidV7TimeRangePredicate {
  bool lower_present = false;
  bool upper_present = false;
  bool lower_inclusive = true;
  bool upper_inclusive = true;
  u64 lower_unix_epoch_millis = 0;
  u64 upper_unix_epoch_millis = 0;
};

struct UuidV7TimePruneDecision {
  bool ok = false;
  UuidV7TimePruneDecisionKind decision = UuidV7TimePruneDecisionKind::fallback_uncompressed;
  bool candidate_range_selected = false;
  bool fallback_to_uncompressed_scan = true;
  bool finality_authority = false;
  bool visibility_authority = false;
  u64 candidate_lower_unix_epoch_millis = 0;
  u64 candidate_upper_unix_epoch_millis = 0;
  std::string refusal_reason;
  std::vector<std::string> evidence;
};

UuidV7IndexEncodedPage BuildUuidV7IndexPageEncoding(
    const UuidV7IndexEncodeRequest& request);
UuidV7IndexEncodedPage DecodeUuidV7IndexPageEncoding(
    const std::vector<byte>& serialized,
    UuidKind expected_kind,
    u64 expected_dictionary_generation);
UuidV7TimePruneDecision PlanUuidV7TimePrefixPrune(
    const UuidV7IndexPageDictionary& dictionary,
    const UuidV7TimeRangePredicate& predicate,
    bool base_row_mga_recheck_required);
const char* UuidV7TimePruneDecisionKindName(UuidV7TimePruneDecisionKind decision);

}  // namespace scratchbird::core::index
