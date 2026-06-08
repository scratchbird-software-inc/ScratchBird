// Copyright (c) 2026 ScratchBird Software Inc.
//
// This Source Code Form is subject to the terms of the Mozilla Public
// License, v. 2.0. If a copy of the MPL was not distributed with this
// file, You can obtain one at https://mozilla.org/MPL/2.0/.
//
// SPDX-License-Identifier: MPL-2.0

#include "common/function_result_helpers.hpp"
#include "dispatch/function_dispatch.hpp"
#include "registry/function_seed_registry.hpp"
#include "sblr/sblr_runtime.hpp"

#include <atomic>
#include <cstdint>
#include <iostream>
#include <mutex>
#include <string>
#include <string_view>
#include <thread>
#include <vector>

namespace fn = scratchbird::engine::functions;
namespace sblr = scratchbird::engine::sblr;

namespace {

std::string JsonEscape(std::string_view text) {
  std::string out;
  for (const char ch : text) {
    switch (ch) {
      case '"': out += "\\\""; break;
      case '\\': out += "\\\\"; break;
      case '\n': out += "\\n"; break;
      default: out.push_back(ch); break;
    }
  }
  return out;
}

sblr::SblrValue BoolValue(bool value) {
  sblr::SblrValue out;
  out.descriptor_id = "boolean";
  out.payload_kind = sblr::SblrValuePayloadKind::boolean;
  out.is_null = false;
  out.has_int64_value = true;
  out.int64_value = value ? 1 : 0;
  out.text_value = value ? "true" : "false";
  return out;
}

fn::FunctionCallRequest Request(std::uint32_t thread_index,
                                std::string function_id,
                                std::vector<sblr::SblrValue> values = {}) {
  fn::FunctionCallRequest request;
  request.context.function_id = std::move(function_id);
  request.context.security_allowed = true;
  request.context.policy_allowed = true;
  request.context.dependency_available = true;
  request.context.sblr_context.database_uuid = "018f0000-0000-7000-8000-00000000d131";
  request.context.sblr_context.statement_uuid = "018f0000-0000-7000-8000-00000000a131";
  request.context.sblr_context.user_uuid = "018f0000-0000-7000-8000-00000000u" + std::to_string(100 + thread_index);
  request.context.sblr_context.transaction_uuid = "018f0000-0000-7000-8000-00000000t" + std::to_string(100 + thread_index);
  request.context.sblr_context.current_timestamp = "2026-05-03T18:31:" + std::to_string(10 + thread_index) + "Z";
  request.context.sblr_context.statement_timestamp = request.context.sblr_context.current_timestamp;
  request.context.sblr_context.last_row_count = 1000 + thread_index;
  request.context.sblr_context.last_row_count_present = true;
  request.context.sblr_context.security_snapshot_uuid = "018f0000-0000-7000-8000-00000000s131";
  request.context.sblr_context.security_context_present = true;
  request.context.sblr_context.transaction_context_present = true;
  for (std::size_t index = 0; index < values.size(); ++index) {
    request.arguments.push_back(fn::FunctionArgument{"arg" + std::to_string(index), std::move(values[index])});
  }
  return request;
}

bool TextScalarEquals(const fn::FunctionCallResult& result, std::string_view expected) {
  return result.result.status == sblr::SblrStatusCode::ok && result.result.diagnostics.empty() &&
         result.result.scalar_values.size() == 1 && fn::ValueAsText(result.result.scalar_values.front()) == expected;
}

bool UintScalarEquals(const fn::FunctionCallResult& result, std::uint64_t expected) {
  return result.result.status == sblr::SblrStatusCode::ok && result.result.diagnostics.empty() &&
         result.result.scalar_values.size() == 1 && result.result.scalar_values.front().has_uint64_value &&
         result.result.scalar_values.front().uint64_value == expected;
}

void AddFailure(std::vector<std::string>* failures, std::mutex* mutex, std::string failure) {
  std::lock_guard<std::mutex> lock(*mutex);
  failures->push_back(std::move(failure));
}

void RunWorker(const fn::FunctionRegistry* registry,
               std::uint32_t thread_index,
               std::uint32_t iterations,
               std::atomic<std::uint64_t>* completed_calls,
               std::vector<std::string>* failures,
               std::mutex* failure_mutex) {
  for (std::uint32_t iteration = 0; iteration < iterations; ++iteration) {
    const auto user = fn::DispatchFunctionCall(*registry, Request(thread_index, "data.scalar.current_user"));
    const auto tx = fn::DispatchFunctionCall(*registry, Request(thread_index, "data.scalar.current_transaction"));
    const auto now = fn::DispatchFunctionCall(*registry, Request(thread_index, "data.scalar.now"));
    const auto row_count = fn::DispatchFunctionCall(*registry, Request(thread_index, "data.scalar.row_count"));
    const auto coalesce = fn::DispatchFunctionCall(
        *registry,
        Request(thread_index,
                "data.scalar.coalesce",
                {fn::MakeNullValue("text"), fn::MakeTextValue("text", "worker-" + std::to_string(thread_index))}));
    const auto branch = fn::DispatchFunctionCall(
        *registry,
        Request(thread_index,
                "data.scalar.if",
                {BoolValue((iteration % 2) == 0), fn::MakeTextValue("text", "even"), fn::MakeTextValue("text", "odd")}));

    const std::string expected_user = "018f0000-0000-7000-8000-00000000u" + std::to_string(100 + thread_index);
    const std::string expected_tx = "018f0000-0000-7000-8000-00000000t" + std::to_string(100 + thread_index);
    const std::string expected_now = "2026-05-03T18:31:" + std::to_string(10 + thread_index) + "Z";
    const std::string expected_branch = (iteration % 2) == 0 ? "even" : "odd";
    if (!TextScalarEquals(user, expected_user)) AddFailure(failures, failure_mutex, "current_user context bleed");
    if (!TextScalarEquals(tx, expected_tx)) AddFailure(failures, failure_mutex, "current_transaction context bleed");
    if (!TextScalarEquals(now, expected_now)) AddFailure(failures, failure_mutex, "current timestamp context bleed");
    if (!UintScalarEquals(row_count, 1000 + thread_index)) AddFailure(failures, failure_mutex, "row_count context bleed");
    if (!TextScalarEquals(coalesce, "worker-" + std::to_string(thread_index))) AddFailure(failures, failure_mutex, "coalesce result mismatch");
    if (!TextScalarEquals(branch, expected_branch)) AddFailure(failures, failure_mutex, "if branch result mismatch");

    sblr::SblrFrameStack stack;
    stack.max_depth = 2;
    sblr::SblrResult failure;
    if (!sblr::PushSblrFrame(&stack, sblr::SblrFrame{.frame_uuid = "thread-frame-1"}, &failure)) {
      AddFailure(failures, failure_mutex, "thread-local frame push failed");
    }
    if (!sblr::PopSblrFrame(&stack, &failure)) {
      AddFailure(failures, failure_mutex, "thread-local frame pop failed");
    }
    completed_calls->fetch_add(7, std::memory_order_relaxed);
  }
}

}  // namespace

