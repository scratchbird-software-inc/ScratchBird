// Copyright (c) 2026 ScratchBird Software Inc.
//
// This Source Code Form is subject to the terms of the Mozilla Public
// License, v. 2.0. If a copy of the MPL was not distributed with this
// file, You can obtain one at https://mozilla.org/MPL/2.0/.
//
// SPDX-License-Identifier: MPL-2.0

#include "metric_registry.hpp"
#include "metric_label_key.hpp"
#include "metric_value_update.hpp"

#include "metric_history.hpp"
#include "metric_observation_queue.hpp"
#include "time.hpp"

#include <algorithm>
#include <cstddef>
#include <limits>
#include <new>
#include <set>
#include <sstream>
#include <stdexcept>
#include <type_traits>
#include <utility>

namespace scratchbird::core::metrics {
namespace {

bool StartsWith(const std::string& value, const std::string& prefix) {
  return value.rfind(prefix, 0) == 0;
}

}  // namespace

MetricValidationResult MetricOk() {
  return {true, "SB_METRICS_OK", {}};
}

MetricValidationResult MetricError(std::string code, std::string detail) {
  return {false, std::move(code), std::move(detail)};
}

const char* MetricTypeName(MetricType type) {
  switch (type) {
    case MetricType::counter: return "counter";
    case MetricType::gauge: return "gauge";
    case MetricType::histogram: return "histogram";
    case MetricType::state: return "state";
    case MetricType::rate: return "rate";
    case MetricType::sample: return "sample";
    case MetricType::derived: return "derived";
  }
  return "unknown";
}

const char* MetricUnitName(MetricUnit unit) {
  switch (unit) {
    case MetricUnit::count: return "count";
    case MetricUnit::bytes: return "bytes";
    case MetricUnit::pages: return "pages";
    case MetricUnit::records: return "records";
    case MetricUnit::transactions: return "transactions";
    case MetricUnit::operations: return "operations";
    case MetricUnit::milliseconds: return "milliseconds";
    case MetricUnit::nanoseconds: return "nanoseconds";
    case MetricUnit::revisions: return "revisions";
    case MetricUnit::events: return "events";
    case MetricUnit::errors: return "errors";
    case MetricUnit::conflicts: return "conflicts";
    case MetricUnit::microseconds: return "microseconds";
    case MetricUnit::seconds: return "seconds";
    case MetricUnit::percent: return "percent";
    case MetricUnit::ratio: return "ratio";
    case MetricUnit::state: return "state";
    case MetricUnit::none: return "none";
  }
  return "unknown";
}

const char* MetricVisibilityScopeName(MetricVisibilityScope scope) {
  switch (scope) {
    case MetricVisibilityScope::baseline: return "baseline";
    case MetricVisibilityScope::self: return "self";
    case MetricVisibilityScope::family: return "family";
    case MetricVisibilityScope::all: return "all";
    case MetricVisibilityScope::cluster: return "cluster";
  }
  return "unknown";
}

const char* MetricReadinessName(MetricReadiness readiness) {
  switch (readiness) {
    case MetricReadiness::unvalidated: return "unvalidated";
    case MetricReadiness::implemented: return "implemented";
    case MetricReadiness::contract_ready_unwired: return "contract_ready_unwired";
    case MetricReadiness::derived: return "derived";
  }
  return "unknown";
}

struct MetricRegistry::ObservationState {
  std::shared_ptr<MetricObservationQueue> queue;
  core::time::LocalTimeAuthorityState clock;
  u64 next_sequence=1;
};

MetricRegistry::MetricRegistry():MetricRegistry(std::shared_ptr<MetricObservationQueue>{}) {}
MetricRegistry::MetricRegistry(std::shared_ptr<MetricObservationQueue> queue)
    :observation_(std::make_unique<ObservationState>()) {
  if (queue && !queue->binding().cluster_uuid.is_nil())
    throw std::invalid_argument("local metric registry cannot own a cluster observation queue");
  observation_->queue=std::move(queue);
}
MetricRegistry::~MetricRegistry()=default;

MetricValidationResult MetricRegistry::BindObservationQueue(
    std::shared_ptr<MetricObservationQueue> queue) {
  std::lock_guard<std::mutex> lock(mutex_);
  if (!queue || observation_->queue || !queue->binding().cluster_uuid.is_nil())
    return MetricError("METRIC.OBSERVATION_SOURCE_UNAVAILABLE",
                       "local node queue missing or already bound");
  auto success = MetricOk();
  observation_->queue = std::move(queue);
  return success;
}

bool MetricRegistry::ObservationOwnerMatches(const MetricUuid& database_uuid,
                                             const MetricUuid& node_uuid) const {
  std::lock_guard<std::mutex> lock(mutex_);
  return observation_->queue &&
      observation_->queue->binding().database_uuid == database_uuid &&
      observation_->queue->binding().node_uuid == node_uuid &&
      observation_->queue->binding().cluster_uuid.is_nil();
}

std::optional<MetricUuid> MetricRegistry::ObservationNodeForDatabase(
    const MetricUuid& database_uuid) const {
  std::lock_guard<std::mutex> lock(mutex_);
  if (!observation_->queue ||
      observation_->queue->binding().database_uuid != database_uuid ||
      !observation_->queue->binding().cluster_uuid.is_nil()) return std::nullopt;
  return observation_->queue->binding().node_uuid;
}

MetricValidationResult MetricRegistry::ValidateDescriptor(const MetricDescriptor& descriptor) const {
  if (!ValidateMetricValueDescriptor(descriptor) || !MetricDescriptorReferencesValid(descriptor, descriptor) ||
      (descriptor.readiness != MetricReadiness::implemented &&
       descriptor.readiness != MetricReadiness::contract_ready_unwired &&
       descriptor.readiness != MetricReadiness::derived))
    return MetricError("METRIC.VALUE_INVALID", descriptor.family);
  if (descriptor.family.empty() || !StartsWith(descriptor.family, "sb_")) {
    return MetricError("SB-METRICS-DESCRIPTOR-FAMILY-INVALID", descriptor.family);
  }
  if (!MetricNamespaceMatchesScope(descriptor)) {
    return MetricError("SB-METRICS-DESCRIPTOR-NAMESPACE-INVALID", descriptor.family);
  }
  if (descriptor.producer_owner.empty()) {
    return MetricError("SB-METRICS-DESCRIPTOR-PRODUCER-MISSING", descriptor.family);
  }
  std::set<std::string> label_keys;
  for (const auto& label : descriptor.labels) {
    if (label.key.empty() || !label_keys.insert(label.key).second ||
        (label.value_type != MetricLabelType::text &&
         label.value_type != MetricLabelType::system_uuid &&
         label.value_type != MetricLabelType::uuid_value))
      return MetricError("SB-METRICS-LABEL-INVALID", descriptor.family);
  }
  if (descriptor.type == MetricType::histogram && descriptor.histogram_buckets.empty()) {
    return MetricError("SB-METRICS-DESCRIPTOR-HISTOGRAM-BUCKETS-MISSING", descriptor.family);
  }
  return MetricOk();
}

MetricValidationResult MetricRegistry::RegisterDescriptor(MetricDescriptor descriptor) {
  std::lock_guard<std::mutex> lock(mutex_);
  const auto validated = ValidateDescriptor(descriptor);
  if (!validated.ok) return validated;
  if (descriptors_.contains(descriptor.metric_uuid))
    return MetricError("METRIC.VALUE_INVALID", "metric UUID already registered");
  if (families_.contains(descriptor.family) || aliases_.contains(descriptor.family))
    return MetricError("SB-METRICS-DESCRIPTOR-DUPLICATE-FAMILY", descriptor.family);

  // Allocate every map node and the success result before publishing anything.
  // The maps have identical allocators and nonthrowing key comparisons; node
  // transfer under the retained mutex cannot expose a partial registration.
  decltype(descriptors_) pending_descriptors;
  decltype(families_) pending_families;
  decltype(aliases_) pending_aliases;
  for (const auto& alias : descriptor.aliases) {
    if (alias.empty() || alias == descriptor.family || families_.contains(alias) ||
        aliases_.contains(alias) ||
        !pending_aliases.emplace(alias, descriptor.metric_uuid).second)
      return MetricError("SB-METRICS-DESCRIPTOR-DUPLICATE-ALIAS", alias);
  }
  const auto identity = descriptor.metric_uuid;
  pending_families.emplace(descriptor.family, identity);
  pending_descriptors.emplace(identity, std::move(descriptor));
  auto success = MetricOk();
  descriptors_.merge(pending_descriptors);
  families_.merge(pending_families);
  aliases_.merge(pending_aliases);
  return success;
}

MetricValidationResult MetricRegistry::RegisterSeries(const MetricSeriesIdentity& series,
                                                      const MetricRetentionPolicy& policy) {
  std::lock_guard<std::mutex> lock(mutex_);
  if(!observation_->queue)
    return MetricError("METRIC.OBSERVATION_SOURCE_UNAVAILABLE", "node queue not bound");
  const auto found=descriptors_.find(series.metric_uuid);
  if(found==descriptors_.end())return MetricError("METRIC.VALUE_INVALID", "descriptor not registered");
  const auto& descriptor=found->second;
  const auto& binding=observation_->queue->binding();
  if(descriptor.cluster_only||descriptor.readiness!=MetricReadiness::implemented||
      series.database_uuid!=binding.database_uuid||series.node_uuid!=binding.node_uuid||!series.cluster_uuid.is_nil())
    return MetricError("METRIC.VALUE_INVALID", "local observation binding mismatch");
  const auto canonical=MakeMetricSeriesIdentity(descriptor,series.labels,policy,series,series.series_uuid,series.series_definition_generation);
  if(!canonical.ok()||canonical.record->series_key!=series.series_key||
      canonical.record->metric_family!=series.metric_family||canonical.record->namespace_path!=series.namespace_path||
      canonical.record->producer_owner!=series.producer_owner||canonical.record->scope_class!=series.scope_class||
      canonical.record->redaction_class!=series.redaction_class)
    return MetricError("METRIC.VALUE_INVALID", "invalid retained series");
  const auto key=NormalizeKey(descriptor.metric_uuid,series.labels);
  if(series_.contains(key)||std::any_of(series_.begin(),series_.end(),[&](const auto& entry){
      return entry.second->series_uuid==series.series_uuid;}))
    return MetricError("METRIC.VALUE_INVALID", "series already registered");
  decltype(series_) pending;
  pending.emplace(key,std::make_shared<const MetricSeriesIdentity>(series));
  auto success=MetricOk();
  series_.merge(pending);
  return success;
}

const MetricDescriptor* MetricRegistry::FindDescriptor(const MetricUuid& identity) const {
  std::lock_guard<std::mutex> lock(mutex_);
  const auto it = descriptors_.find(identity);
  return it == descriptors_.end() ? nullptr : &it->second;
}

const MetricDescriptor* MetricRegistry::FindDescriptor(const std::string& family) const {
  std::lock_guard<std::mutex> lock(mutex_);
  const auto mapping = families_.find(family);
  if (mapping == families_.end()) return nullptr;
  return &descriptors_.at(mapping->second);
}

const MetricDescriptor* MetricRegistry::FindDescriptorOrAlias(const std::string& family_or_alias) const {
  std::lock_guard<std::mutex> lock(mutex_);
  const auto direct = families_.find(family_or_alias);
  if (direct != families_.end()) return &descriptors_.at(direct->second);
  const auto alias = aliases_.find(family_or_alias);
  return alias == aliases_.end() ? nullptr : &descriptors_.at(alias->second);
}

std::vector<MetricDescriptor> MetricRegistry::Descriptors(bool include_cluster) const {
  std::lock_guard<std::mutex> lock(mutex_);
  std::vector<MetricDescriptor> out;
  for (const auto& [family, descriptor] : descriptors_) {
    if (!include_cluster && descriptor.cluster_only) {
      continue;
    }
    out.push_back(descriptor);
  }
  return out;
}

MetricValidationResult MetricRegistry::ValidateLabels(const MetricDescriptor& descriptor,
                                                      const MetricLabelSet& labels) const {
  return ValidateMetricLabelSet(descriptor, labels);
}

MetricValidationResult MetricRegistry::IncrementCounter(const std::string& family,
                                                        MetricLabelSet labels,
                                                        MetricScalar delta,
                                                        const std::string& producer_owner) {
  const auto* descriptor = FindDescriptorOrAlias(family);
  if (descriptor == nullptr) {
    return MetricError("SB-METRICS-FAMILY-UNKNOWN", family);
  }
  return UpdateValue(*descriptor, std::move(labels), delta, {}, producer_owner, MetricType::counter);
}

MetricValidationResult MetricRegistry::SetGauge(const std::string& family,
                                                MetricLabelSet labels,
                                                MetricScalar value,
                                                const std::string& producer_owner) {
  const auto* descriptor = FindDescriptorOrAlias(family);
  if (descriptor == nullptr) {
    return MetricError("SB-METRICS-FAMILY-UNKNOWN", family);
  }
  return UpdateValue(*descriptor, std::move(labels), value, {}, producer_owner, MetricType::gauge);
}

MetricValidationResult MetricRegistry::ObserveHistogram(const std::string& family,
                                                        MetricLabelSet labels,
                                                        MetricScalar value,
                                                        const std::string& producer_owner) {
  const auto* descriptor = FindDescriptorOrAlias(family);
  if (descriptor == nullptr) {
    return MetricError("SB-METRICS-FAMILY-UNKNOWN", family);
  }
  return UpdateValue(*descriptor, std::move(labels), value, {}, producer_owner, MetricType::histogram);
}

MetricValidationResult MetricRegistry::SetState(const std::string& family,
                                                MetricLabelSet labels,
                                                MetricScalar value,
                                                std::string state_text,
                                                const std::string& producer_owner) {
  const auto* descriptor = FindDescriptorOrAlias(family);
  if (descriptor == nullptr) {
    return MetricError("SB-METRICS-FAMILY-UNKNOWN", family);
  }
  return UpdateValue(*descriptor, std::move(labels), value, std::move(state_text), producer_owner, MetricType::state);
}

MetricValidationResult MetricRegistry::UpdateValue(const MetricDescriptor& descriptor,
                                                   MetricLabelSet labels,
                                                   MetricScalar value,
                                                   std::string state_text,
                                                   const std::string& producer_owner,
                                                   MetricType operation_type) {
  if (descriptor.readiness == MetricReadiness::contract_ready_unwired) {
    return MetricError("SB-METRICS-PRODUCER-UNWIRED", descriptor.family);
  }
  if (descriptor.producer_owner != producer_owner) {
    return MetricError("SB-METRICS-PRODUCER-MISMATCH", descriptor.family + ":" + producer_owner);
  }
  if (descriptor.type != operation_type) {
    return MetricError("SB-METRICS-TYPE-MISMATCH", descriptor.family);
  }
  const auto labels_valid = ValidateLabels(descriptor, labels);
  if (!labels_valid.ok) {
    if (descriptor.family != "sb_metric_samples_rejected_total") {
      try {
        (void)IncrementCounter("sb_metric_samples_rejected_total",
                         {{"metric_family", descriptor.family}, {"reason", labels_valid.diagnostic_code}},
                         u64{1},
                         "metrics_registry_manager");
      } catch (const std::bad_alloc&) {
        // Telemetry cannot replace the already established producer refusal.
      } catch (const std::length_error&) {
        // Retain the original refusal when telemetry exceeds container limits.
      }
    }
    return labels_valid;
  }
  std::lock_guard<std::mutex> lock(mutex_);
  const auto key = NormalizeKey(descriptor.metric_uuid, labels);
  const auto retained_series=series_.find(key);
  if(!observation_->queue||retained_series==series_.end()||!observation_->next_sequence)
    return MetricError("METRIC.OBSERVATION_SOURCE_UNAVAILABLE", "local series/source not bound or exhausted");
  const auto existing = current_values_.find(key);
  auto staged = StageMetricValueUpdate(descriptor, labels,
      existing == current_values_.end() ? nullptr : &existing->second, value, state_text);
  if (!staged.ok()) {
    const char* code = "METRIC.VALUE_INVALID";
    switch (staged.error) {
      case MetricValueUpdateError::overflow: code = "METRIC.AGGREGATE_OVERFLOW"; break;
      case MetricValueUpdateError::allocation_failure: code = "METRIC.OBSERVATION_RESOURCE_EXHAUSTED"; break;
      case MetricValueUpdateError::arithmetic_failure: code = "METRIC.ARITHMETIC_FAILED"; break;
      case MetricValueUpdateError::invalid_current: code = "METRIC.CURRENT_VALUE_INVALID"; break;
      default: break;
    }
    return MetricError(code, descriptor.family);
  }
  MetricValue current = std::move(*staged.value);
  // Stage all allocating work before queue admission or visible publication.
  // In particular, operator[] must not install an empty series on failure.
  HistoryEntry history{descriptor.metric_uuid,current};
  history_values_.reserve(history_values_.size() + 1);
  decltype(current_values_) pending;
  if (existing == current_values_.end()) pending.emplace(key, current);
  const auto clock=core::time::ReadLocalNodeClockSnapshot();
  if(!clock.ok()||observation_->clock.accepted_observations==std::numeric_limits<u64>::max())
    return MetricError("METRIC.OBSERVATION_SOURCE_UNAVAILABLE", "local clock unavailable");
  const auto admitted=core::time::ObserveLocalNodeClock(observation_->clock,clock.value,{});
  const auto wall=clock.value.wall_clock;
  constexpr u64 nanos_per_second=1000000000;
  if(!admitted.ok()||wall.unix_seconds<0||
      static_cast<u64>(wall.unix_seconds)>(std::numeric_limits<u64>::max()-wall.nanoseconds)/nanos_per_second)
    return MetricError("METRIC.OBSERVATION_SOURCE_UNAVAILABLE", "local clock refused");
  const u64 observed=static_cast<u64>(wall.unix_seconds)*nanos_per_second+wall.nanoseconds;
  if(!observed)return MetricError("METRIC.OBSERVATION_SOURCE_UNAVAILABLE", "zero observation time");
  observation_->clock=admitted.state;
  const u64 sequence=observation_->next_sequence;
  observation_->next_sequence=sequence==std::numeric_limits<u64>::max()?0:sequence+1;
  const auto sample=MakeMetricRawSampleRecord(descriptor,*retained_series->second,current,observed,observed,sequence);
  if(!sample.ok())return MetricError(sample.error==MetricHistoryRecordError::identity_issuance_failed?
      "METRIC.OBSERVATION_SOURCE_UNAVAILABLE":"METRIC.VALUE_INVALID", "sample construction refused");
  auto success=MetricOk();
  const auto queued=observation_->queue->TryEnqueue(descriptor,*retained_series->second,*sample.record);
  if(queued!=MetricQueueError::none)return MetricError(
      queued==MetricQueueError::full||queued==MetricQueueError::busy||queued==MetricQueueError::resource_exhausted?
      "METRIC.OBSERVATION_RESOURCE_EXHAUSTED":"METRIC.VALUE_INVALID", "observation queue refused");
  static_assert(std::is_nothrow_move_constructible_v<MetricValue>);
  static_assert(std::is_nothrow_move_assignable_v<MetricValue>);
  static_assert(std::is_nothrow_swappable_v<MetricValue>);
  static_assert(std::is_nothrow_move_constructible_v<HistoryEntry>);
  static_assert(std::is_nothrow_move_assignable_v<HistoryEntry>);
  if (existing == current_values_.end()) {
    current_values_.insert(pending.extract(pending.begin()));
  } else {
    using std::swap;
    swap(existing->second, current);
  }
  history_values_.push_back(std::move(history));
  if (history_values_.size() > 4096) {
    history_values_.erase(history_values_.begin(), history_values_.begin() + static_cast<std::ptrdiff_t>(history_values_.size() - 4096));
  }
  return success;
}

std::vector<MetricValue> MetricRegistry::SnapshotCurrent(bool include_cluster) const {
  std::lock_guard<std::mutex> lock(mutex_);
  std::vector<MetricValue> out;
  for (const auto& [key, value] : current_values_) {
    const auto descriptor = descriptors_.find(key.first);
    if (descriptor != descriptors_.end() && !include_cluster && descriptor->second.cluster_only) {
      continue;
    }
    out.push_back(value);
  }
  return out;
}

std::vector<MetricValue> MetricRegistry::SnapshotHistory(bool include_cluster, u64 max_rows) const {
  std::lock_guard<std::mutex> lock(mutex_);
  std::vector<MetricValue> out;
  const u64 start = history_values_.size() > max_rows ? static_cast<u64>(history_values_.size()) - max_rows : 0;
  for (u64 i = start; i < history_values_.size(); ++i) {
    const auto& retained = history_values_[static_cast<std::size_t>(i)];
    const auto& value = retained.value;
    const auto descriptor = descriptors_.find(retained.identity);
    if (descriptor != descriptors_.end() && !include_cluster && descriptor->second.cluster_only) {
      continue;
    }
    out.push_back(value);
  }
  return out;
}

MetricRegistry::CurrentKey MetricRegistry::NormalizeKey(const MetricUuid& identity,
                                                        const MetricLabelSet& labels) const {
  CurrentKey key;
  key.first = identity;
  key.second.reserve(labels.size());
  for (const auto& label : labels) key.second.emplace_back(label.key, label.value);
  std::sort(key.second.begin(), key.second.end());
  return key;
}

MetricRegistry& DefaultMetricRegistry() {
  static MetricRegistry registry;
  return registry;
}

MetricLabelSet RedactSensitiveLabels(const MetricDescriptor& descriptor,
                                      const MetricLabelSet& labels,
                                      bool allow_sensitive_labels) {
  if (allow_sensitive_labels) {
    return labels;
  }
  std::set<std::string> sensitive;
  for (const auto& label : descriptor.labels) {
    if (label.sensitive) {
      sensitive.insert(label.key);
    }
  }
  MetricLabelSet redacted;
  redacted.reserve(labels.size());
  for (const auto& label : labels) {
    redacted.push_back({label.key, sensitive.count(label.key) == 0 ? label.value : MetricLabelValue{std::string("<redacted>")}});
  }
  return redacted;
}

MetricValue RedactSensitiveMetricValue(const MetricDescriptor& descriptor,
                                       MetricValue value,
                                       bool allow_sensitive_labels) {
  value.labels = RedactSensitiveLabels(descriptor, value.labels, allow_sensitive_labels);
  return value;
}

}  // namespace scratchbird::core::metrics
