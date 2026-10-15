// Copyright (c) 2026 ScratchBird Software Inc.
// SPDX-License-Identifier: MPL-2.0
#include "../support/engine_statement_fixture.hpp"
#include "../support/owned_temp_directory.hpp"
#include "engine/statement_monotonic_clock.hpp"
#include "engine/statement_scratch_memory.hpp"
#include "server_engine_bridge/diagnostic_fields.hpp"
#include "transaction/transaction_api.hpp"
#include <atomic>
#include <barrier>
#include <cstdlib>
#include <iostream>
#include <limits>
#include <thread>
#include <type_traits>
#if defined(SB_NATIVE_MONOTONIC_FAULT_TEST)
#include <cerrno>
#include <sys/wait.h>
#include <unistd.h>
#include <time.h>
#endif

thread_local long allocation_budget = -1;
thread_local bool allocation_fault_triggered = false;
void* operator new(std::size_t n) {
  if (allocation_budget >= 0 && allocation_budget-- == 0) {
    allocation_fault_triggered = true; throw std::bad_alloc();
  }
  if (auto* p = std::malloc(n ? n : 1)) return p;
  throw std::bad_alloc();
}
void* operator new[](std::size_t n) { return ::operator new(n); }
void operator delete(void* p) noexcept { std::free(p); }
void operator delete[](void* p) noexcept { std::free(p); }
void operator delete(void* p, std::size_t) noexcept { std::free(p); }
void operator delete[](void* p, std::size_t) noexcept { std::free(p); }
#include "../support/aligned_allocation_fault_bridge.hpp"

#if defined(SB_NATIVE_MONOTONIC_FAULT_TEST)
// Interpose the actual production OS call, not a caller-authorized clock.
thread_local int native_read_mode = 0;
thread_local timespec native_read_value{};
extern "C" int __real_clock_gettime(clockid_t, timespec*) noexcept;
extern "C" int __wrap_clock_gettime(clockid_t which, timespec* value) noexcept {
  if (which == CLOCK_MONOTONIC_RAW && native_read_mode) {
    if (native_read_mode == 1) { errno = EIO; return -1; }
    *value = native_read_value;
    return 0;
  }
  return __real_clock_gettime(which, value);
}
#endif

namespace api = scratchbird::engine::internal_api;
namespace bridge = scratchbird::server_engine_bridge;
namespace tests = scratchbird::tests;
namespace mem = scratchbird::core::memory;
namespace db = scratchbird::storage::database;
namespace time_service = scratchbird::core::time;
namespace platform = scratchbird::core::platform;
using Clock = api::RetainedStatementMonotonicClockV1;
using Owner = std::optional<Clock>;
using Error = api::StatementMonotonicClockReadErrorV1;
std::atomic<unsigned> checks{0};
void Check(bool ok, const char* why) { ++checks; if (!ok) throw std::runtime_error(why); }
static_assert(!std::is_default_constructible_v<Clock> && !std::is_copy_constructible_v<Clock>);
static_assert(std::is_nothrow_move_constructible_v<Clock>);

#if defined(SB_NATIVE_MONOTONIC_FAULT_TEST)
thread_local bool fail_domain_native_read = false;
thread_local unsigned domain_native_reads = 0;
extern "C" time_service::NativeMonotonicTimeSample
__real__ZN11scratchbird4core4time23ReadNativeMonotonicTimeEv() noexcept;
extern "C" time_service::NativeMonotonicTimeSample
__wrap__ZN11scratchbird4core4time23ReadNativeMonotonicTimeEv() noexcept {
  if (!fail_domain_native_read) return __real__ZN11scratchbird4core4time23ReadNativeMonotonicTimeEv();
  // Reach the actual OS reader while faulting only the native domain issuer's
  // direct call, not the earlier ordinary node UUID/snapshot observations.
  ++domain_native_reads;
  const auto previous = native_read_mode; native_read_mode = 1;
  const auto sample = __real__ZN11scratchbird4core4time23ReadNativeMonotonicTimeEv();
  native_read_mode = previous;
  return sample;
}

