// Copyright (c) 2026 ScratchBird Software Inc.
//
// This Source Code Form is subject to the terms of the Mozilla Public
// License, v. 2.0. If a copy of the MPL was not distributed with this
// file, You can obtain one at https://mozilla.org/MPL/2.0/.
//
// SPDX-License-Identifier: MPL-2.0

#pragma once

#include <cstdint>
#include <string>
#include <string_view>
#include <vector>

namespace scratchbird::parser::firebird {

enum class FirebirdScalarProjectionExpressionKind {
  kLiteral,
  kFunction,
};

enum class FirebirdScalarProjectionOutputKind {
  // Neutral engine result is one hex-encoded binary octet.  The Firebird wire
  // renderer presents it as SQL_TEXT(1), CHARACTER SET NONE.
  kBinaryOctet,
  // Neutral engine result is int64.  The Firebird wire renderer bounds and
  // presents compatible scalar results as SQL_SHORT.
  kInt16,
  // Neutral engine result is already-quantized exact fixed-decimal text.  The
  // Firebird wire renderer validates that text and presents its scaled INT16
  // carrier without performing decimal result arithmetic.
  kExactInt16,
  // Neutral engine result is int64.  The Firebird wire renderer bounds and
  // presents the compatible result as SQL_LONG.
  kInt32,
  // Neutral engine result is already-quantized exact fixed-decimal text.  The
  // Firebird wire renderer validates that text and presents its scaled INT32
  // carrier without performing temporal or decimal result arithmetic.
  kExactInt32,
  // Neutral engine result is int64.  The Firebird wire renderer presents
  // compatible scalar results as SQL_INT64.
  kInt64,
  // Neutral engine result is already-quantized exact fixed-decimal text.  The
  // Firebird wire renderer validates that text and presents its scaled INT64
  // carrier without performing arithmetic.
  kExactInt64,
  // Neutral engine result is text.  The Firebird wire renderer validates the
  // bounded byte length and presents it as right-padded SQL_TEXT, CHARACTER
  // SET NONE.  Padding is presentation-only and is never placed in the bound
  // expression sent to the engine.
  kFixedText,
  // Neutral engine result is text.  The Firebird wire renderer validates the
  // bounded byte length and presents it as SQL_VARYING, CHARACTER SET NONE.
  // The parser records only compatibility descriptor metadata; it does not derive or
  // evaluate the result text.
  kVaryingText,
  // Neutral engine result is real64.  The Firebird wire renderer presents
  // compatible scalar results as SQL_DOUBLE.
  kReal64,
  // Neutral engine result is canonical ISO date text.  The Firebird worker
  // validates it and presents SQL_DATE without evaluating the temporal
  // expression.
  kDate,
  // Neutral engine result is canonical ISO timestamp text.  The Firebird
  // worker validates it and presents SQL_TIMESTAMP without evaluating the
  // temporal expression.
  kTimestamp,
  // Neutral engine result is canonical Firebird-precision time text.  The
  // worker validates it and presents SQL_TIME without evaluating the temporal
  // expression.
  kTime,
};

struct FirebirdScalarProjectionBindOptions {
  // The attachment charset controls untyped Firebird string literals.  It is
  // presentation/binding metadata only and never authorizes parser execution.
  std::string attachment_charset;
};

// Bound neutral expression IR for the finite Firebird constant scalar tranche.
// It contains typed literal inputs and neutral function identities only.  No
// result value is computed by the parser.
struct FirebirdScalarProjectionExpression {
  FirebirdScalarProjectionExpressionKind kind{
      FirebirdScalarProjectionExpressionKind::kLiteral};
  std::string type_name;
  std::string encoded_value;
  bool is_null{false};
  std::string function_id;
  std::vector<FirebirdScalarProjectionExpression> arguments;
};

struct FirebirdScalarProjectionItem {
  std::string result_name;
  FirebirdScalarProjectionOutputKind output_kind{
      FirebirdScalarProjectionOutputKind::kInt16};
  bool nullable{false};
  // Genuine Firebird SQLDA numeric scale.  Deriving this from a bound literal
  // is parser metadata work; it never authorizes parser-side result evaluation.
  std::int16_t scale{0};
  // Genuine Firebird SQLDA numeric subtype (NUMERIC = 1, DECIMAL = 2).
  std::int16_t subtype{0};
  // Firebird SQLDA declared byte length for bounded character results.  Zero
  // is required for non-character output kinds.
  std::uint32_t declared_length{0};
  FirebirdScalarProjectionExpression expression;
};

struct FirebirdScalarProjectionRoute {
  std::vector<FirebirdScalarProjectionItem> items;

  [[nodiscard]] bool recognized() const { return !items.empty(); }
};

// Recognizes only the bounded constant ASCII_CHAR/ASCII_VAL/ABS/PI projection
// shapes, the exact admitted HASH('toto'), string-to-INTEGER, numeric-literal,
// and temporal CAST profiles (including exact inputs that the neutral engine
// must reject), the explicitly admitted literal-only numeric/bit/string
// shapes, and the finite temporal shapes used by the admitted Firebird QA
// tranches. The row source must be exactly
// RDB$DATABASE.  Unsupported expressions and query clauses fail closed.
FirebirdScalarProjectionRoute ParseFirebirdScalarProjectionRoute(
    std::string_view firebird_sql,
    FirebirdScalarProjectionBindOptions options = {});

// Serializes the bound expression tree onto the engine-native neutral scalar
// projection route.  This never includes source SQL and never evaluates a
// Firebird scalar result in the parser.
std::string EncodeFirebirdScalarProjectionEnvelope(
    const FirebirdScalarProjectionRoute& route);

}  // namespace scratchbird::parser::firebird
