// Copyright (c) 2026 ScratchBird Software Inc.
// SPDX-License-Identifier: MPL-2.0
#include "engine/statement_management_ack_codec.hpp"

#include <openssl/sha.h>
#include <openssl/evp.h>
#include <algorithm>
#include <cstdlib>
#include <iostream>
#include <limits>
#include <new>
#include <type_traits>

namespace { long fail_after = -1; }
void* operator new(std::size_t n) {
  if (fail_after == 0) throw std::bad_alloc();
  if (fail_after > 0) --fail_after;
  if (void* p = std::malloc(n ? n : 1)) return p;
  throw std::bad_alloc();
}
void* operator new[](std::size_t n) { return ::operator new(n); }
void operator delete(void* p) noexcept { std::free(p); }
void operator delete[](void* p) noexcept { std::free(p); }
void operator delete(void* p, std::size_t) noexcept { std::free(p); }
void operator delete[](void* p, std::size_t) noexcept { std::free(p); }

namespace {
namespace b = scratchbird::server_engine_bridge;
namespace codec = scratchbird::engine::statement_management;
using Uuid = scratchbird::engine::internal_api::EngineUuid;
using Bytes = std::vector<std::uint8_t>;
unsigned checks = 0, failures = 0, allocation_faults = 0;
void Check(bool ok, const char* why) {
  ++checks;
  if (!ok) { ++failures; std::cerr << why << '\n'; }
}
Uuid Identity(unsigned seed) {
  Uuid id;
  for (unsigned i = 0; i != 16; ++i) id.bytes[i] = (seed + i * 19) & 255;
  id.bytes[0] = 0; id.bytes[1] = '\t'; id.bytes[2] = '\n'; id.bytes[3] = '|';
  id.bytes[6] = (id.bytes[6] & 15) | 0x70;
  id.bytes[8] = (id.bytes[8] & 63) | 0x80;
  return id;
}
template<class A> auto Identities() {
  std::vector<std::pair<Uuid A::*, bool>> ids{
    {&A::authenticated_receipt_uuid, false}, {&A::binding_uuid, false}};
  if constexpr (requires { &A::statement_name_uuid; }) ids.push_back({&A::statement_name_uuid, false});
  if constexpr (requires { &A::result_descriptor_uuid; }) ids.push_back({&A::result_descriptor_uuid, true});
  if constexpr (requires { &A::resolution_uuid; }) ids.push_back({&A::resolution_uuid, false});
  if constexpr (requires { &A::parse_uuid; }) ids.push_back({&A::parse_uuid, false});
  if constexpr (requires { &A::check_uuid; }) {
    ids.push_back({&A::check_uuid, false}); ids.push_back({&A::object_uuid, true});
    ids.push_back({&A::schema_tree_uuid, false});
  }
  if constexpr (requires { &A::attach_uuid; }) {
    ids.push_back({&A::attach_uuid, false}); ids.push_back({&A::storage_uuid, false});
    ids.push_back({&A::alias_uuid, false}); ids.push_back({&A::database_uuid, false});
    ids.push_back({&A::catalog_snapshot_uuid, false});
  }
  if constexpr (requires { &A::statement_uuid; }) ids.push_back({&A::statement_uuid, false});
  if constexpr (requires { &A::target_execution_uuid; }) {
    ids.push_back({&A::target_execution_uuid, false}); ids.push_back({&A::target_statement_uuid, false});
    ids.push_back({&A::target_statement_receipt_uuid, false}); ids.push_back({&A::cancel_operation_uuid, false});
    ids.push_back({&A::target_transaction_uuid, true});
  }
  return ids;
}
template<class A> A Fixture() {
  A a;
  unsigned seed = 41;
  for (const auto [member, optional] : Identities<A>()) { (void)optional; a.*member = Identity(seed++); }
  a.occurrence = 0x1020304050607080ULL; a.binding_generation = 0x8877665544332211ULL;
  a.descriptor_sha256.fill(0x12); a.request_evidence_sha256.fill(0x34);
  a.acknowledgement_evidence_sha256.fill(0xac);
  a.exact_bind_ack_bytes = {1, 0, 255, 2};
  a.failure_code = "retained-code"; a.failure_message_key = "retained-key"; a.failure_detail = "retained-detail";
  if constexpr (requires { a.canonical_input_sha256; }) a.canonical_input_sha256.fill(0x56);
  if constexpr (requires { a.object_generation; }) {
    a.object_generation = 121; a.schema_tree_generation = 122; a.visibility_scope_sha256.fill(0x78);
  }
  if constexpr (requires { a.catalog_generation; }) a.catalog_generation = 131;
  if constexpr (requires { a.prepared_generation; }) a.prepared_generation = 141;
  if constexpr (requires { a.reason; }) {
    a.reason = 4; a.mode = 2; a.target_execution_generation = 151;
    a.deadline_monotonic_ns = 0x8877665544332211ULL; a.executor_availability_generation = 152;
  }
  return a;
}
// Independent fixed-offset wire oracle. It never invokes the encoder's writer
// helpers and does not infer fields from the emitted bytes.
void Number(Bytes& out, std::size_t offset, std::uint64_t n, unsigned width) {
  for (unsigned i = 0; i != width; ++i) out.at(offset+i) = static_cast<std::uint8_t>(n >> (8*i));
}
template<class T> void Data(Bytes& out, std::size_t offset, const T& value) {
  std::copy(value.begin(), value.end(), out.begin()+offset);
}
template<class A> Bytes Expected(const A& a, std::string_view magic, std::size_t size, std::size_t hash_offset) {
  Bytes out(size, 0); Data(out, 0, magic); Number(out, 4, 1, 2);
  Number(out, 6, size, 2); Number(out, 8, size, 4);
  Data(out, 16, a.authenticated_receipt_uuid.bytes); Number(out, 32, a.occurrence, 8);
  Data(out, 40, a.binding_uuid.bytes); Number(out, 56, a.binding_generation, 8);
  if constexpr (std::is_same_v<A, b::StatementPrepareBindAckV1>) Data(out, 64, a.statement_name_uuid.bytes);
  if constexpr (requires { a.result_descriptor_uuid; }) Data(out, 64, a.result_descriptor_uuid.bytes);
  if constexpr (requires { a.resolution_uuid; }) Data(out, 64, a.resolution_uuid.bytes);
  if constexpr (requires { a.parse_uuid; }) {
    Data(out, 64, a.parse_uuid.bytes); Data(out, 112, a.canonical_input_sha256);
  }
  if constexpr (requires { a.check_uuid; }) {
    Data(out, 64, a.check_uuid.bytes); Data(out, 80, a.object_uuid.bytes); Number(out, 96, a.object_generation, 8);
    Data(out, 104, a.schema_tree_uuid.bytes); Number(out, 120, a.schema_tree_generation, 8);
    Data(out, 128, a.visibility_scope_sha256);
  }
  if constexpr (requires { a.attach_uuid; }) {
    Data(out, 64, a.attach_uuid.bytes); Data(out, 80, a.storage_uuid.bytes); Data(out, 96, a.alias_uuid.bytes);
    Data(out, 112, a.database_uuid.bytes); Data(out, 128, a.catalog_snapshot_uuid.bytes);
    Number(out, 144, a.catalog_generation, 8);
  }
  if constexpr (requires { a.prepared_generation; }) {
    Data(out, 64, a.statement_uuid.bytes); Data(out, 80, a.statement_name_uuid.bytes);
    Number(out, 96, a.prepared_generation, 8);
  }
  if constexpr (requires { a.reason; }) {
    Data(out, 64, a.target_execution_uuid.bytes); Data(out, 80, a.target_statement_uuid.bytes);
    Data(out, 96, a.target_statement_receipt_uuid.bytes); Data(out, 112, a.cancel_operation_uuid.bytes);
    Data(out, 128, a.target_transaction_uuid.bytes); Number(out, 144, a.target_execution_generation, 8);
    out[152] = a.reason; out[153] = a.mode; Number(out, 156, a.deadline_monotonic_ns, 8);
    Number(out, 164, a.executor_availability_generation, 8);
  }
  if constexpr (requires { a.parse_uuid; }) Data(out, 80, a.descriptor_sha256);
  else Data(out, hash_offset-64, a.descriptor_sha256);
  Data(out, hash_offset-32, a.request_evidence_sha256);
  std::array<unsigned char, 32> hash{};
  Check(SHA256(out.data(), out.size(), hash.data()) != nullptr, "oracle hash failed");
  Data(out, hash_offset, hash);
  return out;
}
template<class A> void Unchanged(const A& a, const A& before) {
  Check(a.acknowledgement_evidence_sha256 == before.acknowledgement_evidence_sha256 &&
        a.exact_bind_ack_bytes == before.exact_bind_ack_bytes &&
        a.failure_code == before.failure_code && a.failure_message_key == before.failure_message_key &&
        a.failure_detail == before.failure_detail, "failed encoder partially published acknowledgement");
}
template<class A, class Encode> void Test(Encode encode, std::string_view magic, std::size_t size,
                                        std::size_t hash_offset, bool retains) {
  const A seed = Fixture<A>();
  const auto success = [&](A a) {
    const auto expected = Expected(a, magic, size, hash_offset);
    const auto previous = a;
    const auto actual = encode(&a);
    Check(actual == expected, "private acknowledgement differs from independent byte oracle");
    Check(std::equal(a.acknowledgement_evidence_sha256.begin(), a.acknowledgement_evidence_sha256.end(),
                     expected.begin()+hash_offset), "published digest differs");
    Check(a.exact_bind_ack_bytes == (retains ? expected : previous.exact_bind_ack_bytes), "retained bytes changed unexpectedly");
    for (const auto [member, optional] : Identities<A>()) {
      (void)optional; Check(a.*member == previous.*member, "encoder changed an identity");
    }
  };
  const auto refusal = [&](A a) {
    const auto previous = a;
    Check(encode(&a).empty(), "invalid private acknowledgement accepted"); Unchanged(a, previous);
  };
  Check(encode(nullptr).empty(), "null acknowledgement accepted"); success(seed);
  // Identical UUID bytes must not select the meaning of the adjacent counter.
  auto repeated = seed; repeated.binding_uuid = repeated.authenticated_receipt_uuid; success(repeated);
  for (const auto [member, optional] : Identities<A>()) {
    auto nil = seed; nil.*member = {};
    if constexpr (requires { nil.object_uuid; }) if (member == &A::object_uuid) nil.object_generation = 0;
    if (optional) success(nil); else refusal(nil);
    for (unsigned version = 0; version != 16; ++version) for (unsigned variant = 0; variant != 4; ++variant) {
      auto a = seed; auto& id = a.*member;
      id.bytes[6] = (id.bytes[6]&15) | (version<<4); id.bytes[8] = (id.bytes[8]&63) | (variant<<6);
      if (version == 7 && variant == 2) success(a); else refusal(a);
    }
  }
  auto bad = seed; bad.occurrence = 0; refusal(bad);
  bad = seed; bad.binding_generation = 0; refusal(bad);
  bad = seed; bad.descriptor_sha256.fill(0); refusal(bad);
  bad = seed; bad.request_evidence_sha256.fill(0); refusal(bad);
  if constexpr (requires { bad.canonical_input_sha256; }) { bad = seed; bad.canonical_input_sha256.fill(0); refusal(bad); }
  if constexpr (requires { bad.object_generation; }) {
    bad = seed; bad.object_generation = 0; refusal(bad);
    bad = seed; bad.object_uuid = {}; refusal(bad);
    bad = seed; bad.schema_tree_generation = 0; refusal(bad);
    bad = seed; bad.visibility_scope_sha256.fill(0); refusal(bad);
  }
  if constexpr (requires { bad.catalog_generation; }) { bad = seed; bad.catalog_generation = 0; refusal(bad); }
  if constexpr (requires { bad.prepared_generation; }) { bad = seed; bad.prepared_generation = 0; refusal(bad); }
  if constexpr (requires { bad.reason; }) {
    for (unsigned reason = 0; reason != 256; ++reason) {
      bad = seed; bad.reason = reason;
      if (reason >= 1 && reason <= 4) success(bad); else refusal(bad);
    }
    for (unsigned mode = 0; mode != 256; ++mode) {
      bad = seed; bad.mode = mode;
      if (mode == 1 || mode == 2) success(bad); else refusal(bad);
    }
    bad = seed; bad.target_execution_generation = 0; refusal(bad);
    bad = seed; bad.executor_availability_generation = 0; refusal(bad);
    bad = seed; bad.deadline_monotonic_ns = 0; success(bad);
  }
  bool completed = false;
  for (long at = 0; at != 128; ++at) {
    auto a = seed; const auto before = a; Bytes wire; bool threw = false;
    fail_after = at;
    try { wire = encode(&a); } catch (const std::bad_alloc&) { threw = true; }
    fail_after = -1;
    if (!threw && !wire.empty()) {
      Check(wire == Expected(seed, magic, size, hash_offset), "successful fault sweep changed bytes");
      completed = true; break;
    }
    ++allocation_faults; Unchanged(a, before);
  }
  Check(completed, "allocation sweep never completed");
  // Exercise actual digest-provider failure, not a fabricated zero digest.
  {
    struct RestoreHashProvider {
      ~RestoreHashProvider() { EVP_set_default_properties(nullptr, ""); }
    } restore;
    auto a = seed;
    Check(EVP_set_default_properties(nullptr, "provider=sb_missing_test_provider") == 1,
          "could not select unavailable digest provider");
    const auto wire = encode(&a);
    Check(wire.empty(), "hash failure published a fabricated acknowledgement");
    Unchanged(a, seed);
  }
  success(seed);
}
}
int main() {
  Test<b::StatementPrepareBindAckV1>(codec::statement_management_prepare_ack, "SPBA", 176, 144, false);
  Test<b::StatementExecuteDirectBindAckV1>(codec::statement_management_execute_direct_ack, "SDBA", 176, 144, false);
  Test<b::StatementNameResolveBindAckV1>(codec::statement_management_name_resolve_ack, "SNBA", 176, 144, true);
  Test<b::StatementParseTextBindAckV1>(codec::statement_management_parse_text_ack, "PTBA", 208, 176, true);
  Test<b::StatementCatalogEpochCheckBindAckV1>(codec::statement_management_catalog_epoch_check_ack, "CEBA", 256, 224, true);
  Test<b::StatementDatabaseAttachBindAckV1>(codec::statement_management_database_attach_ack, "DABA", 256, 216, true);
  Test<b::StatementFreeBindAckV1>(codec::statement_management_free_ack, "SFBA", 208, 168, false);
  Test<b::StatementCancelBindAckV1>(codec::statement_management_cancel_ack, "SCBA", 272, 236, false);
  std::cout << "statement_management_ack checks=" << checks << " allocation_faults=" << allocation_faults
            << " failures=" << failures << '\n';
  return failures ? 1 : 0;
}
