// Copyright (c) 2026 ScratchBird Software Inc.
//
// This Source Code Form is subject to the terms of the Mozilla Public
// License, v. 2.0. If a copy of the MPL was not distributed with this
// file, You can obtain one at https://mozilla.org/MPL/2.0/.
//
// SPDX-License-Identifier: MPL-2.0

#pragma once

#include "cache/sblr_template_cache.hpp"
#include "common/common.hpp"

#include <chrono>
#include <mutex>
#include <string>
#include <unordered_map>

namespace scratchbird::parser::sbsql {

class ParserMetrics {
 public:
  ParserMetrics();
  void Increment(std::string name, std::uint64_t by = 1);
  void SetGauge(std::string name, double value);
  void SetState(ParserState state);
  [[nodiscard]] ParserState State() const;
  [[nodiscard]] std::string SnapshotJson(const ParserConfig& config,
                                         const SessionContext& session,
                                         const SblrTemplateCache& cache) const;
  [[nodiscard]] std::string HeartbeatJson(const ParserConfig& config,
                                          const SessionContext& session,
                                          const SblrTemplateCache& cache,
                                          std::string_view current_operation) const;

 private:
  std::chrono::steady_clock::time_point start_;
  mutable std::mutex mutex_;
  ParserState state_{ParserState::kSpawned};
  std::unordered_map<std::string, std::uint64_t> counters_;
  std::unordered_map<std::string, double> gauges_;
};

} // namespace scratchbird::parser::sbsql
