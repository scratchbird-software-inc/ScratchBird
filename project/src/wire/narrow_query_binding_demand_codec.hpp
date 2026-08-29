// Copyright (c) 2026 ScratchBird Software Inc.
//
// This Source Code Form is subject to the terms of the Mozilla Public
// License, v. 2.0. If a copy of the MPL was not distributed with this
// file, You can obtain one at https://mozilla.org/MPL/2.0/.
//
// SPDX-License-Identifier: MPL-2.0

#pragma once

// Engine-private syntax-demand carrier for narrow-query binding issuance.
//
// SBQNDR01 carries only parser-observed demand and an already-authenticated
// statement receipt.  It deliberately carries no source occurrence,
// descriptor, datatype, hash, result, security, or transaction authority.
// The one resource value is a byte-exact copy of the engine-issued statement
// receipt policy and cannot be selected or defaulted by the parser.  All other
// identities are issued by the engine in SBQNPB01.

#include "narrow_query_binding_codec.hpp"

#include <span>
#include <string>
#include <vector>

namespace scratchbird::wire {

inline constexpr u16 kNarrowQueryBindingDemandVersion = 1;
inline constexpr u16 kNarrowQueryBindingDemandHeaderBytes = 72;
inline constexpr u32 kNarrowQueryBindingDemandResultBoundBytes = 16;
inline constexpr u32 kNarrowQueryBindingDemandSourcePrefixBytes = 32;
inline constexpr u32 kNarrowQueryBindingDemandOutputPrefixBytes = 24;
inline constexpr u32 kNarrowQueryBindingDemandOrderingPrefixBytes = 24;
inline constexpr u64 kNarrowQueryBindingDemandMaximumCarrierBytes =
    16ull * 1024ull * 1024ull;
inline constexpr u16 kNarrowQueryBindingDemandMaximumSources = 9;
inline constexpr u32 kNarrowQueryBindingDemandMaximumOutputs = 16384;
inline constexpr u32 kNarrowQueryBindingDemandMaximumOrderingTerms = 64;
inline constexpr u32 kNarrowQueryBindingDemandMaximumAliasBytes = 128;
inline constexpr u32 kNarrowQueryBindingDemandMaximumSpellingBytes = 4096;

enum class NarrowQueryBindingDemandErrorCode : u16 {
  ok = 0,
  invalid_argument,
  magic_invalid,
  version_invalid,
  extent_invalid,
  reserved_invalid,
  receipt_invalid,
  receipt_mismatch,
  profile_invalid,
  flag_invalid,
  count_invalid,
  source_extent_invalid,
  source_ordinal_invalid,
  source_relation_hint_invalid,
  source_alias_invalid,
  output_extent_invalid,
  output_ordinal_invalid,
  output_source_invalid,
  output_column_spelling_invalid,
  output_name_invalid,
  ordering_extent_invalid,
  ordering_ordinal_invalid,
  ordering_source_invalid,
  ordering_column_spelling_invalid,
  ordering_direction_invalid,
  ordering_null_placement_invalid,
  profile_shape_invalid,
  result_bound_invalid,
  resource_limit_exceeded,
  exact_bytes_mismatch,
};

struct NarrowQueryBindingDemandError {
  NarrowQueryBindingDemandErrorCode code =
      NarrowQueryBindingDemandErrorCode::ok;
  std::string diagnostic_code;
  std::string field;
  u32 record_index = 0;
  std::string detail;

  bool ok() const {
    return code == NarrowQueryBindingDemandErrorCode::ok;
  }
};

struct NarrowQuerySourceDemand {
  u32 source_ordinal = 0;
  bool relation_object_hint_present = false;
  NarrowQueryUuid relation_object_uuid_hint{};
  bool explicit_alias = false;
  std::string alias_spelling;
};

struct NarrowQueryOutputDemand {
  u32 output_ordinal = 0;
  u32 source_ordinal = 0;
  std::string source_column_spelling;
  bool output_name_present = false;
  std::string output_name_spelling;
};

struct NarrowQueryOrderingDemand {
  u32 term_ordinal = 0;
  u32 source_ordinal = 0;
  std::string source_column_spelling;
  NarrowQueryDirection direction = NarrowQueryDirection::ascending;
  NarrowQueryNullPlacement null_placement =
      NarrowQueryNullPlacement::first;
};

struct NarrowQueryBindingDemand {
  NarrowQueryUuid statement_receipt_uuid{};
  NarrowQueryProfile requested_profile =
      NarrowQueryProfile::ordered_projection;
  // Mandatory schema-7032-v71 copy.  This is independent of LIMIT/OFFSET,
  // result rows, transport bytes, optimizer memory, and every carrier limit.
  u64 maximum_mga_relation_decoded_bytes_per_pass = 0;
  bool row_limit_present = false;
  u64 row_limit = 0;
  bool row_offset_present = false;
  u64 row_offset = 0;
  std::vector<NarrowQuerySourceDemand> sources;
  std::vector<NarrowQueryOutputDemand> outputs;
  std::vector<NarrowQueryOrderingDemand> ordering_terms;

  // Populated by decode with the exact canonical carrier.  Encode accepts an
  // empty value or requires a nonempty value to equal the re-encoded carrier.
  std::vector<byte> exact_bytes;
};

struct NarrowQueryBindingDemandValidationContext {
  // The server supplies this only after authenticating and acquiring the
  // statement receipt.  Decode requires byte-exact equality and never creates
  // authority from the carrier itself.
  NarrowQueryUuid authenticated_statement_receipt_uuid{};
  // Exact value retained by the authenticated engine statement receipt.
  // Zero is never a decoder default.
  u64 maximum_mga_relation_decoded_bytes_per_pass = 0;
  u64 maximum_total_bytes = kNarrowQueryBindingDemandMaximumCarrierBytes;
};

// Produces the unique canonical SBQNDR01 encoding.  `encoded` is unchanged on
// refusal.
bool EncodeNarrowQueryBindingDemand(
    const NarrowQueryBindingDemand& demand,
    std::vector<byte>* encoded,
    NarrowQueryBindingDemandError* error);

// Validates all extents and checked arithmetic before allocating decoded
// vectors or strings.  `decoded` is unchanged on refusal.
bool DecodeAndValidateNarrowQueryBindingDemand(
    std::span<const byte> encoded,
    const NarrowQueryBindingDemandValidationContext& context,
    NarrowQueryBindingDemand* decoded,
    NarrowQueryBindingDemandError* error);

const char* NarrowQueryBindingDemandErrorCodeName(
    NarrowQueryBindingDemandErrorCode code);

}  // namespace scratchbird::wire
