// Copyright (c) 2026 ScratchBird Software Inc.
//
// This Source Code Form is subject to the terms of the Mozilla Public
// License, v. 2.0. If a copy of the MPL was not distributed with this
// file, You can obtain one at https://mozilla.org/MPL/2.0/.
//
// SPDX-License-Identifier: MPL-2.0

#pragma once

#include "api_types.hpp"
#include "narrow_query_binding_codec.hpp"
#include "narrow_query_binding_demand_codec.hpp"
#include "typed_result_producer_cursor.hpp"

#include <cstdint>
#include <memory>
#include <vector>

namespace scratchbird::engine::internal_api {

struct MgaRelationStorageDescriptor;

// SEARCH_KEY: SB_ENGINE_NARROW_QUERY_BINDING_AUTHORITY_V1
//
// This is the engine-private authority boundary between the authenticated
// syntax demand (SBQNDR01) and the canonical narrow-query binding (SBQNPB01).
// Parsers may copy the returned bytes but can neither issue nor refresh any
// identity retained by this provider.

struct EngineNarrowQueryResourceGrantV1 {
  EngineUuid grant_receipt_uuid;
  std::uint64_t grant_generation = 0;
  std::uint64_t maximum_source_rows_per_occurrence = 0;
  std::uint64_t maximum_cumulative_source_rows = 0;
  std::uint64_t maximum_result_rows = 0;
  std::uint64_t maximum_join_combinations = 0;
  std::uint64_t maximum_sort_memory_bytes = 0;
  std::uint64_t maximum_batch_rows = 0;
  std::uint64_t maximum_mga_relation_decoded_bytes_per_pass = 0;
  std::uint64_t maximum_typed_result_transport_bytes_per_packet = 0;
};

struct EngineNarrowQueryBindingAuthorityIssueRequestV1 {
  EngineRequestContext context;
  scratchbird::wire::NarrowQueryBindingDemand demand;
  EngineUuid policy_snapshot_uuid;
  std::uint64_t policy_generation = 0;

  // Exact engine resource-policy values.  They are not parser budgets and
  // LIMIT/OFFSET never substitutes for one of these ceilings.
  std::uint64_t maximum_source_rows_per_occurrence = 0;
  std::uint64_t maximum_cumulative_source_rows = 0;
  std::uint64_t maximum_result_rows = 0;
  std::uint64_t maximum_join_combinations = 0;
  std::uint64_t maximum_sort_memory_bytes = 0;
  std::uint64_t maximum_batch_rows = 0;
  std::uint64_t maximum_mga_relation_decoded_bytes_per_pass = 0;
  // Engine-issued independently of the serialized binding demand/carrier.
  // Each exact descriptor vector and each exact row-data packet is checked
  // against this noncumulative ceiling.
  std::uint64_t maximum_typed_result_transport_bytes_per_packet = 0;
};

struct EngineNarrowQueryBindingAuthorityIssueResultV1 {
  bool ok = false;
  EngineApiDiagnostic diagnostic;
  scratchbird::wire::NarrowQueryBinding binding;
  std::vector<std::uint8_t> exact_binding_bytes;
};

EngineNarrowQueryBindingAuthorityIssueResultV1
IssueNarrowQueryBindingAuthorityV1(
    const EngineNarrowQueryBindingAuthorityIssueRequestV1& request);

struct EngineNarrowQueryBindingAuthorityConsumeRequestV1;
struct EngineNarrowQueryBindingAuthorityConsumeResultV1;
struct EngineNarrowQueryBindingAuthoritySnapshotV1;
struct EngineNarrowQueryBindingLivenessResultV1;
struct EngineNarrowQuerySourceOccurrenceAuthorityResultV1;
struct EngineNarrowQueryTypedResultResourceGrantRetentionResultV1;
enum class EngineNarrowQueryWorkClassV1 : std::uint8_t;
enum class EngineNarrowQueryPublicationChargeStatusV1 : std::uint8_t;

class EngineNarrowQueryBindingAuthorityHandleV1 final {
 public:
  // Opaque outside the implementation; declared public only so the private
  // process registry can retain an incomplete shared pointer without exposing
  // any authority fields through this header.
  struct Authority;

