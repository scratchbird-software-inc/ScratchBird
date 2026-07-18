// Copyright (c) 2026 ScratchBird Software Inc.
//
// This Source Code Form is subject to the terms of the Mozilla Public
// License, v. 2.0. If a copy of the MPL was not distributed with this
// file, You can obtain one at https://mozilla.org/MPL/2.0/.
//
// SPDX-License-Identifier: MPL-2.0

#include "sblr_procedural_block_runtime.hpp"

#include "sblr_context_variables.hpp"
#include "sblr_special_forms.hpp"

#include <algorithm>
#include <cctype>
#include <cstdint>
#include <limits>
#include <map>
#include <optional>
#include <set>
#include <string>
#include <string_view>
#include <utility>

namespace scratchbird::engine::sblr {
namespace {

namespace api = scratchbird::engine::internal_api;

constexpr std::uint32_t kMaximumProceduralSlotsV1 = 2;
constexpr std::uint32_t kMaximumProceduralInstructionsV1 = 1;
constexpr std::uint32_t kMaximumProceduralCharacterLengthV1 = 32767;
constexpr std::string_view kProceduralOperationId =
    "transaction.execute_block";

SblrProceduralBlockDecodeResult DecodeFailure(
    bool present,
    std::string diagnostic_code,
    std::string diagnostic_detail) {
  SblrProceduralBlockDecodeResult result;
  result.present = present;
  result.valid = false;
  result.diagnostic_code = std::move(diagnostic_code);
  result.diagnostic_detail = std::move(diagnostic_detail);
  return result;
}

std::pair<std::string_view, std::string_view> SplitOption(
    std::string_view option) {
  const auto separator = option.find(':');
  if (separator == std::string_view::npos) {
    return {option, {}};
  }
  return {option.substr(0, separator), option.substr(separator + 1)};
}

std::string LowerAscii(std::string_view value) {
  std::string out(value);
  std::transform(out.begin(), out.end(), out.begin(), [](unsigned char ch) {
    return static_cast<char>(std::tolower(ch));
  });
  return out;
}

bool ForbiddenSourcePayloadKey(std::string_view key) {
  const std::string lower = LowerAscii(key);
  return lower == "sql" || lower == "source_sql" || lower == "raw_sql" ||
         lower == "parser_ast" || lower == "parser_plan" ||
         lower.find("sql_text") != std::string::npos;
}

bool ParseUnsigned(std::string_view value,
                   std::uint32_t maximum,
                   std::uint32_t* parsed) {
  if (parsed == nullptr || value.empty()) return false;
  std::uint64_t accumulator = 0;
  for (const unsigned char ch : value) {
    if (!std::isdigit(ch)) return false;
    const auto digit = static_cast<std::uint64_t>(ch - '0');
    const auto bounded_maximum = static_cast<std::uint64_t>(maximum);
    if (accumulator > bounded_maximum / 10u ||
        (accumulator == bounded_maximum / 10u &&
         digit > bounded_maximum % 10u)) {
      return false;
    }
    accumulator = accumulator * 10u + digit;
  }
  *parsed = static_cast<std::uint32_t>(accumulator);
  return true;
}

bool ParseSigned(std::string_view value, std::int64_t* parsed) {
  if (parsed == nullptr || value.empty()) return false;
  try {
    std::size_t consumed = 0;
    const auto result = std::stoll(std::string(value), &consumed, 10);
    if (consumed != value.size()) return false;
    *parsed = result;
    return true;
  } catch (...) {
    return false;
  }
}

bool ParseBoolean(std::string_view value, bool* parsed) {
  if (parsed == nullptr) return false;
  if (value == "true") {
    *parsed = true;
    return true;
  }
  if (value == "false") {
    *parsed = false;
    return true;
  }
  return false;
}

SblrResult RuntimeFailure(const SblrExecutionContext& context,
                          std::string diagnostic_id,
                          std::string detail) {
  auto diagnostic = MakeSblrRefusalDiagnostic(
      std::move(diagnostic_id), context, std::move(detail));
  diagnostic.fields.push_back(
      {"operation_id", std::string(kProceduralOperationId)});
  return MakeSblrFailure(SblrStatusCode::execution_failed,
                         std::string(kProceduralOperationId),
                         std::move(diagnostic));
}

const api::EngineProceduralSlotV1* FindSlot(
    const api::EngineProceduralBlockV1& block,
    std::string_view slot_id) {
  const auto found = std::find_if(
      block.slots.begin(), block.slots.end(), [&](const auto& slot) {
        return slot.slot_id == slot_id;
      });
  return found == block.slots.end() ? nullptr : &*found;
}

std::string CanonicalDescriptor(api::EngineProceduralType type) {
  switch (type) {
    case api::EngineProceduralType::character:
      return "character";
    case api::EngineProceduralType::int32:
      return "int32";
  }
  return {};
}

bool ValidateRuntimeBlockModelV1(const api::EngineProceduralBlockV1& block,
                                 std::string* detail) {
  const auto fail = [&](std::string message) {
    if (detail != nullptr) *detail = std::move(message);
    return false;
  };
  if (block.contract != api::kSblrProceduralBlockV1Contract ||
      block.block_kind != "anonymous" || block.input_count != 0 ||
      block.yield_count != 0) {
    return fail("procedural runtime received an invalid or non-zero-yield v1 block");
  }
  if (block.local_count > 1 || block.output_count > 1 ||
      block.slots.size() > kMaximumProceduralSlotsV1 ||
      block.instructions.size() > kMaximumProceduralInstructionsV1 ||
      block.slots.size() != block.local_count + block.output_count) {
    return fail("procedural runtime received inconsistent v1 block counts");
  }

  std::uint32_t local_count = 0;
  std::uint32_t result_count = 0;
  std::set<std::string> slot_ids;
  for (const auto& slot : block.slots) {
    if (!slot_ids.insert(slot.slot_id).second) {
      return fail("procedural runtime received duplicate slot IDs");
    }
    if (slot.kind == api::EngineProceduralSlotKind::local) {
      ++local_count;
      if (slot.slot_id != "local.0") {
        return fail("procedural runtime local slot ID is not canonical");
      }
    } else if (slot.kind == api::EngineProceduralSlotKind::result) {
      ++result_count;
      if (slot.slot_id != "result.0") {
        return fail("procedural runtime result slot ID is not canonical");
      }
    } else {
      return fail("procedural runtime received an invalid slot kind");
    }
    if (slot.type == api::EngineProceduralType::character) {
      if (slot.character_length == 0 ||
          slot.character_length > kMaximumProceduralCharacterLengthV1) {
        return fail("procedural runtime received an invalid character length");
      }
    } else if (slot.type == api::EngineProceduralType::int32) {
      if (slot.character_length != 0) {
        return fail("procedural runtime received character length on an int32 slot");
      }
    } else {
      return fail("procedural runtime received an invalid slot type");
    }
  }
  if (local_count != block.local_count || result_count != block.output_count) {
    return fail("procedural runtime slot kinds do not match declared counts");
  }

  if (block.instructions.empty()) {
    if (block.local_count != 0 || block.output_count > 1) {
      return fail("procedural runtime received an invalid empty-block profile");
    }
    return true;
  }
  if (block.local_count != 1 || block.output_count != 0 ||
      block.slots.size() != 1 ||
      block.slots.front().kind != api::EngineProceduralSlotKind::local ||
      block.slots.front().type != api::EngineProceduralType::character) {
    return fail("procedural runtime received an invalid assignment-block profile");
  }

  const auto& instruction = block.instructions.front();
  if (instruction.kind != api::EngineProceduralInstructionKind::assign ||
      instruction.target_slot_id != "local.0" ||
      instruction.expression_kind !=
          api::EngineProceduralExpressionKind::substring ||
      instruction.source_kind !=
          api::EngineProceduralExpressionSourceKind::context_variable ||
      instruction.source_id != "ctx_current_timestamp" ||
      instruction.source_cast_type != api::EngineProceduralType::character ||
      instruction.start_value != 1 ||
      instruction.length_kind !=
          api::EngineProceduralSubstringLengthKind::to_end) {
    return fail("procedural runtime received an unsupported v1 instruction");
  }
  return true;
}

}  // namespace

SblrProceduralBlockDecodeResult DecodeSblrProceduralBlockV1(
    const std::vector<std::string>& option_envelopes) {
  std::map<std::string, std::string> fields;
  bool present = false;
  for (const auto& option : option_envelopes) {
    const auto [key, value] = SplitOption(option);
    if (!key.starts_with("procedural_")) continue;
    present = true;
    if (!fields.emplace(std::string(key), std::string(value)).second) {
      return DecodeFailure(
          true,
          "SB_SBLR_PROCEDURAL_IR_DUPLICATE_FIELD",
          "duplicate procedural IR field: " + std::string(key));
    }
  }

  // Source execution payloads are forbidden even when the procedural contract
  // marker is missing. Otherwise a source-only transaction.execute_block
  // envelope would fall through to the legacy behavior-row route.
  for (const auto& option : option_envelopes) {
    const auto [key, value] = SplitOption(option);
    (void)value;
    if (ForbiddenSourcePayloadKey(key)) {
      return DecodeFailure(
          true,
          "SB_SBLR_PROCEDURAL_SOURCE_PAYLOAD_FORBIDDEN",
          "procedural IR must not carry SQL text or parser execution payloads");
    }
  }
  if (!present) return {};

  std::set<std::string> consumed;
  const auto take = [&](const std::string& key)
      -> std::optional<std::string> {
    const auto found = fields.find(key);
    if (found == fields.end()) return std::nullopt;
    consumed.insert(key);
    return found->second;
  };
  const auto require = [&](const std::string& key,
                           std::string* value,
                           SblrProceduralBlockDecodeResult* failure) {
    const auto found = take(key);
    if (found.has_value()) {
      *value = *found;
      return true;
    }
    *failure = DecodeFailure(
        true,
        "SB_SBLR_PROCEDURAL_IR_FIELD_REQUIRED",
        "required procedural IR field is absent: " + key);
    return false;
  };

  SblrProceduralBlockDecodeResult failure;
  std::string value;
  if (!require("procedural_ir_contract", &value, &failure)) return failure;
  if (value != api::kSblrProceduralBlockV1Contract) {
    return DecodeFailure(
        true,
        "SB_SBLR_PROCEDURAL_IR_CONTRACT_UNSUPPORTED",
        "unsupported procedural IR contract: " + value);
  }

  api::EngineProceduralBlockV1 block;
  block.contract = value;
  if (!require("procedural_block_kind", &block.block_kind, &failure)) {
    return failure;
  }
  if (block.block_kind != "anonymous") {
    return DecodeFailure(true,
                         "SB_SBLR_PROCEDURAL_IR_PROFILE_UNSUPPORTED",
                         "v1 supports only anonymous procedural blocks");
  }

  const auto parse_count = [&](const std::string& key,
                               std::uint32_t maximum,
                               std::uint32_t* destination) {
    std::string encoded;
    if (!require(key, &encoded, &failure)) return false;
    if (ParseUnsigned(encoded, maximum, destination)) return true;
    failure = DecodeFailure(
        true,
        "SB_SBLR_PROCEDURAL_IR_FIELD_INVALID",
        "invalid or out-of-range procedural IR count: " + key);
    return false;
  };

  std::uint32_t slot_count = 0;
  std::uint32_t instruction_count = 0;
  if (!parse_count("procedural_input_count", 0, &block.input_count) ||
      !parse_count("procedural_local_count", 1, &block.local_count) ||
      !parse_count("procedural_output_count", 1, &block.output_count) ||
      !parse_count("procedural_slot_count", kMaximumProceduralSlotsV1,
                   &slot_count) ||
      !parse_count("procedural_instruction_count",
                   kMaximumProceduralInstructionsV1,
                   &instruction_count) ||
      !parse_count("procedural_yield_count", 0, &block.yield_count)) {
    return failure;
  }
  if (slot_count != block.local_count + block.output_count) {
    return DecodeFailure(
        true,
        "SB_SBLR_PROCEDURAL_IR_SLOT_COUNT_MISMATCH",
        "procedural slot count does not equal local plus output counts");
  }

  block.slots.reserve(slot_count);
  std::uint32_t decoded_locals = 0;
  std::uint32_t decoded_outputs = 0;
  std::set<std::string> slot_ids;
  for (std::uint32_t index = 0; index < slot_count; ++index) {
    const std::string prefix =
        "procedural_slot_" + std::to_string(index) + "_";
    api::EngineProceduralSlotV1 slot;
    std::string kind;
    std::string type;
    std::string nullable;
    if (!require(prefix + "id", &slot.slot_id, &failure) ||
        !require(prefix + "kind", &kind, &failure) ||
        !require(prefix + "type", &type, &failure) ||
        !require(prefix + "nullable", &nullable, &failure)) {
      return failure;
    }
    if (!slot_ids.insert(slot.slot_id).second) {
      return DecodeFailure(true,
                           "SB_SBLR_PROCEDURAL_IR_SLOT_ID_DUPLICATE",
                           "procedural slot IDs must be unique");
    }
    if (kind == "local") {
      slot.kind = api::EngineProceduralSlotKind::local;
      ++decoded_locals;
      if (slot.slot_id != "local.0") {
        return DecodeFailure(
            true,
            "SB_SBLR_PROCEDURAL_IR_SLOT_ID_INVALID",
            "v1 local slot must use canonical ID local.0");
      }
    } else if (kind == "result") {
      slot.kind = api::EngineProceduralSlotKind::result;
      ++decoded_outputs;
      if (slot.slot_id != "result.0") {
        return DecodeFailure(
            true,
            "SB_SBLR_PROCEDURAL_IR_SLOT_ID_INVALID",
            "v1 result slot must use canonical ID result.0");
      }
    } else {
      return DecodeFailure(true,
                           "SB_SBLR_PROCEDURAL_IR_SLOT_KIND_INVALID",
                           "unsupported procedural slot kind: " + kind);
    }
    if (type == "character") {
      slot.type = api::EngineProceduralType::character;
      std::string encoded_length;
      if (!require(prefix + "character_length", &encoded_length, &failure)) {
        return failure;
      }
      if (!ParseUnsigned(encoded_length,
                         kMaximumProceduralCharacterLengthV1,
                         &slot.character_length) ||
          slot.character_length == 0) {
        return DecodeFailure(
            true,
            "SB_SBLR_PROCEDURAL_IR_CHARACTER_LENGTH_INVALID",
            "character slot length must be between 1 and 32767");
      }
    } else if (type == "int32") {
      slot.type = api::EngineProceduralType::int32;
    } else {
      return DecodeFailure(true,
                           "SB_SBLR_PROCEDURAL_IR_SLOT_TYPE_INVALID",
                           "unsupported procedural slot type: " + type);
    }
    if (!ParseBoolean(nullable, &slot.nullable)) {
      return DecodeFailure(true,
                           "SB_SBLR_PROCEDURAL_IR_NULLABILITY_INVALID",
                           "procedural slot nullable must be true or false");
    }
    block.slots.push_back(std::move(slot));
  }
  if (decoded_locals != block.local_count ||
      decoded_outputs != block.output_count) {
    return DecodeFailure(
        true,
        "SB_SBLR_PROCEDURAL_IR_SLOT_KIND_COUNT_MISMATCH",
        "procedural slot kinds do not match declared local and output counts");
  }

  block.instructions.reserve(instruction_count);
  for (std::uint32_t index = 0; index < instruction_count; ++index) {
    const std::string prefix =
        "procedural_instruction_" + std::to_string(index) + "_";
    api::EngineProceduralInstructionV1 instruction;
    std::string kind;
    std::string expression_kind;
    std::string source_kind;
    std::string source_cast_type;
    std::string start_kind;
    std::string start_value;
    std::string length_kind;
    if (!require(prefix + "kind", &kind, &failure) ||
        !require(prefix + "target_slot", &instruction.target_slot_id,
                 &failure) ||
        !require(prefix + "expression_kind", &expression_kind, &failure) ||
        !require(prefix + "source_kind", &source_kind, &failure) ||
        !require(prefix + "source_id", &instruction.source_id, &failure) ||
        !require(prefix + "source_cast_type", &source_cast_type, &failure) ||
        !require(prefix + "start_kind", &start_kind, &failure) ||
        !require(prefix + "start_value", &start_value, &failure) ||
        !require(prefix + "length_kind", &length_kind, &failure)) {
      return failure;
    }
    if (kind != "assign" || expression_kind != "substring" ||
        source_kind != "context_variable" ||
        instruction.source_id != "ctx_current_timestamp" ||
        source_cast_type != "character" ||
        start_kind != "literal_int64" || length_kind != "to_end" ||
        !ParseSigned(start_value, &instruction.start_value) ||
        instruction.start_value != 1 ||
        instruction.target_slot_id != "local.0") {
      return DecodeFailure(
          true,
          "SB_SBLR_PROCEDURAL_IR_INSTRUCTION_UNSUPPORTED",
          "v1 supports only local.0 = substring(cast(ctx_current_timestamp as character), 1, to_end)");
    }
    instruction.kind = api::EngineProceduralInstructionKind::assign;
    instruction.expression_kind =
        api::EngineProceduralExpressionKind::substring;
    instruction.source_kind =
        api::EngineProceduralExpressionSourceKind::context_variable;
    instruction.source_cast_type = api::EngineProceduralType::character;
    instruction.length_kind =
        api::EngineProceduralSubstringLengthKind::to_end;
    block.instructions.push_back(std::move(instruction));
  }

  if (instruction_count == 0) {
    if (block.local_count != 0 || block.output_count > 1) {
      return DecodeFailure(
          true,
          "SB_SBLR_PROCEDURAL_IR_PROFILE_UNSUPPORTED",
          "v1 empty blocks allow no locals and at most one result slot");
    }
  } else if (block.local_count != 1 || block.output_count != 0 ||
             block.slots.size() != 1 ||
             block.slots.front().kind != api::EngineProceduralSlotKind::local ||
             block.slots.front().type != api::EngineProceduralType::character) {
    return DecodeFailure(
        true,
        "SB_SBLR_PROCEDURAL_IR_PROFILE_UNSUPPORTED",
        "v1 assignment block requires exactly one character local and no result slots");
  }

  if (consumed.size() != fields.size()) {
    for (const auto& [key, ignored] : fields) {
      (void)ignored;
      if (consumed.find(key) == consumed.end()) {
        return DecodeFailure(true,
                             "SB_SBLR_PROCEDURAL_IR_UNKNOWN_FIELD",
                             "unknown procedural IR field: " + key);
      }
    }
  }

  SblrProceduralBlockDecodeResult result;
  result.present = true;
  result.valid = true;
  result.block = std::move(block);
  return result;
}

SblrProceduralBlockExecutionResult ExecuteSblrProceduralBlockV1(
    const api::EngineProceduralBlockV1& block,
    const SblrExecutionContext& context) {
  SblrProceduralBlockExecutionResult execution;
  std::string validation_detail;
  if (!ValidateRuntimeBlockModelV1(block, &validation_detail)) {
    execution.result = RuntimeFailure(
        context,
        "SB_SBLR_PROCEDURAL_IR_RUNTIME_CONTRACT_INVALID",
        std::move(validation_detail));
    return execution;
  }

  for (const auto& definition : block.slots) {
    SblrAssignmentSlot slot;
    slot.slot_id = definition.slot_id;
    slot.descriptor_id = CanonicalDescriptor(definition.type);
    slot.kind = definition.kind == api::EngineProceduralSlotKind::local
                    ? SblrAssignmentSlotKind::local_variable
                    : SblrAssignmentSlotKind::routine_result;
    slot.allow_null = definition.nullable;
    slot.value.is_null = true;
    const auto registered = RegisterSblrAssignmentSlot(
        kProceduralOperationId, &execution.assignment_frame, std::move(slot),
        context);
    if (!registered.ok()) {
      execution.result = registered;
      return execution;
    }
  }

  for (const auto& instruction : block.instructions) {
    const auto* target = FindSlot(block, instruction.target_slot_id);
    if (target == nullptr ||
        target->kind != api::EngineProceduralSlotKind::local ||
        target->type != api::EngineProceduralType::character) {
      execution.result = RuntimeFailure(
          context,
          "SB_SBLR_PROCEDURAL_TARGET_SLOT_INVALID",
          "procedural assignment target is not the declared character local");
      return execution;
    }

    const auto source =
        ResolveSblrContextVariable(instruction.source_id, context);
    if (!source.ok()) {
      execution.result = source;
      return execution;
    }
    if (source.scalar_values.size() != 1) {
      execution.result = RuntimeFailure(
          context,
          "SB_SBLR_PROCEDURAL_SOURCE_SHAPE_INVALID",
          "procedural context read did not return exactly one scalar value");
      return execution;
    }

    const auto cast = EvaluateSblrCastForm(
        kProceduralOperationId, source.scalar_values.front(), "character",
        context, true, false);
    if (!cast.ok()) {
      execution.result = cast;
      return execution;
    }
    if (cast.scalar_values.size() != 1) {
      execution.result = RuntimeFailure(
          context,
          "SB_SBLR_PROCEDURAL_CAST_SHAPE_INVALID",
          "procedural source cast did not return exactly one scalar value");
      return execution;
    }

    SblrValue start;
    start.descriptor_id = "int64";
    start.payload_kind = SblrValuePayloadKind::signed_integer;
    start.int64_value = instruction.start_value;
    start.has_int64_value = true;
    start.is_null = false;
    start.text_value = std::to_string(instruction.start_value);
    start.encoded_value = start.text_value;

    const auto& cast_value = cast.scalar_values.front();
    const std::size_t zero_based_start = instruction.start_value <= 0
                                             ? 0
                                             : static_cast<std::size_t>(
                                                   instruction.start_value - 1);
    const std::size_t remaining =
        zero_based_start >= cast_value.text_value.size()
            ? 0
            : cast_value.text_value.size() - zero_based_start;
    if (remaining >
        static_cast<std::size_t>(std::numeric_limits<std::int64_t>::max())) {
      execution.result = RuntimeFailure(
          context,
          "SB_SBLR_PROCEDURAL_SUBSTRING_LENGTH_OVERFLOW",
          "procedural substring-to-end length exceeds int64 range");
      return execution;
    }
    SblrValue length;
    length.descriptor_id = "int64";
    length.payload_kind = SblrValuePayloadKind::signed_integer;
    length.int64_value = static_cast<std::int64_t>(remaining);
    length.has_int64_value = true;
    length.is_null = false;
    length.text_value = std::to_string(length.int64_value);
    length.encoded_value = length.text_value;

    const auto substring = EvaluateSblrSubstringForm(
        kProceduralOperationId, cast_value, start, length, context);
    if (!substring.ok()) {
      execution.result = substring;
      return execution;
    }
    if (substring.scalar_values.size() != 1) {
      execution.result = RuntimeFailure(
          context,
          "SB_SBLR_PROCEDURAL_SUBSTRING_SHAPE_INVALID",
          "procedural substring did not return exactly one scalar value");
      return execution;
    }
    const auto& assigned_value = substring.scalar_values.front();
    if (!assigned_value.is_null && target->character_length != 0 &&
        assigned_value.text_value.size() > target->character_length) {
      execution.result = RuntimeFailure(
          context,
          "SB_SBLR_PROCEDURAL_CHARACTER_LENGTH_EXCEEDED",
          "procedural assignment exceeds the declared character length");
      return execution;
    }
    const auto assigned = AssignSblrSlot(
        kProceduralOperationId, &execution.assignment_frame,
        instruction.target_slot_id, assigned_value, context);
    if (!assigned.ok()) {
      execution.result = assigned;
      return execution;
    }
    ++execution.instructions_executed;
  }

  execution.result = MakeSblrSuccess(std::string(kProceduralOperationId));
  execution.result.mutation_attempted = false;
  execution.result.mutation_committed = false;
  // Rows can only be appended by a yield instruction. The v1 decoder and
  // runtime both require yield_count=0, so an unassigned NOT NULL result slot
  // is never read or validated at block completion.
  return execution;
}

}  // namespace scratchbird::engine::sblr
