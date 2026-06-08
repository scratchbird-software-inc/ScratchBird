// Copyright (c) 2026 ScratchBird Software Inc.
//
// This Source Code Form is subject to the terms of the Mozilla Public
// License, v. 2.0. If a copy of the MPL was not distributed with this
// file, You can obtain one at https://mozilla.org/MPL/2.0/.
//
// SPDX-License-Identifier: MPL-2.0

#include "optimizer_rewrite.hpp"

#include <iostream>
#include <string>
#include <vector>

namespace opt = scratchbird::engine::optimizer;
namespace {
void Expect(bool condition, const char* message, std::vector<std::string>* errors) { if (!condition) errors->push_back(message); }
int Finish(const std::vector<std::string>& errors) { std::cout << "{\"ok\":" << (errors.empty() ? "true" : "false") << ",\"failure_count\":" << errors.size() << "}\n"; return errors.empty() ? 0 : 1; }
}
int main() {
  std::vector<std::string> errors;
  opt::OptimizerBarrierInput barrier;
  barrier.security_context_present = true;
  barrier.grants_proven = true;
  opt::OptimizerExpressionTerm expr;
  expr.term_id = "t1";
  expr.operator_id = "op.add";
  expr.descriptor_digest = "int64";
  expr.literal = true;
  Expect(opt::NormalizeExpression(expr).applied, "expression normalizes", &errors);
  Expect(opt::SafeConstantFold(expr, barrier).applied, "safe literal folds", &errors);
  barrier.function_volatile = true;
  Expect(!opt::SafeConstantFold(expr, barrier).applied, "volatile barrier blocks fold", &errors);
  opt::PredicateNormalizationInput predicate{"scalar_eq", "int64", "int64", false, false};
  Expect(opt::NormalizePredicate(predicate).canonical_form == "eq", "predicate normalizes", &errors);
  opt::ProjectionPruneInput projection{{"a", "b"}, {"a"}, {"b"}};
  Expect(opt::PruneProjection(projection).preserved_column_uuids.size() == 2, "masked column preserved", &errors);
  return Finish(errors);
}
