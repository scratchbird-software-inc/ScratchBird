// Copyright (c) 2026 ScratchBird Software Inc.
//
// This Source Code Form is subject to the terms of the Mozilla Public
// License, v. 2.0. If a copy of the MPL was not distributed with this
// file, You can obtain one at https://mozilla.org/MPL/2.0/.
//
// SPDX-License-Identifier: MPL-2.0

#pragma once

// SB-PAGE-BODY-INTEGRITY-ANCHOR
#include "index_page_family.hpp"
#include "page_header.hpp"
#include "page_registry.hpp"
#include "page_skeleton.hpp"
#include "runtime_platform.hpp"

#include <string>
#include <vector>

namespace scratchbird::storage::page {

using scratchbird::core::platform::DiagnosticRecord;
using scratchbird::core::platform::Status;
using scratchbird::core::platform::byte;
using scratchbird::core::platform::u16;
using scratchbird::core::platform::u64;
using scratchbird::storage::disk::PageHeader;
using scratchbird::storage::disk::PageType;
using scratchbird::storage::disk::SerializedPageHeader;

enum class PageBodyChecksumProfile : u16 {
  fast = 1,
  strong = 2,
  protected_keyed = 3,
  unknown = 0xffffu
};

enum class PageBodyKind : u16 {
  row_data = 1,
  index_btree = 2,
  index_hash = 3,
  index_specialized = 4,
  blob_overflow = 5,
  structured = 6,
  allocation_map = 7,
  unknown = 0xffffu
};

enum class PageBodyAgreementKind : u16 {
  accepted = 0,
  header_refused = 1,
  checksum_refused = 2,
  body_parse_refused = 3,
  body_family_mismatch = 4,
  page_kind_mismatch = 5,
  unsupported_body_kind = 6
};

struct PageBodyChecksumDigest {
  PageBodyChecksumProfile profile = PageBodyChecksumProfile::unknown;
  std::string digest_algorithm;
  std::vector<byte> digest_material;
  u64 low64 = 0;
  u64 high64 = 0;
  u16 digest_bytes = 0;
  bool protected_material_required = false;
  bool protected_material_supplied = false;
};

struct PageBodyChecksumResult {
  Status status;
  DiagnosticRecord diagnostic;
  PageBodyChecksumDigest digest;
  bool matched = false;

  bool ok() const {
    return status.ok();
  }
};

struct PageBodyAgreementRequest {
  SerializedPageHeader header{};
  std::vector<byte> body;
  PageBodyChecksumProfile checksum_profile = PageBodyChecksumProfile::fast;
  std::vector<byte> protected_key_material;
};

struct PageBodyAgreementResult {
  Status status;
  DiagnosticRecord diagnostic;
  PageBodyAgreementKind kind = PageBodyAgreementKind::header_refused;
  PageHeader header;
  PageFamily page_family = PageFamily::unknown;
  PageBodyKind body_kind = PageBodyKind::unknown;
  IndexPageFamilyKind index_family = IndexPageFamilyKind::unknown;
  PageBodyChecksumDigest checksum_digest;
  bool body_family_agreement_valid = false;
  bool production_admitted = false;
  bool production_mutating = false;
  std::vector<std::string> evidence;

  bool ok() const {
    return status.ok() && body_family_agreement_valid;
  }
};

const char* PageBodyChecksumProfileName(PageBodyChecksumProfile profile);
const char* PageBodyKindName(PageBodyKind kind);
const char* PageBodyAgreementKindName(PageBodyAgreementKind kind);
PageBodyChecksumResult ComputePageBodyChecksumDigest(
    PageBodyChecksumProfile profile,
    const std::vector<byte>& body,
    const std::vector<byte>& protected_key_material = {});
PageBodyChecksumResult VerifyPageBodyChecksumDigest(
    PageBodyChecksumProfile profile,
    const std::vector<byte>& body,
    u64 expected_low64,
    u64 expected_high64,
    const std::vector<byte>& protected_key_material = {});
PageBodyChecksumResult VerifyPageBodyChecksumDigestMaterial(
    PageBodyChecksumProfile profile,
    const std::vector<byte>& body,
    const std::vector<byte>& expected_digest_material,
    const std::vector<byte>& protected_key_material = {});
PageBodyAgreementResult ValidatePageBodyAgreement(
    const PageBodyAgreementRequest& request);
DiagnosticRecord MakePageBodyIntegrityDiagnostic(Status status,
                                                 std::string diagnostic_code,
                                                 std::string message_key,
                                                 std::string detail = {});

}  // namespace scratchbird::storage::page
