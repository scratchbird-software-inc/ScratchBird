// Copyright (c) 2026 ScratchBird Software Inc.
//
// This Source Code Form is subject to the terms of the Mozilla Public
// License, v. 2.0. If a copy of the MPL was not distributed with this
// file, You can obtain one at https://mozilla.org/MPL/2.0/.
//
// SPDX-License-Identifier: MPL-2.0

#include "firebird_relation_projection_view.hpp"
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

constexpr std::string_view kSchemaUuid =
    "019f2700-0000-7000-8a00-000000000001";
constexpr std::string_view kRelationUuid =
    "019f2700-0000-7000-8a00-000000000002";
constexpr std::string_view kRelationDescriptorUuid =
    "019f2700-0000-7000-8a00-000000000003";
constexpr std::string_view kSourceColumnUuid =
    "019f2700-0000-7000-8a00-000000000004";
constexpr std::string_view kSourceTypeUuid =
    "019f2700-0000-7000-8a00-000000000005";
constexpr std::string_view kViewUuid =
    "019f2700-0000-7000-8a00-000000000010";
constexpr std::string_view kViewDescriptorUuid =
    "019f2700-0000-7000-8a00-000000000011";
constexpr std::string_view kOutput0Uuid =
    "019f2700-0000-7000-8a00-000000000012";
constexpr std::string_view kOutput0TypeUuid =
    "019f2700-0000-7000-8a00-000000000013";
constexpr std::string_view kOutput1Uuid =
    "019f2700-0000-7000-8a00-000000000014";
constexpr std::string_view kOutput1TypeUuid =
    "019f2700-0000-7000-8a00-000000000015";

[[noreturn]] void Fail(std::string_view message) {
  std::cerr << message << '\n';
  std::exit(EXIT_FAILURE);
}

void Require(bool condition, std::string_view message) {
  if (!condition) Fail(message);
}

