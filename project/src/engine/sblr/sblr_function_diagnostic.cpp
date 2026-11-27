// Copyright (c) 2026 ScratchBird Software Inc.
// SPDX-License-Identifier: MPL-2.0
#include "sblr_function_diagnostic.hpp"
#include "sblr_runtime.hpp"
#include "../internal_api/api_types.hpp"

namespace scratchbird::engine::sblr {
internal_api::EngineApiDiagnostic FunctionDiagnosticToApi(const SblrRuntimeDiagnostic& diagnostic) {
  // Explicit initialization bypasses the API record's fresh-occurrence default.
  // This is transport of the original source, never a second diagnostic emission.
  internal_api::EngineApiDiagnostic out{.occurrence_uuid = diagnostic.occurrence_uuid.bytes};
  out.code = diagnostic.diagnostic_id.empty() ? "SB_DIAG_FUNCTION_EXECUTION_FAILED" : diagnostic.diagnostic_id;
  out.message_key = diagnostic.message_key.empty() ? "engine.function.execution_failed" : diagnostic.message_key;
  out.detail = diagnostic.detail;
  out.error = diagnostic.severity != SblrDiagnosticSeverity::info;
  // Only the declared, bounded conversion parameter is eligible for the public
  // API. Private identity/security fields never become public text. Ambiguous
  // or malformed parameters are omitted, preserving the original failure.
  if (diagnostic.diagnostic_id == "SB_DIAG_FUNCTION_CONVERSION_INPUT") {
    const SblrDiagnosticField* parameter = nullptr;
    for (const auto& field : diagnostic.fields) {
      if (field.key != "conversion_input_text") continue;
      if (parameter != nullptr) return out;
      parameter = &field;
    }
    if (parameter != nullptr) {
      if (const auto* text = std::get_if<std::string>(&parameter->value);
          text != nullptr && !text->empty() && text->size() <= 1024 && text->find('\0') == std::string::npos) {
        out.fields.push_back({parameter->key, *text});
      }
    }
  }
  // CRYPTO.PGP.PROFILE_UNAVAILABLE (owner 2026-11-26, PGP 2 a): the four pgp_* functions are refused until OpenPGP is wired. The ONE public field is
  // `operation`, a closed token naming the refused function as DATA, taken only when exactly one candidate exists and its value is one of the four exact
  // tokens. function_id / function_uuid / package_name and every other field stay private; no argument-derived text can cross.
  if (diagnostic.diagnostic_id == "CRYPTO.PGP.PROFILE_UNAVAILABLE") {
    const SblrDiagnosticField* operation = nullptr;
    for (const auto& field : diagnostic.fields) {
      if (field.key != "operation") continue;
      if (operation != nullptr) return out;
      operation = &field;
    }
    if (operation != nullptr) {
      if (const auto* text = std::get_if<std::string>(&operation->value);
          text != nullptr && (*text == "pgp_sym_encrypt" || *text == "pgp_sym_decrypt" ||
                              *text == "pgp_pub_encrypt" || *text == "pgp_pub_decrypt")) {
        out.fields.push_back({operation->key, *text});
      }
    }
  }
  return out;
}
}
