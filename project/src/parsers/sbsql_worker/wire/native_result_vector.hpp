// Copyright (c) 2026 ScratchBird Software Inc.
//
// This Source Code Form is subject to the terms of the Mozilla Public
// License, v. 2.0. If a copy of the MPL was not distributed with this
// file, You can obtain one at https://mozilla.org/MPL/2.0/.
//
// SPDX-License-Identifier: MPL-2.0
#pragma once

// SEARCH_KEY: SB_NATIVE_RESULT_VECTOR_PARSER (S4h part 1; owner Q310: "the engine returns a message vector (int64 value plus descriptor UUIDs) and
// the parser builds the row").
//
// A NATIVE database answers a scalar projection (query.evaluate_projection) with result_kind "native_scalar_projection_vector": values and
// descriptor UUIDs only, NO column name and NO rendered row (the engine never picks a language). The PARSER builds the client-visible row:
//   * the column NAME comes from the statement's own alias, which the parser lowered (projection_N_name), never from the engine;
//   * the value comes from value[r.c] (a signed 64-bit little-endian carrier);
//   * the TYPE comes from the descriptor UUIDs, mapped through the compiled datatype cohort's identity table (type uuid, descriptor uuid and
//     descriptor generation must name exactly one row, and that row's codec must be the int64 codec); anything else is refused, not guessed.
// The result is the SAME packet shape the legacy engine rendered for the same statement (SBRES002 with operation_id, result_kind
// "scalar_projection_rows", row_count, row[i] and row_meta[i]), so every downstream decoder (the line wire, SBWP) is unchanged.
// A payload that is not a native vector is left alone (status not_native_vector): legacy rows pass through byte for byte.

#include <string>
#include <string_view>
#include <vector>

namespace scratchbird::parser::sbsql {

enum class NativeVectorConversionStatus { not_native_vector, converted, refused };

struct NativeVectorConversion {
  NativeVectorConversionStatus status{NativeVectorConversionStatus::not_native_vector};
  std::string payload;         // the legacy-shaped SBRES002 packet when converted
  std::string refusal_code;    // a machine token (no prose) when refused
  std::string refusal_detail;  // a machine token naming the field/step that failed
};

// `column_names` are the parser's own output names (one per result column, in order).
NativeVectorConversion ConvertNativeScalarProjectionVector(std::string_view engine_payload,
                                                           const std::vector<std::string>& column_names);

}  // namespace scratchbird::parser::sbsql
