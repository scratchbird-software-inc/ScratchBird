// Copyright (c) 2026 ScratchBird Software Inc.
// SPDX-License-Identifier: MPL-2.0
#pragma once

#include "datatype_type_codec_identity_v3.hpp"
#include "scratchbird/engine/blob_lifetime_abi.h"

#include <array>
#include <atomic>
#include <cstddef>
#include <cstdint>
#include <limits>
#include <span>
#include <string_view>
#include <type_traits>

namespace scratchbird::core::datatypes {

using platform::byte;
using platform::u8;
using platform::u16;
using platform::u32;
using platform::u64;

inline constexpr u64 kBlobMaximumLogicalBytesV3 =
    static_cast<u64>(std::numeric_limits<std::int64_t>::max());
inline constexpr u32 kBlobProfileFingerprintPreimageBytesV3 = 3423;

inline constexpr platform::Uuid kBlobV11ReceiptUuid{{
    0x01,0xa1,0x09,0x5f,0xf2,0x05,0x72,0xd3,
    0xab,0xea,0x15,0xc6,0xd4,0x1a,0x4a,0xd4}};
inline constexpr platform::Uuid kBlobV3ProfileUuid{{
    0x01,0xa1,0x09,0x5f,0xf2,0x05,0x73,0x9c,
    0x81,0x66,0x22,0x1d,0xfe,0x6b,0x81,0x8d}};
inline constexpr std::array<byte, 32> kBlobV3ProfileFingerprint{{
    0x62,0xf2,0x2d,0x99,0x1a,0x4f,0xce,0x05,
    0xd7,0x90,0x58,0x4e,0xdd,0xe0,0xc0,0x5c,
    0x66,0x98,0x3d,0x50,0x6a,0xe2,0xa9,0xec,
    0x61,0x66,0xad,0x5c,0x08,0x99,0x3c,0x83}};

struct BlobAuthorityReceiptV3 {
  platform::Uuid receipt_uuid{};
  platform::Uuid catalog_snapshot_uuid{};
  u64 catalog_generation = 0;
  u64 registry_generation = 0;
};

enum class BlobPolicyKindV3 : u8 {
  descriptor = 0,
  canonicalization,
  bounds,
  state,
  memory,
  resource,
  lifetime,
  component_codec,
  row_state,
  hash,
  comparison,
  operation,
  cast,
  index,
  statistics,
  backup,
  protection,
  wire,
  diagnostic,
  metric,
  v1_v2,
  count,
};

inline constexpr std::size_t kBlobPolicyBindingCountV3 =
    static_cast<std::size_t>(BlobPolicyKindV3::count);

struct BlobTypeCodecIdentitySnapshotV3 {
  platform::Uuid catalog_snapshot_uuid{};
  u64 catalog_generation = 0;
  u64 registry_generation = 0;
  platform::Uuid descriptor_uuid{};
  u64 descriptor_generation = 0;
  platform::Uuid type_uuid{};
  u64 type_generation = 0;
  platform::Uuid codec_uuid{};
  u16 codec_version = 0;
  u64 codec_generation = 0;
  u8 datatype_identity_code = 0;
  u8 null_encoding_code = 0;
  u8 byte_order_code = 0;
  u8 representation_code = 0;
  bool null_supported = false;
  bool signed_code = false;
  bool empty_value_distinct_from_sql_null = false;
  bool sql_null_requires_zero_payload = false;
  bool canonical_value_variable_width = false;
  u64 canonical_value_minimum_bytes = 0;
  u64 canonical_value_maximum_bytes = 0;
  u64 canonical_value_transport_width = 0;
  u32 canonical_binary_type_code = 0;
  DatatypePolicyIdentityV3 descriptor_policy{};
  DatatypePolicyIdentityV3 canonicalization_policy{};
  DatatypePolicyIdentityV3 ordering_policy{};
  DatatypePolicyIdentityV3 hash_policy{};
  DatatypePolicyIdentityV3 operation_policy{};
  platform::Uuid policy_profile_uuid{};
  u64 policy_profile_generation = 0;
  std::array<byte, 32> profile_fingerprint_sha256{};
};

struct BlobValidatedProfileHandleV3 {
  BlobAuthorityReceiptV3 receipt;
  BlobTypeCodecIdentitySnapshotV3 identity;
  platform::Uuid profile_uuid{};
  u64 profile_generation = 0;
  std::array<DatatypePolicyIdentityV3, kBlobPolicyBindingCountV3>
      policy_bindings{};
  std::array<byte, 32> profile_fingerprint{};
  u32 profile_fingerprint_preimage_bytes = 0;
};

enum class BlobValueStateV3 : u8 { value = 0, sql_null = 1 };

// A raw span is only a structural input. It is never lifetime authority and
// can never produce a retained public view. Runtime acceptance is available
// only through the generation-specific trusted factory declared below.
struct BlobMaterializedValueViewV3 {
  const BlobValidatedProfileHandleV3* profile = nullptr;
  BlobValueStateV3 state = BlobValueStateV3::value;
  u64 logical_length = 0;
  std::span<const byte> bytes;
};

enum class BlobStructuralDiagnosticV3 : u8 {
  none = 0,
  descriptor_invalid = 1,
  state_invalid = 2,
  length_exceeded = 3,
  canonical_encoding_invalid = 4,
  resource_budget_exceeded = 5,
};

struct BlobExecutionControlV3 {
  u64 maximum_allocation_bytes = 0;
  u64 maximum_content_bytes_read = 0;
  u64 maximum_content_bytes_written = 0;
  u64 maximum_reader_calls = 0;
  std::span<byte> scratch;
};

struct BlobValidationResultV3 {
  platform::Status status{};
  BlobStructuralDiagnosticV3 diagnostic = BlobStructuralDiagnosticV3::none;
  bool ok() const noexcept { return status.ok(); }
};

struct BlobProfileResultV3 {
  platform::Status status{};
  BlobStructuralDiagnosticV3 diagnostic = BlobStructuralDiagnosticV3::none;
  BlobValidatedProfileHandleV3 profile{};
  bool ok() const noexcept { return status.ok(); }
};

BlobProfileResultV3 BuildBlobValidatedProfileHandleV3(
    const BlobAuthorityReceiptV3& receipt,
    const DatatypeTypeCodecIdentityRowV3& identity) noexcept;

BlobProfileResultV3 BuildCurrentBlobValidatedProfileHandleV3(
    const platform::Uuid& receipt_uuid) noexcept;

BlobValidationResultV3 ValidateBlobProfileHandleV3(
    const BlobValidatedProfileHandleV3& profile,
    const BlobExecutionControlV3& control = {}) noexcept;

BlobValidationResultV3 ValidateBlobMaterializedValueViewNoAllocV3(
    const BlobMaterializedValueViewV3& value,
    bool null_allowed = true,
    const BlobExecutionControlV3& control = {}) noexcept;

// The declarations below are the private generation-1 materialized lifetime
// runtime. They do not alter the frozen public C ABI above.

enum class BlobLifetimePublicPhaseV3 : u8 {
  retain = 1,
  probe = 2,
  begin_access = 3,
  protected_access = 4,
  end_access = 5,
  release = 6,
  final_probe = 7,
  atomic_commit = 8,
};

enum class BlobLifetimeGateV3 : u8 {
  before_retain = 1,
  after_retain = 2,
  before_ordinary_probe = 3,
  after_ordinary_probe = 4,
  before_final_probe = 5,
  after_final_probe = 6,
  before_begin_access = 7,
  after_begin_access = 8,
  before_protected_work = 9,
  after_protected_work = 10,
  before_pass_transition = 11,
  before_sink_commit = 12,
  before_publication_or_return = 13,
};

enum class BlobLifetimePhaseV3 : u8 {
  factory_admission = 1,
  retain = 2,
  probe = 3,
  begin_access = 4,
  protected_access = 5,
  end_access = 6,
  release = 7,
  pass_transition = 8,
  atomic_commit = 9,
  move_construct = 10,
  move_replace = 11,
  destructor_cleanup = 12,
  shutdown_drain = 13,
  late_entry = 14,
};

enum class BlobLifetimeInvariantEventV3 : u8 {
  adapter_exception = 1,
  callback_reentrant = 2,
  callback_result_unknown = 3,
  dirty_failure_output = 4,
  malformed_success_ticket = 5,
  end_access_failed = 6,
  release_failed = 7,
  same_ticket_concurrent_use = 8,
  receiver_services_failed = 9,
  shutdown_drain_violation = 10,
  monotonic_clock_regression = 11,
  callback_budget_counter_corrupt = 12,
  progress_counter_overflow = 13,
};

enum class BlobLifetimeOuterDispositionV3 : u8 {
  success = 0,
  public_failure = 1,
  terminated_out_of_band = 2,
};

enum class BlobReceiverSampleStatusV3 : u8 { ok = 0, unavailable = 1 };
enum class BlobReceiverEffectStatusV3 : u8 {
  accepted = 0,
  already_applied = 1,
  fatal = 2,
};
enum class BlobReceiverPinAcquireStatusV3 : u8 {
  acquired = 0,
  draining = 1,
  unavailable = 2,
  fatal = 3,
};

struct BlobMonotonicClockBindingV3 {
  platform::Uuid clock_uuid{};
  u64 clock_generation = 0;
};

struct BlobLifetimeBindingKeyV3 {
  platform::Uuid authority_instance_uuid{};
  u64 authority_instance_generation = 0;
  platform::Uuid lifetime_token_uuid{};
  u64 lifetime_token_generation = 0;
  u64 immutable_binding_generation = 0;
};

struct BlobLifetimeInvariantFactV3 {
  BlobLifetimeInvariantEventV3 event =
      BlobLifetimeInvariantEventV3::receiver_services_failed;
  u8 operation_enum = 0;
  BlobLifetimePhaseV3 phase = BlobLifetimePhaseV3::factory_admission;
  u8 callback_code_or_255 = 255;
  u64 authority_instance_generation = 0;
  u64 lifetime_token_generation = 0;
  u64 immutable_binding_generation = 0;
};

// Internal generation-1 boundary predicate. Receiver adapters and conformance
// tests use the same closed domain check that guards invariant-service entry.
// This header is private to core datatypes; the predicate is not public C ABI.
bool ValidateBlobLifetimeInvariantFactDomainV3(
    const BlobLifetimeBindingKeyV3& key,
    const BlobLifetimeInvariantFactV3& fact) noexcept;

struct BlobMonotonicReadResultV3 {
  BlobReceiverSampleStatusV3 status = BlobReceiverSampleStatusV3::unavailable;
  std::array<u8, 7> reserved{};
  u64 now_monotonic_ns = 0;
};

struct BlobCancellationSampleResultV3 {
  BlobReceiverSampleStatusV3 status = BlobReceiverSampleStatusV3::unavailable;
  u8 cancelled = 0;
  std::array<u8, 6> reserved{};
};

struct BlobReceiverEffectResultV3 {
  BlobReceiverEffectStatusV3 status = BlobReceiverEffectStatusV3::fatal;
  std::array<u8, 7> reserved{};
};

struct BaseBlobLifetimeReceiverServicesV3Generation1 {
  platform::Uuid receiver_services_uuid{};
  u64 receiver_services_generation = 0;
  BlobMonotonicClockBindingV3 monotonic_clock{};
  void* context = nullptr;
  BlobMonotonicReadResultV3 (*read_monotonic_ns)(void*) noexcept = nullptr;
  BlobCancellationSampleResultV3 (*sample_cancellation)(
      void*, u8, BlobLifetimePublicPhaseV3) noexcept = nullptr;
  // The provider is required not to throw. The adapter catches a violating
  // provider, so this private pointer deliberately has no noexcept qualifier.
  BlobReceiverEffectResultV3 (*record_invariant_and_quarantine)(
      void*, const BlobLifetimeBindingKeyV3&,
      const BlobLifetimeInvariantFactV3&) = nullptr;
};

struct BlobReceiverPinCloneResultV3 {
  BlobReceiverPinAcquireStatusV3 status =
      BlobReceiverPinAcquireStatusV3::fatal;
  std::array<u8, 7> reserved{};
  u64 pin_cookie = 0;
};

struct BlobReceiverLifetimePinOpsV3Generation1 {
  void* stable_host_context = nullptr;
  BlobReceiverPinCloneResultV3 (*acquire_pin)(void*) noexcept = nullptr;
  BlobReceiverPinCloneResultV3 (*clone_pin)(void*, u64) noexcept = nullptr;
  void (*drop_pin)(void*, u64) noexcept = nullptr;
};

enum class BlobLifetimeResourceV3 : u8 {
  lifetime_callback_calls = 1,
  logical_bytes_read = 2,
  encoded_bytes_read = 3,
  sink_bytes_written = 4,
  reader_calls = 5,
  sink_calls = 6,
  provider_calls = 7,
  temporary_bytes = 8,
  expanded_bytes = 9,
  backpressure_retries = 10,
  backpressure_wait_ns = 11,
};

struct BlobLifetimeCounterV3 {
  u64 limit = 0;
  u64 invoked = 0;
  u64 reserved_cleanup = 0;
};

struct BlobCleanupReservationConsumptionV3 {
  bool obligation_consumed = false;
  bool cleanup_callback_charge_commit = false;
  bool ledger_terminal_quarantined = false;
};

struct BlobLifetimeBudgetConfigurationV3Generation1 {
  std::array<u64, 11> limits{};
  std::span<byte> scratch{};
  u64 admitted_window_octets = 0;
  u64 admitted_relative_timeout_ns = 0;
};

struct BlobLifetimeBudgetSnapshotV3Generation1 {
  bool configured = false;
  bool frozen = false;
  bool terminal_quarantined = false;
  std::array<BlobLifetimeCounterV3, 12> counters{};
  byte* scratch_data = nullptr;
  u64 scratch_size = 0;
  u64 admitted_window_octets = 0;
  u64 admitted_relative_timeout_ns = 0;
};

class BlobLifetimeReceiverHostV3Generation1;

class BlobLifetimeBudgetLedgerV3Generation1 final {
 public:
  BlobLifetimeBudgetLedgerV3Generation1() noexcept = default;
  BlobLifetimeBudgetLedgerV3Generation1(
      const BlobLifetimeBudgetLedgerV3Generation1&) = delete;
  BlobLifetimeBudgetLedgerV3Generation1& operator=(
      const BlobLifetimeBudgetLedgerV3Generation1&) = delete;

