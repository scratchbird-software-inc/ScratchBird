// Copyright (c) 2026 ScratchBird Software Inc.
// SPDX-License-Identifier: MPL-2.0
#include "server/sblr_admission.hpp"
#include "server_engine_bridge/statement_context.hpp"
#include "server_engine_bridge/admission_token_binding.hpp"
#include <algorithm>
#include <cstdlib>
#include <iostream>
#include <new>
#include <type_traits>

namespace {
long allocation_budget = -1;
unsigned checks = 0, failures = 0;
void Check(bool ok, const char* detail) {
  ++checks;
  if (!ok && failures++ < 20) std::cerr << "FAIL " << detail << '\n';
}
}
void* operator new(std::size_t size) {
  if (allocation_budget == 0) throw std::bad_alloc();
  if (allocation_budget > 0) --allocation_budget;
  if (void* value = std::malloc(size ? size : 1)) return value;
  throw std::bad_alloc();
}
void* operator new[](std::size_t size) { return ::operator new(size); }
void operator delete(void* value) noexcept { std::free(value); }
void operator delete[](void* value) noexcept { std::free(value); }
void operator delete(void* value, std::size_t) noexcept { std::free(value); }
void operator delete[](void* value, std::size_t) noexcept { std::free(value); }

namespace {
namespace bridge = scratchbird::server_engine_bridge;
using Server = scratchbird::server::ServerSblrAdmissionTokenData;
using Engine = bridge::StatementContextDispatchRequest;
using Uuid = scratchbird::core::platform::Uuid;
using Bytes = std::vector<std::uint8_t>;
static_assert(sizeof(Uuid) == 16);
static_assert(std::is_same_v<decltype(Server{}.authenticated_principal_uuid), Uuid>);
static_assert(std::is_same_v<decltype(Engine{}.authenticated_principal_uuid), Uuid>);

template <std::size_t N>
void Pattern(std::array<std::uint8_t, N>* bytes, unsigned start) {
  for (unsigned index = 0; index < N; ++index)
    (*bytes)[index] = static_cast<std::uint8_t>(start + index);
}
Uuid Identity(unsigned start) {
  Uuid value; Pattern(&value.bytes, start);
  value.bytes[6] = 0x70 | (value.bytes[6] & 0xf);
  value.bytes[8] = 0x80 | (value.bytes[8] & 0x3f);
  return value;
}
template <typename T> T Fixture() {
  T value;
  Pattern(&value.container_sha256, 0);
  Pattern(&value.execution_envelope_sha256, 32);
  Pattern(&value.operation_sha256, 64);
  value.authenticated_principal_uuid = Identity(96);
  value.catalog_snapshot_uuid = Identity(112);
  value.engine_mga_statement_uuid = Identity(128);
  value.engine_mga_snapshot_uuid = Identity(144);
  value.catalog_epoch = 0x0102030405060708ULL;
  value.security_epoch = 0x1112131415161718ULL;
  value.resource_epoch = 0x2122232425262728ULL;
  auto& gateway = value.gateway_evidence;
  gateway.source = static_cast<decltype(gateway.source)>(1);
  gateway.disposition = static_cast<decltype(gateway.disposition)>(1);
  gateway.provider_observation_generation = 0x3132333435363738ULL;
  Pattern(&gateway.canonical_payload_sha256, 160);
  gateway.route_snapshot_uuid = Identity(192);
  gateway.security_snapshot_uuid = Identity(208);
  gateway.route_epoch = 5; gateway.route_generation = 6;
  gateway.security_epoch = 7; gateway.security_observation_generation = 8;
  gateway.cluster_context_active = true; gateway.cluster_transaction_active = false;
  gateway.route_fence_present = true;
  auto& executor = value.package_executor_evidence;
  executor.begin_executor_id = std::string("begin\0executor", 14);
  executor.end_executor_id = "end_executor";
  executor.registry_snapshot_uuid = Identity(224);
  executor.executor_evidence_generation = 9;
  Pattern(&executor.canonical_payload_sha256, 240);
  return value;
}
bridge::AdmissionReservationBinding Reservation() {
  return {0x4142434445464748ULL, 1, 0x5152535455565758ULL, 0x61626364, 0x7172737475767778ULL};
}

struct Reader {
  const Bytes& bytes;
  std::size_t offset = 0;
  void Raw(const auto& expected) {
    const bool fits = offset <= bytes.size() && expected.size() <= bytes.size() - offset;
    Check(fits && std::equal(expected.begin(), expected.end(), bytes.begin() + offset),
          "independent binding reader matches exact bytes and field order");
    offset += expected.size();
  }
  void Number(std::uint64_t expected, unsigned width) {
    std::uint64_t actual = 0;
    const bool fits = offset <= bytes.size() && width <= bytes.size() - offset;
    if (fits) for (unsigned index = 0; index < width; ++index)
      actual |= std::uint64_t(bytes[offset + index]) << (8 * index);
    Check(fits && actual == expected, "binding integers have fixed little-endian widths");
    offset += width;
  }
  void Text(std::string_view expected) { Number(expected.size(), 4); Raw(expected); }
};
void CheckLayout(const Bytes& bytes, bool package) {
  Reader reader{bytes};
  reader.Raw(std::string_view("ScratchBird.SBLR.AdmissionToken.BinaryUuid.V2"));
  std::array<std::uint8_t, 96> hashes{}; Pattern(&hashes, 0); reader.Raw(hashes);
  for (const unsigned start : {96u, 112u, 128u, 144u}) reader.Raw(Identity(start).bytes);
  reader.Number(0x0102030405060708ULL, 8);
  reader.Number(0x1112131415161718ULL, 8);
  reader.Number(0x2122232425262728ULL, 8);
  reader.Number(package ? 1 : 0, 1);
  if (package) {
    reader.Number(0x4142434445464748ULL, 8); reader.Number(1, 1);
    reader.Number(0x5152535455565758ULL, 8); reader.Number(0x61626364, 4);
    reader.Number(0x7172737475767778ULL, 8);
    reader.Number(1, 1); reader.Number(1, 1); reader.Number(0x3132333435363738ULL, 8);
    std::array<std::uint8_t, 32> digest{}; Pattern(&digest, 160); reader.Raw(digest);
    reader.Raw(Identity(192).bytes); reader.Raw(Identity(208).bytes);
    for (unsigned value = 5; value <= 8; ++value) reader.Number(value, 8);
    reader.Number(1, 1); reader.Number(0, 1); reader.Number(1, 1);
    reader.Text(std::string("begin\0executor", 14)); reader.Text("end_executor");
    reader.Raw(Identity(224).bytes); reader.Number(9, 8);
    Pattern(&digest, 240); reader.Raw(digest);
  }
  Check(reader.offset == bytes.size(), "binding has no unframed trailing fields");
}
void IdentityAndHashCoverage() {
  auto server = Fixture<Server>();
  const auto reservation = Reservation();
  const auto baseline = bridge::EncodeAdmissionTokenBindingV2(server, reservation);
  for (auto* identity : {&server.authenticated_principal_uuid, &server.catalog_snapshot_uuid,
      &server.engine_mga_statement_uuid, &server.engine_mga_snapshot_uuid,
      &server.gateway_evidence.route_snapshot_uuid, &server.gateway_evidence.security_snapshot_uuid,
      &server.package_executor_evidence.registry_snapshot_uuid}) {
    for (auto& octet : identity->bytes) {
      octet ^= 1;
      Check(bridge::EncodeAdmissionTokenBindingV2(server, reservation) != baseline,
            "all sixteen bytes of every binding identity participate");
      octet ^= 1;
    }
  }
  for (auto* digest : {&server.container_sha256, &server.execution_envelope_sha256,
      &server.operation_sha256, &server.gateway_evidence.canonical_payload_sha256,
      &server.package_executor_evidence.canonical_payload_sha256}) {
    for (auto& octet : *digest) {
      octet ^= 1;
      Check(bridge::EncodeAdmissionTokenBindingV2(server, reservation) != baseline,
            "all bytes of every owning payload digest participate");
      octet ^= 1;
    }
  }
  auto left = Fixture<Server>(), right = left;
  left.package_executor_evidence.begin_executor_id = std::string("a\0b", 3);
  left.package_executor_evidence.end_executor_id = "c";
  right.package_executor_evidence.begin_executor_id = "a";
  right.package_executor_evidence.end_executor_id = std::string("b\0c", 3);
  Check(bridge::EncodeAdmissionTokenBindingV2(left, reservation) !=
        bridge::EncodeAdmissionTokenBindingV2(right, reservation),
        "NUL-containing executor identifiers cannot collide across field boundaries");
}
void MetadataCoverage() {
  auto server = Fixture<Server>();
  auto reservation = Reservation();
  const auto baseline = bridge::EncodeAdmissionTokenBindingV2(server, reservation);
  const auto bit_coverage = [&](auto& field) {
    using Value = std::remove_reference_t<decltype(field)>;
    const auto original = field;
    for (unsigned bit = 0; bit < sizeof(Value) * 8; ++bit) {
      field = static_cast<Value>(static_cast<std::uint64_t>(original) ^
                                 (std::uint64_t{1} << bit));
      Check(bridge::EncodeAdmissionTokenBindingV2(server, reservation) != baseline,
            "every metadata bit participates in the package binding");
    }
    field = original;
  };
  for (auto* value : {&server.catalog_epoch, &server.security_epoch,
      &server.resource_epoch, &reservation.handle, &reservation.payload_size,
      &reservation.resource_policy_generation,
      &server.gateway_evidence.provider_observation_generation,
      &server.gateway_evidence.route_epoch, &server.gateway_evidence.route_generation,
      &server.gateway_evidence.security_epoch,
      &server.gateway_evidence.security_observation_generation,
      &server.package_executor_evidence.executor_evidence_generation}) bit_coverage(*value);
  bit_coverage(reservation.payload_kind);
  bit_coverage(reservation.record_count);
  bit_coverage(server.gateway_evidence.source);
  bit_coverage(server.gateway_evidence.disposition);
  for (auto* value : {&server.gateway_evidence.cluster_context_active,
      &server.gateway_evidence.cluster_transaction_active,
      &server.gateway_evidence.route_fence_present}) {
    *value = !*value;
    Check(bridge::EncodeAdmissionTokenBindingV2(server, reservation) != baseline,
          "each gateway activation flag participates in the binding");
    *value = !*value;
  }
  Check(bridge::EncodeAdmissionTokenBindingV2(server) != baseline,
        "typed reservation presence distinguishes operation and package bindings");
  const auto operation = bridge::EncodeAdmissionTokenBindingV2(server);
  server.package_executor_evidence = {};
  server.gateway_evidence = {};
  Check(bridge::EncodeAdmissionTokenBindingV2(server) == operation,
        "operation classification never depends on executor evidence presence");
  Check(bridge::EncodeAdmissionTokenBindingV2(server, reservation) != operation,
        "missing executor evidence cannot erase an explicit package reservation");
}
void AllocationFailure() {
  const auto server = Fixture<Server>();
  const auto reservation = Reservation();
  const auto expected = bridge::EncodeAdmissionTokenBindingV2(server, reservation);
  bool success = false; unsigned faults = 0;
  for (long budget = 0; budget < 128; ++budget) {
    allocation_budget = budget;
    try {
      const auto value = bridge::EncodeAdmissionTokenBindingV2(server, reservation);
      allocation_budget = -1;
      Check(value == expected, "allocation sweep returns only a complete binding");
      success = true;
    } catch (const std::bad_alloc&) { allocation_budget = -1; ++faults; }
    Check(server.authenticated_principal_uuid == Identity(96) &&
          server.package_executor_evidence.begin_executor_id == std::string("begin\0executor", 14),
          "binding allocation failure never mutates input authority");
    if (success) break;
  }
  Check(success && faults >= 4, "binding allocation fault paths exercised");
  std::cout << "binding_allocation_faults=" << faults << '\n';
}
}
int main() {
  const auto server = Fixture<Server>();
  const auto engine = Fixture<Engine>();
  for (const bool package : {false, true}) {
    const auto reservation = package ? std::optional{Reservation()} : std::nullopt;
    const auto issued = bridge::EncodeAdmissionTokenBindingV2(server, reservation);
    const auto revalidated = bridge::EncodeAdmissionTokenBindingV2(engine, reservation);
    Check(issued == revalidated, "actual server and engine carriers encode identical binding bytes");
    CheckLayout(issued, package);
  }
  IdentityAndHashCoverage(); MetadataCoverage(); AllocationFailure();
  std::cout << "checks=" << checks << " failures=" << failures << '\n';
  return failures ? 1 : 0;
}
