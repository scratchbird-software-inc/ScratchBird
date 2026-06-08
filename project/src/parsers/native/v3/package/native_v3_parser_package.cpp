// Copyright (c) 2026 ScratchBird Software Inc.
//
// This Source Code Form is subject to the terms of the Mozilla Public
// License, v. 2.0. If a copy of the MPL was not distributed with this
// file, You can obtain one at https://mozilla.org/MPL/2.0/.
//
// SPDX-License-Identifier: MPL-2.0

#include "native_v3_parser_package.hpp"

#include "api_diagnostics.hpp"
#include "bound_ast_model.hpp"
#include "native_minimal_parser.hpp"
#include "sblr_dispatch.hpp"
#include "sblr_engine_envelope.hpp"
#include "sblr_envelope.hpp"

#include <sstream>
#include <string_view>
#include <utility>
#include <variant>

namespace scratchbird::parser::native_v3_package {
namespace {

namespace api = scratchbird::engine::internal_api;
namespace ast = scratchbird::parser::ast;
namespace bound = scratchbird::parser::bound_ast;
namespace lowering = scratchbird::parser::lowering;
namespace native = scratchbird::parser::native_v3;
namespace sblr = scratchbird::engine::sblr;

std::string JsonEscape(std::string_view input) {
  std::ostringstream out;
  for (const unsigned char ch : input) {
    switch (ch) {
      case '\\': out << "\\\\"; break;
      case '"': out << "\\\""; break;
      case '\b': out << "\\b"; break;
      case '\f': out << "\\f"; break;
      case '\n': out << "\\n"; break;
      case '\r': out << "\\r"; break;
      case '\t': out << "\\t"; break;
      default:
        if (ch < 0x20) {
          constexpr char kHex[] = "0123456789abcdef";
          out << "\\u00" << kHex[(ch >> 4) & 0x0f] << kHex[ch & 0x0f];
        } else {
          out << ch;
        }
    }
  }
  return out.str();
}

void Fail(NativeV3ParserPackageResult* result, std::string stage, std::string diagnostic) {
  result->ok = false;
  result->failed_stage = std::move(stage);
  result->diagnostics.push_back(std::move(diagnostic));
}

std::string OperationIdForLogicalKey(const std::string& operation_key) {
  if (operation_key == "op.show.version") { return "observability.show_version"; }
  if (operation_key == "op.show.database") { return "observability.show_database"; }
  return {};
}

std::string OpcodeForLogicalKey(const std::string& operation_key) {
  if (operation_key == "op.show.version") { return "sbv3.show.version"; }
  if (operation_key == "op.show.database") { return "sbv3.show.database"; }
  return {};
}

api::EngineRequestContext MakeEngineContext(const NativeV3ParserPackageRequest& request) {
  api::EngineRequestContext context;
  context.trust_mode = api::EngineTrustMode::server_isolated;
  context.request_id = "sbsql-parser-package";
  context.database_path = request.database_path;
  context.database_uuid.canonical = request.database_uuid;
  context.session_uuid.canonical = request.session_uuid;
  context.principal_uuid.canonical = request.principal_uuid;
  context.security_context_present = request.security_context_present;
  context.cluster_authority_available = false;
  context.trace_tags.push_back("parser_package_uuid:" + request.parser_package_uuid);
  context.trace_tags.push_back("parser_package_version:" + request.parser_package_version);
  return context;
}

api::EngineApiRequest MakeBaseApiRequest(const NativeV3ParserPackageRequest& request,
                                         const lowering::LogicalEnvelope& envelope) {
  api::EngineApiRequest api_request;
  api_request.operation_id = OperationIdForLogicalKey(envelope.operation_key);
  api_request.target_database.uuid.canonical = request.database_uuid;
  api_request.target_database.object_kind = "database";
  api_request.option_envelopes.push_back("parser_package_uuid:" + request.parser_package_uuid);
  api_request.option_envelopes.push_back("parser_package_version:" + request.parser_package_version);
  api_request.option_envelopes.push_back("registry_snapshot_uuid:" + request.registry_snapshot_uuid);
  return api_request;
}

sblr::SblrOperationEnvelope MakeEngineSblrEnvelope(const NativeV3ParserPackageRequest& request,
                                                   const lowering::LogicalEnvelope& envelope) {
  sblr::SblrOperationEnvelope engine_envelope =
      sblr::MakeSblrEnvelope(OperationIdForLogicalKey(envelope.operation_key),
                             OpcodeForLogicalKey(envelope.operation_key),
                             envelope.trace_key);
  engine_envelope.result_shape = envelope.result_shape;
  engine_envelope.diagnostic_shape = envelope.diagnostic_shape;
  engine_envelope.parser_package_uuid = request.parser_package_uuid;
  engine_envelope.registry_snapshot_uuid = request.registry_snapshot_uuid;
  engine_envelope.contains_sql_text = false;
  engine_envelope.parser_resolved_names_to_uuids = true;
  engine_envelope.requires_security_context = false;
  engine_envelope.requires_transaction_context = false;
  engine_envelope.requires_cluster_authority = false;
  for (const auto& operand : envelope.operands) {
    engine_envelope.operands.push_back({operand.type, operand.name, operand.value});
  }
  return engine_envelope;
}

api::EngineParserPackageRenderOptions RenderOptions(const NativeV3ParserPackageRequest& request) {
  api::EngineParserPackageRenderOptions options;
  options.parser_package_uuid = request.parser_package_uuid;
  options.parser_package_version = request.parser_package_version;
  options.client_dialect = request.client_dialect;
  options.language_tag = request.language_tag;
  options.redact_internal_detail = true;
  options.include_evidence = true;
  return options;
}

std::string RenderDiagnosticEnvelope(std::string operation_id, std::string code, std::string detail) {
  api::EngineApiResult failure;
  failure.ok = false;
  failure.operation_id = std::move(operation_id);
  failure.diagnostics.push_back(api::MakeEngineApiDiagnostic(std::move(code),
                                                             "parser.sbsql.package",
                                                             std::move(detail),
                                                             true));
  api::EngineParserPackageRenderOptions options;
  options.parser_package_uuid = "00000000-0000-7000-8000-000000000000";
  options.parser_package_version = "diagnostic-only";
  options.client_dialect = "sbsql_v3";
  const auto envelope = api::RenderEngineApiResultForParserPackage(failure, std::move(options));
  return envelope.diagnostics.empty() ? "" : envelope.diagnostics.front().code;
}

}  // namespace

NativeV3ParserPackageResult ExecuteNativeV3ParserPackageRequest(const NativeV3ParserPackageRequest& request) {
  NativeV3ParserPackageResult result;

  if (request.parser_package_uuid.empty()) {
    Fail(&result, "parser_package_context", "parser_package_uuid_required");
    return result;
  }
  if (request.parser_package_version.empty()) {
    Fail(&result, "parser_package_context", "parser_package_version_required");
    return result;
  }
  if (request.cluster_authority_available) {
    Fail(&result, "parser_package_context", "cluster_mapping_unavailable");
    return result;
  }

  native::ParseResult parse_result = native::ParseMinimalIdentityShow(request.command_text);
  if (!parse_result.ok()) {
    result.ast_json = native::SerializeParseResultToJson(parse_result);
    Fail(&result, "parse", "sbsql_parse_failed");
    return result;
  }

  ast::ShowIdentityAst parsed_ast = std::get<ast::ShowIdentityAst>(parse_result.value);
  parsed_ast.header.parser_package_uuid = request.parser_package_uuid;
  parsed_ast.header.parser_package_version = request.parser_package_version;
  parsed_ast.header.registry_snapshot_uuid = request.registry_snapshot_uuid;
  parsed_ast.header.diagnostic_context_id = "sbsql-parser-package";
  result.ast_json = ast::SerializeToJson(parsed_ast);

  bound::BindingContext binding_context;
  binding_context.database_uuid = request.database_uuid;
  binding_context.principal_uuid = request.principal_uuid;
  binding_context.catalog_epoch = request.catalog_epoch;
  binding_context.registry_snapshot_uuid = request.registry_snapshot_uuid;
  binding_context.package_profile = "public_node";
  const bound::BindResult bind_result = bound::BindShowIdentityAst(parsed_ast, binding_context);
  if (!bind_result.ok()) {
    result.bound_ast_json = bound::SerializeBindResultToJson(bind_result);
    Fail(&result, "bind", "sbsql_bind_failed");
    return result;
  }

  const auto& bound_ast = std::get<bound::BoundShowIdentity>(bind_result.value);
  result.bound_ast_json = bound::SerializeToJson(bound_ast);

  const lowering::LoweringResult lower_result = lowering::LowerBoundShowIdentity(bound_ast);
  if (!lower_result.ok()) {
    result.logical_envelope_json = lowering::SerializeLoweringResultToJson(lower_result);
    Fail(&result, "lower", "sbsql_lower_failed");
    return result;
  }

  const auto& logical_envelope = std::get<lowering::LogicalEnvelope>(lower_result.value);
  result.logical_envelope_json = lowering::SerializeToJson(logical_envelope);

  if (OperationIdForLogicalKey(logical_envelope.operation_key).empty()) {
    Fail(&result, "sblr_map", "sbsql_sblr_operation_unmapped");
    return result;
  }

  const sblr::SblrOperationEnvelope engine_envelope = MakeEngineSblrEnvelope(request, logical_envelope);
  result.sblr_contains_sql_text = engine_envelope.contains_sql_text;
  result.sblr_parser_resolved_names_to_uuids = engine_envelope.parser_resolved_names_to_uuids;
  result.sblr_envelope_json = sblr::SerializeSblrEnvelopeToJson(engine_envelope);

  const auto validation = sblr::ValidateSblrEnvelope(engine_envelope);
  result.sblr_envelope_validated = validation.ok;
  if (!validation.ok) {
    Fail(&result, "sblr_validate", "sbsql_sblr_validation_failed");
    return result;
  }

  sblr::SblrDispatchRequest dispatch_request;
  dispatch_request.context = MakeEngineContext(request);
  dispatch_request.envelope = engine_envelope;
  dispatch_request.api_request = MakeBaseApiRequest(request, logical_envelope);
  const sblr::SblrDispatchResult dispatch_result = sblr::DispatchSblrOperation(dispatch_request);
  result.dispatched_to_engine_api = dispatch_result.dispatched_to_api;
  result.dispatch_json = sblr::SerializeSblrDispatchResultToJson(dispatch_result);
  if (!dispatch_result.dispatched_to_api) {
    Fail(&result, "dispatch", RenderDiagnosticEnvelope(engine_envelope.operation_id,
                                                        "SBSQL_DISPATCH_FAILED",
                                                        "engine dispatch refused SBLR"));
    return result;
  }

  result.rendered_result = api::RenderEngineApiResultForParserPackage(dispatch_result.api_result,
                                                                      RenderOptions(request));
  result.rendered_for_parser_package = true;
  std::vector<std::string> render_errors;
  if (!api::ValidateEngineRenderedResultEnvelope(result.rendered_result, &render_errors)) {
    for (const auto& error : render_errors) { result.diagnostics.push_back(error); }
    Fail(&result, "render", "sbsql_render_validation_failed");
    return result;
  }

  result.ok = dispatch_result.api_result.ok && result.rendered_result.ok;
  if (!result.ok) { result.failed_stage = "engine_api"; }
  return result;
}

std::string SerializeNativeV3ParserPackageResultToJson(const NativeV3ParserPackageResult& result) {
  std::ostringstream out;
  out << "{\n";
  out << "  \"ok\": " << (result.ok ? "true" : "false") << ",\n";
  out << "  \"failed_stage\": \"" << JsonEscape(result.failed_stage) << "\",\n";
  out << "  \"parser_is_trusted\": " << (result.parser_is_trusted ? "true" : "false") << ",\n";
  out << "  \"sblr_contains_sql_text\": " << (result.sblr_contains_sql_text ? "true" : "false") << ",\n";
  out << "  \"sblr_parser_resolved_names_to_uuids\": " << (result.sblr_parser_resolved_names_to_uuids ? "true" : "false") << ",\n";
  out << "  \"sblr_envelope_validated\": " << (result.sblr_envelope_validated ? "true" : "false") << ",\n";
  out << "  \"dispatched_to_engine_api\": " << (result.dispatched_to_engine_api ? "true" : "false") << ",\n";
  out << "  \"rendered_for_parser_package\": " << (result.rendered_for_parser_package ? "true" : "false") << ",\n";
  out << "  \"rendered_operation_id\": \"" << JsonEscape(result.rendered_result.operation_id) << "\",\n";
  out << "  \"rendered_row_count\": " << result.rendered_result.rows.size() << ",\n";
  out << "  \"diagnostics\": [\n";
  for (std::size_t i = 0; i < result.diagnostics.size(); ++i) {
    out << "    \"" << JsonEscape(result.diagnostics[i]) << "\"";
    if (i + 1 != result.diagnostics.size()) { out << ","; }
    out << "\n";
  }
  out << "  ]\n";
  out << "}\n";
  return out.str();
}

}  // namespace scratchbird::parser::native_v3_package
