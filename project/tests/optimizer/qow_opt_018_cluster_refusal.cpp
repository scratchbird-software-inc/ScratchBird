// Copyright (c) 2026 ScratchBird Software Inc.
// SPDX-License-Identifier: MPL-2.0

#define SB_RCP080_RUNTIME_FIXTURE_ONLY 1
#include "qow_opt_007_dependency.cpp"

int main() {
  auto unavailable = Admission();
  unavailable.legs[2].local_scope = false;
  unavailable.legs[2].cluster_scope_required = true;
  unavailable.cluster_capability_available = false;
  const auto refused =
      optimizer::CoordinateModelFamilyDependencyDagV1(unavailable);
  auto available = unavailable;
  available.cluster_capability_available = true;
  const auto accepted =
      optimizer::CoordinateModelFamilyDependencyDagV1(available);
  const bool passed = Require(!refused.accepted &&
                                  !refused.data_access_allowed &&
                                  refused.diagnostic_id ==
                                      "SB_MODEL_CLUSTER_CAPABILITY_UNAVAILABLE_V1" &&
                                  accepted.accepted &&
                                  accepted.data_access_allowed,
                              "cluster-required leg capability admission drifted");
  if (!passed) return 1;
  std::cout << "QOW-OPT-018-CLUSTER-REFUSAL: passed;unavailable=refused;available=admitted\n";
  return 0;
}
