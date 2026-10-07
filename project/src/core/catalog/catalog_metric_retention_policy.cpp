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
bool IsFamily(const CatalogMetadataVersionView& record) {
  return record.object_subtype == "metric_retention" ||
      IsCatalogMetricRetentionPolicyPayload(record.record.payload);
}
}  // namespace

constexpr CatalogValueSchemaView CatalogMetricRetentionPolicySchemaView() {
  using T = CatalogValueType;
  static constexpr CatalogValueFieldSchema fields[]{
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
  };
  return {65543, 1, fields};
}
const CatalogValueSchema& CatalogMetricRetentionPolicySchema() {
  static const CatalogValueSchema schema = [] {
    const auto view = CatalogMetricRetentionPolicySchemaView();
    return CatalogValueSchema{view.id, view.version, {view.fields.begin(), view.fields.end()}};
  }();
  return schema;
}

CatalogValueEncodeResult EncodeCatalogMetricRetentionPolicy(const CatalogMetricRetentionPolicy& r) {
  if (!Valid(r)) return {CatalogValueError::invalid_value, {}};
  const auto& p = r.policy;
  std::vector<byte> grains;
  for (const auto grain : p.rollup_grains) grains.push_back(static_cast<byte>(grain));
  // Construct owning fields individually, keeping throwing value construction
  // in separate RAII boundaries. The allocation-fault regression covers unwind
  // here, including the former aggregate-list failure path. This also avoids
  // the initializer-list's second copy of every string/vector.
  std::vector<CatalogValueField> fields;
  fields.reserve(16);
  fields.push_back({1, TypedUuid{UuidKind::object, p.policy_uuid}});
  fields.push_back({2, p.generation});
  fields.push_back({3, p.policy_name});
  fields.push_back({4, Code(p.scope, kScopes)});
  fields.push_back({5, static_cast<u64>(p.mode)});
  fields.push_back({6, p.raw_retention_seconds});
  fields.push_back({7, p.rollup_retention_seconds});
  fields.push_back({8, std::move(grains)});
  fields.push_back({9, p.purge_batch_limit});
  fields.push_back({10, p.max_cardinality});
  fields.push_back({11, Code(p.overflow_behavior, kOverflow)});
  fields.push_back({12, p.edit_right});
  fields.push_back({13, p.default_admin_group});
  fields.push_back({14, p.evidence_required});
  fields.push_back({15, r.origin_transaction_uuid});
  fields.push_back({16, r.origin_local_transaction_id});
  return EncodeCatalogValueBlock(CatalogMetricRetentionPolicySchema(), fields);
}

CatalogMetricRetentionPolicyViewResult DecodeCatalogMetricRetentionPolicyView(std::string_view bytes) {
  if (bytes.size() > kCatalogValueBlockMaxBytes)
    return {CatalogValueError::size_limit, {}};
  std::array<CatalogValueFieldView, 16> fields;
  const auto decoded = DecodeCatalogValueBlockInto(CatalogMetricRetentionPolicySchemaView(),
      {reinterpret_cast<const byte*>(bytes.data()), bytes.size()}, fields);
  if (!decoded.ok()) return {decoded.error, {}};
  const auto& f = decoded.fields;
  const auto scope = *f[3].unsigned_value(), mode = *f[4].unsigned_value();
  const auto overflow = *f[10].unsigned_value();
  if (scope == 0 || scope > kScopes.size() || mode > 2 ||
      overflow == 0 || overflow > kOverflow.size())
    return {CatalogValueError::invalid_value, {}};
  const auto text = [&](std::size_t i) {
    return std::string_view(reinterpret_cast<const char*>(f[i].bytes.data()), f[i].bytes.size());
  };
  CatalogMetricRetentionPolicyView r;
  r.policy_uuid = f[0].identity()->value;
  r.generation = *f[1].unsigned_value();
  r.policy_name = text(2); r.scope = kScopes[scope - 1];
  r.mode = static_cast<metrics::MetricRetentionMode>(mode);
  r.raw_retention_seconds = *f[5].unsigned_value();
  r.rollup_retention_seconds = *f[6].unsigned_value();
  for (const auto grain : f[7].bytes) {
    if (grain > 3) return {CatalogValueError::invalid_value, {}};
    r.rollup_grains[r.rollup_grain_count++] = static_cast<metrics::MetricRollupGrain>(grain);
  }
  r.purge_batch_limit = *f[8].unsigned_value(); r.max_cardinality = *f[9].unsigned_value();
  r.overflow_behavior = kOverflow[overflow - 1];
  r.edit_right = text(11); r.default_admin_group = text(12);
  r.evidence_required = f[13].bytes[0] != 0;
  r.origin_transaction_uuid = *f[14].identity();
  r.origin_local_transaction_id = *f[15].unsigned_value();
  if (!r.generation || !r.origin_local_transaction_id ||
      !metrics::ValidateMetricRetentionPolicyDefinitionView({
          r.policy_name, r.scope, r.mode, r.raw_retention_seconds, r.rollup_retention_seconds,
          std::span(r.rollup_grains).first(r.rollup_grain_count), r.purge_batch_limit,
          r.max_cardinality, r.overflow_behavior, r.edit_right, r.default_admin_group,
          r.evidence_required}).ok)
    return {CatalogValueError::invalid_value, {}};
  return {CatalogValueError::none, r};
}

