// Copyright (c) 2026 ScratchBird Software Inc.
// SPDX-License-Identifier: MPL-2.0
#include "../../src/engine/optimizer/statistics_catalog.hpp"
#include "binary_uuid_fixture.hpp"
#include <cmath>
#include <cstdlib>
#include <iostream>
#include <limits>
#include <new>
#include <stdexcept>
#include <type_traits>

namespace fault {
thread_local long remaining = -1;
thread_local bool hit = false;
}
void* operator new(std::size_t n) {
  if (fault::remaining >= 0 && fault::remaining-- == 0) {
    fault::remaining = 0; fault::hit = true; throw std::bad_alloc();
  }
  if (void* p = std::malloc(n ? n : 1)) return p;
  throw std::bad_alloc();
}
void* operator new[](std::size_t n) { return ::operator new(n); }
void operator delete(void* p) noexcept { std::free(p); }
void operator delete[](void* p) noexcept { std::free(p); }
void operator delete(void* p, std::size_t) noexcept { std::free(p); }
void operator delete[](void* p, std::size_t) noexcept { std::free(p); }

namespace o = scratchbird::engine::optimizer;
using Target = o::OptimizerStatisticTarget;
using Kind = o::OptimizerStatisticTargetKind;
using Reason = o::StatisticsContractReason;
using Uuid = scratchbird::engine::planner::CanonicalPlannerUuid;
static_assert(sizeof(Uuid) == 16);
static_assert(!std::is_constructible_v<Target, std::string>);
namespace {
unsigned checks = 0, faults = 0;
void Check(bool condition, const char* message) {
  ++checks;
  if (!condition) throw std::runtime_error(message);
}
Uuid Id(unsigned suffix) {
  auto id = scratchbird::tests::BinaryUuid("019f0000-0000-7600-8000-000000000000");
  id.bytes[14] = suffix >> 8; id.bytes[15] = suffix; return id;
}
o::OptimizerStatistic Row(double value = 42) {
  return o::MakeStatistic("row_count", "relation", Target::Object(Id(1)), value,
      o::StatisticSource::kCatalogExact, 7, 100, o::CostConfidence::kExact);
}
bool Same(const o::OptimizerStatistic& a, const o::OptimizerStatistic& b) {
  return a.statistic_name == b.statistic_name && a.scope == b.scope && a.target == b.target &&
      a.value == b.value && a.source == b.source && a.stats_epoch == b.stats_epoch &&
      a.freshness_microseconds == b.freshness_microseconds && a.confidence == b.confidence &&
      a.available == b.available && a.cluster_only == b.cluster_only &&
      a.value_domain == b.value_domain && a.exact_unsigned_value == b.exact_unsigned_value;
}
bool Same(const o::OptimizerStatisticsCatalog& a, const o::OptimizerStatisticsCatalog& b) {
  const auto& x = a.Statistics(); const auto& y = b.Statistics();
  if (x.size() != y.size()) return false;
  for (std::size_t i = 0; i < x.size(); ++i) if (!Same(x[i], y[i])) return false;
  return true;
}
void VerifyIdentityAndIsolation() {
  o::OptimizerStatisticsCatalog catalog;
  auto original = Row();
  Check(catalog.Add(original), "real object statistic insertion");
  Check(!catalog.Find("row_count", Target{}), "nil object is not wildcard");
  Check(!catalog.Find("row_count", Target::Object(Id(2))), "unrelated object cannot match");
  Check(!catalog.Find("row_count", Target::LocalDefault()), "object cannot satisfy default selection");
  Check(!catalog.Find("page_count", original.target), "statistic name must match");
  auto changed = original; changed.value = 99;
  Check(!catalog.Add(changed), "duplicate binding cannot silently shadow");
  Check(Same(*catalog.Find("row_count", original.target), original), "duplicate leaves retained value intact");
  original.value = 200;
  Check(catalog.EstimateUnsigned("row_count", original.target, 999) == 42, "catalog retains independent actual value");
  for (unsigned bit = 0; bit < 128; ++bit) {
    auto row = Row(1000 + bit);
    row.target.object_uuid.bytes[bit / 8] ^= 1U << (bit % 8);
    const auto& bytes = row.target.object_uuid.bytes;
    const bool fixture_v7 = (bytes[6] >> 4) == 7 && (bytes[8] & 0xc0) == 0x80;
    Check(catalog.Add(row) == fixture_v7, "independent v7 admission oracle");
    const auto found = catalog.Find("row_count", row.target);
    Check(found.has_value() == fixture_v7, "all128 bits distinguish target");
    if (found) Check(Same(*found, row), "exact mutated object retains actual statistic");
    Check(catalog.EstimateUnsigned("row_count", Target::Object(Id(1)), 999) == 42,
          "mutated object cannot alter original");
  }
  for (const auto target : {Target{Kind::kObject, {}}, Target{Kind::kLocalDefault, Id(1)},
                            Target{Kind::kClusterUnavailable, Id(1)},
                            Target{static_cast<Kind>(255), {}}}) {
    auto row = Row(); row.target = target;
    Check(!catalog.Add(row), "invalid tagged target refuses");
    Check(!catalog.Find("row_count", target), "invalid tagged lookup refuses");
    const auto status = o::ValidateStatistic(row, 60000000);
    Check(!status.ok && status.reason == Reason::kTargetInvalid, "typed target refusal");
    Check(status.diagnostic_code == "SB-STAT-0001", "registered diagnostic only");
    Check(status.object_uuid == target.object_uuid, "diagnostic binary object context");
  }
}
void VerifyNumericValues() {
  for (const auto value : {UINT64_C(0), UINT64_C(9007199254740993), UINT64_MAX}) {
    auto row = o::MakeUnsignedStatistic("row_count", "relation", Target::Object(Id(1)),
        value, o::StatisticSource::kCatalogExact, 7, 0, o::CostConfidence::kExact);
    Check(o::CheckedOptimizerStatisticUnsigned(row) == value, "exact full-width count");
    row.value = -1;
    Check(!o::CheckedOptimizerStatisticUnsigned(row), "exact count cannot override malformed approximation");
  }
  for (double value : {-1.0, -0.75, 0.0, 1.0}) {
    auto row = Row(value); row.value_domain = o::OptimizerStatisticValueDomain::kSignedCorrelation;
    Check(o::ValidateStatistic(row, 60000000).ok, "signed correlation domain");
    Check(!o::CheckedOptimizerStatisticUnsigned(row), "correlation cannot become unsigned count");
    row.exact_unsigned_value = 0;
    Check(!o::ValidateStatistic(row, 60000000).ok, "correlation cannot carry exact count");
  }
  for (double value : {-1.01, 1.01, std::numeric_limits<double>::quiet_NaN()}) {
    auto row = Row(value); row.value_domain = o::OptimizerStatisticValueDomain::kSignedCorrelation;
    Check(!o::ValidateStatistic(row, 60000000).ok, "invalid signed correlation");
  }
  struct Case { double input; std::optional<std::uint64_t> expected; };
  const Case cases[] = {{0, 0}, {0.49, 0}, {0.5, 1}, {1.49, 1}, {1.5, 2},
    {9223372036854775808.0, UINT64_C(9223372036854775808)},
    {18446744073709549568.0, UINT64_C(18446744073709549568)},
    {18446744073709551616.0, std::nullopt},
    {std::numeric_limits<double>::max(), std::nullopt}};
  for (const auto& item : cases) {
    o::OptimizerStatisticsCatalog c; auto row = Row(item.input);
    Check(c.Add(row), "finite nonnegative measurement admission");
    Check(c.TryEstimateUnsigned("row_count", row.target) == item.expected,
          "checked uint64 rounding across signed and unsigned limits");
    Check(c.EstimateUnsigned("row_count", row.target, 777) == item.expected.value_or(777),
          "zero is not missing and overflow uses explicit fallback");
  }
  for (double invalid : {-1.0, -0.01, std::numeric_limits<double>::quiet_NaN(),
                         std::numeric_limits<double>::infinity(),
                         -std::numeric_limits<double>::infinity()}) {
    o::OptimizerStatisticsCatalog c; auto row = Row(invalid);
    Check(!c.Add(row), "nonfinite/negative admission refusal");
    auto status = o::ValidateStatistic(row, 60000000);
    Check(!status.ok && status.reason == Reason::kValueInvalid, "numeric refusal reason");
    Check(!c.TryEstimateUnsigned("row_count", row.target), "invalid value never enters cast");
  }
  for (std::uint64_t age : {60000000ULL, 60000001ULL}) {
    auto row = Row(0); row.freshness_microseconds = age; o::OptimizerStatisticsCatalog c;
    Check(c.Add(row), "stale metadata remains inspectable");
    Check(c.Find("row_count", row.target).has_value(), "metadata lookup is not usability");
    Check(c.TryEstimateUnsigned("row_count", row.target).has_value() == (age == 60000000),
          "exact freshness boundary");
    Check((c.ConfidenceFor("row_count", row.target) == o::CostConfidence::kExact) == (age == 60000000),
          "stale data cannot lend exact confidence");
  }
}
void VerifyProvenance() {
  auto c = o::DefaultLocalStatisticsCatalog();
  Check(c.Statistics().size() == 6, "complete local policy inventory");
  for (const auto& row : c.Statistics()) {
    Check(row.target == Target::LocalDefault() && row.target.object_uuid.is_nil(), "default not object identity");
    Check(row.source == o::StatisticSource::kPolicyDefault && row.confidence == o::CostConfidence::kLow,
          "no fabricated runtime measurement");
    Check(c.UsesPolicyDefault(row.statistic_name, row.target), "explicit default provenance");
  }
  Check(!c.Find("row_count", Target::Object(Id(1))), "no implicit default object fallback");
  Check(c.TryEstimateUnsigned("row_count", Target::LocalDefault()) == 1000, "explicit policy lookup");
  auto status = c.ValidateBenchmarkCleanInputs({"row_count"}, Target::Object(Id(1)));
  Check(!status[0].ok && status[0].reason == Reason::kLocalDefault, "default not benchmark-clean");
  auto zero = Row(0); Check(c.Add(zero), "real zero alongside defaults");
  Check(c.TryEstimateUnsigned("row_count", zero.target) == 0, "default does not override measured zero");
  status = c.ValidateBenchmarkCleanInputs({"row_count"}, zero.target);
  Check(status.size() == 1 && status[0].ok && status[0].diagnostic_code.empty(), "real zero benchmark input");
  auto fake = Row(); fake.target = Target::LocalDefault(); fake.source = o::StatisticSource::kRuntimeMetric;
  Check(!c.Add(fake), "unscoped constant cannot impersonate runtime data");
  for (const auto source : {o::StatisticSource::kRuntimeMetric, o::StatisticSource::kClusterMetric}) {
    o::OptimizerStatisticsCatalog feedback;
    auto row = Row(); row.source = source;
    row.cluster_only = source == o::StatisticSource::kClusterMetric;
    Check(feedback.Add(row), "scoped advisory measurement");
    const auto status = feedback.ValidateBenchmarkCleanInputs({"row_count"}, row.target);
    Check(!status[0].ok, "runtime feedback is not catalog benchmark authority");
    if (row.cluster_only) {
      row.cluster_only = false;
      Check(!o::ValidateStatistic(row, 60000000).ok, "cluster metric cannot hide its source domain");
    }
  }
  for (auto reason : {Reason::kNameRequired, Reason::kScopeRequired, Reason::kSourceInvalid,
                      Reason::kConfidenceUnusable, Reason::kEpochInvalid}) {
    auto row = Row();
    if (reason == Reason::kNameRequired) row.statistic_name.clear();
    if (reason == Reason::kScopeRequired) row.scope.clear();
    if (reason == Reason::kSourceInvalid) row.source = static_cast<o::StatisticSource>(255);
    if (reason == Reason::kConfidenceUnusable) row.confidence = o::CostConfidence::kUnknown;
    if (reason == Reason::kEpochInvalid) row.stats_epoch = 0;
    Check(!c.Add(row), "invalid provenance admission");
    Check(o::ValidateStatistic(row, 60000000).reason == reason, "exact provenance refusal reason");
  }
  Check(o::AddClusterUnavailableStatistics(&c), "complete unavailable cluster inventory");
  Check(c.Statistics().size() == 12, "six defaults plus real object plus five unavailable records");
  for (const char* name : {"cluster_remote_stats_unavailable", "cluster_authority_unavailable",
                         "cluster_route_generation_unavailable", "cluster_safe_execution_fence_unavailable",
                         "cluster_remote_execution_unavailable"}) {
    auto row = c.Find(name, Target::ClusterUnavailable());
    Check(row && !row->available && row->source == o::StatisticSource::kUnavailable && row->cluster_only,
          "cluster absence never measurement or capability");
    Check(!c.TryEstimateUnsigned(name, Target::ClusterUnavailable()), "unavailable cannot cost as zero");
    Check(!c.Find(name, Target::Object(Id(1))), "cluster absence cannot match object");
  }
  auto before = c;
  Check(!o::AddClusterUnavailableStatistics(&c), "duplicate inventory refused");
  Check(Same(before, c), "duplicate inventory leaves catalog unchanged");
  Check(!o::AddClusterUnavailableStatistics(nullptr), "null catalog refusal");
}
void VerifyFaults() {
  auto initial = o::DefaultLocalStatisticsCatalog();
  auto item = Row(); item.statistic_name = std::string(200, 's'); item.scope = std::string(100, 'r');
  for (unsigned operation = 0; operation != 2; ++operation) {
    bool finished = false;
    for (long index = 0; index < 4096; ++index) {
      auto c = initial;
      fault::remaining = index; fault::hit = false;
      bool success = false, escaped = false;
      try { success = operation == 0 ? c.Add(item) : o::AddClusterUnavailableStatistics(&c); }
      catch (...) { escaped = true; }
      const bool hit = fault::hit; fault::remaining = -1;
      Check(!escaped, "publication contains allocation failures");
      if (!hit) { Check(success, "unfaulted publication succeeds"); finished = true; break; }
      ++faults; Check(!success, "OOM cannot report successful publication");
      Check(Same(c, initial), "OOM preserves complete prior catalog");
    }
    Check(finished, "allocation sweep exhausted");
  }
  bool finished = false;
  for (long index = 0; index < 4096; ++index) {
    std::optional<o::OptimizerStatisticsCatalog> result;
    fault::remaining = index; fault::hit = false;
    try { result = o::DefaultLocalStatisticsCatalog(); } catch (const std::bad_alloc&) {}
    const bool hit = fault::hit; fault::remaining = -1;
    if (!hit) {
      Check(result && result->Statistics().size() == 6, "factory publishes full policy inventory");
      finished = true; break;
    }
    ++faults; Check(!result, "factory OOM publishes no partial policy inventory");
  }
  Check(finished, "factory allocation sweep exhausted");
}
}
int main() {
  try {
    VerifyIdentityAndIsolation(); VerifyNumericValues(); VerifyProvenance(); VerifyFaults();
    std::cout << "PASS statistics binary targets checks=" << checks << " faults=" << faults << '\n';
  } catch (const std::exception& error) {
    fault::remaining = -1; std::cerr << error.what() << '\n'; return 1;
  }
}
