// Copyright (c) 2026 ScratchBird Software Inc.
//
// This Source Code Form is subject to the terms of the Mozilla Public
// License, v. 2.0. If a copy of the MPL was not distributed with this
// file, You can obtain one at https://mozilla.org/MPL/2.0/.
//
// SPDX-License-Identifier: MPL-2.0

#include "typed_result_producer_cursor.hpp"

#include "core/uuid/uuid.hpp"
#include "wire/typed_result_packet_view.hpp"

#include <algorithm>
#include <chrono>
#include <exception>
#include <limits>
#include <mutex>
#include <type_traits>
#include <utility>

namespace scratchbird::engine::internal_api {

namespace {

constexpr const char* kInvalidArgument = "SB_ENGINE_STATUS_INVALID_ARGUMENT";
constexpr const char* kAccessDenied = "SECURITY.ACCESS_DENIED";
constexpr const char* kMgaStale = "MGA.TRANSACTION.STALE";
constexpr const char* kDescriptorInvalid = "DATATYPE.DESCRIPTOR.INVALID";
constexpr const char* kCursorStale = "CURSOR.STALE";
constexpr const char* kResourceExceeded = "RESOURCE.BUDGET_EXCEEDED";
constexpr const char* kCancelled = "PROCESS.CANCELLED";
constexpr const char* kFetchFailed = "CURSOR.FETCH_FAILED";

bool UuidPresent(const wire::TypedResultUuid& uuid) {
  return std::any_of(uuid.begin(), uuid.end(), [](byte value) {
    return value != 0;
  });
}

bool IsUuidV7(const wire::TypedResultUuid& uuid) {
  return UuidPresent(uuid) && (uuid[6] & 0xf0u) == 0x70u &&
         (uuid[8] & 0xc0u) == 0x80u;
}

bool IsTerminal(TypedResultProducerCursorLifecycleV1 lifecycle) {
  return lifecycle == TypedResultProducerCursorLifecycleV1::eos ||
         lifecycle == TypedResultProducerCursorLifecycleV1::cancelled ||
         lifecycle == TypedResultProducerCursorLifecycleV1::closed ||
         lifecycle == TypedResultProducerCursorLifecycleV1::revoked;
}

bool ValidCloseReason(TypedResultProducerCloseReasonV1 reason) {
  switch (reason) {
    case TypedResultProducerCloseReasonV1::explicit_close:
    case TypedResultProducerCloseReasonV1::cancellation:
    case TypedResultProducerCloseReasonV1::timeout:
    case TypedResultProducerCloseReasonV1::receipt_invalidation:
    case TypedResultProducerCloseReasonV1::transaction_invalidation:
    case TypedResultProducerCloseReasonV1::disconnect:
    case TypedResultProducerCloseReasonV1::shutdown:
    case TypedResultProducerCloseReasonV1::recovery:
      return true;
  }
  return false;
}

TypedResultProducerCursorLifecycleV1 LifecycleForClose(
    TypedResultProducerCloseReasonV1 reason) {
  switch (reason) {
    case TypedResultProducerCloseReasonV1::explicit_close:
      return TypedResultProducerCursorLifecycleV1::closed;
    case TypedResultProducerCloseReasonV1::cancellation:
      return TypedResultProducerCursorLifecycleV1::cancelled;
    case TypedResultProducerCloseReasonV1::timeout:
    case TypedResultProducerCloseReasonV1::receipt_invalidation:
    case TypedResultProducerCloseReasonV1::transaction_invalidation:
    case TypedResultProducerCloseReasonV1::disconnect:
    case TypedResultProducerCloseReasonV1::shutdown:
    case TypedResultProducerCloseReasonV1::recovery:
      return TypedResultProducerCursorLifecycleV1::revoked;
  }
  return TypedResultProducerCursorLifecycleV1::revoked;
}

TypedResultProducerReleaseReasonV1 ReleaseReasonForClose(
    TypedResultProducerCloseReasonV1 reason) {
  switch (reason) {
    case TypedResultProducerCloseReasonV1::explicit_close:
      return TypedResultProducerReleaseReasonV1::explicit_close;
    case TypedResultProducerCloseReasonV1::cancellation:
      return TypedResultProducerReleaseReasonV1::cancellation;
    case TypedResultProducerCloseReasonV1::timeout:
      return TypedResultProducerReleaseReasonV1::timeout;
    case TypedResultProducerCloseReasonV1::receipt_invalidation:
      return TypedResultProducerReleaseReasonV1::receipt_invalidation;
    case TypedResultProducerCloseReasonV1::transaction_invalidation:
      return TypedResultProducerReleaseReasonV1::transaction_invalidation;
    case TypedResultProducerCloseReasonV1::disconnect:
      return TypedResultProducerReleaseReasonV1::disconnect;
    case TypedResultProducerCloseReasonV1::shutdown:
      return TypedResultProducerReleaseReasonV1::shutdown;
    case TypedResultProducerCloseReasonV1::recovery:
      return TypedResultProducerReleaseReasonV1::recovery;
  }
  return TypedResultProducerReleaseReasonV1::recovery;
}

wire::TypedResultUuid MakeBatchUuid() noexcept {
  // Local, process-private batch identity. Its time bits are never cursor
  // order, MGA visibility, finality, or cluster time authority.
  try {
    const auto millis = std::chrono::duration_cast<std::chrono::milliseconds>(
        std::chrono::system_clock::now().time_since_epoch()).count();
    if (millis < 0 || static_cast<u64>(millis) > 0x0000ffffffffffffull) {
      return {};
    }
    const auto generated = core::uuid::GenerateEngineIdentityV7(
        core::platform::UuidKind::object, static_cast<u64>(millis));
    if (generated.ok()) return generated.value.value.bytes;
  } catch (...) {
    // Entropy or allocation failure must not manufacture an identity or let
    // an exception escape with the carrier stuck in its pulling state.
  }
  return {};
}

TypedResultProducerPullResultV1 PullFailure(
    TypedResultProducerCursorStatusV1 status,
    const char* diagnostic_code,
    std::string detail,
    TypedResultProducerPullOutcomeV1 outcome =
        TypedResultProducerPullOutcomeV1::refused) {
  TypedResultProducerPullResultV1 result;
  result.status = status;
  result.diagnostic_code = diagnostic_code;
  result.detail = std::move(detail);
  result.outcome = outcome;
  return result;
}

}  // namespace

TypedResultProducerStageLeaseV1::TypedResultProducerStageLeaseV1(
    std::unique_ptr<TypedResultProducerStageLeaseActionV1> action) noexcept
    : action_(std::move(action)), pending_(action_ != nullptr) {}

TypedResultProducerStageLeaseV1::~TypedResultProducerStageLeaseV1() {
  Abort();
}

TypedResultProducerStageLeaseV1::TypedResultProducerStageLeaseV1(
    TypedResultProducerStageLeaseV1&& other) noexcept
    : action_(std::move(other.action_)), pending_(other.pending_) {
  other.pending_ = false;
}

TypedResultProducerStageLeaseV1&
TypedResultProducerStageLeaseV1::operator=(
    TypedResultProducerStageLeaseV1&& other) noexcept {
  if (this == &other) return *this;
  Abort();
  action_ = std::move(other.action_);
  pending_ = other.pending_;
  other.pending_ = false;
  return *this;
}

TypedResultProducerStageCommitStatusV1
TypedResultProducerStageLeaseV1::Commit() noexcept {
  if (!pending_) {
    return TypedResultProducerStageCommitStatusV1::stale;
  }
  const auto status = action_
                          ? action_->Commit()
                          : TypedResultProducerStageCommitStatusV1::committed;
  if (status == TypedResultProducerStageCommitStatusV1::committed) {
    pending_ = false;
    action_.reset();
  }
  return status;
}

void TypedResultProducerStageLeaseV1::Abort() noexcept {
  if (!pending_) return;
  if (action_) action_->Abort();
  pending_ = false;
  action_.reset();
}

struct TypedResultProducerCursorCarrierV1::State {
  mutable std::mutex mutation_gate;

