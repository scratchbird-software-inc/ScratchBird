// Copyright (c) 2026 ScratchBird Software Inc.
// SPDX-License-Identifier: MPL-2.0
// SEARCH_KEY: SB_CATALOG_NATIVE_AGENT_RECORDS
#include "catalog_native_agent_record.hpp"
#include "uuid.hpp"

#include <bit>
#include <bitset>
#include <charconv>
#include <cmath>
#include <cstdint>
#include <limits>

namespace scratchbird::core::catalog {
namespace {
using E = CatalogValueError;
using T = CatalogValueType;
constexpr UuidKind kObj = UuidKind::object;
constexpr std::size_t kMaxFields = 48, kArenaBytes = 1024;

bool Id(const Uuid& id) { return uuid::IsEngineIdentityUuid(id); }
bool Typed(const TypedUuid& id, UuidKind kind) { return id.kind == kind && Id(id.value); }
bool SameTyped(const TypedUuid& a, const TypedUuid& b) { return a.kind == b.kind && a.value == b.value; }

// ---- known-value checks ----
template <class En, std::size_t N>
bool InTags(const std::array<CatalogAgentTagEntry<En>, N>& table, En v) {
  for (const auto& e : table)
    if (e.value == v) return true;
  return false;
}
bool KnownCode(CatalogAgentType v) { return IsKnownCatalogAgentType(v); }
bool KnownCode(CatalogAgentPolicyFamily v) { return IsKnownCatalogAgentPolicyFamily(v); }
bool KnownCode(CatalogMetricFamily v) { return IsKnownCatalogMetricFamily(v); }
bool KnownCode(CatalogAgentRuntimeLayer v) { return InTags(kCatalogAgentRuntimeLayerTags, v); }
bool KnownCode(CatalogAgentDeployment v) { return InTags(kCatalogAgentDeploymentTags, v); }
bool KnownCode(CatalogAgentAuthorityClass v) { return InTags(kCatalogAgentAuthorityClassTags, v); }
bool KnownCode(CatalogAgentActivationProfile v) { return InTags(kCatalogAgentActivationProfileTags, v); }
bool KnownCode(CatalogAgentActionMode v) { return InTags(kCatalogAgentActionModeTags, v); }
bool KnownCode(CatalogAgentPolicyClass v) { return InTags(kCatalogAgentPolicyClassTags, v); }
bool KnownCode(CatalogAgentRegistrationState v) { return InTags(kCatalogAgentRegistrationStateTags, v); }
bool KnownCode(CatalogAgentManagedBy v) { return InTags(kCatalogAgentManagedByTags, v); }
bool KnownCode(CatalogAgentLaunchTrigger v) { return InTags(kCatalogAgentLaunchTriggerTags, v); }
bool KnownCode(CatalogAgentInvalidPolicyBehavior v) { return InTags(kCatalogAgentInvalidPolicyBehaviorTags, v); }
// A generated enum: values 1..count, in schema-map order.
template <class En>
bool KnownGenerated(En v, unsigned count) {
  const auto n = static_cast<unsigned>(v);
  return n >= 1 && n <= count;
}
constexpr u64 ScopeMask() {
  u64 mask = 0;
  for (const auto& e : kCatalogAgentScopeTags) mask |= static_cast<u64>(e.value);
  return mask;
}
bool ValidScope(CatalogAgentScope s) {
  const auto bits = static_cast<u64>(s);
  return bits != 0 && (bits & ~ScopeMask()) == 0;
}
// Every entry a listed nonzero code, no duplicate, within capacity; nonempty
// when required.
template <class L>
bool ValidList(const L& list, bool nonempty) {
  if (list.count > L::capacity || (nonempty && !list.count)) return false;
  std::bitset<65536> seen;
  for (const auto v : list.values()) {
    const auto code = static_cast<u16>(v);
    if (!KnownCode(v) || seen.test(code)) return false;
    seen.set(code);
  }
  return true;
}
// Decimal: finite, not negative (no -0), at most `maximum` when bounded. Every
// schema-map decimal has minimum 0 (checked by the generator).
bool ValidDecimal(double v, bool bounded, double maximum) {
  return std::isfinite(v) && !std::signbit(v) && (!bounded || v <= maximum);
}

// ---- field builder: borrowed views over the record (identities) and over
// this object's own storage (numbers, flags, code lists). Ascending tags. ----
struct Fields {
  std::array<CatalogValueFieldView, kMaxFields> views{};
  std::array<std::array<byte, 8>, kMaxFields> numbers{};
  std::array<byte, kMaxFields> flags{};
  std::array<byte, kArenaBytes> arena{};
  std::size_t size = 0, number_count = 0, flag_count = 0, arena_used = 0;
  Fields() = default;
  Fields(const Fields&) = delete;
  Fields& operator=(const Fields&) = delete;
  void Identity(u16 tag, const Uuid& id, UuidKind kind) { views[size++] = {tag, T::engine_identity, kind, id.bytes}; }
  void Number(u16 tag, u64 value) {
    auto& b = numbers[number_count++];
    platform::StoreLittle64(b.data(), value);
    views[size++] = {tag, T::unsigned_integer, UuidKind::unknown, b};
  }
  template <class En>
  void Enum(u16 tag, En v) { Number(tag, static_cast<u64>(v)); }
  void Flag(u16 tag, bool v) {
    auto& b = flags[flag_count++];
    b = v ? 1 : 0;
    views[size++] = {tag, T::boolean, UuidKind::unknown, std::span<const byte>(&b, 1)};
  }
  void Decimal(u16 tag, double v) {
    auto& b = numbers[number_count++];
    platform::StoreLittle64(b.data(), std::bit_cast<u64>(v));
    views[size++] = {tag, T::opaque_bytes, UuidKind::unknown, b};
  }
  template <class L>
  void List(u16 tag, const L& list) {
    const auto start = arena_used;
    for (const auto v : list.values()) {
      const auto code = static_cast<u16>(v);
      arena[arena_used++] = static_cast<byte>(code & 0xff);
      arena[arena_used++] = static_cast<byte>(code >> 8);
    }
    views[size++] = {tag, T::opaque_bytes, UuidKind::unknown,
                     std::span<const byte>(arena).subspan(start, arena_used - start)};
  }
  void OptNumber(u16 tag, const std::optional<u64>& v) {
    if (v) Number(tag, *v);
  }
  void OptFlag(u16 tag, const std::optional<bool>& v) {
    if (v) Flag(tag, *v);
  }
  std::span<const CatalogValueFieldView> View() const { return std::span<const CatalogValueFieldView>(views).first(size); }
  std::size_t EncodedSize() const {
    std::size_t total = kCatalogValueBlockHeaderBytes;
    for (const auto& v : View()) total += 8 + v.bytes.size();
    return total;
  }
};

// ---- decoding helpers (allocation-free) ----
template <class En>
bool DecodeEnum(const CatalogValueFieldView& f, En& out) {
  const auto v = *f.unsigned_value();
  if (v > std::numeric_limits<u16>::max()) return false;
  out = static_cast<En>(v);
  return true;
}
bool Flag(const CatalogValueFieldView& f) { return f.bytes[0] != 0; }
bool DecodeDecimal(const CatalogValueFieldView& f, double& out) {
  if (f.bytes.size() != 8) return false;
  out = std::bit_cast<double>(platform::LoadLittle64(f.bytes.data()));
  return true;
}
template <class L>
bool DecodeList(const CatalogValueFieldView& f, L& out) {
  if (f.bytes.size() % 2 || f.bytes.size() / 2 > L::capacity) return false;
  out = {};
  for (std::size_t i = 0; i < f.bytes.size(); i += 2)
    out.push(static_cast<typename decltype(out.codes)::value_type>(
        static_cast<u16>(f.bytes[i] | (static_cast<u16>(f.bytes[i + 1]) << 8))));
  return true;
}
std::span<const byte> Bytes(std::string_view b) { return {reinterpret_cast<const byte*>(b.data()), b.size()}; }

// ---- boundary text parsing (the spellings the current code writes) ----
bool ParseU64(std::string_view s, u64& out) {
  if (s.empty() || s.front() < '0' || s.front() > '9') return false;
  const auto parsed = std::from_chars(s.data(), s.data() + s.size(), out);
  return parsed.ec == std::errc{} && parsed.ptr == s.data() + s.size();
}
bool ParseFlag01(std::string_view s, bool& out) {  // top-level policy flags
  if (s != "0" && s != "1") return false;
  out = s == "1";
  return true;
}
bool ParseFlagText(std::string_view s, bool& out) {  // config.* booleans
  if (s != "true" && s != "false") return false;
  out = s == "true";
  return true;
}
bool ParseDecimal(std::string_view s, double& out) {
  if (s.empty()) return false;
  const auto parsed = std::from_chars(s.data(), s.data() + s.size(), out);
  return parsed.ec == std::errc{} && parsed.ptr == s.data() + s.size();
}
bool ParseOptU64(std::string_view s, std::optional<u64>& out) {
  u64 v = 0;
  if (!ParseU64(s, v)) return false;
  out = v;
  return true;
}
bool ParseOptFlagText(std::string_view s, std::optional<bool>& out) {
  bool v = false;
  if (!ParseFlagText(s, v)) return false;
  out = v;
  return true;
}
template <class En>
bool ParseEnum(std::string_view s, std::optional<En> (*for_tag)(std::string_view) noexcept, En& out) {
  const auto v = for_tag(s);
  if (!v) return false;
  out = *v;
  return true;
}
// Comma-separated, as the native policy reader's Tokens: empty text is an
// empty list; no empty element, no trailing comma.
template <class L, class En>
bool ParseList(std::string_view s, std::optional<En> (*for_tag)(std::string_view) noexcept, L& out) {
  out = {};
  if (s.empty()) return true;
  if (s.back() == ',') return false;
  while (true) {
    const auto end = s.find(',');
    const auto token = s.substr(0, end);
    const auto v = for_tag(token);
    if (token.empty() || !v || !out.push(*v)) return false;
    if (end == std::string_view::npos) return true;
    s.remove_prefix(end + 1);
  }
}

// Tags 1..3 of every config record.
#define SB_NATIVE_AGENT_CONFIG_HEAD_WIRE                                                             \
  {1, T::engine_identity, true, 16, kObj}, {2, T::engine_identity, true, 16, UuidKind::transaction}, \
      {3, T::unsigned_integer, true, 8}

// ---- GENERATED: codec (gen_native_agent_config.py); never edit by hand ----
// Inputs: agent_policy_schema.cpp sha256 0ff5471aefedc938047b42a0b0b5ac97d5238dbeeda2be89a0016c756e4bef28
//         catalog_agent_constants.hpp sha256 60641899040a89bfa385a6da5a52cd83e85fea7fe7713083cfcb48c7efdd4763
#define SB_NATIVE_AGENT_CONFIG_COMMON_WIRE \
  {64, T::boolean, false, 1}, \
  {65, T::unsigned_integer, false, 8}, \
  {66, T::unsigned_integer, false, 8}, \
  {67, T::unsigned_integer, false, 8}, \
  {68, T::unsigned_integer, false, 8}, \
  {69, T::unsigned_integer, false, 8}, \
  {70, T::unsigned_integer, false, 8}, \
  {71, T::unsigned_integer, false, 8}, \
  {72, T::unsigned_integer, false, 8}, \
  {73, T::unsigned_integer, false, 8}, \
  {74, T::unsigned_integer, false, 8}, \
  {75, T::unsigned_integer, false, 8}, \
  {76, T::unsigned_integer, false, 8}
constexpr NativeAgentConfigFieldDescriptor kCommonFields[]{
    {"protect_foreground_work", 64, NativeAgentConfigSourceType::boolean, NativeAgentConfigValueKind::boolean, false, false, false, 0.0, 0.0},
    {"max_cpu_time_microseconds", 65, NativeAgentConfigSourceType::duration_microseconds, NativeAgentConfigValueKind::unsigned_integer, false, true, false, 1.0, 0.0},
    {"max_memory_bytes", 66, NativeAgentConfigSourceType::byte_count, NativeAgentConfigValueKind::unsigned_integer, false, true, false, 1.0, 0.0},
    {"max_io_bytes", 67, NativeAgentConfigSourceType::byte_count, NativeAgentConfigValueKind::unsigned_integer, false, true, false, 1.0, 0.0},
    {"max_io_ops", 68, NativeAgentConfigSourceType::unsigned_integer, NativeAgentConfigValueKind::unsigned_integer, false, true, false, 0.0, 0.0},
    {"max_thread_slots", 69, NativeAgentConfigSourceType::unsigned_integer, NativeAgentConfigValueKind::unsigned_integer, false, true, false, 0.0, 0.0},
    {"max_queue_depth", 70, NativeAgentConfigSourceType::unsigned_integer, NativeAgentConfigValueKind::unsigned_integer, false, true, false, 0.0, 0.0},
    {"min_run_interval_microseconds", 71, NativeAgentConfigSourceType::duration_microseconds, NativeAgentConfigValueKind::unsigned_integer, false, true, false, 1.0, 0.0},
    {"retry_backoff_microseconds", 72, NativeAgentConfigSourceType::duration_microseconds, NativeAgentConfigValueKind::unsigned_integer, false, true, false, 1.0, 0.0},
    {"watchdog_timeout_microseconds", 73, NativeAgentConfigSourceType::duration_microseconds, NativeAgentConfigValueKind::unsigned_integer, false, true, false, 1.0, 0.0},
    {"max_history_query_rows", 74, NativeAgentConfigSourceType::unsigned_integer, NativeAgentConfigValueKind::unsigned_integer, false, true, false, 0.0, 0.0},
    {"max_evidence_fanout", 75, NativeAgentConfigSourceType::unsigned_integer, NativeAgentConfigValueKind::unsigned_integer, false, true, false, 0.0, 0.0},
    {"max_label_cardinality", 76, NativeAgentConfigSourceType::unsigned_integer, NativeAgentConfigValueKind::unsigned_integer, false, true, false, 0.0, 0.0},
};
void FillCommon(Fields& f, const NativeAgentConfigResourceBudget& r) {
  f.OptFlag(64, r.protect_foreground_work);
  f.OptNumber(65, r.max_cpu_time_microseconds);
  f.OptNumber(66, r.max_memory_bytes);
  f.OptNumber(67, r.max_io_bytes);
  f.OptNumber(68, r.max_io_ops);
  f.OptNumber(69, r.max_thread_slots);
  f.OptNumber(70, r.max_queue_depth);
  f.OptNumber(71, r.min_run_interval_microseconds);
  f.OptNumber(72, r.retry_backoff_microseconds);
  f.OptNumber(73, r.watchdog_timeout_microseconds);
  f.OptNumber(74, r.max_history_query_rows);
  f.OptNumber(75, r.max_evidence_fanout);
  f.OptNumber(76, r.max_label_cardinality);
}
bool DecodeCommon(const CatalogValueFieldView& v, NativeAgentConfigResourceBudget& r) {
  switch (v.id) {
    case 64: r.protect_foreground_work = Flag(v); return true;
    case 65: r.max_cpu_time_microseconds = *v.unsigned_value(); return true;
    case 66: r.max_memory_bytes = *v.unsigned_value(); return true;
    case 67: r.max_io_bytes = *v.unsigned_value(); return true;
    case 68: r.max_io_ops = *v.unsigned_value(); return true;
    case 69: r.max_thread_slots = *v.unsigned_value(); return true;
    case 70: r.max_queue_depth = *v.unsigned_value(); return true;
    case 71: r.min_run_interval_microseconds = *v.unsigned_value(); return true;
    case 72: r.retry_backoff_microseconds = *v.unsigned_value(); return true;
    case 73: r.watchdog_timeout_microseconds = *v.unsigned_value(); return true;
    case 74: r.max_history_query_rows = *v.unsigned_value(); return true;
    case 75: r.max_evidence_fanout = *v.unsigned_value(); return true;
    case 76: r.max_label_cardinality = *v.unsigned_value(); return true;
    default: return false;
  }
}
bool ValidCommon(const NativeAgentConfigResourceBudget& r) {
  return true &&
         (!r.max_cpu_time_microseconds || *r.max_cpu_time_microseconds >= 1ull) &&
         (!r.max_memory_bytes || *r.max_memory_bytes >= 1ull) &&
         (!r.max_io_bytes || *r.max_io_bytes >= 1ull) &&
         (!r.min_run_interval_microseconds || *r.min_run_interval_microseconds >= 1ull) &&
         (!r.retry_backoff_microseconds || *r.retry_backoff_microseconds >= 1ull) &&
         (!r.watchdog_timeout_microseconds || *r.watchdog_timeout_microseconds >= 1ull);
}
bool AssignCommon(NativeAgentConfigResourceBudget& r, std::size_t index, std::string_view v) {
  switch (index) {
    case 0: return ParseOptFlagText(v, r.protect_foreground_work);
    case 1: return ParseOptU64(v, r.max_cpu_time_microseconds);
    case 2: return ParseOptU64(v, r.max_memory_bytes);
    case 3: return ParseOptU64(v, r.max_io_bytes);
    case 4: return ParseOptU64(v, r.max_io_ops);
    case 5: return ParseOptU64(v, r.max_thread_slots);
    case 6: return ParseOptU64(v, r.max_queue_depth);
    case 7: return ParseOptU64(v, r.min_run_interval_microseconds);
    case 8: return ParseOptU64(v, r.retry_backoff_microseconds);
    case 9: return ParseOptU64(v, r.watchdog_timeout_microseconds);
    case 10: return ParseOptU64(v, r.max_history_query_rows);
    case 11: return ParseOptU64(v, r.max_evidence_fanout);
    case 12: return ParseOptU64(v, r.max_label_cardinality);
    default: return false;
  }
}
// ---- node_resource_policy ----
constexpr CatalogValueFieldSchema kNativeAgentConfig_node_resource_policyWire[]{
    SB_NATIVE_AGENT_CONFIG_HEAD_WIRE,
    {4, T::unsigned_integer, true, 8},  // capability_probe_interval
    {5, T::unsigned_integer, true, 8},  // suitability_score_window
    {6, T::unsigned_integer, true, 8},  // stale_after
    SB_NATIVE_AGENT_CONFIG_COMMON_WIRE};
constexpr CatalogValueSchemaView SchemaOf(const NativeAgentConfig_node_resource_policy*) {
  return {65642, kNativeAgentSchemaVersion, kNativeAgentConfig_node_resource_policyWire};
}
constexpr NativeAgentConfigFieldDescriptor kNativeAgentConfig_node_resource_policyFields[]{
    {"capability_probe_interval", 4, NativeAgentConfigSourceType::duration_seconds, NativeAgentConfigValueKind::unsigned_integer, true, true, false, 1.0, 0.0},
    {"suitability_score_window", 5, NativeAgentConfigSourceType::duration_seconds, NativeAgentConfigValueKind::unsigned_integer, true, true, false, 1.0, 0.0},
    {"stale_after", 6, NativeAgentConfigSourceType::duration_seconds, NativeAgentConfigValueKind::unsigned_integer, true, true, false, 1.0, 0.0},
};
std::span<const NativeAgentConfigFieldDescriptor> FieldsOf(const NativeAgentConfig_node_resource_policy*) { return kNativeAgentConfig_node_resource_policyFields; }
constexpr std::array<std::string_view, 0> kNativeAgentConfig_node_resource_policyUnrepresented{};
std::span<const std::string_view> UnrepresentedOf(const NativeAgentConfig_node_resource_policy*) { return kNativeAgentConfig_node_resource_policyUnrepresented; }
void FillFamily(Fields& f, const NativeAgentConfig_node_resource_policy& r) {
  f.Number(4, r.capability_probe_interval);
  f.Number(5, r.suitability_score_window);
  f.Number(6, r.stale_after);
}
// 1 decoded, 0 not a family tag, -1 refused.
int DecodeFamily(const CatalogValueFieldView& v, NativeAgentConfig_node_resource_policy& r) {
  switch (v.id) {
    case 4: r.capability_probe_interval = *v.unsigned_value(); return 1;
    case 5: r.suitability_score_window = *v.unsigned_value(); return 1;
    case 6: r.stale_after = *v.unsigned_value(); return 1;
    default: return 0;
  }
}
bool ValidFamily(const NativeAgentConfig_node_resource_policy& r) {
  return true &&
         r.capability_probe_interval >= 1ull &&
         r.suitability_score_window >= 1ull &&
         r.stale_after >= 1ull;
}
bool AssignFamily(NativeAgentConfig_node_resource_policy& r, std::size_t index, std::string_view v) {
  switch (index) {
    case 0: return ParseU64(v, r.capability_probe_interval);
    case 1: return ParseU64(v, r.suitability_score_window);
    case 2: return ParseU64(v, r.stale_after);
    default: return false;
  }
}
// ---- metric_registry_policy ----
constexpr CatalogValueFieldSchema kNativeAgentConfig_metric_registry_policyWire[]{
    SB_NATIVE_AGENT_CONFIG_HEAD_WIRE,
    {4, T::unsigned_integer, true, 8},  // schema_version
    {5, T::unsigned_integer, true, 8},  // cardinality_limit
    {6, T::unsigned_integer, true, 8},  // retention_window
    {7, T::unsigned_integer, true, 8},  // rollup_interval
    {8, T::boolean, true, 1},  // reject_on_schema_drift
    SB_NATIVE_AGENT_CONFIG_COMMON_WIRE};
constexpr CatalogValueSchemaView SchemaOf(const NativeAgentConfig_metric_registry_policy*) {
  return {65643, kNativeAgentSchemaVersion, kNativeAgentConfig_metric_registry_policyWire};
}
constexpr NativeAgentConfigFieldDescriptor kNativeAgentConfig_metric_registry_policyFields[]{
    {"schema_version", 4, NativeAgentConfigSourceType::unsigned_integer, NativeAgentConfigValueKind::unsigned_integer, true, true, false, 0.0, 0.0},
    {"cardinality_limit", 5, NativeAgentConfigSourceType::unsigned_integer, NativeAgentConfigValueKind::unsigned_integer, true, true, false, 0.0, 0.0},
    {"retention_window", 6, NativeAgentConfigSourceType::duration_seconds, NativeAgentConfigValueKind::unsigned_integer, true, true, false, 1.0, 0.0},
    {"rollup_interval", 7, NativeAgentConfigSourceType::duration_seconds, NativeAgentConfigValueKind::unsigned_integer, true, true, false, 1.0, 0.0},
    {"reject_on_schema_drift", 8, NativeAgentConfigSourceType::boolean, NativeAgentConfigValueKind::boolean, true, false, false, 0.0, 0.0},
};
std::span<const NativeAgentConfigFieldDescriptor> FieldsOf(const NativeAgentConfig_metric_registry_policy*) { return kNativeAgentConfig_metric_registry_policyFields; }
constexpr std::array<std::string_view, 0> kNativeAgentConfig_metric_registry_policyUnrepresented{};
std::span<const std::string_view> UnrepresentedOf(const NativeAgentConfig_metric_registry_policy*) { return kNativeAgentConfig_metric_registry_policyUnrepresented; }
void FillFamily(Fields& f, const NativeAgentConfig_metric_registry_policy& r) {
  f.Number(4, r.schema_version);
  f.Number(5, r.cardinality_limit);
  f.Number(6, r.retention_window);
  f.Number(7, r.rollup_interval);
  f.Flag(8, r.reject_on_schema_drift);
}
// 1 decoded, 0 not a family tag, -1 refused.
int DecodeFamily(const CatalogValueFieldView& v, NativeAgentConfig_metric_registry_policy& r) {
  switch (v.id) {
    case 4: r.schema_version = *v.unsigned_value(); return 1;
    case 5: r.cardinality_limit = *v.unsigned_value(); return 1;
    case 6: r.retention_window = *v.unsigned_value(); return 1;
    case 7: r.rollup_interval = *v.unsigned_value(); return 1;
    case 8: r.reject_on_schema_drift = Flag(v); return 1;
    default: return 0;
  }
}
bool ValidFamily(const NativeAgentConfig_metric_registry_policy& r) {
  return true &&
         r.retention_window >= 1ull &&
         r.rollup_interval >= 1ull;
}
bool AssignFamily(NativeAgentConfig_metric_registry_policy& r, std::size_t index, std::string_view v) {
  switch (index) {
    case 0: return ParseU64(v, r.schema_version);
    case 1: return ParseU64(v, r.cardinality_limit);
    case 2: return ParseU64(v, r.retention_window);
    case 3: return ParseU64(v, r.rollup_interval);
    case 4: return ParseFlagText(v, r.reject_on_schema_drift);
    default: return false;
  }
}
// ---- storage_health_policy ----
constexpr CatalogValueFieldSchema kNativeAgentConfig_storage_health_policyWire[]{
    SB_NATIVE_AGENT_CONFIG_HEAD_WIRE,
    {4, T::unsigned_integer, true, 8},  // fsync_p99_critical_us
    {5, T::unsigned_integer, true, 8},  // unknown_page_quarantine_threshold
    {6, T::unsigned_integer, true, 8},  // checksum_failure_threshold
    {7, T::unsigned_integer, true, 8},  // optimizer_cost_update_window
    SB_NATIVE_AGENT_CONFIG_COMMON_WIRE};
constexpr CatalogValueSchemaView SchemaOf(const NativeAgentConfig_storage_health_policy*) {
  return {65644, kNativeAgentSchemaVersion, kNativeAgentConfig_storage_health_policyWire};
}
constexpr NativeAgentConfigFieldDescriptor kNativeAgentConfig_storage_health_policyFields[]{
    {"fsync_p99_critical_us", 4, NativeAgentConfigSourceType::duration_microseconds, NativeAgentConfigValueKind::unsigned_integer, true, true, false, 1.0, 0.0},
    {"unknown_page_quarantine_threshold", 5, NativeAgentConfigSourceType::unsigned_integer, NativeAgentConfigValueKind::unsigned_integer, true, true, false, 0.0, 0.0},
    {"checksum_failure_threshold", 6, NativeAgentConfigSourceType::unsigned_integer, NativeAgentConfigValueKind::unsigned_integer, true, true, false, 0.0, 0.0},
    {"optimizer_cost_update_window", 7, NativeAgentConfigSourceType::duration_seconds, NativeAgentConfigValueKind::unsigned_integer, true, true, false, 1.0, 0.0},
};
std::span<const NativeAgentConfigFieldDescriptor> FieldsOf(const NativeAgentConfig_storage_health_policy*) { return kNativeAgentConfig_storage_health_policyFields; }
constexpr std::array<std::string_view, 0> kNativeAgentConfig_storage_health_policyUnrepresented{};
std::span<const std::string_view> UnrepresentedOf(const NativeAgentConfig_storage_health_policy*) { return kNativeAgentConfig_storage_health_policyUnrepresented; }
void FillFamily(Fields& f, const NativeAgentConfig_storage_health_policy& r) {
  f.Number(4, r.fsync_p99_critical_us);
  f.Number(5, r.unknown_page_quarantine_threshold);
  f.Number(6, r.checksum_failure_threshold);
  f.Number(7, r.optimizer_cost_update_window);
}
// 1 decoded, 0 not a family tag, -1 refused.
int DecodeFamily(const CatalogValueFieldView& v, NativeAgentConfig_storage_health_policy& r) {
  switch (v.id) {
    case 4: r.fsync_p99_critical_us = *v.unsigned_value(); return 1;
    case 5: r.unknown_page_quarantine_threshold = *v.unsigned_value(); return 1;
    case 6: r.checksum_failure_threshold = *v.unsigned_value(); return 1;
    case 7: r.optimizer_cost_update_window = *v.unsigned_value(); return 1;
    default: return 0;
  }
}
bool ValidFamily(const NativeAgentConfig_storage_health_policy& r) {
  return true &&
         r.fsync_p99_critical_us >= 1ull &&
         r.optimizer_cost_update_window >= 1ull;
}
bool AssignFamily(NativeAgentConfig_storage_health_policy& r, std::size_t index, std::string_view v) {
  switch (index) {
    case 0: return ParseU64(v, r.fsync_p99_critical_us);
    case 1: return ParseU64(v, r.unknown_page_quarantine_threshold);
    case 2: return ParseU64(v, r.checksum_failure_threshold);
    case 3: return ParseU64(v, r.optimizer_cost_update_window);
    default: return false;
  }
}
// ---- memory_governor_policy ----
constexpr CatalogValueFieldSchema kNativeAgentConfig_memory_governor_policyWire[]{
    SB_NATIVE_AGENT_CONFIG_HEAD_WIRE,
    {4, T::unsigned_integer, true, 8},  // emergency_reserve_percent
    {5, T::unsigned_integer, true, 8},  // allocation_failure_pressure
    {6, T::unsigned_integer, true, 8},  // spill_threshold
    {7, T::unsigned_integer, true, 8},  // cache_shrink_limit
    SB_NATIVE_AGENT_CONFIG_COMMON_WIRE};
constexpr CatalogValueSchemaView SchemaOf(const NativeAgentConfig_memory_governor_policy*) {
  return {65645, kNativeAgentSchemaVersion, kNativeAgentConfig_memory_governor_policyWire};
}
constexpr NativeAgentConfigFieldDescriptor kNativeAgentConfig_memory_governor_policyFields[]{
    {"emergency_reserve_percent", 4, NativeAgentConfigSourceType::percent, NativeAgentConfigValueKind::unsigned_integer, true, true, true, 0.0, 100.0},
    {"allocation_failure_pressure", 5, NativeAgentConfigSourceType::unsigned_integer, NativeAgentConfigValueKind::unsigned_integer, true, true, false, 0.0, 0.0},
    {"spill_threshold", 6, NativeAgentConfigSourceType::percent, NativeAgentConfigValueKind::unsigned_integer, true, true, true, 0.0, 100.0},
    {"cache_shrink_limit", 7, NativeAgentConfigSourceType::percent, NativeAgentConfigValueKind::unsigned_integer, true, true, true, 0.0, 100.0},
};
std::span<const NativeAgentConfigFieldDescriptor> FieldsOf(const NativeAgentConfig_memory_governor_policy*) { return kNativeAgentConfig_memory_governor_policyFields; }
constexpr std::array<std::string_view, 0> kNativeAgentConfig_memory_governor_policyUnrepresented{};
std::span<const std::string_view> UnrepresentedOf(const NativeAgentConfig_memory_governor_policy*) { return kNativeAgentConfig_memory_governor_policyUnrepresented; }
void FillFamily(Fields& f, const NativeAgentConfig_memory_governor_policy& r) {
  f.Number(4, r.emergency_reserve_percent);
  f.Number(5, r.allocation_failure_pressure);
  f.Number(6, r.spill_threshold);
  f.Number(7, r.cache_shrink_limit);
}
// 1 decoded, 0 not a family tag, -1 refused.
int DecodeFamily(const CatalogValueFieldView& v, NativeAgentConfig_memory_governor_policy& r) {
  switch (v.id) {
    case 4: r.emergency_reserve_percent = *v.unsigned_value(); return 1;
    case 5: r.allocation_failure_pressure = *v.unsigned_value(); return 1;
    case 6: r.spill_threshold = *v.unsigned_value(); return 1;
    case 7: r.cache_shrink_limit = *v.unsigned_value(); return 1;
    default: return 0;
  }
}
bool ValidFamily(const NativeAgentConfig_memory_governor_policy& r) {
  return true &&
         r.emergency_reserve_percent <= 100ull &&
         r.spill_threshold <= 100ull &&
         r.cache_shrink_limit <= 100ull;
}
bool AssignFamily(NativeAgentConfig_memory_governor_policy& r, std::size_t index, std::string_view v) {
  switch (index) {
    case 0: return ParseU64(v, r.emergency_reserve_percent);
    case 1: return ParseU64(v, r.allocation_failure_pressure);
    case 2: return ParseU64(v, r.spill_threshold);
    case 3: return ParseU64(v, r.cache_shrink_limit);
    default: return false;
  }
}
// ---- index_health_policy ----
constexpr CatalogValueFieldSchema kNativeAgentConfig_index_health_policyWire[]{
    SB_NATIVE_AGENT_CONFIG_HEAD_WIRE,
    {4, T::unsigned_integer, true, 8},  // rebuild_threshold
    {5, T::unsigned_integer, true, 8},  // split_pressure_threshold
    {6, T::unsigned_integer, true, 8},  // unused_index_window
    {7, T::unsigned_integer, true, 8},  // recommendation_cooldown
    SB_NATIVE_AGENT_CONFIG_COMMON_WIRE};
constexpr CatalogValueSchemaView SchemaOf(const NativeAgentConfig_index_health_policy*) {
  return {65646, kNativeAgentSchemaVersion, kNativeAgentConfig_index_health_policyWire};
}
constexpr NativeAgentConfigFieldDescriptor kNativeAgentConfig_index_health_policyFields[]{
    {"rebuild_threshold", 4, NativeAgentConfigSourceType::unsigned_integer, NativeAgentConfigValueKind::unsigned_integer, true, true, false, 0.0, 0.0},
    {"split_pressure_threshold", 5, NativeAgentConfigSourceType::unsigned_integer, NativeAgentConfigValueKind::unsigned_integer, true, true, false, 0.0, 0.0},
    {"unused_index_window", 6, NativeAgentConfigSourceType::duration_seconds, NativeAgentConfigValueKind::unsigned_integer, true, true, false, 1.0, 0.0},
    {"recommendation_cooldown", 7, NativeAgentConfigSourceType::duration_seconds, NativeAgentConfigValueKind::unsigned_integer, true, true, false, 1.0, 0.0},
};
std::span<const NativeAgentConfigFieldDescriptor> FieldsOf(const NativeAgentConfig_index_health_policy*) { return kNativeAgentConfig_index_health_policyFields; }
constexpr std::array<std::string_view, 0> kNativeAgentConfig_index_health_policyUnrepresented{};
std::span<const std::string_view> UnrepresentedOf(const NativeAgentConfig_index_health_policy*) { return kNativeAgentConfig_index_health_policyUnrepresented; }
void FillFamily(Fields& f, const NativeAgentConfig_index_health_policy& r) {
  f.Number(4, r.rebuild_threshold);
  f.Number(5, r.split_pressure_threshold);
  f.Number(6, r.unused_index_window);
  f.Number(7, r.recommendation_cooldown);
}
// 1 decoded, 0 not a family tag, -1 refused.
int DecodeFamily(const CatalogValueFieldView& v, NativeAgentConfig_index_health_policy& r) {
  switch (v.id) {
    case 4: r.rebuild_threshold = *v.unsigned_value(); return 1;
    case 5: r.split_pressure_threshold = *v.unsigned_value(); return 1;
    case 6: r.unused_index_window = *v.unsigned_value(); return 1;
    case 7: r.recommendation_cooldown = *v.unsigned_value(); return 1;
    default: return 0;
  }
}
bool ValidFamily(const NativeAgentConfig_index_health_policy& r) {
  return true &&
         r.unused_index_window >= 1ull &&
         r.recommendation_cooldown >= 1ull;
}
bool AssignFamily(NativeAgentConfig_index_health_policy& r, std::size_t index, std::string_view v) {
  switch (index) {
    case 0: return ParseU64(v, r.rebuild_threshold);
    case 1: return ParseU64(v, r.split_pressure_threshold);
    case 2: return ParseU64(v, r.unused_index_window);
    case 3: return ParseU64(v, r.recommendation_cooldown);
    default: return false;
  }
}
// ---- cluster_autoscale_policy ----
constexpr CatalogValueFieldSchema kNativeAgentConfig_cluster_autoscale_policyWire[]{
    SB_NATIVE_AGENT_CONFIG_HEAD_WIRE,
    {4, T::unsigned_integer, true, 8},  // scale_up_threshold
    {5, T::unsigned_integer, true, 8},  // scale_down_threshold
    {6, T::unsigned_integer, true, 8},  // cooldown_seconds
    {7, T::boolean, true, 1},  // no_scale_down_when_limbo
    SB_NATIVE_AGENT_CONFIG_COMMON_WIRE};
constexpr CatalogValueSchemaView SchemaOf(const NativeAgentConfig_cluster_autoscale_policy*) {
  return {65647, kNativeAgentSchemaVersion, kNativeAgentConfig_cluster_autoscale_policyWire};
}
constexpr NativeAgentConfigFieldDescriptor kNativeAgentConfig_cluster_autoscale_policyFields[]{
    {"scale_up_threshold", 4, NativeAgentConfigSourceType::percent, NativeAgentConfigValueKind::unsigned_integer, true, true, true, 0.0, 100.0},
    {"scale_down_threshold", 5, NativeAgentConfigSourceType::percent, NativeAgentConfigValueKind::unsigned_integer, true, true, true, 0.0, 100.0},
    {"cooldown_seconds", 6, NativeAgentConfigSourceType::duration_seconds, NativeAgentConfigValueKind::unsigned_integer, true, true, false, 1.0, 0.0},
    {"no_scale_down_when_limbo", 7, NativeAgentConfigSourceType::boolean, NativeAgentConfigValueKind::boolean, true, false, false, 0.0, 0.0},
};
std::span<const NativeAgentConfigFieldDescriptor> FieldsOf(const NativeAgentConfig_cluster_autoscale_policy*) { return kNativeAgentConfig_cluster_autoscale_policyFields; }
constexpr std::array<std::string_view, 0> kNativeAgentConfig_cluster_autoscale_policyUnrepresented{};
std::span<const std::string_view> UnrepresentedOf(const NativeAgentConfig_cluster_autoscale_policy*) { return kNativeAgentConfig_cluster_autoscale_policyUnrepresented; }
void FillFamily(Fields& f, const NativeAgentConfig_cluster_autoscale_policy& r) {
  f.Number(4, r.scale_up_threshold);
  f.Number(5, r.scale_down_threshold);
  f.Number(6, r.cooldown_seconds);
  f.Flag(7, r.no_scale_down_when_limbo);
}
// 1 decoded, 0 not a family tag, -1 refused.
int DecodeFamily(const CatalogValueFieldView& v, NativeAgentConfig_cluster_autoscale_policy& r) {
  switch (v.id) {
    case 4: r.scale_up_threshold = *v.unsigned_value(); return 1;
    case 5: r.scale_down_threshold = *v.unsigned_value(); return 1;
    case 6: r.cooldown_seconds = *v.unsigned_value(); return 1;
    case 7: r.no_scale_down_when_limbo = Flag(v); return 1;
    default: return 0;
  }
}
bool ValidFamily(const NativeAgentConfig_cluster_autoscale_policy& r) {
  return true &&
         r.scale_up_threshold <= 100ull &&
         r.scale_down_threshold <= 100ull &&
         r.cooldown_seconds >= 1ull;
}
bool AssignFamily(NativeAgentConfig_cluster_autoscale_policy& r, std::size_t index, std::string_view v) {
  switch (index) {
    case 0: return ParseU64(v, r.scale_up_threshold);
    case 1: return ParseU64(v, r.scale_down_threshold);
    case 2: return ParseU64(v, r.cooldown_seconds);
    case 3: return ParseFlagText(v, r.no_scale_down_when_limbo);
    default: return false;
  }
}
// ---- admission_control_policy ----
constexpr CatalogValueFieldSchema kNativeAgentConfig_admission_control_policyWire[]{
    SB_NATIVE_AGENT_CONFIG_HEAD_WIRE,
    {4, T::unsigned_integer, true, 8},  // listener_queue_threshold
    {5, T::unsigned_integer, true, 8},  // scheduler_queue_threshold
    {6, T::unsigned_integer, true, 8},  // hard_limit_threshold
    SB_NATIVE_AGENT_CONFIG_COMMON_WIRE};
constexpr CatalogValueSchemaView SchemaOf(const NativeAgentConfig_admission_control_policy*) {
  return {65648, kNativeAgentSchemaVersion, kNativeAgentConfig_admission_control_policyWire};
}
constexpr NativeAgentConfigFieldDescriptor kNativeAgentConfig_admission_control_policyFields[]{
    {"listener_queue_threshold", 4, NativeAgentConfigSourceType::unsigned_integer, NativeAgentConfigValueKind::unsigned_integer, true, true, false, 0.0, 0.0},
    {"scheduler_queue_threshold", 5, NativeAgentConfigSourceType::unsigned_integer, NativeAgentConfigValueKind::unsigned_integer, true, true, false, 0.0, 0.0},
    {"hard_limit_threshold", 6, NativeAgentConfigSourceType::unsigned_integer, NativeAgentConfigValueKind::unsigned_integer, true, true, false, 0.0, 0.0},
    {"workload_class_rules", 7, NativeAgentConfigSourceType::token_list, NativeAgentConfigValueKind::unrepresented, true, false, false, 0.0, 0.0},
};
std::span<const NativeAgentConfigFieldDescriptor> FieldsOf(const NativeAgentConfig_admission_control_policy*) { return kNativeAgentConfig_admission_control_policyFields; }
constexpr std::array<std::string_view, 1> kNativeAgentConfig_admission_control_policyUnrepresented{"workload_class_rules"};
std::span<const std::string_view> UnrepresentedOf(const NativeAgentConfig_admission_control_policy*) { return kNativeAgentConfig_admission_control_policyUnrepresented; }
void FillFamily(Fields& f, const NativeAgentConfig_admission_control_policy& r) {
  f.Number(4, r.listener_queue_threshold);
  f.Number(5, r.scheduler_queue_threshold);
  f.Number(6, r.hard_limit_threshold);
}
// 1 decoded, 0 not a family tag, -1 refused.
int DecodeFamily(const CatalogValueFieldView& v, NativeAgentConfig_admission_control_policy& r) {
  switch (v.id) {
    case 4: r.listener_queue_threshold = *v.unsigned_value(); return 1;
    case 5: r.scheduler_queue_threshold = *v.unsigned_value(); return 1;
    case 6: r.hard_limit_threshold = *v.unsigned_value(); return 1;
    default: return 0;
  }
}
bool ValidFamily(const NativeAgentConfig_admission_control_policy& r) {
  return true;
}
bool AssignFamily(NativeAgentConfig_admission_control_policy& r, std::size_t index, std::string_view v) {
  switch (index) {
    case 0: return ParseU64(v, r.listener_queue_threshold);
    case 1: return ParseU64(v, r.scheduler_queue_threshold);
    case 2: return ParseU64(v, r.hard_limit_threshold);
    case 3: return !v.empty();  // accepted, NOT carried
    default: return false;
  }
}
// ---- parser_health_policy ----
constexpr CatalogValueFieldSchema kNativeAgentConfig_parser_health_policyWire[]{
    SB_NATIVE_AGENT_CONFIG_HEAD_WIRE,
    {4, T::unsigned_integer, true, 8},  // parser_crash_quarantine_threshold
    {5, T::unsigned_integer, true, 8},  // drain_cooldown
    {7, T::unsigned_integer, true, 8},  // policy_attach_latency_warn_us
    SB_NATIVE_AGENT_CONFIG_COMMON_WIRE};
constexpr CatalogValueSchemaView SchemaOf(const NativeAgentConfig_parser_health_policy*) {
  return {65649, kNativeAgentSchemaVersion, kNativeAgentConfig_parser_health_policyWire};
}
constexpr NativeAgentConfigFieldDescriptor kNativeAgentConfig_parser_health_policyFields[]{
    {"parser_crash_quarantine_threshold", 4, NativeAgentConfigSourceType::unsigned_integer, NativeAgentConfigValueKind::unsigned_integer, true, true, false, 0.0, 0.0},
    {"drain_cooldown", 5, NativeAgentConfigSourceType::duration_seconds, NativeAgentConfigValueKind::unsigned_integer, true, true, false, 1.0, 0.0},
    {"package_quarantine_rules", 6, NativeAgentConfigSourceType::token_list, NativeAgentConfigValueKind::unrepresented, true, false, false, 0.0, 0.0},
    {"policy_attach_latency_warn_us", 7, NativeAgentConfigSourceType::duration_microseconds, NativeAgentConfigValueKind::unsigned_integer, true, true, false, 1.0, 0.0},
};
std::span<const NativeAgentConfigFieldDescriptor> FieldsOf(const NativeAgentConfig_parser_health_policy*) { return kNativeAgentConfig_parser_health_policyFields; }
constexpr std::array<std::string_view, 1> kNativeAgentConfig_parser_health_policyUnrepresented{"package_quarantine_rules"};
std::span<const std::string_view> UnrepresentedOf(const NativeAgentConfig_parser_health_policy*) { return kNativeAgentConfig_parser_health_policyUnrepresented; }
void FillFamily(Fields& f, const NativeAgentConfig_parser_health_policy& r) {
  f.Number(4, r.parser_crash_quarantine_threshold);
  f.Number(5, r.drain_cooldown);
  f.Number(7, r.policy_attach_latency_warn_us);
}
// 1 decoded, 0 not a family tag, -1 refused.
int DecodeFamily(const CatalogValueFieldView& v, NativeAgentConfig_parser_health_policy& r) {
  switch (v.id) {
    case 4: r.parser_crash_quarantine_threshold = *v.unsigned_value(); return 1;
    case 5: r.drain_cooldown = *v.unsigned_value(); return 1;
    case 7: r.policy_attach_latency_warn_us = *v.unsigned_value(); return 1;
    default: return 0;
  }
}
bool ValidFamily(const NativeAgentConfig_parser_health_policy& r) {
  return true &&
         r.drain_cooldown >= 1ull &&
         r.policy_attach_latency_warn_us >= 1ull;
}
bool AssignFamily(NativeAgentConfig_parser_health_policy& r, std::size_t index, std::string_view v) {
  switch (index) {
    case 0: return ParseU64(v, r.parser_crash_quarantine_threshold);
    case 1: return ParseU64(v, r.drain_cooldown);
    case 2: return !v.empty();  // accepted, NOT carried
    case 3: return ParseU64(v, r.policy_attach_latency_warn_us);
    default: return false;
  }
}
// ---- long_transaction_policy ----
constexpr CatalogValueFieldSchema kNativeAgentConfig_long_transaction_policyWire[]{
    SB_NATIVE_AGENT_CONFIG_HEAD_WIRE,
    {4, T::unsigned_integer, true, 8},  // oldest_snapshot_pressure_seconds
    {5, T::unsigned_integer, true, 8},  // idle_warning_seconds
    {6, T::unsigned_integer, true, 8},  // blocker_escalation_seconds
    {7, T::boolean, true, 1},  // cancel_requires_approval
    SB_NATIVE_AGENT_CONFIG_COMMON_WIRE};
constexpr CatalogValueSchemaView SchemaOf(const NativeAgentConfig_long_transaction_policy*) {
  return {65650, kNativeAgentSchemaVersion, kNativeAgentConfig_long_transaction_policyWire};
}
constexpr NativeAgentConfigFieldDescriptor kNativeAgentConfig_long_transaction_policyFields[]{
    {"oldest_snapshot_pressure_seconds", 4, NativeAgentConfigSourceType::duration_seconds, NativeAgentConfigValueKind::unsigned_integer, true, true, false, 1.0, 0.0},
    {"idle_warning_seconds", 5, NativeAgentConfigSourceType::duration_seconds, NativeAgentConfigValueKind::unsigned_integer, true, true, false, 1.0, 0.0},
    {"blocker_escalation_seconds", 6, NativeAgentConfigSourceType::duration_seconds, NativeAgentConfigValueKind::unsigned_integer, true, true, false, 1.0, 0.0},
    {"cancel_requires_approval", 7, NativeAgentConfigSourceType::boolean, NativeAgentConfigValueKind::boolean, true, false, false, 0.0, 0.0},
};
std::span<const NativeAgentConfigFieldDescriptor> FieldsOf(const NativeAgentConfig_long_transaction_policy*) { return kNativeAgentConfig_long_transaction_policyFields; }
constexpr std::array<std::string_view, 0> kNativeAgentConfig_long_transaction_policyUnrepresented{};
std::span<const std::string_view> UnrepresentedOf(const NativeAgentConfig_long_transaction_policy*) { return kNativeAgentConfig_long_transaction_policyUnrepresented; }
void FillFamily(Fields& f, const NativeAgentConfig_long_transaction_policy& r) {
  f.Number(4, r.oldest_snapshot_pressure_seconds);
  f.Number(5, r.idle_warning_seconds);
  f.Number(6, r.blocker_escalation_seconds);
  f.Flag(7, r.cancel_requires_approval);
}
// 1 decoded, 0 not a family tag, -1 refused.
int DecodeFamily(const CatalogValueFieldView& v, NativeAgentConfig_long_transaction_policy& r) {
  switch (v.id) {
    case 4: r.oldest_snapshot_pressure_seconds = *v.unsigned_value(); return 1;
    case 5: r.idle_warning_seconds = *v.unsigned_value(); return 1;
    case 6: r.blocker_escalation_seconds = *v.unsigned_value(); return 1;
    case 7: r.cancel_requires_approval = Flag(v); return 1;
    default: return 0;
  }
}
bool ValidFamily(const NativeAgentConfig_long_transaction_policy& r) {
  return true &&
         r.oldest_snapshot_pressure_seconds >= 1ull &&
         r.idle_warning_seconds >= 1ull &&
         r.blocker_escalation_seconds >= 1ull;
}
bool AssignFamily(NativeAgentConfig_long_transaction_policy& r, std::size_t index, std::string_view v) {
  switch (index) {
    case 0: return ParseU64(v, r.oldest_snapshot_pressure_seconds);
    case 1: return ParseU64(v, r.idle_warning_seconds);
    case 2: return ParseU64(v, r.blocker_escalation_seconds);
    case 3: return ParseFlagText(v, r.cancel_requires_approval);
    default: return false;
  }
}
// ---- storage_version_cleanup_policy ----
constexpr CatalogValueFieldSchema kNativeAgentConfig_storage_version_cleanup_policyWire[]{
    SB_NATIVE_AGENT_CONFIG_HEAD_WIRE,
    {4, T::boolean, true, 1},  // authoritative_cleanup_horizon_required
    {5, T::unsigned_integer, true, 8},  // cleanup_horizon
    {6, T::unsigned_integer, true, 8},  // max_versions_per_batch
    {7, T::unsigned_integer, true, 8},  // retained_row_pressure
    {8, T::unsigned_integer, true, 8},  // active_blocker_threshold
    SB_NATIVE_AGENT_CONFIG_COMMON_WIRE};
constexpr CatalogValueSchemaView SchemaOf(const NativeAgentConfig_storage_version_cleanup_policy*) {
  return {65651, kNativeAgentSchemaVersion, kNativeAgentConfig_storage_version_cleanup_policyWire};
}
constexpr NativeAgentConfigFieldDescriptor kNativeAgentConfig_storage_version_cleanup_policyFields[]{
    {"authoritative_cleanup_horizon_required", 4, NativeAgentConfigSourceType::boolean, NativeAgentConfigValueKind::boolean, true, false, false, 0.0, 0.0},
    {"cleanup_horizon", 5, NativeAgentConfigSourceType::duration_seconds, NativeAgentConfigValueKind::unsigned_integer, true, true, false, 1.0, 0.0},
    {"max_versions_per_batch", 6, NativeAgentConfigSourceType::unsigned_integer, NativeAgentConfigValueKind::unsigned_integer, true, true, false, 0.0, 0.0},
    {"retained_row_pressure", 7, NativeAgentConfigSourceType::unsigned_integer, NativeAgentConfigValueKind::unsigned_integer, true, true, false, 0.0, 0.0},
    {"active_blocker_threshold", 8, NativeAgentConfigSourceType::unsigned_integer, NativeAgentConfigValueKind::unsigned_integer, true, true, false, 0.0, 0.0},
};
std::span<const NativeAgentConfigFieldDescriptor> FieldsOf(const NativeAgentConfig_storage_version_cleanup_policy*) { return kNativeAgentConfig_storage_version_cleanup_policyFields; }
constexpr std::array<std::string_view, 0> kNativeAgentConfig_storage_version_cleanup_policyUnrepresented{};
std::span<const std::string_view> UnrepresentedOf(const NativeAgentConfig_storage_version_cleanup_policy*) { return kNativeAgentConfig_storage_version_cleanup_policyUnrepresented; }
void FillFamily(Fields& f, const NativeAgentConfig_storage_version_cleanup_policy& r) {
  f.Flag(4, r.authoritative_cleanup_horizon_required);
  f.Number(5, r.cleanup_horizon);
  f.Number(6, r.max_versions_per_batch);
  f.Number(7, r.retained_row_pressure);
  f.Number(8, r.active_blocker_threshold);
}
// 1 decoded, 0 not a family tag, -1 refused.
int DecodeFamily(const CatalogValueFieldView& v, NativeAgentConfig_storage_version_cleanup_policy& r) {
  switch (v.id) {
    case 4: r.authoritative_cleanup_horizon_required = Flag(v); return 1;
    case 5: r.cleanup_horizon = *v.unsigned_value(); return 1;
    case 6: r.max_versions_per_batch = *v.unsigned_value(); return 1;
    case 7: r.retained_row_pressure = *v.unsigned_value(); return 1;
    case 8: r.active_blocker_threshold = *v.unsigned_value(); return 1;
    default: return 0;
  }
}
bool ValidFamily(const NativeAgentConfig_storage_version_cleanup_policy& r) {
  return true &&
         r.cleanup_horizon >= 1ull;
}
bool AssignFamily(NativeAgentConfig_storage_version_cleanup_policy& r, std::size_t index, std::string_view v) {
  switch (index) {
    case 0: return ParseFlagText(v, r.authoritative_cleanup_horizon_required);
    case 1: return ParseU64(v, r.cleanup_horizon);
    case 2: return ParseU64(v, r.max_versions_per_batch);
    case 3: return ParseU64(v, r.retained_row_pressure);
    case 4: return ParseU64(v, r.active_blocker_threshold);
    default: return false;
  }
}
// ---- cleanup_archive_policy ----
constexpr CatalogValueFieldSchema kNativeAgentConfig_cleanup_archive_policyWire[]{
    SB_NATIVE_AGENT_CONFIG_HEAD_WIRE,
    {4, T::unsigned_integer, true, 8},  // cleanup_blocked_threshold
    {5, T::unsigned_integer, true, 8},  // archive_slice_max_age_us
    {6, T::boolean, true, 1},  // authoritative_lwm_required
    {7, T::boolean, true, 1},  // verify_before_release
    SB_NATIVE_AGENT_CONFIG_COMMON_WIRE};
constexpr CatalogValueSchemaView SchemaOf(const NativeAgentConfig_cleanup_archive_policy*) {
  return {65652, kNativeAgentSchemaVersion, kNativeAgentConfig_cleanup_archive_policyWire};
}
constexpr NativeAgentConfigFieldDescriptor kNativeAgentConfig_cleanup_archive_policyFields[]{
    {"cleanup_blocked_threshold", 4, NativeAgentConfigSourceType::unsigned_integer, NativeAgentConfigValueKind::unsigned_integer, true, true, false, 0.0, 0.0},
    {"archive_slice_max_age_us", 5, NativeAgentConfigSourceType::duration_microseconds, NativeAgentConfigValueKind::unsigned_integer, true, true, false, 1.0, 0.0},
    {"authoritative_lwm_required", 6, NativeAgentConfigSourceType::boolean, NativeAgentConfigValueKind::boolean, true, false, false, 0.0, 0.0},
    {"verify_before_release", 7, NativeAgentConfigSourceType::boolean, NativeAgentConfigValueKind::boolean, true, false, false, 0.0, 0.0},
};
std::span<const NativeAgentConfigFieldDescriptor> FieldsOf(const NativeAgentConfig_cleanup_archive_policy*) { return kNativeAgentConfig_cleanup_archive_policyFields; }
constexpr std::array<std::string_view, 0> kNativeAgentConfig_cleanup_archive_policyUnrepresented{};
std::span<const std::string_view> UnrepresentedOf(const NativeAgentConfig_cleanup_archive_policy*) { return kNativeAgentConfig_cleanup_archive_policyUnrepresented; }
void FillFamily(Fields& f, const NativeAgentConfig_cleanup_archive_policy& r) {
  f.Number(4, r.cleanup_blocked_threshold);
  f.Number(5, r.archive_slice_max_age_us);
  f.Flag(6, r.authoritative_lwm_required);
  f.Flag(7, r.verify_before_release);
}
// 1 decoded, 0 not a family tag, -1 refused.
int DecodeFamily(const CatalogValueFieldView& v, NativeAgentConfig_cleanup_archive_policy& r) {
  switch (v.id) {
    case 4: r.cleanup_blocked_threshold = *v.unsigned_value(); return 1;
    case 5: r.archive_slice_max_age_us = *v.unsigned_value(); return 1;
    case 6: r.authoritative_lwm_required = Flag(v); return 1;
    case 7: r.verify_before_release = Flag(v); return 1;
    default: return 0;
  }
}
bool ValidFamily(const NativeAgentConfig_cleanup_archive_policy& r) {
  return true &&
         r.archive_slice_max_age_us >= 1ull;
}
bool AssignFamily(NativeAgentConfig_cleanup_archive_policy& r, std::size_t index, std::string_view v) {
  switch (index) {
    case 0: return ParseU64(v, r.cleanup_blocked_threshold);
    case 1: return ParseU64(v, r.archive_slice_max_age_us);
    case 2: return ParseFlagText(v, r.authoritative_lwm_required);
    case 3: return ParseFlagText(v, r.verify_before_release);
    default: return false;
  }
}
// ---- policy_recommendation_policy ----
constexpr CatalogValueFieldSchema kNativeAgentConfig_policy_recommendation_policyWire[]{
    SB_NATIVE_AGENT_CONFIG_HEAD_WIRE,
    {4, T::opaque_bytes, true, 8},  // recommendation_burn_rate
    {5, T::opaque_bytes, true, 8},  // confidence_threshold
    {6, T::unsigned_integer, true, 8},  // cooldown_seconds
    {7, T::boolean, true, 1},  // no_auto_apply
    SB_NATIVE_AGENT_CONFIG_COMMON_WIRE};
constexpr CatalogValueSchemaView SchemaOf(const NativeAgentConfig_policy_recommendation_policy*) {
  return {65653, kNativeAgentSchemaVersion, kNativeAgentConfig_policy_recommendation_policyWire};
}
constexpr NativeAgentConfigFieldDescriptor kNativeAgentConfig_policy_recommendation_policyFields[]{
    {"recommendation_burn_rate", 4, NativeAgentConfigSourceType::decimal, NativeAgentConfigValueKind::decimal, true, true, true, 0.0, 1.0},
    {"confidence_threshold", 5, NativeAgentConfigSourceType::decimal, NativeAgentConfigValueKind::decimal, true, true, true, 0.0, 1.0},
    {"cooldown_seconds", 6, NativeAgentConfigSourceType::duration_seconds, NativeAgentConfigValueKind::unsigned_integer, true, true, false, 1.0, 0.0},
    {"no_auto_apply", 7, NativeAgentConfigSourceType::boolean, NativeAgentConfigValueKind::boolean, true, false, false, 0.0, 0.0},
};
std::span<const NativeAgentConfigFieldDescriptor> FieldsOf(const NativeAgentConfig_policy_recommendation_policy*) { return kNativeAgentConfig_policy_recommendation_policyFields; }
constexpr std::array<std::string_view, 0> kNativeAgentConfig_policy_recommendation_policyUnrepresented{};
std::span<const std::string_view> UnrepresentedOf(const NativeAgentConfig_policy_recommendation_policy*) { return kNativeAgentConfig_policy_recommendation_policyUnrepresented; }
void FillFamily(Fields& f, const NativeAgentConfig_policy_recommendation_policy& r) {
  f.Decimal(4, r.recommendation_burn_rate);
  f.Decimal(5, r.confidence_threshold);
  f.Number(6, r.cooldown_seconds);
  f.Flag(7, r.no_auto_apply);
}
// 1 decoded, 0 not a family tag, -1 refused.
int DecodeFamily(const CatalogValueFieldView& v, NativeAgentConfig_policy_recommendation_policy& r) {
  switch (v.id) {
    case 4: return DecodeDecimal(v, r.recommendation_burn_rate) ? 1 : -1;
    case 5: return DecodeDecimal(v, r.confidence_threshold) ? 1 : -1;
    case 6: r.cooldown_seconds = *v.unsigned_value(); return 1;
    case 7: r.no_auto_apply = Flag(v); return 1;
    default: return 0;
  }
}
bool ValidFamily(const NativeAgentConfig_policy_recommendation_policy& r) {
  return true &&
         ValidDecimal(r.recommendation_burn_rate, true, 1.0) &&
         ValidDecimal(r.confidence_threshold, true, 1.0) &&
         r.cooldown_seconds >= 1ull;
}
bool AssignFamily(NativeAgentConfig_policy_recommendation_policy& r, std::size_t index, std::string_view v) {
  switch (index) {
    case 0: return ParseDecimal(v, r.recommendation_burn_rate);
    case 1: return ParseDecimal(v, r.confidence_threshold);
    case 2: return ParseU64(v, r.cooldown_seconds);
    case 3: return ParseFlagText(v, r.no_auto_apply);
    default: return false;
  }
}
// ---- distributed_query_policy ----
constexpr CatalogValueFieldSchema kNativeAgentConfig_distributed_query_policyWire[]{
    SB_NATIVE_AGENT_CONFIG_HEAD_WIRE,
    {4, T::unsigned_integer, true, 8},  // fragment_delay_warn_us
    {5, T::unsigned_integer, true, 8},  // queue_sample_interval
    {6, T::unsigned_integer, true, 8},  // remote_fragment_trace_level
    SB_NATIVE_AGENT_CONFIG_COMMON_WIRE};
constexpr CatalogValueSchemaView SchemaOf(const NativeAgentConfig_distributed_query_policy*) {
  return {65654, kNativeAgentSchemaVersion, kNativeAgentConfig_distributed_query_policyWire};
}
constexpr NativeAgentConfigFieldDescriptor kNativeAgentConfig_distributed_query_policyFields[]{
    {"fragment_delay_warn_us", 4, NativeAgentConfigSourceType::duration_microseconds, NativeAgentConfigValueKind::unsigned_integer, true, true, false, 1.0, 0.0},
    {"queue_sample_interval", 5, NativeAgentConfigSourceType::duration_seconds, NativeAgentConfigValueKind::unsigned_integer, true, true, false, 1.0, 0.0},
    {"remote_fragment_trace_level", 6, NativeAgentConfigSourceType::unsigned_integer, NativeAgentConfigValueKind::unsigned_integer, true, true, false, 0.0, 0.0},
};
std::span<const NativeAgentConfigFieldDescriptor> FieldsOf(const NativeAgentConfig_distributed_query_policy*) { return kNativeAgentConfig_distributed_query_policyFields; }
constexpr std::array<std::string_view, 0> kNativeAgentConfig_distributed_query_policyUnrepresented{};
std::span<const std::string_view> UnrepresentedOf(const NativeAgentConfig_distributed_query_policy*) { return kNativeAgentConfig_distributed_query_policyUnrepresented; }
void FillFamily(Fields& f, const NativeAgentConfig_distributed_query_policy& r) {
  f.Number(4, r.fragment_delay_warn_us);
  f.Number(5, r.queue_sample_interval);
  f.Number(6, r.remote_fragment_trace_level);
}
// 1 decoded, 0 not a family tag, -1 refused.
int DecodeFamily(const CatalogValueFieldView& v, NativeAgentConfig_distributed_query_policy& r) {
  switch (v.id) {
    case 4: r.fragment_delay_warn_us = *v.unsigned_value(); return 1;
    case 5: r.queue_sample_interval = *v.unsigned_value(); return 1;
    case 6: r.remote_fragment_trace_level = *v.unsigned_value(); return 1;
    default: return 0;
  }
}
bool ValidFamily(const NativeAgentConfig_distributed_query_policy& r) {
  return true &&
         r.fragment_delay_warn_us >= 1ull &&
         r.queue_sample_interval >= 1ull;
}
bool AssignFamily(NativeAgentConfig_distributed_query_policy& r, std::size_t index, std::string_view v) {
  switch (index) {
    case 0: return ParseU64(v, r.fragment_delay_warn_us);
    case 1: return ParseU64(v, r.queue_sample_interval);
    case 2: return ParseU64(v, r.remote_fragment_trace_level);
    default: return false;
  }
}
// ---- remote_routing_policy ----
constexpr CatalogValueFieldSchema kNativeAgentConfig_remote_routing_policyWire[]{
    SB_NATIVE_AGENT_CONFIG_HEAD_WIRE,
    {4, T::opaque_bytes, true, 8},  // remote_route_latency_weight
    {5, T::boolean, true, 1},  // stale_route_refusal
    {6, T::boolean, true, 1},  // local_fallback_required
    {7, T::boolean, true, 1},  // fence_required
    SB_NATIVE_AGENT_CONFIG_COMMON_WIRE};
constexpr CatalogValueSchemaView SchemaOf(const NativeAgentConfig_remote_routing_policy*) {
  return {65655, kNativeAgentSchemaVersion, kNativeAgentConfig_remote_routing_policyWire};
}
constexpr NativeAgentConfigFieldDescriptor kNativeAgentConfig_remote_routing_policyFields[]{
    {"remote_route_latency_weight", 4, NativeAgentConfigSourceType::decimal, NativeAgentConfigValueKind::decimal, true, true, false, 0.0, 0.0},
    {"stale_route_refusal", 5, NativeAgentConfigSourceType::boolean, NativeAgentConfigValueKind::boolean, true, false, false, 0.0, 0.0},
    {"local_fallback_required", 6, NativeAgentConfigSourceType::boolean, NativeAgentConfigValueKind::boolean, true, false, false, 0.0, 0.0},
    {"fence_required", 7, NativeAgentConfigSourceType::boolean, NativeAgentConfigValueKind::boolean, true, false, false, 0.0, 0.0},
};
std::span<const NativeAgentConfigFieldDescriptor> FieldsOf(const NativeAgentConfig_remote_routing_policy*) { return kNativeAgentConfig_remote_routing_policyFields; }
constexpr std::array<std::string_view, 0> kNativeAgentConfig_remote_routing_policyUnrepresented{};
std::span<const std::string_view> UnrepresentedOf(const NativeAgentConfig_remote_routing_policy*) { return kNativeAgentConfig_remote_routing_policyUnrepresented; }
void FillFamily(Fields& f, const NativeAgentConfig_remote_routing_policy& r) {
  f.Decimal(4, r.remote_route_latency_weight);
  f.Flag(5, r.stale_route_refusal);
  f.Flag(6, r.local_fallback_required);
  f.Flag(7, r.fence_required);
}
// 1 decoded, 0 not a family tag, -1 refused.
int DecodeFamily(const CatalogValueFieldView& v, NativeAgentConfig_remote_routing_policy& r) {
  switch (v.id) {
    case 4: return DecodeDecimal(v, r.remote_route_latency_weight) ? 1 : -1;
    case 5: r.stale_route_refusal = Flag(v); return 1;
    case 6: r.local_fallback_required = Flag(v); return 1;
    case 7: r.fence_required = Flag(v); return 1;
    default: return 0;
  }
}
bool ValidFamily(const NativeAgentConfig_remote_routing_policy& r) {
  return true &&
         ValidDecimal(r.remote_route_latency_weight, false, 0.0);
}
bool AssignFamily(NativeAgentConfig_remote_routing_policy& r, std::size_t index, std::string_view v) {
  switch (index) {
    case 0: return ParseDecimal(v, r.remote_route_latency_weight);
    case 1: return ParseFlagText(v, r.stale_route_refusal);
    case 2: return ParseFlagText(v, r.local_fallback_required);
    case 3: return ParseFlagText(v, r.fence_required);
    default: return false;
  }
}
// ---- optimizer_learning_policy ----
constexpr CatalogValueFieldSchema kNativeAgentConfig_optimizer_learning_policyWire[]{
    SB_NATIVE_AGENT_CONFIG_HEAD_WIRE,
    {4, T::opaque_bytes, true, 8},  // correction_threshold
    {5, T::opaque_bytes, true, 8},  // confidence_floor
    {6, T::unsigned_integer, true, 8},  // max_correction_age
    {7, T::boolean, true, 1},  // no_semantic_override
    SB_NATIVE_AGENT_CONFIG_COMMON_WIRE};
constexpr CatalogValueSchemaView SchemaOf(const NativeAgentConfig_optimizer_learning_policy*) {
  return {65656, kNativeAgentSchemaVersion, kNativeAgentConfig_optimizer_learning_policyWire};
}
constexpr NativeAgentConfigFieldDescriptor kNativeAgentConfig_optimizer_learning_policyFields[]{
    {"correction_threshold", 4, NativeAgentConfigSourceType::decimal, NativeAgentConfigValueKind::decimal, true, true, true, 0.0, 1.0},
    {"confidence_floor", 5, NativeAgentConfigSourceType::decimal, NativeAgentConfigValueKind::decimal, true, true, true, 0.0, 1.0},
    {"max_correction_age", 6, NativeAgentConfigSourceType::duration_seconds, NativeAgentConfigValueKind::unsigned_integer, true, true, false, 1.0, 0.0},
    {"no_semantic_override", 7, NativeAgentConfigSourceType::boolean, NativeAgentConfigValueKind::boolean, true, false, false, 0.0, 0.0},
};
std::span<const NativeAgentConfigFieldDescriptor> FieldsOf(const NativeAgentConfig_optimizer_learning_policy*) { return kNativeAgentConfig_optimizer_learning_policyFields; }
constexpr std::array<std::string_view, 0> kNativeAgentConfig_optimizer_learning_policyUnrepresented{};
std::span<const std::string_view> UnrepresentedOf(const NativeAgentConfig_optimizer_learning_policy*) { return kNativeAgentConfig_optimizer_learning_policyUnrepresented; }
void FillFamily(Fields& f, const NativeAgentConfig_optimizer_learning_policy& r) {
  f.Decimal(4, r.correction_threshold);
  f.Decimal(5, r.confidence_floor);
  f.Number(6, r.max_correction_age);
  f.Flag(7, r.no_semantic_override);
}
// 1 decoded, 0 not a family tag, -1 refused.
int DecodeFamily(const CatalogValueFieldView& v, NativeAgentConfig_optimizer_learning_policy& r) {
  switch (v.id) {
    case 4: return DecodeDecimal(v, r.correction_threshold) ? 1 : -1;
    case 5: return DecodeDecimal(v, r.confidence_floor) ? 1 : -1;
    case 6: r.max_correction_age = *v.unsigned_value(); return 1;
    case 7: r.no_semantic_override = Flag(v); return 1;
    default: return 0;
  }
}
bool ValidFamily(const NativeAgentConfig_optimizer_learning_policy& r) {
  return true &&
         ValidDecimal(r.correction_threshold, true, 1.0) &&
         ValidDecimal(r.confidence_floor, true, 1.0) &&
         r.max_correction_age >= 1ull;
}
bool AssignFamily(NativeAgentConfig_optimizer_learning_policy& r, std::size_t index, std::string_view v) {
  switch (index) {
    case 0: return ParseDecimal(v, r.correction_threshold);
    case 1: return ParseDecimal(v, r.confidence_floor);
    case 2: return ParseU64(v, r.max_correction_age);
    case 3: return ParseFlagText(v, r.no_semantic_override);
    default: return false;
  }
}
// ---- support_bundle_policy ----
constexpr CatalogValueFieldSchema kNativeAgentConfig_support_bundle_policyWire[]{
    SB_NATIVE_AGENT_CONFIG_HEAD_WIRE,
    {4, T::boolean, true, 1},  // completeness_required
    {5, T::boolean, true, 1},  // redaction_required
    {6, T::unsigned_integer, true, 8},  // bundle_expiry
    {7, T::boolean, true, 1},  // secret_classes_forbidden
    SB_NATIVE_AGENT_CONFIG_COMMON_WIRE};
constexpr CatalogValueSchemaView SchemaOf(const NativeAgentConfig_support_bundle_policy*) {
  return {65657, kNativeAgentSchemaVersion, kNativeAgentConfig_support_bundle_policyWire};
}
constexpr NativeAgentConfigFieldDescriptor kNativeAgentConfig_support_bundle_policyFields[]{
    {"completeness_required", 4, NativeAgentConfigSourceType::boolean, NativeAgentConfigValueKind::boolean, true, false, false, 0.0, 0.0},
    {"redaction_required", 5, NativeAgentConfigSourceType::boolean, NativeAgentConfigValueKind::boolean, true, false, false, 0.0, 0.0},
    {"bundle_expiry", 6, NativeAgentConfigSourceType::duration_seconds, NativeAgentConfigValueKind::unsigned_integer, true, true, false, 1.0, 0.0},
    {"secret_classes_forbidden", 7, NativeAgentConfigSourceType::boolean, NativeAgentConfigValueKind::boolean, true, false, false, 0.0, 0.0},
};
std::span<const NativeAgentConfigFieldDescriptor> FieldsOf(const NativeAgentConfig_support_bundle_policy*) { return kNativeAgentConfig_support_bundle_policyFields; }
constexpr std::array<std::string_view, 0> kNativeAgentConfig_support_bundle_policyUnrepresented{};
std::span<const std::string_view> UnrepresentedOf(const NativeAgentConfig_support_bundle_policy*) { return kNativeAgentConfig_support_bundle_policyUnrepresented; }
void FillFamily(Fields& f, const NativeAgentConfig_support_bundle_policy& r) {
  f.Flag(4, r.completeness_required);
  f.Flag(5, r.redaction_required);
  f.Number(6, r.bundle_expiry);
  f.Flag(7, r.secret_classes_forbidden);
}
// 1 decoded, 0 not a family tag, -1 refused.
int DecodeFamily(const CatalogValueFieldView& v, NativeAgentConfig_support_bundle_policy& r) {
  switch (v.id) {
    case 4: r.completeness_required = Flag(v); return 1;
    case 5: r.redaction_required = Flag(v); return 1;
    case 6: r.bundle_expiry = *v.unsigned_value(); return 1;
    case 7: r.secret_classes_forbidden = Flag(v); return 1;
    default: return 0;
  }
}
bool ValidFamily(const NativeAgentConfig_support_bundle_policy& r) {
  return true &&
         r.bundle_expiry >= 1ull;
}
bool AssignFamily(NativeAgentConfig_support_bundle_policy& r, std::size_t index, std::string_view v) {
  switch (index) {
    case 0: return ParseFlagText(v, r.completeness_required);
    case 1: return ParseFlagText(v, r.redaction_required);
    case 2: return ParseU64(v, r.bundle_expiry);
    case 3: return ParseFlagText(v, r.secret_classes_forbidden);
    default: return false;
  }
}
// ---- cluster_scheduler_policy ----
constexpr CatalogValueFieldSchema kNativeAgentConfig_cluster_scheduler_policyWire[]{
    SB_NATIVE_AGENT_CONFIG_HEAD_WIRE,
    {4, T::unsigned_integer, true, 8},  // scheduler_queue_threshold
    {5, T::opaque_bytes, true, 8},  // placement_score_floor
    {6, T::boolean, true, 1},  // fence_required
    {7, T::boolean, true, 1},  // keep_queued_on_uncertainty
    SB_NATIVE_AGENT_CONFIG_COMMON_WIRE};
constexpr CatalogValueSchemaView SchemaOf(const NativeAgentConfig_cluster_scheduler_policy*) {
  return {65658, kNativeAgentSchemaVersion, kNativeAgentConfig_cluster_scheduler_policyWire};
}
constexpr NativeAgentConfigFieldDescriptor kNativeAgentConfig_cluster_scheduler_policyFields[]{
    {"scheduler_queue_threshold", 4, NativeAgentConfigSourceType::unsigned_integer, NativeAgentConfigValueKind::unsigned_integer, true, true, false, 0.0, 0.0},
    {"placement_score_floor", 5, NativeAgentConfigSourceType::decimal, NativeAgentConfigValueKind::decimal, true, true, true, 0.0, 1.0},
    {"fence_required", 6, NativeAgentConfigSourceType::boolean, NativeAgentConfigValueKind::boolean, true, false, false, 0.0, 0.0},
    {"keep_queued_on_uncertainty", 7, NativeAgentConfigSourceType::boolean, NativeAgentConfigValueKind::boolean, true, false, false, 0.0, 0.0},
};
std::span<const NativeAgentConfigFieldDescriptor> FieldsOf(const NativeAgentConfig_cluster_scheduler_policy*) { return kNativeAgentConfig_cluster_scheduler_policyFields; }
constexpr std::array<std::string_view, 0> kNativeAgentConfig_cluster_scheduler_policyUnrepresented{};
std::span<const std::string_view> UnrepresentedOf(const NativeAgentConfig_cluster_scheduler_policy*) { return kNativeAgentConfig_cluster_scheduler_policyUnrepresented; }
void FillFamily(Fields& f, const NativeAgentConfig_cluster_scheduler_policy& r) {
  f.Number(4, r.scheduler_queue_threshold);
  f.Decimal(5, r.placement_score_floor);
  f.Flag(6, r.fence_required);
  f.Flag(7, r.keep_queued_on_uncertainty);
}
// 1 decoded, 0 not a family tag, -1 refused.
int DecodeFamily(const CatalogValueFieldView& v, NativeAgentConfig_cluster_scheduler_policy& r) {
  switch (v.id) {
    case 4: r.scheduler_queue_threshold = *v.unsigned_value(); return 1;
    case 5: return DecodeDecimal(v, r.placement_score_floor) ? 1 : -1;
    case 6: r.fence_required = Flag(v); return 1;
    case 7: r.keep_queued_on_uncertainty = Flag(v); return 1;
    default: return 0;
  }
}
bool ValidFamily(const NativeAgentConfig_cluster_scheduler_policy& r) {
  return true &&
         ValidDecimal(r.placement_score_floor, true, 1.0);
}
bool AssignFamily(NativeAgentConfig_cluster_scheduler_policy& r, std::size_t index, std::string_view v) {
  switch (index) {
    case 0: return ParseU64(v, r.scheduler_queue_threshold);
    case 1: return ParseDecimal(v, r.placement_score_floor);
    case 2: return ParseFlagText(v, r.fence_required);
    case 3: return ParseFlagText(v, r.keep_queued_on_uncertainty);
    default: return false;
  }
}
// ---- job_control_policy ----
constexpr CatalogValueFieldSchema kNativeAgentConfig_job_control_policyWire[]{
    SB_NATIVE_AGENT_CONFIG_HEAD_WIRE,
    {4, T::boolean, true, 1},  // cancel_requires_owner_or_admin
    {5, T::unsigned_integer, true, 8},  // retry_cooldown
    {6, T::unsigned_integer, true, 8},  // suppression_max_duration
    {7, T::boolean, true, 1},  // audit_required
    SB_NATIVE_AGENT_CONFIG_COMMON_WIRE};
constexpr CatalogValueSchemaView SchemaOf(const NativeAgentConfig_job_control_policy*) {
  return {65659, kNativeAgentSchemaVersion, kNativeAgentConfig_job_control_policyWire};
}
constexpr NativeAgentConfigFieldDescriptor kNativeAgentConfig_job_control_policyFields[]{
    {"cancel_requires_owner_or_admin", 4, NativeAgentConfigSourceType::boolean, NativeAgentConfigValueKind::boolean, true, false, false, 0.0, 0.0},
    {"retry_cooldown", 5, NativeAgentConfigSourceType::duration_seconds, NativeAgentConfigValueKind::unsigned_integer, true, true, false, 1.0, 0.0},
    {"suppression_max_duration", 6, NativeAgentConfigSourceType::duration_seconds, NativeAgentConfigValueKind::unsigned_integer, true, true, false, 1.0, 0.0},
    {"audit_required", 7, NativeAgentConfigSourceType::boolean, NativeAgentConfigValueKind::boolean, true, false, false, 0.0, 0.0},
};
std::span<const NativeAgentConfigFieldDescriptor> FieldsOf(const NativeAgentConfig_job_control_policy*) { return kNativeAgentConfig_job_control_policyFields; }
constexpr std::array<std::string_view, 0> kNativeAgentConfig_job_control_policyUnrepresented{};
std::span<const std::string_view> UnrepresentedOf(const NativeAgentConfig_job_control_policy*) { return kNativeAgentConfig_job_control_policyUnrepresented; }
void FillFamily(Fields& f, const NativeAgentConfig_job_control_policy& r) {
  f.Flag(4, r.cancel_requires_owner_or_admin);
  f.Number(5, r.retry_cooldown);
  f.Number(6, r.suppression_max_duration);
  f.Flag(7, r.audit_required);
}
// 1 decoded, 0 not a family tag, -1 refused.
int DecodeFamily(const CatalogValueFieldView& v, NativeAgentConfig_job_control_policy& r) {
  switch (v.id) {
    case 4: r.cancel_requires_owner_or_admin = Flag(v); return 1;
    case 5: r.retry_cooldown = *v.unsigned_value(); return 1;
    case 6: r.suppression_max_duration = *v.unsigned_value(); return 1;
    case 7: r.audit_required = Flag(v); return 1;
    default: return 0;
  }
}
bool ValidFamily(const NativeAgentConfig_job_control_policy& r) {
  return true &&
         r.retry_cooldown >= 1ull &&
         r.suppression_max_duration >= 1ull;
}
bool AssignFamily(NativeAgentConfig_job_control_policy& r, std::size_t index, std::string_view v) {
  switch (index) {
    case 0: return ParseFlagText(v, r.cancel_requires_owner_or_admin);
    case 1: return ParseU64(v, r.retry_cooldown);
    case 2: return ParseU64(v, r.suppression_max_duration);
    case 3: return ParseFlagText(v, r.audit_required);
    default: return false;
  }
}
// ---- backup_policy ----
constexpr CatalogValueFieldSchema kNativeAgentConfig_backup_policyWire[]{
    SB_NATIVE_AGENT_CONFIG_HEAD_WIRE,
    {4, T::unsigned_integer, true, 8},  // stuck_progress_window
    {5, T::boolean, true, 1},  // verification_required
    {7, T::boolean, true, 1},  // cancel_requires_approval
    SB_NATIVE_AGENT_CONFIG_COMMON_WIRE};
constexpr CatalogValueSchemaView SchemaOf(const NativeAgentConfig_backup_policy*) {
  return {65660, kNativeAgentSchemaVersion, kNativeAgentConfig_backup_policyWire};
}
constexpr NativeAgentConfigFieldDescriptor kNativeAgentConfig_backup_policyFields[]{
    {"stuck_progress_window", 4, NativeAgentConfigSourceType::duration_seconds, NativeAgentConfigValueKind::unsigned_integer, true, true, false, 1.0, 0.0},
    {"verification_required", 5, NativeAgentConfigSourceType::boolean, NativeAgentConfigValueKind::boolean, true, false, false, 0.0, 0.0},
    {"blocker_policy", 6, NativeAgentConfigSourceType::token, NativeAgentConfigValueKind::unrepresented, true, false, false, 0.0, 0.0},
    {"cancel_requires_approval", 7, NativeAgentConfigSourceType::boolean, NativeAgentConfigValueKind::boolean, true, false, false, 0.0, 0.0},
};
std::span<const NativeAgentConfigFieldDescriptor> FieldsOf(const NativeAgentConfig_backup_policy*) { return kNativeAgentConfig_backup_policyFields; }
constexpr std::array<std::string_view, 1> kNativeAgentConfig_backup_policyUnrepresented{"blocker_policy"};
std::span<const std::string_view> UnrepresentedOf(const NativeAgentConfig_backup_policy*) { return kNativeAgentConfig_backup_policyUnrepresented; }
void FillFamily(Fields& f, const NativeAgentConfig_backup_policy& r) {
  f.Number(4, r.stuck_progress_window);
  f.Flag(5, r.verification_required);
  f.Flag(7, r.cancel_requires_approval);
}
// 1 decoded, 0 not a family tag, -1 refused.
int DecodeFamily(const CatalogValueFieldView& v, NativeAgentConfig_backup_policy& r) {
  switch (v.id) {
    case 4: r.stuck_progress_window = *v.unsigned_value(); return 1;
    case 5: r.verification_required = Flag(v); return 1;
    case 7: r.cancel_requires_approval = Flag(v); return 1;
    default: return 0;
  }
}
bool ValidFamily(const NativeAgentConfig_backup_policy& r) {
  return true &&
         r.stuck_progress_window >= 1ull;
}
bool AssignFamily(NativeAgentConfig_backup_policy& r, std::size_t index, std::string_view v) {
  switch (index) {
    case 0: return ParseU64(v, r.stuck_progress_window);
    case 1: return ParseFlagText(v, r.verification_required);
    case 2: return !v.empty();  // accepted, NOT carried
    case 3: return ParseFlagText(v, r.cancel_requires_approval);
    default: return false;
  }
}
// ---- archive_policy ----
constexpr CatalogValueFieldSchema kNativeAgentConfig_archive_policyWire[]{
    SB_NATIVE_AGENT_CONFIG_HEAD_WIRE,
    {4, T::unsigned_integer, true, 8},  // archive_queue_pressure
    {5, T::unsigned_integer, true, 8},  // slice_max_age
    {6, T::boolean, true, 1},  // verification_required
    {7, T::boolean, true, 1},  // protected_slice_delete_forbidden
    SB_NATIVE_AGENT_CONFIG_COMMON_WIRE};
constexpr CatalogValueSchemaView SchemaOf(const NativeAgentConfig_archive_policy*) {
  return {65661, kNativeAgentSchemaVersion, kNativeAgentConfig_archive_policyWire};
}
constexpr NativeAgentConfigFieldDescriptor kNativeAgentConfig_archive_policyFields[]{
    {"archive_queue_pressure", 4, NativeAgentConfigSourceType::unsigned_integer, NativeAgentConfigValueKind::unsigned_integer, true, true, false, 0.0, 0.0},
    {"slice_max_age", 5, NativeAgentConfigSourceType::duration_seconds, NativeAgentConfigValueKind::unsigned_integer, true, true, false, 1.0, 0.0},
    {"verification_required", 6, NativeAgentConfigSourceType::boolean, NativeAgentConfigValueKind::boolean, true, false, false, 0.0, 0.0},
    {"protected_slice_delete_forbidden", 7, NativeAgentConfigSourceType::boolean, NativeAgentConfigValueKind::boolean, true, false, false, 0.0, 0.0},
};
std::span<const NativeAgentConfigFieldDescriptor> FieldsOf(const NativeAgentConfig_archive_policy*) { return kNativeAgentConfig_archive_policyFields; }
constexpr std::array<std::string_view, 0> kNativeAgentConfig_archive_policyUnrepresented{};
std::span<const std::string_view> UnrepresentedOf(const NativeAgentConfig_archive_policy*) { return kNativeAgentConfig_archive_policyUnrepresented; }
void FillFamily(Fields& f, const NativeAgentConfig_archive_policy& r) {
  f.Number(4, r.archive_queue_pressure);
  f.Number(5, r.slice_max_age);
  f.Flag(6, r.verification_required);
  f.Flag(7, r.protected_slice_delete_forbidden);
}
// 1 decoded, 0 not a family tag, -1 refused.
int DecodeFamily(const CatalogValueFieldView& v, NativeAgentConfig_archive_policy& r) {
  switch (v.id) {
    case 4: r.archive_queue_pressure = *v.unsigned_value(); return 1;
    case 5: r.slice_max_age = *v.unsigned_value(); return 1;
    case 6: r.verification_required = Flag(v); return 1;
    case 7: r.protected_slice_delete_forbidden = Flag(v); return 1;
    default: return 0;
  }
}
bool ValidFamily(const NativeAgentConfig_archive_policy& r) {
  return true &&
         r.slice_max_age >= 1ull;
}
bool AssignFamily(NativeAgentConfig_archive_policy& r, std::size_t index, std::string_view v) {
  switch (index) {
    case 0: return ParseU64(v, r.archive_queue_pressure);
    case 1: return ParseU64(v, r.slice_max_age);
    case 2: return ParseFlagText(v, r.verification_required);
    case 3: return ParseFlagText(v, r.protected_slice_delete_forbidden);
    default: return false;
  }
}
// ---- restore_drill_policy ----
constexpr CatalogValueFieldSchema kNativeAgentConfig_restore_drill_policyWire[]{
    SB_NATIVE_AGENT_CONFIG_HEAD_WIRE,
    {4, T::boolean, true, 1},  // isolated_target_required
    {5, T::unsigned_integer, true, 8},  // max_duration_us
    {6, T::boolean, true, 1},  // verification_required
    {7, T::boolean, true, 1},  // source_mutation_forbidden
    SB_NATIVE_AGENT_CONFIG_COMMON_WIRE};
constexpr CatalogValueSchemaView SchemaOf(const NativeAgentConfig_restore_drill_policy*) {
  return {65662, kNativeAgentSchemaVersion, kNativeAgentConfig_restore_drill_policyWire};
}
constexpr NativeAgentConfigFieldDescriptor kNativeAgentConfig_restore_drill_policyFields[]{
    {"isolated_target_required", 4, NativeAgentConfigSourceType::boolean, NativeAgentConfigValueKind::boolean, true, false, false, 0.0, 0.0},
    {"max_duration_us", 5, NativeAgentConfigSourceType::duration_microseconds, NativeAgentConfigValueKind::unsigned_integer, true, true, false, 1.0, 0.0},
    {"verification_required", 6, NativeAgentConfigSourceType::boolean, NativeAgentConfigValueKind::boolean, true, false, false, 0.0, 0.0},
    {"source_mutation_forbidden", 7, NativeAgentConfigSourceType::boolean, NativeAgentConfigValueKind::boolean, true, false, false, 0.0, 0.0},
};
std::span<const NativeAgentConfigFieldDescriptor> FieldsOf(const NativeAgentConfig_restore_drill_policy*) { return kNativeAgentConfig_restore_drill_policyFields; }
constexpr std::array<std::string_view, 0> kNativeAgentConfig_restore_drill_policyUnrepresented{};
std::span<const std::string_view> UnrepresentedOf(const NativeAgentConfig_restore_drill_policy*) { return kNativeAgentConfig_restore_drill_policyUnrepresented; }
void FillFamily(Fields& f, const NativeAgentConfig_restore_drill_policy& r) {
  f.Flag(4, r.isolated_target_required);
  f.Number(5, r.max_duration_us);
  f.Flag(6, r.verification_required);
  f.Flag(7, r.source_mutation_forbidden);
}
// 1 decoded, 0 not a family tag, -1 refused.
int DecodeFamily(const CatalogValueFieldView& v, NativeAgentConfig_restore_drill_policy& r) {
  switch (v.id) {
    case 4: r.isolated_target_required = Flag(v); return 1;
    case 5: r.max_duration_us = *v.unsigned_value(); return 1;
    case 6: r.verification_required = Flag(v); return 1;
    case 7: r.source_mutation_forbidden = Flag(v); return 1;
    default: return 0;
  }
}
bool ValidFamily(const NativeAgentConfig_restore_drill_policy& r) {
  return true &&
         r.max_duration_us >= 1ull;
}
bool AssignFamily(NativeAgentConfig_restore_drill_policy& r, std::size_t index, std::string_view v) {
  switch (index) {
    case 0: return ParseFlagText(v, r.isolated_target_required);
    case 1: return ParseU64(v, r.max_duration_us);
    case 2: return ParseFlagText(v, r.verification_required);
    case 3: return ParseFlagText(v, r.source_mutation_forbidden);
    default: return false;
  }
}
// ---- pitr_policy ----
constexpr CatalogValueFieldSchema kNativeAgentConfig_pitr_policyWire[]{
    SB_NATIVE_AGENT_CONFIG_HEAD_WIRE,
    {4, T::unsigned_integer, true, 8},  // minimum_pitr_window_seconds
    {5, T::unsigned_integer, true, 8},  // estimate_refresh_interval
    {6, T::boolean, true, 1},  // restore_plan_requires_approval
    {7, T::unsigned_integer, true, 8},  // archive_freshness_limit
    SB_NATIVE_AGENT_CONFIG_COMMON_WIRE};
constexpr CatalogValueSchemaView SchemaOf(const NativeAgentConfig_pitr_policy*) {
  return {65663, kNativeAgentSchemaVersion, kNativeAgentConfig_pitr_policyWire};
}
constexpr NativeAgentConfigFieldDescriptor kNativeAgentConfig_pitr_policyFields[]{
    {"minimum_pitr_window_seconds", 4, NativeAgentConfigSourceType::duration_seconds, NativeAgentConfigValueKind::unsigned_integer, true, true, false, 1.0, 0.0},
    {"estimate_refresh_interval", 5, NativeAgentConfigSourceType::duration_seconds, NativeAgentConfigValueKind::unsigned_integer, true, true, false, 1.0, 0.0},
    {"restore_plan_requires_approval", 6, NativeAgentConfigSourceType::boolean, NativeAgentConfigValueKind::boolean, true, false, false, 0.0, 0.0},
    {"archive_freshness_limit", 7, NativeAgentConfigSourceType::duration_seconds, NativeAgentConfigValueKind::unsigned_integer, true, true, false, 1.0, 0.0},
};
std::span<const NativeAgentConfigFieldDescriptor> FieldsOf(const NativeAgentConfig_pitr_policy*) { return kNativeAgentConfig_pitr_policyFields; }
constexpr std::array<std::string_view, 0> kNativeAgentConfig_pitr_policyUnrepresented{};
std::span<const std::string_view> UnrepresentedOf(const NativeAgentConfig_pitr_policy*) { return kNativeAgentConfig_pitr_policyUnrepresented; }
void FillFamily(Fields& f, const NativeAgentConfig_pitr_policy& r) {
  f.Number(4, r.minimum_pitr_window_seconds);
  f.Number(5, r.estimate_refresh_interval);
  f.Flag(6, r.restore_plan_requires_approval);
  f.Number(7, r.archive_freshness_limit);
}
// 1 decoded, 0 not a family tag, -1 refused.
int DecodeFamily(const CatalogValueFieldView& v, NativeAgentConfig_pitr_policy& r) {
  switch (v.id) {
    case 4: r.minimum_pitr_window_seconds = *v.unsigned_value(); return 1;
    case 5: r.estimate_refresh_interval = *v.unsigned_value(); return 1;
    case 6: r.restore_plan_requires_approval = Flag(v); return 1;
    case 7: r.archive_freshness_limit = *v.unsigned_value(); return 1;
    default: return 0;
  }
}
bool ValidFamily(const NativeAgentConfig_pitr_policy& r) {
  return true &&
         r.minimum_pitr_window_seconds >= 1ull &&
         r.estimate_refresh_interval >= 1ull &&
         r.archive_freshness_limit >= 1ull;
}
bool AssignFamily(NativeAgentConfig_pitr_policy& r, std::size_t index, std::string_view v) {
  switch (index) {
    case 0: return ParseU64(v, r.minimum_pitr_window_seconds);
    case 1: return ParseU64(v, r.estimate_refresh_interval);
    case 2: return ParseFlagText(v, r.restore_plan_requires_approval);
    case 3: return ParseU64(v, r.archive_freshness_limit);
    default: return false;
  }
}
// ---- identity_lifecycle_policy ----
constexpr CatalogValueFieldSchema kNativeAgentConfig_identity_lifecycle_policyWire[]{
    SB_NATIVE_AGENT_CONFIG_HEAD_WIRE,
    {5, T::boolean, true, 1},  // lock_requires_sec_right
    {6, T::boolean, true, 1},  // evidence_required
    SB_NATIVE_AGENT_CONFIG_COMMON_WIRE};
constexpr CatalogValueSchemaView SchemaOf(const NativeAgentConfig_identity_lifecycle_policy*) {
  return {65664, kNativeAgentSchemaVersion, kNativeAgentConfig_identity_lifecycle_policyWire};
}
constexpr NativeAgentConfigFieldDescriptor kNativeAgentConfig_identity_lifecycle_policyFields[]{
    {"auth_anomaly_policy", 4, NativeAgentConfigSourceType::token, NativeAgentConfigValueKind::unrepresented, true, false, false, 0.0, 0.0},
    {"lock_requires_sec_right", 5, NativeAgentConfigSourceType::boolean, NativeAgentConfigValueKind::boolean, true, false, false, 0.0, 0.0},
    {"evidence_required", 6, NativeAgentConfigSourceType::boolean, NativeAgentConfigValueKind::boolean, true, false, false, 0.0, 0.0},
    {"redaction_class", 7, NativeAgentConfigSourceType::token, NativeAgentConfigValueKind::unrepresented, true, false, false, 0.0, 0.0},
};
std::span<const NativeAgentConfigFieldDescriptor> FieldsOf(const NativeAgentConfig_identity_lifecycle_policy*) { return kNativeAgentConfig_identity_lifecycle_policyFields; }
constexpr std::array<std::string_view, 2> kNativeAgentConfig_identity_lifecycle_policyUnrepresented{"auth_anomaly_policy", "redaction_class"};
std::span<const std::string_view> UnrepresentedOf(const NativeAgentConfig_identity_lifecycle_policy*) { return kNativeAgentConfig_identity_lifecycle_policyUnrepresented; }
void FillFamily(Fields& f, const NativeAgentConfig_identity_lifecycle_policy& r) {
  f.Flag(5, r.lock_requires_sec_right);
  f.Flag(6, r.evidence_required);
}
// 1 decoded, 0 not a family tag, -1 refused.
int DecodeFamily(const CatalogValueFieldView& v, NativeAgentConfig_identity_lifecycle_policy& r) {
  switch (v.id) {
    case 5: r.lock_requires_sec_right = Flag(v); return 1;
    case 6: r.evidence_required = Flag(v); return 1;
    default: return 0;
  }
}
bool ValidFamily(const NativeAgentConfig_identity_lifecycle_policy& r) {
  return true;
}
bool AssignFamily(NativeAgentConfig_identity_lifecycle_policy& r, std::size_t index, std::string_view v) {
  switch (index) {
    case 0: return !v.empty();  // accepted, NOT carried
    case 1: return ParseFlagText(v, r.lock_requires_sec_right);
    case 2: return ParseFlagText(v, r.evidence_required);
    case 3: return !v.empty();  // accepted, NOT carried
    default: return false;
  }
}
// ---- session_control_policy ----
constexpr CatalogValueFieldSchema kNativeAgentConfig_session_control_policyWire[]{
    SB_NATIVE_AGENT_CONFIG_HEAD_WIRE,
    {4, T::boolean, true, 1},  // disconnect_requires_right
    {5, T::unsigned_integer, true, 8},  // reauth_cooldown
    {6, T::boolean, true, 1},  // revoke_requires_sec_right
    SB_NATIVE_AGENT_CONFIG_COMMON_WIRE};
constexpr CatalogValueSchemaView SchemaOf(const NativeAgentConfig_session_control_policy*) {
  return {65665, kNativeAgentSchemaVersion, kNativeAgentConfig_session_control_policyWire};
}
constexpr NativeAgentConfigFieldDescriptor kNativeAgentConfig_session_control_policyFields[]{
    {"disconnect_requires_right", 4, NativeAgentConfigSourceType::boolean, NativeAgentConfigValueKind::boolean, true, false, false, 0.0, 0.0},
    {"reauth_cooldown", 5, NativeAgentConfigSourceType::duration_seconds, NativeAgentConfigValueKind::unsigned_integer, true, true, false, 1.0, 0.0},
    {"revoke_requires_sec_right", 6, NativeAgentConfigSourceType::boolean, NativeAgentConfigValueKind::boolean, true, false, false, 0.0, 0.0},
    {"self_view_redaction", 7, NativeAgentConfigSourceType::token, NativeAgentConfigValueKind::unrepresented, true, false, false, 0.0, 0.0},
};
std::span<const NativeAgentConfigFieldDescriptor> FieldsOf(const NativeAgentConfig_session_control_policy*) { return kNativeAgentConfig_session_control_policyFields; }
constexpr std::array<std::string_view, 1> kNativeAgentConfig_session_control_policyUnrepresented{"self_view_redaction"};
std::span<const std::string_view> UnrepresentedOf(const NativeAgentConfig_session_control_policy*) { return kNativeAgentConfig_session_control_policyUnrepresented; }
void FillFamily(Fields& f, const NativeAgentConfig_session_control_policy& r) {
  f.Flag(4, r.disconnect_requires_right);
  f.Number(5, r.reauth_cooldown);
  f.Flag(6, r.revoke_requires_sec_right);
}
// 1 decoded, 0 not a family tag, -1 refused.
int DecodeFamily(const CatalogValueFieldView& v, NativeAgentConfig_session_control_policy& r) {
  switch (v.id) {
    case 4: r.disconnect_requires_right = Flag(v); return 1;
    case 5: r.reauth_cooldown = *v.unsigned_value(); return 1;
    case 6: r.revoke_requires_sec_right = Flag(v); return 1;
    default: return 0;
  }
}
bool ValidFamily(const NativeAgentConfig_session_control_policy& r) {
  return true &&
         r.reauth_cooldown >= 1ull;
}
bool AssignFamily(NativeAgentConfig_session_control_policy& r, std::size_t index, std::string_view v) {
  switch (index) {
    case 0: return ParseFlagText(v, r.disconnect_requires_right);
    case 1: return ParseU64(v, r.reauth_cooldown);
    case 2: return ParseFlagText(v, r.revoke_requires_sec_right);
    case 3: return !v.empty();  // accepted, NOT carried
    default: return false;
  }
}
// ---- alerting_baseline ----
constexpr CatalogValueFieldSchema kNativeAgentConfig_alerting_baselineWire[]{
    SB_NATIVE_AGENT_CONFIG_HEAD_WIRE,
    {4, T::unsigned_integer, true, 8},  // dedupe_window
    {6, T::unsigned_integer, true, 8},  // silence_max_duration
    {7, T::boolean, true, 1},  // critical_silence_requires_override
    SB_NATIVE_AGENT_CONFIG_COMMON_WIRE};
constexpr CatalogValueSchemaView SchemaOf(const NativeAgentConfig_alerting_baseline*) {
  return {65666, kNativeAgentSchemaVersion, kNativeAgentConfig_alerting_baselineWire};
}
constexpr NativeAgentConfigFieldDescriptor kNativeAgentConfig_alerting_baselineFields[]{
    {"dedupe_window", 4, NativeAgentConfigSourceType::duration_seconds, NativeAgentConfigValueKind::unsigned_integer, true, true, false, 1.0, 0.0},
    {"grouping_rules", 5, NativeAgentConfigSourceType::token_list, NativeAgentConfigValueKind::unrepresented, true, false, false, 0.0, 0.0},
    {"silence_max_duration", 6, NativeAgentConfigSourceType::duration_seconds, NativeAgentConfigValueKind::unsigned_integer, true, true, false, 1.0, 0.0},
    {"critical_silence_requires_override", 7, NativeAgentConfigSourceType::boolean, NativeAgentConfigValueKind::boolean, true, false, false, 0.0, 0.0},
};
std::span<const NativeAgentConfigFieldDescriptor> FieldsOf(const NativeAgentConfig_alerting_baseline*) { return kNativeAgentConfig_alerting_baselineFields; }
constexpr std::array<std::string_view, 1> kNativeAgentConfig_alerting_baselineUnrepresented{"grouping_rules"};
std::span<const std::string_view> UnrepresentedOf(const NativeAgentConfig_alerting_baseline*) { return kNativeAgentConfig_alerting_baselineUnrepresented; }
void FillFamily(Fields& f, const NativeAgentConfig_alerting_baseline& r) {
  f.Number(4, r.dedupe_window);
  f.Number(6, r.silence_max_duration);
  f.Flag(7, r.critical_silence_requires_override);
}
// 1 decoded, 0 not a family tag, -1 refused.
int DecodeFamily(const CatalogValueFieldView& v, NativeAgentConfig_alerting_baseline& r) {
  switch (v.id) {
    case 4: r.dedupe_window = *v.unsigned_value(); return 1;
    case 6: r.silence_max_duration = *v.unsigned_value(); return 1;
    case 7: r.critical_silence_requires_override = Flag(v); return 1;
    default: return 0;
  }
}
bool ValidFamily(const NativeAgentConfig_alerting_baseline& r) {
  return true &&
         r.dedupe_window >= 1ull &&
         r.silence_max_duration >= 1ull;
}
bool AssignFamily(NativeAgentConfig_alerting_baseline& r, std::size_t index, std::string_view v) {
  switch (index) {
    case 0: return ParseU64(v, r.dedupe_window);
    case 1: return !v.empty();  // accepted, NOT carried
    case 2: return ParseU64(v, r.silence_max_duration);
    case 3: return ParseFlagText(v, r.critical_silence_requires_override);
    default: return false;
  }
}
// ---- export_default_baseline ----
constexpr CatalogValueFieldSchema kNativeAgentConfig_export_default_baselineWire[]{
    SB_NATIVE_AGENT_CONFIG_HEAD_WIRE,
    {4, T::boolean, true, 1},  // residency_required
    {5, T::boolean, true, 1},  // redaction_required
    {6, T::unsigned_integer, true, 8},  // queue_backpressure
    {7, T::boolean, true, 1},  // adapter_health_required
    SB_NATIVE_AGENT_CONFIG_COMMON_WIRE};
constexpr CatalogValueSchemaView SchemaOf(const NativeAgentConfig_export_default_baseline*) {
  return {65667, kNativeAgentSchemaVersion, kNativeAgentConfig_export_default_baselineWire};
}
constexpr NativeAgentConfigFieldDescriptor kNativeAgentConfig_export_default_baselineFields[]{
    {"residency_required", 4, NativeAgentConfigSourceType::boolean, NativeAgentConfigValueKind::boolean, true, false, false, 0.0, 0.0},
    {"redaction_required", 5, NativeAgentConfigSourceType::boolean, NativeAgentConfigValueKind::boolean, true, false, false, 0.0, 0.0},
    {"queue_backpressure", 6, NativeAgentConfigSourceType::unsigned_integer, NativeAgentConfigValueKind::unsigned_integer, true, true, false, 0.0, 0.0},
    {"adapter_health_required", 7, NativeAgentConfigSourceType::boolean, NativeAgentConfigValueKind::boolean, true, false, false, 0.0, 0.0},
};
std::span<const NativeAgentConfigFieldDescriptor> FieldsOf(const NativeAgentConfig_export_default_baseline*) { return kNativeAgentConfig_export_default_baselineFields; }
constexpr std::array<std::string_view, 0> kNativeAgentConfig_export_default_baselineUnrepresented{};
std::span<const std::string_view> UnrepresentedOf(const NativeAgentConfig_export_default_baseline*) { return kNativeAgentConfig_export_default_baselineUnrepresented; }
void FillFamily(Fields& f, const NativeAgentConfig_export_default_baseline& r) {
  f.Flag(4, r.residency_required);
  f.Flag(5, r.redaction_required);
  f.Number(6, r.queue_backpressure);
  f.Flag(7, r.adapter_health_required);
}
// 1 decoded, 0 not a family tag, -1 refused.
int DecodeFamily(const CatalogValueFieldView& v, NativeAgentConfig_export_default_baseline& r) {
  switch (v.id) {
    case 4: r.residency_required = Flag(v); return 1;
    case 5: r.redaction_required = Flag(v); return 1;
    case 6: r.queue_backpressure = *v.unsigned_value(); return 1;
    case 7: r.adapter_health_required = Flag(v); return 1;
    default: return 0;
  }
}
bool ValidFamily(const NativeAgentConfig_export_default_baseline& r) {
  return true;
}
bool AssignFamily(NativeAgentConfig_export_default_baseline& r, std::size_t index, std::string_view v) {
  switch (index) {
    case 0: return ParseFlagText(v, r.residency_required);
    case 1: return ParseFlagText(v, r.redaction_required);
    case 2: return ParseU64(v, r.queue_backpressure);
    case 3: return ParseFlagText(v, r.adapter_health_required);
    default: return false;
  }
}
// ---- upgrade_policy ----
constexpr CatalogValueFieldSchema kNativeAgentConfig_upgrade_policyWire[]{
    SB_NATIVE_AGENT_CONFIG_HEAD_WIRE,
    {4, T::boolean, true, 1},  // readiness_required
    {5, T::boolean, true, 1},  // fence_required
    {8, T::boolean, true, 1},  // version_compatibility_required
    SB_NATIVE_AGENT_CONFIG_COMMON_WIRE};
constexpr CatalogValueSchemaView SchemaOf(const NativeAgentConfig_upgrade_policy*) {
  return {65668, kNativeAgentSchemaVersion, kNativeAgentConfig_upgrade_policyWire};
}
constexpr NativeAgentConfigFieldDescriptor kNativeAgentConfig_upgrade_policyFields[]{
    {"readiness_required", 4, NativeAgentConfigSourceType::boolean, NativeAgentConfigValueKind::boolean, true, false, false, 0.0, 0.0},
    {"fence_required", 5, NativeAgentConfigSourceType::boolean, NativeAgentConfigValueKind::boolean, true, false, false, 0.0, 0.0},
    {"rolling_step_order", 6, NativeAgentConfigSourceType::token_list, NativeAgentConfigValueKind::unrepresented, true, false, false, 0.0, 0.0},
    {"rollback_gate", 7, NativeAgentConfigSourceType::token, NativeAgentConfigValueKind::unrepresented, true, false, false, 0.0, 0.0},
    {"version_compatibility_required", 8, NativeAgentConfigSourceType::boolean, NativeAgentConfigValueKind::boolean, true, false, false, 0.0, 0.0},
};
std::span<const NativeAgentConfigFieldDescriptor> FieldsOf(const NativeAgentConfig_upgrade_policy*) { return kNativeAgentConfig_upgrade_policyFields; }
constexpr std::array<std::string_view, 2> kNativeAgentConfig_upgrade_policyUnrepresented{"rolling_step_order", "rollback_gate"};
std::span<const std::string_view> UnrepresentedOf(const NativeAgentConfig_upgrade_policy*) { return kNativeAgentConfig_upgrade_policyUnrepresented; }
void FillFamily(Fields& f, const NativeAgentConfig_upgrade_policy& r) {
  f.Flag(4, r.readiness_required);
  f.Flag(5, r.fence_required);
  f.Flag(8, r.version_compatibility_required);
}
// 1 decoded, 0 not a family tag, -1 refused.
int DecodeFamily(const CatalogValueFieldView& v, NativeAgentConfig_upgrade_policy& r) {
  switch (v.id) {
    case 4: r.readiness_required = Flag(v); return 1;
    case 5: r.fence_required = Flag(v); return 1;
    case 8: r.version_compatibility_required = Flag(v); return 1;
    default: return 0;
  }
}
bool ValidFamily(const NativeAgentConfig_upgrade_policy& r) {
  return true;
}
bool AssignFamily(NativeAgentConfig_upgrade_policy& r, std::size_t index, std::string_view v) {
  switch (index) {
    case 0: return ParseFlagText(v, r.readiness_required);
    case 1: return ParseFlagText(v, r.fence_required);
    case 2: return !v.empty();  // accepted, NOT carried
    case 3: return !v.empty();  // accepted, NOT carried
    case 4: return ParseFlagText(v, r.version_compatibility_required);
    default: return false;
  }
}
// ---- agent_coordinator_policy ----
constexpr CatalogValueFieldSchema kNativeAgentConfig_agent_coordinator_policyWire[]{
    SB_NATIVE_AGENT_CONFIG_HEAD_WIRE,
    {4, T::boolean, true, 1},  // startup_launch_enabled
    {5, T::unsigned_integer, true, 8},  // startup_verification_class
    {6, T::unsigned_integer, true, 8},  // startup_clock_max_backward_ns
    {7, T::unsigned_integer, true, 8},  // startup_clock_max_forward_jump_ns
    {8, T::boolean, true, 1},  // startup_clock_fail_on_rollback
    {9, T::boolean, true, 1},  // startup_clock_fail_on_monotonic_regression
    {10, T::boolean, true, 1},  // startup_clock_fail_on_forward_jump
    {11, T::unsigned_integer, true, 8},  // startup_uuid_max_regression_ms
    {12, T::unsigned_integer, true, 8},  // startup_uuid_same_ms_wait_timeout_ms
    {13, T::boolean, true, 1},  // go_no_go_required
    {14, T::opaque_bytes, true, 10},  // managed_layers
    {15, T::boolean, true, 1},  // worker_launch_forbidden
    {16, T::unsigned_integer, true, 8},  // max_agents_per_cycle
    {17, T::unsigned_integer, true, 8},  // max_schedule_queue_depth
    {18, T::unsigned_integer, true, 8},  // schedule_interval_microseconds
    {19, T::unsigned_integer, true, 8},  // startup_agent
    {20, T::unsigned_integer, true, 8},  // shutdown_agent
    {21, T::boolean, true, 1},  // coordinator_stops_last
    SB_NATIVE_AGENT_CONFIG_COMMON_WIRE};
constexpr CatalogValueSchemaView SchemaOf(const NativeAgentConfig_agent_coordinator_policy*) {
  return {65669, kNativeAgentSchemaVersion, kNativeAgentConfig_agent_coordinator_policyWire};
}
constexpr NativeAgentConfigFieldDescriptor kNativeAgentConfig_agent_coordinator_policyFields[]{
    {"startup_launch_enabled", 4, NativeAgentConfigSourceType::boolean, NativeAgentConfigValueKind::boolean, true, false, false, 0.0, 0.0},
    {"startup_verification_class", 5, NativeAgentConfigSourceType::token, NativeAgentConfigValueKind::enumeration, true, false, false, 0.0, 0.0},
    {"startup_clock_max_backward_ns", 6, NativeAgentConfigSourceType::unsigned_integer, NativeAgentConfigValueKind::unsigned_integer, true, true, false, 0.0, 0.0},
    {"startup_clock_max_forward_jump_ns", 7, NativeAgentConfigSourceType::unsigned_integer, NativeAgentConfigValueKind::unsigned_integer, true, true, false, 0.0, 0.0},
    {"startup_clock_fail_on_rollback", 8, NativeAgentConfigSourceType::boolean, NativeAgentConfigValueKind::boolean, true, false, false, 0.0, 0.0},
    {"startup_clock_fail_on_monotonic_regression", 9, NativeAgentConfigSourceType::boolean, NativeAgentConfigValueKind::boolean, true, false, false, 0.0, 0.0},
    {"startup_clock_fail_on_forward_jump", 10, NativeAgentConfigSourceType::boolean, NativeAgentConfigValueKind::boolean, true, false, false, 0.0, 0.0},
    {"startup_uuid_max_regression_ms", 11, NativeAgentConfigSourceType::unsigned_integer, NativeAgentConfigValueKind::unsigned_integer, true, true, true, 0.0, 281474976710655.0},
    {"startup_uuid_same_ms_wait_timeout_ms", 12, NativeAgentConfigSourceType::unsigned_integer, NativeAgentConfigValueKind::unsigned_integer, true, true, true, 1.0, 18446744073709.0},
    {"go_no_go_required", 13, NativeAgentConfigSourceType::boolean, NativeAgentConfigValueKind::boolean, true, false, false, 0.0, 0.0},
    {"managed_layers", 14, NativeAgentConfigSourceType::token_list, NativeAgentConfigValueKind::layer_list, true, false, false, 0.0, 0.0},
    {"worker_launch_forbidden", 15, NativeAgentConfigSourceType::boolean, NativeAgentConfigValueKind::boolean, true, false, false, 0.0, 0.0},
    {"max_agents_per_cycle", 16, NativeAgentConfigSourceType::unsigned_integer, NativeAgentConfigValueKind::unsigned_integer, true, true, false, 0.0, 0.0},
    {"max_schedule_queue_depth", 17, NativeAgentConfigSourceType::unsigned_integer, NativeAgentConfigValueKind::unsigned_integer, true, true, false, 0.0, 0.0},
    {"schedule_interval_microseconds", 18, NativeAgentConfigSourceType::duration_microseconds, NativeAgentConfigValueKind::unsigned_integer, true, true, false, 1.0, 0.0},
    {"startup_agent", 19, NativeAgentConfigSourceType::token, NativeAgentConfigValueKind::agent_type, true, false, false, 0.0, 0.0},
    {"shutdown_agent", 20, NativeAgentConfigSourceType::token, NativeAgentConfigValueKind::agent_type, true, false, false, 0.0, 0.0},
    {"coordinator_stops_last", 21, NativeAgentConfigSourceType::boolean, NativeAgentConfigValueKind::boolean, true, false, false, 0.0, 0.0},
};
std::span<const NativeAgentConfigFieldDescriptor> FieldsOf(const NativeAgentConfig_agent_coordinator_policy*) { return kNativeAgentConfig_agent_coordinator_policyFields; }
constexpr std::array<std::string_view, 0> kNativeAgentConfig_agent_coordinator_policyUnrepresented{};
std::span<const std::string_view> UnrepresentedOf(const NativeAgentConfig_agent_coordinator_policy*) { return kNativeAgentConfig_agent_coordinator_policyUnrepresented; }
void FillFamily(Fields& f, const NativeAgentConfig_agent_coordinator_policy& r) {
  f.Flag(4, r.startup_launch_enabled);
  f.Enum(5, r.startup_verification_class);
  f.Number(6, r.startup_clock_max_backward_ns);
  f.Number(7, r.startup_clock_max_forward_jump_ns);
  f.Flag(8, r.startup_clock_fail_on_rollback);
  f.Flag(9, r.startup_clock_fail_on_monotonic_regression);
  f.Flag(10, r.startup_clock_fail_on_forward_jump);
  f.Number(11, r.startup_uuid_max_regression_ms);
  f.Number(12, r.startup_uuid_same_ms_wait_timeout_ms);
  f.Flag(13, r.go_no_go_required);
  f.List(14, r.managed_layers);
  f.Flag(15, r.worker_launch_forbidden);
  f.Number(16, r.max_agents_per_cycle);
  f.Number(17, r.max_schedule_queue_depth);
  f.Number(18, r.schedule_interval_microseconds);
  f.Enum(19, r.startup_agent);
  f.Enum(20, r.shutdown_agent);
  f.Flag(21, r.coordinator_stops_last);
}
// 1 decoded, 0 not a family tag, -1 refused.
int DecodeFamily(const CatalogValueFieldView& v, NativeAgentConfig_agent_coordinator_policy& r) {
  switch (v.id) {
    case 4: r.startup_launch_enabled = Flag(v); return 1;
    case 5: return DecodeEnum(v, r.startup_verification_class) ? 1 : -1;
    case 6: r.startup_clock_max_backward_ns = *v.unsigned_value(); return 1;
    case 7: r.startup_clock_max_forward_jump_ns = *v.unsigned_value(); return 1;
    case 8: r.startup_clock_fail_on_rollback = Flag(v); return 1;
    case 9: r.startup_clock_fail_on_monotonic_regression = Flag(v); return 1;
    case 10: r.startup_clock_fail_on_forward_jump = Flag(v); return 1;
    case 11: r.startup_uuid_max_regression_ms = *v.unsigned_value(); return 1;
    case 12: r.startup_uuid_same_ms_wait_timeout_ms = *v.unsigned_value(); return 1;
    case 13: r.go_no_go_required = Flag(v); return 1;
    case 14: return DecodeList(v, r.managed_layers) ? 1 : -1;
    case 15: r.worker_launch_forbidden = Flag(v); return 1;
    case 16: r.max_agents_per_cycle = *v.unsigned_value(); return 1;
    case 17: r.max_schedule_queue_depth = *v.unsigned_value(); return 1;
    case 18: r.schedule_interval_microseconds = *v.unsigned_value(); return 1;
    case 19: return DecodeEnum(v, r.startup_agent) ? 1 : -1;
    case 20: return DecodeEnum(v, r.shutdown_agent) ? 1 : -1;
    case 21: r.coordinator_stops_last = Flag(v); return 1;
    default: return 0;
  }
}
bool ValidFamily(const NativeAgentConfig_agent_coordinator_policy& r) {
  return true &&
         KnownGenerated(r.startup_verification_class, 2) &&
         r.startup_uuid_max_regression_ms <= 281474976710655ull &&
         r.startup_uuid_same_ms_wait_timeout_ms >= 1ull &&
         r.startup_uuid_same_ms_wait_timeout_ms <= 18446744073709ull &&
         ValidList(r.managed_layers, true) &&
         r.schedule_interval_microseconds >= 1ull &&
         KnownCode(r.startup_agent) &&
         KnownCode(r.shutdown_agent);
}
bool AssignFamily(NativeAgentConfig_agent_coordinator_policy& r, std::size_t index, std::string_view v) {
  switch (index) {
    case 0: return ParseFlagText(v, r.startup_launch_enabled);
    case 1: return ParseEnum(v, &NativeAgentConfigEnum_agent_coordinator_policy_startup_verification_classForTag, r.startup_verification_class);
    case 2: return ParseU64(v, r.startup_clock_max_backward_ns);
    case 3: return ParseU64(v, r.startup_clock_max_forward_jump_ns);
    case 4: return ParseFlagText(v, r.startup_clock_fail_on_rollback);
    case 5: return ParseFlagText(v, r.startup_clock_fail_on_monotonic_regression);
    case 6: return ParseFlagText(v, r.startup_clock_fail_on_forward_jump);
    case 7: return ParseU64(v, r.startup_uuid_max_regression_ms);
    case 8: return ParseU64(v, r.startup_uuid_same_ms_wait_timeout_ms);
    case 9: return ParseFlagText(v, r.go_no_go_required);
    case 10: return ParseList(v, &NativeAgentManagedLayerForTag, r.managed_layers);
    case 11: return ParseFlagText(v, r.worker_launch_forbidden);
    case 12: return ParseU64(v, r.max_agents_per_cycle);
    case 13: return ParseU64(v, r.max_schedule_queue_depth);
    case 14: return ParseU64(v, r.schedule_interval_microseconds);
    case 15: return ParseEnum(v, &CatalogAgentTypeForTag, r.startup_agent);
    case 16: return ParseEnum(v, &CatalogAgentTypeForTag, r.shutdown_agent);
    case 17: return ParseFlagText(v, r.coordinator_stops_last);
    default: return false;
  }
}
// ---- agent_startup_policy ----
constexpr CatalogValueFieldSchema kNativeAgentConfig_agent_startup_policyWire[]{
    SB_NATIVE_AGENT_CONFIG_HEAD_WIRE,
    {4, T::unsigned_integer, true, 8},  // launch_trigger
    {5, T::boolean, true, 1},  // engine_verification_required
    {6, T::boolean, true, 1},  // committed_policy_required
    {7, T::boolean, true, 1},  // child_readiness_required
    {8, T::boolean, true, 1},  // configured_components_only
    {9, T::opaque_bytes, true, 64},  // dependency_order
    {10, T::unsigned_integer, true, 8},  // readiness_timeout_microseconds
    {11, T::unsigned_integer, true, 8},  // max_retry_attempts
    SB_NATIVE_AGENT_CONFIG_COMMON_WIRE};
constexpr CatalogValueSchemaView SchemaOf(const NativeAgentConfig_agent_startup_policy*) {
  return {65670, kNativeAgentSchemaVersion, kNativeAgentConfig_agent_startup_policyWire};
}
constexpr NativeAgentConfigFieldDescriptor kNativeAgentConfig_agent_startup_policyFields[]{
    {"launch_trigger", 4, NativeAgentConfigSourceType::token, NativeAgentConfigValueKind::launch_trigger, true, false, false, 0.0, 0.0},
    {"engine_verification_required", 5, NativeAgentConfigSourceType::boolean, NativeAgentConfigValueKind::boolean, true, false, false, 0.0, 0.0},
    {"committed_policy_required", 6, NativeAgentConfigSourceType::boolean, NativeAgentConfigValueKind::boolean, true, false, false, 0.0, 0.0},
    {"child_readiness_required", 7, NativeAgentConfigSourceType::boolean, NativeAgentConfigValueKind::boolean, true, false, false, 0.0, 0.0},
    {"configured_components_only", 8, NativeAgentConfigSourceType::boolean, NativeAgentConfigValueKind::boolean, true, false, false, 0.0, 0.0},
    {"dependency_order", 9, NativeAgentConfigSourceType::token_list, NativeAgentConfigValueKind::agent_type_list, true, false, false, 0.0, 0.0},
    {"readiness_timeout_microseconds", 10, NativeAgentConfigSourceType::duration_microseconds, NativeAgentConfigValueKind::unsigned_integer, true, true, false, 1.0, 0.0},
    {"max_retry_attempts", 11, NativeAgentConfigSourceType::unsigned_integer, NativeAgentConfigValueKind::unsigned_integer, true, true, false, 0.0, 0.0},
};
std::span<const NativeAgentConfigFieldDescriptor> FieldsOf(const NativeAgentConfig_agent_startup_policy*) { return kNativeAgentConfig_agent_startup_policyFields; }
constexpr std::array<std::string_view, 0> kNativeAgentConfig_agent_startup_policyUnrepresented{};
std::span<const std::string_view> UnrepresentedOf(const NativeAgentConfig_agent_startup_policy*) { return kNativeAgentConfig_agent_startup_policyUnrepresented; }
void FillFamily(Fields& f, const NativeAgentConfig_agent_startup_policy& r) {
  f.Enum(4, r.launch_trigger);
  f.Flag(5, r.engine_verification_required);
  f.Flag(6, r.committed_policy_required);
  f.Flag(7, r.child_readiness_required);
  f.Flag(8, r.configured_components_only);
  f.List(9, r.dependency_order);
  f.Number(10, r.readiness_timeout_microseconds);
  f.Number(11, r.max_retry_attempts);
}
// 1 decoded, 0 not a family tag, -1 refused.
int DecodeFamily(const CatalogValueFieldView& v, NativeAgentConfig_agent_startup_policy& r) {
  switch (v.id) {
    case 4: return DecodeEnum(v, r.launch_trigger) ? 1 : -1;
    case 5: r.engine_verification_required = Flag(v); return 1;
    case 6: r.committed_policy_required = Flag(v); return 1;
    case 7: r.child_readiness_required = Flag(v); return 1;
    case 8: r.configured_components_only = Flag(v); return 1;
    case 9: return DecodeList(v, r.dependency_order) ? 1 : -1;
    case 10: r.readiness_timeout_microseconds = *v.unsigned_value(); return 1;
    case 11: r.max_retry_attempts = *v.unsigned_value(); return 1;
    default: return 0;
  }
}
bool ValidFamily(const NativeAgentConfig_agent_startup_policy& r) {
  return true &&
         KnownCode(r.launch_trigger) &&
         ValidList(r.dependency_order, true) &&
         r.readiness_timeout_microseconds >= 1ull;
}
bool AssignFamily(NativeAgentConfig_agent_startup_policy& r, std::size_t index, std::string_view v) {
  switch (index) {
    case 0: return ParseEnum(v, &CatalogAgentLaunchTriggerForTag, r.launch_trigger);
    case 1: return ParseFlagText(v, r.engine_verification_required);
    case 2: return ParseFlagText(v, r.committed_policy_required);
    case 3: return ParseFlagText(v, r.child_readiness_required);
    case 4: return ParseFlagText(v, r.configured_components_only);
    case 5: return ParseList(v, &CatalogAgentTypeForTag, r.dependency_order);
    case 6: return ParseU64(v, r.readiness_timeout_microseconds);
    case 7: return ParseU64(v, r.max_retry_attempts);
    default: return false;
  }
}
// ---- agent_shutdown_policy ----
constexpr CatalogValueFieldSchema kNativeAgentConfig_agent_shutdown_policyWire[]{
    SB_NATIVE_AGENT_CONFIG_HEAD_WIRE,
    {4, T::unsigned_integer, true, 8},  // launch_trigger
    {5, T::boolean, true, 1},  // withdraw_admission_first
    {6, T::boolean, true, 1},  // reverse_dependency_drain
    {7, T::boolean, true, 1},  // drain_observations_before_recorders
    {8, T::boolean, true, 1},  // engine_final_receipt_required
    {9, T::boolean, true, 1},  // cooperative_cancellation_only
    {10, T::unsigned_integer, true, 8},  // drain_timeout_microseconds
    SB_NATIVE_AGENT_CONFIG_COMMON_WIRE};
constexpr CatalogValueSchemaView SchemaOf(const NativeAgentConfig_agent_shutdown_policy*) {
  return {65671, kNativeAgentSchemaVersion, kNativeAgentConfig_agent_shutdown_policyWire};
}
constexpr NativeAgentConfigFieldDescriptor kNativeAgentConfig_agent_shutdown_policyFields[]{
    {"launch_trigger", 4, NativeAgentConfigSourceType::token, NativeAgentConfigValueKind::launch_trigger, true, false, false, 0.0, 0.0},
    {"withdraw_admission_first", 5, NativeAgentConfigSourceType::boolean, NativeAgentConfigValueKind::boolean, true, false, false, 0.0, 0.0},
    {"reverse_dependency_drain", 6, NativeAgentConfigSourceType::boolean, NativeAgentConfigValueKind::boolean, true, false, false, 0.0, 0.0},
    {"drain_observations_before_recorders", 7, NativeAgentConfigSourceType::boolean, NativeAgentConfigValueKind::boolean, true, false, false, 0.0, 0.0},
    {"engine_final_receipt_required", 8, NativeAgentConfigSourceType::boolean, NativeAgentConfigValueKind::boolean, true, false, false, 0.0, 0.0},
    {"cooperative_cancellation_only", 9, NativeAgentConfigSourceType::boolean, NativeAgentConfigValueKind::boolean, true, false, false, 0.0, 0.0},
    {"drain_timeout_microseconds", 10, NativeAgentConfigSourceType::duration_microseconds, NativeAgentConfigValueKind::unsigned_integer, true, true, false, 1.0, 0.0},
};
std::span<const NativeAgentConfigFieldDescriptor> FieldsOf(const NativeAgentConfig_agent_shutdown_policy*) { return kNativeAgentConfig_agent_shutdown_policyFields; }
constexpr std::array<std::string_view, 0> kNativeAgentConfig_agent_shutdown_policyUnrepresented{};
std::span<const std::string_view> UnrepresentedOf(const NativeAgentConfig_agent_shutdown_policy*) { return kNativeAgentConfig_agent_shutdown_policyUnrepresented; }
void FillFamily(Fields& f, const NativeAgentConfig_agent_shutdown_policy& r) {
  f.Enum(4, r.launch_trigger);
  f.Flag(5, r.withdraw_admission_first);
  f.Flag(6, r.reverse_dependency_drain);
  f.Flag(7, r.drain_observations_before_recorders);
  f.Flag(8, r.engine_final_receipt_required);
  f.Flag(9, r.cooperative_cancellation_only);
  f.Number(10, r.drain_timeout_microseconds);
}
// 1 decoded, 0 not a family tag, -1 refused.
int DecodeFamily(const CatalogValueFieldView& v, NativeAgentConfig_agent_shutdown_policy& r) {
  switch (v.id) {
    case 4: return DecodeEnum(v, r.launch_trigger) ? 1 : -1;
    case 5: r.withdraw_admission_first = Flag(v); return 1;
    case 6: r.reverse_dependency_drain = Flag(v); return 1;
    case 7: r.drain_observations_before_recorders = Flag(v); return 1;
    case 8: r.engine_final_receipt_required = Flag(v); return 1;
    case 9: r.cooperative_cancellation_only = Flag(v); return 1;
    case 10: r.drain_timeout_microseconds = *v.unsigned_value(); return 1;
    default: return 0;
  }
}
bool ValidFamily(const NativeAgentConfig_agent_shutdown_policy& r) {
  return true &&
         KnownCode(r.launch_trigger) &&
         r.drain_timeout_microseconds >= 1ull;
}
bool AssignFamily(NativeAgentConfig_agent_shutdown_policy& r, std::size_t index, std::string_view v) {
  switch (index) {
    case 0: return ParseEnum(v, &CatalogAgentLaunchTriggerForTag, r.launch_trigger);
    case 1: return ParseFlagText(v, r.withdraw_admission_first);
    case 2: return ParseFlagText(v, r.reverse_dependency_drain);
    case 3: return ParseFlagText(v, r.drain_observations_before_recorders);
    case 4: return ParseFlagText(v, r.engine_final_receipt_required);
    case 5: return ParseFlagText(v, r.cooperative_cancellation_only);
    case 6: return ParseU64(v, r.drain_timeout_microseconds);
    default: return false;
  }
}
// ---- coordinator_policy ----
constexpr CatalogValueFieldSchema kNativeAgentConfig_coordinator_policyWire[]{
    SB_NATIVE_AGENT_CONFIG_HEAD_WIRE,
    {4, T::unsigned_integer, true, 8},  // heartbeat_interval_microseconds
    {5, T::unsigned_integer, true, 8},  // coordinator_log_poll_interval_microseconds
    {6, T::boolean, true, 1},  // zero_row_short_circuit
    {7, T::unsigned_integer, true, 8},  // cardinality_surprise_threshold_pct
    {8, T::unsigned_integer, true, 8},  // cardinality_surprise_repeat_window_microseconds
    {9, T::unsigned_integer, true, 8},  // max_observation_queue_depth
    {10, T::unsigned_integer, true, 8},  // max_active_leg_workers
    SB_NATIVE_AGENT_CONFIG_COMMON_WIRE};
constexpr CatalogValueSchemaView SchemaOf(const NativeAgentConfig_coordinator_policy*) {
  return {65672, kNativeAgentSchemaVersion, kNativeAgentConfig_coordinator_policyWire};
}
constexpr NativeAgentConfigFieldDescriptor kNativeAgentConfig_coordinator_policyFields[]{
    {"heartbeat_interval_microseconds", 4, NativeAgentConfigSourceType::duration_microseconds, NativeAgentConfigValueKind::unsigned_integer, true, true, false, 1.0, 0.0},
    {"coordinator_log_poll_interval_microseconds", 5, NativeAgentConfigSourceType::duration_microseconds, NativeAgentConfigValueKind::unsigned_integer, true, true, false, 1.0, 0.0},
    {"zero_row_short_circuit", 6, NativeAgentConfigSourceType::boolean, NativeAgentConfigValueKind::boolean, true, false, false, 0.0, 0.0},
    {"cardinality_surprise_threshold_pct", 7, NativeAgentConfigSourceType::percent, NativeAgentConfigValueKind::unsigned_integer, true, true, true, 0.0, 100.0},
    {"cardinality_surprise_repeat_window_microseconds", 8, NativeAgentConfigSourceType::duration_microseconds, NativeAgentConfigValueKind::unsigned_integer, true, true, false, 1.0, 0.0},
    {"max_observation_queue_depth", 9, NativeAgentConfigSourceType::unsigned_integer, NativeAgentConfigValueKind::unsigned_integer, true, true, false, 0.0, 0.0},
    {"max_active_leg_workers", 10, NativeAgentConfigSourceType::unsigned_integer, NativeAgentConfigValueKind::unsigned_integer, true, true, false, 0.0, 0.0},
};
std::span<const NativeAgentConfigFieldDescriptor> FieldsOf(const NativeAgentConfig_coordinator_policy*) { return kNativeAgentConfig_coordinator_policyFields; }
constexpr std::array<std::string_view, 0> kNativeAgentConfig_coordinator_policyUnrepresented{};
std::span<const std::string_view> UnrepresentedOf(const NativeAgentConfig_coordinator_policy*) { return kNativeAgentConfig_coordinator_policyUnrepresented; }
void FillFamily(Fields& f, const NativeAgentConfig_coordinator_policy& r) {
  f.Number(4, r.heartbeat_interval_microseconds);
  f.Number(5, r.coordinator_log_poll_interval_microseconds);
  f.Flag(6, r.zero_row_short_circuit);
  f.Number(7, r.cardinality_surprise_threshold_pct);
  f.Number(8, r.cardinality_surprise_repeat_window_microseconds);
  f.Number(9, r.max_observation_queue_depth);
  f.Number(10, r.max_active_leg_workers);
}
// 1 decoded, 0 not a family tag, -1 refused.
int DecodeFamily(const CatalogValueFieldView& v, NativeAgentConfig_coordinator_policy& r) {
  switch (v.id) {
    case 4: r.heartbeat_interval_microseconds = *v.unsigned_value(); return 1;
    case 5: r.coordinator_log_poll_interval_microseconds = *v.unsigned_value(); return 1;
    case 6: r.zero_row_short_circuit = Flag(v); return 1;
    case 7: r.cardinality_surprise_threshold_pct = *v.unsigned_value(); return 1;
    case 8: r.cardinality_surprise_repeat_window_microseconds = *v.unsigned_value(); return 1;
    case 9: r.max_observation_queue_depth = *v.unsigned_value(); return 1;
    case 10: r.max_active_leg_workers = *v.unsigned_value(); return 1;
    default: return 0;
  }
}
bool ValidFamily(const NativeAgentConfig_coordinator_policy& r) {
  return true &&
         r.heartbeat_interval_microseconds >= 1ull &&
         r.coordinator_log_poll_interval_microseconds >= 1ull &&
         r.cardinality_surprise_threshold_pct <= 100ull &&
         r.cardinality_surprise_repeat_window_microseconds >= 1ull;
}
bool AssignFamily(NativeAgentConfig_coordinator_policy& r, std::size_t index, std::string_view v) {
  switch (index) {
    case 0: return ParseU64(v, r.heartbeat_interval_microseconds);
    case 1: return ParseU64(v, r.coordinator_log_poll_interval_microseconds);
    case 2: return ParseFlagText(v, r.zero_row_short_circuit);
    case 3: return ParseU64(v, r.cardinality_surprise_threshold_pct);
    case 4: return ParseU64(v, r.cardinality_surprise_repeat_window_microseconds);
    case 5: return ParseU64(v, r.max_observation_queue_depth);
    case 6: return ParseU64(v, r.max_active_leg_workers);
    default: return false;
  }
}
// ---- cleanup_cycle_policy ----
constexpr CatalogValueFieldSchema kNativeAgentConfig_cleanup_cycle_policyWire[]{
    SB_NATIVE_AGENT_CONFIG_HEAD_WIRE,
    {4, T::unsigned_integer, true, 8},  // coordinator_liveness_window_microseconds
    {5, T::unsigned_integer, true, 8},  // max_absences_per_sweep
    {6, T::unsigned_integer, true, 8},  // orphaned_leg_kill_timeout_microseconds
    {7, T::boolean, true, 1},  // write_absence_evidence_required
    {8, T::boolean, true, 1},  // degrade_on_stale_log
    SB_NATIVE_AGENT_CONFIG_COMMON_WIRE};
constexpr CatalogValueSchemaView SchemaOf(const NativeAgentConfig_cleanup_cycle_policy*) {
  return {65673, kNativeAgentSchemaVersion, kNativeAgentConfig_cleanup_cycle_policyWire};
}
constexpr NativeAgentConfigFieldDescriptor kNativeAgentConfig_cleanup_cycle_policyFields[]{
    {"coordinator_liveness_window_microseconds", 4, NativeAgentConfigSourceType::duration_microseconds, NativeAgentConfigValueKind::unsigned_integer, true, true, false, 1.0, 0.0},
    {"max_absences_per_sweep", 5, NativeAgentConfigSourceType::unsigned_integer, NativeAgentConfigValueKind::unsigned_integer, true, true, false, 0.0, 0.0},
    {"orphaned_leg_kill_timeout_microseconds", 6, NativeAgentConfigSourceType::duration_microseconds, NativeAgentConfigValueKind::unsigned_integer, true, true, false, 1.0, 0.0},
    {"write_absence_evidence_required", 7, NativeAgentConfigSourceType::boolean, NativeAgentConfigValueKind::boolean, true, false, false, 0.0, 0.0},
    {"degrade_on_stale_log", 8, NativeAgentConfigSourceType::boolean, NativeAgentConfigValueKind::boolean, true, false, false, 0.0, 0.0},
};
std::span<const NativeAgentConfigFieldDescriptor> FieldsOf(const NativeAgentConfig_cleanup_cycle_policy*) { return kNativeAgentConfig_cleanup_cycle_policyFields; }
constexpr std::array<std::string_view, 0> kNativeAgentConfig_cleanup_cycle_policyUnrepresented{};
std::span<const std::string_view> UnrepresentedOf(const NativeAgentConfig_cleanup_cycle_policy*) { return kNativeAgentConfig_cleanup_cycle_policyUnrepresented; }
void FillFamily(Fields& f, const NativeAgentConfig_cleanup_cycle_policy& r) {
  f.Number(4, r.coordinator_liveness_window_microseconds);
  f.Number(5, r.max_absences_per_sweep);
  f.Number(6, r.orphaned_leg_kill_timeout_microseconds);
  f.Flag(7, r.write_absence_evidence_required);
  f.Flag(8, r.degrade_on_stale_log);
}
// 1 decoded, 0 not a family tag, -1 refused.
int DecodeFamily(const CatalogValueFieldView& v, NativeAgentConfig_cleanup_cycle_policy& r) {
  switch (v.id) {
    case 4: r.coordinator_liveness_window_microseconds = *v.unsigned_value(); return 1;
    case 5: r.max_absences_per_sweep = *v.unsigned_value(); return 1;
    case 6: r.orphaned_leg_kill_timeout_microseconds = *v.unsigned_value(); return 1;
    case 7: r.write_absence_evidence_required = Flag(v); return 1;
    case 8: r.degrade_on_stale_log = Flag(v); return 1;
    default: return 0;
  }
}
bool ValidFamily(const NativeAgentConfig_cleanup_cycle_policy& r) {
  return true &&
         r.coordinator_liveness_window_microseconds >= 1ull &&
         r.orphaned_leg_kill_timeout_microseconds >= 1ull;
}
bool AssignFamily(NativeAgentConfig_cleanup_cycle_policy& r, std::size_t index, std::string_view v) {
  switch (index) {
    case 0: return ParseU64(v, r.coordinator_liveness_window_microseconds);
    case 1: return ParseU64(v, r.max_absences_per_sweep);
    case 2: return ParseU64(v, r.orphaned_leg_kill_timeout_microseconds);
    case 3: return ParseFlagText(v, r.write_absence_evidence_required);
    case 4: return ParseFlagText(v, r.degrade_on_stale_log);
    default: return false;
  }
}
// ---- filespace_capacity_policy ----
constexpr CatalogValueFieldSchema kNativeAgentConfig_filespace_capacity_policyWire[]{
    SB_NATIVE_AGENT_CONFIG_HEAD_WIRE,
    {4, T::unsigned_integer, true, 8},  // minimum_free_bytes
    {5, T::unsigned_integer, true, 8},  // minimum_free_percent
    {6, T::unsigned_integer, true, 8},  // growth_rate_window_seconds
    {7, T::unsigned_integer, true, 8},  // depletion_eta_warning_seconds
    {8, T::unsigned_integer, true, 8},  // depletion_eta_action_seconds
    {9, T::boolean, true, 1},  // expand_allowed
    {10, T::boolean, true, 1},  // move_allowed
    {11, T::boolean, true, 1},  // shrink_allowed
    {12, T::boolean, true, 1},  // truncate_allowed
    {13, T::unsigned_integer, true, 8},  // critical_device_error_threshold
    {14, T::unsigned_integer, true, 8},  // fsync_p99_critical_us
    SB_NATIVE_AGENT_CONFIG_COMMON_WIRE};
constexpr CatalogValueSchemaView SchemaOf(const NativeAgentConfig_filespace_capacity_policy*) {
  return {65674, kNativeAgentSchemaVersion, kNativeAgentConfig_filespace_capacity_policyWire};
}
constexpr NativeAgentConfigFieldDescriptor kNativeAgentConfig_filespace_capacity_policyFields[]{
    {"minimum_free_bytes", 4, NativeAgentConfigSourceType::byte_count, NativeAgentConfigValueKind::unsigned_integer, true, true, false, 1.0, 0.0},
    {"minimum_free_percent", 5, NativeAgentConfigSourceType::percent, NativeAgentConfigValueKind::unsigned_integer, true, true, true, 0.0, 100.0},
    {"growth_rate_window_seconds", 6, NativeAgentConfigSourceType::duration_seconds, NativeAgentConfigValueKind::unsigned_integer, true, true, false, 1.0, 0.0},
    {"depletion_eta_warning_seconds", 7, NativeAgentConfigSourceType::duration_seconds, NativeAgentConfigValueKind::unsigned_integer, true, true, false, 1.0, 0.0},
    {"depletion_eta_action_seconds", 8, NativeAgentConfigSourceType::duration_seconds, NativeAgentConfigValueKind::unsigned_integer, true, true, false, 1.0, 0.0},
    {"expand_allowed", 9, NativeAgentConfigSourceType::boolean, NativeAgentConfigValueKind::boolean, true, false, false, 0.0, 0.0},
    {"move_allowed", 10, NativeAgentConfigSourceType::boolean, NativeAgentConfigValueKind::boolean, true, false, false, 0.0, 0.0},
    {"shrink_allowed", 11, NativeAgentConfigSourceType::boolean, NativeAgentConfigValueKind::boolean, true, false, false, 0.0, 0.0},
    {"truncate_allowed", 12, NativeAgentConfigSourceType::boolean, NativeAgentConfigValueKind::boolean, true, false, false, 0.0, 0.0},
    {"critical_device_error_threshold", 13, NativeAgentConfigSourceType::unsigned_integer, NativeAgentConfigValueKind::unsigned_integer, true, true, false, 0.0, 0.0},
    {"fsync_p99_critical_us", 14, NativeAgentConfigSourceType::duration_microseconds, NativeAgentConfigValueKind::unsigned_integer, true, true, false, 1.0, 0.0},
    {"required_approval_class", 15, NativeAgentConfigSourceType::token, NativeAgentConfigValueKind::unrepresented, true, false, false, 0.0, 0.0},
};
std::span<const NativeAgentConfigFieldDescriptor> FieldsOf(const NativeAgentConfig_filespace_capacity_policy*) { return kNativeAgentConfig_filespace_capacity_policyFields; }
constexpr std::array<std::string_view, 1> kNativeAgentConfig_filespace_capacity_policyUnrepresented{"required_approval_class"};
std::span<const std::string_view> UnrepresentedOf(const NativeAgentConfig_filespace_capacity_policy*) { return kNativeAgentConfig_filespace_capacity_policyUnrepresented; }
void FillFamily(Fields& f, const NativeAgentConfig_filespace_capacity_policy& r) {
  f.Number(4, r.minimum_free_bytes);
  f.Number(5, r.minimum_free_percent);
  f.Number(6, r.growth_rate_window_seconds);
  f.Number(7, r.depletion_eta_warning_seconds);
  f.Number(8, r.depletion_eta_action_seconds);
  f.Flag(9, r.expand_allowed);
  f.Flag(10, r.move_allowed);
  f.Flag(11, r.shrink_allowed);
  f.Flag(12, r.truncate_allowed);
  f.Number(13, r.critical_device_error_threshold);
  f.Number(14, r.fsync_p99_critical_us);
}
// 1 decoded, 0 not a family tag, -1 refused.
int DecodeFamily(const CatalogValueFieldView& v, NativeAgentConfig_filespace_capacity_policy& r) {
  switch (v.id) {
    case 4: r.minimum_free_bytes = *v.unsigned_value(); return 1;
    case 5: r.minimum_free_percent = *v.unsigned_value(); return 1;
    case 6: r.growth_rate_window_seconds = *v.unsigned_value(); return 1;
    case 7: r.depletion_eta_warning_seconds = *v.unsigned_value(); return 1;
    case 8: r.depletion_eta_action_seconds = *v.unsigned_value(); return 1;
    case 9: r.expand_allowed = Flag(v); return 1;
    case 10: r.move_allowed = Flag(v); return 1;
    case 11: r.shrink_allowed = Flag(v); return 1;
    case 12: r.truncate_allowed = Flag(v); return 1;
    case 13: r.critical_device_error_threshold = *v.unsigned_value(); return 1;
    case 14: r.fsync_p99_critical_us = *v.unsigned_value(); return 1;
    default: return 0;
  }
}
bool ValidFamily(const NativeAgentConfig_filespace_capacity_policy& r) {
  return true &&
         r.minimum_free_bytes >= 1ull &&
         r.minimum_free_percent <= 100ull &&
         r.growth_rate_window_seconds >= 1ull &&
         r.depletion_eta_warning_seconds >= 1ull &&
         r.depletion_eta_action_seconds >= 1ull &&
         r.fsync_p99_critical_us >= 1ull;
}
bool AssignFamily(NativeAgentConfig_filespace_capacity_policy& r, std::size_t index, std::string_view v) {
  switch (index) {
    case 0: return ParseU64(v, r.minimum_free_bytes);
    case 1: return ParseU64(v, r.minimum_free_percent);
    case 2: return ParseU64(v, r.growth_rate_window_seconds);
    case 3: return ParseU64(v, r.depletion_eta_warning_seconds);
    case 4: return ParseU64(v, r.depletion_eta_action_seconds);
    case 5: return ParseFlagText(v, r.expand_allowed);
    case 6: return ParseFlagText(v, r.move_allowed);
    case 7: return ParseFlagText(v, r.shrink_allowed);
    case 8: return ParseFlagText(v, r.truncate_allowed);
    case 9: return ParseU64(v, r.critical_device_error_threshold);
    case 10: return ParseU64(v, r.fsync_p99_critical_us);
    case 11: return !v.empty();  // accepted, NOT carried
    default: return false;
  }
}
// ---- filespace_shadow_promotion_policy ----
constexpr CatalogValueFieldSchema kNativeAgentConfig_filespace_shadow_promotion_policyWire[]{
    SB_NATIVE_AGENT_CONFIG_HEAD_WIRE,
    {4, T::unsigned_integer, true, 8},  // primary_degradation_threshold
    {5, T::boolean, true, 1},  // candidate_readiness_required
    {6, T::boolean, true, 1},  // catalog_persistence_move_required
    {7, T::boolean, true, 1},  // operator_approval_required
    SB_NATIVE_AGENT_CONFIG_COMMON_WIRE};
constexpr CatalogValueSchemaView SchemaOf(const NativeAgentConfig_filespace_shadow_promotion_policy*) {
  return {65675, kNativeAgentSchemaVersion, kNativeAgentConfig_filespace_shadow_promotion_policyWire};
}
constexpr NativeAgentConfigFieldDescriptor kNativeAgentConfig_filespace_shadow_promotion_policyFields[]{
    {"primary_degradation_threshold", 4, NativeAgentConfigSourceType::unsigned_integer, NativeAgentConfigValueKind::unsigned_integer, true, true, false, 0.0, 0.0},
    {"candidate_readiness_required", 5, NativeAgentConfigSourceType::boolean, NativeAgentConfigValueKind::boolean, true, false, false, 0.0, 0.0},
    {"catalog_persistence_move_required", 6, NativeAgentConfigSourceType::boolean, NativeAgentConfigValueKind::boolean, true, false, false, 0.0, 0.0},
    {"operator_approval_required", 7, NativeAgentConfigSourceType::boolean, NativeAgentConfigValueKind::boolean, true, false, false, 0.0, 0.0},
};
std::span<const NativeAgentConfigFieldDescriptor> FieldsOf(const NativeAgentConfig_filespace_shadow_promotion_policy*) { return kNativeAgentConfig_filespace_shadow_promotion_policyFields; }
constexpr std::array<std::string_view, 0> kNativeAgentConfig_filespace_shadow_promotion_policyUnrepresented{};
std::span<const std::string_view> UnrepresentedOf(const NativeAgentConfig_filespace_shadow_promotion_policy*) { return kNativeAgentConfig_filespace_shadow_promotion_policyUnrepresented; }
void FillFamily(Fields& f, const NativeAgentConfig_filespace_shadow_promotion_policy& r) {
  f.Number(4, r.primary_degradation_threshold);
  f.Flag(5, r.candidate_readiness_required);
  f.Flag(6, r.catalog_persistence_move_required);
  f.Flag(7, r.operator_approval_required);
}
// 1 decoded, 0 not a family tag, -1 refused.
int DecodeFamily(const CatalogValueFieldView& v, NativeAgentConfig_filespace_shadow_promotion_policy& r) {
  switch (v.id) {
    case 4: r.primary_degradation_threshold = *v.unsigned_value(); return 1;
    case 5: r.candidate_readiness_required = Flag(v); return 1;
    case 6: r.catalog_persistence_move_required = Flag(v); return 1;
    case 7: r.operator_approval_required = Flag(v); return 1;
    default: return 0;
  }
}
bool ValidFamily(const NativeAgentConfig_filespace_shadow_promotion_policy& r) {
  return true;
}
bool AssignFamily(NativeAgentConfig_filespace_shadow_promotion_policy& r, std::size_t index, std::string_view v) {
  switch (index) {
    case 0: return ParseU64(v, r.primary_degradation_threshold);
    case 1: return ParseFlagText(v, r.candidate_readiness_required);
    case 2: return ParseFlagText(v, r.catalog_persistence_move_required);
    case 3: return ParseFlagText(v, r.operator_approval_required);
    default: return false;
  }
}
// ---- page_preallocation_policy ----
constexpr CatalogValueFieldSchema kNativeAgentConfig_page_preallocation_policyWire[]{
    SB_NATIVE_AGENT_CONFIG_HEAD_WIRE,
    {4, T::boolean, true, 1},  // preallocation_allowed
    {5, T::unsigned_integer, true, 8},  // forecast_window_seconds
    {6, T::opaque_bytes, true, 8},  // history_confidence_threshold
    {7, T::unsigned_integer, true, 8},  // max_reserved_bytes_per_filespace
    {10, T::unsigned_integer, true, 8},  // allocation_throttle_pages_per_second
    SB_NATIVE_AGENT_CONFIG_COMMON_WIRE};
constexpr CatalogValueSchemaView SchemaOf(const NativeAgentConfig_page_preallocation_policy*) {
  return {65676, kNativeAgentSchemaVersion, kNativeAgentConfig_page_preallocation_policyWire};
}
constexpr NativeAgentConfigFieldDescriptor kNativeAgentConfig_page_preallocation_policyFields[]{
    {"preallocation_allowed", 4, NativeAgentConfigSourceType::boolean, NativeAgentConfigValueKind::boolean, true, false, false, 0.0, 0.0},
    {"forecast_window_seconds", 5, NativeAgentConfigSourceType::duration_seconds, NativeAgentConfigValueKind::unsigned_integer, true, true, false, 1.0, 0.0},
    {"history_confidence_threshold", 6, NativeAgentConfigSourceType::decimal, NativeAgentConfigValueKind::decimal, true, true, true, 0.0, 1.0},
    {"max_reserved_bytes_per_filespace", 7, NativeAgentConfigSourceType::byte_count, NativeAgentConfigValueKind::unsigned_integer, true, true, false, 1.0, 0.0},
    {"allowed_page_families", 8, NativeAgentConfigSourceType::token_list, NativeAgentConfigValueKind::unrepresented, true, false, false, 0.0, 0.0},
    {"workload_calendar", 9, NativeAgentConfigSourceType::token_list, NativeAgentConfigValueKind::unrepresented, true, false, false, 0.0, 0.0},
    {"allocation_throttle_pages_per_second", 10, NativeAgentConfigSourceType::unsigned_integer, NativeAgentConfigValueKind::unsigned_integer, true, true, false, 0.0, 0.0},
};
std::span<const NativeAgentConfigFieldDescriptor> FieldsOf(const NativeAgentConfig_page_preallocation_policy*) { return kNativeAgentConfig_page_preallocation_policyFields; }
constexpr std::array<std::string_view, 2> kNativeAgentConfig_page_preallocation_policyUnrepresented{"allowed_page_families", "workload_calendar"};
std::span<const std::string_view> UnrepresentedOf(const NativeAgentConfig_page_preallocation_policy*) { return kNativeAgentConfig_page_preallocation_policyUnrepresented; }
void FillFamily(Fields& f, const NativeAgentConfig_page_preallocation_policy& r) {
  f.Flag(4, r.preallocation_allowed);
  f.Number(5, r.forecast_window_seconds);
  f.Decimal(6, r.history_confidence_threshold);
  f.Number(7, r.max_reserved_bytes_per_filespace);
  f.Number(10, r.allocation_throttle_pages_per_second);
}
// 1 decoded, 0 not a family tag, -1 refused.
int DecodeFamily(const CatalogValueFieldView& v, NativeAgentConfig_page_preallocation_policy& r) {
  switch (v.id) {
    case 4: r.preallocation_allowed = Flag(v); return 1;
    case 5: r.forecast_window_seconds = *v.unsigned_value(); return 1;
    case 6: return DecodeDecimal(v, r.history_confidence_threshold) ? 1 : -1;
    case 7: r.max_reserved_bytes_per_filespace = *v.unsigned_value(); return 1;
    case 10: r.allocation_throttle_pages_per_second = *v.unsigned_value(); return 1;
    default: return 0;
  }
}
bool ValidFamily(const NativeAgentConfig_page_preallocation_policy& r) {
  return true &&
         r.forecast_window_seconds >= 1ull &&
         ValidDecimal(r.history_confidence_threshold, true, 1.0) &&
         r.max_reserved_bytes_per_filespace >= 1ull;
}
bool AssignFamily(NativeAgentConfig_page_preallocation_policy& r, std::size_t index, std::string_view v) {
  switch (index) {
    case 0: return ParseFlagText(v, r.preallocation_allowed);
    case 1: return ParseU64(v, r.forecast_window_seconds);
    case 2: return ParseDecimal(v, r.history_confidence_threshold);
    case 3: return ParseU64(v, r.max_reserved_bytes_per_filespace);
    case 4: return !v.empty();  // accepted, NOT carried
    case 5: return !v.empty();  // accepted, NOT carried
    case 6: return ParseU64(v, r.allocation_throttle_pages_per_second);
    default: return false;
  }
}
// ---- page_relocation_policy ----
constexpr CatalogValueFieldSchema kNativeAgentConfig_page_relocation_policyWire[]{
    SB_NATIVE_AGENT_CONFIG_HEAD_WIRE,
    {4, T::boolean, true, 1},  // relocation_allowed
    {5, T::boolean, true, 1},  // defragment_allowed
    {6, T::boolean, true, 1},  // publish_shrink_ready_allowed
    {7, T::unsigned_integer, true, 8},  // max_pages_per_interval
    {8, T::unsigned_integer, true, 8},  // interval_seconds
    {10, T::boolean, true, 1},  // require_checksum_valid
    {11, T::boolean, true, 1},  // require_transaction_holds_clear
    {12, T::boolean, true, 1},  // require_backup_archive_holds_clear
    SB_NATIVE_AGENT_CONFIG_COMMON_WIRE};
constexpr CatalogValueSchemaView SchemaOf(const NativeAgentConfig_page_relocation_policy*) {
  return {65677, kNativeAgentSchemaVersion, kNativeAgentConfig_page_relocation_policyWire};
}
constexpr NativeAgentConfigFieldDescriptor kNativeAgentConfig_page_relocation_policyFields[]{
    {"relocation_allowed", 4, NativeAgentConfigSourceType::boolean, NativeAgentConfigValueKind::boolean, true, false, false, 0.0, 0.0},
    {"defragment_allowed", 5, NativeAgentConfigSourceType::boolean, NativeAgentConfigValueKind::boolean, true, false, false, 0.0, 0.0},
    {"publish_shrink_ready_allowed", 6, NativeAgentConfigSourceType::boolean, NativeAgentConfigValueKind::boolean, true, false, false, 0.0, 0.0},
    {"max_pages_per_interval", 7, NativeAgentConfigSourceType::unsigned_integer, NativeAgentConfigValueKind::unsigned_integer, true, true, false, 0.0, 0.0},
    {"interval_seconds", 8, NativeAgentConfigSourceType::duration_seconds, NativeAgentConfigValueKind::unsigned_integer, true, true, false, 1.0, 0.0},
    {"unmovable_blocker_classes", 9, NativeAgentConfigSourceType::token_list, NativeAgentConfigValueKind::unrepresented, true, false, false, 0.0, 0.0},
    {"require_checksum_valid", 10, NativeAgentConfigSourceType::boolean, NativeAgentConfigValueKind::boolean, true, false, false, 0.0, 0.0},
    {"require_transaction_holds_clear", 11, NativeAgentConfigSourceType::boolean, NativeAgentConfigValueKind::boolean, true, false, false, 0.0, 0.0},
    {"require_backup_archive_holds_clear", 12, NativeAgentConfigSourceType::boolean, NativeAgentConfigValueKind::boolean, true, false, false, 0.0, 0.0},
};
std::span<const NativeAgentConfigFieldDescriptor> FieldsOf(const NativeAgentConfig_page_relocation_policy*) { return kNativeAgentConfig_page_relocation_policyFields; }
constexpr std::array<std::string_view, 1> kNativeAgentConfig_page_relocation_policyUnrepresented{"unmovable_blocker_classes"};
std::span<const std::string_view> UnrepresentedOf(const NativeAgentConfig_page_relocation_policy*) { return kNativeAgentConfig_page_relocation_policyUnrepresented; }
void FillFamily(Fields& f, const NativeAgentConfig_page_relocation_policy& r) {
  f.Flag(4, r.relocation_allowed);
  f.Flag(5, r.defragment_allowed);
  f.Flag(6, r.publish_shrink_ready_allowed);
  f.Number(7, r.max_pages_per_interval);
  f.Number(8, r.interval_seconds);
  f.Flag(10, r.require_checksum_valid);
  f.Flag(11, r.require_transaction_holds_clear);
  f.Flag(12, r.require_backup_archive_holds_clear);
}
// 1 decoded, 0 not a family tag, -1 refused.
int DecodeFamily(const CatalogValueFieldView& v, NativeAgentConfig_page_relocation_policy& r) {
  switch (v.id) {
    case 4: r.relocation_allowed = Flag(v); return 1;
    case 5: r.defragment_allowed = Flag(v); return 1;
    case 6: r.publish_shrink_ready_allowed = Flag(v); return 1;
    case 7: r.max_pages_per_interval = *v.unsigned_value(); return 1;
    case 8: r.interval_seconds = *v.unsigned_value(); return 1;
    case 10: r.require_checksum_valid = Flag(v); return 1;
    case 11: r.require_transaction_holds_clear = Flag(v); return 1;
    case 12: r.require_backup_archive_holds_clear = Flag(v); return 1;
    default: return 0;
  }
}
bool ValidFamily(const NativeAgentConfig_page_relocation_policy& r) {
  return true &&
         r.interval_seconds >= 1ull;
}
bool AssignFamily(NativeAgentConfig_page_relocation_policy& r, std::size_t index, std::string_view v) {
  switch (index) {
    case 0: return ParseFlagText(v, r.relocation_allowed);
    case 1: return ParseFlagText(v, r.defragment_allowed);
    case 2: return ParseFlagText(v, r.publish_shrink_ready_allowed);
    case 3: return ParseU64(v, r.max_pages_per_interval);
    case 4: return ParseU64(v, r.interval_seconds);
    case 5: return !v.empty();  // accepted, NOT carried
    case 6: return ParseFlagText(v, r.require_checksum_valid);
    case 7: return ParseFlagText(v, r.require_transaction_holds_clear);
    case 8: return ParseFlagText(v, r.require_backup_archive_holds_clear);
    default: return false;
  }
}
#define SB_NATIVE_AGENT_CONFIG_TYPES(X) \
  X(NativeAgentConfig_node_resource_policy) \
  X(NativeAgentConfig_metric_registry_policy) \
  X(NativeAgentConfig_storage_health_policy) \
  X(NativeAgentConfig_memory_governor_policy) \
  X(NativeAgentConfig_index_health_policy) \
  X(NativeAgentConfig_cluster_autoscale_policy) \
  X(NativeAgentConfig_admission_control_policy) \
  X(NativeAgentConfig_parser_health_policy) \
  X(NativeAgentConfig_long_transaction_policy) \
  X(NativeAgentConfig_storage_version_cleanup_policy) \
  X(NativeAgentConfig_cleanup_archive_policy) \
  X(NativeAgentConfig_policy_recommendation_policy) \
  X(NativeAgentConfig_distributed_query_policy) \
  X(NativeAgentConfig_remote_routing_policy) \
  X(NativeAgentConfig_optimizer_learning_policy) \
  X(NativeAgentConfig_support_bundle_policy) \
  X(NativeAgentConfig_cluster_scheduler_policy) \
  X(NativeAgentConfig_job_control_policy) \
  X(NativeAgentConfig_backup_policy) \
  X(NativeAgentConfig_archive_policy) \
  X(NativeAgentConfig_restore_drill_policy) \
  X(NativeAgentConfig_pitr_policy) \
  X(NativeAgentConfig_identity_lifecycle_policy) \
  X(NativeAgentConfig_session_control_policy) \
  X(NativeAgentConfig_alerting_baseline) \
  X(NativeAgentConfig_export_default_baseline) \
  X(NativeAgentConfig_upgrade_policy) \
  X(NativeAgentConfig_agent_coordinator_policy) \
  X(NativeAgentConfig_agent_startup_policy) \
  X(NativeAgentConfig_agent_shutdown_policy) \
  X(NativeAgentConfig_coordinator_policy) \
  X(NativeAgentConfig_cleanup_cycle_policy) \
  X(NativeAgentConfig_filespace_capacity_policy) \
  X(NativeAgentConfig_filespace_shadow_promotion_policy) \
  X(NativeAgentConfig_page_preallocation_policy) \
  X(NativeAgentConfig_page_relocation_policy)
// ---- end GENERATED: codec ----
#undef SB_NATIVE_AGENT_CONFIG_HEAD_WIRE
#undef SB_NATIVE_AGENT_CONFIG_COMMON_WIRE

// ---- policy schema ----
constexpr CatalogValueFieldSchema policy_fields[]{
    {1, T::engine_identity, true, 16, kObj},                    // definition_uuid
    {2, T::engine_identity, true, 16, UuidKind::database},      // database_uuid
    {3, T::engine_identity, true, 16, kObj},                    // policy_uuid
    {4, T::engine_identity, true, 16, UuidKind::transaction},   // origin transaction
    {5, T::unsigned_integer, true, 8},                          // origin local id
    {6, T::unsigned_integer, true, 8},                          // generation
    {7, T::unsigned_integer, true, 8},                          // agent_type
    {8, T::unsigned_integer, true, 8},                          // policy_family
    {9, T::unsigned_integer, true, 8},                          // policy_class
    {10, T::unsigned_integer, true, 8},                         // policy_generation
    {11, T::unsigned_integer, true, 8},                         // layer
    {12, T::unsigned_integer, true, 8},                         // deployment
    {13, T::unsigned_integer, true, 8},                         // scope (bit set)
    {14, T::unsigned_integer, true, 8},                         // authority
    {15, T::boolean, true, 1},                                  // enabled
    {16, T::unsigned_integer, true, 8},                         // activation
    {17, T::unsigned_integer, true, 8},                         // action_mode
    {18, T::unsigned_integer, true, 8},                         // invalid_policy_behavior
    {19, T::boolean, true, 1},                                  // allow_live_action
    {20, T::boolean, true, 1},                                  // require_manual_approval
    {21, T::boolean, true, 1},                                  // require_dry_run_before_live
    {22, T::boolean, true, 1},                                  // evidence_required
    {23, T::boolean, true, 1},                                  // explainability_required
    {24, T::boolean, true, 1},                                  // editable_seed_policy
    {25, T::unsigned_integer, true, 8},                         // run_interval_microseconds
    {26, T::unsigned_integer, true, 8},                         // jitter_microseconds
    {27, T::unsigned_integer, true, 8},                         // lease_microseconds
    {28, T::unsigned_integer, true, 8},                         // cooldown_microseconds
    {29, T::unsigned_integer, true, 8},                         // max_runtime_microseconds
    {30, T::unsigned_integer, true, 8},                         // max_restart_attempts
    {31, T::unsigned_integer, true, 8},                         // initial_backoff_microseconds
    {32, T::unsigned_integer, true, 8},                         // max_backoff_microseconds
    {33, T::unsigned_integer, true, 8},                         // max_history_query_rows
    {34, T::unsigned_integer, true, 8},                         // max_evidence_fanout
    {35, T::unsigned_integer, true, 8},                         // max_label_cardinality
    {36, T::unsigned_integer, true, 8},                         // action_budget_per_window
    {37, T::opaque_bytes, true, 2 * NativeAgentMetricFamilyList::capacity},  // required_metric_families
    {38, T::opaque_bytes, true, 2 * NativeAgentPolicyFamilyList::capacity},  // policy_dependencies
    {39, T::engine_identity, false, 16, kObj},                  // startup: repair policy
    {40, T::engine_identity, false, 16, kObj},                  // startup: quarantine policy
    {41, T::engine_identity, false, 16, kObj},                  // startup: salvage policy
    {42, T::engine_identity, false, 16, kObj},                  // startup: evidence policy
    {43, T::unsigned_integer, false, 8}};                       // startup: verification levels
constexpr CatalogValueSchemaView policy_schema{kNativeAgentPolicySchemaId, kNativeAgentSchemaVersion, policy_fields};
constexpr CatalogValueFieldSchema registration_fields[]{
    {1, T::engine_identity, true, 16, kObj},                    // definition_uuid
    {2, T::engine_identity, true, 16, UuidKind::database},      // database_uuid
    {3, T::engine_identity, true, 16, kObj},                    // policy_uuid (primary policy)
    {4, T::engine_identity, true, 16, UuidKind::transaction},   // origin transaction
    {5, T::unsigned_integer, true, 8},                          // origin local id
    {6, T::unsigned_integer, true, 8},                          // generation
    {7, T::unsigned_integer, true, 8},                          // agent_type
    {8, T::opaque_bytes, true, 2 * NativeAgentPolicyFamilyList::capacity},  // policy_families
    {9, T::unsigned_integer, true, 8},                          // state
    {10, T::unsigned_integer, true, 8},                         // managed_by
    {11, T::unsigned_integer, true, 8},                         // launch_trigger
    {12, T::boolean, true, 1}};                                 // starts_during_create
constexpr CatalogValueSchemaView registration_schema{kNativeAgentRegistrationSchemaId, kNativeAgentSchemaVersion,
                                                     registration_fields};

// The twelve policy numerics, in wire order (tags 25..36).
constexpr u64 NativeAgentPolicyRecord::* kPolicyNumbers[]{
    &NativeAgentPolicyRecord::run_interval_microseconds, &NativeAgentPolicyRecord::jitter_microseconds,
    &NativeAgentPolicyRecord::lease_microseconds, &NativeAgentPolicyRecord::cooldown_microseconds,
    &NativeAgentPolicyRecord::max_runtime_microseconds, &NativeAgentPolicyRecord::max_restart_attempts,
    &NativeAgentPolicyRecord::initial_backoff_microseconds, &NativeAgentPolicyRecord::max_backoff_microseconds,
    &NativeAgentPolicyRecord::max_history_query_rows, &NativeAgentPolicyRecord::max_evidence_fanout,
    &NativeAgentPolicyRecord::max_label_cardinality, &NativeAgentPolicyRecord::action_budget_per_window};
constexpr std::string_view kPolicyNumberKeys[]{
    "run_interval_microseconds", "jitter_microseconds", "lease_microseconds", "cooldown_microseconds",
    "max_runtime_microseconds", "max_restart_attempts", "initial_backoff_microseconds", "max_backoff_microseconds",
    "max_history_query_rows", "max_evidence_fanout", "max_label_cardinality", "action_budget_per_window"};
// The six policy flags after enabled, in wire order (tags 19..24).
constexpr bool NativeAgentPolicyRecord::* kPolicyFlags[]{
    &NativeAgentPolicyRecord::allow_live_action, &NativeAgentPolicyRecord::require_manual_approval,
    &NativeAgentPolicyRecord::require_dry_run_before_live, &NativeAgentPolicyRecord::evidence_required,
    &NativeAgentPolicyRecord::explainability_required, &NativeAgentPolicyRecord::editable_seed_policy};
constexpr std::string_view kPolicyFlagKeys[]{"allow_live_action", "require_manual_approval",
                                            "require_dry_run_before_live", "evidence_required",
                                            "explainability_required", "editable_seed_policy"};

// ---- validity ----
bool ValidStartup(const NativeAgentPolicyRecord& r) {
  if (!r.startup_verification) return true;
  if (r.agent_type != CatalogAgentType::agent_coordinator) return false;
  const auto& v = *r.startup_verification;
  if (!v.verification_levels || (v.verification_levels & ~u64{1023})) return false;
  const std::array ids{v.repair_policy_uuid, v.quarantine_policy_uuid, v.salvage_policy_uuid, v.evidence_policy_uuid};
  for (std::size_t i = 0; i < ids.size(); ++i) {
    if (!Id(ids[i])) return false;
    for (const auto& excluded : {r.definition_uuid, r.database_uuid, r.policy_uuid, r.origin_transaction_uuid.value})
      if (ids[i] == excluded) return false;
    for (std::size_t j = 0; j < i; ++j)
      if (ids[j] == ids[i]) return false;
  }
  return true;
}
bool ValidRecord(const NativeAgentPolicyRecord& r) {
  return Id(r.definition_uuid) && Id(r.database_uuid) && Id(r.policy_uuid) && r.policy_uuid == r.definition_uuid &&
         Typed(r.origin_transaction_uuid, UuidKind::transaction) && r.origin_local_transaction_id && r.generation &&
         KnownCode(r.agent_type) && KnownCode(r.policy_family) && KnownCode(r.policy_class) && KnownCode(r.layer) &&
         KnownCode(r.deployment) && ValidScope(r.scope) && KnownCode(r.authority) && KnownCode(r.activation) &&
         KnownCode(r.action_mode) && KnownCode(r.invalid_policy_behavior) &&
         ValidList(r.required_metric_families, false) && ValidList(r.policy_dependencies, false) && ValidStartup(r);
}
bool ValidRecord(const NativeAgentRegistrationRecord& r) {
  return Id(r.definition_uuid) && Id(r.database_uuid) && Id(r.policy_uuid) &&
         Typed(r.origin_transaction_uuid, UuidKind::transaction) && r.origin_local_transaction_id && r.generation &&
         KnownCode(r.agent_type) && ValidList(r.policy_families, true) && KnownCode(r.state) &&
         KnownCode(r.managed_by) && KnownCode(r.launch_trigger);
}
template <class R>
bool ValidRecord(const R& r) {
  return Id(r.definition_uuid) && Typed(r.origin_transaction_uuid, UuidKind::transaction) &&
         r.origin_transaction_uuid.value != r.definition_uuid && r.origin_local_transaction_id && ValidFamily(r) &&
         ValidCommon(r.common);
}

// ---- building the field views ----
void Build(Fields& f, const NativeAgentPolicyRecord& r) {
  f.Identity(1, r.definition_uuid, kObj);
  f.Identity(2, r.database_uuid, UuidKind::database);
  f.Identity(3, r.policy_uuid, kObj);
  f.Identity(4, r.origin_transaction_uuid.value, UuidKind::transaction);
  f.Number(5, r.origin_local_transaction_id);
  f.Number(6, r.generation);
  f.Enum(7, r.agent_type);
  f.Enum(8, r.policy_family);
  f.Enum(9, r.policy_class);
  f.Number(10, r.policy_generation);
  f.Enum(11, r.layer);
  f.Enum(12, r.deployment);
  f.Enum(13, r.scope);
  f.Enum(14, r.authority);
  f.Flag(15, r.enabled);
  f.Enum(16, r.activation);
  f.Enum(17, r.action_mode);
  f.Enum(18, r.invalid_policy_behavior);
  for (u16 i = 0; i < 6; ++i) f.Flag(static_cast<u16>(19 + i), r.*kPolicyFlags[i]);
  for (u16 i = 0; i < 12; ++i) f.Number(static_cast<u16>(25 + i), r.*kPolicyNumbers[i]);
  f.List(37, r.required_metric_families);
  f.List(38, r.policy_dependencies);
  if (r.startup_verification) {
    const auto& v = *r.startup_verification;
    f.Identity(39, v.repair_policy_uuid, kObj);
    f.Identity(40, v.quarantine_policy_uuid, kObj);
    f.Identity(41, v.salvage_policy_uuid, kObj);
    f.Identity(42, v.evidence_policy_uuid, kObj);
    f.Number(43, v.verification_levels);
  }
}
void Build(Fields& f, const NativeAgentRegistrationRecord& r) {
  f.Identity(1, r.definition_uuid, kObj);
  f.Identity(2, r.database_uuid, UuidKind::database);
  f.Identity(3, r.policy_uuid, kObj);
  f.Identity(4, r.origin_transaction_uuid.value, UuidKind::transaction);
  f.Number(5, r.origin_local_transaction_id);
  f.Number(6, r.generation);
  f.Enum(7, r.agent_type);
  f.List(8, r.policy_families);
  f.Enum(9, r.state);
  f.Enum(10, r.managed_by);
  f.Enum(11, r.launch_trigger);
  f.Flag(12, r.starts_during_create);
}
template <class R>
void Build(Fields& f, const R& r) {
  f.Identity(1, r.definition_uuid, kObj);
  f.Identity(2, r.origin_transaction_uuid.value, UuidKind::transaction);
  f.Number(3, r.origin_local_transaction_id);
  FillFamily(f, r);
  FillCommon(f, r.common);
}
CatalogValueSchemaView SchemaFor(const NativeAgentPolicyRecord&) { return policy_schema; }
CatalogValueSchemaView SchemaFor(const NativeAgentRegistrationRecord&) { return registration_schema; }
template <class R>
CatalogValueSchemaView SchemaFor(const R& r) { return SchemaOf(&r); }

// ---- the shared encode path ----
template <class R>
std::optional<std::size_t> EncodedBytes(const R& r) {
  if (!ValidRecord(r)) return {};
  Fields f;
  Build(f, r);
  if (ValidateCatalogValueFields(SchemaFor(r), f.View()) != E::none) return {};
  return f.EncodedSize();
}
template <class R>
CatalogValueEncodeIntoResult EncodeInto(const R& r, std::span<byte> out) {
  if (!ValidRecord(r)) return {E::invalid_value, {}};
  const auto dest = reinterpret_cast<std::uintptr_t>(out.data()), input = reinterpret_cast<std::uintptr_t>(&r);
  if ((out.size() && !out.data()) || out.size() > std::numeric_limits<std::uintptr_t>::max() - dest ||
      (dest < input + sizeof(r) && input < dest + out.size()))
    return {E::invalid_backing, {}};
  Fields f;
  Build(f, r);
  return EncodeCatalogValueBlockInto(SchemaFor(r), f.View(), out);
}
template <class R>
CatalogValueEncodeResult EncodeOwning(const R& r) {
  const auto size = EncodedBytes(r);
  if (!size) return {E::invalid_value, {}};
  std::vector<byte> bytes(*size);
  const auto encoded = EncodeInto(r, bytes);
  if (!encoded.ok() || encoded.bytes.size() != bytes.size()) return {encoded.ok() ? E::invalid_value : encoded.error, {}};
  return {E::none, std::move(bytes)};
}

template <std::size_t N>
std::size_t KeyIndex(const std::string_view (&keys)[N], std::string_view key) {
  for (std::size_t i = 0; i < N; ++i)
    if (keys[i] == key) return i;
  return N;
}
}  // namespace

bool NativeAgentPolicyRecord::operator==(const NativeAgentPolicyRecord& o) const noexcept {
  return definition_uuid == o.definition_uuid && database_uuid == o.database_uuid && policy_uuid == o.policy_uuid &&
         SameTyped(origin_transaction_uuid, o.origin_transaction_uuid) &&
         origin_local_transaction_id == o.origin_local_transaction_id && generation == o.generation &&
         agent_type == o.agent_type && policy_family == o.policy_family && policy_class == o.policy_class &&
         policy_generation == o.policy_generation && layer == o.layer && deployment == o.deployment &&
         scope == o.scope && authority == o.authority && enabled == o.enabled && activation == o.activation &&
         action_mode == o.action_mode && invalid_policy_behavior == o.invalid_policy_behavior &&
         allow_live_action == o.allow_live_action && require_manual_approval == o.require_manual_approval &&
         require_dry_run_before_live == o.require_dry_run_before_live && evidence_required == o.evidence_required &&
         explainability_required == o.explainability_required && editable_seed_policy == o.editable_seed_policy &&
         run_interval_microseconds == o.run_interval_microseconds && jitter_microseconds == o.jitter_microseconds &&
         lease_microseconds == o.lease_microseconds && cooldown_microseconds == o.cooldown_microseconds &&
         max_runtime_microseconds == o.max_runtime_microseconds && max_restart_attempts == o.max_restart_attempts &&
         initial_backoff_microseconds == o.initial_backoff_microseconds &&
         max_backoff_microseconds == o.max_backoff_microseconds && max_history_query_rows == o.max_history_query_rows &&
         max_evidence_fanout == o.max_evidence_fanout && max_label_cardinality == o.max_label_cardinality &&
         action_budget_per_window == o.action_budget_per_window &&
         required_metric_families == o.required_metric_families && policy_dependencies == o.policy_dependencies &&
         startup_verification == o.startup_verification;
}
bool NativeAgentRegistrationRecord::operator==(const NativeAgentRegistrationRecord& o) const noexcept {
  return definition_uuid == o.definition_uuid && database_uuid == o.database_uuid && policy_uuid == o.policy_uuid &&
         SameTyped(origin_transaction_uuid, o.origin_transaction_uuid) &&
         origin_local_transaction_id == o.origin_local_transaction_id && generation == o.generation &&
         agent_type == o.agent_type && policy_families == o.policy_families && state == o.state &&
         managed_by == o.managed_by && launch_trigger == o.launch_trigger &&
         starts_during_create == o.starts_during_create;
}

// ---- policy ----
std::optional<std::size_t> NativeAgentPolicyEncodedBytes(const NativeAgentPolicyRecord& r) { return EncodedBytes(r); }
CatalogValueEncodeIntoResult EncodeNativeAgentPolicyInto(const NativeAgentPolicyRecord& r, std::span<byte> out) {
  return EncodeInto(r, out);
}
CatalogValueEncodeResult EncodeNativeAgentPolicy(const NativeAgentPolicyRecord& r) { return EncodeOwning(r); }
NativeAgentDecodeResult<NativeAgentPolicyRecord> DecodeNativeAgentPolicy(std::string_view bytes) {
  std::array<CatalogValueFieldView, kMaxFields> backing;
  const auto d = DecodeCatalogValueBlockInto(policy_schema, Bytes(bytes), backing);
  if (!d.ok()) return {d.error, {}};
  NativeAgentPolicyRecord r;
  CatalogAgentStartupVerificationSelection startup;
  unsigned startup_fields = 0;
  for (const auto& f : d.fields) {
    bool ok = true;
    switch (f.id) {
      case 1: r.definition_uuid = f.identity()->value; break;
      case 2: r.database_uuid = f.identity()->value; break;
      case 3: r.policy_uuid = f.identity()->value; break;
      case 4: r.origin_transaction_uuid = *f.identity(); break;
      case 5: r.origin_local_transaction_id = *f.unsigned_value(); break;
      case 6: r.generation = *f.unsigned_value(); break;
      case 7: ok = DecodeEnum(f, r.agent_type); break;
      case 8: ok = DecodeEnum(f, r.policy_family); break;
      case 9: ok = DecodeEnum(f, r.policy_class); break;
      case 10: r.policy_generation = *f.unsigned_value(); break;
      case 11: ok = DecodeEnum(f, r.layer); break;
      case 12: ok = DecodeEnum(f, r.deployment); break;
      case 13: r.scope = static_cast<CatalogAgentScope>(*f.unsigned_value()); break;  // u64 bit set
      case 14: ok = DecodeEnum(f, r.authority); break;
      case 15: r.enabled = Flag(f); break;
      case 16: ok = DecodeEnum(f, r.activation); break;
      case 17: ok = DecodeEnum(f, r.action_mode); break;
      case 18: ok = DecodeEnum(f, r.invalid_policy_behavior); break;
      case 37: ok = DecodeList(f, r.required_metric_families); break;
      case 38: ok = DecodeList(f, r.policy_dependencies); break;
      case 39: startup.repair_policy_uuid = f.identity()->value; ++startup_fields; break;
      case 40: startup.quarantine_policy_uuid = f.identity()->value; ++startup_fields; break;
      case 41: startup.salvage_policy_uuid = f.identity()->value; ++startup_fields; break;
      case 42: startup.evidence_policy_uuid = f.identity()->value; ++startup_fields; break;
      case 43: startup.verification_levels = *f.unsigned_value(); ++startup_fields; break;
      default:
        if (f.id >= 19 && f.id <= 24) r.*kPolicyFlags[f.id - 19] = Flag(f);
        else if (f.id >= 25 && f.id <= 36) r.*kPolicyNumbers[f.id - 25] = *f.unsigned_value();
        else ok = false;
    }
    if (!ok) return {E::invalid_value, {}};
  }
  if (startup_fields != 0 && startup_fields != 5) return {E::invalid_value, {}};
  if (startup_fields) r.startup_verification = startup;
  if (!ValidRecord(r)) return {E::invalid_value, {}};
  return {E::none, r};
}

// ---- registration ----
std::optional<std::size_t> NativeAgentRegistrationEncodedBytes(const NativeAgentRegistrationRecord& r) {
  return EncodedBytes(r);
}
CatalogValueEncodeIntoResult EncodeNativeAgentRegistrationInto(const NativeAgentRegistrationRecord& r,
                                                                std::span<byte> out) {
  return EncodeInto(r, out);
}
CatalogValueEncodeResult EncodeNativeAgentRegistration(const NativeAgentRegistrationRecord& r) {
  return EncodeOwning(r);
}
NativeAgentDecodeResult<NativeAgentRegistrationRecord> DecodeNativeAgentRegistration(std::string_view bytes) {
  std::array<CatalogValueFieldView, kMaxFields> backing;
  const auto d = DecodeCatalogValueBlockInto(registration_schema, Bytes(bytes), backing);
  if (!d.ok()) return {d.error, {}};
  NativeAgentRegistrationRecord r;
  for (const auto& f : d.fields) {
    bool ok = true;
    switch (f.id) {
      case 1: r.definition_uuid = f.identity()->value; break;
      case 2: r.database_uuid = f.identity()->value; break;
      case 3: r.policy_uuid = f.identity()->value; break;
      case 4: r.origin_transaction_uuid = *f.identity(); break;
      case 5: r.origin_local_transaction_id = *f.unsigned_value(); break;
      case 6: r.generation = *f.unsigned_value(); break;
      case 7: ok = DecodeEnum(f, r.agent_type); break;
      case 8: ok = DecodeList(f, r.policy_families); break;
      case 9: ok = DecodeEnum(f, r.state); break;
      case 10: ok = DecodeEnum(f, r.managed_by); break;
      case 11: ok = DecodeEnum(f, r.launch_trigger); break;
      case 12: r.starts_during_create = Flag(f); break;
      default: ok = false;
    }
    if (!ok) return {E::invalid_value, {}};
  }
  if (!ValidRecord(r)) return {E::invalid_value, {}};
  return {E::none, r};
}

// ---- header binding / selection ----
bool NativeAgentPolicyMatchesHeader(const CatalogTypedRecordView& record) {
  const auto decoded = DecodeNativeAgentPolicy(record.payload);
  return decoded.ok() && record.header.kind == CatalogRecordKind::policy &&
         record.header.object_uuid.kind == kObj && record.header.object_uuid.value == decoded.record->definition_uuid;
}
bool NativeAgentRegistrationMatchesHeader(const CatalogTypedRecordView& record) {
  const auto decoded = DecodeNativeAgentRegistration(record.payload);
  return decoded.ok() && record.header.kind == CatalogRecordKind::config_profile &&
         record.header.object_uuid.kind == kObj && record.header.object_uuid.value == decoded.record->definition_uuid;
}
bool IsNativeAgentPayload(std::string_view bytes) noexcept {
  if (bytes.size() < kCatalogValueBlockHeaderBytes || bytes.substr(0, 4) != "SBCV") return false;
  const auto id = platform::LoadLittle32(reinterpret_cast<const byte*>(bytes.data()) + 16);
  return id >= kNativeAgentFirstSchemaId && id <= kNativeAgentLastSchemaId;
}

// ---- boundary adapters ----
NativeAgentTagResult<NativeAgentPolicyRecord> NativeAgentPolicyFromTags(std::span<const NativeAgentPropertyTag> tags) {
  // Every non-config key the builder writes and the native reader requires.
  static constexpr std::string_view keys[]{
      "policy_name", "policy_class", "policy_family", "policy_generation", "agent_type", "layer", "deployment",
      "scope", "authority", "enabled", "activation", "action_mode", "invalid_policy_behavior",
      "required_metric_families", "policy_dependencies"};
  constexpr std::size_t kKeys = std::size(keys), kTotal = kKeys + 6 + 12;
  NativeAgentPolicyRecord r;
  std::array<bool, kTotal> seen{};
  for (const auto& t : tags) {
    if (t.key.starts_with("config.")) continue;  // the family config record's
    std::size_t index = KeyIndex(keys, t.key);
    if (index == kKeys) {
      const auto flag = KeyIndex(kPolicyFlagKeys, t.key);
      const auto number = KeyIndex(kPolicyNumberKeys, t.key);
      index = flag < 6 ? kKeys + flag : number < 12 ? kKeys + 6 + number : kTotal;
    }
    if (index == kTotal || seen[index]) return {{}, t.key};
    seen[index] = true;
    const auto v = t.value;
    bool ok = false;
    switch (index) {
      case 0: ok = !v.empty(); break;  // policy_name: names table; NOT carried
      case 1: ok = ParseEnum(v, &CatalogAgentPolicyClassForTag, r.policy_class); break;
      case 2: ok = ParseEnum(v, &CatalogAgentPolicyFamilyForTag, r.policy_family); break;
      case 3: ok = ParseU64(v, r.policy_generation); break;
      case 4: ok = ParseEnum(v, &CatalogAgentTypeForTag, r.agent_type); break;
      case 5: ok = ParseEnum(v, &CatalogAgentRuntimeLayerForTag, r.layer); break;
      case 6: ok = ParseEnum(v, &CatalogAgentDeploymentForTag, r.deployment); break;
      case 7: {
        const auto scope = CatalogAgentScopeForPath(v);
        ok = scope.has_value();
        if (ok) r.scope = *scope;
        break;
      }
      case 8: ok = ParseEnum(v, &CatalogAgentAuthorityClassForTag, r.authority); break;
      case 9: ok = ParseFlag01(v, r.enabled); break;
      case 10: ok = ParseEnum(v, &CatalogAgentActivationProfileForTag, r.activation); break;
      case 11: ok = ParseEnum(v, &CatalogAgentActionModeForTag, r.action_mode); break;
      case 12: ok = ParseEnum(v, &CatalogAgentInvalidPolicyBehaviorForTag, r.invalid_policy_behavior); break;
      case 13: ok = ParseList(v, &CatalogMetricFamilyForTag, r.required_metric_families); break;
      case 14: ok = ParseList(v, &CatalogAgentPolicyFamilyForTag, r.policy_dependencies); break;
      default:
        if (index < kKeys + 6) ok = ParseFlag01(v, r.*kPolicyFlags[index - kKeys]);
        else ok = ParseU64(v, r.*kPolicyNumbers[index - kKeys - 6]);
    }
    if (!ok) return {{}, t.key};
  }
  for (std::size_t i = 0; i < kTotal; ++i)
    if (!seen[i]) return {{}, i < kKeys ? keys[i] : i < kKeys + 6 ? kPolicyFlagKeys[i - kKeys] : kPolicyNumberKeys[i - kKeys - 6]};
  return {r, {}};
}
NativeAgentTagResult<NativeAgentRegistrationRecord> NativeAgentRegistrationFromTags(
    std::span<const NativeAgentPropertyTag> tags) {
  static constexpr std::string_view keys[]{"profile_name", "profile_class", "agent_type", "policy_families",
                                           "state", "managed_by", "launch_trigger", "starts_during_create"};
  NativeAgentRegistrationRecord r;
  std::array<bool, std::size(keys)> seen{};
  for (const auto& t : tags) {
    const auto index = KeyIndex(keys, t.key);
    if (index == std::size(keys) || seen[index]) return {{}, t.key};
    seen[index] = true;
    const auto v = t.value;
    bool ok = false;
    switch (index) {
      case 0: ok = !v.empty(); break;  // profile_name: names table; NOT carried
      case 1: ok = v == "agent_registration"; break;  // the record kind says it
      case 2: ok = ParseEnum(v, &CatalogAgentTypeForTag, r.agent_type); break;
      case 3: ok = ParseList(v, &CatalogAgentPolicyFamilyForTag, r.policy_families); break;
      case 4: ok = ParseEnum(v, &CatalogAgentRegistrationStateForTag, r.state); break;
      case 5: ok = ParseEnum(v, &CatalogAgentManagedByForTag, r.managed_by); break;
      case 6: ok = ParseEnum(v, &CatalogAgentLaunchTriggerForTag, r.launch_trigger); break;
      case 7: ok = ParseFlag01(v, r.starts_during_create); break;
    }
    if (!ok) return {{}, t.key};
  }
  for (std::size_t i = 0; i < std::size(keys); ++i)
    if (!seen[i]) return {{}, keys[i]};
  return {r, {}};
}

// ---- config: descriptors, codec, adapter ----
std::span<const NativeAgentConfigFieldDescriptor> NativeAgentConfigFieldsFor(CatalogAgentPolicyFamily family) noexcept {
  std::span<const NativeAgentConfigFieldDescriptor> out;
  WithNativeAgentConfigType(family, [&](auto type) {
    out = FieldsOf(static_cast<const typename decltype(type)::type*>(nullptr));
  });
  return out;
}
std::span<const NativeAgentConfigFieldDescriptor> NativeAgentConfigCommonFields() noexcept { return kCommonFields; }
std::span<const std::string_view> NativeAgentConfigUnrepresentedKeys(CatalogAgentPolicyFamily family) noexcept {
  std::span<const std::string_view> out;
  WithNativeAgentConfigType(family, [&](auto type) {
    out = UnrepresentedOf(static_cast<const typename decltype(type)::type*>(nullptr));
  });
  return out;
}

template <class R>
std::optional<std::size_t> NativeAgentConfigEncodedBytes(const R& r) { return EncodedBytes(r); }
template <class R>
CatalogValueEncodeIntoResult EncodeNativeAgentConfigInto(const R& r, std::span<byte> out) { return EncodeInto(r, out); }
template <class R>
CatalogValueEncodeResult EncodeNativeAgentConfig(const R& r) { return EncodeOwning(r); }
template <class R>
NativeAgentDecodeResult<R> DecodeNativeAgentConfig(std::string_view bytes) {
  std::array<CatalogValueFieldView, kMaxFields> backing;
  const auto d = DecodeCatalogValueBlockInto(SchemaOf(static_cast<const R*>(nullptr)), Bytes(bytes), backing);
  if (!d.ok()) return {d.error, {}};
  R r;
  for (const auto& f : d.fields) {
    switch (f.id) {
      case 1: r.definition_uuid = f.identity()->value; continue;
      case 2: r.origin_transaction_uuid = *f.identity(); continue;
      case 3: r.origin_local_transaction_id = *f.unsigned_value(); continue;
      default: break;
    }
    const int family = DecodeFamily(f, r);
    if (family == 1) continue;
    if (family < 0 || !DecodeCommon(f, r.common)) return {E::invalid_value, {}};
  }
  if (!ValidRecord(r)) return {E::invalid_value, {}};
  return {E::none, r};
}
template <class R>
NativeAgentTagResult<R> NativeAgentConfigFromTags(std::span<const NativeAgentPropertyTag> tags) {
  constexpr std::size_t kCommonOffset = 32;
  const auto fields = FieldsOf(static_cast<const R*>(nullptr));
  static_assert(std::size(kCommonFields) <= 32);
  R r;
  std::array<bool, kCommonOffset + 32> seen{};
  for (const auto& t : tags) {
    std::size_t index = fields.size();
    for (std::size_t i = 0; i < fields.size(); ++i)
      if (fields[i].key == t.key) index = i;
    if (index < fields.size()) {
      if (index >= kCommonOffset || seen[index] || !AssignFamily(r, index, t.value)) return {{}, t.key};
      seen[index] = true;
      continue;
    }
    std::size_t common = std::size(kCommonFields);
    for (std::size_t i = 0; i < std::size(kCommonFields); ++i)
      if (kCommonFields[i].key == t.key) common = i;
    if (common == std::size(kCommonFields) || seen[kCommonOffset + common] || !AssignCommon(r.common, common, t.value))
      return {{}, t.key};
    seen[kCommonOffset + common] = true;
  }
  for (std::size_t i = 0; i < fields.size(); ++i)
    if (fields[i].required && !seen[i]) return {{}, fields[i].key};
  return {r, {}};
}

#define SB_NATIVE_AGENT_CONFIG_INSTANTIATE(R)                                                            \
  template std::optional<std::size_t> NativeAgentConfigEncodedBytes<R>(const R&);                       \
  template CatalogValueEncodeIntoResult EncodeNativeAgentConfigInto<R>(const R&, std::span<byte>);      \
  template CatalogValueEncodeResult EncodeNativeAgentConfig<R>(const R&);                               \
  template NativeAgentDecodeResult<R> DecodeNativeAgentConfig<R>(std::string_view);                     \
  template NativeAgentTagResult<R> NativeAgentConfigFromTags<R>(std::span<const NativeAgentPropertyTag>);
SB_NATIVE_AGENT_CONFIG_TYPES(SB_NATIVE_AGENT_CONFIG_INSTANTIATE)
#undef SB_NATIVE_AGENT_CONFIG_INSTANTIATE
#undef SB_NATIVE_AGENT_CONFIG_TYPES
}  // namespace scratchbird::core::catalog
