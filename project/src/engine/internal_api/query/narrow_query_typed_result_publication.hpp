// Copyright (c) 2026 ScratchBird Software Inc.
//
// This Source Code Form is subject to the terms of the Mozilla Public
// License, v. 2.0. If a copy of the MPL was not distributed with this
// file, You can obtain one at https://mozilla.org/MPL/2.0/.
//
// SPDX-License-Identifier: MPL-2.0

#pragma once

// Engine-private publication boundary for the three exact SBQNPB01 query
// profiles.  This surface accepts only a canonical, live-context-validated
// binding and occurrence-keyed canonical values.  It has no name map, rendered
// row, delimiter parser, descriptor inference, or generic-query fallback.

#include "engine/internal_api/typed_result_producer_cursor.hpp"
#include "wire/narrow_query_binding_codec.hpp"

#include <cstdint>
#include <functional>
#include <memory>
#include <span>
#include <string>
#include <vector>

namespace scratchbird::engine::internal_api {

using scratchbird::core::platform::byte;
using scratchbird::core::platform::u16;
using scratchbird::core::platform::u64;

inline constexpr u16 kNarrowQueryTypedResultPublicationVersionV1 = 1;

enum class NarrowQueryTypedResultPublicationStatusV1 : std::uint8_t {
  ok = 0,
  invalid_argument,
  profile_unavailable,
  binding_invalid,
  descriptor_invalid,
  result_receipt_mismatch,
  row_shape_invalid,
  resource_budget_exceeded,
  cancelled,
  cursor_refused,
  publication_failed,
};

struct NarrowQueryTypedResultOutcomeV1 {
  NarrowQueryTypedResultPublicationStatusV1 status =
      NarrowQueryTypedResultPublicationStatusV1::invalid_argument;
  std::string diagnostic_code;
  std::string detail;

  [[nodiscard]] bool ok() const {
    return status == NarrowQueryTypedResultPublicationStatusV1::ok;
  }

  // Core explicitly forbids retry through a generic, legacy, compatibility,
  // or inferred profile after any refusal on this boundary.
  [[nodiscard]] constexpr bool generic_fallback_permitted() const {
    return false;
  }
};

// Engine result identity expected independently of the evidence inside the
// SBQNPB01 bytes.  The deterministic binding hash is intentionally absent: it
// is evidence, not receipt or authentication authority.
struct NarrowQueryTypedResultExpectedReceiptV1 {
  u16 version = kNarrowQueryTypedResultPublicationVersionV1;
  u16 profile_code = 0;
  wire::NarrowQueryUuid statement_receipt_uuid{};
  wire::TypedResultQueryHandleV1 query_handle;
  u64 row_descriptor_generation = 0;
};

// Frozen receipt carried between preparation and either publication path.
// profile_binding_evidence_sha256 identifies exact SBQNPB01 bytes but never
// grants authority on its own.
struct NarrowQueryTypedResultPublicationReceiptV1 {
  u16 version = kNarrowQueryTypedResultPublicationVersionV1;
  u16 profile_code = 0;
  wire::NarrowQueryUuid statement_receipt_uuid{};
  wire::TypedResultQueryHandleV1 query_handle;
  u64 row_descriptor_generation = 0;
  wire::NarrowQueryHash profile_binding_evidence_sha256{};
};

class NarrowQueryTypedResultPublicationBindingV1;
struct NarrowQueryTypedResultOccurrenceRowV1;
struct NarrowQueryTypedResultMaterializeResultV1;
struct NarrowQueryTypedResultDirectRequestV1;
struct NarrowQueryTypedResultDirectResultV1;
struct NarrowQueryTypedResultCursorOpenRequestV1;
struct NarrowQueryTypedResultCursorOpenResultV1;

struct NarrowQueryTypedResultPrepareRequestV1 {
  u16 version = kNarrowQueryTypedResultPublicationVersionV1;
  std::span<const byte> canonical_profile_binding;
  wire::NarrowQueryBindingValidationContext validation_context;
  NarrowQueryTypedResultExpectedReceiptV1 expected_receipt;
  wire::TypedResultDescriptorAuthorityValidator descriptor_authority;
};

struct NarrowQueryTypedResultPrepareResultV1
    : NarrowQueryTypedResultOutcomeV1 {
  std::shared_ptr<const NarrowQueryTypedResultPublicationBindingV1> binding;
};

class NarrowQueryTypedResultPublicationBindingV1 {
 public:
  NarrowQueryTypedResultPublicationBindingV1(
      const NarrowQueryTypedResultPublicationBindingV1&) = delete;
  NarrowQueryTypedResultPublicationBindingV1& operator=(
      const NarrowQueryTypedResultPublicationBindingV1&) = delete;

