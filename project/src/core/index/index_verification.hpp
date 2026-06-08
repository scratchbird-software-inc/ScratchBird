// Copyright (c) 2026 ScratchBird Software Inc.
//
// This Source Code Form is subject to the terms of the Mozilla Public
// License, v. 2.0. If a copy of the MPL was not distributed with this
// file, You can obtain one at https://mozilla.org/MPL/2.0/.
//
// SPDX-License-Identifier: MPL-2.0

#pragma once

// SB-INDEX-VERIFICATION-CLOSURE-ANCHOR

#include "index_access_method.hpp"

namespace scratchbird::core::index {

using scratchbird::core::platform::u64;

struct IndexVerificationRequest {
  std::vector<IndexCandidate> expected_from_table;
  std::vector<IndexCandidate> observed_from_index;
  bool require_exact_order = false;
};

struct IndexVerificationResult {
  Status status;
  u64 expected_count = 0;
  u64 observed_count = 0;
  u64 missing_count = 0;
  u64 extra_count = 0;
  u64 order_mismatch_count = 0;
  bool rebuild_required = false;
  DiagnosticRecord diagnostic;

  bool ok() const { return status.ok() && missing_count == 0 && extra_count == 0 && order_mismatch_count == 0; }
};

IndexVerificationResult VerifyIndexCandidateSet(const IndexVerificationRequest& request);
DiagnosticRecord MakeIndexVerificationDiagnostic(Status status,
                                                 std::string diagnostic_code,
                                                 std::string message_key,
                                                 std::string detail = {});

}  // namespace scratchbird::core::index