  wire::TypedResultUuid carrier_uuid{};
  u64 carrier_generation = 0;
  wire::TypedResultUuid cursor_uuid{};
  wire::TypedResultUuid session_uuid{};
  wire::TypedResultUuid statement_receipt_uuid{};
  wire::TypedResultUuid statement_snapshot_uuid{};
  wire::TypedResultUuid cancellation_receipt_uuid{};
  u64 cancellation_generation = 0;
  wire::TypedResultUuid resource_grant_receipt_uuid{};
  u64 resource_grant_generation = 0;
  u64 resource_grant_bytes = 0;
  wire::TypedResultUuid execution_uuid{};
  wire::TypedResultUuid result_set_uuid{};
  wire::TypedResultUuid row_descriptor_uuid{};
  u64 row_descriptor_generation = 0;
  wire::TypedResultUuid snapshot_uuid{};
  wire::TypedResultUuid cursor_stream_descriptor_uuid{};
  u16 cursor_stream_descriptor_version = 0;
  u64 cursor_stream_descriptor_generation = 0;
  u64 max_chunk_rows = 0;
  u64 max_chunk_bytes = 0;
  std::vector<byte> result_descriptor_vector;
  wire::TypedResultRowDescriptor row_descriptor;
  wire::TypedResultDescriptorAuthorityValidator descriptor_authority;

  u64 next_batch_ordinal = 0;
  u64 row_position = 0;
  TypedResultProducerCursorLifecycleV1 lifecycle =
      TypedResultProducerCursorLifecycleV1::open;
  bool retained_authority_released = false;
  bool defer_terminal_release = false;
  TypedResultProducerReleaseReasonV1 terminal_release_reason =
      TypedResultProducerReleaseReasonV1::open_refused;
  wire::TypedResultCursorBatchState batch_state;
  // One provisional binary ID for the current ordinal, retained across an
  // aborted stage so re-encoding the same retry preserves exact packet bytes.
  wire::TypedResultUuid pending_batch_uuid{};

  std::unique_ptr<TypedResultStatementReceiptHandleV1> statement_receipt;
  std::unique_ptr<TypedResultMgaSnapshotPinHandleV1> mga_snapshot_pin;
  std::unique_ptr<TypedResultCancellationReceiptHandleV1>
      cancellation_receipt;
  std::unique_ptr<TypedResultResourceGrantReceiptHandleV1>
      resource_grant_receipt;
  std::unique_ptr<TypedResultProducerSourceV1> producer_state;

  ~State() { ReleaseLocked(terminal_release_reason); }

  void ReleaseLocked(TypedResultProducerReleaseReasonV1 reason) noexcept {
    if (retained_authority_released) return;
    retained_authority_released = true;
    pending_batch_uuid = {};
    if (producer_state) producer_state->Close(reason);
    producer_state.reset();
    // Release all carrier-owned dynamic storage before its granting authority.
    // Returned transport storage is a separate owner, not a retained cursor page.
    descriptor_authority = nullptr;
    std::vector<byte>().swap(result_descriptor_vector);
    static_assert(std::is_nothrow_move_assignable_v<wire::TypedResultRowDescriptor>);
    row_descriptor = {};
    batch_state = {};
    if (resource_grant_receipt) resource_grant_receipt->Release(reason);
    resource_grant_receipt.reset();
    if (cancellation_receipt) cancellation_receipt->Release(reason);
    cancellation_receipt.reset();
    if (mga_snapshot_pin) mga_snapshot_pin->Release(reason);
    mga_snapshot_pin.reset();
    if (statement_receipt) statement_receipt->Release(reason);
    statement_receipt.reset();
  }