 private:
  friend class BlobRetainedLifetimeLeaseV3;
  friend class BlobLifetimeReceiverHostV3Generation1;
  friend class BlobLifetimeRuntimeFactoryV3Generation1;
  friend class BlobLifetimeRuntimeConformanceAccessV3;
  bool Configure(
      const BlobLifetimeBudgetConfigurationV3Generation1& configuration)
      noexcept;
  bool Freeze() noexcept;
  bool frozen() const noexcept {
    return frozen_.load(std::memory_order_acquire);
  }
  BlobLifetimeBudgetSnapshotV3Generation1 SnapshotForConformance() noexcept;
  bool ReserveOrdinaryAndCleanup(u64 ordinary, u64 cleanup,
                                 u64* available_before) noexcept;
  bool Charge(BlobLifetimeResourceV3 resource, u64 amount,
              u64* available_before) noexcept;
  bool ChargeOrdinary(u64 amount, u64* available_before) noexcept;
  BlobCleanupReservationConsumptionV3 ConsumeCleanup(u64 amount) noexcept;
  bool CancelCleanup(u64 amount) noexcept;
  bool terminal_quarantined() const noexcept {
    return terminal_quarantined_.load(std::memory_order_acquire);
  }
  std::atomic_flag lock_ = ATOMIC_FLAG_INIT;
  std::atomic<bool> configured_{false};
  std::atomic<bool> frozen_{false};
  std::atomic<bool> terminal_quarantined_{false};
  std::array<BlobLifetimeCounterV3, 12> counters_{};
  std::span<byte> scratch_{};
  u64 admitted_window_octets_ = 0;
  u64 admitted_relative_timeout_ns_ = 0;
};

class BlobLifetimeBudgetControlCapabilityV3Generation1 final {
 public:
  BlobLifetimeBudgetControlCapabilityV3Generation1(
      const BlobLifetimeBudgetControlCapabilityV3Generation1&) = delete;
  BlobLifetimeBudgetControlCapabilityV3Generation1& operator=(
      const BlobLifetimeBudgetControlCapabilityV3Generation1&) = delete;
 private:
  friend class BlobLifetimeReceiverHostV3Generation1;
  friend class BlobLifetimeRuntimeFactoryV3Generation1;
  BlobLifetimeBudgetControlCapabilityV3Generation1(
      BlobLifetimeBudgetLedgerV3Generation1& ledger,
      const BlobReceiverLifetimePinOpsV3Generation1& pin_ops) noexcept
      : ledger_(&ledger), pin_ops_(pin_ops) {}
  BlobLifetimeBudgetLedgerV3Generation1* ledger_ = nullptr;
  BlobReceiverLifetimePinOpsV3Generation1 pin_ops_{};
};

class BlobLifetimeAuthorityAdmissionV3Generation1 final {
 public:
  BlobLifetimeAuthorityAdmissionV3Generation1() noexcept = default;
  BlobLifetimeAuthorityAdmissionV3Generation1(
      const BlobLifetimeAuthorityAdmissionV3Generation1&) = delete;
  BlobLifetimeAuthorityAdmissionV3Generation1& operator=(
      const BlobLifetimeAuthorityAdmissionV3Generation1&) = delete;
  BlobLifetimeAuthorityAdmissionV3Generation1(
      BlobLifetimeAuthorityAdmissionV3Generation1&&) = delete;
  BlobLifetimeAuthorityAdmissionV3Generation1& operator=(
      BlobLifetimeAuthorityAdmissionV3Generation1&&) = delete;
  ~BlobLifetimeAuthorityAdmissionV3Generation1() noexcept;

