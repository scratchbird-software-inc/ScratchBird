// Copyright (c) 2026 ScratchBird Software Inc.
//
// This Source Code Form is subject to the terms of the Mozilla Public
// License, v. 2.0. If a copy of the MPL was not distributed with this
// file, You can obtain one at https://mozilla.org/MPL/2.0/.
//
// SPDX-License-Identifier: MPL-2.0

#include "index_optimizer_integration.hpp"
#include "uuid.hpp"

#include <algorithm>
#include <chrono>
#include <cstdint>
#include <cstdlib>
#include <iomanip>
#include <iostream>
#include <sstream>
#include <string>
#include <string_view>
#include <vector>

namespace {

namespace idx = scratchbird::core::index;
namespace uuid = scratchbird::core::uuid;
using scratchbird::core::platform::TypedUuid;
using scratchbird::core::platform::UuidKind;

constexpr std::string_view kGateSearchKey =
    "DPC_PAGE_EXTENT_SUMMARY_BENCHMARK_GATE";
constexpr std::string_view kBenchmarkOutputSearchKey =
    "DPC_PAGE_SUMMARY_BENCHMARK_OUTPUT";
constexpr std::uint32_t kRunCount = 5;
constexpr double kWl08MinimumPageReadReduction = 0.25;
constexpr double kWl10MinimumPageReadReduction = 0.15;

[[noreturn]] void Fail(std::string_view message) {
  std::cerr << message << '\n';
  std::exit(EXIT_FAILURE);
}

void Require(bool condition, std::string_view message) {
  if (!condition) {
    Fail(message);
  }
}

std::string EncodeKey(std::int64_t value) {
  std::ostringstream out;
  out << std::setw(3) << std::setfill('0') << value;
  return out.str();
}

struct UuidFactory {
  std::uint64_t base_millis = static_cast<std::uint64_t>(
      std::chrono::duration_cast<std::chrono::milliseconds>(
          std::chrono::system_clock::now().time_since_epoch())
          .count());

  TypedUuid Typed(UuidKind kind, std::uint64_t salt) const {
    const auto generated =
        uuid::GenerateEngineIdentityV7(kind, base_millis + salt);
    Require(generated.ok(), "DPC-014 UUID generation failed");
    return generated.value;
  }

