// Copyright (c) 2026 ScratchBird Software Inc.
//
// This Source Code Form is subject to the terms of the Mozilla Public
// License, v. 2.0. If a copy of the MPL was not distributed with this
// file, You can obtain one at https://mozilla.org/MPL/2.0/.
//
// SPDX-License-Identifier: MPL-2.0

#pragma once

#include "page_registry.hpp"
#include "runtime_platform.hpp"

#include <string>
#include <vector>

namespace scratchbird::storage::page {

using scratchbird::core::platform::DiagnosticRecord;
using scratchbird::core::platform::Status;
using scratchbird::core::platform::u32;
using scratchbird::core::platform::u64;
using scratchbird::storage::disk::PageClassification;
using scratchbird::storage::disk::PageType;

enum class PageSkeletonKind : u32 {
  database_header,
  allocation_map,
  catalog,
  transaction_inventory,
  row_data,
  index_btree,
  index_hash,
  index_specialized,
  blob,
  structured,
  unsupported
};

enum class PageSkeletonState : u32 {
  skeleton_only,
  body_layout_reserved,
  body_implemented,
  body_parser_unavailable,
  unsupported
};

enum class PageBodyProductionAdmissionKind : u32 {
  local_engine_mutating,
  local_engine_read_only,
  skeleton_only_refused,
  layout_only_refused,
  reserved_nonmutating,
  external_cluster_provider_required,
  decryption_required,
  unregistered_refused,
  body_refused
};

struct PageSkeletonDescriptor {
  PageType page_type = PageType::unknown;
  PageSkeletonKind skeleton_kind = PageSkeletonKind::unsupported;
  PageFamily family = PageFamily::unknown;
  PageSkeletonState state = PageSkeletonState::unsupported;
  std::string stable_name;
  u64 minimum_payload_bytes = 0;
  bool engine_identity_uuid_v7_required = true;
  bool body_parser_available = false;
  bool body_mutation_available = false;
};

struct PageSkeletonLookupResult {
  Status status;
  PageSkeletonDescriptor descriptor;
  DiagnosticRecord diagnostic;

  bool ok() const {
    return status.ok();
  }
};

struct PageSkeletonClassification {
  Status status;
  PageManagerClassification manager_classification;
  PageSkeletonDescriptor descriptor;
  bool may_interpret_body = false;
  bool may_mutate_body = false;
  DiagnosticRecord diagnostic;

  bool ok() const {
    return status.ok();
  }
};

struct PageBodyProductionAdmissionResult {
  Status status;
  PageSkeletonClassification classification;
  PageBodyProductionAdmissionKind kind = PageBodyProductionAdmissionKind::body_refused;
  bool admitted = false;
  bool may_interpret_body = false;
  bool may_mutate_body = false;
  DiagnosticRecord diagnostic;

  bool ok() const {
    return status.ok() && admitted;
  }
};

const char* PageSkeletonKindName(PageSkeletonKind kind);
const char* PageSkeletonStateName(PageSkeletonState state);
const char* PageBodyProductionAdmissionKindName(PageBodyProductionAdmissionKind kind);
const std::vector<PageSkeletonDescriptor>& BuiltinPageSkeletonRegistry();
PageSkeletonLookupResult LookupPageSkeleton(PageType page_type);
PageSkeletonClassification ClassifyPageSkeleton(const PageClassification& header_classification);
PageBodyProductionAdmissionResult ClassifyPageBodyProductionAdmission(
    const PageClassification& header_classification);
DiagnosticRecord MakePageSkeletonDiagnostic(Status status,
                                            std::string diagnostic_code,
                                            std::string message_key,
                                            std::string detail = {});

}  // namespace scratchbird::storage::page
