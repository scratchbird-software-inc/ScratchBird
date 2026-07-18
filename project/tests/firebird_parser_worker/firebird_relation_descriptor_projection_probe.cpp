// Copyright (c) 2026 ScratchBird Software Inc.
//
// This Source Code Form is subject to the terms of the Mozilla Public
// License, v. 2.0. If a copy of the MPL was not distributed with this
// file, You can obtain one at https://mozilla.org/MPL/2.0/.
//
// SPDX-License-Identifier: MPL-2.0

#include "firebird_worker_session.hpp"
#include "firebird_catalog_projection.hpp"

#include <cstdint>
#include <cstdlib>
#include <iostream>
#include <string>
#include <string_view>
#include <vector>

namespace {

bool Require(bool condition, std::string_view message) {
  if (condition) return true;
  std::cerr << message << '\n';
  return false;
}

bool RequireSqlDa(
    const scratchbird::parser::firebird::
        FirebirdCatalogProjectionSqlDaResult& actual,
    const std::vector<scratchbird::parser::firebird::
                          FirebirdCatalogProjectionSqlDaColumn>& expected,
    std::string_view label) {
  if (!actual.ok) {
    std::cerr << label << " SQLDA failed: " << actual.diagnostic << '\n';
    return false;
  }
  if (actual.columns.size() != expected.size()) {
    std::cerr << label << " SQLDA column count mismatch\n";
    return false;
  }
  for (std::size_t index = 0; index < expected.size(); ++index) {
    const auto& left = actual.columns[index];
    const auto& right = expected[index];
    if (left.field_name != right.field_name ||
        left.alias_name != right.alias_name ||
        left.relation_name != right.relation_name ||
        left.owner_name != right.owner_name ||
        left.sql_type != right.sql_type || left.subtype != right.subtype ||
        left.scale != right.scale || left.length != right.length ||
        left.charset_id != right.charset_id ||
        left.nullable != right.nullable ||
        left.wire_sql_type() != right.wire_sql_type()) {
      std::cerr << label << " SQLDA column " << index << " mismatch\n";
      return false;
    }
  }
  return true;
}

bool RequireFetchBlr(
    const scratchbird::parser::firebird::
        FirebirdCatalogProjectionFetchBlr& actual,
    const std::vector<std::uint8_t>& expected,
    std::uint32_t expected_message_length,
    std::string_view label) {
  if (!actual.ok) {
    std::cerr << label << " fetch BLR failed: " << actual.diagnostic << '\n';
    return false;
  }
  if (actual.bytes != expected ||
      actual.message_length != expected_message_length) {
    std::cerr << label << " fetch BLR/message length mismatch\n";
    return false;
  }
  return true;
}

scratchbird::parser::ipc::PublicRelationColumnDescriptor TextColumn() {
  scratchbird::parser::ipc::PublicRelationColumnDescriptor column;
  column.column_uuid = "11111111-1111-7111-8111-111111111111";
  column.ordinal = 1;
  column.canonical_name_key = "F1";
  column.type_descriptor_uuid = "22222222-2222-7222-8222-222222222222";
  column.type_descriptor_kind = "scalar";
  column.canonical_type_name = "VARCHAR(20)";
  column.encoded_type_descriptor = "type=VARCHAR(20);nullable=false";
  column.nullable = false;
  column.charset_uuid = "33333333-3333-7333-8333-333333333333";
  column.charset_canonical_name = "GBK";
  column.collation_uuid = "44444444-4444-7444-8444-444444444444";
  column.collation_canonical_name = "GBK_UNICODE";
  column.character_length = 20;
  column.charset_min_bytes = 1;
  column.charset_max_bytes = 2;
  column.charset_variable_width = true;
  return column;
}

scratchbird::parser::ipc::PublicRelationColumnDescriptor IntegerColumn() {
  scratchbird::parser::ipc::PublicRelationColumnDescriptor column;
  column.column_uuid = "55555555-5555-7555-8555-555555555555";
  column.ordinal = 0;
  column.canonical_name_key = "ID";
  column.type_descriptor_uuid = "66666666-6666-7666-8666-666666666666";
  column.type_descriptor_kind = "scalar";
  column.canonical_type_name = "INTEGER";
  column.encoded_type_descriptor = "type=INTEGER;nullable=true";
  column.nullable = true;
  return column;
}

} // namespace

