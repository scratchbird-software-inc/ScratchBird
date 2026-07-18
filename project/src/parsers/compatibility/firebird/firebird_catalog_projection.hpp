// Copyright (c) 2026 ScratchBird Software Inc.
//
// This Source Code Form is subject to the terms of the Mozilla Public
// License, v. 2.0. If a copy of the MPL was not distributed with this
// file, You can obtain one at https://mozilla.org/MPL/2.0/.
//
// SPDX-License-Identifier: MPL-2.0

#pragma once

#include <cstdint>
#include <optional>
#include <string>
#include <string_view>
#include <vector>

namespace scratchbird::parser::firebird {

inline constexpr std::string_view kCatalogRelationDescriptorProjectionV1 =
    "engine.catalog.relation_descriptor_projection.v1";
inline constexpr std::string_view kCatalogRelationTypeNameDescriptorV1 =
    "engine.catalog.relation_type_name.v1";
// Neutral engine variants selected by the Firebird-owned fingerprints below.
// The engine never sees the Firebird object names or catalog type mappings.
inline constexpr std::string_view kFirebirdFieldsInfoTypeInventoryV1 =
    "relation.type_inventory.v1";
inline constexpr std::string_view kFirebirdFieldsInfoCharsetInventoryV1 =
    "relation.charset_inventory.v1";

enum class FirebirdCatalogProjectionRouteKind {
  kUnsupported,
  kCreateTypeNameFunction,
  kCreateFieldsInfoTypeView,
  kCreateFieldsInfoCharsetView,
  kSelectFieldsInfoUnbound,
  kSelectFieldsInfoType,
  kSelectFieldsInfoCharset,
};

// A bounded semantic route, never a stored SQL representation.  Names in this
// value are fixed by the recognized Firebird regression shape.  The optional
// message is a SELECT literal presentation value, not catalog authority.
struct FirebirdCatalogProjectionRoute {
  FirebirdCatalogProjectionRouteKind kind{
      FirebirdCatalogProjectionRouteKind::kUnsupported};
  std::string function_name;
  std::string view_name;
  std::string source_relation_name;
  std::string semantic_variant;
  std::optional<std::string> leading_message;

