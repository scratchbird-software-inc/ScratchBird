// Copyright (c) 2026 ScratchBird Software Inc.
// SPDX-License-Identifier: MPL-2.0
#include "dml/delete_durable_authority_codec.hpp"
#include "dml/datatype_operator_registry_projection.hpp"
#include "api_diagnostics.hpp"
#include "hash_digest.hpp"
#include "sblr_executor_row_identity_hash.hpp"
#include <array>

namespace scratchbird::engine::internal_api {
namespace {
namespace w = scratchbird::wire;
namespace projection = datatype_operator_projection;
using Bytes = std::vector<std::uint8_t>;
using Sections = std::array<Bytes, 10>;
constexpr std::string_view kDomain = "ScratchBird.DmlDelete.DurableAuthorityBundle.V1";
bool Fail(EngineApiDiagnostic* out, std::string detail) {
  if (out) *out = MakeEngineApiDiagnostic("DML.DELETE_FAILED", "sblr.dml_delete_rows.authority_bundle_invalid",
                                         std::move(detail), true);
  return false;
}
template<class T> bool Present(const T& bytes) {
  return std::any_of(bytes.begin(), bytes.end(), [](auto b) { return b != 0; });
}
void Put(Bytes& bytes, std::size_t offset, std::uint64_t value, unsigned count = 8) {
  for (unsigned n = 0; n < count; ++n) bytes[offset + n] = static_cast<std::uint8_t>(value >> (8 * n));
}
std::uint64_t Get(std::span<const std::uint8_t> bytes, std::size_t offset, unsigned count = 8) {
  std::uint64_t value = 0;
  for (unsigned n = 0; n < count; ++n) value |= std::uint64_t(bytes[offset + n]) << (8 * n);
  return value;
}
template<std::size_t N> void Put(Bytes& bytes, std::size_t offset, const std::array<std::uint8_t, N>& value) {
  std::copy(value.begin(), value.end(), bytes.begin() + offset);
}
template<std::size_t N> void Get(std::span<const std::uint8_t> bytes, std::size_t offset,
                               std::array<std::uint8_t, N>* out) {
  std::copy_n(bytes.begin() + offset, N, out->begin());
}
bool Uuid(Bytes& bytes, std::size_t offset, const std::string& text) {
  w::TypedUpdateUuid value{};
  if (!projection::TypedUuid(text, &value)) return false;
  Put(bytes, offset, value); return true;
}
std::string Uuid(std::span<const std::uint8_t> bytes, std::size_t offset) {
  w::TypedUpdateUuid value{}; Get(bytes, offset, &value); return projection::UuidText(value);
}
bool Hash(std::string_view domain, std::span<const std::uint8_t> first,
          std::span<const std::uint8_t> second, w::TypedUpdateHash* out) {
  Bytes bytes(domain.begin(), domain.end());
  bytes.insert(bytes.end(), first.begin(), first.end());
  bytes.insert(bytes.end(), second.begin(), second.end());
  const auto hash = scratchbird::core::hash::ComputeSha256Digest(bytes);
  if (!hash.ok()) return false;
  *out = hash.digest; return Present(*out);
}
bool HexHash(const std::string& text, Bytes& bytes, std::size_t offset) {
  if (text.size() != 71 || !text.starts_with("sha256:")) return false;
  const auto digit = [](char c) { return c >= '0' && c <= '9' ? c - '0' : c >= 'a' && c <= 'f' ? c - 'a' + 10 : -1; };
  bool nonzero = false;
  for (unsigned n = 0; n < 32; ++n) {
    const auto a = digit(text[7 + n * 2]), b = digit(text[8 + n * 2]);
    if (a < 0 || b < 0) return false;
    bytes[offset + n] = static_cast<std::uint8_t>(a * 16 + b);
    nonzero |= bytes[offset + n] != 0;
  }
  return nonzero;
}
std::string HexHash(std::span<const std::uint8_t> bytes, std::size_t offset) {
  w::TypedUpdateHash hash{}; Get(bytes, offset, &hash); return "sha256:" + scratchbird::core::hash::HexLower(hash);
}
bool Security(const EngineSecurityPolicySnapshotAuthorityV1& s,
              const std::vector<std::string>& grants, Bytes* out) {
  if (!s.snapshot_generation || !s.security_context_generation || !s.security_generation ||
      !s.policy_generation || !s.admitted_policy_rows.empty() || grants.size() > 1024) return false;
  out->assign(128 + grants.size() * 16, 0);
  auto& b = *out;
  if (!Uuid(b, 0, s.snapshot_uuid) || !Uuid(b, 24, s.authenticated_statement_receipt_uuid) ||
      !Uuid(b, 40, s.security_context_uuid) || !Uuid(b, 80, s.target_relation_uuid)) return false;
  Put(b, 16, s.snapshot_generation); Put(b, 56, s.security_context_generation);
  Put(b, 64, s.security_generation); Put(b, 72, s.policy_generation); Put(b, 96, grants.size(), 4);
  std::string prior;
  for (std::size_t n = 0; n < grants.size(); ++n) {
    if ((!prior.empty() && grants[n] <= prior) || !Uuid(b, 128 + n * 16, grants[n])) return false;
    prior = grants[n];
  }
  return true;
}
bool Effects(const EngineDmlDeleteEffectSnapshotV1& s, Bytes* out) {
  if (!Present(s.snapshot_uuid) || !s.generation || !Present(s.target_relation_uuid) || !s.target_relation_generation ||
      !Present(s.relation_descriptor_uuid) || !s.relation_descriptor_generation || !Present(s.constraint_set_uuid) ||
      !Present(s.trigger_set_uuid) || s.snapshot_uuid == s.constraint_set_uuid || s.snapshot_uuid == s.trigger_set_uuid ||
      s.constraint_set_uuid == s.trigger_set_uuid || s.constraint_count || s.trigger_count || s.index_count > 1048576 ||
      !Present(s.relation_shape_sha256) || !Present(s.index_set_sha256)) return false;
  w::TypedUpdateHash empty_constraints{}, empty_triggers{};
  if (!Hash("ScratchBird.DmlDelete.NoInboundConstraintSet.V1", {}, {}, &empty_constraints) ||
      !Hash("ScratchBird.DmlDelete.NoTriggerSet.V1", {}, {}, &empty_triggers) ||
      s.constraint_set_sha256 != empty_constraints || s.trigger_set_sha256 != empty_triggers) return false;
  out->assign(256, 0); auto& b = *out;
  Put(b, 0, s.snapshot_uuid); Put(b, 16, s.generation); Put(b, 24, s.target_relation_uuid);
  Put(b, 40, s.target_relation_generation); Put(b, 48, s.relation_descriptor_uuid);
  Put(b, 64, s.relation_descriptor_generation); Put(b, 72, s.relation_shape_sha256);
  Put(b, 104, s.index_set_sha256); Put(b, 136, s.constraint_set_sha256); Put(b, 168, s.trigger_set_sha256);
  Put(b, 200, s.index_count, 4); Put(b, 212, s.constraint_set_uuid); Put(b, 228, s.trigger_set_uuid);
  return true;
}
bool Executor(const SblrExecutorAvailabilitySnapshot& s, Bytes* out) {
  if (!s.generation || !s.installed || s.availability_state != SblrExecutorAvailabilityState::installed) return false;
  const SblrExecutorAvailabilityRowIdentity expected{
      "dml.delete_rows", 784, "1.0", "dml_delete_rows_descriptor", "mutation_result", 1};
  if (s.row_identity_sha256 != HashSblrExecutorRowIdentityMaterial(expected)) return false;
  out->assign(128, 0); auto& b = *out;
  if (!Uuid(b, 0, s.snapshot_uuid) || !Uuid(b, 24, s.database_uuid) ||
      !HexHash(s.row_identity_sha256, b, 40) || !HexHash(s.decision_evidence_sha256, b, 72)) return false;
  Put(b, 16, s.generation); b[104] = b[105] = 1; return true;
}
bool DecodeSections(const std::array<std::span<const std::uint8_t>, 10>& sections,
                    DmlDeleteDurableAuthorityBundleV1* out) {
  auto& v = *out;
  w::TypedDeleteCarrierError derror; w::TypedUpdateCarrierError error;
  if (!w::DecodeAndValidateTypedDeleteDescriptor(sections[0], &v.descriptor, &derror) ||
      !w::DecodeAndValidateTypedUpdatePredicateVector(sections[1], &v.predicate, &error) ||
      !w::DecodeAndValidateTypedUpdateDatatypeAuthorityVector(sections[2], &v.datatypes, &error) ||
      !w::DecodeAndValidateTypedUpdateBuiltinOperatorAuthorityVector(sections[3], &v.operators, &error) ||
      !w::DecodeAndValidateTypedUpdateTargetOrder(sections[4], &v.target_order, &error) ||
      !w::DecodeAndValidateTypedUpdateResourceBudget(sections[5], &v.resource_budget, &error) ||
      !w::DecodeAndValidateTypedUpdateRecoveryToken(sections[6], &v.recovery, &error)) return false;
  const auto security = sections[7], effects = sections[8], executor = sections[9];
  if (security.size() < 128 || Get(security, 96, 4) > 1024 ||
      security.size() != 128 + Get(security, 96, 4) * 16 || effects.size() != 256 || executor.size() != 128) return false;
  auto& s = v.security;
  s.snapshot_uuid = Uuid(security, 0); s.snapshot_generation = Get(security, 16);
  s.authenticated_statement_receipt_uuid = Uuid(security, 24); s.security_context_uuid = Uuid(security, 40);
  s.security_context_generation = Get(security, 56); s.security_generation = Get(security, 64);
  s.policy_generation = Get(security, 72); s.target_relation_uuid = Uuid(security, 80);
  for (std::size_t offset = 128; offset < security.size(); offset += 16) v.matched_grant_uuids.push_back(Uuid(security, offset));
  auto& e = v.effects;
  Get(effects, 0, &e.snapshot_uuid); e.generation = Get(effects, 16);
  Get(effects, 24, &e.target_relation_uuid); e.target_relation_generation = Get(effects, 40);
  Get(effects, 48, &e.relation_descriptor_uuid); e.relation_descriptor_generation = Get(effects, 64);
  Get(effects, 72, &e.relation_shape_sha256); Get(effects, 104, &e.index_set_sha256);
  Get(effects, 136, &e.constraint_set_sha256); Get(effects, 168, &e.trigger_set_sha256);
  e.index_count = Get(effects, 200, 4); e.constraint_count = Get(effects, 204, 4); e.trigger_count = Get(effects, 208, 4);
  Get(effects, 212, &e.constraint_set_uuid); Get(effects, 228, &e.trigger_set_uuid);
  auto& x = v.executor;
  x.snapshot_uuid = Uuid(executor, 0); x.generation = Get(executor, 16); x.database_uuid = Uuid(executor, 24);
  x.row_identity_sha256 = HexHash(executor, 40); x.decision_evidence_sha256 = HexHash(executor, 72);
  x.availability_state = static_cast<SblrExecutorAvailabilityState>(executor[104]); x.installed = executor[105] == 1;
  Bytes canonical;
  if (!Security(s, v.matched_grant_uuids, &canonical) || !std::ranges::equal(canonical, security) ||
      !Effects(e, &canonical) || !std::ranges::equal(canonical, effects) ||
      !Executor(x, &canonical) || !std::ranges::equal(canonical, executor)) return false;
  return true;
}
bool Cross(const DmlDeleteDurableAuthorityBundleV1& v, const Bytes& security) {
  const auto& d = v.descriptor; const auto& r = v.resource_budget; const auto& t = v.target_order;
  const auto& recovery = v.recovery; const auto& e = v.effects; const auto& s = v.security;
  w::TypedDeleteCarrierError error; w::TypedUpdateHash security_hash{};
  return Present(v.database_uuid) && Present(v.session_uuid) && Present(v.principal_uuid) &&
      Present(v.bundle_uuid) && v.bundle_generation && Present(v.owner_context_sha256) &&
      Present(v.reserved_statement_savepoint_uuid) && v.reserved_statement_savepoint_uuid != v.bundle_uuid &&
      v.reserved_statement_savepoint_uuid != d.descriptor_uuid && v.reserved_statement_savepoint_uuid != d.operation_uuid &&
      w::ValidateTypedDeleteDatatypeOperatorAuthority(d, v.predicate, v.datatypes, v.operators, &error) &&
      d.deterministic_target_order_uuid == t.target_order_uuid && d.deterministic_target_order_generation == t.target_order_generation &&
      d.authenticated_statement_receipt_uuid == t.authenticated_statement_receipt_uuid &&
      d.target_relation_occurrence_uuid == t.target_relation_occurrence_uuid &&
      d.target_relation_occurrence_generation == t.target_relation_occurrence_generation &&
      d.statement_snapshot_uuid == t.statement_snapshot_uuid && t.maximum_candidate_rows == r.maximum_candidate_rows &&
      t.maximum_candidate_rows <= 1048576 && r.maximum_trigger_depth <= 64 && r.maximum_effects <= 1048576 &&
      d.resource_budget_uuid == r.resource_budget_uuid && d.resource_budget_generation == r.resource_budget_generation &&
      d.authenticated_statement_receipt_uuid == r.authenticated_statement_receipt_uuid && d.owning_transaction_uuid == r.owning_transaction_uuid &&
      d.predicate_node_count <= r.maximum_predicate_nodes &&
      d.recovery_token_uuid == recovery.recovery_token_uuid && d.recovery_generation == recovery.recovery_generation &&
      d.authenticated_statement_receipt_uuid == recovery.authenticated_statement_receipt_uuid &&
      d.owning_transaction_uuid == recovery.owning_transaction_uuid && d.operation_uuid == recovery.operation_uuid &&
      d.descriptor_uuid == recovery.descriptor_uuid && d.descriptor_generation == recovery.descriptor_generation &&
      v.bundle_uuid == recovery.durable_registry_uuid && v.bundle_generation == recovery.durable_registry_generation &&
      projection::UuidText(d.security_snapshot_uuid) == s.snapshot_uuid && projection::UuidText(d.security_context_uuid) == s.security_context_uuid &&
      projection::UuidText(d.authenticated_statement_receipt_uuid) == s.authenticated_statement_receipt_uuid &&
      projection::UuidText(d.target_relation_uuid) == s.target_relation_uuid && d.security_generation == s.security_generation &&
      d.row_policy_count == 0 && projection::UuidText(d.row_policy_set_uuid) == s.snapshot_uuid && d.row_policy_set_generation == s.snapshot_generation &&
      Hash("ScratchBird.DmlDelete.SecuritySnapshot.V1", security, {}, &security_hash) && d.row_policy_set_sha256 == security_hash &&
      d.target_relation_uuid == e.target_relation_uuid && d.target_relation_generation == e.target_relation_generation &&
      d.constraint_set_uuid == e.constraint_set_uuid && d.constraint_set_generation == e.generation && d.constraint_count == e.constraint_count &&
      d.ordered_constraint_set_sha256 == e.constraint_set_sha256 && d.trigger_set_uuid == e.trigger_set_uuid &&
      d.trigger_set_generation == e.generation && d.trigger_count == e.trigger_count && d.ordered_trigger_set_sha256 == e.trigger_set_sha256 &&
      d.executor_availability_generation == v.executor.generation && projection::UuidText(v.database_uuid) == v.executor.database_uuid;
}
}  // namespace

bool ComputeDmlDeleteSecuritySnapshotHashV1(const EngineSecurityPolicySnapshotAuthorityV1& security,
    const std::vector<std::string>& grants, w::TypedUpdateHash* out) {
  Bytes bytes;
  return out && Security(security, grants, &bytes) && Hash("ScratchBird.DmlDelete.SecuritySnapshot.V1", bytes, {}, out);
}

bool EncodeDmlDeleteDurableAuthorityBundleV1(const DmlDeleteDurableAuthorityBundleV1& input,
    Bytes* out, EngineApiDiagnostic* diagnostic) {
  if (!out) return Fail(diagnostic, "output_required");
  if (input.predicate.records.size() > 3 || input.datatypes.records.size() > 2 || input.operators.records.size() > 1)
    return Fail(diagnostic, "closed_profile_bounds");
  Sections sections;
  w::TypedDeleteCarrierError de; w::TypedUpdateCarrierError se;
  if (!w::EncodeTypedDeleteDescriptor(input.descriptor, &sections[0], &de) ||
      !w::EncodeTypedUpdatePredicateVector(input.predicate, &sections[1], &se) ||
      !w::EncodeTypedUpdateDatatypeAuthorityVector(input.datatypes, &sections[2], &se) ||
      !w::EncodeTypedUpdateBuiltinOperatorAuthorityVector(input.operators, &sections[3], &se) ||
      !w::EncodeTypedUpdateTargetOrder(input.target_order, &sections[4], &se) ||
      !w::EncodeTypedUpdateResourceBudget(input.resource_budget, &sections[5], &se) ||
      !w::EncodeTypedUpdateRecoveryToken(input.recovery, &sections[6], &se) ||
      !Security(input.security, input.matched_grant_uuids, &sections[7]) || !Effects(input.effects, &sections[8]) ||
      !Executor(input.executor, &sections[9])) return Fail(diagnostic, "section_encoding");
  std::size_t size = 272;
  std::array<std::span<const std::uint8_t>, 10> spans;
  for (std::size_t n = 0; n < sections.size(); ++n) {
    if (sections[n].empty() || size > kDmlDeleteDurableAuthorityMaximumBytesV1 - 4 ||
        sections[n].size() > kDmlDeleteDurableAuthorityMaximumBytesV1 - size - 4)
      return Fail(diagnostic, "bundle_extent");
    size += 4 + sections[n].size(); spans[n] = sections[n];
  }
  DmlDeleteDurableAuthorityBundleV1 canonical;
  canonical.database_uuid = input.database_uuid; canonical.session_uuid = input.session_uuid;
  canonical.principal_uuid = input.principal_uuid; canonical.bundle_uuid = input.bundle_uuid;
  canonical.bundle_generation = input.bundle_generation; canonical.owner_context_sha256 = input.owner_context_sha256;
  canonical.reserved_statement_savepoint_uuid = input.reserved_statement_savepoint_uuid;
  if (!DecodeSections(spans, &canonical) || !Cross(canonical, sections[7])) return Fail(diagnostic, "provider_cross_binding");
  Bytes bytes(272, 0);
  std::copy_n("DDAB", 4, bytes.begin()); Put(bytes, 4, 1, 2); Put(bytes, 6, 272, 2); Put(bytes, 8, size, 4);
  const auto& d = canonical.descriptor;
  Put(bytes, 16, d.descriptor_uuid); Put(bytes, 32, d.descriptor_generation);
  Put(bytes, 40, input.database_uuid); Put(bytes, 56, input.session_uuid); Put(bytes, 72, input.principal_uuid);
  Put(bytes, 88, d.authenticated_statement_receipt_uuid); Put(bytes, 104, d.owning_transaction_uuid);
  Put(bytes, 120, d.owning_local_transaction_id); Put(bytes, 128, d.structural_occurrence_id);
  Put(bytes, 136, input.bundle_uuid); Put(bytes, 152, input.bundle_generation); Put(bytes, 160, input.owner_context_sha256);
  Put(bytes, 192, d.descriptor_evidence_sha256);
  Put(bytes, 256, input.reserved_statement_savepoint_uuid);
  for (const auto& section : sections) {
    const auto offset = bytes.size(); bytes.resize(offset + 4); Put(bytes, offset, section.size(), 4);
    bytes.insert(bytes.end(), section.begin(), section.end());
  }
  w::TypedUpdateHash hash{};
  if (!Hash(kDomain, std::span(bytes).first(224), std::span(bytes).subspan(256), &hash)) return Fail(diagnostic, "bundle_hash");
  Put(bytes, 224, hash); *out = std::move(bytes); return true;
}

bool DecodeDmlDeleteDurableAuthorityBundleV1(std::span<const std::uint8_t> bytes,
    DmlDeleteDurableAuthorityBundleV1* out, EngineApiDiagnostic* diagnostic) {
  if (!out || bytes.size() < 272 || bytes.size() > kDmlDeleteDurableAuthorityMaximumBytesV1 ||
      !std::equal(bytes.begin(), bytes.begin() + 4, "DDAB") || Get(bytes, 4, 2) != 1 || Get(bytes, 6, 2) != 272 ||
      Get(bytes, 8, 4) != bytes.size() || Get(bytes, 12, 4)) return Fail(diagnostic, "header_or_extent");
  std::array<std::span<const std::uint8_t>, 10> sections;
  std::size_t offset = 272;
  for (auto& section : sections) {
    if (bytes.size() - offset < 4) return Fail(diagnostic, "section_prefix");
    const auto size = Get(bytes, offset, 4); offset += 4;
    if (!size || size > bytes.size() - offset) return Fail(diagnostic, "section_extent");
    section = bytes.subspan(offset, size); offset += size;
  }
  if (offset != bytes.size()) return Fail(diagnostic, "trailing_bytes");
  DmlDeleteDurableAuthorityBundleV1 value;
  Get(bytes, 40, &value.database_uuid); Get(bytes, 56, &value.session_uuid); Get(bytes, 72, &value.principal_uuid);
  Get(bytes, 136, &value.bundle_uuid); value.bundle_generation = Get(bytes, 152);
  Get(bytes, 160, &value.owner_context_sha256); Get(bytes, 224, &value.bundle_evidence_sha256);
  Get(bytes, 256, &value.reserved_statement_savepoint_uuid);
  if (!DecodeSections(sections, &value)) return Fail(diagnostic, "sections");
  Bytes canonical;
  if (!EncodeDmlDeleteDurableAuthorityBundleV1(value, &canonical, diagnostic) || !std::ranges::equal(canonical, bytes))
    return Fail(diagnostic, "canonical_bytes_or_evidence");
  value.exact_bytes = std::move(canonical); *out = std::move(value); return true;
}
}  // namespace scratchbird::engine::internal_api
