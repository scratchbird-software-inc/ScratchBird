// Copyright (c) 2026 ScratchBird Software Inc.
//
// This Source Code Form is subject to the terms of the Mozilla Public
// License, v. 2.0. If a copy of the MPL was not distributed with this
// file, You can obtain one at https://mozilla.org/MPL/2.0/.
//
// SPDX-License-Identifier: MPL-2.0

#pragma once

#include "api_types.hpp"
#include "typed_update_carrier_codec.hpp"

#include <cstdint>
#include <memory>
#include <string>
#include <vector>

namespace scratchbird::engine::internal_api {

// SEARCH_KEY: SB_ENGINE_DML_UPDATE_DATATYPE_OPERATOR_AUTHORITY_PROVIDER_V1
// Engine-private projection of exact live datatype and builtin-operator
// registry rows into DUDV/DUDR and DUOV/DUOE.  The provider accepts only
// already decoded canonical UPDATE carriers; it never infers authority from a
// spelling, value width, host enum, or parser payload.

struct EngineDmlUpdateDatatypeOperatorAuthorityCaptureRequestV1;
struct EngineDmlUpdateDatatypeOperatorAuthorityCaptureResultV1;

struct EngineDmlUpdateDatatypeOperatorBindingRequestV1 {
  EngineRequestContext context;
  bool equality_required = false;
  std::string left_descriptor_uuid;
  std::uint64_t left_descriptor_generation = 0;
  std::string left_type_uuid;
  std::uint64_t left_type_generation = 0;
  std::string right_descriptor_uuid;
  std::uint64_t right_descriptor_generation = 0;
  std::string right_type_uuid;
  std::uint64_t right_type_generation = 0;
};

struct EngineDmlUpdateDatatypeOperatorBindingResultV1 {
  bool ok = false;
  EngineApiDiagnostic diagnostic;
  std::string datatype_snapshot_uuid;
  std::uint64_t datatype_catalog_generation = 0;
  std::uint64_t datatype_registry_generation = 0;
  std::string boolean_descriptor_uuid;
  std::uint64_t boolean_descriptor_generation = 0;
  std::string boolean_type_uuid;
  std::uint64_t boolean_type_generation = 0;
  std::string boolean_codec_id;
  std::uint16_t boolean_codec_version = 0;
  std::uint64_t boolean_codec_generation = 0;
  std::string builtin_operator_snapshot_uuid;
  std::uint64_t builtin_operator_registry_generation = 0;
  std::string equality_operator_uuid;
  std::uint64_t equality_operator_generation = 0;
};

EngineDmlUpdateDatatypeOperatorBindingResultV1
ResolveDmlUpdateDatatypeOperatorBindingAuthorityV1(
    const EngineDmlUpdateDatatypeOperatorBindingRequestV1& request);

class EngineDmlUpdateDatatypeSnapshotHandleV1 final {
 public:
  EngineDmlUpdateDatatypeSnapshotHandleV1() = default;
  bool valid() const noexcept { return authority_ != nullptr; }

 private:
  struct Authority;
  std::shared_ptr<const Authority> authority_;

  friend struct EngineDmlUpdateDatatypeOperatorAuthorityCaptureResultV1;
  friend EngineDmlUpdateDatatypeOperatorAuthorityCaptureResultV1
  CaptureDmlUpdateDatatypeOperatorAuthorityV1(
      const struct EngineDmlUpdateDatatypeOperatorAuthorityCaptureRequestV1&);
  friend EngineApiDiagnostic RevalidateDmlUpdateDatatypeOperatorAuthorityV1(
      const EngineRequestContext&,
      const struct EngineDmlUpdateDatatypeOperatorAuthorityCaptureResultV1&);
};

class EngineDmlUpdateBuiltinOperatorSnapshotHandleV1 final {
 public:
  EngineDmlUpdateBuiltinOperatorSnapshotHandleV1() = default;
  bool valid() const noexcept { return authority_ != nullptr; }

 private:
  struct Authority;
  std::shared_ptr<const Authority> authority_;

  friend struct EngineDmlUpdateDatatypeOperatorAuthorityCaptureResultV1;
  friend EngineDmlUpdateDatatypeOperatorAuthorityCaptureResultV1
  CaptureDmlUpdateDatatypeOperatorAuthorityV1(
      const struct EngineDmlUpdateDatatypeOperatorAuthorityCaptureRequestV1&);
  friend EngineApiDiagnostic RevalidateDmlUpdateDatatypeOperatorAuthorityV1(
      const EngineRequestContext&,
      const struct EngineDmlUpdateDatatypeOperatorAuthorityCaptureResultV1&);
};

struct EngineDmlUpdateDatatypeOperatorAuthorityCaptureRequestV1 {
  EngineRequestContext context;
  std::string authenticated_statement_receipt_uuid;
  std::vector<std::uint8_t> exact_descriptor_dudc;
  std::vector<std::uint8_t> exact_assignment_vector_duav;
  std::vector<std::uint8_t> exact_predicate_vector_duev;
};

struct EngineDmlUpdateDatatypeOperatorAuthorityCaptureResultV1 {
  bool ok = false;
  EngineApiDiagnostic diagnostic;
  scratchbird::wire::TypedUpdateDatatypeAuthorityVector datatypes;
  scratchbird::wire::TypedUpdateBuiltinOperatorAuthorityVector operators;
  std::vector<std::uint8_t> exact_datatype_authority_dudv;
  std::vector<std::uint8_t> exact_builtin_operator_authority_duov;
  EngineDmlUpdateDatatypeSnapshotHandleV1 datatype_snapshot_handle;
  EngineDmlUpdateBuiltinOperatorSnapshotHandleV1 operator_snapshot_handle;
};

EngineDmlUpdateDatatypeOperatorAuthorityCaptureResultV1
CaptureDmlUpdateDatatypeOperatorAuthorityV1(
    const EngineDmlUpdateDatatypeOperatorAuthorityCaptureRequestV1& request);

EngineApiDiagnostic RevalidateDmlUpdateDatatypeOperatorAuthorityV1(
    const EngineRequestContext& context,
    const EngineDmlUpdateDatatypeOperatorAuthorityCaptureResultV1& captured);

// Restart/recovery path.  The caller supplies only canonical decoded carriers
// retaining their byte-identical extents; this provider re-resolves every row
// against the current live registries.  It does not reconstruct a snapshot
// handle from durable bytes.
EngineApiDiagnostic RevalidateRecoveredDmlUpdateDatatypeOperatorAuthorityV1(
    const EngineRequestContext& context,
    const scratchbird::wire::TypedUpdateDescriptorCarrier& descriptor,
    const scratchbird::wire::TypedUpdateAssignmentVector& assignments,
    const scratchbird::wire::TypedUpdatePredicateVector& predicate,
    const scratchbird::wire::TypedUpdateDatatypeAuthorityVector& datatypes,
    const scratchbird::wire::TypedUpdateBuiltinOperatorAuthorityVector&
        operators);

}  // namespace scratchbird::engine::internal_api
