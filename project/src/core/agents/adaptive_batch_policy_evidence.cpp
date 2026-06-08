// Copyright (c) 2026 ScratchBird Software Inc.
//
// This Source Code Form is subject to the terms of the Mozilla Public
// License, v. 2.0. If a copy of the MPL was not distributed with this
// file, You can obtain one at https://mozilla.org/MPL/2.0/.
//
// SPDX-License-Identifier: MPL-2.0

#include "adaptive_batch_policy_evidence.hpp"

#include <utility>

namespace scratchbird::core::agents {
namespace {

std::string BoolText(bool value) {
  return value ? "true" : "false";
}

void Add(std::vector<std::pair<std::string, std::string>>* fields,
         std::string key,
         std::string value) {
  fields->push_back({std::move(key), std::move(value)});
}

}  // namespace

AdaptiveBatchPolicyEvidence BuildAdaptiveBatchPolicyEvidence(
    const AdaptiveBatchPolicyEvidenceRequest& request) {
  AdaptiveBatchPolicyEvidence evidence;
  evidence.family_label = request.family_label;
  evidence.policy_id = request.policy_id;
  evidence.evidence_epoch = request.evidence_epoch;
  evidence.required_epoch = request.required_epoch;
  evidence.evidence_age_microseconds = request.evidence_age_microseconds;
  evidence.max_evidence_age_microseconds =
      request.max_evidence_age_microseconds;
  evidence.backlog_units = request.backlog_units;
  evidence.backlog_budget_units = request.backlog_budget_units;
  evidence.worker_pressure_ppm = request.worker_pressure_ppm;
  evidence.quota_pressure_ppm = request.quota_pressure_ppm;
  evidence.policy_allowed = request.policy_allowed;
  evidence.evidence_present = !request.family_label.empty();
  evidence.evidence_authoritative = request.evidence_authoritative;
  evidence.throttle_allowed = request.throttle_allowed;
  evidence.hard_backlog_pressure = request.hard_backlog_pressure;
  evidence.engine_mga_authoritative = request.engine_mga_authoritative;
  evidence.security_snapshot_bound = request.security_snapshot_bound;
  evidence.grants_proven = request.grants_proven;
  evidence.mga_recheck_required = request.mga_recheck_required;
  evidence.security_recheck_required = request.security_recheck_required;
  evidence.parser_or_donor_authority = request.parser_or_donor_authority;
  evidence.provider_transaction_finality_authority =
      request.provider_transaction_finality_authority;
  evidence.provider_visibility_authority =
      request.provider_visibility_authority;
  evidence.client_autocommit_authority = request.client_autocommit_authority;
  evidence.wal_recovery_authority = request.wal_recovery_authority;
  return evidence;
}

std::vector<std::pair<std::string, std::string>>
SerializeAdaptiveBatchPolicyEvidence(
    const AdaptiveBatchPolicyEvidence& evidence) {
  std::vector<std::pair<std::string, std::string>> fields;
  Add(&fields, "family_label", evidence.family_label);
  Add(&fields, "policy_id", evidence.policy_id);
  Add(&fields, "evidence_epoch", std::to_string(evidence.evidence_epoch));
  Add(&fields, "required_epoch", std::to_string(evidence.required_epoch));
  Add(&fields,
      "evidence_age_microseconds",
      std::to_string(evidence.evidence_age_microseconds));
  Add(&fields,
      "max_evidence_age_microseconds",
      std::to_string(evidence.max_evidence_age_microseconds));
  Add(&fields, "backlog_units", std::to_string(evidence.backlog_units));
  Add(&fields,
      "backlog_budget_units",
      std::to_string(evidence.backlog_budget_units));
  Add(&fields,
      "worker_pressure_ppm",
      std::to_string(evidence.worker_pressure_ppm));
  Add(&fields,
      "quota_pressure_ppm",
      std::to_string(evidence.quota_pressure_ppm));
  Add(&fields, "policy_allowed", BoolText(evidence.policy_allowed));
  Add(&fields, "evidence_present", BoolText(evidence.evidence_present));
  Add(&fields,
      "evidence_authoritative",
      BoolText(evidence.evidence_authoritative));
  Add(&fields, "throttle_allowed", BoolText(evidence.throttle_allowed));
  Add(&fields,
      "hard_backlog_pressure",
      BoolText(evidence.hard_backlog_pressure));
  Add(&fields,
      "engine_mga_authoritative",
      BoolText(evidence.engine_mga_authoritative));
  Add(&fields,
      "security_snapshot_bound",
      BoolText(evidence.security_snapshot_bound));
  Add(&fields, "grants_proven", BoolText(evidence.grants_proven));
  Add(&fields,
      "mga_recheck_required",
      BoolText(evidence.mga_recheck_required));
  Add(&fields,
      "security_recheck_required",
      BoolText(evidence.security_recheck_required));
  Add(&fields,
      "parser_or_donor_authority",
      BoolText(evidence.parser_or_donor_authority));
  Add(&fields,
      "provider_transaction_finality_authority",
      BoolText(evidence.provider_transaction_finality_authority));
  Add(&fields,
      "provider_visibility_authority",
      BoolText(evidence.provider_visibility_authority));
  Add(&fields,
      "client_autocommit_authority",
      BoolText(evidence.client_autocommit_authority));
  Add(&fields,
      "wal_recovery_authority",
      BoolText(evidence.wal_recovery_authority));
  return fields;
}

}  // namespace scratchbird::core::agents
