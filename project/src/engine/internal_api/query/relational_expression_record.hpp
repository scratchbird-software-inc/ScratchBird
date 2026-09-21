// Copyright (c) 2026 ScratchBird Software Inc.
// SPDX-License-Identifier: MPL-2.0
#pragma once

#include "../api_types.hpp"

#include <array>
#include <cstdint>
#include <optional>
#include <string>
#include <vector>

namespace scratchbird::engine::internal_api {

enum class RelationalExpressionKind : std::uint8_t {
  kLiteral = 1,
  kParameter,
  kIdentifier,
  kFunctionCall,
  kUnary,
  kBinary,
  kParenthesized,
};

enum class RelationalLiteralKind : std::uint8_t {
  kNumeric = 1,
  kString,
  kBinary,
  kTemporal,
  kUuid,
  kBoolean,
  kNull,
  kDefault,
  kDocument,
  kVector,
  kRegex,
  kRange,
};

struct RelationalExpressionRecord {
  std::uint32_t expression_id{0};
  RelationalExpressionKind expression_kind{RelationalExpressionKind::kLiteral};
  std::vector<std::uint32_t> child_expression_ids;
  std::uint32_t result_descriptor_id{0};
  std::optional<EngineUuid> function_uuid;
  std::optional<EngineUuid> bound_name_uuid;
  std::optional<RelationalLiteralKind> literal_kind;
  std::optional<std::string> operator_name;
  std::optional<std::string> literal_or_parameter_ref;
  struct LiteralTypedValueV1 {
    EngineUuid descriptor_uuid;
    std::uint64_t descriptor_generation{0};
    std::string value_state;
    std::vector<std::uint8_t> canonical_value_bytes;
    std::array<std::uint8_t,32> canonical_value_sha256{};
  };
  std::optional<LiteralTypedValueV1> literal_typed_value_v1;
  struct ParameterTypedValueV1 {
    EngineUuid descriptor_uuid;
    std::uint64_t descriptor_generation{0};
    std::string value_state;
    std::vector<std::uint8_t> canonical_value_bytes;
    std::array<std::uint8_t,32> canonical_value_sha256{};
  };
  std::optional<ParameterTypedValueV1> parameter_typed_value_v1;
  // query.execute-1.1 keeps d718/gen1 in the ordinary descriptor table.  This
  // structural marker carries only the exact SBXN occurrence/node/handle
  // coordinates required to resolve the receipt-owned contextual lease.
  struct ContextualTextLiteralReferenceV2 {
    std::uint64_t literal_occurrence{0};
    std::uint64_t node_id{0};
    std::uint32_t literal_descriptor_handle{0};
  };
  std::optional<ContextualTextLiteralReferenceV2>
      contextual_text_literal_v2;
};

}  // namespace scratchbird::engine::internal_api