void ColdDomainFailure(tests::FixtureEngineSession& session, const api::EngineRequestContext& base) {
  const auto before = mem::DefaultMemoryManager().Snapshot();
  bridge::StatementContextAcquireRequest request;
  request.engine_context = &base; request.exact_transaction_uuid = base.transaction_uuid;
  bridge::StatementContextReceiptHandle handle;
  bridge::StatementContextReceiptView view; sb_engine_result_t result = nullptr;
  fail_domain_native_read = true;
  const auto status = bridge::AcquireStatementContextReceipt(session.get(), &request, &handle, &view, &result);
  fail_domain_native_read = false;
  bridge::EngineDiagnosticSnapshot diagnostic;
  const bool exact_cause = bridge::CopyEngineDiagnosticSnapshot(result, 0, &diagnostic) &&
      diagnostic.code == "TIME.SOURCE_FAILED" &&
      diagnostic.message_key == "engine.statement_context.monotonic_clock_unavailable";
  Check(result && sb_engine_result_release(result) == SB_ENGINE_STATUS_OK, "cold clock diagnostic cleanup");
  const auto after = mem::DefaultMemoryManager().Snapshot();
  Check(status == SB_ENGINE_STATUS_INTERNAL_ERROR && !handle && view.receipt_uuid.is_nil() &&
      domain_native_reads == 1 && exact_cause, "cold native clock read failure published receipt or missed owning cause");
  Check(before.current_bytes == after.current_bytes && before.active_allocation_count == after.active_allocation_count &&
      before.reserved_capacity_bytes == after.reserved_capacity_bytes &&
      before.active_capacity_reservation_count == after.active_capacity_reservation_count,
      "cold native clock admission failure leaked actual source backing/capacity");
  // The following first real Statement acquisition is the successful retry.
}
#endif

struct Statement {
  bridge::StatementContextReceiptHandle handle;
  api::EngineRequestContext context;
  Statement(tests::FixtureEngineSession& session, const api::EngineRequestContext& base) {
    bridge::StatementContextAcquireRequest request;
    request.engine_context = &base; request.exact_transaction_uuid = base.transaction_uuid;
    bridge::StatementContextReceiptView view;
    sb_engine_result_t result = nullptr;
    const auto status = bridge::AcquireStatementContextReceipt(session.get(), &request, &handle, &view, &result);
    tests::FixtureEngineSession::Check(status, result, "clock receipt");
    try {
      tests::FixtureEngineSession::Check(bridge::CopyStatementContextEngineContextV1(
          handle, &context, nullptr), nullptr, "clock context");
    } catch (...) { Release(); throw; }
  }
  void Release() {
    if (!handle) return;
    tests::FixtureEngineSession::Check(bridge::ReleaseStatementContextReceipt(handle), nullptr, "clock release");
    handle = {};
  }
  ~Statement() { Release(); }
  sb_engine_status_t Retain(Owner& output) const {
    return api::RetainStatementMonotonicClockV1(handle, context.session_uuid.bytes,
        context.statement_receipt_uuid.bytes, &output);
  }
};

