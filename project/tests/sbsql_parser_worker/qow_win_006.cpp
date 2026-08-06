// Copyright (c) 2026 ScratchBird Software Inc.
//
// This Source Code Form is subject to the terms of the Mozilla Public
// License, v. 2.0. If a copy of the MPL was not distributed with this
// file, You can obtain one at https://mozilla.org/MPL/2.0/.
//
// SPDX-License-Identifier: MPL-2.0

#define QOW_WIN_003_FIXTURE_ONLY
#include "qow_win_003.cpp"

#include "executor_foundation.hpp"

#include <cmath>
#include <limits>

namespace {

std::string RankingUuid(const exec::CanonicalWindowRankingFunction function) {
  switch (function) {
    case exec::CanonicalWindowRankingFunction::row_number:
      return "019de5fc-2400-7539-bcce-00eef3ae7220";
    case exec::CanonicalWindowRankingFunction::rank:
      return "019de5fc-2400-7b94-870d-0dd789ca70ab";
    case exec::CanonicalWindowRankingFunction::dense_rank:
      return "019de5fc-2400-741d-bef0-f079fd3ba494";
    case exec::CanonicalWindowRankingFunction::percent_rank:
      return "019de5fc-2400-7d86-86fe-96f3f27b5dd6";
    case exec::CanonicalWindowRankingFunction::cume_dist:
      return "019de5fc-2400-721c-be64-2568b64a02b9";
    case exec::CanonicalWindowRankingFunction::ntile:
      return "019de5fc-2400-7047-9474-232ca488c094";
  }
  return {};
}

exec::CanonicalWindowRankingRequest RankingRequest(
    const exec::CanonicalWindowRankingFunction function,
    const exec::CanonicalWindowFrameExclusion exclusion =
        exec::CanonicalWindowFrameExclusion::no_others) {
  exec::CanonicalWindowRankingRequest request;
  request.frames = ExecuteFrame(
      Window401Request(),
      ExplicitFrame(
          exec::CanonicalWindowFrameUnit::rows,
          FrameBound(exec::CanonicalWindowFrameBoundKind::current_row),
          FrameBound(exec::CanonicalWindowFrameBoundKind::current_row),
          exclusion));
  request.function = function;
  request.function_uuid = RankingUuid(function);
  const bool real =
      function == exec::CanonicalWindowRankingFunction::percent_rank ||
      function == exec::CanonicalWindowRankingFunction::cume_dist;
  request.output_descriptor = WindowDescriptor(
      real ? 4961 : 4960, real ? "real64" : "int64",
      "type_uuid=" + WindowUuid(real ? 4963 : 4962) +
          ";nullability=non_null");
  if (function == exec::CanonicalWindowRankingFunction::ntile) {
    request.ntile_bucket_count = exec::EncodeInt64Value(3);
  }
  return request;
}

std::vector<std::int64_t> IntegerValues(
    const exec::CanonicalWindowRankingResult& result) {
  std::vector<std::int64_t> values;
  for (const auto& value : result.values) {
    const auto decoded = exec::DecodeInt64Value(value);
    values.push_back(decoded.ok() ? decoded.value
                                  : std::numeric_limits<std::int64_t>::min());
  }
  return values;
}

std::vector<double> RealValues(
    const exec::CanonicalWindowRankingResult& result) {
  std::vector<double> values;
  for (const auto& value : result.values) {
    const auto decoded = exec::DecodeReal64Value(value);
    values.push_back(decoded.ok()
                         ? decoded.value
                         : std::numeric_limits<double>::quiet_NaN());
  }
  return values;
}

bool RealSequenceNear(const std::vector<double>& actual,
                      const std::vector<double>& expected) {
  if (actual.size() != expected.size()) return false;
  for (std::size_t index = 0; index < actual.size(); ++index) {
    if (std::isnan(actual[index]) ||
        std::abs(actual[index] - expected[index]) > 1e-12) {
      return false;
    }
  }
  return true;
}

bool ValidateRankingAndDistribution() {
  bool passed = true;
  const auto row_number = exec::ExecuteCanonicalWindowRanking(
      RankingRequest(exec::CanonicalWindowRankingFunction::row_number));
  const auto rank = exec::ExecuteCanonicalWindowRanking(
      RankingRequest(exec::CanonicalWindowRankingFunction::rank));
  const auto dense = exec::ExecuteCanonicalWindowRanking(
      RankingRequest(exec::CanonicalWindowRankingFunction::dense_rank));
  const auto percent = exec::ExecuteCanonicalWindowRanking(
      RankingRequest(exec::CanonicalWindowRankingFunction::percent_rank));
  const auto cume = exec::ExecuteCanonicalWindowRanking(
      RankingRequest(exec::CanonicalWindowRankingFunction::cume_dist));

  passed &= Require401(
      row_number.diagnostic.ok && rank.diagnostic.ok && dense.diagnostic.ok &&
          percent.diagnostic.ok && cume.diagnostic.ok,
      "one or more canonical ranking functions refused");
  passed &= Require401(
      IntegerValues(row_number) ==
          std::vector<std::int64_t>({1, 2, 3, 4, 5, 1, 1, 2, 1}),
      "ROW_NUMBER did not restart at each typed partition");
  passed &= Require401(
      IntegerValues(rank) ==
          std::vector<std::int64_t>({1, 1, 3, 4, 5, 1, 1, 1, 1}),
      "RANK did not use the first row of each exact peer group");
  passed &= Require401(
      IntegerValues(dense) ==
          std::vector<std::int64_t>({1, 1, 2, 3, 4, 1, 1, 1, 1}),
      "DENSE_RANK did not count typed peer groups");
  passed &= Require401(
      RealSequenceNear(RealValues(percent),
                       {0.0, 0.0, 0.5, 0.75, 1.0, 0.0,
                        0.0, 0.0, 0.0}),
      "PERCENT_RANK partition denominator or singleton rule drifted");
  passed &= Require401(
      RealSequenceNear(RealValues(cume),
                       {0.4, 0.4, 0.6, 0.8, 1.0, 1.0,
                        1.0, 1.0, 1.0}),
      "CUME_DIST did not use peer-group end and partition cardinality");
  passed &= Require401(
      row_number.frame_and_exclusion_validated_then_ignored &&
          rank.frame_and_exclusion_validated_then_ignored &&
          dense.frame_and_exclusion_validated_then_ignored &&
          percent.frame_and_exclusion_validated_then_ignored &&
          cume.frame_and_exclusion_validated_then_ignored &&
          rank.selected_plan_uuid == WindowUuid(4301) &&
          rank.causal_counter_id == 40102 &&
          rank.authority.engine_mga_snapshot_bound,
      "frame-class or optimizer/MGA ranking evidence was not retained");

  auto excluded = RankingRequest(
      exec::CanonicalWindowRankingFunction::rank,
      exec::CanonicalWindowFrameExclusion::group);
  const auto excluded_rank =
      exec::ExecuteCanonicalWindowRanking(excluded);
  passed &= Require401(
      excluded_rank.diagnostic.ok &&
          IntegerValues(excluded_rank) == IntegerValues(rank) &&
          excluded.frames.effective_frames[0].effective_row_indices.empty(),
      "RANK used the frame after validating its exclusion");
  return passed;
}

bool ValidateNtileBoundaries() {
  bool passed = true;
  auto request = RankingRequest(exec::CanonicalWindowRankingFunction::ntile);
  auto result = exec::ExecuteCanonicalWindowRanking(request);
  passed &= Require401(
      result.diagnostic.ok &&
          IntegerValues(result) ==
              std::vector<std::int64_t>({1, 1, 2, 2, 3, 1, 1, 2, 1}),
      "NTILE(3) did not assign larger buckets first per partition");

  request.ntile_bucket_count = exec::EncodeInt64Value(8);
  result = exec::ExecuteCanonicalWindowRanking(request);
  passed &= Require401(
      result.diagnostic.ok &&
          IntegerValues(result) ==
              std::vector<std::int64_t>({1, 2, 3, 4, 5, 1, 1, 2, 1}),
      "NTILE with more buckets than rows skipped or duplicated a bucket");

  request.ntile_bucket_count = exec::EncodeInt64Value(1);
  result = exec::ExecuteCanonicalWindowRanking(request);
  passed &= Require401(
      result.diagnostic.ok &&
          IntegerValues(result) ==
              std::vector<std::int64_t>({1, 1, 1, 1, 1, 1, 1, 1, 1}),
      "NTILE(1) did not retain every row in bucket one");

  auto one_partition = Window401Request();
  one_partition.partition_terms.clear();
  one_partition.partition_property_uuid.clear();
  one_partition.physical_dag.nodes[1].required_property_uuids.erase(
      one_partition.physical_dag.nodes[1].required_property_uuids.begin());
  request = RankingRequest(exec::CanonicalWindowRankingFunction::ntile);
  request.frames = ExecuteFrame(
      one_partition,
      ExplicitFrame(
          exec::CanonicalWindowFrameUnit::rows,
          FrameBound(exec::CanonicalWindowFrameBoundKind::unbounded_preceding),
          FrameBound(exec::CanonicalWindowFrameBoundKind::unbounded_following)));
  request.ntile_bucket_count = exec::EncodeInt64Value(4);
  result = exec::ExecuteCanonicalWindowRanking(request);
  passed &= Require401(
      result.diagnostic.ok &&
          IntegerValues(result) ==
              std::vector<std::int64_t>({1, 1, 1, 2, 2, 3, 3, 4, 4}),
      "NTILE quotient/remainder boundary for nine rows and four buckets drifted");
  return passed;
}

bool RankingRefused(const exec::CanonicalWindowRankingResult& result) {
  return !result.diagnostic.ok &&
         (result.diagnostic.diagnostic_code == "QOW-DIAG-WINDOW-RANKING" ||
          result.diagnostic.diagnostic_code == "QOW-DIAG-WINDOW-NTILE") &&
         result.values.empty();
}

bool ValidateRankingRefusals() {
  bool passed = true;
  auto request = RankingRequest(exec::CanonicalWindowRankingFunction::ntile);
  request.ntile_bucket_count.reset();
  passed &= Require401(
      RankingRefused(exec::ExecuteCanonicalWindowRanking(request)),
      "missing NTILE bucket operand was defaulted");

  request = RankingRequest(exec::CanonicalWindowRankingFunction::ntile);
  request.ntile_bucket_count = exec::EncodeInt64Value(0);
  passed &= Require401(
      RankingRefused(exec::ExecuteCanonicalWindowRanking(request)),
      "zero NTILE bucket count entered execution");

  request.ntile_bucket_count = exec::EncodeInt64Value(-1);
  passed &= Require401(
      RankingRefused(exec::ExecuteCanonicalWindowRanking(request)),
      "negative NTILE bucket count entered execution");

  auto null_bucket_count = exec::EncodeInt64Value(1);
  null_bucket_count.state =
      scratchbird::engine::internal_api::EngineValueState::sql_null;
  null_bucket_count.encoded_value.clear();
  request.ntile_bucket_count = null_bucket_count;
  passed &= Require401(
      RankingRefused(exec::ExecuteCanonicalWindowRanking(request)),
      "NULL NTILE bucket count entered execution");

  request = RankingRequest(exec::CanonicalWindowRankingFunction::rank);
  request.ntile_bucket_count = exec::EncodeInt64Value(1);
  passed &= Require401(
      RankingRefused(exec::ExecuteCanonicalWindowRanking(request)),
      "non-NTILE function ignored an NTILE operand");

  request = RankingRequest(exec::CanonicalWindowRankingFunction::rank);
  request.function_uuid = RankingUuid(
      exec::CanonicalWindowRankingFunction::dense_rank);
  passed &= Require401(
      RankingRefused(exec::ExecuteCanonicalWindowRanking(request)),
      "function UUID drift selected another ranking function");

  request = RankingRequest(exec::CanonicalWindowRankingFunction::rank);
  request.output_descriptor = WindowDescriptor(
      4970, "real64",
      "type_uuid=" + WindowUuid(4971) + ";nullability=non_null");
  passed &= Require401(
      RankingRefused(exec::ExecuteCanonicalWindowRanking(request)),
      "integer ranking function wrote a real result descriptor");

  request = RankingRequest(exec::CanonicalWindowRankingFunction::rank);
  request.frames.row_metadata[0].peer_begin = 1;
  passed &= Require401(
      RankingRefused(exec::ExecuteCanonicalWindowRanking(request)),
      "forged peer range entered ranking execution");

  request = RankingRequest(exec::CanonicalWindowRankingFunction::rank);
  request.frames.resolved_frame.unit =
      static_cast<exec::CanonicalWindowFrameUnit>(0xff);
  passed &= Require401(
      RankingRefused(exec::ExecuteCanonicalWindowRanking(request)),
      "forged frame-unit evidence entered ranking execution");

  request = RankingRequest(exec::CanonicalWindowRankingFunction::rank);
  request.frames.resolved_frame.exclusion =
      static_cast<exec::CanonicalWindowFrameExclusion>(0xff);
  passed &= Require401(
      RankingRefused(exec::ExecuteCanonicalWindowRanking(request)),
      "forged frame-exclusion evidence entered ranking execution");

  request = RankingRequest(exec::CanonicalWindowRankingFunction::rank);
  request.frames.effective_frames[0].effective_state =
      exec::CanonicalWindowFrameState::empty;
  passed &= Require401(
      RankingRefused(exec::ExecuteCanonicalWindowRanking(request)),
      "forged post-exclusion empty state entered ranking execution");

  request = RankingRequest(exec::CanonicalWindowRankingFunction::rank);
  request.frames.effective_frames[0].excluded_row_count = 1;
  passed &= Require401(
      RankingRefused(exec::ExecuteCanonicalWindowRanking(request)),
      "forged exclusion row count entered ranking execution");

  request = RankingRequest(exec::CanonicalWindowRankingFunction::rank);
  request.frames.effective_frames[0].exclusion_operand_consumed = false;
  passed &= Require401(
      RankingRefused(exec::ExecuteCanonicalWindowRanking(request)),
      "unconsumed exclusion operand entered ranking execution");

  request = RankingRequest(exec::CanonicalWindowRankingFunction::rank);
  request.frames.base_frame_constructed_before_exclusion = false;
  passed &= Require401(
      RankingRefused(exec::ExecuteCanonicalWindowRanking(request)),
      "missing base-before-exclusion evidence entered ranking execution");

  request = RankingRequest(exec::CanonicalWindowRankingFunction::rank);
  request.frames.defaulted_with_order = true;
  passed &= Require401(
      RankingRefused(exec::ExecuteCanonicalWindowRanking(request)),
      "forged default-frame classification entered ranking execution");

  request = RankingRequest(exec::CanonicalWindowRankingFunction::rank);
  request.parser_execution_authority_claimed = true;
  passed &= Require401(
      RankingRefused(exec::ExecuteCanonicalWindowRanking(request)),
      "parser authority claim entered ranking execution");

  request = RankingRequest(exec::CanonicalWindowRankingFunction::rank);
  request.maximum_output_rows = 1;
  passed &= Require401(
      RankingRefused(exec::ExecuteCanonicalWindowRanking(request)),
      "ranking resource overflow returned partial values");
  return passed;
}

}  // namespace

// QOW-TEST-WIN-006-V1
int main() {
  return ValidateRankingAndDistribution() && ValidateNtileBoundaries() &&
                 ValidateRankingRefusals()
             ? EXIT_SUCCESS
             : EXIT_FAILURE;
}