 private:
  friend class BlobRetainedLifetimeLeaseV3;
  friend class BlobLifetimeRuntimeFactoryV3Generation1;
  friend class BlobLifetimeRuntimeConformanceAccessV3;
  friend struct BlobLeaseConstructionResultV3;

  BlobLifetimeAuthorityAdmissionV3Generation1(
      const BlobLifetimeAuthorityV3& authority,
      const BaseBlobLifetimeReceiverServicesV3Generation1& services,
      const BlobReceiverLifetimePinOpsV3Generation1& pin_ops,
      u64 pin_cookie,
      const BlobReceiverLifetimePinOpsV3Generation1& budget_pin_ops,
      u64 budget_pin_cookie, BlobLifetimeBudgetLedgerV3Generation1* budget,
      BlobMonotonicClockBindingV3 token_clock,
      BlobMonotonicClockBindingV3 request_clock) noexcept
      : authority_(authority), services_(services), pin_ops_(pin_ops),
        pin_cookie_(pin_cookie), budget_pin_ops_(budget_pin_ops),
        budget_pin_cookie_(budget_pin_cookie), budget_(budget),
        token_clock_(token_clock),
        request_clock_(request_clock), admitted_(true) {}

  void DropPin() noexcept;
  void TransferFrom(BlobLifetimeAuthorityAdmissionV3Generation1& source)
      noexcept;

