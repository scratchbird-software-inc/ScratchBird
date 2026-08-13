// Copyright (c) 2026 ScratchBird Software Inc.
// SPDX-License-Identifier: MPL-2.0

#define SB_RCP080_RUNTIME_FIXTURE_ONLY 1
#include "qow_opt_007_dependency.cpp"

int main() {
  bool passed = true;
  const auto accepted =
      optimizer::CoordinateModelFamilyDependencyDagV1(Admission(349));
  passed &= Require(accepted.accepted && accepted.data_access_allowed &&
                        accepted.stable_schedule.size() == 3 &&
                        accepted.stable_schedule[0].leg.physical_node_uuid <
                            accepted.stable_schedule[1].leg.physical_node_uuid,
                    "bounded multimodel costed plan was not selected deterministically");

  auto stale = Admission(349);
  stale.legs[1].family_local_cost.provenance_generation = 0;
  stale.legs[1].candidate_alternatives[0].family_local_cost =
      stale.legs[1].family_local_cost;
  const auto stale_result =
      optimizer::CoordinateModelFamilyDependencyDagV1(stale);
  passed &= Require(!stale_result.accepted &&
                        stale_result.diagnostic_id ==
                            "SB_MODEL_COST_VECTOR_INVALID_V1",
                    "invalid cost provenance was admitted");

  auto alternate = Admission(349);
  auto candidate = alternate.legs[0].candidate_alternatives[0];
  candidate.alternative_uuid = Uuid(50);
  candidate.authority_approved_comparison_rank = 1;
  alternate.legs[0].candidate_alternatives[0]
      .authority_approved_comparison_rank = 2;
  alternate.legs[0].candidate_alternatives.push_back(candidate);
  const auto no_substitution =
      optimizer::CoordinateModelFamilyDependencyDagV1(alternate);
  passed &= Require(!no_substitution.accepted &&
                        no_substitution.diagnostic_id ==
                            "SB_MODEL_NO_ADMITTED_ALTERNATIVE_V1",
                    "coordinator silently substituted a cheaper unselected alternative");
  if (!passed) return 1;
  std::cout << "QOW-OPT-012: passed;cost_provenance=exact;selection=deterministic;substitution=refused\n";
  return 0;
}
