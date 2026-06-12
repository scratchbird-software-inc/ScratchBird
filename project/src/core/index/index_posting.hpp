// Copyright (c) 2026 ScratchBird Software Inc.
//
// This Source Code Form is subject to the terms of the Mozilla Public
// License, v. 2.0. If a copy of the MPL was not distributed with this
// file, You can obtain one at https://mozilla.org/MPL/2.0/.
//
// SPDX-License-Identifier: MPL-2.0

#pragma once

// SB-INDEX-POSTING-CLOSURE-ANCHOR

#include "index_access_method.hpp"

namespace scratchbird::core::index {

using scratchbird::core::platform::byte;
using scratchbird::core::platform::u32;
using scratchbird::core::platform::u64;

enum class IndexPostingFlag : u32 {
  none = 0,
  deleted = 1u << 0,
  provisional = 1u << 1,
  requires_recheck = 1u << 2
};

struct IndexPostingEqualityProof {
  bool proof_present = false;
  bool non_unique_exact = false;
  bool encoded_key_bytewise_stable = false;
  bool stable_row_uuid_locators = false;
  bool preserves_mga_visibility_recheck = false;
  bool parser_or_reference_finality_authority = false;
  bool timestamp_or_uuid_order_finality_authority = false;
};

struct IndexPostingEntry {
  IndexRowLocator locator;
  u64 visible_from_transaction_id = 0;
  u64 visible_until_transaction_id = 0;
  u32 flags = 0;
};

struct IndexPostingList {
  TypedUuid index_uuid;
  std::vector<byte> encoded_key;
  bool compressed_duplicates = false;
  bool recheck_required = true;
  IndexPostingEqualityProof equality_proof;
  std::vector<IndexPostingEntry> entries;
};

struct IndexPostingEvidenceField {
  std::string name;
  std::string value;
};

struct IndexPostingCompressionCounters {
  u64 compressed_key_count = 0;
  u64 posting_entry_count = 0;
  u64 bytes_before = 0;
  u64 bytes_after = 0;
  u64 bytes_saved = 0;
  u64 equality_proof_accepted = 0;
  u64 equality_proof_refused = 0;
  u64 recheck_required = 0;
  u64 non_unique_exact_mode = 0;
};

struct IndexPostingListResult {
  Status status;
  IndexPostingList posting_list;
  std::vector<byte> serialized;
  IndexPostingCompressionCounters counters;
  std::vector<IndexPostingEvidenceField> evidence;
  DiagnosticRecord diagnostic;

  bool ok() const { return status.ok(); }
};

bool IndexPostingEqualityProofAccepted(const IndexPostingEqualityProof& proof);
IndexPostingListResult BuildIndexPostingList(const IndexPostingList& posting_list);
IndexPostingListResult ParseIndexPostingList(const std::vector<byte>& serialized);
DiagnosticRecord MakeIndexPostingDiagnostic(Status status,
                                            std::string diagnostic_code,
                                            std::string message_key,
                                            std::string detail = {});

}  // namespace scratchbird::core::index
