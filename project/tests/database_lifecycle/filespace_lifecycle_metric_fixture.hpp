// Copyright (c) 2026 ScratchBird Software Inc.
// SPDX-License-Identifier: MPL-2.0
#pragma once
#include "metric_builtin_definitions.hpp"
#include "metric_observation_queue.hpp"
#include "metric_value_codec.hpp"
#include <algorithm>
#include <map>
#include <stdexcept>

namespace filespace_lifecycle_metric_test {
namespace m = scratchbird::core::metrics;
inline void Check(bool ok, const char* message) {
  if (!ok) throw std::runtime_error(message);
}
inline m::MetricUuid Id(unsigned value) {
  return {{{1,159,0,0,0,0,0x70,0,0x80,0,0,0,0,0,
            static_cast<unsigned char>(value >> 8),static_cast<unsigned char>(value)}}};
}
// Real production queue/descriptor/series admission. Does not qualify native
// catalog activation, history persistence or physical filespace creation.
class Fixture {
 public:
  Fixture(m::MetricUuid database, m::MetricUuid primary, m::MetricUuid secondary)
      : database_(database) {
    Bind(3);
    policy_.policy_uuid = Id(3); policy_.generation = 1;
    policy_.policy_name = "filespace lifecycle component";
    const auto definitions = m::BuiltinMetricDescriptorDefinitions();
    for (const auto* family : {"sb_storage_filespace_lifecycle_total",
                              "sb_storage_filespace_active_pins", "sb_filespace_role_state"}) {
      const auto found = std::find_if(definitions.begin(), definitions.end(),
          [&](const auto& d) { return d.family == family; });
      Check(found != definitions.end(), "missing lifecycle metric definition");
      m::MetricDescriptor d;
      static_cast<m::MetricDescriptorDefinition&>(d) = *found;
      d.metric_uuid = Id(next_++); d.descriptor_generation = 1;
      d.label_schema_uuid = Id(next_++); d.label_schema_generation = 1;
      d.retention_policy_uuid = policy_.policy_uuid; d.retention_policy_generation = 1;
      d.visibility_policy_uuid = Id(4); d.visibility_policy_generation = 1;
      d.readiness = m::MetricReadiness::implemented;
      Check(m::DefaultMetricRegistry().RegisterDescriptor(d).ok, "register lifecycle descriptor");
      descriptors_.emplace(family, d);
    }
    for (const auto& [id, role] : {std::pair{primary,"active_primary"},
                                 std::pair{secondary,"secondary_data"}}) {
      auto role_labels = Scope(id);
      role_labels.push_back({"filespace_role",role});
      role_labels.push_back({"device_class","file"});
      role_labels.push_back({"state",role});
      Register("sb_filespace_role_state", role_labels);
      for (const auto* state : {"online","detached"}) {
        auto pins = Scope(id);
        pins.push_back({"role",role}); pins.push_back({"state",state});
        Register("sb_storage_filespace_active_pins", pins);
      }
      for (const auto* operation : {"create_filespace","attach_filespace","pin_filespace",
                                     "unpin_filespace","detach_filespace"}) {
        auto labels = Scope(id);
        labels.push_back({"operation",operation}); labels.push_back({"result","ok"});
        labels.push_back({"reason","ok"});
        Register("sb_storage_filespace_lifecycle_total", labels);
      }
    }
    auto error = Scope(secondary);
    error.push_back({"operation","detach_filespace"}); error.push_back({"result","error"});
    error.push_back({"reason","SB-FILESPACE-LIFECYCLE-DETACH-PINNED"});
    Register("sb_storage_filespace_lifecycle_total", error);
  }
  void Bind(std::size_t capacity) {
    auto made = m::MetricObservationQueue::Create({database_,node_,{}}, {capacity,1024*1024});
    Check(made.ok(), "create lifecycle queue");
    queue_ = std::move(made.queue);
    Check(m::DefaultMetricRegistry().BindObservationQueue(queue_).ok, "bind lifecycle queue");
  }
  void Expect(const std::string& family, m::MetricUuid filespace, m::MetricScalar value,
              const std::string& operation = {}, const std::string& outcome = "ok") {
    auto lease = queue_->TryAcquire();
    Check(lease.ok(), "missing lifecycle queue observation");
    const auto& envelope = *lease.lease.observation;
    Check(envelope.binding.database_uuid == database_ && envelope.binding.node_uuid == node_,
          "lifecycle sample escaped binary node/database owner");
    const auto& identity = series_.at(envelope.series_uuid);
    const auto decoded = m::DecodeMetricRawSample(descriptors_.at(family), identity, envelope.bytes);
    Check(decoded.ok() && decoded.record->value.value == value, "wrong exact lifecycle sample value/type");
    const auto& labels = decoded.record->value.labels;
    const auto find = [&](const char* key) {
      return std::find_if(labels.begin(), labels.end(), [&](const auto& l) { return l.key == key; });
    };
    const auto owner = find("filespace_uuid");
    Check(owner != labels.end() && std::get<m::MetricUuid>(owner->value) == filespace,
          "lifecycle sample lost binary filespace owner");
    if (!operation.empty()) {
      const auto op = find("operation"), result = find("result");
      Check(op != labels.end() && std::get<std::string>(op->value) == operation &&
            result != labels.end() && std::get<std::string>(result->value) == outcome,
            "lifecycle operation/outcome mismatch");
    }
    Check(queue_->TryRemove(lease.lease) == m::MetricQueueError::none, "drain lifecycle sample");
  }
  void Success(m::MetricUuid id, const std::string& operation, std::uint64_t pins,
               std::uint64_t role) {
    Expect("sb_storage_filespace_lifecycle_total", id, std::uint64_t{1}, operation);
    Expect("sb_storage_filespace_active_pins", id, pins);
    Expect("sb_filespace_role_state", id, m::MetricEnumValue{role});
  }
  void Empty() { Check(!queue_->TryAcquire().ok(), "unexpected extra lifecycle observation"); }
 private:
  m::MetricLabelSet Scope(m::MetricUuid id) const {
    return {{"component","storage.filespace"},{"database_uuid",database_},
            {"filespace_uuid",id},{"node_uuid",node_}};
  }
  void Register(const std::string& family, m::MetricLabelSet labels) {
    const auto& descriptor = descriptors_.at(family);
    m::MetricHistoryBinding binding;
    static_cast<m::MetricDescriptorBinding&>(binding) = descriptor;
    binding.database_uuid = database_; binding.node_uuid = node_;
    auto identity = m::MakeMetricSeriesIdentity(descriptor, labels, policy_, binding, Id(next_++), 1);
    Check(identity.ok(), "make lifecycle series");
    Check(m::DefaultMetricRegistry().RegisterSeries(*identity.record, policy_).ok, "register lifecycle series");
    series_.emplace(identity.record->series_uuid, *identity.record);
  }
  m::MetricUuid database_, node_ = Id(2);
  unsigned next_ = 100;
  m::MetricRetentionPolicy policy_;
  std::shared_ptr<m::MetricObservationQueue> queue_;
  std::map<std::string,m::MetricDescriptor> descriptors_;
  std::map<m::MetricUuid,m::MetricSeriesIdentity> series_;
};
} // namespace filespace_lifecycle_metric_test
