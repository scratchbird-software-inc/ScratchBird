// Copyright (c) 2026 ScratchBird Software Inc.
//
// This Source Code Form is subject to the terms of the Mozilla Public
// License, v. 2.0. If a copy of the MPL was not distributed with this
// file, You can obtain one at https://mozilla.org/MPL/2.0/.
//
// SPDX-License-Identifier: MPL-2.0

#include "api_types.hpp"
#include "sblr_ddl_create_synonym_runtime.hpp"
#include "sblr_ddl_drop_synonym_runtime.hpp"
#include "sblr_dispatch.hpp"
#include "sblr_engine_envelope.hpp"
#include "sblr_opcode_registry.hpp"

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

sblr::SblrOperationEnvelope Envelope(std::string_view operation_id,
                                     std::string_view opcode,
                                     bool add_descriptor = true) {
  auto envelope = sblr::MakeSblrEnvelope(std::string(operation_id), std::string(opcode), "trace.synonym.dispatch");
  envelope.requires_security_context = true;
  envelope.requires_transaction_context = true;
  envelope.contains_sql_text = false;
  envelope.parser_resolved_names_to_uuids = true;
  envelope.opcode_code = opcode == "SBLR_DDL_DROP_SYNONYM" ? 1575 : 1574;
  envelope.result_shape = "ddl_result";
  envelope.diagnostic_shape = "diagnostic_vector";
  envelope.parser_package_uuid = "019f0000-0000-7000-8000-000000000411";
  envelope.registry_snapshot_uuid = "019f0000-0000-7000-8000-000000000412";
  if (!add_descriptor) return envelope;

  sblr::SblrOperand operand;
  operand.ordinal = 1;
  operand.name = "synonym";
  if (opcode == "SBLR_DDL_DROP_SYNONYM") {
    sblr::SblrDdlDropSynonymDescriptorV1 descriptor;
    descriptor.body[0] = 1;
    descriptor.availability = 1;
    operand.type = "drop_synonym_descriptor";
    operand.value_kind = sblr::SblrValueKind::drop_synonym_descriptor;
    operand.value_body =
        sblr::EncodeSblrDdlDropSynonymDescriptorV1(descriptor, true);
  } else {
    sblr::SblrDdlCreateSynonymDescriptorV1 descriptor;
    descriptor.body[0] = 1;
    descriptor.availability = 1;
    operand.type = "create_synonym_descriptor";
    operand.value_kind = sblr::SblrValueKind::create_synonym_descriptor;
    operand.value_body =
        sblr::EncodeSblrDdlCreateSynonymDescriptorV1(descriptor, true);
  }
  Require(!operand.value_body.empty(),
          "synonym test descriptor did not encode canonically");
  envelope.operands.push_back(std::move(operand));
  return envelope;
}

void RequireExecutorEvidenceRefusal(const sblr::SblrDispatchResult& result,
                                    std::string_view operation_id) {
  for (const auto& diagnostic : result.diagnostics) {
    std::cerr << diagnostic.code << ":" << diagnostic.message << '\n';
  }
  Require(result.envelope_validated, "synonym SBLR envelope failed validation before dispatch");
  Require(!result.accepted,
          "synonym SBLR dispatch bypassed missing executor evidence");
  Require(!result.dispatched_to_api,
          "synonym SBLR dispatch reached an engine API without evidence");
  Require(!result.api_result.ok,
          "synonym SBLR dispatch published a synthetic API success");
  Require(result.api_result.operation_id == operation_id, "synonym SBLR dispatch operation id mismatch");
  Require(!result.diagnostics.empty() &&
              result.diagnostics.front().code ==
                  "SBLR.OPCODE.EXECUTOR_EVIDENCE_MISSING",
          "synonym SBLR dispatch did not preserve the exact evidence refusal");
  Require(result.api_result.evidence.empty(),
          "synonym evidence refusal published engine evidence");
}

