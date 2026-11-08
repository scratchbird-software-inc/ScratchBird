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

// W4d-3 (2026-11-08): the SBTINV01 inventory image (MGA-CANONICAL-INVENTORY-IMAGE-001),
// its codec and its chain readers are removed: nothing writes that family any more.
// The transaction inventory is the in-place one (transaction_state_page.hpp,
// transaction_header_page.hpp), read by transaction_inventory_reader.hpp into the
// views below. The SBTIP003 page body further down belongs to the legacy server
// path and is not part of this removal.
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
struct NativeTransactionInventoryConstView {
  u64 next_local_transaction_id=1,next_commit_sequence=1;
  std::span<const scratchbird::transaction::mga::TransactionInventoryEntry> entries;
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
NativeInventoryViewValidation ValidateNativeTransactionInventoryConstView(
    const NativeTransactionInventoryConstView&,std::span<std::size_t> uniqueness_indices,
    std::span<byte> duplicate_markers) noexcept;

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
