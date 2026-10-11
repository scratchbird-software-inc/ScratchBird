// Copyright (c) 2026 ScratchBird Software Inc.
// SPDX-License-Identifier: MPL-2.0

#include "datatype_blob.hpp"

#include <algorithm>
#include <array>
#include <atomic>
#include <cstddef>
#include <cstdlib>
#include <exception>
#include <iostream>
#include <limits>
#include <mutex>
#include <cstring>
#include <new>
#include <string_view>
#include <sys/resource.h>
#include <sys/mman.h>
#if defined(__linux__)
#include <sys/prctl.h>
#endif
#include <sys/wait.h>
#include <thread>
#include <type_traits>
#include <unistd.h>
#include <utility>

namespace allocation_probe {
std::atomic<bool> enabled{false};
std::atomic<std::uint64_t> calls{0};

void Observe() noexcept {
  if (enabled.load(std::memory_order_relaxed))
    calls.fetch_add(1, std::memory_order_relaxed);
}
}  // namespace allocation_probe

void* operator new(std::size_t size) {
  allocation_probe::Observe();
  if (void* value = std::malloc(size == 0 ? 1 : size)) return value;
  throw std::bad_alloc();
}
void* operator new[](std::size_t size) { return ::operator new(size); }
void operator delete(void* value) noexcept { std::free(value); }
void operator delete[](void* value) noexcept { std::free(value); }
void operator delete(void* value, std::size_t) noexcept { std::free(value); }
void operator delete[](void* value, std::size_t) noexcept {
  std::free(value);
}
void* operator new(std::size_t size, std::align_val_t alignment) {
  allocation_probe::Observe();
  void* value = nullptr;
  const auto bytes = size == 0 ? std::size_t{1} : size;
  if (posix_memalign(&value, static_cast<std::size_t>(alignment), bytes) == 0)
    return value;
  throw std::bad_alloc();
}
void* operator new[](std::size_t size, std::align_val_t alignment) {
  return ::operator new(size, alignment);
}
void operator delete(void* value, std::align_val_t) noexcept {
  std::free(value);
}
void operator delete[](void* value, std::align_val_t) noexcept {
  std::free(value);
}
void operator delete(void* value, std::size_t, std::align_val_t) noexcept {
  std::free(value);
}
void operator delete[](void* value, std::size_t,
                       std::align_val_t) noexcept {
  std::free(value);
}

namespace dt = scratchbird::core::datatypes;
namespace platform = scratchbird::core::platform;

namespace scratchbird::core::datatypes {

class BlobLifetimeRuntimeConformanceAccessV3 final {
 public:
  static void SetCallbackLimit(BlobLifetimeBudgetLedgerV3Generation1& ledger,
                               u64 limit) noexcept {
    BlobLifetimeBudgetConfigurationV3Generation1 configuration{};
    configuration.limits.fill(std::numeric_limits<u64>::max());
    configuration.limits[static_cast<std::size_t>(
        BlobLifetimeResourceV3::lifetime_callback_calls) - 1] = limit;
    if (!ledger.Configure(configuration) || !ledger.Freeze()) std::terminate();
  }

  static bool Configure(
      BlobLifetimeBudgetLedgerV3Generation1& ledger,
      const BlobLifetimeBudgetConfigurationV3Generation1& configuration)
      noexcept {
    return ledger.Configure(configuration);
  }
  static bool Freeze(BlobLifetimeBudgetLedgerV3Generation1& ledger) noexcept {
    return ledger.Freeze();
  }
  static BlobLifetimeBudgetSnapshotV3Generation1 Snapshot(
      BlobLifetimeBudgetLedgerV3Generation1& ledger) noexcept {
    return ledger.SnapshotForConformance();
  }
  static bool Charge(BlobLifetimeBudgetLedgerV3Generation1& ledger,
                     BlobLifetimeResourceV3 resource, u64 amount,
                     u64* available) noexcept {
    return ledger.Charge(resource, amount, available);
  }
  static bool Reserve(BlobLifetimeBudgetLedgerV3Generation1& ledger,
                      u64 ordinary, u64 cleanup, u64* available) noexcept {
    return ledger.ReserveOrdinaryAndCleanup(ordinary, cleanup, available);
  }
  static BlobCleanupReservationConsumptionV3 Consume(
      BlobLifetimeBudgetLedgerV3Generation1& ledger, u64 amount) noexcept {
    return ledger.ConsumeCleanup(amount);
  }
  static bool Cancel(BlobLifetimeBudgetLedgerV3Generation1& ledger,
                     u64 amount) noexcept {
    return ledger.CancelCleanup(amount);
  }

  static BlobLifetimeAuthorityAdmissionV3Generation1 Admission(
      const BlobLifetimeAuthorityV3& authority,
      const BaseBlobLifetimeReceiverServicesV3Generation1& services,
      const BlobReceiverLifetimePinOpsV3Generation1& pin_ops,
      BlobLifetimeBudgetLedgerV3Generation1& ledger,
      u64 pin_cookie) noexcept {
    return BlobLifetimeAuthorityAdmissionV3Generation1(
        authority, services, pin_ops, pin_cookie, pin_ops, pin_cookie + 1,
        &ledger,
        services.monotonic_clock, services.monotonic_clock);
  }

  static bool AdmissionAdmitted(
      const BlobLifetimeAuthorityAdmissionV3Generation1& admission) noexcept {
    return admission.admitted_;
  }

  static BlobBoundMaterializedCarrierV3Generation1 Carrier(
      const BlobMaterializedCarrierBindingV3& binding,
      BlobValueStateV3 state, const byte* data, u64 length) noexcept {
    return BlobBoundMaterializedCarrierV3Generation1(
        binding, state, data, length);
  }

  static platform::Uuid CarrierUuid(
      const BlobBoundMaterializedCarrierV3Generation1& carrier) noexcept {
    return carrier.binding_.carrier_uuid;
  }

  static BlobTrustedInternalVisitorV3 Visitor(
      BlobTrustedInternalVisitorV3::Function function, void* context) noexcept {
    return BlobTrustedInternalVisitorV3(function, context);
  }

  static u64 Invoked(const BlobLifetimeBudgetLedgerV3Generation1& ledger)
      noexcept {
    return ledger.counters_[static_cast<std::size_t>(
        BlobLifetimeResourceV3::lifetime_callback_calls)].invoked;
  }
  static u64 Invoked(const BlobLifetimeBudgetLedgerV3Generation1& ledger,
                     BlobLifetimeResourceV3 resource) noexcept {
    return ledger.counters_[static_cast<std::size_t>(resource)].invoked;
  }
  static u64 ReservedCleanup(
      const BlobLifetimeBudgetLedgerV3Generation1& ledger) noexcept {
    return ledger.counters_[static_cast<std::size_t>(
        BlobLifetimeResourceV3::lifetime_callback_calls)].reserved_cleanup;
  }
  static void SetLimit(BlobLifetimeBudgetLedgerV3Generation1& ledger,
                       BlobLifetimeResourceV3 resource, u64 limit) noexcept {
    ledger.counters_[static_cast<std::size_t>(resource)] = {limit, 0, 0};
  }
  static void SetTokenClockGeneration(
      BlobLifetimeAuthorityAdmissionV3Generation1& admission,
      u64 generation) noexcept {
    admission.token_clock_.clock_generation = generation;
  }

  static void SetCounter(BlobLifetimeBudgetLedgerV3Generation1& ledger,
                         BlobLifetimeResourceV3 resource, u64 limit,
                         u64 invoked, u64 reserved) noexcept {
    ledger.counters_[static_cast<std::size_t>(resource)] =
        {limit, invoked, reserved};
  }

  static BlobLifetimeCounterV3 Counter(
      const BlobLifetimeBudgetLedgerV3Generation1& ledger,
      BlobLifetimeResourceV3 resource) noexcept {
    return ledger.counters_[static_cast<std::size_t>(resource)];
  }

  static bool LedgerQuarantined(
      const BlobLifetimeBudgetLedgerV3Generation1& ledger) noexcept {
    return ledger.terminal_quarantined();
  }

  static BlobLifetimeRuntimeResultV3 ControlReportInvariant(
      BlobRetainedLifetimeLeaseV3& lease, u8 event, u8 operation,
      u8 phase, u8 callback_code, u32 owner_entrants) noexcept {
    return lease.ReportInvariant(
        static_cast<BlobLifetimeInvariantEventV3>(event),
        static_cast<BlobLifetimePhaseV3>(phase), callback_code, operation,
        owner_entrants);
  }

  static platform::Uuid DescriptorUuid(
      const BlobRetainedLifetimeLeaseV3& lease) noexcept {
    return lease.descriptor_uuid_;
  }

  static u64 DescriptorGeneration(
      const BlobRetainedLifetimeLeaseV3& lease) noexcept {
    return lease.descriptor_generation_;
  }

  static u64 CarrierLength(
      const BlobRetainedLifetimeLeaseV3& lease) noexcept {
    return lease.carrier_logical_length_;
  }

  static BlobUuid16V3 RetainTicketUuid(
      const BlobRetainedLifetimeLeaseV3& lease) noexcept {
    return lease.retain_ticket_.retain_ticket_uuid;
  }

  static bool RetainTicketEquals(
      const BlobRetainedLifetimeLeaseV3& lease,
      const BlobUuid16V3& expected) noexcept {
    return std::memcmp(lease.retain_ticket_.retain_ticket_uuid.bytes,
                       expected.bytes, sizeof(expected.bytes)) == 0;
  }

  static BlobLifetimeRuntimeResultV3 Gate(
      BlobRetainedLifetimeLeaseV3& lease, BlobLifetimeGateV3 gate,
      BlobLifetimePublicPhaseV3 public_phase,
      BlobLifetimePhaseV3 invariant_phase) noexcept {
    return lease.OrdinaryGate(gate, public_phase, invariant_phase);
  }

  static BlobLifetimeRuntimeResultV3 BuildFailure(
      std::string_view diagnostic_code,
      std::string_view reason_selector) noexcept {
    return BlobRetainedLifetimeLeaseV3::BuildFailureForConformance(
        diagnostic_code, reason_selector);
  }

  static void MutateInvariantBinding(BlobRetainedLifetimeLeaseV3& lease,
                                     u8 mutation) noexcept {
    if (mutation == 1) lease.token_.authority_instance_uuid = {};
    if (mutation == 2) lease.token_.lifetime_token_uuid = {};
    if (mutation == 3) lease.token_.authority_instance_generation = 0;
    if (mutation == 4) lease.token_.lifetime_token_generation = 0;
    if (mutation == 5) lease.token_.immutable_binding_generation = 0;
  }

  static void SetMoveGuards(BlobRetainedLifetimeLeaseV3& lease,
                            bool state_guard, bool callback_guard) noexcept {
    lease.state_.store(
        state_guard ? BlobRetainedLifetimeLeaseV3::State::access_active
                    : BlobRetainedLifetimeLeaseV3::State::idle,
        std::memory_order_release);
    lease.callback_active_.store(callback_guard, std::memory_order_release);
  }

  static void SetState(BlobRetainedLifetimeLeaseV3& lease,
                       u8 state) noexcept {
    lease.state_.store(static_cast<BlobRetainedLifetimeLeaseV3::State>(state),
                       std::memory_order_release);
  }

  static u8 ControlState(const BlobRetainedLifetimeLeaseV3& lease) noexcept {
    return static_cast<u8>(lease.state_.load(std::memory_order_acquire));
  }

  static bool ControlClosing(
      const BlobRetainedLifetimeLeaseV3& lease) noexcept {
    return lease.state_.closing();
  }

  static u64 ControlDeferred(
      const BlobRetainedLifetimeLeaseV3& lease) noexcept {
    return lease.state_.Deferred();
  }

  static u64 ControlPeekWhileClosing(
      const BlobRetainedLifetimeLeaseV3& lease) noexcept {
    return lease.state_.PeekDeferredWhileClosing();
  }

  static bool ControlPublish(
      BlobRetainedLifetimeLeaseV3& lease,
      BlobLifetimeInvariantEventV3 event, u8 operation,
      BlobLifetimePhaseV3 phase, u8 callback) noexcept {
    return lease.PublishDeferredInvariant(event, operation, phase, callback);
  }

  static bool ControlBeginClosing(
      BlobRetainedLifetimeLeaseV3& lease) noexcept {
    return lease.state_.BeginClosing();
  }

  static bool ControlCompleteReceipt(
      BlobRetainedLifetimeLeaseV3& lease, u64 packed) noexcept {
    return lease.state_.CompleteDeferredServiceReceipt(packed);
  }

  static bool ControlTryFinalize(BlobRetainedLifetimeLeaseV3& lease,
                                 u8 final_state,
                                 u32 owner_entrants = 0) noexcept {
    return lease.state_.TryFinalize(
        static_cast<BlobRetainedLifetimeLeaseV3::State>(final_state),
        owner_entrants);
  }

  static bool ControlEnter(BlobRetainedLifetimeLeaseV3& lease) noexcept {
    return lease.state_.TryEnter();
  }

  static bool ControlEnterReusable(
      BlobRetainedLifetimeLeaseV3& lease) noexcept {
    return lease.state_.TryEnter(true);
  }

  static void ControlLeave(BlobRetainedLifetimeLeaseV3& lease) noexcept {
    lease.state_.Leave();
  }

  static u32 ControlEntrants(
      const BlobRetainedLifetimeLeaseV3& lease) noexcept {
    return lease.state_.entrants();
  }

  static auto ControlClaimTransfer(BlobRetainedLifetimeLeaseV3& lease) noexcept {
    return lease.state_.ClaimIdleForTransfer();
  }

  static auto ControlResetGeneration(BlobRetainedLifetimeLeaseV3& lease,
                                     u8 predecessor,
                                     u8 installed) noexcept {
    return lease.state_.ResetGeneration(
        static_cast<BlobRetainedLifetimeLeaseV3::State>(predecessor),
        static_cast<BlobRetainedLifetimeLeaseV3::State>(installed));
  }

  template <typename ResetOutcome>
  static bool ControlResetInstalled(const ResetOutcome& outcome) noexcept {
    return outcome.status ==
        BlobRetainedLifetimeLeaseV3::AtomicControl::
            GenerationResetStatus::installed;
  }

  template <typename ResetOutcome>
  static u8 ControlResetStatus(const ResetOutcome& outcome) noexcept {
    return static_cast<u8>(outcome.status);
  }

  template <typename ResetOutcome>
  static BlobLifetimeRuntimeResultV3 ControlResetFailure(
      BlobRetainedLifetimeLeaseV3& lease,
      const ResetOutcome& outcome,
      bool destination_context = true) noexcept {
    return lease.GenerationResetFailure(outcome, destination_context);
  }

  static void SetPublicationSuppressed(
      BlobRetainedLifetimeLeaseV3& lease, bool value) noexcept {
    lease.suppress_publication_gate_ = value;
  }

  static bool PublicationSuppressed(
      const BlobRetainedLifetimeLeaseV3& lease) noexcept {
    return lease.suppress_publication_gate_;
  }

  static bool Quarantined(
      const BlobRetainedLifetimeLeaseV3& lease) noexcept {
    return lease.quarantined_.load(std::memory_order_acquire);
  }

  static BlobLifetimeRuntimeResultV3 ReleaseFromExactRetainedPrestate(
      BlobRetainedLifetimeLeaseV3& lease) noexcept {
    BlobRetainedLifetimeLeaseV3::EntrantGuard entrant(lease.state_);
    if (!entrant.acquired()) std::terminate();
    return lease.ReleaseInternal(BlobLifetimePhaseV3::release,
                                 lease.request_.operation);
  }

  static BlobLifetimeRuntimeResultV3 EndFromExactAccessPrestate(
      BlobRetainedLifetimeLeaseV3& lease) noexcept {
    BlobRetainedLifetimeLeaseV3::EntrantGuard entrant(lease.state_);
    if (!entrant.acquired()) return BlobLifetimeRuntimeResultV3{};
    BlobLifetimeAccessTicketV3 access{};
    access.access_ticket_uuid = lease.retain_ticket_.retain_ticket_uuid;
    access.access_ticket_uuid.bytes[0] ^= 0xff;
    access.retain_ticket_uuid = lease.retain_ticket_.retain_ticket_uuid;
    access.lifetime_token_generation = lease.token_.lifetime_token_generation;
    access.immutable_binding_generation =
        lease.token_.immutable_binding_generation;
    lease.state_.store(BlobRetainedLifetimeLeaseV3::State::access_active,
                       std::memory_order_release);
    BlobLifetimeRuntimeResultV3 primary{};
    primary.disposition = BlobLifetimeOuterDispositionV3::success;
    return lease.EndAccessInternal(access, primary);
  }

};

class BlobLifetimeReceiverHostV3Generation1 final {
 public:
  static void PoisonReleasedCapability(
      BlobReceiverMaterializedCapabilityV3Generation1& capability) noexcept {
    capability.logical_length_ = 0;
  }
  static BlobLifetimeBudgetControlCapabilityV3Generation1 BudgetControl(
      BlobLifetimeBudgetLedgerV3Generation1& ledger,
      const BlobReceiverLifetimePinOpsV3Generation1& pin_ops) noexcept {
    return BlobLifetimeBudgetControlCapabilityV3Generation1(ledger, pin_ops);
  }

  static BlobReceiverMaterializedCapabilityV3Generation1 Capability(
      const BlobMaterializedCarrierBindingV3& binding, BlobValueStateV3 state,
      const byte* data, u64 length) noexcept {
    return BlobReceiverMaterializedCapabilityV3Generation1(
        binding, state, data, length);
  }
};

}  // namespace scratchbird::core::datatypes

namespace {

unsigned checks = 0;

[[noreturn]] void Fail(std::string_view text) {
  std::cerr << "FAIL: " << text << '\n';
  std::exit(EXIT_FAILURE);
}

void Check(bool condition, std::string_view text) {
  ++checks;
  if (!condition) Fail(text);
}

void FailureBuilderFailsClosed() {
  constexpr std::array<dt::BlobLifetimeReasonV3, 29> reason_domain{{
      dt::BlobLifetimeReasonV3::authority_table_missing,
      dt::BlobLifetimeReasonV3::authority_abi_mismatch,
      dt::BlobLifetimeReasonV3::authority_profile_binding_mismatch,
      dt::BlobLifetimeReasonV3::token_struct_invalid,
      dt::BlobLifetimeReasonV3::token_identity_invalid,
      dt::BlobLifetimeReasonV3::wrong_authority_instance,
      dt::BlobLifetimeReasonV3::owner_binding_mismatch,
      dt::BlobLifetimeReasonV3::transaction_mismatch,
      dt::BlobLifetimeReasonV3::access_ticket_invalid,
      dt::BlobLifetimeReasonV3::access_ticket_consumed,
      dt::BlobLifetimeReasonV3::callback_result_unknown,
      dt::BlobLifetimeReasonV3::callback_output_impossible,
      dt::BlobLifetimeReasonV3::callback_reentrant,
      dt::BlobLifetimeReasonV3::release_failed,
      dt::BlobLifetimeReasonV3::same_ticket_concurrent_use,
      dt::BlobLifetimeReasonV3::absolute_deadline_expired,
      dt::BlobLifetimeReasonV3::reader_io_failure,
      dt::BlobLifetimeReasonV3::sink_io_failure,
      dt::BlobLifetimeReasonV3::provider_io_failure,
      dt::BlobLifetimeReasonV3::adapter_exception_contained,
      dt::BlobLifetimeReasonV3::end_access_failed,
      dt::BlobLifetimeReasonV3::authority_runtime_unavailable,
      dt::BlobLifetimeReasonV3::monotonic_clock_unavailable,
      dt::BlobLifetimeReasonV3::receiver_services_failed,
      dt::BlobLifetimeReasonV3::shutdown_drain_violation,
      dt::BlobLifetimeReasonV3::authority_capacity_exhausted,
      dt::BlobLifetimeReasonV3::visitor_missing,
      dt::BlobLifetimeReasonV3::clone_alias,
      dt::BlobLifetimeReasonV3::destination_armed,
  }};
  Check(static_cast<std::uint8_t>(dt::BlobLifetimeReasonV3::none) == 0,
        "reason none is exact forbidden-zero value");
  for (std::size_t i = 0; i < reason_domain.size(); ++i) {
    Check(static_cast<std::uint8_t>(reason_domain[i]) == i + 1,
          "reason domain has exact Core numeric assignment");
  }

  const auto unknown =
      dt::BlobLifetimeRuntimeConformanceAccessV3::BuildFailure(
          "BLOB.UNKNOWN_DIAGNOSTIC", "callback_output_impossible");
  Check(unknown.disposition ==
            dt::BlobLifetimeOuterDispositionV3::terminated_out_of_band &&
            unknown.diagnostic_code.empty() && unknown.reason.empty() &&
            unknown.fact.diagnostic ==
                dt::BlobLifetimeDiagnosticCodeV3::none &&
            unknown.fact.reason == dt::BlobLifetimeReasonV3::none &&
            unknown.diagnostic_uuid.is_nil() &&
            unknown.diagnostic_generation == 0,
        "unknown diagnostic with known reason fails closed");
  const auto unknown_reason =
      dt::BlobLifetimeRuntimeConformanceAccessV3::BuildFailure(
          "CINL.LOB.DESCRIPTOR_INVALID", "not_in_closed_reason_domain");
  Check(unknown_reason.disposition ==
            dt::BlobLifetimeOuterDispositionV3::terminated_out_of_band &&
            unknown_reason.diagnostic_code.empty() &&
            unknown_reason.reason.empty() &&
            unknown_reason.fact.diagnostic ==
                dt::BlobLifetimeDiagnosticCodeV3::none &&
            unknown_reason.fact.reason == dt::BlobLifetimeReasonV3::none &&
            unknown_reason.diagnostic_uuid.is_nil() &&
            unknown_reason.diagnostic_generation == 0,
        "known diagnostic with unknown reason fails closed");

  const auto crossed =
      dt::BlobLifetimeRuntimeConformanceAccessV3::BuildFailure(
          "CINL.LOB.DESCRIPTOR_INVALID", "receiver_services_failed");
  Check(crossed.disposition ==
            dt::BlobLifetimeOuterDispositionV3::terminated_out_of_band &&
            crossed.diagnostic_code.empty() && crossed.reason.empty() &&
            crossed.fact.diagnostic ==
                dt::BlobLifetimeDiagnosticCodeV3::none &&
            crossed.fact.reason == dt::BlobLifetimeReasonV3::none &&
            crossed.diagnostic_uuid.is_nil() &&
            crossed.diagnostic_generation == 0,
        "known diagnostic with inadmissible reason fails closed");

  constexpr std::array<std::pair<std::string_view, std::string_view>, 13>
      inadmissible_pairs{{
          {"SECURITY.ACCESS_DENIED", "wrong_owner"},
          {"PROCESS.CANCELLED", "security_denied"},
          {"CINL.LOB.HANDLE_EXPIRED", "callback_output_impossible"},
          {"CINL.LOB.DESCRIPTOR_INVALID", "factory_admission_invalid"},
          {"BLOB.HANDLE_OWNER_MISMATCH", "transaction_mismatch"},
          {"BLOB.HANDLE_MODE_REFUSED", "wrong_owner"},
          {"BLOB.LIFETIME_AUTHORITY_UNAVAILABLE", "callback_reentrant"},
          {"CINL.LOB.STREAM_BACKPRESSURE_EXHAUSTED", "budget_shortage"},
          {"RESOURCE.BUDGET_EXCEEDED", "sampled_true"},
          {"BLOB.IO_FAILED", "reader_io_failure"},
          {"BLOB.INTEGRITY_FAILED", "protected_work_failed"},
          {"BLOB.STATE_INVALID", "token_struct_invalid"},
          {"BLOB.LENGTH_EXCEEDED", "absolute_deadline_expired"},
      }};
  for (const auto& [diagnostic, reason] : inadmissible_pairs) {
    const auto result =
        dt::BlobLifetimeRuntimeConformanceAccessV3::BuildFailure(
            diagnostic, reason);
    Check(result.disposition ==
              dt::BlobLifetimeOuterDispositionV3::terminated_out_of_band &&
              result.diagnostic_code.empty() && result.reason.empty() &&
              result.fact.diagnostic ==
                  dt::BlobLifetimeDiagnosticCodeV3::none &&
              result.fact.reason == dt::BlobLifetimeReasonV3::none,
          "diagnostic/reason cross-pair fails closed");
  }

  const auto no_reason_schema =
      dt::BlobLifetimeRuntimeConformanceAccessV3::BuildFailure(
          "CINL.LOB.HANDLE_EXPIRED", "closed");
  Check(no_reason_schema.disposition ==
            dt::BlobLifetimeOuterDispositionV3::public_failure &&
            no_reason_schema.reason.empty() &&
            no_reason_schema.fact.reason == dt::BlobLifetimeReasonV3::none,
        "private selector does not leak through no-reason schema");
  const auto public_reason_schema =
      dt::BlobLifetimeRuntimeConformanceAccessV3::BuildFailure(
          "CINL.LOB.DESCRIPTOR_INVALID", "callback_output_impossible");
  Check(public_reason_schema.disposition ==
            dt::BlobLifetimeOuterDispositionV3::public_failure &&
            public_reason_schema.reason == "callback_output_impossible" &&
            public_reason_schema.fact.reason ==
                dt::BlobLifetimeReasonV3::callback_output_impossible,
        "public reason schema exposes only canonical reason token");
}

BlobUuid16V3 BlobUuid(std::uint8_t seed) noexcept {
  BlobUuid16V3 result{};
  for (std::size_t index = 0; index < 16; ++index)
    result.bytes[index] = static_cast<std::uint8_t>(seed + index);
  return result;
}

platform::Uuid PlatformUuid(std::uint8_t seed) noexcept {
  platform::Uuid result{};
  for (std::size_t index = 0; index < 16; ++index)
    result.bytes[index] = static_cast<std::uint8_t>(seed + index);
  return result;
}

struct FakeReceiver {
  // Shared receiver fixtures serialize mutable callback bookkeeping. Never
  // retain this lock while waiting at a clock/visitor/invariant test barrier.
  std::recursive_mutex callback_mutex;
  BlobLifetimeUseRequestV3* poison_request_on_drop = nullptr;
  dt::BlobReceiverMaterializedCapabilityV3Generation1* poison_capability_on_drop = nullptr;
  std::array<char, 32> calls{};
  std::size_t call_count = 0;
  std::uint8_t next_ticket = 0x90;
  std::uint64_t now = 100;
  std::uint64_t pins = 1;
  std::uint64_t invariant_calls = 0;
  std::uint64_t clock_calls = 0;
  std::uint64_t cancellation_calls = 0;
  std::uint64_t pin_acquire_calls = 0;
  std::uint64_t pin_clone_calls = 0;
  std::uint64_t pin_drop_calls = 0;
  std::uint8_t retain_code = 0;
  std::uint8_t probe_code = 0;
  std::uint8_t begin_code = 0;
  std::uint8_t end_code = 0;
  std::uint8_t release_code = 0;
  bool malformed_retain_ticket = false;
  bool malformed_access_ticket = false;
  bool dirty_failure_output = false;
  bool clock_unavailable = false;
  bool cancellation_unavailable = false;
  bool clock_dirty_reserved = false;
  bool cancellation_dirty_reserved = false;
  bool cancelled = false;
  bool cancel_on_release = false;
  bool regress_clock = false;
  bool throw_retain = false;
  bool throw_retain_after_output = false;
  bool throw_probe = false;
  bool dirty_probe_ticket = false;
  bool throw_begin = false;
  bool throw_begin_after_output = false;
  bool throw_end = false;
  bool throw_release = false;
  dt::BlobLifetimeBudgetLedgerV3Generation1* corrupt_cancel_ledger = nullptr;
  std::uint8_t corrupt_cancel_phase = 0;
  dt::BlobReceiverPinAcquireStatusV3 pin_clone_status =
      dt::BlobReceiverPinAcquireStatusV3::acquired;
  dt::BlobReceiverPinAcquireStatusV3 pin_acquire_status =
      dt::BlobReceiverPinAcquireStatusV3::acquired;
  std::uint8_t pin_failure_call = 0;
  std::uint8_t pin_clone_failure_call = 0;
  bool pin_dirty_reserved = false;
  std::uint8_t pin_dirty_reserved_index = 0;
  bool pin_zero_cookie = false;
  std::uint8_t malformed_phase = 0;
  std::uint8_t malformed_stimulus = 0;
  BlobLifetimeRetainTicketV3 release_ticket_seen{};
  BlobLifetimeAccessTicketV3 end_ticket_seen{};
  dt::BlobLifetimeInvariantEventV3 last_event =
      dt::BlobLifetimeInvariantEventV3::receiver_services_failed;
  dt::BlobLifetimePhaseV3 last_invariant_phase =
      dt::BlobLifetimePhaseV3::factory_admission;
  std::uint8_t last_invariant_operation = 0;
  std::uint8_t last_invariant_callback = 255;
  std::atomic_flag invariant_record_guard = ATOMIC_FLAG_INIT;
  bool idempotent_invariant_service = false;
  std::uint64_t invariant_accepted_receipts = 0;
  std::uint64_t invariant_already_receipts = 0;
  std::uint64_t invariant_effects = 0;
  std::atomic<bool> block_invariant{false};
  std::atomic<bool> invariant_entered{false};
  std::atomic<bool> finish_invariant{false};
  std::uint64_t block_clock_call = 0;
  std::atomic<bool> clock_entered{false};
  std::atomic<bool> finish_clock{false};
  dt::BlobRetainedLifetimeLeaseV3* reentry_first = nullptr;
  dt::BlobRetainedLifetimeLeaseV3* reentry_second = nullptr;
  dt::BlobRetainedLifetimeLeaseV3* reentry_third = nullptr;
  const dt::BlobValidatedProfileHandleV3* reentry_profile = nullptr;
  dt::BlobLifetimeAuthorityAdmissionV3Generation1* reentry_admission =
      nullptr;
  const dt::BlobBoundMaterializedCarrierV3Generation1* reentry_carrier =
      nullptr;
  const BlobLifetimeTokenV3* reentry_token = nullptr;
  const BlobLifetimeUseRequestV3* reentry_request = nullptr;
  std::uint8_t reentry_action = 0;
  std::uint64_t reentry_attempts = 0;
  dt::BlobLifetimeRuntimeResultV3 reentry_result{};