  BlobLifetimeAuthorityV3 authority_{};
  BaseBlobLifetimeReceiverServicesV3Generation1 services_{};
  BlobReceiverLifetimePinOpsV3Generation1 pin_ops_{};
  u64 pin_cookie_ = 0;
  BlobReceiverLifetimePinOpsV3Generation1 budget_pin_ops_{};
  u64 budget_pin_cookie_ = 0;
  BlobLifetimeBudgetLedgerV3Generation1* budget_ = nullptr;
  BlobMonotonicClockBindingV3 token_clock_{};
  BlobMonotonicClockBindingV3 request_clock_{};
  bool receiver_reclamation_pin_established_ = false;
  bool admitted_ = false;
};

struct BlobMaterializedCarrierBindingV3 {
  platform::Uuid authority_instance_uuid{};
  u64 authority_instance_generation = 0;
  platform::Uuid carrier_uuid{};
  u64 carrier_generation = 0;
  platform::Uuid lifetime_token_uuid{};
  u64 lifetime_token_generation = 0;
  u64 immutable_binding_generation = 0;
  u8 carrier_kind = SB_BLOB_MODE_MATERIALIZED_BYTES_V3;
};

// Implemented by the receiver owner in the stable host. Its exact class name
// is the only construction authority for the unforgeable carrier capability.
class BlobReceiverMaterializedCapabilityV3Generation1 final {
 public:
  BlobReceiverMaterializedCapabilityV3Generation1(
      const BlobReceiverMaterializedCapabilityV3Generation1&) = delete;
  BlobReceiverMaterializedCapabilityV3Generation1& operator=(
      const BlobReceiverMaterializedCapabilityV3Generation1&) = delete;
  BlobReceiverMaterializedCapabilityV3Generation1(
      BlobReceiverMaterializedCapabilityV3Generation1&&) = delete;
  BlobReceiverMaterializedCapabilityV3Generation1& operator=(
      BlobReceiverMaterializedCapabilityV3Generation1&&) = delete;
 private:
  friend class BlobLifetimeReceiverHostV3Generation1;
  friend class BlobLifetimeRuntimeFactoryV3Generation1;
  BlobReceiverMaterializedCapabilityV3Generation1(
      const BlobMaterializedCarrierBindingV3& binding,
      BlobValueStateV3 state, const byte* data, u64 logical_length) noexcept
      : binding_(binding), state_(state), data_(data),
        logical_length_(logical_length) {}
  BlobMaterializedCarrierBindingV3 binding_{};
  BlobValueStateV3 state_ = BlobValueStateV3::value;
  const byte* data_ = nullptr;
  u64 logical_length_ = 0;
};

class BlobBoundMaterializedCarrierV3Generation1 final {
 public:
  BlobBoundMaterializedCarrierV3Generation1(
      const BlobBoundMaterializedCarrierV3Generation1&) = delete;
  BlobBoundMaterializedCarrierV3Generation1& operator=(
      const BlobBoundMaterializedCarrierV3Generation1&) = delete;
  BlobBoundMaterializedCarrierV3Generation1(
      BlobBoundMaterializedCarrierV3Generation1&&) = delete;
  BlobBoundMaterializedCarrierV3Generation1& operator=(
      BlobBoundMaterializedCarrierV3Generation1&&) = delete;

  BlobBoundMaterializedCarrierV3Generation1() noexcept = default;
  BlobValueStateV3 state() const noexcept { return state_; }
  u64 logical_length() const noexcept { return logical_length_; }

