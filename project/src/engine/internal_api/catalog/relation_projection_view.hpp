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

struct CrudRowVersionRecord;
struct EngineDeleteRowsRequest;
struct EngineSelectRowsRequest;
struct MgaRelationStorageDescriptor;

// SEARCH_KEY: SB_ENGINE_RELATION_PROJECTION_VIEW_DESCRIPTOR_V1
//
// SQL-free, dialect-neutral descriptors for the bounded V1 read projection
// and V2 one-column updatable projection. Parser family, SQL text, row storage,
// and transaction finality are deliberately absent. The engine owns every
// view/output/expression identity and retains the exact source descriptor
// dependency needed for fail-closed expansion.
inline constexpr const char* kEngineRelationProjectionViewMarkerV1 =
    "engine.relation_projection_view.v1";
inline constexpr const char* kEngineRelationProjectionViewMarkerV2 =
    "engine.relation_projection_view.v2";
inline constexpr const char* kEngineRelationProjectionSourceColumnV1 =
    "source_column";
inline constexpr const char* kEngineRelationProjectionTypedInt32LiteralV1 =
    "typed_int32_literal";

enum class EngineRelationProjectionExpressionKind : std::uint8_t {
  source_column = 1,
  typed_int32_literal = 2,
};

// The type descriptor identity is intentionally separate from the output
// column identity.  A view output owns both identities; neither is overloaded
// as the expression identity.
struct EngineRelationProjectionTypeDescriptor {
  EngineUuid type_descriptor_uuid;
  std::string descriptor_kind;
  std::string canonical_type_name;
  std::string encoded_descriptor;
};

struct EngineRelationProjectionViewOutput {
  std::uint32_t ordinal = 0;
  EngineUuid output_column_uuid;
  EngineUuid expression_uuid;
  std::string output_name;
  EngineRelationProjectionTypeDescriptor output_type;
  bool nullable = true;
  EngineRelationProjectionExpressionKind expression_kind =
      EngineRelationProjectionExpressionKind::source_column;

  // Present only for source_column.
  EngineUuid source_column_uuid;
  EngineUuid source_column_type_descriptor_uuid;

  // Present only for typed_int32_literal.
  std::int32_t literal_int32 = 0;
};

struct EngineRelationProjectionViewDescriptor {
  bool present = false;
  std::string marker = kEngineRelationProjectionViewMarkerV1;
  EngineUuid view_uuid;
  EngineUuid view_descriptor_uuid;
  std::uint64_t view_descriptor_generation = 0;
  EngineUuid source_relation_uuid;
  EngineUuid source_relation_descriptor_uuid;
  std::uint64_t source_relation_descriptor_generation = 0;
  std::uint64_t source_resource_epoch = 0;
  std::vector<EngineRelationProjectionViewOutput> outputs;
  EngineApiDiagnostic diagnostic;
};

struct EngineRelationProjectionViewCreatePreparation {
  bool ok = false;
  EngineApiDiagnostic diagnostic;
  EngineRelationProjectionViewDescriptor descriptor;
  std::vector<std::string> canonical_persisted_options;
};

// Public name resolution publishes only this bounded output contract.  Source
// relation/column identities, expression identities/kinds, and literal values
// remain engine-side.
struct EngineRelationProjectionViewSemanticOutput {
  std::uint32_t ordinal = 0;
  EngineUuid output_column_uuid;
  std::string output_name;
  EngineRelationProjectionTypeDescriptor output_type;
  bool nullable = true;
};

struct EngineRelationProjectionViewSelectEnvelope {
  bool present = false;
  std::string marker;
  EngineUuid view_uuid;
  EngineUuid view_descriptor_uuid;
  std::uint64_t view_descriptor_generation = 0;
  std::vector<EngineRelationProjectionViewSemanticOutput> outputs;
};

// SQL-free public semantic envelope for a bounded V2 DELETE through an
// updatable one-column projection view. Hidden source relation/column
// identities remain durable engine state and are deliberately absent here.
struct EngineRelationProjectionViewDeleteEnvelope {
  bool present = false;
  std::string marker;
  EngineUuid view_descriptor_uuid;
  std::uint64_t view_descriptor_generation = 0;
  std::vector<EngineRelationProjectionViewSemanticOutput> outputs;
};