void NativeSamples(Clock& clock) {
  allocation_budget = 0; allocation_fault_triggered = false;
  const auto one = clock.Sample(); const auto two = clock.Sample();
  const auto native = time_service::ReadNativeMonotonicTime();
  allocation_budget = -1;
  Check(one.ok() && two.ok() && native.ok() && two.nanoseconds >= one.nanoseconds &&
        native.nanoseconds >= two.nanoseconds && !allocation_fault_triggered,
        "native sample failed, regressed, allocated or changed source");
#if defined(SB_NATIVE_MONOTONIC_FAULT_TEST)
  native_read_mode = 1;
  allocation_budget = 0; allocation_fault_triggered = false;
  const auto failed = clock.Sample(); const auto direct_failed = time_service::ReadNativeMonotonicTime();
  allocation_budget = -1;
  const auto diagnostic = time_service::ReadMonotonicTime();
  const auto snapshot = time_service::ReadLocalNodeClockSnapshot();
  native_read_mode = 0;
  Check(failed.error == Error::source_unavailable && failed.nanoseconds == 0 &&
      !direct_failed.ok() && !allocation_fault_triggered && !diagnostic.ok() &&
      diagnostic.diagnostic.diagnostic_code == "TIME.SOURCE_FAILED" && !snapshot.ok(),
      "OS read failure became successful ticks or lost its real failure vector");
  struct Vector { timespec value; Error error; std::uint64_t expected; };
  constexpr auto maximum = std::numeric_limits<std::uint64_t>::max();
  const Vector vectors[] = {
      {{0, 0}, Error::none, 0}, {{1, 2}, Error::none, 1000000002},
      {{-1, 0}, Error::invalid_sample, 0}, {{1, -1}, Error::invalid_sample, 0},
      {{0, 1000000000}, Error::invalid_sample, 0},
      {{static_cast<time_t>(maximum / 1000000000), static_cast<long>(maximum % 1000000000)}, Error::none, maximum},
      {{static_cast<time_t>(maximum / 1000000000), static_cast<long>(maximum % 1000000000 + 1)}, Error::out_of_range, 0},
      {{static_cast<time_t>(maximum / 1000000000 + 1), 0}, Error::out_of_range, 0}};
  for (const auto& vector : vectors) {
    native_read_value = vector.value; native_read_mode = 2;
    allocation_budget = 0; allocation_fault_triggered = false;
    const auto result = clock.Sample();
    allocation_budget = -1; native_read_mode = 0;
    Check(result.error == vector.error && result.nanoseconds == vector.expected && !allocation_fault_triggered,
        "native clock accepted invalid/overflow input, rejected finite supremum or allocated");
  }
  // No global clamping or fabricated progress. Regression selection belongs
  // to the owning operation, not shared sample publication order.
  native_read_mode = 2; native_read_value = {9, 0};
  const auto high = clock.Sample(); native_read_value = {8, 0}; const auto low = clock.Sample();
  const auto equal = clock.Sample(); native_read_mode = 0;
  Check(high.ok() && low.ok() && equal.ok() && high.nanoseconds == 9000000000 &&
      low.nanoseconds == 8000000000 && equal.nanoseconds == low.nanoseconds,
      "clock hid actual regression or manufactured advancing ticks");
#endif
}