  [[nodiscard]] wire::NarrowQueryProfile profile() const;
  [[nodiscard]] const NarrowQueryTypedResultPublicationReceiptV1& receipt()
      const;
  [[nodiscard]] const std::vector<byte>& canonical_profile_binding() const;
  [[nodiscard]] const std::vector<byte>& result_descriptor_vector() const;
  [[nodiscard]] const wire::TypedResultRowDescriptor& row_descriptor() const;

 private:
  struct State;
  explicit NarrowQueryTypedResultPublicationBindingV1(
      std::shared_ptr<const State> state);

  std::shared_ptr<const State> state_;

  friend NarrowQueryTypedResultPrepareResultV1
  PrepareNarrowQueryTypedResultPublicationV1(
      const NarrowQueryTypedResultPrepareRequestV1& request);
  friend NarrowQueryTypedResultMaterializeResultV1
  MaterializeNarrowQueryTypedResultRowV1(
      const NarrowQueryTypedResultPublicationBindingV1& binding,
      const NarrowQueryTypedResultOccurrenceRowV1& row);
  friend NarrowQueryTypedResultDirectResultV1
  PublishNarrowQueryTypedResultDirectBatchV1(
      const std::shared_ptr<const NarrowQueryTypedResultPublicationBindingV1>&
          binding,
      const NarrowQueryTypedResultDirectRequestV1& request);
  friend NarrowQueryTypedResultCursorOpenResultV1
  OpenNarrowQueryTypedResultCursorV1(
      const std::shared_ptr<const NarrowQueryTypedResultPublicationBindingV1>&
          binding,
      NarrowQueryTypedResultCursorOpenRequestV1 request);
};

// Values are keyed only by the engine-issued output occurrence identity.
// Display names are deliberately absent from this input.
struct NarrowQueryTypedResultOccurrenceCellV1 {
  wire::NarrowQueryUuid output_occurrence_uuid{};
  u64 output_occurrence_generation = 0;
  wire::TypedResultValueState state =
      wire::TypedResultValueState::value_present;
  std::vector<byte> canonical_payload;
};

struct NarrowQueryTypedResultOccurrenceRowV1 {
  u64 row_ordinal = 0;
  std::vector<NarrowQueryTypedResultOccurrenceCellV1> cells;
};

struct NarrowQueryTypedResultMaterializeResultV1
    : NarrowQueryTypedResultOutcomeV1 {
  wire::TypedResultRow row;
};

NarrowQueryTypedResultPrepareResultV1
PrepareNarrowQueryTypedResultPublicationV1(
    const NarrowQueryTypedResultPrepareRequestV1& request);

NarrowQueryTypedResultMaterializeResultV1
MaterializeNarrowQueryTypedResultRowV1(
    const NarrowQueryTypedResultPublicationBindingV1& binding,
    const NarrowQueryTypedResultOccurrenceRowV1& row);

enum class NarrowQueryTypedResultPublicationPhaseV1 : std::uint8_t {
  before_row_materialization = 1,
  immediately_before_publication_barrier = 2,
};

struct NarrowQueryTypedResultPublicationAuthorityRequestV1 {
  NarrowQueryTypedResultPublicationPhaseV1 phase =
      NarrowQueryTypedResultPublicationPhaseV1::before_row_materialization;
  NarrowQueryTypedResultPublicationReceiptV1 receipt;
  u64 row_count = 0;
  u64 encoded_row_packet_bytes = 0;
};

struct NarrowQueryTypedResultPublicationAuthorityDecisionV1 {
  bool accepted = false;
  std::string diagnostic_code;
  std::string detail;
};

using NarrowQueryTypedResultPublicationAuthorityValidatorV1 =
    std::function<NarrowQueryTypedResultPublicationAuthorityDecisionV1(
        const NarrowQueryTypedResultPublicationAuthorityRequestV1&)>;

struct NarrowQueryTypedResultDirectRequestV1 {
  u16 version = kNarrowQueryTypedResultPublicationVersionV1;
  NarrowQueryTypedResultPublicationReceiptV1 receipt;
  wire::TypedResultUuid server_request_uuid{};
  wire::TypedResultUuid batch_uuid{};
  u64 maximum_descriptor_bytes = wire::kTypedResultCarrierMaximumBytes;
  u64 maximum_row_packet_bytes = wire::kTypedResultCarrierMaximumBytes;
  std::vector<NarrowQueryTypedResultOccurrenceRowV1> rows;
  NarrowQueryTypedResultPublicationAuthorityValidatorV1
      publication_authority;
};

struct NarrowQueryTypedResultDirectResultV1
    : NarrowQueryTypedResultOutcomeV1 {
  wire::TypedResultExecuteCarrierV1 execute_carrier;
  wire::TypedResultBatch batch;
};

NarrowQueryTypedResultDirectResultV1
PublishNarrowQueryTypedResultDirectBatchV1(
    const std::shared_ptr<const NarrowQueryTypedResultPublicationBindingV1>&
        binding,
    const NarrowQueryTypedResultDirectRequestV1& request);

// Cursor sources keep occurrence identity until this adapter performs the
// final immutable descriptor projection.  Source acquisition and query-plan
// execution are outside this interface.
struct NarrowQueryTypedResultOccurrenceStageResultV1 {
  TypedResultProducerStageOutcomeV1 outcome =
      TypedResultProducerStageOutcomeV1::refused;
  bool end_of_cursor = false;
  std::vector<NarrowQueryTypedResultOccurrenceRowV1> rows;
  std::string detail;
  TypedResultProducerStageLeaseV1 lease;
};

class NarrowQueryTypedResultOccurrenceSourceV1 {
 public:
  virtual ~NarrowQueryTypedResultOccurrenceSourceV1() = default;