// Engine-only expansion envelope.  It is populated from the durable view
// descriptor after semantic-descriptor revalidation and is evaluated over one
// ordinary MGA-visible relation scan.
struct EngineRelationProjectionEnvelope {
  std::string marker = kEngineRelationProjectionViewMarkerV1;
  EngineUuid relation_uuid;
  EngineUuid relation_descriptor_uuid;
  std::uint64_t relation_descriptor_generation = 0;
  std::uint64_t source_resource_epoch = 0;
  std::vector<EngineRelationProjectionViewOutput> outputs;
};

struct EngineBoundRelationProjectionOutput {
  EngineRelationProjectionViewOutput output;
  std::string source_column_name_key;
};

struct EngineRelationProjectionBindingResult {
  bool ok = false;
  EngineApiDiagnostic diagnostic;
  std::vector<EngineBoundRelationProjectionOutput> outputs;
};

struct EngineRelationProjectionExecutionResult {
  bool ok = false;
  EngineApiDiagnostic diagnostic;
  EngineResultShape result_shape;
  EngineApiU64 scanned_visible_row_count = 0;
};

// Canonical parser-to-engine input descriptor for the V1 typed literal.  Its
// UUID is empty because the engine allocates the persisted output type
// descriptor identity during CREATE VIEW.
EngineDescriptor EngineRelationProjectionInt32LiteralInputDescriptor();

EngineDescriptor EngineRelationProjectionOutputTypeDescriptor(
    const EngineRelationProjectionTypeDescriptor& descriptor);

bool IsEngineRelationProjectionViewCreateRequest(
    const EngineApiRequest& request);

EngineRelationProjectionViewCreatePreparation
PrepareEngineRelationProjectionViewCreate(const EngineApiRequest& request);

// Loads the descriptor visible to the exact selected MGA transaction.
// Ordinary views are classified with present=false; malformed supported-family
// payloads fail closed.
EngineRelationProjectionViewDescriptor DescribeEngineRelationProjectionView(
    const EngineRequestContext& context,
    const std::string& view_uuid);

EngineDescriptor EngineRelationProjectionViewSemanticDescriptor(
    const EngineRelationProjectionViewDescriptor& descriptor);

std::vector<EngineRelationProjectionViewSemanticOutput>
EngineRelationProjectionViewSemanticOutputs(
    const EngineRelationProjectionViewDescriptor& descriptor);

bool IsEngineRelationProjectionViewSelectRequest(
    const EngineSelectRowsRequest& request);

EngineApiDiagnostic ExpandEngineRelationProjectionViewSelect(
    const EngineSelectRowsRequest& request,
    EngineSelectRowsRequest* expanded,
    EngineRelationProjectionViewDescriptor* descriptor);

bool IsEngineRelationProjectionViewDeleteRequest(
    const EngineDeleteRowsRequest& request);

EngineApiDiagnostic ExpandEngineRelationProjectionViewDelete(
    const EngineDeleteRowsRequest& request,
    EngineDeleteRowsRequest* expanded,
    EngineRelationProjectionViewDescriptor* descriptor);

EngineApiDiagnostic ValidateEngineRelationProjectionEnvelope(
    const EngineRelationProjectionEnvelope& envelope);

EngineRelationProjectionBindingResult BindEngineRelationProjectionEnvelope(
    const EngineRelationProjectionEnvelope& envelope,
    const MgaRelationStorageDescriptor& relation_descriptor);

EngineRelationProjectionExecutionResult ExecuteEngineRelationProjection(
    const std::vector<EngineBoundRelationProjectionOutput>& outputs,
    const MgaRelationStorageDescriptor& relation_descriptor,
    std::uint64_t source_resource_epoch,
    const std::vector<CrudRowVersionRecord>& visible_rows);

}  // namespace scratchbird::engine::internal_api
