// Copyright (c) 2026 ScratchBird Software Inc.
// SPDX-License-Identifier: MPL-2.0
#pragma once
#include "metric_history.hpp"
#include "../hash/hash_digest.hpp"
#include <algorithm>
#include <bit>
#include <limits>
#include <span>
#include <stdexcept>
#include <type_traits>

namespace scratchbird::core::metrics {
// Persistence framing only. Catalog bindings, observation admission and MGA
// publication remain the caller's responsibility. UUIDs are always raw16.
namespace history_codec {
using Bytes = std::vector<std::uint8_t>;
inline constexpr std::size_t kMaximumBytes = 64 * 1024 * 1024;
inline constexpr std::size_t kMaximumItems = 65536;
inline constexpr std::size_t kMaximumTextBytes = 1024 * 1024;
struct Invalid {};
template<class A, class T> void Fields(A& archive, T& value);

struct Writer {
  Bytes bytes;
  void Raw(const void* data, std::size_t size) {
    if (size > kMaximumBytes - bytes.size()) throw Invalid{};
    if (!size) return;
    const auto* begin = static_cast<const std::uint8_t*>(data);
    bytes.insert(bytes.end(), begin, begin + size);
  }
  template<class T> requires std::is_integral_v<T>
  void One(const T value) {
    if constexpr (std::is_same_v<T, bool>) One(std::uint8_t(value));
    else {
      using U = std::make_unsigned_t<T>;
      const U bits = static_cast<U>(value);
      for (std::size_t i = 0; i < sizeof(T); ++i) {
        const auto byte = static_cast<std::uint8_t>(bits >> (i * 8));
        Raw(&byte, 1);
      }
    }
  }
  template<class T> requires std::is_enum_v<T>
  void One(T value) { One(static_cast<std::uint32_t>(value)); }
  void One(double value) { One(std::bit_cast<std::uint64_t>(value)); }
  void One(const std::monostate&) {}
  void One(const MetricUuid& value) { Raw(value.bytes.data(), 16); }
  void One(const MetricFloat128& value) { Raw(value.bytes.data(), 16); }
  void One(const MetricDecimal128& value) { Raw(value.bytes.data(), 16); }
  void One(const MetricEnumValue& value) { One(value.code); }
  void One(const std::string& value) {
    if (value.size() > kMaximumTextBytes) throw Invalid{};
    One(static_cast<std::uint32_t>(value.size())); Raw(value.data(), value.size());
  }
  template<class... T> void One(const std::variant<T...>& value) {
    if (value.valueless_by_exception()) throw Invalid{};
    One(static_cast<std::uint8_t>(value.index()));
    std::visit([&](const auto& item) { One(item); }, value);
  }
  void One(const MetricScalar& value) { One(static_cast<const MetricScalar::Base&>(value)); }
  void One(const MetricLabelValue& value) { One(static_cast<const MetricLabelValue::Base&>(value)); }
  template<class T> void One(const std::vector<T>& values) {
    if (values.size() > kMaximumItems) throw Invalid{};
    One(static_cast<std::uint32_t>(values.size()));
    for (const auto& value : values) One(value);
  }
  template<class T> requires (!std::is_arithmetic_v<T> && !std::is_enum_v<T>)
  void One(const T& value) { Fields(*this, value); }
  template<class... T> void operator()(const T&... values) { (One(values), ...); }
};

struct Reader {
  std::span<const std::uint8_t> bytes;
  std::size_t offset = 0;
  void Raw(void* data, std::size_t size) {
    if (size > bytes.size() - offset) throw Invalid{};
    if (size) std::copy_n(bytes.data() + offset, size, static_cast<std::uint8_t*>(data));
    offset += size;
  }
  template<class T> requires std::is_integral_v<T>
  void One(T& value) {
    if constexpr (std::is_same_v<T, bool>) {
      std::uint8_t byte; One(byte); if (byte > 1) throw Invalid{}; value = byte != 0;
    } else {
      using U = std::make_unsigned_t<T>;
      U bits = 0;
      for (std::size_t i = 0; i < sizeof(T); ++i) {
        std::uint8_t byte; Raw(&byte, 1); bits |= static_cast<U>(byte) << (i * 8);
      }
      value = std::bit_cast<T>(bits);
    }
  }
  template<class T> requires std::is_enum_v<T>
  void One(T& value) { std::uint32_t bits; One(bits); value = static_cast<T>(bits); }
  void One(double& value) { std::uint64_t bits; One(bits); value = std::bit_cast<double>(bits); }
  void One(std::monostate&) {}
  void One(MetricUuid& value) { Raw(value.bytes.data(), 16); }
  void One(MetricFloat128& value) { Raw(value.bytes.data(), 16); }
  void One(MetricDecimal128& value) { Raw(value.bytes.data(), 16); }
  void One(MetricEnumValue& value) { One(value.code); }
  void One(std::string& value) {
    std::uint32_t size; One(size);
    if (size > kMaximumTextBytes || size > bytes.size() - offset) throw Invalid{};
    value.assign(reinterpret_cast<const char*>(bytes.data() + offset), size); offset += size;
  }
  template<std::size_t I = 0, class... T>
  void Alternative(std::uint8_t index, std::variant<T...>& value) {
    if constexpr (I == sizeof...(T)) throw Invalid{};
    else if (index == I) {
      std::variant_alternative_t<I, std::variant<T...>> item{};
      One(item); value = std::move(item);
    } else Alternative<I + 1>(index, value);
  }
  template<class... T> void One(std::variant<T...>& value) {
    std::uint8_t index; One(index); Alternative(index, value);
  }
  void One(MetricScalar& value) { One(static_cast<MetricScalar::Base&>(value)); }
  void One(MetricLabelValue& value) { One(static_cast<MetricLabelValue::Base&>(value)); }
  template<class T> void One(std::vector<T>& values) {
    std::uint32_t size; One(size);
    if (size > kMaximumItems || size > bytes.size() - offset) throw Invalid{};
    values.resize(size); for (auto& value : values) One(value);
  }
  template<class T> requires (!std::is_arithmetic_v<T> && !std::is_enum_v<T>)
  void One(T& value) { Fields(*this, value); }
  template<class... T> void operator()(T&... values) { (One(values), ...); }
};

template<class A, class T> void Binding(A& a, T& v) {
  a(v.metric_uuid, v.descriptor_generation, v.label_schema_uuid, v.label_schema_generation,
    v.retention_policy_uuid, v.retention_policy_generation, v.visibility_policy_uuid,
    v.visibility_policy_generation, v.rate_source_counter_uuid, v.rate_source_counter_generation,
    v.database_uuid, v.node_uuid, v.cluster_uuid);
}
template<class A, class T> void Fields(A& a, T& v) {
  using V = std::remove_cv_t<T>;
  if constexpr (std::is_same_v<V, MetricHistoryStore>) {
    a(v.policies, v.series, v.raw_samples, v.rollups, v.evidence);
  } else if constexpr (std::is_same_v<V, MetricRetentionPolicy>) {
    a(v.policy_uuid, v.generation, v.policy_name, v.scope, v.mode,
      v.raw_retention_seconds, v.rollup_retention_seconds, v.rollup_grains,
      v.purge_batch_limit, v.max_cardinality, v.overflow_behavior, v.edit_right,
      v.default_admin_group, v.evidence_required);
  } else if constexpr (std::is_same_v<V, MetricSeriesIdentity>) {
    Binding(a, v);
    a(v.series_uuid, v.series_definition_generation, v.metric_family, v.namespace_path,
      v.producer_owner, v.scope_class, v.labels, v.redaction_class);
    // The derived lookup key never introduces another serialized identity.
    if constexpr (std::is_same_v<A, Reader>) {
      std::vector<std::pair<std::string, MetricLabelValue>> labels;
      for (const auto& label : v.labels) labels.emplace_back(label.key, label.value);
      std::sort(labels.begin(), labels.end());
      v.series_key = {v.database_uuid, v.node_uuid, v.cluster_uuid, v.metric_uuid,
                     v.label_schema_uuid, std::move(labels)};
    }
  } else if constexpr (std::is_same_v<V, MetricRawSampleRecord>) {
    Binding(a, v);
    a(v.sample_uuid, v.series_uuid, v.series_definition_generation, v.metric_family,
      v.labels, v.sample_time_utc_ns, v.collection_time_utc_ns, v.publication_time_utc_ns,
      v.source_sequence, v.revision, v.clock_quality, v.freshness_class, v.value, v.evidence_uuid);
  } else if constexpr (std::is_same_v<V, MetricRollupRecord>) {
    a(v.rollup_uuid, v.series_uuid, v.metric_family, v.grain, v.window_start_microseconds,
      v.window_end_microseconds, v.sample_count, v.min_value, v.max_value, v.avg_value,
      v.last_value, v.sum_value, v.histogram_count, v.histogram_sum, v.histogram_buckets,
      v.state_transition_count, v.last_state_text, v.evidence_uuid);
  } else if constexpr (std::is_same_v<V, MetricRetentionEvidenceRecord>) {
    a(v.evidence_uuid, v.operation, v.policy_uuid, v.series_uuid, v.metric_family,
      v.cutoff_time_microseconds, v.rows_affected, v.actor_uuid, v.transaction_uuid,
      v.decision, v.detail);
  } else if constexpr (std::is_same_v<V, MetricValue>) {
    a(v.family, v.labels, v.type, v.value, v.count, v.sum, v.buckets, v.bucket_bounds,
      v.buckets_cumulative, v.arithmetic_inexact, v.state_text);
  } else if constexpr (std::is_same_v<V, MetricLabel>) {
    a(v.key, v.value);
  } else static_assert(!sizeof(V), "unsupported metric history record");
}
} // namespace history_codec

inline std::optional<history_codec::Bytes> EncodeMetricHistoryStoreBinary(
    const MetricHistoryStore& store) noexcept {
  try {
    history_codec::Writer writer;
    writer.Raw("SBMTH002", 8);
    writer.One(store);
    if (writer.bytes.size() > history_codec::kMaximumBytes - 32) return std::nullopt;
    const auto digest = hash::ComputeSha256Digest(writer.bytes);
    if (!digest.ok()) return std::nullopt;
    writer.Raw(digest.digest.data(), digest.digest.size());
    return std::move(writer.bytes);
  } catch (...) { return std::nullopt; }
}
inline bool DecodeMetricHistoryStoreBinary(std::span<const std::uint8_t> bytes,
                                          MetricHistoryStore* output) noexcept {
  try {
    if (!output || bytes.size() < 60 || bytes.size() > history_codec::kMaximumBytes ||
        std::string_view(reinterpret_cast<const char*>(bytes.data()), 8) != "SBMTH002") return false;
    const auto payload = bytes.first(bytes.size() - 32);
    const auto digest = hash::ComputeSha256Digest(payload.data(), payload.size());
    if (!digest.ok() || !std::equal(digest.digest.begin(), digest.digest.end(), bytes.end() - 32)) return false;
    history_codec::Reader reader{payload, 8};
    MetricHistoryStore decoded;
    reader.One(decoded);
    if (reader.offset != payload.size()) return false;
    *output = std::move(decoded);
    return true;
  } catch (...) { return false; }
}
} // namespace scratchbird::core::metrics