 private:
  friend class BlobRetainedLifetimeLeaseV3;
  friend class BlobLifetimeRuntimeFactoryV3Generation1;
  friend class BlobLifetimeRuntimeConformanceAccessV3;
  BlobBoundMaterializedCarrierV3Generation1(
      const BlobMaterializedCarrierBindingV3& binding,
      BlobValueStateV3 state, const byte* data, u64 logical_length) noexcept
      : binding_(binding), state_(state), data_(data),
        logical_length_(logical_length) {}
  BlobMaterializedCarrierBindingV3 binding_{};
  BlobValueStateV3 state_ = BlobValueStateV3::value;
  const byte* data_ = nullptr;
  u64 logical_length_ = 0;
};

enum class BlobLifetimeFactClassV3 : u8 {
  none = 0,
  security = 1,
  cancellation = 2,
  snapshot = 3,
  binding = 4,
  lifetime_authority_unavailable = 5,
  callback_protocol = 6,
  timeout = 7,
  resource_budget = 8,
  io = 9,
  integrity = 10,
  // A typed cleanup callback refusal has callback-protocol precedence, but
  // remains a distinct closed fact class for exact diagnostic materialization.
  cleanup_protocol = 11,
};

enum class BlobLifetimeDiagnosticCodeV3 : u8 {
  none = 0,
  security_access_denied,
  process_cancelled,
  handle_expired,
  descriptor_invalid,
  handle_owner_mismatch,
  handle_mode_refused,
  lifetime_authority_unavailable,
  stream_backpressure_exhausted,
  resource_budget_exceeded,
  io_failed,
  integrity_failed,
  state_invalid,
  length_exceeded,
};

enum class BlobLifetimeReasonV3 : u8 {
  none = 0,
  authority_table_missing = 1,
  authority_abi_mismatch = 2,
  authority_profile_binding_mismatch = 3,
  token_struct_invalid = 4,
  token_identity_invalid = 5,
  wrong_authority_instance = 6,
  owner_binding_mismatch = 7,
  transaction_mismatch = 8,
  access_ticket_invalid = 9,
  access_ticket_consumed = 10,
  callback_result_unknown = 11,
  callback_output_impossible = 12,
  callback_reentrant = 13,
  release_failed = 14,
  same_ticket_concurrent_use = 15,
  absolute_deadline_expired = 16,
  reader_io_failure = 17,
  sink_io_failure = 18,
  provider_io_failure = 19,
  adapter_exception_contained = 20,
  end_access_failed = 21,
  authority_runtime_unavailable = 22,
  monotonic_clock_unavailable = 23,
  receiver_services_failed = 24,
  shutdown_drain_violation = 25,
  authority_capacity_exhausted = 26,
  visitor_missing = 27,
  clone_alias = 28,
  destination_armed = 29,
};

enum BlobLifetimeDiagnosticParameterBitsV3 : u32 {
  blob_parameter_operation = UINT32_C(1) << 0,
  blob_parameter_phase = UINT32_C(1) << 1,
  blob_parameter_resource = UINT32_C(1) << 2,
  blob_parameter_required = UINT32_C(1) << 3,
  blob_parameter_available = UINT32_C(1) << 4,
  blob_parameter_owner_class = UINT32_C(1) << 5,
  blob_parameter_open_mode = UINT32_C(1) << 6,
  blob_parameter_descriptor_identity = UINT32_C(1) << 7,
  blob_parameter_supplied_state = UINT32_C(1) << 8,
  blob_parameter_actual_length = UINT32_C(1) << 9,
  blob_parameter_maximum_length = UINT32_C(1) << 10,
  blob_parameter_window_octets = UINT32_C(1) << 11,
  blob_parameter_timeout_milliseconds = UINT32_C(1) << 12,
  blob_parameter_lifetime = UINT32_C(1) << 13,
  blob_parameter_expiry_reason = UINT32_C(1) << 14,
  blob_parameter_format = UINT32_C(1) << 15,
  blob_parameter_adapter_kind = UINT32_C(1) << 16,
};

enum class BlobLifetimeDiagnosticLifetimeV3 : u8 {
  none = 0,
  unknown_token = 1,
  wrong_snapshot = 2,
  stale_snapshot = 3,
  token_expired = 4,
  token_revoked = 5,
  owner_closed = 6,
  retain_ticket_invalid = 7,
  retain_ticket_consumed = 8,
  moved_binding = 9,
};

enum class BlobLifetimeDiagnosticExpiryReasonV3 : u8 {
  none = 0,
  unknown_token = 1,
  snapshot_identity_mismatch = 2,
  snapshot_no_longer_current = 3,
  monotonic_expiry = 4,
  explicit_revocation = 5,
  owner_closed = 6,
  retain_ticket_unknown = 7,
  retain_ticket_consumed = 8,
  moved_binding_generation = 9,
};

struct BlobLifetimeDiagnosticParametersV3 {
  u32 present = 0;
  u8 operation_enum = 0;
  BlobLifetimePublicPhaseV3 phase_enum =
      static_cast<BlobLifetimePublicPhaseV3>(0);
  BlobLifetimeResourceV3 resource_enum =
      static_cast<BlobLifetimeResourceV3>(0);
  u8 owner_class_enum = 0;
  u8 open_mode_enum = 0;
  u8 supplied_state_u8 = 0;
  u8 format_enum = 0;
  u8 adapter_kind_enum = 0;
  BlobLifetimeDiagnosticLifetimeV3 lifetime_enum =
      BlobLifetimeDiagnosticLifetimeV3::none;
  BlobLifetimeDiagnosticExpiryReasonV3 expiry_reason_enum =
      BlobLifetimeDiagnosticExpiryReasonV3::none;
  u64 required_u64 = 0;
  u64 available_u64 = 0;
  u64 actual_length_u64 = 0;
  u64 maximum_length_u64 = 0;
  u64 window_octets_u64 = 0;
  u64 timeout_milliseconds_u64 = 0;
  platform::Uuid descriptor_uuid{};
  u64 descriptor_generation = 0;
};

enum class BlobLifetimeAuthoritySourceV3 : u8 {
  none = 0,
  authority_table_missing = 1,
  retain_callback_missing = 2,
  probe_callback_missing = 3,
  begin_access_callback_missing = 4,
  end_access_callback_missing = 5,
  release_callback_missing = 6,
  receiver_services_object_missing = 7,
  read_monotonic_ns_callable_missing = 8,
  sample_cancellation_callable_missing = 9,
  record_invariant_and_quarantine_callable_missing = 10,
  pin_host_failure = 11,
  admission_closed_or_drain = 12,
  monotonic_clock_unavailable = 13,
  cancellation_sampler_unavailable = 14,
  callback_code_17 = 15,
  callback_code_18 = 16,
  datatype_local_state = 17,
};

enum class BlobLifetimeFactStageV3 : u8 {
  factory_admission = 1,
  ordinary_gate = 2,
  callback_return = 3,
  cleanup_return = 4,
  local_precondition = 5,
};

struct BlobLifetimeTypedFactV3 {
  BlobLifetimeFactClassV3 fact_class = BlobLifetimeFactClassV3::none;
  BlobLifetimeDiagnosticCodeV3 diagnostic =
      BlobLifetimeDiagnosticCodeV3::none;
  BlobLifetimeReasonV3 reason = BlobLifetimeReasonV3::none;
  BlobLifetimeAuthoritySourceV3 authority_source =
      BlobLifetimeAuthoritySourceV3::none;
  BlobLifetimeFactStageV3 stage =
      BlobLifetimeFactStageV3::factory_admission;
  bool gate_present = false;
  BlobLifetimeGateV3 gate = static_cast<BlobLifetimeGateV3>(0);
  platform::Uuid diagnostic_uuid{};
  u64 diagnostic_generation = 0;
  BlobLifetimeDiagnosticParametersV3 parameters{};
};

struct BlobProtectedWorkResultV3 {
  u8 status = 1;  // 0 success, 1 typed terminal failure
  std::array<u8, 7> reserved{};
  BlobLifetimeTypedFactV3 fact{};
};

class BlobTrustedInternalVisitorV3 final {
 public:
  BlobTrustedInternalVisitorV3(const BlobTrustedInternalVisitorV3&) = delete;
  BlobTrustedInternalVisitorV3& operator=(
      const BlobTrustedInternalVisitorV3&) = delete;
  BlobTrustedInternalVisitorV3(BlobTrustedInternalVisitorV3&&) = delete;
  BlobTrustedInternalVisitorV3& operator=(
      BlobTrustedInternalVisitorV3&&) = delete;

