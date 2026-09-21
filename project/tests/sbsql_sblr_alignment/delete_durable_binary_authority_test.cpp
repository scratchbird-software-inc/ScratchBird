// Copyright (c) 2026 ScratchBird Software Inc.
// SPDX-License-Identifier: MPL-2.0
// Real DDAB codec and owner comparison, not provider issuance or SQL execution.
#include "dml/delete_durable_authority_codec.hpp"
#include "sblr_executor_row_identity_hash.hpp"
#include "typed_delete_test_fixture.hpp"
#include "hash_digest.hpp"
#include <cstdlib>
#include <iostream>
#include <new>
#include <stdexcept>
#include <type_traits>

namespace { long fail_after = -1; std::size_t allocations = 0, checks = 0, faults = 0; }
void* operator new(std::size_t n) {
  ++allocations;
  if (fail_after >= 0 && fail_after-- == 0) throw std::bad_alloc{};
  if (auto* p = std::malloc(n ? n : 1)) return p;
  throw std::bad_alloc{};
}
void* operator new[](std::size_t n) { return ::operator new(n); }
void operator delete(void* p) noexcept { std::free(p); }
void operator delete(void* p, std::size_t) noexcept { std::free(p); }
void operator delete[](void* p) noexcept { std::free(p); }
void operator delete[](void* p, std::size_t) noexcept { std::free(p); }