  void Push(char code) noexcept {
    if (call_count < calls.size()) calls[call_count++] = code;
  }
};

void AttemptAdminReentry(FakeReceiver& fake) {
  const auto action = std::exchange(fake.reentry_action, std::uint8_t{0});
  if (action == 0) return;
  ++fake.reentry_attempts;
  if (action == 1) {
    auto moved = dt::BlobRetainedLifetimeLeaseV3::MoveConstructFrom(
        std::move(*fake.reentry_first));
    fake.reentry_result = moved.result;
  } else if (action == 2) {
    fake.reentry_result = fake.reentry_second->MoveReplaceFrom(
        std::move(*fake.reentry_first));
  } else if (action == 3) {
    fake.reentry_result = fake.reentry_first->MoveReplaceFrom(
        std::move(*fake.reentry_second));
  } else if (action == 4) {
    fake.reentry_result = fake.reentry_first->CloneInto(
        *fake.reentry_third);
  } else if (action == 5) {
    fake.reentry_result = fake.reentry_third->CloneInto(
        *fake.reentry_first);
  } else if (action == 6) {
    fake.reentry_result = dt::RetainBaseBlobLifetimeLeaseV3Generation1(
        *fake.reentry_profile, std::move(*fake.reentry_admission),
        *fake.reentry_carrier, *fake.reentry_token, *fake.reentry_request,
        *fake.reentry_third);
  } else if (action == 7) {
    fake.reentry_result = dt::ReadBlobMaterializedLengthV3Generation1(
        *fake.reentry_profile, *fake.reentry_carrier,
        std::move(*fake.reentry_admission), *fake.reentry_token,
        *fake.reentry_request, false).runtime;
  } else if (action == 8) {
    auto visitor = dt::BlobLifetimeRuntimeConformanceAccessV3::Visitor(nullptr, nullptr);
    fake.reentry_result = dt::ConsumeBlobMaterializedValueScopedV3Generation1(
        *fake.reentry_profile, *fake.reentry_carrier,
        std::move(*fake.reentry_admission), *fake.reentry_token,
        *fake.reentry_request, visitor, false);
  }
}

BlobLifetimeCallbackResultV3 CallbackFor(FakeReceiver& fake,
                                         std::uint8_t phase,
                                         std::uint8_t ordinary_code) noexcept {
  BlobLifetimeCallbackResultV3 result{};
  if (fake.malformed_phase != phase) {
    result.code = ordinary_code;
    return result;
  }
  if (fake.malformed_stimulus == 1) result.code = 19;
  else if (fake.malformed_stimulus == 2) result.code = 255;
  else if (fake.malformed_stimulus >= 3 &&
           fake.malformed_stimulus <= 9)
    result.reserved_1_7[fake.malformed_stimulus - 3] = 1;
  else if (fake.malformed_stimulus == 11) result.code = 18;
  return result;
}

bool MalformedTicket(const FakeReceiver& fake, std::uint8_t phase) noexcept {
  return fake.malformed_phase == phase &&
         (fake.malformed_stimulus == 10 ||
          fake.malformed_stimulus == 11);
}

BlobLifetimeCallbackResultV3 SCRATCHBIRD_ENGINE_CALL Retain(
    void* context, const BlobLifetimeTokenV3* token,
    const BlobLifetimeUseRequestV3*, BlobLifetimeRetainTicketV3* out) {
  auto& fake = *static_cast<FakeReceiver*>(context);
  std::lock_guard lock(fake.callback_mutex);
  fake.Push('R');
  AttemptAdminReentry(fake);
  if (fake.throw_retain) throw 1;
  if (fake.corrupt_cancel_ledger != nullptr &&
      fake.corrupt_cancel_phase == 1)
    dt::BlobLifetimeRuntimeConformanceAccessV3::SetCounter(
        *fake.corrupt_cancel_ledger,
        dt::BlobLifetimeResourceV3::lifetime_callback_calls, 0, 1, 1);
  const auto result = CallbackFor(fake, 1, fake.retain_code);
  if (result.code == 0 || fake.dirty_failure_output ||
      (fake.malformed_phase == 1 && fake.malformed_stimulus == 11)) {
    out->retain_ticket_uuid = BlobUuid(fake.next_ticket++);
    out->lifetime_token_uuid = token->lifetime_token_uuid;
    out->lifetime_token_generation = token->lifetime_token_generation;
    out->immutable_binding_generation = token->immutable_binding_generation;
    if (fake.malformed_retain_ticket) out->retain_ticket_uuid = {};
    if (MalformedTicket(fake, 1))
      std::memset(out, fake.malformed_stimulus == 10 ? 0xaa : 0xdd,
                  sizeof(*out));
  }
  if (fake.throw_retain_after_output) throw 1;
  return result;
}

BlobLifetimeCallbackResultV3 SCRATCHBIRD_ENGINE_CALL Probe(
    void* context, const BlobLifetimeTokenV3*,
    const BlobLifetimeRetainTicketV3* retain,
    const BlobLifetimeUseRequestV3*) {
  auto& fake = *static_cast<FakeReceiver*>(context);
  std::lock_guard lock(fake.callback_mutex);
  fake.Push('P');
  AttemptAdminReentry(fake);
  if (fake.throw_probe) throw 1;
  if (fake.dirty_probe_ticket || MalformedTicket(fake, 2))
    std::memset(const_cast<BlobLifetimeRetainTicketV3*>(retain),
                0xcc, sizeof(*retain));
  return CallbackFor(fake, 2, fake.probe_code);
}

BlobLifetimeCallbackResultV3 SCRATCHBIRD_ENGINE_CALL Begin(
    void* context, const BlobLifetimeTokenV3* token,
    const BlobLifetimeRetainTicketV3* retain,
    const BlobLifetimeUseRequestV3*, BlobLifetimeAccessTicketV3* out) {
  auto& fake = *static_cast<FakeReceiver*>(context);
  std::lock_guard lock(fake.callback_mutex);
  fake.Push('B');
  AttemptAdminReentry(fake);
  if (fake.throw_begin) throw 1;
  if (fake.corrupt_cancel_ledger != nullptr &&
      fake.corrupt_cancel_phase == 3)
    dt::BlobLifetimeRuntimeConformanceAccessV3::SetCounter(
        *fake.corrupt_cancel_ledger,
        dt::BlobLifetimeResourceV3::lifetime_callback_calls, 1, 2, 2);
  const auto result = CallbackFor(fake, 3, fake.begin_code);
  if (result.code == 0 || fake.dirty_failure_output ||
      (fake.malformed_phase == 3 && fake.malformed_stimulus == 11)) {
    out->access_ticket_uuid = BlobUuid(fake.next_ticket++);
    out->retain_ticket_uuid = retain->retain_ticket_uuid;
    out->lifetime_token_generation = token->lifetime_token_generation;
    out->immutable_binding_generation = token->immutable_binding_generation;
    if (fake.malformed_access_ticket) out->access_ticket_uuid = {};
    if (MalformedTicket(fake, 3))
      std::memset(out, fake.malformed_stimulus == 10 ? 0xbb : 0xdd,
                  sizeof(*out));
  }
  if (fake.throw_begin_after_output) throw 1;
  return result;
}

BlobLifetimeCallbackResultV3 SCRATCHBIRD_ENGINE_CALL End(
    void* context, const BlobLifetimeTokenV3*,
    const BlobLifetimeRetainTicketV3*, BlobLifetimeAccessTicketV3* inout,
    const BlobLifetimeUseRequestV3*) {
  auto& fake = *static_cast<FakeReceiver*>(context);
  std::lock_guard lock(fake.callback_mutex);
  fake.Push('E');
  fake.end_ticket_seen = *inout;
  if (fake.throw_end) throw 1;
  if (MalformedTicket(fake, 4))
    std::memset(inout, 0xcc, sizeof(*inout));
  else
    *inout = {};
  return CallbackFor(fake, 4, fake.end_code);
}

BlobLifetimeCallbackResultV3 SCRATCHBIRD_ENGINE_CALL Release(
    void* context, const BlobLifetimeTokenV3*,
    BlobLifetimeRetainTicketV3* inout) {
  auto& fake = *static_cast<FakeReceiver*>(context);
  std::lock_guard lock(fake.callback_mutex);
  fake.Push('L');
  fake.release_ticket_seen = *inout;
  if (fake.cancel_on_release) fake.cancelled = true;
  if (fake.throw_release) throw 1;
  if (MalformedTicket(fake, 5))
    std::memset(inout, 0xcc, sizeof(*inout));
  else
    *inout = {};
  return CallbackFor(fake, 5, fake.release_code);
}

dt::BlobMonotonicReadResultV3 ReadClock(void* context) noexcept {
  auto& fake = *static_cast<FakeReceiver*>(context);
  bool block = false;
  {
    std::lock_guard lock(fake.callback_mutex);
    block = fake.block_clock_call == ++fake.clock_calls;
  }
  if (block) {
    fake.clock_entered.store(true, std::memory_order_release);
    while (!fake.finish_clock.load(std::memory_order_acquire))
      std::this_thread::yield();
  }
  std::lock_guard lock(fake.callback_mutex);
  if (fake.clock_unavailable)
    return {dt::BlobReceiverSampleStatusV3::unavailable, {}, 0};
  if (fake.regress_clock && fake.clock_calls > 1) fake.now -= 2;
  dt::BlobMonotonicReadResultV3 result{
      dt::BlobReceiverSampleStatusV3::ok, {}, fake.now++};
  if (fake.clock_dirty_reserved) result.reserved[0] = 1;
  return result;
}

dt::BlobCancellationSampleResultV3 SampleCancellation(
    void* context, std::uint8_t, dt::BlobLifetimePublicPhaseV3) noexcept {
  auto& fake = *static_cast<FakeReceiver*>(context);
  std::lock_guard lock(fake.callback_mutex);
  ++fake.cancellation_calls;
  if (fake.cancellation_unavailable)
    return {dt::BlobReceiverSampleStatusV3::unavailable, 0, {}};
  dt::BlobCancellationSampleResultV3 result{
      dt::BlobReceiverSampleStatusV3::ok,
      static_cast<std::uint8_t>(fake.cancelled), {}};
  if (fake.cancellation_dirty_reserved) result.reserved[0] = 1;
  return result;
}

dt::BlobReceiverEffectResultV3 RecordInvariant(
    void* context, const dt::BlobLifetimeBindingKeyV3&,
    const dt::BlobLifetimeInvariantFactV3& fact) {
  auto& fake = *static_cast<FakeReceiver*>(context);
  while (fake.invariant_record_guard.test_and_set(std::memory_order_acquire))
    std::this_thread::yield();
  ++fake.invariant_calls;
  fake.last_event = fact.event;
  fake.last_invariant_phase = fact.phase;
  fake.last_invariant_operation = fact.operation_enum;
  fake.last_invariant_callback = fact.callback_code_or_255;
  const auto status = fake.idempotent_invariant_service &&
          fake.invariant_calls != 1
      ? dt::BlobReceiverEffectStatusV3::already_applied
      : dt::BlobReceiverEffectStatusV3::accepted;
  if (status == dt::BlobReceiverEffectStatusV3::accepted) {
    ++fake.invariant_accepted_receipts;
    ++fake.invariant_effects;
  } else {
    ++fake.invariant_already_receipts;
  }
  fake.invariant_record_guard.clear(std::memory_order_release);
  if (fake.block_invariant.load(std::memory_order_acquire)) {
    fake.invariant_entered.store(true, std::memory_order_release);
    while (!fake.finish_invariant.load(std::memory_order_acquire))
      std::this_thread::yield();
  }
  return {status, {}};
}

dt::BlobReceiverPinCloneResultV3 ClonePin(void* context,
                                         std::uint64_t) noexcept {
  auto& fake = *static_cast<FakeReceiver*>(context);
  std::lock_guard lock(fake.callback_mutex);
  if (++fake.pin_clone_calls == fake.pin_clone_failure_call) {
    dt::BlobReceiverPinCloneResultV3 result{};
    result.status = fake.pin_acquire_status;
    result.pin_cookie = fake.pin_zero_cookie ? 0 : 1000 + fake.pin_clone_calls;
    if (fake.pin_dirty_reserved)
      result.reserved[fake.pin_dirty_reserved_index] = 1;
    if (result.status == dt::BlobReceiverPinAcquireStatusV3::acquired &&
        result.pin_cookie != 0 && !fake.pin_dirty_reserved) ++fake.pins;
    return result;
  }
  if (fake.pin_clone_status !=
      dt::BlobReceiverPinAcquireStatusV3::acquired)
    return {fake.pin_clone_status, {}, 0};
  ++fake.pins;
  return {dt::BlobReceiverPinAcquireStatusV3::acquired, {},
          static_cast<std::uint64_t>(1000 + fake.pins)};
}

dt::BlobReceiverPinCloneResultV3 AcquirePin(void* context) noexcept {
  auto& fake = *static_cast<FakeReceiver*>(context);
  std::lock_guard lock(fake.callback_mutex);
  ++fake.pin_acquire_calls;
  if (fake.pin_failure_call == fake.pin_acquire_calls) {
    dt::BlobReceiverPinCloneResultV3 result{};
    result.status = fake.pin_acquire_status;
    result.pin_cookie = fake.pin_zero_cookie ? 0 : 700 + fake.pin_acquire_calls;
    if (fake.pin_dirty_reserved)
      result.reserved[fake.pin_dirty_reserved_index] = 1;
    if (result.status == dt::BlobReceiverPinAcquireStatusV3::acquired &&
        result.pin_cookie != 0 && !fake.pin_dirty_reserved) ++fake.pins;
    return result;
  }
  ++fake.pins;
  return {dt::BlobReceiverPinAcquireStatusV3::acquired, {},
          static_cast<std::uint64_t>(700 + fake.pin_acquire_calls)};
}

void DropPin(void* context, std::uint64_t) noexcept {
  auto& fake = *static_cast<FakeReceiver*>(context);
  std::lock_guard lock(fake.callback_mutex);
  ++fake.pin_drop_calls;
  if (fake.pins != 0) --fake.pins;
  if (fake.pin_drop_calls == 2) {
    if (fake.poison_request_on_drop != nullptr) {
      fake.poison_request_on_drop->operation = 0;
      fake.poison_request_on_drop->required_owner_class = 0;
    }
    if (fake.poison_capability_on_drop != nullptr)
      dt::BlobLifetimeReceiverHostV3Generation1::PoisonReleasedCapability(
          *fake.poison_capability_on_drop);
  }
}

struct Fixture {
  FakeReceiver fake;
  dt::BlobLifetimeBudgetLedgerV3Generation1 ledger;
  BlobLifetimeTokenV3 token{};
  BlobLifetimeUseRequestV3 request{};
  BlobLifetimeAuthorityV3 authority{};
  dt::BaseBlobLifetimeReceiverServicesV3Generation1 services{};
  dt::BlobReceiverLifetimePinOpsV3Generation1 pins{};
  dt::BlobMaterializedCarrierBindingV3 carrier_binding{};
  dt::BlobValidatedProfileHandleV3 profile{};

  explicit Fixture(std::uint64_t callback_limit) {
    profile = dt::BuildCurrentBlobValidatedProfileHandleV3(
                  dt::kBlobV11ReceiptUuid).profile;
    const auto authority_uuid = BlobUuid(0x10);
    const auto lifetime_uuid = BlobUuid(0x20);
    token.struct_bytes = sizeof(token);
    token.abi_major = SB_BLOB_LIFETIME_ABI_MAJOR_V3;
    token.abi_minor = SB_BLOB_LIFETIME_ABI_MINOR_V3;
    token.lifetime_token_uuid = lifetime_uuid;
    token.lifetime_token_generation = 7;
    token.owner_class = SB_BLOB_OWNER_DATATYPE_OWNED_VALUE_V3;
    token.authority_instance_uuid = authority_uuid;
    token.authority_instance_generation = 3;
    token.database_uuid = BlobUuid(0x30);
    token.transaction_uuid = BlobUuid(0x40);
    token.snapshot_uuid = BlobUuid(0x50);
    token.snapshot_generation = 7;
    token.security_context_uuid = BlobUuid(0x60);
    token.security_generation = 4;
    token.immutable_binding_generation = 9;
    token.mode_bits = SB_BLOB_MODE_IMMUTABLE_READ_V3 |
                      SB_BLOB_MODE_MATERIALIZED_BYTES_V3;

    request.struct_bytes = sizeof(request);
    request.abi_major = SB_BLOB_LIFETIME_ABI_MAJOR_V3;
    request.abi_minor = SB_BLOB_LIFETIME_ABI_MINOR_V3;
    request.operation = SB_BLOB_OP_READ_V3;
    request.required_owner_class = token.owner_class;
    request.required_mode_bits = token.mode_bits;
    request.expected_authority_instance_uuid = authority_uuid;
    request.expected_authority_instance_generation = 3;
    request.expected_database_uuid = token.database_uuid;
    request.expected_session_uuid = token.session_uuid;
    request.expected_statement_uuid = token.statement_uuid;
    request.expected_transaction_uuid = token.transaction_uuid;
    request.expected_snapshot_uuid = token.snapshot_uuid;
    request.expected_snapshot_generation = 7;
    request.expected_security_context_uuid = token.security_context_uuid;
    request.expected_security_generation = 4;

    authority.struct_bytes = sizeof(authority);
    authority.abi_major = SB_BLOB_LIFETIME_ABI_MAJOR_V3;
    authority.abi_minor = SB_BLOB_LIFETIME_ABI_MINOR_V3;
    authority.authority_instance_uuid = authority_uuid;
    authority.authority_instance_generation = 3;
    authority.authority_context = &fake;
    authority.retain = Retain;
    authority.probe = Probe;
    authority.begin_access = Begin;
    authority.end_access = End;
    authority.release = Release;

    services.receiver_services_uuid = PlatformUuid(0x70);
    services.receiver_services_generation = 1;
    services.monotonic_clock = {PlatformUuid(0x80), 1};
    services.context = &fake;
    services.read_monotonic_ns = ReadClock;
    services.sample_cancellation = SampleCancellation;
    services.record_invariant_and_quarantine = RecordInvariant;
    pins = {&fake, AcquirePin, ClonePin, DropPin};
    carrier_binding.authority_instance_uuid = PlatformUuid(0x10);
    carrier_binding.authority_instance_generation = 3;
    carrier_binding.carrier_uuid = PlatformUuid(0xa0);
    carrier_binding.carrier_generation = 1;
    carrier_binding.lifetime_token_uuid = PlatformUuid(0x20);
    carrier_binding.lifetime_token_generation = 7;
    carrier_binding.immutable_binding_generation = 9;
    dt::BlobLifetimeRuntimeConformanceAccessV3::SetCallbackLimit(
        ledger, callback_limit);
  }

