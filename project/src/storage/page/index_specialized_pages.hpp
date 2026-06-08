// Copyright (c) 2026 ScratchBird Software Inc.
//
// This Source Code Form is subject to the terms of the Mozilla Public
// License, v. 2.0. If a copy of the MPL was not distributed with this
// file, You can obtain one at https://mozilla.org/MPL/2.0/.
//
// SPDX-License-Identifier: MPL-2.0

#pragma once

// SB-INDEX-SPECIALIZED-PAGES-CLOSURE-ANCHOR

#include "index_page_family.hpp"

namespace scratchbird::storage::page {

struct IndexSpecializedPageEntry {
  u32 entry_kind = 0;
  u32 ordinal = 0;
  TypedUuid object_uuid;
  u64 numeric_value_a = 0;
  u64 numeric_value_b = 0;
  std::vector<byte> payload;
};

struct IndexSpecializedPageBody {
  IndexPageFamilyHeader header;
  std::vector<IndexSpecializedPageEntry> entries;
};

struct IndexSpecializedPageBodyResult {
  Status status;
  IndexSpecializedPageBody body;
  std::vector<byte> serialized;
  DiagnosticRecord diagnostic;

  bool ok() const { return status.ok(); }
};

IndexSpecializedPageBodyResult BuildIndexSpecializedPageBody(const IndexSpecializedPageBody& body, u32 page_size);
IndexSpecializedPageBodyResult ParseIndexSpecializedPageBody(const std::vector<byte>& serialized);
DiagnosticRecord MakeIndexSpecializedPageDiagnostic(Status status,
                                                    std::string diagnostic_code,
                                                    std::string message_key,
                                                    std::string detail = {});

}  // namespace scratchbird::storage::page
