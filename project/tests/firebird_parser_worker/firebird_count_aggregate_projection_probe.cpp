// Copyright (c) 2026 ScratchBird Software Inc.
//
// This Source Code Form is subject to the terms of the Mozilla Public
// License, v. 2.0. If a copy of the MPL was not distributed with this
// file, You can obtain one at https://mozilla.org/MPL/2.0/.
//
// SPDX-License-Identifier: MPL-2.0

#include "firebird_global_aggregate_projection.hpp"
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

constexpr std::string_view kRetiredConflictingCountUuid =
    "019dffbb-f000-7613-a71e-84b03ef18e1d";
constexpr std::string_view kLegacyAggregateCountSurfaceUuid =
    "019dffbb-f000-7293-b215-aa84d8693576";

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

ipc::PublicNameResolutionResult CompleteResolution() {
  ipc::PublicNameResolutionResult resolved;
  resolved.resolved = true;
  resolved.object_uuid = "019f1000-0000-7000-8a00-000000000001";
  resolved.object_class = "table";
  resolved.relation_descriptor.present = true;
  resolved.relation_descriptor.descriptor_uuid =
      "019f1000-0000-7000-8a00-000000000002";
  resolved.relation_descriptor.relation_uuid = resolved.object_uuid;
  resolved.relation_descriptor.descriptor_generation = 7;
  resolved.relation_descriptor.validated_resource_epoch = 11;
  ipc::PublicRelationColumnDescriptor id;
  id.column_uuid = "019f1000-0000-7000-8a00-000000000003";
  id.ordinal = 0;
  id.canonical_name_key = "ID";
  id.type_descriptor_uuid = "019f1000-0000-7000-8a00-000000000004";
  id.type_descriptor_kind = "scalar";
  id.canonical_type_name = "int32";
  id.encoded_type_descriptor =
      "canonical=int32;precision=32;scale=0;nullable=true";
  id.nullable = true;
  resolved.relation_descriptor.columns.push_back(std::move(id));
  return resolved;
}

std::string CompletePayload() {
  return
      "operation_id=dml.select_rows\n"
      "result_kind=global_aggregate_projection_rowset\n"
      "row_count=1\n"
      "row[0]=CNT_ALL=9;CNT_NN=6;CNT_UNQ=2\n"
      "row_meta[0]=CNT_ALL:int64:not_null;CNT_NN:int64:not_null;CNT_UNQ:int64:not_null\n"
      "evidence=dml_result_projection:global_aggregate_projection\n"
      "evidence=global_aggregate_relation_scan:one_mga_visible_scan\n"
      "evidence=global_aggregate_output_count:3\n"
      "evidence=global_aggregate_function_uuid:019de5fc-2400-784a-9aec-371f8b95b7ea\n";
}

std::vector<fb::FirebirdScalarProjectionWireRow> CompleteRows() {
  fb::FirebirdScalarProjectionWireRow row;
  row.cells.push_back({false, "9"});
  row.cells.push_back({false, "6"});
  row.cells.push_back({false, "2"});
  return {std::move(row)};
}

void RequireRejectedShape(std::string_view sql) {
  const auto route = fb::ParseFirebirdGlobalCountProjectionRoute(sql);
  Require(route.attempted && !route.recognized(),
          "unsupported multi-COUNT shape did not fail closed");
}

