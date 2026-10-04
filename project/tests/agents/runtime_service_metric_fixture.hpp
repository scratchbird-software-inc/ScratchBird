// Copyright (c) 2026 ScratchBird Software Inc.
// SPDX-License-Identifier: MPL-2.0
#pragma once

#include "../support/binary_uuid_fixture.hpp"
#include "metric_builtin_definitions.hpp"
#include "metric_label_key.hpp"
#include "metric_observation_queue.hpp"
#include "metric_value_codec.hpp"
#include <algorithm>
#include <map>
#include <stdexcept>

// Component admission fixture, not native catalog activation. Values and clocks
// come exclusively from the actual runtime-service producer and registry. Each
// process owns exactly one real database, matching the SBsvr ownership model.
namespace runtime_service_metric_test {
namespace m = scratchbird::core::metrics;
inline const auto node = scratchbird::tests::FixtureUuidLiteral(
    "019f0000-0000-7000-8000-00000000ee01");
inline void Check(bool ok, const char* message) {
  if (!ok) throw std::runtime_error(message);
}
class Fixture {
 public:
  explicit Fixture(m::MetricUuid database) : database_(database) {
    auto made = m::MetricObservationQueue::Create({database, node, {}}, {4096, 4*1024*1024});
    Check(made.ok(), "create service observation queue");
    queue_ = std::move(made.queue);
    Check(m::DefaultMetricRegistry().BindObservationQueue(queue_).ok, "bind real service database");
    policy_.policy_uuid = Id(); policy_.generation = 1;
    policy_.policy_name = "runtime service component retention";
    const auto definitions = m::BuiltinMetricDescriptorDefinitions();
    for (const auto* family : {"sb_agent_runtime_service_leases", "sb_agent_runtime_service_actions",
         "sb_agent_runtime_service_history_records", "sb_agent_runtime_service_catalog_generation"}) {
      const auto found = std::find_if(definitions.begin(), definitions.end(),
          [&](const auto& d) { return d.family == family; });
      Check(found != definitions.end(), "missing canonical service metric definition");
      m::MetricDescriptor descriptor;
      static_cast<m::MetricDescriptorDefinition&>(descriptor) = *found;
      descriptor.metric_uuid = Id(); descriptor.descriptor_generation = 1;
      descriptor.label_schema_uuid = Id(); descriptor.label_schema_generation = 1;
      descriptor.retention_policy_uuid = policy_.policy_uuid;
      descriptor.retention_policy_generation = policy_.generation;
      descriptor.visibility_policy_uuid = Id(); descriptor.visibility_policy_generation = 1;
      descriptor.readiness = m::MetricReadiness::implemented;
      Check(m::DefaultMetricRegistry().RegisterDescriptor(descriptor).ok, "register service descriptor");
      descriptors_.emplace(family, descriptor);
    }
    for (const auto* state : {"none", "acquired", "draining", "cancelled", "quarantined", "replay_pending", "expired"})
      Register("sb_agent_runtime_service_leases", {{"component","agent.runtime_service"},{"state",state}});
    for (const auto* state : {"pending", "running", "completed", "cancelled", "replay_pending", "quarantined"})
      Register("sb_agent_runtime_service_actions", {{"component","agent.runtime_service"},{"state",state}});
    Register("sb_agent_runtime_service_history_records", {{"component","agent.runtime_service"}});
    Register("sb_agent_runtime_service_catalog_generation", {{"component","agent.runtime_service"}});
  }
  void VerifyAndDrain() {
    std::size_t count = 0;
    for (;;) {
      auto acquired = queue_->TryAcquire();
      if (acquired.error == m::MetricQueueError::empty) break;
      Check(acquired.ok(), "acquire actual service sample");
      const auto& sample = *acquired.lease.observation;
      Check(sample.binding.database_uuid == database_ && sample.binding.node_uuid == node &&
            sample.binding.cluster_uuid.is_nil(), "wrong service observation owner");
      const auto& series = series_.at(sample.series_uuid);
      const auto& descriptor = descriptors_.at(families_.at(sample.series_uuid));
      Check(m::DecodeMetricRawSample(descriptor, series, sample.bytes).ok(), "invalid producer sample");
      Check(queue_->TryRemove(acquired.lease) == m::MetricQueueError::none, "remove service observation");
      ++count;
    }
    Check(count > 0, "service produced no real observations");
    Check(queue_->Stats().full == 0, "service observation queue overflow");
  }
  void VerifyEmpty() const {
    Check(queue_->Stats().admitted == 0 &&
          m::DefaultMetricRegistry().SnapshotCurrent(false).empty(),
          "unadmitted or foreign-owner service emitted metric observations");
  }
 private:
  m::MetricUuid Id() {
    auto id = scratchbird::tests::FixtureUuidLiteral("019f0000-0000-7000-8000-00000000ef00");
    id.bytes[15] = next_++;
    return id;
  }
  void Register(const std::string& family, m::MetricLabelSet labels) {
    const auto& descriptor = descriptors_.at(family);
    m::MetricHistoryBinding binding;
    static_cast<m::MetricDescriptorBinding&>(binding) = descriptor;
    binding.database_uuid = database_; binding.node_uuid = node;
    auto made = m::MakeMetricSeriesIdentity(descriptor, labels, policy_, binding, Id(), 1);
    Check(made.ok(), "construct owning service series");
    Check(m::DefaultMetricRegistry().RegisterSeries(*made.record, policy_).ok, "register service series");
    series_.emplace(made.record->series_uuid, *made.record);
    families_.emplace(made.record->series_uuid, family);
  }
  unsigned char next_ = 1;
  m::MetricUuid database_;
  m::MetricRetentionPolicy policy_;
  std::shared_ptr<m::MetricObservationQueue> queue_;
  std::map<std::string, m::MetricDescriptor> descriptors_;
  std::map<m::MetricUuid, m::MetricSeriesIdentity> series_;
  std::map<m::MetricUuid, std::string> families_;
};
}
