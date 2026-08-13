// Copyright (c) 2026 ScratchBird Software Inc.
// SPDX-License-Identifier: MPL-2.0

#define SB_RCP080_RUNTIME_FIXTURE_ONLY 1
#include "qow_opt_007_dependency.cpp"

int main() {
  const auto plan =
      optimizer::CoordinateModelFamilyDependencyDagV1(Admission(349));
  const auto root = std::filesystem::temp_directory_path() /
                    "sb_rcp080_qow_ias_008";
  std::filesystem::remove_all(root);
  memory::TempWorkspaceLifecycleManager workspace(
      WorkspacePolicy(root, "rcp080_qow_ias_008"));
  RuntimeCounters counters;
  auto request = RuntimeRequest(plan, &workspace, &counters, 4800);
  const auto original = request.legs[2].execution.execute_provider;
  request.legs[2].execution.execute_provider = [original](const auto& input) {
    auto result = original(input);
    result.provider_batch.output_descriptor_ids[0] = 9999;
    return result;
  };
  const auto refused = executor::ExecuteModelFamilyCompositionV1(request);
  bool passed = Require(!refused.accepted && !refused.root_published &&
                                  refused.no_partial_root &&
                                  refused.root_output_batch.rows.empty() &&
                                  refused.diagnostic_id ==
                                      executor::kModelTypedExchangeInvalid &&
                                  refused.relational_consumer_cleanup_count == 0 &&
                                  counters.publication_revalidations.load() == 0 &&
                                  refused.cleanup_complete &&
                                  workspace.Snapshot().active_bytes == 0,
                              "typed descriptor substitution reached relational consumption or root publication");
  RuntimeCounters recheck_counters;
  auto recheck = RuntimeRequest(plan, &workspace, &recheck_counters, 4810);
  const auto original_recheck_provider =
      recheck.legs[0].execution.execute_provider;
  recheck.legs[0].execution.execute_provider =
      [original_recheck_provider](const auto& input) {
        auto result = original_recheck_provider(input);
        result.provider_batch.security_recheck_complete = false;
        result.provider_batch.properties.security_recheck_complete = false;
        return result;
      };
  const auto recheck_refused =
      executor::ExecuteModelFamilyCompositionV1(recheck);
  const auto recheck_receipt = std::ranges::find_if(
      recheck_refused.rule_receipts,
      [](const auto& receipt) { return receipt.rule_id == "COORD-017-V1"; });
  passed &= Require(!recheck_refused.accepted &&
                        !recheck_refused.root_published &&
                        recheck_refused.no_partial_root &&
                        recheck_refused.diagnostic_id ==
                            "SB_MODEL_EXACT_RECHECK_FAILED_V1" &&
                        recheck_receipt != recheck_refused.rule_receipts.end() &&
                        !recheck_receipt->complete &&
                        recheck_counters.consumer_cleanups.load() == 0 &&
                        recheck_counters.publication_revalidations.load() == 0 &&
                        recheck_refused.cleanup_complete &&
                        workspace.Snapshot().active_bytes == 0,
                    "incomplete exact recheck did not emit exact COORD-017 refusal");

  RuntimeCounters publication_counters;
  auto publication =
      RuntimeRequest(plan, &workspace, &publication_counters, 4820);
  publication.revalidate_publication_state = [plan, &publication_counters] {
    publication_counters.publication_revalidations.fetch_add(1);
    auto state = Publication(plan);
    ++state.current_descriptor_generations[1];
    return state;
  };
  const auto publication_refused =
      executor::ExecuteModelFamilyCompositionV1(publication);
  const auto publication_receipt = std::ranges::find_if(
      publication_refused.rule_receipts,
      [](const auto& receipt) { return receipt.rule_id == "COORD-022-V1"; });
  passed &= Require(!publication_refused.accepted &&
                        !publication_refused.root_published &&
                        publication_refused.no_partial_root &&
                        publication_refused.root_output_batch.rows.empty() &&
                        publication_refused.root_publication_receipt_uuid.empty() &&
                        publication_refused.diagnostic_id ==
                            "SB_MODEL_ROOT_PUBLICATION_REFUSED_V1" &&
                        publication_receipt !=
                            publication_refused.rule_receipts.end() &&
                        !publication_receipt->complete &&
                        publication_counters.publication_revalidations.load() == 1 &&
                        publication_refused.cleanup_complete &&
                        workspace.Snapshot().active_bytes == 0,
                    "final descriptor-generation drift did not emit exact COORD-022 refusal");
  if (!passed) return 1;
  std::cout << "QOW-IAS-008: passed;coord016=typed_refusal;coord017=exact_recheck_refusal;coord022=root_publication_refusal\n";
  return 0;
}