void RequireRegistryContract(std::string_view operation_id,
                             std::string_view opcode,
                             std::uint16_t code,
                             std::string_view operand_contract) {
  const auto* entry = sblr::LookupSblrOperation(operation_id);
  Require(entry != nullptr, "canonical synonym operation is absent from registry");
  Require(entry->operation_id == operation_id && entry->opcode == opcode &&
              entry->code == code,
          "canonical synonym operation identity drifted");
  Require(entry->operand_contract == operand_contract &&
              entry->result_contract == "ddl_result" &&
              entry->executor_id == operation_id,
          "canonical synonym descriptor or executor contract drifted");
  Require(entry->executor_evidence_required &&
              !entry->executor_evidence_accepted &&
              entry->missing_executor_evidence_diagnostic ==
                  "SBLR.OPCODE.EXECUTOR_EVIDENCE_MISSING",
          "synonym executor-evidence gate drifted");
}

}  // namespace

int main() {
  RequireRegistryContract("engine.op.ddl_create_synonym",
                          "SBLR_DDL_CREATE_SYNONYM", 1574,
                          "create_synonym_descriptor");
  RequireRegistryContract("engine.op.ddl_drop_synonym",
                          "SBLR_DDL_DROP_SYNONYM", 1575,
                          "drop_synonym_descriptor");
  Require(sblr::LookupSblrOperation("ddl.create_synonym") == nullptr &&
              sblr::LookupSblrOperation("ddl.synonym.drop") == nullptr,
          "retired synonym aliases remain registered as canonical operations");

  const sblr::SblrDispatchRequest create_request{
      Context(),
      Envelope("engine.op.ddl_create_synonym", "SBLR_DDL_CREATE_SYNONYM"),
      {}};
  RequireExecutorEvidenceRefusal(sblr::DispatchSblrOperation(create_request),
                                 "engine.op.ddl_create_synonym");

  const sblr::SblrDispatchRequest drop_request{
      Context(),
      Envelope("engine.op.ddl_drop_synonym", "SBLR_DDL_DROP_SYNONYM"),
      {}};
  RequireExecutorEvidenceRefusal(sblr::DispatchSblrOperation(drop_request),
                                 "engine.op.ddl_drop_synonym");

  auto descriptorless = create_request;
  descriptorless.envelope = Envelope("engine.op.ddl_create_synonym",
                                     "SBLR_DDL_CREATE_SYNONYM", false);
  const auto descriptorless_result =
      sblr::DispatchSblrOperation(descriptorless);
  Require(!descriptorless_result.envelope_validated &&
              !descriptorless_result.accepted &&
              !descriptorless_result.dispatched_to_api,
          "descriptorless CREATE SYNONYM reached executor-evidence admission");
  Require(!descriptorless_result.diagnostics.empty() &&
              descriptorless_result.diagnostics.front().code ==
                  "SBLR.OPERAND_INVALID",
          "descriptorless CREATE SYNONYM did not fail at operand validation");

  auto retired = create_request;
  retired.envelope.operation_id = "ddl.create_synonym";
  const auto retired_result = sblr::DispatchSblrOperation(retired);
  Require(!retired_result.envelope_validated && !retired_result.accepted &&
              !retired_result.dispatched_to_api,
          "retired CREATE SYNONYM alias reached dispatch");
  Require(!retired_result.diagnostics.empty() &&
              retired_result.diagnostics.front().code ==
                  "SBLR.OPERATION.OPCODE_IDENTITY_MISMATCH",
          "retired CREATE SYNONYM alias did not fail identity validation");

  // A synonym drop must not be admitted through the generic object opcode.
  // This prevents a parser or transport fallback from silently changing the
  // authority/result contract of the dedicated SBLR surface.
  auto malformed = drop_request;
  malformed.envelope.opcode = "SBLR_DDL_DROP_OBJECT";
  const auto malformed_result = sblr::DispatchSblrOperation(malformed);
  Require(!malformed_result.envelope_validated && !malformed_result.accepted &&
              !malformed_result.dispatched_to_api,
          "synonym drop accepted a generic drop-object opcode");

  std::cout << "sbsql_synonym_sblr_dispatch_conformance=passed\n";
  return EXIT_SUCCESS;
}
