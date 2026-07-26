#define QOW_OPT_006_FIXTURE_ONLY 1
#include "qow_opt_006_catalog.cpp"

namespace {

bool ValidateSecurityRefusals() {
  const auto expect_refusal = [](auto mutation,
                                 const std::string_view detail) {
    auto request = Request();
    mutation(request);
    const auto result = opt::AdmitCanonicalOptimizerPlanningRequest(request);
    return Require(!result.admitted && !result.planning_allowed &&
                       !result.data_access_allowed &&
                       result.evidence.size() == 2 &&
                       result.issues.size() == 1 &&
                       result.issues.front().stage ==
                           opt::CanonicalOptimizerAdmissionStage::kSecurity &&
                       result.issues.front().diagnostic_id ==
                           "QOW-DIAG-OPTIMIZER-ADMISSION-SECURITY-V1",
                   detail);
  };
  bool passed = true;
  passed &= expect_refusal(
      [](auto& request) { request.security.engine_owned = false; },
      "non-engine security snapshot was admitted");
  passed &= expect_refusal(
      [](auto& request) { request.security.authorized_object_uuids.clear(); },
      "unauthorized object was admitted");
  passed &= expect_refusal(
      [](auto& request) { request.security.security_epoch = 0; },
      "missing security epoch was admitted");
  passed &= expect_refusal(
      [](auto& request) { ++request.security.catalog_generation; },
      "stale security catalog generation was admitted");
  return passed;
}

}  // namespace

// QOW-TEST-OPT-006-SECURITY-V1
int main() {
  return ValidateSecurityRefusals() ? EXIT_SUCCESS : EXIT_FAILURE;
}
