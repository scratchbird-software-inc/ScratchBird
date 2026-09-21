// Copyright (c) 2026 ScratchBird Software Inc.
// SPDX-License-Identifier: MPL-2.0
#include "server/statement_receipt_release.hpp"

#include <atomic>
#include <cstdlib>
#include <iostream>
#include <latch>
#include <new>
#include <thread>

namespace {
bool fail_allocation = false;
long allocation_budget = -1;
unsigned checks = 0, failures = 0;
void Check(bool ok, const char* message) {
  ++checks;
  if (!ok && failures++ < 20) std::cerr << "FAIL " << message << '\n';
}
}
void* operator new(std::size_t size) {
  if (fail_allocation || allocation_budget == 0) throw std::bad_alloc();
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
namespace server = scratchbird::server;
using Uuid = scratchbird::core::platform::Uuid;
Uuid Identity(unsigned seed) {
  Uuid id;
  for (unsigned i = 0; i < 16; ++i) id.bytes[i] = seed + i;
  id.bytes[6] = 0x70 | (id.bytes[6] & 15);
  id.bytes[8] = 0x80 | (id.bytes[8] & 63);
  return id;
}
const auto statement = Identity(16), receipt_id = Identity(64);
server::ServerStatementContextRecord Record(std::uint64_t handle = 7) {
  server::ServerStatementContextRecord record;
  record.statement_uuid = statement;
  record.receipt.opaque_id = handle;
  record.view.statement_uuid = statement;
  record.view.receipt_uuid = receipt_id;
  return record;
}
bool Retained(const server::ServerSessionRegistry& registry) {
  const auto found = registry.statement_contexts_by_statement_uuid.find(statement);
  return found != registry.statement_contexts_by_statement_uuid.end() &&
      found->second.released && !found->second.release_in_progress &&
      found->second.receipt.opaque_id == 7 && found->second.view.receipt_uuid == receipt_id;
}
void FailureAndRetry() {
  for (const auto status : {SB_ENGINE_STATUS_INTERNAL_ERROR, SB_ENGINE_STATUS_INVALID_HANDLE,
       SB_ENGINE_STATUS_RESOURCE_EXHAUSTED, SB_ENGINE_STATUS_CONFLICT,
       SB_ENGINE_STATUS_SECURITY_DENIED}) {
    server::ServerSessionRegistry registry;
    registry.statement_contexts_by_statement_uuid.emplace(statement, Record());
    bool exact = false, revoked = false;
    const auto release = [&](auto handle, const Uuid& receipt) {
      exact = handle.opaque_id == 7 && receipt == receipt_id;
      const auto found = registry.statement_contexts_by_statement_uuid.find(statement);
      revoked = found != registry.statement_contexts_by_statement_uuid.end() &&
          found->second.released && found->second.release_in_progress;
      return status;
    };
    fail_allocation = true;
    const bool result = server::ReleaseOwnedStatementReceipt(&registry, statement, release);
    fail_allocation = false;
    Check(!result && exact && revoked, "release calls exact owner only after logical revocation");
    Check(Retained(registry), "failed physical release retains the raw owner for retry without allocation");
    Check(server::ReleaseOwnedStatementReceipt(&registry, statement,
        [](auto, const Uuid&) { return SB_ENGINE_STATUS_OK; }), "failed release can be retried");
    Check(registry.statement_contexts_by_statement_uuid.empty(), "successful retry removes owner");
  }
  server::ServerSessionRegistry registry;
  registry.statement_contexts_by_statement_uuid.emplace(statement, Record());
  Check(!server::ReleaseOwnedStatementReceipt(&registry, statement,
      [](auto, const Uuid&) -> sb_engine_status_t { throw std::bad_alloc(); }),
      "throwing owner operation cannot escape destructor cleanup");
  Check(Retained(registry), "exception preserves logically revoked cleanup ownership");
  Check(server::ReleaseOwnedStatementReceipt(&registry, statement,
      [](auto, const Uuid&) { return SB_ENGINE_STATUS_ALREADY_RELEASED; }),
      "engine-confirmed terminal release is idempotent");
}
void ExactKeysAndReplacement() {
  server::ServerSessionRegistry registry;
  registry.statement_contexts_by_statement_uuid.emplace(statement, Record());
  unsigned calls = 0;
  const auto release = [&](auto, const Uuid&) { ++calls; return SB_ENGINE_STATUS_OK; };
  for (unsigned i = 0; i < 16; ++i) {
    auto other = statement; other.bytes[i] ^= 1;
    Check(!server::ReleaseOwnedStatementReceipt(&registry, other, release),
          "every byte of the statement key isolates ownership");
  }
  Check(!server::ReleaseOwnedStatementReceipt(&registry, {}, release), "nil key refused");
  Check(!server::ReleaseOwnedStatementReceipt(nullptr, statement, release), "null registry refused");
  Check(calls == 0 && registry.statement_contexts_by_statement_uuid.size() == 1,
        "nonexistent keys never invoke engine cleanup");
  Check(!server::ReleaseOwnedStatementReceipt(&registry, statement,
      [&](auto, const Uuid&) {
        std::lock_guard lock(*registry.statement_context_mutex);
        registry.statement_contexts_by_statement_uuid.erase(statement);
        registry.statement_contexts_by_statement_uuid.emplace(statement, Record(99));
        return SB_ENGINE_STATUS_OK;
      }), "stale completion cannot release replacement handle");
  const auto found = registry.statement_contexts_by_statement_uuid.find(statement);
  Check(found != registry.statement_contexts_by_statement_uuid.end() &&
        found->second.receipt.opaque_id == 99 && !found->second.released,
        "replacement owner survives old completion");
}
void ConcurrentRelease() {
  server::ServerSessionRegistry registry;
  registry.statement_contexts_by_statement_uuid.emplace(statement, Record());
  std::latch entered(1), finish(1);
  bool first = false;
  std::atomic<unsigned> calls{0};
  std::thread worker([&] {
    first = server::ReleaseOwnedStatementReceipt(&registry, statement,
        [&](auto, const Uuid&) {
          ++calls; entered.count_down(); finish.wait();
          return SB_ENGINE_STATUS_OK;
        });
  });
  entered.wait();
  const bool second = server::ReleaseOwnedStatementReceipt(&registry, statement,
      [&](auto, const Uuid&) { ++calls; return SB_ENGINE_STATUS_OK; });
  Check(!second && calls == 1, "concurrent release does not duplicate the owning operation");
  finish.count_down(); worker.join();
  Check(first && registry.statement_contexts_by_statement_uuid.empty(),
        "first release owns terminal removal");
}
void SessionSelectionAndEnumerationFailure() {
  server::ServerSessionRegistry registry;
  const auto first_session = Identity(128).bytes, other_session = Identity(160).bytes;
  for (unsigned i = 0; i < 3; ++i) {
    auto record = Record(7 + i);
    record.statement_uuid = Identity(16 + 16 * i);
    record.view.statement_uuid = record.statement_uuid;
    record.session_uuid = i == 2 ? other_session : first_session;
    registry.statement_contexts_by_statement_uuid.emplace(record.statement_uuid, record);
  }
  unsigned calls = 0;
  const auto release = [&](auto handle, const Uuid&) {
    ++calls;
    return handle.opaque_id == 7 ? SB_ENGINE_STATUS_INTERNAL_ERROR : SB_ENGINE_STATUS_OK;
  };
  for (long budget = 0; budget < 2; ++budget) {
    allocation_budget = budget;
    const auto failed = server::ReleaseOwnedStatementReceiptsForSession(&registry, first_session, release);
    allocation_budget = -1;
    Check(failed == 0 && calls == 0 && registry.statement_contexts_by_statement_uuid.size() == 3,
          "each enumeration allocation failure neither calls engine nor loses owners");
  }
  bool untouched = true;
  for (const auto& [key, record] : registry.statement_contexts_by_statement_uuid)
    untouched = untouched && !record.released && !record.release_in_progress;
  Check(untouched, "enumeration failure leaves every receipt active");
  Check(server::ReleaseOwnedStatementReceiptsForSession(&registry, first_session, release) == 1 && calls == 2,
        "session release counts only confirmed owners");
  const auto foreign = registry.statement_contexts_by_statement_uuid.find(Identity(48));
  Check(foreign != registry.statement_contexts_by_statement_uuid.end() && !foreign->second.released,
        "another session keeps its execution authority");
  Check(Retained(registry), "failed selected receipt remains retryable");
  Check(server::ReleaseOwnedStatementReceiptsForSession(&registry, first_session,
      [](auto, const Uuid&) { return SB_ENGINE_STATUS_OK; }) == 1,
      "session retry includes logically revoked receipts");
  Check(registry.statement_contexts_by_statement_uuid.size() == 1,
        "session retry removes only its own remaining receipt");
}
}
int main() {
  FailureAndRetry(); ExactKeysAndReplacement(); ConcurrentRelease();
  SessionSelectionAndEnumerationFailure();
  std::cout << "checks=" << checks << " failures=" << failures << '\n';
  return failures ? 1 : 0;
}
