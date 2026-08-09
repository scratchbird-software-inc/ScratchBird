// Copyright (c) 2026 ScratchBird Software Inc.
// SPDX-License-Identifier: MPL-2.0

#define QOW_OPT_010_REPREPARE_FIXTURE_ONLY
#include "qow_opt_010_reprepare.cpp"

#include <chrono>
#include <stdexcept>
#include <thread>

namespace {

bool RequireConcurrency010(const bool condition,
                           const std::string_view detail) {
  if (!condition) {
    std::cerr << "QOW-TEST-OPT-010-CONCURRENCY-V1: " << detail << '\n';
  }
  return condition;
}

bool ValidateConcurrentSingleFlightAndFreshStatements010() {
  Fixture009 fixture;
  if (!fixture.ready) return false;
  auto current = fixture.key;
  AdvanceCatalog010(&current);
  std::atomic<std::uint64_t> callback_count{0};
  auto reprepare = ReprepareRequest010(&fixture, current, &callback_count);
  reprepare.reprepare_once = [current, &callback_count](const auto&) {
    ++callback_count;
    std::this_thread::sleep_for(std::chrono::milliseconds(25));
    return ReplacementCandidate010(current);
  };

  constexpr std::size_t kCallers = 12;
  std::vector<cache::CanonicalExecutablePlanGovernedExecutionResult> results(
      kCallers);
  std::vector<std::size_t> executor_invocations(kCallers, 0);
  auto fresh_contexts = std::make_unique<bool[]>(kCallers);
  for (std::size_t index = 0; index < kCallers; ++index) {
    fresh_contexts[index] = true;
  }
  std::vector<std::thread> callers;
  callers.reserve(kCallers);
  for (std::size_t index = 0; index < kCallers; ++index) {
    callers.emplace_back([&, index] {
      cache::CanonicalExecutablePlanGovernedExecutionRequest governed;
      governed.executable_plan_cache = &fixture.executable_cache;
      governed.reprepare = reprepare;
      governed.engine_execution_authorized = true;
      governed.build_replacement_execution = [&](const auto& replacement) {
        return ExecutionRequest009(
            &fixture, replacement->key,
            FreshStatement009(1800 + index * 20),
            &executor_invocations[index], &fresh_contexts[index]);
      };
      results[index] =
          api::ExecuteCanonicalExecutablePlanAfterSingleReprepare(governed);
    });
  }
  for (auto& caller : callers) caller.join();

  std::size_t leaders = 0;
  std::string replacement_uuid;
  bool passed = callback_count == 1 && fixture.executable_cache.Size() == 1;
  std::unordered_set<std::string> statement_uuids;
  for (std::size_t index = 0; index < kCallers; ++index) {
    const auto& result = results[index];
    leaders += result.reprepare.caller_was_single_flight_leader ? 1 : 0;
    if (replacement_uuid.empty()) {
      replacement_uuid = result.reprepare.replacement_prepared_plan_uuid;
    }
    passed = passed && result.accepted && result.replacement_executed &&
             !result.stale_plan_executed &&
             result.governed_reprepare_attempt_count == 1 &&
             result.reprepare.replacement_prepared_plan_uuid ==
                 replacement_uuid &&
             result.execution.selected_plan_uuid !=
                 fixture.key.selected_plan_uuid && fresh_contexts[index] &&
             executor_invocations[index] == fixture.prepared_plan->nodes.size() &&
             statement_uuids
                 .insert(result.execution.mga_statement_context.statement_uuid)
                 .second;
  }
  passed = passed && leaders == 1;
  if (!passed) {
    std::cerr << "QOW-TEST-OPT-010-CONCURRENCY-V1: callbacks="
              << callback_count << " leaders=" << leaders << " size="
              << fixture.executable_cache.Size() << '\n';
    for (std::size_t index = 0; index < kCallers; ++index) {
      std::cerr << " caller=" << index << " accepted="
                << results[index].accepted << " executed="
                << results[index].replacement_executed << " attempt="
                << results[index].governed_reprepare_attempt_count;
      if (!results[index].issues.empty()) {
        std::cerr << " issue=" << results[index].issues.front().diagnostic_id
                  << ":" << results[index].issues.front().field_id;
      }
      std::cerr << '\n';
    }
  }
  return RequireConcurrency010(
      passed,
      "race did not single-flight or preserve fresh per-statement MGA state");
}

bool ValidateThrowingLeaderNotifiesAllWaiters010() {
  Fixture009 fixture;
  if (!fixture.ready) return false;
  auto current = fixture.key;
  AdvanceCatalog010(&current);
  std::atomic<std::uint64_t> callback_count{0};
  auto request = ReprepareRequest010(&fixture, current, &callback_count);
  request.reprepare_once = [&callback_count](const auto&)
      -> cache::CanonicalExecutablePlanReprepareCandidate {
    ++callback_count;
    std::this_thread::sleep_for(std::chrono::milliseconds(20));
    throw std::runtime_error("fixture callback failure");
  };
  constexpr std::size_t kWaiters = 6;
  std::vector<cache::CanonicalExecutablePlanReprepareResult> results(kWaiters);
  std::vector<std::thread> waiters;
  for (std::size_t index = 0; index < kWaiters; ++index) {
    waiters.emplace_back([&, index] {
      results[index] =
          fixture.executable_cache.InvalidateAndReprepareOnce(request);
    });
  }
  for (auto& waiter : waiters) waiter.join();
  bool passed = callback_count == 1 && fixture.executable_cache.Size() == 0;
  for (const auto& result : results) {
    passed = passed && !result.accepted && result.total_attempt_count == 1 &&
             result.issues.size() == 1 &&
             result.issues.front().diagnostic_id ==
                 "QOW-DIAG-OPT-010-REPREPARE-REFUSAL-V1";
  }
  return RequireConcurrency010(
      passed, "throwing leader stranded, retried, or lost exact refusal");
}

bool ValidateDivergentFollowerFailsClosed010() {
  Fixture009 fixture;
  if (!fixture.ready) return false;
  auto current = fixture.key;
  AdvanceCatalog010(&current);
  std::atomic<std::uint64_t> callback_count{0};
  const auto first_request =
      ReprepareRequest010(&fixture, current, &callback_count);
  const auto first =
      fixture.executable_cache.InvalidateAndReprepareOnce(first_request);
  auto divergent = current;
  ++divergent.security_epoch;
  auto second_request =
      ReprepareRequest010(&fixture, divergent, &callback_count);
  const auto second =
      fixture.executable_cache.InvalidateAndReprepareOnce(second_request);
  return RequireConcurrency010(
      first.accepted && first.replacement_admitted && !second.accepted &&
          second.issues.size() == 1 &&
          second.issues.front().diagnostic_id ==
              "QOW-DIAG-OPT-010-REPREPARE-REFUSAL-V1" &&
          second.issues.front().field_id ==
              "divergent_current_dependency_state" &&
          callback_count == 1,
      "divergent follower inherited an obsolete leader replacement");
}

bool ValidateConcurrencyAuthorityRefusals010() {
  Fixture009 fixture;
  if (!fixture.ready) return false;
  auto current = fixture.key;
  AdvanceCatalog010(&current);
  std::atomic<std::uint64_t> callback_count{0};
  const auto base_reprepare =
      ReprepareRequest010(&fixture, current, &callback_count);
  const auto expect_refusal = [&](const auto& mutate,
                                  const std::string_view detail) {
    std::size_t executor_invocations = 0;
    bool fresh_context = true;
    cache::CanonicalExecutablePlanGovernedExecutionRequest governed;
    governed.executable_plan_cache = &fixture.executable_cache;
    governed.reprepare = base_reprepare;
    governed.engine_execution_authorized = true;
    governed.build_replacement_execution = [&](const auto& replacement) {
      return ExecutionRequest009(&fixture, replacement->key,
                                 FreshStatement009(1960),
                                 &executor_invocations, &fresh_context);
    };
    mutate(governed);
    const auto result =
        api::ExecuteCanonicalExecutablePlanAfterSingleReprepare(governed);
    return RequireConcurrency010(
        !result.accepted && !result.replacement_executed &&
            !result.stale_plan_executed &&
            result.governed_reprepare_attempt_count == 0 &&
            result.issues.size() == 1 &&
            result.issues.front().diagnostic_id ==
                "QOW-DIAG-OPT-010-CONCURRENCY-REFUSAL-V1" &&
            result.issues.front().field_id ==
                "engine_owned_statement_boundary" &&
            callback_count == 0 && fixture.executable_cache.Size() == 1,
        detail);
  };

  bool passed = true;
  passed &= expect_refusal(
      [](auto& request) { request.engine_execution_authorized = false; },
      "missing engine execution authority reached invalidation");
  passed &= expect_refusal(
      [](auto& request) { request.parser_execution_authority_claimed = true; },
      "parser execution authority reached invalidation");
  passed &= expect_refusal(
      [](auto& request) {
        request.transaction_finality_authority_claimed = true;
      },
      "transaction finality authority reached invalidation");
  passed &= expect_refusal(
      [](auto& request) { request.recovery_authority_claimed = true; },
      "recovery authority reached invalidation");
  return passed;
}

}  // namespace

// QOW-TEST-OPT-010-CONCURRENCY-V1
int main() {
  return ValidateConcurrentSingleFlightAndFreshStatements010() &&
                 ValidateThrowingLeaderNotifiesAllWaiters010() &&
                 ValidateDivergentFollowerFailsClosed010() &&
                 ValidateConcurrencyAuthorityRefusals010()
             ? EXIT_SUCCESS
             : EXIT_FAILURE;
}
