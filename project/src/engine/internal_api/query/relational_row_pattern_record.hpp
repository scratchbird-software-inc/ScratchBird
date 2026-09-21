// Copyright (c) 2026 ScratchBird Software Inc.
// SPDX-License-Identifier: MPL-2.0
#pragma once

#include "relational_window_definition_record.hpp"

namespace scratchbird::engine::internal_api {

enum class RelationalRowPatternRowsPerMatch : std::uint8_t {
  kOne = 1,
  kAll,
};

enum class RelationalRowPatternAfterMatchSkip : std::uint8_t {
  kPastLastRow = 1,
  kToNextRow,
  kToFirstVariable,
  kToLastVariable,
};

struct RelationalRowPatternVariableRecord {
  std::string canonical_name_key;
  std::uint32_t minimum_occurrences{1};
  std::optional<std::uint32_t> maximum_occurrences;
  bool reluctant{false};
  std::optional<std::uint32_t> define_expression_id;
  bool define_always_true{false};
};

struct RelationalRowPatternRecord {
  std::uint32_t pattern_id{0};
  std::uint32_t relation_node_id{0};
  std::vector<std::uint32_t> partition_expression_ids;
  std::vector<RelationalPropertyOrderingTerm> ordering_terms;
  std::vector<RelationalRowPatternVariableRecord> variables;
  std::vector<std::uint32_t> measure_expression_ids;
  RelationalRowPatternRowsPerMatch rows_per_match{
      RelationalRowPatternRowsPerMatch::kOne};
  RelationalRowPatternAfterMatchSkip after_match_skip{
      RelationalRowPatternAfterMatchSkip::kToNextRow};
  std::optional<std::string> skip_target_key;
  std::uint32_t maximum_partition_rows{0};
  std::uint32_t maximum_active_states{0};
  std::uint32_t maximum_output_rows{0};
  bool stable_row_identity_tie_break_allowed{false};
};

}  // namespace scratchbird::engine::internal_api
