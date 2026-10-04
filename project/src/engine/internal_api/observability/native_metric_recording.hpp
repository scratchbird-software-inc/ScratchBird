// Copyright (c) 2026 ScratchBird Software Inc.
// SPDX-License-Identifier: MPL-2.0
#pragma once
#include "native_committed_metric_observation.hpp"
#include "metric_observation_queue.hpp"

namespace scratchbird::engine::internal_api {
// METRIC-NATIVE-RECORDING-ACK-SEPARATION-001. All inputs are engine-private.
// The caller retains device, admitted target/transaction and queue lease; these
// helpers neither grant those authorities nor own their cleanup/recovery.
struct NativeMetricRecordingTarget {
  core::platform::TypedUuid relation_uuid, row_uuid;
  core::platform::u64 page_number = 0;
  transaction::mga::TransactionIdentity writer;
};
enum class NativeMetricRecordingError {
  none, lease_unavailable, invalid_binding, invalid_target, invalid_sample,
  native_failure, committed_read_failed, committed_payload_mismatch
};
struct NativeMetricStageResult {
  NativeMetricRecordingError error = NativeMetricRecordingError::invalid_binding;
  core::metrics::MetricQueueError queue_error = core::metrics::MetricQueueError::none;
  bool physical_writer_entered = false;
  bool physical_result_available = false;
  storage::database::PhysicalMgaCowMutationResult physical;
  bool ok() const noexcept {
    return error == NativeMetricRecordingError::none && physical_writer_entered &&
           physical_result_available && physical.ok();
  }
};

inline NativeMetricStageResult StageNativeMetricQueuedObservation(
    storage::disk::FileDevice& device, core::metrics::MetricObservationQueue& queue,
    const core::metrics::MetricObservationLease& lease,
    const core::metrics::MetricDescriptor& descriptor,
    const core::metrics::MetricSeriesIdentity& series,
    const NativeMetricRecordingTarget& target) {
  using E = NativeMetricRecordingError;
  using core::platform::UuidKind;
  NativeMetricStageResult result;
  result.queue_error = queue.TryValidateLease(lease);
  if (result.queue_error != core::metrics::MetricQueueError::none) {
    result.error = E::lease_unavailable; return result;
  }
  const auto& owner = queue.binding();
  if (owner.database_uuid != series.database_uuid || owner.node_uuid != series.node_uuid ||
      !owner.cluster_uuid.is_nil() || !series.cluster_uuid.is_nil() || descriptor.cluster_only)
    return result;
  if (!target.writer.valid() || target.writer.scope != transaction::mga::TransactionScope::local_node ||
      target.relation_uuid.kind != UuidKind::object || target.row_uuid.kind != UuidKind::row ||
      !core::uuid::IsEngineIdentityUuid(target.relation_uuid.value) ||
      !core::uuid::IsEngineIdentityUuid(target.row_uuid.value) || !target.page_number) {
    result.error = E::invalid_target; return result;
  }
  const auto decoded = core::metrics::DecodeMetricRawSample(descriptor, series, lease.observation->bytes);
  if (!decoded.ok() || decoded.record->sample_uuid != lease.observation->sample_uuid ||
      decoded.record->source_sequence != lease.observation->source_sequence) {
    result.error = E::invalid_sample; return result;
  }
  const auto operation_guard = device.AcquireOperationGuard();
  storage::disk::SerializedDatabaseHeader bytes{};
  const auto io = device.ReadAt(0, bytes.data(), bytes.size());
  if (!io.ok()) { result.error = E::native_failure; result.physical.diagnostic = io.diagnostic; return result; }
  const auto header = storage::disk::ParseDatabaseHeader(bytes);
  if (!header.ok()) { result.error = E::native_failure; result.physical.diagnostic = header.diagnostic; return result; }
  if (header.header.database_uuid != owner.database_uuid) return result;
  storage::database::PhysicalMgaCowMutation mutation;
  mutation.relation_uuid = target.relation_uuid; mutation.row_uuid = target.row_uuid;
  mutation.page_number = target.page_number; mutation.transaction_uuid = target.writer.transaction_uuid;
  mutation.existing_local_transaction_id = target.writer.local_id; mutation.use_existing_transaction = true;
  core::datatypes::DatatypeBinaryValue value;
  value.type_id = core::datatypes::CanonicalTypeId::binary; value.payload = lease.observation->bytes;
  mutation.cells.push_back({1, std::move(value)});
  result.physical_writer_entered = true;
  try {
    result.physical = storage::database::WritePhysicalMgaCowUnpublishedMutationToOpenDevice(device, mutation);
    result.physical_result_available = true;
  } catch (...) {
    // Caller still owns the exact target/writer and lease. An exception after
    // entry cannot be reported as no-effects or authorize a fresh-identity retry.
    result.error = E::native_failure; return result;
  }
  result.error = result.physical.ok() ? E::none : E::native_failure;
  return result;
}

struct NativeMetricAcknowledgeResult {
  NativeMetricRecordingError error = NativeMetricRecordingError::committed_read_failed;
  core::metrics::MetricQueueError queue_error = core::metrics::MetricQueueError::none;
  NativeCommittedMetricReadResult committed;
  // A true committed result and false acknowledged result is a removal retry,
  // never permission to repeat the native write with a fresh row identity.
  bool acknowledged = false;
};
inline NativeMetricAcknowledgeResult AcknowledgeNativeCommittedMetricObservation(
    storage::disk::FileDevice& device, core::metrics::MetricObservationQueue& queue,
    const core::metrics::MetricObservationLease& lease,
    const core::metrics::MetricDescriptor& descriptor,
    const core::metrics::MetricSeriesIdentity& series,
    const NativeMetricRecordingTarget& target) {
  using E = NativeMetricRecordingError;
  NativeMetricAcknowledgeResult result;
  result.queue_error = queue.TryValidateLease(lease);
  if (result.queue_error != core::metrics::MetricQueueError::none) {
    result.error = E::lease_unavailable; return result;
  }
  if (queue.binding().database_uuid != series.database_uuid ||
      queue.binding().node_uuid != series.node_uuid || !queue.binding().cluster_uuid.is_nil()) {
    result.error = E::invalid_binding; return result;
  }
  result.committed = ReadNativeCommittedMetricObservation(device, queue.binding().node_uuid,
      target.relation_uuid, target.page_number, target.row_uuid, descriptor, series,
      lease.observation->sample_uuid);
  if (!result.committed.ok()) return result;
  const auto& receipt = *result.committed.observation;
  const auto encoded = core::metrics::EncodeMetricRawSample(descriptor, series, receipt.sample());
  if (!target.writer.valid() || target.writer.scope != transaction::mga::TransactionScope::local_node ||
      receipt.creator_uuid().value != target.writer.transaction_uuid.value ||
      receipt.creator_local_id() != target.writer.local_id.value ||
      !encoded.ok() || encoded.bytes != lease.observation->bytes) {
    result.error = E::committed_payload_mismatch; return result;
  }
  result.error = E::none;
  result.queue_error = queue.TryRemove(lease);
  result.acknowledged = result.queue_error == core::metrics::MetricQueueError::none;
  return result;
}
}  // namespace scratchbird::engine::internal_api
