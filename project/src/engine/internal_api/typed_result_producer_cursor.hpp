// Copyright (c) 2026 ScratchBird Software Inc.
//
// This Source Code Form is subject to the terms of the Mozilla Public
// License, v. 2.0. If a copy of the MPL was not distributed with this
// file, You can obtain one at https://mozilla.org/MPL/2.0/.
//
// SPDX-License-Identifier: MPL-2.0

#pragma once

// SB-ENGINE-TYPED-RESULT-PRODUCER-CURSOR-ANCHOR
//
// Server-private, non-serializable implementation surface for
// QUERY-EXECUTE-PRODUCER-CURSOR-CARRIER-V1.  The carrier accepts typed rows
// only.  It creates and verifies the exact RowDataPacketV1 before one atomic
// publication barrier; no string or aggregate result adaptation exists here.

#include "wire/typed_result_transport_carrier.hpp"

#include <cstdint>
#include <functional>
#include <memory>
#include <string>
#include <vector>

namespace scratchbird::engine::internal_api {

using scratchbird::core::platform::byte;
using scratchbird::core::platform::u16;
using scratchbird::core::platform::u64;

inline constexpr u16 kTypedResultProducerCursorVersionV1 = 1;
inline constexpr u64 kTypedResultProducerMaximumRowsV1 = 1'048'576;
inline constexpr u64 kTypedResultProducerMaximumBytesV1 = 16u * 1024u * 1024u;

enum class TypedResultProducerCursorStatusV1 : std::uint8_t {
  ok = 0,
  invalid_argument,
  access_denied,
  mga_stale,
  descriptor_invalid,
  cursor_stale,
  resource_budget_exceeded,
  cancelled,
  fetch_failed,
};

enum class TypedResultProducerCursorLifecycleV1 : std::uint8_t {
  open = 0,
  pulling,
  eos,
  cancelled,
  closed,
  revoked,
};

enum class TypedResultProducerPullOutcomeV1 : std::uint8_t {
  batch = 0,
  empty_open,
  empty_eos,
  cancelled,
  refused,
};

enum class TypedResultProducerStageOutcomeV1 : std::uint8_t {
  batch = 0,
  empty_open,
  empty_eos,
  cancelled,
  refused,
};

enum class TypedResultProducerCloseReasonV1 : std::uint8_t {
  explicit_close = 0,
  cancellation,
  timeout,
  receipt_invalidation,
  transaction_invalidation,
  disconnect,
  shutdown,
  recovery,
};

enum class TypedResultProducerReleaseReasonV1 : std::uint8_t {
  eos = 0,
  cancellation,
  explicit_close,
  timeout,
  receipt_invalidation,
  transaction_invalidation,
  disconnect,
  shutdown,
  recovery,
  open_refused,
};

enum class TypedResultProducerOwnerObservationV1 : std::uint8_t {
  authorized = 0,
  denied,
};

enum class TypedResultProducerReceiptObservationV1 : std::uint8_t {
  live = 0,
  stale,
};

enum class TypedResultProducerMgaObservationV1 : std::uint8_t {
  live_and_equal = 0,
  stale_or_unequal,
};

enum class TypedResultProducerCancellationObservationV1 : std::uint8_t {
  live = 0,
  requested,
  stale,
};

enum class TypedResultProducerGrantObservationV1 : std::uint8_t {
  live = 0,
  stale_or_released,
  exhausted,
};

class TypedResultStatementReceiptHandleV1 {
 public:
  virtual ~TypedResultStatementReceiptHandleV1() = default;

  virtual TypedResultProducerOwnerObservationV1 ObserveOwner(
      const wire::TypedResultUuid& session_uuid) = 0;
  virtual TypedResultProducerReceiptObservationV1 ObserveReceipt(
      const wire::TypedResultUuid& statement_receipt_uuid) = 0;
  virtual void Release(TypedResultProducerReleaseReasonV1 reason) noexcept = 0;
};

class TypedResultMgaSnapshotPinHandleV1 {
 public:
  virtual ~TypedResultMgaSnapshotPinHandleV1() = default;