 private:
  friend class BlobRetainedLifetimeLeaseV3;
  friend class BlobLifetimeRuntimeConformanceAccessV3;
  friend class BlobLifetimeDatatypeConsumerAccessV3Generation1;
  using Function = BlobProtectedWorkResultV3 (*)(void*,
                                                 std::span<const byte>);
  BlobTrustedInternalVisitorV3(Function function, void* context) noexcept
      : function_(function), context_(context) {}
  Function function_ = nullptr;
  void* context_ = nullptr;
};

class BlobLifetimeFactAccumulatorV3 final {
 public:
  void Observe(const BlobLifetimeTypedFactV3& fact) noexcept;
  BlobLifetimeTypedFactV3 Selected() const noexcept;
  bool empty() const noexcept { return observed_mask_ == 0; }
 private:
  std::array<BlobLifetimeTypedFactV3, 10> first_by_rank_{};
  u16 observed_mask_ = 0;
};

struct BlobLifetimeRuntimeResultV3 {
  BlobLifetimeOuterDispositionV3 disposition =
      BlobLifetimeOuterDispositionV3::public_failure;
  platform::Status status{};
  std::string_view diagnostic_code{};
  std::string_view reason{};
  platform::Uuid diagnostic_uuid{};
  u64 diagnostic_generation = 0;
  u8 callback_code = 255;
  BlobLifetimeResourceV3 resource =
      BlobLifetimeResourceV3::lifetime_callback_calls;
  u64 required = 0;
  u64 available = 0;
  BlobLifetimeTypedFactV3 fact{};
  bool ok() const noexcept {
    return disposition == BlobLifetimeOuterDispositionV3::success;
  }
};

class BlobLifetimeRuntimeFactoryV3Generation1 final {
 public:
  static BlobLifetimeRuntimeResultV3 AdmitMaterialized(
      // Trusted caller-owned scalar, never fetched from an unpinned request.
      u8 trusted_operation,
      const BlobValidatedProfileHandleV3& profile,
      const BlobLifetimeAuthorityV3* authority,
      const BaseBlobLifetimeReceiverServicesV3Generation1* services,
      const BlobReceiverLifetimePinOpsV3Generation1& pin_ops,
      const BlobLifetimeBudgetControlCapabilityV3Generation1& budget_control,
      BlobMonotonicClockBindingV3 token_clock,
      BlobMonotonicClockBindingV3 request_clock,
      const BlobLifetimeTokenV3& token,
      const BlobLifetimeUseRequestV3& request,
      const BlobReceiverMaterializedCapabilityV3Generation1& capability,
      BlobLifetimeAuthorityAdmissionV3Generation1& out_admission,
      BlobBoundMaterializedCarrierV3Generation1& out_carrier) noexcept;
};

class BlobRetainedLifetimeLeaseV3;
struct BlobLeaseConstructionResultV3;

class BlobRetainedLifetimeLeaseV3 final {
 public:
  BlobRetainedLifetimeLeaseV3() noexcept = default;
  BlobRetainedLifetimeLeaseV3(const BlobRetainedLifetimeLeaseV3&) = delete;
  BlobRetainedLifetimeLeaseV3& operator=(
      const BlobRetainedLifetimeLeaseV3&) = delete;
  BlobRetainedLifetimeLeaseV3(BlobRetainedLifetimeLeaseV3&&) = delete;
  BlobRetainedLifetimeLeaseV3& operator=(
      BlobRetainedLifetimeLeaseV3&&) = delete;
  ~BlobRetainedLifetimeLeaseV3() noexcept;