void TestExactParseBindLower() {
  constexpr std::string_view kCanonicalSql =
      "select count(*) as cnt_all, count(id) as cnt_nn,\n"
      "       count(distinct id) as cnt_unq from test;";
  const auto route = fb::ParseFirebirdGlobalCountProjectionRoute(kCanonicalSql);
  Require(route.recognized() && route.source_relation == "TEST" &&
              route.items.size() == 3,
          "canonical Firebird count test_02 shape was not recognized");
  Require(route.items[0].operation ==
                  fb::FirebirdGlobalCountProjectionOperation::kCountStar &&
              route.items[1].operation ==
                  fb::FirebirdGlobalCountProjectionOperation::
                      kCountNonNullField &&
              route.items[2].operation ==
                  fb::FirebirdGlobalCountProjectionOperation::
                      kCountDistinctField &&
              route.items[1].source_column == "ID" &&
              route.items[2].source_column == "ID",
          "canonical Firebird COUNT operations drifted");
  Require(route.items[0].output_alias == "CNT_ALL" &&
              route.items[1].output_alias == "CNT_NN" &&
              route.items[2].output_alias == "CNT_UNQ",
          "canonical Firebird COUNT aliases drifted");
  for (const auto& item : route.items) {
    Require(item.aggregate_function_uuid ==
                fb::kFirebirdCanonicalCountAggregateUuid,
            "Firebird parser did not bind canonical COUNT aggregate UUID");
  }

  const auto resolution = CompleteResolution();
  const auto binding =
      fb::BindFirebirdGlobalCountProjection(route, resolution);
  Require(binding.accepted &&
              binding.relation_uuid == resolution.object_uuid &&
              binding.relation_descriptor_uuid ==
                  resolution.relation_descriptor.descriptor_uuid &&
              binding.relation_descriptor_generation == 7 &&
              binding.source_column.column_uuid ==
                  resolution.relation_descriptor.columns[0].column_uuid,
          "Firebird global COUNT descriptor binding failed");
  const std::string encoded =
      fb::EncodeFirebirdGlobalCountProjectionEnvelope(binding);
  Require(!encoded.empty() &&
              encoded.find("\"contains_sql_text\":false") !=
                  std::string::npos &&
              encoded.find("\"result_projection\":\"sblr.global_aggregate_projection.v1\"") !=
                  std::string::npos &&
              encoded.find("\"projection_count\":\"3\"") !=
                  std::string::npos &&
              encoded.find(fb::kFirebirdCanonicalCountAggregateUuid) !=
                  std::string::npos,
          "Firebird global COUNT neutral envelope is incomplete");
  Require(encoded.find(kRetiredConflictingCountUuid) == std::string::npos &&
              encoded.find(kLegacyAggregateCountSurfaceUuid) ==
                  std::string::npos &&
              encoded.find(kCanonicalSql) == std::string::npos &&
              encoded.find("sbsql") == std::string::npos,
          "Firebird global COUNT envelope leaked SQL, sibling identity, or retired UUID");
  Require(encoded.find(Hex(binding.source_column.column_uuid)) !=
                  std::string::npos &&
              encoded.find(Hex(binding.source_column.type_descriptor_uuid)) !=
                  std::string::npos &&
              encoded.find(Hex(binding.source_column.encoded_type_descriptor)) !=
                  std::string::npos,
          "Firebird global COUNT envelope omitted bound field descriptors");

  const auto simple = fb::ParseFirebirdGlobalCountProjectionRoute(
      "select count(*) from test;");
  Require(!simple.attempted && !simple.recognized(),
          "test_01 single COUNT route was captured by the new test_02 route");
  const auto unrelated = fb::ParseFirebirdGlobalCountProjectionRoute(
      "select account_id, discount, recount from test;");
  Require(!unrelated.attempted && !unrelated.recognized(),
          "identifier text containing COUNT was quarantined as an aggregate call");
}

void TestUnsupportedShapes() {
  RequireRejectedShape(
      "select count(id) as cnt_nn, count(*) as cnt_all, "
      "count(distinct id) as cnt_unq from test");
  RequireRejectedShape(
      "select count(*) as cnt_all, count(id) as cnt_nn, "
      "count(distinct other_id) as cnt_unq from test");
  RequireRejectedShape(
      "select count(*) cnt_all, count(id) as cnt_nn, "
      "count(distinct id) as cnt_unq from test");
  RequireRejectedShape(
      "select count(*) as cnt_all, count(id) as cnt_nn, "
      "count(distinct id) as cnt_unq from test where id > 0");
  RequireRejectedShape(
      "select count(*) as cnt_all, count(id) as cnt_nn from test");
  RequireRejectedShape(
      "select count(*) as x, count(id) as x, "
      "count(distinct id) as y from test");
}

void TestDescriptorRefusals() {
  const auto route = fb::ParseFirebirdGlobalCountProjectionRoute(
      "select count(*) as cnt_all, count(id) as cnt_nn, "
      "count(distinct id) as cnt_unq from test");
  Require(route.recognized(), "descriptor-refusal route parse failed");

  auto missing = CompleteResolution();
  missing.relation_descriptor.present = false;
  Require(!fb::BindFirebirdGlobalCountProjection(route, missing).accepted,
          "missing relation descriptor was accepted");

  auto mismatch = CompleteResolution();
  mismatch.relation_descriptor.relation_uuid =
      "019f1000-0000-7000-8a00-000000000099";
  Require(!fb::BindFirebirdGlobalCountProjection(route, mismatch).accepted,
          "mismatched relation descriptor was accepted");

  auto missing_column = CompleteResolution();
  missing_column.relation_descriptor.columns[0].canonical_name_key = "OTHER";
  Require(!fb::BindFirebirdGlobalCountProjection(route, missing_column).accepted,
          "missing COUNT field was accepted");

  auto ambiguous = CompleteResolution();
  ambiguous.relation_descriptor.columns.push_back(
      ambiguous.relation_descriptor.columns.front());
  ambiguous.relation_descriptor.columns.back().column_uuid =
      "019f1000-0000-7000-8a00-000000000005";
  Require(!fb::BindFirebirdGlobalCountProjection(route, ambiguous).accepted,
          "ambiguous COUNT field was accepted");

  auto incomplete = CompleteResolution();
  incomplete.relation_descriptor.columns[0].encoded_type_descriptor.clear();
  Require(!fb::BindFirebirdGlobalCountProjection(route, incomplete).accepted,
          "incomplete COUNT field descriptor was accepted");

  for (const auto legacy_uuid : {kRetiredConflictingCountUuid,
                                 kLegacyAggregateCountSurfaceUuid}) {
    auto bad_uuid_route = route;
    bad_uuid_route.items[0].aggregate_function_uuid =
        std::string(legacy_uuid);
    Require(!fb::BindFirebirdGlobalCountProjection(
                 bad_uuid_route, CompleteResolution())
                 .accepted,
            "non-canonical COUNT UUID was accepted");
  }
}