CatalogMetricRetentionPolicyResult DecodeCatalogMetricRetentionPolicy(std::string_view bytes) {
  const auto decoded = DecodeCatalogMetricRetentionPolicyView(bytes);
  if (!decoded.ok()) return {decoded.error, {}};
  const auto& v = *decoded.record;
  return {CatalogValueError::none, CatalogMetricRetentionPolicy{
      metrics::MetricRetentionPolicy{metrics::MetricRetentionPolicyDefinition{
          std::string(v.policy_name), std::string(v.scope), v.mode,
          v.raw_retention_seconds, v.rollup_retention_seconds,
          {v.rollup_grains.begin(), v.rollup_grains.begin() + v.rollup_grain_count},
          v.purge_batch_limit, v.max_cardinality, std::string(v.overflow_behavior),
          std::string(v.edit_right), std::string(v.default_admin_group), v.evidence_required},
          v.policy_uuid, v.generation}, v.origin_transaction_uuid, v.origin_local_transaction_id}};
}

bool IsCatalogMetricRetentionPolicyPayload(std::string_view bytes) {
  return bytes.size() >= kCatalogValueBlockHeaderBytes && bytes.substr(0, 4) == "SBCV" &&
      platform::LoadLittle32(reinterpret_cast<const byte*>(bytes.data()) + 16) == 65543;
}
bool CatalogMetricRetentionPolicyMatchesHeader(const CatalogTypedRecordView& r) {
  if (r.header.kind != CatalogRecordKind::policy ||
      !Identity(r.header.object_uuid, UuidKind::object)) return false;
  const auto decoded = DecodeCatalogMetricRetentionPolicyView(r.payload);
  return decoded.ok() && decoded.record->policy_uuid == r.header.object_uuid.value;
}
bool CatalogMetricRetentionPolicyMatchesHeader(const CatalogTypedRecord& r) {
  return CatalogMetricRetentionPolicyMatchesHeader(BorrowCatalogTypedRecord(r));
}
bool CatalogMetricRetentionPolicyMatchesMetadata(const CatalogMetadataVersionView& m) {
  if (!CatalogMetricRetentionPolicyMatchesHeader(m.record) ||
      m.object_subtype != "metric_retention" ||
      !Identity(m.owning_schema_uuid, UuidKind::schema) ||
      !Identity(m.record.header.parent_uuid, UuidKind::object) ||
      m.owning_schema_uuid.value != m.record.header.parent_uuid.value ||
      !Identity(m.default_name_uuid, UuidKind::object) ||
      !Identity(m.name_vector_uuid, UuidKind::object) ||
      !Identity(m.creator_transaction_uuid, UuidKind::transaction)) return false;
  const auto decoded = DecodeCatalogMetricRetentionPolicyView(m.record.payload);
  const auto& r = *decoded.record;
  const auto scope = r.scope == "cluster" ? CatalogAuthorityScope::cluster : CatalogAuthorityScope::local;
  return r.generation == m.definition_version && m.authority_scope == scope &&
      r.origin_local_transaction_id <= m.creator_local_transaction_id &&
      (m.definition_version != 1 ||
       (r.origin_transaction_uuid.value == m.creator_transaction_uuid.value &&
        r.origin_local_transaction_id == m.creator_local_transaction_id));
}
bool CatalogMetricRetentionPolicyPreservesOrigin(
    const CatalogMetadataVersionView& previous, const CatalogMetadataVersionView& successor) {
  if (!IsFamily(previous) && !IsFamily(successor)) return true;
  if (!CatalogMetricRetentionPolicyMatchesMetadata(previous) ||
      !CatalogMetricRetentionPolicyMatchesMetadata(successor)) return false;
  const auto a = DecodeCatalogMetricRetentionPolicyView(previous.record.payload);
  const auto b = DecodeCatalogMetricRetentionPolicyView(successor.record.payload);
  return a.record->policy_uuid == b.record->policy_uuid &&
      a.record->scope == b.record->scope &&
      a.record->origin_transaction_uuid.value == b.record->origin_transaction_uuid.value &&
      a.record->origin_local_transaction_id == b.record->origin_local_transaction_id;
}
bool CatalogMetricRetentionPolicyMatchesMetadata(const CatalogMetadataVersion& m) {
  return CatalogMetricRetentionPolicyMatchesMetadata(BorrowCatalogMetadataVersion(m));
}
bool CatalogMetricRetentionPolicyPreservesOrigin(const CatalogMetadataVersion& a, const CatalogMetadataVersion& b) {
  return CatalogMetricRetentionPolicyPreservesOrigin(BorrowCatalogMetadataVersion(a), BorrowCatalogMetadataVersion(b));
}
}  // namespace scratchbird::core::catalog
