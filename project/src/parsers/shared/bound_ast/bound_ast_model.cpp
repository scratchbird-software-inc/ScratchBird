// Copyright (c) 2026 ScratchBird Software Inc.
//
// This Source Code Form is subject to the terms of the Mozilla Public
// License, v. 2.0. If a copy of the MPL was not distributed with this
// file, You can obtain one at https://mozilla.org/MPL/2.0/.
//
// SPDX-License-Identifier: MPL-2.0

#include "bound_ast_model.hpp"

#include <sstream>

namespace scratchbird::parser::bound_ast {
namespace {

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

BindingDiagnostic Error(std::string code,
                        std::string message,
                        scratchbird::parser::ast::SourceRange source_range) {
  return BindingDiagnostic{std::move(code), std::move(message), source_range};
}

std::string OperationKeyFor(scratchbird::parser::ast::ShowIdentityKind kind) {
  switch (kind) {
    case scratchbird::parser::ast::ShowIdentityKind::kVersion:
      return "op.show.version";
    case scratchbird::parser::ast::ShowIdentityKind::kDatabase:
      return "op.show.database";
    case scratchbird::parser::ast::ShowIdentityKind::kSystem:
      return "op.show.system";
    case scratchbird::parser::ast::ShowIdentityKind::kCapabilities:
      return "op.show.capabilities";
  }
  return "op.unknown";
}

std::string ResultShapeFor(scratchbird::parser::ast::ShowIdentityKind kind) {
  switch (kind) {
    case scratchbird::parser::ast::ShowIdentityKind::kVersion:
      return "rs.show.version.v1";
    case scratchbird::parser::ast::ShowIdentityKind::kDatabase:
      return "rs.show.database.v1";
    case scratchbird::parser::ast::ShowIdentityKind::kSystem:
      return "rs.show.system.v1";
    case scratchbird::parser::ast::ShowIdentityKind::kCapabilities:
      return "rs.show.capabilities.v1";
  }
  return "rs.generic.refusal.v1";
}

std::string TraceKeyFor(scratchbird::parser::ast::ShowIdentityKind kind) {
  switch (kind) {
    case scratchbird::parser::ast::ShowIdentityKind::kVersion:
      return "SBSQL-SURFACE-TRACE-SHOW-VERSION";
    case scratchbird::parser::ast::ShowIdentityKind::kDatabase:
      return "SBSQL-SURFACE-TRACE-SHOW-DATABASE";
    case scratchbird::parser::ast::ShowIdentityKind::kSystem:
      return "SBSQL-SURFACE-TRACE-SHOW-SYSTEM";
    case scratchbird::parser::ast::ShowIdentityKind::kCapabilities:
      return "SBSQL-SURFACE-TRACE-SHOW-CAPABILITIES";
  }
  return "SBSQL-SURFACE-TRACE-UNKNOWN";
}

}  // namespace

bool BindResult::ok() const {
  return std::holds_alternative<BoundShowIdentity>(value);
}

BindResult BindShowIdentityAst(const scratchbird::parser::ast::ShowIdentityAst& ast,
                               const BindingContext& context) {
  if (context.database_uuid.empty()) {
    return BindResult{Error("SBB_DATABASE_UUID_MISSING", "binding context is missing database UUID",
                            ast.header.source_range)};
  }
  if (context.principal_uuid.empty()) {
    return BindResult{Error("SBB_PRINCIPAL_UUID_MISSING", "binding context is missing principal UUID",
                            ast.header.source_range)};
  }
  if (context.package_profile != "public_node" && context.package_profile != "private_cluster" &&
      context.package_profile != "dev_only" && context.package_profile != "test_only") {
    return BindResult{Error("SBB_UNSUPPORTED_PACKAGE_PROFILE", "unsupported package profile",
                            ast.header.source_range)};
  }

  BoundShowIdentity bound;
  bound.show_kind = ast.show_kind;
  bound.header.surface_key = ast.header.surface_key_candidate;
  bound.header.command_family = "sbsql.identity_session";
  bound.header.database_uuid = context.database_uuid;
  bound.header.principal_uuid = context.principal_uuid;
  bound.header.catalog_epoch = context.catalog_epoch;
  bound.header.registry_snapshot_uuid = context.registry_snapshot_uuid;
  bound.header.required_right = "CONNECT";
  bound.header.scope_mode = "baseline";
  bound.header.edition_gate_result = "allowed";
  bound.header.profile_gate_result = "allowed";
  bound.header.sblr_operation_key = OperationKeyFor(ast.show_kind);
  bound.header.result_shape = ResultShapeFor(ast.show_kind);
  bound.header.diagnostic_shape = "diag.generic.success.v1";
  bound.header.trace_key = TraceKeyFor(ast.show_kind);
  return BindResult{std::move(bound)};
}

std::string SerializeToJson(const BoundShowIdentity& bound) {
  std::ostringstream out;
  out << "{\n";
  out << "  \"bound_ast_format_version\": " << bound.header.bound_ast_format_version << ",\n";
  out << "  \"bound_ast_node\": \"BoundShowIdentity\",\n";
  out << "  \"surface_key\": \"" << JsonEscape(bound.header.surface_key) << "\",\n";
  out << "  \"command_family\": \"" << JsonEscape(bound.header.command_family) << "\",\n";
  out << "  \"database_uuid\": \"" << JsonEscape(bound.header.database_uuid) << "\",\n";
  out << "  \"principal_uuid\": \"" << JsonEscape(bound.header.principal_uuid) << "\",\n";
  out << "  \"catalog_epoch\": \"" << JsonEscape(bound.header.catalog_epoch) << "\",\n";
  out << "  \"registry_snapshot_uuid\": \"" << JsonEscape(bound.header.registry_snapshot_uuid) << "\",\n";
  out << "  \"required_right\": \"" << JsonEscape(bound.header.required_right) << "\",\n";
  out << "  \"scope_mode\": \"" << JsonEscape(bound.header.scope_mode) << "\",\n";
  out << "  \"edition_gate_result\": \"" << JsonEscape(bound.header.edition_gate_result) << "\",\n";
  out << "  \"profile_gate_result\": \"" << JsonEscape(bound.header.profile_gate_result) << "\",\n";
  out << "  \"sblr_operation_key\": \"" << JsonEscape(bound.header.sblr_operation_key) << "\",\n";
  out << "  \"result_shape\": \"" << JsonEscape(bound.header.result_shape) << "\",\n";
  out << "  \"diagnostic_shape\": \"" << JsonEscape(bound.header.diagnostic_shape) << "\",\n";
  out << "  \"trace_key\": \"" << JsonEscape(bound.header.trace_key) << "\",\n";
  out << "  \"fields\": {\n";
  out << "    \"show_kind\": \"" << JsonEscape(scratchbird::parser::ast::ToString(bound.show_kind)) << "\"\n";
  out << "  }\n";
  out << "}\n";
  return out.str();
}

std::string SerializeDiagnosticToJson(const BindingDiagnostic& diagnostic) {
  std::ostringstream out;
  out << "{\n";
  out << "  \"diagnostic_code\": \"" << JsonEscape(diagnostic.code) << "\",\n";
  out << "  \"phase\": \"binding\",\n";
  out << "  \"message\": \"" << JsonEscape(diagnostic.message) << "\",\n";
  out << "  \"source_range\": {\"start_byte\": " << diagnostic.source_range.start_byte
      << ", \"end_byte\": " << diagnostic.source_range.end_byte << "}\n";
  out << "}\n";
  return out.str();
}

std::string SerializeBindResultToJson(const BindResult& result) {
  if (const auto* bound = std::get_if<BoundShowIdentity>(&result.value)) {
    return SerializeToJson(*bound);
  }
  return SerializeDiagnosticToJson(std::get<BindingDiagnostic>(result.value));
}

}  // namespace scratchbird::parser::bound_ast
