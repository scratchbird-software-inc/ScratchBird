// Copyright (c) 2026 ScratchBird Software Inc.
//
// This Source Code Form is subject to the terms of the Mozilla Public
// License, v. 2.0. If a copy of the MPL was not distributed with this
// file, You can obtain one at https://mozilla.org/MPL/2.0/.
//
// SPDX-License-Identifier: MPL-2.0

#pragma once

// SEARCH_KEY: SB_NATIVE_CATALOG_LEAF_PAGE. The native catalog leaf (page type
// 6) codec, declared apart from physical_mga_cow_store.hpp so code that only
// reads or writes catalog leaves reaches no checkpoint-layer header (in-place
// writes, step W3; owner Q20 = 1). Moved verbatim; the definitions stay in
// physical_mga_cow_store.cpp, which still includes this through its header.

#include "native_common_page_header.hpp"
#include "row_data_page.hpp"
#include "catalog_page.hpp"
#include "catalog_record_codec.hpp"
#include "catalog_name_envelope.hpp"
#include "datatype_binary_view.hpp"
#include "runtime_platform.hpp"

#include <map>
#include <optional>
#include <span>
#include <vector>

namespace scratchbird::storage::disk {
class FileDevice;
}

namespace scratchbird::storage::database {

struct NativeCatalogLeafPage {
  scratchbird::storage::disk::NativeCommonPageHeader header;
  scratchbird::storage::page::RowDataPageBody body;
};
enum class NativeCatalogLeafError {
  none, invalid_header, invalid_body, invalid_metadata, invalid_integrity,
  hash_failure, resource_exhausted, invalid_filespace, binding_mismatch,
  io_failure, encrypted_requires_crypto_authority, cluster_requires_authority,
  header_policy_requires_authority, invalid_workspace
};
struct NativeCatalogLeafResult {
  NativeCatalogLeafError error = NativeCatalogLeafError::invalid_body;
  std::optional<NativeCatalogLeafPage> page;
  std::map<scratchbird::core::platform::Uuid,
           scratchbird::core::catalog::CatalogMetadataVersion> metadata;
  std::vector<scratchbird::core::platform::byte> bytes;
  std::optional<scratchbird::core::datatypes::DatatypeBinaryDiagnosticView>
      binary_diagnostic;
  bool ok() const noexcept { return error == NativeCatalogLeafError::none && page.has_value(); }
};
// Logical image and common metadata binding only. No inventory finality,
// family/name/security admission, allocation or publication receipt is implied.
NativeCatalogLeafResult EncodeNativeCatalogLeaf(const NativeCatalogLeafPage&) noexcept;
NativeCatalogLeafResult DecodeNativeCatalogLeaf(
    const std::vector<scratchbird::core::platform::byte>&) noexcept;
struct NativeCatalogLeafRecordView {
  scratchbird::core::platform::Uuid version_uuid;
  std::size_t row_index = 0;
  scratchbird::core::catalog::CatalogMetadataVersionView metadata;
  std::optional<scratchbird::core::catalog::CatalogNamePayloadView> name;
};
struct NativeCatalogLeafPageView {
  scratchbird::storage::disk::NativeCommonPageHeader header;
  scratchbird::storage::page::RowDataPageView body;
  // Sorted by complete native version UUID; row_index preserves the physical ordinal.
  std::span<const NativeCatalogLeafRecordView> metadata;
};
struct NativeCatalogLeafViewResult {
  NativeCatalogLeafError error = NativeCatalogLeafError::invalid_body;
  std::optional<NativeCatalogLeafPageView> page;
  scratchbird::core::platform::Status row_status;
  scratchbird::storage::page::RowDataPageViewError row_error =
      scratchbird::storage::page::RowDataPageViewError::none;
  scratchbird::storage::page::RowDataPageDiagnosticView row_diagnostic;
  scratchbird::core::catalog::CatalogRecordDiagnosticView metadata_diagnostic;
  std::optional<scratchbird::core::datatypes::DatatypeBinaryDiagnosticView> binary_diagnostic;
  bool ok() const noexcept { return error == NativeCatalogLeafError::none && page.has_value(); }
};
// Input and all disjoint caller-backed records outlive every returned view.
// Scratch after refusal is unspecified and must never be used as partial evidence.
// Complete structural admission only; no allocation, MGA or current-root authority.
NativeCatalogLeafViewResult DecodeNativeCatalogLeafInto(
    std::span<const scratchbird::core::platform::byte>,
    scratchbird::storage::page::RowDataPageViewWorkspace,
    std::span<NativeCatalogLeafRecordView>) noexcept;
NativeCatalogLeafResult ReadNativeCatalogLeafFromOpenDevice(
    scratchbird::storage::disk::FileDevice&,
    const scratchbird::core::platform::Uuid& database_uuid,
    const scratchbird::storage::page::NativeCatalogRootReference&) noexcept;

}  // namespace scratchbird::storage::database
