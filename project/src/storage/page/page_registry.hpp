// Copyright (c) 2026 ScratchBird Software Inc.
//
// This Source Code Form is subject to the terms of the Mozilla Public
// License, v. 2.0. If a copy of the MPL was not distributed with this
// file, You can obtain one at https://mozilla.org/MPL/2.0/.
//
// SPDX-License-Identifier: MPL-2.0

#pragma once

#include "filespace_lifecycle.hpp"
#include "page_header.hpp"
#include "runtime_platform.hpp"

#include <string>
#include <vector>

namespace scratchbird::storage::page {

using scratchbird::core::platform::DiagnosticRecord;
using scratchbird::core::platform::Status;
using scratchbird::core::platform::u32;
using scratchbird::storage::disk::PageClassification;
using scratchbird::storage::disk::PageClassificationKind;
using scratchbird::storage::disk::PageType;

enum class PageRegistryStatus : u32 {
  selected_current,
  compatibility_transition,
  implemented,
  partially_implemented,
  reserved,
  deferred,
  superseded,
  disabled,
  quarantined
};

enum class PageFamily : u32 {
  startup,
  filespace_control,
  allocation,
  catalog,
  transaction,
  data,
  index,
  blob,
  metrics,
  archive,
  columnar,
  vector,
  graph,
  shard_placement,
  cluster_private,
  encrypted_or_opaque,
  import_export,
  protected_material,
  audit,
  repair,
  typed_dependency,
  superseded,
  reserved,
  unknown
};

struct PageFamilyDescriptor {
  PageType page_type = PageType::unknown;
  PageFamily family = PageFamily::unknown;
  PageRegistryStatus registry_status = PageRegistryStatus::reserved;
  std::string stable_name;
  bool supported_local_read = false;
  bool supported_local_write = false;
  bool cluster_only = false;
  bool encrypted_or_opaque = false;
  bool reserved = false;
  bool uses_index_special_header = false;
  bool typed_payload_dependency_required = false;
  bool resource_dependency_required = false;
  std::vector<scratchbird::storage::filespace::FilespaceRole> legal_filespace_roles;
  std::string implementation_search_key;
};

struct PageRegistryLookupResult {
  Status status;
  PageFamilyDescriptor descriptor;
  DiagnosticRecord diagnostic;

  bool ok() const {
    return status.ok();
  }
};

struct PageManagerClassification {
  Status status;
  PageFamilyDescriptor descriptor;
  PageClassification header_classification;
  bool may_read_body = false;
  bool may_write_body = false;
  bool requires_cluster_authority = false;
  bool requires_decryption = false;
  DiagnosticRecord diagnostic;

  bool ok() const {
    return status.ok();
  }
};

struct PageRegistryValidationResult {
  Status status;
  PageFamilyDescriptor descriptor;
  DiagnosticRecord diagnostic;

  bool ok() const {
    return status.ok();
  }
};

const char* PageRegistryStatusName(PageRegistryStatus status);
bool PageRegistryStatusAllowsProductSupport(PageRegistryStatus status);
const char* PageFamilyName(PageFamily family);
bool IsKnownPageFamilyName(const std::string& stable_name);
const std::vector<PageFamilyDescriptor>& BuiltinPageFamilyRegistry();
PageRegistryLookupResult LookupPageFamily(PageType page_type);
PageManagerClassification ClassifyForPageManager(const PageClassification& header_classification);
PageRegistryValidationResult ValidatePageTypeProductSupportClaim(PageType page_type,
                                                                 std::string claimed_support);
PageRegistryValidationResult ValidatePageTypeFilespaceRole(
    PageType page_type,
    scratchbird::storage::filespace::FilespaceRole filespace_role,
    std::string operation);
DiagnosticRecord MakePageRegistryDiagnostic(Status status,
                                            std::string diagnostic_code,
                                            std::string message_key,
                                            std::string detail = {});

}  // namespace scratchbird::storage::page
