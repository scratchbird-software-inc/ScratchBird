// Copyright (c) 2026 ScratchBird Software Inc.
//
// This Source Code Form is subject to the terms of the Mozilla Public
// License, v. 2.0. If a copy of the MPL was not distributed with this
// file, You can obtain one at https://mozilla.org/MPL/2.0/.
//
// SPDX-License-Identifier: MPL-2.0

#pragma once

#include <string>
#include <string_view>
#include <span>
#include <vector>

namespace scratchbird::parser::firebird {

struct Field {
  std::string name;
  std::string value;
};

struct Diagnostic {
  std::string code;
  std::string severity;
  std::string message;
  std::string component;
  std::vector<Field> fields;
};

struct ParseResult {
  bool ok{false};
  std::string normalized_sql;
  std::string statement_family;
  std::string operation_family;
  std::string lifecycle_operation_id;
  std::string sblr_operation;
  std::string sblr_operation_family;
  std::string engine_api_function;
  std::string lifecycle_mapping_key;
  std::string emulation_diagnostic_code;
  bool scratchbird_lifecycle_api{false};
  bool parser_support_udr_route{false};
  bool exact_emulated_diagnostic{false};
  bool real_firebird_file_effects{false};
  bool donor_engine_sql_executed{false};
  std::string sblr_envelope;
  std::string message_vector_json;
  std::string parser_evidence_json;
};

struct Token {
  std::string kind;
  std::string lexeme;
  std::size_t offset{0};
};

struct SurfaceDescriptor {
  std::string family;
  std::string surface;
  std::string owner;
};

enum class FirebirdMappingDisposition {
  kScratchBirdLifecycleApi,
  kParserSupportUdr,
  kEmulatedNonFileDiagnostic,
};

struct FirebirdLifecycleMappingDescriptor {
  std::string_view mapping_key;
  std::string_view command_family;
  FirebirdMappingDisposition disposition{FirebirdMappingDisposition::kScratchBirdLifecycleApi};
  std::string_view operation_id;
  std::string_view sblr_operation;
  std::string_view sblr_operation_family;
  std::string_view engine_api_function;
  std::string_view request_type;
  std::string_view result_type;
  std::string_view diagnostic_code;
  std::string_view diagnostic_message;
  bool requires_security_context{false};
  bool requires_transaction_context{false};
  bool produces_file_effects{false};
  bool donor_engine_sql_executed{false};
  bool exact_emulated_diagnostic{false};
};

std::string TrimAscii(std::string_view text);
std::string NormalizeWhitespace(std::string_view text);
std::string ToUpperAscii(std::string_view text);
std::string MessageVectorToJson(const std::vector<Diagnostic>& diagnostics);
std::vector<Token> LexTokens(std::string_view sql_text);
ParseResult ParseStatement(std::string_view sql_text);
bool IsNonFileEmulatedOperation(std::string_view normalized_upper_sql);
std::span<const FirebirdLifecycleMappingDescriptor> FirebirdLifecycleMappings();
const FirebirdLifecycleMappingDescriptor* MapFirebirdLifecycleCommand(std::string_view normalized_upper_sql);
const FirebirdLifecycleMappingDescriptor* FindFirebirdLifecycleMappingByOperationId(std::string_view operation_id);
std::string_view FirebirdMappingDispositionName(FirebirdMappingDisposition disposition);
const std::vector<SurfaceDescriptor>& DatatypeSurfaces();
const std::vector<SurfaceDescriptor>& BuiltinFunctionSurfaces();
const std::vector<SurfaceDescriptor>& CatalogOverlaySurfaces();
const std::vector<SurfaceDescriptor>& DiagnosticSurfaces();
std::string FirebirdPackageIdentityJson();
std::string FirebirdLifecycleMappingReportJson();

} // namespace scratchbird::parser::firebird
