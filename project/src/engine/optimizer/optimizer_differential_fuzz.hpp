// Copyright (c) 2026 ScratchBird Software Inc.
//
// This Source Code Form is subject to the terms of the Mozilla Public
// License, v. 2.0. If a copy of the MPL was not distributed with this
// file, You can obtain one at https://mozilla.org/MPL/2.0/.
//
// SPDX-License-Identifier: MPL-2.0

#pragma once

#include <cstddef>
#include <string>
#include <vector>

namespace scratchbird::engine::optimizer {

// SEARCH_KEY: SB_OPTIMIZER_DIFFERENTIAL_PROPERTY_FUZZ_GATE_ODF_027
enum class OptimizerDifferentialCaseClass {
  kPredicateEquivalence,
  kJoinOrderBarrier,
  kRewriteProof,
  kPlanCacheShape,
  kAccessPathMetadata,
};

enum class OptimizerDifferentialOutcome {
  kAcceptedEquivalent,
  kExactRefusalEquivalent,
  kMismatch,
};

struct OptimizerDifferentialCase {
  std::string case_id;
  OptimizerDifferentialCaseClass case_class =
      OptimizerDifferentialCaseClass::kPredicateEquivalence;
  std::string summary;
};

struct OptimizerRouteEvidence {
  bool accepted = false;
  std::string canonical_semantic_digest;
  std::string result_class;
  std::string exact_refusal_diagnostic;
  std::vector<std::string> evidence;
};

struct OptimizerDifferentialCaseResult {
  OptimizerDifferentialCase test_case;
  OptimizerRouteEvidence baseline;
  OptimizerRouteEvidence optimized;
  OptimizerDifferentialOutcome outcome = OptimizerDifferentialOutcome::kMismatch;
  std::string mismatch_reason;
};

struct OptimizerDifferentialFuzzReport {
  std::vector<OptimizerDifferentialCaseResult> results;
  std::size_t accepted_equivalent_count = 0;
  std::size_t exact_refusal_equivalent_count = 0;
  std::size_t mismatch_count = 0;
};

std::vector<OptimizerDifferentialCase> GenerateOptimizerDifferentialFuzzCorpus();
OptimizerDifferentialCaseResult RunOptimizerDifferentialFuzzCase(
    const OptimizerDifferentialCase& test_case);
OptimizerDifferentialFuzzReport RunOptimizerDifferentialFuzzCorpus(
    const std::vector<OptimizerDifferentialCase>& corpus =
        GenerateOptimizerDifferentialFuzzCorpus());
std::string SerializeOptimizerDifferentialEvidence(
    const OptimizerDifferentialFuzzReport& report);
const char* OptimizerDifferentialCaseClassName(
    OptimizerDifferentialCaseClass case_class);
const char* OptimizerDifferentialOutcomeName(
    OptimizerDifferentialOutcome outcome);

}  // namespace scratchbird::engine::optimizer
