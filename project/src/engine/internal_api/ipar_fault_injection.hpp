// Copyright (c) 2026 ScratchBird Software Inc.
//
// This Source Code Form is subject to the terms of the Mozilla Public
// License, v. 2.0. If a copy of the MPL was not distributed with this
// file, You can obtain one at https://mozilla.org/MPL/2.0/.
//
// SPDX-License-Identifier: MPL-2.0

#pragma once

#include "api_diagnostics.hpp"
#include "api_types.hpp"

#include <algorithm>
#include <cstdint>
#include <string>
#include <string_view>
#include <vector>

namespace scratchbird::engine::internal_api {

inline constexpr const char* kIparP706Authority = "durable_mga_transaction_inventory";
inline constexpr const char* kIparP706TrackerId = "IPAR-P7-06";
inline constexpr const char* kIparFaultPointKey = "ipar.fault_injection.point";

inline std::string IparFaultDiagnosticCode(std::string_view point) {
  if (point == "row_append") {
    return "SB-IPAR-P7-06-ROW-APPEND-INJECTED";
  }
  if (point == "index_append") {
    return "SB-IPAR-P7-06-INDEX-APPEND-INJECTED";
  }
  if (point == "copy_batch") {
    return "SB-IPAR-P7-06-COPY-BATCH-INJECTED";
  }
  if (point == "secondary_merge") {
    return "SB-IPAR-P7-06-SECONDARY-MERGE-INJECTED";
  }
  if (point == "disk_preallocation") {
    return "SB-IPAR-P7-06-DISK-PREALLOCATION-INJECTED";
  }
  if (point == "commit_fence") {
    return "SB-IPAR-P7-06-COMMIT-FENCE-INJECTED";
  }
  return "SB-IPAR-P7-06-FAULT-INJECTED";
}

inline std::string IparFaultOptionValue(const std::vector<std::string>& options,
                                        std::string_view key) {
  const std::string equals_prefix = std::string(key) + "=";
  const std::string colon_prefix = std::string(key) + ":";
  for (const auto& option : options) {
    if (option.rfind(equals_prefix, 0) == 0) {
      return option.substr(equals_prefix.size());
    }
    if (option.rfind(colon_prefix, 0) == 0) {
      return option.substr(colon_prefix.size());
    }
  }
  return {};
}

inline bool IparFaultPointRequested(const std::vector<std::string>& options,
                                    std::string_view point) {
  const std::string requested = IparFaultOptionValue(options, kIparFaultPointKey);
  if (requested != point) {
    return false;
  }
  const std::string enabled = IparFaultOptionValue(options, "ipar.fault_injection.enabled");
  return enabled.empty() || enabled == "true" || enabled == "1" || enabled == "enabled";
}

inline std::uint64_t IparFaultOptionU64(const std::vector<std::string>& options,
                                        std::string_view key,
                                        std::uint64_t fallback) {
  const std::string value = IparFaultOptionValue(options, key);
  if (value.empty() || !std::all_of(value.begin(), value.end(), [](unsigned char c) {
        return c >= '0' && c <= '9';
      })) {
    return fallback;
  }
  std::uint64_t parsed = 0;
  for (char c : value) {
    parsed = parsed * 10 + static_cast<std::uint64_t>(c - '0');
  }
  return parsed;
}

inline EngineApiDiagnostic IparFaultDiagnostic(std::string_view operation,
                                               std::string_view point,
                                               std::string detail) {
  if (!detail.empty()) {
    detail += ";";
  }
  detail += "authority=";
  detail += kIparP706Authority;
  return MakeEngineApiDiagnostic(IparFaultDiagnosticCode(point),
                                 "ipar.p7_06.fault_injection." + std::string(point),
                                 std::string(operation) + ":" + detail,
                                 true);
}

inline void AppendIparFaultEvidence(std::vector<EngineEvidenceReference>* evidence,
                                    std::string_view point,
                                    std::string_view recovery_action) {
  if (evidence == nullptr) {
    return;
  }
  evidence->push_back({"ipar_tracker_id", kIparP706TrackerId});
  evidence->push_back({"ipar_fault_injection_point", std::string(point)});
  evidence->push_back({"ipar_fault_injection_diagnostic",
                       IparFaultDiagnosticCode(point)});
  evidence->push_back({"ipar_fault_injection_authority", kIparP706Authority});
  evidence->push_back({"ipar_fault_injection_recovery_action",
                       std::string(recovery_action)});
  evidence->push_back({"ipar_fault_injection_parser_finality", "false"});
  evidence->push_back({"ipar_fault_injection_wal_authority", "false"});
}

}  // namespace scratchbird::engine::internal_api
