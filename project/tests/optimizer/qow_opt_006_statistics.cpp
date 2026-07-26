#define QOW_OPT_006_FIXTURE_ONLY 1
#include "qow_opt_006_catalog.cpp"

namespace {

bool ValidateStatisticsRefusals() {
  const auto expect_refusal = [](auto mutation,
                                 const std::string_view detail) {
    auto request = Request();
    mutation(request);
    const auto result = opt::AdmitCanonicalOptimizerPlanningRequest(request);
    return Require(
        !result.admitted && !result.planning_allowed &&
            !result.data_access_allowed && result.evidence.size() == 6 &&
            result.issues.size() == 1 &&
            result.issues.front().stage ==
                opt::CanonicalOptimizerAdmissionStage::kStatisticsProvenance,
        detail);
  };
  bool passed = true;
  passed &= expect_refusal(
      [](auto& request) { request.statistics.runtime_actuals_present = true; },
      "runtime actual entered pre-access admission");
  passed &= expect_refusal(
      [](auto& request) {
        request.statistics.node_estimates.front().collected_at_monotonic_ns = 1;
      },
      "stale statistic was admitted");
  passed &= expect_refusal(
      [](auto& request) {
        request.statistics.node_estimates.front().catalog_epoch_uuid =
            "019f0000-0000-7300-8000-000000006999";
      },
      "statistics from a different catalog epoch were admitted");
  passed &= expect_refusal(
      [](auto& request) {
        request.statistics.node_estimates.front()
            .derived_from_runtime_actuals = true;
      },
      "runtime-derived estimate was admitted");
  return passed;
}

}  // namespace

// QOW-TEST-OPT-006-STATISTICS-V1
int main() {
  return ValidateStatisticsRefusals() ? EXIT_SUCCESS : EXIT_FAILURE;
}
