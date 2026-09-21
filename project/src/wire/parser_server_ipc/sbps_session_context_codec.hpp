// Copyright (c) 2026 ScratchBird Software Inc.
// SPDX-License-Identifier: MPL-2.0
#pragma once

#include "sbps_name_resolution_request_codec.hpp"

#include <optional>

namespace scratchbird::parser::ipc {

inline constexpr std::uint32_t kPsSessionContextSchemaV1 = 1012;
struct PsSessionContextV1 {
  core::platform::Uuid session_uuid, database_uuid, effective_user_uuid, principal_uuid;
  std::vector<core::platform::Uuid> roles, groups;
  core::platform::Uuid default_schema_uuid, current_schema_uuid;
  std::vector<core::platform::Uuid> search_path;
  core::platform::Uuid default_language_uuid, session_language_uuid, default_charset_uuid,
      default_collation_uuid, default_timezone_uuid;
  std::uint64_t catalog_generation{0}, security_epoch{0}, policy_generation{0}, name_resolution_epoch{0}, resource_epoch{0};
  core::platform::Uuid transaction_policy_uuid, cache_reload_policy_uuid, disclosure_policy_uuid;
  bool operator==(const PsSessionContextV1&) const = default;
};

namespace session_context_detail {
namespace tlv = name_request_detail;
using Uuid = core::platform::Uuid;
inline constexpr std::array identity_members{
    &PsSessionContextV1::session_uuid, &PsSessionContextV1::database_uuid,
    &PsSessionContextV1::effective_user_uuid, &PsSessionContextV1::principal_uuid,
    &PsSessionContextV1::default_schema_uuid, &PsSessionContextV1::current_schema_uuid,
    &PsSessionContextV1::default_language_uuid, &PsSessionContextV1::session_language_uuid,
    &PsSessionContextV1::default_charset_uuid, &PsSessionContextV1::default_collation_uuid,
    &PsSessionContextV1::default_timezone_uuid, &PsSessionContextV1::transaction_policy_uuid,
    &PsSessionContextV1::cache_reload_policy_uuid, &PsSessionContextV1::disclosure_policy_uuid};
inline constexpr std::array<unsigned, 14> identity_fields{1, 2, 3, 4, 7, 8, 10, 11, 12, 13, 14, 20, 21, 22};
inline constexpr std::array vector_members{&PsSessionContextV1::roles, &PsSessionContextV1::groups, &PsSessionContextV1::search_path};
inline constexpr std::array<unsigned, 3> vector_fields{5, 6, 9};
inline constexpr std::array generation_members{&PsSessionContextV1::catalog_generation, &PsSessionContextV1::security_epoch,
    &PsSessionContextV1::policy_generation, &PsSessionContextV1::name_resolution_epoch, &PsSessionContextV1::resource_epoch};
inline bool Measure(const PsSessionContextV1& value, std::size_t limit, std::size_t& size) noexcept {
  // Revision + 22 field headers + 14 UUIDs + 3 vector counts + 5 u64s.
  size = 2 + 22 * 6 + 14 * 16 + 3 * 2 + 5 * 8;
  if (limit < size) return false;
  for (auto member : identity_members) if (!core::uuid::IsEngineIdentityUuid(value.*member)) return false;
  for (auto member : vector_members) {
    const auto& ids = value.*member;
    if (ids.size() > 65535 || ids.size() * 16 > limit - size) return false;
    for (const auto& id : ids) if (!core::uuid::IsEngineIdentityUuid(id)) return false;
    size += ids.size() * 16;
  }
  return true;
}
inline bool Inspect(tlv::View bytes, std::size_t limit, std::array<tlv::View, 22>& fields) noexcept {
  if (!limit || bytes.size() > limit || !tlv::Fields(bytes, fields)) return false;
  for (auto field : identity_fields) if (!tlv::Identity(fields[field - 1])) return false;
  for (auto field : vector_fields) {
    const auto items = fields[field - 1];
    if (items.size() < 2 || tlv::Read(items.first(2)) * 16 != items.size() - 2) return false;
    for (std::size_t offset = 2; offset < items.size(); offset += 16)
      if (!tlv::Identity(items.subspan(offset, 16))) return false;
  }
  for (unsigned field = 15; field <= 19; ++field) if (fields[field - 1].size() != 8) return false;
  return true;
}
} // namespace session_context_detail

inline bool EncodePsSessionContextV1(const PsSessionContextV1& value, std::size_t limit, std::vector<std::uint8_t>* output) {
  namespace d = session_context_detail;
  namespace tlv = name_request_detail;
  std::size_t size;
  if (!output || !d::Measure(value, limit, size)) return false;
  tlv::Bytes staged{1, 0}; staged.reserve(size);
  std::size_t identity_index = 0, vector_index = 0;
  for (unsigned field = 1; field <= 22; ++field) {
    if (identity_index < d::identity_fields.size() && field == d::identity_fields[identity_index]) {
      tlv::Field(staged, field, (value.*d::identity_members[identity_index++]).bytes);
    } else if (vector_index < d::vector_fields.size() && field == d::vector_fields[vector_index]) {
      const auto& ids = value.*d::vector_members[vector_index++];
      tlv::Integer(staged, field, 2); tlv::Integer(staged, 2 + ids.size() * 16, 4); tlv::Integer(staged, ids.size(), 2);
      for (const auto& id : ids) staged.insert(staged.end(), id.bytes.begin(), id.bytes.end());
    } else {
      tlv::Scalar(staged, field, value.*d::generation_members[field - 15], 8);
    }
  }
  *output = std::move(staged); return true;
}

inline bool DecodePsSessionContextV1(std::span<const std::uint8_t> bytes, std::size_t limit, PsSessionContextV1* output) {
  namespace d = session_context_detail;
  namespace tlv = name_request_detail;
  std::array<tlv::View, 22> fields;
  if (!output || !d::Inspect(bytes, limit, fields)) return false;
  PsSessionContextV1 staged;
  for (std::size_t i = 0; i < d::identity_members.size(); ++i)
    staged.*d::identity_members[i] = tlv::IdentityValue(fields[d::identity_fields[i] - 1]);
  for (std::size_t i = 0; i < d::vector_members.size(); ++i) {
    const auto view = fields[d::vector_fields[i] - 1];
    auto& ids = staged.*d::vector_members[i]; ids.reserve(tlv::Read(view.first(2)));
    for (std::size_t offset = 2; offset < view.size(); offset += 16) ids.push_back(tlv::IdentityValue(view.subspan(offset, 16)));
  }
  for (std::size_t i = 0; i < d::generation_members.size(); ++i) staged.*d::generation_members[i] = tlv::Read(fields[14 + i]);
  static_assert(std::is_nothrow_move_assignable_v<PsSessionContextV1>);
  *output = std::move(staged); return true;
}

enum class PsSessionContextTransitionV1 { accepted_attach, session_update };

// The caller first proves channel/frame/profile admission and, for attach,
// its accepted outcome. An update cannot initialize a session, change an
// immutable binding or install a second attachment. No authentication or MGA
// state is created here. Failure preserves the entire previous snapshot.
inline bool PublishPsSessionContextV1(std::span<const std::uint8_t> payload, std::size_t limit,
    const core::platform::Uuid& header_session_uuid, PsSessionContextTransitionV1 transition,
    std::optional<PsSessionContextV1>* current) {
  namespace d = session_context_detail;
  namespace tlv = name_request_detail;
  if (!current || !core::uuid::IsEngineIdentityUuid(header_session_uuid) ||
      (transition != PsSessionContextTransitionV1::accepted_attach && transition != PsSessionContextTransitionV1::session_update) ||
      (transition == PsSessionContextTransitionV1::accepted_attach ? current->has_value() : !current->has_value())) return false;
  std::array<tlv::View, 22> fields;
  if (!d::Inspect(payload, limit, fields) || tlv::IdentityValue(fields[0]) != header_session_uuid) return false;
  if (*current) {
    const auto& old = **current;
    if (old.session_uuid != header_session_uuid || old.database_uuid != tlv::IdentityValue(fields[1]) ||
        old.effective_user_uuid != tlv::IdentityValue(fields[2]) || old.principal_uuid != tlv::IdentityValue(fields[3])) return false;
  }
  PsSessionContextV1 next;
  if (!DecodePsSessionContextV1(payload, limit, &next)) return false;
  static_assert(std::is_nothrow_move_assignable_v<std::optional<PsSessionContextV1>>);
  *current = std::move(next); return true;
}

// Build the exact name-request context from a published session snapshot and
// the identifier profile independently admitted by HELLO. No name/tag lookup,
// alias default, UUID issuance or authority substitution is permitted.
inline bool ProjectPsNameResolveContextV1(const PsSessionContextV1& session,
    const core::platform::Uuid& admitted_identifier_profile_uuid, PsNameResolveContextV1* output) {
  std::size_t size;
  if (!output || !core::uuid::IsEngineIdentityUuid(admitted_identifier_profile_uuid) ||
      !session_context_detail::Measure(session, std::numeric_limits<std::size_t>::max(), size)) return false;
  PsNameResolveContextV1 staged{session.session_uuid, admitted_identifier_profile_uuid, session.current_schema_uuid,
      session.session_language_uuid, session.search_path, session.catalog_generation, session.security_epoch, session.policy_generation};
  static_assert(std::is_nothrow_move_assignable_v<PsNameResolveContextV1>);
  *output = std::move(staged); return true;
}
} // namespace scratchbird::parser::ipc
