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

inline constexpr std::string_view kFirebirdRelationProjectionViewMarkerV1 =
    "engine.relation_projection_view.v1";
inline constexpr std::string_view kFirebirdRelationProjectionViewCreatePacketV1 =
    "rpvc1";
inline constexpr std::string_view kFirebirdRelationProjectionViewSelectPacketV1 =
    "rpvs1";
inline constexpr std::string_view kFirebirdRelationProjectionViewMarkerV2 =
    "engine.relation_projection_view.v2";
inline constexpr std::string_view kFirebirdRelationProjectionViewCreatePacketV2 =
    "rpvc2";
inline constexpr std::string_view kFirebirdRelationProjectionViewDeletePacketV2 =
    "rpvd2";

inline constexpr std::string_view kFirebirdRelationProjectionViewInt32Kind =
    "scalar";
inline constexpr std::string_view kFirebirdRelationProjectionViewInt32Type =
    "int32";
inline constexpr std::string_view
    kFirebirdRelationProjectionViewInt32NotNullDescriptor =
        "canonical=int32;precision=32;scale=0;nullable=false";

struct FirebirdRelationProjectionViewCreateRoute {
  bool attempted{false};
  bool valid{false};
  std::string view_name;
  bool view_name_quoted{false};
  bool explicit_output_names{false};
  std::string source_relation;
  bool source_relation_quoted{false};
  std::string source_column;
  bool source_column_quoted{false};
  std::string source_output_name;
  std::string literal_output_name;
  std::int32_t literal_value{0};

  [[nodiscard]] bool recognized() const { return attempted && valid; }
};

struct FirebirdBoundRelationProjectionViewCreate {
  bool accepted{false};
  FirebirdRelationProjectionViewCreateRoute route;
  std::string schema_uuid;
  std::string relation_uuid;
  std::string relation_descriptor_uuid;
  std::uint64_t relation_descriptor_generation{0};
  std::uint64_t validated_resource_epoch{0};
  ipc::PublicRelationColumnDescriptor source_column;
  ipc::MessageVectorSet messages;
};

struct FirebirdRelationProjectionViewSelectRoute {
  bool attempted{false};
  bool valid{false};
  std::string view_name;
  bool view_name_quoted{false};

  [[nodiscard]] bool recognized() const { return attempted && valid; }
};

struct FirebirdRelationProjectionViewOutputDescriptor {
  std::uint32_t ordinal{0};
  std::string name;
  std::string output_column_uuid;
  std::string type_descriptor_uuid;
  std::string descriptor_kind;
  std::string canonical_type_name;
  std::string encoded_type_descriptor;
  bool nullable{true};
};

struct FirebirdBoundRelationProjectionViewSelect {
  bool accepted{false};
  FirebirdRelationProjectionViewSelectRoute route;
  std::string view_uuid;
  std::string view_descriptor_uuid;
  std::uint64_t view_descriptor_generation{0};
  std::vector<FirebirdRelationProjectionViewOutputDescriptor> outputs;
  std::string semantic_transport;
  ipc::MessageVectorSet messages;
};

// V2 is deliberately a separate bounded surface.  It neither calls nor
// delegates to the V1 classifier: each Firebird parser route remains
// standalone and fail-closed.
struct FirebirdRelationProjectionViewCreateV2Route {
  bool attempted{false};
  bool valid{false};
  std::string view_name;
  bool view_name_quoted{false};
  bool explicit_output_name{false};
  std::string output_name;
  std::string source_relation;
  bool source_relation_quoted{false};
  std::string source_column;
  bool source_column_quoted{false};

  [[nodiscard]] bool recognized() const { return attempted && valid; }
};

struct FirebirdBoundRelationProjectionViewCreateV2 {
  bool accepted{false};
  FirebirdRelationProjectionViewCreateV2Route route;
  std::string schema_uuid;
  std::string relation_uuid;
  std::string relation_descriptor_uuid;
  std::uint64_t relation_descriptor_generation{0};
  std::uint64_t validated_resource_epoch{0};
  ipc::PublicRelationColumnDescriptor source_column;
  ipc::MessageVectorSet messages;
};

