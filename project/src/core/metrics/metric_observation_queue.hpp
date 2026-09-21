// Copyright (c) 2026 ScratchBird Software Inc.
// SPDX-License-Identifier: MPL-2.0
#pragma once
#include "metric_sample_codec.hpp"
#include <atomic>
#include <deque>
#include <memory>
#include <mutex>

namespace scratchbird::core::metrics {
enum class MetricQueueError {
  none, invalid_configuration, invalid_observation, full, busy, duplicate,
  resource_exhausted, empty, stale_lease, token_exhausted
};
struct MetricQueueBinding {MetricUuid database_uuid,node_uuid,cluster_uuid;};
struct MetricQueueLimits {std::size_t maximum_samples=0,maximum_wire_bytes=0;};
struct MetricQueuedObservation {
  MetricHistoryBinding binding;
  MetricUuid sample_uuid,series_uuid;
  u64 source_sequence=0,series_definition_generation=0;
  std::vector<platform::byte> bytes;
};
struct MetricObservationLease {
  u64 token=0;
  std::shared_ptr<const MetricQueuedObservation> observation;
};
struct MetricQueueLeaseResult {
  MetricQueueError error=MetricQueueError::empty;
  MetricObservationLease lease;
  bool ok()const noexcept{return error==MetricQueueError::none&&lease.token&&lease.observation;}
};
struct MetricQueueStats {
  u64 admitted=0,removed=0,full=0,busy=0,invalid=0,duplicate=0,allocation_failures=0;
  u64 queued=0,wire_bytes=0;
};
struct MetricQueueCreateResult;
// Node-owned volatile handoff, not storage, visibility, security or finality.
// No operation waits for the queue mutex, writes a file or calls a producer.
class MetricObservationQueue {
 public:
  static MetricQueueCreateResult Create(MetricQueueBinding,MetricQueueLimits) noexcept;
  MetricObservationQueue(const MetricObservationQueue&)=delete;
  MetricObservationQueue& operator=(const MetricObservationQueue&)=delete;
  const MetricQueueBinding& binding() const noexcept { return binding_; }
  MetricQueueError TryEnqueue(const MetricDescriptor&,const MetricSeriesIdentity&,
      const MetricRawSampleRecord&) noexcept;
  MetricQueueLeaseResult TryAcquire() noexcept;
  MetricQueueError TryRelease(const MetricObservationLease&) noexcept;
  // Caller must first complete actual recording or its recorded loss decision.
  // This removes only volatile bytes; it never certifies a transaction outcome.
  MetricQueueError TryRemove(const MetricObservationLease&) noexcept;
  MetricQueueStats Stats()const noexcept;
 private:
  MetricObservationQueue(MetricQueueBinding binding,MetricQueueLimits limits):binding_(binding),limits_(limits){}
  MetricQueueError Reject(MetricQueueError) noexcept;
  MetricQueueError Finish(const MetricObservationLease&,bool remove) noexcept;
  const MetricQueueBinding binding_;
  const MetricQueueLimits limits_;
  std::mutex mutex_;
  std::deque<std::shared_ptr<const MetricQueuedObservation>> pending_;
  std::size_t wire_bytes_=0;
  u64 next_token_=1,leased_token_=0;
  std::atomic<u64> admitted_{0},removed_{0},full_{0},busy_{0},invalid_{0},duplicate_{0},allocation_failures_{0};
  std::atomic<u64> queued_{0},retained_bytes_{0};
};
struct MetricQueueCreateResult {
  MetricQueueError error=MetricQueueError::invalid_configuration;
  std::unique_ptr<MetricObservationQueue> queue;
  bool ok()const noexcept{return error==MetricQueueError::none&&bool(queue);}
};
} // namespace scratchbird::core::metrics