  void TerminalLocked(TypedResultProducerCursorLifecycleV1 terminal,
                      TypedResultProducerReleaseReasonV1 reason) noexcept {
    lifecycle = terminal;
    terminal_release_reason = reason;
    if (!defer_terminal_release) ReleaseLocked(reason);
  }
};

TypedResultProducerCursorCarrierV1::TypedResultProducerCursorCarrierV1(
    std::unique_ptr<State> state)
    : state_(std::move(state)) {}

TypedResultProducerCursorCarrierV1::~TypedResultProducerCursorCarrierV1() {
  if (!state_) return;
  std::lock_guard<std::mutex> lock(state_->mutation_gate);
  if (!IsTerminal(state_->lifecycle)) {
    state_->TerminalLocked(TypedResultProducerCursorLifecycleV1::revoked,
                           TypedResultProducerReleaseReasonV1::shutdown);
  } else {
    state_->ReleaseLocked(TypedResultProducerReleaseReasonV1::shutdown);
  }
}

TypedResultProducerCursorSnapshotV1
TypedResultProducerCursorCarrierV1::Snapshot() const {
  std::lock_guard<std::mutex> lock(state_->mutation_gate);
  TypedResultProducerCursorSnapshotV1 result;
  result.carrier_uuid = state_->carrier_uuid;
  result.carrier_generation = state_->carrier_generation;
  result.cursor_uuid = state_->cursor_uuid;
  result.session_uuid = state_->session_uuid;
  result.execution_uuid = state_->execution_uuid;
  result.result_set_uuid = state_->result_set_uuid;
  result.row_descriptor_uuid = state_->row_descriptor_uuid;
  result.row_descriptor_generation =
      state_->row_descriptor_generation;
  result.snapshot_uuid = state_->snapshot_uuid;
  result.cursor_stream_descriptor_uuid =
      state_->cursor_stream_descriptor_uuid;
  result.cursor_stream_descriptor_version =
      state_->cursor_stream_descriptor_version;
  result.cursor_stream_descriptor_generation =
      state_->cursor_stream_descriptor_generation;
  result.next_batch_ordinal = state_->next_batch_ordinal;
  result.row_position = state_->row_position;
  result.lifecycle = state_->lifecycle;
  result.retained_authority_released = state_->retained_authority_released;
  return result;
}

const std::vector<byte>&
TypedResultProducerCursorCarrierV1::result_descriptor_vector() const {
  return state_->result_descriptor_vector;
}

const wire::TypedResultRowDescriptor&
TypedResultProducerCursorCarrierV1::row_descriptor() const {
  return state_->row_descriptor;
}

TypedResultProducerOpenResultV1 OpenTypedResultProducerCursorV1(
    TypedResultProducerOpenRequestV1 request) try {
  // Own the incoming handles even if allocating State fails. Once transferred,
  // State's destructor performs the same terminal transition on failed Open.
  struct IncomingOwner {
    TypedResultProducerOpenRequestV1& request;
    ~IncomingOwner() {
      const auto reason = TypedResultProducerReleaseReasonV1::open_refused;
      if (request.producer_state) request.producer_state->Close(reason);
      request.producer_state.reset();
      request.descriptor_authority = nullptr;
      request.row_descriptor = {};
      if (request.resource_grant_receipt) request.resource_grant_receipt->Release(reason);
      request.resource_grant_receipt.reset();
      if (request.cancellation_receipt) request.cancellation_receipt->Release(reason);
      request.cancellation_receipt.reset();
      if (request.mga_snapshot_pin) request.mga_snapshot_pin->Release(reason);
      request.mga_snapshot_pin.reset();
      if (request.statement_receipt) request.statement_receipt->Release(reason);
      request.statement_receipt.reset();
    }
  } incoming{request};
  auto state = std::make_unique<TypedResultProducerCursorCarrierV1::State>();
  state->carrier_uuid = request.carrier_uuid;
  state->carrier_generation = request.carrier_generation;
  state->cursor_uuid = request.cursor_uuid;
  state->session_uuid = request.session_uuid;
  state->statement_receipt_uuid = request.statement_receipt_uuid;
  state->statement_snapshot_uuid = request.statement_snapshot_uuid;
  state->cancellation_receipt_uuid = request.cancellation_receipt_uuid;
  state->cancellation_generation = request.cancellation_generation;
  state->resource_grant_receipt_uuid = request.resource_grant_receipt_uuid;
  state->resource_grant_generation = request.resource_grant_generation;
  state->resource_grant_bytes = request.resource_grant_bytes;
  state->execution_uuid = request.execution_uuid;
  state->result_set_uuid = request.result_set_uuid;
  state->row_descriptor_uuid = request.row_descriptor_uuid;
  state->row_descriptor_generation = request.row_descriptor_generation;
  state->snapshot_uuid = request.snapshot_uuid;
  state->cursor_stream_descriptor_uuid =
      request.cursor_stream_descriptor_uuid;
  state->cursor_stream_descriptor_version =
      request.cursor_stream_descriptor_version;
  state->cursor_stream_descriptor_generation =
      request.cursor_stream_descriptor_generation;
  state->max_chunk_rows = request.max_chunk_rows;
  state->max_chunk_bytes = request.max_chunk_bytes;
  state->row_descriptor = std::move(request.row_descriptor);
  state->descriptor_authority = std::move(request.descriptor_authority);
  state->statement_receipt = std::move(request.statement_receipt);
  state->mga_snapshot_pin = std::move(request.mga_snapshot_pin);
  state->cancellation_receipt = std::move(request.cancellation_receipt);
  state->resource_grant_receipt =
      std::move(request.resource_grant_receipt);
  state->producer_state = std::move(request.producer_state);

  auto refuse = [&](TypedResultProducerCursorStatusV1 status,
                    const char* diagnostic_code,
                    std::string detail,
                    TypedResultProducerReleaseReasonV1 release_reason =
                        TypedResultProducerReleaseReasonV1::open_refused) {
    state->lifecycle =
        status == TypedResultProducerCursorStatusV1::cancelled
            ? TypedResultProducerCursorLifecycleV1::cancelled
            : TypedResultProducerCursorLifecycleV1::revoked;
    state->terminal_release_reason = release_reason;
    TypedResultProducerOpenResultV1 result;
    result.status = status;
    result.diagnostic_code = diagnostic_code;
    result.detail = std::move(detail);
    return result;
  };

  if (request.version != kTypedResultProducerCursorVersionV1 ||
      !IsUuidV7(state->carrier_uuid) || state->carrier_generation == 0 ||
      !UuidPresent(state->cursor_uuid) || !UuidPresent(state->session_uuid) ||
      !UuidPresent(state->statement_receipt_uuid) ||
      !UuidPresent(state->statement_snapshot_uuid) ||
      !UuidPresent(state->cancellation_receipt_uuid) ||
      state->cancellation_generation == 0 ||
      !UuidPresent(state->resource_grant_receipt_uuid) ||
      state->resource_grant_generation == 0 ||
      state->resource_grant_bytes == 0 ||
      !UuidPresent(state->execution_uuid) ||
      !UuidPresent(state->result_set_uuid) ||
      !UuidPresent(state->row_descriptor_uuid) ||
      state->row_descriptor_generation == 0 ||
      !UuidPresent(state->snapshot_uuid) ||
      !UuidPresent(state->cursor_stream_descriptor_uuid) ||
      state->cursor_stream_descriptor_version != 1 ||
      state->cursor_stream_descriptor_generation == 0 ||
      state->max_chunk_rows == 0 || state->max_chunk_bytes == 0 ||
      !state->descriptor_authority || !state->statement_receipt ||
      !state->mga_snapshot_pin || !state->cancellation_receipt ||
      !state->resource_grant_receipt || !state->producer_state) {
    return refuse(TypedResultProducerCursorStatusV1::invalid_argument,
                  kInvalidArgument, "malformed_open_authority_matrix");
  }
  TypedResultProducerOwnerObservationV1 owner;
  try {
    owner = state->statement_receipt->ObserveOwner(state->session_uuid);
  } catch (const std::bad_alloc&) {
    throw;
  } catch (...) {
    owner = TypedResultProducerOwnerObservationV1::denied;
  }
  if (owner != TypedResultProducerOwnerObservationV1::authorized) {
    return refuse(TypedResultProducerCursorStatusV1::access_denied,
                  kAccessDenied, "statement_owner_not_authorized");
  }

  if (state->statement_snapshot_uuid != state->snapshot_uuid) {
    return refuse(TypedResultProducerCursorStatusV1::mga_stale, kMgaStale,
                  "statement_and_result_snapshot_mismatch");
  }

  TypedResultProducerMgaObservationV1 snapshot;
  try {
    snapshot = state->mga_snapshot_pin->ObserveSnapshot(
        state->statement_snapshot_uuid, state->snapshot_uuid);
  } catch (const std::bad_alloc&) {
    throw;
  } catch (...) {
    snapshot = TypedResultProducerMgaObservationV1::stale_or_unequal;
  }
  if (snapshot != TypedResultProducerMgaObservationV1::live_and_equal) {
    return refuse(TypedResultProducerCursorStatusV1::mga_stale, kMgaStale,
                  "statement_snapshot_not_live_and_equal");
  }

  auto encoded_descriptor =
      wire::EncodeTypedResultRowDescriptor(state->row_descriptor);
  if (!encoded_descriptor.ok()) {
    return refuse(TypedResultProducerCursorStatusV1::descriptor_invalid,
                  kDescriptorInvalid,
                  "descriptor_encode:" + encoded_descriptor.detail);
  }
  auto decoded_descriptor =
      wire::DecodeTypedResultRowDescriptor(encoded_descriptor.encoded);
  if (!decoded_descriptor.ok() ||
      decoded_descriptor.encoded != encoded_descriptor.encoded) {
    return refuse(TypedResultProducerCursorStatusV1::descriptor_invalid,
                  kDescriptorInvalid, "descriptor_canonical_roundtrip_failed");
  }
  if (decoded_descriptor.descriptor.descriptor_uuid !=
          state->row_descriptor_uuid ||
      decoded_descriptor.descriptor.descriptor_generation !=
          state->row_descriptor_generation) {
    return refuse(TypedResultProducerCursorStatusV1::descriptor_invalid,
                  kDescriptorInvalid,
                  "query_handle_row_descriptor_identity_mismatch");
  }
  wire::TypedResultDescriptorAuthorityDecision descriptor_decision;
  try {
    descriptor_decision =
        state->descriptor_authority(decoded_descriptor.descriptor);
  } catch (const std::bad_alloc&) {
    throw;
  } catch (...) {
    descriptor_decision.accepted = false;
    descriptor_decision.detail = "descriptor_authority_exception";
  }
  if (!descriptor_decision.accepted) {
    return refuse(
        TypedResultProducerCursorStatusV1::descriptor_invalid,
        kDescriptorInvalid,
        descriptor_decision.detail.empty()
            ? "descriptor_authority_refused"
            : std::move(descriptor_decision.detail));
  }
  state->result_descriptor_vector = std::move(decoded_descriptor.encoded);
  state->row_descriptor = std::move(decoded_descriptor.descriptor);

  TypedResultProducerReceiptObservationV1 receipt;
  try {
    receipt = state->statement_receipt->ObserveReceipt(
        state->statement_receipt_uuid);
  } catch (const std::bad_alloc&) {
    throw;
  } catch (...) {
    receipt = TypedResultProducerReceiptObservationV1::stale;
  }
  if (receipt != TypedResultProducerReceiptObservationV1::live) {
    return refuse(TypedResultProducerCursorStatusV1::cursor_stale,
                  kCursorStale, "statement_receipt_not_live");
  }

  if (state->max_chunk_rows > kTypedResultProducerMaximumRowsV1 ||
      state->max_chunk_bytes > kTypedResultProducerMaximumBytesV1 ||
      state->max_chunk_bytes > state->resource_grant_bytes) {
    return refuse(TypedResultProducerCursorStatusV1::resource_budget_exceeded,
                  kResourceExceeded, "open_bounds_exceed_retained_authority");
  }
  TypedResultProducerGrantObservationV1 grant;
  try {
    grant = state->resource_grant_receipt->ObserveGrant(
        state->resource_grant_receipt_uuid,
        state->resource_grant_generation, state->resource_grant_bytes,
        state->max_chunk_bytes);
  } catch (const std::bad_alloc&) {
    throw;
  } catch (...) {
    grant = TypedResultProducerGrantObservationV1::stale_or_released;
  }
  if (grant != TypedResultProducerGrantObservationV1::live) {
    return refuse(TypedResultProducerCursorStatusV1::resource_budget_exceeded,
                  kResourceExceeded, "resource_grant_not_live_or_sufficient");
  }

  TypedResultProducerCancellationObservationV1 cancellation;
  try {
    cancellation = state->cancellation_receipt->ObserveCancellation(
        state->cancellation_receipt_uuid, state->cancellation_generation);
  } catch (const std::bad_alloc&) {
    throw;
  } catch (...) {
    cancellation = TypedResultProducerCancellationObservationV1::stale;
  }
  if (cancellation ==
      TypedResultProducerCancellationObservationV1::requested) {
    return refuse(TypedResultProducerCursorStatusV1::cancelled, kCancelled,
                  "cancellation_observed_before_open_publication",
                  TypedResultProducerReleaseReasonV1::cancellation);
  }
  if (cancellation != TypedResultProducerCancellationObservationV1::live) {
    return refuse(TypedResultProducerCursorStatusV1::cursor_stale,
                  kCursorStale, "cancellation_receipt_not_live");
  }

  TypedResultProducerOpenResultV1 result;
  result.carrier = std::unique_ptr<TypedResultProducerCursorCarrierV1>(
      new TypedResultProducerCursorCarrierV1(std::move(state)));
  return result;
} catch (const std::bad_alloc&) {
  TypedResultProducerOpenResultV1 result;
  result.status = TypedResultProducerCursorStatusV1::resource_budget_exceeded;
  // Emergency status is allocation-free; rich diagnostic text may be absent.
  return result;
}

TypedResultProducerPullResultV1 PullTypedResultProducerCursorV1(
    TypedResultProducerCursorCarrierV1& carrier,
    const TypedResultProducerPullRequestV1& request) {
  return PullTypedResultProducerCursorV1(
      carrier, request, TypedResultProducerPrecommitGateV1{});
}

TypedResultProducerPullResultV1 PullTypedResultProducerCursorV1(
    TypedResultProducerCursorCarrierV1& carrier,
    const TypedResultProducerPullRequestV1& request,
    const TypedResultProducerPrecommitGateV1& precommit_gate) try {
  auto& state = *carrier.state_;
  std::lock_guard<std::mutex> lock(state.mutation_gate);
  // Declared before all descriptor/stage/encoding temporaries: they are destroyed
  // before any terminal authority release, and a throwing prepublication path
  // cannot strand the carrier in pulling. The mutation gate remains held.
  struct PullOwner {
    TypedResultProducerCursorCarrierV1::State& state;
    ~PullOwner() {
      if (state.lifecycle == TypedResultProducerCursorLifecycleV1::pulling) {
        state.lifecycle = TypedResultProducerCursorLifecycleV1::open;
      }
      state.defer_terminal_release = false;
      if (IsTerminal(state.lifecycle)) state.ReleaseLocked(state.terminal_release_reason);
    }
  } pull_owner{state};
  state.defer_terminal_release = true;

  if (request.version != kTypedResultProducerCursorVersionV1 ||
      !UuidPresent(request.cursor_uuid) || request.carrier_generation == 0 ||
      !UuidPresent(request.cursor_stream_descriptor_uuid) ||
      request.cursor_stream_descriptor_version != 1 ||
      request.cursor_stream_descriptor_generation == 0 ||
      request.maximum_rows == 0 || request.maximum_bytes == 0 ||
      request.timeout_millis == 0) {
    return PullFailure(TypedResultProducerCursorStatusV1::invalid_argument,
                       kInvalidArgument, "malformed_pull_request");
  }
  if (IsTerminal(state.lifecycle) ||
      state.lifecycle != TypedResultProducerCursorLifecycleV1::open) {
    return PullFailure(TypedResultProducerCursorStatusV1::cursor_stale,
                       kCursorStale, "cursor_not_open");
  }

  TypedResultProducerOwnerObservationV1 owner;
  try {
    owner = state.statement_receipt->ObserveOwner(state.session_uuid);
  } catch (const std::bad_alloc&) {
    throw;
  } catch (...) {
    owner = TypedResultProducerOwnerObservationV1::denied;
  }
  if (owner != TypedResultProducerOwnerObservationV1::authorized) {
    state.TerminalLocked(TypedResultProducerCursorLifecycleV1::revoked,
                         TypedResultProducerReleaseReasonV1::receipt_invalidation);
    return PullFailure(TypedResultProducerCursorStatusV1::access_denied,
                       kAccessDenied, "statement_owner_not_authorized");
  }

  TypedResultProducerMgaObservationV1 snapshot;
  try {
    snapshot = state.mga_snapshot_pin->ObserveSnapshot(
        state.statement_snapshot_uuid, state.snapshot_uuid);
  } catch (const std::bad_alloc&) {
    throw;
  } catch (...) {
    snapshot = TypedResultProducerMgaObservationV1::stale_or_unequal;
  }
  if (snapshot != TypedResultProducerMgaObservationV1::live_and_equal) {
    state.TerminalLocked(
        TypedResultProducerCursorLifecycleV1::revoked,
        TypedResultProducerReleaseReasonV1::transaction_invalidation);
    return PullFailure(TypedResultProducerCursorStatusV1::mga_stale,
                       kMgaStale, "statement_snapshot_not_live_and_equal");
  }

  auto current_descriptor =
      wire::EncodeTypedResultRowDescriptor(state.row_descriptor);
  wire::TypedResultDescriptorAuthorityDecision descriptor_decision;
  if (current_descriptor.ok() &&
      current_descriptor.encoded == state.result_descriptor_vector) {
    try {
      descriptor_decision =
          state.descriptor_authority(current_descriptor.descriptor);
    } catch (const std::bad_alloc&) {
    throw;
  } catch (...) {
      descriptor_decision.accepted = false;
      descriptor_decision.detail = "descriptor_authority_exception";
    }
  }
  if (!current_descriptor.ok() ||
      current_descriptor.encoded != state.result_descriptor_vector ||
      current_descriptor.descriptor.descriptor_uuid !=
          state.row_descriptor_uuid ||
      current_descriptor.descriptor.descriptor_generation !=
          state.row_descriptor_generation ||
      !descriptor_decision.accepted) {
    state.TerminalLocked(TypedResultProducerCursorLifecycleV1::revoked,
                         TypedResultProducerReleaseReasonV1::receipt_invalidation);
    return PullFailure(
        TypedResultProducerCursorStatusV1::descriptor_invalid,
        kDescriptorInvalid,
        descriptor_decision.detail.empty()
            ? "descriptor_identity_generation_or_evidence_drift"
            : std::move(descriptor_decision.detail));
  }

  TypedResultProducerReceiptObservationV1 receipt;
  try {
    receipt = state.statement_receipt->ObserveReceipt(
        state.statement_receipt_uuid);
  } catch (const std::bad_alloc&) {
    throw;
  } catch (...) {
    receipt = TypedResultProducerReceiptObservationV1::stale;
  }
  if (receipt != TypedResultProducerReceiptObservationV1::live) {
    state.TerminalLocked(TypedResultProducerCursorLifecycleV1::revoked,
                         TypedResultProducerReleaseReasonV1::receipt_invalidation);
    return PullFailure(TypedResultProducerCursorStatusV1::cursor_stale,
                       kCursorStale, "statement_receipt_not_live");
  }
  if (request.cursor_uuid != state.cursor_uuid ||
      request.carrier_generation != state.carrier_generation ||
      request.cursor_stream_descriptor_uuid !=
          state.cursor_stream_descriptor_uuid ||
      request.cursor_stream_descriptor_version !=
          state.cursor_stream_descriptor_version ||
      request.cursor_stream_descriptor_generation !=
          state.cursor_stream_descriptor_generation ||
      request.expected_batch_ordinal != state.next_batch_ordinal ||
      state.next_batch_ordinal == std::numeric_limits<u64>::max()) {
    return PullFailure(TypedResultProducerCursorStatusV1::cursor_stale,
                       kCursorStale,
                       "cursor_stream_generation_or_sequence_mismatch");
  }

  if (request.maximum_rows > state.max_chunk_rows ||
      request.maximum_rows > kTypedResultProducerMaximumRowsV1 ||
      request.maximum_bytes > state.max_chunk_bytes ||
      request.maximum_bytes > state.resource_grant_bytes ||
      request.maximum_bytes > kTypedResultProducerMaximumBytesV1 ||
      request.maximum_rows >
          std::numeric_limits<u64>::max() - state.row_position) {
    return PullFailure(
        TypedResultProducerCursorStatusV1::resource_budget_exceeded,
        kResourceExceeded, "pull_bounds_exceed_retained_authority");
  }
  TypedResultProducerGrantObservationV1 grant;
  try {
    grant = state.resource_grant_receipt->ObserveGrant(
        state.resource_grant_receipt_uuid,
        state.resource_grant_generation, state.resource_grant_bytes,
        request.maximum_bytes);
  } catch (const std::bad_alloc&) {
    throw;
  } catch (...) {
    grant = TypedResultProducerGrantObservationV1::stale_or_released;
  }
  if (grant != TypedResultProducerGrantObservationV1::live) {
    if (grant == TypedResultProducerGrantObservationV1::stale_or_released) {
      state.TerminalLocked(
          TypedResultProducerCursorLifecycleV1::revoked,
          TypedResultProducerReleaseReasonV1::receipt_invalidation);
    }
    return PullFailure(
        TypedResultProducerCursorStatusV1::resource_budget_exceeded,
        kResourceExceeded, "resource_grant_not_live_or_sufficient");
  }

  auto observe_cancellation = [&]() {
    try {
      return state.cancellation_receipt->ObserveCancellation(
          state.cancellation_receipt_uuid, state.cancellation_generation);
    } catch (const std::bad_alloc&) {
    throw;
  } catch (...) {
      return TypedResultProducerCancellationObservationV1::stale;
    }
  };
  auto cancellation = observe_cancellation();
  if (cancellation ==
      TypedResultProducerCancellationObservationV1::requested) {
    state.TerminalLocked(TypedResultProducerCursorLifecycleV1::cancelled,
                         TypedResultProducerReleaseReasonV1::cancellation);
    return PullFailure(TypedResultProducerCursorStatusV1::cancelled,
                       kCancelled, "cancellation_before_producer_entry",
                       TypedResultProducerPullOutcomeV1::cancelled);
  }
  if (cancellation != TypedResultProducerCancellationObservationV1::live) {
    state.TerminalLocked(TypedResultProducerCursorLifecycleV1::revoked,
                         TypedResultProducerReleaseReasonV1::receipt_invalidation);
    return PullFailure(TypedResultProducerCursorStatusV1::cursor_stale,
                       kCursorStale, "cancellation_receipt_not_live");
  }

  const u64 old_position = state.row_position;
  const u64 old_ordinal = state.next_batch_ordinal;
  state.lifecycle = TypedResultProducerCursorLifecycleV1::pulling;

  TypedResultProducerStageResultV1 staged;
  auto safe_point_cancellation =
      TypedResultProducerCancellationObservationV1::live;
  bool producer_failed = false;
  std::string producer_failure_detail;
  try {
    TypedResultProducerStageRequestV1 stage_request;
    stage_request.next_batch_ordinal = old_ordinal;
    stage_request.row_position = old_position;
    stage_request.maximum_rows = request.maximum_rows;
    stage_request.maximum_bytes = request.maximum_bytes;
    stage_request.timeout_millis = request.timeout_millis;
    stage_request.cancellation_requested = [&]() {
      const auto observed = observe_cancellation();
      if (observed != TypedResultProducerCancellationObservationV1::live) {
        safe_point_cancellation = observed;
      }
      return observed != TypedResultProducerCancellationObservationV1::live;
    };
    staged = state.producer_state->Stage(stage_request);
  } catch (const std::bad_alloc&) {
    throw;
  } catch (const std::exception& error) {
    producer_failed = true;
    producer_failure_detail =
        std::string("producer_exception:") + error.what();
  } catch (...) {
    producer_failed = true;
    producer_failure_detail = "producer_unknown_exception";
  }

  const bool empty_open =
      staged.outcome == TypedResultProducerStageOutcomeV1::empty_open;
  const bool empty_eos =
      staged.outcome == TypedResultProducerStageOutcomeV1::empty_eos;
  if ((empty_open || empty_eos) &&
      (!staged.rows.empty() || staged.end_of_cursor != empty_eos)) {
    state.lifecycle = TypedResultProducerCursorLifecycleV1::open;
    return PullFailure(TypedResultProducerCursorStatusV1::invalid_argument,
                       kInvalidArgument, "malformed_empty_stage_result");
  }
  const bool batch =
      staged.outcome == TypedResultProducerStageOutcomeV1::batch;
  const bool producer_cancelled =
      staged.outcome == TypedResultProducerStageOutcomeV1::cancelled;
  const bool producer_refused =
      staged.outcome == TypedResultProducerStageOutcomeV1::refused;
  if (!empty_open && !empty_eos && !batch && !producer_cancelled &&
      !producer_refused) {
    state.lifecycle = TypedResultProducerCursorLifecycleV1::open;
    return PullFailure(TypedResultProducerCursorStatusV1::invalid_argument,
                       kInvalidArgument, "unknown_stage_outcome");
  }
  if ((producer_cancelled || producer_refused || producer_failed) &&
      (!staged.rows.empty() || staged.end_of_cursor)) {
    state.lifecycle = TypedResultProducerCursorLifecycleV1::open;
    return PullFailure(TypedResultProducerCursorStatusV1::invalid_argument,
                       kInvalidArgument,
                       "malformed_cancelled_or_refused_stage_result");
  }
  const bool lease_required = batch || empty_eos;
  if (staged.lease.pending() != lease_required) {
    staged.lease.Abort();
    state.lifecycle = TypedResultProducerCursorLifecycleV1::open;
    return PullFailure(TypedResultProducerCursorStatusV1::invalid_argument,
                       kInvalidArgument,
                       lease_required ? "required_stage_lease_absent"
                                      : "unexpected_stage_lease_present");
  }

  // Higher-precedence authority invalidations win over a malformed batch,
  // cancellation, or producer failure observed after Stage returned.
  try {
    owner = state.statement_receipt->ObserveOwner(state.session_uuid);
  } catch (const std::bad_alloc&) {
    throw;
  } catch (...) {
    owner = TypedResultProducerOwnerObservationV1::denied;
  }
  if (owner != TypedResultProducerOwnerObservationV1::authorized) {
    staged.lease.Abort();
    state.TerminalLocked(TypedResultProducerCursorLifecycleV1::revoked,
                         TypedResultProducerReleaseReasonV1::receipt_invalidation);
    return PullFailure(TypedResultProducerCursorStatusV1::access_denied,
                       kAccessDenied,
                       "owner_invalidated_after_producer_stage");
  }
  try {
    snapshot = state.mga_snapshot_pin->ObserveSnapshot(
        state.statement_snapshot_uuid, state.snapshot_uuid);
  } catch (const std::bad_alloc&) {
    throw;
  } catch (...) {
    snapshot = TypedResultProducerMgaObservationV1::stale_or_unequal;
  }
  if (snapshot != TypedResultProducerMgaObservationV1::live_and_equal) {
    staged.lease.Abort();
    state.TerminalLocked(
        TypedResultProducerCursorLifecycleV1::revoked,
        TypedResultProducerReleaseReasonV1::transaction_invalidation);
    return PullFailure(TypedResultProducerCursorStatusV1::mga_stale,
                       kMgaStale, "snapshot_invalidated_after_producer_stage");
  }

  if (batch &&
      (staged.rows.empty() || staged.rows.size() > request.maximum_rows)) {
    state.lifecycle = TypedResultProducerCursorLifecycleV1::open;
    return PullFailure(TypedResultProducerCursorStatusV1::descriptor_invalid,
                       kDescriptorInvalid,
                       "staged_batch_row_count_or_shape_invalid");
  }

  TypedResultProducerPullResultV1 published;
  bool encoded_batch_over_bound = false;
  wire::TypedResultCursorBatchState staged_batch_state = state.batch_state;
  if (batch) {
    wire::TypedResultBatch batch;
    batch.execution_uuid = state.execution_uuid;
    batch.result_set_uuid = state.result_set_uuid;
    if (!UuidPresent(state.pending_batch_uuid)) {
      state.pending_batch_uuid = MakeBatchUuid();
    }
    batch.batch_uuid = state.pending_batch_uuid;
    batch.batch_ordinal = old_ordinal;
    batch.end_of_rowset = staged.end_of_cursor;
    batch.cursor_bound = true;
    batch.row_descriptor_uuid = state.row_descriptor.descriptor_uuid;
    batch.row_descriptor_generation =
        state.row_descriptor.descriptor_generation;
    batch.descriptor_evidence_sha256 =
        state.row_descriptor.descriptor_evidence_sha256;
    batch.snapshot_uuid = state.snapshot_uuid;
    batch.cursor_uuid = state.cursor_uuid;
    batch.rows = std::move(staged.rows);
    if (!UuidPresent(batch.batch_uuid)) {
      state.lifecycle = TypedResultProducerCursorLifecycleV1::open;
      return PullFailure(TypedResultProducerCursorStatusV1::fetch_failed,
                         kFetchFailed, "batch_uuid_generation_failed");
    }

    wire::TypedResultCarrierBinding binding;
    binding.kind = wire::TypedResultCarrierKind::ps_fetch_result_v1;
    binding.row_count = batch.rows.size();
    binding.end_of_rowset = batch.end_of_rowset;
    binding.execution_uuid = state.execution_uuid;
    binding.result_set_uuid = state.result_set_uuid;
    binding.snapshot_uuid = state.snapshot_uuid;
    binding.cursor_uuid = state.cursor_uuid;
    binding.cursor_stream_descriptor_uuid =
        state.cursor_stream_descriptor_uuid;
    binding.cursor_stream_descriptor_version =
        state.cursor_stream_descriptor_version;
    binding.cursor_stream_descriptor_generation =
        state.cursor_stream_descriptor_generation;

    auto encoded = wire::EncodeTypedResultBatch(
        batch, state.row_descriptor, binding,
        std::min({request.maximum_bytes, state.max_chunk_bytes,
                  state.resource_grant_bytes, kTypedResultProducerMaximumBytesV1}));
    if (encoded.status == wire::TypedResultCodecStatus::resource_limit_exceeded) {
      // Preserve the retained-authority refusal order and source lease below;
      // the oversized candidate was refused before packet materialization.
      encoded_batch_over_bound = true;
    } else if (!encoded.ok()) {
      state.lifecycle = TypedResultProducerCursorLifecycleV1::open;
      return PullFailure(
          TypedResultProducerCursorStatusV1::descriptor_invalid,
          kDescriptorInvalid, "staged_batch_encode:" + encoded.detail);
    }
    if (encoded_batch_over_bound || encoded.encoded.empty() ||
        encoded.encoded.size() > request.maximum_bytes ||
        encoded.encoded.size() > state.max_chunk_bytes ||
        encoded.encoded.size() > state.resource_grant_bytes ||
        encoded.encoded.size() > kTypedResultProducerMaximumBytesV1) {
      encoded_batch_over_bound = true;
    } else {
      auto decoded = wire::DecodeTypedResultPacketView(
          encoded.encoded.data(), encoded.encoded.size(),
          state.row_descriptor, binding, &staged_batch_state);
      if (!decoded.ok() || !decoded.has_cursor_state) {
        state.lifecycle = TypedResultProducerCursorLifecycleV1::open;
        return PullFailure(
            TypedResultProducerCursorStatusV1::descriptor_invalid,
            kDescriptorInvalid, "staged_batch_canonical_verification_failed");
      }
      published.status = TypedResultProducerCursorStatusV1::ok;
      published.outcome = TypedResultProducerPullOutcomeV1::batch;
      published.row_count = decoded.view.header().row_count;
      published.end_of_cursor = decoded.view.header().end_of_rowset;
      // The encoder's owning copy is distinct from the staged source storage,
      // which must die before the retained source grant is released at EOS.
      // Moving source-owned rows requires a separately retained physical lease.
      // The complete immutable packet has been independently checked. Keep
      // its provisional state private until the existing source/authority
      // barrier below; no second owning decode or payload clone is needed.
      staged_batch_state = std::move(decoded.next_cursor_state);
      published.row_data_packet = std::move(encoded.encoded);
      published.batch = std::move(encoded.batch);
    }
  }

  if (empty_open) {
    published.status = TypedResultProducerCursorStatusV1::ok;
    published.outcome = TypedResultProducerPullOutcomeV1::empty_open;
  } else if (empty_eos) {
    published.status = TypedResultProducerCursorStatusV1::ok;
    published.outcome = TypedResultProducerPullOutcomeV1::empty_eos;
    published.end_of_cursor = true;
  }

  // The outer encoder receives a complete, validated candidate while both the
  // producer lease and carrier cursor remain provisional.  An empty gate is
  // the always-accept path for existing callers.
  if (!producer_cancelled && !producer_refused && !producer_failed &&
      !encoded_batch_over_bound && precommit_gate) {
    TypedResultProducerPrecommitDecisionV1 decision;
    try {
      decision = precommit_gate(published);
    } catch (const std::bad_alloc&) {
    throw;
  } catch (...) {
      decision.accepted = false;
      decision.refusal_status =
          TypedResultProducerCursorStatusV1::fetch_failed;
      decision.diagnostic_code = kFetchFailed;
      decision.detail = "precommit_gate_exception";
    }
    if (!decision.accepted) {
      staged.lease.Abort();
      state.lifecycle = TypedResultProducerCursorLifecycleV1::open;
      if (decision.refusal_status == TypedResultProducerCursorStatusV1::ok) {
        decision.refusal_status =
            TypedResultProducerCursorStatusV1::fetch_failed;
      }
      if (decision.diagnostic_code.empty()) {
        decision.diagnostic_code = kFetchFailed;
      }
      if (decision.detail.empty()) {
        decision.detail = "precommit_gate_refused";
      }
      return PullFailure(decision.refusal_status,
                         decision.diagnostic_code.c_str(),
                         std::move(decision.detail));
    }
  }

  // Immediately-before-publication authority pass, in the Core refusal order.
  try {
    owner = state.statement_receipt->ObserveOwner(state.session_uuid);
  } catch (const std::bad_alloc&) {
    throw;
  } catch (...) {
    owner = TypedResultProducerOwnerObservationV1::denied;
  }
  if (owner != TypedResultProducerOwnerObservationV1::authorized) {
    staged.lease.Abort();
    state.TerminalLocked(TypedResultProducerCursorLifecycleV1::revoked,
                         TypedResultProducerReleaseReasonV1::receipt_invalidation);
    return PullFailure(TypedResultProducerCursorStatusV1::access_denied,
                       kAccessDenied,
                       "owner_invalidated_before_publication_barrier");
  }
  try {
    snapshot = state.mga_snapshot_pin->ObserveSnapshot(
        state.statement_snapshot_uuid, state.snapshot_uuid);
  } catch (const std::bad_alloc&) {
    throw;
  } catch (...) {
    snapshot = TypedResultProducerMgaObservationV1::stale_or_unequal;
  }
  if (snapshot != TypedResultProducerMgaObservationV1::live_and_equal) {
    staged.lease.Abort();
    state.TerminalLocked(
        TypedResultProducerCursorLifecycleV1::revoked,
        TypedResultProducerReleaseReasonV1::transaction_invalidation);
    return PullFailure(TypedResultProducerCursorStatusV1::mga_stale,
                       kMgaStale,
                       "snapshot_invalidated_before_publication_barrier");
  }
  current_descriptor = wire::EncodeTypedResultRowDescriptor(state.row_descriptor);
  descriptor_decision = {};
  if (current_descriptor.ok() &&
      current_descriptor.encoded == state.result_descriptor_vector) {
    try {
      descriptor_decision =
          state.descriptor_authority(current_descriptor.descriptor);
    } catch (const std::bad_alloc&) {
    throw;
  } catch (...) {
      descriptor_decision.accepted = false;
    }
  }
  if (!current_descriptor.ok() ||
      current_descriptor.encoded != state.result_descriptor_vector ||
      current_descriptor.descriptor.descriptor_uuid !=
          state.row_descriptor_uuid ||
      current_descriptor.descriptor.descriptor_generation !=
          state.row_descriptor_generation ||
      !descriptor_decision.accepted) {
    staged.lease.Abort();
    state.TerminalLocked(TypedResultProducerCursorLifecycleV1::revoked,
                         TypedResultProducerReleaseReasonV1::receipt_invalidation);
    return PullFailure(TypedResultProducerCursorStatusV1::descriptor_invalid,
                       kDescriptorInvalid,
                       "descriptor_invalidated_before_publication_barrier");
  }
  try {
    receipt = state.statement_receipt->ObserveReceipt(
        state.statement_receipt_uuid);
  } catch (const std::bad_alloc&) {
    throw;
  } catch (...) {
    receipt = TypedResultProducerReceiptObservationV1::stale;
  }
  if (receipt != TypedResultProducerReceiptObservationV1::live) {
    staged.lease.Abort();
    state.TerminalLocked(TypedResultProducerCursorLifecycleV1::revoked,
                         TypedResultProducerReleaseReasonV1::receipt_invalidation);
    return PullFailure(TypedResultProducerCursorStatusV1::cursor_stale,
                       kCursorStale,
                       "receipt_invalidated_before_publication_barrier");
  }
  if (encoded_batch_over_bound) {
    staged.lease.Abort();
    state.lifecycle = TypedResultProducerCursorLifecycleV1::open;
    return PullFailure(
        TypedResultProducerCursorStatusV1::resource_budget_exceeded,
        kResourceExceeded, "encoded_batch_exceeds_admitted_bound");
  }
  const u64 staged_bytes = published.row_data_packet.size();
  const u64 publication_grant_bytes =
      staged_bytes == 0 ? request.maximum_bytes : staged_bytes;
  try {
    grant = state.resource_grant_receipt->ObserveGrant(
        state.resource_grant_receipt_uuid,
        state.resource_grant_generation, state.resource_grant_bytes,
        publication_grant_bytes);
  } catch (const std::bad_alloc&) {
    throw;
  } catch (...) {
    grant = TypedResultProducerGrantObservationV1::stale_or_released;
  }
  if (grant != TypedResultProducerGrantObservationV1::live) {
    staged.lease.Abort();
    if (grant == TypedResultProducerGrantObservationV1::stale_or_released) {
      state.TerminalLocked(
          TypedResultProducerCursorLifecycleV1::revoked,
          TypedResultProducerReleaseReasonV1::receipt_invalidation);
    } else {
      state.lifecycle = TypedResultProducerCursorLifecycleV1::open;
    }
    return PullFailure(
        TypedResultProducerCursorStatusV1::resource_budget_exceeded,
        kResourceExceeded, "grant_refused_staged_batch_before_publication");
  }
  cancellation =
      safe_point_cancellation !=
              TypedResultProducerCancellationObservationV1::live
          ? safe_point_cancellation
          : observe_cancellation();
  if (cancellation ==
      TypedResultProducerCancellationObservationV1::requested) {
    staged.lease.Abort();
    state.TerminalLocked(TypedResultProducerCursorLifecycleV1::cancelled,
                         TypedResultProducerReleaseReasonV1::cancellation);
    return PullFailure(TypedResultProducerCursorStatusV1::cancelled,
                       kCancelled, "cancellation_before_publication_barrier",
                       TypedResultProducerPullOutcomeV1::cancelled);
  }
  if (cancellation != TypedResultProducerCancellationObservationV1::live) {
    staged.lease.Abort();
    state.TerminalLocked(TypedResultProducerCursorLifecycleV1::revoked,
                         TypedResultProducerReleaseReasonV1::receipt_invalidation);
    return PullFailure(TypedResultProducerCursorStatusV1::cursor_stale,
                       kCursorStale,
                       "cancellation_receipt_invalidated_before_publication");
  }

  if (producer_cancelled) {
    staged.lease.Abort();
    state.TerminalLocked(TypedResultProducerCursorLifecycleV1::cancelled,
                         TypedResultProducerReleaseReasonV1::cancellation);
    return PullFailure(TypedResultProducerCursorStatusV1::cancelled,
                       kCancelled, "producer_safe_point_cancelled",
                       TypedResultProducerPullOutcomeV1::cancelled);
  }
  if (producer_refused || producer_failed) {
    staged.lease.Abort();
    state.lifecycle = TypedResultProducerCursorLifecycleV1::open;
    std::string detail = std::move(producer_failure_detail);
    if (detail.empty()) detail = std::move(staged.detail);
    if (detail.empty()) detail = "producer_refused";
    return PullFailure(TypedResultProducerCursorStatusV1::fetch_failed,
                       kFetchFailed, std::move(detail));
  }

  if (empty_open) {
    state.lifecycle = TypedResultProducerCursorLifecycleV1::open;
    return published;
  }

  const auto source_commit = staged.lease.Commit();
  if (source_commit !=
      TypedResultProducerStageCommitStatusV1::committed) {
    staged.lease.Abort();
    switch (source_commit) {
      case TypedResultProducerStageCommitStatusV1::cancelled:
        state.TerminalLocked(
            TypedResultProducerCursorLifecycleV1::cancelled,
            TypedResultProducerReleaseReasonV1::cancellation);
        return PullFailure(TypedResultProducerCursorStatusV1::cancelled,
                           kCancelled,
                           "source_publication_charge_cancelled",
                           TypedResultProducerPullOutcomeV1::cancelled);
      case TypedResultProducerStageCommitStatusV1::stale:
        state.TerminalLocked(
            TypedResultProducerCursorLifecycleV1::revoked,
            TypedResultProducerReleaseReasonV1::receipt_invalidation);
        return PullFailure(TypedResultProducerCursorStatusV1::cursor_stale,
                           kCursorStale,
                           "source_publication_authority_stale");
      case TypedResultProducerStageCommitStatusV1::
          resource_budget_exceeded:
        state.lifecycle = TypedResultProducerCursorLifecycleV1::open;
        return PullFailure(
            TypedResultProducerCursorStatusV1::resource_budget_exceeded,
            kResourceExceeded,
            "source_publication_charge_exceeds_retained_authority");
      case TypedResultProducerStageCommitStatusV1::committed:
        break;
      default:
        // A source transition has not committed merely because its status is
        // unknown. The lease is already aborted; publish no rows or EOS.
        state.lifecycle = TypedResultProducerCursorLifecycleV1::open;
        return PullFailure(TypedResultProducerCursorStatusV1::fetch_failed,
                           kFetchFailed,
                           "source_publication_commit_status_invalid");
    }
  }

  static_assert(std::is_nothrow_move_assignable_v<
                wire::TypedResultCursorBatchState>);
  static_assert(std::is_nothrow_move_constructible_v<
                TypedResultProducerPullResultV1>);

  // Sole no-fail publication barrier.  The source lease has atomically charged
  // and installed its logical cursor transition; only no-throw carrier stores,
  // swaps, release callbacks, and the already-built return value follow.
  if (empty_eos) {
    state.TerminalLocked(TypedResultProducerCursorLifecycleV1::eos,
                         TypedResultProducerReleaseReasonV1::eos);
    return published;
  }

  state.batch_state = std::move(staged_batch_state);
  state.row_position = old_position + published.row_count;
  state.next_batch_ordinal = old_ordinal + 1;
  state.pending_batch_uuid = {};
  if (published.end_of_cursor) {
    state.TerminalLocked(TypedResultProducerCursorLifecycleV1::eos,
                         TypedResultProducerReleaseReasonV1::eos);
    return published;
  }

  state.lifecycle = TypedResultProducerCursorLifecycleV1::open;
  return published;
} catch (const std::bad_alloc&) {
  TypedResultProducerPullResultV1 result;
  result.status = TypedResultProducerCursorStatusV1::resource_budget_exceeded;
  return result;
}

TypedResultProducerOperationResultV1 CloseTypedResultProducerCursorV1(
    TypedResultProducerCursorCarrierV1& carrier,
    TypedResultProducerCloseReasonV1 reason) {
  auto& state = *carrier.state_;
  std::lock_guard<std::mutex> lock(state.mutation_gate);
  TypedResultProducerOperationResultV1 result;
  result.lifecycle = state.lifecycle;
  if (!ValidCloseReason(reason)) {
    result.status = TypedResultProducerCursorStatusV1::invalid_argument;
    result.diagnostic_code = kInvalidArgument;
    result.detail = "unknown_close_reason";
    return result;
  }
  if (IsTerminal(state.lifecycle) ||
      state.lifecycle != TypedResultProducerCursorLifecycleV1::open) {
    result.status = TypedResultProducerCursorStatusV1::cursor_stale;
    result.diagnostic_code = kCursorStale;
    result.detail = "terminal_transition_already_won";
    return result;
  }
  const auto terminal = LifecycleForClose(reason);
  state.TerminalLocked(terminal, ReleaseReasonForClose(reason));
  result.lifecycle = terminal;
  return result;
}

const char* TypedResultProducerCursorStatusNameV1(
    TypedResultProducerCursorStatusV1 status) {
  switch (status) {
    case TypedResultProducerCursorStatusV1::ok:
      return "ok";
    case TypedResultProducerCursorStatusV1::invalid_argument:
      return "invalid_argument";
    case TypedResultProducerCursorStatusV1::access_denied:
      return "access_denied";
    case TypedResultProducerCursorStatusV1::mga_stale:
      return "mga_stale";
    case TypedResultProducerCursorStatusV1::descriptor_invalid:
      return "descriptor_invalid";
    case TypedResultProducerCursorStatusV1::cursor_stale:
      return "cursor_stale";
    case TypedResultProducerCursorStatusV1::resource_budget_exceeded:
      return "resource_budget_exceeded";
    case TypedResultProducerCursorStatusV1::cancelled:
      return "cancelled";
    case TypedResultProducerCursorStatusV1::fetch_failed:
      return "fetch_failed";
  }
  return "unknown";
}

const char* TypedResultProducerCursorLifecycleNameV1(
    TypedResultProducerCursorLifecycleV1 lifecycle) {
  switch (lifecycle) {
    case TypedResultProducerCursorLifecycleV1::open:
      return "open";
    case TypedResultProducerCursorLifecycleV1::pulling:
      return "pulling";
    case TypedResultProducerCursorLifecycleV1::eos:
      return "eos";
    case TypedResultProducerCursorLifecycleV1::cancelled:
      return "cancelled";
    case TypedResultProducerCursorLifecycleV1::closed:
      return "closed";
    case TypedResultProducerCursorLifecycleV1::revoked:
      return "revoked";
  }
  return "unknown";
}

}  // namespace scratchbird::engine::internal_api