void VerifyWorkerPhysicalSelectDescriptorBoundary() {
  Require(fb::FirebirdStatementRequiresPhysicalSelectDescriptor(
              "SELECT * FROM tb;"),
          "physical SELECT did not request its persisted relation descriptor");
  Require(fb::FirebirdStatementRequiresPhysicalSelectDescriptor(
              "WITH q AS (SELECT id FROM tb) SELECT id FROM q;"),
          "WITH SELECT did not request its persisted relation descriptor");
  Require(fb::FirebirdStatementRequiresPhysicalSelectDescriptor(
              "-- leading trivia\nSELECT * FROM tb;"),
          "line-commented SELECT lost its persisted relation descriptor");
  Require(fb::FirebirdStatementRequiresPhysicalSelectDescriptor(
              "/* leading trivia */ WITH q AS (SELECT id FROM tb) "
              "SELECT id FROM q;"),
          "block-commented WITH SELECT lost its persisted relation descriptor");
  Require(!fb::FirebirdStatementRequiresPhysicalSelectDescriptor(
              "DELETE FROM test WHERE id=10;"),
          "view DELETE escaped into the physical SELECT descriptor preflight");
  Require(!fb::FirebirdStatementRequiresPhysicalSelectDescriptor(
              "UPDATE tb SET id=11 WHERE id=10;"),
          "UPDATE escaped into the physical SELECT descriptor preflight");
  Require(!fb::FirebirdStatementRequiresPhysicalSelectDescriptor(
              "SELECTOR * FROM tb;"),
          "SELECT keyword-prefix near miss requested a relation descriptor");
  Require(!fb::FirebirdStatementRequiresPhysicalSelectDescriptor(
              "WITHH q AS (SELECT id FROM tb) SELECT id FROM q;"),
          "WITH keyword-prefix near miss requested a relation descriptor");
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

ipc::PublicNameResolutionResult SchemaResolution() {
  ipc::PublicNameResolutionResult result;
  result.resolved = true;
  result.object_uuid = kSchemaUuid;
  result.object_class = "schema";
  return result;
}

ipc::PublicNameResolutionResult TableResolution(bool nullable = true) {
  ipc::PublicNameResolutionResult result;
  result.resolved = true;
  result.object_uuid = kRelationUuid;
  result.object_class = "table";
  result.relation_descriptor.present = true;
  result.relation_descriptor.descriptor_uuid = kRelationDescriptorUuid;
  result.relation_descriptor.relation_uuid = kRelationUuid;
  result.relation_descriptor.descriptor_generation = 17;
  result.relation_descriptor.validated_resource_epoch = 23;

  ipc::PublicRelationColumnDescriptor id;
  id.column_uuid = kSourceColumnUuid;
  id.ordinal = 0;
  id.canonical_name_key = "ID";
  id.type_descriptor_uuid = kSourceTypeUuid;
  id.type_descriptor_kind = "scalar";
  id.canonical_type_name = "int32";
  id.encoded_type_descriptor = nullable
                                   ? "canonical=int32;precision=32;scale=0;nullable=true"
                                   : "canonical=int32;precision=32;scale=0;nullable=false";
  id.nullable = nullable;
  result.relation_descriptor.columns.push_back(std::move(id));
  return result;
}

std::string SemanticPacket(std::string_view family = "rpvs1",
                           std::string_view output0_uuid = kOutput0Uuid,
                           std::string_view output0_type_uuid =
                               kOutput0TypeUuid,
                           std::string_view output1_uuid = kOutput1Uuid,
                           std::string_view output1_type_uuid =
                               kOutput1TypeUuid,
                           std::string_view literal_nullable = "0") {
  return std::string(family) + "|" +
         Hex(fb::kFirebirdRelationProjectionViewMarkerV1) + "|" +
         Hex(kViewDescriptorUuid) + "|9|2|0|" + Hex("ID") + "|" +
         Hex(output0_uuid) + "|" + Hex(output0_type_uuid) + "|" +
         Hex("scalar") + "|" + Hex("int32") + "|" +
         Hex("canonical=int32;precision=32;scale=0;nullable=true") +
         "|" + Hex("1") + "|1|" + Hex("NUM") + "|" +
         Hex(output1_uuid) + "|" + Hex(output1_type_uuid) + "|" +
         Hex("scalar") + "|" + Hex("int32") + "|" +
         Hex("canonical=int32;precision=32;scale=0;nullable=false") +
         "|" + Hex(literal_nullable);
}

std::string UpdatableSemanticPacket(
    std::string_view family = "rpvd2",
    std::string_view marker = fb::kFirebirdRelationProjectionViewMarkerV2,
    std::string_view output_name = "ID",
    std::string_view output_uuid = kOutput0Uuid,
    std::string_view output_type_uuid = kOutput0TypeUuid,
    std::string_view descriptor_kind = "scalar",
    std::string_view canonical_type = "int32",
    std::string_view encoded_descriptor =
        "canonical=int32;precision=32;scale=0;nullable=true",
    std::string_view nullable = "1",
    std::string_view ordinal = "0",
    std::string_view output_count = "1",
    std::uint64_t generation = 9) {
  return std::string(family) + "|" + Hex(marker) + "|" +
         Hex(kViewDescriptorUuid) + "|" + std::to_string(generation) + "|" +
         std::string(output_count) + "|" + std::string(ordinal) + "|" +
         Hex(output_name) + "|" + Hex(output_uuid) + "|" +
         Hex(output_type_uuid) + "|" + Hex(descriptor_kind) + "|" +
         Hex(canonical_type) + "|" + Hex(encoded_descriptor) + "|" +
         Hex(nullable);
}

ipc::PublicNameResolutionResult ViewResolution(
    std::string semantic = SemanticPacket()) {
  ipc::PublicNameResolutionResult result;
  result.resolved = true;
  result.object_uuid = kViewUuid;
  result.object_class = "view";
  result.resolution_detail = std::move(semantic);
  return result;
}

ipc::PublicNameResolutionResult UpdatableViewResolution(
    std::string semantic = UpdatableSemanticPacket()) {
  ipc::PublicNameResolutionResult result;
  result.resolved = true;
  result.object_uuid = kViewUuid;
  result.object_class = "view";
  result.resolution_detail = std::move(semantic);
  return result;
}

void TestCreateParseBindEncode() {
  const auto alias_route =
      fb::ParseFirebirdRelationProjectionViewCreateRoute(
          "create view v_test as select id, 5 as x from test;");
  Require(alias_route.recognized() && alias_route.view_name == "V_TEST" &&
              alias_route.source_relation == "TEST" &&
              alias_route.source_column == "ID" &&
              alias_route.source_output_name == "ID" &&
              alias_route.literal_output_name == "X" &&
              alias_route.literal_value == 5 &&
              !alias_route.explicit_output_names,
          "Firebird QA relation-view alias form did not parse exactly");

  const auto explicit_route =
      fb::ParseFirebirdRelationProjectionViewCreateRoute(
          "CREATE VIEW test (id,num) AS SELECT id,5 FROM tb;");
  Require(explicit_route.recognized() &&
              explicit_route.explicit_output_names &&
              explicit_route.view_name == "TEST" &&
              explicit_route.source_relation == "TB" &&
              explicit_route.source_output_name == "ID" &&
              explicit_route.literal_output_name == "NUM" &&
              explicit_route.literal_value == 5,
          "Firebird QA relation-view explicit-column form did not parse");

  const auto bound = fb::BindFirebirdRelationProjectionViewCreate(
      explicit_route, SchemaResolution(), TableResolution());
  Require(bound.accepted && bound.schema_uuid == kSchemaUuid &&
              bound.relation_uuid == kRelationUuid &&
              bound.relation_descriptor_uuid == kRelationDescriptorUuid &&
              bound.relation_descriptor_generation == 17 &&
              bound.validated_resource_epoch == 23 &&
              bound.source_column.column_uuid == kSourceColumnUuid &&
              bound.source_column.type_descriptor_uuid == kSourceTypeUuid,
          "relation-view create did not bind exact engine descriptors");

  const std::string envelope =
      fb::EncodeFirebirdRelationProjectionViewCreateEnvelope(bound);
  Require(!envelope.empty() &&
              envelope.find("\"operation_id\":\"ddl.create_view\"") !=
                  std::string::npos &&
              envelope.find("\"operation_family\":\"sblr.catalog.mutation.v3\"") !=
                  std::string::npos &&
              envelope.find("\"contains_sql_text\":false") !=
                  std::string::npos &&
              envelope.find(fb::kFirebirdRelationProjectionViewMarkerV1) !=
                  std::string::npos &&
              envelope.find("rpvc1|0|0|") != std::string::npos &&
              envelope.find("rpvc1|0|1|") != std::string::npos &&
              envelope.find(Hex("source_column")) != std::string::npos &&
              envelope.find(Hex("typed_int32_literal")) !=
                  std::string::npos &&
              envelope.find(Hex("5")) != std::string::npos &&
              envelope.find("SELECT") == std::string::npos &&
              envelope.find("sbsql") == std::string::npos,
          "rpvc1 create envelope leaked SQL/sibling state or lost typed data");
}

void TestCreatePreemptionAndRefusals() {
  for (const std::string_view sql : {
           "/* lead */ create view v as select id, 5 as x from test",
           "create /* ddl */ view v as select id, 5 as x from test",
           "create view v as /* query */ select id, 5 as x from test",
           "create view v as select id /* before from */, 5 as x from test",
           "create view v as select id, 5 as x /* FROM decoy */ from test",
           "create view v as select id, 5 as x from /* source */ test",
           "create view v as select id, 5 as x from test -- tail"}) {
    const auto route =
        fb::ParseFirebirdRelationProjectionViewCreateRoute(sql);
    Require(route.attempted && !route.recognized(),
            "commented relation-view intent did not preempt and fail closed");
  }

  for (const std::string_view sql : {
           "create view v as select id, 6 as x from test",
           "create view v as select id, -5 as x from test",
           "create view v as select id, 5 from test",
           "create view v (id,id) as select id, 5 from test",
           "create view v as select id, 5 as x from test where id > 0",
           "create view v as select id, 5 as x from test join t2 on 1=1",
           "create view \"v\" as select id, 5 as x from test",
           "create view v as select \"id\", 5 as x from test",
           "create view v as select id, 5 as \"x\" from test",
           "create view schema.v as select id, 5 as x from test"}) {
    const auto route =
        fb::ParseFirebirdRelationProjectionViewCreateRoute(sql);
    Require(!route.recognized(),
            "unsupported relation-view shape escaped the bounded parser");
  }

  const auto route = fb::ParseFirebirdRelationProjectionViewCreateRoute(
      "create view v as select id, 5 as x from test");
  Require(route.recognized(), "create refusal fixture did not parse");
  auto invalid_relation = TableResolution();
  invalid_relation.relation_descriptor.descriptor_uuid = kRelationUuid;
  Require(!fb::BindFirebirdRelationProjectionViewCreate(
               route, SchemaResolution(), invalid_relation)
               .accepted,
          "colliding relation/descriptor UUIDs were admitted");
  auto wrong_type = TableResolution();
  wrong_type.relation_descriptor.columns[0].canonical_type_name = "int64";
  Require(!fb::BindFirebirdRelationProjectionViewCreate(
               route, SchemaResolution(), wrong_type)
               .accepted,
          "non-int32 source descriptor was admitted");
  auto ambiguous = TableResolution();
  ambiguous.relation_descriptor.columns.push_back(
      ambiguous.relation_descriptor.columns.front());
  ambiguous.relation_descriptor.columns.back().column_uuid = kOutput0Uuid;
  Require(!fb::BindFirebirdRelationProjectionViewCreate(
               route, SchemaResolution(), ambiguous)
               .accepted,
          "ambiguous source-column binding was admitted");
}

void TestUpdatableCreateParseBindEncode() {
  const auto explicit_route =
      fb::ParseFirebirdRelationProjectionViewCreateV2Route(
          "CREATE VIEW test (id) AS SELECT id FROM tb;");
  Require(explicit_route.recognized() &&
              explicit_route.view_name == "TEST" &&
              explicit_route.explicit_output_name &&
              explicit_route.output_name == "ID" &&
              explicit_route.source_relation == "TB" &&
              explicit_route.source_column == "ID",
          "Firebird delete-03 one-column CREATE VIEW did not parse exactly");

  const auto implicit_route =
      fb::ParseFirebirdRelationProjectionViewCreateV2Route(
          "create view test as select id from tb;");
  Require(implicit_route.recognized() &&
              !implicit_route.explicit_output_name &&
              implicit_route.output_name == "ID",
          "implicit one-column updatable view output did not parse");

  const auto bound = fb::BindFirebirdRelationProjectionViewCreateV2(
      explicit_route, SchemaResolution(), TableResolution());
  Require(bound.accepted && bound.schema_uuid == kSchemaUuid &&
              bound.relation_uuid == kRelationUuid &&
              bound.relation_descriptor_uuid == kRelationDescriptorUuid &&
              bound.relation_descriptor_generation == 17 &&
              bound.validated_resource_epoch == 23 &&
              bound.source_column.column_uuid == kSourceColumnUuid &&
              bound.source_column.type_descriptor_uuid == kSourceTypeUuid &&
              bound.source_column.nullable,
          "rpvc2 did not bind the exact engine-issued source descriptor");

  const std::string envelope =
      fb::EncodeFirebirdRelationProjectionViewCreateV2Envelope(bound);
  const std::string expected_packet =
      "rpvc2|0|0|" + Hex(kRelationUuid) + "|" +
      Hex(kRelationDescriptorUuid) + "|17|23|" + Hex("ID") + "|" +
      Hex("ID") + "|" + Hex(kSourceColumnUuid) + "|" +
      Hex(kSourceTypeUuid) + "|" + Hex("scalar") + "|" +
      Hex("int32") + "|" +
      Hex("canonical=int32;precision=32;scale=0;nullable=true") +
      "|1|";
  Require(!envelope.empty() &&
              envelope.find("\"operation_id\":\"ddl.create_view\"") !=
                  std::string::npos &&
              envelope.find("\"operation_family\":\"sblr.catalog.mutation.v3\"") !=
                  std::string::npos &&
              envelope.find("\"contains_sql_text\":false") !=
                  std::string::npos &&
              envelope.find(fb::kFirebirdRelationProjectionViewMarkerV2) !=
                  std::string::npos &&
              envelope.find("\"view_projection_count\":\"1\"") !=
                  std::string::npos &&
              envelope.find("\"view_projection_0\":\"" +
                            expected_packet + "\"") !=
                  std::string::npos &&
              envelope.find(Hex(kRelationUuid)) != std::string::npos &&
              envelope.find(Hex(kRelationDescriptorUuid)) !=
                  std::string::npos &&
              envelope.find(Hex(kSourceColumnUuid)) != std::string::npos &&
              envelope.find("rpvc1") == std::string::npos &&
              envelope.find(fb::kFirebirdRelationProjectionViewMarkerV1) ==
                  std::string::npos &&
              envelope.find("SELECT") == std::string::npos &&
              envelope.find("select id") == std::string::npos &&
              envelope.find("sbsql") == std::string::npos,
          "rpvc2 leaked SQL/sibling/V1 state or lost exact descriptors");
}

void TestUpdatableCreateRefusals() {
  for (const std::string_view sql : {
           "/* lead */ create view test (id) as select id from tb",
           "create /* ddl */ view test (id) as select id from tb",
           "create view test (id) as /* query */ select id from tb",
           "create view test (id) as select id from /* source */ tb",
           "create view test (id) as select id from tb -- tail"}) {
    const auto route =
        fb::ParseFirebirdRelationProjectionViewCreateV2Route(sql);
    Require(route.attempted && !route.recognized(),
            "commented rpvc2 intent did not preempt and fail closed");
  }

  for (const std::string_view sql : {
           "create view test (id, x) as select id from tb",
           "create view test (id) as select id, 5 from tb",
           "create view test (id) as select id + 0 from tb",
           "create view test (id) as select id from tb where id = 10",
           "create view test (id) as select id from tb join t2 on 1=1",
           "create view \"test\" (id) as select id from tb",
           "create view test (\"id\") as select id from tb",
           "create view test (id) as select \"id\" from tb",
           "create view test (id) as select id from schema.tb"}) {
    Require(!fb::ParseFirebirdRelationProjectionViewCreateV2Route(sql)
                 .recognized(),
            "non-updatable CREATE VIEW shape escaped bounded rpvc2 parsing");
  }

  const auto route = fb::ParseFirebirdRelationProjectionViewCreateV2Route(
      "create view test (id) as select id from tb");
  Require(route.recognized(), "rpvc2 refusal fixture did not parse");

  auto wrong_schema = SchemaResolution();
  wrong_schema.object_class = "table";
  Require(!fb::BindFirebirdRelationProjectionViewCreateV2(
               route, wrong_schema, TableResolution())
               .accepted,
          "rpvc2 admitted a non-schema parent");

  auto invalid_relation = TableResolution();
  invalid_relation.relation_descriptor.descriptor_uuid = kRelationUuid;
  Require(!fb::BindFirebirdRelationProjectionViewCreateV2(
               route, SchemaResolution(), invalid_relation)
               .accepted,
          "rpvc2 admitted colliding relation/descriptor UUIDs");

  auto wrong_type = TableResolution();
  wrong_type.relation_descriptor.columns[0].canonical_type_name = "int64";
  Require(!fb::BindFirebirdRelationProjectionViewCreateV2(
               route, SchemaResolution(), wrong_type)
               .accepted,
          "rpvc2 admitted a non-int32 source descriptor");

  auto ambiguous = TableResolution();
  ambiguous.relation_descriptor.columns.push_back(
      ambiguous.relation_descriptor.columns.front());
  ambiguous.relation_descriptor.columns.back().column_uuid = kOutput0Uuid;
  Require(!fb::BindFirebirdRelationProjectionViewCreateV2(
               route, SchemaResolution(), ambiguous)
               .accepted,
          "rpvc2 admitted ambiguous source-column binding");
}

void TestUpdatableDeleteParseBindEncode() {
  const auto route = fb::ParseFirebirdRelationProjectionViewDeleteV2Route(
      "DELETE FROM test WHERE id=10;");
  Require(route.recognized() && route.view_name == "TEST" &&
              route.output_name == "ID" && route.predicate_value == 10,
          "Firebird delete-03 view DELETE did not parse exactly");

  const auto bound = fb::BindFirebirdRelationProjectionViewDeleteV2(
      route, UpdatableViewResolution());
  Require(bound.accepted && bound.view_uuid == kViewUuid &&
              bound.view_descriptor_uuid == kViewDescriptorUuid &&
              bound.view_descriptor_generation == 9 &&
              bound.output.ordinal == 0 && bound.output.name == "ID" &&
              bound.output.output_column_uuid == kOutput0Uuid &&
              bound.output.type_descriptor_uuid == kOutput0TypeUuid &&
              bound.output.canonical_type_name == "int32" &&
              bound.output.nullable &&
              bound.semantic_transport == UpdatableSemanticPacket(),
          "rpvd2 did not bind the exact source-opaque semantic output");

  const std::string envelope =
      fb::EncodeFirebirdRelationProjectionViewDeleteV2Envelope(bound);
  Require(!envelope.empty() &&
              envelope.find("\"operation_id\":\"dml.delete_rows\"") !=
                  std::string::npos &&
              envelope.find("\"contains_sql_text\":false") !=
                  std::string::npos &&
              envelope.find(fb::kFirebirdRelationProjectionViewMarkerV2) !=
                  std::string::npos &&
              envelope.find("rpvd2|") != std::string::npos &&
              envelope.find("\"projection_0\":\"" +
                            UpdatableSemanticPacket() + "\"") !=
                  std::string::npos &&
              envelope.find("column_equals") != std::string::npos &&
              envelope.find("predicate_column") != std::string::npos &&
              envelope.find("predicate_value") != std::string::npos &&
              envelope.find("\"predicate_value\":\"10\"") !=
                  std::string::npos &&
              envelope.find("\"predicate_value_type\":\"int32\"") !=
                  std::string::npos &&
              envelope.find(kViewUuid) != std::string::npos &&
              envelope.find(kRelationUuid) == std::string::npos &&
              envelope.find(kRelationDescriptorUuid) == std::string::npos &&
              envelope.find(kSourceColumnUuid) == std::string::npos &&
              envelope.find("rpvc1") == std::string::npos &&
              envelope.find("rpvs1") == std::string::npos &&
              envelope.find(fb::kFirebirdRelationProjectionViewMarkerV1) ==
                  std::string::npos &&
              envelope.find("DELETE FROM") == std::string::npos &&
              envelope.find("delete from") == std::string::npos &&
              envelope.find("sbsql") == std::string::npos,
          "rpvd2 leaked SQL/sibling/hidden-source state or lost predicate data");
}

void TestUpdatableDeleteRefusals() {
  for (const std::string_view sql : {
           "/* lead */ delete from test where id = 10",
           "delete /* dml */ from test where id = 10",
           "delete from /* target */ test where id = 10",
           "delete from test where /* predicate */ id = 10",
           "delete from test where id = 10 -- tail"}) {
    const auto route =
        fb::ParseFirebirdRelationProjectionViewDeleteV2Route(sql);
    Require(route.attempted && !route.recognized(),
            "commented rpvd2 intent did not preempt and fail closed");
  }

  for (const std::string_view sql : {
           "delete from test",
           "delete from test where id <> 10",
           "delete from test where id = 10 or id = 11",
           "delete from test where id = 10 returning id",
           "delete from \"test\" where id = 10",
           "delete from schema.test where id = 10",
           "delete from test where \"id\" = 10",
           "delete from test where id + 0 = 10"}) {
    Require(!fb::ParseFirebirdRelationProjectionViewDeleteV2Route(sql)
                 .recognized(),
            "unsupported view DELETE shape escaped bounded rpvd2 parsing");
  }

  const auto route = fb::ParseFirebirdRelationProjectionViewDeleteV2Route(
      "delete from test where id = 10");
  Require(route.recognized(), "rpvd2 semantic refusal fixture did not parse");

  for (std::string semantic : {
           UpdatableSemanticPacket("rpvd1"),
           UpdatableSemanticPacket(
               "rpvd2", fb::kFirebirdRelationProjectionViewMarkerV1),
           UpdatableSemanticPacket("rpvd2",
                                   fb::kFirebirdRelationProjectionViewMarkerV2,
                                   "OTHER"),
           UpdatableSemanticPacket("rpvd2",
                                   fb::kFirebirdRelationProjectionViewMarkerV2,
                                   "ID", kOutput0Uuid, kOutput0TypeUuid,
                                   "scalar", "int64"),
           UpdatableSemanticPacket("rpvd2",
                                   fb::kFirebirdRelationProjectionViewMarkerV2,
                                   "ID", kOutput0Uuid, kOutput0TypeUuid,
                                   "scalar", "int32",
                                   "canonical=int32;precision=32;scale=0;nullable=true",
                                   "1", "1"),
           UpdatableSemanticPacket("rpvd2",
                                   fb::kFirebirdRelationProjectionViewMarkerV2,
                                   "ID", kOutput0Uuid, kOutput0TypeUuid,
                                   "scalar", "int32",
                                   "canonical=int32;precision=32;scale=0;nullable=true",
                                   "1", "0", "2"),
           UpdatableSemanticPacket("rpvd2",
                                   fb::kFirebirdRelationProjectionViewMarkerV2,
                                   "ID", kViewDescriptorUuid),
           UpdatableSemanticPacket("rpvd2",
                                   fb::kFirebirdRelationProjectionViewMarkerV2,
                                   "ID", kOutput0Uuid, kOutput0Uuid),
           UpdatableSemanticPacket("rpvd2",
                                   fb::kFirebirdRelationProjectionViewMarkerV2,
                                   "ID", kOutput0Uuid, kOutput0TypeUuid,
                                   "scalar", "int32",
                                   "canonical=int32;precision=32;scale=0;nullable=true",
                                   "1", "0", "1", 0)}) {
    Require(!fb::BindFirebirdRelationProjectionViewDeleteV2(
                 route, UpdatableViewResolution(std::move(semantic)))
                 .accepted,
            "malformed/wrong rpvd2 semantic output was admitted");
  }

  auto wrong_class = UpdatableViewResolution();
  wrong_class.object_class = "table";
  Require(!fb::BindFirebirdRelationProjectionViewDeleteV2(route, wrong_class)
               .accepted,
          "rpvd2 admitted a non-view semantic resolution");
}

fb::FirebirdBoundRelationProjectionViewSelect TestSelectParseBindEncode() {
  const auto route = fb::ParseFirebirdRelationProjectionViewSelectRoute(
      "select * from test;");
  Require(route.recognized() && route.view_name == "TEST",
          "relation-view SELECT did not parse");
  for (const std::string_view sql : {
           "/* lead */ select * from test",
           "select /* projection */ * from test",
           "select * /* before from */ from test",
           "select * from /* source */ test",
           "select * from test -- tail"}) {
    const auto refused =
        fb::ParseFirebirdRelationProjectionViewSelectRoute(sql);
    Require(refused.attempted && !refused.recognized(),
            "commented relation-view SELECT did not fail closed");
  }
  for (const std::string_view sql : {
           "select id from test", "select * from test where id = 1",
           "select * from \"test\"", "select * from schema.test"}) {
    Require(!fb::ParseFirebirdRelationProjectionViewSelectRoute(sql)
                 .recognized(),
            "unsupported relation-view SELECT escaped bounded parsing");
  }

  const auto bound = fb::BindFirebirdRelationProjectionViewSelect(
      route, ViewResolution());
  Require(bound.accepted && bound.view_uuid == kViewUuid &&
              bound.view_descriptor_uuid == kViewDescriptorUuid &&
              bound.view_descriptor_generation == 9 &&
              bound.outputs.size() == 2 &&
              bound.outputs[0].ordinal == 0 &&
              bound.outputs[0].name == "ID" &&
              bound.outputs[0].nullable &&
              bound.outputs[1].ordinal == 1 &&
              bound.outputs[1].name == "NUM" &&
              !bound.outputs[1].nullable,
          "rpvs1 semantic descriptor did not bind exact ordered outputs");
  const std::string envelope =
      fb::EncodeFirebirdRelationProjectionViewSelectEnvelope(bound);
  Require(!envelope.empty() &&
              envelope.find("\"operation_id\":\"dml.select_rows\"") !=
                  std::string::npos &&
              envelope.find("\"contains_sql_text\":false") !=
                  std::string::npos &&
              envelope.find(fb::kFirebirdRelationProjectionViewMarkerV1) !=
                  std::string::npos &&
              envelope.find("rpvs1|") != std::string::npos &&
              envelope.find("select * from") == std::string::npos &&
              envelope.find("SELECT * FROM") == std::string::npos &&
              envelope.find("sbsql") == std::string::npos,
          "rpvs1 select envelope leaked SQL/sibling state or lost semantics");
  return bound;
}

void TestSelectSemanticRefusals() {
  const auto route = fb::ParseFirebirdRelationProjectionViewSelectRoute(
      "select * from test");
  Require(route.recognized(), "semantic refusal fixture did not parse");
  for (std::string semantic : {
           SemanticPacket("rpvs0"),
           std::string("rpvs1|00|00|1|2"),
           SemanticPacket("rpvs1", kOutput0Uuid, kOutput0TypeUuid,
                          kOutput0Uuid, kOutput1TypeUuid),
           SemanticPacket("rpvs1", kOutput0Uuid, kOutput0TypeUuid,
                          kOutput1Uuid, kOutput0TypeUuid),
           SemanticPacket("rpvs1", kOutput0Uuid, kOutput0TypeUuid,
                          kOutput1Uuid, kOutput1TypeUuid, "1")}) {
    Require(!fb::BindFirebirdRelationProjectionViewSelect(
                 route, ViewResolution(std::move(semantic)))
                 .accepted,
            "malformed/retired rpvs semantic descriptor was admitted");
  }
  auto wrong_class = ViewResolution();
  wrong_class.object_class = "table";
  Require(!fb::BindFirebirdRelationProjectionViewSelect(route, wrong_class)
               .accepted,
          "non-view semantic resolution was admitted");
}

std::string CompletePayload(std::size_t rows = 2,
                            bool include_all_evidence = true) {
  std::string payload =
      "operation_id=dml.select_rows\n"
      "result_kind=query_rowset\n"
      "row_count=" +
      std::to_string(rows) + "\n";
  if (rows >= 1) {
    payload += "row[0]=ID=3;NUM=5\n"
               "row_meta[0]=ID:int32:not_null;NUM:int32:not_null\n";
  }
  if (rows >= 2) {
    payload += "row[1]=ID=10;NUM=5\n"
               "row_meta[1]=ID:int32:not_null;NUM:int32:not_null\n";
  }
  if (include_all_evidence) {
    payload +=
        "evidence=dml_result_projection:relation_projection\n"
        "evidence=relation_projection_relation_scan:one_mga_visible_scan\n"
        "evidence=relation_projection_visible_rows_scanned:" +
        std::to_string(rows) + "\n"
        "evidence=relation_projection_output_count:2\n"
        "evidence=relation_projection_row_storage:none\n"
        "evidence=relation_projection_view_marker:" +
        std::string(fb::kFirebirdRelationProjectionViewMarkerV1) + "\n"
        "evidence=relation_projection_view_uuid:" +
        std::string(kViewUuid) + "\n"
        "evidence=relation_projection_view_descriptor_uuid:" +
        std::string(kViewDescriptorUuid) + "\n"
        "evidence=relation_projection_view_descriptor_generation:9\n"
        "evidence=relation_projection_view_expansion:engine_owned_sql_free\n"
        "evidence=relation_projection_view_parser_sql:false\n";
  }
  return payload;
}

std::vector<fb::FirebirdScalarProjectionWireRow> CompleteRows() {
  fb::FirebirdScalarProjectionWireRow first;
  first.cells.push_back({false, "3"});
  first.cells.push_back({false, "5"});
  fb::FirebirdScalarProjectionWireRow second;
  second.cells.push_back({false, "10"});
  second.cells.push_back({false, "5"});
  return {std::move(first), std::move(second)};
}

void TestWorkerCompletePacket(
    const fb::FirebirdBoundRelationProjectionViewSelect& binding) {
  std::string diagnostic;
  auto rows = CompleteRows();
  Require(fb::ValidateFirebirdRelationProjectionViewCompletePacket(
              binding.route, binding.outputs, binding.view_uuid,
              binding.view_descriptor_uuid,
              binding.view_descriptor_generation, CompletePayload(), &rows,
              2, false, &diagnostic),
          "valid complete engine relation-view packet was refused");

  auto int_alias_binding = binding;
  int_alias_binding.outputs[0].canonical_type_name = "int";
  int_alias_binding.outputs[0].encoded_type_descriptor =
      "type=int;nullable=true";
  int_alias_binding.outputs[1].name = "X";
  auto int_alias_rows = CompleteRows();
  int_alias_rows.resize(1);
  std::string int_alias_payload = CompletePayload(1);
  constexpr std::string_view kIntAliasRow = "row[0]=ID=3;NUM=5";
  constexpr std::string_view kIntAliasMetadata =
      "row_meta[0]=ID:int32:not_null;NUM:int32:not_null";
  const auto int_alias_row = int_alias_payload.find(kIntAliasRow);
  const auto int_alias_metadata = int_alias_payload.find(kIntAliasMetadata);
  Require(int_alias_row != std::string::npos &&
              int_alias_metadata != std::string::npos,
          "worker int-alias metadata fixture drifted");
  int_alias_payload.replace(
      int_alias_row, kIntAliasRow.size(), "row[0]=ID=3;X=5");
  int_alias_payload.replace(
      int_alias_payload.find(kIntAliasMetadata), kIntAliasMetadata.size(),
      "row_meta[0]=ID:int:not_null;X:int32:not_null");
  Require(fb::ValidateFirebirdRelationProjectionViewCompletePacket(
              int_alias_binding.route, int_alias_binding.outputs,
              int_alias_binding.view_uuid,
              int_alias_binding.view_descriptor_uuid,
              int_alias_binding.view_descriptor_generation,
              int_alias_payload, &int_alias_rows, 1, false, &diagnostic),
          "Firebird INT source metadata with INT32 literal was refused");

  auto mismatched_type_rows = CompleteRows();
  mismatched_type_rows.resize(1);
  std::string mismatched_type_payload = int_alias_payload;
  const auto mismatched_type =
      mismatched_type_payload.find("ID:int:not_null");
  Require(mismatched_type != std::string::npos,
          "worker true type-mismatch fixture drifted");
  mismatched_type_payload.replace(
      mismatched_type, 15, "ID:int64:not_null");
  Require(!fb::ValidateFirebirdRelationProjectionViewCompletePacket(
              int_alias_binding.route, int_alias_binding.outputs,
              int_alias_binding.view_uuid,
              int_alias_binding.view_descriptor_uuid,
              int_alias_binding.view_descriptor_generation,
              mismatched_type_payload, &mismatched_type_rows, 1, false,
              &diagnostic) &&
              mismatched_type_rows.empty() &&
              diagnostic == "engine_int32_relation_view_metadata_required",
          "true relation-view source metadata type mismatch was exposed");

  auto cursor_rows = CompleteRows();
  Require(!fb::ValidateFirebirdRelationProjectionViewCompletePacket(
              binding.route, binding.outputs, binding.view_uuid,
              binding.view_descriptor_uuid,
              binding.view_descriptor_generation, CompletePayload(),
              &cursor_rows, 2, true, &diagnostic) && cursor_rows.empty(),
          "cursor-bearing relation-view result was exposed");

  auto count_rows = CompleteRows();
  Require(!fb::ValidateFirebirdRelationProjectionViewCompletePacket(
              binding.route, binding.outputs, binding.view_uuid,
              binding.view_descriptor_uuid,
              binding.view_descriptor_generation, CompletePayload(),
              &count_rows, 1, false, &diagnostic) && count_rows.empty(),
          "relation-view server/payload row-count mismatch was exposed");

  auto evidence_rows = CompleteRows();
  Require(!fb::ValidateFirebirdRelationProjectionViewCompletePacket(
              binding.route, binding.outputs, binding.view_uuid,
              binding.view_descriptor_uuid,
              binding.view_descriptor_generation,
              CompletePayload(2, false), &evidence_rows, 2, false,
              &diagnostic) && evidence_rows.empty(),
          "relation-view packet without engine authority evidence was exposed");

  auto bad_value_rows = CompleteRows();
  bad_value_rows[0].cells[0].text = "03";
  std::string bad_value_payload = CompletePayload();
  const auto value_pos = bad_value_payload.find("ID=3;");
  Require(value_pos != std::string::npos,
          "worker invalid-value fixture drifted");
  bad_value_payload.replace(value_pos, 5, "ID=03;");
  Require(!fb::ValidateFirebirdRelationProjectionViewCompletePacket(
              binding.route, binding.outputs, binding.view_uuid,
              binding.view_descriptor_uuid,
              binding.view_descriptor_generation, bad_value_payload,
              &bad_value_rows, 2, false, &diagnostic) &&
              bad_value_rows.empty(),
          "non-canonical int32 relation-view value was exposed");

  auto null_literal_rows = CompleteRows();
  null_literal_rows[0].cells[1] = {true, ""};
  std::string null_payload = CompletePayload();
  const auto row_pos = null_payload.find("row[0]=ID=3;NUM=5");
  const auto meta_pos = null_payload.find(
      "row_meta[0]=ID:int32:not_null;NUM:int32:not_null");
  Require(row_pos != std::string::npos && meta_pos != std::string::npos,
          "worker NULL-literal fixture drifted");
  null_payload.replace(row_pos, 18, "row[0]=ID=3;NUM=");
  null_payload.replace(
      null_payload.find("NUM:int32:not_null", meta_pos), 18,
      "NUM:int32:null");
  Require(!fb::ValidateFirebirdRelationProjectionViewCompletePacket(
              binding.route, binding.outputs, binding.view_uuid,
              binding.view_descriptor_uuid,
              binding.view_descriptor_generation, null_payload,
              &null_literal_rows, 2, false, &diagnostic) &&
              null_literal_rows.empty(),
          "NULL literal output was exposed through non-null SQLDA");
}

}  // namespace

int main() {
  VerifyWorkerPhysicalSelectDescriptorBoundary();
  TestCreateParseBindEncode();
  TestCreatePreemptionAndRefusals();
  TestUpdatableCreateParseBindEncode();
  TestUpdatableCreateRefusals();
  TestUpdatableDeleteParseBindEncode();
  TestUpdatableDeleteRefusals();
  const auto binding = TestSelectParseBindEncode();
  TestSelectSemanticRefusals();
  TestWorkerCompletePacket(binding);
  return EXIT_SUCCESS;
}
