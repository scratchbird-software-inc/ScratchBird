// Copyright (c) 2026 ScratchBird Software Inc.
// SPDX-License-Identifier: MPL-2.0
#include "metric_observation_queue.hpp"
#include "uuid.hpp"
#include <algorithm>
#include <limits>
#include <stdexcept>

namespace scratchbird::core::metrics {
namespace {
using E=MetricQueueError;
void Increment(std::atomic<u64>& counter) noexcept {
  auto value=counter.load(std::memory_order_relaxed);
  while(value!=std::numeric_limits<u64>::max()&&
      !counter.compare_exchange_weak(value,value+1,std::memory_order_relaxed)){}
}
}
MetricQueueCreateResult MetricObservationQueue::Create(MetricQueueBinding binding,MetricQueueLimits limits) noexcept {
  if(!core::uuid::IsEngineIdentityUuid(binding.database_uuid)||!core::uuid::IsEngineIdentityUuid(binding.node_uuid)||
      (!binding.cluster_uuid.is_nil()&&!core::uuid::IsEngineIdentityUuid(binding.cluster_uuid))||
      !limits.maximum_samples||limits.maximum_samples>65536||!limits.maximum_wire_bytes||limits.maximum_wire_bytes>64*1024*1024)
    return {E::invalid_configuration,{}};
  try{return {E::none,std::unique_ptr<MetricObservationQueue>(new MetricObservationQueue(binding,limits))};}
  catch(...){return {E::resource_exhausted,{}};}
}
MetricQueueError MetricObservationQueue::Reject(E error) noexcept {
  switch(error){case E::full:Increment(full_);break;case E::busy:Increment(busy_);break;
    case E::duplicate:Increment(duplicate_);break;case E::resource_exhausted:Increment(allocation_failures_);break;
    default:Increment(invalid_);break;}
  return error;
}
MetricQueueError MetricObservationQueue::TryEnqueue(const MetricDescriptor& descriptor,
    const MetricSeriesIdentity& series,const MetricRawSampleRecord& sample) noexcept {
  try {
    if(sample.database_uuid!=binding_.database_uuid||sample.node_uuid!=binding_.node_uuid||sample.publication_time_utc_ns||
        (descriptor.cluster_only?sample.cluster_uuid!=binding_.cluster_uuid:!sample.cluster_uuid.is_nil()))return Reject(E::invalid_observation);
    auto encoded=EncodeMetricRawSample(descriptor,series,sample);
    if(!encoded.ok())return Reject(encoded.error==MetricSampleCodecError::resource_exhausted?E::resource_exhausted:E::invalid_observation);
    if(encoded.bytes.size()>limits_.maximum_wire_bytes)return Reject(E::full);
    auto observation=std::make_shared<MetricQueuedObservation>();
    observation->binding=static_cast<const MetricHistoryBinding&>(sample);observation->sample_uuid=sample.sample_uuid;
    observation->series_uuid=sample.series_uuid;observation->source_sequence=sample.source_sequence;observation->bytes=std::move(encoded.bytes);
    observation->series_definition_generation=sample.series_definition_generation;
    std::unique_lock<std::mutex> guard(mutex_,std::try_to_lock);if(!guard.owns_lock())return Reject(E::busy);
    if(std::any_of(pending_.begin(),pending_.end(),[&](const auto& e){return e->sample_uuid==sample.sample_uuid;}))return Reject(E::duplicate);
    if(pending_.size()>=limits_.maximum_samples||observation->bytes.size()>limits_.maximum_wire_bytes-wire_bytes_)return Reject(E::full);
    pending_.push_back(observation);wire_bytes_+=observation->bytes.size();
    queued_.store(pending_.size(),std::memory_order_relaxed);retained_bytes_.store(wire_bytes_,std::memory_order_relaxed);Increment(admitted_);
    return E::none;
  }catch(const std::bad_alloc&){return Reject(E::resource_exhausted);}
   catch(const std::length_error&){return Reject(E::resource_exhausted);}
   catch(...){return Reject(E::invalid_observation);}
}
MetricQueueLeaseResult MetricObservationQueue::TryAcquire() noexcept {
  try {
    std::unique_lock<std::mutex> guard(mutex_,std::try_to_lock);
    if(!guard.owns_lock()||leased_token_)return {Reject(E::busy),{}};
    if(pending_.empty())return {E::empty,{}};
    if(!next_token_)return {E::token_exhausted,{}};
    leased_token_=next_token_;
    next_token_=next_token_==std::numeric_limits<u64>::max()?0:next_token_+1;
    return {E::none,{leased_token_,pending_.front()}};
  }catch(...){return {Reject(E::invalid_observation),{}};}
}
MetricQueueError MetricObservationQueue::Finish(const MetricObservationLease& lease,bool remove) noexcept {
  try {
    std::unique_lock<std::mutex> guard(mutex_,std::try_to_lock);
    if(!guard.owns_lock())return Reject(E::busy);
    if(!lease.token||lease.token!=leased_token_||pending_.empty()||lease.observation!=pending_.front())return Reject(E::stale_lease);
    if(remove){wire_bytes_-=pending_.front()->bytes.size();pending_.pop_front();
      queued_.store(pending_.size(),std::memory_order_relaxed);retained_bytes_.store(wire_bytes_,std::memory_order_relaxed);Increment(removed_);}
    leased_token_=0;return E::none;
  }catch(...){return Reject(E::invalid_observation);}
}
MetricQueueError MetricObservationQueue::TryRelease(const MetricObservationLease& lease) noexcept{return Finish(lease,false);}
MetricQueueError MetricObservationQueue::TryRemove(const MetricObservationLease& lease) noexcept{return Finish(lease,true);}
MetricQueueStats MetricObservationQueue::Stats()const noexcept {
  return {admitted_.load(std::memory_order_relaxed),removed_.load(std::memory_order_relaxed),full_.load(std::memory_order_relaxed),
      busy_.load(std::memory_order_relaxed),invalid_.load(std::memory_order_relaxed),duplicate_.load(std::memory_order_relaxed),
      allocation_failures_.load(std::memory_order_relaxed),queued_.load(std::memory_order_relaxed),retained_bytes_.load(std::memory_order_relaxed)};
}
} // namespace scratchbird::core::metrics
