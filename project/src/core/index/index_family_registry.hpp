// Copyright (c) 2026 ScratchBird Software Inc.
//
// This Source Code Form is subject to the terms of the Mozilla Public
// License, v. 2.0. If a copy of the MPL was not distributed with this
// file, You can obtain one at https://mozilla.org/MPL/2.0/.
//
// SPDX-License-Identifier: MPL-2.0

#pragma once

// SB-INDEX-FAMILY-REGISTRY-CLOSURE-ANCHOR
#include "runtime_platform.hpp"
#include "uuid.hpp"

#include <string>
#include <string_view>
#include <vector>

namespace scratchbird::core::index {

using scratchbird::core::platform::DiagnosticRecord;
using scratchbird::core::platform::Status;
using scratchbird::core::platform::TypedUuid;
using scratchbird::core::platform::u32;

// Accepted physical/profile families from full-index-family-implementation-closure.
enum class IndexFamily : u32 {
  btree,
  unique_btree,
  expression,
  partial,
  covering,
  hash,
  bitmap,
  brin_zone,
  bloom,
  full_text,
  gin,
  inverted,
  ngram,
  sparse_wand,
  spatial,
  rtree,
  gist,
  spgist,
  vector_exact,
  vector_hnsw,
  vector_ivf,
  columnar_zone,
  document_path,
  graph,
  temporary_work,
  in_memory,
  reference_emulated,
  policy_blocked,
  unknown
};

enum class IndexPersistenceClass : u32 {
  persistent,
  memory_primary_persisted_cold_start,
  memory_only,
  virtual_catalog,
  reference_emulated,
  policy_blocked
};

enum class IndexKeyModel : u32 {
  ordered_key,
  hashed_key,
  token_key,
  spatial_key,
  vector_key,
  zone_summary,
  expression_key,
  predicate_filtered_key,
  covering_payload,
  reference_defined
};

enum class IndexCompletionStatus : u32 {
  accepted_requires_full_implementation,
  policy_blocked_alpha,
  rejected
};

enum class IndexFamilyPhysicalCapabilityBlocker : u32 {
  none,
  contract_only,
  planner_only,
  provider_only,
  lifecycle_only,
  policy_blocked,
  rejected,
  unknown_family
};

struct IndexFamilyDescriptor {
  IndexFamily family = IndexFamily::unknown;
  std::string id;
  std::string canonical_name;
  TypedUuid family_uuid;
  IndexPersistenceClass persistence = IndexPersistenceClass::persistent;
  IndexKeyModel key_model = IndexKeyModel::ordered_key;
  IndexCompletionStatus completion = IndexCompletionStatus::accepted_requires_full_implementation;
  std::string native_physical_family;
  std::string default_semantic_profile;
  std::string metrics_prefix;
  std::string diagnostics_prefix;
  std::string packet_path;
  bool baseline = false;
  bool persistent = true;
  bool requires_mga_recheck = true;
  bool supports_ordering = false;
  bool supports_uniqueness = false;
  bool approximate = false;
};

struct IndexFamilyPhysicalCapabilityState {
  IndexFamily family = IndexFamily::unknown;
  bool declared_capability = false;
  bool planner_contract_capability = false;
  bool static_contract = false;
  bool provider_present = false;
  bool evidence_required = false;
  bool provider_admitted = false;
  bool durable_closure_admitted = false;
  bool implemented = false;
  bool physical_reader = false;
  bool physical_writer = false;
  bool maintenance = false;
  bool validate = false;
  bool repair = false;
  bool recovery_reopen = false;
  bool rebuild = false;
  bool runtime_available = false;
  bool benchmark_clean = false;
  IndexFamilyPhysicalCapabilityBlocker blocker =
      IndexFamilyPhysicalCapabilityBlocker::unknown_family;
  std::string blocker_diagnostic_code;
  std::string blocker_message_key;
  std::string blocker_detail;

  bool physically_complete() const {
    return implemented && physical_reader && physical_writer && maintenance &&
           validate && repair && recovery_reopen && rebuild;
  }
};

struct IndexFamilyLookupResult {
  Status status;
  const IndexFamilyDescriptor* descriptor = nullptr;
  DiagnosticRecord diagnostic;

  bool ok() const { return status.ok() && descriptor != nullptr; }
};

const std::vector<IndexFamilyDescriptor>& BuiltinIndexFamilyDescriptors();
const std::vector<IndexFamilyPhysicalCapabilityState>&
BuiltinIndexFamilyPhysicalCapabilityStates();
const IndexFamilyDescriptor* FindBuiltinIndexFamily(IndexFamily family);
const IndexFamilyPhysicalCapabilityState*
FindBuiltinIndexFamilyPhysicalCapabilityState(IndexFamily family);
IndexFamilyLookupResult FindBuiltinIndexFamilyById(std::string_view id);
const char* IndexFamilyName(IndexFamily family);
const char* IndexPersistenceClassName(IndexPersistenceClass persistence);
const char* IndexKeyModelName(IndexKeyModel key_model);
const char* IndexCompletionStatusName(IndexCompletionStatus completion);
const char* IndexFamilyPhysicalCapabilityBlockerName(
    IndexFamilyPhysicalCapabilityBlocker blocker);
bool IsPolicyBlockedIndexFamily(IndexFamily family);
DiagnosticRecord MakeIndexFamilyDiagnostic(Status status,
                                           std::string diagnostic_code,
                                           std::string message_key,
                                           std::string detail = {});
DiagnosticRecord MakeIndexFamilyCapabilityDiagnostic(
    Status status,
    std::string diagnostic_code,
    std::string message_key,
    std::string family_id,
    std::string detail = {},
    IndexFamilyPhysicalCapabilityBlocker blocker =
        IndexFamilyPhysicalCapabilityBlocker::unknown_family);
DiagnosticRecord MakeIndexFamilyCapabilityBlockerDiagnostic(
    Status status,
    const IndexFamilyPhysicalCapabilityState& state);

}  // namespace scratchbird::core::index
