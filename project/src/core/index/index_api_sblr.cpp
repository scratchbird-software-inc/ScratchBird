// Copyright (c) 2026 ScratchBird Software Inc.
//
// This Source Code Form is subject to the terms of the Mozilla Public
// License, v. 2.0. If a copy of the MPL was not distributed with this
// file, You can obtain one at https://mozilla.org/MPL/2.0/.
//
// SPDX-License-Identifier: MPL-2.0

#include "index_api_sblr.hpp"

#include <utility>

namespace scratchbird::core::index {
namespace {
using scratchbird::core::platform::DiagnosticArgument;
using scratchbird::core::platform::MakeDiagnostic;
using scratchbird::core::platform::Severity;
using scratchbird::core::platform::StatusCode;
using scratchbird::core::platform::Subsystem;

Status OkStatus() { return {StatusCode::ok, Severity::info, Subsystem::engine}; }
Status RefuseStatus() { return {StatusCode::platform_required_feature_missing, Severity::error, Subsystem::engine}; }

bool Mutating(IndexCanonicalOperation operation) {
  return operation == IndexCanonicalOperation::create || operation == IndexCanonicalOperation::alter ||
         operation == IndexCanonicalOperation::drop || operation == IndexCanonicalOperation::rebuild ||
         operation == IndexCanonicalOperation::rebalance || operation == IndexCanonicalOperation::refresh ||
         operation == IndexCanonicalOperation::move ||
         operation == IndexCanonicalOperation::repair_index_family ||
         operation == IndexCanonicalOperation::rebuild_index_family ||
         operation == IndexCanonicalOperation::discard_unpublished_index_state;
}

IndexValidationRepairOperation ValidationRepairOperationFor(
    IndexCanonicalOperation operation) {
  switch (operation) {
    case IndexCanonicalOperation::repair_index_family:
      return IndexValidationRepairOperation::repair;
    case IndexCanonicalOperation::rebuild_index_family:
    case IndexCanonicalOperation::rebuild:
      return IndexValidationRepairOperation::rebuild;
    case IndexCanonicalOperation::discard_unpublished_index_state:
      return IndexValidationRepairOperation::discard_unpublished;
    case IndexCanonicalOperation::validate_index_family:
    case IndexCanonicalOperation::verify:
    case IndexCanonicalOperation::create:
    case IndexCanonicalOperation::alter:
    case IndexCanonicalOperation::drop:
    case IndexCanonicalOperation::rebalance:
    case IndexCanonicalOperation::refresh:
    case IndexCanonicalOperation::move:
    case IndexCanonicalOperation::query_candidates:
    case IndexCanonicalOperation::explain:
    case IndexCanonicalOperation::reference_catalog_projection:
    case IndexCanonicalOperation::unsupported_reference_feature:
      return IndexValidationRepairOperation::validate;
  }
  return IndexValidationRepairOperation::validate;
}
}  // namespace

IndexApiPlan BindIndexSblrOperation(const IndexSblrOperationEnvelope& envelope) {
  IndexApiPlan plan;
  if (envelope.envelope_version != 1 || envelope.contains_sql_text || !envelope.names_resolved_to_uuids) {
    plan.status = RefuseStatus();
    plan.diagnostic = MakeIndexApiSblrDiagnostic(plan.status,
                                                 "SB-INDEX-SBLR-AUTHORITY-VIOLATION",
                                                 "index.sblr.authority_violation");
    return plan;
  }
  if (!envelope.index_uuid.valid() || envelope.family == IndexFamily::unknown) {
    plan.status = RefuseStatus();
    plan.diagnostic = MakeIndexApiSblrDiagnostic(plan.status,
                                                 "SB-INDEX-SBLR-INVALID-ENVELOPE",
                                                 "index.sblr.invalid_envelope");
    return plan;
  }
  if (envelope.operation == IndexCanonicalOperation::unsupported_reference_feature) {
    plan.status = RefuseStatus();
    plan.policy_blocked = true;
    plan.diagnostic = MakeIndexApiSblrDiagnostic(plan.status,
                                                 "SB-INDEX-SBLR-UNSUPPORTED-REFERENCE-FEATURE",
                                                 "index.sblr.unsupported_reference_feature",
                                                 envelope.reference_command);
    return plan;
  }
  plan.status = OkStatus();
  plan.admitted = true;
  plan.mutates_state = Mutating(envelope.operation);
  plan.reference_catalog_projection = envelope.operation == IndexCanonicalOperation::reference_catalog_projection;
  plan.emulated = envelope.parser_surface_is_reference;
  plan.parser_shaping_required = true;
  plan.steps.push_back("validate_sblr_envelope_uuid_authority");
  plan.steps.push_back("bind_canonical_index_operation");
  plan.steps.push_back(envelope.parser_surface_is_reference ? "record_reference_projection_context" : "record_sbsql_context");
  return plan;
}

IndexValidationRepairResult BindIndexValidationRepairSblrOperation(
    const IndexSblrOperationEnvelope& envelope,
    IndexValidationRepairRequest request) {
  const auto api_plan = BindIndexSblrOperation(envelope);
  if (!api_plan.ok()) {
    IndexValidationRepairResult result;
    result.status = api_plan.status;
    result.classification = IndexValidationRepairClass::refused;
    result.fail_closed = true;
    result.diagnostic = api_plan.diagnostic;
    result.actions.push_back("sblr_validation_repair_route_refused");
    result.support_evidence.push_back({"route_surface", "sblr", false});
    result.support_evidence.push_back({"message_vector_key",
                                       result.diagnostic.message_key,
                                       false});
    return result;
  }
  request.operation = ValidationRepairOperationFor(envelope.operation);
  request.validation_family = envelope.validation_family;
  request.target.index_uuid = envelope.index_uuid;
  request.target.physical_family = envelope.family;
  request.target.names_resolved_to_uuids = envelope.names_resolved_to_uuids;
  request.target.contains_sql_text = envelope.contains_sql_text;
  auto result = ExecuteIndexValidationRepairOperation(request);
  result.actions.insert(result.actions.begin(),
                        "sblr_route_bound_to_engine_index_validation_repair");
  result.support_evidence.push_back({"route_surface", "sblr", false});
  return result;
}

IndexApiPlan MapReferenceIndexCommandToCanonicalOperation(const IndexSblrOperationEnvelope& envelope) {
  IndexSblrOperationEnvelope mapped = envelope;
  mapped.parser_surface_is_reference = true;
  mapped.names_resolved_to_uuids = true;
  mapped.contains_sql_text = false;
  if (mapped.reference_command.empty()) {
    mapped.operation = IndexCanonicalOperation::unsupported_reference_feature;
  } else if (mapped.reference_command.find("show") != std::string::npos ||
             mapped.reference_command.find("catalog") != std::string::npos) {
    mapped.operation = IndexCanonicalOperation::reference_catalog_projection;
  } else {
    mapped.operation = envelope.operation;
  }
  return BindIndexSblrOperation(mapped);
}

DiagnosticRecord MakeIndexApiSblrDiagnostic(Status status,
                                            std::string diagnostic_code,
                                            std::string message_key,
                                            std::string detail) {
  std::vector<DiagnosticArgument> arguments;
  if (!detail.empty()) {
    arguments.push_back({"detail", std::move(detail)});
  }
  return MakeDiagnostic(status.code, status.severity, status.subsystem,
                        std::move(diagnostic_code), std::move(message_key),
                        std::move(arguments), {}, "core.index.api_sblr");
}

}  // namespace scratchbird::core::index