void TestWorkerPresentationValidation() {
  const auto route = fb::ParseFirebirdGlobalCountProjectionRoute(
      "select count(*) as cnt_all, count(id) as cnt_nn, "
      "count(distinct id) as cnt_unq from test");
  const auto descriptors =
      fb::DescribeFirebirdGlobalCountProjectionWireDescriptors(route);
  Require(descriptors.size() == 3, "global COUNT SQLDA descriptor count drifted");
  for (std::size_t index = 0; index < descriptors.size(); ++index) {
    Require(descriptors[index].name == route.items[index].output_alias &&
                descriptors[index].source_name ==
                    route.items[index].output_alias &&
                descriptors[index].sql_type == 580 &&
                descriptors[index].length == 8 &&
                descriptors[index].scale == 0 &&
                descriptors[index].subtype == 0 &&
                !descriptors[index].nullable,
            "global COUNT SQL_INT64 descriptor drifted");
  }

  auto rows = CompleteRows();
  std::string diagnostic;
  Require(fb::ValidateFirebirdGlobalCountProjectionCompletePacket(
              route, CompletePayload(), descriptors, &rows, 1, false,
              &diagnostic) &&
              rows.size() == 1 && rows[0].cells.size() == 3 &&
              rows[0].cells[0].text == "9" &&
              rows[0].cells[1].text == "6" &&
              rows[0].cells[2].text == "2",
          "valid engine-owned 9/6/2 COUNT packet was refused");

  auto require_refused = [&](std::string payload,
                             std::vector<fb::FirebirdScalarProjectionWireDescriptor>
                                 actual_descriptors,
                             std::vector<fb::FirebirdScalarProjectionWireRow>
                                 actual_rows,
                             std::uint64_t server_count,
                             bool cursor_present) {
    diagnostic.clear();
    Require(!fb::ValidateFirebirdGlobalCountProjectionCompletePacket(
                route, payload, actual_descriptors, &actual_rows,
                server_count, cursor_present, &diagnostic) &&
                actual_rows.empty() && !diagnostic.empty(),
            "malformed global COUNT packet leaked a row");
  };

  std::string bad_kind = CompletePayload();
  bad_kind.replace(bad_kind.find("global_aggregate_projection_rowset"),
                   std::string("global_aggregate_projection_rowset").size(),
                   "rows");
  require_refused(bad_kind, descriptors, CompleteRows(), 1, false);

  auto bad_descriptors = descriptors;
  bad_descriptors[1].nullable = true;
  require_refused(CompletePayload(), bad_descriptors, CompleteRows(), 1,
                  false);

  auto null_rows = CompleteRows();
  null_rows[0].cells[1].is_null = true;
  null_rows[0].cells[1].text.clear();
  require_refused(CompletePayload(), descriptors, std::move(null_rows), 1,
                  false);

  std::string bad_meta = CompletePayload();
  bad_meta.replace(bad_meta.find("CNT_NN:int64:not_null"),
                   std::string("CNT_NN:int64:not_null").size(),
                   "CNT_NN:text:not_null");
  require_refused(bad_meta, descriptors, CompleteRows(), 1, false);

  std::string missing_evidence = CompletePayload();
  const std::string evidence =
      "evidence=global_aggregate_relation_scan:one_mga_visible_scan\n";
  missing_evidence.erase(missing_evidence.find(evidence), evidence.size());
  require_refused(missing_evidence, descriptors, CompleteRows(), 1, false);

  require_refused(CompletePayload(), descriptors, CompleteRows(), 2, false);
  require_refused(CompletePayload(), descriptors, CompleteRows(), 1, true);
}

}  // namespace

int main() {
  TestExactParseBindLower();
  TestUnsupportedShapes();
  TestDescriptorRefusals();
  TestWorkerPresentationValidation();
  return EXIT_SUCCESS;
}
