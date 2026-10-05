// Copyright (c) 2026 ScratchBird Software Inc.
// SPDX-License-Identifier: MPL-2.0

#include "../../../src/core/datatypes/datatype_timestamp_projection.hpp"

#include <algorithm>
#include <array>
#include <atomic>
#include <cstdint>
#include <cstdlib>
#include <iostream>
#include <memory>
#include <new>
#include <span>
#include <string_view>
#include <thread>

namespace allocation_probe {
std::atomic<bool> fail{false};
std::atomic<std::size_t> fail_at{0};
std::atomic<std::size_t> calls{0};
}

void* operator new(std::size_t size) {
  std::size_t call = 0;
  if (allocation_probe::fail.load(std::memory_order_relaxed)) {
    call = allocation_probe::calls.fetch_add(1, std::memory_order_relaxed) + 1;
  }
  if (allocation_probe::fail.load(std::memory_order_relaxed) &&
      call == allocation_probe::fail_at.load(std::memory_order_relaxed)) {
    throw std::bad_alloc();
  }
  if (void* pointer = std::malloc(size == 0 ? 1 : size)) return pointer;
  throw std::bad_alloc();
}
void* operator new[](std::size_t size) { return ::operator new(size); }
void operator delete(void* pointer) noexcept { std::free(pointer); }
void operator delete[](void* pointer) noexcept { std::free(pointer); }
void operator delete(void* pointer, std::size_t) noexcept { std::free(pointer); }
void operator delete[](void* pointer, std::size_t) noexcept { std::free(pointer); }