void AdmissionAndLifetime(tests::FixtureEngineSession& session, const api::EngineRequestContext& base) {
  Statement first(session, base), second(session, base);
  Owner owner, other;
  Check(api::RetainStatementMonotonicClockV1({}, {}, {}, &owner) == SB_ENGINE_STATUS_INVALID_ARGUMENT && !owner,
      "empty receipt issued clock");
  Check(api::RetainStatementMonotonicClockV1(first.handle, first.context.session_uuid.bytes,
      first.context.statement_receipt_uuid.bytes, nullptr) == SB_ENGINE_STATUS_INVALID_ARGUMENT,
      "null clock output admitted");
  for (unsigned i = 0; i < 16; ++i) {
    auto foreign = first.context.session_uuid.bytes; foreign[i] ^= 0x80;
    Check(api::RetainStatementMonotonicClockV1(first.handle, foreign, first.context.statement_receipt_uuid.bytes,
        &owner) == SB_ENGINE_STATUS_SECURITY_DENIED && !owner, "foreign session retained clock");
    foreign = first.context.statement_receipt_uuid.bytes; foreign[i] ^= 0x80;
    Check(api::RetainStatementMonotonicClockV1(first.handle, first.context.session_uuid.bytes, foreign,
        &owner) == SB_ENGINE_STATUS_INVALID_HANDLE && !owner, "foreign receipt retained clock");
  }
  const auto before = mem::DefaultMemoryManager().Snapshot();
  allocation_budget = 0; allocation_fault_triggered = false;
  const auto retained = first.Retain(owner);
  allocation_budget = -1;
  const auto after = mem::DefaultMemoryManager().Snapshot();
  Check(retained == SB_ENGINE_STATUS_OK && owner && owner->binding() && !allocation_fault_triggered &&
      before.current_bytes == after.current_bytes && before.active_allocation_count == after.active_allocation_count,
      "clock retention allocated a second source/control block or lacked owner");
  Check(first.Retain(owner) == SB_ENGINE_STATUS_INVALID_ARGUMENT, "occupied clock owner replaced");
  Check(second.Retain(other) == SB_ENGINE_STATUS_OK && other->binding(), "second clock retention");
  const auto binding = *owner->binding();
  Check(binding.owner.database == first.context.database_uuid.bytes && binding.owner.session == first.context.session_uuid.bytes &&
      binding.owner.transaction == first.context.transaction_uuid.bytes && binding.owner.statement == first.context.statement_uuid.bytes &&
      binding.owner.receipt == first.context.statement_receipt_uuid.bytes && binding.clock_generation == 1 &&
      scratchbird::core::uuid::IsEngineIdentityUuid(platform::Uuid{binding.clock}), "clock lost exact binary native binding");
  Check(binding.clock != binding.owner.receipt && binding.clock != binding.owner.statement && binding.clock != binding.owner.transaction &&
      binding.clock != binding.owner.session && binding.clock != binding.owner.database &&
      binding.clock == other->binding()->clock && binding.clock_generation == other->binding()->clock_generation &&
      binding.owner.receipt != other->binding()->owner.receipt, "clock domain was a receipt alias or per-receipt invented epoch");
  const auto concurrent_before = mem::DefaultMemoryManager().Snapshot();
  std::atomic<bool> admitted{true}; std::vector<std::thread> issuers;
  for (unsigned i = 0; i < 8; ++i) issuers.emplace_back([&] {
    try {
      Statement concurrent(session, base); Owner clock;
      if (concurrent.Retain(clock) != SB_ENGINE_STATUS_OK || !clock || !clock->binding() ||
          clock->binding()->clock != binding.clock || clock->binding()->clock_generation != binding.clock_generation ||
          clock->binding()->owner.receipt == binding.owner.receipt || !clock->Sample().ok()) admitted = false;
    } catch (...) { admitted = false; }
  });
  for (auto& issuer : issuers) issuer.join();
  const auto concurrent_after = mem::DefaultMemoryManager().Snapshot();
  Check(admitted && concurrent_before.current_bytes == concurrent_after.current_bytes &&
      concurrent_before.active_allocation_count == concurrent_after.active_allocation_count &&
      concurrent_before.reserved_capacity_bytes == concurrent_after.reserved_capacity_bytes &&
      concurrent_before.active_capacity_reservation_count == concurrent_after.active_capacity_reservation_count,
      "concurrent receipt clock admission changed immutable domain or leaked actual backing");
  NativeSamples(*owner);
  Check(api::RunWithStatementScratchMemoryV1(first.context, [&](auto&) {
    Owner nested;
    Check(first.Retain(nested) == SB_ENGINE_STATUS_CONFLICT && !nested, "clock retention entered guarded registry");
    NativeSamples(*owner);
  }) == SB_ENGINE_STATUS_OK, "actual guarded clock sampling");
  Clock moved(std::move(*other));
  Check(!other->binding() && other->Sample().error == Error::owner_unavailable &&
      other->Sample().nanoseconds == 0 && moved.binding() && moved.Sample().ok(), "moved owner fabricated clock custody");
#if defined(SB_NATIVE_MONOTONIC_FAULT_TEST)
  const auto child = ::fork();
  Check(child >= 0, "clock fork failed");
  if (child == 0) {
    // Supported fork/exec boundary: never destroy inherited MM/locks here.
    Owner inherited;
    const auto refused = first.Retain(inherited);
    const auto sample = owner->Sample();
    ::_exit(refused == SB_ENGINE_STATUS_INVALID_HANDLE && !inherited && !owner->binding() &&
        sample.error == Error::process_mismatch && sample.nanoseconds == 0 ? 0 : 73);
  }
  int status = 0;
  Check(::waitpid(child, &status, 0) == child && WIFEXITED(status) && WEXITSTATUS(status) == 0,
      "fork child reused parent's native clock domain or entered inherited receipt registry");
  Check(owner->Sample().ok(), "fork poisoned parent clock");
#endif
  const auto retired = first.handle; first.Release();
  Owner late;
  Check(api::RetainStatementMonotonicClockV1(retired, binding.owner.session, binding.owner.receipt, &late) ==
      SB_ENGINE_STATUS_INVALID_HANDLE && !late && owner->Sample().ok(), "public retirement invalidated owned clock or allowed late retention");
  second.Release();
}

