// Copyright (c) 2026 ScratchBird Software Inc.
// SPDX-License-Identifier: MPL-2.0
#pragma once

#include "database_format.hpp"
#include "disk_device.hpp"
#include "metric_sample_codec.hpp"
#include "physical_mga_cow_store.hpp"
#include "uuid.hpp"

namespace scratchbird::engine::internal_api {

// METRIC-NATIVE-COMMITTED-SAMPLE-READER-001. This component proves committed
// payload visibility, not catalog selection, source admission or authorization.
enum class NativeCommittedMetricReadError {
  none, invalid_request, owner_mismatch, native_read_failed,
  not_visible, invalid_row, invalid_sample
};
struct NativeCommittedMetricReadResult;
class NativeCommittedMetricObservation;
inline NativeCommittedMetricReadResult ReadNativeCommittedMetricObservation(
    storage::disk::FileDevice&, const core::platform::Uuid& node_uuid,
    const core::platform::TypedUuid& relation_uuid, core::platform::u64 page_number,
    const core::platform::TypedUuid& row_uuid,
    const core::metrics::MetricDescriptor&, const core::metrics::MetricSeriesIdentity&,
    const core::platform::Uuid& sample_uuid);

class NativeCommittedMetricObservation {
 public:
  const core::metrics::MetricRawSampleRecord& sample() const noexcept { return sample_; }
  const core::platform::Uuid& row_uuid() const noexcept { return row_uuid_; }
  const core::platform::Uuid& version_uuid() const noexcept { return version_uuid_; }
  const core::platform::TypedUuid& creator_uuid() const noexcept { return creator_uuid_; }
  core::platform::u64 creator_local_id() const noexcept { return creator_local_id_; }
 private:
  friend NativeCommittedMetricReadResult ReadNativeCommittedMetricObservation(
      storage::disk::FileDevice&, const core::platform::Uuid&,
      const core::platform::TypedUuid&, core::platform::u64,
      const core::platform::TypedUuid&, const core::metrics::MetricDescriptor&,
      const core::metrics::MetricSeriesIdentity&, const core::platform::Uuid&);
  NativeCommittedMetricObservation(core::metrics::MetricRawSampleRecord sample,
                                  const storage::page::RowDataRecord& row)
      : sample_(std::move(sample)), row_uuid_(row.row_uuid.value),
        version_uuid_(row.version_uuid), creator_uuid_(row.transaction_uuid),
        creator_local_id_(row.local_transaction_id) {}
  core::metrics::MetricRawSampleRecord sample_;
  core::platform::Uuid row_uuid_, version_uuid_;
  core::platform::TypedUuid creator_uuid_;
  core::platform::u64 creator_local_id_;
};

struct NativeCommittedMetricReadResult {
  NativeCommittedMetricReadError error = NativeCommittedMetricReadError::invalid_request;
  core::platform::DiagnosticRecord diagnostic;
  std::optional<NativeCommittedMetricObservation> observation;
  bool ok() const noexcept {
    return error == NativeCommittedMetricReadError::none && observation.has_value();
  }
};

inline NativeCommittedMetricReadResult ReadNativeCommittedMetricObservation(
    storage::disk::FileDevice& device, const core::platform::Uuid& node_uuid,
    const core::platform::TypedUuid& relation_uuid, core::platform::u64 page_number,
    const core::platform::TypedUuid& row_uuid,
    const core::metrics::MetricDescriptor& descriptor,
    const core::metrics::MetricSeriesIdentity& series,
    const core::platform::Uuid& sample_uuid) {
  using E = NativeCommittedMetricReadError;
  using core::platform::UuidKind;
  if (!core::uuid::IsEngineIdentityUuid(node_uuid) ||
      !core::uuid::IsEngineIdentityUuid(sample_uuid) ||
      relation_uuid.kind != UuidKind::object ||
      !core::uuid::IsEngineIdentityUuid(relation_uuid.value) ||
      row_uuid.kind != UuidKind::row || !core::uuid::IsEngineIdentityUuid(row_uuid.value) ||
      !page_number || descriptor.cluster_only || !series.cluster_uuid.is_nil())
    return {};
  if (series.node_uuid != node_uuid) return {E::owner_mismatch, {}, {}};
  const auto operation_guard = device.AcquireOperationGuard();
  storage::disk::SerializedDatabaseHeader bytes{};
  const auto header_io = device.ReadAt(0, bytes.data(), bytes.size());
  if (!header_io.ok()) return {E::native_read_failed, header_io.diagnostic, {}};
  const auto header = storage::disk::ParseDatabaseHeader(bytes);
  if (!header.ok()) return {E::native_read_failed, header.diagnostic, {}};
  if (header.header.database_uuid != series.database_uuid)
    return {E::owner_mismatch, {}, {}};

  // No caller snapshot or own-transaction visibility is accepted here.
  const auto read = storage::database::ReadPhysicalMgaCowRowsFromOpenDevice(
      device, relation_uuid, page_number, {}, true);
  if (!read.ok()) return {E::native_read_failed, read.diagnostic, {}};
  const storage::page::RowDataRecord* selected = nullptr;
  for (const auto& row : read.visible_rows) {
    if (row.row_uuid.value != row_uuid.value) continue;
    if (selected) return {E::invalid_row, {}, {}};
    selected = &row;
  }
  if (!selected) return {E::not_visible, {}, {}};
  if (selected->deleted || selected->row_uuid.kind != UuidKind::row ||
      selected->cells.size() != 1 || selected->cells.front().column_ordinal != 1 ||
      selected->cells.front().value.type_id != core::datatypes::CanonicalTypeId::binary)
    return {E::invalid_row, {}, {}};
  const auto& payload = selected->cells.front().value.payload;
  auto decoded = core::metrics::DecodeMetricRawSample(descriptor, series, payload);
  if (!decoded.ok() || decoded.record->sample_uuid != sample_uuid)
    return {E::invalid_sample, {}, {}};
  NativeCommittedMetricObservation observation(std::move(*decoded.record), *selected);
  return {E::none, {}, std::move(observation)};
}
}  // namespace scratchbird::engine::internal_api
