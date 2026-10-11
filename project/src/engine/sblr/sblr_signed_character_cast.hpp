// Copyright (c) 2026 ScratchBird Software Inc.
// SPDX-License-Identifier: MPL-2.0
#pragma once
#include "sblr_runtime.hpp"

namespace scratchbird::engine::sblr {
constexpr unsigned SblrSignedIntegerWidth(std::string_view name) noexcept {
  return name == "int8" ? 1 : name == "int16" ? 2 : name == "int32" ? 4 :
      name == "int64" ? 8 : 0;
}
constexpr bool IsSblrSignedCharacterCast(std::string_view source,
                                        std::string_view target) noexcept {
  return (source == "character" && SblrSignedIntegerWidth(target)) ||
      (SblrSignedIntegerWidth(source) && target == "character");
}
// Representation adapter over the descriptor executor and Core checked cast.
// Endpoint bindings are supplied by the owning caller, never minted here.
SblrResult EvaluateSblrSignedCharacterCast(std::string_view operation,
    const SblrValue& value, std::string_view target_name,
    const SblrExecutionContext& context, bool explicit_cast, bool compatibility,
    std::shared_ptr<const internal_api::EngineDescriptor> target);
}