void ConcurrentShutdown(tests::FixtureEngineSession& session, const api::EngineRequestContext& base) {
  Statement statement(session, base); Owner owner;
  Check(statement.Retain(owner) == SB_ENGINE_STATUS_OK, "shutdown clock acquisition");
  const auto handle = statement.handle;
  std::barrier start(9); std::atomic<bool> valid{true}; std::vector<std::thread> workers;
  for (unsigned i = 0; i < 8; ++i) workers.emplace_back([&] {
    start.arrive_and_wait();
    allocation_budget = 0; allocation_fault_triggered = false;
    std::uint64_t previous = 0;
    for (unsigned n = 0; n < 1024; ++n) {
      const auto sample = owner->Sample();
      if (!sample.ok() || sample.nanoseconds < previous || !owner->binding()) valid = false;
      previous = sample.nanoseconds;
    }
    if (allocation_fault_triggered) valid = false;
    allocation_budget = -1;
  });
  start.arrive_and_wait(); session.Close();
  for (auto& worker : workers) worker.join();
  Check(valid && owner->Sample().ok(), "concurrent clock samples used closed session, allocated or regressed");
  Check(bridge::ReleaseStatementContextReceipt(handle) == SB_ENGINE_STATUS_ALREADY_RELEASED,
      "session shutdown retained public clock receipt");
  statement.handle = {};
  Owner late;
  Check(statement.Retain(late) == SB_ENGINE_STATUS_INVALID_ARGUMENT && !late,
      "session shutdown allowed new clock owner");
}

int main() try {
  Check(!mem::RetainDefaultMemoryManagerV1(), "clock lookup initialized a fallback memory manager");
  Check(mem::ConfigureDefaultMemoryManagerForFixture(mem::DefaultLocalEngineMemoryPolicy(),
      "statement_monotonic_clock_ownership").ok(), "actual clock fixture memory policy");
  const auto initial = mem::DefaultMemoryManager().Snapshot();
  tests::OwnedTempDirectory directory;
  db::DatabaseCreateConfig create; create.path = (directory.path() / "clock.sbdb").string(); create.page_size = 8192;
  create.database_uuid = {platform::UuidKind::database, api::GenerateCrudEngineUuid("database")};
  create.filespace_uuid = {platform::UuidKind::filespace, api::GenerateCrudEngineUuid("filespace")};
  tests::ConfigureCredentialedFixtureBootstrap(create);
  Check(db::CreateDatabaseFile(create).ok(), "actual credentialed clock database");
  auto context = tests::BootstrapFixtureOwnerContext(create); tests::FixtureEngineSession session(context);
  api::EngineBeginTransactionRequest begin; begin.context = context; begin.isolation_level = "read_committed";
  const auto begun = api::EngineBeginTransaction(begin); Check(begun.ok, "actual clock MGA BEGIN");
  context.transaction_uuid = begun.transaction_uuid; context.local_transaction_id = begun.local_transaction_id;
  context.snapshot_visible_through_local_transaction_id = begun.snapshot_visible_through_local_transaction_id;
#if defined(SB_NATIVE_MONOTONIC_FAULT_TEST)
  ColdDomainFailure(session, context);
#endif
  AdmissionAndLifetime(session, context); ConcurrentShutdown(session, context);
  const auto final = mem::DefaultMemoryManager().Snapshot();
  Check(final.current_bytes == initial.current_bytes && final.active_allocation_count == initial.active_allocation_count &&
      final.reserved_capacity_bytes == initial.reserved_capacity_bytes &&
      final.active_capacity_reservation_count == initial.active_capacity_reservation_count,
      "clock/control-block retirement leaked actual memory or capacity");
  directory.Cleanup();
  std::cout << "statement_monotonic_clock_ownership_checks=" << checks.load() << '\n';
  return 0;
} catch (const std::exception& error) {
  allocation_budget = -1;
#if defined(SB_NATIVE_MONOTONIC_FAULT_TEST)
  native_read_mode = 0;
#endif
  std::cerr << "statement monotonic clock: " << error.what() << '\n'; return 1;
}
