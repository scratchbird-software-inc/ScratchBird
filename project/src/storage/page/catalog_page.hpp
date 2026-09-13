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
#include "native_common_page_header.hpp"
#include "filespace_page_zero.hpp"

#include <array>
#include <optional>
#include <string>
#include <vector>

namespace scratchbird::storage::page {

using NativeCatalogPageReference = scratchbird::storage::disk::NativePageReference;
struct NativeCatalogRootReference {
  scratchbird::core::platform::u16 role = 0;
  scratchbird::core::platform::u32 page_type = 0;
  NativeCatalogPageReference page;
  scratchbird::core::platform::Uuid object_uuid;
};
// Canonical root family; deliberately distinct from prototype SBCAT001 leaves.
// Logical image validation does not grant crypto, MGA or target-family authority.
struct NativeCatalogRoot {
  scratchbird::storage::disk::NativeCommonPageHeader header;
  scratchbird::core::platform::u16 root_kind = 2;
  scratchbird::core::platform::Uuid object_uuid;
  scratchbird::core::platform::Uuid creator_transaction_uuid;
  scratchbird::core::platform::u64 creator_local_transaction_id = 0;
  scratchbird::core::platform::u64 catalog_generation = 0;
  scratchbird::core::platform::u64 schema_epoch = 0;
  scratchbird::core::platform::u64 security_epoch = 0;
  scratchbird::core::platform::u64 resource_epoch = 0;
  std::optional<NativeCatalogPageReference> predecessor;
  std::array<scratchbird::core::platform::byte, 32> predecessor_sha256{};
  std::vector<NativeCatalogRootReference> roots;
};
enum class NativeCatalogRootError {
  none, invalid_header, invalid_family, invalid_reference, invalid_roots,
  invalid_integrity, hash_failure, resource_exhausted, invalid_filespace,
  binding_mismatch, io_failure, encrypted_requires_crypto_authority, history_mismatch
};
struct NativeCatalogRootResult {
  NativeCatalogRootError error = NativeCatalogRootError::invalid_family;
  std::optional<NativeCatalogRoot> root;
  std::vector<scratchbird::core::platform::byte> bytes;
  bool ok() const { return error == NativeCatalogRootError::none && root.has_value(); }
};
NativeCatalogRootResult EncodeNativeCatalogRoot(const NativeCatalogRoot&) noexcept;
NativeCatalogRootResult DecodeNativeCatalogRoot(const std::vector<scratchbird::core::platform::byte>&) noexcept;
// A supplied PageRefV2 must be resolved through the owning node's filespace
// authority. This verifies that exact target; it never opens another node or
// treats a valid root image as permission to serve its referenced catalogs.
NativeCatalogRootResult ReadNativeCatalogRootFromOpenDevice(
    scratchbird::storage::disk::FileDevice&,
    const scratchbird::core::platform::Uuid& database_uuid,
    const scratchbird::storage::disk::FilespaceRootReference&) noexcept;

using NativeCatalogFilespaceDevice = scratchbird::storage::disk::NativeFilespaceDevice;
struct NativeCatalogRootRangeResult {
  NativeCatalogRootError error = NativeCatalogRootError::invalid_reference;
  // Exact requested head through terminal, newest first. Never a partial range.
  std::vector<NativeCatalogRootResult> roots;
  scratchbird::core::platform::u64 retained_image_bytes = 0;
  bool ok() const noexcept { return error == NativeCatalogRootError::none && !roots.empty(); }
};
// Verifies actual immutable links, not inventory finality or retention authority.
// Acquires device guards in binary filespace UUID order. The caller must not
// already hold an unordered subset of those guards. Never opens/closes devices.
NativeCatalogRootRangeResult ReadNativeCatalogRootRangeFromOpenDevices(
    const scratchbird::core::platform::Uuid& database_uuid,
    const std::vector<NativeCatalogFilespaceDevice>&,
    const scratchbird::storage::disk::FilespaceRootReference& head,
    const scratchbird::storage::disk::FilespaceRootReference& terminal,
    scratchbird::core::platform::u64 maximum_retained_image_bytes) noexcept;

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
