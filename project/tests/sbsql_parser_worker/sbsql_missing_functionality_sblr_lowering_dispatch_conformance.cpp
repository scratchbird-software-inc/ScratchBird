// Copyright (c) 2026 ScratchBird Software Inc.
//
// This Source Code Form is subject to the terms of the Mozilla Public
// License, v. 2.0. If a copy of the MPL was not distributed with this
// file, You can obtain one at https://mozilla.org/MPL/2.0/.
//
// SPDX-License-Identifier: MPL-2.0

#include "ast_model.hpp"
#include "bound_ast_model.hpp"
#include "sblr_admission.hpp"
#include "sblr_envelope.hpp"
#include "sblr_opcode_registry.hpp"
#include "sbsql_v3_binding_catalog.hpp"
#include "sbsql_v3_sblr_catalog.hpp"

#include <iostream>
#include <string>
#include <string_view>
#include <vector>

namespace ast = scratchbird::parser::ast;
namespace bound = scratchbird::parser::bound_ast;
namespace lowering = scratchbird::parser::lowering;
namespace server = scratchbird::server;
namespace v3_binding = scratchbird::parser::sbsql_v3_binding;
namespace v3_sblr = scratchbird::parser::sbsql_v3_sblr;
namespace engine_sblr = scratchbird::engine::sblr;

