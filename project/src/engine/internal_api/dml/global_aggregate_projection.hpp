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
#include <string_view>
#include <vector>

namespace scratchbird::engine::internal_api {

struct CrudRowVersionRecord;
struct MgaRelationStorageDescriptor;

// SEARCH_KEY: SB_ENGINE_GLOBAL_AGGREGATE_PROJECTION_ABI_V1
//
// This is a dialect-neutral, engine-internal operation ABI.  It deliberately
// does not carry a SQL spelling, compatibility profile, parser package, or public
// dialect-specific builtin registry.  A parser binds the canonical aggregate
// function UUID plus relation and column identities before lowering; the
// engine revalidates those identities against the exact persisted MGA relation
// descriptor and owns the visible-row scan.
enum class EngineGlobalAggregateOperation : std::uint8_t {
  count_star = 1,
  count_non_null_field = 2,
  count_distinct_field = 3,
  avg_field = 4,
  avg_distinct_field = 5,
};

// Bounded, dialect-neutral aggregate-input expressions. The direct-field
// value is the existing gag1 behavior and therefore remains the default. The
// checked multiply form is an engine-owned descriptor used by persisted
// global-aggregate views; it is not part of the gag1 carrier.
enum class EngineGlobalAggregateInputExpressionKind : std::uint8_t {
  direct_field = 0,
  int32_literal_times_int32_field_to_int64 = 1,
};

struct EngineGlobalAggregateInputExpression {
  EngineGlobalAggregateInputExpressionKind kind =
      EngineGlobalAggregateInputExpressionKind::direct_field;
  EngineTypedValue int32_literal;
  EngineDescriptor result_descriptor;
};

struct EngineGlobalAggregateFieldBinding {
  EngineUuid column_uuid;
  EngineDescriptor value_descriptor;
};

struct EngineGlobalAggregateProjection {
  EngineGlobalAggregateOperation operation =
      EngineGlobalAggregateOperation::count_star;
  EngineUuid aggregate_function_uuid;
  EngineGlobalAggregateFieldBinding source_field;
  EngineGlobalAggregateInputExpression input_expression;
  std::string output_alias;
  EngineDescriptor result_descriptor;
};

struct EngineGlobalAggregateProjectionEnvelope {
  EngineUuid relation_uuid;
  EngineUuid relation_descriptor_uuid;
  std::uint64_t relation_descriptor_generation = 0;
  std::vector<EngineGlobalAggregateProjection> outputs;
};

struct EngineBoundGlobalAggregateProjection {
  EngineGlobalAggregateOperation operation =
      EngineGlobalAggregateOperation::count_star;
  EngineUuid aggregate_function_uuid;
  // Retained engine-owned identity. Execution resolves this UUID and exact
  // descriptor against the persisted relation descriptor; it never rebinds by
  // a parser-presented or case-folded field name.
  EngineGlobalAggregateFieldBinding source_field;
  EngineGlobalAggregateInputExpression input_expression;
  std::string output_alias;
  EngineDescriptor result_descriptor;
};

struct EngineGlobalAggregateBindingResult {
  bool ok = false;
  EngineApiDiagnostic diagnostic;
  std::vector<EngineBoundGlobalAggregateProjection> outputs;
};

struct EngineGlobalAggregateExecutionResult {
  bool ok = false;
  EngineApiDiagnostic diagnostic;
  EngineResultShape result_shape;
  EngineApiU64 scanned_visible_row_count = 0;
};

// Stable output descriptor for every exact count projection in this ABI.
// COUNT always returns one non-NULL signed int64 value, including on an empty
// relation.  The descriptor has no catalog-object UUID because this internal
// ABI does not mint or select a public builtin identity.
EngineDescriptor EngineGlobalAggregateCountResultDescriptor();

// Canonical global aggregate-registry identity for COUNT. No independently
// copied or retired COUNT UUID is admitted by this ABI.
std::string_view EngineGlobalAggregateCountFunctionUuid();

// Stable nullable result descriptors for direct-relation AVG. Integer inputs
// yield int64 with division truncated toward zero; real64 inputs yield finite
// real64. Empty/all-NULL input uses the same descriptor with SQL NULL state.
EngineDescriptor EngineGlobalAggregateAvgIntegerResultDescriptor();
EngineDescriptor EngineGlobalAggregateAvgRealResultDescriptor();

// Exact descriptors for the bounded checked int32-literal x int32-field
// aggregate-input expression. The expression result is nullable because SQL
// NULL in the source field propagates to the expression and is then ignored by
// AVG.
EngineDescriptor EngineGlobalAggregateExpressionInt32LiteralDescriptor();
EngineDescriptor EngineGlobalAggregateExpressionInt64ResultDescriptor();

// Canonical global aggregate-registry identity for AVG. Independently copied
// or retired AVG UUIDs are rejected by the envelope validator.
std::string_view EngineGlobalAggregateAvgFunctionUuid();

// Performs operation/alias/result-descriptor validation without consulting
// catalog or row state.  Callers must run this before loading relation rows.
EngineApiDiagnostic ValidateGlobalAggregateProjectionEnvelope(
    const EngineGlobalAggregateProjectionEnvelope& envelope);

// Revalidates field UUID and descriptor identity against the exact persisted
// MGA relation descriptor.  This stage must complete before row materialization.
EngineGlobalAggregateBindingResult BindGlobalAggregateProjectionEnvelope(
    const EngineGlobalAggregateProjectionEnvelope& envelope,
    const MgaRelationStorageDescriptor& relation_descriptor);

// Re-resolves every retained field UUID+descriptor against the exact relation
// descriptor, then executes every output in one pass over the already
// MGA-visible rows. DISTINCT uses descriptor-aware canonical equality and
// fails closed when the bound type has no admitted canonicalizer.
EngineGlobalAggregateExecutionResult ExecuteGlobalAggregateProjection(
    const std::vector<EngineBoundGlobalAggregateProjection>& outputs,
    const MgaRelationStorageDescriptor& relation_descriptor,
    const std::vector<CrudRowVersionRecord>& visible_rows);

}  // namespace scratchbird::engine::internal_api