  virtual TypedResultProducerMgaObservationV1 ObserveSnapshot(
      const wire::TypedResultUuid& statement_snapshot_uuid,
      const wire::TypedResultUuid& result_snapshot_uuid) = 0;
  virtual void Release(TypedResultProducerReleaseReasonV1 reason) noexcept = 0;
};

class TypedResultCancellationReceiptHandleV1 {
 public:
  virtual ~TypedResultCancellationReceiptHandleV1() = default;

  virtual TypedResultProducerCancellationObservationV1 ObserveCancellation(
      const wire::TypedResultUuid& receipt_uuid,
      u64 receipt_generation) = 0;
  virtual void Release(TypedResultProducerReleaseReasonV1 reason) noexcept = 0;
};

class TypedResultResourceGrantReceiptHandleV1 {
 public:
  virtual ~TypedResultResourceGrantReceiptHandleV1() = default;

  virtual TypedResultProducerGrantObservationV1 ObserveGrant(
      const wire::TypedResultUuid& receipt_uuid,
      u64 receipt_generation,
      u64 retained_ceiling_bytes,
      u64 requested_bytes) = 0;
  virtual void Release(TypedResultProducerReleaseReasonV1 reason) noexcept = 0;
};

// The callback is private in-memory producer state.  It must poll this probe at
// each producer safe point/page boundary.  A true result means the staged page
// must be discarded and Stage must return `cancelled`.
using TypedResultProducerCancellationProbeV1 = std::function<bool()>;

struct TypedResultProducerStageRequestV1 {
  u64 next_batch_ordinal = 0;
  u64 row_position = 0;
  u64 maximum_rows = 0;
  u64 maximum_bytes = 0;
  u64 timeout_millis = 0;
  TypedResultProducerCancellationProbeV1 cancellation_requested;
};

// Move-only, process-private publication lease for one staged producer result.
// A live lease owns every provisional source transition represented by the
// sibling Stage result.  Commit is the no-fail source publication barrier;
// destruction and explicit Abort discard the transition.  The action is an
// owned engine object, never a parser-supplied callback.
class TypedResultProducerStageLeaseActionV1 {
 public:
  virtual ~TypedResultProducerStageLeaseActionV1() = default;
  enum class CommitStatus : std::uint8_t {
    committed = 0,
    cancelled,
    stale,
    resource_budget_exceeded,
  };

  virtual CommitStatus Commit() noexcept = 0;
  virtual void Abort() noexcept = 0;
};

using TypedResultProducerStageCommitStatusV1 =
    TypedResultProducerStageLeaseActionV1::CommitStatus;

class TypedResultProducerStageLeaseV1 final {
 public:
  // A default lease is inactive.  Stateful batch/EOS producers must return an
  // explicitly owned action; no-mutation outcomes must retain this state.
  TypedResultProducerStageLeaseV1() noexcept = default;
  explicit TypedResultProducerStageLeaseV1(
      std::unique_ptr<TypedResultProducerStageLeaseActionV1> action) noexcept;
  ~TypedResultProducerStageLeaseV1();

  TypedResultProducerStageLeaseV1(
      const TypedResultProducerStageLeaseV1&) = delete;
  TypedResultProducerStageLeaseV1& operator=(
      const TypedResultProducerStageLeaseV1&) = delete;
  TypedResultProducerStageLeaseV1(
      TypedResultProducerStageLeaseV1&& other) noexcept;
  TypedResultProducerStageLeaseV1& operator=(
      TypedResultProducerStageLeaseV1&& other) noexcept;

  [[nodiscard]] TypedResultProducerStageCommitStatusV1 Commit() noexcept;
  void Abort() noexcept;
  [[nodiscard]] bool pending() const noexcept { return pending_; }

 private:
  std::unique_ptr<TypedResultProducerStageLeaseActionV1> action_;
  bool pending_ = false;
};

struct TypedResultProducerStageResultV1 {
  TypedResultProducerStageOutcomeV1 outcome =
      TypedResultProducerStageOutcomeV1::refused;
  bool end_of_cursor = false;
  std::vector<wire::TypedResultRow> rows;
  std::string detail;
  TypedResultProducerStageLeaseV1 lease;
};

class TypedResultProducerSourceV1 {
 public:
  virtual ~TypedResultProducerSourceV1() = default;

