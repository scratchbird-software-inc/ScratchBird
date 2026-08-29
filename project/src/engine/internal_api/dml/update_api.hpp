// Copyright (c) 2026 ScratchBird Software Inc.
//
// This Source Code Form is subject to the terms of the Mozilla Public
// License, v. 2.0. If a copy of the MPL was not distributed with this
// file, You can obtain one at https://mozilla.org/MPL/2.0/.
//
// SPDX-License-Identifier: MPL-2.0

#pragma once

#include "api_types.hpp"

#include <cstdint>
#include <string>
#include <vector>

namespace scratchbird::engine::internal_api {

// SEARCH_KEY: SB_ENGINE_INTERNAL_API_DML_UPDATE_API
struct EngineUpdateRowsRequest : EngineApiRequest {
  EngineObjectReference target_table;
  EnginePredicateEnvelope update_predicate;
  std::vector<std::pair<std::string, EngineTypedValue>> assignments;
  EngineApiU64 limit = 0;
  EngineApiU64 offset = 0;
};
struct EngineUpdateRowsResult : EngineApiResult {
  EngineApiU64 matched_count = 0;
  EngineApiU64 updated_count = 0;
};

// Private, engine-bound demand used only between the authenticated
// statement-context coordinator and the DML binder.  None of these display
// spellings or parser hints are execution authority: BindDmlUpdateRowsV1
// resolves them against the live relation descriptor and publishes only the
// opaque descriptor reference below.
struct EngineDmlUpdateAssignmentDemandV1 {
  std::uint32_t ordinal = 0;
  std::string target_column_spelling;
  std::string literal_spelling;
  std::string literal_type_spelling;
};

struct EngineDmlUpdateRowsBindingDemandV1 {
  std::string authenticated_statement_receipt_uuid;
  std::uint64_t structural_occurrence_id = 0;
  std::string target_relation_uuid_hint;
  std::vector<EngineDmlUpdateAssignmentDemandV1> assignments;
  std::string predicate_kind;
  std::string predicate_column_spelling;
  std::string predicate_literal_spelling;
  std::string predicate_literal_type_spelling;
};

struct EngineDmlUpdateRowsDescriptorRefV1 {
  std::string descriptor_uuid;
  std::uint64_t descriptor_generation = 0;
};

struct EngineDmlUpdateRowsBindResultV1 {
  bool ok = false;
  EngineDmlUpdateRowsDescriptorRefV1 descriptor_ref;
  EngineApiDiagnostic diagnostic;
};

struct EngineDmlUpdateRowsConsumeResultV1 {
  bool ok = false;
  bool immutable_replay = false;
  EngineUpdateRowsRequest request;
  EngineUpdateRowsResult prior_result;
  std::vector<std::uint8_t> canonical_result_bytes;
  EngineApiDiagnostic diagnostic;
};

struct EngineDmlUpdateRowsCompletionResultV1 {
  bool ok = false;
  std::vector<std::uint8_t> canonical_result_bytes;
  EngineApiDiagnostic diagnostic;
};

// Executes one already-bound UPDATE descriptor as one MGA statement.  The
// implementation owns the statement savepoint, rollback, publication
// barrier, and immutable replay transition; callers cannot split mutation
// from descriptor completion.
struct EngineDmlUpdateRowsExecuteResultV1 {
  bool ok = false;
  bool immutable_replay = false;
  EngineUpdateRowsResult update_result;
  std::vector<std::uint8_t> canonical_result_bytes;
  EngineApiDiagnostic diagnostic;
};

// Internal conformance hooks. They are not reachable from SBLR, SBPS, the
// parser worker, or the public ABI. The focused recovery suite uses them to
// emulate process loss at durable coordinator cut points.
enum class EngineDmlUpdateRowsTestFaultPointV1 : std::uint8_t {
  none = 0,
  after_durable_intent = 1,
  after_prepared_outcome = 2,
  after_publication_barrier = 3,
};

void SetDmlUpdateRowsTestFaultPointV1(
    EngineDmlUpdateRowsTestFaultPointV1 fault_point);
void ResetDmlUpdateRowsDescriptorRegistryForTestV1();

EngineDmlUpdateRowsBindResultV1 BindDmlUpdateRowsDescriptorV1(
    const EngineRequestContext& context,
    const EngineDmlUpdateRowsBindingDemandV1& demand);
EngineDmlUpdateRowsConsumeResultV1 ConsumeDmlUpdateRowsDescriptorV1(
    const EngineRequestContext& context,
    const EngineDmlUpdateRowsDescriptorRefV1& descriptor_ref,
    std::uint64_t structural_occurrence_id);
EngineDmlUpdateRowsCompletionResultV1 CompleteDmlUpdateRowsDescriptorV1(
    const EngineRequestContext& context,
    const EngineDmlUpdateRowsDescriptorRefV1& descriptor_ref,
    const EngineUpdateRowsResult& result);
EngineDmlUpdateRowsExecuteResultV1 ExecuteDmlUpdateRowsDescriptorV1(
    const EngineRequestContext& context,
    const EngineDmlUpdateRowsDescriptorRefV1& descriptor_ref,
    std::uint64_t structural_occurrence_id);
EngineUpdateRowsResult EngineUpdateRows(const EngineUpdateRowsRequest& request);

}  // namespace scratchbird::engine::internal_api
