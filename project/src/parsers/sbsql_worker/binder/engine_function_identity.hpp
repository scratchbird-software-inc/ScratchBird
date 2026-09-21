// Copyright (c) 2026 ScratchBird Software Inc.
// SPDX-License-Identifier: MPL-2.0
#pragma once

#include "wire/parser_server_ipc/parser_client_types.hpp"
#include "core/uuid/uuid.hpp"

#include <optional>
#include <string_view>

namespace scratchbird::parser::sbsql {

// Resolve only against the engine-issued statement cohort. The textual builtin
// key selects a profile; it never becomes the function's executable identity.
inline std::optional<core::platform::Uuid> EngineIssuedAggregateFunctionUuid(
    const ipc::ParserStatementContext& context, std::string_view function_name) noexcept {
  constexpr std::string_view prefix = "sb.aggregate.";
  if (function_name.empty()) return std::nullopt;
  const ipc::ParserStatementContext::AggregateFunctionProfile* selected = nullptr;
  for (const auto& profile : context.aggregate_function_profiles) {
    if (!std::string_view(profile.builtin_id).starts_with(prefix) ||
        profile.builtin_id.size() - prefix.size() != function_name.size()) continue;
    bool matches = true;
    for (std::size_t i = 0; i < function_name.size(); ++i) {
      const auto ch = static_cast<unsigned char>(function_name[i]);
      const auto folded = ch >= 'A' && ch <= 'Z' ? ch + ('a' - 'A') : ch;
      if (folded != static_cast<unsigned char>(profile.builtin_id[prefix.size() + i])) {
        matches = false; break;
      }
    }
    if (!matches) continue;
    if (selected) return std::nullopt;
    selected = &profile;
  }
  if (!selected || selected->abi_version != 1 || !selected->executable ||
      !core::uuid::IsEngineIdentityUuid(selected->function_uuid)) return std::nullopt;
  return selected->function_uuid;
}

} // namespace scratchbird::parser::sbsql
