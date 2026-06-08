// Copyright (c) 2026 ScratchBird Software Inc.
//
// This Source Code Form is subject to the terms of the Mozilla Public
// License, v. 2.0. If a copy of the MPL was not distributed with this
// file, You can obtain one at https://mozilla.org/MPL/2.0/.
//
// SPDX-License-Identifier: MPL-2.0

#include "api_types.hpp"
#include "sblr_dispatch.hpp"
#include "sblr_engine_envelope.hpp"

#include <cstdlib>
#include <iostream>
#include <string>
#include <string_view>

namespace {

namespace api = scratchbird::engine::internal_api;
namespace sblr = scratchbird::engine::sblr;

void Require(bool condition, std::string_view message) {
  if (!condition) {
    std::cerr << message << '\n';
    std::exit(EXIT_FAILURE);
  }
}

api::EngineRequestContext Context() {
  api::EngineRequestContext context;
  context.request_id = "synonym-dispatch-conformance";
  context.security_context_present = true;
  context.local_transaction_id = 77;
  context.transaction_uuid.canonical = "019f0000-0000-7000-8000-000000000401";
  context.database_uuid.canonical = "019f0000-0000-7000-8000-000000000402";
  context.current_schema_uuid.canonical = "019f0000-0000-7000-8000-000000000403";
  context.principal_uuid.canonical = "019f0000-0000-7000-8000-000000000404";
  return context;
}

sblr::SblrOperationEnvelope Envelope(std::string_view operation_id, std::string_view opcode) {
  auto envelope = sblr::MakeSblrEnvelope(std::string(operation_id), std::string(opcode), "trace.synonym.dispatch");
  envelope.requires_security_context = true;
  envelope.requires_transaction_context = true;
  envelope.contains_sql_text = false;
  envelope.parser_resolved_names_to_uuids = true;
  return envelope;
}

api::EngineApiRequest CreateSynonymRequest() {
  api::EngineApiRequest request;
  request.target_object.object_kind = "synonym";
  request.related_objects.push_back({{"019f0000-0000-7000-8000-000000000501"}, "table"});
  return request;
}

api::EngineApiRequest DropSynonymRequest() {
  api::EngineApiRequest request;
  request.target_object.uuid.canonical = "019f0000-0000-7000-8000-000000000601";
  request.target_object.object_kind = "synonym";
  return request;
}

void RequireDispatchAccepted(const sblr::SblrDispatchResult& result, std::string_view operation_id) {
  for (const auto& diagnostic : result.diagnostics) {
    std::cerr << diagnostic.code << ":" << diagnostic.message << '\n';
  }
  Require(result.envelope_validated, "synonym SBLR envelope failed validation before dispatch");
  Require(result.accepted, "synonym SBLR dispatch did not accept envelope");
  Require(result.dispatched_to_api, "synonym SBLR dispatch did not route to an engine API");
  Require(result.api_result.operation_id == operation_id, "synonym SBLR dispatch operation id mismatch");
}

}  // namespace

int main() {
  const sblr::SblrDispatchRequest create_request{
      Context(),
      Envelope("ddl.create_synonym", "SBLR_DDL_CREATE_SYNONYM"),
      CreateSynonymRequest()};
  RequireDispatchAccepted(sblr::DispatchSblrOperation(create_request), "ddl.create_synonym");

  const sblr::SblrDispatchRequest drop_request{
      Context(),
      Envelope("ddl.drop_object", "SBLR_DDL_DROP_OBJECT"),
      DropSynonymRequest()};
  RequireDispatchAccepted(sblr::DispatchSblrOperation(drop_request), "ddl.drop_object");

  std::cout << "sbsql_synonym_sblr_dispatch_conformance=passed\n";
  return EXIT_SUCCESS;
}
