// Copyright (c) 2026 ScratchBird Software Inc.
//
// This Source Code Form is subject to the terms of the Mozilla Public
// License, v. 2.0. If a copy of the MPL was not distributed with this
// file, You can obtain one at https://mozilla.org/MPL/2.0/.
//
// SPDX-License-Identifier: MPL-2.0

#pragma once

#include "optimizer_cost_full.hpp"
#include "optimizer_memory_feedback_bridge.hpp"

#include <cstdint>
#include <mutex>
#include <optional>
#include <string>
#include <vector>

namespace scratchbird::engine::optimizer {

// SEARCH_KEY: OEIC_MEMORY_SPILL_FEEDBACK_ENTERPRISE
// Governed memory/spill feedback is optimizer-advisory evidence only. It can
// tune grants and spill costs, but it cannot own transaction finality,
// visibility, parser behavior, donor behavior, security, or recovery outcome.
struct EnterpriseMemorySpillFeedbackApplyRequest {
  std::string feedback_uuid;
  std::string reservation_id;
  std::string memory_snapshot_digest;
  std::string route_label;
  std::string plan_node_id;
  std::uint64_t policy_generation = 0;
  std::uint64_t feedback_generation = 0;
  std::uint64_t catalog_epoch = 0;
  std::uint64_t security_epoch = 0;
  std::uint64_t created_microseconds = 0;
  std::uint64_t expires_after_microseconds = 0;
  CostVector baseline_cost;
  OptimizerMemoryFeedbackEvidence evidence;
};

struct EnterpriseMemorySpillFeedbackRecord {
  std::string feedback_uuid;
  std::string reservation_id;
  std::string memory_snapshot_digest;
  std::string source_kind;
  std::string provenance_digest;
  std::string redaction_class;
  std::string redaction_digest;
  std::string metric_snapshot_digest;
  std::string support_snapshot_digest;
  std::string reservation_token;
  std::uint64_t reservation_generation = 0;
  std::string route_label;
  std::string plan_node_id;
  std::string query_uuid;
  std::string scope_uuid;
  std::uint64_t policy_generation = 0;
  std::uint64_t feedback_generation = 0;
  std::uint64_t catalog_epoch = 0;
  std::uint64_t security_epoch = 0;
  std::uint64_t redaction_epoch = 0;
  std::uint64_t statistics_epoch = 0;
  std::uint64_t created_microseconds = 0;
  std::uint64_t expires_after_microseconds = 0;
  bool valid = true;
  std::string invalidation_reason;
  OptimizerMemoryFeedbackBridgeResult bridge_result;
  OptimizerFeedbackStatus feedback_status;
  CostVector adjusted_cost;
  std::vector<std::string> evidence;
};

struct EnterpriseMemorySpillFeedbackInvalidation {
  std::string scope_uuid;
  std::uint64_t policy_generation = 0;
  std::uint64_t catalog_epoch = 0;
  std::uint64_t security_epoch = 0;
  std::string reason;
};

struct EnterpriseMemorySpillFeedbackSnapshot {
  std::uint64_t total_records = 0;
  std::uint64_t valid_records = 0;
  std::uint64_t invalidated_records = 0;
  std::uint64_t spill_records = 0;
  std::vector<EnterpriseMemorySpillFeedbackRecord> records;
};

struct EnterpriseMemorySpillFeedbackApplyResult {
  bool accepted = false;
  bool benchmark_clean = false;
  bool fail_closed = false;
  std::string diagnostic_code;
  OptimizerMemoryFeedbackBridgeResult bridge_result;
  OptimizerFeedbackStatus feedback_status;
  CostVector adjusted_cost;
  std::vector<std::string> evidence;
};

class EnterpriseMemorySpillFeedbackStore {
 public:
  EnterpriseMemorySpillFeedbackApplyResult Record(
      EnterpriseMemorySpillFeedbackRecord record);
  std::uint64_t Invalidate(const EnterpriseMemorySpillFeedbackInvalidation& event);
  std::uint64_t Expire(std::uint64_t now_microseconds);
  EnterpriseMemorySpillFeedbackSnapshot Snapshot() const;
  std::optional<EnterpriseMemorySpillFeedbackRecord> Find(
      const std::string& feedback_uuid) const;

 private:
  mutable std::mutex mutex_;
  std::vector<EnterpriseMemorySpillFeedbackRecord> records_;
};

EnterpriseMemorySpillFeedbackApplyResult ApplyEnterpriseMemorySpillFeedback(
    const EnterpriseMemorySpillFeedbackApplyRequest& request,
    EnterpriseMemorySpillFeedbackStore* store);

}  // namespace scratchbird::engine::optimizer
