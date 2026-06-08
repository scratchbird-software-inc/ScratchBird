// Copyright (c) 2026 ScratchBird Software Inc.
//
// This Source Code Form is subject to the terms of the Mozilla Public
// License, v. 2.0. If a copy of the MPL was not distributed with this
// file, You can obtain one at https://mozilla.org/MPL/2.0/.
//
// SPDX-License-Identifier: MPL-2.0

#pragma once

#include "sblr_runtime.hpp"

#include <string>
#include <string_view>
#include <vector>

namespace scratchbird::engine::sblr {

enum class SblrTruthValue {
  false_value,
  true_value,
  unknown,
};

struct SblrOperatorEntry {
  std::string operator_id;
  std::string operator_uuid;
  std::string symbol;
  std::string family;
  std::string sblr_opcode;
  bool short_circuit = false;
};

const std::vector<SblrOperatorEntry>& StandardSblrOperatorRegistry();
const SblrOperatorEntry* LookupSblrOperator(std::string_view operator_id);
SblrTruthValue SblrNot(SblrTruthValue value);
SblrTruthValue SblrAnd(SblrTruthValue left, SblrTruthValue right);
SblrTruthValue SblrOr(SblrTruthValue left, SblrTruthValue right);
SblrTruthValue SblrXor(SblrTruthValue left, SblrTruthValue right);
SblrValue MakeSblrTruthValue(SblrTruthValue value);
SblrResult EvaluateSblrComparison(std::string_view operator_id,
                                  const SblrValue& left,
                                  const SblrValue& right,
                                  const SblrExecutionContext& context);
SblrResult EvaluateSblrArithmetic(std::string_view operator_id,
                                  const SblrValue& left,
                                  const SblrValue& right,
                                  const SblrExecutionContext& context);
SblrResult EvaluateSblrUnaryArithmetic(std::string_view operator_id,
                                       const SblrValue& operand,
                                       const SblrExecutionContext& context);
SblrResult EvaluateSblrStringOperator(std::string_view operator_id,
                                      const SblrValue& left,
                                      const SblrValue& right,
                                      const SblrExecutionContext& context);
SblrResult EvaluateSblrDocumentOperator(std::string_view operator_id,
                                        const SblrValue& left,
                                        const SblrValue& right,
                                        const SblrExecutionContext& context);
SblrResult EvaluateSblrCollectionOperator(std::string_view operator_id,
                                          const SblrValue& left,
                                          const SblrValue& right,
                                          const SblrExecutionContext& context);
SblrResult EvaluateSblrVectorOperator(std::string_view operator_id,
                                      const SblrValue& left,
                                      const SblrValue& right,
                                      const SblrExecutionContext& context);
SblrResult EvaluateSblrSpecializedOperatorBridge(std::string_view operator_id,
                                                 const SblrValue& left,
                                                 const SblrValue& right,
                                                 const SblrExecutionContext& context);

}  // namespace scratchbird::engine::sblr
