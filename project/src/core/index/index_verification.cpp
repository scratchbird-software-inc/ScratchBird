// Copyright (c) 2026 ScratchBird Software Inc.
//
// This Source Code Form is subject to the terms of the Mozilla Public
// License, v. 2.0. If a copy of the MPL was not distributed with this
// file, You can obtain one at https://mozilla.org/MPL/2.0/.
//
// SPDX-License-Identifier: MPL-2.0

#include "index_verification.hpp"

#include <algorithm>
#include <string>
#include <utility>

namespace scratchbird::core::index {
namespace {
using scratchbird::core::platform::DiagnosticArgument;
using scratchbird::core::platform::MakeDiagnostic;
using scratchbird::core::platform::Severity;
using scratchbird::core::platform::StatusCode;
using scratchbird::core::platform::Subsystem;

Status OkStatus() { return {StatusCode::ok, Severity::info, Subsystem::engine}; }
Status ErrorStatus() { return {StatusCode::platform_required_feature_missing, Severity::error, Subsystem::engine}; }

std::string CandidateKey(const IndexCandidate& candidate) {
  std::string key = candidate.key.encoded_key;
  key.push_back('|');
  for (scratchbird::core::platform::byte value : candidate.locator.row_uuid.value.bytes) {
    key.push_back(static_cast<char>(value));
  }
  return key;
}
}  // namespace

IndexVerificationResult VerifyIndexCandidateSet(const IndexVerificationRequest& request) {
  IndexVerificationResult result;
  result.expected_count = request.expected_from_table.size();
  result.observed_count = request.observed_from_index.size();
  std::vector<std::string> expected;
  std::vector<std::string> observed;
  expected.reserve(request.expected_from_table.size());
  observed.reserve(request.observed_from_index.size());
  for (const auto& candidate : request.expected_from_table) {
    expected.push_back(CandidateKey(candidate));
  }
  for (const auto& candidate : request.observed_from_index) {
    observed.push_back(CandidateKey(candidate));
  }
  if (request.require_exact_order && expected.size() == observed.size()) {
    for (std::size_t i = 0; i < expected.size(); ++i) {
      if (expected[i] != observed[i]) {
        result.order_mismatch_count++;
      }
    }
  }
  std::sort(expected.begin(), expected.end());
  std::sort(observed.begin(), observed.end());
  std::vector<std::string> missing;
  std::vector<std::string> extra;
  std::set_difference(expected.begin(), expected.end(), observed.begin(), observed.end(), std::back_inserter(missing));
  std::set_difference(observed.begin(), observed.end(), expected.begin(), expected.end(), std::back_inserter(extra));
  result.missing_count = missing.size();
  result.extra_count = extra.size();
  result.rebuild_required = result.missing_count != 0 || result.extra_count != 0 || result.order_mismatch_count != 0;
  result.status = result.rebuild_required ? ErrorStatus() : OkStatus();
  if (result.rebuild_required) {
    result.diagnostic = MakeIndexVerificationDiagnostic(result.status,
                                                        "SB-INDEX-VERIFY-MISMATCH",
                                                        "index.verify.mismatch",
                                                        std::to_string(result.missing_count) + "/" + std::to_string(result.extra_count));
  }
  return result;
}

DiagnosticRecord MakeIndexVerificationDiagnostic(Status status,
                                                 std::string diagnostic_code,
                                                 std::string message_key,
                                                 std::string detail) {
  std::vector<DiagnosticArgument> arguments;
  if (!detail.empty()) {
    arguments.push_back({"detail", std::move(detail)});
  }
  return MakeDiagnostic(status.code, status.severity, status.subsystem,
                        std::move(diagnostic_code), std::move(message_key),
                        std::move(arguments), {}, "core.index.verification");
}

}  // namespace scratchbird::core::index
