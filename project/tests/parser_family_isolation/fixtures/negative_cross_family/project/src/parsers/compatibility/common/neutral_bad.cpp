// Copyright (c) 2026 ScratchBird Software Inc.
// SPDX-License-Identifier: MPL-2.0

namespace scratchbird::parser::beta {}
namespace neutral_beta_alias = scratchbird::parser::beta;

int ParseStatement(const char* sql_text) {
  return sql_text == nullptr ? 0 : 1;
}

const char* RenderSemanticEvidenceJson(const char* dialect_id) {
  return dialect_id;
}

int NeutralDispatch() {
  return scratchbird::parser::beta::Parse();
}

const char* NeutralSemanticDispatch(const char* dialect_id) {
  if (dialect_id == "beta") {
    return "beta.transaction_semantic_policy";
  }
  return "none";
}
