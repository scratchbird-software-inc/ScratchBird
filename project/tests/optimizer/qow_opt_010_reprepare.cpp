// Copyright (c) 2026 ScratchBird Software Inc.
// SPDX-License-Identifier: MPL-2.0

#define main qow_opt_009_reprepare_fixture_main
#include "qow_opt_009.cpp"
#undef main

#include <atomic>

namespace {

bool RequireReprepare010(const bool condition,
                         const std::string_view detail) {
  if (!condition) {
    std::cerr << "QOW-TEST-OPT-010-REPREPARE-V1: " << detail << '\n';
  }
  return condition;
}

void AdvanceCatalog010(cache::CanonicalExecutablePlanCacheKey* current) {
  ++current->catalog_generation;
  const auto schema = std::ranges::find_if(
      current->metadata_generations, [&](const auto& generation) {
        return generation.identity_uuid == current->result_schema_uuid;
      });
  if (schema != current->metadata_generations.end()) {
    schema->generation = current->catalog_generation;
  }
}

cache::CanonicalExecutablePlanReprepareCandidate ReplacementCandidate010(
    const cache::CanonicalExecutablePlanCacheKey& current,
    const bool authorized = true) {
  auto prepare = PrepareRequest009();
  prepare.prepared_plan_uuid = Uuid(1701);
  prepare.prepare_generation = current.prepare_generation + 1;
  prepare.parameter_shape_uuid = current.parameter_shape_uuid;
  prepare.result_schema_uuid = current.result_schema_uuid;
  prepare.selected_physical_dag.catalog_generation =
      current.catalog_generation;
  prepare.selected_physical_dag.selected_plan_uuid = Uuid(1702);

  cache::CanonicalPreparedPlanStore staging;
  const auto staged = cache::PrepareCanonicalPhysicalPlan(prepare, &staging);
  cache::CanonicalExecutablePlanReprepareCandidate candidate;
  candidate.prepare_request = prepare;
  candidate.engine_candidate_authorized = authorized;
  candidate.planner_invocation_count = 1;
  if (!staged.accepted || !staged.prepared_plan) return candidate;

  auto key = current;
  key.cache_plan_uuid = Uuid(1703);
  key.compiled_at_uuidv7 = Uuid(1704);
  key.plan_key_digest = std::string(64, 'd');
  key.plan_status = cache::CanonicalExecutablePlanStatus::kValid;
  key.prepared_plan_uuid = staged.prepared_plan->prepared_plan_uuid;
  key.prepare_generation = staged.prepared_plan->prepare_generation;
  key.parameter_shape_uuid = staged.prepared_plan->parameter_shape_uuid;
  key.result_schema_uuid = staged.prepared_plan->result_schema_uuid;
  key.selected_plan_uuid = staged.prepared_plan->selected_plan_uuid;
  key.selected_plan_signature = staged.prepared_plan->selected_plan_signature;
  key.selected_scalar_score = staged.prepared_plan->selected_scalar_score;
  key.root_physical_node_id = staged.prepared_plan->root_physical_node_id;
  key.published_node_count = staged.prepared_plan->published_node_count;
  key.first_causal_counter_id = staged.prepared_plan->first_causal_counter_id;
  key.bound_sblr_tree_uuid = staged.prepared_plan->bound_sblr_tree_uuid;
  key.catalog_epoch_uuid = staged.prepared_plan->catalog_epoch_uuid;
  key.catalog_generation = staged.prepared_plan->catalog_generation;
  key.parameters = staged.prepared_plan->parameters;
  key.result_descriptors = staged.prepared_plan->result_descriptors;
  key.physical_dependencies = staged.prepared_plan->dependencies;
  candidate.admission_request.key = std::move(key);
  candidate.admission_request.engine_cache_admission_authorized = true;
  return candidate;
}

cache::CanonicalExecutablePlanReprepareRequest ReprepareRequest010(
    Fixture009* fixture,
    const cache::CanonicalExecutablePlanCacheKey& current,
    std::atomic<std::uint64_t>* callback_count,
    const bool candidate_authorized = true) {
  cache::CanonicalExecutablePlanReprepareRequest request;
  request.invalidated_prepared_plan_uuid = fixture->key.prepared_plan_uuid;
  request.current_key = current;
  request.prepared_plan_store = &fixture->prepared_store;
  request.engine_invalidation_authorized = true;
  request.engine_reprepare_authorized = true;
  request.engine_security_revalidated = true;
  request.engine_policy_revalidated = true;
  request.reprepare_once =
      [current, callback_count, candidate_authorized](const auto&) {
        ++*callback_count;
        return ReplacementCandidate010(current, candidate_authorized);
      };
  return request;
}

bool ValidateOneSuccessfulReprepareAndReplacementExecution010() {
  Fixture009 fixture;
  if (!fixture.ready) return false;
  auto current = fixture.key;
  AdvanceCatalog010(&current);
  std::atomic<std::uint64_t> callback_count{0};
  std::size_t executor_invocations = 0;
  bool fresh_context = true;
  cache::CanonicalExecutablePlanGovernedExecutionRequest governed;
  governed.executable_plan_cache = &fixture.executable_cache;
  governed.reprepare =
      ReprepareRequest010(&fixture, current, &callback_count);
  governed.engine_execution_authorized = true;
  governed.build_replacement_execution =
      [&](const auto& replacement) {
        return ExecutionRequest009(
            &fixture, replacement->key, FreshStatement009(1720),
            &executor_invocations, &fresh_context);
      };
  const auto first =
      api::ExecuteCanonicalExecutablePlanAfterSingleReprepare(governed);
  const auto second =
      api::ExecuteCanonicalExecutablePlanAfterSingleReprepare(governed);
  const bool passed = first.accepted && first.replacement_executed &&
          !first.stale_plan_executed && first.reprepare.reprepared &&
          first.reprepare.caller_was_single_flight_leader &&
          first.governed_reprepare_attempt_count == 1 &&
          first.reprepare.replacement_prepared_plan_uuid !=
              fixture.key.prepared_plan_uuid &&
          first.execution.selected_plan_uuid !=
              fixture.key.selected_plan_uuid &&
          second.accepted && second.replacement_executed &&
          !second.reprepare.caller_was_single_flight_leader &&
          second.governed_reprepare_attempt_count == 1 &&
          callback_count == 1 && fresh_context &&
          executor_invocations == fixture.prepared_plan->nodes.size() * 2 &&
          fixture.executable_cache.Size() == 1;
  if (!passed) {
    std::cerr << "QOW-TEST-OPT-010-REPREPARE-V1: first="
              << first.accepted << "/" << first.replacement_executed
              << " second=" << second.accepted << "/"
              << second.replacement_executed << " callback="
              << callback_count << " attempts="
              << first.governed_reprepare_attempt_count << "/"
              << second.governed_reprepare_attempt_count << " size="
              << fixture.executable_cache.Size() << " exec="
              << executor_invocations << '\n';
    for (const auto& issue : first.issues) {
      std::cerr << " first_issue=" << issue.diagnostic_id << ":"
                << issue.field_id << '\n';
    }
    for (const auto& issue : second.issues) {
      std::cerr << " second_issue=" << issue.diagnostic_id << ":"
                << issue.field_id << '\n';
    }
  }
  return RequireReprepare010(
      passed, "success did not perform and reuse exactly one governed replacement");
}

bool ValidateOneFailedReprepareNeverRetriesOrExecutes010() {
  Fixture009 fixture;
  if (!fixture.ready) return false;
  auto current = fixture.key;
  AdvanceCatalog010(&current);
  std::atomic<std::uint64_t> callback_count{0};
  const auto request =
      ReprepareRequest010(&fixture, current, &callback_count, false);
  const auto first = fixture.executable_cache.InvalidateAndReprepareOnce(request);
  const auto second = fixture.executable_cache.InvalidateAndReprepareOnce(request);
  return RequireReprepare010(
      !first.accepted && first.invalidated &&
          first.governed_attempt_count == 1 &&
          first.total_attempt_count == 1 && first.issues.size() == 1 &&
          first.issues.front().diagnostic_id ==
              "QOW-DIAG-OPT-010-REPREPARE-REFUSAL-V1" &&
          !second.accepted && second.governed_attempt_count == 0 &&
          second.total_attempt_count == 1 && callback_count == 1 &&
          fixture.executable_cache.Size() == 0 &&
          first.stale_execution_count == 0 && second.stale_execution_count == 0,
      "failed reprepare retried, admitted, or executed stale state");
}

bool ValidateOneAttemptCannotHideRepeatedOptimizerWork010() {
  bool passed = true;
  for (const bool repeat_search : {false, true}) {
    Fixture009 fixture;
    if (!fixture.ready) return false;
    auto current = fixture.key;
    AdvanceCatalog010(&current);
    std::atomic<std::uint64_t> callback_count{0};
    auto request = ReprepareRequest010(&fixture, current, &callback_count);
    request.reprepare_once =
        [current, &callback_count, repeat_search](const auto&) {
          ++callback_count;
          auto candidate = ReplacementCandidate010(current);
          if (repeat_search) {
            candidate.search_invocation_count = 2;
          } else {
            candidate.optimizer_invocation_count = 2;
          }
          return candidate;
        };
    const auto result =
        fixture.executable_cache.InvalidateAndReprepareOnce(request);
    passed &= !result.accepted && result.governed_attempt_count == 1 &&
              result.total_attempt_count == 1 && callback_count == 1 &&
              fixture.executable_cache.Size() == 0 &&
              result.issues.size() == 1 &&
              result.issues.front().diagnostic_id ==
                  "QOW-DIAG-OPT-010-REPREPARE-REFUSAL-V1" &&
              result.issues.front().field_id ==
                  "governed_reprepare_candidate";
  }
  return RequireReprepare010(
      passed, "one attempt hid repeated optimizer or search invocation");
}

bool ValidateReprepareRouteIsolation010() {
  std::ifstream source_file(SB_QOW_PLAN_API_SOURCE_FILE);
  const std::string source((std::istreambuf_iterator<char>(source_file)),
                           std::istreambuf_iterator<char>());
  const auto begin =
      source.find("QOW-ROUTE-STAGE-OPT-010-REPREPARE-V1-BEGIN");
  const auto end = source.find("QOW-ROUTE-STAGE-OPT-010-REPREPARE-V1-END",
                               begin);
  if (begin == std::string::npos || end == std::string::npos || end <= begin) {
    return RequireReprepare010(false, "production route markers are absent");
  }
  const auto route = source.substr(begin, end - begin);
  return RequireReprepare010(
      route.find("InvalidateAndReprepareOnce") != std::string::npos &&
          route.find("ExecuteCanonicalExecutablePlanCacheHit") !=
              std::string::npos &&
          route.find("EnginePlanOperationUncachedImpl") == std::string::npos &&
          route.find("OptimizeLogicalPlan") == std::string::npos &&
          route.find("SearchCanonicalRelationalMemo") == std::string::npos &&
          route.find("DecodeSblrEnvelope") == std::string::npos,
      "reprepare route reaches an ordinary/planner/fallback entry");
}

}  // namespace

// QOW-TEST-OPT-010-REPREPARE-V1
#ifndef QOW_OPT_010_REPREPARE_FIXTURE_ONLY
int main() {
  return ValidateOneSuccessfulReprepareAndReplacementExecution010() &&
                 ValidateOneFailedReprepareNeverRetriesOrExecutes010() &&
                 ValidateOneAttemptCannotHideRepeatedOptimizerWork010() &&
                 ValidateReprepareRouteIsolation010()
             ? EXIT_SUCCESS
             : EXIT_FAILURE;
}
#endif