  EngineNarrowQueryBindingAuthorityHandleV1() = default;
  EngineNarrowQueryBindingAuthorityHandleV1(
      EngineNarrowQueryBindingAuthorityHandleV1&&) noexcept = default;
  EngineNarrowQueryBindingAuthorityHandleV1& operator=(
      EngineNarrowQueryBindingAuthorityHandleV1&&) noexcept = default;
  EngineNarrowQueryBindingAuthorityHandleV1(
      const EngineNarrowQueryBindingAuthorityHandleV1&) = delete;
  EngineNarrowQueryBindingAuthorityHandleV1& operator=(
      const EngineNarrowQueryBindingAuthorityHandleV1&) = delete;

  bool valid() const noexcept { return authority_ != nullptr; }

 private:
  std::shared_ptr<Authority> authority_;

  friend struct EngineNarrowQueryBindingAuthorityConsumeResultV1;
  friend EngineNarrowQueryBindingAuthorityConsumeResultV1
  ConsumeNarrowQueryBindingAuthorityV1(
      const struct EngineNarrowQueryBindingAuthorityConsumeRequestV1&);
  friend struct EngineNarrowQueryBindingAuthoritySnapshotV1;
  friend bool CopyNarrowQueryBindingAuthoritySnapshotV1(
      const EngineNarrowQueryBindingAuthorityHandleV1&,
      struct EngineNarrowQueryBindingAuthoritySnapshotV1*,
      EngineApiDiagnostic*);
  friend struct EngineNarrowQueryBindingLivenessResultV1;
  friend EngineNarrowQueryBindingLivenessResultV1
  ObserveNarrowQueryBindingLivenessV1(
      EngineNarrowQueryBindingAuthorityHandleV1*,
      const EngineRequestContext&,
      EngineNarrowQueryWorkClassV1,
      std::uint32_t,
      std::uint64_t);
  friend EngineNarrowQueryPublicationChargeStatusV1
  CommitNarrowQueryPublicationChargeV1(
      EngineNarrowQueryBindingAuthorityHandleV1*,
      const EngineRequestContext&,
      std::uint64_t,
      std::uint64_t) noexcept;
  friend EngineNarrowQuerySourceOccurrenceAuthorityResultV1
  RevalidateNarrowQuerySourceOccurrenceAuthorityV1(
      const EngineNarrowQueryBindingAuthorityHandleV1&,
      const EngineRequestContext&,
      std::uint32_t,
      const MgaRelationStorageDescriptor&);
  friend EngineNarrowQueryTypedResultResourceGrantRetentionResultV1
  RetainNarrowQueryTypedResultResourceGrantReceiptV1(
      const EngineNarrowQueryBindingAuthorityHandleV1&,
      const EngineRequestContext&);
  friend EngineApiDiagnostic ReleaseNarrowQueryBindingAuthorityV1(
      EngineNarrowQueryBindingAuthorityHandleV1*);
};

struct EngineNarrowQueryBindingAuthorityConsumeRequestV1 {
  EngineRequestContext context;
  std::vector<std::uint8_t> exact_binding_bytes;
};

struct EngineNarrowQueryBindingAuthorityConsumeResultV1 {
  bool ok = false;
  EngineApiDiagnostic diagnostic;
  scratchbird::wire::NarrowQueryBinding binding;
  EngineNarrowQueryBindingAuthorityHandleV1 authority;
};

EngineNarrowQueryBindingAuthorityConsumeResultV1
ConsumeNarrowQueryBindingAuthorityV1(
    const EngineNarrowQueryBindingAuthorityConsumeRequestV1& request);

struct EngineNarrowQueryBindingAuthoritySnapshotV1 {
  EngineRequestContext pinned_context;
  scratchbird::wire::NarrowQueryBinding binding;
  EngineNarrowQueryResourceGrantV1 resource_grant;
};

bool CopyNarrowQueryBindingAuthoritySnapshotV1(
    const EngineNarrowQueryBindingAuthorityHandleV1& handle,
    EngineNarrowQueryBindingAuthoritySnapshotV1* snapshot,
    EngineApiDiagnostic* diagnostic);

struct EngineNarrowQueryTypedResultResourceGrantRetentionResultV1 {
  bool ok = false;
  EngineApiDiagnostic diagnostic;
  scratchbird::wire::TypedResultUuid grant_receipt_uuid{};
  std::uint64_t grant_generation = 0;
  std::uint64_t maximum_typed_result_transport_bytes_per_packet = 0;
  std::unique_ptr<TypedResultResourceGrantReceiptHandleV1> receipt_handle;
};

// Consumer-only, retain-once adapter from the exact consumed binding grant to
// the typed-result producer's private resource-grant handle.
EngineNarrowQueryTypedResultResourceGrantRetentionResultV1
RetainNarrowQueryTypedResultResourceGrantReceiptV1(
    const EngineNarrowQueryBindingAuthorityHandleV1& handle,
    const EngineRequestContext& context);

enum class EngineNarrowQueryWorkClassV1 : std::uint8_t {
  liveness_only = 0,
  source_rows = 1,
  result_rows = 2,
  join_combinations = 3,
  sort_memory_bytes = 4,
  batch_rows = 5,
  mga_relation_decoded_bytes_per_pass = 6,
};

enum class EngineNarrowQueryPublicationChargeStatusV1 : std::uint8_t {
  committed = 0,
  cancelled,
  stale,
  resource_budget_exceeded,
};

struct EngineNarrowQueryBindingLivenessResultV1 {
  bool ok = false;
  bool cancelled = false;
  bool stale = false;
  bool resource_exhausted = false;
  EngineApiDiagnostic diagnostic;
};

// Checks the exact retained receipt/snapshot/catalog/security/grant identities
// and atomically charges one bounded work class.  Call with liveness_only at
// open/close/publication boundaries.
EngineNarrowQueryBindingLivenessResultV1
ObserveNarrowQueryBindingLivenessV1(
    EngineNarrowQueryBindingAuthorityHandleV1* handle,
    const EngineRequestContext& context,
    EngineNarrowQueryWorkClassV1 work_class,
    std::uint32_t source_ordinal,
    std::uint64_t amount);

// No-allocation publication barrier for the paired cumulative-result and
// per-batch row charges.  Every refusal leaves both counters unchanged.
EngineNarrowQueryPublicationChargeStatusV1
CommitNarrowQueryPublicationChargeV1(
    EngineNarrowQueryBindingAuthorityHandleV1* handle,
    const EngineRequestContext& context,
    std::uint64_t result_rows,
    std::uint64_t batch_rows) noexcept;

struct EngineNarrowQuerySourceOccurrenceAuthorityResultV1 {
  bool ok = false;
  bool stale = false;
  EngineApiDiagnostic diagnostic;
};

// Rebuilds the exact public relation projection from the storage provider's
// current descriptor and byte-compares its retained identity/hash authority.
// This operation is read-only: it never refreshes, synthesizes, consumes, or
// otherwise mutates the binding handle.
EngineNarrowQuerySourceOccurrenceAuthorityResultV1
RevalidateNarrowQuerySourceOccurrenceAuthorityV1(
    const EngineNarrowQueryBindingAuthorityHandleV1& handle,
    const EngineRequestContext& context,
    std::uint32_t source_ordinal,
    const MgaRelationStorageDescriptor& current_descriptor);

EngineApiDiagnostic ReleaseNarrowQueryBindingAuthorityV1(
    EngineNarrowQueryBindingAuthorityHandleV1* handle);

// Session/receipt loss revokes an issued but unpublished binding without
// decoding any parser bytes.  Missing and already-released receipts are
// intentionally non-disclosing.
void RevokeNarrowQueryBindingAuthorityForReceiptV1(
    const std::string& statement_receipt_uuid);

}  // namespace scratchbird::engine::internal_api