  virtual TypedResultProducerStageResultV1 Stage(
      const TypedResultProducerStageRequestV1& request) = 0;
  virtual void Close(TypedResultProducerReleaseReasonV1 reason) noexcept = 0;
};

struct TypedResultProducerOpenRequestV1 {
  u16 version = kTypedResultProducerCursorVersionV1;
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
  wire::TypedResultRowDescriptor row_descriptor;
  wire::TypedResultDescriptorAuthorityValidator descriptor_authority;
  std::unique_ptr<TypedResultStatementReceiptHandleV1> statement_receipt;
  std::unique_ptr<TypedResultMgaSnapshotPinHandleV1> mga_snapshot_pin;
  std::unique_ptr<TypedResultCancellationReceiptHandleV1>
      cancellation_receipt;
  std::unique_ptr<TypedResultResourceGrantReceiptHandleV1>
      resource_grant_receipt;
  std::unique_ptr<TypedResultProducerSourceV1> producer_state;
};

struct TypedResultProducerPullRequestV1 {
  u16 version = kTypedResultProducerCursorVersionV1;
  wire::TypedResultUuid cursor_uuid{};
  u64 carrier_generation = 0;
  wire::TypedResultUuid cursor_stream_descriptor_uuid{};
  u16 cursor_stream_descriptor_version = 0;
  u64 cursor_stream_descriptor_generation = 0;
  u64 expected_batch_ordinal = 0;
  u64 maximum_rows = 0;
  u64 maximum_bytes = 0;
  u64 timeout_millis = 0;
};

struct TypedResultProducerCursorSnapshotV1 {
  wire::TypedResultUuid carrier_uuid{};
  u64 carrier_generation = 0;
  wire::TypedResultUuid cursor_uuid{};
  wire::TypedResultUuid session_uuid{};
  wire::TypedResultUuid execution_uuid{};
  wire::TypedResultUuid result_set_uuid{};
  wire::TypedResultUuid row_descriptor_uuid{};
  u64 row_descriptor_generation = 0;
  wire::TypedResultUuid snapshot_uuid{};
  wire::TypedResultUuid cursor_stream_descriptor_uuid{};
  u16 cursor_stream_descriptor_version = 0;
  u64 cursor_stream_descriptor_generation = 0;
  u64 next_batch_ordinal = 0;
  u64 row_position = 0;
  TypedResultProducerCursorLifecycleV1 lifecycle =
      TypedResultProducerCursorLifecycleV1::revoked;
  bool retained_authority_released = false;
};

struct TypedResultProducerOpenResultV1;
struct TypedResultProducerPullResultV1;
struct TypedResultProducerOperationResultV1;

// Optional engine-internal staging hook used by an outer carrier encoder.  It
// receives only the fully validated candidate Pull result and grants no parser
// authority.  Acceptance merely permits the retained authority pass and the
// existing Pull publication barrier to proceed.
struct TypedResultProducerPrecommitDecisionV1 {
  bool accepted = false;
  TypedResultProducerCursorStatusV1 refusal_status =
      TypedResultProducerCursorStatusV1::fetch_failed;
  std::string diagnostic_code;
  std::string detail;
};

using TypedResultProducerPrecommitGateV1 =
    std::function<TypedResultProducerPrecommitDecisionV1(
        const TypedResultProducerPullResultV1&)>;

class TypedResultProducerCursorCarrierV1 {
 public:
  ~TypedResultProducerCursorCarrierV1();
  TypedResultProducerCursorCarrierV1(
      const TypedResultProducerCursorCarrierV1&) = delete;
  TypedResultProducerCursorCarrierV1& operator=(
      const TypedResultProducerCursorCarrierV1&) = delete;
  TypedResultProducerCursorCarrierV1(
      TypedResultProducerCursorCarrierV1&&) = delete;
  TypedResultProducerCursorCarrierV1& operator=(
      TypedResultProducerCursorCarrierV1&&) = delete;

