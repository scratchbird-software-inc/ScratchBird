// Copyright (c) 2026 ScratchBird Software Inc.
//
// This Source Code Form is subject to the terms of the Mozilla Public
// License, v. 2.0. If a copy of the MPL was not distributed with this
// file, You can obtain one at https://mozilla.org/MPL/2.0/.
//
// SPDX-License-Identifier: MPL-2.0

#include "firebird_global_aggregate_projection.hpp"
#include "firebird_execution_session.hpp"
#include "firebird_worker_session.hpp"

#include <cstdlib>
#include <iostream>
#include <string>
#include <string_view>
#include <utility>
#include <vector>

namespace {

namespace fb = scratchbird::parser::firebird;
namespace ipc = scratchbird::parser::ipc;

constexpr std::string_view kRetiredAvgSurfaceUuid =
    "019dffbb-f000-7fd3-b228-03bf40871b10";
constexpr std::string_view kLegacyAvgSurfaceUuid =
    "019dffbb-f000-710f-9410-919aad901ae2";

[[noreturn]] void Fail(std::string_view message) {
  std::cerr << message << '\n';
  std::exit(EXIT_FAILURE);
}

void Require(bool condition, std::string_view message) {
  if (!condition) Fail(message);
}

std::string Hex(std::string_view value) {
  static constexpr char kHex[] = "0123456789abcdef";
  std::string out;
  out.reserve(value.size() * 2u);
  for (const unsigned char byte : value) {
    out.push_back(kHex[(byte >> 4u) & 0x0fu]);
    out.push_back(kHex[byte & 0x0fu]);
  }
  return out;
}

ipc::PublicNameResolutionResult CompleteResolution(
    std::string canonical_type,
    std::string encoded_descriptor) {
  ipc::PublicNameResolutionResult resolved;
  resolved.resolved = true;
  resolved.object_uuid = "019f2000-0000-7000-8a00-000000000001";
  resolved.object_class = "table";
  resolved.relation_descriptor.present = true;
  resolved.relation_descriptor.descriptor_uuid =
      "019f2000-0000-7000-8a00-000000000002";
  resolved.relation_descriptor.relation_uuid = resolved.object_uuid;
  resolved.relation_descriptor.descriptor_generation = 17;
  resolved.relation_descriptor.validated_resource_epoch = 23;
  ipc::PublicRelationColumnDescriptor id;
  id.column_uuid = "019f2000-0000-7000-8a00-000000000003";
  id.ordinal = 0;
  id.canonical_name_key = "ID";
  id.type_descriptor_uuid = "019f2000-0000-7000-8a00-000000000004";
  id.type_descriptor_kind = "scalar";
  id.canonical_type_name = std::move(canonical_type);
  id.encoded_type_descriptor = std::move(encoded_descriptor);
  id.nullable = true;
  resolved.relation_descriptor.columns.push_back(std::move(id));
  return resolved;
}

ipc::PublicNameResolutionResult CompleteSchemaResolution() {
  ipc::PublicNameResolutionResult resolved;
  resolved.resolved = true;
  resolved.object_uuid = "019f2000-0000-7000-8a00-000000000010";
  resolved.object_class = "schema";
  return resolved;
}

ipc::PublicNameResolutionResult CompleteViewResolution(
    std::string_view alias = "AVG_RESULT",
    std::uint64_t generation = 7) {
  ipc::PublicNameResolutionResult resolved;
  resolved.resolved = true;
  resolved.object_uuid = "019f2000-0000-7000-8a00-000000000011";
  resolved.object_class = "view";
  const std::string descriptor_uuid =
      "019f2000-0000-7000-8a00-000000000012";
  const std::string semantic =
      "marker=" + std::string(fb::kFirebirdGlobalAggregateViewMarkerV1) +
      ";view_uuid=" + resolved.object_uuid +
      ";view_descriptor_generation=" + std::to_string(generation) +
      ";result_alias=" + std::string(alias) +
      ";result_type=int64;result_nullable=true";
  resolved.resolution_detail =
      "gavs1|" + Hex(fb::kFirebirdGlobalAggregateViewMarkerV1) + "|" +
      Hex(descriptor_uuid) + "|" + std::to_string(generation) + "|" +
      Hex("global_aggregate_view") + "|" +
      Hex(fb::kFirebirdGlobalAggregateViewMarkerV1) + "|" +
      Hex(semantic) + "|" + Hex(alias) + "|" + Hex("scalar") + "|" +
      Hex("int64") + "|" +
      Hex("canonical=int64;precision=64;scale=0;nullable=true");
  return resolved;
}

std::string CompletePayload(std::string_view alias,
                            std::string_view type,
                            std::string_view value,
                            bool is_null) {
  return "operation_id=dml.select_rows\n"
         "result_kind=global_aggregate_projection_rowset\n"
         "row_count=1\n"
         "row[0]=" +
         std::string(alias) + "=" + std::string(value) + "\n" +
         "row_meta[0]=" + std::string(alias) + ":" + std::string(type) +
         ":" + (is_null ? "null" : "not_null") + "\n" +
         "evidence=dml_result_projection:global_aggregate_projection\n"
         "evidence=global_aggregate_relation_scan:one_mga_visible_scan\n"
         "evidence=global_aggregate_output_count:1\n"
         "evidence=global_aggregate_function_uuid:" +
         std::string(fb::kFirebirdCanonicalAvgAggregateUuid) + "\n";
}

std::vector<fb::FirebirdScalarProjectionWireRow> OneRow(
    bool is_null,
    std::string value) {
  fb::FirebirdScalarProjectionWireRow row;
  row.cells.push_back({is_null, std::move(value)});
  return {std::move(row)};
}

void RequireRejectedShape(std::string_view sql) {
  const auto route = fb::ParseFirebirdGlobalAvgProjectionRoute(sql);
  Require(route.attempted && !route.recognized(),
          "unsupported AVG shape did not fail closed");
}

void TestParseBindLower() {
  const auto plain = fb::ParseFirebirdGlobalAvgProjectionRoute(
      "select avg(id) from test;");
  Require(plain.recognized() && plain.source_relation == "TEST" &&
              plain.item.source_column == "ID" &&
              plain.item.output_alias == "AVG" &&
              plain.item.operation ==
                  fb::FirebirdGlobalAvgProjectionOperation::kAvgField &&
              plain.item.aggregate_function_uuid ==
                  fb::kFirebirdCanonicalAvgAggregateUuid,
          "exact direct-relation AVG parse drifted");

  const auto distinct = fb::ParseFirebirdGlobalAvgProjectionRoute(
      "select avg(distinct id) as avg_id from test;");
  Require(distinct.recognized() && distinct.item.output_alias == "AVG_ID" &&
              distinct.item.operation ==
                  fb::FirebirdGlobalAvgProjectionOperation::kAvgDistinctField,
          "exact direct-relation AVG DISTINCT parse drifted");

  const auto int_resolution = CompleteResolution(
      "int64", "canonical=int64;precision=64;scale=0;nullable=true");
  const auto integer = fb::BindFirebirdGlobalAvgProjection(
      distinct, int_resolution);
  Require(integer.accepted &&
              integer.result_kind ==
                  fb::FirebirdGlobalAvgResultKind::kNullableInt64 &&
              integer.source_column.column_uuid ==
                  int_resolution.relation_descriptor.columns[0].column_uuid,
          "int64 AVG descriptor binding failed");
  const std::string encoded =
      fb::EncodeFirebirdGlobalAvgProjectionEnvelope(integer);
  Require(!encoded.empty() &&
              encoded.find("\"contains_sql_text\":false") !=
                  std::string::npos &&
              encoded.find(
                  "\"result_projection\":\"sblr.global_aggregate_projection.v1\"") !=
                  std::string::npos &&
              encoded.find("\"projection_count\":\"1\"") !=
                  std::string::npos &&
              encoded.find(fb::kFirebirdCanonicalAvgAggregateUuid) !=
                  std::string::npos &&
              encoded.find("gag1|") != std::string::npos &&
              encoded.find("|5|") != std::string::npos &&
              encoded.find(Hex(integer.source_column.column_uuid)) !=
                  std::string::npos &&
              encoded.find(Hex(integer.source_column.type_descriptor_uuid)) !=
                  std::string::npos,
          "AVG DISTINCT gag1 lowering is incomplete");
  Require(encoded.find(kRetiredAvgSurfaceUuid) == std::string::npos &&
              encoded.find(kLegacyAvgSurfaceUuid) == std::string::npos &&
              encoded.find("select avg") == std::string::npos &&
              encoded.find("sbsql") == std::string::npos,
          "AVG envelope leaked SQL, sibling identity, or rejected UUID");

  const auto real_resolution = CompleteResolution(
      "double precision", "type=double precision;nullable=true");
  const auto real = fb::BindFirebirdGlobalAvgProjection(plain,
                                                        real_resolution);
  Require(real.accepted &&
              real.result_kind ==
                  fb::FirebirdGlobalAvgResultKind::kNullableReal64 &&
              fb::EncodeFirebirdGlobalAvgProjectionEnvelope(real).find(
                  Hex("canonical=real64;precision=64;nullable=true")) !=
                  std::string::npos,
          "DOUBLE PRECISION AVG descriptor binding/lowering failed");
}

void TestRefusals() {
  for (const std::string_view sql : {
           "select avg(id + 1) from test",
           "select avg(id) from test where id > 0",
           "select avg(id), avg(other_id) from test",
           "select avg(id) as x, count(*) as y from test",
           "select avg(id) x from test",
           "select avg(*) from test",
           "select avg(id) from test join other on other.id = test.id",
           "select avg(id) from test group by id"}) {
    RequireRejectedShape(sql);
  }
  const auto unrelated = fb::ParseFirebirdGlobalAvgProjectionRoute(
      "select average_id from test");
  Require(!unrelated.attempted && !unrelated.recognized(),
          "unrelated identifier was quarantined as AVG");

  const auto route = fb::ParseFirebirdGlobalAvgProjectionRoute(
      "select avg(id) as avg_id from test");
  Require(route.recognized(), "AVG refusal route parse failed");

  auto incomplete = CompleteResolution(
      "int64", "canonical=int64;precision=64;scale=0;nullable=true");
  incomplete.relation_descriptor.columns[0].type_descriptor_uuid.clear();
  Require(!fb::BindFirebirdGlobalAvgProjection(route, incomplete).accepted,
          "incomplete AVG descriptor was accepted");

  auto ambiguous = CompleteResolution(
      "int64", "canonical=int64;precision=64;scale=0;nullable=true");
  ambiguous.relation_descriptor.columns.push_back(
      ambiguous.relation_descriptor.columns.front());
  ambiguous.relation_descriptor.columns.back().column_uuid =
      "019f2000-0000-7000-8a00-000000000005";
  Require(!fb::BindFirebirdGlobalAvgProjection(route, ambiguous).accepted,
          "ambiguous AVG descriptor was accepted");

  auto unsupported = CompleteResolution(
      "decimal", "canonical=decimal128;precision=18;scale=2;nullable=true");
  Require(!fb::BindFirebirdGlobalAvgProjection(route, unsupported).accepted,
          "unsupported AVG descriptor was accepted");

  for (const auto bad_uuid : {kRetiredAvgSurfaceUuid,
                              kLegacyAvgSurfaceUuid}) {
    auto bad_route = route;
    bad_route.item.aggregate_function_uuid = std::string(bad_uuid);
    Require(!fb::BindFirebirdGlobalAvgProjection(
                 bad_route,
                 CompleteResolution(
                     "int64",
                     "canonical=int64;precision=64;scale=0;nullable=true"))
                 .accepted,
            "rejected AVG surface UUID was admitted");
  }
}

void TestPersistedAvgViewRoute() {
  const auto create = fb::ParseFirebirdGlobalAggregateViewCreateRoute(
      "create or alter view v_test as select "
      "avg(2100000000*id)as avg_result from test;");
  Require(create.recognized() && create.create_or_alter &&
              create.view_name == "V_TEST" &&
              create.source_relation == "TEST" &&
              create.source_column == "ID" &&
              create.int32_literal == 2100000000 &&
              create.result_alias == "AVG_RESULT",
          "Firebird QA AVG test_06 view definition did not parse exactly");

  const auto relation = CompleteResolution(
      "integer", "type=integer;nullable=false");
  const auto bound_create = fb::BindFirebirdGlobalAggregateViewCreate(
      create, CompleteSchemaResolution(), relation);
  Require(bound_create.accepted,
          "Firebird QA AVG test_06 view definition did not bind");
  const std::string create_envelope =
      fb::EncodeFirebirdGlobalAggregateViewCreateEnvelope(bound_create);
  Require(create_envelope.find("gavc1|") != std::string::npos &&
              create_envelope.find(
                  "\"operation_family\":\"sblr.catalog.mutation.v3\"") !=
                  std::string::npos &&
              create_envelope.find("sblr.ddl.schema.v3") ==
                  std::string::npos &&
              create_envelope.find(
                  "\"view_query_shape\":\"engine.global_aggregate_view.v1\"") !=
                  std::string::npos &&
              create_envelope.find("2100000000*id") == std::string::npos &&
              create_envelope.find("sbsql") == std::string::npos,
          "bounded AVG view create envelope leaked SQL or sibling state");

  for (const std::string_view sql : {
           "create view v as select avg(id) as x from test",
           "create view v as select avg(2147483648 * id) as x from test",
           "create view v as select avg(2 * id) from test",
           "create view v as select avg(2 * id) as x from test where id > 0",
           "create view schema.v as select avg(2 * id) as x from test",
           "create view \"schema\".\"v\" as select avg(2 * id) as x from test",
           "create /* target */ view schema.v as /* query */ select "
           "/* aggregate */ avg(2 * id) as x from test"}) {
    const auto refused = fb::ParseFirebirdGlobalAggregateViewCreateRoute(sql);
    Require(refused.attempted && !refused.recognized(),
            "unsupported AVG view definition did not fail closed");
  }

  const auto commented_create =
      fb::ParseFirebirdGlobalAggregateViewCreateRoute(
          "create /* ddl */ view v_comment as /* query */ select "
          "/* aggregate */ avg(2 * id) as x from /* source */ test");
  Require(commented_create.recognized() &&
              commented_create.view_name == "V_COMMENT" &&
              commented_create.source_relation == "TEST",
          "Firebird comments broke bounded AVG-view intent/parse handling");

  const auto select = fb::ParseFirebirdGlobalAggregateViewSelectRoute(
      "select * from v_test;");
  Require(select.recognized() && select.view_name == "V_TEST",
          "bounded AVG view SELECT did not parse");
  const auto ordinary_candidate =
      fb::ParseFirebirdGlobalAggregateViewSelectRoute(
          "select * from ordinary_table");
  ipc::PublicNameResolutionResult ordinary_not_a_view;
  ordinary_not_a_view.messages.diagnostics.push_back(ipc::MakeDiagnostic(
      "PARSER_SERVER_IPC.NAME_NOT_FOUND_OR_NOT_VISIBLE", "ERROR",
      "ordinary table is not an engine semantic view", "probe"));
  const auto ordinary_view_binding =
      fb::BindFirebirdGlobalAggregateViewSelect(
          ordinary_candidate, ordinary_not_a_view);
  Require(ordinary_candidate.recognized() &&
              !ordinary_view_binding.accepted &&
              ordinary_view_binding.semantic_transport.empty(),
          "ordinary SELECT * was promoted to an aggregate view without "
          "engine semantic evidence");
  fb::FirebirdPipelineResult ordinary_fallback;
  ordinary_fallback.accepted = true;
  ordinary_fallback.global_aggregate_view_select_route = ordinary_candidate;
  ordinary_fallback.global_aggregate_view_result_kind =
      fb::FirebirdGlobalAvgResultKind::kNullableInt64;
  ordinary_fallback.global_aggregate_view_result_alias = "WRONG_VIEW_ALIAS";
  ordinary_fallback.server_result_payload = "ordinary_table_result";
  fb::ApplyFirebirdOrdinaryRelationSelectFallback(
      &ordinary_fallback, "ordinary_table_select_envelope");
  Require(ordinary_fallback.accepted &&
              !ordinary_fallback.global_aggregate_view_select_route
                   .recognized() &&
              ordinary_fallback.global_aggregate_view_result_kind ==
                  fb::FirebirdGlobalAvgResultKind::kUnsupported &&
              ordinary_fallback.global_aggregate_view_result_alias.empty() &&
              ordinary_fallback.sblr_payload ==
                  "ordinary_table_select_envelope" &&
              ordinary_fallback.server_result_payload ==
                  "ordinary_table_result",
          "NAME_NOT_FOUND ordinary-table fallback retained view state or "
          "damaged the ordinary result");
  const auto resolved_view = CompleteViewResolution();
  const auto bound_select = fb::BindFirebirdGlobalAggregateViewSelect(
      select, resolved_view);
  Require(bound_select.accepted &&
              bound_select.result_kind ==
                  fb::FirebirdGlobalAvgResultKind::kNullableInt64 &&
              bound_select.result_alias == "AVG_RESULT",
          "bounded AVG view semantic descriptor did not bind");
  const std::string select_envelope =
      fb::EncodeFirebirdGlobalAggregateViewSelectEnvelope(bound_select);
  Require(select_envelope.find("gavs1|") != std::string::npos &&
              select_envelope.find(
                  "\"target_object_kind\":\"view\"") !=
                  std::string::npos &&
              resolved_view.resolution_detail.find(Hex(relation.object_uuid)) ==
                  std::string::npos &&
              resolved_view.resolution_detail.find(Hex(
                  relation.relation_descriptor.columns[0].column_uuid)) ==
                  std::string::npos &&
              resolved_view.resolution_detail.find(Hex(
                  relation.relation_descriptor.columns[0]
                      .type_descriptor_uuid)) == std::string::npos &&
              resolved_view.resolution_detail.find(Hex("2100000000")) ==
                  std::string::npos,
          "bounded AVG view select leaked source/literal authority");

  auto stale = resolved_view;
  const auto generation = stale.resolution_detail.find("|7|");
  Require(generation != std::string::npos,
          "semantic descriptor generation fixture drifted");
  stale.resolution_detail.replace(generation, 3, "|8|");
  Require(!fb::BindFirebirdGlobalAggregateViewSelect(select, stale).accepted,
          "stale semantic descriptor generation was accepted");
  for (const std::string_view malformed : {
           "gavs1|00|00|1|00|00|00|00|00|00",
           "gavs1|00|00|1|00|00|00|00|00|00|00|00"}) {
    auto invalid = resolved_view;
    invalid.resolution_detail = malformed;
    Require(!fb::BindFirebirdGlobalAggregateViewSelect(select, invalid).accepted,
            "malformed gavs1 field count was accepted");
  }
}

void TestWorkerPresentation() {
  const auto route = fb::ParseFirebirdGlobalAvgProjectionRoute(
      "select avg(id) as avg_id from test");
  Require(route.recognized(), "AVG worker route parse failed");

  const auto integer_descriptors =
      fb::DescribeFirebirdGlobalAvgProjectionWireDescriptors(
          route, fb::FirebirdGlobalAvgResultKind::kNullableInt64);
  Require(integer_descriptors.size() == 1 &&
              integer_descriptors[0].name == "AVG_ID" &&
              integer_descriptors[0].sql_type == 580 &&
              integer_descriptors[0].length == 8 &&
              integer_descriptors[0].nullable,
          "nullable SQL_INT64 AVG SQLDA drifted");

  std::string diagnostic;
  auto integer_rows = OneRow(false, "5");
  Require(fb::ValidateFirebirdGlobalAvgProjectionCompletePacket(
              route,
              fb::FirebirdGlobalAvgResultKind::kNullableInt64,
              CompletePayload("AVG_ID", "int64", "5", false),
              integer_descriptors,
              &integer_rows,
              1,
              false,
              &diagnostic),
          "valid integer AVG worker packet was refused");

  auto null_rows = OneRow(true, "");
  Require(fb::ValidateFirebirdGlobalAvgProjectionCompletePacket(
              route,
              fb::FirebirdGlobalAvgResultKind::kNullableInt64,
              CompletePayload("AVG_ID", "int64", "", true),
              integer_descriptors,
              &null_rows,
              1,
              false,
              &diagnostic),
          "valid SQL NULL AVG worker packet was refused");

  const auto real_descriptors =
      fb::DescribeFirebirdGlobalAvgProjectionWireDescriptors(
          route, fb::FirebirdGlobalAvgResultKind::kNullableReal64);
  Require(real_descriptors.size() == 1 &&
              real_descriptors[0].sql_type == 480 &&
              real_descriptors[0].length == 8 &&
              real_descriptors[0].nullable,
          "nullable SQL_DOUBLE AVG SQLDA drifted");
  auto real_rows = OneRow(false, "5.123456789");
  Require(fb::ValidateFirebirdGlobalAvgProjectionCompletePacket(
              route,
              fb::FirebirdGlobalAvgResultKind::kNullableReal64,
              CompletePayload(
                  "AVG_ID", "real64", "5.123456789", false),
              real_descriptors,
              &real_rows,
              1,
              false,
              &diagnostic),
          "valid real64 AVG worker packet was refused");

  auto malformed_rows = OneRow(false, "5");
  auto malformed_payload = CompletePayload("AVG_ID", "int64", "5", false);
  const auto evidence = malformed_payload.find("one_mga_visible_scan");
  Require(evidence != std::string::npos,
          "AVG malformed test fixture lost scan evidence");
  malformed_payload.replace(evidence,
                            std::string_view("one_mga_visible_scan").size(),
                            "parser_local_scan");
  Require(!fb::ValidateFirebirdGlobalAvgProjectionCompletePacket(
              route,
              fb::FirebirdGlobalAvgResultKind::kNullableInt64,
              malformed_payload,
              integer_descriptors,
              &malformed_rows,
              1,
              false,
              &diagnostic) &&
              malformed_rows.empty(),
          "AVG worker accepted non-MGA evidence or retained rows");

  auto cursor_rows = OneRow(false, "5");
  Require(!fb::ValidateFirebirdGlobalAvgProjectionCompletePacket(
              route,
              fb::FirebirdGlobalAvgResultKind::kNullableInt64,
              CompletePayload("AVG_ID", "int64", "5", false),
              integer_descriptors,
              &cursor_rows,
              1,
              true,
              &diagnostic) &&
              cursor_rows.empty(),
          "AVG worker accepted a cursor continuation");
}

}  // namespace

int main() {
  TestParseBindLower();
  TestRefusals();
  TestPersistedAvgViewRoute();
  TestWorkerPresentation();
  return EXIT_SUCCESS;
}
