// Copyright (c) 2026 ScratchBird Software Inc.
//
// This Source Code Form is subject to the terms of the Mozilla Public
// License, v. 2.0. If a copy of the MPL was not distributed with this
// file, You can obtain one at https://mozilla.org/MPL/2.0/.
//
// SPDX-License-Identifier: MPL-2.0

#pragma once

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

enum class PageFamily : u32 {
  startup,
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
  cluster_private,
  encrypted_or_opaque,
  reserved,
  unknown
};

struct PageFamilyDescriptor {
  PageType page_type = PageType::unknown;
  PageFamily family = PageFamily::unknown;
  std::string stable_name;
  bool supported_local_read = false;
  bool supported_local_write = false;
  bool cluster_only = false;
  bool encrypted_or_opaque = false;
  bool reserved = false;
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

const char* PageFamilyName(PageFamily family);
bool IsKnownPageFamilyName(const std::string& stable_name);
const std::vector<PageFamilyDescriptor>& BuiltinPageFamilyRegistry();
PageRegistryLookupResult LookupPageFamily(PageType page_type);
PageManagerClassification ClassifyForPageManager(const PageClassification& header_classification);
DiagnosticRecord MakePageRegistryDiagnostic(Status status,
                                            std::string diagnostic_code,
                                            std::string message_key,
                                            std::string detail = {});

}  // namespace scratchbird::storage::page
