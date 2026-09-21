// Copyright (c) 2026 ScratchBird Software Inc.
// SPDX-License-Identifier: MPL-2.0
#pragma once
#include "native_common_page_header.hpp"
#include "row_data_page.hpp"
#include <optional>

namespace scratchbird::storage::page {
struct NativeRowDataPage {
  disk::NativeCommonPageHeader header;
  RowDataPageBody body;
};
enum class NativeRowDataError {
  none, invalid_header, invalid_body, invalid_integrity, hash_failure,
  resource_exhausted, invalid_filespace, binding_mismatch, io_failure,
  encrypted_requires_crypto_authority, cluster_requires_authority,
  header_policy_requires_authority, import_requires_authority
};
struct NativeRowDataResult {
  NativeRowDataError error=NativeRowDataError::invalid_body;
  std::optional<NativeRowDataPage> page;
  std::vector<byte> bytes;
  bool ok() const noexcept {return error==NativeRowDataError::none&&page.has_value();}
};
struct NativeRowDataReference {
  core::platform::Uuid relation_uuid;
  disk::NativePageReference page;
  std::optional<core::platform::Uuid> page_uuid;
};
// Canonical physical image only. Native inventory visibility, typed dependency
// ownership, catalog/security, allocation, overflow and publication are separate.
NativeRowDataResult EncodeNativeRowDataPage(const NativeRowDataPage&) noexcept;
NativeRowDataResult DecodeNativeRowDataPage(const std::vector<byte>&) noexcept;
// Borrows and guards one already-owned filespace; never opens/closes/writes it.
NativeRowDataResult ReadNativeRowDataPageFromOpenDevice(disk::FileDevice&,
    const core::platform::Uuid& database_uuid,const NativeRowDataReference&) noexcept;
}  // namespace scratchbird::storage::page