  bool retained() const noexcept;
  static BlobLeaseConstructionResultV3 MoveConstructFrom(
      BlobRetainedLifetimeLeaseV3&& source) noexcept;
  BlobLifetimeRuntimeResultV3 MoveReplaceFrom(
      BlobRetainedLifetimeLeaseV3&& source) noexcept;
  BlobLifetimeRuntimeResultV3 CloneInto(
      BlobRetainedLifetimeLeaseV3& destination) noexcept;
  BlobLifetimeRuntimeResultV3 Probe() noexcept;
  BlobLifetimeRuntimeResultV3 VisitBoundMaterializedBytes(
      const BlobTrustedInternalVisitorV3& visitor) noexcept;
  BlobLifetimeRuntimeResultV3 Release() noexcept;

 private:
  friend class BlobLifetimeRuntimeConformanceAccessV3;
  friend struct BlobLeaseConstructionResultV3;
  friend BlobLifetimeRuntimeResultV3
  RetainBaseBlobLifetimeLeaseV3Generation1(
      const BlobValidatedProfileHandleV3&,
      BlobLifetimeAuthorityAdmissionV3Generation1&&,
      const BlobBoundMaterializedCarrierV3Generation1&,
      const BlobLifetimeTokenV3&, const BlobLifetimeUseRequestV3&,
      BlobRetainedLifetimeLeaseV3&) noexcept;

  enum class State : u8 {
    disarmed = 0,
    retaining,
    idle,
    callback_active,
    access_active,
    moving,
    releasing,
    released,
    terminal,
    moved_from,
    installing,
  };

  // One linearization word owns the public-operation state, the close bit,
  // the first immutable deferred invariant tuple and its receipt, and the
  // bounded entrant count which pins wrapper metadata. A publisher can win
  // before the owner's final CAS or observe a final state; finalization also
  // waits for every losing entrant, so neither evidence nor metadata can be
  // cleared/transferred across an admitted reader.
  class AtomicControl final {
   public:
    enum class GenerationResetStatus : u8 {
      installed = 0,
      wrong_state = 1,
      contended = 2,
      corrupt_protocol = 3,
    };
    struct GenerationResetOutcome final {
      GenerationResetStatus status = GenerationResetStatus::corrupt_protocol;
      State observed_state = State::terminal;
    };

    State load(std::memory_order order) const noexcept;
    void store(State desired, std::memory_order order) noexcept;
    bool compare_exchange_strong(State& expected, State desired,
                                 std::memory_order success) noexcept;
    bool Publish(u64 packed) noexcept;
    u64 Deferred() const noexcept;
    bool BeginClosing() noexcept;
    u64 PeekDeferredWhileClosing() const noexcept;
    bool CompleteDeferredServiceReceipt(u64 packed) noexcept;
    bool TryFinalize(State final_state, u32 owner_entrants) noexcept;
    void WaitForClosingQuiescence(u32 owner_entrants) const noexcept;
    GenerationResetOutcome ResetGeneration(
        State expected_predecessor, State installed_state) noexcept;
    GenerationResetOutcome ClaimIdleForTransfer() noexcept;
    bool TryEnter(bool allow_reusable_predecessor = false) noexcept;
    void Leave() noexcept;
    u32 entrants() const noexcept;
    bool closing() const noexcept;
   private:
    std::atomic<u64> word_{0};
  };

  class EntrantGuard final {
   public:
    explicit EntrantGuard(AtomicControl& control,
                          bool acquire = true,
                          bool allow_reusable_predecessor = false) noexcept
        : control_(acquire ? &control : nullptr),
          acquired_(!acquire ||
                    control.TryEnter(allow_reusable_predecessor)) {}
    ~EntrantGuard() noexcept {
      Release();
    }
    void Release() noexcept {
      if (control_ != nullptr && acquired_) control_->Leave();
      control_ = nullptr;
      acquired_ = false;
    }
    EntrantGuard(const EntrantGuard&) = delete;
    EntrantGuard& operator=(const EntrantGuard&) = delete;
    bool acquired() const noexcept { return acquired_; }
   private:
    AtomicControl* control_ = nullptr;
    bool acquired_ = false;
  };

  BlobLifetimeRuntimeResultV3 AdoptFrom(
      BlobRetainedLifetimeLeaseV3& source,
      bool source_already_guarded_moving = false,
      u8 invariant_operation = 16,
      bool destination_entrant_already_held = false,
      State reserved_destination_predecessor = State::terminal) noexcept;
  BlobLifetimeRuntimeResultV3 RetainFrom(
      const BlobValidatedProfileHandleV3& profile,
      BlobLifetimeAuthorityAdmissionV3Generation1& admission,
      const BlobBoundMaterializedCarrierV3Generation1& carrier,
      const BlobLifetimeTokenV3& token,
      const BlobLifetimeUseRequestV3& request,
      u8 invariant_operation,
      bool entrant_already_held = false,
      bool defer_idle_publication = false,
      bool destination_already_installing = false) noexcept;
  BlobLifetimeRuntimeResultV3 ProbeInternal(
      bool final_probe, bool owner_already_active = false) noexcept;
  BlobLifetimeRuntimeResultV3 EndAccessInternal(
      BlobLifetimeAccessTicketV3 access,
      BlobLifetimeRuntimeResultV3 primary) noexcept;
  BlobLifetimeRuntimeResultV3 ReleaseInternal(
      BlobLifetimePhaseV3 callback_phase,
      u8 invariant_operation,
      BlobLifetimeRuntimeResultV3 prior_primary = {},
      bool owner_already_active = false) noexcept;
  BlobLifetimeRuntimeResultV3 ReleaseOwned(
      BlobLifetimePhaseV3 callback_phase, u8 invariant_operation,
      BlobLifetimeRuntimeResultV3 prior_primary = {}) noexcept;
  BlobLifetimeRuntimeResultV3 ReportInvariant(
      BlobLifetimeInvariantEventV3 event,
      BlobLifetimePhaseV3 phase,
      u8 callback_code_or_255 = 255,
      u8 operation_override_or_zero = 0,
      u32 owner_entrants = 1) noexcept;
  static BlobLifetimeRuntimeResultV3 BuildFailureForConformance(
      std::string_view diagnostic_code,
      std::string_view reason_selector) noexcept;
  BlobLifetimeRuntimeResultV3 OrdinaryGate(
      BlobLifetimeGateV3 gate,
      BlobLifetimePublicPhaseV3 public_phase,
      BlobLifetimePhaseV3 invariant_phase,
      u8 invariant_operation_override_or_zero = 0) noexcept;
  BlobLifetimeRuntimeResultV3 RejectOverlappingUse(
      BlobLifetimePhaseV3 attempted_phase,
      u8 operation_override_or_zero = 0) noexcept;
  void ZeroTicket() noexcept;
  void PrepareInvariantLocalCleanup() noexcept;
  BlobLifetimeRuntimeResultV3 LocalStateFailure(
      State observed, bool destination_context = false) const noexcept;
  BlobLifetimeRuntimeResultV3 GenerationResetFailure(
      const AtomicControl::GenerationResetOutcome& outcome,
      bool destination_context) const noexcept;
  bool PublishDeferredInvariant(BlobLifetimeInvariantEventV3 event,
                                u8 operation,
                                BlobLifetimePhaseV3 phase,
                                u8 callback_code_or_255) noexcept;
  static bool PublishDeferredThunk(void* context,
                                   BlobLifetimeInvariantEventV3 event,
                                   u8 operation,
                                   BlobLifetimePhaseV3 phase,
                                   u8 callback_code_or_255) noexcept;

