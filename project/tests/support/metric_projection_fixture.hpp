// Copyright (c) 2026 ScratchBird Software Inc.
// SPDX-License-Identifier: MPL-2.0
#pragma once

#include "binary_uuid_fixture.hpp"
#include "metric_builtin_definitions.hpp"
#include "metric_label_key.hpp"
#include "metric_observation_queue.hpp"
#include "metric_value_codec.hpp"
#include <algorithm>
#include <map>
#include <stdexcept>

namespace scratchbird::tests {
// Explicit component inputs at the metric producer boundary. This fixture
// qualifies observation admission/projection, NOT catalog activation or the
// storage/datatype operations represented by its supplied measurements.
// Nothing is registered implicitly, and admission never emits a sample.
class MetricProjectionFixture {
 public:
  using Uuid = core::metrics::MetricUuid;
  using Labels = core::metrics::MetricLabelSet;
  using Scalar = core::metrics::MetricScalar;
  MetricProjectionFixture(Uuid database, Uuid node, unsigned fixture_domain,
                          std::size_t observation_capacity = 256)
      : database_(database), node_(node), fixture_domain_(fixture_domain) {
    auto made = core::metrics::MetricObservationQueue::Create(
        {database, node, {}}, {observation_capacity, 1024 * 1024});
    Check(made.ok(), "projection observation queue creation failed");
    queue_ = std::move(made.queue);
    Check(core::metrics::DefaultMetricRegistry().BindObservationQueue(queue_).ok,
          "projection owner binding failed");
    retention_.policy_uuid = Id(); retention_.generation = 1;
    retention_.policy_name = "component projection retention";
  }
  void Admit(const std::string& family, Labels labels) {
    namespace m = core::metrics;
    static const auto definitions = m::BuiltinMetricDescriptorDefinitions();
    const auto found = std::find_if(definitions.begin(), definitions.end(),
          [&](const auto& row) { return row.family == family; });
    Check(found != definitions.end(), "projection family definition missing");
    AdmitDefinition(*found, std::move(labels));
  }
  // An owning component can supply its finite definition set explicitly;
  // fixture UUIDs are not runtime catalog activation or durable evidence.
  void AdmitDefinition(const core::metrics::MetricDescriptorDefinition& definition, Labels labels) {
    namespace m = core::metrics;
    auto& registry = m::DefaultMetricRegistry();
    const auto& family = definition.family;
    if (!descriptors_.contains(family)) {
      m::MetricDescriptor descriptor;
      static_cast<m::MetricDescriptorDefinition&>(descriptor) = definition;
      descriptor.metric_uuid = Id(); descriptor.descriptor_generation = 1;
      descriptor.label_schema_uuid = Id(); descriptor.label_schema_generation = 1;
      descriptor.retention_policy_uuid = retention_.policy_uuid;
      descriptor.retention_policy_generation = retention_.generation;
      descriptor.visibility_policy_uuid = Id(); descriptor.visibility_policy_generation = 1;
      descriptor.readiness = m::MetricReadiness::implemented;
      Check(registry.RegisterDescriptor(descriptor).ok, "projection descriptor refused");
      descriptors_.emplace(family, std::move(descriptor));
    }
    const auto& descriptor = descriptors_.at(family);
    m::MetricHistoryBinding binding;
    static_cast<m::MetricDescriptorBinding&>(binding) = descriptor;
    binding.database_uuid = database_; binding.node_uuid = node_;
    auto series = m::MakeMetricSeriesIdentity(descriptor, labels, retention_, binding, Id(), 1);
    Check(series.ok(), "projection series identity refused");
    Check(registry.RegisterSeries(*series.record, retention_).ok, "projection series refused");
    series_.emplace(series.record->series_uuid, *series.record);
    families_.emplace(series.record->series_uuid, family);
  }
  void Emit(const std::string& family, Labels labels, Scalar value) {
    namespace m = core::metrics;
    const auto& descriptor = descriptors_.at(family);
    auto& registry = m::DefaultMetricRegistry();
    m::MetricValidationResult result;
    switch (descriptor.type) {
      case m::MetricType::counter:
        result = registry.IncrementCounter(family, std::move(labels), std::move(value), descriptor.producer_owner); break;
      case m::MetricType::gauge:
        result = registry.SetGauge(family, std::move(labels), std::move(value), descriptor.producer_owner); break;
      case m::MetricType::histogram:
        result = registry.ObserveHistogram(family, std::move(labels), std::move(value), descriptor.producer_owner); break;
      default: throw std::runtime_error("unsupported projection fixture sample class");
    }
    Produced(result);
  }
  // Account for an independently invoked real producer helper; the queue and
  // decoded samples must subsequently prove every accepted emission.
  void Produced(const core::metrics::MetricValidationResult& result) {
    Check(result.ok, result.diagnostic_code + ":" + result.detail);
    ++emitted_;
  }
  // For subsystem APIs that do not expose individual metric return values.
  // This is an independent expected count, not inferred from the queue;
  // Seal/VerifyAndDrain must prove it against admitted, decoded observations.
  void ExpectProduced(std::size_t count) { emitted_ += count; }
  void Seal() {
    before_ = Snapshot();
    Check(queue_->Stats().admitted == emitted_, "fixture admission emitted or lost observations");
  }
  void VerifyAdmissionRefusals(const std::string& family, const Labels& labels, Scalar value) {
    namespace m = core::metrics;
    const auto& descriptor = descriptors_.at(family);
    auto& registry = m::DefaultMetricRegistry();
    const auto attempt = [&](Labels candidate, const std::string& owner) {
      return descriptor.type == m::MetricType::counter
          ? registry.IncrementCounter(family, std::move(candidate), value, owner)
          : registry.SetGauge(family, std::move(candidate), value, owner);
    };
    Check(!attempt(labels, "foreign_producer").ok, "wrong metric producer accepted");
    auto unregistered = labels;
    unregistered.push_back({"result", "unregistered_fixture_series"});
    Check(!attempt(unregistered, descriptor.producer_owner).ok, "unregistered metric series accepted");
    auto textual = labels;
    std::erase_if(textual, [](const auto& label) { return label.key == "node_uuid"; });
    textual.push_back({"node_uuid", "019d0000-0000-7000-8000-000000000001"});
    Check(!attempt(textual, descriptor.producer_owner).ok, "text system UUID metric label accepted");
    VerifyReadOnly();
  }
  void VerifyReadOnly() const {
    Check(before_ == Snapshot() && queue_->Stats().admitted == emitted_ &&
          queue_->Stats().queued == emitted_, "projection mutated values or observation queue");
  }
  void VerifyAndDrain() {
    namespace m = core::metrics;
    VerifyReadOnly();
    std::size_t count = 0;
    for (;;) {
      auto acquired = queue_->TryAcquire();
      if (acquired.error == m::MetricQueueError::empty) break;
      Check(acquired.ok(), "projection sample acquire failed");
      const auto& sample = *acquired.lease.observation;
      Check(sample.binding.database_uuid == database_ && sample.binding.node_uuid == node_ &&
            sample.binding.cluster_uuid.is_nil(), "projection sample owner mismatch");
      Check(m::DecodeMetricRawSample(descriptors_.at(families_.at(sample.series_uuid)),
              series_.at(sample.series_uuid), sample.bytes).ok(), "projection sample invalid");
      Check(queue_->TryRemove(acquired.lease) == m::MetricQueueError::none,
            "projection sample removal failed");
      ++count;
    }
    Check(count == emitted_ && queue_->Stats().full == 0, "projection sample count mismatch");
  }
 private:
  static void Check(bool ok, const std::string& message) {
    if (!ok) throw std::runtime_error(message);
  }
  Uuid Id() { return FixtureUuid(fixture_domain_, next_++); }
  std::vector<std::vector<core::platform::byte>> Snapshot() const {
    std::vector<std::vector<core::platform::byte>> result;
    for (const auto& value : core::metrics::DefaultMetricRegistry().SnapshotCurrent(false)) {
      auto encoded = core::metrics::EncodeMetricValue(descriptors_.at(value.family), value);
      Check(encoded.ok(), "projection source encoding failed");
      result.push_back(std::move(encoded.bytes));
    }
    return result;
  }
  Uuid database_, node_;
  unsigned fixture_domain_, next_ = 100;
  std::size_t emitted_ = 0;
  core::metrics::MetricRetentionPolicy retention_;
  std::shared_ptr<core::metrics::MetricObservationQueue> queue_;
  std::map<std::string, core::metrics::MetricDescriptor> descriptors_;
  std::map<Uuid, core::metrics::MetricSeriesIdentity> series_;
  std::map<Uuid, std::string> families_;
  std::vector<std::vector<core::platform::byte>> before_;
};
}  // namespace scratchbird::tests