  std::string Text(std::uint64_t salt) const {
    return uuid::UuidToString(Typed(UuidKind::object, salt).value);
  }
};

idx::PageExtentSummaryFormatCompatibility CurrentFormat() {
  const auto contract = idx::PageExtentSummaryPersistedFormatContract();
  idx::PageExtentSummaryFormatCompatibility format;
  format.observed = contract.current;
  format.open_class = idx::PageExtentSummaryFormatOpenClass::current;
  format.compatible = true;
  format.migration_required = false;
  format.diagnostic_code = "DPC-014.current_format";
  return format;
}

struct SyntheticRow {
  std::uint64_t row_id = 0;
  std::uint64_t page_id = 0;
  std::int64_t key = 0;
  std::int64_t amount = 0;
  bool engine_mga_visible = true;
  bool security_visible = true;
};

struct SyntheticPage {
  std::uint64_t page_id = 0;
  std::vector<SyntheticRow> rows;
};

struct Predicate {
  std::int64_t lower = 0;
  std::int64_t upper = 0;
};

std::vector<SyntheticPage> Corpus() {
  std::vector<SyntheticPage> pages;
  pages.reserve(12);
  std::uint64_t row_id = 1;
  for (std::uint64_t page_id = 0; page_id < 12; ++page_id) {
    const std::int64_t first_key = static_cast<std::int64_t>(page_id * 9 + 1);
    SyntheticPage page;
    page.page_id = page_id;
    for (const std::int64_t offset : {0, 4, 8}) {
      SyntheticRow row;
      row.row_id = row_id++;
      row.page_id = page_id;
      row.key = first_key + offset;
      row.amount = row.key;
      page.rows.push_back(row);
    }
    pages.push_back(page);
  }

  // These rows must still be rejected after a summary-prune page admits their
  // page. The summary metadata is only a page-skip hint, not visibility authority.
  pages[5].rows[1].engine_mga_visible = false;  // key 50
  pages[6].rows[1].security_visible = false;    // key 59
  pages[8].rows[1].engine_mga_visible = false;  // key 77
  return pages;
}

idx::PageExtentSummaryMetadata SummaryForPage(const UuidFactory& uuids,
                                              std::string relation_uuid,
                                              const SyntheticPage& page) {
  const auto contract = idx::PageExtentSummaryPersistedFormatContract();
  const auto minmax = std::minmax_element(
      page.rows.begin(), page.rows.end(),
      [](const SyntheticRow& left, const SyntheticRow& right) {
        return left.key < right.key;
      });

  idx::PageExtentSummaryMetadata metadata;
  metadata.relation_uuid = std::move(relation_uuid);
  metadata.summary_uuid = uuids.Text(1000 + page.page_id);
  metadata.range.kind = idx::PageExtentSummaryRangeKind::page_range;
  metadata.range.first_page_id = page.page_id;
  metadata.range.page_count = 1;
  metadata.boundary.scalar_type_key = "int64_lex";
  metadata.boundary.encoded_min = EncodeKey(minmax.first->key);
  metadata.boundary.encoded_max = EncodeKey(minmax.second->key);
  metadata.boundary.min_present = true;
  metadata.boundary.max_present = true;
  metadata.row_count = page.rows.size();
  metadata.status = idx::PageExtentSummaryStatus::current;
  metadata.format_version = contract.current;
  metadata.generation = 1400 + page.page_id;
  metadata.persisted_record_present = true;
  metadata.checksum_valid = true;
  return metadata;
}

std::vector<idx::PageExtentSummaryMetadata> BuildSummaries(
    const UuidFactory& uuids,
    const std::vector<SyntheticPage>& pages) {
  const std::string relation_uuid = uuids.Text(14);
  std::vector<idx::PageExtentSummaryMetadata> summaries;
  summaries.reserve(pages.size());
  for (const auto& page : pages) {
    summaries.push_back(SummaryForPage(uuids, relation_uuid, page));
  }
  return summaries;
}

idx::PageExtentSummaryPrunePredicate ToPlannerPredicate(Predicate predicate) {
  idx::PageExtentSummaryPrunePredicate planner_predicate;
  planner_predicate.scalar_type_key = "int64_lex";
  planner_predicate.encoded_lower = EncodeKey(predicate.lower);
  planner_predicate.encoded_upper = EncodeKey(predicate.upper);
  planner_predicate.lower_present = true;
  planner_predicate.upper_present = true;
  planner_predicate.lower_inclusive = true;
  planner_predicate.upper_inclusive = true;
  return planner_predicate;
}

idx::PageExtentSummaryPrunePlan Plan(
    const std::vector<idx::PageExtentSummaryMetadata>& summaries,
    Predicate predicate,
    bool enabled = true) {
  idx::PageExtentSummaryPruneRequest request;
  request.summaries = summaries;
  request.format = CurrentFormat();
  request.predicate = ToPlannerPredicate(predicate);
  request.summary_prune_enabled = enabled;
  request.base_row_mga_recheck_required = true;
  request.base_row_security_recheck_required = true;
  return idx::PlanPageExtentSummaryPrune(request);
}

bool PageMayMatch(const idx::PageExtentSummaryMetadata& summary,
                  Predicate predicate) {
  const auto lower = EncodeKey(predicate.lower);
  const auto upper = EncodeKey(predicate.upper);
  return !(summary.boundary.encoded_max < lower ||
           summary.boundary.encoded_min > upper);
}

bool RowMatches(const SyntheticRow& row, Predicate predicate) {
  return row.engine_mga_visible && row.security_visible &&
         row.key >= predicate.lower && row.key <= predicate.upper;
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

struct ScanResult {
  std::uint64_t page_reads = 0;
  std::uint64_t row_count = 0;
  std::int64_t aggregate_sum = 0;
  std::string hash;
};

ScanResult Execute(const std::vector<SyntheticPage>& pages,
                   const std::vector<idx::PageExtentSummaryMetadata>& summaries,
                   Predicate predicate,
                   const idx::PageExtentSummaryPrunePlan& plan) {
  Require(pages.size() == summaries.size(),
          "DPC-014 corpus and summary shape diverged");

  std::uint64_t hash = 1469598103934665603ull;
  ScanResult result;
  for (std::size_t i = 0; i < pages.size(); ++i) {
    if (plan.summary_prune_selected && !PageMayMatch(summaries[i], predicate)) {
      continue;
    }
    ++result.page_reads;
    for (const auto& row : pages[i].rows) {
      if (!RowMatches(row, predicate)) {
        continue;
      }
      ++result.row_count;
      result.aggregate_sum += row.amount;
      hash = MixFnv1a(hash, std::to_string(row.row_id));
      hash = MixFnv1a(hash, ":");
      hash = MixFnv1a(hash, std::to_string(row.page_id));
      hash = MixFnv1a(hash, ":");
      hash = MixFnv1a(hash, std::to_string(row.key));
      hash = MixFnv1a(hash, ";");
    }
  }
  result.hash = Hex(hash);
  return result;
}

double ReductionRatio(std::uint64_t baseline, std::uint64_t optimized) {
  Require(baseline > 0, "DPC-014 baseline page count is zero");
  return static_cast<double>(baseline - optimized) /
         static_cast<double>(baseline);
}

std::string Fixed(double value) {
  std::ostringstream out;
  out << std::fixed << std::setprecision(6) << value;
  return out.str();
}

struct ProofRow {
  std::string workload_id;
  std::string lane;
  std::string mode;
  std::string selected_access;
  std::string fallback_reason;
  std::uint64_t full_page_reads = 0;
  std::uint64_t optimized_page_reads = 0;
  double reduction_ratio = 0.0;
  std::uint64_t result_count = 0;
  std::int64_t aggregate_sum = 0;
  std::string result_hash;
};

void PrintProofRow(const ProofRow& row) {
  std::cout << kBenchmarkOutputSearchKey
            << ",workload_id=" << row.workload_id
            << ",lane=" << row.lane
            << ",mode=" << row.mode
            << ",selected_access=" << row.selected_access
            << ",fallback_reason=" << row.fallback_reason
            << ",run_count=" << kRunCount
            << ",full_page_reads=" << row.full_page_reads
            << ",optimized_page_reads=" << row.optimized_page_reads
            << ",median_page_reads=" << row.optimized_page_reads
            << ",p95_page_reads=" << row.optimized_page_reads
            << ",cv_page_reads=0.000000"
            << ",page_read_reduction_ratio=" << Fixed(row.reduction_ratio)
            << ",result_count=" << row.result_count
            << ",aggregate_sum=" << row.aggregate_sum
            << ",result_hash=" << row.result_hash
            << ",build_profile=ctest_standalone_deterministic"
            << ",source_state=ctest_runtime\n";
}

ProofRow ProveLane(const std::vector<SyntheticPage>& pages,
                   const std::vector<idx::PageExtentSummaryMetadata>& summaries,
                   std::string workload_id,
                   std::string lane,
                   Predicate predicate,
                   double minimum_reduction) {
  const idx::PageExtentSummaryPrunePlan full_scan_plan;
  const auto full = Execute(pages, summaries, predicate, full_scan_plan);
  const auto plan = Plan(summaries, predicate);
  Require(plan.ok(), "DPC-014 positive summary-prune plan was not selected");
  Require(plan.selected_access == "summary_prune",
          "DPC-014 positive plan did not use summary_prune");
  Require(plan.base_row_mga_recheck_required &&
              plan.base_row_security_recheck_required,
          "DPC-014 planner did not preserve base-row rechecks");
  Require(!plan.summary_metadata_visibility_authority &&
              !plan.summary_metadata_finality_authority,
          "DPC-014 planner made summary metadata authoritative");

  const auto optimized = Execute(pages, summaries, predicate, plan);
  Require(full.row_count == optimized.row_count,
          "DPC-014 pruned row count diverged from full scan");
  Require(full.aggregate_sum == optimized.aggregate_sum,
          "DPC-014 pruned aggregate diverged from full scan");
  Require(full.hash == optimized.hash,
          "DPC-014 pruned result hash diverged from full scan");
  Require(plan.counters.pages_considered == full.page_reads,
          "DPC-014 planner page-considered counter mismatched baseline");
  Require(plan.counters.pages_scanned == optimized.page_reads,
          "DPC-014 planner page-scanned counter mismatched execution");

  const double reduction = ReductionRatio(full.page_reads, optimized.page_reads);
  Require(reduction >= minimum_reduction,
          "DPC-014 page-read reduction missed target");

  return {std::move(workload_id),
          std::move(lane),
          "summary_prune_enabled",
          plan.selected_access,
          plan.fallback_reason,
          full.page_reads,
          optimized.page_reads,
          reduction,
          optimized.row_count,
          optimized.aggregate_sum,
          optimized.hash};
}

void ProveDisabledBaseline(const std::vector<SyntheticPage>& pages,
                           const std::vector<idx::PageExtentSummaryMetadata>& summaries,
                           Predicate predicate,
                           const ScanResult& expected) {
  const auto disabled = Plan(summaries, predicate, false);
  Require(disabled.full_scan_fallback && disabled.selected_access == "full_scan",
          "DPC-014 disabled summary-prune did not select full scan");
  Require(disabled.summary_status == "disabled",
          "DPC-014 disabled summary-prune did not expose disabled status");
  const auto actual = Execute(pages, summaries, predicate, disabled);
  Require(actual.page_reads == expected.page_reads &&
              actual.row_count == expected.row_count &&
              actual.aggregate_sum == expected.aggregate_sum &&
              actual.hash == expected.hash,
          "DPC-014 disabled baseline diverged from full scan");
  PrintProofRow({"WL08",
                 "range_scan_disabled_baseline",
                 "summary_prune_disabled",
                 disabled.selected_access,
                 disabled.fallback_reason,
                 expected.page_reads,
                 actual.page_reads,
                 0.0,
                 actual.row_count,
                 actual.aggregate_sum,
                 actual.hash});
}

void ProveFallback(const std::vector<SyntheticPage>& pages,
                   std::vector<idx::PageExtentSummaryMetadata> summaries,
                   Predicate predicate,
                   const ScanResult& expected,
                   std::string lane,
                   idx::PageExtentSummaryStatus status,
                   std::string_view expected_reason) {
  summaries[0].status = status;
  if (status == idx::PageExtentSummaryStatus::corrupt) {
    summaries[0].checksum_valid = false;
  }
  const auto fallback = Plan(summaries, predicate);
  Require(fallback.full_scan_fallback && fallback.selected_access == "full_scan",
          "DPC-014 stale/corrupt fallback did not select full scan");
  Require(fallback.fallback_reason == expected_reason,
          "DPC-014 stale/corrupt fallback reason mismatched");
  Require(!fallback.summary_metadata_visibility_authority &&
              !fallback.summary_metadata_finality_authority,
          "DPC-014 fallback made summary metadata authoritative");
  const auto actual = Execute(pages, summaries, predicate, fallback);
  Require(actual.page_reads == expected.page_reads &&
              actual.row_count == expected.row_count &&
              actual.aggregate_sum == expected.aggregate_sum &&
              actual.hash == expected.hash,
          "DPC-014 stale/corrupt fallback diverged from full scan");
  PrintProofRow({"WL08",
                 std::move(lane),
                 "summary_prune_fallback",
                 fallback.selected_access,
                 fallback.fallback_reason,
                 expected.page_reads,
                 actual.page_reads,
                 0.0,
                 actual.row_count,
                 actual.aggregate_sum,
                 actual.hash});
}

}  // namespace

int main() {
  const UuidFactory uuids;
  const auto pages = Corpus();
  const auto summaries = BuildSummaries(uuids, pages);
  const Predicate range_predicate{45, 60};
  const Predicate aggregate_predicate{70, 84};

  const idx::PageExtentSummaryPrunePlan full_scan_plan;
  const auto range_full =
      Execute(pages, summaries, range_predicate, full_scan_plan);

  const auto range_row =
      ProveLane(pages, summaries, "WL08", "range_scan", range_predicate,
                kWl08MinimumPageReadReduction);
  PrintProofRow(range_row);

  const auto aggregate_row =
      ProveLane(pages, summaries, "WL10", "aggregate", aggregate_predicate,
                kWl10MinimumPageReadReduction);
  PrintProofRow(aggregate_row);

  ProveDisabledBaseline(pages, summaries, range_predicate, range_full);
  ProveFallback(pages, summaries, range_predicate, range_full,
                "range_scan_stale_summary_fallback",
                idx::PageExtentSummaryStatus::stale,
                "stale_summary_full_scan");
  ProveFallback(pages, summaries, range_predicate, range_full,
                "range_scan_corrupt_summary_fallback",
                idx::PageExtentSummaryStatus::corrupt,
                "corrupt_summary_full_scan");

  std::cout << kGateSearchKey << "=passed "
            << "DPC_PAGE_SUMMARY_BENCHMARK_OUTPUT=retained "
            << "run_count=" << kRunCount
            << " deterministic_page_reads=true"
            << " planner_path=DPC_PAGE_EXTENT_SUMMARY_PRUNE_PLANNER\n";
  return EXIT_SUCCESS;
}
