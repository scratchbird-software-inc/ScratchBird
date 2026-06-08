// Copyright (c) 2026 ScratchBird Software Inc.
//
// This Source Code Form is subject to the terms of the Mozilla Public
// License, v. 2.0. If a copy of the MPL was not distributed with this
// file, You can obtain one at https://mozilla.org/MPL/2.0/.
//
// SPDX-License-Identifier: MPL-2.0

#pragma once

#include "api_types.hpp"

#include <cstdio>
#include <iostream>
#include <string>

namespace scratchbird::tools::metrics_history_probe {

inline bool Require(bool condition, const std::string& message) {
  if (!condition) {
    std::cerr << "FAIL: " << message << "\n";
    return false;
  }
  return true;
}

inline std::string TempHistoryPath(const std::string& name) {
  return "/tmp/" + name + ".sbmtrh";
}

inline void RemoveTempHistory(const std::string& path) {
  std::remove(path.c_str());
  std::remove((path + ".tmp").c_str());
}

inline scratchbird::engine::internal_api::EngineRequestContext MetricsContext(bool cluster = false,
                                                                              bool retention_control = false,
                                                                              bool sensitive = false) {
  scratchbird::engine::internal_api::EngineRequestContext context;
  context.security_context_present = true;
  context.cluster_authority_available = cluster;
  context.database_uuid.canonical = "018f0000-0000-7000-8000-000000000001";
  context.cluster_uuid.canonical = cluster ? "018f0000-0000-7000-8000-000000000002" : "";
  context.session_uuid.canonical = "018f0000-0000-7000-8000-000000000003";
  context.principal_uuid.canonical = "018f0000-0000-7000-8000-000000000004";
  context.transaction_uuid.canonical = "018f0000-0000-7000-8000-000000000005";
  context.trace_tags = {"right:OBS_METRICS_READ_FAMILY"};
  if (retention_control) {
    context.trace_tags.push_back("right:OBS_METRICS_RETENTION_CONTROL");
  }
  if (sensitive) {
    context.trace_tags.push_back("right:OBS_METRICS_READ_ALL");
  }
  return context;
}

inline bool ContainsValue(const scratchbird::engine::internal_api::EngineApiResult& result,
                          const std::string& value) {
  for (const auto& row : result.result_shape.rows) {
    for (const auto& field : row.fields) {
      if (field.second.encoded_value.find(value) != std::string::npos) {
        return true;
      }
    }
  }
  return false;
}

}  // namespace scratchbird::tools::metrics_history_probe
