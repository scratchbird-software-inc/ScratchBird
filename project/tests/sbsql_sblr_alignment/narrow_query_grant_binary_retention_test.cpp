// SPDX-License-Identifier: MPL-2.0
#define main ExistingNarrowGrantContractMain
#include "narrow_query_binding_authority_scan_bytes_test.cpp"
#undef main
#include <barrier>
#include <cstdio>
#include <new>
#include <thread>
#include <type_traits>
namespace retention_fault {
thread_local long remaining = -1;
thread_local bool hit = false;
}
#ifndef SB_NARROW_GRANT_NO_FAULTS
void* operator new(std::size_t n) {
  if (retention_fault::remaining >= 0 && retention_fault::remaining-- == 0) {
    retention_fault::remaining = 0; retention_fault::hit = true; throw std::bad_alloc();
  }
  if (auto p = std::malloc(n ? n : 1)) return p;
  throw std::bad_alloc();
}
void* operator new[](std::size_t n) { return ::operator new(n); }
void* operator new(std::size_t n, const std::nothrow_t&) noexcept {
  try { return ::operator new(n); } catch (...) { return nullptr; }
}
void* operator new[](std::size_t n, const std::nothrow_t&) noexcept { return ::operator new(n, std::nothrow); }
void operator delete(void* p) noexcept { std::free(p); }
void operator delete[](void* p) noexcept { ::operator delete(p); }
void operator delete(void* p, std::size_t) noexcept { ::operator delete(p); }
void operator delete[](void* p, std::size_t) noexcept { ::operator delete(p); }
#endif
namespace {
unsigned checks = 0, failures = 0, injected = 0;
void Check(bool value, const char* why) {
  ++checks; if (!value) { ++failures; std::fprintf(stderr, "FAIL %s\n", why); }
}
auto IssueRequest(Fixture& fixture) {
  api::EngineNarrowQueryBindingAuthorityIssueRequestV1 issue;
  issue.context = BindingContext(&fixture);
  issue.demand = MakeDemand(issue.context, fixture);
  issue.policy_snapshot_uuid.canonical = NewUuid(platform::UuidKind::object);
  issue.policy_generation = issue.context.authorization_context.policy_epoch;
  issue.maximum_source_rows_per_occurrence = 64;
  issue.maximum_cumulative_source_rows = 64;
  issue.maximum_result_rows = 64;
  issue.maximum_join_combinations = 64;
  issue.maximum_sort_memory_bytes = 1;
  issue.maximum_batch_rows = 16;
  issue.maximum_mga_relation_decoded_bytes_per_pass = kScanBytes;
  issue.maximum_typed_result_transport_bytes_per_packet = kTransportBytes;
  return issue;
}
auto Consume(const api::EngineNarrowQueryBindingAuthorityIssueRequestV1& issue) {
  auto issued = api::IssueNarrowQueryBindingAuthorityV1(issue);
  RequireOk(issued, "fixture issue");
  api::EngineNarrowQueryBindingAuthorityConsumeRequestV1 consume;
  consume.context = issue.context;
  consume.context.trace_tags = {"private_narrow_query_binding_consumer"};
  consume.exact_binding_bytes = std::move(issued.exact_binding_bytes);
  auto result = api::ConsumeNarrowQueryBindingAuthorityV1(consume);
  RequireOk(result, "fixture consume");
  return result;
}
void ReceiptChecks(api::EngineNarrowQueryTypedResultResourceGrantRetentionResultV1& retained) {
  Check(retained.ok && retained.receipt_handle, "retention returned no actual handle");
  Check(retained.status == api::EngineNarrowQueryTypedResultResourceGrantRetentionStatusV1::retained,
        "retained owner lost typed success status");
  if (!retained.receipt_handle) return;
  bool escaped = false;
  auto observed = api::TypedResultProducerGrantObservationV1::stale_or_released;
  retention_fault::remaining = 0; retention_fault::hit = false;
  try {
    observed = retained.receipt_handle->ObserveGrant(retained.grant_receipt_uuid,
        retained.grant_generation, kTransportBytes, kTransportBytes);
  } catch (const std::bad_alloc&) { escaped = true; }
  retention_fault::remaining = -1;
  Check(!escaped && !retention_fault::hit &&
        observed == api::TypedResultProducerGrantObservationV1::live,
        "binary grant observation allocated or refused live owner");
  auto wrong = retained.grant_receipt_uuid; wrong[15] ^= 1;
  Check(retained.receipt_handle->ObserveGrant(wrong, retained.grant_generation,
        kTransportBytes, 1) == api::TypedResultProducerGrantObservationV1::stale_or_released,
        "wrong binary grant identity admitted");
  Check(retained.receipt_handle->ObserveGrant(retained.grant_receipt_uuid,
        retained.grant_generation + 1, kTransportBytes, 1) ==
        api::TypedResultProducerGrantObservationV1::stale_or_released, "wrong grant generation admitted");
  Check(retained.receipt_handle->ObserveGrant(retained.grant_receipt_uuid,
        retained.grant_generation, kTransportBytes, kTransportBytes + 1) ==
        api::TypedResultProducerGrantObservationV1::exhausted, "packet ceiling exceeded");
}
void FaultSweep(Fixture& fixture) {
  auto issue = IssueRequest(fixture);
  auto context = issue.context;
  context.trace_tags = {"private_narrow_query_binding_consumer"};
  for (int mode = 0; mode != 3; ++mode) {
    bool complete = false;
    for (long n = 0; n != 4096 && !complete; ++n) {
      auto consumed = Consume(issue);
      auto call_context = context;
      if (mode == 1) ++call_context.resource_epoch;
      api::EngineNarrowQueryTypedResultResourceGrantRetentionResultV1 first;
      if (mode == 2) {
        first = api::RetainNarrowQueryTypedResultResourceGrantReceiptV1(consumed.authority, context);
        Require(first.ok, "first retention");
      }
      api::EngineNarrowQueryTypedResultResourceGrantRetentionResultV1 retained;
      bool escaped = false;
      retention_fault::remaining = n; retention_fault::hit = false;
      try { retained = api::RetainNarrowQueryTypedResultResourceGrantReceiptV1(consumed.authority, call_context); }
      catch (const std::bad_alloc&) { escaped = true; }
      retention_fault::remaining = -1;
      if (retention_fault::hit) ++injected;
      else complete = true;
      Check(!escaped, "retention allocation failure escaped typed API");
      if (retention_fault::hit) Check(!retained.ok && !retained.receipt_handle, "faulted retention published success");
      if (retention_fault::hit && !escaped)
        Check(retained.status == api::EngineNarrowQueryTypedResultResourceGrantRetentionStatusV1::resource_exhausted &&
              retained.diagnostic.error, "allocation failure lost typed exhaustion status");
      if (mode != 0) Check(!retained.ok, "invalid/repeated retention admitted");
      if (mode != 2 && !retained.ok) {
        retained = api::RetainNarrowQueryTypedResultResourceGrantReceiptV1(consumed.authority, context);
        Check(retained.ok && retained.receipt_handle, "failed retention consumed retain-once authority");
      }
      if (mode == 2) ReceiptChecks(first); else ReceiptChecks(retained);
      api::EngineNarrowQueryBindingAuthoritySnapshotV1 snapshot;
      api::EngineApiDiagnostic diagnostic;
      Check(api::CopyNarrowQueryBindingAuthoritySnapshotV1(consumed.authority, &snapshot, &diagnostic) &&
            snapshot.resource_grant.grant_receipt_uuid == snapshot.binding.resource_grant_receipt_uuid,
            "internal binary grant identity differs from canonical binding");
      if (retained.receipt_handle) {
        Check(!api::RetainNarrowQueryTypedResultResourceGrantReceiptV1(consumed.authority, context).ok,
              "second owning retention admitted");
        retention_fault::remaining = 0;
        retained.receipt_handle->Release(api::TypedResultProducerReleaseReasonV1::shutdown);
        retained.receipt_handle->Release(api::TypedResultProducerReleaseReasonV1::shutdown);
        retention_fault::remaining = -1;
        Check(retained.receipt_handle->ObserveGrant(retained.grant_receipt_uuid,
              retained.grant_generation, kTransportBytes, 1) ==
              api::TypedResultProducerGrantObservationV1::stale_or_released, "released grant stayed live");
      }
      api::ReleaseNarrowQueryBindingAuthorityNoAllocV1(&consumed.authority);
    }
    Check(complete, "retention allocation sweep did not finish");
  }
  Rollback(issue.context);
}
void Concurrent(Fixture& fixture) {
  auto issue = IssueRequest(fixture);
  auto consumed = Consume(issue);
  auto context = issue.context;
  context.trace_tags = {"private_narrow_query_binding_consumer"};
  constexpr unsigned count = 8;
  std::barrier gate(count);
  std::array<api::EngineNarrowQueryTypedResultResourceGrantRetentionResultV1, count> results;
  std::vector<std::thread> threads;
  for (unsigned i = 0; i != count; ++i) threads.emplace_back([&, i] {
    gate.arrive_and_wait();
    results[i] = api::RetainNarrowQueryTypedResultResourceGrantReceiptV1(consumed.authority, context);
  });
  for (auto& t : threads) t.join();
  unsigned owners = 0;
  for (auto& r : results) if (r.ok) { ++owners; ReceiptChecks(r); }
  Check(owners == 1, "concurrent retain published multiple/no owners");
  api::ReleaseNarrowQueryBindingAuthorityNoAllocV1(&consumed.authority);
  for (auto& r : results) if (r.receipt_handle)
    Check(r.receipt_handle->ObserveGrant(r.grant_receipt_uuid, r.grant_generation,
          kTransportBytes, 1) == api::TypedResultProducerGrantObservationV1::stale_or_released,
          "binding revocation left grant live");
  Rollback(issue.context);
}
}
int main() {
  Check(sizeof(api::EngineNarrowQueryResourceGrantV1{}.grant_receipt_uuid) == 16 &&
        std::is_trivially_copyable_v<decltype(api::EngineNarrowQueryResourceGrantV1{}.grant_receipt_uuid)>,
        "engine grant receipt is not binary16");
#ifndef SB_NARROW_GRANT_NO_FAULTS
  {
    auto fixture = MakeFixture();
    FaultSweep(fixture);
  }
#endif
  {
    auto fixture = MakeFixture();
    Concurrent(fixture);
  }
  std::printf("narrow grant retention checks=%u allocation_faults=%u failures=%u\n", checks, injected, failures);
  return failures ? 1 : 0;
}
