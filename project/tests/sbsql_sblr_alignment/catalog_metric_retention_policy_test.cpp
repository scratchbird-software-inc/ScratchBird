// Copyright (c) 2026 ScratchBird Software Inc.
// SPDX-License-Identifier: MPL-2.0
#include "catalog_metric_retention_policy.hpp"
#include <openssl/sha.h>
#include <algorithm>
#include <array>
#include <iostream>
#include <limits>

namespace c = scratchbird::core::catalog;
namespace m = scratchbird::core::metrics;
namespace p = scratchbird::core::platform;
namespace {
unsigned checks = 0, failures = 0;

void Check(bool ok, const char* why) {
  ++checks;
  if (!ok) { ++failures; std::cerr << "FAIL " << why << '\n'; }
}
bool SharedRetentionOrigin(const c::CatalogMetadataVersion& a,const c::CatalogMetadataVersion& b) {
  const bool family=c::CatalogMetricRetentionPolicyPreservesOrigin(a,b);
  const bool shared=c::CatalogMetadataPreservesFamilyOrigin(a,b);
  Check(shared==family,"shared native origin dispatcher differs from family contract");
  return family;
}
p::TypedUuid Id(p::UuidKind kind, unsigned char tag) {
  return {kind, p::Uuid{{1,2,3,4,5,6,0x71,8,0x89,10,11,12,13,14,15,tag}}};
}
void Put(std::string& s, std::size_t at, p::u64 value, unsigned width) {
  for (unsigned i = 0; i < width; ++i) s.at(at + i) = char(value >> (8 * i));
}
p::u64 Get(const std::string& s, std::size_t at, unsigned width) {
  p::u64 value = 0;
  for (unsigned i = 0; i < width; ++i) value |= p::u64(static_cast<unsigned char>(s.at(at + i))) << (8 * i);
  return value;
}
std::string Number(p::u64 value) { std::string s(8, 0); Put(s, 0, value, 8); return s; }
std::string Uuid(const p::Uuid& id) { return {reinterpret_cast<const char*>(id.bytes.data()), 16}; }
void Field(std::string& s, unsigned id, unsigned type, const std::string& bytes) {
  const auto at = s.size(); s.resize(at + 8, 0);
  Put(s, at, id, 2); Put(s, at + 2, type, 1); Put(s, at + 4, bytes.size(), 4); s += bytes;
}
// Independent spec-layout oracle: deliberately does not call production schema/encoder.
std::string Golden(const c::CatalogMetricRetentionPolicy& r) {
  const auto& v = r.policy;
  std::string s(24, 0); s.replace(0, 4, "SBCV");
  Put(s, 4, 1, 2); Put(s, 6, 24, 2); Put(s, 12, 16, 4); Put(s, 16, 65543, 4); Put(s, 20, 1, 2);
  Field(s, 1, 5, Uuid(v.policy_uuid)); Field(s, 2, 1, Number(v.generation));
  Field(s, 3, 3, v.policy_name);
  const auto scope = v.scope == "local" ? 1 : v.scope == "database" ? 2 : v.scope == "node" ? 3 : 4;
  Field(s, 4, 1, Number(scope)); Field(s, 5, 1, Number(static_cast<p::u64>(v.mode)));
  Field(s, 6, 1, Number(v.raw_retention_seconds)); Field(s, 7, 1, Number(v.rollup_retention_seconds));
  std::string grains; for (const auto grain : v.rollup_grains) grains += char(grain);
  Field(s, 8, 4, grains); Field(s, 9, 1, Number(v.purge_batch_limit)); Field(s, 10, 1, Number(v.max_cardinality));
  const auto overflow = v.overflow_behavior == "reject_and_evidence" ? 1 :
      v.overflow_behavior == "quarantine_new_series" ? 2 : 3;
  Field(s, 11, 1, Number(overflow)); Field(s, 12, 3, v.edit_right); Field(s, 13, 3, v.default_admin_group);
  Field(s, 14, 2, std::string(1, v.evidence_required ? 1 : 0));
  Field(s, 15, 5, Uuid(r.origin_transaction_uuid.value)); Field(s, 16, 1, Number(r.origin_local_transaction_id));
  Put(s, 8, s.size(), 4); return s;
}
std::size_t Offset(const std::string& s, unsigned id) {
  for (std::size_t at = 24; at < s.size(); at += 8 + Get(s, at + 4, 4))
    if (Get(s, at, 2) == id) return at;
  throw "missing oracle field";
}
c::CatalogMetricRetentionPolicy Policy() {
  c::CatalogMetricRetentionPolicy r;
  r.policy.policy_uuid = Id(p::UuidKind::object, 1).value;
  r.policy.generation = 1; r.policy.policy_name = "policy-\xc3\xa9";
  r.origin_transaction_uuid = Id(p::UuidKind::transaction, 2);
  r.origin_local_transaction_id = 11; return r;
}
c::CatalogMetadataVersion Metadata(const c::CatalogMetricRetentionPolicy& r) {
  c::CatalogMetadataVersion v;
  v.record.header.kind = c::CatalogRecordKind::policy;
  v.record.header.row_uuid = Id(p::UuidKind::row, 3);
  v.record.header.object_uuid = {p::UuidKind::object, r.policy.policy_uuid};
  v.record.header.parent_uuid = Id(p::UuidKind::object, 4);
  v.record.payload = Golden(r);
  v.owning_schema_uuid = Id(p::UuidKind::schema, 4);
  v.owner_uuid = Id(p::UuidKind::principal, 5); v.audit_uuid = Id(p::UuidKind::object, 6);
  v.default_name_uuid = Id(p::UuidKind::object, 7); v.name_vector_uuid = Id(p::UuidKind::object, 8);
  v.creator_transaction_uuid = r.origin_transaction_uuid;
  v.creator_local_transaction_id = r.origin_local_transaction_id;
  v.definition_version = r.policy.generation;
  v.schema_epoch = v.security_epoch = v.catalog_generation = v.dependency_generation = v.invalidation_generation = 1;
  v.lifecycle = c::CatalogObjectLifecycle::active; v.status = c::CatalogObjectStatus::active;
  v.trace_search_key = "RETENTION-POLICY-ORACLE"; v.object_subtype = "metric_retention"; v.retention_class = "catalog_history";
  if (r.policy.scope == "cluster") v.authority_scope = c::CatalogAuthorityScope::cluster;
  return v;
}
void Refused(std::string_view bytes) {
  const auto result = c::DecodeCatalogMetricRetentionPolicy(bytes);
  Check(!result.ok() && !result.record, "malformed payload published partial policy");
}
void RoundTrip(const c::CatalogMetricRetentionPolicy& r) {
  const auto expected = Golden(r);
  const auto encoded = c::EncodeCatalogMetricRetentionPolicy(r);
  Check(encoded.ok() && std::string(encoded.bytes.begin(), encoded.bytes.end()) == expected, "independent byte oracle mismatch");
  const auto decoded = c::DecodeCatalogMetricRetentionPolicy(expected);
  Check(decoded.ok() && Golden(*decoded.record) == expected, "native policy decode mismatch");
  const auto metadata = Metadata(r);
  Check(c::CatalogMetricRetentionPolicyMatchesMetadata(metadata), "valid metadata binding refused");
  const auto wrapped = c::EncodeCatalogMetadataVersion(metadata);
  Check(wrapped.ok(), "actual native envelope refused");
  if (wrapped.ok()) {
    const auto reread = c::DecodeCatalogMetadataVersion(wrapped.bytes);
    Check(reread.ok() && reread.record.record.payload == expected, "actual native envelope lost policy");
  }
}
// Rehash independently so decode refusals cannot be attributed to digest mismatch.
void Reseal(std::vector<p::byte>& bytes) {
  std::fill(bytes.begin() + 320, bytes.begin() + 352, 0);
  std::array<unsigned char, 32> hash{};
  SHA256(bytes.data(), bytes.size(), hash.data());
  std::copy(hash.begin(), hash.end(), bytes.begin() + 320);
}
void Binding() {
  const auto initial = Metadata(Policy());
  for (unsigned which = 0; which < 15; ++which) {
    auto v = initial;
    switch (which) {
      case 0: v.record.header.object_uuid.value.bytes[15]++; break;
      case 1: v.record.header.object_uuid.kind = p::UuidKind::schema; break;
      case 2: v.record.header.kind = c::CatalogRecordKind::table_descriptor; break;
      case 3: v.object_subtype = "other_policy"; break;
      case 4: v.definition_version++; break;
      case 5: v.record.header.parent_uuid.value.bytes[15]++; break;
      case 6: v.record.header.parent_uuid.kind = p::UuidKind::schema; break;
      case 7: v.owning_schema_uuid = {}; break;
      case 8: v.default_name_uuid = {}; break;
      case 9: v.name_vector_uuid = {}; break;
      case 10: v.authority_scope = c::CatalogAuthorityScope::cluster; break;
      case 11: v.creator_transaction_uuid.value.bytes[15]++; break;
      case 12: v.creator_local_transaction_id--; break;
      case 13: v.creator_local_transaction_id++; break;
      case 14: v.record.payload = "policy_uuid=seed-metrics-current-only\n"; break;
    }
    Check(!c::CatalogMetricRetentionPolicyMatchesMetadata(v), "invalid family/common binding accepted");
    const auto result = c::EncodeCatalogMetadataVersion(v);
    Check(!result.ok() && result.bytes.empty(), "envelope admitted mismatched policy");
  }
  auto v = initial; v.catalog_generation = 99;
  Check(c::EncodeCatalogMetadataVersion(v).ok(), "definition generation confused with catalog generation");
  const auto wrapped = c::EncodeCatalogMetadataVersion(initial);
  if (wrapped.ok()) {
    auto resealed = wrapped.bytes; Reseal(resealed);
    Check(c::DecodeCatalogMetadataVersion(resealed).ok(), "independent reseal oracle invalid");
    // Mutate common definition generation and creator identity, resealing digest.
    for (const std::size_t at : {std::size_t(32), std::size_t(127)}) {
      auto bytes = wrapped.bytes; bytes[at]++; Reseal(bytes);
      Check(!c::DecodeCatalogMetadataVersion(bytes).ok(), "rehash bypassed binding validation");
    }
  }
  auto next = Policy(); next.policy.generation = 2;
  auto successor = Metadata(next);
  successor.creator_transaction_uuid = Id(p::UuidKind::transaction, 12);
  successor.creator_local_transaction_id = 12;
  Check(c::EncodeCatalogMetadataVersion(successor).ok() &&
        SharedRetentionOrigin(initial, successor), "valid successor refused");
  successor.record.header.deleted = true; successor.lifecycle = c::CatalogObjectLifecycle::dropped;
  successor.status = c::CatalogObjectStatus::retired;
  successor.retired_transaction_uuid = successor.creator_transaction_uuid;
  Check(c::EncodeCatalogMetadataVersion(successor).ok() &&
        SharedRetentionOrigin(initial, successor), "retirement lost original creation");
  for (unsigned which = 0; which < 4; ++which) {
    auto changed = next;
    if (which == 0) changed.origin_transaction_uuid.value.bytes[15]++;
    if (which == 1) changed.origin_local_transaction_id--;
    if (which == 2) changed.policy.policy_uuid.bytes[15]++;
    if (which == 3) changed.policy.scope = "node";
    auto other = Metadata(changed);
    other.creator_transaction_uuid = successor.creator_transaction_uuid; other.creator_local_transaction_id = 12;
    Check(c::EncodeCatalogMetadataVersion(other).ok(), "individually valid changed-origin fixture rejected");
    Check(!SharedRetentionOrigin(initial, other), "successor changed immutable origin");
  }
  auto other = initial; other.object_subtype = "generic"; other.record.payload = "unrelated-family";
  Check(!SharedRetentionOrigin(initial, other) &&
        !SharedRetentionOrigin(other, initial), "family swap bypassed origin");
  Check(SharedRetentionOrigin(other, other), "unrelated policy family redefined");
  const auto typed = c::EncodeCatalogTypedRecord(initial.record, 3);
  Check(typed.ok() && c::DecodeCatalogTypedRecord(typed.row).ok(), "typed policy header round trip failed");
  for (unsigned which = 0; which < 3; ++which) {
    auto record = initial.record;
    if (which == 0) record.header.kind = c::CatalogRecordKind::table_descriptor;
    if (which == 1) record.header.object_uuid.value.bytes[15]++;
    if (which == 2) Put(record.payload, Offset(record.payload, 2) + 8, 0, 8);
    Check(!c::EncodeCatalogTypedRecord(record, 3).ok(), "typed header admitted invalid binary policy");
  }
  if (typed.ok()) {
    auto row = typed.row;
    row.payload[71] ^= 1; // Common object UUID, without a text adapter.
    Check(!c::DecodeCatalogTypedRecord(row).ok(), "typed decoder lost family/object binding");
  }
}
void Malformed() {
  const auto golden = Golden(Policy());
  for (std::size_t length = 0; length < golden.size(); ++length) Refused(std::string_view(golden).substr(0, length));
  Refused(golden + "x"); Refused("policy_uuid=seed-metrics-current-only\n");
  for (const unsigned at : {0u, 4u, 6u, 8u, 12u, 16u, 20u, 22u}) {
    auto bytes = golden; bytes[at] ^= 1; Refused(bytes);
  }
  for (unsigned id = 1; id <= 16; ++id) {
    const auto at = Offset(golden, id);
    auto bytes = golden; Put(bytes, at, 100, 2); Refused(bytes);
    bytes = golden; Put(bytes, at + 2, 255, 1); Refused(bytes);
    bytes = golden; Put(bytes, at + 3, 1, 1); Refused(bytes);
    bytes = golden; Put(bytes, at + 4, 0xffffffff, 4); Refused(bytes);
    bytes = golden; bytes.erase(at, 8 + Get(bytes, at + 4, 4));
    Put(bytes, 8, bytes.size(), 4); Put(bytes, 12, 15, 4); Refused(bytes);
    bytes = golden; const auto field = bytes.substr(at, 8 + Get(bytes, at + 4, 4));
    bytes.insert(at, field); Put(bytes, 8, bytes.size(), 4); Put(bytes, 12, 17, 4); Refused(bytes);
  }
  for (unsigned id : {1u, 15u}) {
    const auto at = Offset(golden, id) + 8;
    for (unsigned version = 0; version < 16; ++version) if (version != 7) {
      auto bytes = golden; bytes[at + 6] = char(version << 4); Refused(bytes);
    }
    for (unsigned variant : {0u, 0x40u, 0xc0u}) {
      auto bytes = golden; bytes[at + 8] = char(variant); Refused(bytes);
    }
    auto bytes = golden; bytes.replace(at, 16, std::string(16, 0)); Refused(bytes);
  }
  for (unsigned id : {2u, 9u, 10u, 16u}) {
    auto bytes = golden; Put(bytes, Offset(bytes, id) + 8, 0, 8); Refused(bytes);
  }
  for (unsigned id : {4u, 5u, 11u}) {
    auto bytes = golden; Put(bytes, Offset(bytes, id) + 8, 256, 8); Refused(bytes);
  }
  auto bytes = golden; Put(bytes, Offset(bytes, 14) + 8, 0, 1); Refused(bytes);
  for (unsigned id : {3u, 12u, 13u}) {
    bytes = golden; bytes[Offset(bytes, id) + 8] = '\0'; Refused(bytes);
    bytes = golden; bytes[Offset(bytes, id) + 8] = char(0xff); Refused(bytes);
  }
  auto r = Policy(); r.policy.mode = m::MetricRetentionMode::raw_and_rollup;
  r.policy.raw_retention_seconds = r.policy.rollup_retention_seconds = 1;
  r.policy.rollup_grains = {m::MetricRollupGrain::one_hour, m::MetricRollupGrain::one_hour};
  Refused(Golden(r)); Check(!c::EncodeCatalogMetricRetentionPolicy(r).ok(), "duplicate grains encoded");
  r.policy.rollup_grains = {static_cast<m::MetricRollupGrain>(255)};
  Refused(Golden(r)); Check(!c::EncodeCatalogMetricRetentionPolicy(r).ok(), "unknown grain encoded");
  r = Policy(); r.policy.raw_retention_seconds = 1; Refused(Golden(r));
  for (auto member : {&m::MetricRetentionPolicyDefinition::policy_name,
                      &m::MetricRetentionPolicyDefinition::edit_right,
                      &m::MetricRetentionPolicyDefinition::default_admin_group}) {
    r = Policy(); r.policy.*member = std::string(4096, 'x'); RoundTrip(r);
    r.policy.*member += 'x'; Refused(Golden(r));
    const auto refused = c::EncodeCatalogMetricRetentionPolicy(r);
    Check(!refused.ok() && refused.bytes.empty(), "oversize annotation encoded");
    r.policy.*member = ""; Refused(Golden(r));
  }
  r = Policy(); r.origin_transaction_uuid.kind = p::UuidKind::object;
  Check(!c::EncodeCatalogMetricRetentionPolicy(r).ok(), "wrong origin kind encoded");
}
}  // namespace
int main() {
  for (const auto scope : {"local", "database", "node", "cluster"})
    for (const auto overflow : {"reject_and_evidence", "quarantine_new_series", "overflow_only_if_not_automation"})
      for (unsigned mode = 0; mode <= 2; ++mode) {
        auto r = Policy(); r.policy.scope = scope; r.policy.overflow_behavior = overflow;
        r.policy.mode = static_cast<m::MetricRetentionMode>(mode);
        if (mode == 1) r.policy.raw_retention_seconds = std::numeric_limits<p::u64>::max();
        if (mode) {
          r.policy.rollup_retention_seconds = std::numeric_limits<p::u64>::max();
          r.policy.rollup_grains = {m::MetricRollupGrain::long_summary, m::MetricRollupGrain::one_day,
              m::MetricRollupGrain::one_hour, m::MetricRollupGrain::one_minute};
        }
        RoundTrip(r);
      }
  Binding(); Malformed();
  std::cout << "metric retention native catalog checks=" << checks << " failures=" << failures << '\n';
  return failures ? 1 : 0;
}