int main() {
  scratchbird::parser::ipc::PublicNameResolutionResult resolved;
  resolved.resolved = true;
  resolved.object_uuid = "77777777-7777-7777-8777-777777777777";
  resolved.canonical_name = "users.public.FRESH_FIREBIRD_TABLE";
  resolved.object_class = "table";
  resolved.relation_descriptor.present = true;
  resolved.relation_descriptor.descriptor_uuid =
      "88888888-8888-7888-8888-888888888888";
  resolved.relation_descriptor.relation_uuid = resolved.object_uuid;
  resolved.relation_descriptor.descriptor_generation = 17;
  resolved.relation_descriptor.validated_resource_epoch = 29;
  // Supply reverse order to verify that persisted ordinals, not parser
  // overlay insertion order, determine Firebird SQLDA order.
  resolved.relation_descriptor.columns = {TextColumn(), IntegerColumn()};

  const auto projected =
      scratchbird::parser::firebird::ProjectFirebirdPersistedRelationMetadata(
          resolved, "unused_fallback");
  bool ok = true;
  ok = Require(projected.present,
               "fresh persisted relation projection was absent") && ok;
  ok = Require(projected.name == "FRESH_FIREBIRD_TABLE",
               "canonical physical relation leaf was not projected") && ok;
  ok = Require(projected.relation_uuid == resolved.object_uuid &&
                   projected.descriptor_generation == 17 &&
                   projected.validated_resource_epoch == 29,
               "engine descriptor identity/generation was lost") && ok;
  ok = Require(projected.columns.size() == 2 &&
                   projected.columns[0].name == "ID" &&
                   projected.columns[1].name == "F1",
               "persisted column ordinals were not preserved") && ok;
  if (projected.columns.size() == 2) {
    const auto& id = projected.columns[0];
    const auto& text = projected.columns[1];
    ok = Require(id.canonical_type_name == "INTEGER" && id.sql_type == 496 &&
                     id.length == 4 && id.nullable,
                 "canonical integer type/nullability was not rendered") && ok;
    ok = Require(text.canonical_type_name == "VARCHAR(20)" &&
                     text.sql_type == 448 && text.length == 40 &&
                     !text.nullable && text.character_length == 20,
                 "canonical text type/nullability/length was not rendered") && ok;
    ok = Require(
             text.charset_uuid ==
                     "33333333-3333-7333-8333-333333333333" &&
                 text.collation_uuid ==
                     "44444444-4444-7444-8444-444444444444" &&
                 text.character_set == "GBK" &&
                 text.collation == "GBK_UNICODE",
             "charset/collation UUID projection was lost") && ok;
  }

  using scratchbird::parser::firebird::
      BindFirebirdCatalogProjectionViewVariant;
  using scratchbird::parser::firebird::
      BuildFirebirdCatalogProjectionFetchBlr;
  using scratchbird::parser::firebird::
      DescribeFirebirdCatalogProjectionSqlDa;
  using scratchbird::parser::firebird::
      FirebirdCatalogProjectionSqlDaColumn;
  using scratchbird::parser::firebird::
      FirebirdCatalogProjectionSqlDaProfile;
  using scratchbird::parser::firebird::
      FirebirdCatalogProjectionRouteKind;
  using scratchbird::parser::firebird::
      FirebirdCatalogProjectionRouteForExactRebind;
  using scratchbird::parser::firebird::
      ParseFirebirdCatalogProjectionRoute;
  using scratchbird::parser::firebird::
      RenderFirebirdCatalogProjectionPayload;
  using scratchbird::parser::firebird::
      kCatalogRelationDescriptorProjectionV1;
  using scratchbird::parser::firebird::
      kFirebirdFieldsInfoCharsetInventoryV1;
  using scratchbird::parser::firebird::
      kFirebirdFieldsInfoTypeInventoryV1;

  // Exact upstream functional/table/create/test_01.py function body. In
  // particular, the final type code is written `,261,` without whitespace.
  constexpr std::string_view kUpstreamTypeNameFunction = R"FBSQL(
    create or alter function fn_get_type_name(a_type smallint, a_subtype smallint) returns varchar(2048) as
        declare ftype varchar(2048);
    begin
        ftype =
            decode( a_type
                    ,  7, decode(coalesce(a_subtype,0),  0, 'smallint',             1, 'numeric', 'unknown') -- 1 => small numerics [-327.68..327.67] (i.e. with mantissa that can be fit in -32768 ... 32767)
                    ,  8, decode(coalesce(a_subtype,0),  0, 'integer',              1, 'numeric', 2, 'decimal', 'unknown') -- 1: for numeric with mantissa >= 32768 and up to 9 digits, 2: for decimals up to 9 digits
                    , 10, 'float'
                    , 12, 'date'
                    , 13, 'time without time zone'
                    , 14, decode(coalesce(a_subtype,0),  0, 'char',                 1, 'binary', 'unknown')
                    , 16, decode(coalesce(a_subtype,0),  0, 'bigint',               1, 'numeric', 2, 'decimal', 'unknown')
                    , 23, 'boolean'
                    , 24, 'decfloat(16)'
                    , 25, 'decfloat(34)'
                    , 26, 'int128'
                    , 27, 'double precision' -- also for numeric and decimal, both with size >= 10, if sql_dialect = 1
                    , 28, 'time with time zone'
                    , 29, 'timestamp with time zone'
                    , 35, 'timestamp without time zone'
                    , 37, decode(coalesce(a_subtype,0),  0, 'varchar',              1, 'varbinary', 'unknown')
                    ,261, decode(coalesce(a_subtype,0),  0, 'blob sub_type binary', 1, 'blob sub_type text', 'unknown')
                  );
        if (ftype = 'unknown') then
            ftype = ftype || '__type_'  || coalesce(a_type, '[null]') || '__subtype_' || coalesce(a_subtype, '[null]');
        return ftype;
    end
  )FBSQL";
  const auto function_route =
      ParseFirebirdCatalogProjectionRoute(kUpstreamTypeNameFunction);
  ok = Require(
           function_route.kind ==
                   FirebirdCatalogProjectionRouteKind::
                       kCreateTypeNameFunction &&
               function_route.function_name == "FN_GET_TYPE_NAME",
           "exact upstream ,261, type-name function was not classified") &&
       ok;
  std::string prefixed_code(kUpstreamTypeNameFunction);
  const auto code_261 = prefixed_code.find(",261,");
  if (code_261 != std::string::npos) {
    prefixed_code.replace(code_261, 5, ",1261,");
  }
  ok = Require(
           !ParseFirebirdCatalogProjectionRoute(prefixed_code).recognized(),
           "type-code classifier accepted 1261 as the delimited code 261") &&
       ok;

  const auto without_message = ParseFirebirdCatalogProjectionRoute(
      "select v.* from v_fields_info v;");
  const auto with_message = ParseFirebirdCatalogProjectionRoute(
      "select 'presentation only' as msg, v.* from v_fields_info as v;");
  ok = Require(
           without_message.kind ==
                   FirebirdCatalogProjectionRouteKind::
                       kSelectFieldsInfoUnbound &&
               without_message.semantic_variant.empty() &&
               !without_message.leading_message,
           "catalog SELECT without MSG was not left engine-unbound") &&
       ok;
  ok = Require(
           with_message.kind ==
                   FirebirdCatalogProjectionRouteKind::
                       kSelectFieldsInfoUnbound &&
               with_message.semantic_variant.empty() &&
               with_message.leading_message &&
               *with_message.leading_message == "presentation only",
           "catalog SELECT MSG was not retained as presentation-only data") &&
       ok;

  const std::string type_detail =
      std::string(kCatalogRelationDescriptorProjectionV1) + ":" +
      std::string(kFirebirdFieldsInfoTypeInventoryV1);
  const std::string charset_detail =
      std::string(kCatalogRelationDescriptorProjectionV1) + ":" +
      std::string(kFirebirdFieldsInfoCharsetInventoryV1);
  const auto type_without_message =
      BindFirebirdCatalogProjectionViewVariant(without_message, type_detail);
  const auto type_with_message =
      BindFirebirdCatalogProjectionViewVariant(with_message, type_detail);
  const auto charset_without_message =
      BindFirebirdCatalogProjectionViewVariant(without_message, charset_detail);
  const auto charset_with_message =
      BindFirebirdCatalogProjectionViewVariant(with_message, charset_detail);
  ok = Require(
           type_without_message && type_with_message &&
               type_without_message->kind ==
                   FirebirdCatalogProjectionRouteKind::kSelectFieldsInfoType &&
               type_with_message->kind ==
                   FirebirdCatalogProjectionRouteKind::kSelectFieldsInfoType,
           "engine type variant was incorrectly coupled to MSG presence") &&
       ok;
  ok = Require(
           charset_without_message && charset_with_message &&
               charset_without_message->kind ==
                   FirebirdCatalogProjectionRouteKind::
                       kSelectFieldsInfoCharset &&
               charset_with_message->kind ==
                   FirebirdCatalogProjectionRouteKind::
                       kSelectFieldsInfoCharset,
           "engine charset variant was incorrectly coupled to MSG presence") &&
       ok;
  if (type_with_message && charset_without_message) {
    const auto type_rebind =
        FirebirdCatalogProjectionRouteForExactRebind(*type_with_message);
    const auto charset_rebind =
        FirebirdCatalogProjectionRouteForExactRebind(
            *charset_without_message);
    ok = Require(
             type_rebind.kind ==
                     FirebirdCatalogProjectionRouteKind::
                         kSelectFieldsInfoUnbound &&
                 type_rebind.semantic_variant.empty() &&
                 type_rebind.leading_message ==
                     type_with_message->leading_message &&
                 type_rebind.view_name == type_with_message->view_name &&
                 type_rebind.source_relation_name ==
                     type_with_message->source_relation_name,
             "type projection rebind retained transaction-bound semantics") &&
         ok;
    ok = Require(
             charset_rebind.kind ==
                     FirebirdCatalogProjectionRouteKind::
                         kSelectFieldsInfoUnbound &&
                 charset_rebind.semantic_variant.empty() &&
                 charset_rebind.view_name ==
                     charset_without_message->view_name &&
                 charset_rebind.source_relation_name ==
                     charset_without_message->source_relation_name,
             "charset projection rebind retained transaction-bound semantics") &&
         ok;
  }
  ok = Require(
           FirebirdCatalogProjectionRouteForExactRebind(function_route).kind ==
               FirebirdCatalogProjectionRouteKind::kCreateTypeNameFunction,
           "DDL projection route changed during exact rebind normalization") &&
       ok;
  for (const std::string_view malformed : {
           std::string_view{},
           kCatalogRelationDescriptorProjectionV1,
           std::string_view(
               "engine.catalog.relation_descriptor_projection.v1:"),
           std::string_view(
               "engine.catalog.relation_descriptor_projection.v1:relation.type_inventory.v1:trailing"),
           std::string_view(
               "engine.catalog.relation_descriptor_projection.v1:firebird.fields_info.type_inventory.v1")}) {
    ok = Require(
             !BindFirebirdCatalogProjectionViewVariant(without_message,
                                                        malformed),
             "malformed/foreign engine view detail did not fail closed") &&
         ok;
  }

  const auto view_column = [](std::string name,
                              std::uint32_t sql_type,
                              std::uint32_t length,
                              std::uint16_t charset_id) {
    return FirebirdCatalogProjectionSqlDaColumn{
        .field_name = name,
        .alias_name = name,
        .relation_name = "V_FIELDS_INFO",
        .owner_name = "SYSDBA",
        .sql_type = sql_type,
        .subtype = sql_type == 452 || sql_type == 448 ? charset_id : 0,
        .length = length,
        .charset_id = charset_id,
        .nullable = true,
    };
  };
  const FirebirdCatalogProjectionSqlDaColumn message_column{
      .field_name = "CONSTANT",
      .alias_name = "MSG",
      .sql_type = 452,
      .length = 17,
      .charset_id = 0,
      .nullable = false,
  };
  const std::vector<FirebirdCatalogProjectionSqlDaColumn> typed_columns{
      view_column("FIELD_NAME", 452, 252, 4),
      view_column("FIELD_TYPE", 448, 8192, 4),
      view_column("FIELD_POS", 500, 2, 0),
      view_column("FIELD_CHAR_LEN", 500, 2, 0),
      view_column("FIELD_CSET_ID", 500, 2, 0),
      view_column("FIELD_COLL_ID", 500, 2, 0),
      view_column("CSET_NAME", 452, 252, 4),
      view_column("FIELD_COLLATION", 452, 252, 4),
  };
  const std::vector<FirebirdCatalogProjectionSqlDaColumn> charset_columns{
      view_column("FIELD_NAME", 452, 252, 4),
      view_column("FIELD_CHAR_LEN", 500, 2, 0),
      view_column("FIELD_CSET_ID", 500, 2, 0),
      view_column("FIELD_COLL_ID", 500, 2, 0),
      view_column("CSET_NAME", 452, 252, 4),
      view_column("FIELD_COLLATION", 452, 252, 4),
  };
  auto with_leading_message =
      [&](const std::vector<FirebirdCatalogProjectionSqlDaColumn>& columns) {
        std::vector<FirebirdCatalogProjectionSqlDaColumn> with_message{
            message_column};
        with_message.insert(with_message.end(), columns.begin(), columns.end());
        return with_message;
      };

  if (type_without_message && type_with_message &&
      charset_without_message && charset_with_message) {
    const auto typed_sqlda =
        DescribeFirebirdCatalogProjectionSqlDa(*type_without_message);
    const auto typed_message_sqlda =
        DescribeFirebirdCatalogProjectionSqlDa(*type_with_message);
    const auto charset_sqlda =
        DescribeFirebirdCatalogProjectionSqlDa(*charset_without_message);
    const auto charset_message_sqlda =
        DescribeFirebirdCatalogProjectionSqlDa(*charset_with_message);
    ok = RequireSqlDa(typed_sqlda, typed_columns,
                      "typed catalog projection") &&
         ok;
    ok = RequireSqlDa(typed_message_sqlda,
                      with_leading_message(typed_columns),
                      "typed catalog projection with MSG") &&
         ok;
    ok = RequireSqlDa(charset_sqlda, charset_columns,
                      "charset catalog projection") &&
         ok;
    ok = RequireSqlDa(charset_message_sqlda,
                      with_leading_message(charset_columns),
                      "charset catalog projection with MSG") &&
         ok;

    // Exact Firebird 5.0.4 BlrFromMessage output. In particular, the four
    // typed numeric columns use blr_short (7), never blr_long (8), and every
    // value is followed by a blr_short null-indicator slot.
    const std::vector<std::uint8_t> typed_blr{
        5, 2, 4, 0, 16, 0,
        15, 4, 0, 252, 0, 7, 0,
        38, 4, 0, 0, 32, 7, 0,
        7, 0, 7, 0,
        7, 0, 7, 0,
        7, 0, 7, 0,
        7, 0, 7, 0,
        15, 4, 0, 252, 0, 7, 0,
        15, 4, 0, 252, 0, 7, 0,
        255, 76,
    };
    const std::vector<std::uint8_t> typed_message_blr{
        5, 2, 4, 0, 18, 0,
        15, 0, 0, 17, 0, 7, 0,
        15, 4, 0, 252, 0, 7, 0,
        38, 4, 0, 0, 32, 7, 0,
        7, 0, 7, 0,
        7, 0, 7, 0,
        7, 0, 7, 0,
        7, 0, 7, 0,
        15, 4, 0, 252, 0, 7, 0,
        15, 4, 0, 252, 0, 7, 0,
        255, 76,
    };
    const std::vector<std::uint8_t> charset_blr{
        5, 2, 4, 0, 12, 0,
        15, 4, 0, 252, 0, 7, 0,
        7, 0, 7, 0,
        7, 0, 7, 0,
        7, 0, 7, 0,
        15, 4, 0, 252, 0, 7, 0,
        15, 4, 0, 252, 0, 7, 0,
        255, 76,
    };
    const std::vector<std::uint8_t> charset_message_blr{
        5, 2, 4, 0, 14, 0,
        15, 0, 0, 17, 0, 7, 0,
        15, 4, 0, 252, 0, 7, 0,
        7, 0, 7, 0,
        7, 0, 7, 0,
        7, 0, 7, 0,
        15, 4, 0, 252, 0, 7, 0,
        15, 4, 0, 252, 0, 7, 0,
        255, 76,
    };
    ok = RequireFetchBlr(BuildFirebirdCatalogProjectionFetchBlr(
                             typed_sqlda.columns),
                         typed_blr, 8974, "typed catalog projection") &&
         ok;
    ok = RequireFetchBlr(BuildFirebirdCatalogProjectionFetchBlr(
                             typed_message_sqlda.columns),
                         typed_message_blr, 8994,
                         "typed catalog projection with MSG") &&
         ok;
    ok = RequireFetchBlr(BuildFirebirdCatalogProjectionFetchBlr(
                             charset_sqlda.columns),
                         charset_blr, 774, "charset catalog projection") &&
         ok;
    ok = RequireFetchBlr(BuildFirebirdCatalogProjectionFetchBlr(
                             charset_message_sqlda.columns),
                         charset_message_blr, 794,
                         "charset catalog projection with MSG") &&
         ok;

    const auto dialect_one_blr =
        BuildFirebirdCatalogProjectionFetchBlr(charset_sqlda.columns, 1);
    ok = Require(dialect_one_blr.ok && !dialect_one_blr.bytes.empty() &&
                     dialect_one_blr.bytes.front() == 4 &&
                     dialect_one_blr.message_length == 774,
                 "dialect-one fetch BLR did not use version 4") &&
         ok;

    FirebirdCatalogProjectionSqlDaProfile alternate_profile;
    alternate_profile.database_default_charset_id = 0;
    alternate_profile.database_default_charset_max_bytes_per_character = 1;
    alternate_profile.statement_charset_id = 4;
    alternate_profile.statement_charset_max_bytes_per_character = 4;
    const auto alternate = DescribeFirebirdCatalogProjectionSqlDa(
        *type_with_message, alternate_profile);
    ok = Require(alternate.ok && alternate.columns.size() == 9 &&
                     alternate.columns[0].sql_type == 452 &&
                     alternate.columns[0].subtype == 4 &&
                     alternate.columns[0].length == 68 &&
                     alternate.columns[0].charset_id == 4 &&
                     alternate.columns[2].sql_type == 448 &&
                     alternate.columns[2].subtype == 0 &&
                     alternate.columns[2].length == 2048 &&
                     alternate.columns[2].charset_id == 0 &&
                     alternate.columns[1].length == 252 &&
                     alternate.columns[1].subtype == 4 &&
                     alternate.columns[1].charset_id == 4,
                 "SQLDA charset profile was not applied at parser boundary") &&
         ok;
  }

  ok = Require(
           !DescribeFirebirdCatalogProjectionSqlDa(without_message).ok &&
               !DescribeFirebirdCatalogProjectionSqlDa(function_route).ok,
           "unbound/DDL catalog projection produced SQLDA") &&
       ok;
  FirebirdCatalogProjectionSqlDaProfile invalid_profile;
  invalid_profile.database_default_charset_max_bytes_per_character = 0;
  if (type_without_message) {
    ok = Require(!DescribeFirebirdCatalogProjectionSqlDa(
                      *type_without_message, invalid_profile)
                      .ok,
                 "invalid charset profile produced SQLDA") &&
         ok;
  }
  auto invalid_short = view_column("FIELD_POS", 500, 4, 0);
  ok = Require(!BuildFirebirdCatalogProjectionFetchBlr({invalid_short}).ok &&
                   !BuildFirebirdCatalogProjectionFetchBlr({}).ok,
               "malformed catalog SQLDA produced fetch BLR") &&
       ok;

  const std::string relation_uuid =
      "99999999-9999-7999-8999-999999999999";
  const std::string descriptor_uuid =
      "aaaaaaaa-aaaa-7aaa-8aaa-aaaaaaaaaaaa";
  const std::string neutral_payload =
      "row_count=2\n"
      "row[0]=column_uuid=bbbbbbbb-bbbb-7bbb-8bbb-bbbbbbbbbbbb;"
      "canonical_name_key=CHARCOL;ordinal=1;canonical_type_name=CHAR(3);"
      "character_length=3;charset_uuid=cccccccc-cccc-7ccc-8ccc-cccccccccccc;"
      "charset_canonical_name=NONE;collation_uuid=dddddddd-dddd-7ddd-8ddd-dddddddddddd;"
      "collation_canonical_name=NONE;text_large_object=false\n"
      "row[1]=column_uuid=eeeeeeee-eeee-7eee-8eee-eeeeeeeeeeee;"
      "canonical_name_key=ID;ordinal=0;canonical_type_name=INTEGER;"
      "character_length=;charset_uuid=;charset_canonical_name=;"
      "collation_uuid=;collation_canonical_name=;text_large_object=false\n"
      "evidence=catalog_projection_marker:engine.catalog.relation_descriptor_projection.v1\n"
      "evidence=catalog_projection_relation_uuid:" + relation_uuid + "\n" +
      "evidence=catalog_projection_descriptor_uuid:" + descriptor_uuid + "\n" +
      "evidence=catalog_projection_descriptor_generation:17\n"
      "evidence=catalog_projection_parser_sql:false\n";
  const auto rendered = RenderFirebirdCatalogProjectionPayload(
      *type_without_message, neutral_payload, relation_uuid, descriptor_uuid,
      17);
  ok = Require(rendered.ok && rendered.rows.size() == 2,
               "neutral catalog rows did not render") &&
       ok;
  if (rendered.ok && rendered.rows.size() == 2) {
    const auto& none_row = rendered.rows[0].cells;
    const auto& scalar_row = rendered.rows[1].cells;
    ok = Require(
             none_row.size() == 8 && none_row[4] && *none_row[4] == "0" &&
                 none_row[5] && *none_row[5] == "0" && none_row[6] &&
                 *none_row[6] == "NONE" && none_row[7] &&
                 *none_row[7] == "NONE",
             "seeded NONE charset/collation rendered as SQL NULL") &&
         ok;
    ok = Require(
             scalar_row.size() == 8 && !scalar_row[3] && !scalar_row[4] &&
                 !scalar_row[5] && !scalar_row[6] && !scalar_row[7],
             "resource-free scalar metadata did not remain SQL NULL") &&
         ok;
  }
  return ok ? EXIT_SUCCESS : EXIT_FAILURE;
}