  dt::BlobLifetimeAuthorityAdmissionV3Generation1 Admission() {
    fake.pins += 2;
    return dt::BlobLifetimeRuntimeConformanceAccessV3::Admission(
        authority, services, pins, ledger,
        static_cast<std::uint64_t>(100 + fake.pins));
  }
};

struct VisitContext {
  std::array<platform::byte, 8> copied{};
  std::size_t copied_size = 0;
  dt::BlobRetainedLifetimeLeaseV3* active_lease = nullptr;
  bool attempt_move = false;
  bool move_refused = false;
  bool attempt_self_replace = false;
  bool self_replace_ok = false;
};

[[maybe_unused]] dt::BlobProtectedWorkResultV3 CopyVisitor(
    void* context, std::span<const platform::byte> bytes) noexcept {
  auto& visit = *static_cast<VisitContext*>(context);
  visit.copied_size = bytes.size();
  std::copy(bytes.begin(), bytes.end(), visit.copied.begin());
  if (visit.attempt_move) {
    const auto moved = dt::BlobRetainedLifetimeLeaseV3::MoveConstructFrom(
        std::move(*visit.active_lease));
    visit.move_refused = !moved.result.ok() && moved.lease() == nullptr &&
        moved.result.reason == "same_ticket_concurrent_use";
  }
  if (visit.attempt_self_replace) {
    const auto result = visit.active_lease->MoveReplaceFrom(
        std::move(*visit.active_lease));
    visit.self_replace_ok = result.ok();
  }
  return {0, {}, {}};
}

dt::BlobProtectedWorkResultV3 ResourceVisitor(
    void*, std::span<const platform::byte>) noexcept {
  dt::BlobLifetimeTypedFactV3 fact{};
  fact.fact_class = dt::BlobLifetimeFactClassV3::resource_budget;
  fact.diagnostic = dt::BlobLifetimeDiagnosticCodeV3::resource_budget_exceeded;
  fact.reason = dt::BlobLifetimeReasonV3::none;
  fact.stage = dt::BlobLifetimeFactStageV3::callback_return;
  fact.diagnostic_uuid = platform::Uuid{{
      0xde,0xdc,0x1c,0x9b,0x38,0x79,0x57,0x70,
      0x8f,0x8e,0xc0,0xc9,0x27,0xa0,0xc3,0x4d}};
  fact.diagnostic_generation = 1;
  fact.parameters.present = dt::blob_parameter_resource |
      dt::blob_parameter_required | dt::blob_parameter_available;
  fact.parameters.resource_enum =
      dt::BlobLifetimeResourceV3::logical_bytes_read;
  fact.parameters.required_u64 = 2;
  fact.parameters.available_u64 = 1;
  return {1, {}, fact};
}

struct TypedFailureVisitorContext {
  dt::BlobLifetimeTypedFactV3 fact{};
};

dt::BlobProtectedWorkResultV3 TypedFailureVisitor(
    void* context, std::span<const platform::byte>) noexcept {
  return {1, {}, static_cast<TypedFailureVisitorContext*>(context)->fact};
}

struct BlockingVisitContext {
  std::atomic<bool> entered{false};
  std::atomic<bool> finish{false};
};

dt::BlobProtectedWorkResultV3 BlockingVisitor(
    void* context, std::span<const platform::byte>) noexcept {
  auto& block = *static_cast<BlockingVisitContext*>(context);
  block.entered.store(true, std::memory_order_release);
  while (!block.finish.load(std::memory_order_acquire))
    std::this_thread::yield();
  return {0, {}, {}};
}

dt::BlobProtectedWorkResultV3 ProtocolViolationVisitor(
    void* context, std::span<const platform::byte>) noexcept {
  const auto stimulus = *static_cast<const std::uint8_t*>(context);
  dt::BlobProtectedWorkResultV3 result{};
  if (stimulus == 1) result.status = 2;
  if (stimulus == 2) result.reserved[0] = 1;
  // Stimulus 3 keeps status=1 and the default, out-of-domain typed fact.
  return result;
}

dt::BlobProtectedWorkResultV3 CorruptLedgerResourceVisitor(
    void* context, std::span<const platform::byte> bytes) noexcept {
  auto* ledger =
      static_cast<dt::BlobLifetimeBudgetLedgerV3Generation1*>(context);
  dt::BlobLifetimeRuntimeConformanceAccessV3::SetCounter(
      *ledger, dt::BlobLifetimeResourceV3::lifetime_callback_calls,
      0, 2, 2);
  return ResourceVisitor(nullptr, bytes);
}

#include "base_blob_lifetime_v7_generated_vectors.inc"

void ProductionPrecedence() {
  for (std::uint8_t expected = 1; expected <= 10; ++expected) {
    dt::BlobLifetimeFactAccumulatorV3 facts;
    for (std::uint8_t rank = expected; rank <= 10; ++rank) {
      dt::BlobLifetimeTypedFactV3 fact{};
      fact.fact_class = static_cast<dt::BlobLifetimeFactClassV3>(rank);
      fact.diagnostic = static_cast<dt::BlobLifetimeDiagnosticCodeV3>(rank);
      facts.Observe(fact);
    }
    Check(static_cast<std::uint8_t>(facts.Selected().fact_class) == expected,
          "production precedence suffix selected wrong rank");
    for (std::uint8_t lower = expected + 1; lower <= 10; ++lower) {
      dt::BlobLifetimeFactAccumulatorV3 pair;
      dt::BlobLifetimeTypedFactV3 low{};
      low.fact_class = static_cast<dt::BlobLifetimeFactClassV3>(lower);
      dt::BlobLifetimeTypedFactV3 high{};
      high.fact_class = static_cast<dt::BlobLifetimeFactClassV3>(expected);
      pair.Observe(low);
      pair.Observe(high);
      Check(static_cast<std::uint8_t>(pair.Selected().fact_class) == expected,
            "production precedence pair selected wrong rank");
    }
  }
}

void ExactInvariantDomain() {
  const auto make_key = [] {
    return dt::BlobLifetimeBindingKeyV3{
        PlatformUuid(0x10), 7, PlatformUuid(0x20), 11, 13};
  };
  for (const auto& row : kAllowedInvariantRows) {
    const auto key = make_key();
    dt::BlobLifetimeInvariantFactV3 fact{
        static_cast<dt::BlobLifetimeInvariantEventV3>(row.event),
        row.operation, static_cast<dt::BlobLifetimePhaseV3>(row.phase),
        row.callback_code, 7, 11, 13};
    Check(dt::ValidateBlobLifetimeInvariantFactDomainV3(key, fact),
          "authority-allowed invariant row was rejected");
    for (std::uint8_t code = 0; code != 19; ++code) {
      fact.callback_code_or_255 = code;
      Check(dt::ValidateBlobLifetimeInvariantFactDomainV3(key, fact),
            "allowed invariant callback numeric was rejected");
    }
  }
  for (const auto& row : kIllegalInvariantRows) {
    auto key = make_key();
    dt::BlobLifetimeInvariantFactV3 fact{
        static_cast<dt::BlobLifetimeInvariantEventV3>(row.event),
        row.operation, static_cast<dt::BlobLifetimePhaseV3>(row.phase),
        row.callback_code, 7, 11, 13};
    if (row.mutation == 1) key.authority_instance_uuid = {};
    if (row.mutation == 2) key.lifetime_token_uuid = {};
    if (row.mutation == 3) key.authority_instance_generation = 0;
    if (row.mutation == 4) key.lifetime_token_generation = 0;
    if (row.mutation == 5) key.immutable_binding_generation = 0;
    fact.authority_instance_generation = key.authority_instance_generation;
    fact.lifetime_token_generation = key.lifetime_token_generation;
    fact.immutable_binding_generation = key.immutable_binding_generation;
    Check(!dt::ValidateBlobLifetimeInvariantFactDomainV3(key, fact),
          "authority-illegal invariant row was accepted");
  }
}

void FactoryF02AndRepresentability() {
  constexpr std::array<dt::BlobLifetimeAuthoritySourceV3, 12> sources{{
      dt::BlobLifetimeAuthoritySourceV3::authority_table_missing,
      dt::BlobLifetimeAuthoritySourceV3::retain_callback_missing,
      dt::BlobLifetimeAuthoritySourceV3::probe_callback_missing,
      dt::BlobLifetimeAuthoritySourceV3::begin_access_callback_missing,
      dt::BlobLifetimeAuthoritySourceV3::end_access_callback_missing,
      dt::BlobLifetimeAuthoritySourceV3::release_callback_missing,
      dt::BlobLifetimeAuthoritySourceV3::receiver_services_object_missing,
      dt::BlobLifetimeAuthoritySourceV3::read_monotonic_ns_callable_missing,
      dt::BlobLifetimeAuthoritySourceV3::sample_cancellation_callable_missing,
      dt::BlobLifetimeAuthoritySourceV3::
          record_invariant_and_quarantine_callable_missing,
      dt::BlobLifetimeAuthoritySourceV3::pin_host_failure,
      dt::BlobLifetimeAuthoritySourceV3::admission_closed_or_drain,
  }};
  for (std::size_t index = 0; index < sources.size(); ++index) {
    Fixture fixture(16);
    auto authority = fixture.authority;
    auto services = fixture.services;
    auto pins = fixture.pins;
    const BlobLifetimeAuthorityV3* authority_pointer = &authority;
    const dt::BaseBlobLifetimeReceiverServicesV3Generation1*
        services_pointer = &services;
    if (index == 0) authority_pointer = nullptr;
    if (index == 1) authority.retain = nullptr;
    if (index == 2) authority.probe = nullptr;
    if (index == 3) authority.begin_access = nullptr;
    if (index == 4) authority.end_access = nullptr;
    if (index == 5) authority.release = nullptr;
    if (index == 6) services_pointer = nullptr;
    if (index == 7) services.read_monotonic_ns = nullptr;
    if (index == 8) services.sample_cancellation = nullptr;
    if (index == 9) services.record_invariant_and_quarantine = nullptr;
    if (index == 10) pins.acquire_pin = nullptr;
    if (index == 11) {
      fixture.fake.pin_failure_call = 1;
      fixture.fake.pin_acquire_status =
          dt::BlobReceiverPinAcquireStatusV3::draining;
      fixture.fake.pin_zero_cookie = true;
    }
    auto budget =
        dt::BlobLifetimeReceiverHostV3Generation1::BudgetControl(
            fixture.ledger, fixture.pins);
    auto capability =
        dt::BlobLifetimeReceiverHostV3Generation1::Capability(
            fixture.carrier_binding, dt::BlobValueStateV3::value, nullptr, 0);
    dt::BlobLifetimeAuthorityAdmissionV3Generation1 admission;
    dt::BlobBoundMaterializedCarrierV3Generation1 carrier;
    const auto result = dt::BlobLifetimeRuntimeFactoryV3Generation1::
        AdmitMaterialized(
            SB_BLOB_OP_READ_V3,
            fixture.profile, authority_pointer, services_pointer, pins, budget,
            fixture.services.monotonic_clock,
            fixture.services.monotonic_clock, fixture.token, fixture.request,
            capability, admission, carrier);
    Check(!result.ok(), "F02 factory source unexpectedly admitted");
    Check(result.fact.fact_class ==
              dt::BlobLifetimeFactClassV3::lifetime_authority_unavailable &&
              result.fact.stage ==
                  dt::BlobLifetimeFactStageV3::factory_admission &&
              !result.fact.gate_present &&
              result.fact.authority_source == sources[index],
          "F02 factory source/stage/gate classification mismatch");
    Check((result.fact.parameters.present & dt::blob_parameter_phase) != 0 &&
              result.fact.parameters.phase_enum ==
                  dt::BlobLifetimePublicPhaseV3::retain,
          "F02 factory phase parameter mismatch");
  }

  {
    Fixture fixture(16);
    const platform::byte dummy = 0;
    auto budget =
        dt::BlobLifetimeReceiverHostV3Generation1::BudgetControl(
            fixture.ledger, fixture.pins);
    auto capability =
        dt::BlobLifetimeReceiverHostV3Generation1::Capability(
            fixture.carrier_binding, dt::BlobValueStateV3::value, &dummy,
            dt::kBlobMaximumLogicalBytesV3 + 1);
    dt::BlobLifetimeAuthorityAdmissionV3Generation1 admission;
    dt::BlobBoundMaterializedCarrierV3Generation1 carrier;
    const auto result = dt::BlobLifetimeRuntimeFactoryV3Generation1::
        AdmitMaterialized(
            SB_BLOB_OP_READ_V3,
            fixture.profile, &fixture.authority, &fixture.services,
            fixture.pins, budget, fixture.services.monotonic_clock,
            fixture.services.monotonic_clock, fixture.token, fixture.request,
            capability, admission, carrier);
    Check(!result.ok() && result.diagnostic_code == "BLOB.LENGTH_EXCEEDED" &&
              result.reason.empty(),
          "nonrepresentable materialized length was not rejected");
    Check((result.fact.parameters.present &
           (dt::blob_parameter_actual_length |
            dt::blob_parameter_maximum_length |
            dt::blob_parameter_operation)) ==
              (dt::blob_parameter_actual_length |
               dt::blob_parameter_maximum_length |
               dt::blob_parameter_operation) &&
              result.fact.parameters.actual_length_u64 ==
                  dt::kBlobMaximumLogicalBytesV3 + 1 &&
              result.fact.parameters.maximum_length_u64 ==
                  dt::kBlobMaximumLogicalBytesV3,
          "nonrepresentable length parameters are not exact");
    Check(fixture.fake.pin_acquire_calls == 2 &&
              fixture.fake.pin_drop_calls == 2,
          "representability refusal leaked a factory pin");
  }
}

void FactoryAdmissionPrecedence() {
  // Independent first-failure oracle from the handoff's arbiter order. Every
  // pair, not only adjacent faults, must select the earlier admission gate.
  struct Expected {
    std::string_view code;
    std::string_view reason;
  };
  constexpr std::array<Expected, 16> expected{{
      {"CINL.LOB.DESCRIPTOR_INVALID", "authority_profile_binding_mismatch"},
      {"CINL.LOB.DESCRIPTOR_INVALID", "authority_abi_mismatch"},
      {"CINL.LOB.DESCRIPTOR_INVALID", "authority_abi_mismatch"},
      {"CINL.LOB.DESCRIPTOR_INVALID", "token_struct_invalid"},
      {"CINL.LOB.DESCRIPTOR_INVALID", "token_identity_invalid"},
      {"CINL.LOB.DESCRIPTOR_INVALID", "wrong_authority_instance"},
      {"BLOB.HANDLE_OWNER_MISMATCH", "owner_binding_mismatch"},
      {"CINL.LOB.DESCRIPTOR_INVALID", "transaction_mismatch"},
      {"BLOB.HANDLE_MODE_REFUSED", ""},
      {"BLOB.STATE_INVALID", ""},
      {"CINL.LOB.DESCRIPTOR_INVALID", "token_struct_invalid"},
      {"BLOB.STATE_INVALID", ""},
      {"BLOB.LENGTH_EXCEEDED", ""},
      {"BLOB.LIFETIME_AUTHORITY_UNAVAILABLE", "authority_table_missing"},
      {"BLOB.LIFETIME_AUTHORITY_UNAVAILABLE", "receiver_services_failed"},
      {"CINL.LOB.DESCRIPTOR_INVALID", "authority_profile_binding_mismatch"},
  }};
  for (std::size_t first = 0; first < expected.size(); ++first) {
    for (std::size_t second = first; second < expected.size(); ++second) {
      Fixture fixture(16);
      const auto canonical_descriptor = fixture.profile.identity;
      auto profile = fixture.profile;
      auto authority = fixture.authority;
      auto services = fixture.services;
      auto token = fixture.token;
      auto request = fixture.request;
      dt::BlobLifetimeBudgetLedgerV3Generation1 unfrozen;
      const auto fault = [&](std::size_t gate) noexcept {
        return first == gate || second == gate;
      };
      if (fault(0)) {
        profile.identity.descriptor_uuid = PlatformUuid(0xe0);
        profile.identity.descriptor_generation = 0xdeadbeef;
      }
      if (fault(1)) authority.struct_bytes = 0;
      if (fault(3)) token.reserved_33_39[0] = 1;
      if (fault(4)) request.expected_database_uuid = BlobUuid(0xb0);
      if (fault(5)) authority.authority_instance_uuid = BlobUuid(0xc0);
      if (fault(6)) request.required_owner_class =
          SB_BLOB_OWNER_ENCODED_ARTIFACT_BORROW_V3;
      if (fault(7)) request.expected_transaction_uuid = BlobUuid(0xd0);
      if (fault(8)) request.operation = SB_BLOB_OP_RESTORE_V3;
      if (fault(13)) authority.retain = nullptr;
      if (fault(14)) services.read_monotonic_ns = nullptr;
      if (fault(15)) ++services.monotonic_clock.clock_generation;
      auto budget = dt::BlobLifetimeReceiverHostV3Generation1::BudgetControl(
          fault(2) ? unfrozen : fixture.ledger, fixture.pins);
      const platform::byte dummy = 0;
      const auto state = fault(10)
          ? static_cast<dt::BlobValueStateV3>(255)
          : (fault(11) ? dt::BlobValueStateV3::sql_null
                       : dt::BlobValueStateV3::value);
      auto capability = dt::BlobLifetimeReceiverHostV3Generation1::Capability(
          fixture.carrier_binding, state, &dummy,
          fault(12) ? dt::kBlobMaximumLogicalBytesV3 + 1 : 1);
      dt::BlobLifetimeAuthorityAdmissionV3Generation1 admission;
      auto carrier = dt::BlobLifetimeRuntimeConformanceAccessV3::Carrier(
          fault(9) ? fixture.carrier_binding
                   : dt::BlobMaterializedCarrierBindingV3{},
          dt::BlobValueStateV3::value, nullptr, 0);
      const auto result = dt::BlobLifetimeRuntimeFactoryV3Generation1::
          AdmitMaterialized(
              fault(8) ? SB_BLOB_OP_RESTORE_V3 : SB_BLOB_OP_READ_V3,
              profile, &authority, &services, fixture.pins, budget,
              fixture.services.monotonic_clock,
              fixture.services.monotonic_clock, token, request, capability,
              admission, carrier);
      if (result.diagnostic_code != expected[first].code ||
          result.reason != expected[first].reason) {
        std::cerr << "factory gates=" << first << ',' << second
                  << " actual=" << result.diagnostic_code << '/'
                  << result.reason << '\n';
        Fail("factory first-failure order");
      }
      Check(!result.ok(), "factory multi-fault admitted");
      if ((result.fact.parameters.present &
           dt::blob_parameter_descriptor_identity) != 0) {
        Check(result.fact.parameters.descriptor_uuid ==
                  canonical_descriptor.descriptor_uuid &&
                  result.fact.parameters.descriptor_generation ==
                      canonical_descriptor.descriptor_generation,
              "factory leaked unadmitted descriptor identity");
      }
      Check(!dt::BlobLifetimeRuntimeConformanceAccessV3::AdmissionAdmitted(
                admission) &&
                fixture.fake.pin_acquire_calls == 2 &&
                fixture.fake.pin_drop_calls == 2 &&
                fixture.fake.call_count == 0 &&
                fixture.fake.clock_calls == 0,
            "factory failure acquired effects or leaked pins");
      Check(dt::BlobLifetimeRuntimeConformanceAccessV3::CarrierUuid(carrier) ==
                (fault(9) ? fixture.carrier_binding.carrier_uuid
                          : platform::Uuid{}),
            "factory failure mutated caller output");
    }
  }
}

void FactoryDiagnosticsBeforeUnpin() {
  for (const unsigned failure_kind : {0U, 1U, 2U}) {
    const bool length_failure = failure_kind == 1;
    const bool owner_failure = failure_kind == 2;
    Fixture fixture(16);
    auto request = fixture.request;
    if (failure_kind == 0) request.operation = SB_BLOB_OP_RESTORE_V3;
    if (owner_failure)
      request.required_owner_class = SB_BLOB_OWNER_ENCODED_ARTIFACT_BORROW_V3;
    const auto admitted_operation = request.operation;
    auto budget = dt::BlobLifetimeReceiverHostV3Generation1::BudgetControl(
        fixture.ledger, fixture.pins);
    const platform::byte dummy = 0;
    const auto length = length_failure ? dt::kBlobMaximumLogicalBytesV3 + 1 : 1;
    auto capability = dt::BlobLifetimeReceiverHostV3Generation1::Capability(
        fixture.carrier_binding, dt::BlobValueStateV3::value, &dummy, length);
    fixture.fake.poison_request_on_drop = &request;
    fixture.fake.poison_capability_on_drop = &capability;
    dt::BlobLifetimeAuthorityAdmissionV3Generation1 admission;
    dt::BlobBoundMaterializedCarrierV3Generation1 carrier;
    const auto result = dt::BlobLifetimeRuntimeFactoryV3Generation1::AdmitMaterialized(
        failure_kind == 0 ? SB_BLOB_OP_RESTORE_V3 : SB_BLOB_OP_READ_V3,
        fixture.profile, &fixture.authority, &fixture.services, fixture.pins,
        budget, fixture.services.monotonic_clock, fixture.services.monotonic_clock,
        fixture.token, request, capability, admission, carrier);
    Check(!result.ok() && fixture.fake.pin_drop_calls == 2 && request.operation == 0,
          "last factory pin drop did not poison released request");
    if (!owner_failure)
      Check(result.fact.parameters.operation_enum == admitted_operation,
            "factory read diagnostic operation after last pin drop");
    else
      Check(result.fact.parameters.owner_class_enum == fixture.token.owner_class,
            "factory owner diagnostic did not preserve authenticated token owner");
    if (length_failure)
      Check(result.fact.parameters.actual_length_u64 == length,
            "factory read diagnostic length after last pin drop");
  }
}

void ExactF02OrdinaryGates() {
  struct GateRow {
    dt::BlobLifetimeGateV3 gate;
    dt::BlobLifetimePublicPhaseV3 public_phase;
    dt::BlobLifetimePhaseV3 invariant_phase;
  };
  constexpr std::array<GateRow, 13> rows{{
      {dt::BlobLifetimeGateV3::before_retain,
       dt::BlobLifetimePublicPhaseV3::retain,
       dt::BlobLifetimePhaseV3::retain},
      {dt::BlobLifetimeGateV3::after_retain,
       dt::BlobLifetimePublicPhaseV3::retain,
       dt::BlobLifetimePhaseV3::retain},
      {dt::BlobLifetimeGateV3::before_ordinary_probe,
       dt::BlobLifetimePublicPhaseV3::probe,
       dt::BlobLifetimePhaseV3::probe},
      {dt::BlobLifetimeGateV3::after_ordinary_probe,
       dt::BlobLifetimePublicPhaseV3::probe,
       dt::BlobLifetimePhaseV3::probe},
      {dt::BlobLifetimeGateV3::before_final_probe,
       dt::BlobLifetimePublicPhaseV3::probe,
       dt::BlobLifetimePhaseV3::probe},
      {dt::BlobLifetimeGateV3::after_final_probe,
       dt::BlobLifetimePublicPhaseV3::probe,
       dt::BlobLifetimePhaseV3::probe},
      {dt::BlobLifetimeGateV3::before_begin_access,
       dt::BlobLifetimePublicPhaseV3::begin_access,
       dt::BlobLifetimePhaseV3::begin_access},
      {dt::BlobLifetimeGateV3::after_begin_access,
       dt::BlobLifetimePublicPhaseV3::begin_access,
       dt::BlobLifetimePhaseV3::begin_access},
      {dt::BlobLifetimeGateV3::before_protected_work,
       dt::BlobLifetimePublicPhaseV3::protected_access,
       dt::BlobLifetimePhaseV3::protected_access},
      {dt::BlobLifetimeGateV3::after_protected_work,
       dt::BlobLifetimePublicPhaseV3::protected_access,
       dt::BlobLifetimePhaseV3::protected_access},
      {dt::BlobLifetimeGateV3::before_pass_transition,
       dt::BlobLifetimePublicPhaseV3::final_probe,
       dt::BlobLifetimePhaseV3::pass_transition},
      {dt::BlobLifetimeGateV3::before_sink_commit,
       dt::BlobLifetimePublicPhaseV3::atomic_commit,
       dt::BlobLifetimePhaseV3::atomic_commit},
      {dt::BlobLifetimeGateV3::before_publication_or_return,
       dt::BlobLifetimePublicPhaseV3::atomic_commit,
       dt::BlobLifetimePhaseV3::atomic_commit},
  }};
  for (const auto& row : rows) {
    for (std::uint8_t source = 0; source < 2; ++source) {
      Fixture fixture(16);
      auto carrier = dt::BlobLifetimeRuntimeConformanceAccessV3::Carrier(
          fixture.carrier_binding, dt::BlobValueStateV3::value, nullptr, 0);
      dt::BlobRetainedLifetimeLeaseV3 lease;
      Check(dt::RetainBaseBlobLifetimeLeaseV3Generation1(
                fixture.profile, fixture.Admission(), carrier, fixture.token,
                fixture.request, lease).ok(),
            "F02 ordinary-gate setup retain failed");
      fixture.fake.clock_unavailable = source == 0;
      fixture.fake.cancellation_unavailable = source == 1;
      const auto result =
          dt::BlobLifetimeRuntimeConformanceAccessV3::Gate(
              lease, row.gate, row.public_phase, row.invariant_phase);
      Check(!result.ok() &&
                result.fact.fact_class ==
                    dt::BlobLifetimeFactClassV3::
                        lifetime_authority_unavailable &&
                result.fact.stage ==
                    dt::BlobLifetimeFactStageV3::ordinary_gate &&
                result.fact.gate_present && result.fact.gate == row.gate &&
                result.fact.parameters.phase_enum == row.public_phase,
            "F02 ordinary-gate typed fact mismatch");
      Check(result.fact.authority_source ==
                (source == 0
                     ? dt::BlobLifetimeAuthoritySourceV3::
                           monotonic_clock_unavailable
                     : dt::BlobLifetimeAuthoritySourceV3::
                           cancellation_sampler_unavailable),
            "F02 ordinary-gate source mismatch");
      fixture.fake.clock_unavailable = false;
      fixture.fake.cancellation_unavailable = false;
      Check(lease.Release().ok(), "F02 ordinary-gate cleanup release failed");
    }
  }
}

void ExactF02CallbackReturns() {
  enum class Phase : std::uint8_t {
    retain, probe, begin_access, end_access, release
  };
  constexpr std::array<Phase, 5> phases{{
      Phase::retain, Phase::probe, Phase::begin_access,
      Phase::end_access, Phase::release}};
  for (const auto phase : phases) {
    for (const std::uint8_t code : {std::uint8_t{17}, std::uint8_t{18}}) {
      Fixture fixture(16);
      const std::array<platform::byte, 1> bytes{{0x7f}};
      auto carrier = dt::BlobLifetimeRuntimeConformanceAccessV3::Carrier(
          fixture.carrier_binding, dt::BlobValueStateV3::value,
          bytes.data(), bytes.size());
      dt::BlobRetainedLifetimeLeaseV3 lease;
      dt::BlobLifetimeRuntimeResultV3 result;
      if (phase == Phase::retain) {
        fixture.fake.retain_code = code;
        result = dt::RetainBaseBlobLifetimeLeaseV3Generation1(
            fixture.profile, fixture.Admission(), carrier, fixture.token,
            fixture.request, lease);
      } else {
        Check(dt::RetainBaseBlobLifetimeLeaseV3Generation1(
                  fixture.profile, fixture.Admission(), carrier, fixture.token,
                  fixture.request, lease).ok(),
              "F02 callback setup retain failed");
        VisitContext context;
        auto visitor = dt::BlobLifetimeRuntimeConformanceAccessV3::Visitor(
            CopyVisitor, &context);
        if (phase == Phase::probe) {
          fixture.fake.probe_code = code;
          result = lease.Probe();
        } else if (phase == Phase::begin_access) {
          fixture.fake.begin_code = code;
          result = lease.VisitBoundMaterializedBytes(visitor);
        } else if (phase == Phase::end_access) {
          fixture.fake.end_code = code;
          result = lease.VisitBoundMaterializedBytes(visitor);
        } else {
          fixture.fake.release_code = code;
          result = lease.Release();
        }
      }
      const bool cleanup = phase == Phase::end_access ||
                           phase == Phase::release;
      Check(!result.ok() && result.callback_code == code &&
                result.fact.authority_source ==
                    (code == 17
                         ? dt::BlobLifetimeAuthoritySourceV3::callback_code_17
                         : dt::BlobLifetimeAuthoritySourceV3::callback_code_18),
            "F02 callback source/code mismatch");
      Check(result.fact.stage ==
                (cleanup ? dt::BlobLifetimeFactStageV3::cleanup_return
                         : dt::BlobLifetimeFactStageV3::callback_return) &&
                !result.fact.gate_present,
            "F02 callback stage/gate mismatch");
      Check(result.fact.fact_class ==
                (cleanup ? dt::BlobLifetimeFactClassV3::cleanup_protocol
                         : dt::BlobLifetimeFactClassV3::
                               lifetime_authority_unavailable),
            "F02 callback fact class mismatch");
    }
  }
}

std::uint8_t CountCall(const FakeReceiver& fake, char value) noexcept {
  std::uint8_t count = 0;
  for (std::size_t index = 0; index < fake.call_count; ++index)
    if (fake.calls[index] == value) ++count;
  return count;
}

bool ExecuteMalformedRow(const MalformedRow& row, bool explicit_release = false) {
  // Non-retain vectors declare a post-dispatch prestate. Establish the lease
  // under setup headroom, then install the vector's exact ledger bytes below.
  Fixture fixture(row.phase == 1 ? row.limit : UINT64_C(16));
  fixture.fake.malformed_phase = row.phase;
  fixture.fake.malformed_stimulus = row.stimulus;
  const std::array<platform::byte, 1> bytes{{0x5a}};
  auto carrier = dt::BlobLifetimeRuntimeConformanceAccessV3::Carrier(
      fixture.carrier_binding, dt::BlobValueStateV3::value,
      bytes.data(), bytes.size());
  dt::BlobRetainedLifetimeLeaseV3 lease;
  dt::BlobLifetimeRuntimeResultV3 result;
  VisitContext context;
  auto visitor = dt::BlobLifetimeRuntimeConformanceAccessV3::Visitor(
      CopyVisitor, &context);
  if (row.phase == 1) {
    result = dt::RetainBaseBlobLifetimeLeaseV3Generation1(
        fixture.profile, fixture.Admission(), carrier, fixture.token,
        fixture.request, lease);
  } else {
    fixture.fake.malformed_phase = 0;
    if (!dt::RetainBaseBlobLifetimeLeaseV3Generation1(
             fixture.profile, fixture.Admission(), carrier, fixture.token,
             fixture.request, lease).ok())
      return false;
    fixture.fake.call_count = 0;
    fixture.fake.invariant_calls = 0;
    dt::BlobLifetimeRuntimeConformanceAccessV3::SetCounter(
        fixture.ledger,
        dt::BlobLifetimeResourceV3::lifetime_callback_calls,
        row.limit, 0, row.phase == 4 ? 2 : 1);
    fixture.fake.malformed_phase = row.phase;
    if (row.phase == 2)
      result = lease.Probe();
    else if (row.phase == 3)
      result = lease.VisitBoundMaterializedBytes(visitor);
    else if (row.phase == 4)
      result =
          dt::BlobLifetimeRuntimeConformanceAccessV3::
              EndFromExactAccessPrestate(lease);
    else
      result = explicit_release ? lease.Release()
          : dt::BlobLifetimeRuntimeConformanceAccessV3::
                ReleaseFromExactRetainedPrestate(lease);
  }
  const auto expected_event =
      static_cast<dt::BlobLifetimeInvariantEventV3>(row.expected_event);
  const bool ok = row.expected_return == "refused" &&
      row.expected_process_termination == "none" &&
      result.disposition ==
          dt::BlobLifetimeOuterDispositionV3::public_failure &&
      result.diagnostic_code == row.expected_diagnostic &&
      result.reason == row.expected_reason &&
      result.fact.stage == (row.phase >= 4
          ? dt::BlobLifetimeFactStageV3::cleanup_return
          : dt::BlobLifetimeFactStageV3::callback_return) &&
      CountCall(fixture.fake, 'R') == row.retain &&
      CountCall(fixture.fake, 'P') == row.probe &&
      CountCall(fixture.fake, 'B') == row.begin &&
      CountCall(fixture.fake, 'E') == row.end &&
      CountCall(fixture.fake, 'L') == row.release &&
      fixture.fake.invariant_calls == row.invariants &&
      !lease.retained() &&
      (row.invariants == 0 ||
       (fixture.fake.last_event == expected_event &&
        fixture.fake.last_invariant_phase ==
            static_cast<dt::BlobLifetimePhaseV3>(
                row.expected_invariant_phase) &&
        fixture.fake.last_invariant_operation ==
            (explicit_release ? 18 : 2) &&
        fixture.fake.last_invariant_callback ==
            row.expected_invariant_callback));
  if (!ok) {
    std::cerr << "MALFORMED_ROW case=" << row.case_id
              << " phase=" << unsigned(row.phase)
              << " stimulus=" << unsigned(row.stimulus)
              << " limit=" << row.limit
              << " disposition=" << unsigned(result.disposition)
              << " diagnostic=" << result.diagnostic_code
              << " reason=" << result.reason
              << " calls=" << unsigned(CountCall(fixture.fake, 'R')) << '/'
              << unsigned(CountCall(fixture.fake, 'P')) << '/'
              << unsigned(CountCall(fixture.fake, 'B')) << '/'
              << unsigned(CountCall(fixture.fake, 'E')) << '/'
              << unsigned(CountCall(fixture.fake, 'L'))
              << " invariant=" << fixture.fake.invariant_calls << '\n';
  }
  return ok;
}

[[maybe_unused]] std::size_t ExactMalformedRows() {
  std::array<std::size_t, 5> failures_by_phase{};
  std::array<std::size_t, 11> failures_by_stimulus{};
  std::size_t failures = 0;
  for (const auto& row : kMalformedRows) {
    const pid_t child = fork();
    Check(child >= 0, "fork failed for malformed callback row");
    if (child == 0) {
      const rlimit no_core{0, 0};
      (void)setrlimit(RLIMIT_CORE, &no_core);
      const bool ok = ExecuteMalformedRow(row);
      std::cerr.flush();
      _exit(ok ? 0 : 1);
    }
    int status = 0;
    Check(waitpid(child, &status, 0) == child,
          "waitpid failed for malformed callback row");
    ++checks;
    if (!WIFEXITED(status) || WEXITSTATUS(status) != 0) {
      std::cerr << "MALFORMED_FAIL case=" << row.case_id;
      if (WIFSIGNALED(status))
        std::cerr << " signal=" << WTERMSIG(status);
      else
        std::cerr << " exit=" << WEXITSTATUS(status);
      std::cerr << '\n';
      ++failures;
      ++failures_by_phase[row.phase - 1];
      ++failures_by_stimulus[row.stimulus - 1];
    }
  }
  if (failures != 0) {
    std::cerr << "MALFORMED_RUNTIME_FAILURES total=" << failures
              << " phase=";
    for (const auto value : failures_by_phase) std::cerr << value << ',';
    std::cerr << " stimulus=";
    for (const auto value : failures_by_stimulus) std::cerr << value << ',';
    std::cerr << '\n';
  }
  return failures;
}

void CorruptLedgerCleanup() {
  Fixture fixture(8);
  auto carrier = dt::BlobLifetimeRuntimeConformanceAccessV3::Carrier(
      fixture.carrier_binding, dt::BlobValueStateV3::value, nullptr, 0);
  dt::BlobRetainedLifetimeLeaseV3 lease;
  Check(dt::RetainBaseBlobLifetimeLeaseV3Generation1(
            fixture.profile, fixture.Admission(), carrier, fixture.token,
            fixture.request, lease).ok(),
        "corrupt-ledger setup retain failed");
  fixture.fake.call_count = 0;
  fixture.fake.invariant_calls = 0;
  dt::BlobLifetimeRuntimeConformanceAccessV3::SetCounter(
      fixture.ledger, dt::BlobLifetimeResourceV3::lifetime_callback_calls,
      0, 1, 1);
  const auto result = lease.Release();
  const auto counter = dt::BlobLifetimeRuntimeConformanceAccessV3::Counter(
      fixture.ledger, dt::BlobLifetimeResourceV3::lifetime_callback_calls);
  Check(!result.ok() &&
            result.disposition ==
                dt::BlobLifetimeOuterDispositionV3::terminated_out_of_band,
        "corrupt cleanup did not terminate the public path");
  Check(CountCall(fixture.fake, 'L') == 1 &&
            fixture.fake.invariant_calls == 1 &&
            fixture.fake.last_event ==
                dt::BlobLifetimeInvariantEventV3::
                    callback_budget_counter_corrupt &&
            fixture.fake.last_invariant_phase ==
                dt::BlobLifetimePhaseV3::release,
        "corrupt cleanup did not run once before quarantine");
  Check(counter.limit == 0 && counter.invoked == 1 &&
            counter.reserved_cleanup == 0 &&
            dt::BlobLifetimeRuntimeConformanceAccessV3::LedgerQuarantined(
                fixture.ledger),
        "corrupt cleanup repaired or fabricated numeric ledger state");
}

void ExactMoveIdentityAndGuards() {
  for (const bool source_guard : {false, true}) {
    for (const bool destination_guard : {false, true}) {
      Fixture fixture(16);
      auto carrier = dt::BlobLifetimeRuntimeConformanceAccessV3::Carrier(
          fixture.carrier_binding, dt::BlobValueStateV3::value, nullptr, 0);
      dt::BlobRetainedLifetimeLeaseV3 lease;
      Check(dt::RetainBaseBlobLifetimeLeaseV3Generation1(
                fixture.profile, fixture.Admission(), carrier, fixture.token,
                fixture.request, lease).ok(),
            "self-move setup retain failed");
      dt::BlobLifetimeRuntimeConformanceAccessV3::SetMoveGuards(
          lease, source_guard, destination_guard);
      const auto result = lease.MoveReplaceFrom(std::move(lease));
      Check(result.ok() && fixture.fake.invariant_calls == 0,
            "self move replacement did not precede guard observation");
      dt::BlobLifetimeRuntimeConformanceAccessV3::SetMoveGuards(
          lease, false, false);
      Check(lease.Release().ok(), "self-move cleanup failed");
    }
  }

  for (const auto& guards :
       {std::pair{true, false}, std::pair{false, true},
        std::pair{true, true}}) {
    Fixture fixture(32);
    auto carrier = dt::BlobLifetimeRuntimeConformanceAccessV3::Carrier(
        fixture.carrier_binding, dt::BlobValueStateV3::value, nullptr, 0);
    dt::BlobRetainedLifetimeLeaseV3 source;
    dt::BlobRetainedLifetimeLeaseV3 destination;
    Check(dt::RetainBaseBlobLifetimeLeaseV3Generation1(
              fixture.profile, fixture.Admission(), carrier, fixture.token,
              fixture.request, source).ok() &&
              source.CloneInto(destination).ok(),
          "distinct same-binding move setup failed");
    fixture.fake.invariant_calls = 0;
    fixture.fake.idempotent_invariant_service = true;
    dt::BlobLifetimeRuntimeConformanceAccessV3::SetMoveGuards(
        source, guards.first, false);
    dt::BlobLifetimeRuntimeConformanceAccessV3::SetMoveGuards(
        destination, guards.second, false);
    const auto result = destination.MoveReplaceFrom(std::move(source));
    Check(!result.ok() &&
              result.reason == "same_ticket_concurrent_use" &&
              fixture.fake.invariant_calls == 0,
          "distinct same-binding guarded move was not provisional refusal");
    dt::BlobLifetimeRuntimeConformanceAccessV3::SetMoveGuards(
        source, false, false);
    dt::BlobLifetimeRuntimeConformanceAccessV3::SetMoveGuards(
        destination, false, false);
    (void)source.Release();
    (void)destination.Release();
    const auto winners = static_cast<unsigned>(guards.first) +
                         static_cast<unsigned>(guards.second);
    Check(fixture.fake.invariant_calls == winners &&
              fixture.fake.invariant_accepted_receipts == 1 &&
              fixture.fake.invariant_already_receipts == winners - 1 &&
              fixture.fake.invariant_effects == 1 &&
              fixture.fake.pin_drop_calls == 4,
          "guard cleanup did not obtain each receipt and one binding effect");
  }

  {
    Fixture source_fixture(32);
    Fixture destination_fixture(32);
    destination_fixture.token.lifetime_token_uuid = BlobUuid(0xb0);
    destination_fixture.carrier_binding.lifetime_token_uuid =
        PlatformUuid(0xb0);
    auto source_carrier =
        dt::BlobLifetimeRuntimeConformanceAccessV3::Carrier(
            source_fixture.carrier_binding, dt::BlobValueStateV3::value,
            nullptr, 0);
    auto destination_carrier =
        dt::BlobLifetimeRuntimeConformanceAccessV3::Carrier(
            destination_fixture.carrier_binding, dt::BlobValueStateV3::value,
            nullptr, 0);
    dt::BlobRetainedLifetimeLeaseV3 source;
    dt::BlobRetainedLifetimeLeaseV3 destination;
    Check(dt::RetainBaseBlobLifetimeLeaseV3Generation1(
              source_fixture.profile, source_fixture.Admission(),
              source_carrier, source_fixture.token, source_fixture.request,
              source).ok() &&
              dt::RetainBaseBlobLifetimeLeaseV3Generation1(
                  destination_fixture.profile,
                  destination_fixture.Admission(), destination_carrier,
                  destination_fixture.token, destination_fixture.request,
                  destination).ok(),
          "distinct-binding move setup failed");
    dt::BlobLifetimeRuntimeConformanceAccessV3::SetMoveGuards(
        source, true, false);
    dt::BlobLifetimeRuntimeConformanceAccessV3::SetMoveGuards(
        destination, true, false);
    const auto result = destination.MoveReplaceFrom(std::move(source));
    Check(!result.ok() && result.reason == "same_ticket_concurrent_use",
          "distinct-binding guarded move was not refused");
    dt::BlobLifetimeRuntimeConformanceAccessV3::SetMoveGuards(
        source, false, false);
    dt::BlobLifetimeRuntimeConformanceAccessV3::SetMoveGuards(
        destination, false, false);
    (void)source.Release();
    (void)destination.Release();
    if (source_fixture.fake.invariant_calls != 1 ||
        destination_fixture.fake.invariant_calls != 1)
      std::cerr << "MOVE_GUARD_OBS distinct_binding_invariants="
                << source_fixture.fake.invariant_calls << '/'
                << destination_fixture.fake.invariant_calls << '\n';
    Check(source_fixture.fake.invariant_calls == 1 &&
              destination_fixture.fake.invariant_calls == 1,
          "distinct-binding guard cleanup did not quarantine both bindings");
  }

  {
    Fixture fixture(32);
    auto carrier = dt::BlobLifetimeRuntimeConformanceAccessV3::Carrier(
        fixture.carrier_binding, dt::BlobValueStateV3::value, nullptr, 0);
    dt::BlobRetainedLifetimeLeaseV3 source;
    dt::BlobRetainedLifetimeLeaseV3 destination;
    Check(dt::RetainBaseBlobLifetimeLeaseV3Generation1(
              fixture.profile, fixture.Admission(), carrier, fixture.token,
              fixture.request, source).ok() &&
              source.CloneInto(destination).ok(),
          "invalid-destination move setup failed");
    dt::BlobLifetimeRuntimeConformanceAccessV3::SetState(destination, 8);
    const auto result = destination.MoveReplaceFrom(std::move(source));
    Check(!result.ok() && result.reason.empty() &&
              source.retained(),
          "terminal move destination consumed or changed the source");
    dt::BlobLifetimeRuntimeConformanceAccessV3::SetState(destination, 2);
    Check(source.Release().ok(), "invalid-destination source cleanup failed");
    Check(destination.Release().ok(),
          "invalid-destination destination cleanup failed");
  }
}

void PinSelectorObservability() {
  Fixture fixture(16);
  auto budget = dt::BlobLifetimeReceiverHostV3Generation1::BudgetControl(
      fixture.ledger, fixture.pins);
  auto capability = dt::BlobLifetimeReceiverHostV3Generation1::Capability(
      fixture.carrier_binding, dt::BlobValueStateV3::value, nullptr, 0);
  const auto page_size = static_cast<std::size_t>(sysconf(_SC_PAGESIZE));
  Check(page_size >= sizeof(BlobLifetimeUseRequestV3), "request guard page size");
  void* page = mmap(nullptr, page_size, PROT_READ | PROT_WRITE,
                    MAP_PRIVATE | MAP_ANONYMOUS, -1, 0);
  Check(page != MAP_FAILED, "request guard page allocation");
  auto* request = new (page) BlobLifetimeUseRequestV3(fixture.request);
  Check(mprotect(page, page_size, PROT_NONE) == 0, "request guard protection");
  for (unsigned surface = 0; surface != 5; ++surface) {
  for (unsigned op_index = 0; op_index != 16; ++op_index) {
    const auto operation = static_cast<std::uint8_t>(op_index == 15 ? 255 : op_index);
    auto pins = fixture.pins;
    if (surface == 0) pins.acquire_pin = nullptr;
    const auto old_acquires = fixture.fake.pin_acquire_calls;
    const auto old_drops = fixture.fake.pin_drop_calls;
    fixture.fake.pin_failure_call = static_cast<std::uint8_t>(
        old_acquires + (surface <= 2 ? 1 : 2));
    fixture.fake.pin_acquire_status = (surface % 2) == 1
        ? dt::BlobReceiverPinAcquireStatusV3::unavailable
        : dt::BlobReceiverPinAcquireStatusV3::draining;
    fixture.fake.pin_zero_cookie = true;
    dt::BlobLifetimeAuthorityAdmissionV3Generation1 admission;
    dt::BlobBoundMaterializedCarrierV3Generation1 carrier;
    const auto result = dt::BlobLifetimeRuntimeFactoryV3Generation1::AdmitMaterialized(
        operation, fixture.profile, &fixture.authority, &fixture.services, pins,
        budget, fixture.services.monotonic_clock, fixture.services.monotonic_clock,
        fixture.token, *request, capability, admission, carrier);
    const bool valid = operation >= 1 && operation <= 13;
    Check(valid ? result.diagnostic_code == "BLOB.LIFETIME_AUTHORITY_UNAVAILABLE" &&
                       result.fact.parameters.operation_enum == operation &&
                       result.fact.parameters.phase_enum == dt::BlobLifetimePublicPhaseV3::retain &&
                       result.reason == (surface == 2 || surface == 4
                           ? "shutdown_drain_violation" : "receiver_services_failed")
                : result.disposition == dt::BlobLifetimeOuterDispositionV3::terminated_out_of_band &&
                       result.diagnostic_code.empty(),
          "pre-pin selector was not independent and bounded");
    Check(fixture.fake.pin_acquire_calls - old_acquires ==
              (valid && surface != 0 ? (surface <= 2 ? 1U : 2U) : 0U) &&
              fixture.fake.pin_drop_calls - old_drops ==
              (valid && surface >= 3 ? 1U : 0U),
          "pre-pin selector called unexpected provider or leaked first pin");
  }
  }
  fixture.fake.pin_failure_call = 0;
  const auto completed_acquires = fixture.fake.pin_acquire_calls;
  const auto completed_drops = fixture.fake.pin_drop_calls;
  Check(mprotect(page, page_size, PROT_READ | PROT_WRITE) == 0,
        "request guard restore");
  request->~BlobLifetimeUseRequestV3();
  Check(munmap(page, page_size) == 0, "request guard unmap");
  for (const std::uint8_t operation : {std::uint8_t{1}, std::uint8_t{13}}) {
    dt::BlobLifetimeAuthorityAdmissionV3Generation1 admission;
    dt::BlobBoundMaterializedCarrierV3Generation1 carrier;
    const auto result = dt::BlobLifetimeRuntimeFactoryV3Generation1::AdmitMaterialized(
        operation, fixture.profile, &fixture.authority, &fixture.services, fixture.pins,
        budget, fixture.services.monotonic_clock, fixture.services.monotonic_clock,
        fixture.token, fixture.request, capability, admission, carrier);
    Check(result.diagnostic_code == "CINL.LOB.DESCRIPTOR_INVALID" &&
              result.reason == "token_struct_invalid",
          "trusted selector mismatch passed pinned request admission");
  }
  Check(fixture.fake.pin_acquire_calls == completed_acquires + 4 &&
            fixture.fake.pin_drop_calls == completed_drops + 4,
        "selector observability leaked pins");
}

void FactoryStateAndClockBoundaries() {
  const platform::byte dummy = 0;
  for (unsigned flags = 0; flags != 4; ++flags) {
  for (unsigned modes = 0; modes != 8; ++modes) {
    Fixture fixture(16);
    fixture.token.mode_bits = SB_BLOB_MODE_IMMUTABLE_READ_V3 | (modes << 3) | (flags << 1);
    fixture.request.required_mode_bits = fixture.token.mode_bits;
    const bool single = modes == 1 || modes == 2 || modes == 4;
    auto budget = dt::BlobLifetimeReceiverHostV3Generation1::BudgetControl(
        fixture.ledger, fixture.pins);
    auto capability = dt::BlobLifetimeReceiverHostV3Generation1::Capability(
        fixture.carrier_binding, dt::BlobValueStateV3::value, nullptr, 0);
    dt::BlobLifetimeAuthorityAdmissionV3Generation1 admission;
    dt::BlobBoundMaterializedCarrierV3Generation1 carrier;
    const auto result = dt::BlobLifetimeRuntimeFactoryV3Generation1::AdmitMaterialized(
        SB_BLOB_OP_READ_V3, fixture.profile, &fixture.authority, &fixture.services,
        fixture.pins, budget, fixture.services.monotonic_clock,
        fixture.services.monotonic_clock, fixture.token, fixture.request,
        capability, admission, carrier);
    if (!single) Check(result.reason == "token_struct_invalid", "ambiguous token carrier bits");
    else if (modes == 1 && (flags & 1) == 0) Check(result.ok(), "materialized carrier refused");
    else Check(result.diagnostic_code == "BLOB.HANDLE_MODE_REFUSED" &&
                   result.reason.empty() &&
                   result.fact.parameters.open_mode_enum == (modes == 1 ? 4 : modes == 2 ? 5 : 6) &&
                   result.fact.parameters.operation_enum == SB_BLOB_OP_READ_V3,
               "mode refusal fabricated a materialized mode");
  }
  }
  // Exercise each population field independently, including partial bindings.
  for (unsigned field = 0; field != 9; ++field) {
    for (const unsigned state : {0U, 1U, 2U, 3U, 255U}) {
      Fixture fixture(16);
      dt::BlobMaterializedCarrierBindingV3 binding;
      if (field == 0) binding.authority_instance_uuid = PlatformUuid(1);
      if (field == 1) binding.authority_instance_generation = 1;
      if (field == 2) binding.carrier_uuid = PlatformUuid(1);
      if (field == 3) binding.carrier_generation = 1;
      if (field == 4) binding.lifetime_token_uuid = PlatformUuid(1);
      if (field == 5) binding.lifetime_token_generation = 1;
      if (field == 6) binding.immutable_binding_generation = 1;
      auto carrier = dt::BlobLifetimeRuntimeConformanceAccessV3::Carrier(
          binding, static_cast<dt::BlobValueStateV3>(state),
          field == 7 ? &dummy : nullptr, field == 8 ? 1 : 0);
      auto budget = dt::BlobLifetimeReceiverHostV3Generation1::BudgetControl(
          fixture.ledger, fixture.pins);
      auto capability = dt::BlobLifetimeReceiverHostV3Generation1::Capability(
          fixture.carrier_binding, dt::BlobValueStateV3::value, nullptr, 0);
      dt::BlobLifetimeAuthorityAdmissionV3Generation1 admission;
      const auto result = dt::BlobLifetimeRuntimeFactoryV3Generation1::AdmitMaterialized(
          SB_BLOB_OP_READ_V3, fixture.profile, &fixture.authority, &fixture.services,
          fixture.pins, budget, fixture.services.monotonic_clock,
          fixture.services.monotonic_clock, fixture.token, fixture.request,
          capability, admission, carrier);
      Check(state <= 2 ? result.diagnostic_code == "BLOB.STATE_INVALID" &&
                result.reason.empty() &&
                result.fact.parameters.present == (dt::blob_parameter_supplied_state |
                                                    dt::blob_parameter_operation) &&
                result.fact.parameters.supplied_state_u8 == state &&
                result.fact.parameters.operation_enum == SB_BLOB_OP_READ_V3
            : result.diagnostic_code == "CINL.LOB.DESCRIPTOR_INVALID" &&
                result.reason == "token_struct_invalid",
            "output population field/state diagnostic");
      Check(fixture.fake.pin_drop_calls == 2 && fixture.fake.call_count == 0 &&
                !dt::BlobLifetimeRuntimeConformanceAccessV3::AdmissionAdmitted(admission),
            "populated output acquired effects");
    }
  }
  for (const unsigned state : {0U, 1U, 2U, 3U, 255U}) {
    for (bool pointer : {false, true}) {
      for (bool length : {false, true}) {
        Fixture fixture(16);
        auto budget = dt::BlobLifetimeReceiverHostV3Generation1::BudgetControl(
            fixture.ledger, fixture.pins);
        auto capability = dt::BlobLifetimeReceiverHostV3Generation1::Capability(
            fixture.carrier_binding, static_cast<dt::BlobValueStateV3>(state),
            pointer ? &dummy : nullptr, length ? 1 : 0);
        dt::BlobLifetimeAuthorityAdmissionV3Generation1 admission;
        dt::BlobBoundMaterializedCarrierV3Generation1 carrier;
        const auto result = dt::BlobLifetimeRuntimeFactoryV3Generation1::AdmitMaterialized(
            SB_BLOB_OP_READ_V3, fixture.profile, &fixture.authority, &fixture.services,
            fixture.pins, budget, fixture.services.monotonic_clock,
            fixture.services.monotonic_clock, fixture.token, fixture.request,
            capability, admission, carrier);
        const bool admitted = state == 0 ? (!length || pointer)
                                       : state == 1 && !length && !pointer;
        if (admitted) Check(result.ok(), "valid state relation rejected");
        else if (state > 2)
          Check(result.reason == "token_struct_invalid", "unknown input state domain");
        else
          Check(result.diagnostic_code == "BLOB.STATE_INVALID" && result.reason.empty() &&
                    result.fact.parameters.present == (dt::blob_parameter_supplied_state |
                                                        dt::blob_parameter_operation) &&
                    result.fact.parameters.supplied_state_u8 == state &&
                    result.fact.parameters.operation_enum == SB_BLOB_OP_READ_V3,
                "known input state relation diagnostic");
      }
    }
  }
  // Every clock binding has independent nil/generation/mismatch failures.
  for (unsigned source = 0; source != 3; ++source) {
    for (unsigned fault = 0; fault != 4; ++fault) {
      for (bool missing_service : {false, true}) {
        Fixture fixture(16);
        auto services = fixture.services;
        auto token_clock = services.monotonic_clock;
        auto request_clock = services.monotonic_clock;
        auto& selected = source == 0 ? services.monotonic_clock
                           : source == 1 ? token_clock : request_clock;
        if (fault == 0) selected.clock_uuid = {};
        if (fault == 1) selected.clock_generation = 0;
        if (fault == 2) selected.clock_uuid = PlatformUuid(0xe0);
        if (fault == 3) ++selected.clock_generation;
        if (missing_service) services.read_monotonic_ns = nullptr;
        auto budget = dt::BlobLifetimeReceiverHostV3Generation1::BudgetControl(
            fixture.ledger, fixture.pins);
        auto capability = dt::BlobLifetimeReceiverHostV3Generation1::Capability(
            fixture.carrier_binding, dt::BlobValueStateV3::value, nullptr, 0);
        dt::BlobLifetimeAuthorityAdmissionV3Generation1 admission;
        dt::BlobBoundMaterializedCarrierV3Generation1 carrier;
        const auto result = dt::BlobLifetimeRuntimeFactoryV3Generation1::AdmitMaterialized(
            SB_BLOB_OP_READ_V3, fixture.profile, &fixture.authority, &services,
            fixture.pins, budget, token_clock, request_clock, fixture.token,
            fixture.request, capability, admission, carrier);
        Check(result.reason == (missing_service ? "receiver_services_failed"
                  : fault < 2 ? "monotonic_clock_unavailable"
                              : "authority_profile_binding_mismatch"),
              "clock absence/mismatch precedence");
        Check(fixture.fake.pin_drop_calls == 2 && fixture.fake.clock_calls == 0 &&
                  fixture.fake.call_count == 0, "clock gate executed callbacks");
      }
    }
  }
  // An already owned admission is preserved; only newly acquired pins drop.
  {
    Fixture fixture(16);
    auto admission = fixture.Admission();
    const auto existing_pins = fixture.fake.pins;
    auto budget = dt::BlobLifetimeReceiverHostV3Generation1::BudgetControl(
        fixture.ledger, fixture.pins);
    auto capability = dt::BlobLifetimeReceiverHostV3Generation1::Capability(
        fixture.carrier_binding, static_cast<dt::BlobValueStateV3>(255), nullptr, 0);
    dt::BlobBoundMaterializedCarrierV3Generation1 carrier;
    const auto result = dt::BlobLifetimeRuntimeFactoryV3Generation1::AdmitMaterialized(
        SB_BLOB_OP_READ_V3, fixture.profile, &fixture.authority, &fixture.services,
        fixture.pins, budget, fixture.services.monotonic_clock,
        fixture.services.monotonic_clock, fixture.token, fixture.request,
        capability, admission, carrier);
    Check(result.reason == "destination_armed" && fixture.fake.pins == existing_pins &&
              fixture.fake.pin_drop_calls == 2 &&
              dt::BlobLifetimeRuntimeConformanceAccessV3::AdmissionAdmitted(admission),
          "armed admission overwritten or input state checked prematurely");
  }
}

void FactoryEarlyGateObservability() {
  Fixture fixture(16);
  const auto page_size = static_cast<std::size_t>(sysconf(_SC_PAGESIZE));
  void* page = mmap(nullptr, page_size, PROT_READ | PROT_WRITE,
                    MAP_PRIVATE | MAP_ANONYMOUS, -1, 0);
  Check(page != MAP_FAILED, "factory guard allocation");
  auto* request = new (page) BlobLifetimeUseRequestV3(fixture.request);
  Check(mprotect(page, page_size, PROT_NONE) == 0, "factory guard protection");
  for (unsigned gate = 0; gate != 3; ++gate) {
    auto authority = fixture.authority;
    const bool bad_abi = gate == 1;
    if (bad_abi) authority.struct_bytes = 0;
    dt::BlobLifetimeBudgetLedgerV3Generation1 unfrozen;
    auto budget = dt::BlobLifetimeReceiverHostV3Generation1::BudgetControl(
        bad_abi ? fixture.ledger : unfrozen, fixture.pins);
    auto capability = dt::BlobLifetimeReceiverHostV3Generation1::Capability(
        fixture.carrier_binding, dt::BlobValueStateV3::value, nullptr, 0);
    dt::BlobLifetimeAuthorityAdmissionV3Generation1 admission;
    dt::BlobBoundMaterializedCarrierV3Generation1 carrier;
    const auto result = dt::BlobLifetimeRuntimeFactoryV3Generation1::AdmitMaterialized(
        SB_BLOB_OP_READ_V3, fixture.profile, gate == 2 ? nullptr : &authority, &fixture.services,
        fixture.pins, budget, fixture.services.monotonic_clock,
        fixture.services.monotonic_clock, fixture.token, *request,
        capability, admission, carrier);
    Check(result.reason == (gate == 2 ? "authority_table_missing" : "authority_abi_mismatch"),
          "factory ABI/freeze/null-table gate read later request");
  }
  Check(mprotect(page, page_size, PROT_READ | PROT_WRITE) == 0, "factory guard restore");
  request->~BlobLifetimeUseRequestV3();
  Check(munmap(page, page_size) == 0, "factory guard unmap");
  Check(fixture.fake.pin_drop_calls == 6 && fixture.fake.call_count == 0,
        "early factory gates leaked pins or called authority");
}

void NullVisitorStatePrecedence() {
  using Access = dt::BlobLifetimeRuntimeConformanceAccessV3;
  using Lifetime = dt::BlobLifetimeDiagnosticLifetimeV3;
  using Expiry = dt::BlobLifetimeDiagnosticExpiryReasonV3;
  struct LocalState {
    std::uint8_t state;
    Lifetime lifetime;
    Expiry expiry;
  };
  constexpr std::array<LocalState,4> cases{{
      {0,Lifetime::retain_ticket_invalid,Expiry::retain_ticket_unknown},
      {7,Lifetime::retain_ticket_consumed,Expiry::retain_ticket_consumed},
      {8,Lifetime::owner_closed,Expiry::owner_closed},
      {9,Lifetime::moved_binding,Expiry::moved_binding_generation},
  }};
  const auto visitor = Access::Visitor(nullptr, nullptr);
  for (const auto& row : cases) {
    dt::BlobRetainedLifetimeLeaseV3 lease;
    Access::SetState(lease,row.state);
    const auto result = lease.VisitBoundMaterializedBytes(visitor);
    Check(result.diagnostic_code == "CINL.LOB.HANDLE_EXPIRED" && result.reason.empty() &&
              result.fact.parameters.present == (dt::blob_parameter_lifetime | dt::blob_parameter_expiry_reason) &&
              result.fact.parameters.lifetime_enum == row.lifetime &&
              result.fact.parameters.expiry_reason_enum == row.expiry &&
              Access::ControlState(lease) == row.state,
          "null visitor fabricated descriptor on unbound/final lease");
  }
  Fixture fixture(16);
  dt::BlobRetainedLifetimeLeaseV3 lease;
  auto carrier = Access::Carrier(fixture.carrier_binding,dt::BlobValueStateV3::value,nullptr,0);
  Check(dt::RetainBaseBlobLifetimeLeaseV3Generation1(
            fixture.profile,fixture.Admission(),carrier,fixture.token,fixture.request,lease).ok(),
        "null visitor retain setup");
  const auto calls=fixture.fake.call_count;
  const auto clocks=fixture.fake.clock_calls;
  const auto result=lease.VisitBoundMaterializedBytes(visitor);
  Check(result.reason=="visitor_missing" &&
            result.fact.parameters.descriptor_uuid==fixture.profile.identity.descriptor_uuid &&
            result.fact.parameters.descriptor_generation==fixture.profile.identity.descriptor_generation &&
            result.fact.stage==dt::BlobLifetimeFactStageV3::local_precondition && lease.retained() &&
            fixture.fake.call_count==calls && fixture.fake.clock_calls==clocks,
        "retained null visitor lost identity or mutated owner");
  Check(lease.Release().ok(), "null visitor explicit release");
}

void PinResultProtocol() {
  // All four surfaces share the same status/cookie/reserved-byte contract.
  // Check both legal zero-cookie refusals and process death for malformed
  // replies, including each reserved byte independently.
  for (unsigned surface = 0; surface != 4; ++surface) {
    for (const unsigned status : {0U, 1U, 2U, 3U, 4U, 255U}) {
      for (int dirty_byte = -1; dirty_byte != 7; ++dirty_byte) {
        for (const bool zero_cookie : {false, true}) {
          const bool malformed = dirty_byte >= 0 || status >= 3 ||
              (status == 0 ? zero_cookie : !zero_cookie);
          const pid_t child = fork();
          Check(child >= 0, "pin reply fork");
          if (child == 0) {
            std::set_terminate([] { std::abort(); });
            Fixture fixture(64);
            dt::BlobRetainedLifetimeLeaseV3 source;
            dt::BlobRetainedLifetimeLeaseV3 destination;
            if (surface >= 2) {
              auto bound = dt::BlobLifetimeRuntimeConformanceAccessV3::Carrier(
                  fixture.carrier_binding, dt::BlobValueStateV3::value, nullptr, 0);
              if (!dt::RetainBaseBlobLifetimeLeaseV3Generation1(
                      fixture.profile, fixture.Admission(), bound, fixture.token,
                      fixture.request, source).ok()) _exit(2);
              fixture.fake.pin_clone_failure_call = surface - 1;
            } else fixture.fake.pin_failure_call = surface + 1;
            fixture.fake.pin_acquire_status =
                static_cast<dt::BlobReceiverPinAcquireStatusV3>(status);
            fixture.fake.pin_zero_cookie = zero_cookie;
            fixture.fake.pin_dirty_reserved = dirty_byte >= 0;
            fixture.fake.pin_dirty_reserved_index = dirty_byte < 0 ? 0 : dirty_byte;
            auto budget = dt::BlobLifetimeReceiverHostV3Generation1::BudgetControl(
                fixture.ledger, fixture.pins);
            auto capability = dt::BlobLifetimeReceiverHostV3Generation1::Capability(
                fixture.carrier_binding, dt::BlobValueStateV3::value, nullptr, 0);
            dt::BlobLifetimeAuthorityAdmissionV3Generation1 admission;
            dt::BlobBoundMaterializedCarrierV3Generation1 carrier;
            const auto result = surface >= 2 ? source.CloneInto(destination)
                : dt::BlobLifetimeRuntimeFactoryV3Generation1::AdmitMaterialized(
                    SB_BLOB_OP_READ_V3, fixture.profile, &fixture.authority,
                    &fixture.services, fixture.pins, budget,
                    fixture.services.monotonic_clock, fixture.services.monotonic_clock,
                    fixture.token, fixture.request, capability, admission, carrier);
            if (malformed) _exit(3); // A malformed provider must not return.
            if (status == 0) {
              if (!result.ok()) _exit(4);
              if (surface >= 2 &&
                  (!source.Release().ok() || !destination.Release().ok())) _exit(5);
              _exit(0);
            }
            const bool draining = status == 1;
            const bool exact = result.disposition ==
                    dt::BlobLifetimeOuterDispositionV3::public_failure &&
                result.diagnostic_code == "BLOB.LIFETIME_AUTHORITY_UNAVAILABLE" &&
                result.reason == (draining ? "shutdown_drain_violation"
                                           : "receiver_services_failed") &&
                result.fact.parameters.present ==
                    (dt::blob_parameter_phase | dt::blob_parameter_operation) &&
                result.fact.parameters.operation_enum == SB_BLOB_OP_READ_V3 &&
                result.fact.parameters.phase_enum == dt::BlobLifetimePublicPhaseV3::retain &&
                fixture.fake.pin_drop_calls == (surface % 2) &&
                fixture.fake.invariant_calls == 0;
            if (surface >= 2 && (!source.retained() || !source.Release().ok()))
              _exit(6);
            _exit(exact ? 0 : 7);
          }
          int wait_status = 0;
          Check(waitpid(child, &wait_status, 0) == child, "pin reply waitpid");
          Check(malformed ? WIFSIGNALED(wait_status) && WTERMSIG(wait_status) == SIGABRT
                          : WIFEXITED(wait_status) && WEXITSTATUS(wait_status) == 0,
                "pin reply did not obey exact refusal/termination contract");
        }
      }
    }
  }
  {
    Fixture fixture(16);
    auto budget =
        dt::BlobLifetimeReceiverHostV3Generation1::BudgetControl(
            fixture.ledger, fixture.pins);
    auto capability =
        dt::BlobLifetimeReceiverHostV3Generation1::Capability(
            fixture.carrier_binding, dt::BlobValueStateV3::value, nullptr, 0);
    {
      dt::BlobLifetimeAuthorityAdmissionV3Generation1 admission;
      dt::BlobBoundMaterializedCarrierV3Generation1 carrier;
      const auto result = dt::BlobLifetimeRuntimeFactoryV3Generation1::
          AdmitMaterialized(
              SB_BLOB_OP_READ_V3,
              fixture.profile, &fixture.authority, &fixture.services,
              fixture.pins, budget, fixture.services.monotonic_clock,
              fixture.services.monotonic_clock, fixture.token,
              fixture.request, capability, admission, carrier);
      Check(result.ok() && fixture.fake.pin_acquire_calls == 2 &&
                fixture.fake.pin_drop_calls == 0,
            "valid admission pin pair was not retained");
    }
    Check(fixture.fake.pin_drop_calls == 2,
          "admission destruction did not drop both pins exactly once");
  }
}

void CleanupPrecedence() {
  for (const std::uint8_t end_code :
       {std::uint8_t{SB_BLOB_CALLBACK_SECURITY_DENIED_V3},
        std::uint8_t{SB_BLOB_CALLBACK_RESOURCE_EXHAUSTED_V3}}) {
    Fixture fixture(16);
    fixture.fake.end_code = end_code;
    const std::array<platform::byte, 1> bytes{{0x42}};
    auto carrier = dt::BlobLifetimeRuntimeConformanceAccessV3::Carrier(
        fixture.carrier_binding, dt::BlobValueStateV3::value,
        bytes.data(), bytes.size());
    dt::BlobRetainedLifetimeLeaseV3 lease;
    Check(dt::RetainBaseBlobLifetimeLeaseV3Generation1(
              fixture.profile, fixture.Admission(), carrier, fixture.token,
              fixture.request, lease).ok(),
          "cleanup precedence setup retain failed");
    auto visitor = dt::BlobLifetimeRuntimeConformanceAccessV3::Visitor(
        ResourceVisitor, nullptr);
    const auto result = lease.VisitBoundMaterializedBytes(visitor);
    if (end_code == SB_BLOB_CALLBACK_SECURITY_DENIED_V3) {
      Check(!result.ok() &&
                result.fact.fact_class ==
                    dt::BlobLifetimeFactClassV3::security &&
                result.diagnostic_code == "SECURITY.ACCESS_DENIED",
            "cleanup SECURITY did not override lower existing primary");
    } else {
      Check(!result.ok() &&
                result.fact.fact_class ==
                    dt::BlobLifetimeFactClassV3::resource_budget &&
                result.diagnostic_code == "RESOURCE.BUDGET_EXCEEDED",
            "non-SECURITY cleanup replaced an existing primary");
    }
    if (CountCall(fixture.fake, 'E') != 1 ||
        CountCall(fixture.fake, 'L') != 1 ||
        fixture.fake.invariant_calls != 1 ||
        fixture.fake.last_event !=
            dt::BlobLifetimeInvariantEventV3::end_access_failed)
      std::cerr << "CLEANUP_PRECEDENCE_OBS code=" << unsigned(end_code)
                << " result=" << result.diagnostic_code << '/'
                << result.reason << " calls="
                << unsigned(CountCall(fixture.fake, 'E')) << '/'
                << unsigned(CountCall(fixture.fake, 'L'))
                << " invariants=" << fixture.fake.invariant_calls
                << " tuple=" << unsigned(fixture.fake.last_event) << '/'
                << unsigned(fixture.fake.last_invariant_phase) << '/'
                << unsigned(fixture.fake.last_invariant_operation) << '/'
                << unsigned(fixture.fake.last_invariant_callback) << '\n';
    Check(CountCall(fixture.fake, 'E') == 1 &&
              CountCall(fixture.fake, 'L') == 1 &&
              fixture.fake.invariant_calls == 1 &&
              fixture.fake.last_event ==
                  dt::BlobLifetimeInvariantEventV3::end_access_failed,
          "cleanup precedence path did not end/release/quarantine exactly");
  }
}

void AllocationFreeMaterializedPath() {
  {
    Fixture factory_fixture(16);
    auto budget = dt::BlobLifetimeReceiverHostV3Generation1::BudgetControl(
        factory_fixture.ledger, factory_fixture.pins);
    auto capability = dt::BlobLifetimeReceiverHostV3Generation1::Capability(
        factory_fixture.carrier_binding, dt::BlobValueStateV3::value,
        nullptr, 0);
    dt::BlobLifetimeAuthorityAdmissionV3Generation1 admission;
    dt::BlobBoundMaterializedCarrierV3Generation1 admitted_carrier;
    const auto before_factory = allocation_probe::calls.load();
    allocation_probe::enabled.store(true);
    const auto admitted = dt::BlobLifetimeRuntimeFactoryV3Generation1::
        AdmitMaterialized(
            SB_BLOB_OP_READ_V3,
            factory_fixture.profile, &factory_fixture.authority,
            &factory_fixture.services, factory_fixture.pins, budget,
            factory_fixture.services.monotonic_clock,
            factory_fixture.services.monotonic_clock, factory_fixture.token,
            factory_fixture.request, capability, admission,
            admitted_carrier);
    allocation_probe::enabled.store(false);
    Check(admitted.ok() &&
              allocation_probe::calls.load() == before_factory,
          "materialized factory path allocated through global operator new");
  }

  Fixture fixture(16);
  const std::array<platform::byte, 2> bytes{{0x42, 0x43}};
  auto carrier = dt::BlobLifetimeRuntimeConformanceAccessV3::Carrier(
      fixture.carrier_binding, dt::BlobValueStateV3::value, bytes.data(),
      bytes.size());
  dt::BlobRetainedLifetimeLeaseV3 lease;
  VisitContext visit{};
  auto visitor = dt::BlobLifetimeRuntimeConformanceAccessV3::Visitor(
      CopyVisitor, &visit);

  const auto before_retain = allocation_probe::calls.load();
  allocation_probe::enabled.store(true);
  const auto retained = dt::RetainBaseBlobLifetimeLeaseV3Generation1(
      fixture.profile, fixture.Admission(), carrier, fixture.token,
      fixture.request, lease);
  allocation_probe::enabled.store(false);
  Check(retained.ok() && allocation_probe::calls.load() == before_retain,
        "materialized retain path allocated through global operator new");

  const auto before_probe = allocation_probe::calls.load();
  allocation_probe::enabled.store(true);
  const auto probed = lease.Probe();
  allocation_probe::enabled.store(false);
  Check(probed.ok() && allocation_probe::calls.load() == before_probe,
        "materialized probe path allocated through global operator new");

  const auto before_visit = allocation_probe::calls.load();
  allocation_probe::enabled.store(true);
  const auto visited = lease.VisitBoundMaterializedBytes(visitor);
  allocation_probe::enabled.store(false);
  Check(visited.ok() && visit.copied_size == bytes.size() &&
            allocation_probe::calls.load() == before_visit,
        "materialized protected path allocated through global operator new");

  const auto before_release = allocation_probe::calls.load();
  allocation_probe::enabled.store(true);
  const auto released = lease.Release();
  allocation_probe::enabled.store(false);
  Check(released.ok() && allocation_probe::calls.load() == before_release,
        "materialized release path allocated through global operator new");
}

bool BudgetSnapshotsEqual(
    const dt::BlobLifetimeBudgetSnapshotV3Generation1& left,
    const dt::BlobLifetimeBudgetSnapshotV3Generation1& right) noexcept {
  if (left.configured != right.configured || left.frozen != right.frozen ||
      left.terminal_quarantined != right.terminal_quarantined ||
      left.scratch_data != right.scratch_data ||
      left.scratch_size != right.scratch_size ||
      left.admitted_window_octets != right.admitted_window_octets ||
      left.admitted_relative_timeout_ns !=
          right.admitted_relative_timeout_ns)
    return false;
  for (std::size_t index = 0; index < left.counters.size(); ++index) {
    if (left.counters[index].limit != right.counters[index].limit ||
        left.counters[index].invoked != right.counters[index].invoked ||
        left.counters[index].reserved_cleanup !=
            right.counters[index].reserved_cleanup)
      return false;
  }
  return true;
}

void BudgetConfigureFreezeLifecycle() {
  dt::BlobLifetimeBudgetLedgerV3Generation1 ledger;
  const auto pristine =
      dt::BlobLifetimeRuntimeConformanceAccessV3::Snapshot(ledger);
  Check(!pristine.configured && !pristine.frozen &&
            !pristine.terminal_quarantined && pristine.scratch_data == nullptr &&
            pristine.scratch_size == 0,
        "pristine budget snapshot was not exactly empty");
  Check(!dt::BlobLifetimeRuntimeConformanceAccessV3::Freeze(ledger) &&
            BudgetSnapshotsEqual(
                pristine,
                dt::BlobLifetimeRuntimeConformanceAccessV3::Snapshot(ledger)),
        "freeze-before-configure mutated the pristine ledger");

  std::array<platform::byte, 32> scratch{};
  dt::BlobLifetimeBudgetConfigurationV3Generation1 configuration{};
  for (std::size_t index = 0; index < configuration.limits.size(); ++index)
    configuration.limits[index] = 100 + index;
  configuration.scratch = scratch;
  configuration.admitted_window_octets = 4096;
  configuration.admitted_relative_timeout_ns = 1234567;
  const auto allocation_before = allocation_probe::calls.load();
  allocation_probe::enabled.store(true);
  const bool configured =
      dt::BlobLifetimeRuntimeConformanceAccessV3::Configure(
          ledger, configuration);
  allocation_probe::enabled.store(false);
  const auto configured_snapshot =
      dt::BlobLifetimeRuntimeConformanceAccessV3::Snapshot(ledger);
  Check(configured && allocation_probe::calls.load() == allocation_before &&
            configured_snapshot.configured && !configured_snapshot.frozen &&
            configured_snapshot.scratch_data == scratch.data() &&
            configured_snapshot.scratch_size == scratch.size() &&
            configured_snapshot.admitted_window_octets == 4096 &&
            configured_snapshot.admitted_relative_timeout_ns == 1234567,
        "budget configure did not copy exact allocation-free receiver state");
  Check(configured_snapshot.counters[0].limit == 0 &&
            configured_snapshot.counters[0].invoked == 0 &&
            configured_snapshot.counters[0].reserved_cleanup == 0,
        "budget configuration wrote reserved counter zero");
  for (std::size_t index = 0; index < configuration.limits.size(); ++index) {
    Check(configured_snapshot.counters[index + 1].limit == 100 + index &&
              configured_snapshot.counters[index + 1].invoked == 0 &&
              configured_snapshot.counters[index + 1].reserved_cleanup == 0,
          "budget configuration did not copy an exact resource limit");
  }

  std::uint64_t available = 99;
  Check(!dt::BlobLifetimeRuntimeConformanceAccessV3::Charge(
            ledger, dt::BlobLifetimeResourceV3::logical_bytes_read, 1,
            &available) &&
            available == 0 &&
            BudgetSnapshotsEqual(
                configured_snapshot,
                dt::BlobLifetimeRuntimeConformanceAccessV3::Snapshot(ledger)),
        "unfrozen charge did not refuse without mutation");

  auto replacement = configuration;
  replacement.limits.fill(7);
  replacement.scratch = {};
  replacement.admitted_window_octets = 8;
  replacement.admitted_relative_timeout_ns = 9;
  Check(!dt::BlobLifetimeRuntimeConformanceAccessV3::Configure(
            ledger, replacement) &&
            BudgetSnapshotsEqual(
                configured_snapshot,
                dt::BlobLifetimeRuntimeConformanceAccessV3::Snapshot(ledger)),
        "second configure mutated configured ledger");
  const auto allocation_before_freeze = allocation_probe::calls.load();
  allocation_probe::enabled.store(true);
  const bool freeze_result =
      dt::BlobLifetimeRuntimeConformanceAccessV3::Freeze(ledger);
  const auto frozen =
      dt::BlobLifetimeRuntimeConformanceAccessV3::Snapshot(ledger);
  allocation_probe::enabled.store(false);
  Check(freeze_result &&
            allocation_probe::calls.load() == allocation_before_freeze,
        "budget freeze/snapshot allocated or refused configured ledger");
  Check(frozen.configured && frozen.frozen &&
            !dt::BlobLifetimeRuntimeConformanceAccessV3::Freeze(ledger) &&
            BudgetSnapshotsEqual(
                frozen,
                dt::BlobLifetimeRuntimeConformanceAccessV3::Snapshot(ledger)) &&
            !dt::BlobLifetimeRuntimeConformanceAccessV3::Configure(
                ledger, replacement) &&
            BudgetSnapshotsEqual(
                frozen,
                dt::BlobLifetimeRuntimeConformanceAccessV3::Snapshot(ledger)),
        "frozen ledger accepted freeze or reconfiguration mutation");

  available = 0;
  Check(dt::BlobLifetimeRuntimeConformanceAccessV3::Charge(
            ledger, dt::BlobLifetimeResourceV3::logical_bytes_read, 5,
            &available) && available == 101,
        "frozen resource charge was not admitted");
  Check(dt::BlobLifetimeRuntimeConformanceAccessV3::Reserve(
            ledger, 1, 1, &available) && available == 100,
        "frozen callback reservation was not admitted");
  const auto consumed =
      dt::BlobLifetimeRuntimeConformanceAccessV3::Consume(ledger, 1);
  Check(consumed.obligation_consumed &&
            consumed.cleanup_callback_charge_commit &&
            !consumed.ledger_terminal_quarantined,
        "reserved cleanup did not consume and commit exactly once");

  dt::BlobLifetimeBudgetLedgerV3Generation1 missing_reservation;
  Check(dt::BlobLifetimeRuntimeConformanceAccessV3::Configure(
            missing_reservation, configuration) &&
            dt::BlobLifetimeRuntimeConformanceAccessV3::Freeze(
                missing_reservation),
        "missing-reservation ledger setup failed");
  const auto missing = dt::BlobLifetimeRuntimeConformanceAccessV3::Consume(
      missing_reservation, 1);
  Check(!missing.obligation_consumed &&
            !missing.cleanup_callback_charge_commit &&
            missing.ledger_terminal_quarantined &&
            dt::BlobLifetimeRuntimeConformanceAccessV3::Snapshot(
                missing_reservation).terminal_quarantined,
        "missing cleanup reservation did not terminally quarantine ledger");

  Fixture fixture(16);
  dt::BlobLifetimeBudgetLedgerV3Generation1 unfrozen;
  auto budget = dt::BlobLifetimeReceiverHostV3Generation1::BudgetControl(
      unfrozen, fixture.pins);
  auto capability = dt::BlobLifetimeReceiverHostV3Generation1::Capability(
      fixture.carrier_binding, dt::BlobValueStateV3::value, nullptr, 0);
  dt::BlobLifetimeAuthorityAdmissionV3Generation1 admission;
  dt::BlobBoundMaterializedCarrierV3Generation1 carrier;
  const auto result = dt::BlobLifetimeRuntimeFactoryV3Generation1::
      AdmitMaterialized(
          SB_BLOB_OP_READ_V3,
          fixture.profile, &fixture.authority, &fixture.services, fixture.pins,
          budget, fixture.services.monotonic_clock,
          fixture.services.monotonic_clock, fixture.token, fixture.request,
          capability, admission, carrier);
  Check(!result.ok() &&
            result.diagnostic_code == "CINL.LOB.DESCRIPTOR_INVALID" &&
            result.reason == "authority_abi_mismatch" &&
            result.fact.reason ==
                dt::BlobLifetimeReasonV3::authority_abi_mismatch &&
            fixture.fake.pin_drop_calls == 2,
        "factory did not refuse unfrozen budget capability with exact cleanup");
}

void TwoThreadCloneMoveGuardContention() {
  for (const bool clone_operation : {true, false}) {
    Fixture fixture(32);
    const std::array<platform::byte, 1> bytes{{0x42}};
    auto carrier = dt::BlobLifetimeRuntimeConformanceAccessV3::Carrier(
        fixture.carrier_binding, dt::BlobValueStateV3::value, bytes.data(),
        bytes.size());
    dt::BlobRetainedLifetimeLeaseV3 source;
    dt::BlobRetainedLifetimeLeaseV3 destination;
    Check(dt::RetainBaseBlobLifetimeLeaseV3Generation1(
              fixture.profile, fixture.Admission(), carrier, fixture.token,
              fixture.request, source).ok(),
          "contention setup retain failed");
    BlockingVisitContext block;
    auto visitor = dt::BlobLifetimeRuntimeConformanceAccessV3::Visitor(
        BlockingVisitor, &block);
    dt::BlobLifetimeRuntimeResultV3 visit_result{};
    std::thread visiting([&] {
      visit_result = source.VisitBoundMaterializedBytes(visitor);
    });
    while (!block.entered.load(std::memory_order_acquire))
      std::this_thread::yield();

    bool refused = false;
    if (clone_operation) {
      const auto result = source.CloneInto(destination);
      refused = !result.ok() &&
          result.reason == "same_ticket_concurrent_use" &&
          !destination.retained();
    } else {
      const auto moved = dt::BlobRetainedLifetimeLeaseV3::MoveConstructFrom(
          std::move(source));
      refused = !moved.result.ok() && moved.lease() == nullptr &&
          moved.result.reason == "same_ticket_concurrent_use";
    }
    block.finish.store(true, std::memory_order_release);
    visiting.join();
    Check(refused,
          clone_operation
              ? "two-thread clone did not refuse the active source atomically"
              : "two-thread move did not refuse the active source atomically");
    Check(!visit_result.ok() || !source.retained(),
          "two-thread guard refusal left an independently armed source");
  }
}

void AtomicWinnerReceiptProtocol() {
  dt::BlobRetainedLifetimeLeaseV3 lease;
  dt::BlobLifetimeRuntimeConformanceAccessV3::SetState(lease, 3);
  std::atomic<unsigned> ready{0};
  std::atomic<bool> start{false};
  std::array<bool, 2> won{{false, false}};
  std::thread first([&] {
    ready.fetch_add(1, std::memory_order_release);
    while (!start.load(std::memory_order_acquire)) std::this_thread::yield();
    won[0] = dt::BlobLifetimeRuntimeConformanceAccessV3::ControlPublish(
        lease, dt::BlobLifetimeInvariantEventV3::same_ticket_concurrent_use,
        15, dt::BlobLifetimePhaseV3::retain, 255);
  });
  std::thread second([&] {
    ready.fetch_add(1, std::memory_order_release);
    while (!start.load(std::memory_order_acquire)) std::this_thread::yield();
    won[1] = dt::BlobLifetimeRuntimeConformanceAccessV3::ControlPublish(
        lease, dt::BlobLifetimeInvariantEventV3::callback_reentrant,
        15, dt::BlobLifetimePhaseV3::retain, 255);
  });
  while (ready.load(std::memory_order_acquire) != 2)
    std::this_thread::yield();
  start.store(true, std::memory_order_release);
  first.join();
  second.join();
  Check(won[0] != won[1],
        "packed control did not elect exactly one tuple publisher");
  const auto winner =
      dt::BlobLifetimeRuntimeConformanceAccessV3::ControlDeferred(lease);
  Check(winner != 0 &&
            !dt::BlobLifetimeRuntimeConformanceAccessV3::ControlClosing(lease),
        "winning tuple was not immutable before close");
  Check(!dt::BlobLifetimeRuntimeConformanceAccessV3::ControlPublish(
            lease,
            dt::BlobLifetimeInvariantEventV3::callback_budget_counter_corrupt,
            15, dt::BlobLifetimePhaseV3::retain, 255) &&
            dt::BlobLifetimeRuntimeConformanceAccessV3::ControlDeferred(
                lease) == winner,
        "late publisher replaced the immutable winning tuple");
  Check(dt::BlobLifetimeRuntimeConformanceAccessV3::ControlBeginClosing(lease) &&
            dt::BlobLifetimeRuntimeConformanceAccessV3::ControlClosing(lease) &&
            dt::BlobLifetimeRuntimeConformanceAccessV3::
                ControlPeekWhileClosing(lease) == winner,
        "close transition did not preserve the winning tuple");
  Check(!dt::BlobLifetimeRuntimeConformanceAccessV3::ControlTryFinalize(
            lease, 8),
        "owner finalized before service receipt");
  Check(!dt::BlobLifetimeRuntimeConformanceAccessV3::ControlCompleteReceipt(
            lease, winner ^ UINT64_C(1)) &&
            dt::BlobLifetimeRuntimeConformanceAccessV3::
                ControlPeekWhileClosing(lease) == winner,
        "wrong service receipt consumed the winning tuple");
  Check(dt::BlobLifetimeRuntimeConformanceAccessV3::ControlCompleteReceipt(
            lease, winner) &&
            dt::BlobLifetimeRuntimeConformanceAccessV3::
                ControlPeekWhileClosing(lease) == 0,
        "exact service receipt did not retire the tuple");
  Check(!dt::BlobLifetimeRuntimeConformanceAccessV3::ControlCompleteReceipt(
            lease, winner),
        "service receipt was accepted twice");
  Check(dt::BlobLifetimeRuntimeConformanceAccessV3::ControlTryFinalize(
            lease, 8) &&
            dt::BlobLifetimeRuntimeConformanceAccessV3::ControlState(lease) ==
                8 &&
            !dt::BlobLifetimeRuntimeConformanceAccessV3::ControlClosing(lease),
        "receipt did not authorize the single final transition");
  Check(!dt::BlobLifetimeRuntimeConformanceAccessV3::ControlPublish(
            lease, dt::BlobLifetimeInvariantEventV3::same_ticket_concurrent_use,
            15, dt::BlobLifetimePhaseV3::retain, 255),
        "final state accepted a late publisher");

  dt::BlobRetainedLifetimeLeaseV3 entrant_lease;
  dt::BlobLifetimeRuntimeConformanceAccessV3::SetState(entrant_lease, 3);
  Check(dt::BlobLifetimeRuntimeConformanceAccessV3::ControlEnter(
            entrant_lease) &&
            dt::BlobLifetimeRuntimeConformanceAccessV3::ControlEnter(
                entrant_lease) &&
            dt::BlobLifetimeRuntimeConformanceAccessV3::ControlEntrants(
                entrant_lease) == 2 &&
            dt::BlobLifetimeRuntimeConformanceAccessV3::ControlBeginClosing(
                entrant_lease),
        "entrant reference setup failed");
  Check(!dt::BlobLifetimeRuntimeConformanceAccessV3::ControlTryFinalize(
            entrant_lease, 8, 1),
        "owner finalized while a losing metadata entrant was live");
  dt::BlobLifetimeRuntimeConformanceAccessV3::ControlLeave(entrant_lease);
  Check(dt::BlobLifetimeRuntimeConformanceAccessV3::ControlTryFinalize(
            entrant_lease, 8, 1) &&
            dt::BlobLifetimeRuntimeConformanceAccessV3::ControlEntrants(
                entrant_lease) == 1,
        "owner did not finalize after the losing entrant retired");
  dt::BlobLifetimeRuntimeConformanceAccessV3::ControlLeave(entrant_lease);
  Check(dt::BlobLifetimeRuntimeConformanceAccessV3::ControlEntrants(
            entrant_lease) == 0 &&
            !dt::BlobLifetimeRuntimeConformanceAccessV3::ControlEnter(
                entrant_lease),
        "final state admitted a metadata reader after owner return");
}

void GenerationResetProtocol() {
  for (const auto active_state :
       {std::uint8_t{1}, std::uint8_t{3}, std::uint8_t{4},
        std::uint8_t{5}, std::uint8_t{6}}) {
    dt::BlobRetainedLifetimeLeaseV3 lease;
    dt::BlobLifetimeRuntimeConformanceAccessV3::SetState(lease, active_state);
    Check(dt::BlobLifetimeRuntimeConformanceAccessV3::ControlEnter(lease) &&
              dt::BlobLifetimeRuntimeConformanceAccessV3::
                  ControlBeginClosing(lease),
          "close-wins entrant barrier setup failed");
    bool late_entered = true;
    std::thread late([&] {
      late_entered = dt::BlobLifetimeRuntimeConformanceAccessV3::
          ControlEnter(lease);
      if (late_entered)
        dt::BlobLifetimeRuntimeConformanceAccessV3::ControlLeave(lease);
    });
    late.join();
    Check(!late_entered &&
              dt::BlobLifetimeRuntimeConformanceAccessV3::ControlEntrants(
                  lease) == 1 &&
              dt::BlobLifetimeRuntimeConformanceAccessV3::ControlDeferred(
                  lease) == 0,
          "closing state admitted a late metadata entrant");
    Check(dt::BlobLifetimeRuntimeConformanceAccessV3::ControlTryFinalize(
              lease, 8, 1),
          "close-wins entrant barrier could not finalize owner");
    dt::BlobLifetimeRuntimeConformanceAccessV3::ControlLeave(lease);
  }

  {
    dt::BlobRetainedLifetimeLeaseV3 lease;
    Check(dt::BlobLifetimeRuntimeConformanceAccessV3::ControlEnter(lease) &&
              dt::BlobLifetimeRuntimeConformanceAccessV3::ControlEnter(lease) &&
              dt::BlobLifetimeRuntimeConformanceAccessV3::ControlEntrants(
                  lease) == 2,
          "generation-reset contender setup failed");
    const auto contended_reset =
        dt::BlobLifetimeRuntimeConformanceAccessV3::
            ControlResetGeneration(lease, 0, 10);
    Check(!dt::BlobLifetimeRuntimeConformanceAccessV3::
              ControlResetInstalled(contended_reset) &&
              dt::BlobLifetimeRuntimeConformanceAccessV3::
                  ControlResetStatus(contended_reset) == 2 &&
              dt::BlobLifetimeRuntimeConformanceAccessV3::ControlState(lease) ==
                  0,
          "generation reset admitted more than the sole installer entrant");
    // The decisive contended outcome remains immutable after the losing
    // entrant retires. Result selection must not resample entrant count.
    dt::BlobLifetimeRuntimeConformanceAccessV3::ControlLeave(lease);
    const auto delayed_result =
        dt::BlobLifetimeRuntimeConformanceAccessV3::ControlResetFailure(
            lease, contended_reset);
    Check(delayed_result.disposition ==
              dt::BlobLifetimeOuterDispositionV3::public_failure &&
              delayed_result.diagnostic_code ==
                  "CINL.LOB.DESCRIPTOR_INVALID" &&
              delayed_result.reason == "same_ticket_concurrent_use" &&
              delayed_result.fact.reason ==
                  dt::BlobLifetimeReasonV3::same_ticket_concurrent_use &&
              dt::BlobLifetimeRuntimeConformanceAccessV3::ControlEntrants(
                  lease) == 1,
          "generation reset resampled a retired contender");
    const auto installed_reset =
        dt::BlobLifetimeRuntimeConformanceAccessV3::
            ControlResetGeneration(lease, 0, 10);
    Check(dt::BlobLifetimeRuntimeConformanceAccessV3::
              ControlResetInstalled(installed_reset) &&
              dt::BlobLifetimeRuntimeConformanceAccessV3::ControlState(lease) ==
                  10 &&
              dt::BlobLifetimeRuntimeConformanceAccessV3::ControlEntrants(
                  lease) == 1 &&
              !dt::BlobLifetimeRuntimeConformanceAccessV3::ControlEnter(lease),
          "installing transition did not preserve sole entrant and exclude late entry");
    dt::BlobLifetimeRuntimeResultV3 contender{};
    std::thread contender_thread([&] { contender = lease.Probe(); });
    contender_thread.join();
    Check(contender.disposition ==
              dt::BlobLifetimeOuterDispositionV3::terminated_out_of_band &&
              contender.diagnostic_code.empty() && contender.reason.empty() &&
              dt::BlobLifetimeRuntimeConformanceAccessV3::ControlEntrants(
                  lease) == 1 &&
              dt::BlobLifetimeRuntimeConformanceAccessV3::ControlDeferred(
                  lease) == 0,
          "mid-install contender accessed metadata or published a tuple");
    const auto rollback_reset =
        dt::BlobLifetimeRuntimeConformanceAccessV3::
            ControlResetGeneration(lease, 10, 0);
    Check(dt::BlobLifetimeRuntimeConformanceAccessV3::
              ControlResetInstalled(rollback_reset),
          "clean installing rollback failed");
    dt::BlobLifetimeRuntimeConformanceAccessV3::ControlLeave(lease);
  }

  {
    dt::BlobRetainedLifetimeLeaseV3 stale;
    dt::BlobLifetimeRuntimeConformanceAccessV3::SetState(stale, 3);
    Check(dt::BlobLifetimeRuntimeConformanceAccessV3::ControlPublish(
              stale,
              dt::BlobLifetimeInvariantEventV3::same_ticket_concurrent_use,
              15, dt::BlobLifetimePhaseV3::retain, 255) &&
              dt::BlobLifetimeRuntimeConformanceAccessV3::ControlBeginClosing(
                  stale),
          "stale-generation setup did not publish and close");
    const auto tuple =
        dt::BlobLifetimeRuntimeConformanceAccessV3::ControlDeferred(stale);
    Check(dt::BlobLifetimeRuntimeConformanceAccessV3::ControlCompleteReceipt(
              stale, tuple) &&
              dt::BlobLifetimeRuntimeConformanceAccessV3::ControlTryFinalize(
                  stale, 8),
          "stale-generation setup did not receipt and finalize");
    // Test-only state mutation deliberately simulates the forbidden bug: a
    // state-only write that relabels terminal as reusable while retaining the
    // prior generation's accepted/receipt bits.
    dt::BlobLifetimeRuntimeConformanceAccessV3::SetState(stale, 0);
    Check(dt::BlobLifetimeRuntimeConformanceAccessV3::ControlEnter(stale),
          "stale generation did not admit the sole reset entrant");
    const auto corrupt_reset =
        dt::BlobLifetimeRuntimeConformanceAccessV3::
            ControlResetGeneration(stale, 0, 10);
    Check(!dt::BlobLifetimeRuntimeConformanceAccessV3::
              ControlResetInstalled(corrupt_reset) &&
              dt::BlobLifetimeRuntimeConformanceAccessV3::
                  ControlResetStatus(corrupt_reset) == 3 &&
              dt::BlobLifetimeRuntimeConformanceAccessV3::ControlState(stale) ==
                  0,
          "generation reset erased stale winner/receipt corruption");
    dt::BlobLifetimeRuntimeConformanceAccessV3::ControlLeave(stale);
    dt::BlobLifetimeRuntimeConformanceAccessV3::SetState(stale, 8);
  }

  for (const auto final_predecessor :
       {std::uint8_t{7}, std::uint8_t{9}}) {
    dt::BlobRetainedLifetimeLeaseV3 lease;
    Check(dt::BlobLifetimeRuntimeConformanceAccessV3::ControlEnter(lease),
          "prior-generation owner entrant setup failed");
    // Pause the prior owner after its final CAS to released/moved_from but
    // before its metadata clear/transfer and entrant retirement.
    dt::BlobLifetimeRuntimeConformanceAccessV3::SetState(
        lease, final_predecessor);
    bool acquired_during_prior_owner = true;
    std::thread contender([&] {
      acquired_during_prior_owner =
          dt::BlobLifetimeRuntimeConformanceAccessV3::
              ControlEnterReusable(lease);
      if (acquired_during_prior_owner)
        dt::BlobLifetimeRuntimeConformanceAccessV3::ControlLeave(lease);
    });
    contender.join();
    Check(!acquired_during_prior_owner &&
              dt::BlobLifetimeRuntimeConformanceAccessV3::ControlEntrants(
                  lease) == 1,
          "reusable predecessor admitted before prior owner retirement");
    dt::BlobLifetimeRuntimeConformanceAccessV3::ControlLeave(lease);
    Check(dt::BlobLifetimeRuntimeConformanceAccessV3::
              ControlEnterReusable(lease) &&
              dt::BlobLifetimeRuntimeConformanceAccessV3::ControlEntrants(
                  lease) == 1,
          "reusable predecessor was not claimable after owner retirement");
    const auto reset =
        dt::BlobLifetimeRuntimeConformanceAccessV3::ControlResetGeneration(
            lease, final_predecessor, 10);
    Check(dt::BlobLifetimeRuntimeConformanceAccessV3::
              ControlResetInstalled(reset),
          "retired reusable predecessor did not install a new generation");
    const auto rollback =
        dt::BlobLifetimeRuntimeConformanceAccessV3::ControlResetGeneration(
            lease, 10, 0);
    Check(dt::BlobLifetimeRuntimeConformanceAccessV3::
              ControlResetInstalled(rollback),
          "reusable predecessor test rollback failed");
    dt::BlobLifetimeRuntimeConformanceAccessV3::ControlLeave(lease);
  }
}

void PreCloseEntrantQuiescence() {
  Fixture fixture(32);
  const std::array<platform::byte, 3> bytes{{0x31, 0x32, 0x33}};
  auto carrier = dt::BlobLifetimeRuntimeConformanceAccessV3::Carrier(
      fixture.carrier_binding, dt::BlobValueStateV3::value, bytes.data(),
      bytes.size());
  dt::BlobRetainedLifetimeLeaseV3 lease;
  Check(dt::RetainBaseBlobLifetimeLeaseV3Generation1(
            fixture.profile, fixture.Admission(), carrier, fixture.token,
            fixture.request, lease).ok(),
        "pre-close entrant fixture retain failed");
  const auto descriptor_before =
      dt::BlobLifetimeRuntimeConformanceAccessV3::DescriptorUuid(lease);
  const auto descriptor_generation_before =
      dt::BlobLifetimeRuntimeConformanceAccessV3::DescriptorGeneration(lease);
  const auto ticket_before =
      dt::BlobLifetimeRuntimeConformanceAccessV3::RetainTicketUuid(lease);
  Check(dt::BlobLifetimeRuntimeConformanceAccessV3::ControlEnter(lease),
        "pre-close owner entrant setup failed");
  Check(dt::BlobLifetimeRuntimeConformanceAccessV3::ControlEnter(lease),
        "pre-close losing entrant setup failed");
  dt::BlobLifetimeRuntimeConformanceAccessV3::SetState(lease, 3);
  Check(dt::BlobLifetimeRuntimeConformanceAccessV3::ControlPublish(
            lease,
            dt::BlobLifetimeInvariantEventV3::same_ticket_concurrent_use,
            fixture.request.operation,
            dt::BlobLifetimePhaseV3::protected_access, 255),
        "pre-close invariant tuple setup failed");
  Check(dt::BlobLifetimeRuntimeConformanceAccessV3::ControlBeginClosing(lease),
        "pre-close closing setup failed");
  std::atomic<bool> report_started{false};
  dt::BlobLifetimeRuntimeResultV3 reported{};
  std::thread owner([&] {
    report_started.store(true, std::memory_order_release);
    reported = dt::BlobLifetimeRuntimeConformanceAccessV3::
        ControlReportInvariant(
            lease,
            static_cast<std::uint8_t>(
                dt::BlobLifetimeInvariantEventV3::
                    same_ticket_concurrent_use),
            fixture.request.operation,
            static_cast<std::uint8_t>(
                dt::BlobLifetimePhaseV3::protected_access),
            255,
            1);
  });
  while (!report_started.load(std::memory_order_acquire))
    std::this_thread::yield();
  // ReportInvariant is spinning on the packed entrant count. The admitted
  // loser still owns its metadata pin, so no local cleanup or service receipt
  // may be observable.
  Check(fixture.fake.invariant_calls == 0 &&
            dt::BlobLifetimeRuntimeConformanceAccessV3::DescriptorUuid(lease) ==
                descriptor_before &&
            dt::BlobLifetimeRuntimeConformanceAccessV3::DescriptorGeneration(
                lease) == descriptor_generation_before &&
            dt::BlobLifetimeRuntimeConformanceAccessV3::CarrierLength(lease) ==
                bytes.size() &&
            dt::BlobLifetimeRuntimeConformanceAccessV3::RetainTicketEquals(
                lease, ticket_before),
        "invariant cleanup began before the admitted entrant retired");
  dt::BlobLifetimeRuntimeConformanceAccessV3::ControlLeave(lease);
  owner.join();
  const auto pending =
      dt::BlobLifetimeRuntimeConformanceAccessV3::ControlPeekWhileClosing(
          lease);
  Check(!reported.ok() &&
            reported.reason == "same_ticket_concurrent_use" &&
            fixture.fake.invariant_calls == 1 && pending != 0 &&
            dt::BlobLifetimeRuntimeConformanceAccessV3::DescriptorGeneration(
                lease) == 0 &&
            dt::BlobLifetimeRuntimeConformanceAccessV3::CarrierLength(lease) ==
                0 &&
            dt::BlobLifetimeRuntimeConformanceAccessV3::RetainTicketEquals(
                lease, BlobUuid16V3{}),
        "invariant cleanup did not follow entrant retirement exactly");
  Check(dt::BlobLifetimeRuntimeConformanceAccessV3::ControlCompleteReceipt(
            lease, pending) &&
            dt::BlobLifetimeRuntimeConformanceAccessV3::ControlTryFinalize(
                lease, 8, 1),
        "pre-close entrant fixture could not receipt/finalize");
  dt::BlobLifetimeRuntimeConformanceAccessV3::ControlLeave(lease);
}

void CloneSourceWinnerCleansRetainedDestination() {
  Fixture fixture(64);
  const std::array<platform::byte, 2> bytes{{0x71, 0x72}};
  auto carrier = dt::BlobLifetimeRuntimeConformanceAccessV3::Carrier(
      fixture.carrier_binding, dt::BlobValueStateV3::value, bytes.data(),
      bytes.size());
  dt::BlobRetainedLifetimeLeaseV3 source;
  dt::BlobRetainedLifetimeLeaseV3 destination;
  Check(dt::RetainBaseBlobLifetimeLeaseV3Generation1(
            fixture.profile, fixture.Admission(), carrier, fixture.token,
            fixture.request, source).ok(),
        "clone source-winner fixture retain failed");
  // Clone's destination retain samples before_retain then after_retain. Pause
  // in the latter after the retain callback returned a valid ticket.
  fixture.fake.block_clock_call = fixture.fake.clock_calls + 2;
  dt::BlobLifetimeRuntimeResultV3 clone_result{};
  std::thread clone([&] { clone_result = source.CloneInto(destination); });
  while (!fixture.fake.clock_entered.load(std::memory_order_acquire))
    std::this_thread::yield();
  dt::BlobRetainedLifetimeLeaseV3 other_destination;
  const auto overlapping = source.CloneInto(other_destination);
  Check(!overlapping.ok() &&
            overlapping.reason == "same_ticket_concurrent_use" &&
            dt::BlobLifetimeRuntimeConformanceAccessV3::ControlDeferred(
                source) != 0 && !other_destination.retained(),
        "actual overlapping Clone did not publish a source invariant");
  fixture.fake.finish_clock.store(true, std::memory_order_release);
  clone.join();
  Check(!clone_result.ok() &&
            clone_result.reason == "same_ticket_concurrent_use" &&
            CountCall(fixture.fake, 'R') == 2 &&
            CountCall(fixture.fake, 'L') == 2 &&
            fixture.fake.pin_drop_calls == 4 && fixture.fake.pins == 1 &&
            fixture.fake.invariant_calls == 1 &&
            fixture.fake.last_invariant_operation == 15 &&
            fixture.fake.last_invariant_phase ==
                dt::BlobLifetimePhaseV3::retain &&
            !destination.retained() && !source.retained() &&
            dt::BlobLifetimeRuntimeConformanceAccessV3::ControlState(
                destination) == 7 &&
            dt::BlobLifetimeRuntimeConformanceAccessV3::ControlState(source) ==
                8 &&
            dt::BlobLifetimeRuntimeConformanceAccessV3::ControlDeferred(
                source) == 0,
        "clone source winner did not release destination and retire pins");
}

void TransferAdmissionReservation() {
  using Access = dt::BlobLifetimeRuntimeConformanceAccessV3;
  for (const unsigned entrants : {1U, 2U, 8U}) {
    dt::BlobRetainedLifetimeLeaseV3 lease;
    Access::SetState(lease, 2);
    for (unsigned i = 0; i != entrants; ++i)
      Check(Access::ControlEnter(lease), "transfer reservation entrant");
    const auto won = Access::ControlClaimTransfer(lease);
    Check(Access::ControlResetInstalled(won) &&
              Access::ControlState(lease) == 5 &&
              Access::ControlEntrants(lease) == entrants,
          "transfer reservation did not preserve all admitted entrants");
    const auto lost = Access::ControlClaimTransfer(lease);
    Check(Access::ControlResetStatus(lost) == 1 &&
              Access::ControlState(lease) == 5,
          "second transfer owner was admitted");
    Check(Access::ControlPublish(
              lease, dt::BlobLifetimeInvariantEventV3::same_ticket_concurrent_use,
              16, dt::BlobLifetimePhaseV3::move_construct, 255),
          "reservation loser could not publish against active owner");
    const auto tuple = Access::ControlDeferred(lease);
    Check(tuple != 0 && Access::ControlBeginClosing(lease),
          "reservation owner lost overlap evidence");
    for (unsigned i = 1; i != entrants; ++i) Access::ControlLeave(lease);
    Check(!Access::ControlTryFinalize(lease, 2, 1) &&
              Access::ControlCompleteReceipt(lease, tuple) &&
              Access::ControlTryFinalize(lease, 8, 1),
          "reservation finalized without winner receipt");
    Access::ControlLeave(lease);
  }
  {
    dt::BlobRetainedLifetimeLeaseV3 lease;
    Access::SetState(lease, 2);
    const auto missing = Access::ControlClaimTransfer(lease);
    Check(Access::ControlResetStatus(missing) == 2 &&
              Access::ControlState(lease) == 2,
          "transfer reservation accepted missing entrant");
    Access::SetState(lease, 0);
  }
}

// Test-only debugger rendezvous: no production hooks or timing sleeps. Run
// this mode only through probes/transfer_admission_gdb.py, which holds the
// pair owner between its two real CAS claims while the second owner starts.
struct RetainPublicationDebug {
  Fixture* fixture = nullptr;
  dt::BlobRetainedLifetimeLeaseV3* lease = nullptr;
  bool probe = false;
  dt::BlobLifetimeRuntimeResultV3 owner_result;
  dt::BlobLifetimeRuntimeResultV3 contender_result;
};
RetainPublicationDebug* g_retain_debug = nullptr;

[[gnu::noinline]] void RetainContenderReturned() {
  std::atomic_signal_fence(std::memory_order_seq_cst);
}
[[gnu::noinline]] void RetainPublicationOwner() {
  auto& d = *g_retain_debug;
  if (d.probe) d.owner_result = d.lease->Probe();
  else {
    auto carrier = dt::BlobLifetimeRuntimeConformanceAccessV3::Carrier(
        d.fixture->carrier_binding, dt::BlobValueStateV3::value, nullptr, 0);
    d.owner_result = dt::RetainBaseBlobLifetimeLeaseV3Generation1(
        d.fixture->profile, d.fixture->Admission(), carrier, d.fixture->token,
        d.fixture->request, *d.lease);
  }
}
[[gnu::noinline]] void RetainPublicationContender() {
  auto& d = *g_retain_debug;
  auto carrier = dt::BlobLifetimeRuntimeConformanceAccessV3::Carrier(
      d.fixture->carrier_binding, dt::BlobValueStateV3::value, nullptr, 0);
  d.contender_result = dt::RetainBaseBlobLifetimeLeaseV3Generation1(
      d.fixture->profile, d.fixture->Admission(), carrier, d.fixture->token,
      d.fixture->request, *d.lease);
  RetainContenderReturned();
}

void RetainPublicationDebuggerCases() {
  using Access = dt::BlobLifetimeRuntimeConformanceAccessV3;
  for (bool probe : {false, true}) {
    Fixture fixture(64);
    dt::BlobRetainedLifetimeLeaseV3 lease;
    if (probe) {
      auto carrier = Access::Carrier(fixture.carrier_binding,
                                    dt::BlobValueStateV3::value, nullptr, 0);
      Check(dt::RetainBaseBlobLifetimeLeaseV3Generation1(
                fixture.profile, fixture.Admission(), carrier, fixture.token,
                fixture.request, lease).ok(), "retain-publication probe setup");
    }
    RetainPublicationDebug debug{&fixture, &lease, probe, {}, {}};
    g_retain_debug = &debug;
    fixture.fake.block_clock_call = fixture.fake.clock_calls + 2;
    std::thread owner(RetainPublicationOwner);
    while (!fixture.fake.clock_entered.load(std::memory_order_acquire))
      std::this_thread::yield();
    std::thread contender(RetainPublicationContender);
    // GDB holds the contender's real entrant at LocalStateFailure, resumes the
    // owner's post-callback clock, and releases that entrant only after close.
    owner.join();
    contender.join();
    Check(debug.owner_result.ok() && lease.retained() &&
              Access::ControlState(lease) == 2 && Access::ControlEntrants(lease) == 0,
          "successful retain/probe silently released its ticket");
    Check(debug.contender_result.reason == "destination_armed" &&
              fixture.fake.invariant_calls == 0 &&
              Access::ControlDeferred(lease) == 0 && CountCall(fixture.fake, 'L') == 0,
          "metadata-only contender manufactured cleanup/invariant");
    Check(lease.Release().ok() && CountCall(fixture.fake, 'L') == 1 &&
              fixture.fake.pins == 1,
          "retained ticket or pins not conserved through final release");
    g_retain_debug = nullptr;
  }
}

struct PairDebugState {
  dt::BlobRetainedLifetimeLeaseV3* first = nullptr;
  dt::BlobRetainedLifetimeLeaseV3* second = nullptr;
  FakeReceiver* second_fake = nullptr;
  std::atomic<bool> probe_ready{false};
  std::atomic<bool> start_probe{false};
  dt::BlobLifetimeRuntimeResultV3 pair_result;
  dt::BlobLifetimeRuntimeResultV3 probe_result;
};
PairDebugState* g_pair_debug = nullptr;

[[gnu::noinline]] void PairOwnerReturned() {
  std::atomic_signal_fence(std::memory_order_seq_cst);
}
[[gnu::noinline]] void PairProbeReturned() {
  std::atomic_signal_fence(std::memory_order_seq_cst);
}
[[gnu::noinline]] void PairProbeWorker() {
  g_pair_debug->probe_ready.store(true, std::memory_order_release);
  while (!g_pair_debug->start_probe.load(std::memory_order_acquire))
    std::this_thread::yield();
  g_pair_debug->probe_result = g_pair_debug->second->Probe();
  PairProbeReturned();
}

void ActiveSecondClaimDebuggerCases() {
  using Access = dt::BlobLifetimeRuntimeConformanceAccessV3;
  for (const bool clone : {false, true}) {
    for (const bool source_first : {false, true}) {
      Fixture source_fixture(64);
      Fixture destination_fixture(64);
      destination_fixture.token.lifetime_token_uuid = BlobUuid(0xb0);
      destination_fixture.carrier_binding.lifetime_token_uuid =
          PlatformUuid(0xb0);
      std::array<dt::BlobRetainedLifetimeLeaseV3, 2> leases;
      auto& source = leases[source_first ? 0 : 1];
      auto& destination = leases[source_first ? 1 : 0];
      const auto retain = [&](Fixture& fixture, auto& lease) {
        auto carrier = Access::Carrier(fixture.carrier_binding,
                                      dt::BlobValueStateV3::value, nullptr, 0);
        Check(dt::RetainBaseBlobLifetimeLeaseV3Generation1(
                  fixture.profile, fixture.Admission(), carrier, fixture.token,
                  fixture.request, lease).ok(), "active-second retain");
      };
      retain(source_fixture, source);
      if (!clone || source_first) retain(destination_fixture, destination);
      PairDebugState debug;
      debug.first = &leases[0];
      debug.second = &leases[1];
      debug.second_fake = source_first ? &destination_fixture.fake
                                       : &source_fixture.fake;
      debug.second_fake->block_clock_call = debug.second_fake->clock_calls + 1;
      g_pair_debug = &debug;
      std::thread probe(PairProbeWorker);
      while (!debug.probe_ready.load(std::memory_order_acquire))
        std::this_thread::yield();
      std::thread pair([&] {
        debug.pair_result = clone ? source.CloneInto(destination)
            : destination.MoveReplaceFrom(std::move(source));
        Check(debug.pair_result.reason == "same_ticket_concurrent_use" &&
                  Access::ControlState(*debug.first) ==
                      (clone && !source_first ? 0 : 2) &&
                  Access::ControlEntrants(*debug.first) == 0 &&
                  Access::ControlEntrants(*debug.second) == 1 &&
                  Access::ControlDeferred(*debug.first) == 0 &&
                  Access::ControlDeferred(*debug.second) != 0 &&
                  source_fixture.fake.invariant_calls == 0 &&
                  destination_fixture.fake.invariant_calls == 0,
              "active-second failure did not retire foreign entrant before rollback");
        PairOwnerReturned();
      });
      pair.join();
      probe.join();
      Check(debug.probe_result.reason == "same_ticket_concurrent_use" &&
                Access::ControlState(*debug.second) == 8 &&
                Access::ControlEntrants(*debug.second) == 0 &&
                Access::ControlDeferred(*debug.second) == 0 &&
                debug.second_fake->invariant_calls == 1 &&
                debug.second_fake->last_invariant_operation == (clone ? 15 : 17) &&
                debug.second_fake->last_invariant_phase ==
                    (clone ? dt::BlobLifetimePhaseV3::retain
                           : dt::BlobLifetimePhaseV3::move_replace) &&
                debug.second_fake->pin_drop_calls == 2,
            "active-second owner lost its receipt or cleanup");
      if (debug.first->retained())
        Check(debug.first->Release().ok(), "active-second first owner cleanup");
      g_pair_debug = nullptr;
    }
  }
}

void TransferPairAdmissionRollback() {
  using Access = dt::BlobLifetimeRuntimeConformanceAccessV3;
  for (const bool source_first : {false, true}) {
    for (const bool clone : {false, true}) {
      Fixture fixture(64);
      auto carrier = Access::Carrier(fixture.carrier_binding,
                                    dt::BlobValueStateV3::value, nullptr, 0);
      std::array<dt::BlobRetainedLifetimeLeaseV3, 2> leases;
      auto& source = leases[source_first ? 0 : 1];
      auto& destination = leases[source_first ? 1 : 0];
      Check(dt::RetainBaseBlobLifetimeLeaseV3Generation1(
                fixture.profile, fixture.Admission(), carrier, fixture.token,
                fixture.request, source).ok(),
            "pair rollback source retain");
      // A pre-admitted unbound entrant prevents sole-installer reset. In the
      // source-first order the source claim must be rolled back; in the other
      // order it must never have been claimed. Neither case has a destination
      // binding against which an invariant receipt could legitimately exist.
      Check(Access::ControlEnter(destination), "pair rollback destination entrant");
      const auto result = clone ? source.CloneInto(destination)
          : destination.MoveReplaceFrom(std::move(source));
      Check(!result.ok() && result.reason == "same_ticket_concurrent_use" &&
                source.retained() && Access::ControlState(destination) == 0 &&
                Access::ControlEntrants(source) == 0 &&
                Access::ControlEntrants(destination) == 1 &&
                Access::ControlDeferred(source) == 0 &&
                Access::ControlDeferred(destination) == 0 &&
                fixture.fake.invariant_calls == 0,
            "pair installer contention lost ownership or invented a binding");
      Access::ControlLeave(destination);
      const auto retry = clone ? source.CloneInto(destination)
          : destination.MoveReplaceFrom(std::move(source));
      Check(retry.ok() && destination.retained(),
            "pair rollback did not permit clean retry");
      if (clone) Check(source.Release().ok(), "pair retry source release");
      Check(destination.Release().ok() &&
                fixture.fake.pin_drop_calls == (clone ? 4 : 2),
            "pair rollback/retry leaked pins");
    }
  }
  // A second entrant admitted while the source was idle must survive the
  // transfer reservation and be able to publish to that active owner.
  {
    Fixture fixture(64);
    auto carrier = Access::Carrier(fixture.carrier_binding,
                                  dt::BlobValueStateV3::value, nullptr, 0);
    dt::BlobRetainedLifetimeLeaseV3 source;
    Check(dt::RetainBaseBlobLifetimeLeaseV3Generation1(
              fixture.profile, fixture.Admission(), carrier, fixture.token,
              fixture.request, source).ok(),
          "preadmitted move source retain");
    Check(Access::ControlEnter(source), "preadmitted move source entrant");
    dt::BlobLifetimeRuntimeResultV3 moved;
    std::thread owner([&] {
      auto constructed = dt::BlobRetainedLifetimeLeaseV3::MoveConstructFrom(
          std::move(source));
      moved = constructed.result;
    });
    while (!Access::ControlClosing(source)) std::this_thread::yield();
    Check(Access::ControlState(source) == 5 &&
              Access::ControlEntrants(source) == 2 &&
              Access::ControlPublish(
                  source,
                  dt::BlobLifetimeInvariantEventV3::same_ticket_concurrent_use,
                  16, dt::BlobLifetimePhaseV3::move_construct, 255),
          "preadmitted move loser could not publish to reserved owner");
    Access::ControlLeave(source);
    owner.join();
    Check(!moved.ok() && Access::ControlState(source) == 8 &&
              Access::ControlEntrants(source) == 0 &&
              Access::ControlDeferred(source) == 0 &&
              fixture.fake.invariant_calls == 1 &&
              fixture.fake.pin_drop_calls == 2,
          "preadmitted move winner did not service tuple and retire pins");
  }
}

void TransferPublicationOrdering() {
  using Access = dt::BlobLifetimeRuntimeConformanceAccessV3;
  // A source entrant pins Clone immediately before its final publication.
  // The destination must remain provisional too; publishing it idle while
  // Clone still owns provisional transfer states recreates the lost-overlap gap.
  {
    Fixture fixture(64);
    auto carrier = Access::Carrier(fixture.carrier_binding,
                                  dt::BlobValueStateV3::value, nullptr, 0);
    dt::BlobRetainedLifetimeLeaseV3 source;
    dt::BlobRetainedLifetimeLeaseV3 destination;
    Check(dt::RetainBaseBlobLifetimeLeaseV3Generation1(
              fixture.profile, fixture.Admission(), carrier, fixture.token,
              fixture.request, source).ok(),
          "transfer publication source retain");
    fixture.fake.block_clock_call = fixture.fake.clock_calls + 2;
    dt::BlobLifetimeRuntimeResultV3 cloned;
    std::thread worker([&] { cloned = source.CloneInto(destination); });
    while (!fixture.fake.clock_entered.load(std::memory_order_acquire))
      std::this_thread::yield();
    Check(Access::ControlEnter(source), "transfer barrier entrant");
    fixture.fake.finish_clock.store(true, std::memory_order_release);
    while (!Access::ControlClosing(source)) std::this_thread::yield();
    Check(!source.retained() && !destination.retained() &&
              Access::ControlClosing(destination) &&
              Access::ControlState(source) == 5 &&
              Access::ControlState(destination) == 1,
          "Clone exposed idle while transfer ownership remained provisional");
    Check(!Access::ControlEnter(destination),
          "Clone admitted a destination entrant before paired publication");
    Access::ControlLeave(source);
    worker.join();
    Check(cloned.ok() && source.retained() && destination.retained() &&
              !Access::ControlClosing(source) &&
              !Access::ControlClosing(destination) &&
              Access::ControlEntrants(source) == 0 &&
              Access::ControlEntrants(destination) == 0,
          "Clone publication did not retire closing bits and entrants");
    Check(source.Release().ok() && destination.Release().ok() &&
              fixture.fake.pin_drop_calls == 4 &&
              fixture.fake.invariant_calls == 0,
          "Clone paired publication leaked obligations");
  }
  // Exercise actual MoveReplace publication in both operand address orders,
  // for empty, armed and reusable destinations. This also supplies debugger
  // breakpoint coverage of the exact final CAS without production hooks.
  for (const bool source_first : {false, true}) {
    for (const unsigned predecessor : {0U, 1U, 2U}) {
      Fixture fixture(64);
      auto carrier = Access::Carrier(fixture.carrier_binding,
                                    dt::BlobValueStateV3::value, nullptr, 0);
      std::array<dt::BlobRetainedLifetimeLeaseV3, 2> leases;
      auto& source = leases[source_first ? 0 : 1];
      auto& destination = leases[source_first ? 1 : 0];
      Check(dt::RetainBaseBlobLifetimeLeaseV3Generation1(
                fixture.profile, fixture.Admission(), carrier, fixture.token,
                fixture.request, source).ok(),
            "MoveReplace publication source retain");
      if (predecessor != 0) {
        Check(source.CloneInto(destination).ok(),
              "MoveReplace publication destination retain");
        if (predecessor == 2)
          Check(destination.Release().ok(),
                "MoveReplace publication reusable destination");
      }
      Check(destination.MoveReplaceFrom(std::move(source)).ok() &&
                destination.retained() && !source.retained() &&
                !Access::ControlClosing(source) &&
                !Access::ControlClosing(destination) &&
                Access::ControlEntrants(source) == 0 &&
                Access::ControlEntrants(destination) == 0,
            "MoveReplace publication retained closing bits or owner entrants");
      Check(destination.Release().ok() &&
                fixture.fake.pin_drop_calls == (predecessor == 0 ? 2 : 4) &&
                fixture.fake.invariant_calls == 0,
            "MoveReplace publication leaked obligations");
    }
  }
}

void PostCallbackGateAndServiceReceiptBarrier() {
  Fixture fixture(32);
  const std::array<platform::byte, 1> bytes{{0x42}};
  auto carrier = dt::BlobLifetimeRuntimeConformanceAccessV3::Carrier(
      fixture.carrier_binding, dt::BlobValueStateV3::value, bytes.data(),
      bytes.size());
  dt::BlobRetainedLifetimeLeaseV3 lease;
  Check(dt::RetainBaseBlobLifetimeLeaseV3Generation1(
            fixture.profile, fixture.Admission(), carrier, fixture.token,
            fixture.request, lease).ok(),
        "barrier setup retain failed");
  fixture.fake.block_clock_call = fixture.fake.clock_calls + 2;
  fixture.fake.block_invariant.store(true, std::memory_order_release);
  dt::BlobLifetimeRuntimeResultV3 owner_result{};
  std::thread owner([&] { owner_result = lease.Probe(); });
  while (!fixture.fake.clock_entered.load(std::memory_order_acquire))
    std::this_thread::yield();
  Check(dt::BlobLifetimeRuntimeConformanceAccessV3::ControlState(lease) == 3 &&
            !dt::BlobLifetimeRuntimeConformanceAccessV3::ControlClosing(lease) &&
            dt::BlobLifetimeRuntimeConformanceAccessV3::ControlDeferred(
                lease) == 0,
        "post-callback gate did not retain the active owner state");
  dt::BlobRetainedLifetimeLeaseV3 rejected_destination;
  const auto loser = lease.CloneInto(rejected_destination);
  const auto winner =
      dt::BlobLifetimeRuntimeConformanceAccessV3::ControlDeferred(lease);
  Check(!loser.ok() && loser.reason == "same_ticket_concurrent_use" &&
            winner != 0 && !rejected_destination.retained(),
        "post-callback gate loser did not publish one exact tuple");
  fixture.fake.finish_clock.store(true, std::memory_order_release);
  while (!fixture.fake.invariant_entered.load(std::memory_order_acquire))
    std::this_thread::yield();
  Check(dt::BlobLifetimeRuntimeConformanceAccessV3::ControlState(lease) == 6 &&
            dt::BlobLifetimeRuntimeConformanceAccessV3::ControlClosing(lease) &&
            dt::BlobLifetimeRuntimeConformanceAccessV3::
                ControlPeekWhileClosing(lease) == winner &&
            fixture.fake.pin_drop_calls == 0,
        "service window published final state or dropped pins before receipt");
  dt::BlobRetainedLifetimeLeaseV3 second_destination;
  const auto late_loser = lease.CloneInto(second_destination);
  Check(!late_loser.ok() &&
            late_loser.disposition ==
                dt::BlobLifetimeOuterDispositionV3::terminated_out_of_band &&
            late_loser.diagnostic_code.empty() && late_loser.reason.empty() &&
            dt::BlobLifetimeRuntimeConformanceAccessV3::
                ControlPeekWhileClosing(lease) == winner,
        "close-winning service window admitted metadata or replaced tuple");
  fixture.fake.finish_invariant.store(true, std::memory_order_release);
  owner.join();
  Check(!owner_result.ok() &&
            owner_result.reason == "same_ticket_concurrent_use" &&
            fixture.fake.invariant_calls == 1 &&
            fixture.fake.invariant_accepted_receipts == 1 &&
            fixture.fake.invariant_already_receipts == 0 &&
            dt::BlobLifetimeRuntimeConformanceAccessV3::ControlState(lease) ==
                8 &&
            dt::BlobLifetimeRuntimeConformanceAccessV3::ControlDeferred(
                lease) == 0 && fixture.fake.pin_drop_calls == 2,
        "service receipt did not precede final state and pin retirement");
}

void SameBindingIndependentReceipts() {
  Fixture fixture(64);
  fixture.fake.idempotent_invariant_service = true;
  const std::array<platform::byte, 1> bytes{{0x42}};
  auto carrier = dt::BlobLifetimeRuntimeConformanceAccessV3::Carrier(
      fixture.carrier_binding, dt::BlobValueStateV3::value, bytes.data(),
      bytes.size());
  dt::BlobRetainedLifetimeLeaseV3 first;
  dt::BlobRetainedLifetimeLeaseV3 second;
  Check(dt::RetainBaseBlobLifetimeLeaseV3Generation1(
            fixture.profile, fixture.Admission(), carrier, fixture.token,
            fixture.request, first).ok() && first.CloneInto(second).ok(),
        "same-binding receipt setup failed");
  BlockingVisitContext first_block;
  BlockingVisitContext second_block;
  auto first_visitor = dt::BlobLifetimeRuntimeConformanceAccessV3::Visitor(
      BlockingVisitor, &first_block);
  auto second_visitor = dt::BlobLifetimeRuntimeConformanceAccessV3::Visitor(
      BlockingVisitor, &second_block);
  dt::BlobLifetimeRuntimeResultV3 first_owner{};
  dt::BlobLifetimeRuntimeResultV3 second_owner{};
  std::thread first_thread([&] {
    first_owner = first.VisitBoundMaterializedBytes(first_visitor);
  });
  std::thread second_thread([&] {
    second_owner = second.VisitBoundMaterializedBytes(second_visitor);
  });
  while (!first_block.entered.load(std::memory_order_acquire) ||
         !second_block.entered.load(std::memory_order_acquire))
    std::this_thread::yield();
  dt::BlobRetainedLifetimeLeaseV3 first_rejected;
  dt::BlobRetainedLifetimeLeaseV3 second_rejected;
  const auto first_loser = first.CloneInto(first_rejected);
  const auto second_loser = second.CloneInto(second_rejected);
  Check(!first_loser.ok() && !second_loser.ok() &&
            first_loser.reason == "same_ticket_concurrent_use" &&
            second_loser.reason == "same_ticket_concurrent_use",
        "same-binding wrappers did not each retain their own winner tuple");
  first_block.finish.store(true, std::memory_order_release);
  second_block.finish.store(true, std::memory_order_release);
  first_thread.join();
  second_thread.join();
  Check(!first_owner.ok() && !second_owner.ok() &&
            fixture.fake.invariant_calls == 2 &&
            fixture.fake.invariant_accepted_receipts == 1 &&
            fixture.fake.invariant_already_receipts == 1 &&
            fixture.fake.invariant_effects == 1,
        "same binding did not produce two calls and accepted/already receipts");
  Check(dt::BlobLifetimeRuntimeConformanceAccessV3::ControlState(first) == 8 &&
            dt::BlobLifetimeRuntimeConformanceAccessV3::ControlState(second) ==
                8 &&
            dt::BlobLifetimeRuntimeConformanceAccessV3::ControlDeferred(first) ==
                0 &&
            dt::BlobLifetimeRuntimeConformanceAccessV3::ControlDeferred(second) ==
                0,
        "same-binding wrappers did not independently receipt and finalize");
}

void PublicationSuppressionIsOperationLocal() {
  Fixture fixture(32);
  const std::array<platform::byte, 1> bytes{{0x42}};
  auto carrier = dt::BlobLifetimeRuntimeConformanceAccessV3::Carrier(
      fixture.carrier_binding, dt::BlobValueStateV3::value, bytes.data(),
      bytes.size());
  dt::BlobRetainedLifetimeLeaseV3 source;
  Check(dt::RetainBaseBlobLifetimeLeaseV3Generation1(
            fixture.profile, fixture.Admission(), carrier, fixture.token,
            fixture.request, source).ok(),
        "publication suppression setup retain failed");
  dt::BlobLifetimeRuntimeConformanceAccessV3::SetPublicationSuppressed(
      source, true);
  Check(source.Probe().ok() &&
            !dt::BlobLifetimeRuntimeConformanceAccessV3::
                PublicationSuppressed(source),
        "stale publication suppression escaped into a later probe");
  dt::BlobLifetimeRuntimeConformanceAccessV3::SetPublicationSuppressed(
      source, true);
  auto moved = dt::BlobRetainedLifetimeLeaseV3::MoveConstructFrom(
      std::move(source));
  Check(moved.result.ok() && moved.lease() != nullptr &&
            !dt::BlobLifetimeRuntimeConformanceAccessV3::
                PublicationSuppressed(*moved.lease()) &&
            !dt::BlobLifetimeRuntimeConformanceAccessV3::
                PublicationSuppressed(source),
        "operation-local publication suppression transferred with ownership");
  Check(moved.lease()->Release().ok(),
        "publication suppression move cleanup failed");
}

void CleanBindingReuse() {
  {
    Fixture fixture(64);
    const std::array<platform::byte, 1> bytes{{0x42}};
    auto carrier = dt::BlobLifetimeRuntimeConformanceAccessV3::Carrier(
        fixture.carrier_binding, dt::BlobValueStateV3::value, bytes.data(),
        bytes.size());
    dt::BlobRetainedLifetimeLeaseV3 lease;
    fixture.fake.now = 1000;
    fixture.fake.retain_code = SB_BLOB_CALLBACK_SECURITY_DENIED_V3;
    const auto first = dt::RetainBaseBlobLifetimeLeaseV3Generation1(
        fixture.profile, fixture.Admission(), carrier, fixture.token,
        fixture.request, lease);
    Check(!first.ok() &&
              first.fact.fact_class == dt::BlobLifetimeFactClassV3::security &&
              dt::BlobLifetimeRuntimeConformanceAccessV3::ControlState(lease) ==
                  0,
          "clean failed retain did not return a reusable disarmed wrapper");
    fixture.fake.retain_code = SB_BLOB_CALLBACK_OK_V3;
    fixture.fake.now = 10;
    const auto second = dt::RetainBaseBlobLifetimeLeaseV3Generation1(
        fixture.profile, fixture.Admission(), carrier, fixture.token,
        fixture.request, lease);
    Check(second.ok() && lease.retained() &&
              fixture.fake.invariant_calls == 0 &&
              !dt::BlobLifetimeRuntimeConformanceAccessV3::Quarantined(lease) &&
              !dt::BlobLifetimeRuntimeConformanceAccessV3::
                  PublicationSuppressed(lease),
          "new retain inherited prior binding monotonic/quarantine/gate state");
    BlockingVisitContext block;
    auto visitor = dt::BlobLifetimeRuntimeConformanceAccessV3::Visitor(
        BlockingVisitor, &block);
    dt::BlobLifetimeRuntimeResultV3 owner_result{};
    std::thread owner([&] {
      owner_result = lease.VisitBoundMaterializedBytes(visitor);
    });
    while (!block.entered.load(std::memory_order_acquire))
      std::this_thread::yield();
    dt::BlobRetainedLifetimeLeaseV3 rejected;
    const auto loser = lease.CloneInto(rejected);
    block.finish.store(true, std::memory_order_release);
    owner.join();
    Check(!loser.ok() && !owner_result.ok() &&
              fixture.fake.invariant_calls == 1 &&
              fixture.fake.invariant_accepted_receipts == 1 &&
              dt::BlobLifetimeRuntimeConformanceAccessV3::ControlState(lease) ==
                  8,
          "reused clean wrapper did not accept and service a new winner tuple");
  }

  {
    Fixture fixture(96);
    const std::array<platform::byte, 1> bytes{{0x42}};
    auto carrier = dt::BlobLifetimeRuntimeConformanceAccessV3::Carrier(
        fixture.carrier_binding, dt::BlobValueStateV3::value, bytes.data(),
        bytes.size());
    dt::BlobRetainedLifetimeLeaseV3 source;
    dt::BlobRetainedLifetimeLeaseV3 destination;
    Check(dt::RetainBaseBlobLifetimeLeaseV3Generation1(
              fixture.profile, fixture.Admission(), carrier, fixture.token,
              fixture.request, source).ok() &&
              dt::RetainBaseBlobLifetimeLeaseV3Generation1(
                  fixture.profile, fixture.Admission(), carrier, fixture.token,
                  fixture.request, destination).ok(),
          "released-destination reuse setup failed");
    const auto replaced = destination.MoveReplaceFrom(std::move(source));
    Check(replaced.ok() && destination.retained() &&
              dt::BlobLifetimeRuntimeConformanceAccessV3::ControlState(source) ==
                  9,
          "move-replace did not reuse a clean released destination");
    BlockingVisitContext block;
    auto visitor = dt::BlobLifetimeRuntimeConformanceAccessV3::Visitor(
        BlockingVisitor, &block);
    dt::BlobLifetimeRuntimeResultV3 owner_result{};
    std::thread owner([&] {
      owner_result = destination.VisitBoundMaterializedBytes(visitor);
    });
    while (!block.entered.load(std::memory_order_acquire))
      std::this_thread::yield();
    dt::BlobRetainedLifetimeLeaseV3 rejected;
    const auto loser = destination.CloneInto(rejected);
    block.finish.store(true, std::memory_order_release);
    owner.join();
    Check(!loser.ok() && !owner_result.ok() &&
              fixture.fake.invariant_calls == 1 &&
              dt::BlobLifetimeRuntimeConformanceAccessV3::ControlState(
                  destination) == 8,
          "adopted released destination did not accept a new winner tuple");
  }

  {
    Fixture fixture(96);
    const std::array<platform::byte, 1> bytes{{0x42}};
    auto carrier = dt::BlobLifetimeRuntimeConformanceAccessV3::Carrier(
        fixture.carrier_binding, dt::BlobValueStateV3::value, bytes.data(),
        bytes.size());
    dt::BlobRetainedLifetimeLeaseV3 former_source;
    Check(dt::RetainBaseBlobLifetimeLeaseV3Generation1(
              fixture.profile, fixture.Admission(), carrier, fixture.token,
              fixture.request, former_source).ok(),
          "moved-from destination reuse setup retain failed");
    auto first_move = dt::BlobRetainedLifetimeLeaseV3::MoveConstructFrom(
        std::move(former_source));
    Check(first_move.result.ok() && first_move.lease() != nullptr &&
              dt::BlobLifetimeRuntimeConformanceAccessV3::ControlState(
                  former_source) == 9,
          "moved-from destination reuse setup move failed");
    dt::BlobRetainedLifetimeLeaseV3 incoming;
    Check(dt::RetainBaseBlobLifetimeLeaseV3Generation1(
              fixture.profile, fixture.Admission(), carrier, fixture.token,
              fixture.request, incoming).ok() &&
              former_source.MoveReplaceFrom(std::move(incoming)).ok() &&
              former_source.retained(),
          "authorized moved-from destination reuse failed");
    Check(former_source.Release().ok() && first_move.lease()->Release().ok(),
          "moved-from destination reuse cleanup failed");
  }

  {
    Fixture fixture(32);
    dt::BlobRetainedLifetimeLeaseV3 terminal;
    dt::BlobLifetimeRuntimeConformanceAccessV3::SetState(terminal, 8);
    const auto carrier = dt::BlobLifetimeRuntimeConformanceAccessV3::Carrier(
        fixture.carrier_binding, dt::BlobValueStateV3::value, nullptr, 0);
    const auto result = dt::RetainBaseBlobLifetimeLeaseV3Generation1(
        fixture.profile, fixture.Admission(), carrier, fixture.token,
        fixture.request, terminal);
    Check(!result.ok() &&
              dt::BlobLifetimeRuntimeConformanceAccessV3::ControlState(
                  terminal) == 8,
          "terminal wrapper was reused as a new binding generation");
  }
}

void AdministrativeCallbackReentry() {
  for (const std::uint8_t action : {std::uint8_t{1}, std::uint8_t{2},
                                    std::uint8_t{3}}) {
    Fixture fixture(96);
    const std::array<platform::byte, 1> bytes{{0x42}};
    auto carrier = dt::BlobLifetimeRuntimeConformanceAccessV3::Carrier(
        fixture.carrier_binding, dt::BlobValueStateV3::value, bytes.data(),
        bytes.size());
    dt::BlobRetainedLifetimeLeaseV3 active;
    dt::BlobRetainedLifetimeLeaseV3 peer;
    Check(dt::RetainBaseBlobLifetimeLeaseV3Generation1(
              fixture.profile, fixture.Admission(), carrier, fixture.token,
              fixture.request, active).ok(),
          "admin reentry active setup failed");
    if (action != 1)
      Check(active.CloneInto(peer).ok(),
            "admin reentry peer setup failed");
    fixture.fake.reentry_first = &active;
    fixture.fake.reentry_second = &peer;
    fixture.fake.reentry_action = action;
    const auto outer = active.Probe();
    Check(!outer.ok() && !fixture.fake.reentry_result.ok() &&
              fixture.fake.reentry_result.reason == "callback_reentrant" &&
              fixture.fake.reentry_attempts == 1 &&
              fixture.fake.invariant_calls == 1 &&
              fixture.fake.last_event ==
                  dt::BlobLifetimeInvariantEventV3::callback_reentrant &&
              fixture.fake.last_invariant_operation == fixture.request.operation &&
              fixture.fake.last_invariant_phase ==
                  dt::BlobLifetimePhaseV3::probe &&
              fixture.fake.last_invariant_callback == 255 &&
              dt::BlobLifetimeRuntimeConformanceAccessV3::ControlState(active) ==
                  8 &&
              (action == 1 || peer.retained()),
          "admin callback reentry did not preserve active outer provenance");
    if (action != 1)
      Check(peer.Release().ok(), "admin reentry peer cleanup failed");
  }
}

void RetainReentryUsesCanonicalDescriptor() {
  for (const std::uint8_t action : {6, 7, 8}) {
  for (const bool null_shortcut : {false, true}) {
  Fixture fixture(96);
  const std::array<platform::byte, 1> bytes{{0x42}};
  auto carrier = dt::BlobLifetimeRuntimeConformanceAccessV3::Carrier(
      fixture.carrier_binding, dt::BlobValueStateV3::value, bytes.data(),
      bytes.size());
  dt::BlobRetainedLifetimeLeaseV3 active;
  dt::BlobRetainedLifetimeLeaseV3 destination;
  Check(dt::RetainBaseBlobLifetimeLeaseV3Generation1(
            fixture.profile, fixture.Admission(), carrier, fixture.token,
            fixture.request, active).ok(),
        "retain reentry canonical setup failed");
  auto poisoned_profile = fixture.profile;
  if (!null_shortcut) {
    poisoned_profile.identity.descriptor_uuid = PlatformUuid(0xe1);
    poisoned_profile.identity.descriptor_generation = UINT64_C(0xdeadbeef);
  }
  auto null_carrier = dt::BlobLifetimeRuntimeConformanceAccessV3::Carrier(
      fixture.carrier_binding, dt::BlobValueStateV3::sql_null, nullptr, 0);
  auto nested_admission = fixture.Admission();
  fixture.fake.reentry_first = &active;
  fixture.fake.reentry_third = &destination;
  fixture.fake.reentry_profile = &poisoned_profile;
  fixture.fake.reentry_admission = &nested_admission;
  fixture.fake.reentry_carrier = null_shortcut ? &null_carrier : &carrier;
  fixture.fake.reentry_token = &fixture.token;
  fixture.fake.reentry_request = &fixture.request;
  fixture.fake.reentry_action = action;
  const auto outer = active.Probe();
  const auto& nested = fixture.fake.reentry_result;
  Check(!outer.ok() && !nested.ok() &&
            nested.reason == "callback_reentrant" &&
            nested.fact.parameters.descriptor_uuid ==
                fixture.profile.identity.descriptor_uuid &&
            nested.fact.parameters.descriptor_generation ==
                fixture.profile.identity.descriptor_generation &&
            (null_shortcut || (nested.fact.parameters.descriptor_uuid !=
                poisoned_profile.identity.descriptor_uuid &&
            nested.fact.parameters.descriptor_generation !=
                poisoned_profile.identity.descriptor_generation)) &&
            dt::BlobLifetimeRuntimeConformanceAccessV3::AdmissionAdmitted(
                nested_admission) &&
            dt::BlobLifetimeRuntimeConformanceAccessV3::ControlState(
                destination) == 0 &&
            CountCall(fixture.fake, 'R') == 1 &&
            CountCall(fixture.fake, 'P') == 1 &&
            CountCall(fixture.fake, 'L') == 1 &&
            fixture.fake.reentry_attempts == 1 &&
            fixture.fake.invariant_calls == 1,
        "consumer reentry trusted poisoned profile or NULL shortcut");
  }
  }
}

void DeferredProbePrimaryMatrix() {
  for (unsigned stimulus = 0; stimulus != 6; ++stimulus) {
    Fixture fixture(32);
    auto carrier = dt::BlobLifetimeRuntimeConformanceAccessV3::Carrier(
        fixture.carrier_binding, dt::BlobValueStateV3::value, nullptr, 0);
    dt::BlobRetainedLifetimeLeaseV3 lease;
    dt::BlobRetainedLifetimeLeaseV3 scratch;
    Check(dt::RetainBaseBlobLifetimeLeaseV3Generation1(
              fixture.profile, fixture.Admission(), carrier, fixture.token,
              fixture.request, lease).ok(), "deferred probe setup");
    fixture.fake.reentry_first = &lease;
    fixture.fake.reentry_third = &scratch;
    fixture.fake.reentry_action = 4;
    if (stimulus == 1 || stimulus == 2)
      fixture.fake.probe_code = SB_BLOB_CALLBACK_SECURITY_DENIED_V3;
    fixture.fake.dirty_probe_ticket = stimulus == 2;
    fixture.fake.throw_probe = stimulus == 3;
    if (stimulus == 4)
      fixture.fake.release_code = SB_BLOB_CALLBACK_SECURITY_DENIED_V3;
    if (stimulus == 5)
      fixture.fake.probe_code = SB_BLOB_CALLBACK_AUTHORITY_UNAVAILABLE_V3;
    const auto result = lease.Probe();
    const bool security = stimulus == 1 || stimulus == 2 || stimulus == 4;
    Check(result.disposition == dt::BlobLifetimeOuterDispositionV3::public_failure &&
              result.diagnostic_code == (security ? "SECURITY.ACCESS_DENIED"
                                                  : "CINL.LOB.DESCRIPTOR_INVALID") &&
              result.reason == (security ? "" : "callback_reentrant"),
          "deferred probe lost typed primary or SECURITY");
    BlobLifetimeRetainTicketV3 retained{};
    retained.retain_ticket_uuid = BlobUuid(0x90);
    retained.lifetime_token_uuid = fixture.token.lifetime_token_uuid;
    retained.lifetime_token_generation = fixture.token.lifetime_token_generation;
    retained.immutable_binding_generation = fixture.token.immutable_binding_generation;
    Check(std::memcmp(&fixture.fake.release_ticket_seen, &retained,
                      sizeof(retained)) == 0 &&
              CountCall(fixture.fake, 'P') == 1 &&
              CountCall(fixture.fake, 'L') == 1 &&
              fixture.fake.invariant_calls == 1 &&
              fixture.fake.last_event ==
                  dt::BlobLifetimeInvariantEventV3::callback_reentrant &&
              fixture.fake.last_invariant_operation == 2 &&
              fixture.fake.last_invariant_phase == dt::BlobLifetimePhaseV3::probe &&
              fixture.fake.pin_drop_calls == 2 && !lease.retained() &&
              !scratch.retained(),
          "deferred probe lost exact ticket cleanup or invariant receipt");
  }
}

void DeferredCallbackObligationMatrix() {
  for (const bool retaining : {true, false}) {
    for (unsigned stimulus = 0; stimulus != 11; ++stimulus) {
      Fixture fixture(64);
      const std::array<platform::byte, 1> bytes{{0x42}};
      auto carrier = dt::BlobLifetimeRuntimeConformanceAccessV3::Carrier(
          fixture.carrier_binding, dt::BlobValueStateV3::value,
          bytes.data(), bytes.size());
      dt::BlobRetainedLifetimeLeaseV3 lease;
      dt::BlobRetainedLifetimeLeaseV3 scratch;
      if (!retaining) {
        Check(dt::RetainBaseBlobLifetimeLeaseV3Generation1(
                  fixture.profile, fixture.Admission(), carrier, fixture.token,
                  fixture.request, lease).ok(), "deferred begin setup");
        fixture.fake.call_count = 0;
      }
      fixture.fake.reentry_first = &lease;
      fixture.fake.reentry_third = &scratch;
      fixture.fake.reentry_action = 4;
      auto& code = retaining ? fixture.fake.retain_code : fixture.fake.begin_code;
      if (stimulus == 1 || stimulus == 2)
        code = SB_BLOB_CALLBACK_AUTHORITY_UNAVAILABLE_V3;
      if (stimulus == 5 || stimulus == 6)
        code = SB_BLOB_CALLBACK_SECURITY_DENIED_V3;
      fixture.fake.dirty_failure_output =
          stimulus == 2 || stimulus == 6 || stimulus == 8;
      if (stimulus == 3) {
        if (retaining) fixture.fake.throw_retain = true;
        else fixture.fake.throw_begin = true;
      }
      if (stimulus == 4) {
        if (retaining) fixture.fake.throw_retain_after_output = true;
        else fixture.fake.throw_begin_after_output = true;
      }
      if (stimulus == 7 || stimulus == 8) {
        fixture.fake.malformed_phase = retaining ? 1 : 3;
        fixture.fake.malformed_stimulus = 1;
      }
      if (stimulus == 9) {
        if (retaining) fixture.fake.malformed_retain_ticket = true;
        else fixture.fake.malformed_access_ticket = true;
      }
      if (stimulus == 10) {
        if (retaining)
          fixture.fake.release_code = SB_BLOB_CALLBACK_SECURITY_DENIED_V3;
        else fixture.fake.end_code = SB_BLOB_CALLBACK_SECURITY_DENIED_V3;
      }
      VisitContext visit;
      auto visitor = dt::BlobLifetimeRuntimeConformanceAccessV3::Visitor(
          CopyVisitor, &visit);
      const auto result = retaining
          ? dt::RetainBaseBlobLifetimeLeaseV3Generation1(
                fixture.profile, fixture.Admission(), carrier, fixture.token,
                fixture.request, lease)
          : lease.VisitBoundMaterializedBytes(visitor);
      const bool security = stimulus == 5 || stimulus == 6 || stimulus == 10;
      Check(result.disposition == dt::BlobLifetimeOuterDispositionV3::public_failure &&
                result.diagnostic_code == (security ? "SECURITY.ACCESS_DENIED"
                                                    : "CINL.LOB.DESCRIPTOR_INVALID") &&
                result.reason == (security ? "" : "callback_reentrant") &&
                fixture.fake.reentry_result.reason == "callback_reentrant",
            "deferred callback lost primary or SECURITY precedence");
      const unsigned obligation = stimulus == 0 || stimulus == 4 ||
          stimulus == 8 || stimulus == 9 || stimulus == 10;
      const auto counter = dt::BlobLifetimeRuntimeConformanceAccessV3::Counter(
          fixture.ledger, dt::BlobLifetimeResourceV3::lifetime_callback_calls);
      Check(CountCall(fixture.fake, 'R') == (retaining ? 1 : 0) &&
                CountCall(fixture.fake, 'B') == (retaining ? 0 : 1) &&
                CountCall(fixture.fake, 'E') == (retaining ? 0 : obligation) &&
                CountCall(fixture.fake, 'L') == (retaining ? obligation : 1) &&
                counter.invoked == (retaining ? 1 + obligation : 3 + obligation) &&
                counter.reserved_cleanup == 0,
            "deferred callback fabricated or lost cleanup obligation");
      Check(fixture.fake.invariant_calls == 1 &&
                fixture.fake.last_event ==
                    dt::BlobLifetimeInvariantEventV3::callback_reentrant &&
                fixture.fake.last_invariant_phase ==
                    (retaining ? dt::BlobLifetimePhaseV3::retain
                               : dt::BlobLifetimePhaseV3::begin_access) &&
                fixture.fake.last_invariant_operation == 2 &&
                fixture.fake.last_invariant_callback == 255 &&
                fixture.fake.pin_drop_calls == 2 &&
                dt::BlobLifetimeRuntimeConformanceAccessV3::ControlState(lease) == 8 &&
                !scratch.retained() && visit.copied_size == 0,
            "deferred callback lost receipt, pins, or unpublished output");
    }
  }
}

bool OuterCloneCallbackReentryCase(std::uint8_t action) {
  Fixture fixture(96);
  const std::array<platform::byte, 1> bytes{{0x42}};
  auto carrier = dt::BlobLifetimeRuntimeConformanceAccessV3::Carrier(
      fixture.carrier_binding, dt::BlobValueStateV3::value, bytes.data(),
      bytes.size());
  dt::BlobRetainedLifetimeLeaseV3 source;
  dt::BlobRetainedLifetimeLeaseV3 destination;
  dt::BlobRetainedLifetimeLeaseV3 scratch;
  if (!dt::RetainBaseBlobLifetimeLeaseV3Generation1(
           fixture.profile, fixture.Admission(), carrier, fixture.token,
           fixture.request, source).ok())
    return false;
  fixture.fake.reentry_first = action == 4 ? &source : &destination;
  fixture.fake.reentry_third = &scratch;
  fixture.fake.reentry_action = action;
  const auto outer = source.CloneInto(destination);
  const bool exact = outer.disposition ==
          dt::BlobLifetimeOuterDispositionV3::public_failure &&
      outer.diagnostic_code == "CINL.LOB.DESCRIPTOR_INVALID" &&
      outer.reason == "callback_reentrant" &&
      outer.fact.reason == dt::BlobLifetimeReasonV3::callback_reentrant &&
      outer.fact.parameters.descriptor_uuid ==
          fixture.profile.identity.descriptor_uuid &&
      outer.fact.parameters.descriptor_generation ==
          fixture.profile.identity.descriptor_generation &&
      !fixture.fake.reentry_result.ok() &&
      fixture.fake.reentry_result.reason == "callback_reentrant" &&
      fixture.fake.reentry_attempts == 1 && fixture.fake.invariant_calls == 1 &&
      fixture.fake.last_event ==
          dt::BlobLifetimeInvariantEventV3::callback_reentrant &&
      fixture.fake.last_invariant_operation == 15 &&
      fixture.fake.last_invariant_phase == dt::BlobLifetimePhaseV3::retain &&
      fixture.fake.last_invariant_callback == 255 && source.retained() &&
      !scratch.retained();
  return exact && source.Release().ok();
}

void ProtectedFactAuthorityFailClosed() {
  constexpr std::array<std::uint8_t, 4> cases{{1, 2, 3, 4}};
  for (const auto test_case : cases) {
    Fixture fixture(32);
    fixture.request.operation = test_case == 1 ? 2 : 1;
    const std::array<platform::byte, 1> bytes{{0x42}};
    auto carrier = dt::BlobLifetimeRuntimeConformanceAccessV3::Carrier(
        fixture.carrier_binding, dt::BlobValueStateV3::value, bytes.data(),
        bytes.size());
    dt::BlobRetainedLifetimeLeaseV3 lease;
    Check(dt::RetainBaseBlobLifetimeLeaseV3Generation1(
              fixture.profile, fixture.Admission(), carrier, fixture.token,
              fixture.request, lease).ok(),
          "protected-fact fail-closed setup retain failed");
    TypedFailureVisitorContext context;
    auto& fact = context.fact;
    fact.stage = dt::BlobLifetimeFactStageV3::callback_return;
    fact.diagnostic_generation = 1;
    if (test_case == 1 || test_case == 3) {
      fact.fact_class = dt::BlobLifetimeFactClassV3::io;
      fact.diagnostic = dt::BlobLifetimeDiagnosticCodeV3::io_failed;
      fact.reason = test_case == 1
          ? dt::BlobLifetimeReasonV3::sink_io_failure
          : dt::BlobLifetimeReasonV3::none;
      fact.diagnostic_uuid = platform::Uuid{{
          0x01,0xa1,0x0c,0x52,0x66,0x99,0x7e,0xdb,
          0xb4,0x6f,0xaf,0x2b,0x0a,0x3d,0x5a,0xca}};
      if (test_case == 1) {
        fact.parameters.present = dt::blob_parameter_operation |
            dt::blob_parameter_phase | dt::blob_parameter_adapter_kind;
        fact.parameters.operation_enum = fixture.request.operation;
        fact.parameters.phase_enum =
            dt::BlobLifetimePublicPhaseV3::protected_access;
        fact.parameters.adapter_kind_enum = 1;
      }
    } else {
      fact.fact_class = dt::BlobLifetimeFactClassV3::integrity;
      fact.diagnostic = dt::BlobLifetimeDiagnosticCodeV3::integrity_failed;
      fact.reason = test_case == 2
          ? static_cast<dt::BlobLifetimeReasonV3>(30)
          : dt::BlobLifetimeReasonV3::reader_io_failure;
      fact.diagnostic_uuid = platform::Uuid{{
          0x01,0xa1,0x09,0x5f,0xf2,0x05,0x78,0x79,
          0xb9,0x23,0x42,0x5b,0xe9,0x3f,0x2c,0x1a}};
      if (test_case == 2) {
        fact.parameters.present = dt::blob_parameter_format;
        fact.parameters.format_enum = 1;
      }
    }
    auto visitor = dt::BlobLifetimeRuntimeConformanceAccessV3::Visitor(
        TypedFailureVisitor, &context);
    const auto result = lease.VisitBoundMaterializedBytes(visitor);
    Check(result.disposition ==
              dt::BlobLifetimeOuterDispositionV3::terminated_out_of_band &&
              result.diagnostic_code.empty() && result.reason.empty() &&
              result.fact.diagnostic ==
                  dt::BlobLifetimeDiagnosticCodeV3::none &&
              CountCall(fixture.fake, 'E') == 1 &&
              CountCall(fixture.fake, 'L') == 1 &&
              fixture.fake.invariant_calls == 1 &&
              fixture.fake.last_event ==
                  dt::BlobLifetimeInvariantEventV3::receiver_services_failed,
          "unpublished IO/integrity fact escaped fail-closed validation");
  }
}

bool ExactInvariantTuple(const FakeReceiver& fake,
                         dt::BlobLifetimeInvariantEventV3 event,
                         dt::BlobLifetimePhaseV3 phase,
                         std::uint8_t operation,
                         std::uint8_t callback) noexcept {
  return fake.invariant_calls == 1 && fake.last_event == event &&
         fake.last_invariant_phase == phase &&
         fake.last_invariant_operation == operation &&
         fake.last_invariant_callback == callback;
}

template <typename Function>
bool RunIsolatedRegression(std::string_view name, Function function) {
  const pid_t child = fork();
  Check(child >= 0, "fork failed for isolated regression");
  if (child == 0) {
    const rlimit no_core{0, 0};
    (void)setrlimit(RLIMIT_CORE, &no_core);
    const bool ok = function();
    if (!ok) std::cerr << "KNOWN_GAP name=" << name << '\n';
    _exit(ok ? 0 : 1);
  }
  int status = 0;
  Check(waitpid(child, &status, 0) == child,
        "waitpid failed for isolated regression");
  ++checks;
  if (WIFEXITED(status) && WEXITSTATUS(status) == 0) {
    std::cerr << "REGRESSION_PASS name=" << name << '\n';
    return true;
  }
  if (WIFSIGNALED(status)) {
    std::cerr << "KNOWN_GAP name=" << name
              << " signal=" << WTERMSIG(status) << '\n';
  }
  return false;
}

std::size_t ReleaseOuterOperationRegressions() {
  std::size_t failures = 0;

  // Destination cleanup performed by move-replace is operation 17. The
  // operation-wide invariant domain requires callback_code_or_255 == 255.
  if (!RunIsolatedRegression(
          "move_replace_release_outer_operation_17", [] {
            Fixture fixture(16);
            auto carrier = dt::BlobLifetimeRuntimeConformanceAccessV3::Carrier(
                fixture.carrier_binding, dt::BlobValueStateV3::value, nullptr,
                0);
            dt::BlobRetainedLifetimeLeaseV3 source;
            dt::BlobRetainedLifetimeLeaseV3 destination;
            if (!dt::RetainBaseBlobLifetimeLeaseV3Generation1(
                     fixture.profile, fixture.Admission(), carrier,
                     fixture.token, fixture.request, source).ok() ||
                !source.CloneInto(destination).ok())
              return false;
            fixture.fake.invariant_calls = 0;
            fixture.fake.release_code =
                SB_BLOB_CALLBACK_RESOURCE_EXHAUSTED_V3;
            const auto result =
                destination.MoveReplaceFrom(std::move(source));
            const bool ok = !result.ok() &&
                ExactInvariantTuple(
                    fixture.fake,
                    dt::BlobLifetimeInvariantEventV3::release_failed,
                    dt::BlobLifetimePhaseV3::release, 17,
                    SB_BLOB_CALLBACK_RESOURCE_EXHAUSTED_V3);
            fixture.fake.release_code = SB_BLOB_CALLBACK_OK_V3;
            (void)source.Release();
            return ok;
          }))
    ++failures;

  // An explicit Release operation is operation 18.
  if (!RunIsolatedRegression(
          "explicit_release_outer_operation_18", [] {
            Fixture fixture(16);
            auto carrier = dt::BlobLifetimeRuntimeConformanceAccessV3::Carrier(
                fixture.carrier_binding, dt::BlobValueStateV3::value, nullptr,
                0);
            dt::BlobRetainedLifetimeLeaseV3 lease;
            if (!dt::RetainBaseBlobLifetimeLeaseV3Generation1(
                     fixture.profile, fixture.Admission(), carrier,
                     fixture.token, fixture.request, lease).ok())
              return false;
            fixture.fake.invariant_calls = 0;
            fixture.fake.release_code =
                SB_BLOB_CALLBACK_RESOURCE_EXHAUSTED_V3;
            const auto result = lease.Release();
            return !result.ok() &&
                ExactInvariantTuple(
                    fixture.fake,
                    dt::BlobLifetimeInvariantEventV3::release_failed,
                    dt::BlobLifetimePhaseV3::release, 18,
                    SB_BLOB_CALLBACK_RESOURCE_EXHAUSTED_V3);
          }))
    ++failures;

  // Destructor cleanup is operation 19.
  if (!RunIsolatedRegression(
          "destructor_release_outer_operation_19", [] {
            Fixture fixture(16);
            auto carrier = dt::BlobLifetimeRuntimeConformanceAccessV3::Carrier(
                fixture.carrier_binding, dt::BlobValueStateV3::value, nullptr,
                0);
            {
              dt::BlobRetainedLifetimeLeaseV3 lease;
              if (!dt::RetainBaseBlobLifetimeLeaseV3Generation1(
                       fixture.profile, fixture.Admission(), carrier,
                       fixture.token, fixture.request, lease).ok())
                return false;
              fixture.fake.invariant_calls = 0;
              fixture.fake.release_code =
                  SB_BLOB_CALLBACK_RESOURCE_EXHAUSTED_V3;
            }
            return ExactInvariantTuple(
                fixture.fake,
                dt::BlobLifetimeInvariantEventV3::release_failed,
                dt::BlobLifetimePhaseV3::destructor_cleanup, 19,
                SB_BLOB_CALLBACK_RESOURCE_EXHAUSTED_V3);
          }))
    ++failures;
  return failures;
}

std::size_t ReservedSamplerPrecedenceRegressions() {
  std::size_t failures = 0;

  // Reserved sampler bytes invalidate the whole result before its time value
  // may be consumed, including before monotonic-regression comparison.
  if (!RunIsolatedRegression(
          "clock_reserved_precedes_time_consumption", [] {
            Fixture fixture(16);
            auto carrier = dt::BlobLifetimeRuntimeConformanceAccessV3::Carrier(
                fixture.carrier_binding, dt::BlobValueStateV3::value, nullptr,
                0);
            dt::BlobRetainedLifetimeLeaseV3 lease;
            if (!dt::RetainBaseBlobLifetimeLeaseV3Generation1(
                     fixture.profile, fixture.Admission(), carrier,
                     fixture.token, fixture.request, lease).ok())
              return false;
            fixture.fake.clock_dirty_reserved = true;
            fixture.fake.regress_clock = true;
            const auto result = dt::BlobLifetimeRuntimeConformanceAccessV3::Gate(
                lease, dt::BlobLifetimeGateV3::before_ordinary_probe,
                dt::BlobLifetimePublicPhaseV3::probe,
                dt::BlobLifetimePhaseV3::probe);
            const bool ok = !result.ok() &&
                result.diagnostic_code ==
                    "BLOB.LIFETIME_AUTHORITY_UNAVAILABLE" &&
                result.reason == "monotonic_clock_unavailable";
            fixture.fake.clock_dirty_reserved = false;
            fixture.fake.regress_clock = false;
            (void)lease.Release();
            return ok;
          }))
    ++failures;

  // Reserved sampler bytes likewise invalidate cancellation before the
  // cancellation flag may be interpreted.
  if (!RunIsolatedRegression(
          "cancellation_reserved_precedes_flag_consumption", [] {
            Fixture fixture(16);
            auto carrier = dt::BlobLifetimeRuntimeConformanceAccessV3::Carrier(
                fixture.carrier_binding, dt::BlobValueStateV3::value, nullptr,
                0);
            dt::BlobRetainedLifetimeLeaseV3 lease;
            if (!dt::RetainBaseBlobLifetimeLeaseV3Generation1(
                     fixture.profile, fixture.Admission(), carrier,
                     fixture.token, fixture.request, lease).ok())
              return false;
            fixture.fake.cancellation_dirty_reserved = true;
            fixture.fake.cancelled = true;
            const auto result = dt::BlobLifetimeRuntimeConformanceAccessV3::Gate(
                lease, dt::BlobLifetimeGateV3::before_ordinary_probe,
                dt::BlobLifetimePublicPhaseV3::probe,
                dt::BlobLifetimePhaseV3::probe);
            const bool ok = !result.ok() &&
                result.diagnostic_code ==
                    "BLOB.LIFETIME_AUTHORITY_UNAVAILABLE" &&
                result.reason == "receiver_services_failed";
            fixture.fake.cancellation_dirty_reserved = false;
            fixture.fake.cancelled = false;
            (void)lease.Release();
            return ok;
          }))
    ++failures;
  return failures;
}

std::size_t DirtyTicketRegressions() {
  std::size_t failures = 0;
  for (const std::uint8_t phase : {std::uint8_t{4}, std::uint8_t{5}}) {
    const MalformedRow* selected = nullptr;
    for (const auto& row : kMalformedRows) {
      if (row.phase == phase && row.stimulus == 11) {
        selected = &row;
        break;
      }
    }
    Check(selected != nullptr, "dirty ticket vector missing");
    const auto name = phase == 4
        ? std::string_view{"dirty_end_ticket_exact_contract"}
        : std::string_view{"dirty_release_ticket_exact_contract"};
    if (!RunIsolatedRegression(name, [selected] {
          return ExecuteMalformedRow(*selected);
        }))
      ++failures;
    if (phase == 5 &&
        !RunIsolatedRegression("explicit_release_dirty_ticket_operation_18",
                              [selected] {
          return ExecuteMalformedRow(*selected, true);
        }))
      ++failures;
  }
  return failures;
}

std::size_t AdapterExceptionRegressions() {
  std::size_t failures = 0;
  for (std::uint8_t phase = 1; phase <= 5; ++phase) {
    const std::array<std::string_view, 5> names{{
        "retain_adapter_exception", "probe_adapter_exception",
        "begin_adapter_exception", "end_adapter_exception",
        "release_adapter_exception"}};
    if (!RunIsolatedRegression(names[phase - 1], [phase] {
          Fixture fixture(16);
          const std::array<platform::byte, 1> bytes{{0x42}};
          auto carrier = dt::BlobLifetimeRuntimeConformanceAccessV3::Carrier(
              fixture.carrier_binding, dt::BlobValueStateV3::value,
              phase == 3 || phase == 4 ? bytes.data() : nullptr,
              phase == 3 || phase == 4 ? bytes.size() : 0);
          dt::BlobRetainedLifetimeLeaseV3 lease;
          if (phase == 1) fixture.fake.throw_retain = true;
          auto result = dt::RetainBaseBlobLifetimeLeaseV3Generation1(
              fixture.profile, fixture.Admission(), carrier, fixture.token,
              fixture.request, lease);
          if (phase == 1) {
            const auto counter =
                dt::BlobLifetimeRuntimeConformanceAccessV3::Counter(
                    fixture.ledger,
                    dt::BlobLifetimeResourceV3::lifetime_callback_calls);
            const bool ok = !result.ok() && result.diagnostic_code.empty() &&
                ExactInvariantTuple(
                    fixture.fake,
                    dt::BlobLifetimeInvariantEventV3::adapter_exception,
                    dt::BlobLifetimePhaseV3::retain, 2, 255) &&
                CountCall(fixture.fake, 'R') == 1 &&
                CountCall(fixture.fake, 'L') == 0 &&
                counter.invoked == 1 && counter.reserved_cleanup == 0;
            if (!ok)
              std::cerr << "ADAPTER_OBS phase=1 disposition="
                        << unsigned(result.disposition)
                        << " diagnostic=" << result.diagnostic_code
                        << " invariants=" << fixture.fake.invariant_calls
                        << " tuple=" << unsigned(fixture.fake.last_event) << '/'
                        << unsigned(fixture.fake.last_invariant_phase) << '/'
                        << unsigned(fixture.fake.last_invariant_operation) << '/'
                        << unsigned(fixture.fake.last_invariant_callback)
                        << " calls=" << unsigned(fixture.fake.call_count) << '\n';
            return ok;
          }
          if (!result.ok()) return false;
          fixture.fake.call_count = 0;
          fixture.fake.invariant_calls = 0;
          if (phase == 2) {
            fixture.fake.throw_probe = true;
            result = lease.Probe();
          } else if (phase == 3 || phase == 4) {
            fixture.fake.throw_begin = phase == 3;
            fixture.fake.throw_end = phase == 4;
            VisitContext visit{};
            auto visitor = dt::BlobLifetimeRuntimeConformanceAccessV3::Visitor(
                CopyVisitor, &visit);
            result = lease.VisitBoundMaterializedBytes(visitor);
          } else {
            fixture.fake.throw_release = true;
            result = lease.Release();
          }
          const auto invariant_phase = static_cast<dt::BlobLifetimePhaseV3>(
              phase == 2 ? 3 : phase == 3 ? 4 : phase == 4 ? 6 : 7);
          const bool exact_calls =
              phase == 2
                  ? CountCall(fixture.fake, 'P') == 1 &&
                        CountCall(fixture.fake, 'L') == 1
              : phase == 3
                  ? CountCall(fixture.fake, 'B') == 1 &&
                        CountCall(fixture.fake, 'E') == 0 &&
                        CountCall(fixture.fake, 'L') == 1
              : phase == 4
                  ? CountCall(fixture.fake, 'B') == 1 &&
                        CountCall(fixture.fake, 'E') == 1 &&
                        CountCall(fixture.fake, 'L') == 1
                  : CountCall(fixture.fake, 'L') == 1;
          const bool ok = !result.ok() && result.diagnostic_code.empty() &&
              exact_calls && ExactInvariantTuple(
                  fixture.fake,
                  dt::BlobLifetimeInvariantEventV3::adapter_exception,
                  invariant_phase, phase == 5 ? 18 : 2, 255);
          if (!ok)
            std::cerr << "ADAPTER_OBS phase=" << unsigned(phase)
                      << " disposition=" << unsigned(result.disposition)
                      << " diagnostic=" << result.diagnostic_code
                      << " invariants=" << fixture.fake.invariant_calls
                      << " tuple=" << unsigned(fixture.fake.last_event) << '/'
                      << unsigned(fixture.fake.last_invariant_phase) << '/'
                      << unsigned(fixture.fake.last_invariant_operation) << '/'
                      << unsigned(fixture.fake.last_invariant_callback)
                      << " calls=" << unsigned(fixture.fake.call_count) << '\n';
          return ok;
        }))
      ++failures;
  }
  return failures;
}

std::size_t AdapterExceptionOutputMatrix() {
  struct Row {
    std::string_view name;
    std::uint8_t phase;
    bool output_before_throw;
    bool cleanup_security;
    std::uint8_t expected_end;
    std::uint8_t expected_release;
    std::uint64_t expected_invoked;
  };
  constexpr std::array<Row, 6> rows{{
      {"retain_throw_zero_output", 1, false, false, 0, 0, 1},
      {"retain_throw_nonzero_output", 1, true, false, 0, 1, 2},
      {"retain_throw_nonzero_output_security_cleanup", 1, true, true, 0, 1,
       2},
      {"begin_throw_zero_output", 3, false, false, 0, 1, 3},
      {"begin_throw_nonzero_output", 3, true, false, 1, 1, 4},
      {"begin_throw_nonzero_output_security_cleanup", 3, true, true, 1, 1,
       4},
  }};
  std::size_t failures = 0;
  for (const auto& row : rows) {
    if (!RunIsolatedRegression(row.name, [row] {
          Fixture fixture(16);
          const std::array<platform::byte, 1> bytes{{0x42}};
          auto carrier = dt::BlobLifetimeRuntimeConformanceAccessV3::Carrier(
              fixture.carrier_binding, dt::BlobValueStateV3::value,
              row.phase == 3 ? bytes.data() : nullptr,
              row.phase == 3 ? bytes.size() : 0);
          dt::BlobRetainedLifetimeLeaseV3 lease;
          dt::BlobLifetimeRuntimeResultV3 result;
          if (row.phase == 1) {
            fixture.fake.throw_retain = !row.output_before_throw;
            fixture.fake.throw_retain_after_output = row.output_before_throw;
            if (row.cleanup_security)
              fixture.fake.release_code = SB_BLOB_CALLBACK_SECURITY_DENIED_V3;
            result = dt::RetainBaseBlobLifetimeLeaseV3Generation1(
                fixture.profile, fixture.Admission(), carrier, fixture.token,
                fixture.request, lease);
          } else {
            if (!dt::RetainBaseBlobLifetimeLeaseV3Generation1(
                     fixture.profile, fixture.Admission(), carrier,
                     fixture.token, fixture.request, lease).ok())
              return false;
            fixture.fake.call_count = 0;
            fixture.fake.invariant_calls = 0;
            fixture.fake.throw_begin = !row.output_before_throw;
            fixture.fake.throw_begin_after_output = row.output_before_throw;
            if (row.cleanup_security)
              fixture.fake.end_code = SB_BLOB_CALLBACK_SECURITY_DENIED_V3;
            VisitContext visit{};
            auto visitor = dt::BlobLifetimeRuntimeConformanceAccessV3::Visitor(
                CopyVisitor, &visit);
            result = lease.VisitBoundMaterializedBytes(visitor);
          }

          BlobLifetimeRetainTicketV3 expected_retain{};
          expected_retain.retain_ticket_uuid = BlobUuid(0x90);
          expected_retain.lifetime_token_uuid =
              fixture.token.lifetime_token_uuid;
          expected_retain.lifetime_token_generation =
              fixture.token.lifetime_token_generation;
          expected_retain.immutable_binding_generation =
              fixture.token.immutable_binding_generation;
          BlobLifetimeAccessTicketV3 expected_access{};
          expected_access.access_ticket_uuid = BlobUuid(0x91);
          expected_access.retain_ticket_uuid =
              expected_retain.retain_ticket_uuid;
          expected_access.lifetime_token_generation =
              fixture.token.lifetime_token_generation;
          expected_access.immutable_binding_generation =
              fixture.token.immutable_binding_generation;
          const BlobLifetimeRetainTicketV3 zero_retain{};
          const BlobLifetimeAccessTicketV3 zero_access{};

          const auto counter =
              dt::BlobLifetimeRuntimeConformanceAccessV3::Counter(
                  fixture.ledger,
                  dt::BlobLifetimeResourceV3::lifetime_callback_calls);
          const bool exact_result = row.cleanup_security
              ? result.disposition ==
                    dt::BlobLifetimeOuterDispositionV3::public_failure &&
                    result.diagnostic_code == "SECURITY.ACCESS_DENIED" &&
                    result.reason.empty()
              : result.disposition ==
                    dt::BlobLifetimeOuterDispositionV3::
                        terminated_out_of_band &&
                    result.diagnostic_code.empty() && result.reason.empty();
          const bool exact_release_ticket = row.expected_release == 0
              ? std::memcmp(&fixture.fake.release_ticket_seen,
                            &zero_retain,
                            sizeof(BlobLifetimeRetainTicketV3)) == 0
              : std::memcmp(&fixture.fake.release_ticket_seen,
                            &expected_retain,
                            sizeof(expected_retain)) == 0;
          const bool exact_end_ticket = row.expected_end == 0
              ? std::memcmp(&fixture.fake.end_ticket_seen,
                            &zero_access,
                            sizeof(BlobLifetimeAccessTicketV3)) == 0
              : std::memcmp(&fixture.fake.end_ticket_seen, &expected_access,
                            sizeof(expected_access)) == 0;
          return exact_result && !lease.retained() &&
              CountCall(fixture.fake, 'R') == (row.phase == 1 ? 1 : 0) &&
              CountCall(fixture.fake, 'B') == (row.phase == 3 ? 1 : 0) &&
              CountCall(fixture.fake, 'E') == row.expected_end &&
              CountCall(fixture.fake, 'L') == row.expected_release &&
              counter.invoked == row.expected_invoked &&
              counter.reserved_cleanup == 0 && exact_release_ticket &&
              exact_end_ticket && ExactInvariantTuple(
                  fixture.fake,
                  dt::BlobLifetimeInvariantEventV3::adapter_exception,
                  row.phase == 1 ? dt::BlobLifetimePhaseV3::retain
                                 : dt::BlobLifetimePhaseV3::begin_access,
                  2, 255);
        }))
      ++failures;
  }
  return failures;
}

std::size_t ProtectedProtocolRegressions() {
  std::size_t failures = 0;
  const std::array<std::string_view, 3> names{{
      "protected_unknown_status", "protected_dirty_reserved",
      "protected_invalid_typed_fact"}};
  for (std::uint8_t stimulus = 1; stimulus <= 3; ++stimulus) {
    if (!RunIsolatedRegression(names[stimulus - 1], [stimulus] {
          Fixture fixture(16);
          const std::array<platform::byte, 1> bytes{{0x42}};
          auto carrier = dt::BlobLifetimeRuntimeConformanceAccessV3::Carrier(
              fixture.carrier_binding, dt::BlobValueStateV3::value,
              bytes.data(), bytes.size());
          dt::BlobRetainedLifetimeLeaseV3 lease;
          if (!dt::RetainBaseBlobLifetimeLeaseV3Generation1(
                   fixture.profile, fixture.Admission(), carrier,
                   fixture.token, fixture.request, lease).ok())
            return false;
          fixture.fake.call_count = 0;
          fixture.fake.invariant_calls = 0;
          auto local_stimulus = stimulus;
          auto visitor = dt::BlobLifetimeRuntimeConformanceAccessV3::Visitor(
              ProtocolViolationVisitor, &local_stimulus);
          const auto result = lease.VisitBoundMaterializedBytes(visitor);
          return !result.ok() && result.diagnostic_code.empty() &&
              CountCall(fixture.fake, 'B') == 1 &&
              CountCall(fixture.fake, 'E') == 1 &&
              CountCall(fixture.fake, 'L') == 1 &&
              ExactInvariantTuple(
                  fixture.fake,
                  dt::BlobLifetimeInvariantEventV3::receiver_services_failed,
                  dt::BlobLifetimePhaseV3::protected_access, 2, 255);
        }))
      ++failures;
  }
  return failures;
}

std::size_t MonotonicAndCallbackQuarantineRegressions() {
  std::size_t failures = 0;
  if (!RunIsolatedRegression("monotonic_regression_exact_quarantine", [] {
        Fixture fixture(16);
        auto carrier = dt::BlobLifetimeRuntimeConformanceAccessV3::Carrier(
            fixture.carrier_binding, dt::BlobValueStateV3::value, nullptr, 0);
        dt::BlobRetainedLifetimeLeaseV3 lease;
        if (!dt::RetainBaseBlobLifetimeLeaseV3Generation1(
                 fixture.profile, fixture.Admission(), carrier, fixture.token,
                 fixture.request, lease).ok())
          return false;
        fixture.fake.call_count = 0;
        fixture.fake.invariant_calls = 0;
        const auto cancellation_before = fixture.fake.cancellation_calls;
        fixture.fake.regress_clock = true;
        const auto result = lease.Probe();
        fixture.fake.regress_clock = false;
        const bool ok = !result.ok() && result.diagnostic_code.empty() &&
            fixture.fake.cancellation_calls == cancellation_before &&
            CountCall(fixture.fake, 'P') == 0 &&
            CountCall(fixture.fake, 'L') == 1 &&
            ExactInvariantTuple(
                fixture.fake,
                dt::BlobLifetimeInvariantEventV3::monotonic_clock_regression,
                dt::BlobLifetimePhaseV3::probe, 2, 255);
        if (!ok)
          std::cerr << "MONOTONIC_OBS disposition="
                    << unsigned(result.disposition)
                    << " diagnostic=" << result.diagnostic_code
                    << " cancellation=" << cancellation_before << '/'
                    << fixture.fake.cancellation_calls
                    << " invariants=" << fixture.fake.invariant_calls
                    << " tuple=" << unsigned(fixture.fake.last_event) << '/'
                    << unsigned(fixture.fake.last_invariant_phase) << '/'
                    << unsigned(fixture.fake.last_invariant_operation) << '/'
                    << unsigned(fixture.fake.last_invariant_callback) << '\n';
        return ok;
      }))
    ++failures;

  if (!RunIsolatedRegression("probe_code_1_callback_quarantine", [] {
        Fixture fixture(16);
        auto carrier = dt::BlobLifetimeRuntimeConformanceAccessV3::Carrier(
            fixture.carrier_binding, dt::BlobValueStateV3::value, nullptr, 0);
        dt::BlobRetainedLifetimeLeaseV3 lease;
        if (!dt::RetainBaseBlobLifetimeLeaseV3Generation1(
                 fixture.profile, fixture.Admission(), carrier, fixture.token,
                 fixture.request, lease).ok())
          return false;
        fixture.fake.call_count = 0;
        fixture.fake.invariant_calls = 0;
        fixture.fake.probe_code = SB_BLOB_CALLBACK_INVALID_ARGUMENT_V3;
        const auto result = lease.Probe();
        return !result.ok() &&
            result.diagnostic_code == "CINL.LOB.DESCRIPTOR_INVALID" &&
            result.reason == "callback_output_impossible" &&
            CountCall(fixture.fake, 'P') == 1 &&
            CountCall(fixture.fake, 'L') == 1 &&
            ExactInvariantTuple(
                fixture.fake,
                dt::BlobLifetimeInvariantEventV3::callback_result_unknown,
                dt::BlobLifetimePhaseV3::probe, 2, 1);
      }))
    ++failures;
  return failures;
}

std::size_t CorruptLedgerRegression() {
  if (RunIsolatedRegression("corrupt_ledger_exact_cleanup", [] {
        CorruptLedgerCleanup();
        return true;
      }))
    return 0;
  return 1;
}

std::size_t MoveAtomicityRegression() {
  if (RunIsolatedRegression("typed_move_identity_and_guard_atomicity", [] {
        ExactMoveIdentityAndGuards();
        return true;
      }))
    return 0;
  return 1;
}

std::size_t CleanupPrecedenceRegression() {
  if (RunIsolatedRegression("cleanup_fact_precedence", [] {
        CleanupPrecedence();
        return true;
      }))
    return 0;
  return 1;
}

std::size_t CancelCleanupCorruptionRegressions() {
  std::size_t failures = 0;
  for (const std::uint8_t phase : {std::uint8_t{1}, std::uint8_t{3}}) {
    for (const std::uint8_t code :
         {std::uint8_t{SB_BLOB_CALLBACK_SECURITY_DENIED_V3},
          std::uint8_t{SB_BLOB_CALLBACK_RESOURCE_EXHAUSTED_V3}}) {
      const std::string_view name = phase == 1
          ? (code == SB_BLOB_CALLBACK_SECURITY_DENIED_V3
                 ? "retain_cancel_corrupt_security"
                 : "retain_cancel_corrupt_private")
          : (code == SB_BLOB_CALLBACK_SECURITY_DENIED_V3
                 ? "begin_cancel_corrupt_security"
                 : "begin_cancel_corrupt_private");
      if (!RunIsolatedRegression(name, [phase, code] {
            Fixture fixture(16);
            const std::array<platform::byte, 1> bytes{{0x42}};
            auto carrier = dt::BlobLifetimeRuntimeConformanceAccessV3::Carrier(
                fixture.carrier_binding, dt::BlobValueStateV3::value,
                phase == 3 ? bytes.data() : nullptr,
                phase == 3 ? bytes.size() : 0);
            fixture.fake.corrupt_cancel_ledger = &fixture.ledger;
            fixture.fake.corrupt_cancel_phase = phase;
            dt::BlobRetainedLifetimeLeaseV3 lease;
            dt::BlobLifetimeRuntimeResultV3 result;
            if (phase == 1) {
              fixture.fake.retain_code = code;
              result = dt::RetainBaseBlobLifetimeLeaseV3Generation1(
                  fixture.profile, fixture.Admission(), carrier,
                  fixture.token, fixture.request, lease);
            } else {
              fixture.fake.corrupt_cancel_phase = 0;
              if (!dt::RetainBaseBlobLifetimeLeaseV3Generation1(
                       fixture.profile, fixture.Admission(), carrier,
                       fixture.token, fixture.request, lease).ok())
                return false;
              fixture.fake.call_count = 0;
              fixture.fake.invariant_calls = 0;
              fixture.fake.corrupt_cancel_phase = 3;
              fixture.fake.begin_code = code;
              VisitContext visit{};
              auto visitor =
                  dt::BlobLifetimeRuntimeConformanceAccessV3::Visitor(
                      CopyVisitor, &visit);
              result = lease.VisitBoundMaterializedBytes(visitor);
            }
            const auto counter =
                dt::BlobLifetimeRuntimeConformanceAccessV3::Counter(
                    fixture.ledger,
                    dt::BlobLifetimeResourceV3::lifetime_callback_calls);
            const bool security =
                code == SB_BLOB_CALLBACK_SECURITY_DENIED_V3;
            const bool exact_result = security
                ? result.disposition ==
                      dt::BlobLifetimeOuterDispositionV3::public_failure &&
                      result.diagnostic_code == "SECURITY.ACCESS_DENIED"
                : result.disposition ==
                      dt::BlobLifetimeOuterDispositionV3::
                          terminated_out_of_band &&
                      result.diagnostic_code.empty();
            const bool exact_counts = phase == 1
                ? CountCall(fixture.fake, 'R') == 1 &&
                      CountCall(fixture.fake, 'L') == 0 &&
                      counter.invoked == 1 &&
                      counter.reserved_cleanup == 1
                : CountCall(fixture.fake, 'B') == 1 &&
                      CountCall(fixture.fake, 'E') == 0 &&
                      CountCall(fixture.fake, 'L') == 1 &&
                      counter.invoked == 2 &&
                      counter.reserved_cleanup == 1;
            return exact_result && exact_counts && !lease.retained() &&
                dt::BlobLifetimeRuntimeConformanceAccessV3::
                    LedgerQuarantined(fixture.ledger) &&
                ExactInvariantTuple(
                    fixture.fake,
                    dt::BlobLifetimeInvariantEventV3::
                        callback_budget_counter_corrupt,
                    phase == 1 ? dt::BlobLifetimePhaseV3::retain
                               : dt::BlobLifetimePhaseV3::begin_access,
                    2, 255);
          }))
        ++failures;
    }
  }
  return failures;
}

bool ExactPreservedResourcePrimary(const dt::BlobLifetimeRuntimeResultV3& result) {
  const auto expected = ResourceVisitor(nullptr, {}).fact;
  return result.disposition == dt::BlobLifetimeOuterDispositionV3::public_failure &&
      result.diagnostic_code == "RESOURCE.BUDGET_EXCEEDED" &&
      result.reason.empty() && result.diagnostic_uuid == expected.diagnostic_uuid &&
      result.diagnostic_generation == expected.diagnostic_generation &&
      result.fact.fact_class == expected.fact_class &&
      result.fact.diagnostic == expected.diagnostic &&
      result.fact.reason == expected.reason &&
      result.fact.parameters.present == expected.parameters.present &&
      result.fact.parameters.resource_enum == expected.parameters.resource_enum &&
      result.fact.parameters.required_u64 == 2 &&
      result.fact.parameters.available_u64 == 1;
}

std::size_t PriorPrimaryPrivateFailureRegressions() {
  std::size_t failures = 0;
  for (const bool throw_end : {true, false}) {
    const auto name = throw_end
        ? std::string_view{"prior_primary_then_end_adapter_exception"}
        : std::string_view{"prior_primary_then_release_adapter_exception"};
    if (!RunIsolatedRegression(name, [throw_end] {
          Fixture fixture(16);
          const std::array<platform::byte, 1> bytes{{0x42}};
          auto carrier = dt::BlobLifetimeRuntimeConformanceAccessV3::Carrier(
              fixture.carrier_binding, dt::BlobValueStateV3::value,
              bytes.data(), bytes.size());
          dt::BlobRetainedLifetimeLeaseV3 lease;
          if (!dt::RetainBaseBlobLifetimeLeaseV3Generation1(
                   fixture.profile, fixture.Admission(), carrier,
                   fixture.token, fixture.request, lease).ok())
            return false;
          fixture.fake.call_count = 0;
          fixture.fake.invariant_calls = 0;
          fixture.fake.throw_end = throw_end;
          fixture.fake.throw_release = !throw_end;
          auto visitor = dt::BlobLifetimeRuntimeConformanceAccessV3::Visitor(
              ResourceVisitor, nullptr);
          const auto result = lease.VisitBoundMaterializedBytes(visitor);
          return ExactPreservedResourcePrimary(result) &&
              CountCall(fixture.fake, 'E') == 1 &&
              CountCall(fixture.fake, 'L') == 1 &&
              ExactInvariantTuple(
                  fixture.fake,
                  dt::BlobLifetimeInvariantEventV3::adapter_exception,
                  throw_end ? dt::BlobLifetimePhaseV3::end_access
                            : dt::BlobLifetimePhaseV3::release,
                  2, 255);
        }))
      ++failures;
  }

  if (!RunIsolatedRegression("prior_primary_then_corrupt_cleanup", [] {
        Fixture fixture(16);
        const std::array<platform::byte, 1> bytes{{0x42}};
        auto carrier = dt::BlobLifetimeRuntimeConformanceAccessV3::Carrier(
            fixture.carrier_binding, dt::BlobValueStateV3::value,
            bytes.data(), bytes.size());
        dt::BlobRetainedLifetimeLeaseV3 lease;
        if (!dt::RetainBaseBlobLifetimeLeaseV3Generation1(
                 fixture.profile, fixture.Admission(), carrier, fixture.token,
                 fixture.request, lease).ok())
          return false;
        fixture.fake.call_count = 0;
        fixture.fake.invariant_calls = 0;
        auto visitor = dt::BlobLifetimeRuntimeConformanceAccessV3::Visitor(
            CorruptLedgerResourceVisitor, &fixture.ledger);
        const auto result = lease.VisitBoundMaterializedBytes(visitor);
        const auto counter =
            dt::BlobLifetimeRuntimeConformanceAccessV3::Counter(
                fixture.ledger,
                dt::BlobLifetimeResourceV3::lifetime_callback_calls);
        return ExactPreservedResourcePrimary(result) &&
            CountCall(fixture.fake, 'E') == 1 &&
            CountCall(fixture.fake, 'L') == 1 &&
            counter.limit == 0 && counter.invoked == 2 &&
            counter.reserved_cleanup == 0 &&
            dt::BlobLifetimeRuntimeConformanceAccessV3::LedgerQuarantined(
                fixture.ledger) &&
            ExactInvariantTuple(
                fixture.fake,
                dt::BlobLifetimeInvariantEventV3::
                    callback_budget_counter_corrupt,
                dt::BlobLifetimePhaseV3::end_access, 2, 255);
      }))
    ++failures;
  return failures;
}

std::size_t DestructorProtocolRegressions() {
  struct Row {
    std::string_view name;
    std::uint8_t stimulus;
    dt::BlobLifetimeInvariantEventV3 event;
    std::uint8_t callback;
  };
  constexpr std::array<Row, 3> rows{{
      {"destructor_release_adapter_exception", 0,
       dt::BlobLifetimeInvariantEventV3::adapter_exception, 255},
      {"destructor_release_malformed_result", 1,
       dt::BlobLifetimeInvariantEventV3::callback_result_unknown, 255},
      {"destructor_release_dirty_ticket", 10,
       dt::BlobLifetimeInvariantEventV3::dirty_failure_output, 0},
  }};
  std::size_t failures = 0;
  for (const auto& row : rows) {
    if (!RunIsolatedRegression(row.name, [row] {
          Fixture fixture(16);
          auto carrier = dt::BlobLifetimeRuntimeConformanceAccessV3::Carrier(
              fixture.carrier_binding, dt::BlobValueStateV3::value, nullptr,
              0);
          {
            dt::BlobRetainedLifetimeLeaseV3 lease;
            if (!dt::RetainBaseBlobLifetimeLeaseV3Generation1(
                     fixture.profile, fixture.Admission(), carrier,
                     fixture.token, fixture.request, lease).ok())
              return false;
            fixture.fake.call_count = 0;
            fixture.fake.invariant_calls = 0;
            if (row.stimulus == 0)
              fixture.fake.throw_release = true;
            else {
              fixture.fake.malformed_phase = 5;
              fixture.fake.malformed_stimulus = row.stimulus;
            }
          }
          return CountCall(fixture.fake, 'L') == 1 &&
              ExactInvariantTuple(
                  fixture.fake, row.event,
                  dt::BlobLifetimePhaseV3::destructor_cleanup, 19,
                  row.callback);
        }))
      ++failures;
  }
  return failures;
}

std::size_t CloneCallbackReentryRegressions() {
  std::size_t failures = 0;
  for (const std::uint8_t action : {std::uint8_t{4}, std::uint8_t{5}}) {
    const auto name = action == 4
        ? std::string_view{"outer_clone_reentry_source_operand"}
        : std::string_view{"outer_clone_reentry_destination_operand"};
    if (!RunIsolatedRegression(name, [action] {
          return OuterCloneCallbackReentryCase(action);
        }))
      ++failures;
  }
  return failures;
}

[[maybe_unused]] std::size_t KnownGapRegressions() {
  return ReleaseOuterOperationRegressions() +
         ReservedSamplerPrecedenceRegressions() +
         DirtyTicketRegressions() + AdapterExceptionRegressions() +
         AdapterExceptionOutputMatrix() +
         ProtectedProtocolRegressions() +
         MonotonicAndCallbackQuarantineRegressions() +
         CorruptLedgerRegression() + MoveAtomicityRegression() +
         CleanupPrecedenceRegression() +
         CancelCleanupCorruptionRegressions() +
         PriorPrimaryPrivateFailureRegressions() +
         DestructorProtocolRegressions() +
         CloneCallbackReentryRegressions();
}

void Exact95CallbackDiagnosticMaterialization() {
  static_assert(static_cast<std::uint8_t>(
                    dt::BlobLifetimeReasonV3::authority_table_missing) == 1);
  static_assert(static_cast<std::uint8_t>(
                    dt::BlobLifetimeReasonV3::callback_output_impossible) ==
                12);
  static_assert(static_cast<std::uint8_t>(
                    dt::BlobLifetimeReasonV3::same_ticket_concurrent_use) ==
                15);
  static_assert(static_cast<std::uint8_t>(
                    dt::BlobLifetimeReasonV3::authority_capacity_exhausted) ==
                26);
  static_assert(static_cast<std::uint8_t>(
                    dt::BlobLifetimeReasonV3::visitor_missing) == 27);
  static_assert(static_cast<std::uint8_t>(
                    dt::BlobLifetimeReasonV3::destination_armed) == 29);
  enum class Phase : std::uint8_t {
    retain, probe, begin_access, end_access, release
  };
  constexpr std::array<Phase, 5> phases{{
      Phase::retain, Phase::probe, Phase::begin_access,
      Phase::end_access, Phase::release}};
  for (const auto phase : phases) {
    for (std::uint8_t code = 0; code <= 18; ++code) {
      Fixture fixture(32);
      const std::array<platform::byte, 1> bytes{{0x45}};
      auto carrier = dt::BlobLifetimeRuntimeConformanceAccessV3::Carrier(
          fixture.carrier_binding, dt::BlobValueStateV3::value,
          bytes.data(), bytes.size());
      dt::BlobRetainedLifetimeLeaseV3 lease;
      dt::BlobLifetimeRuntimeResultV3 result;
      if (phase == Phase::retain) {
        fixture.fake.retain_code = code;
        result = dt::RetainBaseBlobLifetimeLeaseV3Generation1(
            fixture.profile, fixture.Admission(), carrier, fixture.token,
            fixture.request, lease);
      } else {
        Check(dt::RetainBaseBlobLifetimeLeaseV3Generation1(
                  fixture.profile, fixture.Admission(), carrier,
                  fixture.token, fixture.request, lease).ok(),
              "95-map setup retain");
        VisitContext context;
        auto visitor = dt::BlobLifetimeRuntimeConformanceAccessV3::Visitor(
            CopyVisitor, &context);
        if (phase == Phase::probe) {
          fixture.fake.probe_code = code;
          result = lease.Probe();
        } else if (phase == Phase::begin_access) {
          fixture.fake.begin_code = code;
          result = lease.VisitBoundMaterializedBytes(visitor);
        } else if (phase == Phase::end_access) {
          fixture.fake.end_code = code;
          result = lease.VisitBoundMaterializedBytes(visitor);
        } else {
          fixture.fake.release_code = code;
          result = lease.Release();
        }
      }
      if (code == 0) {
        Check(result.ok(), "95-map OK row");
        if (lease.retained()) Check(lease.Release().ok(), "95-map OK cleanup");
        continue;
      }
      Check(!result.ok() && result.callback_code == code,
            "95-map non-OK result/code");
      const bool cleanup = phase == Phase::end_access ||
                           phase == Phase::release;
      dt::BlobLifetimeFactClassV3 expected_class =
          dt::BlobLifetimeFactClassV3::callback_protocol;
      if (code == 2 || (code >= 7 && code <= 10) || code == 12 ||
          ((code == 13 || code == 14) && phase != Phase::retain))
        expected_class = dt::BlobLifetimeFactClassV3::snapshot;
      else if (code == 3 || code == 5 || code == 6 || code == 11)
        expected_class = dt::BlobLifetimeFactClassV3::binding;
      else if (code == 4)
        expected_class = dt::BlobLifetimeFactClassV3::security;
      else if (code == 17 || code == 18)
        expected_class = cleanup
            ? dt::BlobLifetimeFactClassV3::cleanup_protocol
            : dt::BlobLifetimeFactClassV3::lifetime_authority_unavailable;
      Check(result.fact.fact_class == expected_class,
            "95-map exact fact class");
      Check(result.fact.stage ==
                (cleanup ? dt::BlobLifetimeFactStageV3::cleanup_return
                         : dt::BlobLifetimeFactStageV3::callback_return) &&
                !result.fact.gate_present,
            "95-map stage/gate");
      const auto& p = result.fact.parameters;
      constexpr auto descriptor = dt::blob_parameter_descriptor_identity;
      constexpr auto lifetime = dt::blob_parameter_lifetime |
                                dt::blob_parameter_expiry_reason;
      if (code == 2 || (code >= 7 && code <= 10) || code == 12 ||
          ((code == 13 || code == 14) && phase != Phase::retain)) {
        Check(p.present == lifetime, "95-map lifetime presence");
        Check(p.lifetime_enum !=
                  dt::BlobLifetimeDiagnosticLifetimeV3::none &&
              p.expiry_reason_enum !=
                  dt::BlobLifetimeDiagnosticExpiryReasonV3::none,
              "95-map lifetime values");
        dt::BlobLifetimeDiagnosticLifetimeV3 expected_lifetime{};
        dt::BlobLifetimeDiagnosticExpiryReasonV3 expected_expiry{};
        switch (code) {
          case 2:
            expected_lifetime =
                dt::BlobLifetimeDiagnosticLifetimeV3::unknown_token;
            expected_expiry =
                dt::BlobLifetimeDiagnosticExpiryReasonV3::unknown_token;
            break;
          case 7:
            expected_lifetime =
                dt::BlobLifetimeDiagnosticLifetimeV3::wrong_snapshot;
            expected_expiry = dt::BlobLifetimeDiagnosticExpiryReasonV3::
                snapshot_identity_mismatch;
            break;
          case 8:
            expected_lifetime =
                dt::BlobLifetimeDiagnosticLifetimeV3::stale_snapshot;
            expected_expiry = dt::BlobLifetimeDiagnosticExpiryReasonV3::
                snapshot_no_longer_current;
            break;
          case 9:
            expected_lifetime =
                dt::BlobLifetimeDiagnosticLifetimeV3::token_expired;
            expected_expiry =
                dt::BlobLifetimeDiagnosticExpiryReasonV3::monotonic_expiry;
            break;
          case 10:
            expected_lifetime =
                dt::BlobLifetimeDiagnosticLifetimeV3::token_revoked;
            expected_expiry = dt::BlobLifetimeDiagnosticExpiryReasonV3::
                explicit_revocation;
            break;
          case 12:
            expected_lifetime =
                dt::BlobLifetimeDiagnosticLifetimeV3::owner_closed;
            expected_expiry =
                dt::BlobLifetimeDiagnosticExpiryReasonV3::owner_closed;
            break;
          case 13:
            expected_lifetime = dt::BlobLifetimeDiagnosticLifetimeV3::
                retain_ticket_invalid;
            expected_expiry = dt::BlobLifetimeDiagnosticExpiryReasonV3::
                retain_ticket_unknown;
            break;
          case 14:
            expected_lifetime = dt::BlobLifetimeDiagnosticLifetimeV3::
                retain_ticket_consumed;
            expected_expiry = dt::BlobLifetimeDiagnosticExpiryReasonV3::
                retain_ticket_consumed;
            break;
          default:
            break;
        }
        Check(p.lifetime_enum == expected_lifetime &&
                  p.expiry_reason_enum == expected_expiry,
              "95-map exact lifetime pair");
      } else if (code == 5) {
        Check(p.present == dt::blob_parameter_owner_class &&
                  p.owner_class_enum == fixture.request.required_owner_class,
              "95-map owner");
      } else if (code == 11) {
        Check(p.present == (dt::blob_parameter_open_mode |
                            dt::blob_parameter_operation) &&
                  p.open_mode_enum == 4 &&
                  p.operation_enum == fixture.request.operation,
              "95-map mode/operation");
      } else if (code == 17 || code == 18) {
        if (cleanup) {
          Check(result.fact.fact_class ==
                    dt::BlobLifetimeFactClassV3::cleanup_protocol &&
                    p.present == descriptor,
                "95-map cleanup17/18");
        } else {
          Check(result.fact.fact_class ==
                    dt::BlobLifetimeFactClassV3::
                        lifetime_authority_unavailable &&
                    p.present == (dt::blob_parameter_operation |
                                  dt::blob_parameter_phase),
                "95-map ordinary17/18");
        }
      } else if (code == 4) {
        Check(result.fact.fact_class ==
                  dt::BlobLifetimeFactClassV3::security && p.present == 0,
              "95-map security");
      } else {
        Check(p.present == descriptor, "95-map descriptor protocol");
      }
      if (p.present == descriptor) {
        Check(p.descriptor_uuid == fixture.profile.identity.descriptor_uuid &&
                  p.descriptor_generation ==
                      fixture.profile.identity.descriptor_generation,
              "95-map descriptor identity");
      }
      if (code == 15) {
        Check(result.fact.reason ==
                  (phase == Phase::end_access
                       ? dt::BlobLifetimeReasonV3::access_ticket_invalid
                       : dt::BlobLifetimeReasonV3::callback_output_impossible),
              "95-map code15 phase reason");
      }
      if (code == 16) {
        Check(result.fact.reason ==
                  (phase == Phase::end_access
                       ? dt::BlobLifetimeReasonV3::access_ticket_consumed
                       : dt::BlobLifetimeReasonV3::callback_output_impossible),
              "95-map code16 phase reason");
      }
      dt::BlobLifetimeReasonV3 expected_reason =
          dt::BlobLifetimeReasonV3::none;
      switch (code) {
        case 1:
          expected_reason =
              dt::BlobLifetimeReasonV3::callback_output_impossible;
          break;
        case 3:
          expected_reason =
              dt::BlobLifetimeReasonV3::wrong_authority_instance;
          break;
        case 5:
          expected_reason =
              dt::BlobLifetimeReasonV3::owner_binding_mismatch;
          break;
        case 6:
          expected_reason = dt::BlobLifetimeReasonV3::transaction_mismatch;
          break;
        case 13:
        case 14:
          if (phase == Phase::retain)
            expected_reason =
                dt::BlobLifetimeReasonV3::callback_output_impossible;
          break;
        case 15:
          expected_reason = phase == Phase::end_access
              ? dt::BlobLifetimeReasonV3::access_ticket_invalid
              : dt::BlobLifetimeReasonV3::callback_output_impossible;
          break;
        case 16:
          expected_reason = phase == Phase::end_access
              ? dt::BlobLifetimeReasonV3::access_ticket_consumed
              : dt::BlobLifetimeReasonV3::callback_output_impossible;
          break;
        case 17:
          expected_reason = cleanup
              ? (phase == Phase::end_access
                    ? dt::BlobLifetimeReasonV3::end_access_failed
                    : dt::BlobLifetimeReasonV3::release_failed)
              : dt::BlobLifetimeReasonV3::authority_capacity_exhausted;
          break;
        case 18:
          expected_reason = cleanup
              ? (phase == Phase::end_access
                    ? dt::BlobLifetimeReasonV3::end_access_failed
                    : dt::BlobLifetimeReasonV3::release_failed)
              : dt::BlobLifetimeReasonV3::authority_runtime_unavailable;
          break;
        default:
          break;
      }
      Check(result.fact.reason == expected_reason,
            "95-map exact reason or forbidden-zero");
    }
  }
}

dt::BlobMaterializedLengthResultV3 AdmittedLength(
    Fixture& fixture, dt::BlobValueStateV3 state,
    std::span<const platform::byte> bytes, bool null_allowed = true,
    unsigned post_admission_mutation = 0, bool use_scoped_consumer = false) {
  auto budget = dt::BlobLifetimeReceiverHostV3Generation1::BudgetControl(
      fixture.ledger, fixture.pins);
  auto capability = dt::BlobLifetimeReceiverHostV3Generation1::Capability(
      fixture.carrier_binding, state, bytes.data(), bytes.size());
  dt::BlobLifetimeAuthorityAdmissionV3Generation1 admission;
  dt::BlobBoundMaterializedCarrierV3Generation1 carrier;
  const auto admitted = dt::BlobLifetimeRuntimeFactoryV3Generation1::AdmitMaterialized(
      SB_BLOB_OP_READ_V3, fixture.profile, &fixture.authority, &fixture.services,
      fixture.pins, budget, fixture.services.monotonic_clock,
      fixture.services.monotonic_clock, fixture.token, fixture.request,
      capability, admission, carrier);
  if (!admitted.ok()) return {admitted};
  if (post_admission_mutation == 1) ++fixture.profile.receipt.registry_generation;
  if (post_admission_mutation == 2) ++fixture.token.immutable_binding_generation;
  if (use_scoped_consumer) {
    VisitContext context;
    auto visitor = dt::BlobLifetimeRuntimeConformanceAccessV3::Visitor(
        CopyVisitor, &context);
    return {dt::ConsumeBlobMaterializedValueScopedV3Generation1(
        fixture.profile, carrier, std::move(admission), fixture.token,
        fixture.request, visitor, null_allowed)};
  }
  return dt::ReadBlobMaterializedLengthV3Generation1(
      fixture.profile, carrier, std::move(admission), fixture.token,
      fixture.request, null_allowed);
}

void AuthenticatedMaterializedLength() {
  const std::array<platform::byte, 4096> content{};
  for (const auto state : {dt::BlobValueStateV3::value,
                          dt::BlobValueStateV3::sql_null}) {
    for (const auto length : {std::size_t{0}, std::size_t{1}, content.size()}) {
      if (state == dt::BlobValueStateV3::sql_null && length != 0) continue;
      Fixture fixture(3);
      // Zero content-read allowance proves length never traverses the payload.
      dt::BlobLifetimeRuntimeConformanceAccessV3::SetCounter(
          fixture.ledger, dt::BlobLifetimeResourceV3::logical_bytes_read, 0, 0, 0);
      const auto bytes = length == 0 ? std::span<const platform::byte>{}
                                    : std::span{content}.first(length);
      const auto before = allocation_probe::calls.load();
      allocation_probe::enabled.store(true);
      const auto result = AdmittedLength(fixture, state, bytes);
      allocation_probe::enabled.store(false);
      Check(allocation_probe::calls.load() == before,
            "authenticated length allocated memory");
      Check(result.ok() && result.length == length &&
                result.is_null == (state == dt::BlobValueStateV3::sql_null),
            "authenticated length conflated NULL/empty or lost exact length");
      Check(fixture.fake.call_count == 3 && fixture.fake.calls[0] == 'R' &&
                fixture.fake.calls[1] == 'P' && fixture.fake.calls[2] == 'L' &&
                fixture.fake.pin_drop_calls == 2 && fixture.fake.pins == 1,
            "length did not retain/probe/release and drain both pins exactly");
      Check(dt::BlobLifetimeRuntimeConformanceAccessV3::Invoked(
                fixture.ledger, dt::BlobLifetimeResourceV3::logical_bytes_read) == 0,
            "length charged a content read");
    }
  }
  for (std::uint64_t budget = 0; budget != 3; ++budget) {
    Fixture fixture(budget);
    const auto result = AdmittedLength(fixture, dt::BlobValueStateV3::value, content);
    Check(!result.ok() && result.length == 0 && !result.is_null &&
              result.runtime.diagnostic_code == "RESOURCE.BUDGET_EXCEEDED",
          "length ignored callback budget or published failed metadata");
    Check(CountCall(fixture.fake, 'R') == (budget == 2 ? 1 : 0) &&
              CountCall(fixture.fake, 'P') == 0 &&
              CountCall(fixture.fake, 'L') == (budget == 2 ? 1 : 0) &&
              fixture.fake.pin_drop_calls == 2,
          "length callback shortage lost reserved cleanup");
  }
  for (const bool scoped_consumer : {false, true}) {
    for (const unsigned mutation : {1U, 2U}) {
      Fixture fixture(3);
      const auto result = AdmittedLength(fixture, dt::BlobValueStateV3::sql_null,
                                        {}, false, mutation, scoped_consumer);
      Check(!result.ok() && !result.is_null && result.length == 0 &&
                fixture.fake.call_count == 0 && fixture.fake.pin_drop_calls == 2,
            "NULL shortcut bypassed profile/binding validation");
      if (mutation == 1)
        Check(result.runtime.diagnostic_code == "CINL.LOB.DESCRIPTOR_INVALID" &&
                  result.runtime.reason == "authority_profile_binding_mismatch",
              "nonnullable NULL outranked stale profile");
      else
        Check(result.runtime.disposition ==
                  dt::BlobLifetimeOuterDispositionV3::terminated_out_of_band,
              "nonnullable NULL hid broken private admission");
    }
  }
  {
    Fixture fixture(3);
    fixture.fake.poison_request_on_drop = &fixture.request;
    const auto result = AdmittedLength(fixture, dt::BlobValueStateV3::value, content);
    Check(result.ok() && result.length == content.size() && !result.is_null &&
              fixture.request.operation == 0 && fixture.fake.pin_drop_calls == 2,
          "length depended on request after final unpin");
  }
  {
    Fixture fixture(3);
    const auto result = AdmittedLength(fixture, dt::BlobValueStateV3::sql_null,
                                      {}, false);
    Check(!result.ok() && result.runtime.diagnostic_code == "BLOB.STATE_INVALID" &&
              result.runtime.fact.parameters.supplied_state_u8 == 1 &&
              result.runtime.fact.parameters.operation_enum == SB_BLOB_OP_READ_V3 &&
              result.length == 0 && !result.is_null &&
              fixture.fake.call_count == 0 && fixture.fake.pin_drop_calls == 2,
          "nonnullable length did not refuse before lifetime callbacks");
  }
  // Exercise all legal callback failures at every callback used by length.
  // The existing 95-cell diagnostic suite independently verifies each mapping.
  for (const auto phase : {1, 2, 5}) {
    for (std::uint8_t code = 1; code <= 18; ++code) {
      Fixture fixture(3);
      if (phase == 1) fixture.fake.retain_code = code;
      if (phase == 2) fixture.fake.probe_code = code;
      if (phase == 5) fixture.fake.release_code = code;
      const auto result = AdmittedLength(fixture, dt::BlobValueStateV3::value, content);
      Check(!result.ok() && !result.is_null && result.length == 0,
            "lifetime failure published length");
      Check(CountCall(fixture.fake, 'L') == (phase == 1 ? 0 : 1) &&
                CountCall(fixture.fake, 'B') == 0 &&
                CountCall(fixture.fake, 'E') == 0 &&
                fixture.fake.pin_drop_calls == 2,
            "length failure repeated cleanup or accessed bytes");
    }
  }
  for (const bool security_cleanup : {false, true}) {
    Fixture fixture(3);
    fixture.fake.probe_code = SB_BLOB_CALLBACK_STALE_SNAPSHOT_V3;
    fixture.fake.release_code = security_cleanup ? SB_BLOB_CALLBACK_SECURITY_DENIED_V3
                                               : SB_BLOB_CALLBACK_AUTHORITY_UNAVAILABLE_V3;
    const auto result = AdmittedLength(fixture, dt::BlobValueStateV3::value, content);
    Check(!result.ok() && result.length == 0 &&
              result.runtime.diagnostic_code == (security_cleanup
                  ? "SECURITY.ACCESS_DENIED" : "CINL.LOB.HANDLE_EXPIRED") &&
              CountCall(fixture.fake, 'L') == 1,
          "length lost sticky primary or cleanup SECURITY override");
  }
  {
    Fixture fixture(3);
    fixture.fake.cancel_on_release = true;
    const auto result = AdmittedLength(fixture, dt::BlobValueStateV3::value, content);
    Check(!result.ok() && result.runtime.diagnostic_code == "PROCESS.CANCELLED" &&
              result.length == 0 && !result.is_null &&
              CountCall(fixture.fake, 'L') == 1,
          "length published before final post-release cancellation gate");
  }
  {
    Fixture fixture(3);
    fixture.fake.throw_release = true;
    const auto result = AdmittedLength(fixture, dt::BlobValueStateV3::value, content);
    Check(result.runtime.disposition ==
              dt::BlobLifetimeOuterDispositionV3::terminated_out_of_band &&
              result.length == 0 && !result.is_null &&
              fixture.fake.invariant_calls == 1 && fixture.fake.pin_drop_calls == 2,
          "length published after release exception or lost quarantine");
  }
}

} // namespace

