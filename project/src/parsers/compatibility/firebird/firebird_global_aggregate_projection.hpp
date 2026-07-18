// Copyright (c) 2026 ScratchBird Software Inc.
//
// This Source Code Form is subject to the terms of the Mozilla Public
// License, v. 2.0. If a copy of the MPL was not distributed with this
// file, You can obtain one at https://mozilla.org/MPL/2.0/.
//
// SPDX-License-Identifier: MPL-2.0

#pragma once

#include "parser_server_client.hpp"

#include <cstdint>
#include <string>
#include <string_view>
#include <vector>

namespace scratchbird::parser::firebird {

// Canonical aggregate-registry authority for COUNT.  This is deliberately not
// either the retired conflicting `count` identity or the separate legacy
// `sb.aggregate.count` surface identity.  Every admitted projection item is
// bound to this one aggregate UUID before neutral lowering.
inline constexpr std::string_view kFirebirdCanonicalCountAggregateUuid =
    "019de5fc-2400-784a-9aec-371f8b95b7ea";

inline constexpr std::string_view kFirebirdCanonicalAvgAggregateUuid =
    "019de5fc-2400-78ac-b50c-45b832831004";

// Neutral bounded-view markers are transport contracts, not parser-family
// identities.  The Firebird parser emits them without delegating parsing,
// binding, or execution to any sibling parser.
inline constexpr std::string_view kFirebirdGlobalAggregateViewMarkerV1 =
    "engine.global_aggregate_view.v1";
inline constexpr std::string_view
    kFirebirdGlobalAggregateViewInt32MultiplyV1 =
        "int32_literal_times_int32_field_to_int64";

enum class FirebirdGlobalCountProjectionOperation : std::uint8_t {
  kUnsupported = 0,
  kCountStar = 1,
  kCountNonNullField = 2,
  kCountDistinctField = 3,
};

struct FirebirdGlobalCountProjectionItem {
  FirebirdGlobalCountProjectionOperation operation{
      FirebirdGlobalCountProjectionOperation::kUnsupported};
  std::string aggregate_function_uuid;
  std::string source_column;
  bool source_column_quoted{false};
  std::string output_alias;
};

// Exact first executable tranche: one direct relation and, in order,
// COUNT(*), COUNT(field), COUNT(DISTINCT field), each with an explicit alias.
// `attempted` distinguishes an unsupported multi-COUNT shape from an unrelated
// SELECT so the caller can refuse it instead of falling through to another
// compatibility implementation.
struct FirebirdGlobalCountProjectionRoute {
  bool attempted{false};
  bool valid{false};
  std::string source_relation;
  bool source_relation_quoted{false};
  std::vector<FirebirdGlobalCountProjectionItem> items;

  [[nodiscard]] bool recognized() const {
    return valid && items.size() == 3;
  }
};

struct FirebirdBoundGlobalCountProjection {
  bool accepted{false};
  FirebirdGlobalCountProjectionRoute route;
  std::string relation_uuid;
  std::string relation_descriptor_uuid;
  std::uint64_t relation_descriptor_generation{0};
  std::uint64_t validated_resource_epoch{0};
  ipc::PublicRelationColumnDescriptor source_column;
  ipc::MessageVectorSet messages;
};

enum class FirebirdGlobalAvgProjectionOperation : std::uint8_t {
  kUnsupported = 0,
  kAvgField = 4,
  kAvgDistinctField = 5,
};

enum class FirebirdGlobalAvgResultKind : std::uint8_t {
  kUnsupported = 0,
  kNullableInt64 = 1,
  kNullableReal64 = 2,
};

struct FirebirdGlobalAvgProjectionItem {
  FirebirdGlobalAvgProjectionOperation operation{
      FirebirdGlobalAvgProjectionOperation::kUnsupported};
  std::string aggregate_function_uuid;
  std::string source_column;
  bool source_column_quoted{false};
  std::string output_alias;
};

// Exact direct-relation tranche: SELECT AVG([DISTINCT] field) [AS alias]
// FROM relation. The parser retains binding and presentation state only; the
// neutral engine owns typed accumulation, distinctness, NULL, and final value.
struct FirebirdGlobalAvgProjectionRoute {
  bool attempted{false};
  bool valid{false};
  std::string source_relation;
  bool source_relation_quoted{false};
  FirebirdGlobalAvgProjectionItem item;