  BlobLifetimeAuthorityAdmissionV3Generation1 admission_{};
  BlobMaterializedCarrierBindingV3 carrier_binding_{};
  BlobValueStateV3 carrier_state_ = BlobValueStateV3::value;
  const byte* carrier_data_ = nullptr;
  u64 carrier_logical_length_ = 0;
  BlobLifetimeTokenV3 token_{};
  BlobLifetimeUseRequestV3 request_{};
  platform::Uuid descriptor_uuid_{};
  u64 descriptor_generation_ = 0;
  BlobLifetimeRetainTicketV3 retain_ticket_{};
  AtomicControl state_{};
  std::atomic<bool> callback_active_{false};
  std::atomic<bool> quarantined_{false};
  u64 prior_monotonic_ns_ = 0;
  bool has_prior_monotonic_ns_ = false;
  bool suppress_publication_gate_ = false;
};

struct BlobLeaseConstructionResultV3 final {
  BlobLeaseConstructionResultV3(const BlobLeaseConstructionResultV3&) = delete;
  BlobLeaseConstructionResultV3& operator=(
      const BlobLeaseConstructionResultV3&) = delete;
  BlobLeaseConstructionResultV3(BlobLeaseConstructionResultV3&&) = delete;
  BlobLeaseConstructionResultV3& operator=(
      BlobLeaseConstructionResultV3&&) = delete;

  BlobLifetimeRuntimeResultV3 result;
  BlobRetainedLifetimeLeaseV3 value;
  BlobRetainedLifetimeLeaseV3* lease() noexcept {
    return result.ok() ? &value : nullptr;
  }
  const BlobRetainedLifetimeLeaseV3* lease() const noexcept {
    return result.ok() ? &value : nullptr;
  }

 private:
  friend class BlobRetainedLifetimeLeaseV3;
  explicit BlobLeaseConstructionResultV3(
      BlobLifetimeRuntimeResultV3 result_value) noexcept
      : result(result_value) {}
  BlobLeaseConstructionResultV3(
      BlobLifetimeRuntimeResultV3 result_value,
      BlobRetainedLifetimeLeaseV3& source,
      bool source_already_guarded_moving = false) noexcept;
};

static_assert(sizeof(BlobReceiverEffectResultV3) == 8);
static_assert(std::is_trivially_copyable_v<BlobReceiverEffectResultV3>);
static_assert(!std::is_copy_constructible_v<BlobRetainedLifetimeLeaseV3>);
static_assert(!std::is_move_constructible_v<BlobRetainedLifetimeLeaseV3>);
static_assert(!std::is_copy_assignable_v<BlobRetainedLifetimeLeaseV3>);
static_assert(!std::is_move_assignable_v<BlobRetainedLifetimeLeaseV3>);
static_assert(!std::is_copy_constructible_v<
              BlobBoundMaterializedCarrierV3Generation1>);
static_assert(std::is_trivially_copyable_v<BlobTypeCodecIdentitySnapshotV3>);
static_assert(std::is_trivially_copyable_v<BlobValidatedProfileHandleV3>);
static_assert(!std::is_copy_constructible_v<
              BlobReceiverMaterializedCapabilityV3Generation1>);
static_assert(!std::is_move_constructible_v<
              BlobReceiverMaterializedCapabilityV3Generation1>);
static_assert(!std::is_copy_constructible_v<
              BlobLifetimeBudgetControlCapabilityV3Generation1>);
static_assert(!std::is_move_constructible_v<
              BlobBoundMaterializedCarrierV3Generation1>);

BlobLifetimeRuntimeResultV3 RetainBaseBlobLifetimeLeaseV3Generation1(
    const BlobValidatedProfileHandleV3& profile,
    BlobLifetimeAuthorityAdmissionV3Generation1&& trusted_admission,
    const BlobBoundMaterializedCarrierV3Generation1& carrier,
    const BlobLifetimeTokenV3& token,
    const BlobLifetimeUseRequestV3& request,
    BlobRetainedLifetimeLeaseV3& destination) noexcept;

BlobLifetimeRuntimeResultV3 ConsumeBlobMaterializedValueScopedV3Generation1(
    const BlobValidatedProfileHandleV3& profile,
    const BlobBoundMaterializedCarrierV3Generation1& carrier,
    BlobLifetimeAuthorityAdmissionV3Generation1&& trusted_admission,
    const BlobLifetimeTokenV3& token,
    const BlobLifetimeUseRequestV3& request,
    const BlobTrustedInternalVisitorV3& visitor,
    bool null_allowed) noexcept;

}  // namespace scratchbird::core::datatypes
