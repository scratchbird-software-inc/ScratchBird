// Copyright (c) 2026 ScratchBird Software Inc.
//
// This Source Code Form is subject to the terms of the Mozilla Public
// License, v. 2.0. If a copy of the MPL was not distributed with this
// file, You can obtain one at https://mozilla.org/MPL/2.0/.
//
// SPDX-License-Identifier: MPL-2.0

#include "time_range_summary_pruning.hpp"

// DPC_TIME_RANGE_SUMMARY_PRUNING

#include "index_key_encoding.hpp"

#include <algorithm>
#include <sstream>
#include <utility>

namespace scratchbird::core::index {
namespace {

using scratchbird::core::platform::DiagnosticArgument;
using scratchbird::core::platform::MakeDiagnostic;
using scratchbird::core::platform::Severity;
using scratchbird::core::platform::StatusCode;
using scratchbird::core::platform::Subsystem;

Status OkStatus() { return {StatusCode::ok, Severity::info, Subsystem::engine}; }
Status WarnStatus() { return {StatusCode::ok, Severity::warning, Subsystem::engine}; }

bool SameFormatVersion(PageExtentSummaryFormatVersion left,
                       PageExtentSummaryFormatVersion right) {
  return left.major == right.major && left.minor == right.minor;
}

u64 PageSpanForRange(const PageExtentSummaryRange& range) {
  if (range.kind == PageExtentSummaryRangeKind::page_range) {
    return range.page_count;
  }
  return range.extent_count;
}

bool SameScalarType(const TimeRangeSummaryDescriptor& descriptor,
                    const TimeRangeSummaryPredicate& predicate) {
  return predicate.time_scalar_type_key.empty() ||
         descriptor.time_scalar_type_key.empty() ||
         descriptor.time_scalar_type_key == predicate.time_scalar_type_key;
}

bool EncodedLess(std::string_view left, std::string_view right) {
  if (IsOrderPreservingIndexKeyEncoding(left) &&
      IsOrderPreservingIndexKeyEncoding(right)) {
    const auto compare = CompareEncodedIndexKeyBytes(left, right);
    return compare.ok() && compare.comparison < 0;
  }
  return left.compare(right) < 0;
}

bool EncodedGreater(std::string_view left, std::string_view right) {
  if (IsOrderPreservingIndexKeyEncoding(left) &&
      IsOrderPreservingIndexKeyEncoding(right)) {
    const auto compare = CompareEncodedIndexKeyBytes(left, right);
    return compare.ok() && compare.comparison > 0;
  }
  return left.compare(right) > 0;
}

bool EncodedEqual(std::string_view left, std::string_view right) {
  return left == right;
}

bool SummaryOutsidePredicate(const TimeRangeSummaryDescriptor& descriptor,
                             const TimeRangeSummaryPredicate& predicate) {
  const bool has_non_null_bounds =
      descriptor.min_time_present && descriptor.max_time_present;
  if (!has_non_null_bounds) {
    return predicate.lower_present || predicate.upper_present;
  }
  if (predicate.lower_present) {
    if (EncodedLess(descriptor.encoded_max_time,
                    predicate.encoded_lower_time)) {
      return true;
    }
    if (EncodedEqual(descriptor.encoded_max_time,
                     predicate.encoded_lower_time) &&
        (!descriptor.max_inclusive || !predicate.lower_inclusive)) {
      return true;
    }
  }
  if (predicate.upper_present) {
    if (EncodedGreater(descriptor.encoded_min_time,
                       predicate.encoded_upper_time)) {
      return true;
    }
    if (EncodedEqual(descriptor.encoded_min_time,
                     predicate.encoded_upper_time) &&
        (!descriptor.min_inclusive || !predicate.upper_inclusive)) {
      return true;
    }
  }
  return false;
}

DiagnosticRecord Diagnostic(Status status,
                            std::string diagnostic_code,
                            std::string message_key,
                            std::string detail = {}) {
  std::vector<DiagnosticArgument> arguments;
  if (!detail.empty()) {
    arguments.push_back({"detail", std::move(detail)});
  }
  return MakeDiagnostic(status.code,
                        status.severity,
                        status.subsystem,
                        std::move(diagnostic_code),
                        std::move(message_key),
                        std::move(arguments),
                        {},
                        "core.index.time_range_summary_pruning");
}

TimeRangeSummaryPrunePlan Fallback(TimeRangeSummaryFallbackReason reason,
                                   std::string diagnostic_code,
                                   std::string message_key,
                                   std::string detail = {}) {
  TimeRangeSummaryPrunePlan plan;
  plan.status = WarnStatus();
  plan.selected_category = IndexPlanCategory::fallback_full_scan;
  plan.selected_access = "full_scan";
  plan.prune_reason = "none";
  plan.fallback_reason = TimeRangeSummaryFallbackReasonName(reason);
  plan.summary_prune_selected = false;
  plan.exact_fallback_required = true;
  plan.base_row_mga_recheck_required = true;
  plan.base_row_security_recheck_required = true;
  plan.summary_metadata_visibility_authority = false;
  plan.summary_metadata_finality_authority = false;
  plan.authority_source = kTimeRangeSummaryAuthoritySource;
  plan.diagnostic = Diagnostic(plan.status,
                               std::move(diagnostic_code),
                               std::move(message_key),
                               std::move(detail));
  plan.actions.push_back("select_exact_full_scan_fallback");
  plan.actions.push_back("do_not_use_time_summary_as_visibility_or_finality_authority");
  return plan;
}

TimeRangeSummaryRangeEvidence RangeEvidence(
    const TimeRangeSummaryDescriptor& descriptor,
    std::string decision) {
  TimeRangeSummaryRangeEvidence evidence;
  evidence.range = descriptor.range;
  evidence.generation = descriptor.generation;
  evidence.summary_status = PageExtentSummaryStatusName(descriptor.status);
  evidence.decision = std::move(decision);
  return evidence;
}

bool HasNonNullRows(const TimeRangeSummaryDescriptor& descriptor) {
  return descriptor.row_count > 0 && descriptor.null_count < descriptor.row_count;
}

std::uint64_t MixFnv1a(std::uint64_t hash, std::string_view text) {
  for (const unsigned char ch : text) {
    hash ^= ch;
    hash *= 1099511628211ull;
  }
  return hash;
}

std::string Hex(std::uint64_t value) {
  std::ostringstream out;
  out << "0x" << std::hex << std::nouppercase << value;
  return out.str();
}

std::string RowIdHash(const std::vector<std::string>& row_ids) {
  std::uint64_t hash = 1469598103934665603ull;
  for (const auto& row_id : row_ids) {
    hash = MixFnv1a(hash, row_id);
    hash = MixFnv1a(hash, ";");
  }
  return Hex(hash);
}

bool FormatCurrentAndCompatible(const TimeRangeSummaryDescriptor& descriptor,
                                const PageExtentSummaryFormatCompatibility& format) {
  return format.compatible &&
         format.open_class == PageExtentSummaryFormatOpenClass::current &&
         !format.migration_required &&
         SameFormatVersion(descriptor.format_version, format.observed);
}

bool PredicateUsesUnsafeLegacyEnvelope(const TimeRangeSummaryPredicate& predicate) {
  return (predicate.lower_present &&
          IsUnsafeLegacyIndexKeyEncoding(predicate.encoded_lower_time)) ||
         (predicate.upper_present &&
          IsUnsafeLegacyIndexKeyEncoding(predicate.encoded_upper_time));
}

}  // namespace

bool TimeRangeSummaryDescriptorIdentityValid(
    const TimeRangeSummaryDescriptor& descriptor) {
  return PageExtentSummaryUuidTextValid(descriptor.table_uuid) &&
         PageExtentSummaryUuidTextValid(descriptor.index_uuid) &&
         PageExtentSummaryUuidTextValid(descriptor.range_family_uuid) &&
         PageExtentSummaryUuidTextValid(descriptor.summary_uuid);
}

bool TimeRangeSummaryDescriptorAuthorityClean(
    const TimeRangeSummaryDescriptor& descriptor) {
  return descriptor.authority_source == kTimeRangeSummaryAuthoritySource &&
         !descriptor.parser_finality_authority_claimed &&
         !descriptor.client_finality_authority_claimed &&
         !descriptor.timestamp_finality_authority_claimed &&
         !descriptor.uuid_ordering_finality_authority_claimed &&
         !descriptor.event_stream_finality_authority_claimed &&
         !descriptor.donor_finality_authority_claimed &&
         !descriptor.write_ahead_log_finality_authority_claimed;
}

bool TimeRangeSummaryDescriptorBoundsValid(
    const TimeRangeSummaryDescriptor& descriptor) {
  if (!PageExtentSummaryRangeValid(descriptor.range)) {
    return false;
  }
  if (descriptor.null_count > descriptor.row_count) {
    return false;
  }
  if (descriptor.null_count > 0 && !descriptor.nulls_present) {
    return false;
  }
  if (!HasNonNullRows(descriptor)) {
    return !descriptor.min_time_present &&
           !descriptor.max_time_present &&
           descriptor.encoded_min_time.empty() &&
           descriptor.encoded_max_time.empty();
  }
  if (descriptor.time_scalar_type_key.empty() ||
      !descriptor.min_time_present ||
      !descriptor.max_time_present ||
      descriptor.encoded_min_time.empty() ||
      descriptor.encoded_max_time.empty()) {
    return false;
  }
  if (IsUnsafeLegacyIndexKeyEncoding(descriptor.encoded_min_time) ||
      IsUnsafeLegacyIndexKeyEncoding(descriptor.encoded_max_time)) {
    return false;
  }
  if (EncodedGreater(descriptor.encoded_min_time,
                     descriptor.encoded_max_time)) {
    return false;
  }
  if (EncodedEqual(descriptor.encoded_min_time,
                   descriptor.encoded_max_time) &&
      (!descriptor.min_inclusive || !descriptor.max_inclusive)) {
    return false;
  }
  return true;
}

TimeRangeSummaryPrunePlan PlanTimeRangeSummaryPrune(
    const TimeRangeSummaryPruneRequest& request) {
  if (!request.time_range_prune_enabled) {
    auto plan = Fallback(
        TimeRangeSummaryFallbackReason::disabled_summary_exact_fallback,
        "INDEX.TIME_RANGE_SUMMARY.DISABLED_EXACT_FALLBACK",
        "index.time_range_summary.disabled_exact_fallback");
    plan.summary_status = "disabled";
    plan.actions.push_back("time_range_summary_feature_disabled");
    return plan;
  }
  if (!request.base_row_mga_recheck_required ||
      !request.base_row_security_recheck_required) {
    auto plan = Fallback(
        TimeRangeSummaryFallbackReason::non_authoritative_summary_exact_fallback,
        "INDEX.TIME_RANGE_SUMMARY.BASE_ROW_RECHECK_REQUIRED",
        "index.time_range_summary.base_row_recheck_required");
    plan.summary_status = "authority_refused";
    plan.actions.push_back("require_base_row_mga_and_security_recheck");
    return plan;
  }
  if (request.summaries.empty()) {
    auto plan = Fallback(
        TimeRangeSummaryFallbackReason::missing_summary_exact_fallback,
        "INDEX.TIME_RANGE_SUMMARY.MISSING_EXACT_FALLBACK",
        "index.time_range_summary.missing_exact_fallback");
    plan.summary_status = PageExtentSummaryStatusName(PageExtentSummaryStatus::missing);
    return plan;
  }
  if (PredicateUsesUnsafeLegacyEnvelope(request.predicate)) {
    auto plan = Fallback(
        TimeRangeSummaryFallbackReason::corrupt_summary_exact_fallback,
        "INDEX.TIME_RANGE_SUMMARY.UNSAFE_LEGACY_KEY_EXACT_FALLBACK",
        "index.time_range_summary.unsafe_legacy_key_exact_fallback");
    plan.summary_status = "unsafe_legacy_key";
    plan.actions.push_back("refuse_unsafe_legacy_length_prefixed_key_order");
    return plan;
  }

  TimeRangeSummaryPrunePlan plan;
  plan.status = OkStatus();
  plan.selected_category = IndexPlanCategory::summary_prune;
  plan.selected_access = "time_range_summary_prune";
  plan.prune_reason = "time_bounds_current";
  plan.fallback_reason =
      TimeRangeSummaryFallbackReasonName(TimeRangeSummaryFallbackReason::none);
  plan.summary_status = PageExtentSummaryStatusName(PageExtentSummaryStatus::current);
  plan.summary_prune_selected = true;
  plan.exact_fallback_required = false;
  plan.base_row_mga_recheck_required = true;
  plan.base_row_security_recheck_required = true;
  plan.authority_source = kTimeRangeSummaryAuthoritySource;

  for (const auto& summary : request.summaries) {
    ++plan.counters.prune_candidates;
    const auto pages = PageSpanForRange(summary.range);
    plan.counters.pages_considered += pages;
    plan.summary_generation = std::max(plan.summary_generation,
                                       summary.generation);

    const auto copy_fallback_state = [&plan, &summary](TimeRangeSummaryPrunePlan fallback) {
      fallback.counters = plan.counters;
      fallback.summary_status = PageExtentSummaryStatusName(summary.status);
      fallback.summary_generation = summary.generation;
      fallback.range_evidence = plan.range_evidence;
      fallback.range_evidence.push_back(RangeEvidence(summary, "exact_fallback"));
      return fallback;
    };

    if (!TimeRangeSummaryDescriptorIdentityValid(summary)) {
      auto fallback = Fallback(
          TimeRangeSummaryFallbackReason::invalid_identity_exact_fallback,
          "INDEX.TIME_RANGE_SUMMARY.INVALID_IDENTITY_EXACT_FALLBACK",
          "index.time_range_summary.invalid_identity_exact_fallback");
      fallback.actions.push_back("ignore_time_summary_until_generated_uuid_identity_is_supplied");
      return copy_fallback_state(std::move(fallback));
    }
    if (!TimeRangeSummaryDescriptorAuthorityClean(summary)) {
      auto fallback = Fallback(
          TimeRangeSummaryFallbackReason::non_authoritative_summary_exact_fallback,
          "INDEX.TIME_RANGE_SUMMARY.NON_AUTHORITATIVE_EXACT_FALLBACK",
          "index.time_range_summary.non_authoritative_exact_fallback",
          summary.authority_source);
      fallback.actions.push_back("discard_time_summary_external_finality_claim");
      return copy_fallback_state(std::move(fallback));
    }
    if (!FormatCurrentAndCompatible(summary, request.format) ||
        summary.status == PageExtentSummaryStatus::incompatible_format ||
        !SameScalarType(summary, request.predicate)) {
      auto fallback = Fallback(
          TimeRangeSummaryFallbackReason::incompatible_summary_exact_fallback,
          "INDEX.TIME_RANGE_SUMMARY.INCOMPATIBLE_EXACT_FALLBACK",
          "index.time_range_summary.incompatible_exact_fallback",
          request.format.diagnostic_code);
      fallback.actions.push_back("refuse_time_summary_until_format_and_predicate_match");
      return copy_fallback_state(std::move(fallback));
    }
    if (!summary.persisted_record_present ||
        summary.status == PageExtentSummaryStatus::missing) {
      auto fallback = Fallback(
          TimeRangeSummaryFallbackReason::missing_summary_exact_fallback,
          "INDEX.TIME_RANGE_SUMMARY.MISSING_EXACT_FALLBACK",
          "index.time_range_summary.missing_exact_fallback");
      fallback.actions.push_back("schedule_time_summary_rebuild");
      return copy_fallback_state(std::move(fallback));
    }
    if (!summary.checksum_valid ||
        summary.status == PageExtentSummaryStatus::corrupt ||
        !TimeRangeSummaryDescriptorBoundsValid(summary) ||
        summary.generation == 0) {
      auto fallback = Fallback(
          TimeRangeSummaryFallbackReason::corrupt_summary_exact_fallback,
          "INDEX.TIME_RANGE_SUMMARY.CORRUPT_EXACT_FALLBACK",
          "index.time_range_summary.corrupt_exact_fallback");
      fallback.actions.push_back("quarantine_time_summary_until_repair_validates_base_ranges");
      return copy_fallback_state(std::move(fallback));
    }
    if (summary.status == PageExtentSummaryStatus::stale) {
      auto fallback = Fallback(
          TimeRangeSummaryFallbackReason::stale_summary_exact_fallback,
          "INDEX.TIME_RANGE_SUMMARY.STALE_EXACT_FALLBACK",
          "index.time_range_summary.stale_exact_fallback");
      fallback.actions.push_back("refresh_time_summary_from_authoritative_base_ranges");
      return copy_fallback_state(std::move(fallback));
    }

    if (SummaryOutsidePredicate(summary, request.predicate)) {
      ++plan.counters.ranges_pruned;
      plan.counters.pages_pruned += pages;
      plan.range_evidence.push_back(RangeEvidence(summary, "pruned"));
    } else {
      ++plan.counters.ranges_scanned;
      plan.counters.pages_scanned += pages;
      plan.range_evidence.push_back(RangeEvidence(summary, "scan_with_base_row_recheck"));
    }
  }

  plan.diagnostic = Diagnostic(plan.status,
                               "INDEX.TIME_RANGE_SUMMARY.SELECTED",
                               "index.time_range_summary.selected");
  plan.actions.push_back("use_current_time_range_summary_bounds_for_prune_admission");
  plan.actions.push_back("scan_admitted_ranges_with_base_row_mga_recheck");
  plan.actions.push_back("scan_admitted_ranges_with_security_recheck");
  plan.actions.push_back("do_not_use_time_summary_as_visibility_or_finality_authority");
  return plan;
}

TimeRangeSummaryResultEqualityEvidence BuildTimeRangeSummaryResultEqualityEvidence(
    const std::vector<std::string>& baseline_row_ids,
    const std::vector<std::string>& planned_row_ids) {
  TimeRangeSummaryResultEqualityEvidence evidence;
  evidence.baseline_row_count = baseline_row_ids.size();
  evidence.planned_row_count = planned_row_ids.size();
  evidence.baseline_result_hash = RowIdHash(baseline_row_ids);
  evidence.planned_result_hash = RowIdHash(planned_row_ids);
  evidence.exact_match = baseline_row_ids == planned_row_ids &&
                         evidence.baseline_result_hash ==
                             evidence.planned_result_hash;
  evidence.deterministic_row_ids = planned_row_ids;
  return evidence;
}

const char* TimeRangeSummaryFallbackReasonName(
    TimeRangeSummaryFallbackReason reason) {
  switch (reason) {
    case TimeRangeSummaryFallbackReason::none:
      return "none";
    case TimeRangeSummaryFallbackReason::disabled_summary_exact_fallback:
      return "disabled_summary_exact_fallback";
    case TimeRangeSummaryFallbackReason::missing_summary_exact_fallback:
      return "missing_summary_exact_fallback";
    case TimeRangeSummaryFallbackReason::stale_summary_exact_fallback:
      return "stale_summary_exact_fallback";
    case TimeRangeSummaryFallbackReason::corrupt_summary_exact_fallback:
      return "corrupt_summary_exact_fallback";
    case TimeRangeSummaryFallbackReason::incompatible_summary_exact_fallback:
      return "incompatible_summary_exact_fallback";
    case TimeRangeSummaryFallbackReason::non_authoritative_summary_exact_fallback:
      return "non_authoritative_summary_exact_fallback";
    case TimeRangeSummaryFallbackReason::invalid_identity_exact_fallback:
      return "invalid_identity_exact_fallback";
  }
  return "unknown";
}

}  // namespace scratchbird::core::index