  [[nodiscard]] bool recognized() const {
    return valid &&
           (item.operation == FirebirdGlobalAvgProjectionOperation::kAvgField ||
            item.operation ==
                FirebirdGlobalAvgProjectionOperation::kAvgDistinctField);
  }
};

struct FirebirdBoundGlobalAvgProjection {
  bool accepted{false};
  FirebirdGlobalAvgProjectionRoute route;
  std::string relation_uuid;
  std::string relation_descriptor_uuid;
  std::uint64_t relation_descriptor_generation{0};
  std::uint64_t validated_resource_epoch{0};
  ipc::PublicRelationColumnDescriptor source_column;
  FirebirdGlobalAvgResultKind result_kind{
      FirebirdGlobalAvgResultKind::kUnsupported};
  ipc::MessageVectorSet messages;
};

// Exact Firebird QA AVG test_06 view definition:
// CREATE [OR ALTER] VIEW name AS
// SELECT AVG(int32_literal * int32_field) AS alias FROM relation.
struct FirebirdGlobalAggregateViewCreateRoute {
  bool attempted{false};
  bool valid{false};
  bool create_or_alter{false};
  std::string view_name;
  bool view_name_quoted{false};
  std::string source_relation;
  bool source_relation_quoted{false};
  std::string source_column;
  bool source_column_quoted{false};
  std::int32_t int32_literal{0};
  std::string result_alias;

  [[nodiscard]] bool recognized() const { return attempted && valid; }
};

struct FirebirdBoundGlobalAggregateViewCreate {
  bool accepted{false};
  FirebirdGlobalAggregateViewCreateRoute route;
  std::string schema_uuid;
  std::string relation_uuid;
  std::string relation_descriptor_uuid;
  std::uint64_t relation_descriptor_generation{0};
  ipc::PublicRelationColumnDescriptor source_column;
  ipc::MessageVectorSet messages;
};

// SELECT * FROM view is parsed independently.  Binding promotes it to this
// bounded route only when neutral name resolution returns the exact
// engine-owned global-aggregate semantic descriptor.
struct FirebirdGlobalAggregateViewSelectRoute {
  bool attempted{false};
  bool valid{false};
  std::string view_name;
  bool view_name_quoted{false};

  [[nodiscard]] bool recognized() const { return attempted && valid; }
};

struct FirebirdBoundGlobalAggregateViewSelect {
  bool accepted{false};
  FirebirdGlobalAggregateViewSelectRoute route;
  std::string view_uuid;
  std::string view_descriptor_uuid;
  std::uint64_t view_descriptor_generation{0};
  std::string result_alias;
  FirebirdGlobalAvgResultKind result_kind{
      FirebirdGlobalAvgResultKind::kUnsupported};
  std::string semantic_transport;
  ipc::MessageVectorSet messages;
};

FirebirdGlobalCountProjectionRoute ParseFirebirdGlobalCountProjectionRoute(
    std::string_view firebird_sql);

// Binds only against the complete engine-owned V3 relation descriptor returned
// on the exact selected MGA transaction.  No parser overlay, generated UUID,
// sibling parser registry, or worker row state participates.
FirebirdBoundGlobalCountProjection BindFirebirdGlobalCountProjection(
    const FirebirdGlobalCountProjectionRoute& route,
    const ipc::PublicNameResolutionResult& resolved_relation);

// Lowers a successfully bound route to the dialect-neutral global aggregate
// projection transport.  The payload carries no SQL text and no result value.
std::string EncodeFirebirdGlobalCountProjectionEnvelope(
    const FirebirdBoundGlobalCountProjection& binding);

std::string_view FirebirdGlobalCountProjectionOperationName(
    FirebirdGlobalCountProjectionOperation operation);

FirebirdGlobalAvgProjectionRoute ParseFirebirdGlobalAvgProjectionRoute(
    std::string_view firebird_sql);

FirebirdBoundGlobalAvgProjection BindFirebirdGlobalAvgProjection(
    const FirebirdGlobalAvgProjectionRoute& route,
    const ipc::PublicNameResolutionResult& resolved_relation);

std::string EncodeFirebirdGlobalAvgProjectionEnvelope(
    const FirebirdBoundGlobalAvgProjection& binding);

std::string_view FirebirdGlobalAvgProjectionOperationName(
    FirebirdGlobalAvgProjectionOperation operation);

FirebirdGlobalAggregateViewCreateRoute
ParseFirebirdGlobalAggregateViewCreateRoute(std::string_view firebird_sql);

FirebirdBoundGlobalAggregateViewCreate BindFirebirdGlobalAggregateViewCreate(
    const FirebirdGlobalAggregateViewCreateRoute& route,
    const ipc::PublicNameResolutionResult& resolved_schema,
    const ipc::PublicNameResolutionResult& resolved_relation);

std::string EncodeFirebirdGlobalAggregateViewCreateEnvelope(
    const FirebirdBoundGlobalAggregateViewCreate& binding);

FirebirdGlobalAggregateViewSelectRoute
ParseFirebirdGlobalAggregateViewSelectRoute(std::string_view firebird_sql);

FirebirdBoundGlobalAggregateViewSelect BindFirebirdGlobalAggregateViewSelect(
    const FirebirdGlobalAggregateViewSelectRoute& route,
    const ipc::PublicNameResolutionResult& resolved_view);

std::string EncodeFirebirdGlobalAggregateViewSelectEnvelope(
    const FirebirdBoundGlobalAggregateViewSelect& binding);

}  // namespace scratchbird::parser::firebird
