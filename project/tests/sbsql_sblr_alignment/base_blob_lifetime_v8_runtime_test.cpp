// Copyright (c) 2026 ScratchBird Software Inc.
// SPDX-License-Identifier: MPL-2.0

#include "datatype_blob.hpp"
#include "datatype_blob_component.hpp"
#include "datatype_operations.hpp"

#include <algorithm>
#include <array>
#include <atomic>
#include <cstddef>
#include <cstdlib>
#include <exception>
#include <iostream>
#include <limits>
#include <memory>
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
                               u64 limit, u64 relative_timeout_ns = 0) noexcept {
    BlobLifetimeBudgetConfigurationV3Generation1 configuration{};
    configuration.limits.fill(std::numeric_limits<u64>::max());
    configuration.limits[static_cast<std::size_t>(
        BlobLifetimeResourceV3::lifetime_callback_calls) - 1] = limit;
    configuration.admitted_relative_timeout_ns = relative_timeout_ns;
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
    return ledger.Snapshot();
  }
  static bool Charge(BlobLifetimeBudgetLedgerV3Generation1& ledger,
                     BlobLifetimeResourceV3 resource, u64 amount,
                     u64* available) noexcept {
    return ledger.Charge(resource, amount, available);
  }
  static bool ReserveSink(BlobLifetimeBudgetLedgerV3Generation1& ledger,u64 bytes,u64 calls,
      BlobLifetimeResourceV3& resource,u64& available) noexcept { return ledger.ReserveSinkWork(bytes,calls,resource,available); }
  static bool ConsumeSink(BlobLifetimeBudgetLedgerV3Generation1& ledger,u64 bytes) noexcept {
    return ledger.ConsumeReservedSinkCall(bytes);
  }
  static bool CancelSink(BlobLifetimeBudgetLedgerV3Generation1& ledger,u64 bytes,u64 calls) noexcept {
    return ledger.CancelSinkWork(bytes,calls);
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
  static bool ScalarMatches(const BlobRetainedLifetimeLeaseV3& lease,
                            BlobValueStateV3 state, const byte* bytes) noexcept {
    return lease.carrier_state_ == state && lease.carrier_data_ == bytes;
  }

  static bool LogicalRoutingMatches(const BlobRetainedLifetimeLeaseV3& lease,
                                    void* context, BlobReadAtV3 read_at) noexcept {
    return lease.carrier_binding_.carrier_kind == SB_BLOB_MODE_LOGICAL_READER_V3 &&
        lease.carrier_data_ == nullptr &&
        lease.carrier_reader_context_ == context &&
        lease.carrier_read_at_ == read_at;
  }

  static bool ReaderRoutingCleared(const BlobRetainedLifetimeLeaseV3& lease)
      noexcept {
    return lease.carrier_reader_context_ == nullptr &&
        lease.carrier_read_at_ == nullptr && lease.carrier_encoded_extent_ == 0;
  }

  static bool EncodedRoutingMatches(const BlobRetainedLifetimeLeaseV3& lease,
      void* context, BlobReadAtV3 read_at, u64 extent) noexcept {
    return lease.carrier_binding_.carrier_kind == SB_BLOB_MODE_OPAQUE_ENCODED_READER_V3 &&
        lease.carrier_data_ == nullptr && lease.carrier_logical_length_ == 0 &&
        lease.carrier_reader_context_ == context && lease.carrier_read_at_ == read_at &&
        lease.carrier_encoded_extent_ == extent;
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

  static auto ControlClaimTransfer(BlobRetainedLifetimeLeaseV3& lease,
                                    u8 operation = 16) noexcept {
    return lease.state_.ClaimIdleForTransfer(operation);
  }
  static BlobLifetimeRuntimeResultV3 ReleaseTransferOwner(
      BlobRetainedLifetimeLeaseV3& lease, u8 operation) noexcept {
    return lease.ReleaseOwned(BlobLifetimePhaseV3::release,operation);
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

  // Test capabilities belong to the existing conformance accessor, not the
  // reserved production receiver-host identity. A different class definition
  // under that production name would violate ODR once the real host is linked.
  static BlobReceiverEncodedReaderCapabilityV3Generation1 EncodedCapability(
      const BlobCarrierBindingV3& binding, u64 extent,
      void* context, BlobReadAtV3 read_at) noexcept {
    return BlobReceiverEncodedReaderCapabilityV3Generation1(binding, extent, context, read_at);
  }
  static BlobReceiverEncodedReaderCapabilityV3Generation1* EncodedCapabilityAt(
      void* storage, const BlobCarrierBindingV3& binding, void* context,
      BlobReadAtV3 read_at) noexcept {
    return new (storage) BlobReceiverEncodedReaderCapabilityV3Generation1(
        binding, UINT64_MAX, context, read_at);
  }
  static BlobReceiverLogicalReaderCapabilityV3Generation1 LogicalCapability(
      const BlobCarrierBindingV3& binding, BlobValueStateV3 state, u64 length,
      void* context, BlobReadAtV3 read_at) noexcept {
    return BlobReceiverLogicalReaderCapabilityV3Generation1(
        binding, state, length, context, read_at);
  }
  static BlobReceiverLogicalReaderCapabilityV3Generation1* LogicalCapabilityAt(
      void* storage, const BlobCarrierBindingV3& binding,
      void* context, BlobReadAtV3 read_at) noexcept {
    return new (storage) BlobReceiverLogicalReaderCapabilityV3Generation1(
        binding, BlobValueStateV3::value, 0, context, read_at);
  }
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
  void* poison_reader_scratch_on_last_drop = nullptr;
  std::size_t poison_reader_scratch_size = 0;
  dt::BlobReceiverMaterializedCapabilityV3Generation1* poison_capability_on_drop = nullptr;
  // A short-read two-pass traversal can exceed32 lifetime callbacks. Keep
  // complete bounded evidence; never silently truncate the recorded sequence.
  std::array<char, 128> calls{};
  std::size_t call_count = 0;
  std::uint8_t next_ticket = 0x90;
  std::uint64_t now = 100;
  std::uint64_t pins = 1;
  std::uint64_t invariant_calls = 0;
  std::uint64_t clock_calls = 0;
  std::uint64_t clock_action_call = 0;
  void (*clock_action)(void*) noexcept = nullptr;
  void* clock_action_context = nullptr;
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
  bool cancel_on_probe = false;
  std::uint64_t clock_on_probe = 0;
  std::atomic<bool> block_probe{false};
  std::atomic<bool> probe_entered{false};
  std::atomic<bool> finish_probe{false};
  bool cancel_on_next_retain = false;
  bool block_release = false;
  std::atomic<bool> release_entered{false};
  std::atomic<bool> finish_release{false};
  bool block_clone_pin = false;
  std::atomic<bool> clone_pin_entered{false};
  std::atomic<bool> finish_clone_pin{false};
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
  std::uint64_t defer_on_clock_call = 0;
  dt::BlobRetainedLifetimeLeaseV3* defer_clock_lease = nullptr;
  bool defer_clock_private = false;
  dt::BlobLifetimePhaseV3 defer_clock_phase = dt::BlobLifetimePhaseV3::protected_access;
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
    if (call_count == calls.size()) Fail("receiver callback evidence capacity exhausted");
    calls[call_count++] = code;
  }
};

thread_local std::span<platform::byte> reader_destination_to_scrub;

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
  if (fake.cancel_on_next_retain) {
    fake.cancel_on_next_retain = false;
    fake.cancelled = true;
  }
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
  if (fake.block_probe) {
    fake.probe_entered.store(true, std::memory_order_release);
    while (!fake.finish_probe.load(std::memory_order_acquire)) std::this_thread::yield();
  }
  if (fake.cancel_on_probe) fake.cancelled = true;
  if (fake.clock_on_probe != 0) fake.now = fake.clock_on_probe;
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
  if (!reader_destination_to_scrub.empty()) {
    if (!std::all_of(reader_destination_to_scrub.begin(), reader_destination_to_scrub.end(),
                     [](auto b) { return b == 0; }))
      Fail("reader destination remained dirty at end_access");
    reader_destination_to_scrub = {};
  }
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
  if (fake.block_release) {
    fake.release_entered.store(true,std::memory_order_release);
    while (!fake.finish_release.load(std::memory_order_acquire)) std::this_thread::yield();
  }
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
  if (fake.clock_action != nullptr && fake.clock_action_call == fake.clock_calls)
    fake.clock_action(fake.clock_action_context);
  if (fake.defer_on_clock_call == fake.clock_calls && fake.defer_clock_lease != nullptr) {
    if (fake.defer_clock_private)
      (void)dt::BlobLifetimeRuntimeConformanceAccessV3::ControlPublish(*fake.defer_clock_lease,
          dt::BlobLifetimeInvariantEventV3::receiver_services_failed,2,
          fake.defer_clock_phase,255);
    else fake.reentry_result = fake.defer_clock_lease->Probe();
  }
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
  if (fake.block_clone_pin) {
    fake.clone_pin_entered.store(true,std::memory_order_release);
    while (!fake.finish_clone_pin.load(std::memory_order_acquire)) std::this_thread::yield();
  }
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
  if (fake.pins == 0 && fake.poison_reader_scratch_on_last_drop != nullptr &&
      mprotect(fake.poison_reader_scratch_on_last_drop, fake.poison_reader_scratch_size,
               PROT_NONE) != 0) std::terminate();
  if (fake.pin_drop_calls == 2) {
    if (fake.poison_request_on_drop != nullptr) {
      fake.poison_request_on_drop->operation = 0;
      fake.poison_request_on_drop->required_owner_class = 0;
    }
    if (fake.poison_capability_on_drop != nullptr)
      dt::BlobLifetimeRuntimeConformanceAccessV3::PoisonReleasedCapability(
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

  explicit Fixture(std::uint64_t callback_limit, std::uint64_t relative_timeout_ns = 0,
      const dt::BlobLifetimeBudgetConfigurationV3Generation1* custom_budget = nullptr) {
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
    if (custom_budget != nullptr) {
      if (!dt::BlobLifetimeRuntimeConformanceAccessV3::Configure(ledger, *custom_budget) ||
          !dt::BlobLifetimeRuntimeConformanceAccessV3::Freeze(ledger)) std::terminate();
    } else {
      dt::BlobLifetimeRuntimeConformanceAccessV3::SetCallbackLimit(
          ledger, callback_limit, relative_timeout_ns);
    }
  }

  dt::BlobLifetimeAuthorityAdmissionV3Generation1 Admission() {
    fake.pins += 2;
    return dt::BlobLifetimeRuntimeConformanceAccessV3::Admission(
        authority, services, pins, ledger,
        static_cast<std::uint64_t>(100 + fake.pins));
  }
};

dt::BlobReaderResultV3 NeverReadDuringAdmission(
    void*, const dt::BlobReaderRequestV3&) {
  Fail("logical reader invoked before guarded access");
}

void ReaderResultClosedShapeMatrix() {
  using Kind = dt::BlobReaderResultKindV3;
  constexpr std::uint64_t requested = 64, window = 41, timeout = 17;
  std::array<dt::BlobReaderDiagnosticFactsV3, 9> payloads{};
  payloads[2].lifetime_enum = payloads[2].expiry_reason_enum = 1;
  payloads[3].reason_enum = 22;
  payloads[4].reason_enum = 11;
  payloads[5].reason_enum = 16;
  payloads[5].window_octets = window;
  payloads[5].timeout_milliseconds = timeout;
  payloads[6].resource_enum = 2;
  payloads[6].required = 1;
  payloads[7].reason_enum = 17;
  payloads[8].format_enum = 6;
  payloads[8].reason_enum = 37;
  const auto validate = [&](const auto& result, Kind expected,
                             std::uint32_t selected = 0) {
    const auto actual = dt::ValidateBlobReaderResultV3Generation1(
        result, requested, window, timeout);
    Check(actual.kind == expected &&
              static_cast<std::uint32_t>(actual.selected_fact) == selected,
          "logical reader closed result shape/fact precedence differs");
  };
  const auto before = allocation_probe::calls.load();
  allocation_probe::enabled.store(true);
  dt::BlobReaderResultV3 progress;
  progress.status = 0;
  progress.actual_octets = 1;
  validate(progress, Kind::progress);
  for (std::uint32_t bits = 0; bits < 512; ++bits) {
    unsigned selected_index = 0;
    if (bits != 0)
      while ((bits & (UINT32_C(1) << selected_index)) == 0) ++selected_index;
    for (unsigned status = 0; status < 256; ++status) {
      for (const auto actual : {UINT64_C(0), UINT64_C(1), requested,
                               requested + 1, UINT64_MAX}) {
        dt::BlobReaderResultV3 result;
        result.status = static_cast<std::uint8_t>(status);
        result.actual_octets = actual;
        result.fact_bits = bits;
        if (bits != 0) result.diagnostic_facts = payloads[selected_index];
        const bool terminal = status == 1 && bits != 0 && actual <= requested;
        const bool ok = status == 0 && bits == 0 && actual != 0 && actual <= requested;
        validate(result, terminal ? Kind::terminal_fact
                                   : ok ? Kind::progress : Kind::protocol_violation,
                 terminal ? UINT32_C(1) << selected_index : 0);
      }
    }
  }
  constexpr std::array<unsigned, 9> used_fields{
      0, 0, 3, 16, 16, 16 | 32 | 64, 4 | 128 | 256, 16, 8 | 16};
  for (unsigned bit = 0; bit < 9; ++bit) {
    dt::BlobReaderResultV3 result;
    result.actual_octets = 1;
    result.fact_bits = UINT32_C(1) << bit;
    result.diagnostic_facts = payloads[bit];
    for (std::size_t reserved = 0; reserved < 7; ++reserved) {
      auto bad = result;
      bad.reserved[reserved] = 1;
      validate(bad, Kind::protocol_violation);
    }
    for (unsigned field = 0; field < 9; ++field) {
      if ((used_fields[bit] & (1U << field)) != 0) continue;
      auto bad = result;
      auto& facts = bad.diagnostic_facts;
      switch (field) {
        case 0: facts.lifetime_enum = 1; break;
        case 1: facts.expiry_reason_enum = 1; break;
        case 2: facts.resource_enum = 1; break;
        case 3: facts.format_enum = 1; break;
        case 4: facts.reason_enum = 1; break;
        case 5: facts.window_octets = 1; break;
        case 6: facts.timeout_milliseconds = 1; break;
        case 7: facts.required = 1; break;
        case 8: facts.available = 1; break;
      }
      validate(bad, Kind::protocol_violation);
    }
    for (unsigned unknown = 9; unknown < 32; ++unknown) {
      auto bad = result;
      bad.fact_bits |= UINT32_C(1) << unknown;
      validate(bad, Kind::protocol_violation);
    }
  }
  for (unsigned lifetime = 0; lifetime < 256; ++lifetime) {
    for (unsigned expiry = 0; expiry < 256; ++expiry) {
      dt::BlobReaderResultV3 result;
      result.fact_bits = 4;
      result.diagnostic_facts.lifetime_enum = lifetime;
      result.diagnostic_facts.expiry_reason_enum = expiry;
      const bool valid = lifetime >= 1 && lifetime <= 9 && expiry == lifetime;
      validate(result, valid ? Kind::terminal_fact : Kind::protocol_violation,
               valid ? 4 : 0);
    }
  }
  for (unsigned reason = 0; reason < 256; ++reason) {
    for (unsigned index : {3U, 4U, 7U}) {
      dt::BlobReaderResultV3 result;
      result.fact_bits = UINT32_C(1) << index;
      result.diagnostic_facts.reason_enum = reason;
      const bool valid = index == 3 ? reason == 22 || reason == 26
          : index == 4 ? reason == 11 || reason == 12 || reason == 13 || reason == 15
          : reason == 17;
      validate(result, valid ? Kind::terminal_fact : Kind::protocol_violation,
               valid ? result.fact_bits : 0);
    }
    for (unsigned format = 0; format < 256; ++format) {
      dt::BlobReaderResultV3 result;
      result.fact_bits = 256;
      result.diagnostic_facts.format_enum = format;
      result.diagnostic_facts.reason_enum = reason;
      const bool valid = (format == 1 && (reason == 30 || reason == 31)) ||
          (format == 2 && (reason == 32 || reason == 33 || reason == 35)) ||
          (format == 4 && reason == 36) || (format == 5 && reason == 34) ||
          (format == 6 && reason == 37);
      validate(result, valid ? Kind::terminal_fact : Kind::protocol_violation,
               valid ? 256 : 0);
    }
  }
  for (unsigned resource = 0; resource < 256; ++resource) {
    for (const auto bounds : {std::pair{UINT64_C(0), UINT64_C(0)},
                              std::pair{UINT64_C(1), UINT64_C(0)},
                              std::pair{UINT64_MAX, UINT64_MAX - 1},
                              std::pair{UINT64_MAX, UINT64_MAX},
                              std::pair{UINT64_C(0), UINT64_MAX}}) {
      dt::BlobReaderResultV3 result;
      result.fact_bits = 64;
      result.diagnostic_facts.resource_enum = resource;
      result.diagnostic_facts.required = bounds.first;
      result.diagnostic_facts.available = bounds.second;
      const bool valid = resource >= 1 && resource <= 11 && bounds.first > bounds.second;
      validate(result, valid ? Kind::terminal_fact : Kind::protocol_violation,
               valid ? 64 : 0);
    }
  }
  dt::BlobReaderResultV3 timed;
  timed.fact_bits = 32;
  timed.diagnostic_facts = payloads[5];
  validate(timed, Kind::terminal_fact, 32);
  ++timed.diagnostic_facts.window_octets;
  validate(timed, Kind::protocol_violation);
  timed.diagnostic_facts = payloads[5];
  ++timed.diagnostic_facts.timeout_milliseconds;
  validate(timed, Kind::protocol_violation);
  Check(dt::ValidateBlobReaderResultV3Generation1(progress, 0, window, timeout).kind ==
            Kind::protocol_violation,
        "reader reply accepted a zero-request callback");
  progress.actual_octets = UINT64_MAX;
  Check(dt::ValidateBlobReaderResultV3Generation1(progress, UINT64_MAX, window, timeout).kind ==
            Kind::progress,
        "opaque reader result truncated a uint64 progress count");
  progress.status = 1;
  progress.fact_bits = 1;
  const auto maximum_failure = dt::ValidateBlobReaderResultV3Generation1(
      progress, UINT64_MAX, window, timeout);
  Check(maximum_failure.kind == Kind::terminal_fact &&
            maximum_failure.selected_fact == dt::BlobReaderFactV3::security_denied,
        "reader result truncated exact uint64 discarded work");
  for (const auto reason : {UINT32_C(256), UINT32_C(0x80000000), UINT32_MAX}) {
    for (unsigned bit = 0; bit < 9; ++bit) {
      dt::BlobReaderResultV3 result;
      result.fact_bits = UINT32_C(1) << bit;
      result.diagnostic_facts = payloads[bit];
      result.diagnostic_facts.reason_enum = reason;
      validate(result, Kind::protocol_violation);
    }
  }
  allocation_probe::enabled.store(false);
  Check(allocation_probe::calls.load() == before,
        "reader-result validation allocated memory");
}

void LogicalReaderFactoryAdmission() {
  using Capability = dt::BlobReceiverLogicalReaderCapabilityV3Generation1;
  using Carrier = dt::BlobBoundLogicalReaderCarrierV3Generation1;
  static_assert(!std::is_default_constructible_v<Capability>);
  static_assert(!std::is_copy_constructible_v<Capability>);
  static_assert(!std::is_move_constructible_v<Capability>);
  static_assert(!std::is_copy_constructible_v<Carrier>);
  static_assert(!std::is_move_constructible_v<Carrier>);
  // Closed owner/operation/mode matrix, including every known and unknown bit.
  // No logical callback may run, including for the admitted maximum extent.
  for (unsigned owner = 1; owner != 4; ++owner) {
    for (unsigned op = 1; op != 14; ++op) {
      for (unsigned choice = 0; choice != 153; ++choice) {
        const unsigned modes = choice < 128 ? choice
            : (0x13U | (1U << (choice - 128 + 7)));
        Fixture f(0);
        f.fake.pins = 0;
        f.token.owner_class = owner;
        f.request.required_owner_class = owner;
        if (owner == SB_BLOB_OWNER_EXECUTION_BORROW_V3) {
          f.token.session_uuid = f.request.expected_session_uuid = BlobUuid(0xb0);
          f.token.statement_uuid = f.request.expected_statement_uuid = BlobUuid(0xc0);
        }
        f.token.mode_bits = modes;
        f.request.required_mode_bits = SB_BLOB_MODE_IMMUTABLE_READ_V3 |
                                       SB_BLOB_MODE_LOGICAL_READER_V3;
        f.request.operation = op;
        f.carrier_binding.carrier_kind = SB_BLOB_MODE_LOGICAL_READER_V3;
        auto cap = dt::BlobLifetimeRuntimeConformanceAccessV3::LogicalCapability(
            f.carrier_binding, dt::BlobValueStateV3::value,
            dt::kBlobMaximumLogicalBytesV3, &f.fake, NeverReadDuringAdmission);
        auto budget = dt::BlobLifetimeRuntimeConformanceAccessV3::BudgetControl(
            f.ledger, f.pins);
        const bool replay_required = op == 3 || op == 8;
        const bool expected = owner != 3 && op >= 2 && op <= 8 &&
            (modes & 0x39) == 0x11 && (modes & ~0x3f) == 0 &&
            (!replay_required || (modes & 2) != 0);
        const auto allocation_before = allocation_probe::calls.load();
        allocation_probe::enabled.store(true);
        {
          dt::BlobLifetimeAuthorityAdmissionV3Generation1 admission;
          Carrier carrier;
          const auto result = dt::BlobLifetimeRuntimeFactoryV3Generation1::
              AdmitLogicalReader(op, f.profile, &f.authority, &f.services,
                  f.pins, budget, f.services.monotonic_clock,
                  f.services.monotonic_clock, f.token, f.request, cap,
                  admission, carrier);
          allocation_probe::enabled.store(false);
          if (result.ok() != expected) {
            std::cerr << "logical factory owner/op/modes=" << owner << '/'
                      << op << '/' << modes << " code=" << result.diagnostic_code
                      << " reason=" << result.reason << '\n';
            Fail("logical-reader closed admission matrix");
          }
          Check(allocation_probe::calls.load() == allocation_before,
                "logical admission allocated");
          Check(f.fake.pins == (expected ? 2U : 0U),
                "logical admission pin custody");
          Check(carrier.logical_length() ==
                    (expected ? dt::kBlobMaximumLogicalBytesV3 : 0),
                "logical admission extent or atomic output");
          Check(f.fake.call_count == 0 && f.fake.clock_calls == 0 &&
                    f.fake.cancellation_calls == 0,
                "logical admission invoked content or runtime callbacks");
          if (!expected) {
            const auto carrier_bits = modes & 0x38;
            const bool malformed = modes == 0 || (modes & ~0x3f) != 0 ||
                carrier_bits == 0 || (carrier_bits & (carrier_bits - 1)) != 0;
            Check(result.fact.stage == dt::BlobLifetimeFactStageV3::factory_admission &&
                      (malformed
                        ? result.diagnostic_code == "CINL.LOB.DESCRIPTOR_INVALID" &&
                          result.reason == "token_struct_invalid"
                        : result.diagnostic_code == "BLOB.HANDLE_MODE_REFUSED" &&
                          result.reason.empty() &&
                          result.fact.parameters.operation_enum == op &&
                          result.fact.parameters.open_mode_enum ==
                              ((modes & 8) != 0 ? 4 : (modes & 16) != 0 ? 5 : 6)),
                  "logical admission exact typed mode/shape diagnostic");
          }
        }
        Check(f.fake.pins == 0 && f.fake.pin_drop_calls == 2,
              "logical admission did not drain both pins");
      }
    }
  }

  for (unsigned mutation = 0; mutation != 28; ++mutation) {
    Fixture f(0);
    f.fake.pins = 0;
    f.token.mode_bits = SB_BLOB_MODE_IMMUTABLE_READ_V3 |
                       SB_BLOB_MODE_REPLAYABLE_SAME_SNAPSHOT_V3 |
                       SB_BLOB_MODE_LOGICAL_READER_V3;
    f.request.required_mode_bits = f.token.mode_bits;
    f.request.operation = SB_BLOB_OP_COMPONENT_ENCODE_V3;
    f.carrier_binding.carrier_kind = SB_BLOB_MODE_LOGICAL_READER_V3;
    auto state = dt::BlobValueStateV3::value;
    std::uint64_t length = 17;
    void* context = &f.fake;
    dt::BlobReadAtV3 read_at = NeverReadDuringAdmission;
    auto clock = f.services.monotonic_clock;
    switch (mutation) {
      case 0: break;
      case 1: length = 0; break;
      case 2: length = 0; state = dt::BlobValueStateV3::sql_null; break;
      case 3: state = dt::BlobValueStateV3::sql_null; break;
      case 4: state = static_cast<dt::BlobValueStateV3>(2); break;
      case 5: state = static_cast<dt::BlobValueStateV3>(255); break;
      case 6: length = dt::kBlobMaximumLogicalBytesV3 + 1; break;
      case 7: length = UINT64_MAX; break;
      case 8: context = nullptr; break;
      case 9: read_at = nullptr; break;
      case 10: f.carrier_binding.carrier_uuid = {}; break;
      case 11: f.carrier_binding.carrier_generation = 0; break;
      case 12: f.carrier_binding.lifetime_token_generation++; break;
      case 13: f.carrier_binding.immutable_binding_generation++; break;
      case 14: f.carrier_binding.authority_instance_generation++; break;
      case 15: f.carrier_binding.lifetime_token_uuid = PlatformUuid(0xd0); break;
      case 16: f.request.expected_database_uuid = BlobUuid(0xd0); break;
      case 17: f.request.expected_transaction_uuid = BlobUuid(0xd0); break;
      case 18: f.request.expected_security_generation++; break;
      case 19: f.request.expected_snapshot_generation++; break;
      case 20: f.request.required_owner_class = 1; break;
      case 21: clock.clock_generation++; break;
      case 22: f.authority.probe = nullptr; break;
      case 23: f.services.read_monotonic_ns = nullptr; break;
      case 24: f.profile.identity.descriptor_generation++; break;
      case 25: f.carrier_binding.carrier_kind = SB_BLOB_MODE_MATERIALIZED_BYTES_V3; break;
      case 26: length = 0; context = nullptr; break;
      case 27: length = 0; state = dt::BlobValueStateV3::sql_null; read_at = nullptr; break;
    }
    auto cap = dt::BlobLifetimeRuntimeConformanceAccessV3::LogicalCapability(
        f.carrier_binding, state, length, context, read_at);
    auto budget = dt::BlobLifetimeRuntimeConformanceAccessV3::BudgetControl(
        f.ledger, f.pins);
    {
      dt::BlobLifetimeAuthorityAdmissionV3Generation1 admission;
      Carrier carrier;
      const auto result = dt::BlobLifetimeRuntimeFactoryV3Generation1::
          AdmitLogicalReader(SB_BLOB_OP_COMPONENT_ENCODE_V3, f.profile,
              &f.authority, &f.services, f.pins, budget, clock, clock,
              f.token, f.request, cap, admission, carrier);
      Check(result.ok() == (mutation < 3), "logical-reader admission mutation");
      Check(carrier.logical_length() == (mutation < 3 ? length : 0) &&
                carrier.state() == (mutation < 3 ? state : dt::BlobValueStateV3::value),
            "logical-reader refusal mutated output");
      if (mutation == 6 || mutation == 7) {
        Check(result.diagnostic_code == "BLOB.LENGTH_EXCEEDED" &&
                  result.fact.parameters.actual_length_u64 == length &&
                  result.fact.parameters.maximum_length_u64 == dt::kBlobMaximumLogicalBytesV3,
              "logical-reader exact extent diagnostic");
      }
      if (result.ok()) {
        // A populated output is not silently rebound and its original pin
        // custody remains intact after the failed second admission.
        const auto second = dt::BlobLifetimeRuntimeFactoryV3Generation1::
            AdmitLogicalReader(SB_BLOB_OP_COMPONENT_ENCODE_V3, f.profile,
                &f.authority, &f.services, f.pins, budget, clock, clock,
                f.token, f.request, cap, admission, carrier);
        Check(!second.ok() && second.diagnostic_code == "BLOB.STATE_INVALID" &&
                  f.fake.pins == 2 && carrier.logical_length() == length &&
                  carrier.state() == state,
              "logical-reader second admission changed original custody");
      }
    }
    Check(f.fake.pins == 0, "logical-reader mutation pin leak");
  }

  // The receiver-created capability itself is unstable until both factory
  // pins succeed. Fault-protected backing proves it is not eagerly inspected.
  const auto page_size = static_cast<std::size_t>(sysconf(_SC_PAGESIZE));
  void* page = mmap(nullptr, page_size, PROT_READ | PROT_WRITE,
                    MAP_PRIVATE | MAP_ANONYMOUS, -1, 0);
  Check(page != MAP_FAILED, "logical capability guard allocation");
  Fixture guarded(0);
  guarded.fake.pins = 0;
  auto* cap = dt::BlobLifetimeRuntimeConformanceAccessV3::LogicalCapabilityAt(
      page, guarded.carrier_binding, &guarded.fake, NeverReadDuringAdmission);
  auto budget = dt::BlobLifetimeRuntimeConformanceAccessV3::BudgetControl(
      guarded.ledger, guarded.pins);
  Check(mprotect(page, page_size, PROT_NONE) == 0, "logical capability guard");
  for (unsigned failed_pin = 1; failed_pin != 3; ++failed_pin) {
    guarded.fake.pin_acquire_calls = 0;
    guarded.fake.pin_failure_call = failed_pin;
    guarded.fake.pin_acquire_status = dt::BlobReceiverPinAcquireStatusV3::draining;
    guarded.fake.pin_zero_cookie = true;
    dt::BlobLifetimeAuthorityAdmissionV3Generation1 admission;
    Carrier carrier;
    const auto result = dt::BlobLifetimeRuntimeFactoryV3Generation1::
        AdmitLogicalReader(SB_BLOB_OP_COMPONENT_ENCODE_V3, guarded.profile,
            &guarded.authority, &guarded.services, guarded.pins, budget,
            guarded.services.monotonic_clock, guarded.services.monotonic_clock,
            guarded.token, guarded.request, *cap, admission, carrier);
    Check(result.diagnostic_code == "BLOB.LIFETIME_AUTHORITY_UNAVAILABLE" &&
              guarded.fake.pins == 0 && guarded.fake.call_count == 0 &&
              guarded.fake.pin_acquire_calls == failed_pin,
          "logical factory inspected unpinned capability or leaked pin");
  }
  Check(mprotect(page, page_size, PROT_READ | PROT_WRITE) == 0,
        "logical capability guard restore");
  cap->~BlobReceiverLogicalReaderCapabilityV3Generation1();
  Check(munmap(page, page_size) == 0, "logical capability guard unmap");
}

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

struct PostWorkObservationContext {
  FakeReceiver* receiver = nullptr;
  unsigned stimulus = 0;
  bool fail_work = false;
};

dt::BlobProtectedWorkResultV3 PostWorkObservationVisitor(
    void* context, std::span<const platform::byte> bytes) noexcept {
  const auto& work = *static_cast<PostWorkObservationContext*>(context);
  auto& f = *work.receiver;
  switch (work.stimulus) {
    case 1: f.cancelled = true; break;
    case 2: case 3: case 8: f.now = 200; break;
    case 4: f.clock_unavailable = true; break;
    case 5: f.cancellation_unavailable = true; break;
    case 6: f.clock_dirty_reserved = true; break;
    case 7: f.cancelled = true; f.now = 200; break;
    case 9: f.clock_unavailable = true; f.cancelled = true; break;
    case 10: case 11: f.cancellation_unavailable = true; f.now = 200; break;
    case 12: f.now = 0; break;
  }
  return work.fail_work ? ResourceVisitor(nullptr, bytes)
                        : dt::BlobProtectedWorkResultV3{0, {}, {}};
}

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
        dt::BlobLifetimeRuntimeConformanceAccessV3::BudgetControl(
            fixture.ledger, fixture.pins);
    auto capability =
        dt::BlobLifetimeRuntimeConformanceAccessV3::Capability(
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
        dt::BlobLifetimeRuntimeConformanceAccessV3::BudgetControl(
            fixture.ledger, fixture.pins);
    auto capability =
        dt::BlobLifetimeRuntimeConformanceAccessV3::Capability(
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
      auto budget = dt::BlobLifetimeRuntimeConformanceAccessV3::BudgetControl(
          fault(2) ? unfrozen : fixture.ledger, fixture.pins);
      const platform::byte dummy = 0;
      const auto state = fault(10)
          ? static_cast<dt::BlobValueStateV3>(255)
          : (fault(11) ? dt::BlobValueStateV3::sql_null
                       : dt::BlobValueStateV3::value);
      auto capability = dt::BlobLifetimeRuntimeConformanceAccessV3::Capability(
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
    auto budget = dt::BlobLifetimeRuntimeConformanceAccessV3::BudgetControl(
        fixture.ledger, fixture.pins);
    const platform::byte dummy = 0;
    const auto length = length_failure ? dt::kBlobMaximumLogicalBytesV3 + 1 : 1;
    auto capability = dt::BlobLifetimeRuntimeConformanceAccessV3::Capability(
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
  auto budget = dt::BlobLifetimeRuntimeConformanceAccessV3::BudgetControl(
      fixture.ledger, fixture.pins);
  auto capability = dt::BlobLifetimeRuntimeConformanceAccessV3::Capability(
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
    auto budget = dt::BlobLifetimeRuntimeConformanceAccessV3::BudgetControl(
        fixture.ledger, fixture.pins);
    auto capability = dt::BlobLifetimeRuntimeConformanceAccessV3::Capability(
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
      auto budget = dt::BlobLifetimeRuntimeConformanceAccessV3::BudgetControl(
          fixture.ledger, fixture.pins);
      auto capability = dt::BlobLifetimeRuntimeConformanceAccessV3::Capability(
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
        auto budget = dt::BlobLifetimeRuntimeConformanceAccessV3::BudgetControl(
            fixture.ledger, fixture.pins);
        auto capability = dt::BlobLifetimeRuntimeConformanceAccessV3::Capability(
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
        auto budget = dt::BlobLifetimeRuntimeConformanceAccessV3::BudgetControl(
            fixture.ledger, fixture.pins);
        auto capability = dt::BlobLifetimeRuntimeConformanceAccessV3::Capability(
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
    auto budget = dt::BlobLifetimeRuntimeConformanceAccessV3::BudgetControl(
        fixture.ledger, fixture.pins);
    auto capability = dt::BlobLifetimeRuntimeConformanceAccessV3::Capability(
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
    auto budget = dt::BlobLifetimeRuntimeConformanceAccessV3::BudgetControl(
        bad_abi ? fixture.ledger : unfrozen, fixture.pins);
    auto capability = dt::BlobLifetimeRuntimeConformanceAccessV3::Capability(
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
  // All six surfaces share the same status/cookie/reserved-byte contract.
  // Check both legal zero-cookie refusals and process death for malformed
  // replies, including each reserved byte independently.
  for (unsigned surface = 0; surface != 6; ++surface) {
    const bool clone_surface = surface >= 2 && surface < 4;
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
            if (clone_surface) {
              auto bound = dt::BlobLifetimeRuntimeConformanceAccessV3::Carrier(
                  fixture.carrier_binding, dt::BlobValueStateV3::value, nullptr, 0);
              if (!dt::RetainBaseBlobLifetimeLeaseV3Generation1(
                      fixture.profile, fixture.Admission(), bound, fixture.token,
                      fixture.request, source).ok()) _exit(2);
              fixture.fake.pin_clone_failure_call = surface - 1;
            } else fixture.fake.pin_failure_call = surface % 2 + 1;
            fixture.fake.pin_acquire_status =
                static_cast<dt::BlobReceiverPinAcquireStatusV3>(status);
            fixture.fake.pin_zero_cookie = zero_cookie;
            fixture.fake.pin_dirty_reserved = dirty_byte >= 0;
            fixture.fake.pin_dirty_reserved_index = dirty_byte < 0 ? 0 : dirty_byte;
            auto budget = dt::BlobLifetimeRuntimeConformanceAccessV3::BudgetControl(
                fixture.ledger, fixture.pins);
            auto capability = dt::BlobLifetimeRuntimeConformanceAccessV3::Capability(
                fixture.carrier_binding, dt::BlobValueStateV3::value, nullptr, 0);
            dt::BlobLifetimeAuthorityAdmissionV3Generation1 admission;
            dt::BlobBoundMaterializedCarrierV3Generation1 carrier;
            dt::BlobBoundLogicalReaderCarrierV3Generation1 logical_carrier;
            const auto result = [&]() {
              if (clone_surface) return source.CloneInto(destination);
              if (surface >= 4) {
                fixture.token.mode_bits = SB_BLOB_MODE_IMMUTABLE_READ_V3 |
                                          SB_BLOB_MODE_LOGICAL_READER_V3;
                fixture.request.required_mode_bits = fixture.token.mode_bits;
                fixture.carrier_binding.carrier_kind = SB_BLOB_MODE_LOGICAL_READER_V3;
                auto logical = dt::BlobLifetimeRuntimeConformanceAccessV3::LogicalCapability(
                    fixture.carrier_binding, dt::BlobValueStateV3::value, 0,
                    &fixture.fake, NeverReadDuringAdmission);
                return dt::BlobLifetimeRuntimeFactoryV3Generation1::AdmitLogicalReader(
                    SB_BLOB_OP_READ_V3, fixture.profile, &fixture.authority,
                    &fixture.services, fixture.pins, budget,
                    fixture.services.monotonic_clock, fixture.services.monotonic_clock,
                    fixture.token, fixture.request, logical, admission, logical_carrier);
              }
              return dt::BlobLifetimeRuntimeFactoryV3Generation1::AdmitMaterialized(
                    SB_BLOB_OP_READ_V3, fixture.profile, &fixture.authority,
                    &fixture.services, fixture.pins, budget,
                    fixture.services.monotonic_clock, fixture.services.monotonic_clock,
                    fixture.token, fixture.request, capability, admission, carrier);
            }();
            if (malformed) _exit(3); // A malformed provider must not return.
            if (status == 0) {
              if (!result.ok()) _exit(4);
              if (clone_surface &&
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
            if (clone_surface && (!source.retained() || !source.Release().ok()))
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
        dt::BlobLifetimeRuntimeConformanceAccessV3::BudgetControl(
            fixture.ledger, fixture.pins);
    auto capability =
        dt::BlobLifetimeRuntimeConformanceAccessV3::Capability(
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
    auto budget = dt::BlobLifetimeRuntimeConformanceAccessV3::BudgetControl(
        factory_fixture.ledger, factory_fixture.pins);
    auto capability = dt::BlobLifetimeRuntimeConformanceAccessV3::Capability(
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
      left.lifetime_callback_terminal_quarantined !=
          right.lifetime_callback_terminal_quarantined ||
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
  auto budget = dt::BlobLifetimeRuntimeConformanceAccessV3::BudgetControl(
      unfrozen, fixture.pins);
  auto capability = dt::BlobLifetimeRuntimeConformanceAccessV3::Capability(
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

void ProtectedWorkFailureObservation() {
  using Access = dt::BlobLifetimeRuntimeConformanceAccessV3;
  constexpr std::array<std::string_view, 13> codes{{
      "", "PROCESS.CANCELLED", "CINL.LOB.HANDLE_EXPIRED",
      "CINL.LOB.STREAM_BACKPRESSURE_EXHAUSTED", "BLOB.LIFETIME_AUTHORITY_UNAVAILABLE",
      "BLOB.LIFETIME_AUTHORITY_UNAVAILABLE", "BLOB.LIFETIME_AUTHORITY_UNAVAILABLE",
      "PROCESS.CANCELLED", "CINL.LOB.HANDLE_EXPIRED", "PROCESS.CANCELLED",
      "CINL.LOB.HANDLE_EXPIRED", "BLOB.LIFETIME_AUTHORITY_UNAVAILABLE", ""}};
  for (bool fail_work : {false, true}) {
    for (unsigned stimulus = 0; stimulus < codes.size(); ++stimulus) {
      Fixture f(8);
      f.fake.pins = 0;
      if (stimulus == 2 || stimulus == 8 || stimulus == 10)
        f.token.valid_until_monotonic_ns = 200;
      if (stimulus == 3 || stimulus == 7 || stimulus == 8 || stimulus == 11)
        f.request.absolute_deadline_monotonic_ns = 200;
      const std::array<platform::byte, 1> bytes{0x51};
      auto carrier = Access::Carrier(f.carrier_binding, dt::BlobValueStateV3::value,
                                    bytes.data(), bytes.size());
      dt::BlobRetainedLifetimeLeaseV3 lease;
      Check(dt::RetainBaseBlobLifetimeLeaseV3Generation1(f.profile, f.Admission(),
          carrier, f.token, f.request, lease).ok(), "post-work observation retain");
      PostWorkObservationContext context{&f.fake, stimulus, fail_work};
      auto visitor = Access::Visitor(PostWorkObservationVisitor, &context);
      const auto before = allocation_probe::calls.load();
      allocation_probe::enabled.store(true);
      const auto result = lease.VisitBoundMaterializedBytes(visitor);
      const auto clocks_before_cleanup = f.fake.clock_calls;
      const auto cancellations_before_cleanup = f.fake.cancellation_calls;
      const bool clean = stimulus == 0 && !fail_work;
      const auto released = clean ? lease.Release() : dt::BlobLifetimeRuntimeResultV3{};
      allocation_probe::enabled.store(false);
      Check(clocks_before_cleanup == (clean ? 8 : 6) &&
                cancellations_before_cleanup == (clean ? 8 : stimulus == 12 ? 5 : 6),
            "protected work result skipped or duplicated post-return observation");
      if (clean) {
        Check(result.ok() && released.ok(), "clean protected work observation refused");
      } else if (stimulus == 12) {
        Check(result.disposition == dt::BlobLifetimeOuterDispositionV3::terminated_out_of_band &&
                  result.diagnostic_code.empty() && f.fake.invariant_calls == 1 &&
                  f.fake.last_event == dt::BlobLifetimeInvariantEventV3::monotonic_clock_regression,
              "post-work clock regression lost private quarantine or manufactured a diagnostic");
      } else {
        const auto expected = stimulus == 0 ? std::string_view{"RESOURCE.BUDGET_EXCEEDED"}
                                            : codes[stimulus];
        Check(!result.ok() && result.diagnostic_code == expected,
              "post-work observation failed total failure precedence");
        if (stimulus == 0) {
          Check(result.required == 2 && result.available == 1 &&
                    result.fact.stage == dt::BlobLifetimeFactStageV3::callback_return,
                "post-work successful gate replaced original exact resource fact");
        } else {
          Check(result.fact.stage == dt::BlobLifetimeFactStageV3::ordinary_gate &&
                    result.fact.gate_present &&
                    result.fact.gate == dt::BlobLifetimeGateV3::after_protected_work,
                "post-work failure lost its actual observation gate");
        }
      }
      Check(CountCall(f.fake,'R') == 1 && CountCall(f.fake,'B') == 1 &&
                CountCall(f.fake,'E') == 1 && CountCall(f.fake,'L') == 1 &&
                CountCall(f.fake,'P') == (clean ? 1 : 0) && f.fake.pins == 0 &&
                Access::ReservedCleanup(f.ledger) == 0 &&
                Access::Invoked(f.ledger) == (clean ? 5 : 4) &&
                allocation_probe::calls.load() == before,
            "post-work failure changed exact cleanup, callback accounting or allocation");
    }
  }
}

void ProtectedWorkCleanupSelection() {
  using Access = dt::BlobLifetimeRuntimeConformanceAccessV3;
  const std::array<platform::byte, 1> bytes{0x51};
  for (bool release_phase : {false, true}) {
    for (bool security : {false, true}) {
      Fixture f(8);
      f.fake.pins = 0;
      const auto code = security ? SB_BLOB_CALLBACK_SECURITY_DENIED_V3
                                 : SB_BLOB_CALLBACK_WRONG_OWNER_V3;
      if (release_phase) f.fake.release_code = code;
      else f.fake.end_code = code;
      auto carrier = Access::Carrier(f.carrier_binding, dt::BlobValueStateV3::value,
                                    bytes.data(), bytes.size());
      dt::BlobRetainedLifetimeLeaseV3 lease;
      Check(dt::RetainBaseBlobLifetimeLeaseV3Generation1(f.profile, f.Admission(), carrier,
                f.token, f.request, lease).ok(), "post-work cleanup primary retain");
      auto visitor = Access::Visitor(ResourceVisitor, nullptr);
      const auto result = lease.VisitBoundMaterializedBytes(visitor);
      Check(result.diagnostic_code == (security ? "SECURITY.ACCESS_DENIED" : "RESOURCE.BUDGET_EXCEEDED") &&
                result.fact.stage == (security ? dt::BlobLifetimeFactStageV3::cleanup_return
                                               : dt::BlobLifetimeFactStageV3::callback_return) &&
                CountCall(f.fake,'E') == 1 && CountCall(f.fake,'L') == 1 &&
                CountCall(f.fake,'P') == 0 && f.fake.clock_calls == 6 &&
                f.fake.cancellation_calls == 6 && f.fake.pins == 0 &&
                f.fake.invariant_calls == 1 && Access::ReservedCleanup(f.ledger) == 0,
            "post-work observation changed sticky cleanup primary or security override");
    }
  }
  for (unsigned violation = 0; violation < 4; ++violation) {
    Fixture f(8);
    f.fake.pins = 0;
    auto carrier = Access::Carrier(f.carrier_binding, dt::BlobValueStateV3::value,
                                  bytes.data(), bytes.size());
    dt::BlobRetainedLifetimeLeaseV3 lease;
    Check(dt::RetainBaseBlobLifetimeLeaseV3Generation1(f.profile, f.Admission(), carrier,
              f.token, f.request, lease).ok(), "private protected work retain");
    std::pair<FakeReceiver*, unsigned> context{&f.fake, violation};
    auto visitor = Access::Visitor(
        [](void* raw, std::span<const platform::byte>) -> dt::BlobProtectedWorkResultV3 {
          auto& c = *static_cast<std::pair<FakeReceiver*, unsigned>*>(raw);
          c.first->cancelled = true;
          if (c.second == 3) throw 1;
          dt::BlobProtectedWorkResultV3 result;
          if (c.second == 0) result.status = 2;
          if (c.second == 1) result.reserved[0] = 1;
          return result; // violation2: terminal failure with no admitted fact
        }, &context);
    const auto result = lease.VisitBoundMaterializedBytes(visitor);
    Check(result.disposition == dt::BlobLifetimeOuterDispositionV3::terminated_out_of_band &&
              result.diagnostic_code.empty() && f.fake.clock_calls == 5 &&
              f.fake.cancellation_calls == 5 && f.fake.invariant_calls == 1 &&
              f.fake.last_event == dt::BlobLifetimeInvariantEventV3::receiver_services_failed &&
              f.fake.pins == 0 && CountCall(f.fake,'E') == 1 && CountCall(f.fake,'L') == 1 &&
              CountCall(f.fake,'P') == 0 && Access::ReservedCleanup(f.ledger) == 0,
          "private protected failure entered a later ordinary observation or lost cleanup");
  }
}

void ExactAdmittedTimeoutRounding() {
  using Access = dt::BlobLifetimeRuntimeConformanceAccessV3;
  for (const std::uint64_t ns : {UINT64_C(0), UINT64_C(1), UINT64_C(999999),
        UINT64_C(1000000), UINT64_C(1000001), UINT64_MAX-UINT64_C(999999),
        UINT64_MAX-UINT64_C(1), UINT64_MAX}) {
    Fixture f(8, ns);
    f.fake.pins = 0;
    f.request.absolute_deadline_monotonic_ns = 200;
    const std::array<platform::byte, 1> bytes{0x51};
    auto carrier = Access::Carrier(f.carrier_binding, dt::BlobValueStateV3::value,
                                  bytes.data(), bytes.size());
    dt::BlobRetainedLifetimeLeaseV3 lease;
    Check(dt::RetainBaseBlobLifetimeLeaseV3Generation1(f.profile, f.Admission(), carrier,
              f.token, f.request, lease).ok(), "timeout rounding retain");
    PostWorkObservationContext context{&f.fake, 3, false};
    auto visitor = Access::Visitor(PostWorkObservationVisitor, &context);
    const auto before = allocation_probe::calls.load();
    allocation_probe::enabled.store(true);
    const auto result = lease.VisitBoundMaterializedBytes(visitor);
    allocation_probe::enabled.store(false);
    const std::uint64_t expected = ns/UINT64_C(1000000) + (ns%UINT64_C(1000000) != 0);
    Check(result.diagnostic_code == "CINL.LOB.STREAM_BACKPRESSURE_EXHAUSTED" &&
              result.fact.parameters.timeout_milliseconds_u64 == expected &&
              result.fact.parameters.window_octets_u64 == 0 &&
              CountCall(f.fake,'E') == 1 && CountCall(f.fake,'L') == 1 && f.fake.pins == 0 &&
              allocation_probe::calls.load() == before,
          "relative timeout rounding fabricated saturation instead of exact ceil milliseconds");
  }
}

#include "base_blob_component_vectors.inc"

std::size_t LoadBlobGolden(const BlobComponentGoldenVector& v,
                           std::span<platform::byte> destination) {
  Check(v.component_hex.size()%2 == 0 && v.component_hex.size()/2 <= destination.size(),
        "invalid compiled BLOB component oracle extent");
  const auto digit = [](char c) -> platform::byte {
    if (c >= '0' && c <= '9') return c-'0';
    if (c >= 'a' && c <= 'f') return c-'a'+10;
    Fail("invalid compiled BLOB component oracle hex digit");
  };
  for (std::size_t i = 0; i < v.component_hex.size()/2; ++i)
    destination[i] = static_cast<platform::byte>((digit(v.component_hex[2*i])<<4)|digit(v.component_hex[2*i+1]));
  return v.component_hex.size()/2;
}

void BlobComponentGoldenDigestPasses() {
  const auto admitted = dt::BuildCurrentBlobValidatedProfileHandleV3(dt::kBlobV11ReceiptUuid);
  Check(admitted.ok(),"native component exact profile admission");
  for (const auto& v : kBlobComponentGoldenVectors) {
    std::array<platform::byte,360> frame{};
    const auto total = LoadBlobGolden(v,frame);
    const auto content = std::span<const platform::byte>(frame).subspan(320,total-320);
    for (std::size_t split = 0; split <= content.size(); ++split) {
      dt::BlobComponentDigestPassV3 encoder,decoder;
      dt::BlobComponentHeaderV3 encoded{};
      const auto before = allocation_probe::calls.load();
      allocation_probe::enabled.store(true);
      Check(encoder.BeginEncode(admitted.profile,v.state,content.size()).ok() &&
                encoder.UpdateContent(content.first(split)).ok() &&
                encoder.UpdateContent({}).ok() &&
                encoder.UpdateContent(content.subspan(split)).ok() && encoder.FinishEncode(encoded).ok(),
            "native component digest pass failed a canonical split including zero-length updates");
      Check(std::equal(encoded.begin(),encoded.end(),frame.begin()),
            "SBBLOB03 encoder differs from independent complete binary header oracle");
      Check(decoder.BeginDecode(admitted.profile,total,std::span(frame).first(320)).ok() &&
                decoder.UpdateContent(content.first(split)).ok() &&
                decoder.UpdateContent(content.subspan(split)).ok() && decoder.FinishDecode().ok(),
            "SBBLOB03 decoder rejected independent frame or chunk partition");
      Check(allocation_probe::calls.load() == before,"component framing/digests allocated heap memory");
      allocation_probe::enabled.store(false);
      const auto saved = encoded;
      Check(!encoder.FinishEncode(encoded).ok() && encoded == saved && !decoder.FinishDecode().ok(),
            "component finalized more than once or changed output after failure");
      encoder.Scrub(); decoder.Scrub();
      Check(encoder.BeginEncode(admitted.profile,v.state,content.size()).ok() &&
                encoder.UpdateContent(content).ok() && encoder.FinishEncode(encoded).ok() && encoded == saved,
            "explicit scrub did not allow an independent deterministic new pass");
    }
  }
}

void BlobComponentFieldAndIntegrityFailures() {
  const auto profile = dt::BuildCurrentBlobValidatedProfileHandleV3(dt::kBlobV11ReceiptUuid).profile;
  std::array<platform::byte,360> full{};
  const auto total = LoadBlobGolden(kBlobComponentGoldenVectors[3],full); // abc
  dt::BlobComponentHeaderV3 valid{};
  std::copy_n(full.begin(),320,valid.begin());
  const auto check_failure = [&](const dt::BlobComponentHeaderV3& changed,
      std::uint64_t extent, std::uint32_t reason, std::uint64_t offset) {
    const auto allocations_before = allocation_probe::calls.load();
    allocation_probe::enabled.store(true);
    const auto first_gate = dt::ValidateBlobComponentHeaderNoAllocV3(profile,extent,changed);
    allocation_probe::enabled.store(false);
    dt::BlobComponentDigestPassV3 decoder;
    const auto result = decoder.BeginDecode(profile,extent,changed);
    Check(first_gate.failure == result.failure && first_gate.diagnostic_uuid == result.diagnostic_uuid &&
              first_gate.diagnostic_generation == result.diagnostic_generation &&
              first_gate.diagnostic_code == result.diagnostic_code && first_gate.reason_enum == result.reason_enum &&
              first_gate.format_enum == result.format_enum && first_gate.failure_offset_present == result.failure_offset_present &&
              first_gate.failure_offset == result.failure_offset && allocation_probe::calls.load() == allocations_before,
          "initial header-only gate differs from complete digest initialization or allocates before field validation");
    Check(!result.ok() && result.failure == dt::BlobComponentPassFailureV3::canonical_encoding_invalid &&
              result.diagnostic_code == "BLOB.CANONICAL_ENCODING_INVALID" && result.format_enum == 1 &&
              result.reason_enum == reason && result.failure_offset_present && result.failure_offset == offset &&
              result.diagnostic_generation == 1 && result.diagnostic_uuid.bytes == std::array<platform::byte,16>{{
                0x01,0xa1,0x09,0x5f,0xf2,0x05,0x7f,0x8b,0xbe,0xac,0x36,0xa9,0xdc,0x66,0xaa,0x68}},
          "component malformed field lost exact canonical reason/offset/precedence");
    Check(!decoder.UpdateContent({}).ok() && !decoder.FinishDecode().ok(),
          "failed header left a usable hash pass");
  };
  for (std::size_t n = 0; n < 320; ++n) {
    dt::BlobComponentDigestPassV3 decoder;
    const auto result = decoder.BeginDecode(profile,total,std::span(valid).first(n));
    Check(result.reason_enum == 41 && result.failure_offset == n && result.format_enum == 1,
          "truncated header read outside exact extent or fabricated failure offset");
  }
  // Every individual identity octet reports its containing FIELD start,
  // including binary UUID bytes, u64 generations and the profile fingerprint.
  constexpr std::array<std::size_t,16> fields{{40,56,64,80,88,104,112,120,136,144,152,168,184,200,216,248}};
  for (std::size_t i = 0; i+1 < fields.size(); ++i)
    for (std::size_t p = fields[i]; p < fields[i+1]; ++p) {
      auto h = valid; h[p] ^= 0x80;
      check_failure(h,total,51,fields[i]);
    }
  for (std::size_t p = 0; p < 8; ++p) { auto h = valid; h[p] ^= 1; check_failure(h,total,42,p); }
  for (std::size_t p : {8U,9U}) { auto h = valid; h[p] ^= 1; check_failure(h,total,43,8); }
  for (std::size_t p : {10U,11U}) { auto h = valid; h[p] ^= 1; check_failure(h,total,44,10); }
  for (std::size_t p = 12; p < 16; ++p) { auto h = valid; h[p] = 1; check_failure(h,total,45,12); }
  for (std::size_t p = 25; p < 32; ++p) { auto h = valid; h[p] = 1; check_failure(h,total,46,p); }
  for (std::size_t p = 312; p < 320; ++p) { auto h = valid; h[p] = 1; check_failure(h,total,46,p); }
  { auto h = valid; h[39] = 0x80; check_failure(h,total,55,32); }
  for (std::size_t p = 16; p < 24; ++p) { auto h = valid; h[p] ^= 1; check_failure(h,total,47,16); }
  check_failure(valid,total-1,48,total-1);
  check_failure(valid,total+1,48,total);
  check_failure(valid,UINT64_MAX,48,total);
  for (std::uint64_t extent : {0U,1U,319U}) check_failure(valid,extent,41,extent);
  { auto h = valid; h[24] = 2; check_failure(h,total+1,48,total); }
  { auto h = valid; h[24] = 1; h[40] ^= 1; check_failure(h,total,50,32); }
  { auto h = valid; h[24] = 2; check_failure(h,total,49,24); }
  { auto h = valid; h[24] = 1; check_failure(h,total,50,32); }
  // Adjacent gates with both faults present must select the earlier gate,
  // independently of fault injection order.
  struct Fault { std::size_t byte; platform::byte value; std::uint32_t reason; std::size_t offset; };
  constexpr std::array<Fault,11> ordered{{
    {0,0,42,0},{8,4,43,8},{10,0,44,10},{12,1,45,12},{25,1,46,25},{312,1,46,312},
    {39,0x80,55,32},{16,0,47,16},{24,2,49,24},{40,0,51,40},{247,0,51,216}}};
  for (std::size_t i = 0; i+1 < ordered.size(); ++i) {
    auto h = valid; h[ordered[i+1].byte] = ordered[i+1].value; h[ordered[i].byte] = ordered[i].value;
    check_failure(h,total,ordered[i].reason,ordered[i].offset);
  }
  // Header-digest fields are intentionally not part of the frame preimage.
  // Verify them only after exact content, with state/content before frame.
  for (unsigned fault = 0; fault < 4; ++fault) {
    auto h = valid;
    auto content = std::span(full).subspan(320,total-320);
    if (fault == 0 || fault == 2) h[248] ^= 1;
    if (fault == 1 || fault == 2) h[311] ^= 1;
    if (fault == 3) content[1] ^= 1;
    dt::BlobComponentDigestPassV3 decoder;
    Check(decoder.BeginDecode(profile,total,h).ok() && decoder.UpdateContent(content).ok(),
          "digest fields incorrectly entered header structural checks");
    const auto result = decoder.FinishDecode();
    Check(result.failure == dt::BlobComponentPassFailureV3::integrity_failed &&
              result.diagnostic_code == "BLOB.INTEGRITY_FAILED" && result.format_enum == 1 &&
              result.reason_enum == (fault == 1 ? 31U : 30U) && !result.failure_offset_present &&
              result.diagnostic_generation == 1 && result.diagnostic_uuid.bytes == std::array<platform::byte,16>{{
                0x01,0xa1,0x09,0x5f,0xf2,0x05,0x78,0x79,0xb9,0x23,0x42,0x5b,0xe9,0x3f,0x2c,0x1a}},
          "verified integrity mismatch lost precedence/provenance or invented encoding offset");
    if (fault == 3) content[1] ^= 1;
  }
}

void BlobComponentPassLifecycleAndBounds() {
  const auto profile = dt::BuildCurrentBlobValidatedProfileHandleV3(dt::kBlobV11ReceiptUuid).profile;
  dt::BlobComponentHeaderV3 output; output.fill(0x5a);
  const auto saved = output;
  dt::BlobComponentDigestPassV3 pass;
  Check(!pass.FinishEncode(output).ok() && output == saved,
        "uninitialized component pass published or changed output");
  pass.Scrub();
  Check(pass.BeginEncode(profile,dt::BlobValueStateV3::value,3).ok() &&
            pass.UpdateContent({}).ok() && !pass.FinishEncode(output).ok() && output == saved,
        "incomplete content silently finalized or changed output");
  pass.Scrub();
  std::array<platform::byte,4> bytes{{0,1,2,3}};
  Check(pass.BeginEncode(profile,dt::BlobValueStateV3::value,3).ok() &&
            !pass.UpdateContent(bytes).ok() && !pass.FinishEncode(output).ok() && output == saved,
        "overlong content admitted, wrapped or published partial encoding");
  pass.Scrub();
  Check(pass.BeginEncode(profile,dt::BlobValueStateV3::value,dt::kBlobMaximumLogicalBytesV3).ok() &&
            !pass.FinishEncode(output).ok() && output == saved,
        "INT64_MAX shape was not representable or fabricated absent content");
  pass.Scrub();
  Check(!pass.BeginEncode(profile,dt::BlobValueStateV3::value,dt::kBlobMaximumLogicalBytesV3+1).ok(),
        "above logical maximum admitted");
  pass.Scrub();
  Check(!pass.BeginEncode(profile,dt::BlobValueStateV3::sql_null,1).ok(),"NULL payload admitted");
  pass.Scrub();
  Check(!pass.BeginEncode(profile,static_cast<dt::BlobValueStateV3>(2),0).ok(),"locator-like state admitted");
  pass.Scrub();
  auto invalid = profile; ++invalid.identity.hash_policy.generation;
  Check(!pass.BeginEncode(invalid,dt::BlobValueStateV3::value,0).ok(),"substituted hash policy admitted");
  pass.Scrub();
  Check(pass.BeginEncode(profile,dt::BlobValueStateV3::value,0).ok(),"alias test initialize");
  const auto self = std::span<const platform::byte>(reinterpret_cast<const platform::byte*>(&pass),sizeof(pass));
  Check(pass.UpdateContent(self).failure == dt::BlobComponentPassFailureV3::input_overlap &&
            pass.FinishEncode(output).ok(),"input alias consumed live codec state or destroyed valid pass");
  pass.Scrub();
  Check(pass.BeginDecode(profile,320,self).failure == dt::BlobComponentPassFailureV3::input_overlap &&
            pass.BeginEncode(profile,dt::BlobValueStateV3::value,0).ok(),
        "header alias mutated or read live framing state");
  pass.Scrub();
  std::array<platform::byte,360> null_frame{};
  (void)LoadBlobGolden(kBlobComponentGoldenVectors[0],null_frame);
  const auto refused = pass.BeginDecode(profile,320,std::span(null_frame).first(320),false);
  Check(refused.reason_enum == 50 && refused.failure_offset == 32,
        "nonnull decode target accepted SQL NULL");
  pass.Scrub();
  Check(pass.BeginEncode(profile,dt::BlobValueStateV3::value,0).ok() &&
            !pass.BeginEncode(profile,dt::BlobValueStateV3::value,0).ok() &&
            !pass.FinishEncode(output).ok(),
        "second initialize silently reset active work or revived failed state");
  pass.Scrub();
  std::array<platform::byte,360> abc{};
  const auto size = LoadBlobGolden(kBlobComponentGoldenVectors[3],abc);
  Check(pass.BeginDecode(profile,size,std::span(abc).first(320)).ok() &&
            pass.UpdateContent(std::span(abc).subspan(320,2)).ok() &&
            pass.FinishDecode().failure == dt::BlobComponentPassFailureV3::content_extent_mismatch,
        "decoder accepted missing content or fabricated encoded-field evidence from caller schedule");
}

void BlobComponentFragmentedHashBoundariesAndScrub() {
  const auto profile = dt::BuildCurrentBlobValidatedProfileHandleV3(dt::kBlobV11ReceiptUuid).profile;
  std::array<platform::byte,65537> content{};
  for (std::size_t i = 0; i < content.size(); ++i) content[i] = static_cast<platform::byte>((i*131U)^(i>>7));
  for (std::size_t length : {63U,64U,65U,511U,512U,513U,1023U,1024U,1025U,4095U,4096U,4097U,65535U,65536U,65537U}) {
    dt::BlobComponentDigestPassV3 whole;
    dt::BlobComponentHeaderV3 expected{};
    const auto bytes = std::span<const platform::byte>(content).first(length);
    Check(whole.BeginEncode(profile,dt::BlobValueStateV3::value,length).ok() &&
              whole.UpdateContent(bytes).ok() && whole.FinishEncode(expected).ok(),
          "complete long native component digest pass failed");
    for (std::size_t chunk : {1U,31U,64U,65U,1023U,1024U,1025U,4096U}) {
      dt::BlobComponentDigestPassV3 encode,decode;
      dt::BlobComponentHeaderV3 actual{};
      const auto before = allocation_probe::calls.load();
      allocation_probe::enabled.store(true);
      Check(encode.BeginEncode(profile,dt::BlobValueStateV3::value,length).ok() &&
                decode.BeginDecode(profile,320+length,expected).ok(),"fragmented long pass initialization");
      for (std::size_t offset = 0; offset < length; offset += std::min(chunk,length-offset)) {
        const auto part = bytes.subspan(offset,std::min(chunk,length-offset));
        Check(encode.UpdateContent(part).ok() && decode.UpdateContent(part).ok(),
              "fragmented BLAKE3 block/chunk boundary lost content or progress");
      }
      Check(encode.FinishEncode(actual).ok() && actual == expected && decode.FinishDecode().ok() &&
                allocation_probe::calls.load() == before,
            "content segmentation changed native component digests or allocated memory");
      allocation_probe::enabled.store(false);
    }
    // A same-length substitution cannot pass merely because the frame's
    // structural header/extent/identity is valid and unchanged.
    dt::BlobComponentDigestPassV3 changed;
    Check(changed.BeginDecode(profile,320+length,expected).ok(),"long integrity input admission");
    const auto at = length/2;
    content[at] ^= 0x40;
    Check(changed.UpdateContent(bytes).ok() && changed.FinishDecode().reason_enum == 30,
          "long same-length content substitution escaped logical digest verification");
    content[at] ^= 0x40;
  }
  // Caller-owned, zero-initialized fixed workspace permits byte-exact evidence
  // that the destructor scrubbed every retained header/digest/hash-state byte,
  // including an active buffered plaintext pass, not just finalized states.
  for (unsigned phase = 0; phase < 4; ++phase) {
    alignas(dt::BlobComponentDigestPassV3)
        std::array<platform::byte,sizeof(dt::BlobComponentDigestPassV3)> workspace{};
    auto* pass = std::construct_at(reinterpret_cast<dt::BlobComponentDigestPassV3*>(workspace.data()));
    dt::BlobComponentHeaderV3 header{};
    Check(pass->BeginEncode(profile,dt::BlobValueStateV3::value,40).ok() &&
              pass->UpdateContent(std::span(content).first(phase == 0 ? 17 : 40)).ok(),
          "scrub test buffered native content admission");
    if (phase == 1) Check(pass->FinishEncode(header).ok(),"scrub finalized encode");
    if (phase == 2) Check(!pass->UpdateContent(std::span(content).first(1)).ok(),"scrub failed encode");
    if (phase == 3) {
      Check(pass->FinishEncode(header).ok(),"scrub decode header construction"); pass->Scrub();
      Check(pass->BeginDecode(profile,360,header).ok() && pass->UpdateContent(std::span(content).first(17)).ok(),
            "scrub active decode input");
    }
    std::destroy_at(pass);
    Check(std::all_of(workspace.begin(),workspace.end(),[](auto b) { return b == 0; }),
          "component destruction retained native content, expected digest or hash state");
  }
}

void BlobComponentInitialHeaderAdmission() {
  const auto profile = dt::BuildCurrentBlobValidatedProfileHandleV3(dt::kBlobV11ReceiptUuid).profile;
  const auto page = static_cast<std::size_t>(sysconf(_SC_PAGESIZE));
  Check(page >= 320,"native page cannot contain component header fixture");
  auto* region = static_cast<platform::byte*>(mmap(nullptr,3*page,PROT_NONE,MAP_PRIVATE|MAP_ANONYMOUS,-1,0));
  Check(region != MAP_FAILED,"header-only guarded source allocation");
  auto* source = region+page;
  for (const auto& v : kBlobComponentGoldenVectors) {
    Check(mprotect(source,page,PROT_READ|PROT_WRITE) == 0,"header-only source write setup");
    std::array<platform::byte,360> full{};
    const auto extent = LoadBlobGolden(v,full);
    std::copy_n(full.begin(),320,source+page-320);
    Check(mprotect(source,page,PROT_READ) == 0,"header-only source read-only guard");
    // Each shortened span ends at PROT_NONE, including the zero span located
    // inside the guard page. An overread, write or input scrub is a real fault.
    const auto allocations_before = allocation_probe::calls.load();
    allocation_probe::enabled.store(true);
    for (std::size_t n = 0; n <= 320; ++n) {
      const auto result = dt::ValidateBlobComponentHeaderNoAllocV3(
          profile,extent,{source+page-n,n});
      if (n == 320) Check(result.ok(),"exact header-only shape gate rejected independent oracle");
      else Check(result.failure == dt::BlobComponentPassFailureV3::canonical_encoding_invalid &&
                     result.reason_enum == 41 && result.failure_offset_present && result.failure_offset == n,
                 "short initial span read outside admission or lost first-unavailable offset");
    }
    const auto excess = dt::ValidateBlobComponentHeaderNoAllocV3(profile,extent,{source+page-321,321});
    Check(excess.failure == dt::BlobComponentPassFailureV3::input_extent_invalid &&
              excess.diagnostic_code.empty() && excess.reason_enum == 0 &&
              excess.diagnostic_uuid == platform::Uuid{} && excess.diagnostic_generation == 0 &&
              allocation_probe::calls.load() == allocations_before,
          "non-exact header call invented malformed-frame authority or allocated hash/header state");
    allocation_probe::enabled.store(false);
  }
  auto invalid = profile; ++invalid.identity.descriptor_generation;
  const auto before = dt::ValidateBlobComponentHeaderNoAllocV3(invalid,320,{});
  Check(before.failure == dt::BlobComponentPassFailureV3::profile_invalid && before.diagnostic_code.empty(),
        "invalid profile was relabeled as observed input failure");
  Check(munmap(region,3*page) == 0,"header-only guarded source cleanup");
  std::array<platform::byte,360> full{};
  const auto extent = LoadBlobGolden(kBlobComponentGoldenVectors[3],full);
  auto header = std::span(full).first(320);
  Check(dt::ValidateBlobComponentHeaderNoAllocV3(profile,extent,header).ok(),
        "initial shape test admission");
  full[40] ^= 1;
  dt::BlobComponentDigestPassV3 changed;
  Check(changed.BeginDecode(profile,extent,header).reason_enum == 51,
        "earlier header shape success admitted changed identity into digest pass");
  full[40] ^= 1; full[248] ^= 1; full[280] ^= 1;
  Check(dt::ValidateBlobComponentHeaderNoAllocV3(profile,extent,header).ok(),
        "initial shape incorrectly claimed to verify unprocessed digest fields");
  changed.Scrub();
  Check(changed.BeginDecode(profile,extent,header).ok() &&
            changed.UpdateContent(std::span(full).subspan(320,extent-320)).ok() &&
            changed.FinishDecode().reason_enum == 30,
        "shape-only success bypassed both-digest verification or registered mismatch precedence");
}

struct GuardedReaderContext {
  FakeReceiver* fake = nullptr;
  unsigned mode = 0;
  std::uint64_t calls = 0;
  std::uint64_t visited = 0;
  std::uint64_t visitor_calls = 0;
  std::uint64_t expected_reader_offset = 0;
  std::uint64_t expected_deadline = 0;
};

dt::BlobReaderResultV3 GuardedReader(void* raw, const dt::BlobReaderRequestV3& request) {
  auto& c = *static_cast<GuardedReaderContext*>(raw);
  if (CountCall(*c.fake, 'B') != CountCall(*c.fake, 'E')+1 ||
      request.offset != c.expected_reader_offset || request.requested_octets == 0 ||
      request.requested_octets > 4 || request.destination_capacity != request.requested_octets)
    std::cerr << "reader boundary B=" << +CountCall(*c.fake, 'B') << " E=" << +CountCall(*c.fake, 'E')
              << " offset=" << request.offset << " expected=" << c.expected_reader_offset
              << " requested=" << request.requested_octets << " capacity=" << request.destination_capacity << '\n';
  Check(CountCall(*c.fake, 'B') == CountCall(*c.fake, 'E')+1 &&
            request.offset == c.expected_reader_offset && request.requested_octets > 0 &&
            request.requested_octets <= 4 && request.destination_capacity == request.requested_octets,
        "reader callback escaped its access pair or requested the wrong bounded offset");
  Check(request.absolute_deadline_monotonic_ns == c.expected_deadline,
        "reader callback did not receive effective minimum nonzero deadline");
  ++c.calls;
  reader_destination_to_scrub = {request.destination,
                                static_cast<std::size_t>(request.destination_capacity)};
  for (std::uint64_t i = 0; i < request.destination_capacity; ++i)
    request.destination[i] = static_cast<platform::byte>(0x91U ^ (request.offset+i));
  if (c.mode == 6) throw 1;
  dt::BlobReaderResultV3 result;
  result.status = 0;
  result.actual_octets = request.requested_octets;
  if (c.mode == 1) result.actual_octets = 1;
  if (c.mode == 2) result.actual_octets = 0;
  if (c.mode == 3 || c.mode == 19) result.actual_octets = request.requested_octets+1;
  if (c.mode == 4) result.status = 255;
  if (c.mode == 5) result.reserved[3] = 1;
  if (c.mode >= 7 && c.mode <= 15) {
    result.status = 1;
    constexpr std::array<std::uint32_t,9> facts{{128,1,2,4,8,16,32,64,256}};
    result.fact_bits = facts[c.mode-7];
    if (c.mode == 7) result.diagnostic_facts.reason_enum = 17;
    if (c.mode == 10) result.diagnostic_facts.lifetime_enum = result.diagnostic_facts.expiry_reason_enum = 3;
    if (c.mode == 11) result.diagnostic_facts.reason_enum = 26;
    if (c.mode == 12) result.diagnostic_facts.reason_enum = 12;
    if (c.mode == 13) { result.diagnostic_facts.reason_enum = 16; result.diagnostic_facts.window_octets = 4; }
    if (c.mode == 14) {
      result.diagnostic_facts.resource_enum = 5;
      result.diagnostic_facts.required = 7; result.diagnostic_facts.available = 6;
    }
    if (c.mode == 15) { result.diagnostic_facts.reason_enum = 37; result.diagnostic_facts.format_enum = 6; }
  }
  if (c.mode == 16 || c.mode == 20) c.fake->cancelled = true;
  if (c.mode == 20) {
    result.status = 1;
    result.fact_bits = 64;
    result.diagnostic_facts.resource_enum = 5;
    result.diagnostic_facts.required = 7; result.diagnostic_facts.available = 6;
  }
  if (c.mode == 18) result.actual_octets = UINT64_MAX;
  c.expected_reader_offset += result.actual_octets;
  return result;
}

dt::BlobProtectedWorkResultV3 GuardedReaderVisitor(void* raw,
    std::span<const platform::byte> bytes) {
  auto& c = *static_cast<GuardedReaderContext*>(raw);
  ++c.visitor_calls;
  Check(CountCall(*c.fake,'B') == CountCall(*c.fake,'E')+1,
        "reader visitor ran outside an exact access pair");
  for (const auto b : bytes) {
    Check(b == static_cast<platform::byte>(0x91U ^ c.visited),
          "reader visitor lost native octets or short-read offset");
    ++c.visited;
  }
  if (c.mode == 17) throw 1;
  dt::BlobProtectedWorkResultV3 result;
  result.status = c.mode == 21 ? 255 : 0;
  return result;
}

void RetainGuardedReader(Fixture& f, std::uint64_t extent, bool encoded,
    GuardedReaderContext& context, dt::BlobRetainedLifetimeLeaseV3& lease,
    dt::BlobValueStateV3 state = dt::BlobValueStateV3::value,
    dt::BlobReadAtV3 read_at = GuardedReader) {
  using Host = dt::BlobLifetimeRuntimeConformanceAccessV3;
  f.token.mode_bits = SB_BLOB_MODE_IMMUTABLE_READ_V3 |
      (encoded ? SB_BLOB_MODE_OPAQUE_ENCODED_READER_V3 : SB_BLOB_MODE_LOGICAL_READER_V3);
  if (!encoded && (f.request.operation == 3 || f.request.operation == 8))
    f.token.mode_bits |= SB_BLOB_MODE_REPLAYABLE_SAME_SNAPSHOT_V3;
  if (encoded) f.token.owner_class = f.request.required_owner_class = SB_BLOB_OWNER_ENCODED_ARTIFACT_BORROW_V3;
  f.request.required_mode_bits = SB_BLOB_MODE_IMMUTABLE_READ_V3;
  f.carrier_binding.carrier_kind = encoded ? SB_BLOB_MODE_OPAQUE_ENCODED_READER_V3
                                          : SB_BLOB_MODE_LOGICAL_READER_V3;
  const auto td = f.token.valid_until_monotonic_ns;
  const auto rd = f.request.absolute_deadline_monotonic_ns;
  context.expected_deadline = td == 0 ? rd : rd == 0 ? td : std::min(td,rd);
  auto budget = Host::BudgetControl(f.ledger, f.pins);
  dt::BlobLifetimeAuthorityAdmissionV3Generation1 admission;
  if (encoded) {
    auto cap = Host::EncodedCapability(f.carrier_binding, extent, &context, read_at);
    dt::BlobBoundEncodedReaderCarrierV3Generation1 carrier;
    Check(dt::BlobLifetimeRuntimeFactoryV3Generation1::AdmitEncodedReader(f.request.operation,
        f.profile, &f.authority, &f.services, f.pins, budget, f.services.monotonic_clock,
        f.services.monotonic_clock, f.token, f.request, cap, admission, carrier).ok(),
        "guarded encoded factory admission");
    Check(dt::RetainBaseBlobLifetimeLeaseV3Generation1(f.profile, std::move(admission),
        carrier, f.token, f.request, lease).ok(), "guarded encoded retain");
  } else {
    auto cap = Host::LogicalCapability(f.carrier_binding, state,
                                      extent, &context, read_at);
    dt::BlobBoundLogicalReaderCarrierV3Generation1 carrier;
    Check(dt::BlobLifetimeRuntimeFactoryV3Generation1::AdmitLogicalReader(f.request.operation,
        f.profile, &f.authority, &f.services, f.pins, budget, f.services.monotonic_clock,
        f.services.monotonic_clock, f.token, f.request, cap, admission, carrier).ok(),
        "guarded logical factory admission");
    Check(dt::RetainBaseBlobLifetimeLeaseV3Generation1(f.profile, std::move(admission),
        carrier, f.token, f.request, lease).ok(), "guarded logical retain");
  }
}

void GuardedReaderExecution() {
  using Access = dt::BlobLifetimeRuntimeConformanceAccessV3;
  for (unsigned op = 2; op <= 13; ++op) {
    const bool encoded = op >= 9;
    for(bool ranged:{false,true}) {
      if(ranged && !encoded) continue;
    for (unsigned mode = 0; mode <= 21; ++mode) {
      std::array<platform::byte,16> scratch{};
      dt::BlobLifetimeBudgetConfigurationV3Generation1 config;
      config.limits.fill(UINT64_MAX);
      config.scratch = scratch;
      config.admitted_window_octets = 4;
      if (mode == 19) config.limits[(encoded ? 3 : 2)-1] = 4;
      Fixture f(64,0,&config);
      f.fake.pins = 0;
      f.request.operation = op;
      if (mode%4 == 1) f.token.valid_until_monotonic_ns = 200;
      if (mode%4 == 2) f.request.absolute_deadline_monotonic_ns = 300;
      if (mode%4 == 3) {
        f.token.valid_until_monotonic_ns = 300;
        f.request.absolute_deadline_monotonic_ns = 200;
      }
      GuardedReaderContext context{&f.fake,mode};
      if(ranged) context.expected_reader_offset=context.visited=2;
      dt::BlobRetainedLifetimeLeaseV3 lease;
      RetainGuardedReader(f,mode == 19 && !ranged ? 4 : 9,encoded,context,lease);
      const auto bytes_resource = encoded ? dt::BlobLifetimeResourceV3::encoded_bytes_read
                                          : dt::BlobLifetimeResourceV3::logical_bytes_read;
      if (mode == 18) Access::SetCounter(f.ledger,bytes_resource,UINT64_MAX,1,0);
      auto visitor = Access::Visitor(GuardedReaderVisitor,&context);
      const auto before = allocation_probe::calls.load();
      allocation_probe::enabled.store(true);
      const auto result = ranged ? lease.VisitBoundEncodedRangeBytes(2,mode==19 ? 4 : 7,visitor)
                                 : lease.VisitBoundReaderBytes(visitor);
      const bool success = mode <= 1;
      const auto released = success ? lease.Release() : dt::BlobLifetimeRuntimeResultV3{};
      allocation_probe::enabled.store(false);
      if (success) {
        Check(result.ok() && released.ok() && context.visited == 9 &&
                  context.calls == (mode == 0 ? (ranged ? 2 : 3) : (ranged ? 7 : 9)) &&
                  CountCall(f.fake,'B') == context.calls && CountCall(f.fake,'E') == context.calls &&
                  CountCall(f.fake,'P') == 1 && Access::Invoked(f.ledger) == 2*context.calls+3,
              "complete guarded traversal lost exact bytes/callback formula/final probe");
      } else {
        const bool private_failure = mode == 6 || mode == 17 || mode == 18 || mode == 21;
        constexpr std::array<std::string_view,22> expected{{"","",
          "CINL.LOB.DESCRIPTOR_INVALID","CINL.LOB.DESCRIPTOR_INVALID","CINL.LOB.DESCRIPTOR_INVALID",
          "CINL.LOB.DESCRIPTOR_INVALID","","BLOB.IO_FAILED","SECURITY.ACCESS_DENIED",
          "PROCESS.CANCELLED","CINL.LOB.HANDLE_EXPIRED","BLOB.LIFETIME_AUTHORITY_UNAVAILABLE",
          "CINL.LOB.DESCRIPTOR_INVALID","CINL.LOB.STREAM_BACKPRESSURE_EXHAUSTED",
          "RESOURCE.BUDGET_EXCEEDED","CINL.LOB.DESCRIPTOR_INVALID","PROCESS.CANCELLED","","",
          "CINL.LOB.DESCRIPTOR_INVALID","PROCESS.CANCELLED",""}};
        Check(!result.ok() && result.diagnostic_code == expected[mode] &&
                  (result.disposition == dt::BlobLifetimeOuterDispositionV3::terminated_out_of_band) == private_failure &&
                  CountCall(f.fake,'B') == 1 && CountCall(f.fake,'E') == 1 && CountCall(f.fake,'P') == 0,
              "guarded reader failure lost exact classification, cleanup or precedence");
        if (mode == 7) Check(result.fact.parameters.adapter_kind_enum == 1 &&
            result.fact.parameters.operation_enum == op && result.fact.reason == dt::BlobLifetimeReasonV3::reader_io_failure,
            "actual reader IO lost admitted adapter/operation provenance");
        if (mode == 11) Check(result.fact.authority_source == dt::BlobLifetimeAuthoritySourceV3::reader_callback,
            "reader availability fact fabricated a lifetime callback code");
        Check(context.visitor_calls == ((mode == 17 || mode == 21) ? 1U : 0U),
              "unaccepted reader bytes reached visitor");
      }
      Check(CountCall(f.fake,'L') == 1 && f.fake.pins == 0 &&
                Access::ReservedCleanup(f.ledger) == 0 &&
                Access::Invoked(f.ledger,dt::BlobLifetimeResourceV3::temporary_bytes) == 4 &&
                allocation_probe::calls.load() == before &&
                std::all_of(scratch.begin(),scratch.end(),[](auto b) { return b == 0; }),
            "guarded reader leaked pins, dirty scratch, allocations or temporary charge");
      const auto observed = Access::Invoked(f.ledger,bytes_resource);
      Check(observed == (mode == 6 || mode == 18 ? (mode == 18 ? 1U : 0U) :
              success ? (ranged ? 7U : 9U) : mode == 2 ? 0U : mode == 3 || mode == 19 ? 5U : 4U),
            "returned byte count was not charged before malformed/failed reply rejection");
    }
    }
  }
}

void GuardedEncodedRangeBoundaries() {
  using Access=dt::BlobLifetimeRuntimeConformanceAccessV3;
  for(unsigned operation=9;operation<=13;++operation) for(unsigned edge=0;edge<8;++edge) {
    std::array<platform::byte,16> scratch{};
    dt::BlobLifetimeBudgetConfigurationV3Generation1 config;
    config.limits.fill(UINT64_MAX); config.scratch=scratch; config.admitted_window_octets=4;
    Fixture f(64,0,&config); f.fake.pins=0; f.request.operation=operation;
    const std::uint64_t extent=edge<4 ? UINT64_MAX : 9;
    std::uint64_t offset=extent,length=0;
    if(edge==0) { offset=extent-3; length=3; }
    if(edge==2) length=1; // End+one must not wrap to zero.
    if(edge==3) { offset=extent-3; length=4; }
    if(edge==4) offset=10;
    if(edge==5) { offset=8; length=2; }
    if(edge==6) { offset=0; length=UINT64_MAX; }
    if(edge==7) { offset=9; length=0; }
    const bool valid=edge==0 || edge==1 || edge==7;
    GuardedReaderContext context{&f.fake,0};
    context.expected_reader_offset=context.visited=offset;
    dt::BlobRetainedLifetimeLeaseV3 lease;
    RetainGuardedReader(f,extent,true,context,lease);
    auto visitor=Access::Visitor(GuardedReaderVisitor,&context);
    const auto before=allocation_probe::calls.load(); allocation_probe::enabled.store(true);
    const auto result=lease.VisitBoundEncodedRangeBytes(offset,length,visitor);
    if(lease.retained()) Check(lease.Release().ok(),"encoded range boundary retained cleanup");
    allocation_probe::enabled.store(false);
    Check(result.ok()==valid && (valid || result.disposition==dt::BlobLifetimeOuterDispositionV3::terminated_out_of_band),
        "encoded range overflow/out-of-bounds was accepted or fabricated a public data failure");
    Check(context.calls==(edge==0 ? 1u : 0u) && context.visitor_calls==context.calls &&
        CountCall(f.fake,'B')==context.calls && CountCall(f.fake,'E')==context.calls &&
        CountCall(f.fake,'P')==(valid ? 1u : 0u) && CountCall(f.fake,'L')==1 && f.fake.pins==0 &&
        Access::Invoked(f.ledger,dt::BlobLifetimeResourceV3::encoded_bytes_read)==(edge==0 ? 3u : 0u) &&
        Access::Invoked(f.ledger,dt::BlobLifetimeResourceV3::logical_bytes_read)==0 &&
        Access::ReservedCleanup(f.ledger)==0 && allocation_probe::calls.load()==before &&
        std::all_of(scratch.begin(),scratch.end(),[](auto b){return b==0;}),
        "encoded range boundary read outside bounds, skipped empty probe or leaked scratch/tickets/pins");
  }
  // Range selection cannot turn a logical reader into an encoded capability.
  {
    std::array<platform::byte,16> scratch{};
    dt::BlobLifetimeBudgetConfigurationV3Generation1 config;
    config.limits.fill(UINT64_MAX); config.scratch=scratch; config.admitted_window_octets=4;
    Fixture f(64,0,&config); f.fake.pins=0; f.request.operation=2;
    GuardedReaderContext context{&f.fake,0}; dt::BlobRetainedLifetimeLeaseV3 lease;
    RetainGuardedReader(f,9,false,context,lease); auto visitor=Access::Visitor(GuardedReaderVisitor,&context);
    const auto result=lease.VisitBoundEncodedRangeBytes(0,1,visitor);
    Check(!result.ok() && result.disposition==dt::BlobLifetimeOuterDispositionV3::terminated_out_of_band &&
        context.calls==0 && context.visitor_calls==0 && f.fake.pins==0 && CountCall(f.fake,'L')==1,
        "encoded range method upgraded a logical capability or leaked its cleanup");
  }
}

void ReservedSinkWorkAccounting() {
  using Access=dt::BlobLifetimeRuntimeConformanceAccessV3;
  using Resource=dt::BlobLifetimeResourceV3;
  dt::BlobLifetimeBudgetLedgerV3Generation1 ledger;
  dt::BlobLifetimeBudgetConfigurationV3Generation1 config;
  config.limits.fill(UINT64_MAX);
  config.limits[static_cast<unsigned>(Resource::sink_bytes_written)-1]=12;
  config.limits[static_cast<unsigned>(Resource::sink_calls)-1]=3;
  Check(Access::Configure(ledger,config)&&Access::Freeze(ledger),"reserved sink actual ledger construction");
  Resource resource{}; std::uint64_t available=0;
  Check(Access::ReserveSink(ledger,8,2,resource,available),"exact sink reservation refused");
  const auto initial=Access::Snapshot(ledger);
  Check(initial.counters[4].reserved_work==8 && initial.counters[6].reserved_work==2,
      "sink reservation did not atomically retain both credits");
  Check(!Access::ReserveSink(ledger,5,1,resource,available) && resource==Resource::sink_bytes_written && available==4 &&
      !Access::ReserveSink(ledger,1,2,resource,available) && resource==Resource::sink_calls && available==1,
      "sink reservation admitted excess bytes/calls or lost exact shortage");
  const auto refused=Access::Snapshot(ledger);
  Check(refused.counters[4].reserved_work==8 && refused.counters[6].reserved_work==2 &&
      refused.counters[4].invoked==0 && refused.counters[6].invoked==0,
      "failed sink reservation partially changed other credits or fabricated invocation");
  Check(!Access::Charge(ledger,Resource::sink_bytes_written,5,&available) && available==4 &&
      Access::Charge(ledger,Resource::sink_bytes_written,4,&available) && Access::ConsumeSink(ledger,2) &&
      Access::CancelSink(ledger,6,1),"reserved sink work was stolen, not settled or not cancelled");
  auto settled=Access::Snapshot(ledger);
  Check(settled.counters[4].invoked==6 && settled.counters[6].invoked==1 &&
      !settled.counters[4].reserved_work && !settled.counters[6].reserved_work,
      "sink cancellation refunded real work or retained unused debt");
  Check(Access::ReserveSink(ledger,6,1,resource,available),"remaining exact sink capacity refused");
  Access::SetCounter(ledger,Resource::logical_bytes_read,1,2,0);
  Check(!Access::Charge(ledger,Resource::logical_bytes_read,0,&available) &&
      Access::Snapshot(ledger).terminal_quarantined && Access::CancelSink(ledger,6,1),
      "other-resource quarantine blocked unconditional reserved sink cleanup");
  settled=Access::Snapshot(ledger);
  Check(settled.counters[4].invoked==6 && settled.counters[6].invoked==1 &&
      !settled.counters[4].reserved_work && !settled.counters[6].reserved_work && settled.terminal_quarantined,
      "sink cleanup hid quarantine or invented/refunded actual work");
  dt::BlobLifetimeBudgetLedgerV3Generation1 maximum;
  config.limits.fill(UINT64_MAX);
  Check(Access::Configure(maximum,config)&&Access::Freeze(maximum)&&
      Access::ReserveSink(maximum,UINT64_MAX,1,resource,available)&&Access::ConsumeSink(maximum,UINT64_MAX)&&
      !Access::ReserveSink(maximum,1,0,resource,available)&&resource==Resource::sink_bytes_written&&available==0&&
      Access::CancelSink(maximum,0,0),"sink UINT64_MAX settlement wrapped or rejected its exact maximum");
  const auto exact=Access::Snapshot(maximum);
  Check(exact.counters[4].invoked==UINT64_MAX && exact.counters[6].invoked==1 &&
      !exact.counters[4].reserved_work && !exact.counters[6].reserved_work && !exact.terminal_quarantined,
      "exact maximum sink settlement corrupted other counters");
}

dt::BlobReaderResultV3 ReplayReader(void* raw, const dt::BlobReaderRequestV3& request) {
  auto& c = *static_cast<GuardedReaderContext*>(raw);
  if (request.offset == 0 && c.calls != 0) {
    Check(CountCall(*c.fake, 'P') == 1 && reader_destination_to_scrub.empty(),
          "replay reset offset without exactly one transition probe or retained protected bytes");
    c.expected_reader_offset = 0;
  }
  return GuardedReader(raw, request);
}

void GuardedReaderReplayExecution() {
  using Access = dt::BlobLifetimeRuntimeConformanceAccessV3;
  for (unsigned op : {3U,8U}) {
    for (unsigned short_read : {0U,1U}) {
      for (std::uint64_t extent : {0U,1U,4U,9U}) {
        std::array<platform::byte,4> scratch{};
        dt::BlobLifetimeBudgetConfigurationV3Generation1 config;
        config.limits.fill(UINT64_MAX);
        config.scratch = scratch;
        config.admitted_window_octets = 4;
        const auto calls_per_pass = short_read ? extent : (extent+3)/4;
        config.limits[0] = extent == 0 ? 3 : 4*calls_per_pass+4;
        config.limits[1] = 2*extent;
        config.limits[4] = 2*calls_per_pass;
        config.limits[7] = std::min<std::uint64_t>(4,extent);
        Fixture f(0,0,&config);
        f.fake.pins = 0;
        f.request.operation = op;
        GuardedReaderContext source{&f.fake,short_read};
        GuardedReaderContext first{&f.fake}, second{&f.fake};
        dt::BlobRetainedLifetimeLeaseV3 lease;
        RetainGuardedReader(f,extent,false,source,lease,dt::BlobValueStateV3::value,ReplayReader);
        auto first_visitor = Access::Visitor(GuardedReaderVisitor,&first);
        auto second_visitor = Access::Visitor(GuardedReaderVisitor,&second);
        const auto before = allocation_probe::calls.load();
        allocation_probe::enabled.store(true);
        const auto result = lease.VisitBoundReplayableReaderBytes(first_visitor,second_visitor);
        const auto released = lease.Release();
        allocation_probe::enabled.store(false);
        Check(result.ok() && released.ok() && first.visited == extent && second.visited == extent &&
                  source.calls == 2*calls_per_pass && CountCall(f.fake,'P') == (extent ? 2U : 1U) &&
                  CountCall(f.fake,'B') == source.calls && CountCall(f.fake,'E') == source.calls &&
                  Access::Invoked(f.ledger) == config.limits[0] &&
                  Access::Invoked(f.ledger,dt::BlobLifetimeResourceV3::logical_bytes_read) == 2*extent &&
                  Access::Invoked(f.ledger,dt::BlobLifetimeResourceV3::reader_calls) == source.calls &&
                  Access::Invoked(f.ledger,dt::BlobLifetimeResourceV3::temporary_bytes) == std::min<std::uint64_t>(4,extent) &&
                  allocation_probe::calls.load() == before && f.fake.pins == 0 &&
                  Access::ReservedCleanup(f.ledger) == 0,
              "replay lost exact shared budgets, independent offsets, exclusive ownership or 2N+4 formula");
        Check(std::all_of(scratch.begin(),scratch.end(),[](auto b) { return b == 0; }),
              "replay retained source octets across access or operation completion");
      }
    }
  }
  // NULL is independently authenticated, not replayed and not confused with
  // empty VALUE. Neither consumer may observe an empty protected span.
  Fixture f(3);
  f.fake.pins = 0;
  f.request.operation = 8;
  GuardedReaderContext source{&f.fake},first{&f.fake},second{&f.fake};
  dt::BlobRetainedLifetimeLeaseV3 lease;
  RetainGuardedReader(f,0,false,source,lease,dt::BlobValueStateV3::sql_null,ReplayReader);
  auto a = Access::Visitor(GuardedReaderVisitor,&first);
  auto b = Access::Visitor(GuardedReaderVisitor,&second);
  Check(lease.VisitBoundReplayableReaderBytes(a,b).ok() && lease.Release().ok() &&
            source.calls == 0 && first.visitor_calls == 0 && second.visitor_calls == 0 &&
            CountCall(f.fake,'P') == 1 && Access::Invoked(f.ledger) == 3 && f.fake.pins == 0,
        "replay SQL NULL bypassed authentication or invoked a payload consumer");
}

void GuardedReaderReplayFailureAndOwnership() {
  using Access = dt::BlobLifetimeRuntimeConformanceAccessV3;
  for (unsigned failure = 0; failure < 12; ++failure) {
    const auto page = static_cast<std::size_t>(sysconf(_SC_PAGESIZE));
    auto* backing = static_cast<platform::byte*>(mmap(nullptr,page,PROT_READ|PROT_WRITE,
        MAP_PRIVATE|MAP_ANONYMOUS,-1,0));
    Check(backing != MAP_FAILED,"replay poison backing allocation");
    dt::BlobLifetimeBudgetConfigurationV3Generation1 config;
    config.limits.fill(UINT64_MAX);
    config.scratch = {backing,4};
    config.admitted_window_octets = 4;
    // Nine bytes in three calls per pass. Exhaust exactly at the first
    // transition, or independently at the second pass's first byte/call.
    if (failure == 8) config.limits[0] = 8; // retain+first pass+reserved release
    if (failure == 9) config.limits[1] = 9;
    if (failure == 10) config.limits[4] = 3;
    Fixture f(0,0,&config);
    f.fake.pins = 0;
    f.request.operation = 8;
    if (failure == 7) f.token.valid_until_monotonic_ns = 200;
    GuardedReaderContext source{&f.fake},first{&f.fake},second{&f.fake};
    dt::BlobRetainedLifetimeLeaseV3 lease;
    RetainGuardedReader(f,9,false,source,lease,dt::BlobValueStateV3::value,ReplayReader);
    f.fake.poison_reader_scratch_on_last_drop = backing;
    f.fake.poison_reader_scratch_size = page;
    if (failure == 0) f.fake.probe_code = SB_BLOB_CALLBACK_STALE_SNAPSHOT_V3;
    if (failure == 1) f.fake.probe_code = SB_BLOB_CALLBACK_SECURITY_DENIED_V3;
    if (failure == 2) f.fake.probe_code = SB_BLOB_CALLBACK_AUTHORITY_UNAVAILABLE_V3;
    if (failure == 3) f.fake.throw_probe = true;
    if (failure == 4) f.fake.dirty_probe_ticket = true;
    if (failure == 5) { f.fake.malformed_phase = 2; f.fake.malformed_stimulus = 1; }
    if (failure == 6) f.fake.cancel_on_probe = true;
    if (failure == 7) f.fake.clock_on_probe = 200;
    if (failure == 11) second.mode = 17; // second consumer throws after actual read
    auto a = Access::Visitor(GuardedReaderVisitor,&first);
    auto b = Access::Visitor(GuardedReaderVisitor,&second);
    const auto result = lease.VisitBoundReplayableReaderBytes(a,b);
    constexpr std::array<std::string_view,12> expected{{
      "CINL.LOB.HANDLE_EXPIRED","SECURITY.ACCESS_DENIED","BLOB.LIFETIME_AUTHORITY_UNAVAILABLE","",
      "CINL.LOB.DESCRIPTOR_INVALID","CINL.LOB.DESCRIPTOR_INVALID","PROCESS.CANCELLED",
      "CINL.LOB.HANDLE_EXPIRED","RESOURCE.BUDGET_EXCEEDED","RESOURCE.BUDGET_EXCEEDED",
      "RESOURCE.BUDGET_EXCEEDED",""}};
    Check(!result.ok() && result.diagnostic_code == expected[failure] && first.visited == 9 &&
              source.calls == (failure == 11 ? 4U : 3U) &&
              second.visitor_calls == (failure == 11 ? 1U : 0U) &&
              CountCall(f.fake,'P') == (failure == 8 ? 0U : 1U) &&
              CountCall(f.fake,'L') == 1 && f.fake.pins == 0 &&
              Access::ReservedCleanup(f.ledger) == 0 &&
              Access::Invoked(f.ledger,dt::BlobLifetimeResourceV3::logical_bytes_read) == (failure == 11 ? 13U : 9U),
          "replay failure admitted second-pass work, lost exact precedence/settlement or accessed backing after unpin");
    if (failure >= 8 && failure <= 10) {
      Check(result.resource == (failure == 8 ? dt::BlobLifetimeResourceV3::lifetime_callback_calls :
                  failure == 9 ? dt::BlobLifetimeResourceV3::logical_bytes_read : dt::BlobLifetimeResourceV3::reader_calls) &&
                result.required == (failure == 9 ? 4U : 1U) && result.available == 0,
            "replay exhaustion lost exact independent remaining resource parameters");
    }
    Check(munmap(backing,page) == 0,"replay poison backing cleanup");
    f.fake.poison_reader_scratch_on_last_drop = nullptr;
  }
  // Hold the actual transition callback while a second thread tries Release.
  // The losing thread must not see idle, steal the owner or acquire an access
  // pair. The original owner drains the deferred failure without pass two.
  {
    std::array<platform::byte,4> scratch{};
    dt::BlobLifetimeBudgetConfigurationV3Generation1 config;
    config.limits.fill(UINT64_MAX); config.scratch = scratch; config.admitted_window_octets = 4;
    Fixture f(0,0,&config); f.fake.pins = 0; f.request.operation = 8;
    GuardedReaderContext source{&f.fake},first{&f.fake},second{&f.fake};
    dt::BlobRetainedLifetimeLeaseV3 lease;
    RetainGuardedReader(f,9,false,source,lease,dt::BlobValueStateV3::value,ReplayReader);
    f.fake.block_probe = true;
    auto a = Access::Visitor(GuardedReaderVisitor,&first);
    auto b = Access::Visitor(GuardedReaderVisitor,&second);
    dt::BlobLifetimeRuntimeResultV3 owner;
    std::thread worker([&] { owner = lease.VisitBoundReplayableReaderBytes(a,b); });
    while (!f.fake.probe_entered.load(std::memory_order_acquire)) std::this_thread::yield();
    const auto losing = lease.Release();
    f.fake.finish_probe.store(true,std::memory_order_release);
    worker.join();
    Check(!owner.ok() && !losing.ok() && owner.reason == "same_ticket_concurrent_use" &&
              losing.reason == owner.reason && source.calls == 3 && second.visitor_calls == 0 &&
              CountCall(f.fake,'P') == 1 && CountCall(f.fake,'L') == 1 &&
              f.fake.last_invariant_operation == 8 &&
              f.fake.last_invariant_phase == dt::BlobLifetimePhaseV3::protected_access &&
              f.fake.invariant_calls == 1 && f.fake.pins == 0,
          "transition probe exposed an idle window or lost actual owner provenance/cleanup");
  }
  // Concrete capabilities, not an operation selector, govern replay. These
  // internal misuse cases cannot call a reader or publish an empty success.
  for (bool encoded : {false,true}) {
    Fixture f(16); f.fake.pins = 0; f.request.operation = encoded ? 9 : 2;
    GuardedReaderContext source{&f.fake};
    dt::BlobRetainedLifetimeLeaseV3 lease;
    RetainGuardedReader(f,0,encoded,source,lease);
    auto visitor = Access::Visitor(GuardedReaderVisitor,&source);
    const auto result = lease.VisitBoundReplayableReaderBytes(visitor,visitor);
    Check(result.disposition == dt::BlobLifetimeOuterDispositionV3::terminated_out_of_band &&
              source.calls == 0 && CountCall(f.fake,'B') == 0 && CountCall(f.fake,'P') == 0 &&
              CountCall(f.fake,'L') == 1 && f.fake.invariant_calls == 1 && f.fake.pins == 0,
          "wrong concrete capability acquired replay authority through empty shortcut");
  }
}

struct ReplayTransitionObservation {
  GuardedReaderContext first;
  dt::BlobRetainedLifetimeLeaseV3* lease = nullptr;
  unsigned stimulus = 0;
  std::uint64_t cancellation_calls_before_gate = 0;
  dt::BlobLifetimeRuntimeResultV3 nested;
};

void ReplayTransitionClockAction(void* raw) noexcept {
  auto& c = *static_cast<ReplayTransitionObservation*>(raw);
  auto& f = *c.first.fake;
  c.cancellation_calls_before_gate = f.cancellation_calls;
  switch (c.stimulus) {
    case 0: f.clock_unavailable = true; break;
    case 1: f.cancellation_unavailable = true; break;
    case 2: f.clock_unavailable = f.cancellation_unavailable = true; break;
    case 3: f.cancelled = true; break;
    case 4: case 11: f.now = 200; break;
    case 5: c.nested = c.lease->Probe(); break;
    case 6: c.nested = c.lease->Release(); break;
    case 7:
      (void)dt::BlobLifetimeRuntimeConformanceAccessV3::ControlPublish(*c.lease,
          dt::BlobLifetimeInvariantEventV3::receiver_services_failed,8,
          dt::BlobLifetimePhaseV3::pass_transition,255);
      break;
    case 8: f.clock_dirty_reserved = true; break;
    case 9: f.cancellation_dirty_reserved = true; break;
    case 10: f.now = 0; break;
    default: std::terminate();
  }
}

dt::BlobProtectedWorkResultV3 ReplayTransitionArmingVisitor(
    void* raw, std::span<const platform::byte> bytes) {
  auto& c = *static_cast<ReplayTransitionObservation*>(raw);
  auto result = GuardedReaderVisitor(&c.first,bytes);
  if (c.first.visited == 9) {
    // The next sample is the returned-visitor observation; the following
    // sample is exactly the registered before_pass_transition gate. Target
    // that real call, not a callback-free emulation of the transition.
    c.first.fake->clock_action_call = c.first.fake->clock_calls+2;
    c.first.fake->clock_action = ReplayTransitionClockAction;
    c.first.fake->clock_action_context = &c;
  }
  return result;
}

void GuardedReaderReplayTransitionObservations() {
  using Access = dt::BlobLifetimeRuntimeConformanceAccessV3;
  for (unsigned stimulus = 0; stimulus < 12; ++stimulus) {
    std::array<platform::byte,4> scratch{};
    dt::BlobLifetimeBudgetConfigurationV3Generation1 config;
    config.limits.fill(UINT64_MAX); config.scratch = scratch; config.admitted_window_octets = 4;
    Fixture f(0,0,&config); f.fake.pins = 0; f.request.operation = 8;
    if (stimulus == 4) f.token.valid_until_monotonic_ns = 200;
    if (stimulus == 11) f.request.absolute_deadline_monotonic_ns = 200;
    GuardedReaderContext source{&f.fake},second{&f.fake};
    dt::BlobRetainedLifetimeLeaseV3 lease;
    RetainGuardedReader(f,9,false,source,lease,dt::BlobValueStateV3::value,ReplayReader);
    ReplayTransitionObservation observed{{&f.fake},&lease,stimulus};
    auto a = Access::Visitor(ReplayTransitionArmingVisitor,&observed);
    auto b = Access::Visitor(GuardedReaderVisitor,&second);
    const auto result = lease.VisitBoundReplayableReaderBytes(a,b);
    constexpr std::array<std::string_view,12> expected{{
      "BLOB.LIFETIME_AUTHORITY_UNAVAILABLE","BLOB.LIFETIME_AUTHORITY_UNAVAILABLE",
      "BLOB.LIFETIME_AUTHORITY_UNAVAILABLE","PROCESS.CANCELLED","CINL.LOB.HANDLE_EXPIRED",
      "CINL.LOB.DESCRIPTOR_INVALID","CINL.LOB.DESCRIPTOR_INVALID","",
      "BLOB.LIFETIME_AUTHORITY_UNAVAILABLE","BLOB.LIFETIME_AUTHORITY_UNAVAILABLE","",
      "CINL.LOB.STREAM_BACKPRESSURE_EXHAUSTED"}};
    Check(!result.ok() && result.diagnostic_code == expected[stimulus] && source.calls == 3 &&
              observed.first.visited == 9 && second.visitor_calls == 0 &&
              CountCall(f.fake,'P') == 0 && CountCall(f.fake,'L') == 1 &&
              f.fake.pins == 0 && Access::ReservedCleanup(f.ledger) == 0 &&
              Access::Invoked(f.ledger) == 8,
          "transition admission ignored sampler-time failure or invoked a new probe/pass after deferred conflict");
    if (stimulus <= 4 || stimulus == 8 || stimulus == 9 || stimulus == 11) {
      Check(result.fact.gate_present && result.fact.gate == dt::BlobLifetimeGateV3::before_pass_transition &&
                result.fact.stage == dt::BlobLifetimeFactStageV3::ordinary_gate,
            "transition failure substituted ordinary-probe gate provenance");
      if (stimulus != 4 && stimulus != 11)
        Check(result.fact.parameters.operation_enum == 8 &&
                  result.fact.parameters.phase_enum == dt::BlobLifetimePublicPhaseV3::final_probe,
              "transition failure lost actual operation or registered final_probe public phase");
      if (stimulus == 2) Check(result.reason == "monotonic_clock_unavailable",
          "simultaneous transition sampler failure lost deterministic clock-first tie");
    }
    if (stimulus == 5 || stimulus == 6)
      Check(!observed.nested.ok() && result.reason == "same_ticket_concurrent_use" &&
                f.fake.last_invariant_operation == 8 &&
                f.fake.last_invariant_phase == dt::BlobLifetimePhaseV3::protected_access,
            "sampler-time overlap lost actual owner and exact public conflict primary");
    Check(f.fake.cancellation_calls == observed.cancellation_calls_before_gate+
              (stimulus == 7 || stimulus == 10 ? 0U : 1U),
          "private transition failure sampled cancellation or public failure skipped required sampling");
  }
}

void GuardedReaderAdmissionAndPinLifetime() {
  using Access = dt::BlobLifetimeRuntimeConformanceAccessV3;
  for (bool encoded : {false,true}) {
    for (unsigned stimulus = 0; stimulus < 6; ++stimulus) {
      std::array<platform::byte,4> scratch{};
      dt::BlobLifetimeBudgetConfigurationV3Generation1 config;
      config.limits.fill(UINT64_MAX);
      config.scratch = stimulus >= 3 ? std::span(scratch).first(3) : std::span(scratch);
      config.admitted_window_octets = 4;
      if (stimulus <= 1) config.limits[(encoded ? 3 : 2)-1] = stimulus == 0 ? 0 : 3;
      if (stimulus == 2) config.limits[4] = 0;
      Fixture f(32,0,&config);
      f.fake.pins = 0;
      f.request.operation = encoded ? 9 : 2;
      if (stimulus == 5) f.token.valid_until_monotonic_ns = 200;
      GuardedReaderContext context{&f.fake};
      dt::BlobRetainedLifetimeLeaseV3 lease;
      RetainGuardedReader(f,9,encoded,context,lease);
      if (stimulus == 4) f.fake.cancelled = true;
      if (stimulus == 5) f.fake.now = 200;
      auto visitor = Access::Visitor(GuardedReaderVisitor,&context);
      const auto result = lease.VisitBoundReaderBytes(visitor);
      Check(result.diagnostic_code == (stimulus == 4 ? "PROCESS.CANCELLED" : stimulus == 5 ?
                "CINL.LOB.HANDLE_EXPIRED" : "RESOURCE.BUDGET_EXCEEDED") &&
                context.calls == 0 && context.visitor_calls == 0 && f.fake.pins == 0 &&
                CountCall(f.fake,'L') == 1 && CountCall(f.fake,'B') == (stimulus <= 2 ? 1U : 0U) &&
                CountCall(f.fake,'E') == (stimulus <= 2 ? 1U : 0U) &&
                Access::Invoked(f.ledger,dt::BlobLifetimeResourceV3::reader_calls) == 0,
            "reader admission leaked work or let scratch shortage outrank cancellation/expiry");
      if (stimulus < 4) Check(result.required == (stimulus == 2 ? 1U : 4U) &&
          result.available == (stimulus == 1 || stimulus == 3 ? 3U : 0U),
          "reader preflight fabricated resource required/available");
      Check(Access::Invoked(f.ledger,dt::BlobLifetimeResourceV3::temporary_bytes) ==
                (stimulus <= 2 ? 4U : 0U),
            "failed arena preflight changed cursor/charge or masked higher priority failure");
    }
    for (unsigned failure = 0; failure < 9; ++failure) {
      const auto page = static_cast<std::size_t>(sysconf(_SC_PAGESIZE));
      auto* backing = static_cast<platform::byte*>(mmap(nullptr,page,PROT_READ|PROT_WRITE,
          MAP_PRIVATE|MAP_ANONYMOUS,-1,0));
      Check(backing != MAP_FAILED,"poison reader backing allocation");
      dt::BlobLifetimeBudgetConfigurationV3Generation1 config;
      config.limits.fill(UINT64_MAX);
      config.scratch = {backing,page};
      config.admitted_window_octets = 4;
      Fixture f(32,0,&config);
      f.fake.pins = 0;
      f.request.operation = encoded ? 9 : 2;
      GuardedReaderContext context{&f.fake,failure == 6 ? 6U : failure == 7 ? 17U : failure == 8 ? 21U : 0U};
      dt::BlobRetainedLifetimeLeaseV3 lease;
      RetainGuardedReader(f,9,encoded,context,lease);
      f.fake.poison_reader_scratch_on_last_drop = backing;
      f.fake.poison_reader_scratch_size = page;
      if (failure == 0) f.fake.begin_code = SB_BLOB_CALLBACK_SECURITY_DENIED_V3;
      if (failure == 1) f.fake.throw_begin = true;
      if (failure == 2) f.fake.end_code = SB_BLOB_CALLBACK_WRONG_OWNER_V3;
      if (failure == 3) f.fake.throw_end = true;
      if (failure == 4) f.fake.probe_code = SB_BLOB_CALLBACK_SECURITY_DENIED_V3;
      if (failure == 5) f.fake.dirty_probe_ticket = true;
      auto visitor = Access::Visitor(GuardedReaderVisitor,&context);
      const auto result = lease.VisitBoundReaderBytes(visitor);
      Check(!result.ok() && f.fake.pins == 0 && CountCall(f.fake,'L') == 1,
            "failing reader helper retained custody or accessed scrubbed backing after final unpin");
      Check(munmap(backing,page) == 0,"poison reader backing cleanup");
      f.fake.poison_reader_scratch_on_last_drop = nullptr;
    }
  }
}

void GuardedReaderSharedArenaAndBytes() {
  using Access = dt::BlobLifetimeRuntimeConformanceAccessV3;
  using Host = dt::BlobLifetimeRuntimeConformanceAccessV3;
  for (std::uint64_t arena_bytes : {7U,8U}) {
    std::array<platform::byte,8> scratch{};
    dt::BlobLifetimeBudgetConfigurationV3Generation1 config;
    config.limits.fill(UINT64_MAX);
    config.scratch = std::span(scratch).first(arena_bytes);
    config.admitted_window_octets = 4;
    Fixture f(64,0,&config);
    f.fake.pins = 0;
    GuardedReaderContext context{&f.fake};
    dt::BlobRetainedLifetimeLeaseV3 lease;
    RetainGuardedReader(f,9,false,context,lease);
    auto visitor = Access::Visitor(GuardedReaderVisitor,&context);
    Check(lease.VisitBoundReaderBytes(visitor).ok(),"first arena operation");
    context.visited = context.expected_reader_offset = 0;
    const auto result = lease.VisitBoundReaderBytes(visitor);
    if (arena_bytes == 7) {
      Check(result.diagnostic_code == "RESOURCE.BUDGET_EXCEEDED" && result.required == 4 &&
                result.available == 3 && result.resource == dt::BlobLifetimeResourceV3::temporary_bytes &&
                context.calls == 3 && Access::Invoked(f.ledger,dt::BlobLifetimeResourceV3::temporary_bytes) == 4,
            "one-short arena reused or partially charged a consumed workspace");
    } else {
      Check(result.ok() && context.calls == 6 && context.visited == 9 &&
                Access::Invoked(f.ledger,dt::BlobLifetimeResourceV3::temporary_bytes) == 8 && lease.Release().ok(),
            "exact arena boundary refused or recharged per chunk");
    }
    Check(f.fake.pins == 0 && Access::ReservedCleanup(f.ledger) == 0,
          "shared arena operation lost final cleanup");
  }
  struct ConcurrentReader {
    std::atomic<unsigned> entered{0};
    std::atomic<unsigned> completed{0};
    std::atomic<bool> go{false};
    std::atomic<bool> finish{false};
    std::array<std::atomic<std::uintptr_t>,2> destinations{};
  };
  const auto reader = [](void* raw, const dt::BlobReaderRequestV3& request) -> dt::BlobReaderResultV3 {
    auto& c = *static_cast<ConcurrentReader*>(raw);
    const auto slot = c.entered.fetch_add(1);
    if (slot >= 2 || request.requested_octets != 4) Fail("concurrent reader bounds");
    reader_destination_to_scrub = {request.destination,4};
    std::fill_n(request.destination,4,0xa5);
    c.destinations[slot].store(reinterpret_cast<std::uintptr_t>(request.destination),std::memory_order_release);
    while (!c.finish.load(std::memory_order_acquire)) std::this_thread::yield();
    dt::BlobReaderResultV3 result;
    result.status = 0;
    result.actual_octets = 4;
    return result;
  };
  const auto work = [](void*, std::span<const platform::byte> bytes) -> dt::BlobProtectedWorkResultV3 {
    if (bytes.size() != 4 || !std::all_of(bytes.begin(),bytes.end(),[](auto b) { return b == 0xa5; }))
      Fail("concurrent workspace alias changed protected content");
    dt::BlobProtectedWorkResultV3 result;
    result.status = 0;
    return result;
  };
  for (bool encoded : {false,true}) {
    for (std::uint64_t byte_budget : {4U,8U}) {
      std::array<platform::byte,8> scratch{};
      dt::BlobLifetimeBudgetConfigurationV3Generation1 config;
      config.limits.fill(UINT64_MAX);
      config.limits[(encoded ? 3 : 2)-1] = byte_budget;
      config.scratch = scratch;
      config.admitted_window_octets = 4;
      Fixture f(64,0,&config);
      f.fake.pins = 0;
      f.token.mode_bits = SB_BLOB_MODE_IMMUTABLE_READ_V3 | SB_BLOB_MODE_CONCURRENT_IMMUTABLE_READ_V3 |
          (encoded ? SB_BLOB_MODE_OPAQUE_ENCODED_READER_V3 : SB_BLOB_MODE_LOGICAL_READER_V3);
      f.request.operation = encoded ? 9 : 2;
      f.request.required_mode_bits = SB_BLOB_MODE_IMMUTABLE_READ_V3;
      f.carrier_binding.carrier_kind = encoded ? SB_BLOB_MODE_OPAQUE_ENCODED_READER_V3 : SB_BLOB_MODE_LOGICAL_READER_V3;
      if (encoded) f.token.owner_class = f.request.required_owner_class = SB_BLOB_OWNER_ENCODED_ARTIFACT_BORROW_V3;
      ConcurrentReader context;
      auto budget = Host::BudgetControl(f.ledger,f.pins);
      dt::BlobLifetimeAuthorityAdmissionV3Generation1 admission;
      dt::BlobRetainedLifetimeLeaseV3 first,second;
      if (encoded) {
        auto cap = Host::EncodedCapability(f.carrier_binding,4,&context,reader);
        dt::BlobBoundEncodedReaderCarrierV3Generation1 carrier;
        Check(dt::BlobLifetimeRuntimeFactoryV3Generation1::AdmitEncodedReader(9,f.profile,&f.authority,
            &f.services,f.pins,budget,f.services.monotonic_clock,f.services.monotonic_clock,
            f.token,f.request,cap,admission,carrier).ok(),"concurrent encoded admission");
        Check(dt::RetainBaseBlobLifetimeLeaseV3Generation1(f.profile,std::move(admission),carrier,
            f.token,f.request,first).ok(),"concurrent encoded retain");
      } else {
        auto cap = Host::LogicalCapability(f.carrier_binding,dt::BlobValueStateV3::value,4,&context,reader);
        dt::BlobBoundLogicalReaderCarrierV3Generation1 carrier;
        Check(dt::BlobLifetimeRuntimeFactoryV3Generation1::AdmitLogicalReader(2,f.profile,&f.authority,
            &f.services,f.pins,budget,f.services.monotonic_clock,f.services.monotonic_clock,
            f.token,f.request,cap,admission,carrier).ok(),"concurrent logical admission");
        Check(dt::RetainBaseBlobLifetimeLeaseV3Generation1(f.profile,std::move(admission),carrier,
            f.token,f.request,first).ok(),"concurrent logical retain");
      }
      Check(first.CloneInto(second).ok(),"concurrent reader clone");
      auto visitor = Access::Visitor(work,nullptr);
      std::array<dt::BlobLifetimeRuntimeResultV3,2> results;
      const auto run = [&](unsigned index, dt::BlobRetainedLifetimeLeaseV3& lease) {
        while (!context.go.load(std::memory_order_acquire)) std::this_thread::yield();
        results[index] = lease.VisitBoundReaderBytes(visitor);
        context.completed.fetch_add(1,std::memory_order_release);
      };
      std::thread a(run,0,std::ref(first)),b(run,1,std::ref(second));
      context.go.store(true,std::memory_order_release);
      const auto expected_calls = byte_budget == 4 ? 1U : 2U;
      while (context.entered.load(std::memory_order_acquire) != expected_calls ||
          (byte_budget == 4 && context.completed.load(std::memory_order_acquire) != 1U)) std::this_thread::yield();
      for (unsigned i = 0; i < expected_calls; ++i)
        while (context.destinations[i].load(std::memory_order_acquire) == 0) std::this_thread::yield();
      if (expected_calls == 2) Check(context.destinations[0] != context.destinations[1],
          "concurrent cloned leases share mutable scratch");
      const auto held = Access::Snapshot(f.ledger);
      Check(held.counters[encoded ? 3 : 2].reserved_work == byte_budget &&
                held.counters[encoded ? 3 : 2].invoked == 0,
            "shared byte budget was not atomically reserved before reader calls");
      context.finish.store(true,std::memory_order_release);
      a.join(); b.join();
      unsigned successes = 0;
      for (unsigned i = 0; i < 2; ++i) {
        if (results[i].ok()) {
          ++successes;
          Check((i == 0 ? first : second).Release().ok(),"concurrent success cleanup");
        } else Check(results[i].diagnostic_code == "RESOURCE.BUDGET_EXCEEDED" &&
                      results[i].required == 4 && results[i].available == 0,
                      "concurrent byte reservation overcommitted or fabricated refusal");
      }
      const auto settled = Access::Snapshot(f.ledger);
      Check(successes == expected_calls && f.fake.pins == 0 &&
                settled.counters[encoded ? 3 : 2].invoked == byte_budget &&
                settled.counters[encoded ? 3 : 2].reserved_work == 0 &&
                std::all_of(scratch.begin(),scratch.end(),[](auto v) { return v == 0; }),
            "concurrent scratch/counter cleanup lost actual work");
    }
  }
}

void GuardedReaderEmptyAndPrivateGates() {
  using Access = dt::BlobLifetimeRuntimeConformanceAccessV3;
  for (unsigned kind = 0; kind < 3; ++kind) {
    Fixture f(3);
    f.fake.pins = 0;
    f.request.operation = kind == 2 ? 9 : 2;
    GuardedReaderContext context{&f.fake};
    dt::BlobRetainedLifetimeLeaseV3 lease;
    RetainGuardedReader(f,0,kind == 2,context,lease,
        kind == 1 ? dt::BlobValueStateV3::sql_null : dt::BlobValueStateV3::value);
    auto visitor = Access::Visitor(GuardedReaderVisitor,&context);
    Check(lease.VisitBoundReaderBytes(visitor).ok() && lease.Release().ok() &&
              context.calls == 0 && context.visitor_calls == 0 && f.fake.pins == 0 &&
              CountCall(f.fake,'R') == 1 && CountCall(f.fake,'P') == 1 && CountCall(f.fake,'L') == 1 &&
              CountCall(f.fake,'B') == 0 && CountCall(f.fake,'E') == 0,
          "empty reader bypassed authentication or allocated/read content");
  }
  struct Context {
    Fixture* fixture;
    dt::BlobRetainedLifetimeLeaseV3* lease;
    unsigned violation;
    unsigned visitors = 0;
  };
  for (unsigned violation = 0; violation < 3; ++violation) {
    std::array<platform::byte,4> scratch{};
    dt::BlobLifetimeBudgetConfigurationV3Generation1 config;
    config.limits.fill(UINT64_MAX); config.scratch = scratch; config.admitted_window_octets = 4;
    Fixture f(16,0,&config);
    f.fake.pins = 0;
    f.token.mode_bits = SB_BLOB_MODE_IMMUTABLE_READ_V3 | SB_BLOB_MODE_LOGICAL_READER_V3;
    f.request.required_mode_bits = SB_BLOB_MODE_IMMUTABLE_READ_V3;
    f.carrier_binding.carrier_kind = SB_BLOB_MODE_LOGICAL_READER_V3;
    dt::BlobRetainedLifetimeLeaseV3 lease;
    Context context{&f,&lease,violation};
    const auto reader = [](void* raw,const dt::BlobReaderRequestV3& request) -> dt::BlobReaderResultV3 {
      auto& c = *static_cast<Context*>(raw);
      reader_destination_to_scrub = {request.destination,static_cast<std::size_t>(request.destination_capacity)};
      std::fill_n(request.destination,request.destination_capacity,0x85);
      if (c.violation == 0) {
        Access::SetCounter(c.fixture->ledger,dt::BlobLifetimeResourceV3::expanded_bytes,0,1,0);
        std::uint64_t available;
        (void)Access::Charge(c.fixture->ledger,dt::BlobLifetimeResourceV3::expanded_bytes,1,&available);
      } else if (c.violation == 1) {
        (void)Access::ControlPublish(*c.lease,dt::BlobLifetimeInvariantEventV3::receiver_services_failed,
            c.fixture->request.operation,dt::BlobLifetimePhaseV3::protected_access,255);
      }
      dt::BlobReaderResultV3 result;
      result.status = 0; result.actual_octets = request.requested_octets;
      return result;
    };
    using Host = dt::BlobLifetimeRuntimeConformanceAccessV3;
    auto cap = Host::LogicalCapability(f.carrier_binding,dt::BlobValueStateV3::value,4,&context,reader);
    auto budget = Host::BudgetControl(f.ledger,f.pins);
    dt::BlobLifetimeAuthorityAdmissionV3Generation1 admission;
    dt::BlobBoundLogicalReaderCarrierV3Generation1 carrier;
    Check(dt::BlobLifetimeRuntimeFactoryV3Generation1::AdmitLogicalReader(2,f.profile,&f.authority,
        &f.services,f.pins,budget,f.services.monotonic_clock,f.services.monotonic_clock,
        f.token,f.request,cap,admission,carrier).ok(),"private gate reader admission");
    Check(dt::RetainBaseBlobLifetimeLeaseV3Generation1(f.profile,std::move(admission),carrier,
        f.token,f.request,lease).ok(),"private gate reader retain");
    auto visitor = Access::Visitor([](void* raw,std::span<const platform::byte>) -> dt::BlobProtectedWorkResultV3 {
      auto& c = *static_cast<Context*>(raw);
      ++c.visitors;
      (void)Access::ControlPublish(*c.lease,dt::BlobLifetimeInvariantEventV3::receiver_services_failed,
          c.fixture->request.operation,dt::BlobLifetimePhaseV3::protected_access,255);
      dt::BlobProtectedWorkResultV3 result; result.status = 0; return result;
    },&context);
    const auto result = lease.VisitBoundReaderBytes(visitor);
    Check(result.disposition == dt::BlobLifetimeOuterDispositionV3::terminated_out_of_band &&
              context.visitors == (violation == 2 ? 1U : 0U) &&
              f.fake.clock_calls == (violation == 2 ? 8U : 6U) &&
              f.fake.cancellation_calls == f.fake.clock_calls && f.fake.pins == 0 &&
              Access::Invoked(f.ledger,dt::BlobLifetimeResourceV3::logical_bytes_read) == 4 &&
              Access::Counter(f.ledger,dt::BlobLifetimeResourceV3::logical_bytes_read).reserved_work == 0 &&
              CountCall(f.fake,'E') == 1 && CountCall(f.fake,'L') == 1,
          "private reader/visitor failure performed ordinary work or erased actual bytes");
  }
}

void ProtectedReaderSamplerAdmission() {
  using Access = dt::BlobLifetimeRuntimeConformanceAccessV3;
  for (bool reader : {false,true}) {
    for (bool private_failure : {false,true}) {
      for (bool after_begin : {false,true}) {
        std::array<platform::byte,4> scratch{};
        dt::BlobLifetimeBudgetConfigurationV3Generation1 config;
        config.limits.fill(UINT64_MAX); config.scratch = scratch; config.admitted_window_octets = 4;
        Fixture f(16,0,&config);
        f.fake.pins = 0;
        GuardedReaderContext context{&f.fake};
        dt::BlobRetainedLifetimeLeaseV3 lease;
        const std::array<platform::byte,4> materialized{{1,2,3,4}};
        if (reader) RetainGuardedReader(f,4,false,context,lease);
        else {
          auto carrier = Access::Carrier(f.carrier_binding,dt::BlobValueStateV3::value,
                                         materialized.data(),materialized.size());
          Check(dt::RetainBaseBlobLifetimeLeaseV3Generation1(f.profile,f.Admission(),carrier,
              f.token,f.request,lease).ok(),"materialized sampler admission retain");
        }
        f.fake.defer_on_clock_call = (reader ? 5U : 4U)+(after_begin ? 0U : 1U);
        f.fake.defer_clock_lease = &lease;
        f.fake.defer_clock_private = private_failure;
        f.fake.defer_clock_phase = after_begin ? dt::BlobLifetimePhaseV3::begin_access
                                               : dt::BlobLifetimePhaseV3::protected_access;
        unsigned visits = 0;
        auto visitor = Access::Visitor([](void* raw,std::span<const platform::byte>) -> dt::BlobProtectedWorkResultV3 {
          ++*static_cast<unsigned*>(raw);
          dt::BlobProtectedWorkResultV3 r; r.status = 0; return r;
        },&visits);
        const auto result = reader ? lease.VisitBoundReaderBytes(visitor) : lease.VisitBoundMaterializedBytes(visitor);
        Check(!result.ok() && (result.disposition == dt::BlobLifetimeOuterDispositionV3::terminated_out_of_band) == private_failure &&
                  visits == 0 && context.calls == 0 && f.fake.pins == 0 &&
                  f.fake.clock_calls == f.fake.defer_on_clock_call &&
                  f.fake.cancellation_calls == f.fake.clock_calls-(private_failure ? 1U : 0U) &&
                  CountCall(f.fake,'B') == 1 && CountCall(f.fake,'E') == 1 && CountCall(f.fake,'L') == 1 &&
                  CountCall(f.fake,'P') == 0 && Access::Invoked(f.ledger,dt::BlobLifetimeResourceV3::reader_calls) == 0,
              "sampler-time violation admitted later ordinary sampling, reader or visitor");
      }
    }
  }
}

void EncodedReaderAdmissionAndRetention() {
  using Host = dt::BlobLifetimeRuntimeConformanceAccessV3;
  using Access = dt::BlobLifetimeRuntimeConformanceAccessV3;
  using Capability = dt::BlobReceiverEncodedReaderCapabilityV3Generation1;
  using Carrier = dt::BlobBoundEncodedReaderCarrierV3Generation1;
  static_assert(!std::is_default_constructible_v<Capability>);
  static_assert(!std::is_copy_constructible_v<Capability>);
  static_assert(!std::is_move_constructible_v<Capability>);
  static_assert(!std::is_copy_constructible_v<Carrier>);
  static_assert(!std::is_move_constructible_v<Carrier>);
  const auto prepare = [](Fixture& f, std::uint8_t op) {
    f.fake.pins = 0;
    f.token.owner_class = f.request.required_owner_class = SB_BLOB_OWNER_ENCODED_ARTIFACT_BORROW_V3;
    f.token.mode_bits = SB_BLOB_MODE_IMMUTABLE_READ_V3 | SB_BLOB_MODE_OPAQUE_ENCODED_READER_V3;
    f.request.required_mode_bits = SB_BLOB_MODE_IMMUTABLE_READ_V3;
    f.request.operation = op;
    f.carrier_binding.carrier_kind = SB_BLOB_MODE_OPAQUE_ENCODED_READER_V3;
  };
  const auto admit = [](Fixture& f, const Capability& capability,
                        dt::BlobLifetimeAuthorityAdmissionV3Generation1& admission,
                        Carrier& carrier) {
    auto budget = Host::BudgetControl(f.ledger, f.pins);
    return dt::BlobLifetimeRuntimeFactoryV3Generation1::AdmitEncodedReader(
        f.request.operation, f.profile, &f.authority, &f.services, f.pins, budget,
        f.services.monotonic_clock, f.services.monotonic_clock,
        f.token, f.request, capability, admission, carrier);
  };
  for (unsigned owner = 1; owner <= 3; ++owner) {
    for (unsigned op = 1; op <= 13; ++op) {
      for (unsigned choice = 0; choice < 153; ++choice) {
        Fixture f(0);
        prepare(f, op);
        f.token.owner_class = f.request.required_owner_class = owner;
        if (owner == SB_BLOB_OWNER_EXECUTION_BORROW_V3) {
          f.token.session_uuid = f.request.expected_session_uuid = BlobUuid(0xb0);
          f.token.statement_uuid = f.request.expected_statement_uuid = BlobUuid(0xc0);
        }
        const auto modes = choice < 128 ? choice : (0x21U | (1U << (choice-128+7)));
        f.token.mode_bits = modes;
        auto cap = Host::EncodedCapability(f.carrier_binding, UINT64_MAX,
                                           &f.fake, NeverReadDuringAdmission);
        const bool expected = owner == 3 && op >= 9 &&
            (modes & ~0x3fU) == 0 && (modes & 0x3bU) == 0x21U;
        const auto before = allocation_probe::calls.load();
        {
          dt::BlobLifetimeAuthorityAdmissionV3Generation1 admission;
          Carrier carrier;
          allocation_probe::enabled.store(true);
          const auto result = admit(f, cap, admission, carrier);
          allocation_probe::enabled.store(false);
          Check(result.ok() == expected && allocation_probe::calls.load() == before &&
                    f.fake.pins == (expected ? 2U : 0U) &&
                    carrier.encoded_extent() == (expected ? UINT64_MAX : 0),
                "opaque reader owner/operation/mode/extent admission matrix");
          Check(f.fake.call_count == 0 && f.fake.clock_calls == 0 &&
                    f.fake.cancellation_calls == 0,
                "opaque admission performed runtime/content work");
        }
        Check(f.fake.pins == 0 && f.fake.pin_drop_calls == 2,
              "opaque admission pin leak");
      }
    }
  }
  for (std::uint8_t op = 9; op <= 13; ++op) {
    for (const std::uint64_t extent : {UINT64_C(0), UINT64_C(1),
          dt::kBlobMaximumLogicalBytesV3 + 1, dt::kBlobMaximumLogicalBytesV3 + 320,
          UINT64_MAX}) {
      Fixture f(16);
      prepare(f, op);
      auto cap = Host::EncodedCapability(f.carrier_binding, extent, &f.fake,
                                         NeverReadDuringAdmission);
      dt::BlobLifetimeAuthorityAdmissionV3Generation1 admission;
      Carrier carrier;
      Check(admit(f, cap, admission, carrier).ok(), "opaque extent admission");
      const auto before = allocation_probe::calls.load();
      allocation_probe::enabled.store(true);
      dt::BlobRetainedLifetimeLeaseV3 lease, clone, destination;
      const auto retained = dt::RetainBaseBlobLifetimeLeaseV3Generation1(
          f.profile, std::move(admission), carrier, f.token, f.request, lease);
      const bool original_routing = Access::EncodedRoutingMatches(
          lease, &f.fake, NeverReadDuringAdmission, extent);
      const auto cloned = lease.CloneInto(clone);
      auto moved = dt::BlobRetainedLifetimeLeaseV3::MoveConstructFrom(std::move(clone));
      const bool moved_routing = moved.result.ok() && Access::EncodedRoutingMatches(
          *moved.lease(), &f.fake, NeverReadDuringAdmission, extent);
      const auto replaced = moved.result.ok() ? destination.MoveReplaceFrom(std::move(*moved.lease()))
                                       : moved.result;
      const bool destination_routing = Access::EncodedRoutingMatches(
          destination, &f.fake, NeverReadDuringAdmission, extent);
      const auto probed = lease.Probe();
      const auto first_release = lease.Release();
      const auto second_release = destination.Release();
      allocation_probe::enabled.store(false);
      Check(retained.ok() && original_routing && cloned.ok() && moved_routing &&
                replaced.ok() && destination_routing && probed.ok() && first_release.ok() &&
                second_release.ok() && Access::ReaderRoutingCleared(lease) &&
                Access::ReaderRoutingCleared(clone) && Access::ReaderRoutingCleared(destination) &&
                f.fake.pins == 0 && CountCall(f.fake,'R') == 2 &&
                CountCall(f.fake,'P') == 1 && CountCall(f.fake,'L') == 2 &&
                CountCall(f.fake,'B') == 0 && Access::Invoked(f.ledger) == 5 &&
                Access::ReservedCleanup(f.ledger) == 0 && allocation_probe::calls.load() == before,
            "opaque retention/clone/move/probe/release lost full extent or custody");
    }
  }
  // Revalidation must reject foreign token carrier modes even when the
  // caller changes its required mask to match the forged token.
  for (const std::uint32_t substituted : {SB_BLOB_MODE_MATERIALIZED_BYTES_V3,
                                          SB_BLOB_MODE_LOGICAL_READER_V3}) {
    Fixture f(8);
    prepare(f, 9);
    auto cap = Host::EncodedCapability(f.carrier_binding, UINT64_MAX, &f.fake,
                                       NeverReadDuringAdmission);
    {
      dt::BlobLifetimeAuthorityAdmissionV3Generation1 admission;
      Carrier carrier;
      Check(admit(f, cap, admission, carrier).ok(), "opaque substitution setup");
      f.token.mode_bits = f.request.required_mode_bits = SB_BLOB_MODE_IMMUTABLE_READ_V3 | substituted;
      dt::BlobRetainedLifetimeLeaseV3 lease;
      const auto refused = dt::RetainBaseBlobLifetimeLeaseV3Generation1(
          f.profile, std::move(admission), carrier, f.token, f.request, lease);
      Check(!refused.ok() && !lease.retained() && f.fake.call_count == 0 && f.fake.pins == 2,
            "opaque retained validation admitted substituted token mode");
    }
    Check(f.fake.pins == 0, "opaque failed revalidation lost admission pins");
  }
  for (unsigned mutation = 0; mutation < 8; ++mutation) {
    Fixture f(8);
    prepare(f, 10);
    void* context = &f.fake;
    dt::BlobReadAtV3 read_at = NeverReadDuringAdmission;
    switch (mutation) {
      case 0: context = nullptr; break;
      case 1: read_at = nullptr; break;
      case 2: f.carrier_binding.carrier_generation = 0; break;
      case 3: ++f.carrier_binding.immutable_binding_generation; break;
      case 4: ++f.request.expected_security_generation; break;
      case 5: ++f.profile.receipt.registry_generation; break;
      case 6: f.carrier_binding.carrier_kind = SB_BLOB_MODE_LOGICAL_READER_V3; break;
      case 7: f.authority.probe = nullptr; break;
    }
    auto cap = Host::EncodedCapability(f.carrier_binding, 0, context, read_at);
    dt::BlobLifetimeAuthorityAdmissionV3Generation1 admission;
    Carrier carrier;
    const auto result = admit(f, cap, admission, carrier);
    Check(!result.ok() && carrier.encoded_extent() == 0 && f.fake.pins == 0 && f.fake.call_count == 0,
          "opaque malformed capability admitted or mutated output");
  }
  for (std::uint8_t op = 9; op <= 13; ++op) {
    for (unsigned phase = 0; phase < 3; ++phase) {
      Fixture f(8);
      prepare(f, op);
      auto cap = Host::EncodedCapability(f.carrier_binding, UINT64_MAX, &f.fake,
                                         NeverReadDuringAdmission);
      dt::BlobLifetimeAuthorityAdmissionV3Generation1 admission;
      Carrier carrier;
      Check(admit(f, cap, admission, carrier).ok(), "opaque callback mode setup");
      dt::BlobRetainedLifetimeLeaseV3 lease;
      if (phase == 0) f.fake.retain_code = SB_BLOB_CALLBACK_WRONG_MODE_V3;
      auto result = dt::RetainBaseBlobLifetimeLeaseV3Generation1(
          f.profile, std::move(admission), carrier, f.token, f.request, lease);
      if (phase == 1) {
        Check(result.ok(), "opaque probe mode retain");
        f.fake.probe_code = SB_BLOB_CALLBACK_WRONG_MODE_V3;
        result = lease.Probe();
      } else if (phase == 2) {
        Check(result.ok(), "opaque release mode retain");
        f.fake.release_code = SB_BLOB_CALLBACK_WRONG_MODE_V3;
        result = lease.Release();
      }
      Check(result.diagnostic_code == "BLOB.HANDLE_MODE_REFUSED" &&
                result.fact.parameters.open_mode_enum == 6 &&
                result.fact.parameters.operation_enum == op && f.fake.pins == 0 &&
                Access::ReaderRoutingCleared(lease),
            "opaque callback diagnostic substituted scalar mode or leaked custody");
    }
  }
  {
    Fixture f(8);
    prepare(f, 11);
    auto cap = Host::EncodedCapability(f.carrier_binding, UINT64_MAX, &f.fake,
                                       NeverReadDuringAdmission);
    dt::BlobLifetimeAuthorityAdmissionV3Generation1 admission;
    Carrier carrier;
    Check(admit(f, cap, admission, carrier).ok(), "opaque populated output setup");
    dt::BlobLifetimeAuthorityAdmissionV3Generation1 other;
    const auto refused = admit(f, cap, other, carrier);
    Check(refused.diagnostic_code == "BLOB.STATE_INVALID" &&
              refused.fact.parameters.supplied_state_u8 == 2 &&
              carrier.encoded_extent() == UINT64_MAX && f.fake.pins == 2,
          "opaque populated output lost encoded-artifact state/custody");
    dt::BlobRetainedLifetimeLeaseV3 lease;
    f.fake.retain_code = SB_BLOB_CALLBACK_WRONG_MODE_V3;
    const auto failed = dt::RetainBaseBlobLifetimeLeaseV3Generation1(
        f.profile, std::move(admission), carrier, f.token, f.request, lease);
    Check(failed.diagnostic_code == "BLOB.HANDLE_MODE_REFUSED" &&
              failed.fact.parameters.open_mode_enum == 6 && f.fake.pins == 0 &&
              Access::ReaderRoutingCleared(lease),
          "opaque failed retain mode diagnostic or clear-before-unpin");
    f.fake.retain_code = 0;
    dt::BlobLifetimeAuthorityAdmissionV3Generation1 retry;
    Carrier fresh;
    Check(admit(f, cap, retry, fresh).ok() &&
              dt::RetainBaseBlobLifetimeLeaseV3Generation1(f.profile, std::move(retry),
                  fresh, f.token, f.request, lease).ok(), "opaque failed retain reuse");
    VisitContext visit;
    auto visitor = Access::Visitor(CopyVisitor, &visit);
    const auto wrong_consumer = lease.VisitBoundMaterializedBytes(visitor);
    Check(wrong_consumer.disposition == dt::BlobLifetimeOuterDispositionV3::terminated_out_of_band &&
              visit.copied_size == 0 && CountCall(f.fake,'B') == 0 &&
              CountCall(f.fake,'L') == 1 && f.fake.pins == 0 && Access::ReaderRoutingCleared(lease),
          "opaque carrier exposed scalar/materialized content");
  }
  const auto page_size = static_cast<std::size_t>(sysconf(_SC_PAGESIZE));
  void* page = mmap(nullptr, page_size, PROT_READ | PROT_WRITE,
                    MAP_PRIVATE | MAP_ANONYMOUS, -1, 0);
  Check(page != MAP_FAILED, "opaque capability guard allocation");
  Fixture guarded(0);
  prepare(guarded, 9);
  auto* guarded_cap = Host::EncodedCapabilityAt(page, guarded.carrier_binding,
      &guarded.fake, NeverReadDuringAdmission);
  Check(mprotect(page, page_size, PROT_NONE) == 0, "opaque capability protect");
  for (unsigned pin = 1; pin <= 2; ++pin) {
    guarded.fake.pin_acquire_calls = 0;
    guarded.fake.pin_failure_call = pin;
    guarded.fake.pin_acquire_status = dt::BlobReceiverPinAcquireStatusV3::draining;
    guarded.fake.pin_zero_cookie = true;
    dt::BlobLifetimeAuthorityAdmissionV3Generation1 admission;
    Carrier carrier;
    const auto result = admit(guarded, *guarded_cap, admission, carrier);
    Check(result.diagnostic_code == "BLOB.LIFETIME_AUTHORITY_UNAVAILABLE" &&
              guarded.fake.pins == 0 && guarded.fake.call_count == 0 &&
              guarded.fake.pin_acquire_calls == pin,
          "opaque factory read unstable capability before both pins");
  }
  Check(mprotect(page, page_size, PROT_READ | PROT_WRITE) == 0,
        "opaque capability restore");
  guarded_cap->~BlobReceiverEncodedReaderCapabilityV3Generation1();
  auto* guarded_carrier = new (page) Carrier;
  Check(mprotect(page, page_size, PROT_NONE) == 0, "opaque retained carrier protect");
  dt::BlobLifetimeAuthorityAdmissionV3Generation1 absent;
  dt::BlobRetainedLifetimeLeaseV3 lease;
  const auto refused = dt::RetainBaseBlobLifetimeLeaseV3Generation1(
      guarded.profile, std::move(absent), *guarded_carrier,
      guarded.token, guarded.request, lease);
  Check(!refused.ok() && guarded.fake.call_count == 0 && guarded.fake.pins == 0,
        "opaque retained validation read carrier without admission");
  Check(mprotect(page, page_size, PROT_READ | PROT_WRITE) == 0,
        "opaque retained carrier restore");
  guarded_carrier->~BlobBoundEncodedReaderCarrierV3Generation1();
  Check(munmap(page, page_size) == 0,
        "opaque capability guard cleanup");
}

void ContextualNullDescriptorBinding() {
  namespace en = scratchbird::engine;
  using Failure = dt::BlobContextualNullFailureV3;
  const auto admitted = dt::BuildCurrentBlobValidatedProfileHandleV3(dt::kBlobV11ReceiptUuid);
  Check(admitted.ok(), "contextual BLOB target profile admission");
  const auto& profile = admitted.profile;
  const auto target_descriptor = [&]() {
    en::ExecutionTypeDescriptor d;
    std::copy(profile.identity.descriptor_uuid.bytes.begin(),
              profile.identity.descriptor_uuid.bytes.end(), d.descriptor_uuid.bytes);
    d.descriptor_epoch = profile.identity.descriptor_generation;
    d.canonical_type_id = static_cast<std::uint32_t>(dt::CanonicalTypeId::blob);
    d.family = en::ExecutionTypeFamily::blob;
    d.width_class = en::ExecutionTypeWidthClass::descriptor_defined;
    d.stable_name = std::string(256, 'b');
    return d;
  };
  const auto equal = [](const en::ExecutionTypeDescriptor& a,
                        const en::ExecutionTypeDescriptor& b) {
    const auto uuid_equal = [](const en::Uuid& x, const en::Uuid& y) {
      return std::equal(std::begin(x.bytes), std::end(x.bytes), std::begin(y.bytes));
    };
    bool domains_equal = a.domain_stack.size() == b.domain_stack.size();
    for (std::size_t i = 0; domains_equal && i < a.domain_stack.size(); ++i)
      domains_equal = uuid_equal(a.domain_stack[i], b.domain_stack[i]);
    return uuid_equal(a.descriptor_uuid,b.descriptor_uuid) && a.descriptor_epoch==b.descriptor_epoch &&
        a.canonical_type_id==b.canonical_type_id && a.family==b.family && a.width_class==b.width_class &&
        a.stable_name==b.stable_name && a.bit_width==b.bit_width && a.precision==b.precision &&
        a.scale==b.scale && a.length==b.length && a.vector_dimensions==b.vector_dimensions &&
        a.container_rank==b.container_rank && a.modifier_flags==b.modifier_flags &&
        uuid_equal(a.domain_uuid,b.domain_uuid) && domains_equal &&
        uuid_equal(a.charset_uuid,b.charset_uuid) && uuid_equal(a.collation_uuid,b.collation_uuid) &&
        uuid_equal(a.timezone_uuid,b.timezone_uuid) && uuid_equal(a.element_descriptor_uuid,b.element_descriptor_uuid) &&
        uuid_equal(a.security_policy_uuid,b.security_policy_uuid) && a.nullable_allowed==b.nullable_allowed &&
        a.descriptor_authoritative==b.descriptor_authoritative && a.parser_independent==b.parser_independent;
  };
  const dt::DatatypeOperationValue clean{dt::CanonicalTypeId::null_type, {}, true};
  const auto invoke = [&](const dt::BlobValidatedProfileHandleV3& p,
                          const dt::DatatypeOperationValue& source,
                          dt::DatatypeCastContext context,
                          en::ExecutionTypeDescriptor& target) {
    const auto before = allocation_probe::calls.load();
    allocation_probe::enabled.store(true);
    auto result = dt::BindBlobContextualNullV3(p, source, context, std::move(target));
    allocation_probe::enabled.store(false);
    Check(allocation_probe::calls.load() == before,
          "contextual BLOB binding allocated on success or failure");
    return result;
  };
  for (const auto context : {dt::DatatypeCastContext::implicit,
                            dt::DatatypeCastContext::assignment,
                            dt::DatatypeCastContext::explicit_cast}) {
    for (unsigned alias = 0; alias < 3; ++alias) {
      auto target = target_descriptor();
      if (alias == 0) target.stable_name.clear();
      if (alias == 1) target.stable_name = "not-a-datatype-name";
      const auto exact_target = target;
      const auto* owned_name = target.stable_name.data();
      const auto result = invoke(profile, clean, context, target);
      Check(result.ok() && result.failure == Failure::none && result.binding &&
                result.binding->state == dt::BlobValueStateV3::sql_null &&
                equal(result.binding->descriptor, exact_target) &&
                (alias != 2 || result.binding->descriptor.stable_name.data() == owned_name) &&
                dt::ValidateBlobProfileHandleV3(result.binding->profile).ok(),
            "contextual BLOB binding lost exact owned target or profile");
    }
  }
  const auto refused = [&](const dt::BlobValidatedProfileHandleV3& p,
                           const dt::DatatypeOperationValue& source,
                           dt::DatatypeCastContext context,
                           en::ExecutionTypeDescriptor& target, Failure expected) {
    const auto prior = target;
    const auto* name = target.stable_name.data();
    const auto result = invoke(p, source, context, target);
    Check(!result.ok() && !result.binding && result.failure == expected &&
              equal(target, prior) && target.stable_name.data() == name,
          "contextual BLOB refusal published output, moved input or selected wrong primary");
  };
  for (unsigned mutation = 0; mutation < 24; ++mutation) {
    auto target = target_descriptor();
    switch (mutation) {
      case 0: target.descriptor_uuid.bytes[15] ^= 1; break;
      case 1: ++target.descriptor_epoch; break;
      case 2: target.canonical_type_id = 0; break;
      case 3: target.family = en::ExecutionTypeFamily::binary; break;
      case 4: target.width_class = en::ExecutionTypeWidthClass::variable; break;
      case 5: target.bit_width = 1; break;
      case 6: target.precision = 1; break;
      case 7: target.scale = 1; break;
      case 8: target.length = 1; break;
      case 9: target.vector_dimensions = 1; break;
      case 10: target.container_rank = 1; break;
      case 11: target.modifier_flags = UINT64_MAX; break;
      case 12: target.domain_uuid = target.descriptor_uuid; break;
      case 13: target.domain_stack.push_back(target.descriptor_uuid); break;
      case 14: target.charset_uuid = target.descriptor_uuid; break;
      case 15: target.collation_uuid = target.descriptor_uuid; break;
      case 16: target.timezone_uuid = target.descriptor_uuid; break;
      case 17: target.element_descriptor_uuid = target.descriptor_uuid; break;
      case 18: target.security_policy_uuid = target.descriptor_uuid; break;
      case 19: target.descriptor_authoritative = false; break;
      case 20: target.parser_independent = false; break;
      case 21: target.descriptor_epoch = 0; break;
      case 22: target.family = en::ExecutionTypeFamily::unknown; break;
      case 23: target.width_class = en::ExecutionTypeWidthClass::unknown; break;
    }
    refused(profile, clean, dt::DatatypeCastContext::implicit, target,
            Failure::target_descriptor_invalid);
  }
  for (unsigned mutation = 0; mutation < 26; ++mutation) {
    auto source = clean;
    auto& d = source.descriptor;
    auto target = target_descriptor();
    switch (mutation) {
      case 0: d.descriptor_uuid = target.descriptor_uuid; break;
      case 1: d.descriptor_epoch = 1; break;
      case 2: d.canonical_type_id = 500; break;
      case 3: d.family = en::ExecutionTypeFamily::null_type; break;
      case 4: d.width_class = en::ExecutionTypeWidthClass::fixed; break;
      case 5: d.stable_name = "NULL"; break;
      case 6: d.bit_width = 1; break;
      case 7: d.precision = 1; break;
      case 8: d.scale = 1; break;
      case 9: d.length = 1; break;
      case 10: d.vector_dimensions = 1; break;
      case 11: d.container_rank = 1; break;
      case 12: d.modifier_flags = 1; break;
      case 13: d.domain_uuid = target.descriptor_uuid; break;
      case 14: d.domain_stack.push_back(target.descriptor_uuid); break;
      case 15: d.charset_uuid = target.descriptor_uuid; break;
      case 16: d.collation_uuid = target.descriptor_uuid; break;
      case 17: d.timezone_uuid = target.descriptor_uuid; break;
      case 18: d.element_descriptor_uuid = target.descriptor_uuid; break;
      case 19: d.security_policy_uuid = target.descriptor_uuid; break;
      case 20: d.nullable_allowed = false; break;
      case 21: d.descriptor_authoritative = false; break;
      case 22: d.parser_independent = false; break;
      case 23: source.is_null = false; break;
      case 24: source.encoded_value = "not NULL"; break;
      case 25: source.type_id = dt::CanonicalTypeId::blob; break;
    }
    refused(profile, source, dt::DatatypeCastContext::implicit, target,
        mutation < 23 ? Failure::source_descriptor_invalid :
        mutation < 25 ? Failure::source_state_invalid : Failure::contextual_source_required);
  }
  auto target = target_descriptor();
  for (std::uint32_t raw = 3; raw <= UINT16_MAX; ++raw)
    refused(profile, clean, static_cast<dt::DatatypeCastContext>(raw), target,
            Failure::invalid_cast_context);
  target.nullable_allowed = false;
  refused(profile, clean, dt::DatatypeCastContext::assignment, target,
          Failure::null_not_admitted);
  for (std::size_t policy = 0; policy < dt::kBlobPolicyBindingCountV3; ++policy) {
    auto mismatched = profile;
    ++mismatched.policy_bindings[policy].generation;
    refused(mismatched, clean, dt::DatatypeCastContext::implicit, target,
            Failure::target_descriptor_invalid);
  }
  auto bad_profile = profile;
  ++bad_profile.receipt.registry_generation;
  auto dirty_source = clean;
  dirty_source.is_null = false;
  refused(profile, dirty_source, static_cast<dt::DatatypeCastContext>(UINT16_MAX),
          target, Failure::source_state_invalid);
  dirty_source.descriptor.stable_name = "not-empty-sentinel";
  refused(profile, dirty_source, static_cast<dt::DatatypeCastContext>(UINT16_MAX),
          target, Failure::source_descriptor_invalid);
  refused(profile, clean, static_cast<dt::DatatypeCastContext>(UINT16_MAX),
          target, Failure::null_not_admitted);
  refused(bad_profile, dirty_source, static_cast<dt::DatatypeCastContext>(UINT16_MAX),
          target, Failure::target_descriptor_invalid);
  // An aliased source descriptor cannot be both the exact BLOB target and an
  // untyped contextual sentinel. Refusal must not move its shared metadata.
  auto aliased = clean;
  aliased.descriptor = target_descriptor();
  refused(profile, aliased, dt::DatatypeCastContext::implicit,
          aliased.descriptor, Failure::source_descriptor_invalid);
  Check(!dt::BlobContextualNullBindingResultV3{}.ok(),
        "default contextual BLOB result claims binding");
}

void IndependentResourceCleanupAccounting() {
  for (unsigned resource = 2; resource <= 11; ++resource) {
    for (unsigned shape = 0; shape < 3; ++shape) {
      dt::BlobLifetimeBudgetLedgerV3Generation1 ledger;
      dt::BlobLifetimeRuntimeConformanceAccessV3::SetCallbackLimit(ledger, 8);
      std::uint64_t available = 0;
      Check(dt::BlobLifetimeRuntimeConformanceAccessV3::Reserve(ledger, 1, 2,
                                                               &available),
            "independent cleanup setup reservation");
      const auto family = static_cast<dt::BlobLifetimeResourceV3>(resource);
      dt::BlobLifetimeRuntimeConformanceAccessV3::SetCounter(
          ledger, family, shape == 0 ? 0 : shape == 1 ? 1 : UINT64_MAX,
          shape == 0 ? 1 : shape == 1 ? 1 : UINT64_MAX, shape == 0 ? 0 : 1);
      const auto corrupt = dt::BlobLifetimeRuntimeConformanceAccessV3::Counter(
          ledger, family);
      Check(!dt::BlobLifetimeRuntimeConformanceAccessV3::Charge(
                ledger, family, 1, &available) && available == 0,
            "noncallback corrupt counter admitted work");
      Check(!dt::BlobLifetimeRuntimeConformanceAccessV3::Reserve(
                ledger, 1, 0, &available) && available == 0,
            "quarantined operation admitted an ordinary callback");
      const auto first = dt::BlobLifetimeRuntimeConformanceAccessV3::Consume(ledger, 1);
      const auto second = dt::BlobLifetimeRuntimeConformanceAccessV3::Consume(ledger, 1);
      Check(first.obligation_consumed && first.cleanup_callback_charge_commit &&
                first.ledger_terminal_quarantined && second.obligation_consumed &&
                second.cleanup_callback_charge_commit && second.ledger_terminal_quarantined,
            "noncallback quarantine suppressed healthy exact cleanup accounting");
      const auto callback = dt::BlobLifetimeRuntimeConformanceAccessV3::Counter(
          ledger, dt::BlobLifetimeResourceV3::lifetime_callback_calls);
      const auto after = dt::BlobLifetimeRuntimeConformanceAccessV3::Counter(ledger, family);
      const auto snapshot = dt::BlobLifetimeRuntimeConformanceAccessV3::Snapshot(ledger);
      Check(callback.invoked == 3 && callback.reserved_cleanup == 0 &&
                after.limit == corrupt.limit && after.invoked == corrupt.invoked &&
                after.reserved_cleanup == corrupt.reserved_cleanup &&
                snapshot.terminal_quarantined &&
                !snapshot.lifetime_callback_terminal_quarantined &&
                !dt::BlobLifetimeRuntimeConformanceAccessV3::Reserve(
                    ledger, 1, 0, &available) && available == 0,
            "cleanup repaired subject counter or resurrected quarantined ordinary work");
    }
  }
  {
    dt::BlobLifetimeBudgetLedgerV3Generation1 ledger;
    dt::BlobLifetimeRuntimeConformanceAccessV3::SetCallbackLimit(ledger, 8);
    std::uint64_t available = 0;
    Check(dt::BlobLifetimeRuntimeConformanceAccessV3::Reserve(ledger, 1, 2, &available),
          "concurrent independent cleanup setup reservation");
    dt::BlobLifetimeRuntimeConformanceAccessV3::SetCounter(
        ledger, dt::BlobLifetimeResourceV3::logical_bytes_read, 0, 1, 0);
    Check(!dt::BlobLifetimeRuntimeConformanceAccessV3::Charge(
        ledger, dt::BlobLifetimeResourceV3::logical_bytes_read, 1, &available),
        "concurrent independent cleanup corruption setup");
    std::atomic<unsigned> ready{0};
    std::atomic<bool> go{false};
    std::array<dt::BlobCleanupReservationConsumptionV3, 2> outcomes{};
    const auto consume = [&](unsigned index) {
      ready.fetch_add(1, std::memory_order_release);
      while (!go.load(std::memory_order_acquire)) std::this_thread::yield();
      outcomes[index] = dt::BlobLifetimeRuntimeConformanceAccessV3::Consume(ledger, 1);
    };
    std::thread first(consume, 0), second(consume, 1);
    while (ready.load(std::memory_order_acquire) != 2) std::this_thread::yield();
    go.store(true, std::memory_order_release);
    first.join();
    second.join();
    for (const auto& outcome : outcomes)
      Check(outcome.obligation_consumed && outcome.cleanup_callback_charge_commit &&
                outcome.ledger_terminal_quarantined,
            "concurrent mandatory cleanup lost a reservation or numeric charge");
    Check(dt::BlobLifetimeRuntimeConformanceAccessV3::Invoked(ledger) == 3 &&
              dt::BlobLifetimeRuntimeConformanceAccessV3::ReservedCleanup(ledger) == 0 &&
              !dt::BlobLifetimeRuntimeConformanceAccessV3::Snapshot(ledger)
                  .lifetime_callback_terminal_quarantined,
          "concurrent independent cleanup double-charged or poisoned healthy counter");
  }
  {
    dt::BlobLifetimeBudgetLedgerV3Generation1 ledger;
    dt::BlobLifetimeRuntimeConformanceAccessV3::SetCallbackLimit(ledger, 8);
    std::uint64_t available = 0;
    Check(dt::BlobLifetimeRuntimeConformanceAccessV3::Reserve(ledger, 1, 2, &available),
          "independent prospective cleanup reservation");
    dt::BlobLifetimeRuntimeConformanceAccessV3::SetCounter(
        ledger, dt::BlobLifetimeResourceV3::logical_bytes_read, 0, 1, 0);
    Check(!dt::BlobLifetimeRuntimeConformanceAccessV3::Charge(
              ledger, dt::BlobLifetimeResourceV3::logical_bytes_read, 1, &available) &&
              dt::BlobLifetimeRuntimeConformanceAccessV3::Cancel(ledger, 1),
          "other resource quarantine prevented cancellation of prospective cleanup");
    const auto final = dt::BlobLifetimeRuntimeConformanceAccessV3::Consume(ledger, 1);
    Check(final.obligation_consumed && final.cleanup_callback_charge_commit &&
              final.ledger_terminal_quarantined &&
              dt::BlobLifetimeRuntimeConformanceAccessV3::ReservedCleanup(ledger) == 0,
          "prospective cancellation lost real release accounting");
  }
  {
    dt::BlobLifetimeBudgetLedgerV3Generation1 ledger;
    dt::BlobLifetimeRuntimeConformanceAccessV3::SetCallbackLimit(ledger, 8);
    dt::BlobLifetimeRuntimeConformanceAccessV3::SetCounter(
        ledger, dt::BlobLifetimeResourceV3::lifetime_callback_calls, 1, 2, 2);
    std::uint64_t available = 0;
    Check(!dt::BlobLifetimeRuntimeConformanceAccessV3::Charge(
              ledger, dt::BlobLifetimeResourceV3::lifetime_callback_calls, 1, &available),
          "generic callback charge bypassed sticky callback corruption");
    // Fault injection makes the numbers look healthy; the original callback
    // corruption must remain sticky and must never fabricate an invoked count.
    dt::BlobLifetimeRuntimeConformanceAccessV3::SetCounter(
        ledger, dt::BlobLifetimeResourceV3::lifetime_callback_calls, 8, 1, 1);
    const auto cleanup = dt::BlobLifetimeRuntimeConformanceAccessV3::Consume(ledger, 1);
    const auto snapshot = dt::BlobLifetimeRuntimeConformanceAccessV3::Snapshot(ledger);
    Check(cleanup.obligation_consumed && !cleanup.cleanup_callback_charge_commit &&
              snapshot.terminal_quarantined &&
              snapshot.lifetime_callback_terminal_quarantined &&
              dt::BlobLifetimeRuntimeConformanceAccessV3::Invoked(ledger) == 1 &&
              dt::BlobLifetimeRuntimeConformanceAccessV3::ReservedCleanup(ledger) == 0,
          "numeric repair resurrected terminal callback ledger");
  }
  {
    Fixture fixture(8);
    const std::array<platform::byte, 1> content{0x51};
    auto carrier = dt::BlobLifetimeRuntimeConformanceAccessV3::Carrier(
        fixture.carrier_binding, dt::BlobValueStateV3::value, content.data(), 1);
    dt::BlobRetainedLifetimeLeaseV3 lease;
    Check(dt::RetainBaseBlobLifetimeLeaseV3Generation1(
        fixture.profile, fixture.Admission(), carrier, fixture.token,
        fixture.request, lease).ok(), "independent cleanup runtime retain");
    dt::BlobLifetimeRuntimeConformanceAccessV3::SetCounter(
        fixture.ledger, dt::BlobLifetimeResourceV3::logical_bytes_read, 0, 1, 0);
    VisitContext visit;
    auto visitor = dt::BlobLifetimeRuntimeConformanceAccessV3::Visitor(CopyVisitor, &visit);
    const auto result = lease.VisitBoundMaterializedBytes(visitor);
    Check(result.disposition == dt::BlobLifetimeOuterDispositionV3::terminated_out_of_band &&
              CountCall(fixture.fake, 'R') == 1 && CountCall(fixture.fake, 'B') == 1 &&
              CountCall(fixture.fake, 'E') == 1 && CountCall(fixture.fake, 'L') == 1 &&
              CountCall(fixture.fake, 'P') == 0 && visit.copied_size == 0 &&
              dt::BlobLifetimeRuntimeConformanceAccessV3::Invoked(fixture.ledger) == 4 &&
              dt::BlobLifetimeRuntimeConformanceAccessV3::ReservedCleanup(fixture.ledger) == 0,
          "runtime noncallback corruption lost exact end/release accounting");
  }
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

bool ReleaseLosesToOrdinaryWork(std::uint8_t op, unsigned carrier_kind) {
  using Access = dt::BlobLifetimeRuntimeConformanceAccessV3;
  std::array<platform::byte,8> scratch{};
  dt::BlobLifetimeBudgetConfigurationV3Generation1 config;
  config.limits.fill(UINT64_MAX); config.scratch = scratch; config.admitted_window_octets = 4;
  Fixture f(32,0,&config);
  f.fake.pins = 0;
  f.request.operation = op;
  dt::BlobRetainedLifetimeLeaseV3 lease;
  GuardedReaderContext reader_context{&f.fake};
  const std::array<platform::byte,1> bytes{{0x51}};
  if (carrier_kind == 0) {
    auto carrier = Access::Carrier(f.carrier_binding,dt::BlobValueStateV3::value,bytes.data(),1);
    if (!dt::RetainBaseBlobLifetimeLeaseV3Generation1(f.profile,f.Admission(),carrier,
        f.token,f.request,lease).ok()) return false;
  } else RetainGuardedReader(f,4,carrier_kind == 2,reader_context,lease);
  struct Context {
    dt::BlobRetainedLifetimeLeaseV3* lease;
    dt::BlobLifetimeRuntimeResultV3 loser;
    unsigned calls = 0;
  } context{&lease,{}};
  auto visitor = Access::Visitor([](void* raw,std::span<const platform::byte>) -> dt::BlobProtectedWorkResultV3 {
    auto& c = *static_cast<Context*>(raw);
    ++c.calls;
    c.loser = c.lease->Release();
    dt::BlobProtectedWorkResultV3 result; result.status = 0; return result;
  },&context);
  const auto result = carrier_kind == 0 ? lease.VisitBoundMaterializedBytes(visitor)
                                        : lease.VisitBoundReaderBytes(visitor);
  return result.reason == "same_ticket_concurrent_use" &&
      context.loser.reason == "same_ticket_concurrent_use" && context.calls == 1 &&
      f.fake.pins == 0 && CountCall(f.fake,'R') == 1 && CountCall(f.fake,'B') == 1 &&
      CountCall(f.fake,'E') == 1 && CountCall(f.fake,'L') == 1 && CountCall(f.fake,'P') == 0 &&
      ExactInvariantTuple(f.fake,dt::BlobLifetimeInvariantEventV3::same_ticket_concurrent_use,
          dt::BlobLifetimePhaseV3::protected_access,op,255);
}

std::size_t ReleaseOverlappingOrdinaryRegressions() {
  std::size_t failures = 0;
  for (const std::uint8_t op : {1,2,4,5,6,7})
    if (!RunIsolatedRegression("release_loses_to_materialized_work",[op] {
      return ReleaseLosesToOrdinaryWork(op,0);
    })) ++failures;
  for (std::uint8_t op = 2; op <= 13; ++op)
    if (!RunIsolatedRegression("release_loses_to_guarded_reader_work",[op] {
      return ReleaseLosesToOrdinaryWork(op,op >= 9 ? 2 : 1);
    })) ++failures;
  return failures;
}

std::size_t ReleaseAdministrativeOverlapRegressions() {
  using Access = dt::BlobLifetimeRuntimeConformanceAccessV3;
  std::size_t failures = 0;
  // These control-level tests use real retained tickets, admitted entrants,
  // native transfer claims and actual cleanup. They are not substitutes for
  // the real Clone/Move API interleavings below and elsewhere in this suite.
  for (std::uint8_t operation = 15; operation <= 17; ++operation) {
    for (bool during_release : {false,true}) {
      if (!RunIsolatedRegression(during_release ? "release_loses_to_administrative_cleanup" :
              "release_loses_to_administrative_claim",[operation,during_release] {
        Fixture f(16);
        f.fake.pins = 0;
        auto carrier = Access::Carrier(f.carrier_binding,dt::BlobValueStateV3::value,nullptr,0);
        dt::BlobRetainedLifetimeLeaseV3 lease;
        if (!dt::RetainBaseBlobLifetimeLeaseV3Generation1(f.profile,f.Admission(),carrier,
            f.token,f.request,lease).ok()) return false;
        dt::BlobLifetimeRuntimeResultV3 owner;
        dt::BlobLifetimeRuntimeResultV3 loser;
        if (during_release) {
          f.fake.block_release = true;
          std::thread thread([&] {
            if (!Access::ControlEnter(lease) ||
                static_cast<std::uint8_t>(Access::ControlClaimTransfer(lease,operation).status) != 0)
              Fail("administrative cleanup owner admission");
            owner = Access::ReleaseTransferOwner(lease,operation);
            Access::ControlLeave(lease);
          });
          while (!f.fake.release_entered.load(std::memory_order_acquire)) std::this_thread::yield();
          loser = lease.Release();
          f.fake.finish_release.store(true,std::memory_order_release);
          thread.join();
        } else {
          if (!Access::ControlEnter(lease) ||
              static_cast<std::uint8_t>(Access::ControlClaimTransfer(lease,operation).status) != 0)
            return false;
          loser = lease.Release();
          owner = Access::ReleaseTransferOwner(lease,operation);
          Access::ControlLeave(lease);
        }
        const auto phase = during_release ? dt::BlobLifetimePhaseV3::release : operation == 15 ?
            dt::BlobLifetimePhaseV3::retain : operation == 16 ? dt::BlobLifetimePhaseV3::move_construct :
            dt::BlobLifetimePhaseV3::move_replace;
        return loser.reason == "same_ticket_concurrent_use" && owner.reason == "same_ticket_concurrent_use" &&
            CountCall(f.fake,'R') == 1 && CountCall(f.fake,'L') == 1 && f.fake.pins == 0 &&
            Access::ControlEntrants(lease) == 0 && ExactInvariantTuple(f.fake,
                dt::BlobLifetimeInvariantEventV3::same_ticket_concurrent_use,phase,operation,255);
      })) ++failures;
    }
  }
  if (!RunIsolatedRegression("second_release_loses_to_explicit_release_owner",[] {
    Fixture f(16); f.fake.pins = 0;
    auto carrier = Access::Carrier(f.carrier_binding,dt::BlobValueStateV3::value,nullptr,0);
    dt::BlobRetainedLifetimeLeaseV3 lease;
    if (!dt::RetainBaseBlobLifetimeLeaseV3Generation1(f.profile,f.Admission(),carrier,
        f.token,f.request,lease).ok()) return false;
    f.fake.block_release = true;
    dt::BlobLifetimeRuntimeResultV3 owner;
    std::thread thread([&] { owner = lease.Release(); });
    while (!f.fake.release_entered.load(std::memory_order_acquire)) std::this_thread::yield();
    const auto loser = lease.Release();
    f.fake.finish_release.store(true,std::memory_order_release);
    thread.join();
    return owner.reason == "same_ticket_concurrent_use" && loser.reason == "same_ticket_concurrent_use" &&
        CountCall(f.fake,'L') == 1 && f.fake.pins == 0 && ExactInvariantTuple(f.fake,
            dt::BlobLifetimeInvariantEventV3::same_ticket_concurrent_use,dt::BlobLifetimePhaseV3::release,18,255);
  })) ++failures;
  for (unsigned kind = 0; kind < 3; ++kind) {
    if (!RunIsolatedRegression("release_loses_to_ordinary_failure_cleanup",[kind] {
      std::array<platform::byte,4> scratch{};
      dt::BlobLifetimeBudgetConfigurationV3Generation1 config;
      config.limits.fill(UINT64_MAX); config.scratch = scratch; config.admitted_window_octets = 4;
      Fixture f(16,0,&config); f.fake.pins = 0;
      f.request.operation = kind == 2 ? 9 : 2;
      dt::BlobRetainedLifetimeLeaseV3 lease;
      GuardedReaderContext context{&f.fake};
      const std::array<platform::byte,1> bytes{{0x51}};
      if (kind == 0) {
        auto carrier = Access::Carrier(f.carrier_binding,dt::BlobValueStateV3::value,bytes.data(),1);
        if (!dt::RetainBaseBlobLifetimeLeaseV3Generation1(f.profile,f.Admission(),carrier,
            f.token,f.request,lease).ok()) return false;
      } else RetainGuardedReader(f,4,kind == 2,context,lease);
      f.fake.block_release = true;
      auto visitor = Access::Visitor(ResourceVisitor,nullptr);
      dt::BlobLifetimeRuntimeResultV3 owner;
      std::thread thread([&] {
        owner = kind == 0 ? lease.VisitBoundMaterializedBytes(visitor) : lease.VisitBoundReaderBytes(visitor);
      });
      while (!f.fake.release_entered.load(std::memory_order_acquire)) std::this_thread::yield();
      const auto loser = lease.Release();
      f.fake.finish_release.store(true,std::memory_order_release);
      thread.join();
      return owner.diagnostic_code == "RESOURCE.BUDGET_EXCEEDED" &&
          owner.required == 2 && owner.available == 1 && loser.reason == "same_ticket_concurrent_use" &&
          f.fake.pins == 0 && CountCall(f.fake,'E') == 1 && CountCall(f.fake,'L') == 1 &&
          ExactInvariantTuple(f.fake,dt::BlobLifetimeInvariantEventV3::same_ticket_concurrent_use,
                              dt::BlobLifetimePhaseV3::protected_access,f.request.operation,255);
    })) ++failures;
  }
  if (!RunIsolatedRegression("release_loses_to_real_clone_before_new_retain",[] {
    Fixture f(32); f.fake.pins = 0;
    auto carrier = Access::Carrier(f.carrier_binding,dt::BlobValueStateV3::value,nullptr,0);
    dt::BlobRetainedLifetimeLeaseV3 source,destination;
    if (!dt::RetainBaseBlobLifetimeLeaseV3Generation1(f.profile,f.Admission(),carrier,
        f.token,f.request,source).ok()) return false;
    f.fake.block_clone_pin = true;
    dt::BlobLifetimeRuntimeResultV3 owner;
    std::thread thread([&] { owner = source.CloneInto(destination); });
    while (!f.fake.clone_pin_entered.load(std::memory_order_acquire)) std::this_thread::yield();
    const auto loser = source.Release();
    f.fake.finish_clone_pin.store(true,std::memory_order_release);
    thread.join();
    return !source.retained() && !destination.retained() && f.fake.pins == 0 &&
        owner.reason == "same_ticket_concurrent_use" && loser.reason == "same_ticket_concurrent_use" &&
        ExactInvariantTuple(f.fake,dt::BlobLifetimeInvariantEventV3::same_ticket_concurrent_use,
                            dt::BlobLifetimePhaseV3::retain,15,255);
  })) ++failures;
  if (!RunIsolatedRegression("release_loses_to_real_clone_failure_cleanup",[] {
    Fixture f(32); f.fake.pins = 0;
    auto carrier = Access::Carrier(f.carrier_binding,dt::BlobValueStateV3::value,nullptr,0);
    dt::BlobRetainedLifetimeLeaseV3 source,destination;
    if (!dt::RetainBaseBlobLifetimeLeaseV3Generation1(f.profile,f.Admission(),carrier,
        f.token,f.request,source).ok()) return false;
    f.fake.block_release = true;
    f.fake.cancel_on_next_retain = true;
    dt::BlobLifetimeRuntimeResultV3 owner;
    std::thread thread([&] { owner = source.CloneInto(destination); });
    while (!f.fake.release_entered.load(std::memory_order_acquire)) std::this_thread::yield();
    const auto loser = destination.Release();
    f.fake.finish_release.store(true,std::memory_order_release);
    thread.join();
    const bool exact = owner.diagnostic_code == "PROCESS.CANCELLED" &&
        loser.reason == "same_ticket_concurrent_use" && !destination.retained() && source.retained() &&
        ExactInvariantTuple(f.fake,dt::BlobLifetimeInvariantEventV3::same_ticket_concurrent_use,
                            dt::BlobLifetimePhaseV3::release,15,255);
    f.fake.cancelled = false;
    return source.Release().ok() && f.fake.pins == 0 && exact;
  })) ++failures;
  if (!RunIsolatedRegression("release_loses_to_real_move_replace_destination_cleanup",[] {
    Fixture f(32); f.fake.pins = 0;
    auto carrier = Access::Carrier(f.carrier_binding,dt::BlobValueStateV3::value,nullptr,0);
    dt::BlobRetainedLifetimeLeaseV3 source,destination;
    if (!dt::RetainBaseBlobLifetimeLeaseV3Generation1(f.profile,f.Admission(),carrier,
        f.token,f.request,source).ok() || !dt::RetainBaseBlobLifetimeLeaseV3Generation1(
        f.profile,f.Admission(),carrier,f.token,f.request,destination).ok()) return false;
    f.fake.block_release = true;
    dt::BlobLifetimeRuntimeResultV3 owner;
    std::thread thread([&] { owner = destination.MoveReplaceFrom(std::move(source)); });
    while (!f.fake.release_entered.load(std::memory_order_acquire)) std::this_thread::yield();
    const auto loser = destination.Release();
    f.fake.finish_release.store(true,std::memory_order_release);
    thread.join();
    const bool exact = owner.reason == "same_ticket_concurrent_use" &&
        loser.reason == "same_ticket_concurrent_use" && !destination.retained() && source.retained() &&
        ExactInvariantTuple(f.fake,dt::BlobLifetimeInvariantEventV3::same_ticket_concurrent_use,
                            dt::BlobLifetimePhaseV3::release,17,255);
    return source.Release().ok() && f.fake.pins == 0 && exact;
  })) ++failures;
  return failures;
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
  return ReleaseOverlappingOrdinaryRegressions() + ReleaseAdministrativeOverlapRegressions() +
         ReleaseOuterOperationRegressions() +
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
  auto budget = dt::BlobLifetimeRuntimeConformanceAccessV3::BudgetControl(
      fixture.ledger, fixture.pins);
  auto capability = dt::BlobLifetimeRuntimeConformanceAccessV3::Capability(
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

void ConfigureLogicalFixture(Fixture& fixture) {
  fixture.fake.pins = 0;
  fixture.token.mode_bits = SB_BLOB_MODE_IMMUTABLE_READ_V3 |
                            SB_BLOB_MODE_LOGICAL_READER_V3;
  fixture.request.required_mode_bits = fixture.token.mode_bits;
  fixture.carrier_binding.carrier_kind = SB_BLOB_MODE_LOGICAL_READER_V3;
}

dt::BlobLogicalReaderLengthResultV3 AdmittedLogicalLength(
    Fixture& fixture, dt::BlobValueStateV3 state, std::uint64_t length,
    bool null_allowed = true, unsigned post_admission_mutation = 0) {
  ConfigureLogicalFixture(fixture);
  auto budget = dt::BlobLifetimeRuntimeConformanceAccessV3::BudgetControl(
      fixture.ledger, fixture.pins);
  auto capability = dt::BlobLifetimeRuntimeConformanceAccessV3::LogicalCapability(
      fixture.carrier_binding, state, length, &fixture.fake,
      NeverReadDuringAdmission);
  dt::BlobLifetimeAuthorityAdmissionV3Generation1 admission;
  dt::BlobBoundLogicalReaderCarrierV3Generation1 carrier;
  const auto admitted = dt::BlobLifetimeRuntimeFactoryV3Generation1::AdmitLogicalReader(
      SB_BLOB_OP_READ_V3, fixture.profile, &fixture.authority, &fixture.services,
      fixture.pins, budget, fixture.services.monotonic_clock,
      fixture.services.monotonic_clock, fixture.token, fixture.request,
      capability, admission, carrier);
  if (!admitted.ok()) return {admitted};
  if (post_admission_mutation == 1) ++fixture.profile.receipt.registry_generation;
  if (post_admission_mutation == 2) ++fixture.token.immutable_binding_generation;
  return dt::ReadBlobLogicalReaderLengthV3Generation1(
      fixture.profile, carrier, std::move(admission), fixture.token,
      fixture.request, null_allowed);
}

void AuthenticatedBlobIdentityCasts() {
  namespace en = scratchbird::engine;
  using Access = dt::BlobLifetimeRuntimeConformanceAccessV3;
  const auto descriptor = [](const dt::BlobValidatedProfileHandleV3& profile) {
    en::ExecutionTypeDescriptor d;
    std::copy(profile.identity.descriptor_uuid.bytes.begin(),
              profile.identity.descriptor_uuid.bytes.end(), d.descriptor_uuid.bytes);
    d.descriptor_epoch = profile.identity.descriptor_generation;
    d.canonical_type_id = static_cast<std::uint32_t>(dt::CanonicalTypeId::blob);
    d.family = en::ExecutionTypeFamily::blob;
    d.width_class = en::ExecutionTypeWidthClass::descriptor_defined;
    return d;
  };
  for (bool logical : {false, true}) {
    for (const auto context : {dt::DatatypeCastContext::implicit,
                              dt::DatatypeCastContext::assignment,
                              dt::DatatypeCastContext::explicit_cast}) {
      for (unsigned scenario = 0; scenario < 26; ++scenario) {
        Fixture f(32);
        f.fake.pins = 0;  // No fixture-only initial owner: the factory acquires both pins.
        const bool null = scenario % 2 == 0;
        const auto state = null ? dt::BlobValueStateV3::sql_null : dt::BlobValueStateV3::value;
        const std::array<platform::byte,3> bytes{0,255,129};
        const auto length = null || scenario / 2 == 12 ? 0 :
            logical ? dt::kBlobMaximumLogicalBytesV3 : bytes.size();
        dt::BlobRetainedLifetimeLeaseV3 source, target;
        auto control = dt::BlobLifetimeRuntimeConformanceAccessV3::BudgetControl(f.ledger, f.pins);
        dt::BlobLifetimeAuthorityAdmissionV3Generation1 admission;
        if (logical) {
          ConfigureLogicalFixture(f);
          const auto capability = dt::BlobLifetimeRuntimeConformanceAccessV3::LogicalCapability(
              f.carrier_binding, state, length, &f.fake, NeverReadDuringAdmission);
          dt::BlobBoundLogicalReaderCarrierV3Generation1 carrier;
          Check(dt::BlobLifetimeRuntimeFactoryV3Generation1::AdmitLogicalReader(
              SB_BLOB_OP_READ_V3, f.profile, &f.authority, &f.services, f.pins,
              control, f.services.monotonic_clock, f.services.monotonic_clock,
              f.token, f.request, capability, admission, carrier).ok(), "identity logical admission");
          Check(dt::RetainBaseBlobLifetimeLeaseV3Generation1(f.profile, std::move(admission),
              carrier, f.token, f.request, source).ok(), "identity logical retain");
        } else {
          const auto capability = dt::BlobLifetimeRuntimeConformanceAccessV3::Capability(
              f.carrier_binding, state, null ? nullptr : bytes.data(), length);
          dt::BlobBoundMaterializedCarrierV3Generation1 carrier;
          Check(dt::BlobLifetimeRuntimeFactoryV3Generation1::AdmitMaterialized(
              SB_BLOB_OP_READ_V3, f.profile, &f.authority, &f.services, f.pins,
              control, f.services.monotonic_clock, f.services.monotonic_clock,
              f.token, f.request, capability, admission, carrier).ok(), "identity materialized admission");
          Check(dt::RetainBaseBlobLifetimeLeaseV3Generation1(f.profile, std::move(admission),
              carrier, f.token, f.request, source).ok(), "identity materialized retain");
        }
        auto p = f.profile;
        auto a = descriptor(p), b = a;
        b.stable_name = "alias-is-data-not-authority";
        const auto alias = source.CastIdentityInto(p, a, b, context, source);
        Check(!alias.ok() && source.retained() && f.fake.pins == 2 && CountCall(f.fake, 'R') == 1,
              "identity cast aliased its source or changed ownership on alias refusal");
        auto selected_context = context;
        switch (scenario / 2) {
          case 1: ++b.descriptor_epoch; break;
          case 2: b.charset_uuid.bytes[0] = 1; break;
          case 3: a.domain_stack.push_back(a.descriptor_uuid); break;
          case 4: ++p.receipt.registry_generation; break;
          case 5: b.nullable_allowed = false; break;
          case 6: selected_context = static_cast<dt::DatatypeCastContext>(65535); break;
          case 7: f.fake.pin_clone_status = dt::BlobReceiverPinAcquireStatusV3::unavailable; break;
          case 8: f.fake.retain_code = SB_BLOB_CALLBACK_SECURITY_DENIED_V3; break;
          case 9: f.fake.retain_code = SB_BLOB_CALLBACK_STALE_SNAPSHOT_V3; break;
          case 10: f.fake.cancelled = true; break;
          case 11: f.fake.clock_unavailable = true; break;
        }
        const auto allocations = allocation_probe::calls.load();
        allocation_probe::enabled.store(true);
        const auto cast = source.CastIdentityInto(p, a, b, selected_context, target);
        allocation_probe::enabled.store(false);
        Check(allocation_probe::calls.load() == allocations, "identity cast allocated");
        const bool positive = scenario / 2 == 0 || scenario / 2 == 12 || (scenario / 2 == 5 && !null);
        Check(cast.ok() == positive && target.retained() == positive,
              "identity cast admitted invalid binding or refused valid VALUE/NULL");
        Check(source.retained(), "identity cast consumed source");
        Check(CountCall(f.fake, 'B') == 0 && CountCall(f.fake, 'E') == 0,
              "identity cast accessed content");
        if (positive) {
          Check(Access::CarrierLength(target) == length &&
                    (logical ? Access::LogicalRoutingMatches(target, &f.fake, NeverReadDuringAdmission)
                             : Access::ScalarMatches(target, state, null ? nullptr : bytes.data())),
                "identity cast lost exact scalar or reader routing");
          Check(f.fake.pins == 4 && CountCall(f.fake, 'R') == 2,
                "identity cast failed distinct result pin/ticket ownership");
          const auto armed = source.CastIdentityInto(p, a, b, selected_context, target);
          Check(!armed.ok() && source.retained() && target.retained() && f.fake.pins == 4 &&
                    CountCall(f.fake, 'R') == 2, "identity cast overwrote an armed result");
          if (!logical && !null && length != 0) {
            VisitContext copied;
            auto visitor = Access::Visitor(CopyVisitor, &copied);
            Check(target.VisitBoundMaterializedBytes(visitor).ok() && copied.copied_size == bytes.size() &&
                      std::equal(bytes.begin(), bytes.end(), copied.copied.begin()),
                  "identity result did not preserve exact binary content");
          }
          if (scenario / 2 == 12) {
            Check(source.Release().ok() && f.fake.pins == 2 && target.Probe().ok(),
                  "identity result depended on source wrapper lifetime");
            Check(target.Release().ok() && f.fake.pins == 0 && Access::ReservedCleanup(f.ledger) == 0,
                  "empty/NULL independent identity cleanup");
            continue;
          }
          Check(target.Release().ok(), "identity result cleanup");
        } else {
          Check(f.fake.pins == 2 && Access::ReaderRoutingCleared(target),
                "failed identity cast published unowned routing or leaked pins");
        }
        f.fake.cancelled = false;
        f.fake.clock_unavailable = false;
        Check(source.Release().ok() && f.fake.pins == 0 && Access::ReservedCleanup(f.ledger) == 0,
              "identity cast failed exact cleanup");
      }
    }
  }
}

void AuthenticatedLogicalReaderLength() {
  for (const auto state : {dt::BlobValueStateV3::value,
                          dt::BlobValueStateV3::sql_null}) {
    for (const auto length : {UINT64_C(0), UINT64_C(1),
                             dt::kBlobMaximumLogicalBytesV3}) {
      if (state == dt::BlobValueStateV3::sql_null && length != 0) continue;
      Fixture fixture(3);
      dt::BlobLifetimeRuntimeConformanceAccessV3::SetCounter(
          fixture.ledger, dt::BlobLifetimeResourceV3::logical_bytes_read, 0, 0, 0);
      dt::BlobLifetimeRuntimeConformanceAccessV3::SetCounter(
          fixture.ledger, dt::BlobLifetimeResourceV3::reader_calls, 0, 0, 0);
      const auto before = allocation_probe::calls.load();
      allocation_probe::enabled.store(true);
      const auto result = AdmittedLogicalLength(fixture, state, length);
      allocation_probe::enabled.store(false);
      Check(allocation_probe::calls.load() == before,
            "logical metadata length allocated memory");
      Check(result.ok() && result.length == length &&
                result.is_null == (state == dt::BlobValueStateV3::sql_null),
            "logical reader exact uint64 metadata length/state");
      Check(fixture.fake.call_count == 3 && fixture.fake.calls[0] == 'R' &&
                fixture.fake.calls[1] == 'P' && fixture.fake.calls[2] == 'L' &&
                fixture.fake.pins == 0 && fixture.fake.pin_drop_calls == 2 &&
                dt::BlobLifetimeRuntimeConformanceAccessV3::ReservedCleanup(
                    fixture.ledger) == 0,
            "logical metadata length did not retain/probe/release exactly once");
    }
  }
  for (std::uint64_t budget = 0; budget < 3; ++budget) {
    Fixture fixture(budget);
    const auto result = AdmittedLogicalLength(fixture, dt::BlobValueStateV3::value, 1);
    Check(!result.ok() && !result.is_null && result.length == 0 &&
              result.runtime.diagnostic_code == "RESOURCE.BUDGET_EXCEEDED" &&
              CountCall(fixture.fake, 'R') == (budget == 2 ? 1 : 0) &&
              CountCall(fixture.fake, 'P') == 0 &&
              CountCall(fixture.fake, 'L') == (budget == 2 ? 1 : 0) &&
              fixture.fake.pins == 0 &&
              dt::BlobLifetimeRuntimeConformanceAccessV3::ReservedCleanup(
                  fixture.ledger) == 0,
          "logical length shortage lost cleanup or published failed metadata");
  }
  for (unsigned mutation = 0; mutation < 3; ++mutation) {
    Fixture fixture(3);
    const auto result = AdmittedLogicalLength(fixture, dt::BlobValueStateV3::sql_null,
                                             0, false, mutation);
    Check(!result.ok() && !result.is_null && result.length == 0 &&
              fixture.fake.call_count == 0 && fixture.fake.pins == 0,
          "logical NULL shortcut bypassed binding/profile/nullability gates");
    Check(mutation == 0
              ? result.runtime.diagnostic_code == "BLOB.STATE_INVALID"
              : mutation == 1
                  ? result.runtime.reason == "authority_profile_binding_mismatch"
                  : result.runtime.disposition ==
                      dt::BlobLifetimeOuterDispositionV3::terminated_out_of_band,
          "logical length wrong NULL/binding failure precedence");
  }
  // Both carriers share the complete callback/cleanup matrix. A logical
  // callback mode refusal must describe logical mode5, never hardcoded mode4.
  for (unsigned phase = 0; phase < 3; ++phase) {
    for (std::uint8_t code = 0; code <= 18; ++code) {
      Fixture materialized(3), logical(3);
      for (auto* fixture : {&materialized, &logical}) {
        if (phase == 0) fixture->fake.retain_code = code;
        else if (phase == 1) fixture->fake.probe_code = code;
        else fixture->fake.release_code = code;
      }
      const auto expected = AdmittedLength(materialized, dt::BlobValueStateV3::value, {});
      const auto result = AdmittedLogicalLength(logical, dt::BlobValueStateV3::value, 1);
      Check(result.runtime.disposition == expected.runtime.disposition &&
                result.runtime.diagnostic_code == expected.runtime.diagnostic_code &&
                result.runtime.reason == expected.runtime.reason &&
                result.runtime.fact.fact_class == expected.runtime.fact.fact_class &&
                logical.fake.call_count == materialized.fake.call_count &&
                logical.fake.pins == 0 &&
                (!result.ok() ? result.length == 0 && !result.is_null
                              : result.length == 1 && !result.is_null),
            "logical metadata callback matrix changed primary/cleanup semantics");
      if ((result.runtime.fact.parameters.present &
           dt::blob_parameter_open_mode) != 0)
        Check(result.runtime.fact.parameters.open_mode_enum == 5 &&
                  expected.runtime.fact.parameters.open_mode_enum == 4,
              "logical callback diagnostic did not preserve actual mode");
    }
  }
  for (bool cancellation : {false, true}) {
    Fixture fixture(3);
    if (cancellation) fixture.fake.cancel_on_release = true;
    else fixture.fake.poison_request_on_drop = &fixture.request;
    const auto result = AdmittedLogicalLength(fixture, dt::BlobValueStateV3::value,
                                             dt::kBlobMaximumLogicalBytesV3);
    Check(cancellation
              ? !result.ok() && result.length == 0 && !result.is_null &&
                    result.runtime.diagnostic_code == "PROCESS.CANCELLED"
              : result.ok() && result.length == dt::kBlobMaximumLogicalBytesV3,
          "logical metadata read after unpin or ignored final cancellation");
  }
}

void RetainedLogicalReaderTransfers() {
  for (const auto operation : {SB_BLOB_OP_READ_V3, SB_BLOB_OP_REPLAY_V3,
                               SB_BLOB_OP_COMPONENT_ENCODE_V3}) {
  Fixture fixture(16);
  ConfigureLogicalFixture(fixture);
  fixture.request.operation = operation;
  if (operation != SB_BLOB_OP_READ_V3) {
    fixture.token.mode_bits |= SB_BLOB_MODE_REPLAYABLE_SAME_SNAPSHOT_V3;
    fixture.request.required_mode_bits = fixture.token.mode_bits;
  }
  auto budget = dt::BlobLifetimeRuntimeConformanceAccessV3::BudgetControl(
      fixture.ledger, fixture.pins);
  auto capability = dt::BlobLifetimeRuntimeConformanceAccessV3::LogicalCapability(
      fixture.carrier_binding, dt::BlobValueStateV3::value, 3, &fixture.fake,
      NeverReadDuringAdmission);
  dt::BlobLifetimeAuthorityAdmissionV3Generation1 admission;
  dt::BlobBoundLogicalReaderCarrierV3Generation1 carrier;
  Check(dt::BlobLifetimeRuntimeFactoryV3Generation1::AdmitLogicalReader(
      operation, fixture.profile, &fixture.authority, &fixture.services,
      fixture.pins, budget, fixture.services.monotonic_clock,
      fixture.services.monotonic_clock, fixture.token, fixture.request,
      capability, admission, carrier).ok(), "logical transfer factory admission");
  dt::BlobRetainedLifetimeLeaseV3 source, clone, destination;
  Check(dt::RetainBaseBlobLifetimeLeaseV3Generation1(
      fixture.profile, std::move(admission), carrier, fixture.token,
      fixture.request, source).ok(), "logical transfer retain");
  fixture.fake.pin_clone_status = dt::BlobReceiverPinAcquireStatusV3::unavailable;
  const auto refused_clone = source.CloneInto(clone);
  Check(!refused_clone.ok() && source.retained() && fixture.fake.pins == 2 &&
            dt::BlobLifetimeRuntimeConformanceAccessV3::LogicalRoutingMatches(
                source, &fixture.fake, NeverReadDuringAdmission) &&
            dt::BlobLifetimeRuntimeConformanceAccessV3::ReaderRoutingCleared(clone),
        "logical failed clone damaged source or published unowned routing");
  fixture.fake.pin_clone_status = dt::BlobReceiverPinAcquireStatusV3::acquired;
  Check(source.CloneInto(clone).ok(), "logical clone rejected admitted carrier");
  auto moved = dt::BlobRetainedLifetimeLeaseV3::MoveConstructFrom(std::move(source));
  Check(moved.result.ok() && destination.MoveReplaceFrom(std::move(moved.value)).ok(),
        "logical fallible move lost carrier");
  for (auto* lease : {&clone, &destination})
    Check(dt::BlobLifetimeRuntimeConformanceAccessV3::LogicalRoutingMatches(
              *lease, &fixture.fake, NeverReadDuringAdmission) && lease->Probe().ok(),
          "logical transfer lost exact pinned reader routing");
  Check(dt::BlobLifetimeRuntimeConformanceAccessV3::ReaderRoutingCleared(source) &&
            dt::BlobLifetimeRuntimeConformanceAccessV3::ReaderRoutingCleared(moved.value),
        "logical moved-from retained unowned reader routing");
  VisitContext context;
  const auto visitor = dt::BlobLifetimeRuntimeConformanceAccessV3::Visitor(
      CopyVisitor, &context);
  const auto refused = destination.VisitBoundMaterializedBytes(visitor);
  Check(refused.disposition == dt::BlobLifetimeOuterDispositionV3::terminated_out_of_band &&
            CountCall(fixture.fake, 'B') == 0 && CountCall(fixture.fake, 'E') == 0 &&
            dt::BlobLifetimeRuntimeConformanceAccessV3::ReaderRoutingCleared(destination),
        "logical carrier was exposed as materialized bytes or lost cleanup");
  Check(clone.Release().ok() && fixture.fake.pins == 0 &&
            CountCall(fixture.fake, 'R') == 2 && CountCall(fixture.fake, 'L') == 2 &&
            dt::BlobLifetimeRuntimeConformanceAccessV3::ReaderRoutingCleared(clone) &&
            dt::BlobLifetimeRuntimeConformanceAccessV3::ReservedCleanup(fixture.ledger) == 0,
        "logical clone/move cleanup leaked pins or release obligations");
  }
}

void FailedLogicalRetainClearsAndReusesRouting() {
  for (unsigned failure = 0; failure < 4; ++failure) {
    Fixture fixture(failure < 2 ? failure : 3);
    ConfigureLogicalFixture(fixture);
    if (failure == 2) fixture.fake.cancelled = true;
    if (failure == 3) fixture.fake.retain_code = SB_BLOB_CALLBACK_SECURITY_DENIED_V3;
    auto budget = dt::BlobLifetimeRuntimeConformanceAccessV3::BudgetControl(
        fixture.ledger, fixture.pins);
    auto capability = dt::BlobLifetimeRuntimeConformanceAccessV3::LogicalCapability(
        fixture.carrier_binding, dt::BlobValueStateV3::value, 7, &fixture.fake,
        NeverReadDuringAdmission);
    dt::BlobLifetimeAuthorityAdmissionV3Generation1 admission;
    dt::BlobBoundLogicalReaderCarrierV3Generation1 carrier;
    Check(dt::BlobLifetimeRuntimeFactoryV3Generation1::AdmitLogicalReader(
        SB_BLOB_OP_READ_V3, fixture.profile, &fixture.authority, &fixture.services,
        fixture.pins, budget, fixture.services.monotonic_clock,
        fixture.services.monotonic_clock, fixture.token, fixture.request,
        capability, admission, carrier).ok(), "failed-retain factory setup");
    dt::BlobRetainedLifetimeLeaseV3 lease;
    const auto refused = dt::RetainBaseBlobLifetimeLeaseV3Generation1(
        fixture.profile, std::move(admission), carrier, fixture.token,
        fixture.request, lease);
    Check(!refused.ok() && !lease.retained() && fixture.fake.pins == 0 &&
              fixture.fake.pin_drop_calls == 2 &&
              CountCall(fixture.fake, 'L') == 0 &&
              dt::BlobLifetimeRuntimeConformanceAccessV3::ReaderRoutingCleared(lease) &&
              dt::BlobLifetimeRuntimeConformanceAccessV3::CarrierLength(lease) == 0,
          "failed logical retain left unowned routing or invented release");
    Fixture replacement(3);
    ConfigureLogicalFixture(replacement);
    auto replacement_budget = dt::BlobLifetimeRuntimeConformanceAccessV3::BudgetControl(
        replacement.ledger, replacement.pins);
    auto replacement_capability =
        dt::BlobLifetimeRuntimeConformanceAccessV3::LogicalCapability(
            replacement.carrier_binding, dt::BlobValueStateV3::value, 9,
            &replacement.fake, NeverReadDuringAdmission);
    dt::BlobLifetimeAuthorityAdmissionV3Generation1 replacement_admission;
    dt::BlobBoundLogicalReaderCarrierV3Generation1 replacement_carrier;
    Check(dt::BlobLifetimeRuntimeFactoryV3Generation1::AdmitLogicalReader(
        SB_BLOB_OP_READ_V3, replacement.profile, &replacement.authority,
        &replacement.services, replacement.pins, replacement_budget,
        replacement.services.monotonic_clock, replacement.services.monotonic_clock,
        replacement.token, replacement.request, replacement_capability,
        replacement_admission, replacement_carrier).ok(), "reuse factory setup");
    Check(dt::RetainBaseBlobLifetimeLeaseV3Generation1(
        replacement.profile, std::move(replacement_admission), replacement_carrier,
        replacement.token, replacement.request, lease).ok() &&
              dt::BlobLifetimeRuntimeConformanceAccessV3::LogicalRoutingMatches(
                  lease, &replacement.fake, NeverReadDuringAdmission) &&
              dt::BlobLifetimeRuntimeConformanceAccessV3::CarrierLength(lease) == 9 &&
              lease.Probe().ok() && lease.Release().ok() && replacement.fake.pins == 0 &&
              dt::BlobLifetimeRuntimeConformanceAccessV3::ReaderRoutingCleared(lease),
          "failed logical retain wrapper could not admit fresh owned routing");
  }
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
  if (mode == "release_overlap") {
    if (ReleaseOverlappingOrdinaryRegressions()+ReleaseAdministrativeOverlapRegressions() != 0) return EXIT_FAILURE;
    std::cout << "PASS release overlap checks=" << checks << '\n';
    return EXIT_SUCCESS;
  }
  if (mode == "post_work") {
    ProtectedWorkFailureObservation();
    ProtectedWorkCleanupSelection();
    std::cout << "PASS protected work observations checks=" << checks << '\n';
    return EXIT_SUCCESS;
  }
  if (mode == "guarded_reader") {
    GuardedReaderExecution();
    GuardedReaderAdmissionAndPinLifetime();
    GuardedReaderSharedArenaAndBytes();
    GuardedReaderEmptyAndPrivateGates();
    ProtectedReaderSamplerAdmission();
    std::cout << "PASS guarded reader checks=" << checks << '\n';
    return EXIT_SUCCESS;
  }
  if (mode == "guarded_replay") {
    GuardedReaderReplayExecution();
    GuardedReaderReplayFailureAndOwnership();
    GuardedReaderReplayTransitionObservations();
    std::cout << "PASS guarded replay checks=" << checks << '\n';
    return EXIT_SUCCESS;
  }
  if (mode == "component_digest") {
    BlobComponentGoldenDigestPasses();
    BlobComponentFieldAndIntegrityFailures();
    BlobComponentPassLifecycleAndBounds();
    BlobComponentFragmentedHashBoundariesAndScrub();
    BlobComponentInitialHeaderAdmission();
    std::cout << "PASS native component digest checks=" << checks << '\n';
    return EXIT_SUCCESS;
  }
  if (mode == "timeout_rounding") {
    ExactAdmittedTimeoutRounding();
    std::cout << "PASS exact timeout rounding checks=" << checks << '\n';
    return EXIT_SUCCESS;
  }
  if (mode == "factory") {
    LogicalReaderFactoryAdmission();
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
  BlobComponentGoldenDigestPasses();
  BlobComponentFieldAndIntegrityFailures();
  BlobComponentPassLifecycleAndBounds();
  BlobComponentFragmentedHashBoundariesAndScrub();
  BlobComponentInitialHeaderAdmission();
  ProtectedWorkFailureObservation();
  ProtectedWorkCleanupSelection();
  GuardedReaderExecution();
  GuardedReaderReplayExecution();
  GuardedReaderReplayFailureAndOwnership();
  GuardedReaderReplayTransitionObservations();
  GuardedReaderAdmissionAndPinLifetime();
  GuardedReaderSharedArenaAndBytes();
  GuardedReaderEmptyAndPrivateGates();
  ProtectedReaderSamplerAdmission();
  ExactAdmittedTimeoutRounding();
  ReaderResultClosedShapeMatrix();
  LogicalReaderFactoryAdmission();
  AuthenticatedLogicalReaderLength();
  AuthenticatedBlobIdentityCasts();
  RetainedLogicalReaderTransfers();
  FailedLogicalRetainClearsAndReusesRouting();
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
  ContextualNullDescriptorBinding();
  EncodedReaderAdmissionAndRetention();
  GuardedEncodedRangeBoundaries();
  ReservedSinkWorkAccounting();
  IndependentResourceCleanupAccounting();
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
