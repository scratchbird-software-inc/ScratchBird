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

#include <cstdint>
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

// SEARCH_KEY: SB_NATIVE_TRANSACTION_BOUNDARY_PARSER (S5). A NATIVE database answers COMMIT / ROLLBACK with result_kind
// "native_transaction_boundary": ended_order (u64), ended_uuid, finality (machine token committed | rolled_back), new_order (u64), new_uuid. No
// prose, no name. The parser decodes it strictly and builds any client text itself.
struct NativeTransactionBoundary {
  std::uint64_t ended_order{0};
  std::string ended_uuid;      // 16 bytes
  bool committed{false};
  std::uint64_t new_order{0};
  std::string new_uuid;        // 16 bytes
};
enum class NativeBoundaryParseStatus { not_native_boundary, parsed, refused };
// `expect_commit`: the statement asked for a commit (finality must be committed) or a rollback (rolled_back).
NativeBoundaryParseStatus ParseNativeTransactionBoundary(std::string_view engine_payload, bool expect_commit,
                                                         NativeTransactionBoundary* out);

}  // namespace scratchbird::parser::sbsql
