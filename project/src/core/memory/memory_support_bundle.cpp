// Copyright (c) 2026 ScratchBird Software Inc.
//
// This Source Code Form is subject to the terms of the Mozilla Public
// License, v. 2.0. If a copy of the MPL was not distributed with this
// file, You can obtain one at https://mozilla.org/MPL/2.0/.
//
// SPDX-License-Identifier: MPL-2.0

#include "memory_support_bundle.hpp"

#include <algorithm>
#include <cctype>
#include <cstdio>
#include <cstring>
#include <iomanip>
#include <iterator>
#include <sstream>
#include <utility>

namespace scratchbird::core::memory {
namespace {

using scratchbird::core::platform::Severity;
using scratchbird::core::platform::StatusCode;
using scratchbird::core::platform::Subsystem;

constexpr const char* kAuthorityBoundary =
    "memory_support_bundle.authority_scope=evidence_only_not_transaction_finality_visibility_security_recovery_parser_reference_wal_or_benchmark_authority";
constexpr const char* kExpandedAuthorityBoundary =
    "memory_support_bundle.authority_scope=evidence_only_not_transaction_finality_visibility_security_authorization_recovery_parser_reference_wal_benchmark_optimizer_plan_index_finality_or_agent_action_authority";
constexpr const char* kProtectedExcluded = "<protected-material-excluded>";
constexpr const char* kTruncatedSuffix = "...<truncated>";

static_assert(std::char_traits<char>::length(kExpandedAuthorityBoundary) <
                  MemorySupportBundleEmergencySummary{}.authority_scope.size(),
              "emergency summary authority scope buffer must hold the full boundary");

Status OkStatus() {
  return {StatusCode::ok, Severity::info, Subsystem::memory};
}

std::string Lower(std::string value) {
  std::transform(value.begin(), value.end(), value.begin(), [](unsigned char ch) {
    return static_cast<char>(std::tolower(ch));
  });
  return value;
}

bool LooksProtected(std::string_view value) {
  const auto lower = Lower(std::string(value));
  const char* needles[] = {"secret", "password", "passwd", "pwd=", "token",
                           "private_key", "credential", "verifier", "seed",
                           "cleartext", "plaintext", "api_key", "apikey",
                           "key_material", "raw_key", "kms_plaintext",
                           "bearer ", "hsm", "kms", "protected_reference"};
  for (const char* needle : needles) {
    if (lower.find(needle) != std::string::npos) {
      return true;
    }
  }
  return false;
}

bool LooksProtectedNoAlloc(std::string_view value) {
  constexpr std::string_view needles[] = {
      "secret", "password", "passwd", "pwd=", "token", "private_key",
      "credential", "verifier", "seed", "cleartext", "plaintext", "api_key",
      "apikey", "key_material", "raw_key", "kms_plaintext", "bearer ",
      "hsm", "kms", "protected_reference"};
  for (std::size_t i = 0; i < value.size(); ++i) {
    for (std::string_view needle : needles) {
      if (i + needle.size() > value.size()) {
        continue;
      }
      bool match = true;
      for (std::size_t j = 0; j < needle.size(); ++j) {
        const auto ch =
            static_cast<char>(std::tolower(static_cast<unsigned char>(value[i + j])));
        if (ch != needle[j]) {
          match = false;
          break;
        }
      }
      if (match) {
        return true;
      }
    }
  }
  return false;
}

std::string DigestRow(std::string_view key,
                      std::string_view value,
                      std::string_view redaction_class,
                      bool redacted) {
  u64 hash = 1469598103934665603ull;
  auto mix = [&hash](std::string_view text) {
    for (unsigned char ch : text) {
      hash ^= static_cast<u64>(ch);
      hash *= 1099511628211ull;
    }
  };
  mix("MMCH_MEMORY_SUPPORT_BUNDLE_EVIDENCE");
  mix(key);
  mix(value);
  mix(redaction_class);
  mix(redacted ? "redacted" : "plain");
  std::ostringstream out;
  out << "fnv64-evidence-v1:" << std::hex << std::setw(16)
      << std::setfill('0') << hash;
  return out.str();
}

std::string TruncateForBundle(std::string_view value, u64 max_bytes) {
  if (max_bytes == 0) {
    return {};
  }
  if (value.size() <= max_bytes) {
    return std::string(value);
  }
  const std::string_view suffix = kTruncatedSuffix;
  if (max_bytes <= suffix.size()) {
    return std::string(value.substr(0, static_cast<std::size_t>(max_bytes)));
  }
  std::string output(value.substr(
      0, static_cast<std::size_t>(max_bytes - suffix.size())));
  output.append(suffix);
  return output;
}

u64 RowSizeEstimate(std::string_view key, std::string_view value) {
  return static_cast<u64>(key.size() + value.size() + 64);
}

MemorySupportBundleLimits NormalizeLimits(MemorySupportBundleLimits limits,
                                          bool low_memory_mode) {
  if (limits.max_rows == 0) {
    limits.max_rows = 1;
  }
  if (limits.max_output_bytes == 0) {
    limits.max_output_bytes = 1;
  }
  if (limits.max_key_bytes == 0) {
    limits.max_key_bytes = 1;
  }
  if (limits.max_value_bytes == 0) {
    limits.max_value_bytes = 1;
  }
  if (low_memory_mode) {
    limits.max_rows = std::min<u64>(limits.max_rows, 96);
    limits.max_output_bytes =
        std::min<u64>(limits.max_output_bytes, 12ull * 1024ull);
    limits.max_key_bytes = std::min<u64>(limits.max_key_bytes, 96);
    limits.max_value_bytes = std::min<u64>(limits.max_value_bytes, 160);
    limits.max_diagnostic_arguments =
        std::min<u64>(limits.max_diagnostic_arguments, 8);
    limits.max_metrics = std::min<u64>(limits.max_metrics, 16);
    limits.max_pressure_transitions =
        std::min<u64>(limits.max_pressure_transitions, 8);
    limits.max_allocation_classes =
        std::min<u64>(limits.max_allocation_classes, 8);
  }
  return limits;
}

bool BoundedAppendRow(MemorySupportBundleResult* result,
                      MemorySupportBundleRow row,
                      const MemorySupportBundleLimits& limits) {
  const u64 row_bytes = RowSizeEstimate(row.key, row.value);
  if (result->rows.size() >= limits.max_rows ||
      result->output_bytes > limits.max_output_bytes ||
      row_bytes > limits.max_output_bytes - result->output_bytes) {
    ++result->dropped_row_count;
    return false;
  }
  result->output_bytes += row_bytes;
  result->rows.push_back(std::move(row));
  return true;
}

MemorySupportBundleRow MakeRow(std::string key,
                               std::string_view value,
                               bool allow_protected_material,
                               bool exclude_protected_material,
                               const MemorySupportBundleLimits& limits,
                               const char* redaction_class = "public") {
  MemorySupportBundleRow row;
  row.key = TruncateForBundle(key, limits.max_key_bytes);
  const bool protected_value = LooksProtected(row.key) || LooksProtected(value);
  row.redacted = protected_value && (!allow_protected_material || exclude_protected_material);
  row.redaction_class = row.redacted ? "protected_material" : redaction_class;
  if (row.redacted && exclude_protected_material) {
    row.value = kProtectedExcluded;
  } else if (row.redacted) {
    row.value = "<redacted>";
  } else {
    row.value = TruncateForBundle(value, limits.max_value_bytes);
  }
  row.tamper_evidence_digest =
      DigestRow(row.key, row.value, row.redaction_class, row.redacted);
  return row;
}

bool AddRow(MemorySupportBundleResult* result,
            std::string key,
            std::string_view value,
            bool allow_protected_material,
            bool exclude_protected_material,
            const MemorySupportBundleLimits& limits,
            const char* redaction_class = "public") {
  auto row = MakeRow(std::move(key),
                     value,
                     allow_protected_material,
                     exclude_protected_material,
                     limits,
                     redaction_class);
  const bool redacted = row.redacted;
  const bool appended = BoundedAppendRow(result, std::move(row), limits);
  if (appended && redacted) {
    ++result->redacted_row_count;
  }
  return appended;
}

Status InvalidRequestStatus() {
  return {StatusCode::memory_invalid_request, Severity::error, Subsystem::memory};
}

bool ProtectedReviewPasses(const ProtectedMemorySecurityReview& review,
                           std::vector<std::string>* evidence) {
  if (!review.enabled) {
    evidence->push_back("memory_support_bundle.protected_memory_review.enabled=false");
    return true;
  }
  bool ok = true;
  auto mark = [&](std::string key, bool value) {
    evidence->push_back(std::move(key) + "=" + (value ? "true" : "false"));
    ok = ok && value;
  };
  evidence->push_back("CEIC-023_PROTECTED_MEMORY_SECURITY_REVIEW");
  mark("memory_support_bundle.protected_scan.diagnostics_logs_exceptions_complete",
       review.diagnostics_log_exception_scan_complete);
  mark("memory_support_bundle.protected_scan.support_bundle_complete",
       review.support_bundle_scan_complete);
  mark("memory_support_bundle.protected_zeroization.not_optimized_away",
       review.zeroization_not_optimized_away);
  mark("memory_support_bundle.protected_zeroization.zero_on_release_required",
       review.protected_buffer_zero_on_release);
  mark("memory_support_bundle.protected_secret_routes.protected_buffers",
       review.hsm_kms_plugin_routes_use_protected_buffers);
  mark("memory_support_bundle.protected_material.exclusion_required",
       review.require_protected_material_exclusion);

  bool platform_attempts_present = !review.require_platform_protection_attempts;
  for (const auto& protected_evidence : review.protected_buffer_evidence) {
    platform_attempts_present =
        platform_attempts_present ||
        (protected_evidence.platform_lock_attempted &&
         protected_evidence.no_dump_attempted &&
         protected_evidence.protected_material_redacted &&
         protected_evidence.zero_on_release);
  }
  mark("memory_support_bundle.protected_platform.lock_no_dump_attempt_evidence",
       platform_attempts_present);

  bool routes_safe = true;
  for (const auto& route : review.secret_routes) {
    routes_safe = routes_safe && route.routed_through_protected_buffer &&
                  route.protected_reference_only &&
                  !route.plaintext_material_observed;
  }
  mark("memory_support_bundle.protected_secret_routes.no_plaintext_route",
       routes_safe);
  return ok;
}

void SetEmergencyRow(MemorySupportBundleEmergencyRow* row,
                     std::string_view key,
                     std::string_view value,
                     bool redacted) {
  std::snprintf(row->key.data(), row->key.size(), "%.*s",
                static_cast<int>(std::min<std::size_t>(key.size(), row->key.size() - 1)),
                key.data());
  std::snprintf(row->value.data(), row->value.size(), "%.*s",
                static_cast<int>(std::min<std::size_t>(value.size(), row->value.size() - 1)),
                value.data());
  row->redacted = redacted;
}

bool AddEmergencyRow(MemorySupportBundleEmergencySummary* summary,
                     std::string_view key,
                     std::string_view value) {
  if (summary->row_count >= summary->max_rows ||
      summary->row_count >= summary->rows.size()) {
    ++summary->dropped_row_count;
    return false;
  }
  const bool redacted = LooksProtectedNoAlloc(key) || LooksProtectedNoAlloc(value);
  SetEmergencyRow(&summary->rows[static_cast<std::size_t>(summary->row_count)],
                  key,
                  redacted ? kProtectedExcluded : value,
                  redacted);
  ++summary->row_count;
  return true;
}

}  // namespace

std::string RedactMemorySupportBundleValue(std::string value,
                                           bool allow_protected_material) {
  if (!allow_protected_material && LooksProtected(value)) {
    return kProtectedExcluded;
  }
  return value;
}

const char* MemorySupportBundleModeName(MemorySupportBundleMode mode) {
  switch (mode) {
    case MemorySupportBundleMode::standard:
      return "standard";
    case MemorySupportBundleMode::low_memory:
      return "low_memory";
    case MemorySupportBundleMode::emergency_summary:
      return "emergency_summary";
  }
  return "standard";
}

MemorySupportBundleEmergencySummary BuildMemorySupportBundleEmergencySummary(
    const MemorySupportBundleRequest& request) {
  MemorySupportBundleEmergencySummary summary;
  summary.emitted = true;
  std::snprintf(summary.authority_scope.data(),
                summary.authority_scope.size(),
                "%s",
                kExpandedAuthorityBoundary);

  auto add_number = [&summary](std::string_view key, u64 value) {
    char buffer[32] = {};
    std::snprintf(buffer, sizeof(buffer), "%llu",
                  static_cast<unsigned long long>(value));
    AddEmergencyRow(&summary, key, buffer);
  };

  AddEmergencyRow(&summary, "mode", MemorySupportBundleModeName(request.mode));
  add_number("snapshot.current_bytes", request.snapshot.current_bytes);
  add_number("snapshot.peak_bytes", request.snapshot.peak_bytes);
  add_number("snapshot.failure_count", request.snapshot.failure_count);
  add_number("snapshot.policy_rejection_count",
             request.snapshot.policy_rejection_count);
  add_number("snapshot.leak_candidate_count",
             request.snapshot.leak_candidate_count);

  const MemoryContextSnapshot* selected[4] = {};
  std::size_t selected_count = 0;
  for (const auto& context : request.snapshot.contexts) {
    std::size_t insert_at = selected_count;
    while (insert_at > 0 &&
           selected[insert_at - 1]->current_bytes < context.current_bytes) {
      if (insert_at < std::size(selected)) {
        selected[insert_at] = selected[insert_at - 1];
      }
      --insert_at;
    }
    if (insert_at < std::size(selected)) {
      selected[insert_at] = &context;
      if (selected_count < std::size(selected)) {
        ++selected_count;
      }
    }
  }
  for (std::size_t i = 0; i < selected_count; ++i) {
    char key[64] = {};
    std::snprintf(key, sizeof(key), "top_context.%zu.scope", i);
    AddEmergencyRow(&summary, key, selected[i]->scope_id);
    std::snprintf(key, sizeof(key), "top_context.%zu.current_bytes", i);
    add_number(key, selected[i]->current_bytes);
  }

  std::size_t transition_count = 0;
  for (const auto& transition : request.pressure_transitions) {
    if (transition_count >= 2) {
      ++summary.dropped_row_count;
      break;
    }
    char key[64] = {};
    std::snprintf(key, sizeof(key), "pressure_transition.%zu", transition_count);
    char value[96] = {};
    std::snprintf(value,
                  sizeof(value),
                  "%s->%s",
                  transition.previous_state.c_str(),
                  transition.new_state.c_str());
    AddEmergencyRow(&summary, key, value);
    ++transition_count;
  }
  return summary;
}

MemorySupportBundleResult BuildMemorySupportBundleEvidence(
    MemorySupportBundleRequest request) {
  MemorySupportBundleResult result;
  result.status = OkStatus();
  result.low_memory_mode = request.mode == MemorySupportBundleMode::low_memory ||
                           request.mode == MemorySupportBundleMode::emergency_summary;
  result.emergency_summary_mode =
      request.mode == MemorySupportBundleMode::emergency_summary;
  result.redaction_before_buffering = request.redaction_before_buffering;
  result.protected_material_excluded = request.exclude_protected_material;
  request.limits = NormalizeLimits(request.limits, result.low_memory_mode);
  if (result.low_memory_mode) {
    request.max_top_contexts = std::min<u64>(request.max_top_contexts, 4);
    request.max_top_categories = std::min<u64>(request.max_top_categories, 4);
  }
  result.output_byte_limit = request.limits.max_output_bytes;
  result.row_limit = request.limits.max_rows;
  if (result.emergency_summary_mode) {
    result.emergency_summary = BuildMemorySupportBundleEmergencySummary(request);
  }

  result.evidence.push_back("MMCH_MEMORY_SUPPORT_BUNDLE_EVIDENCE");
  result.evidence.push_back("CEIC-023_MEMORY_SUPPORT_BUNDLE_LOW_MEMORY");
  result.evidence.push_back(kAuthorityBoundary);
  result.evidence.push_back(kExpandedAuthorityBoundary);
  result.evidence.push_back("memory_support_bundle.profile=" + request.bundle_profile);
  result.evidence.push_back("memory_support_bundle.redaction_profile=" +
                            request.redaction_profile);
  result.evidence.push_back("memory_support_bundle.protected_material_allowed=" +
                            std::string(request.allow_protected_material ? "true" : "false"));
  result.evidence.push_back("memory_support_bundle.mode=" +
                            std::string(MemorySupportBundleModeName(request.mode)));
  result.evidence.push_back("memory_support_bundle.redaction_before_buffering=" +
                            std::string(request.redaction_before_buffering ? "true" : "false"));
  result.evidence.push_back("memory_support_bundle.protected_material_excluded=" +
                            std::string(request.exclude_protected_material ? "true" : "false"));
  result.evidence.push_back("memory_support_bundle.output_byte_limit=" +
                            std::to_string(request.limits.max_output_bytes));
  result.evidence.push_back("memory_support_bundle.row_limit=" +
                            std::to_string(request.limits.max_rows));
  result.evidence.push_back(
      "memory_support_bundle.no_authority.transaction_finality=true");
  result.evidence.push_back(
      "memory_support_bundle.no_authority.visibility=true");
  result.evidence.push_back(
      "memory_support_bundle.no_authority.authorization_security=true");
  result.evidence.push_back("memory_support_bundle.no_authority.recovery=true");
  result.evidence.push_back("memory_support_bundle.no_authority.parser_reference_wal=true");
  result.evidence.push_back(
      "memory_support_bundle.no_authority.benchmark_optimizer_index_agent=true");

  result.protected_memory_review_passed =
      ProtectedReviewPasses(request.protected_memory_review, &result.evidence);
  if ((request.allow_protected_material && request.exclude_protected_material) ||
      !request.redaction_before_buffering ||
      !result.protected_memory_review_passed) {
    result.status = InvalidRequestStatus();
    result.evidence.push_back("memory_support_bundle.fail_closed=true");
  } else {
    result.evidence.push_back("memory_support_bundle.fail_closed=false");
  }

  auto add = [&](std::string key,
                 std::string_view value,
                 const char* redaction_class = "public") {
    return AddRow(&result,
                  std::move(key),
                  value,
                  request.allow_protected_material,
                  request.exclude_protected_material,
                  request.limits,
                  redaction_class);
  };

  if (request.include_self_accounting) {
    add("self.mode", MemorySupportBundleModeName(request.mode));
    add("self.row_limit", std::to_string(request.limits.max_rows));
    add("self.output_byte_limit",
        std::to_string(request.limits.max_output_bytes));
    add("self.redaction_before_buffering",
        request.redaction_before_buffering ? "true" : "false");
    add("self.protected_material_excluded",
        request.exclude_protected_material ? "true" : "false");
  }

  add("snapshot.current_bytes", std::to_string(request.snapshot.current_bytes));
  add("snapshot.peak_bytes", std::to_string(request.snapshot.peak_bytes));
  add("snapshot.allocation_count",
      std::to_string(request.snapshot.allocation_count));
  add("snapshot.deallocation_count",
      std::to_string(request.snapshot.deallocation_count));
  add("snapshot.failure_count", std::to_string(request.snapshot.failure_count));
  add("snapshot.policy_rejection_count",
      std::to_string(request.snapshot.policy_rejection_count));
  add("snapshot.unknown_pointer_failure_count",
      std::to_string(request.snapshot.unknown_pointer_failure_count));
  add("snapshot.leak_candidate_count",
      std::to_string(request.snapshot.leak_candidate_count));
  add("snapshot.page_buffer_current_bytes",
      std::to_string(request.snapshot.page_buffer_current_bytes));
  add("snapshot.arena_current_bytes",
      std::to_string(request.snapshot.arena_current_bytes));

  std::vector<const MemoryContextSnapshot*> contexts;
  contexts.reserve(static_cast<std::size_t>(std::min<u64>(request.max_top_contexts, 16)));
  for (const auto& context : request.snapshot.contexts) {
    auto position = std::lower_bound(
        contexts.begin(),
        contexts.end(),
        context.current_bytes,
        [](const MemoryContextSnapshot* left, u64 bytes) {
          return left->current_bytes > bytes;
        });
    contexts.insert(position, &context);
    if (contexts.size() > request.max_top_contexts) {
      contexts.pop_back();
    }
  }
  for (const auto* context : contexts) {
    const std::string prefix = "top_context." + std::to_string(result.top_context_count);
    add(prefix + ".scope_kind", context->scope_kind);
    add(prefix + ".scope_id", context->scope_id, "identifier");
    add(prefix + ".current_bytes", std::to_string(context->current_bytes));
    add(prefix + ".peak_bytes", std::to_string(context->peak_bytes));
    add(prefix + ".active_allocation_count",
        std::to_string(context->active_allocation_count));
    ++result.top_context_count;
  }

  std::vector<const MemoryCategorySnapshot*> categories;
  categories.reserve(static_cast<std::size_t>(std::min<u64>(request.max_top_categories, 16)));
  for (const auto& category : request.snapshot.categories) {
    auto position = std::lower_bound(
        categories.begin(),
        categories.end(),
        category.current_bytes,
        [](const MemoryCategorySnapshot* left, u64 bytes) {
          return left->current_bytes > bytes;
        });
    categories.insert(position, &category);
    if (categories.size() > request.max_top_categories) {
      categories.pop_back();
    }
  }
  for (const auto* category : categories) {
    const std::string prefix = "top_category." + std::to_string(result.top_category_count);
    add(prefix + ".category", MemoryCategoryName(category->category));
    add(prefix + ".current_bytes", std::to_string(category->current_bytes));
    add(prefix + ".peak_bytes", std::to_string(category->peak_bytes));
    add(prefix + ".failure_count", std::to_string(category->failure_count));
    ++result.top_category_count;
  }

  for (const auto& transition : request.pressure_transitions) {
    if (result.pressure_transition_count >= request.limits.max_pressure_transitions) {
      ++result.dropped_row_count;
      break;
    }
    const std::string prefix =
        "pressure_transition." + std::to_string(result.pressure_transition_count);
    add(prefix + ".previous_state", transition.previous_state);
    add(prefix + ".new_state", transition.new_state);
    add(prefix + ".trigger", transition.trigger);
    add(prefix + ".current_bytes", std::to_string(transition.current_bytes));
    add(prefix + ".limit_bytes", std::to_string(transition.limit_bytes));
    add(prefix + ".emergency", transition.emergency ? "true" : "false");
    ++result.pressure_transition_count;
  }

  for (const auto& allocation_class : request.allocation_classes) {
    if (result.allocation_class_count >= request.limits.max_allocation_classes) {
      ++result.dropped_row_count;
      break;
    }
    const std::string prefix =
        "allocation_class." + std::to_string(result.allocation_class_count);
    add(prefix + ".class", allocation_class.memory_class);
    add(prefix + ".current_bytes",
        std::to_string(allocation_class.current_bytes));
    add(prefix + ".peak_bytes", std::to_string(allocation_class.peak_bytes));
    add(prefix + ".allocation_count",
        std::to_string(allocation_class.allocation_count));
    add(prefix + ".release_count",
        std::to_string(allocation_class.release_count));
    add(prefix + ".failure_count",
        std::to_string(allocation_class.failure_count));
    ++result.allocation_class_count;
  }

  if (!request.memory_fragmentation_rows.empty()) {
    result.evidence.push_back("CEIC-028_FRAGMENTATION_PROFILER_DIFF");
    result.evidence.push_back(
        "memory_support_bundle.fragmentation_profiler.included=true");
  }
  for (const auto& row : request.memory_fragmentation_rows) {
    if (result.memory_fragmentation_row_count >=
        request.max_memory_fragmentation_rows) {
      ++result.dropped_row_count;
      break;
    }
    const std::string prefix = "memory_fragmentation." +
                               std::to_string(
                                   result.memory_fragmentation_row_count);
    add(prefix + "." + row.key, row.value, row.redaction_class.c_str());
    ++result.memory_fragmentation_row_count;
  }

  if (!request.memory_working_set_locality_rows.empty()) {
    result.evidence.push_back("CEIC-029_ADAPTIVE_BATCH_WORKING_SET_LOCALITY");
    result.evidence.push_back(
        "memory_support_bundle.working_set_locality.included=true");
  }
  for (const auto& row : request.memory_working_set_locality_rows) {
    if (result.memory_working_set_locality_row_count >=
        request.max_memory_working_set_locality_rows) {
      ++result.dropped_row_count;
      break;
    }
    const std::string prefix = "memory_working_set_locality." +
                               std::to_string(
                                   result.memory_working_set_locality_row_count);
    add(prefix + "." + row.key, row.value, row.redaction_class.c_str());
    ++result.memory_working_set_locality_row_count;
  }

  if (request.include_foreign_memory) {
    result.evidence.push_back("CEIC-016_FOREIGN_MEMORY_RESERVATION_COVERAGE");
    result.evidence.push_back(
        "memory_support_bundle.foreign_memory.included=true");
    add("foreign_memory.snapshot.active_reservation_count",
        std::to_string(
            request.foreign_memory_snapshot.active_reservation_count));
    add("foreign_memory.snapshot.current_estimated_bytes",
        std::to_string(
            request.foreign_memory_snapshot.current_estimated_bytes));
    add("foreign_memory.snapshot.peak_estimated_bytes",
        std::to_string(request.foreign_memory_snapshot.peak_estimated_bytes));
    add("foreign_memory.snapshot.current_observed_bytes",
        std::to_string(
            request.foreign_memory_snapshot.current_observed_bytes));
    add("foreign_memory.snapshot.peak_observed_bytes",
        std::to_string(request.foreign_memory_snapshot.peak_observed_bytes));
    add("foreign_memory.snapshot.over_limit_refusal_count",
        std::to_string(
            request.foreign_memory_snapshot.over_limit_refusal_count));
    add("foreign_memory.snapshot.fail_closed_refusal_count",
        std::to_string(
            request.foreign_memory_snapshot.fail_closed_refusal_count));

    for (const auto& source : request.foreign_memory_snapshot.sources) {
      if (result.foreign_source_count >= request.max_foreign_sources) {
        break;
      }
      const std::string prefix =
          "foreign_memory.source." + std::to_string(result.foreign_source_count);
      add(prefix + ".source", ForeignMemorySourceName(source.source));
      add(prefix + ".active_reservation_count",
          std::to_string(source.active_reservation_count));
      add(prefix + ".current_estimated_bytes",
          std::to_string(source.current_estimated_bytes));
      add(prefix + ".peak_estimated_bytes",
          std::to_string(source.peak_estimated_bytes));
      add(prefix + ".current_observed_bytes",
          std::to_string(source.current_observed_bytes));
      add(prefix + ".peak_observed_bytes",
          std::to_string(source.peak_observed_bytes));
      ++result.foreign_source_count;
    }

    for (const auto& scope : request.foreign_memory_snapshot.owning_scopes) {
      if (result.foreign_owning_scope_count >=
          request.max_foreign_owning_scopes) {
        break;
      }
      const std::string prefix = "foreign_memory.owning_scope." +
                                 std::to_string(
                                     result.foreign_owning_scope_count);
      add(prefix + ".owning_scope", scope.owning_scope, "identifier");
      add(prefix + ".active_reservation_count",
          std::to_string(scope.active_reservation_count));
      add(prefix + ".current_estimated_bytes",
          std::to_string(scope.current_estimated_bytes));
      add(prefix + ".peak_estimated_bytes",
          std::to_string(scope.peak_estimated_bytes));
      add(prefix + ".current_observed_bytes",
          std::to_string(scope.current_observed_bytes));
      add(prefix + ".peak_observed_bytes",
          std::to_string(scope.peak_observed_bytes));
      ++result.foreign_owning_scope_count;
    }
  }

  if (request.include_failure_reasons) {
    for (const auto& diagnostic : request.diagnostics) {
      const std::string prefix =
          "failure_reason." + std::to_string(result.failure_reason_count);
      add(prefix + ".diagnostic_code", diagnostic.diagnostic_code);
      add(prefix + ".message_key", diagnostic.message_key);
      u64 argument_count = 0;
      for (const auto& argument : diagnostic.arguments) {
        if (argument_count >= request.limits.max_diagnostic_arguments) {
          ++result.dropped_row_count;
          break;
        }
        add(prefix + ".argument." + argument.key, argument.value);
        ++argument_count;
      }
      ++result.failure_reason_count;
    }
  }

  if (request.include_metrics) {
    for (const auto& metric : request.metrics) {
      if (result.metric_count >= request.limits.max_metrics) {
        ++result.dropped_row_count;
        break;
      }
      const std::string prefix = "metric." + std::to_string(result.metric_count);
      add(prefix + ".family", metric.family);
      add(prefix + ".value", std::to_string(metric.value));
      ++result.metric_count;
    }
  }

  if (request.protected_memory_review.enabled) {
    add("protected_memory_review.enabled", "true");
    add("protected_memory_review.passed",
        result.protected_memory_review_passed ? "true" : "false");
    add("protected_memory_review.scan_complete",
        (request.protected_memory_review.diagnostics_log_exception_scan_complete &&
         request.protected_memory_review.support_bundle_scan_complete)
            ? "true"
            : "false");
    add("protected_memory_review.zeroization_not_optimized_away",
        request.protected_memory_review.zeroization_not_optimized_away ? "true"
                                                                      : "false");
    for (const auto& evidence : request.protected_memory_review.protected_buffer_evidence) {
      const std::string prefix = "protected_buffer." +
                                 std::to_string(result.protected_buffer_evidence_count);
      add(prefix + ".redacted",
          evidence.protected_material_redacted ? "true" : "false");
      add(prefix + ".zero_on_release",
          evidence.zero_on_release ? "true" : "false");
      add(prefix + ".platform", evidence.platform_name);
      add(prefix + ".lock_attempted",
          evidence.platform_lock_attempted ? "true" : "false");
      add(prefix + ".no_dump_attempted",
          evidence.no_dump_attempted ? "true" : "false");
      add(prefix + ".authority_scope", evidence.authority_scope);
      ++result.protected_buffer_evidence_count;
    }
    for (const auto& route : request.protected_memory_review.secret_routes) {
      const std::string prefix =
          "protected_route." + std::to_string(result.protected_secret_route_count);
      add(prefix + ".source_kind", route.source_kind, "secret_route");
      add(prefix + ".route_id", route.route_id, "identifier");
      add(prefix + ".protected_buffer",
          route.routed_through_protected_buffer ? "true" : "false");
      add(prefix + ".protected_reference_only",
          route.protected_reference_only ? "true" : "false");
      add(prefix + ".plaintext_observed",
          route.plaintext_material_observed ? "true" : "false");
      ++result.protected_secret_route_count;
    }
  }

  result.evidence.push_back("memory_support_bundle.row_count=" +
                            std::to_string(result.rows.size()));
  result.evidence.push_back("memory_support_bundle.redacted_row_count=" +
                            std::to_string(result.redacted_row_count));
  result.evidence.push_back("memory_support_bundle.foreign_source_count=" +
                            std::to_string(result.foreign_source_count));
  result.evidence.push_back(
      "memory_support_bundle.foreign_owning_scope_count=" +
      std::to_string(result.foreign_owning_scope_count));
  result.evidence.push_back("memory_support_bundle.pressure_transition_count=" +
                            std::to_string(result.pressure_transition_count));
  result.evidence.push_back("memory_support_bundle.allocation_class_count=" +
                            std::to_string(result.allocation_class_count));
  result.evidence.push_back(
      "memory_support_bundle.memory_fragmentation_row_count=" +
      std::to_string(result.memory_fragmentation_row_count));
  result.evidence.push_back(
      "memory_support_bundle.memory_working_set_locality_row_count=" +
      std::to_string(result.memory_working_set_locality_row_count));
  result.evidence.push_back("memory_support_bundle.output_bytes=" +
                            std::to_string(result.output_bytes));
  result.evidence.push_back("memory_support_bundle.dropped_row_count=" +
                            std::to_string(result.dropped_row_count));
  result.evidence.push_back(
      "memory_support_bundle.emergency_summary.allocation_free_model=" +
      std::string(result.emergency_summary.allocation_free_model ? "true" : "false"));
  result.evidence.push_back("memory_support_bundle.tamper_evidence_digest=row_level");
  return result;
}

}  // namespace scratchbird::core::memory
