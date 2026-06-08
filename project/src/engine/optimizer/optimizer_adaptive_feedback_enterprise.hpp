// Copyright (c) 2026 ScratchBird Software Inc.
//
// This Source Code Form is subject to the terms of the Mozilla Public
// License, v. 2.0. If a copy of the MPL was not distributed with this
// file, You can obtain one at https://mozilla.org/MPL/2.0/.
//
// SPDX-License-Identifier: MPL-2.0

#pragma once

#include "adaptive_cardinality_feedback.hpp"

#include <cstdint>
#include <mutex>
#include <optional>
#include <string>
#include <vector>

namespace scratchbird::engine::optimizer {

// SEARCH_KEY: OEIC_ADAPTIVE_CARDINALITY_FEEDBACK_ENTERPRISE
// Enterprise adaptive feedback stores scoped advisory records with aging,
// invalidation, bind-profile evidence, and quarantine state. It is never row
// truth, finality, visibility, parser, donor, benchmark, or recovery authority.
struct EnterpriseAdaptiveFeedbackApplyRequest {
  std::string feedback_uuid;
  std::string scope_uuid;
  std::string bind_profile_digest;
  std::string predicate_digest;
  std::string metric_snapshot_digest;
  std::uint64_t feedback_generation = 0;
  std::uint64_t policy_generation = 0;
  std::uint64_t catalog_epoch = 0;
  std::uint64_t security_epoch = 0;
  std::uint64_t created_microseconds = 0;
  std::uint64_t expires_after_microseconds = 0;
  AdaptiveCardinalityFeedbackRequest adaptive_request;
};

struct EnterpriseAdaptiveFeedbackRecord {
  std::string feedback_uuid;
  std::string scope_uuid;
  std::string bind_profile_digest;
  std::string predicate_digest;
  std::string metric_snapshot_digest;
  std::string route_label;
  std::string baseline_plan_hash;
  std::string variant_plan_hash;
  std::uint64_t feedback_generation = 0;
  std::uint64_t policy_generation = 0;
  std::uint64_t catalog_epoch = 0;
  std::uint64_t security_epoch = 0;
  std::uint64_t created_microseconds = 0;
  std::uint64_t expires_after_microseconds = 0;
  bool valid = true;
  bool bind_sensitive_variant_created = false;
  bool misestimate_quarantined = false;
  bool extended_stat_requested = false;
  std::string invalidation_reason;
  AdaptiveCardinalityFeedbackResult adaptive_result;
  std::vector<std::string> evidence;
};

struct EnterpriseAdaptiveFeedbackInvalidation {
  std::string scope_uuid;
  std::uint64_t policy_generation = 0;
  std::uint64_t catalog_epoch = 0;
  std::uint64_t security_epoch = 0;
  std::string reason;
};

struct EnterpriseAdaptiveFeedbackSnapshot {
  std::uint64_t total_records = 0;
  std::uint64_t valid_records = 0;
  std::uint64_t invalidated_records = 0;
  std::uint64_t quarantined_records = 0;
  std::vector<EnterpriseAdaptiveFeedbackRecord> records;
};

class EnterpriseAdaptiveFeedbackStore {
 public:
  AdaptiveCardinalityFeedbackResult Record(
      EnterpriseAdaptiveFeedbackRecord record);
  std::uint64_t Invalidate(const EnterpriseAdaptiveFeedbackInvalidation& event);
  std::uint64_t Expire(std::uint64_t now_microseconds);
  EnterpriseAdaptiveFeedbackSnapshot Snapshot() const;
  std::optional<EnterpriseAdaptiveFeedbackRecord> Find(
      const std::string& feedback_uuid) const;

 private:
  mutable std::mutex mutex_;
  std::vector<EnterpriseAdaptiveFeedbackRecord> records_;
};

AdaptiveCardinalityFeedbackResult ApplyEnterpriseAdaptiveFeedback(
    const EnterpriseAdaptiveFeedbackApplyRequest& request,
    EnterpriseAdaptiveFeedbackStore* store);

}  // namespace scratchbird::engine::optimizer