namespace {

bool Require(bool condition, std::string_view message) {
  if (condition) return true;
  std::cerr << message << "\n";
  return false;
}

ast::AstFamily AstFamilyFor(std::string_view command_family) {
  if (command_family == "sbsql.migration_management") return ast::AstFamily::kMigrationManagement;
  if (command_family == "sbsql.temporal_bitemporal" ||
      command_family == "sbsql.versioned_history_mutate") {
    return ast::AstFamily::kTemporalBitemporal;
  }
  if (command_family == "sbsql.structured_types" ||
      command_family == "sbsql.kv_structured_read" ||
      command_family == "sbsql.kv_structured_mutate") {
    return ast::AstFamily::kStructuredType;
  }
  if (command_family == "sbsql.ddl_catalog_gaps") return ast::AstFamily::kDdlCatalog;
  if (command_family == "sbsql.dml_upsert_variants") return ast::AstFamily::kDml;
  if (command_family == "sbsql.bulk_import_export" ||
      command_family == "sbsql.bulk_export") {
    return ast::AstFamily::kBulkImportExport;
  }
  if (command_family == "sbsql.native_system_variables" ||
      command_family == "sbsql.last_day_builtin") {
    return ast::AstFamily::kSystemVariable;
  }
  if (command_family == "sbsql.acceleration_management") return ast::AstFamily::kAcceleration;
  if (command_family == "sbsql.transaction_lock_compatibility" ||
      command_family == "sbsql.transaction") {
    return ast::AstFamily::kTransactionControl;
  }
  return ast::AstFamily::kCompatibilityRouteBackfill;
}

std::string RequiredRightFor(std::string_view command_family) {
  const auto profile = v3_binding::BindingProfileForCommandFamily(command_family);
  if (!profile.required_right.empty()) return profile.required_right;
  return "COMPATIBILITY_ROUTE_VALIDATE";
}

std::string ScopeModeFor(std::string_view command_family) {
  const auto profile = v3_binding::BindingProfileForCommandFamily(command_family);
  if (!profile.scope_mode.empty()) return profile.scope_mode;
  return "compatibility_scope";
}

std::string DescriptorProfileFor(std::string_view command_family) {
  const auto profile = v3_binding::BindingProfileForCommandFamily(command_family);
  if (!profile.descriptor_binding_profile.empty()) return profile.descriptor_binding_profile;
  return "compatibility_descriptor_profile";
}

bool ValidateRoute(const v3_sblr::CommandFamilySblrRoute& route) {
  bool ok = true;
  ok &= Require(!route.command_family.empty(), "route command family missing");
  ok &= Require(!route.canonical_operation_family.empty(), "canonical operation family missing");
  ok &= Require(!route.route_operation_family.empty(), "route operation family missing");
  ok &= Require(!route.operation_id.empty(), "operation id missing");
  ok &= Require(!route.sblr_opcode.empty(), "SBLR opcode missing");
  ok &= Require(!route.contains_raw_sql_text, "route allows raw SQL text");

  const bound::BindingContext context{
      "018f0000-0000-7000-8000-000000000011",
      "018f0000-0000-7000-8000-000000000012",
      "catalog-epoch-sbsql-miss-003",
      "registry-snapshot-sbsql-miss-003",
      "public_node"};
  const auto bound_evidence = bound::MakeBoundStatementFamilyEvidence(
      AstFamilyFor(route.command_family),
      "SBSQL-MISS-003",
      route.command_family,
      RequiredRightFor(route.command_family),
      ScopeModeFor(route.command_family),
      DescriptorProfileFor(route.command_family),
      route.operation_id,
      route.result_shape,
      context);
  const lowering::SblrRouteDescriptor lowering_route{
      route.canonical_operation_family,
      route.route_operation_family,
      route.operation_id,
      route.sblr_opcode,
      route.result_shape,
      route.diagnostic_shape,
      route.payload_class,
      route.requires_public_abi_dispatch,
      route.contains_raw_sql_text};
  const auto lowered = lowering::LowerBoundStatementFamilyEvidence(bound_evidence,
                                                                    lowering_route);
  ok &= Require(lowered.ok(),
                "bound statement family did not lower to a logical SBLR envelope");
  if (lowered.ok()) {
    const auto& envelope = std::get<lowering::LogicalEnvelope>(lowered.value);
    ok &= Require(envelope.operation_family == route.route_operation_family,
                  "lowered route operation family mismatch");
    ok &= Require(envelope.canonical_operation_family == route.canonical_operation_family,
                  "lowered canonical operation family mismatch");
    ok &= Require(envelope.operation_key == route.operation_id,
                  "lowered operation id mismatch");
    ok &= Require(envelope.sblr_opcode == route.sblr_opcode,
                  "lowered opcode mismatch");
    ok &= Require(!envelope.contains_raw_sql_text,
                  "lowered envelope contains raw SQL text");
  }

  const auto opcode_entry = v3_sblr::MakeOpcodeEntryForRoute(route);
  std::vector<std::string> opcode_errors;
  ok &= Require(v3_sblr::ValidateOpcodeEntry(opcode_entry, &opcode_errors),
                "parser SBLR opcode entry did not validate");
  for (const auto& error : opcode_errors) std::cerr << route.command_family << ": " << error << "\n";

  const auto envelope = v3_sblr::MakeEnvelopeForRoute(
      route,
      "binding-epoch-sbsql-miss-003",
      "018f0000-0000-7000-8000-000000000013",
      "descriptor-digest-sbsql-miss-003");
  std::vector<std::string> envelope_errors;
  ok &= Require(v3_sblr::ValidateEnvelope(opcode_entry, envelope, &envelope_errors),
                "parser SBLR envelope did not validate");
  for (const auto& error : envelope_errors) std::cerr << route.command_family << ": " << error << "\n";

  std::vector<std::string> decode_errors;
  const auto decoded = v3_sblr::DecodeEnvelopeForProbe(
      v3_sblr::EncodeEnvelopeForProbe(envelope), &decode_errors);
  ok &= Require(decoded.has_value(), "probe envelope did not decode");
  ok &= Require(decoded.has_value() && decoded->sblr_operation == route.sblr_opcode,
                "decoded probe envelope opcode mismatch");

  const auto* engine_entry = engine_sblr::LookupSblrOperation(route.operation_id);
  ok &= Require(engine_entry != nullptr,
                std::string("engine opcode registry missing operation ") + route.operation_id);
  if (engine_entry != nullptr) {
    ok &= Require(engine_entry->opcode == route.sblr_opcode,
                  std::string("engine opcode mismatch for ") + route.operation_id);
    ok &= Require(engine_entry->support == engine_sblr::SblrOpcodeSupport::implemented,
                  std::string("engine opcode is not implemented for ") + route.operation_id);
  }

  const server::ServerSblrAdmissionRequest request{
      v3_sblr::EncodeRouteForServerAdmission(route),
      false};
  const auto admission = server::AdmitServerSblrEnvelope(request);
  ok &= Require(admission.admitted,
                std::string("server rejected lowered route for ") + route.command_family);
  if (!admission.admitted) {
    for (const auto& diagnostic : admission.diagnostics) {
      std::cerr << route.command_family << ": " << diagnostic.code << " "
                << diagnostic.message_key << "\n";
    }
  } else {
    ok &= Require(admission.operation_family == route.route_operation_family,
                  "server admission family mismatch");
    ok &= Require(admission.operation_id == route.operation_id,
                  "server admission operation id mismatch");
  }

  auto raw_sql_payload = v3_sblr::EncodeRouteForServerAdmission(route);
  raw_sql_payload += "sql_text=SELECT 1\n";
  const auto raw_sql_admission = server::AdmitServerSblrEnvelope({raw_sql_payload, false});
  ok &= Require(!raw_sql_admission.admitted,
                "server admitted a lowered route carrying SQL text");
  return ok;
}

}  // namespace

int main() {
  bool ok = true;
  const auto routes = v3_sblr::RequiredMissingFunctionalityRoutes();
  ok &= Require(routes.size() >= 12, "missing-functionality SBLR route map is incomplete");
  for (const auto& route : routes) ok &= ValidateRoute(route);
  ok &= Require(v3_sblr::RouteForCommandFamily("sbsql.unallocated") == nullptr,
                "unallocated command family unexpectedly has a SBLR route");
  if (!ok) return 1;
  std::cout << "SBSQL missing-functionality SBLR lowering/dispatch contract passed\n";
  return 0;
}