int main() {
  constexpr std::uint32_t kThreadCount = 8;
  constexpr std::uint32_t kIterations = 64;
  auto seeds = fn::BuildStandardFunctionSeedPackage();
  const auto before_entries = seeds.registry.Entries();
  const auto before_count = before_entries.size();

  std::atomic<std::uint64_t> completed_calls{0};
  std::vector<std::string> failures;
  std::mutex failure_mutex;
  std::vector<std::thread> workers;
  workers.reserve(kThreadCount);
  for (std::uint32_t index = 0; index < kThreadCount; ++index) {
    workers.emplace_back(RunWorker, &seeds.registry, index, kIterations, &completed_calls, &failures, &failure_mutex);
  }
  for (auto& worker : workers) worker.join();

  const auto after_entries = seeds.registry.Entries();
  const bool registry_count_stable = before_count == after_entries.size();
  if (!registry_count_stable) failures.push_back("registry entry count changed during concurrent reads");
  for (const auto& entry : before_entries) {
    const auto* after = seeds.registry.Lookup(entry.function_id);
    if (after == nullptr || after->function_uuid != entry.function_uuid ||
        after->implementation_state != entry.implementation_state ||
        after->optimizer_metadata.descriptor_rule != entry.optimizer_metadata.descriptor_rule) {
      failures.push_back("registry entry mutated: " + entry.function_id);
    }
  }

  const auto expected_calls = static_cast<std::uint64_t>(kThreadCount) * kIterations * 7;
  if (completed_calls.load(std::memory_order_relaxed) != expected_calls) {
    failures.push_back("completed call count mismatch");
  }

  std::cout << "{\n";
  std::cout << "  \"ok\": " << (failures.empty() ? "true" : "false") << ",\n";
  std::cout << "  \"threads\": " << kThreadCount << ",\n";
  std::cout << "  \"iterations_per_thread\": " << kIterations << ",\n";
  std::cout << "  \"completed_calls\": " << completed_calls.load(std::memory_order_relaxed) << ",\n";
  std::cout << "  \"registry_count\": " << after_entries.size() << ",\n";
  std::cout << "  \"failures\": [";
  for (std::size_t i = 0; i < failures.size(); ++i) {
    if (i != 0) std::cout << ", ";
    std::cout << "\"" << JsonEscape(failures[i]) << "\"";
  }
  std::cout << "]\n";
  std::cout << "}\n";
  return failures.empty() ? 0 : 1;
}