namespace api = scratchbird::engine::internal_api;
namespace w = scratchbird::wire;
namespace f = scratchbird::tests::delete_carrier;
using Bytes = std::vector<std::uint8_t>;
using Uuid = api::EngineUuid;
namespace {
void Check(bool ok, const char* message) { ++checks; if (!ok) throw std::runtime_error(message); }
Uuid Id(unsigned n) { return Uuid{f::Uuid(n)}; }
w::TypedUpdateHash Hash(std::string_view domain, std::span<const std::uint8_t> body = {}) {
  Bytes bytes(domain.begin(), domain.end()); bytes.insert(bytes.end(), body.begin(), body.end());
  const auto hash = scratchbird::core::hash::ComputeSha256Digest(bytes);
  Check(hash.ok(), "oracle SHA256 failed"); return hash.digest;
}
void Number(Bytes& out, std::size_t offset, std::uint64_t n, unsigned width = 8) {
  for (unsigned i = 0; i < width; ++i) out[offset + i] = static_cast<std::uint8_t>(n >> (8 * i));
}
std::uint64_t Number(std::span<const std::uint8_t> bytes, std::size_t offset, unsigned width = 4) {
  std::uint64_t n = 0; for (unsigned i = 0; i < width; ++i) n |= std::uint64_t(bytes[offset + i]) << (8 * i);
  return n;
}
api::DmlDeleteDurableAuthorityBundleV1 Bundle() {
  auto fixture = f::Make(0);
  api::DmlDeleteDurableAuthorityBundleV1 b;
  b.database_uuid = f::Uuid(50); b.session_uuid = f::Uuid(51); b.principal_uuid = f::Uuid(52);
  b.bundle_uuid = f::Uuid(53); b.reserved_statement_savepoint_uuid = f::Uuid(54); b.bundle_generation = 1;
  b.owner_context_sha256.fill(5);
  auto& d = fixture.descriptor;
  auto& s = b.security;
  s.snapshot_uuid = Uuid{d.security_snapshot_uuid}; s.snapshot_generation = 1;
  s.authenticated_statement_receipt_uuid = Uuid{d.authenticated_statement_receipt_uuid};
  s.security_context_uuid = Uuid{d.security_context_uuid}; s.security_context_generation = 1;
  s.security_generation = s.policy_generation = 1; s.target_relation_uuid = Uuid{d.target_relation_uuid};
  b.matched_grant_uuids = {Id(60), Id(61)};
  d.row_policy_set_uuid = s.snapshot_uuid.bytes;
  Check(api::ComputeDmlDeleteSecuritySnapshotHashV1(s, b.matched_grant_uuids, &d.row_policy_set_sha256), "security hash");
  auto& e = b.effects;
  e.snapshot_uuid = f::Uuid(62); e.generation = 1;
  e.target_relation_uuid = d.target_relation_uuid; e.target_relation_generation = 1;
  e.relation_descriptor_uuid = f::Uuid(63); e.relation_descriptor_generation = 1;
  e.constraint_set_uuid = d.constraint_set_uuid; e.trigger_set_uuid = d.trigger_set_uuid;
  e.relation_shape_sha256.fill(6); e.index_set_sha256.fill(7);
  e.constraint_set_sha256 = Hash("ScratchBird.DmlDelete.NoInboundConstraintSet.V1");
  e.trigger_set_sha256 = Hash("ScratchBird.DmlDelete.NoTriggerSet.V1");
  d.ordered_constraint_set_sha256 = e.constraint_set_sha256; d.ordered_trigger_set_sha256 = e.trigger_set_sha256;
  f::Seal(fixture);
  b.descriptor = fixture.descriptor; b.predicate = fixture.predicate;
  b.datatypes = fixture.datatypes; b.operators = fixture.operators;
  auto& t = b.target_order;
  t.target_order_uuid = d.deterministic_target_order_uuid; t.target_order_generation = 1;
  t.authenticated_statement_receipt_uuid = d.authenticated_statement_receipt_uuid;
  t.target_relation_occurrence_uuid = d.target_relation_occurrence_uuid; t.target_relation_occurrence_generation = 1;
  t.statement_snapshot_uuid = d.statement_snapshot_uuid; t.maximum_candidate_rows = 16;
  auto& r = b.resource_budget;
  r.resource_budget_uuid = d.resource_budget_uuid; r.resource_budget_generation = 1;
  r.authenticated_statement_receipt_uuid = d.authenticated_statement_receipt_uuid; r.owning_transaction_uuid = d.owning_transaction_uuid;
  r.cancellation_token_uuid = f::Uuid(64); r.cancellation_generation = 1;
  r.grant_receipt_uuid = f::Uuid(65); r.grant_receipt_generation = 1;
  r.maximum_assignments = 1; r.maximum_predicate_nodes = 3; r.maximum_candidate_rows = 16;
  r.maximum_trigger_depth = 1; r.maximum_effects = 16; r.maximum_total_canonical_value_bytes = 1024;
  auto& recovery = b.recovery;
  recovery.recovery_token_uuid = d.recovery_token_uuid; recovery.recovery_generation = 1;
  recovery.authenticated_statement_receipt_uuid = d.authenticated_statement_receipt_uuid;
  recovery.owning_transaction_uuid = d.owning_transaction_uuid; recovery.operation_uuid = d.operation_uuid;
  recovery.descriptor_uuid = d.descriptor_uuid; recovery.descriptor_generation = 1;
  recovery.statement_savepoint_profile_uuid = f::Uuid(66); recovery.statement_savepoint_profile_generation = 1;
  recovery.durable_registry_uuid = b.bundle_uuid; recovery.durable_registry_generation = 1;
  auto& x = b.executor;
  x.snapshot_uuid = Id(67); x.generation = 1; x.database_uuid = Uuid{b.database_uuid}; x.installed = true;
  x.availability_state = api::SblrExecutorAvailabilityState::installed;
  x.row_identity_sha256 = api::HashSblrExecutorRowIdentityMaterial({"dml.delete_rows", 784, "1.0", "dml_delete_rows_descriptor", "mutation_result", 1});
  x.decision_evidence_sha256 = "sha256:" + std::string(64, 'a');
  return b;
}
void OwnerBindings() {
  auto bundle = Bundle(); auto& d = bundle.descriptor;
  api::EngineRequestContext c;
  c.database_path = "DDAB component fixture only";
  c.database_uuid = Uuid{bundle.database_uuid}; c.session_uuid = Uuid{bundle.session_uuid};
  c.principal_uuid = Uuid{bundle.principal_uuid}; c.transaction_uuid = Uuid{d.owning_transaction_uuid};
  c.local_transaction_id = d.owning_local_transaction_id; c.statement_uuid = Id(71);
  c.statement_receipt_uuid = Uuid{d.authenticated_statement_receipt_uuid};
  c.statement_snapshot_uuid = Uuid{d.statement_snapshot_uuid}; c.statement_snapshot_generation = 1;
  c.statement_metadata_snapshot_uuid = Uuid{d.catalog_snapshot_uuid}; c.statement_metadata_snapshot_engine_owned = true;
  c.catalog_generation_id = d.catalog_generation; c.resource_epoch = 1; c.resource_admission_uuid = Id(72);
  c.security_context_present = true; c.security_epoch = 1; c.authorization_context.present = true;
  c.authorization_context.authority_uuid = bundle.security.security_context_uuid;
  c.authorization_context.security_context_generation = bundle.security.security_context_generation;
  c.authorization_context.principal_uuid = c.principal_uuid; c.authorization_context.catalog_generation_id = 1;
  c.authorization_context.security_epoch = c.authorization_context.policy_epoch = 1;
  c.datatype_catalog_snapshot_uuid = Uuid{bundle.datatypes.identity.vector_uuid};
  c.datatype_catalog_generation = c.datatype_registry_generation = 1;
  Check(api::ComputeDmlDeleteOwnerContextHashV1(c, &bundle.owner_context_sha256), "owner hash");
  Check(api::MatchesDmlDeleteDurableAuthorityOwnerV1(c, bundle), "exact binary owner comparison");
  for (unsigned slot = 0; slot < 15; ++slot) {
    auto changed = c;
    Uuid* fields[]{&changed.database_uuid, &changed.session_uuid, &changed.principal_uuid,
        &changed.transaction_uuid, &changed.statement_uuid, &changed.statement_receipt_uuid,
        &changed.statement_snapshot_uuid, &changed.statement_metadata_snapshot_uuid,
        &changed.catalog_epoch_uuid, &changed.resource_admission_uuid,
        &changed.authorization_context.authority_uuid, &changed.authorization_context.principal_uuid,
        &changed.current_role_uuid, &changed.transaction_policy_snapshot_uuid, &changed.datatype_catalog_snapshot_uuid};
    *fields[slot] = Id(90);
    Check(!api::MatchesDmlDeleteDurableAuthorityOwnerV1(changed, bundle), "changed owner identity matched");
    fields[slot]->bytes[6] = 0x40;
    w::TypedUpdateHash hash{}; hash.fill(9); const auto saved = hash;
    Check(!api::ComputeDmlDeleteOwnerContextHashV1(changed, &hash) && hash == saved,
        "malformed nonnil owner identity treated as optional absence");
  }
  for (unsigned field = 0; field < 6; ++field) {
    auto changed = c;
    if (field == 0) ++changed.security_epoch;
    if (field == 1) ++changed.catalog_generation_id;
    if (field == 2) ++changed.local_transaction_id;
    if (field == 3) changed.read_only_mode = true;
    if (field == 4) changed.cluster_transaction_active = true;
    if (field == 5) changed.route_fence_present = true;
    Check(!api::MatchesDmlDeleteDurableAuthorityOwnerV1(changed, bundle), "changed owner fence matched");
  }
}
void RoundtripAndFaults() {
  static_assert(std::is_same_v<decltype(api::EngineSecurityPolicySnapshotAuthorityV1{}.snapshot_uuid), Uuid>);
  static_assert(std::is_same_v<decltype(api::EngineSecurityPolicyCatalogRowIdentityV1{}.policy_uuid), Uuid>);
  static_assert(std::is_same_v<decltype(api::SblrExecutorAvailabilitySnapshot{}.database_uuid), Uuid>);
  auto b = Bundle(); Bytes bytes; api::EngineApiDiagnostic error;
  Check(api::EncodeDmlDeleteDurableAuthorityBundleV1(b, &bytes, &error), error.detail.c_str());
  Check(bytes.size() > 272 && std::equal(bytes.begin(), bytes.begin() + 4, "DDAB") &&
      Number(bytes, 8) == bytes.size() && std::equal(b.database_uuid.begin(), b.database_uuid.end(), bytes.begin() + 40), "DDAB header layout");
  std::size_t offset = 272;
  for (unsigned n = 0; n < 7; ++n) offset += 4 + Number(bytes, offset);
  Check(Number(bytes, offset) == 160, "security section extent"); offset += 4;
  Bytes expected(160, 0);
  const auto identity = [&](std::size_t at, const Uuid& id) { std::copy(id.bytes.begin(), id.bytes.end(), expected.begin() + at); };
  identity(0, b.security.snapshot_uuid); Number(expected, 16, 1);
  identity(24, b.security.authenticated_statement_receipt_uuid); identity(40, b.security.security_context_uuid);
  Number(expected, 56, 1); Number(expected, 64, 1); Number(expected, 72, 1);
  identity(80, b.security.target_relation_uuid); Number(expected, 96, 2, 4);
  identity(128, b.matched_grant_uuids[0]); identity(144, b.matched_grant_uuids[1]);
  Check(std::equal(expected.begin(), expected.end(), bytes.begin() + offset), "security section differs from independent Core layout");
  Check(Hash("ScratchBird.DmlDelete.SecuritySnapshot.V1", expected) == b.descriptor.row_policy_set_sha256, "security preimage oracle");
  api::DmlDeleteDurableAuthorityBundleV1 decoded;
  Check(api::DecodeDmlDeleteDurableAuthorityBundleV1(bytes, &decoded, &error), "decode");
  Bytes again; Check(api::EncodeDmlDeleteDurableAuthorityBundleV1(decoded, &again, &error) && bytes == again, "exact roundtrip");
  Check(decoded.security == b.security && decoded.matched_grant_uuids == b.matched_grant_uuids, "binary security identity roundtrip");
  for (std::size_t n = 0; n < bytes.size(); ++n) {
    api::DmlDeleteDurableAuthorityBundleV1 out; out.bundle_uuid = f::Uuid(99); out.exact_bytes = {4, 3, 2, 1};
    Check(!api::DecodeDmlDeleteDurableAuthorityBundleV1(std::span(bytes).first(n), &out, &error) &&
        out.bundle_uuid == f::Uuid(99) && out.exact_bytes == Bytes({4, 3, 2, 1}), "truncation published");
    auto bad = bytes; bad[n] ^= 1;
    Check(!api::DecodeDmlDeleteDurableAuthorityBundleV1(bad, &out, &error) && out.bundle_uuid == f::Uuid(99), "corruption accepted");
  }
  for (bool decode : {false, true}) {
    bool complete = false;
    for (long n = 0; n < 1024 && !complete; ++n) {
      Bytes out{9, 8, 7}; api::DmlDeleteDurableAuthorityBundleV1 value; value.bundle_uuid = f::Uuid(99);
      fail_after = n;
      try {
        const bool ok = decode ? api::DecodeDmlDeleteDurableAuthorityBundleV1(bytes, &value, nullptr)
                               : api::EncodeDmlDeleteDurableAuthorityBundleV1(b, &out, nullptr);
        fail_after = -1;
        if (ok) complete = true;
        else {
          ++faults;
          Check(out == Bytes({9, 8, 7}) && value.bundle_uuid == f::Uuid(99), "allocation refusal partially published");
        }
      } catch (const std::bad_alloc&) {
        fail_after = -1; ++faults;
        Check(out == Bytes({9, 8, 7}) && value.bundle_uuid == f::Uuid(99), "allocation failure partially published");
      } catch (...) { fail_after = -1; throw; }
    }
    Check(complete, "allocation sweep did not terminate");
  }
  for (unsigned slot = 0; slot < 6; ++slot) for (unsigned version = 0; version < 16; ++version)
    for (unsigned variant = 0; variant < 4; ++variant) if (version != 7 || variant != 2) {
    auto changed = b;
    auto* id = slot == 0 ? &changed.session_uuid : slot == 1 ? &changed.principal_uuid :
        slot == 2 ? &changed.bundle_uuid : slot == 3 ? &changed.reserved_statement_savepoint_uuid :
        slot == 4 ? &changed.effects.snapshot_uuid : &changed.effects.relation_descriptor_uuid;
    (*id)[6] = static_cast<std::uint8_t>(version << 4);
    (*id)[8] = static_cast<std::uint8_t>(variant << 6);
    if (slot == 2) changed.recovery.durable_registry_uuid = *id;
    Bytes out{9}; Check(!api::EncodeDmlDeleteDurableAuthorityBundleV1(changed, &out, &error) && out == Bytes({9}), "non-v7 DDAB system identity admitted");
  }
  for (const std::size_t identity_offset : {56u, 72u, 256u}) {
    auto bad = bytes; bad[identity_offset + 6] = 0x40;
    Bytes preimage(bad.begin(), bad.begin() + 224);
    preimage.insert(preimage.end(), bad.begin() + 256, bad.end());
    const auto digest = Hash("ScratchBird.DmlDelete.DurableAuthorityBundle.V1", preimage);
    std::copy(digest.begin(), digest.end(), bad.begin() + 224);
    api::DmlDeleteDurableAuthorityBundleV1 out; out.bundle_uuid = f::Uuid(99);
    Check(!api::DecodeDmlDeleteDurableAuthorityBundleV1(bad, &out, &error) && out.bundle_uuid == f::Uuid(99),
        "rehashed non-v7 owner became durable authority");
  }
  for (unsigned kind = 0; kind < 5; ++kind) {
    auto grants = b.matched_grant_uuids;
    if (kind == 0) std::reverse(grants.begin(), grants.end());
    if (kind == 1) grants.push_back(grants.back());
    if (kind == 2) grants[0] = {};
    if (kind == 3) grants[0].bytes[6] = 0x40;
    if (kind == 4) grants.resize(1025, Id(70));
    w::TypedUpdateHash hash{}; hash.fill(9); const auto saved = hash;
    Check(!api::ComputeDmlDeleteSecuritySnapshotHashV1(b.security, grants, &hash) && hash == saved,
        "invalid privilege-source UUIDs published security evidence");
  }
}
}
int main() try {
  RoundtripAndFaults(); OwnerBindings();
  std::cout << "PASS DDAB binary authority checks=" << checks << " allocation_faults=" << faults << "; component only\n";
} catch (const std::exception& e) {
  fail_after = -1; std::cerr << "FAIL after " << checks << " checks: " << e.what() << '\n'; return 1;
}
