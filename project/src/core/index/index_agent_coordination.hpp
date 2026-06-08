// Copyright (c) 2026 ScratchBird Software Inc.
//
// This Source Code Form is subject to the terms of the Mozilla Public
// License, v. 2.0. If a copy of the MPL was not distributed with this
// file, You can obtain one at https://mozilla.org/MPL/2.0/.
//
// SPDX-License-Identifier: MPL-2.0

#pragma once

// SB-INDEX-AGENT-COORDINATION-CLOSURE-ANCHOR

#include "index_metrics.hpp"
#include "index_residency.hpp"

namespace scratchbird::core::index {

enum class IndexAgentRecommendationKind : u32 {
  observe = 1,
  verify = 2,
  rebuild = 3,
  rebalance = 4,
  warm = 5,
  cool = 6,
  request_pages = 7,
  request_filespace = 8
};

struct IndexAgentObservation {
  TypedUuid index_uuid;
  IndexFamily family = IndexFamily::unknown;
  double fragmentation_ratio = 0;
  double read_amplification_ratio = 0;
  double stale_resource_count = 0;
  double resident_bytes = 0;
  double memory_pressure_score = 0;
  bool policy_allows_action = false;
};

struct IndexAgentRecommendation {
  Status status;
  bool admitted = false;
  IndexAgentRecommendationKind kind = IndexAgentRecommendationKind::observe;
  std::string owner_agent;
  std::string explanation;
  DiagnosticRecord diagnostic;

  bool ok() const { return status.ok() && admitted; }
};

IndexAgentRecommendation RecommendIndexHealthAction(const IndexAgentObservation& observation);
IndexResidencyDecision RecommendIndexResidencyAction(const IndexAgentObservation& observation,
                                                     const IndexResidencyRequest& request);
DiagnosticRecord MakeIndexAgentDiagnostic(Status status,
                                          std::string diagnostic_code,
                                          std::string message_key,
                                          std::string detail = {});

}  // namespace scratchbird::core::index