namespace {
namespace dt = scratchbird::core::datatypes;
namespace p = scratchbird::core::platform;

unsigned checks = 0;
unsigned failures = 0;
void Check(bool value, std::string_view message) {
  ++checks;
  if (!value) {
    ++failures;
    std::cerr << "FAIL " << message << '\n';
  }
}

p::Uuid D710() {
  return p::Uuid{{0x01, 0x9d, 0, 0, 0, 0, 0x70, 0,
                  0x80, 0, 0, 0, 0, 0, 0xd7, 0x10}};
}
p::Uuid V7(p::byte discriminator) {
  return p::Uuid{{0x01, 0xa1, 0x04, 0xf5, 0, discriminator, 0x70, 0,
                  0x80, 0, 0, 0, 0, 0, 0, discriminator}};
}
std::shared_ptr<const dt::TimestampValidatedProfileHandleV3> Profile() {
  auto result = dt::BuildCurrentTimestampValidatedProfileHandleV3(D710());
  Check(result.ok(), "current d710 timestamp profile builds");
  return std::make_shared<const dt::TimestampValidatedProfileHandleV3>(
      std::move(result.profile));
}

const dt::DatatypeTypeCodecIdentityRowV3* Identity(dt::CanonicalTypeId type) {
  for (const auto& row : dt::CurrentDatatypeTypeCodecIdentityRowsV3()) {
    if (row.legacy_fields.catalog_snapshot_uuid == D710() &&
        row.legacy_fields.catalog_generation == 10 &&
        row.legacy_fields.registry_generation == 10 &&
        row.legacy_fields.canonical_binary_type_code ==
            static_cast<p::u32>(type)) return &row;
  }
  return nullptr;
}

scratchbird::engine::ExecutionTypeDescriptor Descriptor(dt::CanonicalTypeId type) {
  auto manifest = dt::LoadCurrentCoreDatatypeCatalogManifest();
  Check(manifest.ok(), "catalog manifest loads for cast fixture");
  auto row = dt::LookupDatatypeCatalogRow(manifest.manifest, type);
  Check(row.ok() && row.manifest.descriptor_rows.size() == 1,
        "cast descriptor row is unique");
  dt::CatalogExecutionTypeMetadata metadata;
  metadata.descriptor_uuid = row.manifest.descriptor_rows.front().descriptor_uuid;
  metadata.descriptor_epoch = row.manifest.descriptor_rows.front().descriptor_epoch;
  auto result = dt::LookupExecutionTypeDescriptorFromCatalog(type, metadata);
  Check(result.ok(), "cast execution descriptor resolves");
  return result.descriptor;
}

void BeginHeapFailure(std::size_t at) {
  allocation_probe::calls.store(0, std::memory_order_relaxed);
  allocation_probe::fail_at.store(at, std::memory_order_relaxed);
  allocation_probe::fail.store(true, std::memory_order_release);
}
void EndHeapFailure() {
  allocation_probe::fail.store(false, std::memory_order_release);
}

struct CancelProbe { unsigned calls = 0; unsigned cancel_at = 0; };
bool CancelAt(void* raw) noexcept {
  auto& probe = *static_cast<CancelProbe*>(raw);
  return ++probe.calls == probe.cancel_at;
}
dt::TimestampExecutionControlV3 Control(CancelProbe& probe,
                                        p::u64 budget = ~p::u64{0}) {
  dt::TimestampExecutionControlV3 control;
  control.maximum_allocation_bytes = budget;
  control.cancelled = CancelAt;
  control.cancellation_context = &probe;
  return control;
}

struct PinProbe {
  unsigned calls = 0;
  unsigned cancel_at = 0;
  std::weak_ptr<const dt::TimestampValidatedProfileHandleV3> profile;
  long observed = 0;
};
bool CancelWithPin(void* raw) noexcept {
  auto& probe = *static_cast<PinProbe*>(raw);
  probe.observed = probe.profile.use_count();
  return ++probe.calls == probe.cancel_at;
}
dt::TimestampExecutionControlV3 PinControl(PinProbe& probe) {
  dt::TimestampExecutionControlV3 control;
  control.cancelled = CancelWithPin;
  control.cancellation_context = &probe;
  return control;
}

struct ScrubObservation {
  std::array<unsigned,
             static_cast<std::size_t>(dt::TimestampScrubClassV3::count)> calls{};
  bool all_zero = true;
};
void ObserveScrub(void* raw, dt::TimestampScrubClassV3 scrub_class,
                  const p::byte* bytes, p::u64 extent) noexcept {
  auto& observation = *static_cast<ScrubObservation*>(raw);
  ++observation.calls[static_cast<std::size_t>(scrub_class)];
  for (p::u64 index = 0; index != extent; ++index)
    observation.all_zero &= bytes[index] == p::byte{0};
}
dt::TimestampExecutionControlV3 Observed(ScrubObservation& observation) {
  dt::TimestampExecutionControlV3 control;
  control.observe_scrubbed = ObserveScrub;
  control.scrub_observer_context = &observation;
  return control;
}

struct Fixtures {
  std::shared_ptr<const dt::TimestampValidatedProfileHandleV3> profile;
  dt::TimestampOwnedValueV3 lower;
  dt::TimestampOwnedValueV3 upper;
  dt::TimestampOwnedValueV3 null_value;
  dt::TimestampZoneMapRequestV3 zone;
  std::shared_ptr<const dt::TimestampStatisticsProviderEvidenceHandleV3> evidence;
  std::array<dt::TimestampStatisticsHistogramRecordV3, 1> histogram{};
  std::array<dt::TimestampStatisticsMcvRecordV3, 1> mcv{};
  dt::TimestampStatisticsProjectionV3 statistics;
};

Fixtures MakeFixtures() {
  Fixtures fixture;
  fixture.profile = Profile();
  fixture.lower = {fixture.profile, dt::TimestampValueStateV3::value, -1,
                   86'399'999'999'999ull};
  fixture.upper = {fixture.profile, dt::TimestampValueStateV3::value, 0, 0};
  fixture.null_value = {fixture.profile, dt::TimestampValueStateV3::sql_null, 0, 0};
  fixture.zone.profile = fixture.profile;
  fixture.zone.minimum = &fixture.lower;
  fixture.zone.maximum = &fixture.upper;
  fixture.zone.value_count = 2;
  auto evidence = std::make_shared<dt::TimestampStatisticsProviderEvidenceHandleV3>();
  evidence->provider_evidence_uuid = V7(3);
  evidence->authenticated_population = true;
  evidence->source_authorized = true;
  evidence->privacy_admitted = true;
  evidence->evidence_class =
      dt::TimestampStatisticsEvidenceClassV3::full_visible_population;
  evidence->schema_epoch = 11;
  evidence->collection_epoch = 12;
  evidence->security_epoch = 13;
  fixture.evidence = evidence;
  fixture.histogram[0] = {0, 0, 3};
  fixture.mcv[0].value_hash[31] = 1;
  fixture.mcv[0].frequency = 1;
  fixture.statistics.profile = fixture.profile;
  fixture.statistics.provider_evidence = fixture.evidence;
  fixture.statistics.statistics_uuid = V7(1);
  fixture.statistics.source_object_uuid = V7(2);
  fixture.statistics.schema_epoch = 11;
  fixture.statistics.collection_epoch = 12;
  fixture.statistics.security_epoch = 13;
  fixture.statistics.row_count = 3;
  fixture.statistics.value_count = 3;
  fixture.statistics.minimum_maximum_present = true;
  fixture.statistics.minimum_civil_day = fixture.lower.civil_day;
  fixture.statistics.minimum_nanoseconds_since_midnight =
      fixture.lower.nanoseconds_since_midnight;
  fixture.statistics.maximum_civil_day = fixture.upper.civil_day;
  fixture.statistics.maximum_nanoseconds_since_midnight =
      fixture.upper.nanoseconds_since_midnight;
  evidence->source_object_uuid = fixture.statistics.source_object_uuid;
  evidence->row_count = fixture.statistics.row_count;
  evidence->null_count = fixture.statistics.null_count;
  evidence->value_count = fixture.statistics.value_count;
  evidence->minimum_maximum_present =
      fixture.statistics.minimum_maximum_present;
  evidence->minimum_civil_day = fixture.statistics.minimum_civil_day;
  evidence->minimum_nanoseconds_since_midnight =
      fixture.statistics.minimum_nanoseconds_since_midnight;
  evidence->maximum_civil_day = fixture.statistics.maximum_civil_day;
  evidence->maximum_nanoseconds_since_midnight =
      fixture.statistics.maximum_nanoseconds_since_midnight;
  evidence->histogram.assign(fixture.histogram.begin(), fixture.histogram.end());
  evidence->mcv.assign(fixture.mcv.begin(), fixture.mcv.end());
  return fixture;
}

void RefreshFixtureSpans(Fixtures& fixture) {
  fixture.statistics.histogram = fixture.histogram;
  fixture.statistics.mcv = fixture.mcv;
}

dt::TimestampIndexProjectionRequestV3 BtreeRequest(const Fixtures& fixture) {
  dt::TimestampIndexProjectionRequestV3 request;
  request.compatibility = dt::LookupTimestampIndexCompatibilityIdentityV3(
      dt::TimestampIndexFamilyV3::btree);
  request.mode = dt::TimestampProjectionModeV3::ascending_nulls_last;
  request.value = &fixture.lower;
  return request;
}

dt::TimestampExecutionControlV3 Budget(p::u64 bytes) {
  dt::TimestampExecutionControlV3 control;
  control.maximum_allocation_bytes = bytes;
  return control;
}

void ExactCapacityAndPrecedence(Fixtures& fixture) {
  std::array<p::byte, 16> component{};
  component.fill(0xa5);
  const auto component_before = component;
  Check(!dt::EncodeCanonicalTimestampComponentIntoNoAllocV3(
             fixture.lower, component.data(), 15).ok() &&
            component == component_before,
        "component one-byte-short preserves output");
  Check(dt::EncodeCanonicalTimestampComponentIntoNoAllocV3(
            fixture.lower, component.data(), 16).ok(),
        "component exact capacity publishes once");

  std::array<p::byte, 32> hash{};
  hash.fill(0xa5);
  const auto hash_before = hash;
  Check(!dt::HashTimestampValueIntoNoAllocV3(
             fixture.lower, hash.data(), 31).ok() && hash == hash_before,
        "hash one-byte-short preserves output");
  Check(dt::HashTimestampValueIntoNoAllocV3(
            fixture.lower, hash.data(), 32).ok(),
        "hash exact capacity publishes once");

  std::array<p::byte, 112> key{};
  key.fill(0xa5);
  const auto key_before = key;
  Check(!dt::MakeTimestampSortKeyIntoNoAllocV3(
             fixture.lower, dt::TimestampSortDirectionV3::ascending,
             dt::TimestampNullModeV3::nulls_last, key.data(), 111).ok() &&
            key == key_before,
        "ordered key one-byte-short preserves output");
  Check(dt::MakeTimestampSortKeyIntoNoAllocV3(
            fixture.lower, dt::TimestampSortDirectionV3::ascending,
            dt::TimestampNullModeV3::nulls_last, key.data(), 112).ok(),
        "ordered key exact capacity publishes once");

  auto check_boundary = [&](p::u64 extent, auto&& invoke,
                            std::string_view exact_message,
                            std::string_view short_message) {
    auto exact = invoke(Budget(extent));
    auto short_result = invoke(Budget(extent - 1));
    Check(exact.ok() && exact.bytes.size() == extent, exact_message);
    Check(!short_result.ok() && short_result.bytes.empty() &&
              short_result.diagnostic.diagnostic_code ==
                  "RESOURCE.BUDGET_EXCEEDED",
          short_message);
  };
  check_boundary(dt::kTimestampCoveringValueBytesV3,
                 [&](const auto& control) {
                   return dt::EncodeTimestampCoveringValueV3(fixture.lower,
                                                              control);
                 },
                 "covering exact capacity publishes once",
                 "covering one-byte-short publishes nothing");
  check_boundary(dt::kTimestampRangePairBytesV3,
                 [&](const auto& control) {
                   return dt::EncodeTimestampRangePairV3(
                       fixture.lower, fixture.upper, control);
                 },
                 "range exact capacity publishes once",
                 "range one-byte-short publishes nothing");
  fixture.zone.provider_max_key_bytes = dt::kTimestampZoneMapValueBytesV3;
  check_boundary(dt::kTimestampZoneMapValueBytesV3,
                 [&](const auto& control) {
                   auto request = fixture.zone;
                   request.control = control;
                   return dt::EncodeTimestampZoneMapV3(request);
                 },
                 "zone exact capacity publishes once",
                 "zone one-byte-short publishes nothing");
  constexpr p::u64 statistics_extent =
      dt::kTimestampStatisticsHeaderBytesV3 + 24 + 48;
  check_boundary(statistics_extent,
                 [&](const auto& control) {
                   return dt::EncodeTimestampStatisticsProjectionV3(
                       fixture.statistics, control);
                 },
                 "statistics exact capacity publishes once",
                 "statistics one-byte-short publishes nothing");
  check_boundary(dt::kTimestampBackupValueBytesV3,
                 [&](const auto& control) {
                   return dt::EncodeTimestampBackupTupleV3(fixture.lower,
                                                           control);
                 },
                 "backup exact capacity publishes once",
                 "backup one-byte-short publishes nothing");

  auto zone_both_short = fixture.zone;
  zone_both_short.provider_max_key_bytes =
      dt::kTimestampZoneMapValueBytesV3 - 1;
  zone_both_short.control = Budget(dt::kTimestampZoneMapValueBytesV3 - 1);
  auto zone_precedence = dt::EncodeTimestampZoneMapV3(zone_both_short);
  Check(!zone_precedence.ok() &&
            zone_precedence.diagnostic.detail == "zone_provider_budget",
        "zone provider budget precedes resource budget");

  auto projection = BtreeRequest(fixture);
  projection.provider_max_key_bytes = dt::kTimestampValueSortKeyBytesV3 - 1;
  projection.control = Budget(dt::kTimestampValueSortKeyBytesV3 - 1);
  auto both_short = dt::ProjectTimestampIndexValueV3(projection);
  Check(!both_short.ok() && both_short.bytes.empty() &&
            both_short.diagnostic.detail == "provider_budget",
        "index provider budget precedes resource budget");
  projection.provider_max_key_bytes = dt::kTimestampValueSortKeyBytesV3;
  auto resource_short = dt::ProjectTimestampIndexValueV3(projection);
  Check(!resource_short.ok() && resource_short.bytes.empty() &&
            resource_short.diagnostic.diagnostic_code ==
                "RESOURCE.BUDGET_EXCEEDED",
        "index resource refusal follows admitted provider capacity");
}

void NullEmptyAndMaximumCapacity(Fixtures& fixture) {
  auto null_component = dt::EncodeCanonicalTimestampComponentIntoNoAllocV3(
      fixture.null_value, nullptr, 0);
  Check(null_component.ok() && null_component.bytes_required == 0 &&
            null_component.bytes_written == 0 && null_component.containing_null,
        "NULL component publishes at exact zero capacity");

  std::array<p::byte, 32> hash{};
  hash.fill(0xa5);
  const auto hash_before = hash;
  Check(!dt::HashTimestampValueIntoNoAllocV3(
             fixture.null_value, hash.data(), 31).ok() && hash == hash_before,
        "NULL hash one-byte-short preserves output");
  Check(dt::HashTimestampValueIntoNoAllocV3(
            fixture.null_value, hash.data(), 32).ok(),
        "NULL hash publishes at exact capacity");

  std::array<p::byte, 100> null_key{};
  null_key.fill(0xa5);
  const auto key_before = null_key;
  Check(!dt::MakeTimestampSortKeyIntoNoAllocV3(
             fixture.null_value, dt::TimestampSortDirectionV3::ascending,
             dt::TimestampNullModeV3::nulls_last, null_key.data(), 99).ok() &&
            null_key == key_before,
        "NULL ordered key one-byte-short preserves output");
  Check(dt::MakeTimestampSortKeyIntoNoAllocV3(
            fixture.null_value, dt::TimestampSortDirectionV3::ascending,
            dt::TimestampNullModeV3::nulls_last, null_key.data(), 100).ok(),
        "NULL ordered key publishes at exact capacity");

  auto null_cover = dt::EncodeTimestampCoveringValueV3(
      fixture.null_value, Budget(dt::kTimestampCoveringNullBytesV3));
  auto short_null_cover = dt::EncodeTimestampCoveringValueV3(
      fixture.null_value, Budget(dt::kTimestampCoveringNullBytesV3 - 1));
  Check(null_cover.ok() &&
            null_cover.bytes.size() == dt::kTimestampCoveringNullBytesV3,
        "NULL covering publishes at exact capacity");
  Check(!short_null_cover.ok() && short_null_cover.bytes.empty(),
        "NULL covering one-byte-short publishes nothing");

  dt::TimestampZoneMapRequestV3 empty_zone;
  empty_zone.profile = fixture.profile;
  empty_zone.null_count = 1;
  empty_zone.provider_max_key_bytes = dt::kTimestampZoneMapEmptyBytesV3;
  empty_zone.control = Budget(dt::kTimestampZoneMapEmptyBytesV3);
  auto exact_empty_zone = dt::EncodeTimestampZoneMapV3(empty_zone);
  empty_zone.control = Budget(dt::kTimestampZoneMapEmptyBytesV3 - 1);
  auto short_empty_zone = dt::EncodeTimestampZoneMapV3(empty_zone);
  Check(exact_empty_zone.ok() &&
            exact_empty_zone.bytes.size() == dt::kTimestampZoneMapEmptyBytesV3,
        "empty zone map publishes at exact capacity");
  Check(!short_empty_zone.ok() && short_empty_zone.bytes.empty(),
        "empty zone map one-byte-short publishes nothing");

  auto null_backup = dt::EncodeTimestampBackupTupleV3(
      fixture.null_value, Budget(dt::kTimestampBackupNullBytesV3));
  auto short_null_backup = dt::EncodeTimestampBackupTupleV3(
      fixture.null_value, Budget(dt::kTimestampBackupNullBytesV3 - 1));
  Check(null_backup.ok() &&
            null_backup.bytes.size() == dt::kTimestampBackupNullBytesV3,
        "NULL backup publishes at exact capacity");
  Check(!short_null_backup.ok() && short_null_backup.bytes.empty(),
        "NULL backup one-byte-short publishes nothing");

  auto empty_evidence =
      std::make_shared<dt::TimestampStatisticsProviderEvidenceHandleV3>();
  empty_evidence->provider_evidence_uuid = V7(6);
  empty_evidence->source_object_uuid = V7(5);
  empty_evidence->authenticated_population = true;
  empty_evidence->source_authorized = true;
  empty_evidence->privacy_admitted = true;
  empty_evidence->evidence_class =
      dt::TimestampStatisticsEvidenceClassV3::full_visible_population;
  empty_evidence->schema_epoch = 21;
  empty_evidence->collection_epoch = 22;
  empty_evidence->security_epoch = 23;
  dt::TimestampStatisticsProjectionV3 empty_statistics;
  empty_statistics.profile = fixture.profile;
  empty_statistics.provider_evidence = empty_evidence;
  empty_statistics.statistics_uuid = V7(4);
  empty_statistics.source_object_uuid = V7(5);
  empty_statistics.schema_epoch = 21;
  empty_statistics.collection_epoch = 22;
  empty_statistics.security_epoch = 23;
  auto exact_empty_statistics = dt::EncodeTimestampStatisticsProjectionV3(
      empty_statistics, Budget(dt::kTimestampStatisticsHeaderBytesV3));
  auto short_empty_statistics = dt::EncodeTimestampStatisticsProjectionV3(
      empty_statistics, Budget(dt::kTimestampStatisticsHeaderBytesV3 - 1));
  Check(exact_empty_statistics.ok() && exact_empty_statistics.bytes.size() ==
            dt::kTimestampStatisticsHeaderBytesV3,
        "empty statistics publishes at exact header capacity");
  Check(!short_empty_statistics.ok() && short_empty_statistics.bytes.empty(),
        "empty statistics one-byte-short publishes nothing");

  std::array<dt::TimestampStatisticsHistogramRecordV3, 64> histogram{};
  std::array<dt::TimestampStatisticsMcvRecordV3, 64> mcv{};
  for (std::size_t index = 0; index != histogram.size(); ++index) {
    histogram[index] = {0, static_cast<p::u64>(index),
                        static_cast<p::u64>(index + 1)};
    mcv[index].value_hash[31] = static_cast<p::byte>(index + 1);
    mcv[index].frequency = 1;
  }
  auto maximum_evidence =
      std::make_shared<dt::TimestampStatisticsProviderEvidenceHandleV3>();
  maximum_evidence->provider_evidence_uuid = V7(9);
  maximum_evidence->source_object_uuid = V7(8);
  maximum_evidence->authenticated_population = true;
  maximum_evidence->source_authorized = true;
  maximum_evidence->privacy_admitted = true;
  maximum_evidence->evidence_class =
      dt::TimestampStatisticsEvidenceClassV3::full_visible_population;
  maximum_evidence->schema_epoch = 31;
  maximum_evidence->collection_epoch = 32;
  maximum_evidence->security_epoch = 33;
  maximum_evidence->row_count = 64;
  maximum_evidence->value_count = 64;
  maximum_evidence->minimum_maximum_present = true;
  maximum_evidence->maximum_nanoseconds_since_midnight = 63;
  maximum_evidence->histogram.assign(histogram.begin(), histogram.end());
  maximum_evidence->mcv.assign(mcv.begin(), mcv.end());
  dt::TimestampStatisticsProjectionV3 maximum_statistics;
  maximum_statistics.profile = fixture.profile;
  maximum_statistics.provider_evidence = maximum_evidence;
  maximum_statistics.statistics_uuid = V7(7);
  maximum_statistics.source_object_uuid = V7(8);
  maximum_statistics.schema_epoch = 31;
  maximum_statistics.collection_epoch = 32;
  maximum_statistics.security_epoch = 33;
  maximum_statistics.row_count = 64;
  maximum_statistics.value_count = 64;
  maximum_statistics.minimum_maximum_present = true;
  maximum_statistics.maximum_nanoseconds_since_midnight = 63;
  maximum_statistics.histogram = histogram;
  maximum_statistics.mcv = mcv;
  auto exact_maximum_statistics = dt::EncodeTimestampStatisticsProjectionV3(
      maximum_statistics, Budget(dt::kTimestampStatisticsMaximumBytesV3));
  auto short_maximum_statistics = dt::EncodeTimestampStatisticsProjectionV3(
      maximum_statistics, Budget(dt::kTimestampStatisticsMaximumBytesV3 - 1));
  Check(exact_maximum_statistics.ok() &&
            exact_maximum_statistics.bytes.size() ==
                dt::kTimestampStatisticsMaximumBytesV3,
        "maximum statistics publishes at exact 5152-byte capacity");
  Check(!short_maximum_statistics.ok() &&
            short_maximum_statistics.bytes.empty(),
        "maximum statistics one-byte-short publishes nothing");
}

void AliasingAndBatchAtomicity(Fixtures& fixture) {
  std::array<std::int32_t, 3> days{{-1, 0, 1}};
  std::array<p::u64, 3> times{{86'399'999'999'999ull, 0, 1}};
  std::array<p::byte, 1> bitmap{{0}};
  std::array<std::int32_t, 3> out_days{{7, 7, 7}};
  std::array<p::u64, 3> out_times{{7, 7, 7}};
  std::array<p::byte, 1> out_bitmap{{0x7f}};
  Check(dt::MaterializeTimestampBatchIntoV3(
            fixture.profile, days, times, bitmap, out_days.data(),
            sizeof(out_days), out_times.data(), sizeof(out_times),
            out_bitmap.data(), out_bitmap.size()).ok() &&
            out_days == days && out_times == times && out_bitmap == bitmap,
        "batch exact nonoverlap publishes all spans together");

  out_days.fill(7); out_times.fill(7); out_bitmap.fill(0x7f);
  const auto days_before = out_days;
  const auto times_before = out_times;
  const auto bitmap_before = out_bitmap;
  Check(!dt::MaterializeTimestampBatchIntoV3(
             fixture.profile, days, times, bitmap, out_days.data(),
             sizeof(out_days) - 1, out_times.data(), sizeof(out_times),
             out_bitmap.data(), out_bitmap.size()).ok() &&
            out_days == days_before && out_times == times_before &&
            out_bitmap == bitmap_before,
        "batch capacity preflight preserves all spans");

  Check(!dt::MaterializeTimestampBatchIntoV3(
             fixture.profile, days, times, bitmap,
             const_cast<std::int32_t*>(days.data()), sizeof(days),
             out_times.data(), sizeof(out_times), out_bitmap.data(),
             out_bitmap.size()).ok() && days[0] == -1,
        "undeclared exact input-output overlap refuses unchanged");

  alignas(p::u64) std::array<p::byte, 64> shared{};
  shared.fill(0xa5);
  const auto shared_before = shared;
  Check(!dt::MaterializeTimestampBatchIntoV3(
             fixture.profile, days, times, bitmap,
             reinterpret_cast<std::int32_t*>(shared.data()), sizeof(days),
             reinterpret_cast<p::u64*>(shared.data() + 8), sizeof(times),
             shared.data() + 48, bitmap.size()).ok() && shared == shared_before,
        "partial output overlap refuses unchanged");

  for (unsigned at = 1; at <= 2; ++at) {
    out_days = days_before; out_times = times_before; out_bitmap = bitmap_before;
    CancelProbe cancel{0, at};
    auto result = dt::MaterializeTimestampBatchIntoV3(
        fixture.profile, days, times, bitmap, out_days.data(), sizeof(out_days),
        out_times.data(), sizeof(out_times), out_bitmap.data(), out_bitmap.size(),
        Control(cancel));
    Check(!result.ok() && cancel.calls == at && out_days == days_before &&
              out_times == times_before && out_bitmap == bitmap_before,
          "each batch cancellation checkpoint preserves all spans");
  }
}

template <typename Invoke>
void CheckByteCancellations(std::string_view message, unsigned checkpoints,
                            Invoke&& invoke) {
  for (unsigned at = 1; at <= checkpoints; ++at) {
    CancelProbe cancel{0, at};
    auto result = invoke(Control(cancel));
    Check(!result.ok() && result.bytes.empty() && cancel.calls == at &&
              result.diagnostic.diagnostic_code == "PROCESS.CANCELLED",
          message);
  }
}

void CancellationAtomicity(Fixtures& fixture) {
  std::array<p::byte, 112> output{};
  output.fill(0xa5);
  const auto before = output;
  for (unsigned boundary = 0; boundary != 3; ++boundary) {
    CancelProbe cancel{0, 1};
    auto control = Control(cancel);
    bool refused = boundary == 0
        ? !dt::EncodeCanonicalTimestampComponentIntoNoAllocV3(
               fixture.lower, output.data(), 16, control).ok()
        : boundary == 1
        ? !dt::HashTimestampValueIntoNoAllocV3(
               fixture.lower, output.data(), 32, control).ok()
        : !dt::MakeTimestampSortKeyIntoNoAllocV3(
               fixture.lower, dt::TimestampSortDirectionV3::ascending,
               dt::TimestampNullModeV3::nulls_last, output.data(), 112,
               control).ok();
    Check(refused && output == before,
          "bounded final cancellation preserves caller output");
  }

  CheckByteCancellations("component owning cancellation is atomic", 2,
      [&](const auto& c) { return dt::EncodeCanonicalTimestampComponentV3(fixture.lower, c); });
  CheckByteCancellations("hash owning cancellation is atomic", 2,
      [&](const auto& c) { return dt::HashTimestampValueV3(fixture.lower, c); });
  CheckByteCancellations("key owning cancellation is atomic", 2,
      [&](const auto& c) { return dt::MakeTimestampSortKeyV3(
          fixture.lower, dt::TimestampSortDirectionV3::ascending,
          dt::TimestampNullModeV3::nulls_last, c); });
  CheckByteCancellations("covering cancellation is atomic", 2,
      [&](const auto& c) { return dt::EncodeTimestampCoveringValueV3(fixture.lower, c); });
  CheckByteCancellations("range cancellation is atomic", 2,
      [&](const auto& c) { return dt::EncodeTimestampRangePairV3(fixture.lower, fixture.upper, c); });
  CheckByteCancellations("statistics group/final cancellation is atomic", 3,
      [&](const auto& c) { return dt::EncodeTimestampStatisticsProjectionV3(fixture.statistics, c); });
  CheckByteCancellations("backup cancellation is atomic", 2,
      [&](const auto& c) { return dt::EncodeTimestampBackupTupleV3(fixture.lower, c); });

  for (unsigned at = 1; at <= 2; ++at) {
    CancelProbe cancel{0, at};
    auto request = fixture.zone;
    request.control = Control(cancel);
    auto result = dt::EncodeTimestampZoneMapV3(request);
    Check(!result.ok() && result.bytes.empty() && cancel.calls == at,
          "zone cancellation is atomic");
  }

  auto statistics = dt::EncodeTimestampStatisticsProjectionV3(fixture.statistics);
  Check(statistics.ok(), "statistics decode cancellation fixture builds");
  for (unsigned at = 1; at <= 3; ++at) {
    CancelProbe cancel{0, at};
    auto decoded = dt::DecodeTimestampStatisticsProjectionV3(
        fixture.profile, statistics.bytes, Control(cancel));
    Check(!decoded.ok() && decoded.statistics.profile == nullptr &&
              decoded.statistics.histogram.empty() &&
              decoded.statistics.mcv.empty() && cancel.calls == at,
          "each statistics decode checkpoint publishes no partial result");
  }
  auto component = dt::EncodeCanonicalTimestampComponentV3(fixture.lower);
  auto key = dt::MakeTimestampSortKeyV3(
      fixture.lower, dt::TimestampSortDirectionV3::ascending,
      dt::TimestampNullModeV3::nulls_last);
  auto covering = dt::EncodeTimestampCoveringValueV3(fixture.lower);
  auto range = dt::EncodeTimestampRangePairV3(fixture.lower, fixture.upper);
  auto zone = dt::EncodeTimestampZoneMapV3(fixture.zone);
  auto backup = dt::EncodeTimestampBackupTupleV3(fixture.lower);
  Check(component.ok() && key.ok() && covering.ok() && range.ok() &&
            zone.ok() && backup.ok(), "decoder cancellation fixtures build");
  {
    CancelProbe cancel{0, 1};
    auto result = dt::DecodeCanonicalTimestampComponentNoAllocV3(
        *fixture.profile, dt::TimestampValueStateV3::value, false,
        component.bytes, Control(cancel));
    Check(!result.ok() && result.value.profile == nullptr,
          "component decode final cancellation publishes no view");
  }
  {
    CancelProbe cancel{0, 1};
    auto result = dt::DecodeTimestampSortKeyNoAllocV3(
        *fixture.profile, key.bytes, Control(cancel));
    Check(!result.ok() && result.value.profile == nullptr,
          "key decode final cancellation publishes no view");
  }
  {
    CancelProbe cancel{0, 1};
    auto result = dt::DecodeTimestampCoveringValueNoAllocV3(
        *fixture.profile, true, covering.bytes, Control(cancel));
    Check(!result.ok() && result.value.profile_handle == nullptr &&
              cancel.calls == 1,
          "covering decode final checkpoint publishes no view");
  }
  {
    CancelProbe cancel{0, 1};
    auto result = dt::DecodeTimestampRangePairNoAllocV3(
        *fixture.profile, range.bytes, Control(cancel));
    Check(!result.ok() && result.value.lower_key.empty() &&
              result.value.upper_key.empty(),
          "range decode final cancellation publishes no spans");
  }
  {
    CancelProbe cancel{0, 1};
    auto result = dt::DecodeTimestampZoneMapNoAllocV3(
        *fixture.profile, zone.bytes, Control(cancel));
    Check(!result.ok() && result.value.profile_handle == nullptr &&
              result.value.minimum_key.empty() && result.value.maximum_key.empty(),
          "zone decode final cancellation publishes no view");
  }
  {
    CancelProbe cancel{0, 1};
    auto result = dt::DecodeTimestampBackupTupleNoAllocV3(
        *fixture.profile, true, backup.bytes, Control(cancel));
    Check(!result.ok() && result.tuple.profile_handle == nullptr,
          "backup decode final cancellation publishes no view");
  }
  auto projection = BtreeRequest(fixture);
  for (unsigned at = 1; at <= 2; ++at) {
    CancelProbe cancel{0, at};
    projection.control = Control(cancel);
    auto result = dt::ProjectTimestampIndexValueV3(projection);
    Check(!result.ok() && result.bytes.empty() && cancel.calls == at,
          "each index projection checkpoint publishes no bytes");
  }
}

void ScrubCoverage(Fixtures& fixture) {
  ScrubObservation observation;
  auto control = Observed(observation);
  Check(dt::ValidateTimestampProfileHandleV3(*fixture.profile, control).ok(),
        "profile stages scrub");
  auto component = dt::EncodeCanonicalTimestampComponentV3(fixture.lower);
  std::array<p::byte, 112> output{};
  Check(component.ok() &&
            dt::DecodeCanonicalTimestampComponentNoAllocV3(
                *fixture.profile, dt::TimestampValueStateV3::value, false,
                component.bytes, control).ok() &&
            dt::EncodeCanonicalTimestampComponentIntoNoAllocV3(
                fixture.lower, output.data(), 16, control).ok(),
        "component stages scrub");
  Check(dt::RenderCanonicalTimestampV3(fixture.lower, true, control).ok(),
        "render stages scrub");
  Check(dt::HashTimestampValueV3(fixture.lower, control).ok(),
        "hash stages scrub");
  auto key = dt::MakeTimestampSortKeyV3(
      fixture.lower, dt::TimestampSortDirectionV3::ascending,
      dt::TimestampNullModeV3::nulls_last, control);
  Check(key.ok() && dt::DecodeTimestampSortKeyNoAllocV3(
                        *fixture.profile, key.bytes, control).ok(),
        "ordered-key stages scrub");

  auto* character_identity = Identity(dt::CanonicalTypeId::character);
  auto character_descriptor = Descriptor(dt::CanonicalTypeId::character);
  dt::TimestampCastRequestV3 cast;
  cast.one_based_policy_row = 50;
  cast.timestamp_source = &fixture.lower;
  cast.scalar_target = dt::CanonicalTypeId::character;
  cast.scalar_target_descriptor = character_descriptor;
  cast.scalar_target_identity = character_identity;
  cast.context = dt::DatatypeCastContext::explicit_cast;
  cast.target_null_allowed = character_descriptor.nullable_allowed;
  cast.control = control;
  Check(dt::CastTimestampValueV3(cast).ok(), "character cast stage scrubs");

  std::array<std::int32_t, 2> days{{-1, 0}}, out_days{};
  std::array<p::u64, 2> times{{86'399'999'999'999ull, 0}}, out_times{};
  std::array<p::byte, 1> bitmap{{0}}, out_bitmap{};
  Check(dt::MaterializeTimestampBatchIntoV3(
            fixture.profile, days, times, bitmap, out_days.data(),
            sizeof(out_days), out_times.data(), sizeof(out_times),
            out_bitmap.data(), out_bitmap.size(), control).ok(),
        "batch stages scrub after publication");

  auto val = dt::EncodeTimestampSbdvalComposedV3(fixture.lower, true, control);
  auto pv = dt::EncodeTimestampSbdpvComposedV3(fixture.lower, true, control);
  Check(val.ok() && pv.ok() &&
            dt::DecodeTimestampSbdvalComposedNoAllocV3(
                *fixture.profile, true, val.bytes, control).ok() &&
            dt::DecodeTimestampSbdpvComposedNoAllocV3(
                *fixture.profile, true, pv.bytes, control).ok(),
        "composed framing stages scrub");

  auto covering = dt::EncodeTimestampCoveringValueV3(fixture.lower, control);
  auto range = dt::EncodeTimestampRangePairV3(
      fixture.lower, fixture.upper, control);
  auto zone_request = fixture.zone;
  zone_request.control = control;
  auto zone = dt::EncodeTimestampZoneMapV3(zone_request);
  auto statistics = dt::EncodeTimestampStatisticsProjectionV3(
      fixture.statistics, control);
  auto backup = dt::EncodeTimestampBackupTupleV3(fixture.lower, control);
  Check(covering.ok() && range.ok() && zone.ok() && statistics.ok() &&
            backup.ok(), "projection scrub fixtures publish");
  Check(dt::DecodeTimestampCoveringValueNoAllocV3(
            *fixture.profile, true, covering.bytes, control).ok() &&
            dt::DecodeTimestampRangePairNoAllocV3(
                *fixture.profile, range.bytes, control).ok() &&
            dt::DecodeTimestampZoneMapNoAllocV3(
                *fixture.profile, zone.bytes, control).ok() &&
            dt::DecodeTimestampStatisticsProjectionV3(
                fixture.profile, statistics.bytes, control).ok() &&
            dt::DecodeTimestampBackupTupleNoAllocV3(
                *fixture.profile, true, backup.bytes, control).ok(),
        "projection decode stages scrub");

  auto cancelling = [&](CancelProbe& cancel) {
    auto result = Control(cancel);
    result.observe_scrubbed = ObserveScrub;
    result.scrub_observer_context = &observation;
    return result;
  };
  { CancelProbe c{0, 2}; Check(!dt::EncodeCanonicalTimestampComponentV3(
        fixture.lower, cancelling(c)).ok(), "component owner scrubs on failure"); }
  { CancelProbe c{0, 2}; Check(!dt::HashTimestampValueV3(
        fixture.lower, cancelling(c)).ok(), "hash owner scrubs on failure"); }
  { CancelProbe c{0, 2}; Check(!dt::MakeTimestampSortKeyV3(
        fixture.lower, dt::TimestampSortDirectionV3::ascending,
        dt::TimestampNullModeV3::nulls_last, cancelling(c)).ok(),
        "key owner scrubs on failure"); }
  { CancelProbe c{0, 2}; Check(!dt::EncodeTimestampSbdvalComposedV3(
        fixture.lower, true, cancelling(c)).ok(), "SBDVAL owner scrubs on failure"); }
  { CancelProbe c{0, 2}; Check(!dt::EncodeTimestampSbdpvComposedV3(
        fixture.lower, true, cancelling(c)).ok(), "SBDPV owner scrubs on failure"); }
  { CancelProbe c{0, 2}; Check(!dt::EncodeTimestampBackupTupleV3(
        fixture.lower, cancelling(c)).ok(), "projection owner scrubs on failure"); }

  Check(observation.all_zero,
        "every scrub callback observes zeroed mutable private storage");
  for (std::size_t index = 0; index != observation.calls.size(); ++index) {
    ++checks;
    if (observation.calls[index] == 0) {
      ++failures;
      std::cerr << "FAIL unexercised timestamp scrub class index=" << index
                << '\n';
    }
  }
}

void AllocationFailureAndRetry(Fixtures& fixture) {
  auto fail_once = [&](auto&& invoke, std::string_view message) {
    BeginHeapFailure(1);
    auto failed = invoke();
    EndHeapFailure();
    Check(!failed.ok() && failed.bytes.empty() &&
              failed.diagnostic.diagnostic_code == "RESOURCE.BUDGET_EXCEEDED",
          message);
    auto retry = invoke();
    Check(retry.ok() && !retry.bytes.empty(),
          "deterministic retry succeeds after heap failure");
  };
  fail_once([&] { return dt::EncodeCanonicalTimestampComponentV3(fixture.lower); },
            "component heap failure publishes nothing");
  fail_once([&] { return dt::HashTimestampValueV3(fixture.lower); },
            "hash heap failure publishes nothing");
  fail_once([&] { return dt::MakeTimestampSortKeyV3(
                fixture.lower, dt::TimestampSortDirectionV3::ascending,
                dt::TimestampNullModeV3::nulls_last); },
            "key heap failure publishes nothing");
  fail_once([&] { return dt::EncodeTimestampCoveringValueV3(fixture.lower); },
            "covering heap failure publishes nothing");
  fail_once([&] { return dt::EncodeTimestampRangePairV3(
                fixture.lower, fixture.upper); },
            "range heap failure publishes nothing");
  fail_once([&] { return dt::EncodeTimestampZoneMapV3(fixture.zone); },
            "zone heap failure publishes nothing");
  fail_once([&] { return dt::EncodeTimestampStatisticsProjectionV3(
                fixture.statistics); },
            "statistics heap failure publishes nothing");
  fail_once([&] { return dt::EncodeTimestampBackupTupleV3(fixture.lower); },
            "backup heap failure publishes nothing");

  auto encoded = dt::EncodeTimestampStatisticsProjectionV3(fixture.statistics);
  Check(encoded.ok(), "statistics heap-failure fixture builds");
  for (std::size_t at = 1; at <= 2; ++at) {
    BeginHeapFailure(at);
    auto decoded = dt::DecodeTimestampStatisticsProjectionV3(
        fixture.profile, encoded.bytes);
    EndHeapFailure();
    Check(!decoded.ok() && decoded.statistics.profile == nullptr &&
              decoded.statistics.histogram.empty() &&
              decoded.statistics.mcv.empty(),
          "statistics decode allocation failure publishes no result");
  }
  Check(dt::DecodeTimestampStatisticsProjectionV3(
            fixture.profile, encoded.bytes).ok(),
        "statistics decode retry succeeds");

  std::array<std::int32_t, 3> days{{-1, 0, 1}}, out_days{{7, 7, 7}};
  std::array<p::u64, 3> times{{86'399'999'999'999ull, 0, 1}},
      out_times{{7, 7, 7}};
  std::array<p::byte, 1> bitmap{{0}}, out_bitmap{{0x7f}};
  const auto before_days = out_days;
  const auto before_times = out_times;
  const auto before_bitmap = out_bitmap;
  for (std::size_t at = 1; at <= 3; ++at) {
    out_days = before_days; out_times = before_times; out_bitmap = before_bitmap;
    BeginHeapFailure(at);
    auto failed = dt::MaterializeTimestampBatchIntoV3(
        fixture.profile, days, times, bitmap, out_days.data(), sizeof(out_days),
        out_times.data(), sizeof(out_times), out_bitmap.data(), out_bitmap.size());
    EndHeapFailure();
    Check(!failed.ok() && out_days == before_days &&
              out_times == before_times && out_bitmap == before_bitmap,
          "batch allocation failure preserves all outputs");
  }
  Check(dt::MaterializeTimestampBatchIntoV3(
            fixture.profile, days, times, bitmap, out_days.data(),
            sizeof(out_days), out_times.data(), sizeof(out_times),
            out_bitmap.data(), out_bitmap.size()).ok(),
        "batch retry succeeds after all allocation failures");
  for (std::size_t at = 1; at <= 3; ++at) {
    BeginHeapFailure(at);
    auto failed = dt::MaterializeTimestampBatchV3(
        fixture.profile, days, times, bitmap);
    EndHeapFailure();
    Check(!failed.ok() && failed.batch.profile == nullptr &&
              failed.batch.civil_days.empty() &&
              failed.batch.nanoseconds_since_midnight.empty() &&
              failed.batch.null_bitmap_lsb0.empty(),
          "owned batch allocation failure publishes no partial result");
  }
  Check(dt::MaterializeTimestampBatchV3(
            fixture.profile, days, times, bitmap).ok(),
        "owned batch retry succeeds after all allocation failures");

  auto projection = BtreeRequest(fixture);
  BeginHeapFailure(1);
  auto failed_projection = dt::ProjectTimestampIndexValueV3(projection);
  EndHeapFailure();
  Check(!failed_projection.ok() && failed_projection.bytes.empty() &&
            failed_projection.diagnostic.diagnostic_code ==
                "RESOURCE.BUDGET_EXCEEDED",
        "index projection heap failure publishes no result");
  Check(dt::ProjectTimestampIndexValueV3(projection).ok(),
        "index projection retry succeeds");
}

void PinLifetimeAndOwnership(Fixtures& fixture) {
  auto check_pin = [&](auto&& invoke, unsigned cancel_at,
                       std::string_view message) {
    const long baseline = fixture.profile.use_count();
    PinProbe probe{0, cancel_at, fixture.profile, 0};
    auto result = invoke(PinControl(probe));
    Check(!result.ok() && probe.calls == cancel_at &&
              probe.observed == baseline + 1 &&
              fixture.profile.use_count() == baseline,
          message);
  };
  check_pin([&](const auto& c) {
      return dt::EncodeCanonicalTimestampComponentV3(fixture.lower, c); }, 2,
      "component holds exactly one operation pin through final checkpoint");
  check_pin([&](const auto& c) {
      return dt::HashTimestampValueV3(fixture.lower, c); }, 2,
      "hash holds exactly one operation pin through final checkpoint");
  check_pin([&](const auto& c) {
      return dt::MakeTimestampSortKeyV3(
          fixture.lower, dt::TimestampSortDirectionV3::ascending,
          dt::TimestampNullModeV3::nulls_last, c); }, 2,
      "key holds exactly one operation pin through final checkpoint");
  check_pin([&](const auto& c) {
      return dt::EncodeTimestampCoveringValueV3(fixture.lower, c); }, 2,
      "covering holds exactly one operation pin through final checkpoint");
  check_pin([&](const auto& c) {
      return dt::EncodeTimestampRangePairV3(fixture.lower, fixture.upper, c); }, 2,
      "range holds exactly one operation pin through final checkpoint");
  check_pin([&](const auto& c) {
      fixture.zone.control = c;
      auto result = dt::EncodeTimestampZoneMapV3(fixture.zone);
      fixture.zone.control = {};
      return result; }, 2,
      "zone holds exactly one operation pin through final checkpoint");
  check_pin([&](const auto& c) {
      return dt::EncodeTimestampStatisticsProjectionV3(fixture.statistics, c); }, 3,
      "statistics holds exactly one profile pin through final checkpoint");
  check_pin([&](const auto& c) {
      return dt::EncodeTimestampBackupTupleV3(fixture.lower, c); }, 2,
      "backup holds exactly one operation pin through final checkpoint");
  check_pin([&](const auto& c) {
      auto request = BtreeRequest(fixture);
      request.control = c;
      return dt::ProjectTimestampIndexValueV3(request); }, 2,
      "index projection holds exactly one operation pin through final checkpoint");

  auto encoded = dt::EncodeTimestampStatisticsProjectionV3(fixture.statistics);
  const long before_decode = fixture.profile.use_count();
  {
    auto decoded = dt::DecodeTimestampStatisticsProjectionV3(
        fixture.profile, encoded.bytes);
    Check(decoded.ok() && fixture.profile.use_count() == before_decode + 1 &&
              decoded.statistics.profile.get() == fixture.profile.get(),
          "statistics decode transfers exactly one profile owner");
  }
  Check(fixture.profile.use_count() == before_decode,
        "statistics decode owner releases exactly once");

  std::array<std::int32_t, 1> days{{0}};
  std::array<p::u64, 1> times{{0}};
  std::array<p::byte, 1> bitmap{{0}};
  const long before_batch = fixture.profile.use_count();
  {
    auto batch = dt::MaterializeTimestampBatchV3(
        fixture.profile, days, times, bitmap);
    Check(batch.ok() && fixture.profile.use_count() == before_batch + 1 &&
              batch.batch.profile.get() == fixture.profile.get(),
          "owned batch transfers exactly one profile owner");
  }
  Check(fixture.profile.use_count() == before_batch,
        "owned batch profile owner releases exactly once");
}

void ConcurrentDisjointPublication(Fixtures& fixture) {
  auto oracle = dt::EncodeCanonicalTimestampComponentV3(fixture.lower);
  Check(oracle.ok(), "concurrency oracle builds");
  const long pins = fixture.profile.use_count();
  std::atomic<bool> start{false};
  std::array<bool, 8> accepted{};
  std::array<std::thread, 8> workers;
  for (std::size_t index = 0; index != workers.size(); ++index) {
    workers[index] = std::thread([&, index] {
      std::array<p::byte, 16> output{};
      while (!start.load(std::memory_order_acquire)) {}
      bool stable = true;
      for (unsigned iteration = 0; iteration != 250; ++iteration) {
        auto result = dt::EncodeCanonicalTimestampComponentIntoNoAllocV3(
            fixture.lower, output.data(), output.size());
        stable &= result.ok() &&
            std::equal(output.begin(), output.end(), oracle.bytes.begin());
      }
      accepted[index] = stable;
    });
  }
  start.store(true, std::memory_order_release);
  for (auto& worker : workers) worker.join();
  Check(std::all_of(accepted.begin(), accepted.end(),
                    [](bool value) { return value; }),
        "shared immutable profile/input gives deterministic disjoint outputs");
  Check(fixture.profile.use_count() == pins,
        "concurrent disjoint publication leaks no profile owner");
  // Authority assigns a single caller-owned writer to each output span.
}

void NoPartialFactOrMetricPublication() {
  dt::TimestampDiagnosticRouteExecutionRequestV3 unknown_route;
  unknown_route.diagnostic_code = "UNKNOWN.TIMESTAMP.ROUTE";
  auto route = dt::ExecuteTimestampDiagnosticRouteV3(unknown_route);
  Check(!route.ok() && !route.diagnostic_fact_emitted &&
            !route.metric_mutation_admitted && route.datatype_outcome_unchanged &&
            route.metric_outcome_unchanged,
        "failed diagnostic route publishes no fact or metric mutation");
  dt::TimestampMetricEvidenceEnvelopeV3 evidence;
  dt::TimestampMetricEvidenceValidationContextV3 context;
  auto metric = dt::ValidateTimestampMetricEvidenceV3(evidence, context);
  Check(!metric.receiving_owner_update_admitted &&
            metric.datatype_outcome_unchanged,
        "failed metric admission publishes no update or datatype change");
}

}  // namespace

int main() {
  auto fixture = MakeFixtures();
  RefreshFixtureSpans(fixture);
  ExactCapacityAndPrecedence(fixture);
  NullEmptyAndMaximumCapacity(fixture);
  AliasingAndBatchAtomicity(fixture);
  CancellationAtomicity(fixture);
  ScrubCoverage(fixture);
  AllocationFailureAndRetry(fixture);
  PinLifetimeAndOwnership(fixture);
  ConcurrentDisjointPublication(fixture);
  NoPartialFactOrMetricPublication();
  if (failures != 0) {
    std::cerr << "FAIL base.timestamp V3 publication atomicity checks=" << checks
              << " failures=" << failures << '\n';
    return 1;
  }
  std::cout << "PASS base.timestamp V3 publication atomicity checks=" << checks
            << '\n';
}
