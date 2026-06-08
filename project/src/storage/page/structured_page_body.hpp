// Copyright (c) 2026 ScratchBird Software Inc.
//
// This Source Code Form is subject to the terms of the Mozilla Public
// License, v. 2.0. If a copy of the MPL was not distributed with this
// file, You can obtain one at https://mozilla.org/MPL/2.0/.
//
// SPDX-License-Identifier: MPL-2.0

#pragma once

// SB-STRUCTURED-PAGE-BODY-ANCHOR
// Generic structured page bodies are used for declared engine-owned page
// families whose durable payload is a typed record list rather than a
// row/index/blob-specific physical format.

#include "page_header.hpp"
#include "page_registry.hpp"
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
using scratchbird::storage::disk::PageType;

inline constexpr u32 kStructuredPageBodyHeaderBytes = 80;
inline constexpr u16 kStructuredPageBodyFormatMajor = 1;
inline constexpr u16 kStructuredPageBodyFormatMinor = 0;

enum class StructuredPageBodyMutationKind : u16 {
  upsert_record = 1,
  delete_record = 2,
  clear_records = 3
};

struct StructuredPageBodyRecord {
  u32 record_kind = 0;
  u32 ordinal = 0;
  std::string key;
  std::vector<byte> payload;
};

struct StructuredPageBody {
  PageType page_type = PageType::unknown;
  PageFamily page_family = PageFamily::unknown;
  u64 page_number = 0;
  u64 generation = 1;
  std::vector<StructuredPageBodyRecord> records;
};

struct StructuredPageBodyMutation {
  StructuredPageBodyMutationKind kind =
      StructuredPageBodyMutationKind::upsert_record;
  StructuredPageBodyRecord record;
};

struct StructuredPageBodyResult {
  Status status;
  StructuredPageBody body;
  std::vector<byte> serialized;
  DiagnosticRecord diagnostic;

  bool ok() const {
    return status.ok();
  }
};

const char* StructuredPageBodyMutationKindName(
    StructuredPageBodyMutationKind kind);
u64 ComputeStructuredPageBodyChecksum(const std::vector<byte>& record_bytes);
StructuredPageBodyResult BuildStructuredPageBody(
    const StructuredPageBody& body,
    u32 page_size);
StructuredPageBodyResult ParseStructuredPageBody(
    const std::vector<byte>& serialized);
StructuredPageBodyResult ApplyStructuredPageBodyMutation(
    const StructuredPageBody& body,
    const StructuredPageBodyMutation& mutation,
    u32 page_size);
DiagnosticRecord MakeStructuredPageBodyDiagnostic(Status status,
                                                  std::string diagnostic_code,
                                                  std::string message_key,
                                                  std::string detail = {});

}  // namespace scratchbird::storage::page
