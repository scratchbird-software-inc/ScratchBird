// Copyright (c) 2026 ScratchBird Software Inc.
// SPDX-License-Identifier: MPL-2.0

#include "datatype_blob.hpp"

#include <algorithm>
#include <array>
#include <bit>
#include <functional>
#include <iterator>
#include <utility>

namespace scratchbird::core::datatypes {
namespace {

using platform::Severity;
using platform::Status;
using platform::StatusCode;
using platform::Subsystem;

constexpr platform::Uuid U(std::array<byte, 16> bytes) noexcept {
  return platform::Uuid{bytes};
}

constexpr DatatypePolicyIdentityV3 P(std::array<byte, 16> bytes) noexcept {
  return {U(bytes), 1};
}

constexpr u64 MaximumMaterializedSpanForAddressSpace(u64 size_max) noexcept {
  return std::min(kBlobMaximumLogicalBytesV3, size_max);
}

static_assert(MaximumMaterializedSpanForAddressSpace(UINT32_MAX) ==
              UINT32_MAX);
static_assert(MaximumMaterializedSpanForAddressSpace(UINT64_MAX) ==
              kBlobMaximumLogicalBytesV3);

inline constexpr std::array<DatatypePolicyIdentityV3,
                            kBlobPolicyBindingCountV3>
    kExpectedPolicies{{
        P({0x01,0xa1,0x09,0x5f,0xf2,0x05,0x7d,0xaf,
           0xb6,0xf1,0x29,0x17,0x24,0x84,0x0d,0xe4}),
        P({0x01,0xa1,0x09,0x5f,0xf2,0x05,0x7f,0xc9,
           0x8f,0xec,0xe6,0xad,0xa4,0x88,0x9d,0x1c}),
        P({0x01,0xa1,0x09,0x5f,0xf2,0x05,0x79,0x29,
           0x86,0xcf,0x34,0x23,0x5d,0x85,0xac,0xc5}),
        P({0x01,0xa1,0x09,0x5f,0xf2,0x05,0x71,0xf5,
           0x98,0x22,0x14,0x8c,0x23,0x85,0xb0,0x00}),
        P({0x01,0xa1,0x09,0x5f,0xf2,0x05,0x7b,0x9a,
           0xa7,0x17,0x68,0x01,0xdc,0xf9,0x9d,0xad}),
        P({0x01,0xa1,0x09,0x5f,0xf2,0x05,0x70,0x92,
           0xb6,0xce,0x7c,0x4f,0xe3,0x5a,0xa2,0xbe}),
        P({0x01,0xa1,0x09,0x5f,0xf2,0x05,0x75,0x4c,
           0x8d,0xe7,0xca,0xcf,0x91,0xaf,0xb2,0x58}),
        P({0x01,0xa1,0x09,0x5f,0xf2,0x05,0x77,0x0d,
           0xb5,0x0b,0x16,0x42,0x49,0x90,0xfe,0x25}),
        P({0x01,0xa1,0x09,0x5f,0xf2,0x05,0x71,0x84,
           0xad,0x9c,0x9f,0x89,0x17,0x61,0x84,0xc4}),
        P({0x01,0xa1,0x09,0x5f,0xf2,0x05,0x77,0xf1,
           0xaa,0xc4,0x0e,0x0b,0xd3,0x9f,0xe3,0x98}),
        P({0x01,0xa1,0x09,0x5f,0xf2,0x05,0x74,0xae,
           0xa0,0xcc,0x24,0xaa,0x66,0x5f,0x50,0x47}),
        P({0x01,0xa1,0x09,0x5f,0xf2,0x05,0x7c,0x23,
           0xa0,0xd0,0x91,0xcc,0xd2,0xa0,0x78,0xa6}),
        P({0x01,0xa1,0x09,0x5f,0xf2,0x05,0x7d,0x40,
           0x9f,0x48,0xd5,0x6d,0x0b,0xcf,0x34,0xb2}),
        P({0x01,0xa1,0x09,0x5f,0xf2,0x05,0x74,0xfb,
           0x9e,0x63,0xaa,0xa3,0x8d,0x70,0xd8,0x90}),
        P({0x01,0xa1,0x09,0x5f,0xf2,0x05,0x74,0x4b,
           0x99,0xd9,0xd8,0xd7,0x2f,0x10,0xe3,0xea}),
        P({0x01,0xa1,0x09,0x5f,0xf2,0x05,0x7e,0xd4,
           0x8d,0x29,0xf2,0x57,0x43,0x16,0x7c,0x32}),
        P({0x01,0xa1,0x09,0x5f,0xf2,0x05,0x71,0x79,
           0xbd,0xa3,0xca,0x90,0xf4,0x29,0x99,0xe1}),
        P({0x01,0xa1,0x09,0x5f,0xf2,0x05,0x76,0x57,
           0x8e,0xd2,0xa5,0x11,0xbb,0xa1,0x7b,0xe6}),
        P({0x01,0xa1,0x09,0x5f,0xf2,0x05,0x7b,0x41,
           0xaf,0xce,0x7b,0x75,0x81,0x8d,0x27,0x30}),
        P({0x01,0xa1,0x09,0x5f,0xf2,0x05,0x74,0x12,
           0x9c,0x35,0x1e,0xf8,0x8f,0xa9,0xeb,0x7f}),
        P({0x01,0xa1,0x09,0x5f,0xf2,0x05,0x79,0x29,
           0xbf,0x02,0x86,0xaa,0x82,0x85,0x0a,0x2c}),
    }};

Status OkStatus() noexcept {
  return {StatusCode::ok, Severity::info, Subsystem::datatypes};
}

Status ErrorStatus() noexcept {
  return {StatusCode::platform_required_feature_missing, Severity::error,
          Subsystem::datatypes};
}

Status ResourceStatus() noexcept {
  return {StatusCode::memory_allocation_failed, Severity::error,
          Subsystem::datatypes};
}

template <typename Result>
Result Success() noexcept {
  Result result;
  result.status = OkStatus();
  result.diagnostic = BlobStructuralDiagnosticV3::none;
  return result;
}

template <typename Result>
Result Failure(BlobStructuralDiagnosticV3 diagnostic,
               Status status = ErrorStatus()) noexcept {
  Result result;
  result.status = status;
  result.diagnostic = diagnostic;
  return result;
}

bool SamePolicy(const DatatypePolicyIdentityV3& left,
                const DatatypePolicyIdentityV3& right) noexcept {
  return left.uuid == right.uuid && left.generation == right.generation;
}

bool ExactReceipt(const BlobAuthorityReceiptV3& receipt) noexcept {
  return receipt.receipt_uuid == kBlobV11ReceiptUuid &&
         receipt.catalog_snapshot_uuid == kDatatypeCohortV11 &&
         receipt.catalog_generation == 11 &&
         receipt.registry_generation == 11;
}

const DatatypeTypeCodecIdentityRowV3* CurrentBlobIdentity() noexcept {
  const DatatypeTypeCodecIdentityRowV3* found = nullptr;
  for (const auto& row : CurrentDatatypeTypeCodecIdentityRowsV3()) {
    if (!IsExactCanonicalBlobTypeCodecIdentityV3(row)) continue;
    if (found != nullptr) return nullptr;
    found = &row;
  }
  return found;
}


BlobTypeCodecIdentitySnapshotV3 SnapshotBlobIdentity(
    const DatatypeTypeCodecIdentityRowV3& row) noexcept {
  const auto& legacy = row.legacy_fields;
  const auto& native = row.native_fields;
  return {
      legacy.catalog_snapshot_uuid,
      legacy.catalog_generation,
      legacy.registry_generation,
      legacy.descriptor_uuid,
      legacy.descriptor_generation,
      legacy.type_uuid,
      legacy.type_generation,
      legacy.codec_uuid,
      legacy.codec_version,
      legacy.codec_generation,
      legacy.datatype_identity_code,
      legacy.null_encoding_code,
      legacy.byte_order_code,
      legacy.representation_code,
      legacy.null_supported,
      legacy.signed_code,
      legacy.empty_value_distinct_from_sql_null,
      legacy.sql_null_requires_zero_payload,
      native.canonical_value_variable_width,
      native.canonical_value_minimum_bytes,
      native.canonical_value_maximum_bytes,
      native.canonical_value_transport_width,
      legacy.canonical_binary_type_code,
      row.descriptor_policy,
      row.canonicalization_policy,
      row.ordering_policy,
      row.hash_policy,
      row.operation_policy,
      native.policy_profile_uuid,
      native.policy_profile_generation,
      native.profile_fingerprint_sha256};
}

bool SameBlobIdentitySnapshot(const BlobTypeCodecIdentitySnapshotV3& left,
                              const BlobTypeCodecIdentitySnapshotV3& right)
    noexcept {
  return left.catalog_snapshot_uuid == right.catalog_snapshot_uuid &&
      left.catalog_generation == right.catalog_generation &&
      left.registry_generation == right.registry_generation &&
      left.descriptor_uuid == right.descriptor_uuid &&
      left.descriptor_generation == right.descriptor_generation &&
      left.type_uuid == right.type_uuid &&
      left.type_generation == right.type_generation &&
      left.codec_uuid == right.codec_uuid &&
      left.codec_version == right.codec_version &&
      left.codec_generation == right.codec_generation &&
      left.datatype_identity_code == right.datatype_identity_code &&
      left.null_encoding_code == right.null_encoding_code &&
      left.byte_order_code == right.byte_order_code &&
      left.representation_code == right.representation_code &&
      left.null_supported == right.null_supported &&
      left.signed_code == right.signed_code &&
      left.empty_value_distinct_from_sql_null ==
          right.empty_value_distinct_from_sql_null &&
      left.sql_null_requires_zero_payload ==
          right.sql_null_requires_zero_payload &&
      left.canonical_value_variable_width ==
          right.canonical_value_variable_width &&
      left.canonical_value_minimum_bytes ==
          right.canonical_value_minimum_bytes &&
      left.canonical_value_maximum_bytes ==
          right.canonical_value_maximum_bytes &&
      left.canonical_value_transport_width ==
          right.canonical_value_transport_width &&
      left.canonical_binary_type_code == right.canonical_binary_type_code &&
      SamePolicy(left.descriptor_policy, right.descriptor_policy) &&
      SamePolicy(left.canonicalization_policy, right.canonicalization_policy) &&
      SamePolicy(left.ordering_policy, right.ordering_policy) &&
      SamePolicy(left.hash_policy, right.hash_policy) &&
      SamePolicy(left.operation_policy, right.operation_policy) &&
      left.policy_profile_uuid == right.policy_profile_uuid &&
      left.policy_profile_generation == right.policy_profile_generation &&
      left.profile_fingerprint_sha256 == right.profile_fingerprint_sha256;
}

bool ExactProfile(const BlobValidatedProfileHandleV3& profile) noexcept {
  const auto* current = CurrentBlobIdentity();
  if (current == nullptr) return false;
  const auto expected_identity = SnapshotBlobIdentity(*current);
  if (!ExactReceipt(profile.receipt) ||
      !SameBlobIdentitySnapshot(profile.identity, expected_identity) ||
      profile.identity.catalog_snapshot_uuid !=
          profile.receipt.catalog_snapshot_uuid ||
      profile.identity.catalog_generation != profile.receipt.catalog_generation ||
      profile.identity.registry_generation !=
          profile.receipt.registry_generation ||
      profile.profile_uuid != kBlobV3ProfileUuid ||
      profile.profile_generation != 1 ||
      profile.profile_fingerprint != kBlobV3ProfileFingerprint ||
      profile.profile_fingerprint_preimage_bytes !=
          kBlobProfileFingerprintPreimageBytesV3) {
    return false;
  }

  for (std::size_t index = 0; index < kExpectedPolicies.size(); ++index) {
    if (!SamePolicy(profile.policy_bindings[index],
                    kExpectedPolicies[index])) {
      return false;
    }
  }

  return SamePolicy(profile.policy_bindings[
                        static_cast<std::size_t>(BlobPolicyKindV3::descriptor)],
                    profile.identity.descriptor_policy) &&
         SamePolicy(profile.policy_bindings[static_cast<std::size_t>(
                        BlobPolicyKindV3::canonicalization)],
                    profile.identity.canonicalization_policy) &&
         SamePolicy(profile.policy_bindings[
                        static_cast<std::size_t>(BlobPolicyKindV3::comparison)],
                    profile.identity.ordering_policy) &&
         SamePolicy(profile.policy_bindings[
                        static_cast<std::size_t>(BlobPolicyKindV3::hash)],
                    profile.identity.hash_policy) &&
         SamePolicy(profile.policy_bindings[
                        static_cast<std::size_t>(BlobPolicyKindV3::operation)],
                    profile.identity.operation_policy) &&
         profile.identity.policy_profile_uuid == profile.profile_uuid &&
         profile.identity.policy_profile_generation ==
             profile.profile_generation &&
         profile.identity.profile_fingerprint_sha256 ==
             profile.profile_fingerprint;
}


}  // namespace

namespace {
inline constexpr u64 kLeaseControlStateMaskV8 = UINT64_C(0x0f);
inline constexpr u64 kLeaseControlClosingV8 = UINT64_C(1) << 4;
inline constexpr unsigned kLeaseControlTupleShiftV8 = 5;
inline constexpr u64 kLeaseControlTupleValueMaskV8 =
    (UINT64_C(1) << 18) - 1;
inline constexpr u64 kLeaseControlTupleMaskV8 =
    kLeaseControlTupleValueMaskV8 << kLeaseControlTupleShiftV8;
inline constexpr u64 kLeaseControlInvariantAcceptedV8 = UINT64_C(1) << 23;
inline constexpr u64 kLeaseControlServiceReceiptV8 = UINT64_C(1) << 24;
inline constexpr unsigned kLeaseControlEntrantsShiftV8 = 25;
inline constexpr u64 kLeaseControlEntrantsValueMaskV8 = UINT64_C(0xffffffff);
inline constexpr u64 kLeaseControlEntrantsMaskV8 =
    kLeaseControlEntrantsValueMaskV8 << kLeaseControlEntrantsShiftV8;
static_assert(UINT8_C(10) <= UINT8_C(0x0f));  // State::installing.
static_assert(kLeaseControlTupleValueMaskV8 < (UINT64_C(1) << 19));
static_assert((UINT64_C(13) | (UINT64_C(31) << 4) |
               (UINT64_C(15) << 9) | (UINT64_C(31) << 13)) <=
              kLeaseControlTupleValueMaskV8);
static_assert(kLeaseControlEntrantsShiftV8 + 32 <= 64);
}

BlobRetainedLifetimeLeaseV3::State
BlobRetainedLifetimeLeaseV3::AtomicControl::load(
    std::memory_order order) const noexcept {
  return static_cast<State>(word_.load(order) & kLeaseControlStateMaskV8);
}

void BlobRetainedLifetimeLeaseV3::AtomicControl::store(
    State desired, std::memory_order) noexcept {
  u64 observed = word_.load(std::memory_order_acquire);
  for (;;) {
    const u64 replacement =
        (observed & ~kLeaseControlStateMaskV8) | static_cast<u64>(desired);
    if (word_.compare_exchange_weak(observed, replacement,
                                    std::memory_order_acq_rel,
                                    std::memory_order_acquire))
      return;
  }
}

bool BlobRetainedLifetimeLeaseV3::AtomicControl::compare_exchange_strong(
    State& expected, State desired, std::memory_order success) noexcept {
  u64 observed = word_.load(std::memory_order_acquire);
  for (;;) {
    const State actual =
        static_cast<State>(observed & kLeaseControlStateMaskV8);
    if (actual != expected) {
      expected = actual;
      return false;
    }
    const u64 replacement =
        (observed & ~kLeaseControlStateMaskV8) | static_cast<u64>(desired);
    if (word_.compare_exchange_weak(observed, replacement, success,
                                    std::memory_order_acquire))
      return true;
  }
}

bool BlobRetainedLifetimeLeaseV3::AtomicControl::Publish(u64 packed) noexcept {
  packed &= kLeaseControlTupleValueMaskV8;
  if (packed == 0) return false;
  u64 observed = word_.load(std::memory_order_acquire);
  for (;;) {
    const State state =
        static_cast<State>(observed & kLeaseControlStateMaskV8);
    const bool owner_open = state == State::retaining ||
        state == State::callback_active || state == State::access_active ||
        state == State::moving || state == State::releasing;
    if (!owner_open ||
        (observed & kLeaseControlInvariantAcceptedV8) != 0)
      return false;
    const u64 replacement = observed |
        kLeaseControlInvariantAcceptedV8 |
        (packed << kLeaseControlTupleShiftV8);
    if (word_.compare_exchange_weak(observed, replacement,
                                    std::memory_order_acq_rel,
                                    std::memory_order_acquire))
      return true;
  }
}

u64 BlobRetainedLifetimeLeaseV3::AtomicControl::Deferred() const noexcept {
  return (word_.load(std::memory_order_acquire) & kLeaseControlTupleMaskV8) >>
      kLeaseControlTupleShiftV8;
}

bool BlobRetainedLifetimeLeaseV3::AtomicControl::BeginClosing() noexcept {
  u64 observed = word_.load(std::memory_order_acquire);
  for (;;) {
    const State state =
        static_cast<State>(observed & kLeaseControlStateMaskV8);
    if (state == State::disarmed || state == State::idle ||
        state == State::released || state == State::terminal ||
        state == State::moved_from || state == State::installing)
      return false;
    if ((observed & kLeaseControlClosingV8) != 0) return true;
    if (word_.compare_exchange_weak(observed,
                                    observed | kLeaseControlClosingV8,
                                    std::memory_order_acq_rel,
                                    std::memory_order_acquire))
      return true;
  }
}

u64 BlobRetainedLifetimeLeaseV3::AtomicControl::PeekDeferredWhileClosing()
    const noexcept {
  const u64 observed = word_.load(std::memory_order_acquire);
  if ((observed & kLeaseControlClosingV8) == 0) return 0;
  return (observed & kLeaseControlTupleMaskV8) >>
      kLeaseControlTupleShiftV8;
}

bool BlobRetainedLifetimeLeaseV3::AtomicControl::
CompleteDeferredServiceReceipt(u64 packed) noexcept {
  packed &= kLeaseControlTupleValueMaskV8;
  u64 observed = word_.load(std::memory_order_acquire);
  for (;;) {
    const u64 observed_packed =
        (observed & kLeaseControlTupleMaskV8) >> kLeaseControlTupleShiftV8;
    if ((observed & kLeaseControlClosingV8) == 0 ||
        (observed & kLeaseControlInvariantAcceptedV8) == 0 ||
        (observed & kLeaseControlServiceReceiptV8) != 0 ||
        observed_packed != packed || packed == 0)
      return false;
    const u64 replacement =
        (observed & ~kLeaseControlTupleMaskV8) |
        kLeaseControlServiceReceiptV8;
    if (word_.compare_exchange_weak(observed, replacement,
                                    std::memory_order_acq_rel,
                                    std::memory_order_acquire))
      return true;
  }
}

bool BlobRetainedLifetimeLeaseV3::AtomicControl::TryFinalize(
    State final_state, u32 owner_entrants) noexcept {
  u64 observed = word_.load(std::memory_order_acquire);
  for (;;) {
    const bool winner =
        (observed & kLeaseControlInvariantAcceptedV8) != 0;
    const bool receipt = (observed & kLeaseControlServiceReceiptV8) != 0;
    if ((observed & kLeaseControlClosingV8) == 0 ||
        (observed & kLeaseControlTupleMaskV8) != 0 ||
        (winner && !receipt) ||
        ((observed & kLeaseControlEntrantsMaskV8) >>
             kLeaseControlEntrantsShiftV8) != owner_entrants)
      return false;
    const u64 replacement = static_cast<u64>(final_state) |
        (observed & (kLeaseControlInvariantAcceptedV8 |
                     kLeaseControlServiceReceiptV8 |
                     kLeaseControlEntrantsMaskV8));
    if (word_.compare_exchange_weak(observed, replacement,
                                    std::memory_order_acq_rel,
                                    std::memory_order_acquire))
      return true;
  }
}

void BlobRetainedLifetimeLeaseV3::AtomicControl::
WaitForClosingQuiescence(u32 owner_entrants) const noexcept {
  for (;;) {
    const u64 observed = word_.load(std::memory_order_acquire);
    if ((observed & kLeaseControlClosingV8) == 0) std::terminate();
    const u64 entrants = (observed & kLeaseControlEntrantsMaskV8) >>
        kLeaseControlEntrantsShiftV8;
    if (entrants < owner_entrants) std::terminate();
    if (entrants == owner_entrants) return;
  }
}

BlobRetainedLifetimeLeaseV3::AtomicControl::GenerationResetOutcome
BlobRetainedLifetimeLeaseV3::AtomicControl::ResetGeneration(
    State expected_predecessor, State installed_state) noexcept {
  u64 observed = word_.load(std::memory_order_acquire);
  for (;;) {
    const State actual =
        static_cast<State>(observed & kLeaseControlStateMaskV8);
    const u64 entrants = (observed & kLeaseControlEntrantsMaskV8) >>
        kLeaseControlEntrantsShiftV8;
    if (actual != expected_predecessor)
      return {GenerationResetStatus::wrong_state, actual};
    const u64 predecessor_protocol_bits = observed &
        ~(kLeaseControlStateMaskV8 | kLeaseControlEntrantsMaskV8);
    if (predecessor_protocol_bits != 0)
      return {GenerationResetStatus::corrupt_protocol, actual};
    if (entrants != 1)
      return {GenerationResetStatus::contended, actual};
    // Legal reusable predecessors are protocol-clean. A new binding
    // generation inherits only already-admitted metadata entrants; any stale
    // closing/winner/tuple/receipt is corruption and is refused, never erased.
    const u64 replacement =
        (observed & kLeaseControlEntrantsMaskV8) |
        static_cast<u64>(installed_state);
    if (word_.compare_exchange_weak(observed, replacement,
                                    std::memory_order_acq_rel,
                                    std::memory_order_acquire))
      return {GenerationResetStatus::installed, installed_state};
  }
}

BlobRetainedLifetimeLeaseV3::AtomicControl::GenerationResetOutcome
BlobRetainedLifetimeLeaseV3::AtomicControl::ClaimIdleForTransfer() noexcept {
  u64 observed = word_.load(std::memory_order_acquire);
  for (;;) {
    const State actual =
        static_cast<State>(observed & kLeaseControlStateMaskV8);
    if (actual != State::idle)
      return {GenerationResetStatus::wrong_state, actual};
    if ((observed & ~(kLeaseControlStateMaskV8 |
                      kLeaseControlEntrantsMaskV8)) != 0)
      return {GenerationResetStatus::corrupt_protocol, actual};
    if ((observed & kLeaseControlEntrantsMaskV8) == 0)
      return {GenerationResetStatus::contended, actual};
    const u64 replacement = (observed & kLeaseControlEntrantsMaskV8) |
        static_cast<u64>(State::moving);
    if (word_.compare_exchange_weak(observed, replacement,
                                    std::memory_order_acq_rel,
                                    std::memory_order_acquire))
      return {GenerationResetStatus::installed, State::moving};
  }
}

bool BlobRetainedLifetimeLeaseV3::AtomicControl::TryEnter(
    bool allow_reusable_predecessor) noexcept {
  u64 observed = word_.load(std::memory_order_acquire);
  for (;;) {
    const State state =
        static_cast<State>(observed & kLeaseControlStateMaskV8);
    // Closing and entrant admission share this observation/CAS word. Once an
    // owner publishes closing, no later metadata entrant can extend its
    // retirement window even while the visible operation state is active.
    if ((observed & kLeaseControlClosingV8) != 0 ||
        state == State::terminal || state == State::installing ||
        (!allow_reusable_predecessor &&
         (state == State::released || state == State::moved_from)))
      return false;
    const u64 count = (observed & kLeaseControlEntrantsMaskV8) >>
        kLeaseControlEntrantsShiftV8;
    // A released or moved-from wrapper is reusable only after the prior
    // binding's owner has retired its entrant. The same CAS that observes
    // zero claims the new generation's sole installer entrant, preventing a
    // metadata reader from overlapping the old owner's post-final clearing.
    if (allow_reusable_predecessor &&
        (state == State::released || state == State::moved_from) &&
        count != 0)
      return false;
    if (count == kLeaseControlEntrantsValueMaskV8) return false;
    const u64 replacement = observed +
        (UINT64_C(1) << kLeaseControlEntrantsShiftV8);
    if (word_.compare_exchange_weak(observed, replacement,
                                    std::memory_order_acq_rel,
                                    std::memory_order_acquire))
      return true;
  }
}

void BlobRetainedLifetimeLeaseV3::AtomicControl::Leave() noexcept {
  u64 observed = word_.load(std::memory_order_acquire);
  for (;;) {
    const u64 count = (observed & kLeaseControlEntrantsMaskV8) >>
        kLeaseControlEntrantsShiftV8;
    if (count == 0) std::terminate();
    const u64 replacement = observed -
        (UINT64_C(1) << kLeaseControlEntrantsShiftV8);
    if (word_.compare_exchange_weak(observed, replacement,
                                    std::memory_order_acq_rel,
                                    std::memory_order_acquire))
      return;
  }
}

u32 BlobRetainedLifetimeLeaseV3::AtomicControl::entrants() const noexcept {
  return static_cast<u32>((word_.load(std::memory_order_acquire) &
                           kLeaseControlEntrantsMaskV8) >>
                          kLeaseControlEntrantsShiftV8);
}

bool BlobRetainedLifetimeLeaseV3::AtomicControl::closing() const noexcept {
  return (word_.load(std::memory_order_acquire) & kLeaseControlClosingV8) != 0;
}

void BlobLifetimeFactAccumulatorV3::Observe(
    const BlobLifetimeTypedFactV3& fact) noexcept {
  const u8 rank = fact.fact_class == BlobLifetimeFactClassV3::cleanup_protocol
      ? static_cast<u8>(BlobLifetimeFactClassV3::callback_protocol)
      : static_cast<u8>(fact.fact_class);
  if (rank == 0 || rank > 10) return;
  const u16 bit = static_cast<u16>(UINT16_C(1) << (rank - 1));
  if ((observed_mask_ & bit) == 0) {
    first_by_rank_[rank - 1] = fact;
    observed_mask_ = static_cast<u16>(observed_mask_ | bit);
  }
}

BlobLifetimeTypedFactV3 BlobLifetimeFactAccumulatorV3::Selected()
    const noexcept {
  for (u8 rank = 1; rank <= 10; ++rank) {
    const u16 bit = static_cast<u16>(UINT16_C(1) << (rank - 1));
    if ((observed_mask_ & bit) != 0) return first_by_rank_[rank - 1];
  }
  return {};
}

BlobProfileResultV3 BuildBlobValidatedProfileHandleV3(
    const BlobAuthorityReceiptV3& receipt,
    const DatatypeTypeCodecIdentityRowV3& identity) noexcept {
  if (!ExactReceipt(receipt) ||
      !IsExactCanonicalBlobTypeCodecIdentityV3(identity) ||
      identity.legacy_fields.catalog_snapshot_uuid !=
          receipt.catalog_snapshot_uuid ||
      identity.legacy_fields.catalog_generation != receipt.catalog_generation ||
      identity.legacy_fields.registry_generation !=
          receipt.registry_generation) {
    return Failure<BlobProfileResultV3>(
        BlobStructuralDiagnosticV3::descriptor_invalid);
  }

  auto result = Success<BlobProfileResultV3>();
  result.profile.receipt = receipt;
  result.profile.identity = SnapshotBlobIdentity(identity);
  result.profile.profile_uuid = kBlobV3ProfileUuid;
  result.profile.profile_generation = 1;
  result.profile.policy_bindings = kExpectedPolicies;
  result.profile.profile_fingerprint = kBlobV3ProfileFingerprint;
  result.profile.profile_fingerprint_preimage_bytes =
      kBlobProfileFingerprintPreimageBytesV3;
  if (!ExactProfile(result.profile)) {
    return Failure<BlobProfileResultV3>(
        BlobStructuralDiagnosticV3::descriptor_invalid);
  }
  return result;
}

BlobProfileResultV3 BuildCurrentBlobValidatedProfileHandleV3(
    const platform::Uuid& receipt_uuid) noexcept {
  if (receipt_uuid != kBlobV11ReceiptUuid) {
    return Failure<BlobProfileResultV3>(
        BlobStructuralDiagnosticV3::descriptor_invalid);
  }
  const auto* identity = CurrentBlobIdentity();
  if (identity == nullptr) {
    return Failure<BlobProfileResultV3>(
        BlobStructuralDiagnosticV3::descriptor_invalid);
  }
  return BuildBlobValidatedProfileHandleV3(
      {receipt_uuid, kDatatypeCohortV11, 11, 11}, *identity);
}

BlobValidationResultV3 ValidateBlobProfileHandleV3(
    const BlobValidatedProfileHandleV3& profile,
    const BlobExecutionControlV3&) noexcept {
  if (!ExactProfile(profile)) {
    return Failure<BlobValidationResultV3>(
        BlobStructuralDiagnosticV3::descriptor_invalid);
  }
  return Success<BlobValidationResultV3>();
}

BlobValidationResultV3 ValidateBlobMaterializedValueViewNoAllocV3(
    const BlobMaterializedValueViewV3& value, bool null_allowed,
    const BlobExecutionControlV3&) noexcept {
  if (value.profile == nullptr || !ExactProfile(*value.profile)) {
    return Failure<BlobValidationResultV3>(
        BlobStructuralDiagnosticV3::descriptor_invalid);
  }
  if (value.state != BlobValueStateV3::value &&
      value.state != BlobValueStateV3::sql_null) {
    return Failure<BlobValidationResultV3>(
        BlobStructuralDiagnosticV3::state_invalid);
  }
  if (value.state == BlobValueStateV3::sql_null) {
    if (value.logical_length != 0 || !value.bytes.empty() || !null_allowed) {
      return Failure<BlobValidationResultV3>(
          BlobStructuralDiagnosticV3::state_invalid);
    }
    return Success<BlobValidationResultV3>();
  }
  if (value.logical_length > kBlobMaximumLogicalBytesV3) {
    return Failure<BlobValidationResultV3>(
        BlobStructuralDiagnosticV3::length_exceeded);
  }
  if (value.logical_length != static_cast<u64>(value.bytes.size())) {
    return Failure<BlobValidationResultV3>(
        BlobStructuralDiagnosticV3::canonical_encoding_invalid);
  }
  // Raw nonempty bytes cannot prove a receiver-authenticated carrier or an
  // active retain/access obligation. Refuse them before dereference.
  if (value.logical_length != 0) {
    return Failure<BlobValidationResultV3>(
        BlobStructuralDiagnosticV3::descriptor_invalid);
  }
  return Success<BlobValidationResultV3>();
}

namespace {

bool BlobUuidIsNil(const BlobUuid16V3& value) noexcept {
  u8 aggregate = 0;
  for (u8 octet : value.bytes) aggregate |= octet;
  return aggregate == 0;
}

bool BlobUuidEqual(const BlobUuid16V3& left,
                   const BlobUuid16V3& right) noexcept {
  return std::equal(std::begin(left.bytes), std::end(left.bytes),
                    std::begin(right.bytes));
}


struct ActiveLifetimeCallbackV3 {
  bool active = false;
  BlobUuid16V3 authority_instance_uuid{};
  u64 authority_instance_generation = 0;
  BlobUuid16V3 lifetime_token_uuid{};
  u64 lifetime_token_generation = 0;
  u64 immutable_binding_generation = 0;
  BlobLifetimePhaseV3 phase = BlobLifetimePhaseV3::retain;
  u8 operation = 0;
  void* publisher_context = nullptr;
  bool (*publish)(void*, BlobLifetimeInvariantEventV3, u8,
                  BlobLifetimePhaseV3, u8) noexcept = nullptr;
};

thread_local ActiveLifetimeCallbackV3 gActiveLifetimeCallbackV3;
thread_local bool gInvariantServiceActiveV3 = false;

bool SameActiveCallbackBinding(const BlobLifetimeTokenV3& token) noexcept {
  return gActiveLifetimeCallbackV3.active &&
      BlobUuidEqual(gActiveLifetimeCallbackV3.authority_instance_uuid,
                    token.authority_instance_uuid) &&
      gActiveLifetimeCallbackV3.authority_instance_generation ==
          token.authority_instance_generation &&
      BlobUuidEqual(gActiveLifetimeCallbackV3.lifetime_token_uuid,
                    token.lifetime_token_uuid) &&
      gActiveLifetimeCallbackV3.lifetime_token_generation ==
          token.lifetime_token_generation &&
      gActiveLifetimeCallbackV3.immutable_binding_generation ==
          token.immutable_binding_generation;
}

bool SameLifetimeBinding(const BlobLifetimeTokenV3& left,
                         const BlobLifetimeTokenV3& right) noexcept {
  return BlobUuidEqual(left.authority_instance_uuid,
                       right.authority_instance_uuid) &&
         left.authority_instance_generation ==
             right.authority_instance_generation &&
         BlobUuidEqual(left.lifetime_token_uuid, right.lifetime_token_uuid) &&
         left.lifetime_token_generation == right.lifetime_token_generation &&
         left.immutable_binding_generation ==
             right.immutable_binding_generation;
}

bool LifetimeBindingLess(const BlobLifetimeTokenV3& left,
                         const BlobLifetimeTokenV3& right) noexcept {
  if (std::lexicographical_compare(
          std::begin(left.authority_instance_uuid.bytes),
          std::end(left.authority_instance_uuid.bytes),
          std::begin(right.authority_instance_uuid.bytes),
          std::end(right.authority_instance_uuid.bytes))) return true;
  if (!BlobUuidEqual(left.authority_instance_uuid,
                     right.authority_instance_uuid)) return false;
  if (left.authority_instance_generation !=
      right.authority_instance_generation) {
    return left.authority_instance_generation <
           right.authority_instance_generation;
  }
  if (std::lexicographical_compare(
          std::begin(left.lifetime_token_uuid.bytes),
          std::end(left.lifetime_token_uuid.bytes),
          std::begin(right.lifetime_token_uuid.bytes),
          std::end(right.lifetime_token_uuid.bytes))) return true;
  if (!BlobUuidEqual(left.lifetime_token_uuid,
                     right.lifetime_token_uuid)) return false;
  if (left.lifetime_token_generation != right.lifetime_token_generation)
    return left.lifetime_token_generation < right.lifetime_token_generation;
  return left.immutable_binding_generation <
         right.immutable_binding_generation;
}

constexpr u8 EncodeDeferredCallbackCode(u8 code) noexcept {
  return code == 255 ? 31 : code;
}

constexpr u8 DecodeDeferredCallbackCode(u64 packed) noexcept {
  const u8 encoded = static_cast<u8>((packed >> 13) & UINT64_C(0x1f));
  return encoded == 31 ? 255 : encoded;
}

constexpr u64 PackDeferredInvariant(BlobLifetimeInvariantEventV3 event,
                                    u8 operation,
                                    BlobLifetimePhaseV3 phase,
                                    u8 callback_code_or_255) noexcept {
  return static_cast<u64>(event) |
      (static_cast<u64>(operation) << 4) |
      (static_cast<u64>(phase) << 9) |
      (static_cast<u64>(EncodeDeferredCallbackCode(callback_code_or_255))
       << 13);
}

constexpr BlobLifetimeInvariantEventV3 DeferredEvent(u64 packed) noexcept {
  return static_cast<BlobLifetimeInvariantEventV3>(packed & UINT64_C(0xf));
}

constexpr u8 DeferredOperation(u64 packed) noexcept {
  return static_cast<u8>((packed >> 4) & UINT64_C(0x1f));
}

constexpr BlobLifetimePhaseV3 DeferredPhase(u64 packed) noexcept {
  return static_cast<BlobLifetimePhaseV3>((packed >> 9) & UINT64_C(0xf));
}

bool MarkActiveBindingReentry(const BlobLifetimeTokenV3& token) noexcept {
  if (!SameActiveCallbackBinding(token) ||
      gActiveLifetimeCallbackV3.publish == nullptr) {
    return false;
  }
  (void)gActiveLifetimeCallbackV3.publish(
      gActiveLifetimeCallbackV3.publisher_context,
      BlobLifetimeInvariantEventV3::callback_reentrant,
      gActiveLifetimeCallbackV3.operation,
      gActiveLifetimeCallbackV3.phase, 255);
  return true;
}

class LifetimeCallbackScopeV3 final {
 public:
  LifetimeCallbackScopeV3(std::atomic<bool>& active,
                          const BlobLifetimeTokenV3& token,
                          BlobLifetimePhaseV3 phase,
                          u8 operation,
                          void* publisher_context,
                          bool (*publish)(void*, BlobLifetimeInvariantEventV3,
                                          u8, BlobLifetimePhaseV3,
                                          u8) noexcept) noexcept
      : active_(active), previous_(gActiveLifetimeCallbackV3) {
    active_.store(true, std::memory_order_release);
    gActiveLifetimeCallbackV3 = {
        true, token.authority_instance_uuid,
        token.authority_instance_generation, token.lifetime_token_uuid,
        token.lifetime_token_generation, token.immutable_binding_generation,
        phase, operation, publisher_context, publish};
  }
  ~LifetimeCallbackScopeV3() noexcept {
    gActiveLifetimeCallbackV3 = previous_;
    active_.store(false, std::memory_order_release);
  }
  LifetimeCallbackScopeV3(const LifetimeCallbackScopeV3&) = delete;
  LifetimeCallbackScopeV3& operator=(const LifetimeCallbackScopeV3&) = delete;
 private:
  std::atomic<bool>& active_;
  ActiveLifetimeCallbackV3 previous_{};
};

bool PlatformUuidIsNil(const platform::Uuid& value) noexcept {
  u8 aggregate = 0;
  for (u8 octet : value.bytes) aggregate |= octet;
  return aggregate == 0;
}

bool PlatformEqualsBlob(const platform::Uuid& left,
                        const BlobUuid16V3& right) noexcept {
  return std::equal(left.bytes.begin(), left.bytes.end(),
                    std::begin(right.bytes));
}

template <typename T>
bool AllZero(const T& value) noexcept {
  const auto* first = reinterpret_cast<const u8*>(&value);
  return std::all_of(first, first + sizeof(T), [](u8 octet) {
    return octet == 0;
  });
}

bool ResultShapeValid(const BlobLifetimeCallbackResultV3& result) noexcept {
  return result.code <= SB_BLOB_CALLBACK_AUTHORITY_UNAVAILABLE_V3 &&
         std::all_of(std::begin(result.reserved_1_7),
                     std::end(result.reserved_1_7),
                     [](u8 octet) { return octet == 0; });
}

// Exact closed projection of the 647-row successor invariant whitelist.
// Derived from the accepted lifetime invariant contract; exact domain tested below.
inline constexpr std::array<std::array<u16, 21>, 13>
    kInvariantPhaseMaskByEventAndOperationV7{{
        {{UINT16_C(0x006e), UINT16_C(0x006e), UINT16_C(0x006e), UINT16_C(0x006e), UINT16_C(0x006e), UINT16_C(0x006e), UINT16_C(0x006e), UINT16_C(0x006e), UINT16_C(0x006e), UINT16_C(0x006e), UINT16_C(0x006e), UINT16_C(0x006e), UINT16_C(0x006e), UINT16_C(0x0000), UINT16_C(0x0042), UINT16_C(0x0040), UINT16_C(0x0040), UINT16_C(0x0040), UINT16_C(0x0800), UINT16_C(0x0000), UINT16_C(0x0000)}},
        {{UINT16_C(0x006e), UINT16_C(0x006e), UINT16_C(0x006e), UINT16_C(0x006e), UINT16_C(0x006e), UINT16_C(0x006e), UINT16_C(0x006e), UINT16_C(0x006e), UINT16_C(0x006e), UINT16_C(0x006e), UINT16_C(0x006e), UINT16_C(0x006e), UINT16_C(0x006e), UINT16_C(0x0000), UINT16_C(0x0042), UINT16_C(0x0040), UINT16_C(0x0040), UINT16_C(0x0040), UINT16_C(0x0800), UINT16_C(0x0000), UINT16_C(0x0000)}},
        {{UINT16_C(0x006e), UINT16_C(0x006e), UINT16_C(0x006e), UINT16_C(0x006e), UINT16_C(0x006e), UINT16_C(0x006e), UINT16_C(0x006e), UINT16_C(0x006e), UINT16_C(0x006e), UINT16_C(0x006e), UINT16_C(0x006e), UINT16_C(0x006e), UINT16_C(0x006e), UINT16_C(0x0000), UINT16_C(0x0042), UINT16_C(0x0040), UINT16_C(0x0040), UINT16_C(0x0040), UINT16_C(0x0800), UINT16_C(0x0000), UINT16_C(0x0000)}},
        {{UINT16_C(0x006e), UINT16_C(0x006e), UINT16_C(0x006e), UINT16_C(0x006e), UINT16_C(0x006e), UINT16_C(0x006e), UINT16_C(0x006e), UINT16_C(0x006e), UINT16_C(0x006e), UINT16_C(0x006e), UINT16_C(0x006e), UINT16_C(0x006e), UINT16_C(0x006e), UINT16_C(0x0000), UINT16_C(0x0042), UINT16_C(0x0040), UINT16_C(0x0040), UINT16_C(0x0040), UINT16_C(0x0800), UINT16_C(0x0000), UINT16_C(0x0000)}},
        {{UINT16_C(0x000a), UINT16_C(0x000a), UINT16_C(0x000a), UINT16_C(0x000a), UINT16_C(0x000a), UINT16_C(0x000a), UINT16_C(0x000a), UINT16_C(0x000a), UINT16_C(0x000a), UINT16_C(0x000a), UINT16_C(0x000a), UINT16_C(0x000a), UINT16_C(0x000a), UINT16_C(0x0000), UINT16_C(0x0002), UINT16_C(0x0000), UINT16_C(0x0000), UINT16_C(0x0000), UINT16_C(0x0000), UINT16_C(0x0000), UINT16_C(0x0000)}},
        {{UINT16_C(0x0020), UINT16_C(0x0020), UINT16_C(0x0020), UINT16_C(0x0020), UINT16_C(0x0020), UINT16_C(0x0020), UINT16_C(0x0020), UINT16_C(0x0020), UINT16_C(0x0020), UINT16_C(0x0020), UINT16_C(0x0020), UINT16_C(0x0020), UINT16_C(0x0020), UINT16_C(0x0000), UINT16_C(0x0000), UINT16_C(0x0000), UINT16_C(0x0000), UINT16_C(0x0000), UINT16_C(0x0000), UINT16_C(0x0000), UINT16_C(0x0000)}},
        {{UINT16_C(0x0040), UINT16_C(0x0040), UINT16_C(0x0040), UINT16_C(0x0040), UINT16_C(0x0040), UINT16_C(0x0040), UINT16_C(0x0040), UINT16_C(0x0040), UINT16_C(0x0040), UINT16_C(0x0040), UINT16_C(0x0040), UINT16_C(0x0040), UINT16_C(0x0040), UINT16_C(0x0000), UINT16_C(0x0040), UINT16_C(0x0040), UINT16_C(0x0040), UINT16_C(0x0040), UINT16_C(0x0800), UINT16_C(0x0000), UINT16_C(0x0000)}},
        {{UINT16_C(0x0010), UINT16_C(0x0010), UINT16_C(0x0010), UINT16_C(0x0010), UINT16_C(0x0010), UINT16_C(0x0010), UINT16_C(0x0010), UINT16_C(0x0010), UINT16_C(0x0010), UINT16_C(0x0010), UINT16_C(0x0010), UINT16_C(0x0010), UINT16_C(0x0010), UINT16_C(0x0000), UINT16_C(0x0042), UINT16_C(0x0240), UINT16_C(0x0440), UINT16_C(0x0040), UINT16_C(0x0000), UINT16_C(0x0000), UINT16_C(0x0000)}},
        {{UINT16_C(0x019e), UINT16_C(0x019e), UINT16_C(0x019e), UINT16_C(0x019e), UINT16_C(0x019e), UINT16_C(0x019e), UINT16_C(0x019e), UINT16_C(0x019e), UINT16_C(0x019e), UINT16_C(0x019e), UINT16_C(0x019e), UINT16_C(0x019e), UINT16_C(0x019e), UINT16_C(0x0001), UINT16_C(0x0002), UINT16_C(0x0000), UINT16_C(0x0000), UINT16_C(0x0000), UINT16_C(0x0000), UINT16_C(0x0000), UINT16_C(0x0000)}},
        {{UINT16_C(0x0000), UINT16_C(0x0000), UINT16_C(0x0000), UINT16_C(0x0000), UINT16_C(0x0000), UINT16_C(0x0000), UINT16_C(0x0000), UINT16_C(0x0000), UINT16_C(0x0000), UINT16_C(0x0000), UINT16_C(0x0000), UINT16_C(0x0000), UINT16_C(0x0000), UINT16_C(0x0000), UINT16_C(0x0000), UINT16_C(0x0000), UINT16_C(0x0000), UINT16_C(0x0000), UINT16_C(0x0000), UINT16_C(0x1000), UINT16_C(0x2000)}},
        {{UINT16_C(0x019e), UINT16_C(0x019e), UINT16_C(0x019e), UINT16_C(0x019e), UINT16_C(0x019e), UINT16_C(0x019e), UINT16_C(0x019e), UINT16_C(0x019e), UINT16_C(0x019e), UINT16_C(0x019e), UINT16_C(0x019e), UINT16_C(0x019e), UINT16_C(0x019e), UINT16_C(0x0000), UINT16_C(0x0002), UINT16_C(0x0000), UINT16_C(0x0000), UINT16_C(0x0000), UINT16_C(0x0000), UINT16_C(0x0000), UINT16_C(0x0000)}},
        {{UINT16_C(0x01fe), UINT16_C(0x01fe), UINT16_C(0x01fe), UINT16_C(0x01fe), UINT16_C(0x01fe), UINT16_C(0x01fe), UINT16_C(0x01fe), UINT16_C(0x01fe), UINT16_C(0x01fe), UINT16_C(0x01fe), UINT16_C(0x01fe), UINT16_C(0x01fe), UINT16_C(0x01fe), UINT16_C(0x0001), UINT16_C(0x0042), UINT16_C(0x0040), UINT16_C(0x0040), UINT16_C(0x0040), UINT16_C(0x0800), UINT16_C(0x0000), UINT16_C(0x0000)}},
        {{UINT16_C(0x0010), UINT16_C(0x0010), UINT16_C(0x0010), UINT16_C(0x0010), UINT16_C(0x0010), UINT16_C(0x0010), UINT16_C(0x0010), UINT16_C(0x0010), UINT16_C(0x0010), UINT16_C(0x0010), UINT16_C(0x0010), UINT16_C(0x0010), UINT16_C(0x0010), UINT16_C(0x0000), UINT16_C(0x0000), UINT16_C(0x0000), UINT16_C(0x0000), UINT16_C(0x0000), UINT16_C(0x0000), UINT16_C(0x0000), UINT16_C(0x0000)}},
    }};

static_assert([]() constexpr {
  u32 allowed = 0;
  for (const auto& event : kInvariantPhaseMaskByEventAndOperationV7)
    for (u16 phases : event) allowed += std::popcount(phases);
  return allowed == 647;
}());

bool InvariantCombinationAllowed(BlobLifetimeInvariantEventV3 event,
                                 u8 operation,
                                 BlobLifetimePhaseV3 phase,
                                 u8 callback_code_or_255) noexcept {
  const u8 event_value = static_cast<u8>(event);
  const u8 phase_value = static_cast<u8>(phase);
  if (event_value < 1 || event_value > 13 || operation < 1 ||
      operation > 21 || phase_value < 1 || phase_value > 14 ||
      (callback_code_or_255 > 18 && callback_code_or_255 != 255))
    return false;
  const u16 allowed_phases =
      kInvariantPhaseMaskByEventAndOperationV7[event_value - 1]
                                                [operation - 1];
  return (allowed_phases &
          static_cast<u16>(UINT16_C(1) << (phase_value - 1))) != 0;
}

bool InvariantFactDomainValid(const BlobLifetimeBindingKeyV3& key,
                              const BlobLifetimeInvariantFactV3& fact)
    noexcept {
  return !PlatformUuidIsNil(key.authority_instance_uuid) &&
      !PlatformUuidIsNil(key.lifetime_token_uuid) &&
      key.authority_instance_generation != 0 &&
      key.lifetime_token_generation != 0 &&
      key.immutable_binding_generation != 0 &&
      fact.authority_instance_generation ==
          key.authority_instance_generation &&
      fact.lifetime_token_generation == key.lifetime_token_generation &&
      fact.immutable_binding_generation ==
          key.immutable_binding_generation &&
      InvariantCombinationAllowed(fact.event, fact.operation_enum, fact.phase,
                                  fact.callback_code_or_255);
}

BlobLifetimeRuntimeResultV3 RuntimeSuccess() noexcept {
  return {BlobLifetimeOuterDispositionV3::success, OkStatus(), {}, {}};
}

BlobLifetimeDiagnosticCodeV3 TypedDiagnostic(
    std::string_view code) noexcept {
  if (code == "SECURITY.ACCESS_DENIED")
    return BlobLifetimeDiagnosticCodeV3::security_access_denied;
  if (code == "PROCESS.CANCELLED")
    return BlobLifetimeDiagnosticCodeV3::process_cancelled;
  if (code == "CINL.LOB.HANDLE_EXPIRED")
    return BlobLifetimeDiagnosticCodeV3::handle_expired;
  if (code == "CINL.LOB.DESCRIPTOR_INVALID")
    return BlobLifetimeDiagnosticCodeV3::descriptor_invalid;
  if (code == "BLOB.HANDLE_OWNER_MISMATCH")
    return BlobLifetimeDiagnosticCodeV3::handle_owner_mismatch;
  if (code == "BLOB.HANDLE_MODE_REFUSED")
    return BlobLifetimeDiagnosticCodeV3::handle_mode_refused;
  if (code == "BLOB.LIFETIME_AUTHORITY_UNAVAILABLE")
    return BlobLifetimeDiagnosticCodeV3::lifetime_authority_unavailable;
  if (code == "CINL.LOB.STREAM_BACKPRESSURE_EXHAUSTED")
    return BlobLifetimeDiagnosticCodeV3::stream_backpressure_exhausted;
  if (code == "RESOURCE.BUDGET_EXCEEDED")
    return BlobLifetimeDiagnosticCodeV3::resource_budget_exceeded;
  if (code == "BLOB.IO_FAILED") return BlobLifetimeDiagnosticCodeV3::io_failed;
  if (code == "BLOB.INTEGRITY_FAILED")
    return BlobLifetimeDiagnosticCodeV3::integrity_failed;
  if (code == "BLOB.STATE_INVALID")
    return BlobLifetimeDiagnosticCodeV3::state_invalid;
  if (code == "BLOB.LENGTH_EXCEEDED")
    return BlobLifetimeDiagnosticCodeV3::length_exceeded;
  return BlobLifetimeDiagnosticCodeV3::none;
}

BlobLifetimeReasonV3 TypedReason(std::string_view reason) noexcept {
#define BLOB_REASON(token) if (reason == #token) return BlobLifetimeReasonV3::token
  BLOB_REASON(authority_table_missing);
  BLOB_REASON(authority_abi_mismatch);
  BLOB_REASON(authority_profile_binding_mismatch);
  BLOB_REASON(token_struct_invalid);
  BLOB_REASON(token_identity_invalid);
  BLOB_REASON(wrong_authority_instance);
  BLOB_REASON(owner_binding_mismatch);
  BLOB_REASON(transaction_mismatch);
  BLOB_REASON(access_ticket_invalid);
  BLOB_REASON(access_ticket_consumed);
  BLOB_REASON(callback_result_unknown);
  BLOB_REASON(callback_output_impossible);
  BLOB_REASON(callback_reentrant);
  BLOB_REASON(release_failed);
  BLOB_REASON(same_ticket_concurrent_use);
  BLOB_REASON(absolute_deadline_expired);
  BLOB_REASON(reader_io_failure);
  BLOB_REASON(sink_io_failure);
  BLOB_REASON(provider_io_failure);
  BLOB_REASON(adapter_exception_contained);
  BLOB_REASON(end_access_failed);
  BLOB_REASON(authority_runtime_unavailable);
  BLOB_REASON(monotonic_clock_unavailable);
  BLOB_REASON(receiver_services_failed);
  BLOB_REASON(shutdown_drain_violation);
  BLOB_REASON(authority_capacity_exhausted);
  BLOB_REASON(visitor_missing);
  BLOB_REASON(clone_alias);
  BLOB_REASON(destination_armed);
#undef BLOB_REASON
  if (reason == "wrong_owner")
    return BlobLifetimeReasonV3::owner_binding_mismatch;
  return BlobLifetimeReasonV3::none;
}

bool KnownRuntimeReason(std::string_view reason) noexcept {
  constexpr std::array<std::string_view, 18> internal{{
      "moved_binding/moved_binding_generation",
      "unknown_token", "security_denied", "wrong_snapshot",
      "stale_snapshot", "monotonic_expiry", "revoked", "wrong_mode",
      "closed", "retained_ticket_invalid", "retained_ticket_consumed",
      "sampled_true", "budget_shortage", "lifetime_callback_calls",
      "length_not_representable",
      "sql_null_not_admitted",
      "carrier_state_not_admitted",
      "wrong_owner"}};
  if (TypedReason(reason) != BlobLifetimeReasonV3::none) return true;
  return std::find(internal.begin(), internal.end(), reason) != internal.end();
}

bool DiagnosticReasonPairAllowed(
    BlobLifetimeDiagnosticCodeV3 diagnostic,
    BlobLifetimeReasonV3 typed_reason,
    std::string_view selector) noexcept {
  switch (diagnostic) {
    case BlobLifetimeDiagnosticCodeV3::security_access_denied:
      return typed_reason == BlobLifetimeReasonV3::none &&
          selector == "security_denied";
    case BlobLifetimeDiagnosticCodeV3::process_cancelled:
      return typed_reason == BlobLifetimeReasonV3::none &&
          selector == "sampled_true";
    case BlobLifetimeDiagnosticCodeV3::handle_expired:
      return typed_reason == BlobLifetimeReasonV3::none &&
          (selector == "moved_binding/moved_binding_generation" ||
           selector == "unknown_token" || selector == "wrong_snapshot" ||
           selector == "stale_snapshot" ||
           selector == "monotonic_expiry" || selector == "revoked" ||
           selector == "closed" ||
           selector == "retained_ticket_invalid" ||
           selector == "retained_ticket_consumed");
    case BlobLifetimeDiagnosticCodeV3::descriptor_invalid:
      switch (typed_reason) {
        case BlobLifetimeReasonV3::authority_abi_mismatch:
        case BlobLifetimeReasonV3::authority_profile_binding_mismatch:
        case BlobLifetimeReasonV3::token_struct_invalid:
        case BlobLifetimeReasonV3::token_identity_invalid:
        case BlobLifetimeReasonV3::wrong_authority_instance:
        case BlobLifetimeReasonV3::transaction_mismatch:
        case BlobLifetimeReasonV3::access_ticket_invalid:
        case BlobLifetimeReasonV3::access_ticket_consumed:
        case BlobLifetimeReasonV3::callback_result_unknown:
        case BlobLifetimeReasonV3::callback_output_impossible:
        case BlobLifetimeReasonV3::callback_reentrant:
        case BlobLifetimeReasonV3::release_failed:
        case BlobLifetimeReasonV3::same_ticket_concurrent_use:
        case BlobLifetimeReasonV3::end_access_failed:
        case BlobLifetimeReasonV3::visitor_missing:
        case BlobLifetimeReasonV3::clone_alias:
        case BlobLifetimeReasonV3::destination_armed:
          return true;
        default:
          return false;
      }
    case BlobLifetimeDiagnosticCodeV3::handle_owner_mismatch:
      return typed_reason == BlobLifetimeReasonV3::owner_binding_mismatch &&
          (selector == "wrong_owner" ||
           selector == "owner_binding_mismatch");
    case BlobLifetimeDiagnosticCodeV3::handle_mode_refused:
      return typed_reason == BlobLifetimeReasonV3::none &&
          selector == "wrong_mode";
    case BlobLifetimeDiagnosticCodeV3::lifetime_authority_unavailable:
      return typed_reason == BlobLifetimeReasonV3::authority_table_missing ||
          typed_reason == BlobLifetimeReasonV3::authority_runtime_unavailable ||
          typed_reason == BlobLifetimeReasonV3::monotonic_clock_unavailable ||
          typed_reason == BlobLifetimeReasonV3::receiver_services_failed ||
          typed_reason == BlobLifetimeReasonV3::shutdown_drain_violation ||
          typed_reason == BlobLifetimeReasonV3::authority_capacity_exhausted;
    case BlobLifetimeDiagnosticCodeV3::stream_backpressure_exhausted:
      return typed_reason == BlobLifetimeReasonV3::absolute_deadline_expired;
    case BlobLifetimeDiagnosticCodeV3::resource_budget_exceeded:
      return typed_reason == BlobLifetimeReasonV3::none &&
          (selector == "budget_shortage" ||
           selector == "lifetime_callback_calls");
    case BlobLifetimeDiagnosticCodeV3::io_failed:
    case BlobLifetimeDiagnosticCodeV3::integrity_failed:
      return false;
    case BlobLifetimeDiagnosticCodeV3::state_invalid:
      return typed_reason == BlobLifetimeReasonV3::none &&
          (selector == "sql_null_not_admitted" ||
           selector == "carrier_state_not_admitted");
    case BlobLifetimeDiagnosticCodeV3::length_exceeded:
      return typed_reason == BlobLifetimeReasonV3::none &&
          selector == "length_not_representable";
    case BlobLifetimeDiagnosticCodeV3::none:
      return false;
  }
  return false;
}

std::string_view PublicReasonToken(BlobLifetimeReasonV3 reason) noexcept {
  switch (reason) {
#define BLOB_PUBLIC_REASON(token) \
    case BlobLifetimeReasonV3::token: return #token
    BLOB_PUBLIC_REASON(authority_table_missing);
    BLOB_PUBLIC_REASON(authority_abi_mismatch);
    BLOB_PUBLIC_REASON(authority_profile_binding_mismatch);
    BLOB_PUBLIC_REASON(token_struct_invalid);
    BLOB_PUBLIC_REASON(token_identity_invalid);
    BLOB_PUBLIC_REASON(wrong_authority_instance);
    BLOB_PUBLIC_REASON(owner_binding_mismatch);
    BLOB_PUBLIC_REASON(transaction_mismatch);
    BLOB_PUBLIC_REASON(access_ticket_invalid);
    BLOB_PUBLIC_REASON(access_ticket_consumed);
    BLOB_PUBLIC_REASON(callback_result_unknown);
    BLOB_PUBLIC_REASON(callback_output_impossible);
    BLOB_PUBLIC_REASON(callback_reentrant);
    BLOB_PUBLIC_REASON(release_failed);
    BLOB_PUBLIC_REASON(same_ticket_concurrent_use);
    BLOB_PUBLIC_REASON(absolute_deadline_expired);
    BLOB_PUBLIC_REASON(reader_io_failure);
    BLOB_PUBLIC_REASON(sink_io_failure);
    BLOB_PUBLIC_REASON(provider_io_failure);
    BLOB_PUBLIC_REASON(adapter_exception_contained);
    BLOB_PUBLIC_REASON(end_access_failed);
    BLOB_PUBLIC_REASON(authority_runtime_unavailable);
    BLOB_PUBLIC_REASON(monotonic_clock_unavailable);
    BLOB_PUBLIC_REASON(receiver_services_failed);
    BLOB_PUBLIC_REASON(shutdown_drain_violation);
    BLOB_PUBLIC_REASON(authority_capacity_exhausted);
    BLOB_PUBLIC_REASON(visitor_missing);
    BLOB_PUBLIC_REASON(clone_alias);
    BLOB_PUBLIC_REASON(destination_armed);
#undef BLOB_PUBLIC_REASON
    case BlobLifetimeReasonV3::none:
      return {};
  }
  return {};
}

BlobLifetimeFactClassV3 TypedFactClass(
    BlobLifetimeDiagnosticCodeV3 diagnostic,
    BlobLifetimeReasonV3 reason) noexcept {
  switch (diagnostic) {
    case BlobLifetimeDiagnosticCodeV3::security_access_denied:
      return BlobLifetimeFactClassV3::security;
    case BlobLifetimeDiagnosticCodeV3::process_cancelled:
      return BlobLifetimeFactClassV3::cancellation;
    case BlobLifetimeDiagnosticCodeV3::handle_expired:
      return BlobLifetimeFactClassV3::snapshot;
    case BlobLifetimeDiagnosticCodeV3::handle_owner_mismatch:
    case BlobLifetimeDiagnosticCodeV3::handle_mode_refused:
      return BlobLifetimeFactClassV3::binding;
    case BlobLifetimeDiagnosticCodeV3::lifetime_authority_unavailable:
      return BlobLifetimeFactClassV3::lifetime_authority_unavailable;
    case BlobLifetimeDiagnosticCodeV3::stream_backpressure_exhausted:
      return BlobLifetimeFactClassV3::timeout;
    case BlobLifetimeDiagnosticCodeV3::resource_budget_exceeded:
      return BlobLifetimeFactClassV3::resource_budget;
    case BlobLifetimeDiagnosticCodeV3::io_failed:
      return BlobLifetimeFactClassV3::io;
    case BlobLifetimeDiagnosticCodeV3::integrity_failed:
      return BlobLifetimeFactClassV3::integrity;
    case BlobLifetimeDiagnosticCodeV3::descriptor_invalid:
      if (reason == BlobLifetimeReasonV3::wrong_authority_instance ||
          reason == BlobLifetimeReasonV3::transaction_mismatch)
        return BlobLifetimeFactClassV3::binding;
      return BlobLifetimeFactClassV3::callback_protocol;
    default:
      return BlobLifetimeFactClassV3::callback_protocol;
  }
}

BlobLifetimeRuntimeResultV3 RuntimeFailure(
    std::string_view code, std::string_view reason,
    Status status = ErrorStatus()) noexcept {
  BlobLifetimeRuntimeResultV3 result{
      BlobLifetimeOuterDispositionV3::public_failure, status, code, reason};
  if (code == "CINL.LOB.DESCRIPTOR_INVALID")
    result.diagnostic_uuid = U({0x01,0xa1,0x09,0x5f,0xf2,0x05,0x7a,0x4c,
                                0x8e,0x41,0x52,0x7e,0x88,0x3a,0x2d,0xc4});
  else if (code == "CINL.LOB.HANDLE_EXPIRED")
    result.diagnostic_uuid = U({0x01,0xa1,0x09,0x5f,0xf2,0x05,0x76,0xf6,
                                0x9b,0x03,0xa8,0xa5,0x40,0x7f,0xdb,0x63});
  else if (code == "SECURITY.ACCESS_DENIED")
    result.diagnostic_uuid = U({0x1a,0x8d,0x26,0xb0,0x87,0x55,0x56,0xad,
                                0xa1,0x70,0xb2,0x4a,0x3a,0xaf,0x14,0x07});
  else if (code == "BLOB.HANDLE_OWNER_MISMATCH")
    result.diagnostic_uuid = U({0x01,0xa1,0x09,0x5f,0xf2,0x05,0x7f,0x86,
                                0xa1,0x2c,0x8f,0x07,0xc6,0xcf,0x43,0x13});
  else if (code == "BLOB.HANDLE_MODE_REFUSED")
    result.diagnostic_uuid = U({0x01,0xa1,0x09,0x5f,0xf2,0x05,0x78,0xbd,
                                0x8a,0x08,0x14,0x50,0x5a,0xc9,0xae,0x82});
  else if (code == "BLOB.LIFETIME_AUTHORITY_UNAVAILABLE")
    result.diagnostic_uuid = U({0x01,0xa1,0x0c,0x52,0x66,0x9b,0x7a,0x09,
                                0xa3,0xc1,0x66,0x6d,0xad,0x4b,0x48,0x8e});
  else if (code == "PROCESS.CANCELLED")
    result.diagnostic_uuid = U({0x0f,0x42,0x5d,0xe2,0x6b,0xc7,0x51,0xd4,
                                0x8d,0x34,0x72,0xd1,0x07,0xad,0x45,0xe7});
  else if (code == "CINL.LOB.STREAM_BACKPRESSURE_EXHAUSTED")
    result.diagnostic_uuid = U({0x01,0xa1,0x09,0x5f,0xf2,0x05,0x74,0x4a,
                                0x99,0x3f,0x92,0xc9,0x77,0x0c,0x11,0xa4});
  else if (code == "RESOURCE.BUDGET_EXCEEDED")
    result.diagnostic_uuid = U({0xde,0xdc,0x1c,0x9b,0x38,0x79,0x57,0x70,
                                0x8f,0x8e,0xc0,0xc9,0x27,0xa0,0xc3,0x4d});
  else if (code == "BLOB.STATE_INVALID")
    result.diagnostic_uuid = U({0x01,0xa1,0x09,0x5f,0xf2,0x05,0x78,0xb9,
                                0xb0,0x5c,0x43,0x0e,0x3c,0x37,0xdf,0xa4});
  else if (code == "BLOB.LENGTH_EXCEEDED")
    result.diagnostic_uuid = U({0x01,0xa1,0x09,0x5f,0xf2,0x05,0x71,0x86,
                                0x9e,0x85,0xf3,0x65,0x9d,0x0f,0x73,0xdf});
  else if (code == "BLOB.IO_FAILED")
    result.diagnostic_uuid = U({0x01,0xa1,0x0c,0x52,0x66,0x99,0x7e,0xdb,
                                0xb4,0x6f,0xaf,0x2b,0x0a,0x3d,0x5a,0xca});
  else if (code == "BLOB.INTEGRITY_FAILED")
    result.diagnostic_uuid = U({0x01,0xa1,0x09,0x5f,0xf2,0x05,0x78,0x79,
                                0xb9,0x23,0x42,0x5b,0xe9,0x3f,0x2c,0x1a});
  if (!PlatformUuidIsNil(result.diagnostic_uuid))
    result.diagnostic_generation = 1;
  result.fact.diagnostic = TypedDiagnostic(code);
  result.fact.reason = TypedReason(reason);
  if (result.fact.diagnostic == BlobLifetimeDiagnosticCodeV3::none ||
      !KnownRuntimeReason(reason) ||
      !DiagnosticReasonPairAllowed(result.fact.diagnostic,
                                   result.fact.reason, reason)) {
    result.disposition =
        BlobLifetimeOuterDispositionV3::terminated_out_of_band;
    result.status = ErrorStatus();
    result.diagnostic_code = {};
    result.reason = {};
    result.diagnostic_uuid = {};
    result.diagnostic_generation = 0;
    result.fact = {};
    return result;
  }
  result.fact.fact_class = TypedFactClass(result.fact.diagnostic,
                                          result.fact.reason);
  result.reason = PublicReasonToken(result.fact.reason);
  if (reason == "callback_reentrant" ||
      reason == "same_ticket_concurrent_use" ||
      reason == "visitor_missing" || reason == "clone_alias" ||
      reason == "destination_armed") {
    result.fact.authority_source =
        BlobLifetimeAuthoritySourceV3::datatype_local_state;
    result.fact.stage = BlobLifetimeFactStageV3::local_precondition;
  }
  result.fact.diagnostic_uuid = result.diagnostic_uuid;
  result.fact.diagnostic_generation = result.diagnostic_generation;
  return result;
}

BlobLifetimeRuntimeResultV3 RuntimeTerminated() noexcept {
  return {BlobLifetimeOuterDispositionV3::terminated_out_of_band,
          ErrorStatus(), {}, {}};
}

BlobLifetimeRuntimeResultV3 RuntimeFromProtectedFact(
    const BlobLifetimeTypedFactV3& fact) noexcept {
  std::string_view code;
  std::string_view reason;
  switch (fact.fact_class) {
    case BlobLifetimeFactClassV3::resource_budget:
      if (fact.diagnostic !=
              BlobLifetimeDiagnosticCodeV3::resource_budget_exceeded ||
          fact.reason != BlobLifetimeReasonV3::none)
        return RuntimeTerminated();
      code = "RESOURCE.BUDGET_EXCEEDED";
      reason = "budget_shortage";
      break;
    case BlobLifetimeFactClassV3::io:
    case BlobLifetimeFactClassV3::integrity:
      // Successor Core must close exact operation/phase/reason/adapter
      // compatibility for IO and exact format/reason compatibility for
      // integrity. Until that authority is immutable, reject both classes.
      return RuntimeTerminated();
    default:
      return RuntimeTerminated();
  }
  auto result = RuntimeFailure(
      code, reason,
      fact.fact_class == BlobLifetimeFactClassV3::resource_budget
          ? ResourceStatus() : ErrorStatus());
  if (fact.diagnostic_uuid != result.diagnostic_uuid ||
      fact.diagnostic_generation != result.diagnostic_generation)
    return RuntimeTerminated();
  if (fact.authority_source != BlobLifetimeAuthoritySourceV3::none ||
      fact.stage != BlobLifetimeFactStageV3::callback_return ||
      fact.gate_present || static_cast<u8>(fact.gate) != 0)
    return RuntimeTerminated();
  const auto& p = fact.parameters;
  const bool unrelated_zero = p.operation_enum == 0 &&
      static_cast<u8>(p.phase_enum) == 0 && p.owner_class_enum == 0 &&
      p.open_mode_enum == 0 && p.supplied_state_u8 == 0 &&
      p.actual_length_u64 == 0 && p.maximum_length_u64 == 0 &&
      p.window_octets_u64 == 0 && p.timeout_milliseconds_u64 == 0 &&
      p.format_enum == 0 && p.adapter_kind_enum == 0 &&
      p.lifetime_enum == BlobLifetimeDiagnosticLifetimeV3::none &&
      p.expiry_reason_enum ==
          BlobLifetimeDiagnosticExpiryReasonV3::none &&
      PlatformUuidIsNil(p.descriptor_uuid) && p.descriptor_generation == 0;
  if (!unrelated_zero) return RuntimeTerminated();
  if (fact.fact_class == BlobLifetimeFactClassV3::resource_budget) {
    constexpr u32 required = blob_parameter_resource |
        blob_parameter_required | blob_parameter_available;
    if (p.present != required ||
        static_cast<u8>(p.resource_enum) < 1 ||
        static_cast<u8>(p.resource_enum) > 11 ||
        p.required_u64 <= p.available_u64)
      return RuntimeTerminated();
  } else if (p.present != 0 || static_cast<u8>(p.resource_enum) != 0 ||
             p.required_u64 != 0 || p.available_u64 != 0) {
    return RuntimeTerminated();
  }
  result.fact = fact;
  result.resource = fact.parameters.resource_enum;
  result.required = fact.parameters.required_u64;
  result.available = fact.parameters.available_u64;
  return result;
}


u8 FailureRank(const BlobLifetimeRuntimeResultV3& result) noexcept {
  if (result.ok()) return 255;
  if (result.disposition ==
      BlobLifetimeOuterDispositionV3::terminated_out_of_band) return 0;
  return result.fact.fact_class == BlobLifetimeFactClassV3::cleanup_protocol
      ? static_cast<u8>(BlobLifetimeFactClassV3::callback_protocol)
      : static_cast<u8>(result.fact.fact_class);
}

BlobLifetimeRuntimeResultV3 SelectByV7Precedence(
    const BlobLifetimeRuntimeResultV3& first_observed,
    const BlobLifetimeRuntimeResultV3& second_observed) noexcept {
  if (first_observed.ok()) return second_observed;
  if (second_observed.ok()) return first_observed;
  if (FailureRank(first_observed) == 0) return first_observed;
  if (FailureRank(second_observed) == 0) return second_observed;
  BlobLifetimeFactAccumulatorV3 accumulator;
  accumulator.Observe(first_observed.fact);
  accumulator.Observe(second_observed.fact);
  const auto selected = accumulator.Selected();
  return selected.fact_class == first_observed.fact.fact_class
      ? first_observed : second_observed;
}

BlobLifetimeRuntimeResultV3 LocalLifetimeFailure(
    std::string_view selector,
    BlobLifetimeDiagnosticLifetimeV3 lifetime,
    BlobLifetimeDiagnosticExpiryReasonV3 expiry) noexcept {
  auto result = RuntimeFailure("CINL.LOB.HANDLE_EXPIRED", selector);
  if (result.disposition ==
      BlobLifetimeOuterDispositionV3::terminated_out_of_band)
    return result;
  result.fact.authority_source =
      BlobLifetimeAuthoritySourceV3::datatype_local_state;
  result.fact.stage = BlobLifetimeFactStageV3::local_precondition;
  result.fact.parameters.present = blob_parameter_lifetime |
                                    blob_parameter_expiry_reason;
  result.fact.parameters.lifetime_enum = lifetime;
  result.fact.parameters.expiry_reason_enum = expiry;
  return result;
}

BlobLifetimeRuntimeResultV3 UnretainedFailure() noexcept {
  return LocalLifetimeFailure(
      "retained_ticket_invalid",
      BlobLifetimeDiagnosticLifetimeV3::retain_ticket_invalid,
      BlobLifetimeDiagnosticExpiryReasonV3::retain_ticket_unknown);
}

BlobLifetimeRuntimeResultV3 ConsumedFailure() noexcept {
  return LocalLifetimeFailure(
      "retained_ticket_consumed",
      BlobLifetimeDiagnosticLifetimeV3::retain_ticket_consumed,
      BlobLifetimeDiagnosticExpiryReasonV3::retain_ticket_consumed);
}

BlobLifetimeRuntimeResultV3 OwnerClosedFailure() noexcept {
  return LocalLifetimeFailure(
      "closed", BlobLifetimeDiagnosticLifetimeV3::owner_closed,
      BlobLifetimeDiagnosticExpiryReasonV3::owner_closed);
}

BlobLifetimeRuntimeResultV3 MovedBindingFailure() noexcept {
  return LocalLifetimeFailure(
      "moved_binding/moved_binding_generation",
      BlobLifetimeDiagnosticLifetimeV3::moved_binding,
      BlobLifetimeDiagnosticExpiryReasonV3::moved_binding_generation);
}

BlobLifetimeRuntimeResultV3 ProtocolFailure(
    std::string_view reason,
    const platform::Uuid& descriptor_uuid,
    u64 descriptor_generation,
    BlobLifetimeFactStageV3 stage = BlobLifetimeFactStageV3::local_precondition) noexcept {
  if ((reason == "callback_result_unknown" || reason == "callback_output_impossible") &&
      stage != BlobLifetimeFactStageV3::callback_return &&
      stage != BlobLifetimeFactStageV3::cleanup_return)
    return RuntimeTerminated();
  auto result = RuntimeFailure("CINL.LOB.DESCRIPTOR_INVALID", reason);
  result.fact.stage = stage;
  result.fact.parameters.present = blob_parameter_descriptor_identity;
  result.fact.parameters.descriptor_uuid = descriptor_uuid;
  result.fact.parameters.descriptor_generation = descriptor_generation;
  return result;
}

BlobLifetimeRuntimeResultV3 ConcurrentUseFailure(
    const platform::Uuid& descriptor_uuid,
    u64 descriptor_generation) noexcept {
  auto result = ProtocolFailure("same_ticket_concurrent_use", descriptor_uuid,
                                descriptor_generation);
  result.fact.authority_source =
      BlobLifetimeAuthoritySourceV3::datatype_local_state;
  result.fact.stage = BlobLifetimeFactStageV3::local_precondition;
  return result;
}

BlobLifetimeRuntimeResultV3 CanonicalConcurrentUseFailure() noexcept {
  const auto* canonical = CurrentBlobIdentity();
  if (canonical == nullptr) return RuntimeTerminated();
  return ConcurrentUseFailure(canonical->legacy_fields.descriptor_uuid,
                              canonical->legacy_fields.descriptor_generation);
}

BlobLifetimeRuntimeResultV3 DeferredInvariantPrimary(
    u64 packed, const platform::Uuid& descriptor_uuid,
    u64 descriptor_generation) noexcept {
  const auto event = DeferredEvent(packed);
  if (event == BlobLifetimeInvariantEventV3::callback_reentrant)
    return ProtocolFailure("callback_reentrant", descriptor_uuid,
                           descriptor_generation);
  if (event == BlobLifetimeInvariantEventV3::same_ticket_concurrent_use)
    return ConcurrentUseFailure(descriptor_uuid, descriptor_generation);
  return RuntimeTerminated();
}

BlobLifetimeRuntimeResultV3 CanonicalProtocolFailure(
    std::string_view reason) noexcept {
  const auto* canonical = CurrentBlobIdentity();
  if (canonical == nullptr) return RuntimeTerminated();
  return ProtocolFailure(reason, canonical->legacy_fields.descriptor_uuid,
                         canonical->legacy_fields.descriptor_generation);
}

void ValidatePinReplyOrTerminate(const BlobReceiverPinCloneResultV3& reply) noexcept {
  if (!AllZero(reply.reserved)) std::terminate();
  switch (reply.status) {
    case BlobReceiverPinAcquireStatusV3::acquired:
      if (reply.pin_cookie == 0) std::terminate();
      return;
    case BlobReceiverPinAcquireStatusV3::draining:
    case BlobReceiverPinAcquireStatusV3::unavailable:
      if (reply.pin_cookie != 0) std::terminate();
      return;
    case BlobReceiverPinAcquireStatusV3::fatal:
      std::terminate();
  }
  std::terminate();
}

BlobLifetimeRuntimeResultV3 PinFailure(
    BlobReceiverPinAcquireStatusV3 status, u8 operation) noexcept {
  const bool draining = status == BlobReceiverPinAcquireStatusV3::draining;
  auto failure = RuntimeFailure("BLOB.LIFETIME_AUTHORITY_UNAVAILABLE",
      draining ? "shutdown_drain_violation" : "receiver_services_failed");
  failure.fact.authority_source = draining
      ? BlobLifetimeAuthoritySourceV3::admission_closed_or_drain
      : BlobLifetimeAuthoritySourceV3::pin_host_failure;
  failure.fact.parameters.present = blob_parameter_phase | blob_parameter_operation;
  failure.fact.parameters.phase_enum = BlobLifetimePublicPhaseV3::retain;
  failure.fact.parameters.operation_enum = operation;
  return failure;
}

BlobLifetimeRuntimeResultV3 BudgetFailure() noexcept {
  return RuntimeFailure("RESOURCE.BUDGET_EXCEEDED",
                        "lifetime_callback_calls", ResourceStatus());
}

BlobLifetimeRuntimeResultV3 BudgetFailure(BlobLifetimeResourceV3 resource,
                                          u64 required,
                                          u64 available) noexcept {
  auto result = BudgetFailure();
  result.resource = resource;
  result.required = required;
  result.available = available;
  result.fact.parameters.present |= blob_parameter_resource |
                                     blob_parameter_required |
                                     blob_parameter_available;
  result.fact.parameters.resource_enum = resource;
  result.fact.parameters.required_u64 = required;
  result.fact.parameters.available_u64 = available;
  return result;
}

BlobLifetimeRuntimeResultV3 CallbackFailure(
    u8 code, BlobLifetimePhaseV3 phase,
    const BlobLifetimeUseRequestV3& request,
    const platform::Uuid& descriptor_uuid,
    u64 descriptor_generation) noexcept {
  BlobLifetimeRuntimeResultV3 result;
  switch (code) {
    case SB_BLOB_CALLBACK_INVALID_ARGUMENT_V3:
      result = RuntimeFailure("CINL.LOB.DESCRIPTOR_INVALID",
                            "callback_output_impossible");
      break;
    case SB_BLOB_CALLBACK_UNKNOWN_TOKEN_V3:
      result = RuntimeFailure("CINL.LOB.HANDLE_EXPIRED", "unknown_token"); break;
    case SB_BLOB_CALLBACK_WRONG_AUTHORITY_V3:
      result = RuntimeFailure("CINL.LOB.DESCRIPTOR_INVALID",
                            "wrong_authority_instance");
      break;
    case SB_BLOB_CALLBACK_SECURITY_DENIED_V3:
      result = RuntimeFailure("SECURITY.ACCESS_DENIED", "security_denied"); break;
    case SB_BLOB_CALLBACK_WRONG_OWNER_V3:
      result = RuntimeFailure("BLOB.HANDLE_OWNER_MISMATCH", "wrong_owner"); break;
    case SB_BLOB_CALLBACK_WRONG_TRANSACTION_V3:
      result = RuntimeFailure("CINL.LOB.DESCRIPTOR_INVALID",
                            "transaction_mismatch");
      break;
    case SB_BLOB_CALLBACK_WRONG_SNAPSHOT_V3:
      result = RuntimeFailure("CINL.LOB.HANDLE_EXPIRED", "wrong_snapshot"); break;
    case SB_BLOB_CALLBACK_STALE_SNAPSHOT_V3:
      result = RuntimeFailure("CINL.LOB.HANDLE_EXPIRED", "stale_snapshot"); break;
    case SB_BLOB_CALLBACK_EXPIRED_V3:
      result = RuntimeFailure("CINL.LOB.HANDLE_EXPIRED", "monotonic_expiry"); break;
    case SB_BLOB_CALLBACK_REVOKED_V3:
      result = RuntimeFailure("CINL.LOB.HANDLE_EXPIRED", "revoked"); break;
    case SB_BLOB_CALLBACK_WRONG_MODE_V3:
      result = RuntimeFailure("BLOB.HANDLE_MODE_REFUSED", "wrong_mode"); break;
    case SB_BLOB_CALLBACK_CLOSED_V3:
      result = RuntimeFailure("CINL.LOB.HANDLE_EXPIRED", "closed"); break;
    case SB_BLOB_CALLBACK_RETAINED_TICKET_INVALID_V3:
      result = phase == BlobLifetimePhaseV3::retain
          ? RuntimeFailure("CINL.LOB.DESCRIPTOR_INVALID",
                           "callback_output_impossible")
          : RuntimeFailure("CINL.LOB.HANDLE_EXPIRED",
                           "retained_ticket_invalid");
      break;
    case SB_BLOB_CALLBACK_RETAINED_TICKET_CONSUMED_V3:
      result = phase == BlobLifetimePhaseV3::retain
          ? RuntimeFailure("CINL.LOB.DESCRIPTOR_INVALID",
                           "callback_output_impossible")
          : RuntimeFailure("CINL.LOB.HANDLE_EXPIRED",
                           "retained_ticket_consumed");
      break;
    case SB_BLOB_CALLBACK_ACCESS_TICKET_INVALID_V3:
      result = RuntimeFailure("CINL.LOB.DESCRIPTOR_INVALID",
          phase == BlobLifetimePhaseV3::end_access
              ? "access_ticket_invalid" : "callback_output_impossible");
      break;
    case SB_BLOB_CALLBACK_ACCESS_TICKET_CONSUMED_V3:
      result = RuntimeFailure("CINL.LOB.DESCRIPTOR_INVALID",
          phase == BlobLifetimePhaseV3::end_access
              ? "access_ticket_consumed" : "callback_output_impossible");
      break;
    case SB_BLOB_CALLBACK_RESOURCE_EXHAUSTED_V3:
      result = (phase == BlobLifetimePhaseV3::end_access ||
                phase == BlobLifetimePhaseV3::release ||
                phase == BlobLifetimePhaseV3::destructor_cleanup)
          ? RuntimeFailure("CINL.LOB.DESCRIPTOR_INVALID",
                           phase == BlobLifetimePhaseV3::end_access
                               ? "end_access_failed" : "release_failed")
          : RuntimeFailure("BLOB.LIFETIME_AUTHORITY_UNAVAILABLE",
                           "authority_capacity_exhausted");
      break;
    case SB_BLOB_CALLBACK_AUTHORITY_UNAVAILABLE_V3:
      result = (phase == BlobLifetimePhaseV3::end_access ||
                phase == BlobLifetimePhaseV3::release ||
                phase == BlobLifetimePhaseV3::destructor_cleanup)
          ? RuntimeFailure("CINL.LOB.DESCRIPTOR_INVALID",
                           phase == BlobLifetimePhaseV3::end_access
                               ? "end_access_failed" : "release_failed")
          : RuntimeFailure("BLOB.LIFETIME_AUTHORITY_UNAVAILABLE",
                           "authority_runtime_unavailable");
      break;
    default:
      result = RuntimeFailure("CINL.LOB.DESCRIPTOR_INVALID",
                            "callback_result_unknown");
      break;
  }
  result.callback_code = code;
  result.fact.stage = (phase == BlobLifetimePhaseV3::end_access ||
                       phase == BlobLifetimePhaseV3::release ||
                       phase == BlobLifetimePhaseV3::destructor_cleanup)
      ? BlobLifetimeFactStageV3::cleanup_return
      : BlobLifetimeFactStageV3::callback_return;
  result.fact.gate_present = false;
  switch (code) {
    case SB_BLOB_CALLBACK_UNKNOWN_TOKEN_V3:
      result.fact.parameters.present |= blob_parameter_lifetime |
                                         blob_parameter_expiry_reason;
      result.fact.parameters.lifetime_enum =
          BlobLifetimeDiagnosticLifetimeV3::unknown_token;
      result.fact.parameters.expiry_reason_enum =
          BlobLifetimeDiagnosticExpiryReasonV3::unknown_token;
      break;
    case SB_BLOB_CALLBACK_WRONG_SNAPSHOT_V3:
      result.fact.parameters.present |= blob_parameter_lifetime |
                                         blob_parameter_expiry_reason;
      result.fact.parameters.lifetime_enum =
          BlobLifetimeDiagnosticLifetimeV3::wrong_snapshot;
      result.fact.parameters.expiry_reason_enum =
          BlobLifetimeDiagnosticExpiryReasonV3::snapshot_identity_mismatch;
      break;
    case SB_BLOB_CALLBACK_STALE_SNAPSHOT_V3:
      result.fact.parameters.present |= blob_parameter_lifetime |
                                         blob_parameter_expiry_reason;
      result.fact.parameters.lifetime_enum =
          BlobLifetimeDiagnosticLifetimeV3::stale_snapshot;
      result.fact.parameters.expiry_reason_enum =
          BlobLifetimeDiagnosticExpiryReasonV3::snapshot_no_longer_current;
      break;
    case SB_BLOB_CALLBACK_EXPIRED_V3:
      result.fact.parameters.present |= blob_parameter_lifetime |
                                         blob_parameter_expiry_reason;
      result.fact.parameters.lifetime_enum =
          BlobLifetimeDiagnosticLifetimeV3::token_expired;
      result.fact.parameters.expiry_reason_enum =
          BlobLifetimeDiagnosticExpiryReasonV3::monotonic_expiry;
      break;
    case SB_BLOB_CALLBACK_REVOKED_V3:
      result.fact.parameters.present |= blob_parameter_lifetime |
                                         blob_parameter_expiry_reason;
      result.fact.parameters.lifetime_enum =
          BlobLifetimeDiagnosticLifetimeV3::token_revoked;
      result.fact.parameters.expiry_reason_enum =
          BlobLifetimeDiagnosticExpiryReasonV3::explicit_revocation;
      break;
    case SB_BLOB_CALLBACK_CLOSED_V3:
      result.fact.parameters.present |= blob_parameter_lifetime |
                                         blob_parameter_expiry_reason;
      result.fact.parameters.lifetime_enum =
          BlobLifetimeDiagnosticLifetimeV3::owner_closed;
      result.fact.parameters.expiry_reason_enum =
          BlobLifetimeDiagnosticExpiryReasonV3::owner_closed;
      break;
    case SB_BLOB_CALLBACK_RETAINED_TICKET_INVALID_V3:
      if (phase != BlobLifetimePhaseV3::retain) {
        result.fact.parameters.present |= blob_parameter_lifetime |
                                           blob_parameter_expiry_reason;
        result.fact.parameters.lifetime_enum =
            BlobLifetimeDiagnosticLifetimeV3::retain_ticket_invalid;
        result.fact.parameters.expiry_reason_enum =
            BlobLifetimeDiagnosticExpiryReasonV3::retain_ticket_unknown;
      }
      break;
    case SB_BLOB_CALLBACK_RETAINED_TICKET_CONSUMED_V3:
      if (phase != BlobLifetimePhaseV3::retain) {
        result.fact.parameters.present |= blob_parameter_lifetime |
                                           blob_parameter_expiry_reason;
        result.fact.parameters.lifetime_enum =
            BlobLifetimeDiagnosticLifetimeV3::retain_ticket_consumed;
        result.fact.parameters.expiry_reason_enum =
            BlobLifetimeDiagnosticExpiryReasonV3::retain_ticket_consumed;
      }
      break;
    default:
      break;
  }
  if (result.fact.diagnostic ==
      BlobLifetimeDiagnosticCodeV3::descriptor_invalid) {
    result.fact.parameters.present |= blob_parameter_descriptor_identity;
    result.fact.parameters.descriptor_uuid = descriptor_uuid;
    result.fact.parameters.descriptor_generation = descriptor_generation;
  } else if (result.fact.diagnostic ==
             BlobLifetimeDiagnosticCodeV3::handle_owner_mismatch) {
    result.fact.parameters.present |= blob_parameter_owner_class;
    result.fact.parameters.owner_class_enum = request.required_owner_class;
  } else if (result.fact.diagnostic ==
             BlobLifetimeDiagnosticCodeV3::handle_mode_refused) {
    result.fact.parameters.present |= blob_parameter_open_mode |
                                      blob_parameter_operation;
    result.fact.parameters.open_mode_enum = 4;
    result.fact.parameters.operation_enum = request.operation;
  }
  if (code == SB_BLOB_CALLBACK_RESOURCE_EXHAUSTED_V3 ||
      code == SB_BLOB_CALLBACK_AUTHORITY_UNAVAILABLE_V3) {
    result.fact.authority_source =
        code == SB_BLOB_CALLBACK_RESOURCE_EXHAUSTED_V3
            ? BlobLifetimeAuthoritySourceV3::callback_code_17
            : BlobLifetimeAuthoritySourceV3::callback_code_18;
    const bool cleanup = phase == BlobLifetimePhaseV3::end_access ||
        phase == BlobLifetimePhaseV3::release ||
        phase == BlobLifetimePhaseV3::destructor_cleanup;
    if (cleanup) {
      result.fact.fact_class = BlobLifetimeFactClassV3::cleanup_protocol;
    } else {
      result.fact.parameters.present |= blob_parameter_operation |
                                        blob_parameter_phase;
      result.fact.parameters.operation_enum = request.operation;
    }
    if (!cleanup) switch (phase) {
      case BlobLifetimePhaseV3::retain:
        result.fact.parameters.phase_enum = BlobLifetimePublicPhaseV3::retain;
        break;
      case BlobLifetimePhaseV3::probe:
        result.fact.parameters.phase_enum = BlobLifetimePublicPhaseV3::probe;
        break;
      case BlobLifetimePhaseV3::begin_access:
        result.fact.parameters.phase_enum =
            BlobLifetimePublicPhaseV3::begin_access;
        break;
      case BlobLifetimePhaseV3::end_access:
        result.fact.parameters.phase_enum =
            BlobLifetimePublicPhaseV3::end_access;
        break;
      default:
        result.fact.parameters.phase_enum = BlobLifetimePublicPhaseV3::release;
        break;
    }
  }
  return result;
}

bool CallbackRequiresQuarantine(BlobLifetimePhaseV3 phase, u8 code) noexcept {
  if (code == SB_BLOB_CALLBACK_OK_V3) return false;
  if (phase == BlobLifetimePhaseV3::end_access ||
      phase == BlobLifetimePhaseV3::release ||
      phase == BlobLifetimePhaseV3::destructor_cleanup) return true;
  if (phase == BlobLifetimePhaseV3::retain)
    return code == 1 || (code >= 13 && code <= 16);
  return code == 1 || code == 15 || code == 16;
}

enum class BlobFactoryStructuralClassV3 : u8 {
  valid = 0,
  abi_invalid,
  token_shape_invalid,
  token_identity_invalid,
  authority_instance_mismatch,
  owner_mismatch,
  transaction_mismatch,
  mode_mismatch,
};

BlobFactoryStructuralClassV3 ClassifyFactoryStructure(
    const BlobLifetimeAuthorityV3& authority,
    const BlobLifetimeTokenV3& token,
    const BlobLifetimeUseRequestV3& request,
    const BlobMaterializedCarrierBindingV3& carrier) noexcept {
  if (authority.struct_bytes != sizeof(BlobLifetimeAuthorityV3) ||
      authority.abi_major != SB_BLOB_LIFETIME_ABI_MAJOR_V3 ||
      authority.abi_minor != SB_BLOB_LIFETIME_ABI_MINOR_V3) {
    return BlobFactoryStructuralClassV3::abi_invalid;
  }
  const auto carrier_modes = token.mode_bits &
      (SB_BLOB_MODE_MATERIALIZED_BYTES_V3 | SB_BLOB_MODE_LOGICAL_READER_V3 |
       SB_BLOB_MODE_OPAQUE_ENCODED_READER_V3);
  if (token.struct_bytes != sizeof(BlobLifetimeTokenV3) ||
      token.abi_major != SB_BLOB_LIFETIME_ABI_MAJOR_V3 ||
      token.abi_minor != SB_BLOB_LIFETIME_ABI_MINOR_V3 ||
      request.struct_bytes != sizeof(BlobLifetimeUseRequestV3) ||
      request.abi_major != SB_BLOB_LIFETIME_ABI_MAJOR_V3 ||
      request.abi_minor != SB_BLOB_LIFETIME_ABI_MINOR_V3 ||
      !AllZero(token.reserved_33_39) || !AllZero(token.reserved_196_199) ||
      !AllZero(request.reserved_10_11) ||
      token.owner_class < SB_BLOB_OWNER_EXECUTION_BORROW_V3 ||
      token.owner_class > SB_BLOB_OWNER_ENCODED_ARTIFACT_BORROW_V3 ||
      request.required_owner_class < SB_BLOB_OWNER_EXECUTION_BORROW_V3 ||
      request.required_owner_class > SB_BLOB_OWNER_ENCODED_ARTIFACT_BORROW_V3 ||
      request.operation < SB_BLOB_OP_VALIDATE_VIEW_V3 ||
      request.operation > SB_BLOB_OP_RESTORE_V3 ||
      token.mode_bits == 0 || request.required_mode_bits == 0 ||
      (token.mode_bits & ~SB_BLOB_MODE_KNOWN_MASK_V3) != 0 ||
      (request.required_mode_bits & ~SB_BLOB_MODE_KNOWN_MASK_V3) != 0 ||
      carrier_modes == 0 || (carrier_modes & (carrier_modes - 1)) != 0) {
    return BlobFactoryStructuralClassV3::token_shape_invalid;
  }
  const bool session_nil = BlobUuidIsNil(token.session_uuid);
  const bool statement_nil = BlobUuidIsNil(token.statement_uuid);
  if (BlobUuidIsNil(token.lifetime_token_uuid) ||
      BlobUuidIsNil(token.authority_instance_uuid) ||
      BlobUuidIsNil(token.database_uuid) ||
      BlobUuidIsNil(token.transaction_uuid) ||
      BlobUuidIsNil(token.snapshot_uuid) ||
      BlobUuidIsNil(token.security_context_uuid) ||
      token.lifetime_token_generation == 0 ||
      token.authority_instance_generation == 0 ||
      token.snapshot_generation == 0 || token.security_generation == 0 ||
      token.immutable_binding_generation == 0 ||
      token.lifetime_token_generation != token.snapshot_generation ||
      session_nil != statement_nil ||
      (token.owner_class == SB_BLOB_OWNER_EXECUTION_BORROW_V3 &&
       (session_nil || statement_nil))) {
    return BlobFactoryStructuralClassV3::token_shape_invalid;
  }
  if (!BlobUuidEqual(request.expected_database_uuid, token.database_uuid) ||
      !BlobUuidEqual(request.expected_session_uuid, token.session_uuid) ||
      !BlobUuidEqual(request.expected_statement_uuid, token.statement_uuid) ||
      !BlobUuidEqual(request.expected_snapshot_uuid, token.snapshot_uuid) ||
      request.expected_snapshot_generation != token.snapshot_generation ||
      !BlobUuidEqual(request.expected_security_context_uuid,
                     token.security_context_uuid) ||
      request.expected_security_generation != token.security_generation ||
      !PlatformEqualsBlob(carrier.lifetime_token_uuid,
                          token.lifetime_token_uuid) ||
      carrier.lifetime_token_generation != token.lifetime_token_generation ||
      carrier.immutable_binding_generation !=
          token.immutable_binding_generation ||
      PlatformUuidIsNil(carrier.carrier_uuid) ||
      carrier.carrier_generation == 0) {
    return BlobFactoryStructuralClassV3::token_identity_invalid;
  }
  if (BlobUuidIsNil(authority.authority_instance_uuid) ||
      !BlobUuidEqual(authority.authority_instance_uuid,
                     token.authority_instance_uuid) ||
      authority.authority_instance_generation !=
          token.authority_instance_generation ||
      !BlobUuidEqual(request.expected_authority_instance_uuid,
                     token.authority_instance_uuid) ||
      request.expected_authority_instance_generation !=
          token.authority_instance_generation ||
      !PlatformEqualsBlob(carrier.authority_instance_uuid,
                          token.authority_instance_uuid) ||
      carrier.authority_instance_generation !=
          token.authority_instance_generation) {
    return BlobFactoryStructuralClassV3::authority_instance_mismatch;
  }
  if (request.required_owner_class != token.owner_class)
    return BlobFactoryStructuralClassV3::owner_mismatch;
  if (!BlobUuidEqual(request.expected_transaction_uuid,
                     token.transaction_uuid))
    return BlobFactoryStructuralClassV3::transaction_mismatch;
  const bool required_modes_present =
      (token.mode_bits & request.required_mode_bits) ==
          request.required_mode_bits;
  const bool materialized_modes =
      (token.mode_bits & SB_BLOB_MODE_IMMUTABLE_READ_V3) != 0 &&
      (token.mode_bits & SB_BLOB_MODE_MATERIALIZED_BYTES_V3) != 0 &&
      (token.mode_bits & (SB_BLOB_MODE_REPLAYABLE_SAME_SNAPSHOT_V3 |
                          SB_BLOB_MODE_LOGICAL_READER_V3 |
                          SB_BLOB_MODE_OPAQUE_ENCODED_READER_V3)) == 0;
  const bool materialized_owner =
      token.owner_class != SB_BLOB_OWNER_ENCODED_ARTIFACT_BORROW_V3;
  const bool materialized_operation =
      request.operation == SB_BLOB_OP_VALIDATE_VIEW_V3 ||
      request.operation == SB_BLOB_OP_READ_V3 ||
      request.operation == SB_BLOB_OP_MATERIALIZE_V3 ||
      request.operation == SB_BLOB_OP_HASH_V3 ||
      request.operation == SB_BLOB_OP_COMPARE_V3 ||
      request.operation == SB_BLOB_OP_INTRINSIC_TRANSFORM_V3;
  if (carrier.carrier_kind != SB_BLOB_MODE_MATERIALIZED_BYTES_V3 ||
      !required_modes_present || !materialized_modes || !materialized_owner ||
      !materialized_operation)
    return BlobFactoryStructuralClassV3::mode_mismatch;
  return BlobFactoryStructuralClassV3::valid;
}

bool TokenAndRequestShapeValid(const BlobLifetimeAuthorityV3& authority,
                               const BlobLifetimeTokenV3& token,
                               const BlobLifetimeUseRequestV3& request,
                               const BlobMaterializedCarrierBindingV3& carrier)
    noexcept {
  if (ClassifyFactoryStructure(authority, token, request, carrier) !=
      BlobFactoryStructuralClassV3::valid) return false;
  if (authority.struct_bytes != sizeof(BlobLifetimeAuthorityV3) ||
      authority.abi_major != SB_BLOB_LIFETIME_ABI_MAJOR_V3 ||
      authority.abi_minor != SB_BLOB_LIFETIME_ABI_MINOR_V3 ||
      BlobUuidIsNil(authority.authority_instance_uuid) ||
      authority.authority_instance_generation == 0 ||
      authority.retain == nullptr || authority.probe == nullptr ||
      authority.begin_access == nullptr || authority.end_access == nullptr ||
      authority.release == nullptr ||
      token.struct_bytes != sizeof(BlobLifetimeTokenV3) ||
      token.abi_major != SB_BLOB_LIFETIME_ABI_MAJOR_V3 ||
      token.abi_minor != SB_BLOB_LIFETIME_ABI_MINOR_V3 ||
      request.struct_bytes != sizeof(BlobLifetimeUseRequestV3) ||
      request.abi_major != SB_BLOB_LIFETIME_ABI_MAJOR_V3 ||
      request.abi_minor != SB_BLOB_LIFETIME_ABI_MINOR_V3 ||
      !AllZero(token.reserved_33_39) || !AllZero(token.reserved_196_199) ||
      !AllZero(request.reserved_10_11) ||
      token.owner_class < SB_BLOB_OWNER_EXECUTION_BORROW_V3 ||
      token.owner_class > SB_BLOB_OWNER_ENCODED_ARTIFACT_BORROW_V3 ||
      request.operation < SB_BLOB_OP_VALIDATE_VIEW_V3 ||
      request.operation > SB_BLOB_OP_RESTORE_V3 ||
      request.required_owner_class != token.owner_class ||
      token.mode_bits == 0 || request.required_mode_bits == 0 ||
      (token.mode_bits & ~SB_BLOB_MODE_KNOWN_MASK_V3) != 0 ||
      (request.required_mode_bits & ~SB_BLOB_MODE_KNOWN_MASK_V3) != 0 ||
      (token.mode_bits & request.required_mode_bits) != request.required_mode_bits ||
      (token.mode_bits & SB_BLOB_MODE_IMMUTABLE_READ_V3) == 0 ||
      (token.mode_bits & SB_BLOB_MODE_MATERIALIZED_BYTES_V3) == 0 ||
      (token.mode_bits & (SB_BLOB_MODE_LOGICAL_READER_V3 |
                          SB_BLOB_MODE_OPAQUE_ENCODED_READER_V3)) != 0 ||
      BlobUuidIsNil(token.lifetime_token_uuid) ||
      BlobUuidIsNil(token.authority_instance_uuid) ||
      BlobUuidIsNil(token.database_uuid) ||
      BlobUuidIsNil(token.transaction_uuid) ||
      BlobUuidIsNil(token.snapshot_uuid) ||
      BlobUuidIsNil(token.security_context_uuid) ||
      token.lifetime_token_generation == 0 ||
      token.authority_instance_generation == 0 || token.snapshot_generation == 0 ||
      token.security_generation == 0 || token.immutable_binding_generation == 0 ||
      token.lifetime_token_generation != token.snapshot_generation ||
      !BlobUuidEqual(authority.authority_instance_uuid,
                     token.authority_instance_uuid) ||
      authority.authority_instance_generation !=
          token.authority_instance_generation ||
      !BlobUuidEqual(request.expected_authority_instance_uuid,
                     token.authority_instance_uuid) ||
      request.expected_authority_instance_generation !=
          token.authority_instance_generation ||
      !BlobUuidEqual(request.expected_database_uuid, token.database_uuid) ||
      !BlobUuidEqual(request.expected_session_uuid, token.session_uuid) ||
      !BlobUuidEqual(request.expected_statement_uuid, token.statement_uuid) ||
      !BlobUuidEqual(request.expected_transaction_uuid, token.transaction_uuid) ||
      !BlobUuidEqual(request.expected_snapshot_uuid, token.snapshot_uuid) ||
      request.expected_snapshot_generation != token.snapshot_generation ||
      !BlobUuidEqual(request.expected_security_context_uuid,
                     token.security_context_uuid) ||
      request.expected_security_generation != token.security_generation ||
      !PlatformEqualsBlob(carrier.authority_instance_uuid,
                          token.authority_instance_uuid) ||
      carrier.authority_instance_generation !=
          token.authority_instance_generation ||
      !PlatformEqualsBlob(carrier.lifetime_token_uuid,
                          token.lifetime_token_uuid) ||
      carrier.lifetime_token_generation != token.lifetime_token_generation ||
      carrier.immutable_binding_generation !=
          token.immutable_binding_generation ||
      PlatformUuidIsNil(carrier.carrier_uuid) || carrier.carrier_generation == 0 ||
      carrier.carrier_kind != SB_BLOB_MODE_MATERIALIZED_BYTES_V3) {
    return false;
  }
  const bool session_nil = BlobUuidIsNil(token.session_uuid);
  const bool statement_nil = BlobUuidIsNil(token.statement_uuid);
  if (session_nil != statement_nil) return false;
  if (token.owner_class == SB_BLOB_OWNER_EXECUTION_BORROW_V3 &&
      (session_nil || statement_nil)) return false;
  if (token.owner_class == SB_BLOB_OWNER_ENCODED_ARTIFACT_BORROW_V3)
    return false;
  switch (request.operation) {
    case SB_BLOB_OP_VALIDATE_VIEW_V3:
    case SB_BLOB_OP_READ_V3:
    case SB_BLOB_OP_MATERIALIZE_V3:
    case SB_BLOB_OP_HASH_V3:
    case SB_BLOB_OP_COMPARE_V3:
    case SB_BLOB_OP_INTRINSIC_TRANSFORM_V3:
      return true;
    default:
      return false;
  }
}

bool RetainTicketValid(const BlobLifetimeRetainTicketV3& ticket,
                       const BlobLifetimeTokenV3& token) noexcept {
  return !BlobUuidIsNil(ticket.retain_ticket_uuid) &&
         !BlobUuidEqual(ticket.retain_ticket_uuid, token.lifetime_token_uuid) &&
         BlobUuidEqual(ticket.lifetime_token_uuid, token.lifetime_token_uuid) &&
         ticket.lifetime_token_generation == token.lifetime_token_generation &&
         ticket.immutable_binding_generation == token.immutable_binding_generation;
}

bool RetainTicketEqual(const BlobLifetimeRetainTicketV3& left,
                       const BlobLifetimeRetainTicketV3& right) noexcept {
  return BlobUuidEqual(left.retain_ticket_uuid, right.retain_ticket_uuid) &&
         BlobUuidEqual(left.lifetime_token_uuid, right.lifetime_token_uuid) &&
         left.lifetime_token_generation == right.lifetime_token_generation &&
         left.immutable_binding_generation ==
             right.immutable_binding_generation;
}

bool AccessTicketValid(const BlobLifetimeAccessTicketV3& ticket,
                       const BlobLifetimeRetainTicketV3& retain,
                       const BlobLifetimeTokenV3& token) noexcept {
  return !BlobUuidIsNil(ticket.access_ticket_uuid) &&
         !BlobUuidEqual(ticket.access_ticket_uuid, token.lifetime_token_uuid) &&
         !BlobUuidEqual(ticket.access_ticket_uuid, retain.retain_ticket_uuid) &&
         BlobUuidEqual(ticket.retain_ticket_uuid, retain.retain_ticket_uuid) &&
         ticket.lifetime_token_generation == token.lifetime_token_generation &&
         ticket.immutable_binding_generation == token.immutable_binding_generation;
}

}  // namespace

BlobLifetimeRuntimeResultV3
BlobRetainedLifetimeLeaseV3::BuildFailureForConformance(
    std::string_view diagnostic_code,
    std::string_view reason_selector) noexcept {
  return RuntimeFailure(diagnostic_code, reason_selector);
}

bool ValidateBlobLifetimeInvariantFactDomainV3(
    const BlobLifetimeBindingKeyV3& key,
    const BlobLifetimeInvariantFactV3& fact) noexcept {
  return InvariantFactDomainValid(key, fact);
}

bool BlobLifetimeBudgetLedgerV3Generation1::Configure(
    const BlobLifetimeBudgetConfigurationV3Generation1& configuration)
    noexcept {
  while (lock_.test_and_set(std::memory_order_acquire)) {}
  const bool pristine = !configured_.load(std::memory_order_relaxed) &&
      !frozen_.load(std::memory_order_relaxed) &&
      !terminal_quarantined_.load(std::memory_order_relaxed);
  if (pristine) {
    for (std::size_t i = 0; i < configuration.limits.size(); ++i) {
      counters_[i + 1] = {configuration.limits[i], 0, 0};
    }
    scratch_ = configuration.scratch;
    admitted_window_octets_ = configuration.admitted_window_octets;
    admitted_relative_timeout_ns_ =
        configuration.admitted_relative_timeout_ns;
    configured_.store(true, std::memory_order_release);
  }
  lock_.clear(std::memory_order_release);
  return pristine;
}

bool BlobLifetimeBudgetLedgerV3Generation1::Freeze() noexcept {
  while (lock_.test_and_set(std::memory_order_acquire)) {}
  const bool may_freeze = configured_.load(std::memory_order_relaxed) &&
      !frozen_.load(std::memory_order_relaxed) &&
      !terminal_quarantined_.load(std::memory_order_relaxed);
  if (may_freeze) frozen_.store(true, std::memory_order_release);
  lock_.clear(std::memory_order_release);
  return may_freeze;
}

BlobLifetimeBudgetSnapshotV3Generation1
BlobLifetimeBudgetLedgerV3Generation1::SnapshotForConformance() noexcept {
  while (lock_.test_and_set(std::memory_order_acquire)) {}
  BlobLifetimeBudgetSnapshotV3Generation1 snapshot{};
  snapshot.configured = configured_.load(std::memory_order_relaxed);
  snapshot.frozen = frozen_.load(std::memory_order_relaxed);
  snapshot.terminal_quarantined =
      terminal_quarantined_.load(std::memory_order_relaxed);
  snapshot.counters = counters_;
  snapshot.scratch_data = scratch_.data();
  snapshot.scratch_size = static_cast<u64>(scratch_.size());
  snapshot.admitted_window_octets = admitted_window_octets_;
  snapshot.admitted_relative_timeout_ns = admitted_relative_timeout_ns_;
  lock_.clear(std::memory_order_release);
  return snapshot;
}

bool BlobLifetimeBudgetLedgerV3Generation1::ReserveOrdinaryAndCleanup(
    u64 ordinary, u64 cleanup, u64* available_before) noexcept {
  while (lock_.test_and_set(std::memory_order_acquire)) {}
  auto& counter = counters_[static_cast<std::size_t>(
      BlobLifetimeResourceV3::lifetime_callback_calls)];
  const bool valid = frozen_.load(std::memory_order_relaxed) &&
      !terminal_quarantined_.load(std::memory_order_relaxed) &&
      counter.invoked <= counter.limit &&
      counter.reserved_cleanup <= counter.limit - counter.invoked;
  if (frozen_.load(std::memory_order_relaxed) && !valid)
    terminal_quarantined_.store(true, std::memory_order_release);
  const u64 available = valid
      ? counter.limit - counter.invoked - counter.reserved_cleanup : 0;
  if (available_before != nullptr) *available_before = available;
  const bool admitted = valid && ordinary <= available &&
                        cleanup <= available - ordinary;
  if (admitted) {
    counter.invoked += ordinary;
    counter.reserved_cleanup += cleanup;
  }
  lock_.clear(std::memory_order_release);
  return admitted;
}

bool BlobLifetimeBudgetLedgerV3Generation1::ChargeOrdinary(
    u64 amount, u64* available_before) noexcept {
  return ReserveOrdinaryAndCleanup(amount, 0, available_before);
}

bool BlobLifetimeBudgetLedgerV3Generation1::Charge(
    BlobLifetimeResourceV3 resource, u64 amount,
    u64* available_before) noexcept {
  const auto index = static_cast<std::size_t>(resource);
  if (index == 0 || index >= counters_.size()) return false;
  while (lock_.test_and_set(std::memory_order_acquire)) {}
  auto& counter = counters_[index];
  const bool valid = frozen_.load(std::memory_order_relaxed) &&
      !terminal_quarantined_.load(std::memory_order_relaxed) &&
      counter.invoked <= counter.limit &&
      counter.reserved_cleanup <= counter.limit - counter.invoked;
  if (frozen_.load(std::memory_order_relaxed) && !valid)
    terminal_quarantined_.store(true, std::memory_order_release);
  const u64 available = valid
      ? counter.limit - counter.invoked - counter.reserved_cleanup : 0;
  if (available_before != nullptr) *available_before = available;
  const bool admitted = valid && amount <= available;
  if (admitted) counter.invoked += amount;
  lock_.clear(std::memory_order_release);
  return admitted;
}

BlobCleanupReservationConsumptionV3
BlobLifetimeBudgetLedgerV3Generation1::ConsumeCleanup(u64 amount) noexcept {
  while (lock_.test_and_set(std::memory_order_acquire)) {}
  auto& counter = counters_[static_cast<std::size_t>(
      BlobLifetimeResourceV3::lifetime_callback_calls)];
  // The reservation is the cleanup authority. Consume it before invocation
  // even if the numeric ledger is already corrupt. Never retry or recreate it.
  const bool obligation_exists = frozen_.load(std::memory_order_relaxed) &&
      amount <= counter.reserved_cleanup;
  if (obligation_exists) counter.reserved_cleanup -= amount;
  const bool numeric_charge_can_commit = obligation_exists &&
      !terminal_quarantined_.load(std::memory_order_relaxed) &&
      counter.invoked <= counter.limit && amount <= counter.limit - counter.invoked &&
      counter.reserved_cleanup <= counter.limit - counter.invoked - amount;
  if (numeric_charge_can_commit) {
    counter.invoked += amount;
  } else {
    // Preserve invoked exactly. The actual cleanup is evidenced independently
    // by the callback trace owned by the receiver; this ledger is never
    // admitted again.
    terminal_quarantined_.store(true, std::memory_order_release);
  }
  const BlobCleanupReservationConsumptionV3 result{
      obligation_exists, numeric_charge_can_commit,
      terminal_quarantined_.load(std::memory_order_relaxed)};
  lock_.clear(std::memory_order_release);
  return result;
}

bool BlobLifetimeBudgetLedgerV3Generation1::CancelCleanup(u64 amount) noexcept {
  while (lock_.test_and_set(std::memory_order_acquire)) {}
  auto& counter = counters_[static_cast<std::size_t>(
      BlobLifetimeResourceV3::lifetime_callback_calls)];
  const bool valid = frozen_.load(std::memory_order_relaxed) &&
      !terminal_quarantined_.load(std::memory_order_relaxed) &&
      counter.invoked <= counter.limit &&
      counter.reserved_cleanup <= counter.limit - counter.invoked &&
      amount <= counter.reserved_cleanup;
  if (valid) counter.reserved_cleanup -= amount;
  else terminal_quarantined_.store(true, std::memory_order_release);
  lock_.clear(std::memory_order_release);
  return valid;
}

BlobLifetimeAuthorityAdmissionV3Generation1::
    ~BlobLifetimeAuthorityAdmissionV3Generation1() noexcept {
  DropPin();
}

void BlobLifetimeAuthorityAdmissionV3Generation1::DropPin() noexcept {
  if (budget_pin_cookie_ != 0 && budget_pin_ops_.drop_pin != nullptr) {
    budget_pin_ops_.drop_pin(budget_pin_ops_.stable_host_context,
                             budget_pin_cookie_);
  }
  if (pin_cookie_ != 0 && pin_ops_.drop_pin != nullptr) {
    pin_ops_.drop_pin(pin_ops_.stable_host_context, pin_cookie_);
  }
  budget_pin_cookie_ = 0;
  pin_cookie_ = 0;
  budget_ = nullptr;
  admitted_ = false;
  authority_ = {};
  services_ = {};
  pin_ops_ = {};
  budget_pin_ops_ = {};
  token_clock_ = {};
  request_clock_ = {};
  receiver_reclamation_pin_established_ = false;
}

void BlobLifetimeAuthorityAdmissionV3Generation1::TransferFrom(
    BlobLifetimeAuthorityAdmissionV3Generation1& source) noexcept {
  if (this == &source) return;
  DropPin();
  authority_ = source.authority_;
  services_ = source.services_;
  pin_ops_ = source.pin_ops_;
  pin_cookie_ = source.pin_cookie_;
  budget_pin_ops_ = source.budget_pin_ops_;
  budget_pin_cookie_ = source.budget_pin_cookie_;
  budget_ = source.budget_;
  token_clock_ = source.token_clock_;
  request_clock_ = source.request_clock_;
  receiver_reclamation_pin_established_ =
      source.receiver_reclamation_pin_established_;
  admitted_ = source.admitted_;
  source.pin_cookie_ = 0;
  source.budget_pin_cookie_ = 0;
  source.budget_ = nullptr;
  source.admitted_ = false;
  source.authority_ = {};
  source.services_ = {};
  source.pin_ops_ = {};
  source.budget_pin_ops_ = {};
  source.token_clock_ = {};
  source.request_clock_ = {};
  source.receiver_reclamation_pin_established_ = false;
}

BlobLifetimeRuntimeResultV3
BlobLifetimeRuntimeFactoryV3Generation1::AdmitMaterialized(
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
    BlobBoundMaterializedCarrierV3Generation1& out_carrier) noexcept {
  if (trusted_operation < SB_BLOB_OP_VALIDATE_VIEW_V3 ||
      trusted_operation > SB_BLOB_OP_RESTORE_V3)
    return RuntimeTerminated();
  // Only the stable-host pin surface is examined before pin acquisition. The
  // authority table, services, carrier, token, and their pointed-to state are
  // not dereferenced until the host admits and pins them.
  if (pin_ops.acquire_pin == nullptr || pin_ops.clone_pin == nullptr ||
      pin_ops.drop_pin == nullptr) {
    return PinFailure(BlobReceiverPinAcquireStatusV3::unavailable,
                      trusted_operation);
  }
  const auto acquired = pin_ops.acquire_pin(pin_ops.stable_host_context);
  ValidatePinReplyOrTerminate(acquired);
  if (acquired.status != BlobReceiverPinAcquireStatusV3::acquired)
    return PinFailure(acquired.status, trusted_operation);

  // The budget capability is receiver-owned unstable state too. It becomes
  // readable only after the stable-host admission pin above has been acquired.
  if (budget_control.ledger_ == nullptr ||
      budget_control.pin_ops_.acquire_pin == nullptr ||
      budget_control.pin_ops_.clone_pin == nullptr ||
      budget_control.pin_ops_.drop_pin == nullptr) {
    pin_ops.drop_pin(pin_ops.stable_host_context, acquired.pin_cookie);
    return PinFailure(BlobReceiverPinAcquireStatusV3::unavailable,
                      trusted_operation);
  }
  const auto budget_acquired = budget_control.pin_ops_.acquire_pin(
      budget_control.pin_ops_.stable_host_context);
  ValidatePinReplyOrTerminate(budget_acquired);
  if (budget_acquired.status != BlobReceiverPinAcquireStatusV3::acquired) {
    pin_ops.drop_pin(pin_ops.stable_host_context, acquired.pin_cookie);
    return PinFailure(budget_acquired.status, trusted_operation);
  }
  const auto drop_acquired_pins = [&]() noexcept {
    budget_control.pin_ops_.drop_pin(
        budget_control.pin_ops_.stable_host_context,
        budget_acquired.pin_cookie);
    pin_ops.drop_pin(pin_ops.stable_host_context, acquired.pin_cookie);
  };
  const auto* canonical = CurrentBlobIdentity();
  if (canonical == nullptr) {
    drop_acquired_pins();
    return RuntimeTerminated();
  }
  const auto fail_after_pin = [&](std::string_view code,
                                  std::string_view reason,
                                  BlobLifetimeAuthoritySourceV3 source =
                                      BlobLifetimeAuthoritySourceV3::none)
      noexcept {
    auto failure = RuntimeFailure(code, reason);
    failure.fact.stage = BlobLifetimeFactStageV3::factory_admission;
    failure.fact.gate_present = false;
    failure.fact.authority_source = source;
    if (failure.fact.diagnostic ==
        BlobLifetimeDiagnosticCodeV3::lifetime_authority_unavailable) {
      failure.fact.parameters.present = blob_parameter_phase | blob_parameter_operation;
      failure.fact.parameters.operation_enum = trusted_operation;
      failure.fact.parameters.phase_enum = BlobLifetimePublicPhaseV3::retain;
    } else if (failure.fact.diagnostic ==
               BlobLifetimeDiagnosticCodeV3::handle_owner_mismatch) {
      failure.fact.parameters.present = blob_parameter_owner_class;
      failure.fact.parameters.owner_class_enum = token.owner_class;
    } else if (failure.fact.diagnostic ==
               BlobLifetimeDiagnosticCodeV3::handle_mode_refused) {
      failure.fact.parameters.present = blob_parameter_open_mode |
                                         blob_parameter_operation;
      failure.fact.parameters.open_mode_enum =
          (token.mode_bits & SB_BLOB_MODE_MATERIALIZED_BYTES_V3) != 0 ? 4 :
          (token.mode_bits & SB_BLOB_MODE_LOGICAL_READER_V3) != 0 ? 5 : 6;
      failure.fact.parameters.operation_enum = request.operation;
    } else if (failure.fact.diagnostic ==
               BlobLifetimeDiagnosticCodeV3::descriptor_invalid) {
      failure.fact.parameters.present = blob_parameter_descriptor_identity;
      failure.fact.parameters.descriptor_uuid =
          canonical->legacy_fields.descriptor_uuid;
      failure.fact.parameters.descriptor_generation =
          canonical->legacy_fields.descriptor_generation;
    }
    // All request/capability selectors must be copied before either lifetime
    // pin can release its backing storage. The returned fact owns its values.
    drop_acquired_pins();
    return failure;
  };
  // The profile is checked before any unstable structure is interpreted.
  // Failure descriptors always come from immutable canonical authority.
  if (!ExactProfile(profile))
    return fail_after_pin("CINL.LOB.DESCRIPTOR_INVALID",
                          "authority_profile_binding_mismatch");
  if (authority == nullptr)
    return fail_after_pin("BLOB.LIFETIME_AUTHORITY_UNAVAILABLE",
        "authority_table_missing",
        BlobLifetimeAuthoritySourceV3::authority_table_missing);

  if (authority->struct_bytes != sizeof(BlobLifetimeAuthorityV3) ||
      authority->abi_major != SB_BLOB_LIFETIME_ABI_MAJOR_V3 ||
      authority->abi_minor != SB_BLOB_LIFETIME_ABI_MINOR_V3 ||
      !budget_control.ledger_->frozen())
    return fail_after_pin("CINL.LOB.DESCRIPTOR_INVALID",
                          "authority_abi_mismatch");
  if (request.operation != trusted_operation)
    return fail_after_pin("CINL.LOB.DESCRIPTOR_INVALID", "token_struct_invalid");
  const auto structural = ClassifyFactoryStructure(
      *authority, token, request, capability.binding_);
  switch (structural) {
    case BlobFactoryStructuralClassV3::valid:
    case BlobFactoryStructuralClassV3::abi_invalid:
      break;
    case BlobFactoryStructuralClassV3::token_shape_invalid:
      return fail_after_pin("CINL.LOB.DESCRIPTOR_INVALID",
                            "token_struct_invalid");
    case BlobFactoryStructuralClassV3::token_identity_invalid:
      return fail_after_pin("CINL.LOB.DESCRIPTOR_INVALID",
                            "token_identity_invalid");
    case BlobFactoryStructuralClassV3::authority_instance_mismatch:
      return fail_after_pin("CINL.LOB.DESCRIPTOR_INVALID",
                            "wrong_authority_instance");
    case BlobFactoryStructuralClassV3::owner_mismatch:
      return fail_after_pin("BLOB.HANDLE_OWNER_MISMATCH",
                            "owner_binding_mismatch");
    case BlobFactoryStructuralClassV3::transaction_mismatch:
      return fail_after_pin("CINL.LOB.DESCRIPTOR_INVALID",
                            "transaction_mismatch");
    case BlobFactoryStructuralClassV3::mode_mismatch:
      return fail_after_pin("BLOB.HANDLE_MODE_REFUSED", "wrong_mode");
  }
  const auto fail_state_after_pin = [&](BlobValueStateV3 state) noexcept {
    auto failure = RuntimeFailure("BLOB.STATE_INVALID", "carrier_state_not_admitted");
    failure.fact.stage = BlobLifetimeFactStageV3::factory_admission;
    failure.fact.parameters.present = blob_parameter_supplied_state |
                                      blob_parameter_operation;
    failure.fact.parameters.supplied_state_u8 = static_cast<u8>(state);
    failure.fact.parameters.operation_enum = trusted_operation;
    drop_acquired_pins();
    return failure;
  };
  // A default carrier has VALUE state but no binding. Population is determined
  // fieldwise, never from padding or the default state byte alone.
  const auto& output_binding = out_carrier.binding_;
  const bool output_populated =
      !PlatformUuidIsNil(output_binding.authority_instance_uuid) ||
      output_binding.authority_instance_generation != 0 ||
      !PlatformUuidIsNil(output_binding.carrier_uuid) ||
      output_binding.carrier_generation != 0 ||
      !PlatformUuidIsNil(output_binding.lifetime_token_uuid) ||
      output_binding.lifetime_token_generation != 0 ||
      output_binding.immutable_binding_generation != 0 ||
      out_carrier.data_ != nullptr || out_carrier.logical_length_ != 0;
  if (output_populated) {
    if (static_cast<u8>(out_carrier.state_) > 2)
      return fail_after_pin("CINL.LOB.DESCRIPTOR_INVALID", "token_struct_invalid");
    return fail_state_after_pin(out_carrier.state_);
  }
  if (out_admission.admitted_ || out_admission.pin_cookie_ != 0 ||
      out_admission.budget_pin_cookie_ != 0) {
    return fail_after_pin(
        "CINL.LOB.DESCRIPTOR_INVALID", "destination_armed",
        BlobLifetimeAuthoritySourceV3::datatype_local_state);
  }
  if (static_cast<u8>(capability.state_) > 2)
    return fail_after_pin("CINL.LOB.DESCRIPTOR_INVALID",
                          "token_struct_invalid");
  if (capability.state_ != BlobValueStateV3::value &&
      capability.state_ != BlobValueStateV3::sql_null)
    return fail_state_after_pin(capability.state_);
  if (capability.state_ == BlobValueStateV3::sql_null &&
      (capability.logical_length_ != 0 || capability.data_ != nullptr))
    return fail_state_after_pin(capability.state_);
  if (capability.logical_length_ != 0 && capability.data_ == nullptr)
    return fail_state_after_pin(capability.state_);

  constexpr u64 kMaximumAddressableSpan =
      static_cast<u64>(std::numeric_limits<std::size_t>::max());
  constexpr u64 kMaximumMaterializedSpan =
      MaximumMaterializedSpanForAddressSpace(kMaximumAddressableSpan);
  if (capability.logical_length_ > kMaximumMaterializedSpan) {
    auto failure = RuntimeFailure("BLOB.LENGTH_EXCEEDED",
                                  "length_not_representable");
    failure.fact.parameters.present = blob_parameter_actual_length |
                                      blob_parameter_maximum_length |
                                      blob_parameter_operation;
    failure.fact.parameters.actual_length_u64 = capability.logical_length_;
    failure.fact.parameters.maximum_length_u64 = kMaximumMaterializedSpan;
    failure.fact.parameters.operation_enum = request.operation;
    drop_acquired_pins();
    return failure;
  }

  if (authority->retain == nullptr)
    return fail_after_pin("BLOB.LIFETIME_AUTHORITY_UNAVAILABLE",
        "authority_table_missing",
        BlobLifetimeAuthoritySourceV3::retain_callback_missing);
  if (authority->probe == nullptr)
    return fail_after_pin("BLOB.LIFETIME_AUTHORITY_UNAVAILABLE",
        "authority_table_missing",
        BlobLifetimeAuthoritySourceV3::probe_callback_missing);
  if (authority->begin_access == nullptr)
    return fail_after_pin("BLOB.LIFETIME_AUTHORITY_UNAVAILABLE",
        "authority_table_missing",
        BlobLifetimeAuthoritySourceV3::begin_access_callback_missing);
  if (authority->end_access == nullptr)
    return fail_after_pin("BLOB.LIFETIME_AUTHORITY_UNAVAILABLE",
        "authority_table_missing",
        BlobLifetimeAuthoritySourceV3::end_access_callback_missing);
  if (authority->release == nullptr)
    return fail_after_pin("BLOB.LIFETIME_AUTHORITY_UNAVAILABLE",
        "authority_table_missing",
        BlobLifetimeAuthoritySourceV3::release_callback_missing);
  if (services == nullptr)
    return fail_after_pin("BLOB.LIFETIME_AUTHORITY_UNAVAILABLE",
        "receiver_services_failed",
        BlobLifetimeAuthoritySourceV3::receiver_services_object_missing);
  if (PlatformUuidIsNil(services->receiver_services_uuid) ||
      services->receiver_services_generation == 0)
    return fail_after_pin("BLOB.LIFETIME_AUTHORITY_UNAVAILABLE",
        "receiver_services_failed",
        BlobLifetimeAuthoritySourceV3::receiver_services_object_missing);
  if (services->read_monotonic_ns == nullptr)
    return fail_after_pin("BLOB.LIFETIME_AUTHORITY_UNAVAILABLE",
        "receiver_services_failed",
        BlobLifetimeAuthoritySourceV3::read_monotonic_ns_callable_missing);
  if (services->sample_cancellation == nullptr)
    return fail_after_pin("BLOB.LIFETIME_AUTHORITY_UNAVAILABLE",
        "receiver_services_failed",
        BlobLifetimeAuthoritySourceV3::sample_cancellation_callable_missing);
  if (services->record_invariant_and_quarantine == nullptr)
    return fail_after_pin("BLOB.LIFETIME_AUTHORITY_UNAVAILABLE",
        "receiver_services_failed",
        BlobLifetimeAuthoritySourceV3::record_invariant_and_quarantine_callable_missing);
  if (PlatformUuidIsNil(services->monotonic_clock.clock_uuid) ||
      services->monotonic_clock.clock_generation == 0 ||
      PlatformUuidIsNil(token_clock.clock_uuid) || token_clock.clock_generation == 0 ||
      PlatformUuidIsNil(request_clock.clock_uuid) || request_clock.clock_generation == 0)
    return fail_after_pin("BLOB.LIFETIME_AUTHORITY_UNAVAILABLE",
        "monotonic_clock_unavailable",
        BlobLifetimeAuthoritySourceV3::monotonic_clock_unavailable);
  if (
      token_clock.clock_uuid != services->monotonic_clock.clock_uuid ||
      request_clock.clock_uuid != services->monotonic_clock.clock_uuid ||
      token_clock.clock_generation != services->monotonic_clock.clock_generation ||
      request_clock.clock_generation !=
          services->monotonic_clock.clock_generation) {
    return fail_after_pin("CINL.LOB.DESCRIPTOR_INVALID",
                          "authority_profile_binding_mismatch");
  }
  out_admission.authority_ = *authority;
  out_admission.services_ = *services;
  out_admission.pin_ops_ = pin_ops;
  out_admission.pin_cookie_ = acquired.pin_cookie;
  out_admission.budget_pin_ops_ = budget_control.pin_ops_;
  out_admission.budget_pin_cookie_ = budget_acquired.pin_cookie;
  out_admission.budget_ = budget_control.ledger_;
  out_admission.token_clock_ = token_clock;
  out_admission.request_clock_ = request_clock;
  out_admission.admitted_ = true;
  out_carrier.binding_ = capability.binding_;
  out_carrier.state_ = capability.state_;
  out_carrier.data_ = capability.data_;
  out_carrier.logical_length_ = capability.logical_length_;
  return RuntimeSuccess();
}

BlobRetainedLifetimeLeaseV3::~BlobRetainedLifetimeLeaseV3() noexcept {
  (void)ReleaseInternal(BlobLifetimePhaseV3::destructor_cleanup, 19);
}

bool BlobRetainedLifetimeLeaseV3::retained() const noexcept {
  return state_.load(std::memory_order_acquire) == State::idle;
}

void BlobRetainedLifetimeLeaseV3::ZeroTicket() noexcept {
  retain_ticket_ = {};
}

BlobLifetimeRuntimeResultV3 BlobRetainedLifetimeLeaseV3::LocalStateFailure(
    State observed, bool destination_context) const noexcept {
  switch (observed) {
    case State::disarmed:
      return UnretainedFailure();
    case State::released:
      return ConsumedFailure();
    case State::moved_from:
      return MovedBindingFailure();
    case State::terminal:
      return OwnerClosedFailure();
    case State::retaining:
    case State::idle:
    case State::callback_active:
    case State::access_active:
    case State::moving:
    case State::releasing:
    case State::installing:
      if (destination_context) {
        auto result = CanonicalProtocolFailure("destination_armed");
        result.fact.authority_source =
            BlobLifetimeAuthoritySourceV3::datatype_local_state;
        result.fact.stage = BlobLifetimeFactStageV3::local_precondition;
        return result;
      }
      return OwnerClosedFailure();
  }
  return RuntimeTerminated();
}

BlobLifetimeRuntimeResultV3
BlobRetainedLifetimeLeaseV3::GenerationResetFailure(
    const AtomicControl::GenerationResetOutcome& outcome,
    bool destination_context) const noexcept {
  switch (outcome.status) {
    case AtomicControl::GenerationResetStatus::installed:
      return RuntimeSuccess();
    case AtomicControl::GenerationResetStatus::wrong_state:
      return LocalStateFailure(outcome.observed_state, destination_context);
    case AtomicControl::GenerationResetStatus::contended:
      return CanonicalConcurrentUseFailure();
    case AtomicControl::GenerationResetStatus::corrupt_protocol:
      return RuntimeTerminated();
  }
  return RuntimeTerminated();
}

void BlobRetainedLifetimeLeaseV3::PrepareInvariantLocalCleanup() noexcept {
  ZeroTicket();
  carrier_binding_ = {};
  carrier_data_ = nullptr;
  carrier_logical_length_ = 0;
  descriptor_uuid_ = {};
  descriptor_generation_ = 0;
  quarantined_.store(true, std::memory_order_release);
}

bool BlobRetainedLifetimeLeaseV3::PublishDeferredInvariant(
    BlobLifetimeInvariantEventV3 event,
    u8 operation,
    BlobLifetimePhaseV3 phase,
    u8 callback_code_or_255) noexcept {
  return state_.Publish(PackDeferredInvariant(
      event, operation, phase, callback_code_or_255));
}

bool BlobRetainedLifetimeLeaseV3::PublishDeferredThunk(
    void* context, BlobLifetimeInvariantEventV3 event, u8 operation,
    BlobLifetimePhaseV3 phase, u8 callback_code_or_255) noexcept {
  return static_cast<BlobRetainedLifetimeLeaseV3*>(context)
      ->PublishDeferredInvariant(event, operation, phase,
                                 callback_code_or_255);
}

BlobLeaseConstructionResultV3::BlobLeaseConstructionResultV3(
    BlobLifetimeRuntimeResultV3 result_value,
    BlobRetainedLifetimeLeaseV3& source,
    bool source_already_guarded_moving) noexcept
    : result(result_value) {
  if (result.ok()) {
    result = value.AdoptFrom(source, source_already_guarded_moving, 16, false);
  }
}


BlobLifetimeRuntimeResultV3 BlobRetainedLifetimeLeaseV3::RejectOverlappingUse(
    BlobLifetimePhaseV3 attempted_phase,
    u8 operation_override_or_zero) noexcept {
  const bool reentrant = callback_active_.load(std::memory_order_acquire) &&
                         SameActiveCallbackBinding(token_);
  const auto event = reentrant
      ? BlobLifetimeInvariantEventV3::callback_reentrant
      : BlobLifetimeInvariantEventV3::same_ticket_concurrent_use;
  BlobLifetimePhaseV3 invariant_phase = attempted_phase;
  if (!reentrant && operation_override_or_zero != 15 &&
      attempted_phase != BlobLifetimePhaseV3::move_construct &&
      attempted_phase != BlobLifetimePhaseV3::move_replace) {
    invariant_phase = BlobLifetimePhaseV3::protected_access;
  }
  u8 operation = operation_override_or_zero != 0
      ? operation_override_or_zero : request_.operation;
  if (operation_override_or_zero == 0 &&
      attempted_phase == BlobLifetimePhaseV3::move_construct)
    operation = 16;
  if (operation_override_or_zero == 0 &&
      attempted_phase == BlobLifetimePhaseV3::move_replace)
    operation = 17;
  (void)PublishDeferredInvariant(event, operation, invariant_phase, 255);
  // A losing caller may be racing the owner's local zero. The descriptor is
  // the immutable base.blob registry identity, so materialize the provisional
  // diagnostic from that stable authority rather than reading wrapper fields.
  return reentrant ? CanonicalProtocolFailure("callback_reentrant")
                   : CanonicalConcurrentUseFailure();
}

BlobLifetimeRuntimeResultV3 BlobRetainedLifetimeLeaseV3::AdoptFrom(
    BlobRetainedLifetimeLeaseV3& source,
    bool source_already_guarded_moving,
    u8 invariant_operation,
    bool destination_entrant_already_held,
    State reserved_destination_predecessor) noexcept {
  if (&source == this) return RuntimeSuccess();
  EntrantGuard destination_entrant(state_,
                                   !destination_entrant_already_held);
  if (!destination_entrant.acquired()) return RuntimeTerminated();
  State expected = source_already_guarded_moving ? State::moving : State::idle;
  const auto source_claim = source_already_guarded_moving
      ? AtomicControl::GenerationResetOutcome{
            AtomicControl::GenerationResetStatus::installed, State::moving}
      : source.state_.ClaimIdleForTransfer();
  if ((!source_already_guarded_moving &&
       source_claim.status != AtomicControl::GenerationResetStatus::installed) ||
      (source_already_guarded_moving &&
       source.state_.load(std::memory_order_acquire) != State::moving)) {
    if (!source_already_guarded_moving) {
      if (source_claim.status != AtomicControl::GenerationResetStatus::wrong_state)
        return source.GenerationResetFailure(source_claim, false);
      expected = source_claim.observed_state;
    }
    if (expected == State::moved_from || expected == State::disarmed ||
        expected == State::released || expected == State::terminal)
      return source.LocalStateFailure(expected);
    return source.RejectOverlappingUse(
        BlobLifetimePhaseV3::move_construct);
  }

  const auto rollback_source = [&](BlobLifetimeRuntimeResultV3 result)
      noexcept {
    (void)source.state_.BeginClosing();
    source.state_.WaitForClosingQuiescence(1);
    for (;;) {
      if (source.state_.Deferred() != 0) {
        return source.ReleaseOwned(BlobLifetimePhaseV3::release,
                                   invariant_operation, std::move(result));
      }
      if (source.state_.TryFinalize(State::idle, 1)) return result;
    }
  };

  const bool reserved = reserved_destination_predecessor != State::terminal;
  const State destination_predecessor = reserved
      ? reserved_destination_predecessor
      : state_.load(std::memory_order_acquire);
  if (destination_predecessor != State::disarmed &&
      destination_predecessor != State::released &&
      destination_predecessor != State::moved_from) {
    return rollback_source(LocalStateFailure(destination_predecessor, true));
  }
  const auto destination_reset =
      state_.ResetGeneration(reserved ? State::installing
                                      : destination_predecessor,
                             State::installing);
  if (destination_reset.status !=
      AtomicControl::GenerationResetStatus::installed) {
    return rollback_source(GenerationResetFailure(destination_reset, true));
  }
  (void)source.state_.BeginClosing();
  for (;;) {
    if (source.state_.Deferred() != 0) {
      // A winner published after moving began owns this source. The moving
      // owner consumes the exact armed release obligation, services its own
      // immutable tuple, and drops its own pin before the move can return.
      const auto result = source.ReleaseOwned(
          BlobLifetimePhaseV3::release, invariant_operation,
          CanonicalConcurrentUseFailure());
      const auto destination_rollback =
          state_.ResetGeneration(State::installing, destination_predecessor);
      if (destination_rollback.status !=
          AtomicControl::GenerationResetStatus::installed)
        std::terminate();
      return result;
    }
    if (source.state_.TryFinalize(State::moved_from, 1)) break;
  }
  admission_.TransferFrom(source.admission_);
  carrier_binding_ = source.carrier_binding_;
  carrier_state_ = source.carrier_state_;
  carrier_data_ = source.carrier_data_;
  carrier_logical_length_ = source.carrier_logical_length_;
  token_ = source.token_;
  request_ = source.request_;
  descriptor_uuid_ = source.descriptor_uuid_;
  descriptor_generation_ = source.descriptor_generation_;
  retain_ticket_ = source.retain_ticket_;
  prior_monotonic_ns_ = source.prior_monotonic_ns_;
  has_prior_monotonic_ns_ = source.has_prior_monotonic_ns_;
  // Publication suppression belongs to the operation which sampled the
  // failing gate.  A successful transfer can only acquire an idle source and
  // starts with a fresh operation-local gate state.
  suppress_publication_gate_ = false;
  quarantined_.store(source.quarantined_.load(), std::memory_order_release);
  source.ZeroTicket();
  source.carrier_binding_ = {};
  source.carrier_data_ = nullptr;
  source.carrier_logical_length_ = 0;
  source.descriptor_uuid_ = {};
  source.descriptor_generation_ = 0;
  source.prior_monotonic_ns_ = 0;
  source.has_prior_monotonic_ns_ = false;
  source.suppress_publication_gate_ = false;
  State installing = State::installing;
  if (!state_.compare_exchange_strong(installing, State::moving,
                                      std::memory_order_acq_rel))
    std::terminate();
  (void)state_.BeginClosing();
  state_.WaitForClosingQuiescence(1);
  for (;;) {
    if (state_.Deferred() != 0) {
      return ReleaseOwned(BlobLifetimePhaseV3::release,
                          invariant_operation,
                          CanonicalConcurrentUseFailure());
    }
    if (state_.TryFinalize(State::idle, 1)) return RuntimeSuccess();
  }
}

BlobLeaseConstructionResultV3 BlobRetainedLifetimeLeaseV3::MoveConstructFrom(
    BlobRetainedLifetimeLeaseV3&& source) noexcept {
  EntrantGuard source_entrant(source.state_);
  if (!source_entrant.acquired()) {
    const State observed = source.state_.load(std::memory_order_acquire);
    if (observed == State::released || observed == State::terminal ||
        observed == State::moved_from)
      return BlobLeaseConstructionResultV3(
          source.LocalStateFailure(observed));
    return BlobLeaseConstructionResultV3(RuntimeTerminated());
  }
  if (MarkActiveBindingReentry(source.token_)) {
    return BlobLeaseConstructionResultV3(
        CanonicalProtocolFailure("callback_reentrant"));
  }
  const auto claim = source.state_.ClaimIdleForTransfer();
  if (claim.status != AtomicControl::GenerationResetStatus::installed) {
    if (claim.status != AtomicControl::GenerationResetStatus::wrong_state)
      return BlobLeaseConstructionResultV3(
          source.GenerationResetFailure(claim, false));
    const State state = claim.observed_state;
    if (state == State::moved_from || state == State::disarmed ||
        state == State::released || state == State::terminal)
      return BlobLeaseConstructionResultV3(
          source.LocalStateFailure(state));
    if (state == State::retaining || state == State::callback_active ||
        state == State::access_active || state == State::moving ||
        state == State::releasing || state == State::installing ||
        source.callback_active_.load(std::memory_order_acquire)) {
      return BlobLeaseConstructionResultV3(
          source.RejectOverlappingUse(BlobLifetimePhaseV3::move_construct));
    }
    return BlobLeaseConstructionResultV3(source.LocalStateFailure(state));
  }
  return BlobLeaseConstructionResultV3(RuntimeSuccess(), source, true);
}

BlobLifetimeRuntimeResultV3 BlobRetainedLifetimeLeaseV3::MoveReplaceFrom(
    BlobRetainedLifetimeLeaseV3&& source) noexcept {
  if (&source == this) return RuntimeSuccess();

  BlobRetainedLifetimeLeaseV3* first = this;
  BlobRetainedLifetimeLeaseV3* second = &source;
  if (std::less<const void*>{}(second, first)) std::swap(first, second);
  EntrantGuard first_entrant(first->state_, true, first == this);
  if (!first_entrant.acquired()) {
    const State observed = first->state_.load(std::memory_order_acquire);
    return observed == State::released || observed == State::terminal ||
            observed == State::moved_from
        ? first->LocalStateFailure(observed) : RuntimeTerminated();
  }
  EntrantGuard second_entrant(second->state_, true, second == this);
  if (!second_entrant.acquired()) {
    const State observed = second->state_.load(std::memory_order_acquire);
    return observed == State::released || observed == State::terminal ||
            observed == State::moved_from
        ? second->LocalStateFailure(observed) : RuntimeTerminated();
  }
  if (MarkActiveBindingReentry(token_) ||
      MarkActiveBindingReentry(source.token_)) {
    return CanonicalProtocolFailure("callback_reentrant");
  }
  const State source_observed = source.state_.load(std::memory_order_acquire);
  const State destination_observed = state_.load(std::memory_order_acquire);
  const bool source_guarded = source_observed == State::retaining ||
      source_observed == State::callback_active ||
      source_observed == State::access_active ||
      source_observed == State::moving ||
      source_observed == State::releasing ||
      source_observed == State::installing ||
      source.callback_active_.load(std::memory_order_acquire);
  const bool destination_guarded =
      destination_observed == State::retaining ||
      destination_observed == State::callback_active ||
      destination_observed == State::access_active ||
      destination_observed == State::moving ||
      destination_observed == State::releasing ||
      destination_observed == State::installing ||
      callback_active_.load(std::memory_order_acquire);
  if (source_guarded || destination_guarded) {
    if (source_guarded && destination_guarded &&
        SameLifetimeBinding(source.token_, token_)) {
      // Each wrapper owns its own immutable tuple and independently obtains
      // its own service receipt. The receiver's idempotence key collapses the
      // same-binding effects into accepted/already_applied.
      (void)first->PublishDeferredInvariant(
          BlobLifetimeInvariantEventV3::same_ticket_concurrent_use, 17,
          BlobLifetimePhaseV3::move_replace, 255);
      (void)second->PublishDeferredInvariant(
          BlobLifetimeInvariantEventV3::same_ticket_concurrent_use, 17,
          BlobLifetimePhaseV3::move_replace, 255);
    } else {
      BlobRetainedLifetimeLeaseV3* lower = &source;
      BlobRetainedLifetimeLeaseV3* upper = this;
      if (LifetimeBindingLess(upper->token_, lower->token_))
        std::swap(lower, upper);
      if ((lower == &source && source_guarded) ||
          (lower == this && destination_guarded)) {
        (void)lower->PublishDeferredInvariant(
            BlobLifetimeInvariantEventV3::same_ticket_concurrent_use, 17,
            BlobLifetimePhaseV3::move_replace, 255);
      }
      if ((upper == &source && source_guarded) ||
          (upper == this && destination_guarded)) {
        (void)upper->PublishDeferredInvariant(
            BlobLifetimeInvariantEventV3::same_ticket_concurrent_use, 17,
            BlobLifetimePhaseV3::move_replace, 255);
      }
    }
    return CanonicalConcurrentUseFailure();
  }

  // Reserve both packed controls in address order. A failed second claim
  // retires its unowned entrant before the first owner waits for quiescence.
  State destination_predecessor = State::terminal;
  const auto claim = [&](BlobRetainedLifetimeLeaseV3* lease) noexcept {
    if (lease == &source) return lease->state_.ClaimIdleForTransfer();
    destination_predecessor = state_.load(std::memory_order_acquire);
    if (destination_predecessor == State::idle)
      return state_.ClaimIdleForTransfer();
    if (destination_predecessor == State::disarmed ||
        destination_predecessor == State::released ||
        destination_predecessor == State::moved_from)
      return state_.ResetGeneration(destination_predecessor, State::installing);
    return AtomicControl::GenerationResetOutcome{
        AtomicControl::GenerationResetStatus::wrong_state,
        destination_predecessor};
  };
  const auto failure = [&](BlobRetainedLifetimeLeaseV3* lease,
                           const auto& outcome) noexcept {
    if (outcome.status == AtomicControl::GenerationResetStatus::wrong_state &&
        outcome.observed_state != State::installing &&
        (outcome.observed_state == State::retaining ||
         outcome.observed_state == State::callback_active ||
         outcome.observed_state == State::access_active ||
         outcome.observed_state == State::moving ||
         outcome.observed_state == State::releasing))
      return lease->RejectOverlappingUse(BlobLifetimePhaseV3::move_replace, 17);
    return lease->GenerationResetFailure(outcome, lease == this);
  };
  const auto rollback = [&](BlobRetainedLifetimeLeaseV3* lease,
                            BlobLifetimeRuntimeResultV3 result) noexcept {
    if (lease == this && destination_predecessor != State::idle) {
      const auto reset = state_.ResetGeneration(State::installing,
                                                destination_predecessor);
      if (reset.status != AtomicControl::GenerationResetStatus::installed)
        std::terminate();
      return result;
    }
    (void)lease->state_.BeginClosing();
    lease->state_.WaitForClosingQuiescence(1);
    for (;;) {
      if (lease->state_.Deferred() != 0)
        return lease->ReleaseOwned(BlobLifetimePhaseV3::release, 17,
                                   std::move(result));
      if (lease->state_.TryFinalize(State::idle, 1)) return result;
    }
  };
  const auto first_claim = claim(first);
  if (first_claim.status != AtomicControl::GenerationResetStatus::installed)
    return failure(first, first_claim);
  const auto second_claim = claim(second);
  if (second_claim.status != AtomicControl::GenerationResetStatus::installed) {
    auto result = failure(second, second_claim);
    second_entrant.Release();
    return rollback(first, std::move(result));
  }
  if (destination_predecessor == State::idle) {
    const auto released = ReleaseOwned(BlobLifetimePhaseV3::release, 17);
    if (!released.ok()) return rollback(&source, released);
    return AdoptFrom(source, true, 17, true);
  }
  return AdoptFrom(source, true, 17, true, destination_predecessor);
}

BlobLifetimeRuntimeResultV3 BlobRetainedLifetimeLeaseV3::RetainFrom(
    const BlobValidatedProfileHandleV3& profile,
    BlobLifetimeAuthorityAdmissionV3Generation1& admission,
    const BlobBoundMaterializedCarrierV3Generation1& carrier,
    const BlobLifetimeTokenV3& token,
    const BlobLifetimeUseRequestV3& request,
    u8 invariant_operation,
    bool entrant_already_held,
    bool defer_idle_publication,
    bool destination_already_installing) noexcept {
  EntrantGuard entrant(state_, !entrant_already_held);
  if (!entrant.acquired()) {
    const State observed = state_.load(std::memory_order_acquire);
    return observed == State::released || observed == State::terminal ||
            observed == State::moved_from
        ? LocalStateFailure(observed, true) : RuntimeTerminated();
  }
  if (MarkActiveBindingReentry(token))
    return CanonicalProtocolFailure("callback_reentrant");
  const State destination_state = state_.load(std::memory_order_acquire);
  if (destination_state != (destination_already_installing
                               ? State::installing : State::disarmed)) {
    if (callback_active_.load(std::memory_order_acquire))
      return RejectOverlappingUse(BlobLifetimePhaseV3::retain);
    return LocalStateFailure(destination_state, true);
  }
  if (!ExactProfile(profile)) {
    const auto* canonical = CurrentBlobIdentity();
    if (canonical == nullptr) return RuntimeTerminated();
    auto failure = ProtocolFailure(
        "authority_profile_binding_mismatch",
        canonical->legacy_fields.descriptor_uuid,
        canonical->legacy_fields.descriptor_generation);
    failure.fact.authority_source =
        BlobLifetimeAuthoritySourceV3::datatype_local_state;
    failure.fact.stage = BlobLifetimeFactStageV3::local_precondition;
    return failure;
  }
  if (!admission.admitted_ || admission.budget_ == nullptr ||
      admission.pin_cookie_ == 0 ||
      admission.services_.read_monotonic_ns == nullptr ||
      admission.services_.sample_cancellation == nullptr ||
      admission.services_.record_invariant_and_quarantine == nullptr ||
      PlatformUuidIsNil(admission.services_.receiver_services_uuid) ||
      admission.services_.receiver_services_generation == 0 ||
      PlatformUuidIsNil(admission.services_.monotonic_clock.clock_uuid) ||
      admission.services_.monotonic_clock.clock_generation == 0 ||
      admission.token_clock_.clock_uuid !=
          admission.services_.monotonic_clock.clock_uuid ||
      admission.request_clock_.clock_uuid !=
          admission.services_.monotonic_clock.clock_uuid ||
      admission.token_clock_.clock_generation !=
          admission.services_.monotonic_clock.clock_generation ||
      admission.request_clock_.clock_generation !=
          admission.services_.monotonic_clock.clock_generation ||
      !TokenAndRequestShapeValid(admission.authority_, token, request,
                                 carrier.binding_) ||
      (carrier.state_ != BlobValueStateV3::sql_null &&
       carrier.state_ != BlobValueStateV3::value) ||
      (carrier.state_ == BlobValueStateV3::sql_null &&
       (carrier.logical_length_ != 0 || carrier.data_ != nullptr)) ||
      (carrier.logical_length_ != 0 && carrier.data_ == nullptr) ||
      carrier.logical_length_ > kBlobMaximumLogicalBytesV3)
    return RuntimeTerminated();
  const auto generation_reset =
      state_.ResetGeneration(destination_already_installing
                                 ? State::installing : State::disarmed,
                             State::installing);
  if (generation_reset.status !=
      AtomicControl::GenerationResetStatus::installed)
    return GenerationResetFailure(generation_reset, true);
  // The reset CAS starts a new binding generation. No ticket, monotonic
  // sample, quarantine marker, callback activity, or publication decision is
  // inherited from the reusable predecessor.
  ZeroTicket();
  prior_monotonic_ns_ = 0;
  has_prior_monotonic_ns_ = false;
  quarantined_.store(false, std::memory_order_release);
  callback_active_.store(false, std::memory_order_release);
  suppress_publication_gate_ = false;
  admission_.TransferFrom(admission);
  carrier_binding_ = carrier.binding_;
  carrier_state_ = carrier.state_;
  carrier_data_ = carrier.data_;
  carrier_logical_length_ = carrier.logical_length_;
  token_ = token;
  request_ = request;
  descriptor_uuid_ = profile.identity.descriptor_uuid;
  descriptor_generation_ = profile.identity.descriptor_generation;
  State installing = State::installing;
  if (!state_.compare_exchange_strong(installing, State::retaining,
                                      std::memory_order_acq_rel))
    std::terminate();
  const auto close_without_ticket = [&](BlobLifetimeRuntimeResultV3 primary,
                                        State clean_state) noexcept {
    (void)state_.BeginClosing();
    state_.WaitForClosingQuiescence(1);
    const u64 pending = state_.PeekDeferredWhileClosing();
    const bool invariant_seen = pending != 0;
    if (invariant_seen) {
      const BlobLifetimeRuntimeResultV3 invariant = ReportInvariant(
          DeferredEvent(pending), DeferredPhase(pending),
          DecodeDeferredCallbackCode(pending), DeferredOperation(pending), 1);
      if (!state_.CompleteDeferredServiceReceipt(pending)) std::terminate();
      if (primary.ok()) primary = invariant;
    }
    if (!state_.TryFinalize(invariant_seen ? State::terminal : clean_state, 1))
      std::terminate();
    admission_.DropPin();
    return primary;
  };
  auto gate = OrdinaryGate(BlobLifetimeGateV3::before_retain,
                           BlobLifetimePublicPhaseV3::retain,
                           BlobLifetimePhaseV3::retain,
                           invariant_operation);
  if (!gate.ok()) {
    return close_without_ticket(gate, State::disarmed);
  }
  u64 available = 0;
  if (!admission_.budget_->ReserveOrdinaryAndCleanup(1, 1, &available)) {
    if (admission_.budget_->terminal_quarantined()) {
      (void)PublishDeferredInvariant(
          BlobLifetimeInvariantEventV3::callback_budget_counter_corrupt,
          invariant_operation, BlobLifetimePhaseV3::retain, 255);
      return close_without_ticket(RuntimeSuccess(), State::terminal);
    }
    return close_without_ticket(
        BudgetFailure(BlobLifetimeResourceV3::lifetime_callback_calls,
                      2, available),
        State::disarmed);
  }
  BlobLifetimeRetainTicketV3 ticket{};
  BlobLifetimeCallbackResultV3 callback{};
  bool retain_threw = false;
  const auto conservative_release = [&]() noexcept {
    BlobLifetimeRuntimeResultV3 cleanup_public = RuntimeSuccess();
    const auto cleanup = admission_.budget_->ConsumeCleanup(1);
    const bool cleanup_counter_corrupt =
        !cleanup.obligation_consumed || cleanup.ledger_terminal_quarantined;
    if (cleanup_counter_corrupt) {
      (void)PublishDeferredInvariant(
          BlobLifetimeInvariantEventV3::callback_budget_counter_corrupt,
          invariant_operation, BlobLifetimePhaseV3::release, 255);
    }
    BlobLifetimeCallbackResultV3 released{};
    bool release_threw = false;
    try {
      LifetimeCallbackScopeV3 callback_scope(
          callback_active_, token_, BlobLifetimePhaseV3::release,
          invariant_operation, this,
          &BlobRetainedLifetimeLeaseV3::PublishDeferredThunk);
      released = admission_.authority_.release(
          admission_.authority_.authority_context, &token_, &ticket);
    } catch (...) {
      release_threw = true;
    }
    const bool dirty_ticket = !AllZero(ticket);
    if (release_threw) {
      (void)PublishDeferredInvariant(
          BlobLifetimeInvariantEventV3::adapter_exception,
          invariant_operation, BlobLifetimePhaseV3::release, 255);
    } else if (!ResultShapeValid(released)) {
      (void)PublishDeferredInvariant(
          BlobLifetimeInvariantEventV3::callback_result_unknown,
          invariant_operation, BlobLifetimePhaseV3::release,
          released.code <= 18 ? released.code : 255);
      if (cleanup_public.ok())
        cleanup_public = ProtocolFailure("callback_result_unknown", descriptor_uuid_, descriptor_generation_, BlobLifetimeFactStageV3::cleanup_return);
    } else if (dirty_ticket) {
      (void)PublishDeferredInvariant(
          BlobLifetimeInvariantEventV3::dirty_failure_output,
          invariant_operation, BlobLifetimePhaseV3::release, released.code);
      const auto callback_failure =
          released.code != SB_BLOB_CALLBACK_OK_V3
          ? CallbackFailure(released.code, BlobLifetimePhaseV3::release,
                            request_, descriptor_uuid_,
                            descriptor_generation_)
          : RuntimeSuccess();
      cleanup_public = callback_failure.fact.fact_class ==
              BlobLifetimeFactClassV3::security
          ? callback_failure
          : ProtocolFailure("callback_output_impossible", descriptor_uuid_,
                            descriptor_generation_, BlobLifetimeFactStageV3::cleanup_return);
    } else if (released.code != SB_BLOB_CALLBACK_OK_V3) {
      (void)PublishDeferredInvariant(
          BlobLifetimeInvariantEventV3::release_failed,
          invariant_operation, BlobLifetimePhaseV3::release, released.code);
      cleanup_public = CallbackFailure(
          released.code, BlobLifetimePhaseV3::release, request_,
          descriptor_uuid_, descriptor_generation_);
    }
    ticket = {};
    if (cleanup_counter_corrupt &&
        cleanup_public.fact.fact_class != BlobLifetimeFactClassV3::security)
      cleanup_public = RuntimeTerminated();
    return cleanup_public;
  };
  const auto cancel_prospective_release = [&]() noexcept {
    if (admission_.budget_->CancelCleanup(1)) return true;
    (void)PublishDeferredInvariant(
        BlobLifetimeInvariantEventV3::callback_budget_counter_corrupt,
        invariant_operation, BlobLifetimePhaseV3::retain, 255);
    return false;
  };
  const auto finish_invariant = [&](BlobLifetimeRuntimeResultV3 primary)
      noexcept {
    (void)state_.BeginClosing();
    state_.WaitForClosingQuiescence(1);
    const u64 deferred = state_.PeekDeferredWhileClosing();
    BlobLifetimeRuntimeResultV3 invariant_result = RuntimeTerminated();
    if (deferred != 0) {
      invariant_result = ReportInvariant(
          DeferredEvent(deferred), DeferredPhase(deferred),
          DecodeDeferredCallbackCode(deferred), DeferredOperation(deferred),
          1);
      if (!state_.CompleteDeferredServiceReceipt(deferred))
        std::terminate();
    }
    if (!state_.TryFinalize(State::terminal, 1)) std::terminate();
    admission_.DropPin();
    return primary.ok() ? invariant_result : primary;
  };

  try {
    LifetimeCallbackScopeV3 callback_scope(
        callback_active_, token_, BlobLifetimePhaseV3::retain,
        invariant_operation, this,
          &BlobRetainedLifetimeLeaseV3::PublishDeferredThunk);
    callback = admission_.authority_.retain(
        admission_.authority_.authority_context, &token, &request, &ticket);
  } catch (...) {
    retain_threw = true;
    (void)PublishDeferredInvariant(
        BlobLifetimeInvariantEventV3::adapter_exception,
        invariant_operation, BlobLifetimePhaseV3::retain, 255);
  }

  if (state_.Deferred() != 0) {
    auto primary = DeferredInvariantPrimary(state_.Deferred(), descriptor_uuid_,
                                             descriptor_generation_);
    const bool recognized = !retain_threw && ResultShapeValid(callback);
    if (recognized && callback.code == SB_BLOB_CALLBACK_SECURITY_DENIED_V3)
      primary = CallbackFailure(callback.code, BlobLifetimePhaseV3::retain,
                                request_, descriptor_uuid_, descriptor_generation_);
    // A recognized non-OK result creates no prospective obligation, even
    // with dirty output. An exception/unknown result with nonzero output
    // requires conservative cleanup; a returned OK requires cleanup even
    // when its output is malformed or zero.
    const bool obligation = recognized
        ? callback.code == SB_BLOB_CALLBACK_OK_V3
        : !AllZero(ticket) || (!retain_threw && callback.code == SB_BLOB_CALLBACK_OK_V3);
    if (obligation) {
      const auto cleanup = conservative_release();
      if (cleanup.fact.fact_class == BlobLifetimeFactClassV3::security)
        primary = cleanup;
    } else {
      (void)cancel_prospective_release();
      ticket = {};
    }
    return finish_invariant(primary);
  }
  if (!ResultShapeValid(callback)) {
    (void)PublishDeferredInvariant(
        BlobLifetimeInvariantEventV3::callback_result_unknown,
        invariant_operation, BlobLifetimePhaseV3::retain,
        callback.code <= 18 ? callback.code : 255);
    auto primary = ProtocolFailure("callback_result_unknown", descriptor_uuid_, descriptor_generation_, BlobLifetimeFactStageV3::callback_return);
    if (callback.code == SB_BLOB_CALLBACK_OK_V3 || !AllZero(ticket)) {
      const auto cleanup = conservative_release();
      if (cleanup.fact.fact_class == BlobLifetimeFactClassV3::security)
        primary = cleanup;
    } else {
      const bool cancelled = cancel_prospective_release();
      ticket = {};
      if (!cancelled) primary = RuntimeTerminated();
    }
    return finish_invariant(primary);
  }
  if (callback.code != SB_BLOB_CALLBACK_OK_V3) {
    const bool cancelled = cancel_prospective_release();
    BlobLifetimeRuntimeResultV3 primary = CallbackFailure(
        callback.code, BlobLifetimePhaseV3::retain, request_,
        descriptor_uuid_, descriptor_generation_);
    if (!AllZero(ticket)) {
      (void)PublishDeferredInvariant(
          BlobLifetimeInvariantEventV3::dirty_failure_output,
          invariant_operation, BlobLifetimePhaseV3::retain, callback.code);
      ticket = {};
      if (primary.fact.fact_class != BlobLifetimeFactClassV3::security)
        primary = ProtocolFailure("callback_output_impossible",
                                  descriptor_uuid_, descriptor_generation_, BlobLifetimeFactStageV3::callback_return);
      if (!cancelled &&
          primary.fact.fact_class != BlobLifetimeFactClassV3::security)
        primary = RuntimeTerminated();
      return finish_invariant(primary);
    }
    if (!cancelled) {
      return finish_invariant(
          primary.fact.fact_class == BlobLifetimeFactClassV3::security
              ? primary : RuntimeTerminated());
    }
    const auto after_gate = OrdinaryGate(
        BlobLifetimeGateV3::after_retain,
        BlobLifetimePublicPhaseV3::retain, BlobLifetimePhaseV3::retain,
        invariant_operation);
    primary = SelectByV7Precedence(primary, after_gate);
    if (CallbackRequiresQuarantine(BlobLifetimePhaseV3::retain,
                                   callback.code)) {
      (void)PublishDeferredInvariant(
          BlobLifetimeInvariantEventV3::callback_result_unknown,
          invariant_operation, BlobLifetimePhaseV3::retain, callback.code);
      return finish_invariant(primary);
    }
    return close_without_ticket(primary, State::disarmed);
  }
  if (!RetainTicketValid(ticket, token)) {
    (void)PublishDeferredInvariant(
        BlobLifetimeInvariantEventV3::malformed_success_ticket,
        invariant_operation, BlobLifetimePhaseV3::retain,
        SB_BLOB_CALLBACK_OK_V3);
    auto primary = ProtocolFailure("callback_output_impossible",
                                   descriptor_uuid_, descriptor_generation_, BlobLifetimeFactStageV3::callback_return);
    const auto cleanup = conservative_release();
    if (cleanup.fact.fact_class == BlobLifetimeFactClassV3::security)
      primary = cleanup;
    return finish_invariant(primary);
  }
  retain_ticket_ = ticket;
  gate = OrdinaryGate(BlobLifetimeGateV3::after_retain,
                      BlobLifetimePublicPhaseV3::retain,
                      BlobLifetimePhaseV3::retain,
                      invariant_operation);
  if (!gate.ok()) {
    return ReleaseOwned(BlobLifetimePhaseV3::release,
                        invariant_operation, gate);
  }
  (void)state_.BeginClosing();
  // A metadata-only refuser may still hold an entrant without publishing an
  // invariant. Its presence is not a failed retain and must not release a
  // successfully acquired ticket. Closing prevents new entrants while we wait.
  state_.WaitForClosingQuiescence(1);
  if (state_.Deferred() != 0)
    return ReleaseOwned(BlobLifetimePhaseV3::release, invariant_operation);
  if (defer_idle_publication) {
    return RuntimeSuccess();
  }
  if (!state_.TryFinalize(State::idle, 1))
    return ReleaseOwned(BlobLifetimePhaseV3::release, invariant_operation,
                        RuntimeTerminated());
  return RuntimeSuccess();
}

BlobLifetimeRuntimeResultV3 RetainBaseBlobLifetimeLeaseV3Generation1(
    const BlobValidatedProfileHandleV3& profile,
    BlobLifetimeAuthorityAdmissionV3Generation1&& trusted_admission,
    const BlobBoundMaterializedCarrierV3Generation1& carrier,
    const BlobLifetimeTokenV3& token,
    const BlobLifetimeUseRequestV3& request,
    BlobRetainedLifetimeLeaseV3& destination) noexcept {
  return destination.RetainFrom(profile, trusted_admission, carrier, token,
                                request, request.operation);
}

BlobLifetimeRuntimeResultV3 BlobRetainedLifetimeLeaseV3::Probe() noexcept {
  EntrantGuard entrant(state_);
  if (!entrant.acquired()) {
    const State observed = state_.load(std::memory_order_acquire);
    return observed == State::released || observed == State::terminal ||
            observed == State::moved_from
        ? LocalStateFailure(observed) : RuntimeTerminated();
  }
  return ProbeInternal(false);
}

BlobLifetimeRuntimeResultV3 BlobRetainedLifetimeLeaseV3::ProbeInternal(
    bool final_probe, bool owner_already_active) noexcept {
  if (MarkActiveBindingReentry(token_))
    return ProtocolFailure("callback_reentrant", descriptor_uuid_,
                           descriptor_generation_);
  State expected = owner_already_active ? State::access_active : State::idle;
  if (!state_.compare_exchange_strong(expected, State::callback_active,
                                       std::memory_order_acq_rel)) {
    if (expected == State::moved_from) return MovedBindingFailure();
    if (expected == State::retaining || expected == State::callback_active ||
        expected == State::access_active || expected == State::moving ||
        expected == State::releasing || expected == State::installing ||
        callback_active_.load(std::memory_order_acquire)) {
      return RejectOverlappingUse(BlobLifetimePhaseV3::probe);
    }
    return LocalStateFailure(expected);
  }
  if (!owner_already_active) suppress_publication_gate_ = false;
  if (state_.Deferred() != 0)
    return ReleaseOwned(BlobLifetimePhaseV3::release, request_.operation);
  auto gate = OrdinaryGate(
      final_probe ? BlobLifetimeGateV3::before_final_probe
                  : BlobLifetimeGateV3::before_ordinary_probe,
      BlobLifetimePublicPhaseV3::probe, BlobLifetimePhaseV3::probe);
  if (!gate.ok()) {
      return ReleaseOwned(BlobLifetimePhaseV3::release,
                           request_.operation, gate);
  }
  u64 available = 0;
  if (!admission_.budget_->ChargeOrdinary(1, &available)) {
      if (admission_.budget_->terminal_quarantined()) {
    (void)PublishDeferredInvariant(BlobLifetimeInvariantEventV3::callback_budget_counter_corrupt, request_.operation,
                                   BlobLifetimePhaseV3::probe, 255);
      return ReleaseOwned(BlobLifetimePhaseV3::release,
                             request_.operation, RuntimeTerminated());
    }
    const auto failure = BudgetFailure(
        BlobLifetimeResourceV3::lifetime_callback_calls, 1, available);
    return ReleaseOwned(BlobLifetimePhaseV3::release,
                           request_.operation, failure);
  }
  BlobLifetimeCallbackResultV3 callback{};
  const BlobLifetimeRetainTicketV3 retained_before_callback = retain_ticket_;
  try {
    LifetimeCallbackScopeV3 callback_scope(
        callback_active_, token_, BlobLifetimePhaseV3::probe,
        request_.operation, this,
          &BlobRetainedLifetimeLeaseV3::PublishDeferredThunk);
    callback = admission_.authority_.probe(
        admission_.authority_.authority_context, &token_, &retain_ticket_,
        &request_);
  } catch (...) {
    retain_ticket_ = retained_before_callback;
    (void)PublishDeferredInvariant(
        BlobLifetimeInvariantEventV3::adapter_exception,
        request_.operation, BlobLifetimePhaseV3::probe, 255);
    const auto primary = DeferredInvariantPrimary(
        state_.Deferred(), descriptor_uuid_, descriptor_generation_);
    return ReleaseOwned(BlobLifetimePhaseV3::release,
                        request_.operation, primary);
  }
  const bool dirty_retain_ticket =
      !RetainTicketEqual(retain_ticket_, retained_before_callback);
  if (dirty_retain_ticket) retain_ticket_ = retained_before_callback;
  const u64 probe_forced =
      state_.Deferred();
  if (probe_forced != 0) {
    quarantined_.store(true, std::memory_order_release);
    auto primary = DeferredInvariantPrimary(
        probe_forced, descriptor_uuid_, descriptor_generation_);
    if (ResultShapeValid(callback) &&
        callback.code == SB_BLOB_CALLBACK_SECURITY_DENIED_V3)
      primary = CallbackFailure(callback.code, BlobLifetimePhaseV3::probe,
                                request_, descriptor_uuid_, descriptor_generation_);
    return ReleaseOwned(BlobLifetimePhaseV3::release, request_.operation, primary);
  }
  if (dirty_retain_ticket) {
    (void)PublishDeferredInvariant(
        BlobLifetimeInvariantEventV3::dirty_failure_output,
        request_.operation, BlobLifetimePhaseV3::probe,
        ResultShapeValid(callback) && callback.code <= 18
            ? callback.code : 255);
    const auto callback_failure = ResultShapeValid(callback) &&
            callback.code != SB_BLOB_CALLBACK_OK_V3
        ? CallbackFailure(callback.code, BlobLifetimePhaseV3::probe,
                          request_, descriptor_uuid_, descriptor_generation_)
        : RuntimeSuccess();
    const auto selected = callback_failure.fact.fact_class ==
            BlobLifetimeFactClassV3::security
        ? callback_failure
        : ProtocolFailure("callback_output_impossible", descriptor_uuid_,
                          descriptor_generation_, BlobLifetimeFactStageV3::callback_return);
    return ReleaseOwned(BlobLifetimePhaseV3::release,
                           request_.operation, selected);
  }
  if (!ResultShapeValid(callback)) {
    (void)PublishDeferredInvariant(
        BlobLifetimeInvariantEventV3::callback_result_unknown,
        request_.operation, BlobLifetimePhaseV3::probe,
        callback.code <= 18 ? callback.code : 255);
    const auto malformed = ProtocolFailure("callback_result_unknown", descriptor_uuid_, descriptor_generation_, BlobLifetimeFactStageV3::callback_return);
    return ReleaseOwned(BlobLifetimePhaseV3::release,
                           request_.operation, malformed);
  }
  const auto callback_result = callback.code == SB_BLOB_CALLBACK_OK_V3
      ? RuntimeSuccess()
      : CallbackFailure(callback.code, BlobLifetimePhaseV3::probe,
                        request_, descriptor_uuid_, descriptor_generation_);
  gate = OrdinaryGate(
      final_probe ? BlobLifetimeGateV3::after_final_probe
                  : BlobLifetimeGateV3::after_ordinary_probe,
      BlobLifetimePublicPhaseV3::probe, BlobLifetimePhaseV3::probe);
  const auto selected = SelectByV7Precedence(callback_result, gate);
  if (!selected.ok()) {
    if (CallbackRequiresQuarantine(BlobLifetimePhaseV3::probe,
                                   callback.code)) {
      (void)PublishDeferredInvariant(
          BlobLifetimeInvariantEventV3::callback_result_unknown,
          request_.operation, BlobLifetimePhaseV3::probe,
          callback.code <= 18 ? callback.code : 255);
    }
    return ReleaseOwned(BlobLifetimePhaseV3::release,
                           request_.operation, selected);
  }
  (void)state_.BeginClosing();
  state_.WaitForClosingQuiescence(1);
  if (state_.Deferred() != 0)
    return ReleaseOwned(BlobLifetimePhaseV3::release, request_.operation);
  if (!state_.TryFinalize(State::idle, 1))
    return ReleaseOwned(BlobLifetimePhaseV3::release, request_.operation,
                        RuntimeTerminated());
  return selected;
}

BlobLifetimeRuntimeResultV3
BlobRetainedLifetimeLeaseV3::VisitBoundMaterializedBytes(
    const BlobTrustedInternalVisitorV3& visitor) noexcept {
  EntrantGuard entrant(state_);
  if (!entrant.acquired()) {
    const State observed = state_.load(std::memory_order_acquire);
    return observed == State::released || observed == State::terminal ||
            observed == State::moved_from
        ? LocalStateFailure(observed) : RuntimeTerminated();
  }
  if (MarkActiveBindingReentry(token_))
    return ProtocolFailure("callback_reentrant", descriptor_uuid_,
                           descriptor_generation_);
  if (visitor.function_ == nullptr) {
    const State observed = state_.load(std::memory_order_acquire);
    if (observed == State::idle)
      return ProtocolFailure("visitor_missing", descriptor_uuid_,
                             descriptor_generation_);
    if (observed == State::disarmed || observed == State::released ||
        observed == State::moved_from || observed == State::terminal)
      return LocalStateFailure(observed);
    return RejectOverlappingUse(BlobLifetimePhaseV3::begin_access);
  }
  State expected = State::idle;
  if (!state_.compare_exchange_strong(expected, State::access_active,
                                       std::memory_order_acq_rel)) {
    if (expected == State::moved_from) return MovedBindingFailure();
    if (expected == State::retaining || expected == State::callback_active ||
        expected == State::access_active || expected == State::moving ||
        expected == State::releasing || expected == State::installing ||
        callback_active_.load(std::memory_order_acquire)) {
      return RejectOverlappingUse(BlobLifetimePhaseV3::begin_access);
    }
    return LocalStateFailure(expected);
  }
  suppress_publication_gate_ = false;
  if (state_.Deferred() != 0)
    return ReleaseOwned(BlobLifetimePhaseV3::release, request_.operation);
  auto primary = OrdinaryGate(BlobLifetimeGateV3::before_begin_access,
                              BlobLifetimePublicPhaseV3::begin_access,
                              BlobLifetimePhaseV3::begin_access);
  if (!primary.ok()) {
      return ReleaseOwned(BlobLifetimePhaseV3::release,
                           request_.operation, primary);
  }
  u64 available = 0;
  if (!admission_.budget_->ReserveOrdinaryAndCleanup(1, 1, &available)) {
      if (admission_.budget_->terminal_quarantined()) {
    (void)PublishDeferredInvariant(BlobLifetimeInvariantEventV3::callback_budget_counter_corrupt, request_.operation,
                                   BlobLifetimePhaseV3::begin_access, 255);
      return ReleaseOwned(BlobLifetimePhaseV3::release,
                             request_.operation, RuntimeTerminated());
    }
    const auto failure = BudgetFailure(
        BlobLifetimeResourceV3::lifetime_callback_calls, 2, available);
    return ReleaseOwned(BlobLifetimePhaseV3::release,
                           request_.operation, failure);
  }
  BlobLifetimeAccessTicketV3 access{};
  BlobLifetimeCallbackResultV3 begin{};
  bool begin_threw = false;
  try {
    LifetimeCallbackScopeV3 callback_scope(
        callback_active_, token_, BlobLifetimePhaseV3::begin_access,
        request_.operation, this,
          &BlobRetainedLifetimeLeaseV3::PublishDeferredThunk);
    begin = admission_.authority_.begin_access(
        admission_.authority_.authority_context, &token_, &retain_ticket_,
        &request_, &access);
  } catch (...) {
    begin_threw = true;
  }
  const auto conservative_end = [&]() noexcept {
    BlobLifetimeRuntimeResultV3 cleanup_public = RuntimeSuccess();
    const auto cleanup = admission_.budget_->ConsumeCleanup(1);
    const bool cleanup_counter_corrupt =
        !cleanup.obligation_consumed || cleanup.ledger_terminal_quarantined;
    if (cleanup_counter_corrupt) {
      (void)PublishDeferredInvariant(
          BlobLifetimeInvariantEventV3::callback_budget_counter_corrupt,
          request_.operation, BlobLifetimePhaseV3::end_access, 255);
    }
    BlobLifetimeCallbackResultV3 ended{};
    bool end_threw = false;
    try {
      LifetimeCallbackScopeV3 callback_scope(
          callback_active_, token_, BlobLifetimePhaseV3::end_access,
          request_.operation, this,
          &BlobRetainedLifetimeLeaseV3::PublishDeferredThunk);
      ended = admission_.authority_.end_access(
          admission_.authority_.authority_context, &token_, &retain_ticket_,
          &access, &request_);
    } catch (...) {
      end_threw = true;
    }
    if (end_threw) {
      (void)PublishDeferredInvariant(
          BlobLifetimeInvariantEventV3::adapter_exception,
          request_.operation, BlobLifetimePhaseV3::end_access, 255);
    } else if (!ResultShapeValid(ended)) {
      (void)PublishDeferredInvariant(
          BlobLifetimeInvariantEventV3::callback_result_unknown,
          request_.operation, BlobLifetimePhaseV3::end_access,
          ended.code <= 18 ? ended.code : 255);
      if (cleanup_public.ok())
        cleanup_public = ProtocolFailure("callback_result_unknown", descriptor_uuid_, descriptor_generation_, BlobLifetimeFactStageV3::cleanup_return);
    } else if (!AllZero(access)) {
      (void)PublishDeferredInvariant(
          BlobLifetimeInvariantEventV3::dirty_failure_output,
          request_.operation, BlobLifetimePhaseV3::end_access, ended.code);
      const auto callback_failure = ended.code != SB_BLOB_CALLBACK_OK_V3
          ? CallbackFailure(ended.code, BlobLifetimePhaseV3::end_access,
                            request_, descriptor_uuid_, descriptor_generation_)
          : RuntimeSuccess();
      cleanup_public = callback_failure.fact.fact_class ==
              BlobLifetimeFactClassV3::security
          ? callback_failure
          : ProtocolFailure("callback_output_impossible", descriptor_uuid_,
                            descriptor_generation_, BlobLifetimeFactStageV3::cleanup_return);
    } else if (ended.code != SB_BLOB_CALLBACK_OK_V3) {
      (void)PublishDeferredInvariant(
          BlobLifetimeInvariantEventV3::end_access_failed,
          request_.operation, BlobLifetimePhaseV3::end_access, ended.code);
      cleanup_public = CallbackFailure(
          ended.code, BlobLifetimePhaseV3::end_access, request_,
          descriptor_uuid_, descriptor_generation_);
    }
    access = {};
    if (cleanup_counter_corrupt &&
        cleanup_public.fact.fact_class != BlobLifetimeFactClassV3::security)
      cleanup_public = RuntimeTerminated();
    return cleanup_public;
  };
  const auto cancel_prospective_end = [&]() noexcept {
    if (admission_.budget_->CancelCleanup(1)) return true;
    (void)PublishDeferredInvariant(
        BlobLifetimeInvariantEventV3::callback_budget_counter_corrupt,
        request_.operation, BlobLifetimePhaseV3::begin_access, 255);
    return false;
  };

  const u64 begin_forced =
      state_.Deferred();
  if (begin_forced != 0) {
    auto selected = DeferredInvariantPrimary(begin_forced, descriptor_uuid_,
                                              descriptor_generation_);
    const bool recognized = !begin_threw && ResultShapeValid(begin);
    if (recognized && begin.code == SB_BLOB_CALLBACK_SECURITY_DENIED_V3)
      selected = CallbackFailure(begin.code, BlobLifetimePhaseV3::begin_access,
                                 request_, descriptor_uuid_, descriptor_generation_);
    const bool obligation = recognized
        ? begin.code == SB_BLOB_CALLBACK_OK_V3
        : !AllZero(access) || (!begin_threw && begin.code == SB_BLOB_CALLBACK_OK_V3);
    if (obligation) {
      const auto cleanup = conservative_end();
      if (cleanup.fact.fact_class == BlobLifetimeFactClassV3::security)
        selected = cleanup;
    } else {
      (void)cancel_prospective_end();
      access = {};
    }
    return ReleaseOwned(BlobLifetimePhaseV3::release, request_.operation, selected);
  }
  if (begin_threw || !ResultShapeValid(begin)) {
    (void)PublishDeferredInvariant(
        begin_threw ? BlobLifetimeInvariantEventV3::adapter_exception
                    : BlobLifetimeInvariantEventV3::callback_result_unknown,
        request_.operation, BlobLifetimePhaseV3::begin_access,
        begin_threw ? 255 : (begin.code <= 18 ? begin.code : 255));
    BlobLifetimeRuntimeResultV3 selected = begin_threw
        ? RuntimeTerminated()
        : ProtocolFailure("callback_result_unknown", descriptor_uuid_, descriptor_generation_, BlobLifetimeFactStageV3::callback_return);
    const bool conservative_obligation = !AllZero(access) ||
        (!begin_threw && begin.code == SB_BLOB_CALLBACK_OK_V3);
    if (conservative_obligation) {
      const auto cleanup = conservative_end();
      if (cleanup.fact.fact_class == BlobLifetimeFactClassV3::security)
        selected = cleanup;
    } else {
      const bool cancelled = cancel_prospective_end();
      access = {};
      if (!cancelled) selected = RuntimeTerminated();
    }
      return ReleaseOwned(BlobLifetimePhaseV3::release,
                           request_.operation, selected);
  }
  if (begin.code != SB_BLOB_CALLBACK_OK_V3) {
    const bool cancelled = cancel_prospective_end();
      BlobLifetimeRuntimeResultV3 selected = CallbackFailure(
        begin.code, BlobLifetimePhaseV3::begin_access, request_,
        descriptor_uuid_, descriptor_generation_);
    if (!AllZero(access)) {
      (void)PublishDeferredInvariant(
          BlobLifetimeInvariantEventV3::dirty_failure_output,
          request_.operation, BlobLifetimePhaseV3::begin_access, begin.code);
      access = {};
      if (selected.fact.fact_class != BlobLifetimeFactClassV3::security)
        selected = ProtocolFailure("callback_output_impossible",
                                   descriptor_uuid_, descriptor_generation_, BlobLifetimeFactStageV3::callback_return);
    } else {
      const auto after_gate = OrdinaryGate(
          BlobLifetimeGateV3::after_begin_access,
          BlobLifetimePublicPhaseV3::begin_access,
          BlobLifetimePhaseV3::begin_access);
      selected = SelectByV7Precedence(selected, after_gate);
      if (CallbackRequiresQuarantine(BlobLifetimePhaseV3::begin_access,
                                     begin.code)) {
        (void)PublishDeferredInvariant(
            BlobLifetimeInvariantEventV3::callback_result_unknown,
            request_.operation, BlobLifetimePhaseV3::begin_access,
            begin.code);
      }
    }
    if (!cancelled &&
        selected.fact.fact_class != BlobLifetimeFactClassV3::security)
      selected = RuntimeTerminated();
    return ReleaseOwned(BlobLifetimePhaseV3::release,
                           request_.operation, selected);
  }
  if (!AccessTicketValid(access, retain_ticket_, token_)) {
    (void)PublishDeferredInvariant(
        BlobLifetimeInvariantEventV3::malformed_success_ticket,
        request_.operation, BlobLifetimePhaseV3::begin_access,
        SB_BLOB_CALLBACK_OK_V3);
    BlobLifetimeRuntimeResultV3 selected = ProtocolFailure(
        "callback_output_impossible", descriptor_uuid_,
        descriptor_generation_, BlobLifetimeFactStageV3::callback_return);
    const auto cleanup = conservative_end();
    if (cleanup.fact.fact_class == BlobLifetimeFactClassV3::security) {
      selected = cleanup;
    }
    return ReleaseOwned(BlobLifetimePhaseV3::release,
                        request_.operation, selected);
  }

  primary = OrdinaryGate(BlobLifetimeGateV3::after_begin_access,
                         BlobLifetimePublicPhaseV3::begin_access,
                         BlobLifetimePhaseV3::begin_access);
  if (primary.ok())
    primary = OrdinaryGate(BlobLifetimeGateV3::before_protected_work,
                           BlobLifetimePublicPhaseV3::protected_access,
                           BlobLifetimePhaseV3::protected_access);
  u64 byte_available = 0;
  if (primary.ok() && !admission_.budget_->Charge(
          BlobLifetimeResourceV3::logical_bytes_read,
          carrier_logical_length_, &byte_available)) {
    if (admission_.budget_->terminal_quarantined()) {
    (void)PublishDeferredInvariant(BlobLifetimeInvariantEventV3::callback_budget_counter_corrupt, request_.operation,
                                   BlobLifetimePhaseV3::protected_access, 255);
      primary = RuntimeTerminated();
    } else {
      primary = BudgetFailure(BlobLifetimeResourceV3::logical_bytes_read,
                              carrier_logical_length_, byte_available);
    }
  }
  BlobProtectedWorkResultV3 work{};
  if (primary.ok()) {
    try {
      work = visitor.function_(visitor.context_,
          std::span<const byte>(carrier_data_,
              static_cast<std::size_t>(carrier_logical_length_)));
    } catch (...) {
      primary = RuntimeTerminated();
      (void)PublishDeferredInvariant(BlobLifetimeInvariantEventV3::adapter_exception, request_.operation,
                                   BlobLifetimePhaseV3::protected_access, 255);
    }
    if (primary.ok() && ((work.status != 0 && work.status != 1) ||
                         !AllZero(work.reserved))) {
      primary = RuntimeTerminated();
    (void)PublishDeferredInvariant(BlobLifetimeInvariantEventV3::receiver_services_failed, request_.operation,
                                   BlobLifetimePhaseV3::protected_access, 255);
    } else if (primary.ok() && work.status != 0) {
      primary = RuntimeFromProtectedFact(work.fact);
      if (primary.disposition ==
          BlobLifetimeOuterDispositionV3::terminated_out_of_band) {
    (void)PublishDeferredInvariant(BlobLifetimeInvariantEventV3::receiver_services_failed, request_.operation,
                                   BlobLifetimePhaseV3::protected_access, 255);
      }
    }
    if (primary.ok())
      primary = OrdinaryGate(BlobLifetimeGateV3::after_protected_work,
                             BlobLifetimePublicPhaseV3::protected_access,
                             BlobLifetimePhaseV3::protected_access);
  }

  return EndAccessInternal(access, primary);
}

BlobLifetimeRuntimeResultV3 BlobRetainedLifetimeLeaseV3::EndAccessInternal(
    BlobLifetimeAccessTicketV3 access,
    BlobLifetimeRuntimeResultV3 primary) noexcept {
  BlobLifetimeCallbackResultV3 ended{};
  const auto end_cleanup = admission_.budget_->ConsumeCleanup(1);
  if (!end_cleanup.obligation_consumed ||
      end_cleanup.ledger_terminal_quarantined) {
    (void)PublishDeferredInvariant(
        BlobLifetimeInvariantEventV3::callback_budget_counter_corrupt,
        request_.operation, BlobLifetimePhaseV3::end_access, 255);
  }
  bool end_threw = false;
  try {
    LifetimeCallbackScopeV3 callback_scope(
        callback_active_, token_, BlobLifetimePhaseV3::end_access,
        request_.operation, this,
          &BlobRetainedLifetimeLeaseV3::PublishDeferredThunk);
    ended = admission_.authority_.end_access(
        admission_.authority_.authority_context, &token_, &retain_ticket_,
        &access, &request_);
  } catch (...) {
    end_threw = true;
  }

  if (end_threw) {
    (void)PublishDeferredInvariant(
        BlobLifetimeInvariantEventV3::adapter_exception, request_.operation,
        BlobLifetimePhaseV3::end_access, 255);
  } else if (!ResultShapeValid(ended)) {
    (void)PublishDeferredInvariant(
        BlobLifetimeInvariantEventV3::callback_result_unknown,
        request_.operation, BlobLifetimePhaseV3::end_access,
        ended.code <= 18 ? ended.code : 255);
    if (primary.ok())
      primary = ProtocolFailure("callback_result_unknown", descriptor_uuid_,
                                descriptor_generation_, BlobLifetimeFactStageV3::cleanup_return);
  } else if (!AllZero(access)) {
    // The exact private whitelist calls forbidden phase-ticket material at
    // end_access dirty_failure_output. The public malformed-OK primary still
    // uses callback_output_impossible.
    (void)PublishDeferredInvariant(
        BlobLifetimeInvariantEventV3::dirty_failure_output,
        request_.operation, BlobLifetimePhaseV3::end_access, ended.code);
    const auto callback_failure = ended.code != SB_BLOB_CALLBACK_OK_V3
        ? CallbackFailure(ended.code, BlobLifetimePhaseV3::end_access,
                          request_, descriptor_uuid_, descriptor_generation_)
        : RuntimeSuccess();
    if (callback_failure.fact.fact_class ==
        BlobLifetimeFactClassV3::security) {
      primary = callback_failure;
    } else if (primary.ok()) {
      primary = ProtocolFailure("callback_output_impossible",
                                descriptor_uuid_, descriptor_generation_, BlobLifetimeFactStageV3::cleanup_return);
    }
  } else if (ended.code != SB_BLOB_CALLBACK_OK_V3) {
    (void)PublishDeferredInvariant(
        BlobLifetimeInvariantEventV3::end_access_failed, request_.operation,
        BlobLifetimePhaseV3::end_access, ended.code);
    const auto cleanup_failure = CallbackFailure(
        ended.code, BlobLifetimePhaseV3::end_access, request_,
        descriptor_uuid_, descriptor_generation_);
    if (primary.ok() ||
        cleanup_failure.fact.fact_class == BlobLifetimeFactClassV3::security)
      primary = cleanup_failure;
  }
  access = {};
  if (state_.Deferred() != 0 ||
      !primary.ok()) {
    return ReleaseOwned(BlobLifetimePhaseV3::release,
                           request_.operation, primary);
  }
  return ProbeInternal(true, true);
}

BlobLifetimeRuntimeResultV3 BlobRetainedLifetimeLeaseV3::ReleaseOwned(
    BlobLifetimePhaseV3 callback_phase, u8 invariant_operation,
    BlobLifetimeRuntimeResultV3 prior_primary) noexcept {
  return ReleaseInternal(callback_phase, invariant_operation, prior_primary,
                         true);
}

BlobLifetimeRuntimeResultV3 BlobRetainedLifetimeLeaseV3::ReleaseInternal(
    BlobLifetimePhaseV3 callback_phase,
    u8 invariant_operation,
    BlobLifetimeRuntimeResultV3 prior_primary,
    bool owner_already_active) noexcept {
  if (prior_primary.diagnostic_code.empty() &&
      prior_primary.disposition == BlobLifetimeOuterDispositionV3::public_failure)
    prior_primary = RuntimeSuccess();
  if (MarkActiveBindingReentry(token_))
    return ProtocolFailure("callback_reentrant", descriptor_uuid_,
                           descriptor_generation_);
  State expected = owner_already_active
      ? state_.load(std::memory_order_acquire) : State::idle;
  if (owner_already_active &&
      expected != State::retaining && expected != State::callback_active &&
      expected != State::access_active && expected != State::moving &&
      expected != State::releasing) {
    return RuntimeTerminated();
  }
  if (!state_.compare_exchange_strong(expected, State::releasing,
                                       std::memory_order_acq_rel)) {
    if (expected == State::disarmed || expected == State::released ||
        expected == State::moved_from) {
      if (callback_phase == BlobLifetimePhaseV3::destructor_cleanup)
        return RuntimeSuccess();
      return LocalStateFailure(expected);
    }
    return RejectOverlappingUse(callback_phase, invariant_operation);
  }
  if (!owner_already_active) suppress_publication_gate_ = false;
  const auto release_cleanup = admission_.budget_->ConsumeCleanup(1);
  const bool cleanup_counter_corrupt =
      !release_cleanup.obligation_consumed ||
      release_cleanup.ledger_terminal_quarantined;
  const u8 outer_operation = invariant_operation;
  if (!release_cleanup.obligation_consumed) {
    (void)PublishDeferredInvariant(
        BlobLifetimeInvariantEventV3::callback_budget_counter_corrupt,
        outer_operation, callback_phase, 255);
  }
  if (release_cleanup.ledger_terminal_quarantined) {
    (void)PublishDeferredInvariant(
        BlobLifetimeInvariantEventV3::callback_budget_counter_corrupt,
        outer_operation, callback_phase, 255);
  }
  BlobLifetimeCallbackResultV3 callback{};
  bool threw = false;
  try {
    LifetimeCallbackScopeV3 callback_scope(
        callback_active_, token_, callback_phase,
        invariant_operation, this,
        &BlobRetainedLifetimeLeaseV3::PublishDeferredThunk);
    callback = admission_.authority_.release(
        admission_.authority_.authority_context, &token_, &retain_ticket_);
  } catch (...) {
    threw = true;
  }
  const bool dirty_ticket = !AllZero(retain_ticket_);
  if (threw) {
    (void)PublishDeferredInvariant(
        BlobLifetimeInvariantEventV3::adapter_exception,
        outer_operation, callback_phase, 255);
  } else if (!ResultShapeValid(callback)) {
    (void)PublishDeferredInvariant(
        BlobLifetimeInvariantEventV3::callback_result_unknown,
        outer_operation, callback_phase,
        callback.code <= 18 ? callback.code : 255);
  } else if (dirty_ticket) {
    // release has no legal malformed_success_ticket invariant row. Any
    // returned ticket material is dirty_failure_output even when code is OK.
    (void)PublishDeferredInvariant(
        BlobLifetimeInvariantEventV3::dirty_failure_output,
        outer_operation, callback_phase, callback.code);
  } else if (callback.code != SB_BLOB_CALLBACK_OK_V3) {
    (void)PublishDeferredInvariant(
        BlobLifetimeInvariantEventV3::release_failed,
        outer_operation, callback_phase, callback.code);
  }
  ZeroTicket();

  BlobLifetimeRuntimeResultV3 cleanup_public = RuntimeSuccess();
  if (!threw && dirty_ticket) {
    const auto callback_failure = ResultShapeValid(callback) &&
            callback.code != SB_BLOB_CALLBACK_OK_V3
        ? CallbackFailure(callback.code, callback_phase, request_,
                          descriptor_uuid_, descriptor_generation_)
        : RuntimeSuccess();
    cleanup_public = callback_failure.fact.fact_class ==
            BlobLifetimeFactClassV3::security
        ? callback_failure
        : ProtocolFailure("callback_output_impossible", descriptor_uuid_,
                          descriptor_generation_, BlobLifetimeFactStageV3::cleanup_return);
  }
  if (!threw && !ResultShapeValid(callback) && cleanup_public.ok())
    cleanup_public = ProtocolFailure("callback_result_unknown", descriptor_uuid_, descriptor_generation_, BlobLifetimeFactStageV3::cleanup_return);
  if (!threw && !dirty_ticket && ResultShapeValid(callback) &&
      callback.code != SB_BLOB_CALLBACK_OK_V3) {
    cleanup_public = CallbackFailure(callback.code, callback_phase, request_,
                                     descriptor_uuid_, descriptor_generation_);
  }
  (void)state_.BeginClosing();
  const u32 owner_entrants =
      callback_phase == BlobLifetimePhaseV3::destructor_cleanup ? 0 : 1;
  state_.WaitForClosingQuiescence(owner_entrants);
  const u64 deferred = state_.PeekDeferredWhileClosing();
  BlobLifetimeRuntimeResultV3 invariant_result = RuntimeSuccess();
  if (deferred != 0) {
    invariant_result = ReportInvariant(
        DeferredEvent(deferred), DeferredPhase(deferred),
        DecodeDeferredCallbackCode(deferred), DeferredOperation(deferred),
        owner_entrants);
    if (!state_.CompleteDeferredServiceReceipt(deferred))
      std::terminate();
  }
  BlobLifetimeRuntimeResultV3 selected = prior_primary.ok()
      ? cleanup_public : prior_primary;
  if (!cleanup_public.ok() &&
      cleanup_public.fact.fact_class == BlobLifetimeFactClassV3::security)
    selected = cleanup_public;
  if (selected.ok() && cleanup_counter_corrupt) {
    selected = RuntimeTerminated();
  } else if (deferred != 0 &&
      selected.ok()) {
    selected = invariant_result;
  }
  if (selected.ok() &&
      callback_phase != BlobLifetimePhaseV3::destructor_cleanup &&
             !suppress_publication_gate_) {
    selected = OrdinaryGate(
        BlobLifetimeGateV3::before_publication_or_return,
        BlobLifetimePublicPhaseV3::atomic_commit,
        BlobLifetimePhaseV3::atomic_commit);
  }
  bool invariant_seen = deferred != 0;
  for (;;) {
    const State final_state = invariant_seen ? State::terminal
                                             : State::released;
    if (state_.TryFinalize(final_state, owner_entrants))
      break;
    const u64 raced = state_.PeekDeferredWhileClosing();
    if (raced == 0) continue;
    invariant_seen = true;
    const BlobLifetimeRuntimeResultV3 raced_result = ReportInvariant(
        DeferredEvent(raced), DeferredPhase(raced),
        DecodeDeferredCallbackCode(raced), DeferredOperation(raced),
        owner_entrants);
    if (!state_.CompleteDeferredServiceReceipt(raced))
      std::terminate();
    if (selected.ok()) selected = raced_result;
  }
  carrier_binding_ = {};
  carrier_data_ = nullptr;
  carrier_logical_length_ = 0;
  descriptor_uuid_ = {};
  descriptor_generation_ = 0;
  admission_.DropPin();
  return selected;
}

BlobLifetimeRuntimeResultV3 BlobRetainedLifetimeLeaseV3::ReportInvariant(
    BlobLifetimeInvariantEventV3 event,
    BlobLifetimePhaseV3 phase,
    u8 callback_code_or_255,
    u8 operation_override_or_zero,
    u32 owner_entrants) noexcept {
  // Every already-reserved callback must have been consumed by the caller.
  // Closing rejects new entrants. This acquire observation proves every
  // earlier non-owner metadata entrant has retired before local bytes are
  // cleared or the receiver service is entered.
  state_.WaitForClosingQuiescence(owner_entrants);
  const platform::Uuid invariant_descriptor_uuid = descriptor_uuid_;
  const u64 invariant_descriptor_generation = descriptor_generation_;
  PrepareInvariantLocalCleanup();
  if (gInvariantServiceActiveV3) std::terminate();
  u8 invariant_operation = operation_override_or_zero != 0
      ? operation_override_or_zero : request_.operation;
  if (phase == BlobLifetimePhaseV3::move_construct) invariant_operation = 16;
  if (phase == BlobLifetimePhaseV3::move_replace) invariant_operation = 17;
  if (phase == BlobLifetimePhaseV3::destructor_cleanup)
    invariant_operation = 19;
  const BlobLifetimeBindingKeyV3 key{
      platform::Uuid{{token_.authority_instance_uuid.bytes[0],
                      token_.authority_instance_uuid.bytes[1],
                      token_.authority_instance_uuid.bytes[2],
                      token_.authority_instance_uuid.bytes[3],
                      token_.authority_instance_uuid.bytes[4],
                      token_.authority_instance_uuid.bytes[5],
                      token_.authority_instance_uuid.bytes[6],
                      token_.authority_instance_uuid.bytes[7],
                      token_.authority_instance_uuid.bytes[8],
                      token_.authority_instance_uuid.bytes[9],
                      token_.authority_instance_uuid.bytes[10],
                      token_.authority_instance_uuid.bytes[11],
                      token_.authority_instance_uuid.bytes[12],
                      token_.authority_instance_uuid.bytes[13],
                      token_.authority_instance_uuid.bytes[14],
                      token_.authority_instance_uuid.bytes[15]}},
      token_.authority_instance_generation,
      platform::Uuid{{token_.lifetime_token_uuid.bytes[0],
                      token_.lifetime_token_uuid.bytes[1],
                      token_.lifetime_token_uuid.bytes[2],
                      token_.lifetime_token_uuid.bytes[3],
                      token_.lifetime_token_uuid.bytes[4],
                      token_.lifetime_token_uuid.bytes[5],
                      token_.lifetime_token_uuid.bytes[6],
                      token_.lifetime_token_uuid.bytes[7],
                      token_.lifetime_token_uuid.bytes[8],
                      token_.lifetime_token_uuid.bytes[9],
                      token_.lifetime_token_uuid.bytes[10],
                      token_.lifetime_token_uuid.bytes[11],
                      token_.lifetime_token_uuid.bytes[12],
                      token_.lifetime_token_uuid.bytes[13],
                      token_.lifetime_token_uuid.bytes[14],
                      token_.lifetime_token_uuid.bytes[15]}},
      token_.lifetime_token_generation,
      token_.immutable_binding_generation};
  const BlobLifetimeInvariantFactV3 fact{
      event, invariant_operation, phase, callback_code_or_255,
      token_.authority_instance_generation,
      token_.lifetime_token_generation,
      token_.immutable_binding_generation};
  // The exact 647-row triple whitelist and every scalar/binding domain are
  // enforced locally before service entry. An illegal fact is private
  // corruption and is never normalized or delivered to the receiver.
  if (!ValidateBlobLifetimeInvariantFactDomainV3(key, fact)) std::terminate();
  gInvariantServiceActiveV3 = true;
  try {
    const auto effect = admission_.services_.record_invariant_and_quarantine(
        admission_.services_.context, key, fact);
    if (!AllZero(effect.reserved) ||
        (effect.status != BlobReceiverEffectStatusV3::accepted &&
         effect.status != BlobReceiverEffectStatusV3::already_applied)) {
      std::terminate();
    }
    // In this exact eight-byte service ABI, accepted/already_applied is the
    // typed receipt that the receiver atomically recorded the fact,
    // quarantined the binding, and retained its receiver-owned reclamation
    // pin. Wrapper admission and budget pins may be dropped only afterward.
    admission_.receiver_reclamation_pin_established_ = true;
    gInvariantServiceActiveV3 = false;
  } catch (...) {
    std::terminate();
  }
  if (event == BlobLifetimeInvariantEventV3::same_ticket_concurrent_use)
    return ConcurrentUseFailure(invariant_descriptor_uuid,
                                invariant_descriptor_generation);
  if (event == BlobLifetimeInvariantEventV3::callback_reentrant)
    return ProtocolFailure("callback_reentrant", invariant_descriptor_uuid,
                           invariant_descriptor_generation);
  return RuntimeTerminated();
}

BlobLifetimeRuntimeResultV3 BlobRetainedLifetimeLeaseV3::OrdinaryGate(
    BlobLifetimeGateV3 gate,
    BlobLifetimePublicPhaseV3 public_phase,
    BlobLifetimePhaseV3 invariant_phase,
    u8 invariant_operation_override_or_zero) noexcept {
  const auto clock = admission_.services_.read_monotonic_ns(
      admission_.services_.context);
  const bool clock_reserved_valid = AllZero(clock.reserved);
  const bool clock_shape_valid = clock_reserved_valid &&
      (clock.status == BlobReceiverSampleStatusV3::ok ||
       clock.status == BlobReceiverSampleStatusV3::unavailable);
  if (clock_shape_valid && clock.status == BlobReceiverSampleStatusV3::ok &&
      has_prior_monotonic_ns_ && clock.now_monotonic_ns < prior_monotonic_ns_) {
    suppress_publication_gate_ = true;
    quarantined_.store(true, std::memory_order_release);
    (void)PublishDeferredInvariant(
        BlobLifetimeInvariantEventV3::monotonic_clock_regression,
        invariant_operation_override_or_zero != 0
            ? invariant_operation_override_or_zero : request_.operation,
        invariant_phase, 255);
    return RuntimeTerminated();
  }
  if (clock_shape_valid && clock.status == BlobReceiverSampleStatusV3::ok) {
    prior_monotonic_ns_ = clock.now_monotonic_ns;
    has_prior_monotonic_ns_ = true;
  }
  const auto cancellation = admission_.services_.sample_cancellation(
      admission_.services_.context, request_.operation, public_phase);
  const bool cancellation_reserved_valid = AllZero(cancellation.reserved);
  const bool cancellation_shape_valid = cancellation_reserved_valid &&
      (cancellation.status == BlobReceiverSampleStatusV3::ok ||
       cancellation.status == BlobReceiverSampleStatusV3::unavailable) &&
      cancellation.cancelled <= 1;
  if (cancellation_shape_valid &&
      cancellation.status == BlobReceiverSampleStatusV3::ok &&
      cancellation.cancelled == 1) {
    suppress_publication_gate_ = true;
    auto result = RuntimeFailure("PROCESS.CANCELLED", "sampled_true");
    result.fact.stage = BlobLifetimeFactStageV3::ordinary_gate;
    result.fact.gate_present = true;
    result.fact.gate = gate;
    result.fact.parameters.present = blob_parameter_operation |
                                     blob_parameter_phase;
    result.fact.parameters.operation_enum = request_.operation;
    result.fact.parameters.phase_enum = public_phase;
    return result;
  }
  // Both samplers were observed in order. True cancellation has rank 2.
  // Otherwise a clock failure is the earlier same-class authority fact and
  // wins a simultaneous cancellation-service failure.
  if (!clock_shape_valid ||
      clock.status != BlobReceiverSampleStatusV3::ok) {
    suppress_publication_gate_ = true;
    auto result = RuntimeFailure("BLOB.LIFETIME_AUTHORITY_UNAVAILABLE",
                                 "monotonic_clock_unavailable");
    result.fact.stage = BlobLifetimeFactStageV3::ordinary_gate;
    result.fact.gate_present = true;
    result.fact.authority_source =
        BlobLifetimeAuthoritySourceV3::monotonic_clock_unavailable;
    result.fact.gate = gate;
    result.fact.parameters.present = blob_parameter_operation |
                                     blob_parameter_phase;
    result.fact.parameters.operation_enum = request_.operation;
    result.fact.parameters.phase_enum = public_phase;
    return result;
  }
  if (!cancellation_shape_valid ||
      cancellation.status != BlobReceiverSampleStatusV3::ok) {
    suppress_publication_gate_ = true;
    auto result = RuntimeFailure("BLOB.LIFETIME_AUTHORITY_UNAVAILABLE",
                                 "receiver_services_failed");
    result.fact.stage = BlobLifetimeFactStageV3::ordinary_gate;
    result.fact.gate_present = true;
    result.fact.authority_source =
        BlobLifetimeAuthoritySourceV3::cancellation_sampler_unavailable;
    result.fact.gate = gate;
    result.fact.parameters.present = blob_parameter_operation |
                                     blob_parameter_phase;
    result.fact.parameters.operation_enum = request_.operation;
    result.fact.parameters.phase_enum = public_phase;
    return result;
  }
  const bool token_expired = token_.valid_until_monotonic_ns != 0 &&
      clock.now_monotonic_ns >= token_.valid_until_monotonic_ns;
  const bool request_expired = request_.absolute_deadline_monotonic_ns != 0 &&
      clock.now_monotonic_ns >= request_.absolute_deadline_monotonic_ns;
  if (token_expired) {
    suppress_publication_gate_ = true;
    auto result = RuntimeFailure("CINL.LOB.HANDLE_EXPIRED",
                                 "monotonic_expiry");
    result.fact.stage = BlobLifetimeFactStageV3::ordinary_gate;
    result.fact.gate_present = true;
    result.fact.gate = gate;
    result.fact.parameters.present = blob_parameter_lifetime |
                                     blob_parameter_expiry_reason;
    result.fact.parameters.lifetime_enum =
        BlobLifetimeDiagnosticLifetimeV3::token_expired;
    result.fact.parameters.expiry_reason_enum =
        BlobLifetimeDiagnosticExpiryReasonV3::monotonic_expiry;
    return result;
  }
  if (request_expired) {
    suppress_publication_gate_ = true;
    auto result = RuntimeFailure("CINL.LOB.STREAM_BACKPRESSURE_EXHAUSTED",
                                 "absolute_deadline_expired");
    result.fact.stage = BlobLifetimeFactStageV3::ordinary_gate;
    result.fact.gate_present = true;
    result.fact.gate = gate;
    result.fact.parameters.present = blob_parameter_window_octets |
                                     blob_parameter_timeout_milliseconds;
    result.fact.parameters.window_octets_u64 =
        admission_.budget_->admitted_window_octets_;
    const u64 timeout_ns =
        admission_.budget_->admitted_relative_timeout_ns_;
    result.fact.parameters.timeout_milliseconds_u64 = timeout_ns == 0
        ? 0 : (timeout_ns > UINT64_MAX - UINT64_C(999999)
            ? UINT64_MAX : (timeout_ns + UINT64_C(999999)) /
                UINT64_C(1000000));
    return result;
  }
  return RuntimeSuccess();
}

BlobLifetimeRuntimeResultV3 BlobRetainedLifetimeLeaseV3::Release() noexcept {
  EntrantGuard entrant(state_);
  if (!entrant.acquired()) {
    const State observed = state_.load(std::memory_order_acquire);
    return observed == State::released || observed == State::terminal ||
            observed == State::moved_from
        ? LocalStateFailure(observed) : RuntimeTerminated();
  }
  return ReleaseInternal(BlobLifetimePhaseV3::release, 18);
}

BlobLifetimeRuntimeResultV3 BlobRetainedLifetimeLeaseV3::CloneInto(
    BlobRetainedLifetimeLeaseV3& destination) noexcept {
  if (&destination == this)
    return CanonicalProtocolFailure("clone_alias");
  BlobRetainedLifetimeLeaseV3* first = this;
  BlobRetainedLifetimeLeaseV3* second = &destination;
  if (std::less<const void*>{}(second, first)) std::swap(first, second);
  EntrantGuard first_entrant(first->state_);
  if (!first_entrant.acquired()) {
    const State observed = first->state_.load(std::memory_order_acquire);
    return observed == State::released || observed == State::terminal ||
            observed == State::moved_from
        ? first->LocalStateFailure(observed) : RuntimeTerminated();
  }
  EntrantGuard second_entrant(second->state_);
  if (!second_entrant.acquired()) {
    const State observed = second->state_.load(std::memory_order_acquire);
    return observed == State::released || observed == State::terminal ||
            observed == State::moved_from
        ? second->LocalStateFailure(observed) : RuntimeTerminated();
  }
  if (MarkActiveBindingReentry(token_) ||
      MarkActiveBindingReentry(destination.token_)) {
    return CanonicalProtocolFailure("callback_reentrant");
  }
  const auto claim = [&](BlobRetainedLifetimeLeaseV3* lease) noexcept {
    return lease == this ? state_.ClaimIdleForTransfer()
        : destination.state_.ResetGeneration(State::disarmed,
                                              State::installing);
  };
  const auto failure = [&](BlobRetainedLifetimeLeaseV3* lease,
                           const auto& outcome) noexcept {
    if (outcome.status == AtomicControl::GenerationResetStatus::wrong_state &&
        outcome.observed_state != State::installing &&
        (outcome.observed_state == State::retaining ||
         outcome.observed_state == State::callback_active ||
         outcome.observed_state == State::access_active ||
         outcome.observed_state == State::moving ||
         outcome.observed_state == State::releasing))
      return lease->RejectOverlappingUse(BlobLifetimePhaseV3::retain, 15);
    return lease->GenerationResetFailure(outcome, lease == &destination);
  };
  const auto reset_destination = [&]() noexcept {
    const auto reset = destination.state_.ResetGeneration(
        State::installing, State::disarmed);
    if (reset.status != AtomicControl::GenerationResetStatus::installed)
      std::terminate();
  };
  const auto first_claim = claim(first);
  if (first_claim.status != AtomicControl::GenerationResetStatus::installed)
    return failure(first, first_claim);
  const auto second_claim = claim(second);
  if (second_claim.status != AtomicControl::GenerationResetStatus::installed) {
    auto result = failure(second, second_claim);
    second_entrant.Release();
    if (first == &destination) {
      reset_destination();
      return result;
    }
    (void)state_.BeginClosing();
    state_.WaitForClosingQuiescence(1);
    for (;;) {
      if (state_.Deferred() != 0)
        return ReleaseOwned(BlobLifetimePhaseV3::release, 15,
                            std::move(result));
      if (state_.TryFinalize(State::idle, 1)) return result;
    }
  }
  suppress_publication_gate_ = false;
  bool destination_retained_by_clone = false;
  const auto finish_source = [&](BlobLifetimeRuntimeResultV3 result) noexcept {
    if (destination.state_.load(std::memory_order_acquire) == State::installing)
      reset_destination();
    (void)state_.BeginClosing();
    state_.WaitForClosingQuiescence(1);
    for (;;) {
      if (state_.Deferred() != 0) {
        if (destination_retained_by_clone) {
          // RetainFrom left the destination closed and provisional. It has
          // never been observable as idle, and Clone still owns its entrant.
          result = destination.ReleaseOwned(
              BlobLifetimePhaseV3::release, 15, std::move(result));
          destination_retained_by_clone = false;
        }
        return ReleaseOwned(BlobLifetimePhaseV3::release, 15,
                            std::move(result));
      }
      // Both bindings are quiescent and all transfer metadata is settled.
      if (destination_retained_by_clone) {
        if (!destination.state_.TryFinalize(State::idle, 1))
          std::terminate();
        destination_retained_by_clone = false;
      }
      if (state_.TryFinalize(State::idle, 1)) return result;
    }
  };
  const auto cloned = admission_.pin_ops_.clone_pin(
      admission_.pin_ops_.stable_host_context, admission_.pin_cookie_);
  ValidatePinReplyOrTerminate(cloned);
  if (cloned.status != BlobReceiverPinAcquireStatusV3::acquired) {
    const auto result = finish_source(PinFailure(cloned.status, request_.operation));
    return result;
  }
  const auto budget_cloned = admission_.budget_pin_ops_.clone_pin(
      admission_.budget_pin_ops_.stable_host_context,
      admission_.budget_pin_cookie_);
  ValidatePinReplyOrTerminate(budget_cloned);
  if (budget_cloned.status != BlobReceiverPinAcquireStatusV3::acquired) {
    admission_.pin_ops_.drop_pin(admission_.pin_ops_.stable_host_context,
                                 cloned.pin_cookie);
    const auto result = finish_source(
        PinFailure(budget_cloned.status, request_.operation));
    return result;
  }

  BlobLifetimeAuthorityAdmissionV3Generation1 admission;
  admission.authority_ = admission_.authority_;
  admission.services_ = admission_.services_;
  admission.pin_ops_ = admission_.pin_ops_;
  admission.pin_cookie_ = cloned.pin_cookie;
  admission.budget_pin_ops_ = admission_.budget_pin_ops_;
  admission.budget_pin_cookie_ = budget_cloned.pin_cookie;
  admission.budget_ = admission_.budget_;
  admission.token_clock_ = admission_.token_clock_;
  admission.request_clock_ = admission_.request_clock_;
  admission.admitted_ = true;
  BlobBoundMaterializedCarrierV3Generation1 carrier;
  carrier.binding_ = carrier_binding_;
  carrier.state_ = carrier_state_;
  carrier.data_ = carrier_data_;
  carrier.logical_length_ = carrier_logical_length_;
  const auto built = BuildCurrentBlobValidatedProfileHandleV3(kBlobV11ReceiptUuid);
  if (!built.ok()) {
    auto profile_failure = ProtocolFailure(
        "authority_profile_binding_mismatch",
        descriptor_uuid_, descriptor_generation_);
    profile_failure.fact.authority_source =
        BlobLifetimeAuthoritySourceV3::datatype_local_state;
    profile_failure.fact.stage =
        BlobLifetimeFactStageV3::local_precondition;
    const auto result = finish_source(profile_failure);
    return result;
  }
  const auto clone_result = destination.RetainFrom(
      built.profile, admission, carrier, token_, request_, 15, true, true, true);
  destination_retained_by_clone = clone_result.ok();
  const auto result = finish_source(clone_result);
  return result;
}

BlobLifetimeRuntimeResultV3 ConsumeBlobMaterializedValueScopedV3Generation1(
    const BlobValidatedProfileHandleV3& profile,
    const BlobBoundMaterializedCarrierV3Generation1& carrier,
    BlobLifetimeAuthorityAdmissionV3Generation1&& trusted_admission,
    const BlobLifetimeTokenV3& token,
  const BlobLifetimeUseRequestV3& request,
  const BlobTrustedInternalVisitorV3& visitor,
  bool null_allowed) noexcept {
  if (carrier.state() == BlobValueStateV3::sql_null && !null_allowed) {
    auto failure = RuntimeFailure("BLOB.STATE_INVALID",
                                  "sql_null_not_admitted");
    failure.fact.parameters.present = blob_parameter_supplied_state |
                                      blob_parameter_operation;
    failure.fact.parameters.supplied_state_u8 =
        static_cast<u8>(BlobValueStateV3::sql_null);
    failure.fact.parameters.operation_enum = request.operation;
    return failure;
  }
  BlobRetainedLifetimeLeaseV3 lease;
  auto retained = RetainBaseBlobLifetimeLeaseV3Generation1(
      profile, std::move(trusted_admission), carrier, token, request, lease);
  if (!retained.ok()) return retained;
  BlobLifetimeRuntimeResultV3 primary = RuntimeSuccess();
  if (carrier.state() == BlobValueStateV3::value &&
      carrier.logical_length() != 0) {
    primary = lease.VisitBoundMaterializedBytes(visitor);
  } else {
    primary = lease.Probe();
  }
  const auto released = lease.Release();
  if (primary.ok()) return released;
  if (!released.ok() &&
      released.fact.fact_class == BlobLifetimeFactClassV3::security)
    return released;
  return primary;
}

}  // namespace scratchbird::core::datatypes