  virtual NarrowQueryTypedResultOccurrenceStageResultV1 Stage(
      const TypedResultProducerStageRequestV1& request) = 0;
  virtual void Close(TypedResultProducerReleaseReasonV1 reason) noexcept = 0;
};

struct NarrowQueryTypedResultCursorOpenRequestV1 {
  u16 version = kNarrowQueryTypedResultPublicationVersionV1;
  NarrowQueryTypedResultPublicationReceiptV1 receipt;
  wire::TypedResultUuid server_request_uuid{};
  wire::TypedResultUuid carrier_uuid{};
  u64 carrier_generation = 0;
  wire::TypedResultUuid cursor_uuid{};
  wire::TypedResultUuid session_uuid{};
  u64 resource_grant_bytes = 0;
  u64 maximum_descriptor_bytes = wire::kTypedResultCarrierMaximumBytes;
  wire::TypedResultCursorStreamDescriptorV1 cursor_stream_descriptor;
  std::unique_ptr<TypedResultStatementReceiptHandleV1> statement_receipt;
  std::unique_ptr<TypedResultMgaSnapshotPinHandleV1> mga_snapshot_pin;
  std::unique_ptr<TypedResultCancellationReceiptHandleV1>
      cancellation_receipt;
  std::unique_ptr<TypedResultResourceGrantReceiptHandleV1>
      resource_grant_receipt;
  std::unique_ptr<NarrowQueryTypedResultOccurrenceSourceV1> producer_state;
};

struct NarrowQueryTypedResultCursorOpenResultV1
    : NarrowQueryTypedResultOutcomeV1 {
  wire::TypedResultExecuteCarrierV1 execute_carrier;
  std::unique_ptr<TypedResultProducerCursorCarrierV1> cursor;
};

NarrowQueryTypedResultCursorOpenResultV1 OpenNarrowQueryTypedResultCursorV1(
    const std::shared_ptr<const NarrowQueryTypedResultPublicationBindingV1>&
        binding,
    NarrowQueryTypedResultCursorOpenRequestV1 request);

const char* NarrowQueryTypedResultPublicationStatusNameV1(
    NarrowQueryTypedResultPublicationStatusV1 status);

}  // namespace scratchbird::engine::internal_api
