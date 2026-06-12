// Copyright (c) 2026 ScratchBird Software Inc.
//
// This Source Code Form is subject to the terms of the Mozilla Public
// License, v. 2.0. If a copy of the MPL was not distributed with this
// file, You can obtain one at https://mozilla.org/MPL/2.0/.
//
// SPDX-License-Identifier: MPL-2.0

#include "memory.hpp"

#include <cstdlib>
#include <iostream>
#include <string>
#include <string_view>

namespace {

namespace mem = scratchbird::core::memory;
namespace platform = scratchbird::core::platform;

[[noreturn]] void Fail(std::string_view message) {
  std::cerr << message << '\n';
  std::exit(EXIT_FAILURE);
}

void Require(bool condition, std::string_view message) {
  if (!condition) {
    Fail(message);
  }
}

mem::AllocationPolicy Policy(std::string_view name = "mmch_014_failure_injection") {
  mem::AllocationPolicy policy;
  policy.policy_name = std::string(name);
  policy.hard_limit_bytes = 8ull * 1024ull * 1024ull;
  policy.soft_limit_bytes = 8ull * 1024ull * 1024ull;
  policy.per_context_limit_bytes = 8ull * 1024ull * 1024ull;
  policy.page_buffer_pool_limit_bytes = 4ull * 1024ull * 1024ull;
  policy.track_allocations = true;
  policy.zero_memory_on_release = true;
  return policy;
}

mem::MemoryTag Tag(std::string_view purpose,
                   std::string_view context,
                   mem::MemoryCategory category = mem::MemoryCategory::test_probe,
                   std::string_view callsite = "") {
  mem::MemoryTag tag;
  tag.purpose = std::string(purpose);
  tag.callsite = std::string(callsite);
  tag.category = category;
  tag.lifetime = mem::MemoryLifetime::temporary;
  tag.owner = "owner-mmch-014";
  tag.context_id = std::string(context);
  tag.database_id = "db-mmch-014";
  tag.session_id = "session-mmch-014";
  tag.transaction_id = "txn-mmch-014";
  tag.statement_id = std::string(context);
  tag.query_id = "query-mmch-014";
  return tag;
}

bool DiagnosticArgEquals(const platform::DiagnosticRecord& diagnostic,
                         std::string_view key,
                         std::string_view value) {
  for (const auto& arg : diagnostic.arguments) {
    if (arg.key == key && arg.value == value) {
      return true;
    }
  }
  return false;
}

const mem::MemoryFailureInjectionRuleSnapshot* FindRule(
    const mem::MemoryFailureInjectionSnapshot& snapshot,
    std::string_view rule_id) {
  for (const auto& rule : snapshot.rules) {
    if (rule.rule.rule_id == rule_id) {
      return &rule;
    }
  }
  return nullptr;
}

mem::MemoryFailureInjectionRule Rule(std::string_view rule_id,
                                     std::uint64_t fail_on_matched_sequence) {
  mem::MemoryFailureInjectionRule rule;
  rule.rule_id = std::string(rule_id);
  rule.fail_on_matched_sequence = fail_on_matched_sequence;
  return rule;
}

mem::MemoryFailureInjectionConfiguration Config(mem::MemoryFailureInjectionRule rule) {
  mem::MemoryFailureInjectionConfiguration configuration;
  configuration.test_guard = mem::MakeMemoryFailureInjectionTestGuard();
  configuration.fixture_enabled = true;
  configuration.fixture_name = "mmch_014_allocation_failure_injection_gate";
  configuration.evidence_note =
      "allocator_test_evidence_only_not_transaction_visibility_security_recovery_parser_reference_or_benchmark_authority";
  configuration.rules.push_back(std::move(rule));
  return configuration;
}

void RequireInjected(const mem::AllocationResult& result, std::string_view rule_id) {
  Require(!result.ok(), "MMCH-014 injected allocation unexpectedly succeeded");
  Require(result.status.code == platform::StatusCode::memory_allocation_failed,
          "MMCH-014 injected allocation status code changed");
  Require(result.diagnostic.diagnostic_code == "SB-MEMORY-ALLOC-FAILURE-INJECTED",
          "MMCH-014 injected allocation diagnostic code changed");
  Require(DiagnosticArgEquals(result.diagnostic, "failure_injection_rule_id", rule_id),
          "MMCH-014 injected diagnostic missing rule id");
  Require(DiagnosticArgEquals(
              result.diagnostic,
              "failure_injection_authority_scope",
              "test_fixture_evidence_only_not_transaction_finality_visibility_security_recovery_parser_reference_or_benchmark_authority"),
          "MMCH-014 injected diagnostic authority scope changed");
}

void NoHookByDefault() {
  mem::MemoryManager manager(Policy("mmch_014_default_off"));
  Require(!manager.FailureInjectionSnapshot().enabled,
          "MMCH-014 failure injection should be disabled by default");

  const auto allocation = manager.Allocate(128, 0, Tag("default_off", "ctx-default"));
  Require(allocation.ok(), "MMCH-014 default-off allocation failed");
  Require(manager.Deallocate(allocation.pointer, Tag("default_off", "ctx-default")).ok(),
          "MMCH-014 default-off release failed");
  Require(manager.Snapshot().failure_count == 0,
          "MMCH-014 default-off path recorded an unexpected failure");
}

void ProductionBlockedEnablementAndRefusal() {
  mem::MemoryManager manager(Policy("mmch_014_blocked_enablement"));
  auto blocked_rule = Rule("production_blocked", 1);
  mem::MemoryFailureInjectionConfiguration blocked;
  blocked.fixture_enabled = true;
  blocked.fixture_name = "mmch_014_missing_compile_guard";
  blocked.rules.push_back(blocked_rule);

  const auto blocked_result = manager.EnableAllocationFailureInjection(blocked);
  Require(!blocked_result.ok(), "MMCH-014 enablement without test guard was accepted");
  Require(blocked_result.blocked, "MMCH-014 blocked enablement did not mark result blocked");
  Require(blocked_result.diagnostic.diagnostic_code == "SB-MEMORY-FAILURE-INJECTION-ENABLE-BLOCKED",
          "MMCH-014 missing-guard diagnostic code changed");
  Require(!manager.FailureInjectionSnapshot().enabled,
          "MMCH-014 missing-guard enablement changed allocator state");

  auto missing_fixture = Config(blocked_rule);
  missing_fixture.fixture_enabled = false;
  missing_fixture.fixture_name.clear();
  const auto missing_fixture_result = manager.EnableAllocationFailureInjection(missing_fixture);
  Require(!missing_fixture_result.ok(), "MMCH-014 enablement without fixture was accepted");
  Require(missing_fixture_result.diagnostic.diagnostic_code == "SB-MEMORY-FAILURE-INJECTION-FIXTURE-REQUIRED",
          "MMCH-014 missing-fixture diagnostic code changed");
  Require(!manager.FailureInjectionSnapshot().enabled,
          "MMCH-014 missing-fixture enablement changed allocator state");

  const auto allocation = manager.Allocate(64, 0, Tag("production_blocked", "ctx-blocked"));
  Require(allocation.ok(), "MMCH-014 blocked enablement still injected failure");
  Require(manager.Deallocate(allocation.pointer, Tag("production_blocked", "ctx-blocked")).ok(),
          "MMCH-014 blocked-enable release failed");
}

void SequenceBasedFailure() {
  mem::MemoryManager manager(Policy("mmch_014_sequence"));
  auto rule = Rule("sequence_second_match", 2);
  const auto enabled = manager.EnableAllocationFailureInjection(Config(rule));
  Require(enabled.ok(), "MMCH-014 sequence failure injection enablement failed");

  const auto first = manager.Allocate(64, 0, Tag("sequence", "ctx-sequence"));
  Require(first.ok(), "MMCH-014 first sequence allocation failed");
  const auto second = manager.Allocate(64, 0, Tag("sequence", "ctx-sequence"));
  RequireInjected(second, "sequence_second_match");
  Require(DiagnosticArgEquals(second.diagnostic, "failure_injection_matched_sequence", "2"),
          "MMCH-014 sequence diagnostic missing matched sequence");

  const auto third = manager.Allocate(64, 0, Tag("sequence", "ctx-sequence"));
  Require(third.ok(), "MMCH-014 sequence rule failed more than once");

  const auto snapshot = manager.FailureInjectionSnapshot();
  const auto* rule_snapshot = FindRule(snapshot, "sequence_second_match");
  Require(rule_snapshot != nullptr, "MMCH-014 sequence rule snapshot missing");
  Require(snapshot.observed_allocation_sequence == 3,
          "MMCH-014 observed allocation sequence mismatch");
  Require(rule_snapshot->matched_sequence == 3,
          "MMCH-014 matched sequence snapshot mismatch");
  Require(rule_snapshot->failure_count == 1,
          "MMCH-014 sequence failure count mismatch");

  Require(manager.Deallocate(first.pointer, Tag("sequence", "ctx-sequence")).ok(),
          "MMCH-014 first sequence release failed");
  Require(manager.Deallocate(third.pointer, Tag("sequence", "ctx-sequence")).ok(),
          "MMCH-014 third sequence release failed");
}

void CategoryContextPurposeMatching() {
  mem::MemoryManager manager(Policy("mmch_014_matching"));
  auto rule = Rule("category_context_purpose", 1);
  rule.purpose = "target-purpose";
  rule.category = mem::MemoryCategory::diagnostics;
  rule.scope_kind = mem::MemoryFailureInjectionScopeKind::context;
  rule.scope_id = "ctx-target";
  const auto enabled = manager.EnableAllocationFailureInjection(Config(rule));
  Require(enabled.ok(), "MMCH-014 matching failure injection enablement failed");

  const auto wrong_category = manager.Allocate(
      32, 0, Tag("target-purpose", "ctx-target", mem::MemoryCategory::test_probe));
  Require(wrong_category.ok(), "MMCH-014 category mismatch injected unexpectedly");

  const auto wrong_purpose = manager.Allocate(
      32, 0, Tag("other-purpose", "ctx-target", mem::MemoryCategory::diagnostics));
  Require(wrong_purpose.ok(), "MMCH-014 purpose mismatch injected unexpectedly");

  const auto wrong_context = manager.Allocate(
      32, 0, Tag("target-purpose", "ctx-other", mem::MemoryCategory::diagnostics));
  Require(wrong_context.ok(), "MMCH-014 context mismatch injected unexpectedly");

  const auto matched = manager.Allocate(
      32, 0, Tag("target-purpose", "ctx-target", mem::MemoryCategory::diagnostics));
  RequireInjected(matched, "category_context_purpose");
  Require(DiagnosticArgEquals(matched.diagnostic, "purpose", "target-purpose"),
          "MMCH-014 matching diagnostic missing purpose");
  Require(DiagnosticArgEquals(matched.diagnostic, "category", "diagnostics"),
          "MMCH-014 matching diagnostic missing category");
  Require(DiagnosticArgEquals(matched.diagnostic, "context_id", "ctx-target"),
          "MMCH-014 matching diagnostic missing context id");

  const auto snapshot = manager.FailureInjectionSnapshot();
  const auto* rule_snapshot = FindRule(snapshot, "category_context_purpose");
  Require(rule_snapshot != nullptr && rule_snapshot->matched_sequence == 1,
          "MMCH-014 nonmatching allocations advanced matched sequence");

  Require(manager.Deallocate(wrong_category.pointer, Tag("target-purpose", "ctx-target")).ok(),
          "MMCH-014 wrong-category release failed");
  Require(manager.Deallocate(wrong_purpose.pointer, Tag("other-purpose", "ctx-target")).ok(),
          "MMCH-014 wrong-purpose release failed");
  Require(manager.Deallocate(wrong_context.pointer, Tag("target-purpose", "ctx-other")).ok(),
          "MMCH-014 wrong-context release failed");
}

void PageBufferInjection() {
  mem::MemoryManager manager(Policy("mmch_014_page_buffer"));
  auto rule = Rule("page_buffer_first_match", 1);
  rule.callsite = "core.memory.page_buffer.allocate";
  rule.purpose = "page-buffer-target";
  rule.category = mem::MemoryCategory::page_buffer;
  rule.scope_kind = mem::MemoryFailureInjectionScopeKind::context;
  rule.scope_id = "ctx-page-buffer";
  const auto enabled = manager.EnableAllocationFailureInjection(Config(rule));
  Require(enabled.ok(), "MMCH-014 page-buffer injection enablement failed");

  mem::PageBufferRequest request;
  request.page_size = 4096;
  request.page_count = 2;
  request.tag = Tag("page-buffer-target", "ctx-page-buffer");
  const auto page_buffer = manager.AllocatePageBuffer(request);
  Require(!page_buffer.ok(), "MMCH-014 page-buffer allocation was not injected");
  Require(page_buffer.diagnostic.diagnostic_code == "SB-MEMORY-ALLOC-FAILURE-INJECTED",
          "MMCH-014 page-buffer diagnostic code changed");
  Require(DiagnosticArgEquals(page_buffer.diagnostic, "callsite", "core.memory.page_buffer.allocate"),
          "MMCH-014 page-buffer diagnostic missing callsite");
  Require(DiagnosticArgEquals(page_buffer.diagnostic, "category", "page_buffer"),
          "MMCH-014 page-buffer diagnostic missing category");
  Require(manager.Snapshot().page_buffer_current_bytes == 0,
          "MMCH-014 injected page-buffer allocation leaked page-buffer bytes");

  const auto snapshot = manager.FailureInjectionSnapshot();
  const auto* rule_snapshot = FindRule(snapshot, "page_buffer_first_match");
  Require(rule_snapshot != nullptr && rule_snapshot->failure_count == 1,
          "MMCH-014 page-buffer failure count mismatch");
}

}  // namespace

int main() {
  std::cout << "MMCH-014 authority_note=allocation_failure_injection_evidence_only;"
               "not_transaction_finality_visibility_security_recovery_parser_reference_or_benchmark_authority"
            << '\n';
  NoHookByDefault();
  ProductionBlockedEnablementAndRefusal();
  SequenceBasedFailure();
  CategoryContextPurposeMatching();
  PageBufferInjection();
  return EXIT_SUCCESS;
}
