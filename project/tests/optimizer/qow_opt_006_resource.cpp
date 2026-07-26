#define QOW_OPT_006_FIXTURE_ONLY 1
#include "qow_opt_006_catalog.cpp"

namespace {

bool ValidateResourceRefusals() {
  const auto expect_refusal = [](auto mutation,
                                 const std::string_view detail) {
    auto request = Request();
    mutation(request);
    const auto result = opt::AdmitCanonicalOptimizerPlanningRequest(request);
    return Require(!result.admitted && !result.planning_allowed &&
                       !result.data_access_allowed &&
                       result.evidence.size() == 5 &&
                       result.issues.size() == 1 &&
                       result.issues.front().stage ==
                           opt::CanonicalOptimizerAdmissionStage::kResource &&
                       result.issues.front().diagnostic_id ==
                           "QOW-DIAG-OPTIMIZER-ADMISSION-RESOURCE-V1",
                   detail);
  };
  bool passed = true;
  passed &= expect_refusal(
      [](auto& request) { request.resource.engine_owned = false; },
      "non-engine resource snapshot was admitted");
  passed &= expect_refusal(
      [](auto& request) { request.resource.memory_budget_bytes = 0; },
      "zero planning memory was admitted");
  passed &= expect_refusal(
      [](auto& request) { request.resource.maximum_memo_groups = 0; },
      "undersized memo budget was admitted");
  passed &= expect_refusal(
      [](auto& request) { request.resource.maximum_planning_time_ns = 0; },
      "missing planning time boundary was admitted");
  return passed;
}

}  // namespace

// QOW-TEST-OPT-006-RESOURCE-V1
int main() {
  return ValidateResourceRefusals() ? EXIT_SUCCESS : EXIT_FAILURE;
}
