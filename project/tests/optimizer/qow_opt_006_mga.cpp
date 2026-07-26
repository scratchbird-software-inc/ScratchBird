#define QOW_OPT_006_FIXTURE_ONLY 1
#include "qow_opt_006_catalog.cpp"

namespace {

bool ValidateMgaRefusals() {
  const auto expect_refusal = [](auto mutation,
                                 const std::string_view detail) {
    auto request = Request();
    mutation(request);
    const auto result = opt::AdmitCanonicalOptimizerPlanningRequest(request);
    return Require(
        !result.admitted && !result.planning_allowed &&
            !result.data_access_allowed && result.evidence.size() == 3 &&
            result.issues.size() == 1 &&
            result.issues.front().stage ==
                opt::CanonicalOptimizerAdmissionStage::kMgaStatementBoundary &&
            result.issues.front().diagnostic_id ==
                "QOW-DIAG-OPTIMIZER-ADMISSION-MGA-V1",
        detail);
  };
  bool passed = true;
  passed &= expect_refusal(
      [](auto& request) { request.mga.engine_owned = false; },
      "non-engine MGA snapshot was admitted");
  passed &= expect_refusal(
      [](auto& request) { ++request.mga.local_transaction_id; },
      "different MGA transaction was admitted");
  passed &= expect_refusal(
      [](auto& request) { request.mga.statement_snapshot_fixed = false; },
      "unfixed statement snapshot was admitted");
  passed &= expect_refusal(
      [](auto& request) { request.mga.finality_authority_claimed = true; },
      "optimizer finality authority was admitted");
  return passed;
}

}  // namespace

// QOW-TEST-OPT-006-MGA-V1
int main() {
  return ValidateMgaRefusals() ? EXIT_SUCCESS : EXIT_FAILURE;
}
