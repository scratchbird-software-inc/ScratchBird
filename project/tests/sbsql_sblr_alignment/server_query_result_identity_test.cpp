// Copyright (c) 2026 ScratchBird Software Inc.
// SPDX-License-Identifier: MPL-2.0
#include "server/query_result_identity.hpp"

#include <cstdlib>
#include <iostream>
#include <new>

namespace {
bool fail_allocation = false;
unsigned checks = 0, failures = 0;
void Check(bool ok, const char* message) {
  ++checks;
  if (!ok && failures++ < 15) std::cerr << "FAIL " << message << '\n';
}
}
void* operator new(std::size_t size) {
  if (fail_allocation) throw std::bad_alloc();
  if (void* value = std::malloc(size ? size : 1)) return value;
  throw std::bad_alloc();
}
void* operator new[](std::size_t size) { return ::operator new(size); }
void operator delete(void* value) noexcept { std::free(value); }
void operator delete[](void* value) noexcept { std::free(value); }
void operator delete(void* value, std::size_t) noexcept { std::free(value); }
void operator delete[](void* value, std::size_t) noexcept { std::free(value); }

namespace {
namespace server = scratchbird::server;
namespace bridge = scratchbird::server_engine_bridge;
using Uuid = scratchbird::core::platform::Uuid;
using Query = bridge::StatementQueryExecuteResultHandleView;
using Catalog = bridge::StatementCatalogIntrospectResultHandleView;
Uuid Identity(unsigned seed) {
  Uuid value;
  for (unsigned i = 0; i < 16; ++i) value.bytes[i] = seed + i;
  value.bytes[6] = 0x70 | (value.bytes[6] & 0x0f);
  value.bytes[8] = 0x80 | (value.bytes[8] & 0x3f);
  return value;
}
Query Handle() { return {Identity(16), Identity(48), Identity(80), Identity(112)}; }
bool Matches(const server::ServerCursorRecord& cursor, const Query& handle) {
  return cursor.execution_uuid == handle.execution_uuid.bytes &&
      cursor.result_set_uuid == handle.result_set_uuid.bytes &&
      cursor.row_descriptor_uuid == handle.row_descriptor_uuid.bytes &&
      cursor.snapshot_uuid == handle.snapshot_uuid.bytes;
}
void CheckInvalid(Query candidate) {
  const Query previous{Identity(128), Identity(160), Identity(192), Identity(224)};
  server::ServerCursorRecord cursor;
  Check(server::AdoptQueryResultIdentities(previous, &cursor), "seed existing cursor");
  cursor.row_packet = "unchanged";
  cursor.stream_descriptor_generation = 17;
  Check(!server::AdoptQueryResultIdentities(candidate, &cursor), "invalid cohort refused");
  Check(Matches(cursor, previous) && cursor.row_packet == "unchanged" &&
        cursor.stream_descriptor_generation == 17, "refusal leaves the complete cursor cohort unchanged");
  const Catalog catalog{candidate.execution_uuid, candidate.result_set_uuid,
                        candidate.row_descriptor_uuid, candidate.snapshot_uuid};
  Check(!server::AdoptQueryResultIdentities(catalog, &cursor), "invalid catalog cohort refused");
  Check(Matches(cursor, previous), "catalog refusal cannot partly publish identities");
}
void InvalidIdentities() {
  const auto baseline = Handle();
  for (unsigned field = 0; field < 4; ++field) {
    auto candidate = baseline;
    Uuid* values[] = {&candidate.execution_uuid, &candidate.result_set_uuid,
                     &candidate.row_descriptor_uuid, &candidate.snapshot_uuid};
    const auto original = *values[field];
    *values[field] = {}; CheckInvalid(candidate);
    for (unsigned version = 0; version < 16; ++version) {
      if (version == 7) continue;
      *values[field] = original;
      values[field]->bytes[6] = (version << 4) | (original.bytes[6] & 15);
      CheckInvalid(candidate);
    }
    for (unsigned variant : {0u, 1u, 3u}) {
      *values[field] = original;
      values[field]->bytes[8] = (variant << 6) | (original.bytes[8] & 63);
      CheckInvalid(candidate);
    }
  }
}
void RawPublication() {
  auto handle = Handle();
  server::ServerCursorRecord cursor;
  for (auto* field : {&handle.execution_uuid, &handle.result_set_uuid,
                     &handle.row_descriptor_uuid, &handle.snapshot_uuid}) {
    for (auto& octet : field->bytes) {
      octet ^= 1; // Retains the system version and variant bits.
      fail_allocation = true;
      const bool ok = server::AdoptQueryResultIdentities(handle, &cursor);
      fail_allocation = false;
      Check(ok && Matches(cursor, handle), "query publication preserves every byte without allocation");
      const Catalog catalog{handle.execution_uuid, handle.result_set_uuid,
                            handle.row_descriptor_uuid, handle.snapshot_uuid};
      cursor.execution_uuid = {};
      fail_allocation = true;
      const bool catalog_ok = server::AdoptQueryResultIdentities(catalog, &cursor);
      fail_allocation = false;
      Check(catalog_ok && Matches(cursor, handle), "catalog request identity maps to execution identity exactly");
      octet ^= 1;
    }
  }
  Check(!server::AdoptQueryResultIdentities(handle, nullptr), "null destination refused");
}
void TransactionIdentityShape() {
  const auto valid = Identity(16);
  Check(server::IsCompleteEngineTransactionIdentity(1, valid), "raw transaction identity and local id admitted");
  Check(server::IsCompleteEngineTransactionIdentity(UINT64_MAX, valid), "local transaction counter retains full unsigned width");
  Check(!server::IsCompleteEngineTransactionIdentity(0, valid), "zero local transaction id refused");
  Check(!server::IsCompleteEngineTransactionIdentity(1, {}), "nil transaction UUID refused");
  for (unsigned version = 0; version < 16; ++version) {
    auto candidate = valid;
    candidate.bytes[6] = (version << 4) | (candidate.bytes[6] & 15);
    Check(server::IsCompleteEngineTransactionIdentity(1, candidate) == (version == 7),
          "only the system UUID version identifies an engine transaction");
  }
  for (unsigned variant = 0; variant < 4; ++variant) {
    auto candidate = valid;
    candidate.bytes[8] = (variant << 6) | (candidate.bytes[8] & 63);
    Check(server::IsCompleteEngineTransactionIdentity(1, candidate) == (variant == 2),
          "transaction identity requires the RFC UUID variant");
  }
}
}
int main() {
  InvalidIdentities(); RawPublication(); TransactionIdentityShape();
  std::cout << "checks=" << checks << " failures=" << failures << '\n';
  return failures ? 1 : 0;
}
