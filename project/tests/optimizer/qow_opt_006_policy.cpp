#define QOW_OPT_006_FIXTURE_ONLY 1
#include "qow_opt_006_catalog.cpp"

namespace {

bool ValidatePolicyRefusals() {
  const auto expect_refusal = [](auto mutation,
                                 const std::string_view detail) {
    auto request = Request();
    mutation(request);
    const auto result = opt::AdmitCanonicalOptimizerPlanningRequest(request);
    return Require(
        !result.admitted && !result.planning_allowed &&
            !result.data_access_allowed && result.evidence.size() == 4 &&
            result.issues.size() == 1 &&
            result.issues.front().stage ==
                opt::CanonicalOptimizerAdmissionStage::kPolicyCapability &&
            result.issues.front().diagnostic_id ==
                "QOW-DIAG-OPTIMIZER-ADMISSION-POLICY-CAPABILITY-V1",
        detail);
  };
  bool passed = true;
  passed &= expect_refusal(
      [](auto& request) {
        request.policy_capability.supported_node_kinds.clear();
      },
      "unsupported logical node was admitted");
  passed &= expect_refusal(
      [](auto& request) {
        request.policy_capability.capability_abi_version = 0;
      },
      "missing capability ABI was admitted");
  passed &= expect_refusal(
      [](auto& request) {
        request.policy_capability.cluster_capability_claimed = true;
      },
      "cluster capability entered the standalone admission lane");
  passed &= expect_refusal(
      [](auto& request) { ++request.policy_capability.policy_epoch; },
      "stale policy epoch was admitted");
  return passed;
}

}  // namespace

// QOW-TEST-OPT-006-POLICY-V1
int main() {
  return ValidatePolicyRefusals() ? EXIT_SUCCESS : EXIT_FAILURE;
}
