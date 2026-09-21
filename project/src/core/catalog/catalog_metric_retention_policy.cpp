// Copyright (c) 2026 ScratchBird Software Inc.
// SPDX-License-Identifier: MPL-2.0
#include "catalog_metric_retention_policy.hpp"
#include "uuid.hpp"
#include <array>
#include <cstring>
#include <utility>

namespace scratchbird::core::catalog {
namespace {
constexpr std::array<std::string_view, 4> kScopes{"local", "database", "node", "cluster"};
constexpr std::array<std::string_view, 3> kOverflow{
    "reject_and_evidence", "quarantine_new_series", "overflow_only_if_not_automation"};
template<std::size_t N> u64 Code(std::string_view name, const std::array<std::string_view, N>& names) {
  for (std::size_t i = 0; i < N; ++i) if (name == names[i]) return i + 1;
  return 0;
}
bool Identity(const TypedUuid& id, UuidKind kind) {
  return id.kind == kind && uuid::IsEngineIdentityUuid(id.value);
}
bool Valid(const CatalogMetricRetentionPolicy& record) {
  return metrics::ValidateMetricRetentionPolicy(record.policy).ok &&
      Identity(record.origin_transaction_uuid, UuidKind::transaction) &&
      record.origin_local_transaction_id != 0;
}
bool IsFamily(const CatalogMetadataVersion& record) {
  return record.object_subtype == "metric_retention" ||
      IsCatalogMetricRetentionPolicyPayload(record.record.payload);
}
}  // namespace

const CatalogValueSchema& CatalogMetricRetentionPolicySchema() {
  using T = CatalogValueType;
  static const CatalogValueSchema schema{65543, 1, {
      {1, T::engine_identity, true, 16, UuidKind::object},
      {2, T::unsigned_integer, true, 8},
      {3, T::utf8_text, true, 4096},
      {4, T::unsigned_integer, true, 8},
      {5, T::unsigned_integer, true, 8},
      {6, T::unsigned_integer, true, 8},
      {7, T::unsigned_integer, true, 8},
      {8, T::opaque_bytes, true, 4},
      {9, T::unsigned_integer, true, 8},
      {10, T::unsigned_integer, true, 8},
      {11, T::unsigned_integer, true, 8},
      {12, T::utf8_text, true, 4096},
      {13, T::utf8_text, true, 4096},
      {14, T::boolean, true, 1},
      {15, T::engine_identity, true, 16, UuidKind::transaction},
      {16, T::unsigned_integer, true, 8},
  }};
  return schema;
}

CatalogValueEncodeResult EncodeCatalogMetricRetentionPolicy(const CatalogMetricRetentionPolicy& r) {
  if (!Valid(r)) return {CatalogValueError::invalid_value, {}};
  const auto& p = r.policy;
  std::vector<byte> grains;
  for (const auto grain : p.rollup_grains) grains.push_back(static_cast<byte>(grain));
  return EncodeCatalogValueBlock(CatalogMetricRetentionPolicySchema(), {
      {1, TypedUuid{UuidKind::object, p.policy_uuid}}, {2, p.generation},
      {3, p.policy_name}, {4, Code(p.scope, kScopes)}, {5, static_cast<u64>(p.mode)},
      {6, p.raw_retention_seconds}, {7, p.rollup_retention_seconds}, {8, std::move(grains)},
      {9, p.purge_batch_limit}, {10, p.max_cardinality}, {11, Code(p.overflow_behavior, kOverflow)},
      {12, p.edit_right}, {13, p.default_admin_group}, {14, p.evidence_required},
      {15, r.origin_transaction_uuid}, {16, r.origin_local_transaction_id},
  });
}

CatalogMetricRetentionPolicyResult DecodeCatalogMetricRetentionPolicy(std::string_view bytes) {
  if (bytes.size() > kCatalogValueBlockMaxBytes)
    return {CatalogValueError::size_limit, {}};
  const auto decoded = DecodeCatalogValueBlock(CatalogMetricRetentionPolicySchema(),
      std::vector<byte>(bytes.begin(), bytes.end()));
  if (!decoded.ok()) return {decoded.error, {}};
  const auto& f = decoded.fields;
  const auto scope = std::get<u64>(f[3].value);
  const auto mode = std::get<u64>(f[4].value);
  const auto overflow = std::get<u64>(f[10].value);
  if (scope == 0 || scope > kScopes.size() || mode > 2 ||
      overflow == 0 || overflow > kOverflow.size())
    return {CatalogValueError::invalid_value, {}};
  CatalogMetricRetentionPolicy r;
  auto& p = r.policy;
  p.policy_uuid = std::get<TypedUuid>(f[0].value).value;
  p.generation = std::get<u64>(f[1].value);
  p.policy_name = std::get<std::string>(f[2].value);
  p.scope = kScopes[scope - 1];
  p.mode = static_cast<metrics::MetricRetentionMode>(mode);
  p.raw_retention_seconds = std::get<u64>(f[5].value);
  p.rollup_retention_seconds = std::get<u64>(f[6].value);
  for (const auto grain : std::get<std::vector<byte>>(f[7].value)) {
    if (grain > 3) return {CatalogValueError::invalid_value, {}};
    p.rollup_grains.push_back(static_cast<metrics::MetricRollupGrain>(grain));
  }
  p.purge_batch_limit = std::get<u64>(f[8].value);
  p.max_cardinality = std::get<u64>(f[9].value);
  p.overflow_behavior = kOverflow[overflow - 1];
  p.edit_right = std::get<std::string>(f[11].value);
  p.default_admin_group = std::get<std::string>(f[12].value);
  p.evidence_required = std::get<bool>(f[13].value);
  r.origin_transaction_uuid = std::get<TypedUuid>(f[14].value);
  r.origin_local_transaction_id = std::get<u64>(f[15].value);
  if (!Valid(r)) return {CatalogValueError::invalid_value, {}};
  return {CatalogValueError::none, std::move(r)};
}

bool IsCatalogMetricRetentionPolicyPayload(std::string_view bytes) {
  return bytes.size() >= kCatalogValueBlockHeaderBytes && bytes.substr(0, 4) == "SBCV" &&
      platform::LoadLittle32(reinterpret_cast<const byte*>(bytes.data()) + 16) == 65543;
}
bool CatalogMetricRetentionPolicyMatchesHeader(const CatalogTypedRecord& r) {
  if (r.header.kind != CatalogRecordKind::policy ||
      !Identity(r.header.object_uuid, UuidKind::object)) return false;
  const auto decoded = DecodeCatalogMetricRetentionPolicy(r.payload);
  return decoded.ok() && decoded.record->policy.policy_uuid == r.header.object_uuid.value;
}
bool CatalogMetricRetentionPolicyMatchesMetadata(const CatalogMetadataVersion& m) {
  if (!CatalogMetricRetentionPolicyMatchesHeader(m.record) ||
      m.object_subtype != "metric_retention" ||
      !Identity(m.owning_schema_uuid, UuidKind::schema) ||
      !Identity(m.record.header.parent_uuid, UuidKind::object) ||
      m.owning_schema_uuid.value != m.record.header.parent_uuid.value ||
      !Identity(m.default_name_uuid, UuidKind::object) ||
      !Identity(m.name_vector_uuid, UuidKind::object) ||
      !Identity(m.creator_transaction_uuid, UuidKind::transaction)) return false;
  const auto decoded = DecodeCatalogMetricRetentionPolicy(m.record.payload);
  const auto& r = *decoded.record;
  const auto scope = r.policy.scope == "cluster" ? CatalogAuthorityScope::cluster : CatalogAuthorityScope::local;
  return r.policy.generation == m.definition_version && m.authority_scope == scope &&
      r.origin_local_transaction_id <= m.creator_local_transaction_id &&
      (m.definition_version != 1 ||
       (r.origin_transaction_uuid.value == m.creator_transaction_uuid.value &&
        r.origin_local_transaction_id == m.creator_local_transaction_id));
}
bool CatalogMetricRetentionPolicyPreservesOrigin(
    const CatalogMetadataVersion& previous, const CatalogMetadataVersion& successor) {
  if (!IsFamily(previous) && !IsFamily(successor)) return true;
  if (!CatalogMetricRetentionPolicyMatchesMetadata(previous) ||
      !CatalogMetricRetentionPolicyMatchesMetadata(successor)) return false;
  const auto a = DecodeCatalogMetricRetentionPolicy(previous.record.payload);
  const auto b = DecodeCatalogMetricRetentionPolicy(successor.record.payload);
  return a.record->policy.policy_uuid == b.record->policy.policy_uuid &&
      a.record->policy.scope == b.record->policy.scope &&
      a.record->origin_transaction_uuid.value == b.record->origin_transaction_uuid.value &&
      a.record->origin_local_transaction_id == b.record->origin_local_transaction_id;
}
}  // namespace scratchbird::core::catalog
