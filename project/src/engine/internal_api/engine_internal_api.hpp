// Copyright (c) 2026 ScratchBird Software Inc.
//
// This Source Code Form is subject to the terms of the Mozilla Public
// License, v. 2.0. If a copy of the MPL was not distributed with this
// file, You can obtain one at https://mozilla.org/MPL/2.0/.
//
// SPDX-License-Identifier: MPL-2.0

#pragma once

#include "datatype_descriptor.hpp"
#include "runtime_platform.hpp"
#include "uuid.hpp"

#include <string>
#include <vector>

namespace scratchbird::engine::internal_api {

using scratchbird::core::datatypes::CanonicalTypeId;
using scratchbird::core::platform::DiagnosticRecord;
using scratchbird::core::platform::Status;
using scratchbird::core::platform::TypedUuid;
using scratchbird::core::platform::UuidKind;
using scratchbird::core::platform::byte;
using scratchbird::core::platform::u16;
using scratchbird::core::platform::u32;

inline constexpr u32 kEngineInternalApiMajor = 1;
inline constexpr u32 kEngineInternalApiMinor = 0;
inline constexpr u32 kSblrEnvelopeMajor = 1;
inline constexpr u32 kSblrEnvelopeMinor = 0;

enum class SblrEnvelopeKind : u16 {
  native_sblr,
  management_sblr,
  system_bootstrap_sblr,
  unknown
};

enum class EngineOperationCode : u32 {
  show_version = 1,
  show_database = 2,
  show_database_resources = 3,
  catalog_lookup_by_uuid = 100,
  catalog_lookup_by_localized_path = 101,
  transaction_begin = 200,
  transaction_commit = 201,
  transaction_rollback = 202,
  unknown = 0xffffffffu
};

enum class OperationAuthorityClass : u16 {
  baseline_inspect,
  catalog_inspect,
  catalog_mutate,
  transaction_control,
  data_read,
  data_mutate,
  management_inspect,
  management_control,
  unknown
};

enum class EngineResultCardinality : u16 {
  none,
  single_row,
  bounded_rows,
  stream,
  unknown
};

struct EngineContext {
  TypedUuid database_uuid;
  TypedUuid session_uuid;
  TypedUuid principal_uuid;
  std::string trace_id;
  bool cluster_authority_active = false;
  bool parser_is_trusted = false;
};

struct SblrEnvelope {
  u32 envelope_major = kSblrEnvelopeMajor;
  u32 envelope_minor = kSblrEnvelopeMinor;
  SblrEnvelopeKind kind = SblrEnvelopeKind::unknown;
  std::vector<byte> payload;
  bool contains_sql_text = false;
  bool parser_resolved_names_to_uuids = false;
};

struct EngineColumnDescriptor {
  std::string stable_name;
  CanonicalTypeId type_id = CanonicalTypeId::unknown;
  bool nullable = true;
};

struct EngineResultShape {
  EngineResultCardinality cardinality = EngineResultCardinality::unknown;
  std::vector<EngineColumnDescriptor> columns;
  bool canonical_diagnostics = true;
  bool parser_package_shaping_required = true;
};

struct BoundEngineOperation {
  EngineOperationCode operation_code = EngineOperationCode::unknown;
  OperationAuthorityClass authority_class = OperationAuthorityClass::unknown;
  EngineResultShape result_shape;
  bool mutates_state = false;
  bool requires_engine_security_check = true;
};

struct EngineDispatchRequest {
  EngineContext context;
  SblrEnvelope envelope;
  BoundEngineOperation operation;
};

struct EngineApiValidationResult {
  Status status;
  DiagnosticRecord diagnostic;

  bool ok() const {
    return status.ok();
  }
};

struct EngineResultShapeResult {
  Status status;
  EngineResultShape result_shape;
  DiagnosticRecord diagnostic;

  bool ok() const {
    return status.ok();
  }
};

struct EngineDispatchRequestResult {
  Status status;
  EngineDispatchRequest request;
  DiagnosticRecord diagnostic;

  bool ok() const {
    return status.ok();
  }
};

const char* SblrEnvelopeKindName(SblrEnvelopeKind kind);
const char* EngineOperationCodeName(EngineOperationCode operation_code);
const char* OperationAuthorityClassName(OperationAuthorityClass authority_class);
const char* EngineResultCardinalityName(EngineResultCardinality cardinality);
EngineApiValidationResult ValidateEngineContext(const EngineContext& context);
EngineApiValidationResult ValidateSblrEnvelope(const SblrEnvelope& envelope);
EngineApiValidationResult ValidateEngineColumnDescriptor(const EngineColumnDescriptor& column);
EngineResultShapeResult MakeEngineResultShape(EngineResultCardinality cardinality,
                                              std::vector<EngineColumnDescriptor> columns);
EngineResultShapeResult ValidateEngineResultShape(const EngineResultShape& result_shape);
EngineApiValidationResult ValidateBoundEngineOperation(const BoundEngineOperation& operation);
EngineDispatchRequestResult MakeEngineDispatchRequest(EngineContext context,
                                                      SblrEnvelope envelope,
                                                      BoundEngineOperation operation);
EngineDispatchRequestResult ValidateEngineDispatchRequest(const EngineDispatchRequest& request);
DiagnosticRecord MakeEngineApiDiagnostic(Status status,
                                         std::string diagnostic_code,
                                         std::string message_key,
                                         std::string detail = {});

}  // namespace scratchbird::engine::internal_api
