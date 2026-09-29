// Copyright (c) 2026 ScratchBird Software Inc.
// SPDX-License-Identifier: MPL-2.0
#include "metric_builtin_definitions.hpp"
#include "metric_value_update.hpp"
#include <iostream>
#include <set>
#include <stdexcept>
#include <type_traits>

namespace m = scratchbird::core::metrics;
static_assert(!std::is_base_of_v<m::MetricDescriptorBinding, m::MetricDescriptorDefinition>);

void Require(bool value, const std::string& reason) {
  if (!value) throw std::runtime_error(reason);
}

int main() {
  try {
    m::MetricRegistry registry;
    Require(registry.Descriptors().empty(), "unbound registry invented descriptor authority");
    auto definitions = m::BuiltinMetricDescriptorDefinitions();
    Require(definitions.size() == 303, "compiled family inventory changed");
    std::set<std::string> families;
    std::size_t local = 0, cluster = 0;
    for (const auto& definition : definitions) {
      Require(families.insert(definition.family).second, "duplicate family: " + definition.family);
      Require(m::ValidateStoredMetricValueDescriptor(definition),
              "invalid exact value definition: " + definition.family);
      Require(definition.unit != m::MetricUnit::count && definition.unit != m::MetricUnit::state,
              "retired unit in definition: " + definition.family);
      Require(definition.cluster_only == definition.namespace_path.starts_with("cluster.sys.metrics."),
              "definition scope mismatch: " + definition.family);
      definition.cluster_only ? ++cluster : ++local;
      if (definition.type == m::MetricType::counter)
        Require(definition.value_type == m::MetricScalarType::uint64,
                "event or byte counter lost exact unsigned value: " + definition.family);
      if (definition.family == "sb_dml_insert_rows_per_batch") {
        Require(definition.value_type == m::MetricScalarType::uint64,
                "row count histogram lost exact unsigned values");
        Require(definition.histogram_buckets == std::vector<m::MetricScalar>{
                    m::u64{1},m::u64{5},m::u64{10},m::u64{50},m::u64{100},
                    m::u64{500},m::u64{1000},m::u64{5000},m::u64{10000},
                    m::u64{50000},m::u64{100000},m::u64{500000},m::u64{1000000}},
                "row count histogram has inexact or changed bounds");
        const auto exact = m::StageMetricValueUpdate(
            definition, {}, nullptr, m::u64{9007199254740993ULL});
        Require(exact.ok() &&
                    std::get<m::u64>(exact.value->value) == m::u64{9007199254740993ULL},
                "compiled row histogram rounded a count above binary64 exact range");
      }
      if (definition.family == "sb_cluster_node_role_state")
        Require(definition.enum_values == std::vector<m::u64>{0,1,2,3,4,5,6,7,8,9,10,11},
                "cluster lifecycle definition omits node states");
      if (definition.family == "sb_filespace_role_state")
        Require(definition.enum_values == std::vector<m::u64>{0,1,2,3,4,5,6,7,8,9,10,11,12,13,14,15,16},
                "filespace role definition collapses distinct lifecycle roles");
      if (definition.family == "sb_filespace_growth_rate_bytes_per_second" ||
          definition.family == "sb_page_family_growth_rate_pages_per_second")
        Require(definition.value_type == m::MetricScalarType::float64,
                "fractional growth rate declared as integral capacity");
      if (definition.type == m::MetricType::gauge && definition.unit == m::MetricUnit::bytes &&
          definition.family != "sb_filespace_growth_rate_bytes_per_second")
        Require(definition.value_type == m::MetricScalarType::uint64,
                "capacity gauge lost exact unsigned bytes: " + definition.family);
      m::MetricDescriptor unbound;
      static_cast<m::MetricDescriptorDefinition&>(unbound) = definition;
      const auto rejected = registry.RegisterDescriptor(std::move(unbound));
      Require(!rejected.ok && rejected.diagnostic_code == "METRIC.VALUE_INVALID",
              "compiled definition acquired runtime identity/readiness: " + definition.family);
    }
    Require(local > 100 && cluster != 0, "definition scope inventories missing");
    Require(registry.Descriptors().empty() && registry.SnapshotCurrent().empty(),
            "definition enumeration or rejected registration mutated runtime state");
    definitions.front().family = "caller_mutation";
    Require(m::BuiltinMetricDescriptorDefinitions().front().family != "caller_mutation",
            "definition enumeration shared mutable state");
    std::cout << "metric_builtin_definitions=passed families=" << families.size()
              << " local=" << local << " cluster=" << cluster
              << " runtime_activation_claimed=false\n";
  } catch (const std::exception& error) {
    std::cerr << error.what() << '\n';
    return 1;
  }
}