int main(int argc, char** argv) {
#if defined(__linux__)
  // RLIMIT_CORE alone does not stop a piped systemd core collector. Death-test
  // children inherit this setting and must not leave system-owned artifacts.
  if (prctl(PR_SET_DUMPABLE, 0) != 0) Fail("cannot disable test core dumps");
#endif
  const std::string_view mode = argc > 1 ? std::string_view{argv[1]}
                                         : std::string_view{argv[0]};
  if (mode == "factory") {
    FactoryEarlyGateObservability();
    FactoryStateAndClockBoundaries();
    PinSelectorObservability();
    FactoryDiagnosticsBeforeUnpin();
    FactoryAdmissionPrecedence();
    FactoryF02AndRepresentability();
    PinResultProtocol();
    std::cout << "PASS base.blob factory admission checks=" << checks << '\n';
    return EXIT_SUCCESS;
  }
  if (mode == "deferred") {
    DeferredCallbackObligationMatrix();
    DeferredProbePrimaryMatrix();
    std::cout << "PASS deferred callback obligations checks=" << checks << '\n';
    return EXIT_SUCCESS;
  }
  if (mode == "active_second") {
    ActiveSecondClaimDebuggerCases();
    std::cout << "PASS active second-claim cases checks=" << checks << '\n';
    return EXIT_SUCCESS;
  }
  if (mode == "retain_publication") {
    RetainPublicationDebuggerCases();
    std::cout << "PASS retain/probe publication interleavings checks=" << checks << '\n';
    return EXIT_SUCCESS;
  }
  if (mode == "transfers") {
    TransferAdmissionReservation();
    TransferPairAdmissionRollback();
    TransferPublicationOrdering();
    CloneSourceWinnerCleansRetainedDestination();
    std::cout << "PASS base.blob transfer publication checks=" << checks << '\n';
    return EXIT_SUCCESS;
  }
  if (mode.find("malformed") != std::string_view::npos) {
    const auto failures = ExactMalformedRows();
    if (failures != 0) return EXIT_FAILURE;
    std::cout << "PASS base.blob V7 malformed runtime checks=" << checks
              << '\n';
    return EXIT_SUCCESS;
  }
  if (mode.find("known_gaps") != std::string_view::npos) {
    const auto failures = KnownGapRegressions();
    if (failures != 0) {
      std::cerr << "KNOWN_GAP_FAILURES total=" << failures << '\n';
      return EXIT_FAILURE;
    }
    std::cout << "PASS base.blob V7 targeted regressions checks=" << checks
              << '\n';
    return EXIT_SUCCESS;
  }
  if (mode.find("contention") != std::string_view::npos) {
    TwoThreadCloneMoveGuardContention();
    std::cout << "PASS base.blob V7 contention checks=" << checks << '\n';
    return EXIT_SUCCESS;
  }
  ProductionPrecedence();
  AuthenticatedMaterializedLength();
  NullVisitorStatePrecedence();
  FactoryEarlyGateObservability();
  FactoryStateAndClockBoundaries();
  FailureBuilderFailsClosed();
  ExactInvariantDomain();
  FactoryF02AndRepresentability();
  FactoryDiagnosticsBeforeUnpin();
  FactoryAdmissionPrecedence();
  ExactF02OrdinaryGates();
  ExactF02CallbackReturns();
  Exact95CallbackDiagnosticMaterialization();
  PinResultProtocol();
  PinSelectorObservability();
  AllocationFreeMaterializedPath();
  BudgetConfigureFreezeLifecycle();
  TwoThreadCloneMoveGuardContention();
  AtomicWinnerReceiptProtocol();
  GenerationResetProtocol();
  TransferAdmissionReservation();
  TransferPairAdmissionRollback();
  PreCloseEntrantQuiescence();
  CloneSourceWinnerCleansRetainedDestination();
  TransferPublicationOrdering();
  PostCallbackGateAndServiceReceiptBarrier();
  SameBindingIndependentReceipts();
  PublicationSuppressionIsOperationLocal();
  CleanBindingReuse();
  AdministrativeCallbackReentry();
  RetainReentryUsesCanonicalDescriptor();
  DeferredCallbackObligationMatrix();
  DeferredProbePrimaryMatrix();
  ProtectedFactAuthorityFailClosed();
  // These were historically separate "known gaps". They are mandatory
  // regressions now: the default suite must fail if any of them regresses.
  const auto mandatory_failures = KnownGapRegressions() + ExactMalformedRows();
  if (mandatory_failures != 0) return EXIT_FAILURE;
  std::cout << "PASS base.blob V7 runtime integration checks=" << checks << '\n';
  return EXIT_SUCCESS;
}