  [[nodiscard]] bool recognized() const {
    return kind != FirebirdCatalogProjectionRouteKind::kUnsupported;
  }
  [[nodiscard]] bool is_select() const {
    return kind ==
               FirebirdCatalogProjectionRouteKind::kSelectFieldsInfoUnbound ||
           kind == FirebirdCatalogProjectionRouteKind::kSelectFieldsInfoType ||
           kind ==
               FirebirdCatalogProjectionRouteKind::kSelectFieldsInfoCharset;
  }
  [[nodiscard]] bool is_ddl() const { return recognized() && !is_select(); }
};

FirebirdCatalogProjectionRoute ParseFirebirdCatalogProjectionRoute(
    std::string_view firebird_sql);

// Refines an unbound SELECT exclusively from the exact engine-owned view
// descriptor returned by transaction-routed name resolution. Unknown,
// malformed, or presentation-incompatible details fail closed.
std::optional<FirebirdCatalogProjectionRoute>
BindFirebirdCatalogProjectionViewVariant(
    const FirebirdCatalogProjectionRoute& route,
    std::string_view engine_resolution_detail);

// A bound SELECT variant is valid only for the exact transaction that issued
// the view descriptor. Rebinding on a different engine transaction must first
// discard that semantic refinement, while retaining only the parser-owned
// Firebird presentation shape, then resolve the persisted view again.
FirebirdCatalogProjectionRoute FirebirdCatalogProjectionRouteForExactRebind(
    const FirebirdCatalogProjectionRoute& route);

std::string_view FirebirdCatalogProjectionRouteName(
    FirebirdCatalogProjectionRouteKind kind);

// These encoders accept only engine-issued UUIDs/descriptors supplied by the
// exact transaction binder.  They never accept or embed source SQL.
std::string EncodeFirebirdCatalogProjectionFunctionEnvelope(
    const FirebirdCatalogProjectionRoute& route,
    std::string_view schema_uuid);
std::string EncodeFirebirdCatalogProjectionViewEnvelope(
    const FirebirdCatalogProjectionRoute& route,
    std::string_view schema_uuid,
    std::string_view function_uuid);
std::string EncodeFirebirdCatalogProjectionSelectEnvelope(
    const FirebirdCatalogProjectionRoute& route,
    std::string_view view_uuid,
    std::string_view relation_uuid,
    std::string_view relation_descriptor_uuid,
    std::uint64_t relation_descriptor_generation);

struct FirebirdCatalogProjectionRenderedRow {
  std::vector<std::optional<std::string>> cells;
};

struct FirebirdCatalogProjectionRenderResult {
  bool ok{false};
  std::string diagnostic;
  std::vector<FirebirdCatalogProjectionRenderedRow> rows;
};

// Firebird SQLDA is a parser/wire presentation contract.  These values are
// deliberately derived only after the engine has bound the neutral persisted
// view variant; they do not describe or authorize engine catalog storage.
// The profile defaults match the UTF8 database and NONE statement charset
// used by the Firebird 5.0.4 relation-descriptor regression oracle.
struct FirebirdCatalogProjectionSqlDaProfile {
  std::uint16_t database_default_charset_id{4};
  std::uint16_t database_default_charset_max_bytes_per_character{4};
  std::uint16_t statement_charset_id{0};
  std::uint16_t statement_charset_max_bytes_per_character{1};
};

struct FirebirdCatalogProjectionSqlDaColumn {
  std::string field_name;
  std::string alias_name;
  std::string relation_name;
  std::string owner_name;
  // Base Firebird SQL type. The nullable flag is added as the low SQLDA bit
  // only when the descriptor is written to the Firebird wire protocol.
  std::uint32_t sql_type{0};
  // Legacy isc_info_sql_sub_type carries the text type/charset id for TEXT
  // and VARYING. The newer IMessageMetadata API exposes character set as a
  // separate value, retained below for fetch-message BLR construction.
  std::int32_t subtype{0};
  std::int32_t scale{0};
  std::uint32_t length{0};
  std::uint16_t charset_id{0};
  bool nullable{false};

  [[nodiscard]] std::uint32_t wire_sql_type() const {
    return sql_type + (nullable ? 1U : 0U);
  }
};

struct FirebirdCatalogProjectionSqlDaResult {
  bool ok{false};
  std::string diagnostic;
  std::vector<FirebirdCatalogProjectionSqlDaColumn> columns;
};

struct FirebirdCatalogProjectionFetchBlr {
  bool ok{false};
  std::string diagnostic;
  std::uint32_t message_length{0};
  std::vector<std::uint8_t> bytes;
};

// Produces the exact Firebird 5.0.4 SQLDA presentation for the two recognized
// V_FIELDS_INFO variants. An unbound SELECT, DDL route, foreign variant, or
// invalid charset profile fails closed.
FirebirdCatalogProjectionSqlDaResult
DescribeFirebirdCatalogProjectionSqlDa(
    const FirebirdCatalogProjectionRoute& route,
    const FirebirdCatalogProjectionSqlDaProfile& profile = {});

// Reproduces Firebird 5.0.4 BlrFromMessage for the bounded catalog projection
// datatypes. Every SQLDA value receives a SHORT null-indicator slot, including
// values whose SQLDA nullable bit is clear.
FirebirdCatalogProjectionFetchBlr BuildFirebirdCatalogProjectionFetchBlr(
    const std::vector<FirebirdCatalogProjectionSqlDaColumn>& columns,
    std::uint32_t sql_dialect = 3);

// Converts neutral engine catalog rows to the exact Firebird view projection.
// Firebird type names and numeric charset/collation ids are presentation-only
// outputs and are deliberately absent from the engine envelope and row shape.
FirebirdCatalogProjectionRenderResult RenderFirebirdCatalogProjectionPayload(
    const FirebirdCatalogProjectionRoute& route,
    std::string_view neutral_payload,
    std::string_view expected_relation_uuid = {},
    std::string_view expected_descriptor_uuid = {},
    std::uint64_t expected_descriptor_generation = 0);

}  // namespace scratchbird::parser::firebird
