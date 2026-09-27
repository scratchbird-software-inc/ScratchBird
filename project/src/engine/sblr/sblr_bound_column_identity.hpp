// Copyright (c) 2026 ScratchBird Software Inc.
// SPDX-License-Identifier: MPL-2.0
#pragma once
#include "sblr_engine_envelope.hpp"
#include "core/uuid/uuid.hpp"
#include <algorithm>
#include <charconv>
#include <set>

namespace scratchbird::engine::sblr {

struct SblrColumnIdentityBinding {
  core::platform::Uuid column_uuid;
  core::platform::Uuid value_descriptor_uuid;
  core::platform::Uuid datatype_descriptor_uuid;
  std::uint64_t datatype_descriptor_generation = 0;
  core::platform::Uuid type_uuid;
  bool operator==(const SblrColumnIdentityBinding&) const = default;
};

inline bool IsColumnIdentityRole(std::string_view name) noexcept {
  return name.starts_with("column_") && name.size() > 7 &&
      ((name[7] >= '0' && name[7] <= '9') || name[7] == '+' || name[7] == '-') &&
      (name.ends_with("_uuid") || name.ends_with("_datatype_binding"));
}

// Structural projection only. Catalog and statement authority remain with
// the engine's DDL admission; a declaration never issues catalog identities.
// On refusal the destination is untouched, including partially decoded input.
inline bool DecodeSblrColumnIdentities(const SblrOperationEnvelope& envelope,
    std::vector<SblrColumnIdentityBinding>* output) {
  if (!output) return false;
  const auto role_count = std::count_if(envelope.operands.begin(), envelope.operands.end(),
      [](const auto& operand) { return IsColumnIdentityRole(operand.name); });
  if (role_count == 0) { output->clear(); return true; }
  if (envelope.operation_id != "ddl.create_table" ||
      envelope.opcode != "SBLR_DDL_CREATE_TABLE" || role_count % 4 != 0) return false;
  const auto count = static_cast<std::size_t>(role_count / 4);
  std::vector<SblrColumnIdentityBinding> staged(count);
  std::vector<unsigned> seen(count);
  for (std::size_t ordinal = 0; ordinal < envelope.operands.size(); ++ordinal) {
    const auto& operand = envelope.operands[ordinal];
    if (!IsColumnIdentityRole(operand.name)) continue;
    const auto tail = std::string_view(operand.name).substr(7);
    const auto split = tail.find('_');
    if (split == std::string_view::npos || split == 0) return false;
    const auto digits = tail.substr(0, split);
    if (digits.size() > 1 && digits.front() == '0') return false;
    std::size_t index = 0;
    const auto parsed = std::from_chars(digits.data(), digits.data() + digits.size(), index);
    if (parsed.ec != std::errc{} || parsed.ptr != digits.data() + digits.size() || index >= count)
      return false;
    const auto role = tail.substr(split + 1);
    const bool datatype = role == "datatype_binding";
    const unsigned bit = role == "uuid" ? 1 : role == "value_descriptor_uuid" ? 2 :
        datatype ? 4 : role == "type_uuid" ? 8 : 0;
    if (!bit || (seen[index] & bit) || operand.ordinal != ordinal + 1 ||
        !operand.value.empty() || operand.value_flags != 0 ||
        operand.type != (datatype ? "column_datatype_binding" : "uuid") ||
        operand.value_kind != (datatype ? SblrValueKind::profile_ref : SblrValueKind::uuid_ref) ||
        operand.value_body.size() != (datatype ? 24 : 16)) return false;
    core::platform::Uuid identity;
    std::copy_n(operand.value_body.begin(), 16, identity.bytes.begin());
    if (!core::uuid::IsEngineIdentityUuid(identity)) return false;
    auto& binding = staged[index];
    if (bit == 1) binding.column_uuid = identity;
    if (bit == 2) binding.value_descriptor_uuid = identity;
    if (bit == 8) binding.type_uuid = identity;
    if (datatype) {
      binding.datatype_descriptor_uuid = identity;
      for (unsigned byte = 0; byte < 8; ++byte)
        binding.datatype_descriptor_generation |=
            std::uint64_t(operand.value_body[16 + byte]) << (8 * byte);
      if (!binding.datatype_descriptor_generation) return false;
    }
    seen[index] |= bit;
  }
  std::set<core::platform::Uuid> occurrences;
  std::set<core::platform::Uuid> catalog_ids;
  for (const auto& binding : staged) {
    catalog_ids.insert(binding.datatype_descriptor_uuid);
    catalog_ids.insert(binding.type_uuid);
  }
  for (std::size_t index = 0; index < count; ++index) {
    const auto& binding = staged[index];
    if (seen[index] != 15 || !occurrences.insert(binding.column_uuid).second ||
        !occurrences.insert(binding.value_descriptor_uuid).second ||
        catalog_ids.contains(binding.column_uuid) ||
        catalog_ids.contains(binding.value_descriptor_uuid)) return false;
  }
  *output = std::move(staged);
  return true;
}

inline void AppendSblrColumnIdentityBinding(SblrOperationEnvelope& envelope,
    std::size_t index, const SblrColumnIdentityBinding& binding) {
  const auto append = [&](const char* role, const core::platform::Uuid& identity,
                          bool datatype = false) {
    SblrOperand operand;
    operand.ordinal = static_cast<std::uint32_t>(envelope.operands.size() + 1);
    operand.name = "column_" + std::to_string(index) + "_" + role;
    operand.type = datatype ? "column_datatype_binding" : "uuid";
    operand.value_kind = datatype ? SblrValueKind::profile_ref : SblrValueKind::uuid_ref;
    operand.value_body.assign(identity.bytes.begin(), identity.bytes.end());
    if (datatype) for (unsigned byte = 0; byte < 8; ++byte)
      operand.value_body.push_back(binding.datatype_descriptor_generation >> (8 * byte));
    envelope.operands.push_back(std::move(operand));
  };
  append("uuid", binding.column_uuid);
  append("value_descriptor_uuid", binding.value_descriptor_uuid);
  append("datatype_binding", binding.datatype_descriptor_uuid, true);
  append("type_uuid", binding.type_uuid);
}

} // namespace scratchbird::engine::sblr