  [[nodiscard]] TypedResultProducerCursorSnapshotV1 Snapshot() const;
  [[nodiscard]] const std::vector<byte>& result_descriptor_vector() const;
  [[nodiscard]] const wire::TypedResultRowDescriptor& row_descriptor() const;

 private:
  struct State;
  explicit TypedResultProducerCursorCarrierV1(std::unique_ptr<State> state);

  std::unique_ptr<State> state_;

  friend TypedResultProducerOpenResultV1 OpenTypedResultProducerCursorV1(
      TypedResultProducerOpenRequestV1 request);
  friend TypedResultProducerPullResultV1 PullTypedResultProducerCursorV1(
      TypedResultProducerCursorCarrierV1& carrier,
      const TypedResultProducerPullRequestV1& request);
  friend TypedResultProducerPullResultV1 PullTypedResultProducerCursorV1(
      TypedResultProducerCursorCarrierV1& carrier,
      const TypedResultProducerPullRequestV1& request,
      const TypedResultProducerPrecommitGateV1& precommit_gate);
  friend TypedResultProducerOperationResultV1 CloseTypedResultProducerCursorV1(
      TypedResultProducerCursorCarrierV1& carrier,
      TypedResultProducerCloseReasonV1 reason);
};

struct TypedResultProducerOpenResultV1 {
  TypedResultProducerCursorStatusV1 status =
      TypedResultProducerCursorStatusV1::ok;
  std::string diagnostic_code;
  std::string detail;
  std::unique_ptr<TypedResultProducerCursorCarrierV1> carrier;

  [[nodiscard]] bool ok() const {
    return status == TypedResultProducerCursorStatusV1::ok && carrier != nullptr;
  }
};

struct TypedResultProducerPullResultV1 {
  TypedResultProducerCursorStatusV1 status =
      TypedResultProducerCursorStatusV1::ok;
  std::string diagnostic_code;
  std::string detail;
  TypedResultProducerPullOutcomeV1 outcome =
      TypedResultProducerPullOutcomeV1::refused;
  u64 row_count = 0;
  bool end_of_cursor = false;
  std::vector<byte> row_data_packet;
  wire::TypedResultBatch batch;

  [[nodiscard]] bool ok() const {
    return status == TypedResultProducerCursorStatusV1::ok;
  }
};

struct TypedResultProducerOperationResultV1 {
  TypedResultProducerCursorStatusV1 status =
      TypedResultProducerCursorStatusV1::ok;
  std::string diagnostic_code;
  std::string detail;
  TypedResultProducerCursorLifecycleV1 lifecycle =
      TypedResultProducerCursorLifecycleV1::revoked;

  [[nodiscard]] bool ok() const {
    return status == TypedResultProducerCursorStatusV1::ok;
  }
};

TypedResultProducerOpenResultV1 OpenTypedResultProducerCursorV1(
    TypedResultProducerOpenRequestV1 request);

TypedResultProducerPullResultV1 PullTypedResultProducerCursorV1(
    TypedResultProducerCursorCarrierV1& carrier,
    const TypedResultProducerPullRequestV1& request);

// Same Core Pull operation and exact request/result carriers.  This overload is
// private engine composition for staging an outer result before Pull commits;
// it does not add a transport operation or a serialized field.
TypedResultProducerPullResultV1 PullTypedResultProducerCursorV1(
    TypedResultProducerCursorCarrierV1& carrier,
    const TypedResultProducerPullRequestV1& request,
    const TypedResultProducerPrecommitGateV1& precommit_gate);

TypedResultProducerOperationResultV1 CloseTypedResultProducerCursorV1(
    TypedResultProducerCursorCarrierV1& carrier,
    TypedResultProducerCloseReasonV1 reason);

const char* TypedResultProducerCursorStatusNameV1(
    TypedResultProducerCursorStatusV1 status);
const char* TypedResultProducerCursorLifecycleNameV1(
    TypedResultProducerCursorLifecycleV1 lifecycle);

}  // namespace scratchbird::engine::internal_api
