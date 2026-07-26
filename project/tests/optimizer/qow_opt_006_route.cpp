#define QOW_OPT_006_FIXTURE_ONLY 1
#include "qow_opt_006_catalog.cpp"

namespace {

bool ValidateRouteRefusals() {
  const auto expect_refusal = [](auto mutation,
                                 const std::string_view detail) {
    auto request = Request();
    mutation(request);
    const auto result = opt::AdmitCanonicalOptimizerPlanningRequest(request);
    return Require(
        !result.admitted && !result.planning_allowed &&
            !result.data_access_allowed && result.evidence.size() == 7 &&
            result.issues.size() == 1 &&
            result.issues.front().stage ==
                opt::CanonicalOptimizerAdmissionStage::kCanonicalRoute &&
            result.issues.front().diagnostic_id ==
                "QOW-DIAG-OPTIMIZER-ADMISSION-ROUTE-V1",
        detail);
  };
  bool passed = true;
  passed &= expect_refusal(
      [](auto& request) { request.route.route_id = "legacy.query.route"; },
      "legacy route was admitted");
  passed &= expect_refusal(
      [](auto& request) { request.route.operation_id = "query.plan_operation"; },
      "noncanonical operation was admitted");
  passed &= expect_refusal(
      [](auto& request) { request.route.native_local_route = false; },
      "non-native route was admitted");
  passed &= expect_refusal(
      [](auto& request) { request.route.cluster_route_claimed = true; },
      "cluster route entered the standalone admission lane");
  return passed;
}

}  // namespace

// QOW-TEST-OPT-006-ROUTE-V1
int main() {
  return ValidateRouteRefusals() ? EXIT_SUCCESS : EXIT_FAILURE;
}
