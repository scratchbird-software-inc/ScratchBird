// Copyright (c) 2026 ScratchBird Software Inc.
//
// This Source Code Form is subject to the terms of the Mozilla Public
// License, v. 2.0. If a copy of the MPL was not distributed with this
// file, You can obtain one at https://mozilla.org/MPL/2.0/.
//
// SPDX-License-Identifier: MPL-2.0

#pragma once

#include "sblr_runtime.hpp"

#include <functional>
#include <string>
#include <string_view>
#include <vector>

namespace scratchbird::engine::sblr {

struct SblrCaseBranch {
  SblrValue condition;
  SblrValue result;
};

using SblrExpressionThunk = std::function<SblrResult()>;

struct SblrCaseThunkBranch {
  SblrExpressionThunk condition;
  SblrExpressionThunk result;
};

SblrResult EvaluateSblrCaseSearchedForm(std::string_view operation_id,
                                        const std::vector<SblrCaseBranch>& branches,
                                        const SblrValue& else_value);
SblrResult EvaluateSblrCaseSearchedFormLazy(std::string_view operation_id,
                                            const std::vector<SblrCaseThunkBranch>& branches,
                                            SblrExpressionThunk else_value,
                                            const SblrExecutionContext& context);
SblrResult EvaluateSblrIifForm(std::string_view operation_id,
                               const SblrValue& condition,
                               const SblrValue& true_value,
                               const SblrValue& false_value);
SblrResult EvaluateSblrIifFormLazy(std::string_view operation_id,
                                   SblrExpressionThunk condition,
                                   SblrExpressionThunk true_value,
                                   SblrExpressionThunk false_value,
                                   const SblrExecutionContext& context);
SblrResult EvaluateSblrCoalesceForm(std::string_view operation_id, const std::vector<SblrValue>& values);
SblrResult EvaluateSblrCoalesceFormLazy(std::string_view operation_id,
                                        const std::vector<SblrExpressionThunk>& values,
                                        const SblrExecutionContext& context);
SblrResult EvaluateSblrNullIfForm(std::string_view operation_id, const SblrValue& left, const SblrValue& right);
SblrResult EvaluateSblrExistsForm(std::string_view operation_id, bool row_exists);
SblrResult EvaluateSblrInListForm(std::string_view operation_id,
                                  const SblrValue& probe,
                                  const std::vector<SblrValue>& candidates);
SblrResult EvaluateSblrInListFormLazy(std::string_view operation_id,
                                      SblrExpressionThunk probe,
                                      const std::vector<SblrExpressionThunk>& candidates,
                                      const SblrExecutionContext& context);
SblrResult EvaluateSblrBetweenForm(std::string_view operation_id,
                                   const SblrValue& value,
                                   const SblrValue& lower,
                                   const SblrValue& upper);
SblrResult EvaluateSblrBetweenFormLazy(std::string_view operation_id,
                                       SblrExpressionThunk value,
                                       SblrExpressionThunk lower,
                                       SblrExpressionThunk upper,
                                       const SblrExecutionContext& context);
SblrResult EvaluateSblrSubstringForm(std::string_view operation_id,
                                     const SblrValue& value,
                                     const SblrValue& start,
                                     const SblrValue& length,
                                     const SblrExecutionContext& context);
SblrResult EvaluateSblrCastForm(std::string_view operation_id,
                                const SblrValue& value,
                                std::string_view target_descriptor_id,
                                const SblrExecutionContext& context,
                                bool explicit_cast = true,
                                bool donor_compatibility_profile = false);
SblrResult EvaluateSblrExtractForm(std::string_view operation_id,
                                   std::string_view field_name,
                                   const SblrValue& value,
                                   const SblrExecutionContext& context);
SblrResult RefuseSblrSpecialForm(std::string_view operation_id, const SblrExecutionContext& context, std::string detail);

}  // namespace scratchbird::engine::sblr