struct FirebirdRelationProjectionViewDeleteV2Route {
  bool attempted{false};
  bool valid{false};
  std::string view_name;
  bool view_name_quoted{false};
  std::string output_name;
  bool output_name_quoted{false};
  std::int32_t predicate_value{0};

  [[nodiscard]] bool recognized() const { return attempted && valid; }
};

struct FirebirdBoundRelationProjectionViewDeleteV2 {
  bool accepted{false};
  FirebirdRelationProjectionViewDeleteV2Route route;
  std::string view_uuid;
  std::string view_descriptor_uuid;
  std::uint64_t view_descriptor_generation{0};
  FirebirdRelationProjectionViewOutputDescriptor output;
  std::string semantic_transport;
  ipc::MessageVectorSet messages;
};

// Recognizes only the first SQL-free durable row-view surface: one direct
// int32 source column followed by one int32 literal.  Other CREATE VIEW shapes
// remain outside this bounded route so existing unrelated Firebird behavior is
// neither silently promoted nor delegated to another parser family.
FirebirdRelationProjectionViewCreateRoute
ParseFirebirdRelationProjectionViewCreateRoute(std::string_view firebird_sql);

FirebirdBoundRelationProjectionViewCreate
BindFirebirdRelationProjectionViewCreate(
    const FirebirdRelationProjectionViewCreateRoute& route,
    const ipc::PublicNameResolutionResult& resolved_schema,
    const ipc::PublicNameResolutionResult& resolved_relation);

std::string EncodeFirebirdRelationProjectionViewCreateEnvelope(
    const FirebirdBoundRelationProjectionViewCreate& binding);

FirebirdRelationProjectionViewSelectRoute
ParseFirebirdRelationProjectionViewSelectRoute(std::string_view firebird_sql);

FirebirdBoundRelationProjectionViewSelect
BindFirebirdRelationProjectionViewSelect(
    const FirebirdRelationProjectionViewSelectRoute& route,
    const ipc::PublicNameResolutionResult& resolved_view);

std::string EncodeFirebirdRelationProjectionViewSelectEnvelope(
    const FirebirdBoundRelationProjectionViewSelect& binding);

// Recognizes exactly CREATE VIEW v [(out)] AS SELECT direct_int_column
// FROM one_table.  Names and descriptors are bound only from the engine's
// public V3 resolution contract before SQL-free rpvc2 is emitted.
FirebirdRelationProjectionViewCreateV2Route
ParseFirebirdRelationProjectionViewCreateV2Route(
    std::string_view firebird_sql);

FirebirdBoundRelationProjectionViewCreateV2
BindFirebirdRelationProjectionViewCreateV2(
    const FirebirdRelationProjectionViewCreateV2Route& route,
    const ipc::PublicNameResolutionResult& resolved_schema,
    const ipc::PublicNameResolutionResult& resolved_relation);

std::string EncodeFirebirdRelationProjectionViewCreateV2Envelope(
    const FirebirdBoundRelationProjectionViewCreateV2& binding);

// Recognizes exactly DELETE FROM v WHERE out = int32_literal.  Binding uses
// only the public, source-opaque rpvd2 semantic descriptor.  The parser never
// expands the view or evaluates/deletes base rows.
FirebirdRelationProjectionViewDeleteV2Route
ParseFirebirdRelationProjectionViewDeleteV2Route(
    std::string_view firebird_sql);

FirebirdBoundRelationProjectionViewDeleteV2
BindFirebirdRelationProjectionViewDeleteV2(
    const FirebirdRelationProjectionViewDeleteV2Route& route,
    const ipc::PublicNameResolutionResult& resolved_view);

std::string EncodeFirebirdRelationProjectionViewDeleteV2Envelope(
    const FirebirdBoundRelationProjectionViewDeleteV2& binding);

}  // namespace scratchbird::parser::firebird
