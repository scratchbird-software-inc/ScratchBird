// Copyright (c) 2026 ScratchBird Software Inc.
//
// This Source Code Form is subject to the terms of the Mozilla Public
// License, v. 2.0. If a copy of the MPL was not distributed with this
// file, You can obtain one at https://mozilla.org/MPL/2.0/.
//
// SPDX-License-Identifier: MPL-2.0

#pragma once

// SB-TXN-INVENTORY-PAGE-ANCHOR
#include "runtime_platform.hpp"
#include "transaction_horizon.hpp"
#include "transaction_inventory.hpp"
#include "native_common_page_header.hpp"
#include "filespace_page_zero.hpp"
#include "disk_device.hpp"

#include <array>
#include <map>
#include <optional>
#include <span>
#include <string>
#include <vector>

namespace scratchbird::storage::page {

using scratchbird::core::platform::DiagnosticRecord;
using scratchbird::core::platform::Status;
using scratchbird::core::platform::byte;
using scratchbird::core::platform::u16;
using scratchbird::core::platform::u32;
using scratchbird::core::platform::u64;
using scratchbird::transaction::mga::LocalTransactionHorizons;
using scratchbird::transaction::mga::LocalTransactionInventory;

// MGA-CANONICAL-INVENTORY-IMAGE-001. No prototype-header or fixed-page fallback.
struct NativeTransactionInventoryPage {
  scratchbird::storage::disk::NativeCommonPageHeader header;
  scratchbird::core::platform::Uuid object_uuid;
  u64 inventory_generation = 0;
  std::optional<scratchbird::storage::disk::NativePageReference> previous;
  std::optional<scratchbird::storage::disk::NativePageReference> next;
  LocalTransactionInventory inventory;
};
struct NativeTransactionInventoryView {
  u64 next_local_transaction_id=1,next_commit_sequence=1;
  std::span<scratchbird::transaction::mga::TransactionInventoryEntry> entries;
  // Intentionally no publication-CAS base: inspection is not publication admission.
};
struct NativeTransactionInventoryPageView {
  scratchbird::storage::disk::NativeCommonPageHeader header;
  scratchbird::core::platform::Uuid object_uuid;
  u64 inventory_generation=0;
  std::optional<scratchbird::storage::disk::NativePageReference> previous,next;
  NativeTransactionInventoryView inventory;
};
enum class NativeInventoryError {
  none, invalid_header, invalid_family, invalid_reference, invalid_inventory,
  invalid_integrity, hash_failure, resource_exhausted, invalid_filespace,
  binding_mismatch, io_failure, encrypted_requires_crypto_authority, chain_mismatch,
  invalid_backing
};
struct NativeInventoryViewValidation {
  NativeInventoryError error=NativeInventoryError::invalid_inventory;
  const char* detail="";
  bool ok() const noexcept{return error==NativeInventoryError::none;}
};
// Common structural validation of a complete retained inventory, not page-chain
// ordering, evolution, transaction outcome or selection authority. Scratch may
// be reused after return; records are never sorted or mutated.
NativeInventoryViewValidation ValidateNativeTransactionInventoryView(
    const NativeTransactionInventoryView&,std::span<std::size_t> uniqueness_indices,
    std::span<byte> duplicate_markers) noexcept;
// Complete successor consistency for retained inventories, not transaction,
// retention, recovery or publication authority. Indices/markers need max(before,
// after) slots; UUID indices need before.size() slots. Reuses only caller scratch
// and never changes either record array. Read-only inputs may share storage;
// writable scratch must be aligned and disjoint from both inputs/descriptors.
NativeInventoryViewValidation ValidateNativeTransactionInventoryEvolutionView(
    const NativeTransactionInventoryView& before,const NativeTransactionInventoryView& after,
    std::span<std::size_t> indices,std::span<std::size_t> prior_uuid_indices,
    std::span<byte> markers) noexcept;
struct NativeTransactionInventoryPageResult {
  NativeInventoryError error = NativeInventoryError::invalid_family;
  std::optional<NativeTransactionInventoryPage> page;
  std::vector<byte> bytes;
  bool ok() const noexcept { return error == NativeInventoryError::none && page.has_value(); }
};
NativeTransactionInventoryPageResult EncodeNativeTransactionInventoryPage(
    const NativeTransactionInventoryPage&) noexcept;
NativeTransactionInventoryPageResult DecodeNativeTransactionInventoryPage(
    const std::vector<byte>&) noexcept;
struct NativeTransactionInventoryPageViewResult {
  NativeInventoryError error=NativeInventoryError::invalid_family;
  std::optional<NativeTransactionInventoryPageView> page;
  bool ok() const noexcept {return error==NativeInventoryError::none&&page.has_value();}
};
// Entries must outlive the view. Scratch and input may be discarded after decode.
// All regions must be disjoint; failures return no usable view or authority.
NativeTransactionInventoryPageViewResult DecodeNativeTransactionInventoryPageInto(
    std::span<const byte>,std::span<scratchbird::transaction::mga::TransactionInventoryEntry>,
    std::span<std::size_t> uniqueness_indices,std::span<byte> duplicate_markers) noexcept;
struct NativeTransactionInventoryChainResult {
  NativeInventoryError error = NativeInventoryError::invalid_reference;
  std::vector<NativeTransactionInventoryPageResult> pages;
  LocalTransactionInventory inventory;
  LocalTransactionHorizons horizons;
  u64 retained_image_bytes = 0;
  bool ok() const noexcept { return error == NativeInventoryError::none && !pages.empty(); }
};
// Borrowed devices only, ordered operation guards, exact complete chain. This
// verifies the supplied root; checkpoint selection/publication owns authority.
// No publication-CAS base is issued and no partial inventory is returned.
NativeTransactionInventoryChainResult ReadNativeTransactionInventoryChainFromOpenDevices(
    const scratchbird::core::platform::Uuid& database_uuid,
    const std::vector<scratchbird::storage::disk::NativeFilespaceDevice>&,
    const scratchbird::storage::disk::FilespaceRootReference& head,
    u64 maximum_retained_image_bytes) noexcept;

// Explicit immutable historical verification, not current inventory selection or
// transaction authority. The owner authenticates the root and exact contexts.
// The ceiling includes input contexts and actual immutable-byte observations.
NativeTransactionInventoryChainResult ReadNativeTransactionInventoryChainAtHistoricalRootFromOpenDevices(
    const scratchbird::core::platform::Uuid& database_uuid,
    const std::vector<scratchbird::storage::disk::NativeFilespaceDevice>&,
    const scratchbird::storage::disk::FilespaceRootReference& head,
    const std::array<byte,32>& root_sha256,
    const std::map<scratchbird::core::platform::Uuid,std::vector<byte>>& page_zero_images,
    u64 maximum_retained_image_bytes) noexcept;


struct NativeInventoryHistoricalPageZero {scratchbird::core::platform::Uuid filespace_uuid;std::span<const byte> image;};
enum class NativeInventoryChainReadContext { current,historical };
struct NativeInventoryChainPageView {NativeTransactionInventoryPageView page;std::span<const byte> image;};
struct NativeInventoryChainView {
  NativeInventoryError error=NativeInventoryError::invalid_reference;
  std::span<const NativeInventoryChainPageView> pages;
  NativeTransactionInventoryView inventory;
  LocalTransactionHorizons horizons;
  u64 retained_image_bytes=0;
  std::size_t backing_bytes_used=0;
  bool ok() const noexcept{return error==NativeInventoryError::none&&!pages.empty()&&horizons.valid;}
};
struct NativeInventoryChainDeviceRead {
  NativeInventoryChainView chain;
  Status io_status;
  DiagnosticRecord io_diagnostic;
  u64 physical_bytes_read=0;
  bool ok() const noexcept{return chain.ok();}
};
// Complete current/historical actual inventory with caller-owned input-disjoint
// backing and exact-device batches surviving ALL enclosing source fences.
// Shared image/chain/global-structure/horizon rules; no publication CAS base,
// selected inventory, transaction outcome or cleanup authority is granted.
NativeInventoryChainDeviceRead ReadNativeTransactionInventoryChainInto(
    const scratchbird::core::platform::Uuid&,
    std::span<const scratchbird::storage::disk::NativeFilespaceDevice>,
    const scratchbird::storage::disk::FilespaceRootReference&,u64,
    NativeInventoryChainReadContext,const std::array<byte,32>* root_sha256,
    std::span<const NativeInventoryHistoricalPageZero>,
    std::span<scratchbird::storage::disk::FileDevice::ReadLatencyBatch* const>,std::span<byte> backing) noexcept;

inline constexpr u32 kTransactionInventoryPageDigestBytes = 32;
inline constexpr u32 kTransactionInventoryPageBodyHeaderBytes = 152;
using TransactionInventoryPageDigest =
    std::array<byte, kTransactionInventoryPageDigestBytes>;

struct TransactionInventoryPageBody {
  u64 page_number = 0;
  u64 previous_page_number = 0;
  u64 next_page_number = 0;
  u64 inventory_generation = 1;
  TransactionInventoryPageDigest chain_digest{};
  LocalTransactionInventory inventory;
  LocalTransactionHorizons horizons;
};

struct TransactionInventoryPageBodyResult {
  Status status;
  TransactionInventoryPageBody body;
  std::vector<byte> serialized;
  DiagnosticRecord diagnostic;

  bool ok() const {
    return status.ok();
  }
};

u64 ComputeTransactionInventoryPageChecksum(const std::vector<byte>& body);
TransactionInventoryPageDigest ComputeTransactionInventoryPageChecksumDigest(
    const std::vector<byte>& body);
TransactionInventoryPageDigest ComputeTransactionInventoryPageChainDigest(
    const TransactionInventoryPageBody& body);
u64 TransactionInventoryPageDigestLow64(
    const TransactionInventoryPageDigest& digest);
bool TransactionInventoryPageDigestPresent(
    const TransactionInventoryPageDigest& digest);
u32 MaxTransactionInventoryEntriesPerPage(u32 page_size);
TransactionInventoryPageBodyResult BuildTransactionInventoryPageBody(const TransactionInventoryPageBody& body,
                                                                     u32 page_size);
TransactionInventoryPageBodyResult ParseTransactionInventoryPageBody(const std::vector<byte>& serialized,
                                                                     u64 page_number);
DiagnosticRecord MakeTransactionInventoryPageDiagnostic(Status status,
                                                       std::string diagnostic_code,
                                                       std::string message_key,
                                                       std::string detail = {});

}  // namespace scratchbird::storage::page
