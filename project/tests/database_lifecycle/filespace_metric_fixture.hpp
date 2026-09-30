// Copyright (c) 2026 ScratchBird Software Inc.
// SPDX-License-Identifier: MPL-2.0
#pragma once

#include "metric_builtin_definitions.hpp"
#include "metric_label_key.hpp"
#include "metric_observation_queue.hpp"
#include "metric_value_codec.hpp"
#include <algorithm>
#include <map>
#include <stdexcept>

// Component fixture using the actual descriptor, series, queue and binary codecs.
// This is not native catalog activation or durable metric-history acceptance.
namespace filespace_metric_test {
namespace m = scratchbird::core::metrics;
inline void Check(bool value, const char* detail) {
  if (!value) throw std::runtime_error(detail);
}
inline m::MetricUuid Id(unsigned value) {
  m::MetricUuid id;
  id.bytes = {1,159,0,0,0,0,112,0,128,0,0,0,0,0,
      static_cast<unsigned char>(value >> 8), static_cast<unsigned char>(value)};
  return id;
}
class Fixture {
 public:
  explicit Fixture(std::size_t capacity = 1024) {
    auto made = m::MetricObservationQueue::Create({database, node, {}}, {capacity, 4*1024*1024});
    Check(made.ok(), "create real filespace observation queue");
    queue = std::move(made.queue);
    Check(m::DefaultMetricRegistry().BindObservationQueue(queue).ok, "bind node queue");
    policy.policy_uuid = Id(4); policy.generation = 1;
    policy.policy_name = "filespace component retention";
  }
  void RegisterDescriptors() {
    const auto definitions = m::BuiltinMetricDescriptorDefinitions();
    for (const auto* family : {"sb_filespace_total_bytes", "sb_filespace_used_bytes",
         "sb_filespace_free_bytes", "sb_filespace_reserved_bytes",
         "sb_agent_filespace_capacity_requests_total"}) {
      const auto found = std::find_if(definitions.begin(), definitions.end(),
          [&](const auto& d) { return d.family == family; });
      Check(found != definitions.end(), "missing builtin filespace descriptor");
      m::MetricDescriptor descriptor;
      static_cast<m::MetricDescriptorDefinition&>(descriptor) = *found;
      Check(descriptor.value_type == m::MetricScalarType::uint64, "byte/count schema not uint64");
      descriptor.metric_uuid = Id(next++); descriptor.descriptor_generation = 1;
      descriptor.label_schema_uuid = Id(next++); descriptor.label_schema_generation = 1;
      descriptor.retention_policy_uuid = policy.policy_uuid;
      descriptor.retention_policy_generation = policy.generation;
      descriptor.visibility_policy_uuid = Id(5); descriptor.visibility_policy_generation = 1;
      descriptor.readiness = m::MetricReadiness::implemented;
      Check(m::DefaultMetricRegistry().RegisterDescriptor(descriptor).ok, "register builtin descriptor");
      descriptors.emplace(family, descriptor);
    }
  }
  m::MetricLabelSet Labels(m::MetricUuid filespace) const {
    return {{"component","storage.filespace"},{"database_uuid",database},
        {"filespace_uuid",filespace},{"node_uuid",node},
        {"filespace_role","secondary_data"},{"device_class","file"}};
  }
  void Register(const std::string& family, m::MetricLabelSet labels) {
    const auto& descriptor = descriptors.at(family);
    m::MetricHistoryBinding binding;
    static_cast<m::MetricDescriptorBinding&>(binding) = descriptor;
    binding.database_uuid = database; binding.node_uuid = node;
    auto made = m::MakeMetricSeriesIdentity(descriptor, labels, policy, binding, Id(next++), 1);
    Check(made.ok(), "construct real filespace series");
    Check(m::DefaultMetricRegistry().RegisterSeries(*made.record, policy).ok, "register filespace series");
    series.emplace(made.record->series_uuid, *made.record);
  }
  void RegisterFilespace(m::MetricUuid filespace) {
    for (const auto* family : {"sb_filespace_total_bytes", "sb_filespace_used_bytes", "sb_filespace_free_bytes"})
      Register(family, Labels(filespace));
    auto labels = Labels(filespace);
    labels.push_back({"reason_class","preallocated"});
    Register("sb_filespace_reserved_bytes", labels);
    for (const auto* result : {"completed", "filespace_capacity_manager", "direct_sysarch"}) {
      Register("sb_agent_filespace_capacity_requests_total", {
          {"component","agent.filespace_capacity"},{"agent_type","filespace_capacity_manager"},
          {"filespace_uuid",filespace},
          {"request_class",std::string(result)=="completed" ? "preallocate" : "physical_growth"},
          {"result",result}});
    }
  }
  void Expect(const std::string& family, m::MetricUuid filespace, std::uint64_t value) {
    auto lease = queue->TryAcquire(); Check(lease.ok(), "producer omitted real queue sample");
    const auto& envelope = *lease.lease.observation;
    Check(envelope.binding.database_uuid==database && envelope.binding.node_uuid==node &&
        envelope.binding.cluster_uuid.is_nil(), "sample escaped local binary owner binding");
    const auto& identity = series.at(envelope.series_uuid);
    const auto& descriptor = descriptors.at(family);
    const auto decoded = m::DecodeMetricRawSample(descriptor, identity, envelope.bytes);
    Check(decoded.ok(), "decode sample against expected builtin family");
    Check(std::holds_alternative<std::uint64_t>(decoded.record->value.value) &&
        std::get<std::uint64_t>(decoded.record->value.value)==value, "sample rounded or changed exact uint64 value");
    const auto& labels = decoded.record->value.labels;
    const auto found = std::find_if(labels.begin(), labels.end(),
        [](const auto& label) { return label.key=="filespace_uuid"; });
    Check(found!=labels.end() && std::get<m::MetricUuid>(found->value)==filespace,
        "sample lost binary filespace identity");
    Check(m::MakeMetricSeriesKey(family, labels)==m::MakeMetricSeriesKey(family, identity.labels),
        "sample labels differ from admitted series");
    Check(queue->TryRemove(lease.lease)==m::MetricQueueError::none, "drain verified observation");
  }
  const m::MetricUuid database = Id(1), node = Id(2);
  std::shared_ptr<m::MetricObservationQueue> queue;
 private:
  unsigned next = 100;
  m::MetricRetentionPolicy policy;
  std::map<std::string,m::MetricDescriptor> descriptors;
  std::map<m::MetricUuid,m::MetricSeriesIdentity> series;
};
inline Fixture& Runtime() {
  static Fixture fixture;
  static const bool registered = [&] { fixture.RegisterDescriptors(); return true; }();
  (void)registered;
  return fixture;
}
}  // namespace filespace_metric_test
