// Copyright (c) 2026 ScratchBird Software Inc.
//
// This Source Code Form is subject to the terms of the Mozilla Public
// License, v. 2.0. If a copy of the MPL was not distributed with this
// file, You can obtain one at https://mozilla.org/MPL/2.0/.
//
// SPDX-License-Identifier: MPL-2.0

#pragma once

#include "api_types.hpp"
#include "catalog/relation_projection_view.hpp"
#include <array>

namespace scratchbird::engine::internal_api {

// SEARCH_KEY: SB_ENGINE_INTERNAL_API_DML_DELETE_API
struct EngineDeleteRowsRequest : EngineApiRequest {
  EngineObjectReference target_table;
  EnginePredicateEnvelope delete_predicate;
  EngineRelationProjectionViewDeleteEnvelope relation_projection_view;
  std::string delete_surface_variant = "delete";
  std::string batch_on_column;
  EngineApiU64 batch_limit_rows = 0;
  EngineApiU64 limit = 0;
  EngineApiU64 offset = 0;
  std::string series_name;
  bool tombstone_only = true;
};
struct EngineDeleteRowsResult : EngineApiResult {
  EngineApiU64 matched_count = 0;
  EngineApiU64 deleted_count = 0;
};

// Private authenticated-coordinator demand. Spellings are parser input, not
// datatype, relation or execution authority. The DELETE binder resolves them
// against the live engine-owned descriptor; no UPDATE assignment is implied.
struct EngineDmlDeleteRowsBindingDemandV1 {
  std::string authenticated_statement_receipt_uuid;
  std::uint64_t structural_occurrence_id = 0;
  std::string target_relation_uuid_hint;
  std::string predicate_kind;
  std::string predicate_column_spelling;
  std::string predicate_literal_spelling;
  std::string predicate_literal_type_spelling;
};
struct EngineDmlDeleteRowsDescriptorRefV1 {
  std::array<std::uint8_t, 16> descriptor_uuid{};
  std::uint64_t descriptor_generation = 0;
};
struct EngineDmlDeleteRowsBindResultV1 {
  bool ok = false;
  EngineDmlDeleteRowsDescriptorRefV1 descriptor_ref;
  EngineApiDiagnostic diagnostic;
};
struct EngineDmlDeleteRowsExecuteResultV1 {
  bool ok = false;
  bool immutable_replay = false;
  EngineDeleteRowsResult delete_result;
  std::vector<std::uint8_t> canonical_result_bytes;
  EngineApiDiagnostic diagnostic;
};
EngineDmlDeleteRowsBindResultV1 BindDmlDeleteRowsDescriptorV1(
    const EngineRequestContext&, const EngineDmlDeleteRowsBindingDemandV1&);
EngineDmlDeleteRowsExecuteResultV1 ExecuteDmlDeleteRowsDescriptorV1(
    const EngineRequestContext&, const EngineDmlDeleteRowsDescriptorRefV1&,
    std::uint64_t structural_occurrence_id);
enum class EngineDmlDeleteRowsTestFaultPointV1 : std::uint8_t {
  none, after_native_marker, after_durable_intent, after_prepared_outcome, after_publication_barrier
};
// Private component fault hook, never a parser/SBPS/public ABI option.
void SetDmlDeleteRowsTestFaultPointV1(EngineDmlDeleteRowsTestFaultPointV1);
EngineDeleteRowsResult EngineDeleteRows(const EngineDeleteRowsRequest& request);

}  // namespace scratchbird::engine::internal_api
