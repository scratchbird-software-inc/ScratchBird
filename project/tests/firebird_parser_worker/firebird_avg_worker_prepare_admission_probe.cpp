// Copyright (c) 2026 ScratchBird Software Inc.
//
// This Source Code Form is subject to the terms of the Mozilla Public
// License, v. 2.0. If a copy of the MPL was not distributed with this
// file, You can obtain one at https://mozilla.org/MPL/2.0/.
//
// SPDX-License-Identifier: MPL-2.0

#include "firebird_global_aggregate_projection.hpp"
#include "scratchbird/engine/sblr_envelope.hpp"
#include "sblr_admission.hpp"
#include "sblr_dispatch_server.hpp"

#include <cstdlib>
#include <iostream>
#include <string>
#include <string_view>
#include <utility>

namespace {

namespace fb = scratchbird::parser::firebird;
namespace ipc = scratchbird::parser::ipc;
namespace server = scratchbird::server;

[[noreturn]] void Fail(std::string_view message) {
  std::cerr << message << '\n';
  std::exit(EXIT_FAILURE);
}

void Require(bool condition, std::string_view message) {
  if (!condition) Fail(message);
}

ipc::PublicNameResolutionResult SchemaResolution() {
  ipc::PublicNameResolutionResult result;
  result.resolved = true;
  result.object_uuid = "019f2100-0000-7000-8a00-000000000001";
  result.object_class = "schema";
  return result;
}

ipc::PublicNameResolutionResult IntegerTableResolution() {
  ipc::PublicNameResolutionResult result;
  result.resolved = true;
  result.object_uuid = "019f2100-0000-7000-8a00-000000000002";
  result.object_class = "table";
  result.relation_descriptor.present = true;
  result.relation_descriptor.descriptor_uuid =
      "019f2100-0000-7000-8a00-000000000003";
  result.relation_descriptor.relation_uuid = result.object_uuid;
  result.relation_descriptor.descriptor_generation = 1;
  result.relation_descriptor.validated_resource_epoch = 1;

  ipc::PublicRelationColumnDescriptor id;
  id.column_uuid = "019f2100-0000-7000-8a00-000000000004";
  id.ordinal = 0;
  id.canonical_name_key = "ID";
  id.type_descriptor_uuid = "019f2100-0000-7000-8a00-000000000005";
  id.type_descriptor_kind = "scalar";
  // This is the exact persisted spelling published by CREATE TABLE ...
  // INTEGER, not a parser-invented int32 alias.
  id.canonical_type_name = "integer";
  id.encoded_type_descriptor = "type=integer;nullable=false";
  id.nullable = false;
  result.relation_descriptor.columns.push_back(std::move(id));
  return result;
}

std::string ReplaceAll(std::string value,
                       std::string_view from,
                       std::string_view to) {
  std::size_t offset = 0;
  while ((offset = value.find(from, offset)) != std::string::npos) {
    value.replace(offset, from.size(), to);
    offset += to.size();
  }
  return value;
}

void PrintDiagnostics(const server::ServerSblrAdmissionResult& result) {
  for (const auto& diagnostic : result.diagnostics) {
    std::cerr << diagnostic.code << ": " << diagnostic.safe_message << '\n';
  }
}

}  // namespace

int main() {
  constexpr std::string_view kExactIsqlSql =
      "create or alter view v_test as select "
      "avg(2100000000*id)as avg_result from test";

  const auto route =
      fb::ParseFirebirdGlobalAggregateViewCreateRoute(kExactIsqlSql);
  Require(route.recognized() && route.create_or_alter &&
              route.view_name == "V_TEST" &&
              route.source_relation == "TEST" &&
              route.source_column == "ID" &&
              route.int32_literal == 2100000000 &&
              route.result_alias == "AVG_RESULT",
          "exact Firebird isql AVG-view prepare shape did not parse");

  const auto bound = fb::BindFirebirdGlobalAggregateViewCreate(
      route, SchemaResolution(), IntegerTableResolution());
  Require(bound.accepted,
          "exact Firebird isql AVG-view prepare shape did not bind");

  const std::string parser_envelope =
      fb::EncodeFirebirdGlobalAggregateViewCreateEnvelope(bound);
  Require(!parser_envelope.empty() &&
              parser_envelope.find("\"operation_id\":\"ddl.create_view\"") !=
                  std::string::npos &&
              parser_envelope.find(
                  "\"operation_family\":\"sblr.catalog.mutation.v3\"") !=
                  std::string::npos &&
              parser_envelope.find("sblr.ddl.schema.v3") ==
                  std::string::npos &&
              parser_envelope.find("2100000000*id") ==
                  std::string::npos,
          "worker prepare envelope did not use the registered SQL-free "
          "catalog-mutation family");

  const auto admitted = server::AdmitServerSblrEnvelope(
      server::ServerSblrAdmissionRequest{parser_envelope, false});
  if (!admitted.admitted) PrintDiagnostics(admitted);
  Require(admitted.admitted &&
              admitted.operation_id == "ddl.create_view" &&
              admitted.operation_family == "sblr.catalog.mutation.v3" &&
              admitted.requires_public_abi_dispatch,
          "generic server prepare admission rejected the exact standalone "
          "Firebird AVG-view envelope");

  const std::string neutral_envelope =
      server::EncodeCreateViewPublicAbiEnvelopeForTest(parser_envelope);
  const auto decoded = scratchbird::engine::DecodeSblrEnvelopeBytes(
      reinterpret_cast<const std::uint8_t*>(neutral_envelope.data()),
      static_cast<std::uint64_t>(neutral_envelope.size()));
  Require(decoded.status == scratchbird::engine::SblrCodecStatus::ok &&
              decoded.envelope.payload_kind ==
                  scratchbird::engine::SblrPayloadKind::operation_envelope &&
              !decoded.envelope.canonical_bytes.empty(),
          "generic server bridge did not emit a public operation envelope");
  const std::string canonical(decoded.envelope.canonical_bytes.begin(),
                              decoded.envelope.canonical_bytes.end());
  Require(canonical.find(
              "sblr_operation_family=sblr.catalog.mutation.v3\n") !=
              std::string::npos &&
              canonical.find(
                  "operand=text\tview_projection_count\t1\n") !=
                  std::string::npos &&
              canonical.find(
                  "operand=text\tview_projection_0\tgavc1|") !=
                  std::string::npos,
          "generic server bridge did not preserve the admitted AVG-view "
          "prepare operands");

  // Keep the generic server strict: the correction belongs in the standalone
  // parser lowerer, not in a Firebird-specific server compatibility branch.
  const std::string legacy_envelope = ReplaceAll(
      parser_envelope, "sblr.catalog.mutation.v3", "sblr.ddl.schema.v3");
  const auto legacy = server::AdmitServerSblrEnvelope(
      server::ServerSblrAdmissionRequest{legacy_envelope, false});
  Require(!legacy.admitted && !legacy.diagnostics.empty() &&
              legacy.diagnostics.front().code ==
                  "PARSER_SERVER_IPC.SBLR_REVALIDATION_FAILED",
          "generic server admitted the retired unregistered DDL family");
  return EXIT_SUCCESS;
}
