// Copyright (c) 2026 ScratchBird Software Inc.
//
// This Source Code Form is subject to the terms of the Mozilla Public
// License, v. 2.0. If a copy of the MPL was not distributed with this
// file, You can obtain one at https://mozilla.org/MPL/2.0/.
//
// SPDX-License-Identifier: MPL-2.0

#pragma once

#include "api_types.hpp"
#include "scratchbird/engine/engine.h"

#include <cstdint>
#include <array>
#include <string>
#include <string_view>
#include <vector>

namespace scratchbird::server_engine_bridge {

// Private server-to-engine handle. The monotonically issued value is opaque to
// the server, is never reused, and identifies engine-owned receipt state. It is
// not part of the public C ABI and must never be encoded into SBLR.
struct StatementContextReceiptHandle {
  std::uint64_t opaque_id = 0;

  [[nodiscard]] explicit operator bool() const { return opaque_id != 0; }
  friend bool operator==(const StatementContextReceiptHandle&,
                         const StatementContextReceiptHandle&) = default;
};

enum class StatementDescriptorProfileKind : std::uint8_t {
  kNumericNonNull = 1,
  kNumericNullable = 2,
  kTextNonNull = 3,
  kTextNullable = 4,
  kBooleanNonNull = 5,
  kBooleanNullable = 6,
};

struct StatementDescriptorProfile {
  StatementDescriptorProfileKind profile_kind =
      StatementDescriptorProfileKind::kNumericNonNull;
  std::uint16_t slot = 0;
  std::string descriptor_uuid;
  std::string type_uuid;
  std::string collation_uuid;
  bool nullable = false;
  std::uint32_t width = 0;
  std::uint32_t precision = 0;
  std::uint32_t scale = 0;
};

struct StatementAggregateFunctionProfile {
  std::uint16_t abi_version = 0;
  std::string builtin_id;
  std::string function_uuid;
  bool executable = false;
};

// Exact engine-issued view returned at acquisition. Later Packet 7 stages may
// project the bounded parser fields from this value, but the opaque receipt
// remains the authority presented back to the engine.
struct StatementContextReceiptView {
  std::string receipt_uuid;
  std::string statement_uuid;
  std::string owning_transaction_uuid;
  std::string statement_snapshot_uuid;
  std::string statement_metadata_snapshot_uuid;
  std::string catalog_epoch_uuid;
  std::string security_context_uuid;
  std::string optimizer_capability_snapshot_uuid;
  std::string optimizer_resource_snapshot_uuid;
  std::string optimizer_route_snapshot_uuid;
  std::string bound_ast_uuid;
  std::string count_function_uuid;
  std::string sum_function_uuid;
  std::string avg_function_uuid;
  std::string min_function_uuid;
  std::string max_function_uuid;
  std::vector<StatementAggregateFunctionProfile> aggregate_function_profiles;
  std::vector<StatementDescriptorProfile> descriptor_profiles;

  std::uint64_t owning_local_transaction_id = 0;
  std::uint64_t visible_committed_high_watermark = 0;
  std::uint64_t oldest_active_local_transaction_id = 0;
  std::uint64_t oldest_interesting_local_transaction_id = 0;
  std::uint64_t oldest_snapshot_local_transaction_id = 0;
  std::uint64_t retention_horizon_local_transaction_id = 0;
  std::uint64_t publication_inventory_next_local_transaction_id = 0;
  std::vector<std::uint64_t> active_excluded_local_transaction_ids;
  std::vector<std::uint64_t> in_doubt_excluded_local_transaction_ids;
  bool inventory_authoritative = false;
  bool snapshot_complete = false;

  std::uint64_t catalog_generation_id = 0;
  std::uint64_t security_epoch = 0;
  std::uint64_t resource_epoch = 0;
  std::uint64_t optimizer_route_epoch = 0;
  std::uint64_t optimizer_route_generation = 0;
  std::uint64_t optimizer_memory_budget_bytes = 0;
  std::uint64_t optimizer_maximum_candidate_count = 0;
  std::uint64_t optimizer_maximum_memo_groups = 0;
  std::uint64_t optimizer_maximum_search_steps = 0;
  std::uint64_t optimizer_maximum_planning_time_ns = 0;
  bool optimizer_spill_allowed = false;
};

struct StatementContextAcquireRequest {
  // Copied immediately by the engine. This private internal context preserves
  // the complete materialized authorization, resource, optimizer, and route
  // state without narrowing it through sb_engine_request_context_v1_t.
  const scratchbird::engine::internal_api::EngineRequestContext*
      engine_context = nullptr;
  std::string_view exact_transaction_uuid;
};

// Private immutable dispatch receipt. The server supplies bytes that already
// passed canonical admission plus the admission digests; the engine re-decodes
// and re-hashes all three layers and binds them to the still-live receipt.
// No form of this request is exposed through the public C ABI.
struct StatementContextDispatchRequest {
  StatementContextReceiptHandle receipt;
  sb_engine_session_t engine_session = nullptr;
  std::vector<std::uint8_t> canonical_container_bytes;
  std::vector<std::uint8_t> canonical_execution_envelope_bytes;
  std::vector<std::uint8_t> canonical_operation_bytes;
  std::array<std::uint8_t, 32> container_sha256{};
  std::array<std::uint8_t, 32> execution_envelope_sha256{};
  std::array<std::uint8_t, 32> operation_sha256{};
  std::array<std::uint8_t, 32> admission_binding_sha256{};
  std::string authenticated_principal_uuid;
  std::string catalog_snapshot_uuid;
  std::string engine_mga_statement_uuid;
  std::string engine_mga_snapshot_uuid;
  std::uint64_t catalog_epoch = 0;
  std::uint64_t security_epoch = 0;
  std::uint64_t resource_epoch = 0;
  std::vector<std::uint8_t> data_packet;
};

// Selects the exact active durable-inventory transaction and publishes one
// statement-stable snapshot. No caller-supplied UUID or numeric high-water is
// accepted as statement authority.
sb_engine_status_t AcquireStatementContextReceipt(
    sb_engine_session_t session,
    const StatementContextAcquireRequest* request,
    StatementContextReceiptHandle* out_receipt,
    StatementContextReceiptView* out_view,
    sb_engine_result_t* out_result);

// Releases the engine-owned receipt exactly once and revokes its published
// statement snapshot. Repeated release returns ALREADY_RELEASED.
sb_engine_status_t ReleaseStatementContextReceipt(
    StatementContextReceiptHandle receipt);

// Revalidates a server admission token against current MGA receipt authority,
// materializes canonical typed operand bodies, and dispatches query.execute.
sb_engine_status_t DispatchStatementContextReceipt(
    const StatementContextDispatchRequest* request,
    sb_engine_result_t* out_result);

}  // namespace scratchbird::server_engine_bridge
