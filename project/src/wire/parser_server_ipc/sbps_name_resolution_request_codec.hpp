// Copyright (c) 2026 ScratchBird Software Inc.
// SPDX-License-Identifier: MPL-2.0
#pragma once

#include "binary_identity_io.hpp"
#include "../../core/datatypes/canonical_utf8.hpp"

#include <algorithm>
#include <array>
#include <limits>
#include <span>
#include <string>
#include <string_view>
#include <type_traits>
#include <utility>
#include <vector>

namespace scratchbird::parser::ipc {

// Canonical schema 1032, message 32. This codec grants no profile activation,
// session binding, name visibility, privilege or transaction authority.
inline constexpr std::uint32_t kPsNameResolveRequestSchemaV1 = 1032;
inline constexpr std::uint16_t kPsNameResolveRequestMessageV1 = 32;
struct PsIdentifierComponentV1 {
  std::string spelling;
  bool quoted{false}, case_folded{false};
  bool operator==(const PsIdentifierComponentV1&) const = default;
};
struct PsQualifiedIdentifierV1 {
  std::uint8_t qualification_kind{0};
  std::vector<PsIdentifierComponentV1> components;
  bool source_span_present{false};
  std::uint64_t source_start_utf8_byte{0}, source_length_utf8_bytes{0};
  bool operator==(const PsQualifiedIdentifierV1&) const = default;
};
struct PsNameResolveRequestV1 {
  core::platform::Uuid session_uuid, identifier_profile_uuid;
  PsQualifiedIdentifierV1 source_identifier;
  core::platform::Uuid current_schema_uuid;
  std::vector<core::platform::Uuid> search_path;
  core::platform::Uuid session_language_uuid;
  std::vector<std::uint8_t> object_kind_filter, required_privileges;
  bool include_grant_summary{false};
  std::uint64_t catalog_generation{0}, security_epoch{0}, policy_generation{0};
  bool operator==(const PsNameResolveRequestV1&) const = default;
};
// Owned by the bound server session, not populated from the request. Matching
// these fields is necessary but not sufficient for authorization/dispatch.
struct PsNameResolveContextV1 {
  core::platform::Uuid session_uuid, identifier_profile_uuid, current_schema_uuid, session_language_uuid;
  std::vector<core::platform::Uuid> search_path;
  std::uint64_t catalog_generation{0}, security_epoch{0}, policy_generation{0};
};

// Transaction-aware revision 2 of schema 1032. Encoding/decoding does not
// activate this revision on a channel or establish transaction ownership.
struct PsNameResolveRequestV2 {
  PsNameResolveRequestV1 name;
  std::uint64_t local_transaction_id{0};
  core::platform::Uuid transaction_uuid;
  std::uint8_t projection_flags{0};
  bool operator==(const PsNameResolveRequestV2&) const = default;
};
struct PsNameResolveContextV2 {
  PsNameResolveContextV1 name;
  std::uint64_t local_transaction_id{0};
  core::platform::Uuid transaction_uuid;
  bool relation_projection_allowed{false};
};

namespace name_request_detail {
using Bytes = std::vector<std::uint8_t>;
using View = std::span<const std::uint8_t>;
inline std::uint64_t Read(View bytes) noexcept {
  std::uint64_t value = 0;
  for (std::size_t i = 0; i < bytes.size(); ++i) value |= std::uint64_t(bytes[i]) << (8 * i);
  return value;
}
inline void Integer(Bytes& bytes, std::uint64_t value, unsigned width) {
  for (unsigned i = 0; i < width; ++i) bytes.push_back(static_cast<std::uint8_t>(value >> (8 * i)));
}
inline bool Identity(View bytes) noexcept {
  core::platform::Uuid id; std::size_t offset = 0;
  return bytes.size() == 16 && wire::parser_server_ipc::ReadEngineIdentityUuid(bytes, &offset, &id);
}
inline core::platform::Uuid IdentityValue(View bytes) noexcept {
  core::platform::Uuid id; std::copy_n(bytes.begin(), 16, id.bytes.begin()); return id;
}
inline bool Text(View bytes) noexcept {
  return !bytes.empty() && std::find(bytes.begin(), bytes.end(), 0) == bytes.end() &&
      core::datatypes::ValidateCanonicalUtf8(bytes.data(), bytes.size());
}
inline bool Boolean(View bytes) noexcept { return bytes.size() == 1 && bytes[0] <= 1; }
template<std::size_t N> inline bool Fields(View input, std::array<View, N>& output,
                                          std::uint16_t revision = 1) noexcept {
  if (input.size() < 2 || Read(input.first(2)) != revision) return false;
  std::size_t offset = 2;
  for (std::size_t i = 0; i < N; ++i) {
    if (input.size() - offset < 6 || Read(input.subspan(offset, 2)) != i + 1) return false;
    const auto size = Read(input.subspan(offset + 2, 4)); offset += 6;
    if (size > input.size() - offset) return false;
    output[i] = input.subspan(offset, static_cast<std::size_t>(size)); offset += size;
  }
  return offset == input.size();
}
inline bool Codes(View values, unsigned maximum) noexcept {
  if (values.empty() || values.size() > maximum) return false;
  std::uint32_t seen = 0;
  for (auto value : values) {
    if (value == 0 || value > maximum || (seen & (1u << value))) return false;
    seen |= 1u << value;
  }
  return true;
}
inline bool CodeVector(View bytes, unsigned maximum) noexcept {
  return bytes.size() >= 2 && Read(bytes.first(2)) == bytes.size() - 2 && Codes(bytes.subspan(2), maximum);
}
struct RequestView {
  std::array<View, 12> fields;
  std::array<View, 5> identifier;
  std::array<std::array<View, 3>, 4> components;
  std::size_t component_count{0};
};
// Fully validate every nested record and identity before allocating output.
inline bool Inspect(View bytes, std::size_t limit, RequestView& out,
                    std::uint16_t revision = 1) noexcept {
  if (limit == 0 || bytes.size() > limit || !Fields(bytes, out.fields, revision)) return false;
  const auto& f = out.fields;
  for (auto index : {0u, 1u, 3u, 5u}) if (!Identity(f[index])) return false;
  if (!Boolean(f[8]) || f[9].size() != 8 || f[10].size() != 8 || f[11].size() != 8 ||
      !CodeVector(f[6], 19) || !CodeVector(f[7], 12)) return false;
  if (f[4].size() < 2 || Read(f[4].first(2)) * 16 != f[4].size() - 2) return false;
  for (std::size_t offset = 2; offset < f[4].size(); offset += 16)
    if (!Identity(f[4].subspan(offset, 16))) return false;
  if (!Fields(f[2], out.identifier)) return false;
  const auto& q = out.identifier;
  if (q[0].size() != 1 || q[0][0] < 1 || q[0][0] > 4 || !Boolean(q[2]) ||
      q[3].size() != 8 || q[4].size() != 8 || q[1].size() < 2 ||
      Read(q[1].first(2)) != q[0][0]) return false;
  if (q[2][0] ? Read(q[4]) == 0 : (Read(q[3]) != 0 || Read(q[4]) != 0)) return false;
  out.component_count = q[0][0];
  std::size_t offset = 2;
  for (std::size_t i = 0; i < out.component_count; ++i) {
    if (q[1].size() - offset < 4) return false;
    const auto size = Read(q[1].subspan(offset, 4)); offset += 4;
    if (size > q[1].size() - offset || !Fields(q[1].subspan(offset, size), out.components[i])) return false;
    const auto& component = out.components[i];
    if (!Text(component[0]) || !Boolean(component[1]) || !Boolean(component[2])) return false;
    offset += size;
  }
  return offset == q[1].size();
}
inline void Field(Bytes& output, unsigned id, View bytes) {
  Integer(output, id, 2); Integer(output, bytes.size(), 4);
  output.insert(output.end(), bytes.begin(), bytes.end());
}
inline void Scalar(Bytes& output, unsigned id, std::uint64_t value, unsigned width) {
  Integer(output, id, 2); Integer(output, width, 4); Integer(output, value, width);
}
inline View StringBytes(std::string_view value) noexcept {
  return {reinterpret_cast<const std::uint8_t*>(value.data()), value.size()};
}
inline bool Measure(const PsNameResolveRequestV1& value, std::size_t limit, std::size_t& total) noexcept {
  const auto& q = value.source_identifier;
  if (limit == 0 || q.qualification_kind < 1 || q.qualification_kind > 4 || q.components.size() != q.qualification_kind ||
      (q.source_span_present ? q.source_length_utf8_bytes == 0 : (q.source_start_utf8_byte != 0 || q.source_length_utf8_bytes != 0)) ||
      value.search_path.size() > 65535 || !Codes(value.object_kind_filter, 19) || !Codes(value.required_privileges, 12)) return false;
  for (const auto& id : {value.session_uuid, value.identifier_profile_uuid, value.current_schema_uuid, value.session_language_uuid})
    if (!core::uuid::IsEngineIdentityUuid(id)) return false;
  for (const auto& id : value.search_path) if (!core::uuid::IsEngineIdentityUuid(id)) return false;
  std::size_t qualified_size = 52; // revision, five TLVs, enum, count, bool and two u64s
  for (const auto& component : q.components) {
    if (!Text(StringBytes(component.spelling)) || component.spelling.size() > std::numeric_limits<std::uint32_t>::max() - 22 ||
        component.spelling.size() > limit || qualified_size > limit - component.spelling.size()) return false;
    qualified_size += component.spelling.size();
    if (limit - qualified_size < 26) return false;
    qualified_size += 26; // vector element length + component revision/three TLVs/two bools
  }
  if (qualified_size > std::numeric_limits<std::uint32_t>::max()) return false;
  total = qualified_size;
  const auto rest = 169 + value.search_path.size() * 16 + value.object_kind_filter.size() + value.required_privileges.size();
  if (total > limit || rest > limit - total) return false;
  total += rest; return true;
}
} // namespace name_request_detail

inline bool EncodePsNameResolveRequestV1(const PsNameResolveRequestV1& value, std::size_t maximum_payload_bytes,
                                         std::vector<std::uint8_t>* output) {
  namespace d = name_request_detail;
  std::size_t size = 0;
  if (!output || !d::Measure(value, maximum_payload_bytes, size)) return false;
  const auto& q = value.source_identifier;
  d::Bytes components; d::Integer(components, q.components.size(), 2);
  for (const auto& item : q.components) {
    d::Bytes component{1, 0};
    d::Field(component, 1, d::StringBytes(item.spelling));
    d::Scalar(component, 2, item.quoted, 1); d::Scalar(component, 3, item.case_folded, 1);
    d::Integer(components, component.size(), 4); components.insert(components.end(), component.begin(), component.end());
  }
  d::Bytes qualified{1, 0};
  d::Scalar(qualified, 1, q.qualification_kind, 1); d::Field(qualified, 2, components);
  d::Scalar(qualified, 3, q.source_span_present, 1);
  d::Scalar(qualified, 4, q.source_start_utf8_byte, 8); d::Scalar(qualified, 5, q.source_length_utf8_bytes, 8);
  d::Bytes search; d::Integer(search, value.search_path.size(), 2);
  for (const auto& id : value.search_path) search.insert(search.end(), id.bytes.begin(), id.bytes.end());
  const auto codes = [](const auto& values) {
    d::Bytes result; d::Integer(result, values.size(), 2); result.insert(result.end(), values.begin(), values.end()); return result;
  };
  d::Bytes staged{1, 0}; staged.reserve(size);
  d::Field(staged, 1, value.session_uuid.bytes); d::Field(staged, 2, value.identifier_profile_uuid.bytes);
  d::Field(staged, 3, qualified); d::Field(staged, 4, value.current_schema_uuid.bytes); d::Field(staged, 5, search);
  d::Field(staged, 6, value.session_language_uuid.bytes); d::Field(staged, 7, codes(value.object_kind_filter));
  d::Field(staged, 8, codes(value.required_privileges)); d::Scalar(staged, 9, value.include_grant_summary, 1);
  d::Scalar(staged, 10, value.catalog_generation, 8); d::Scalar(staged, 11, value.security_epoch, 8);
  d::Scalar(staged, 12, value.policy_generation, 8);
  if (staged.size() != size) return false;
  *output = std::move(staged); return true;
}

inline PsNameResolveRequestV1 ProjectPsNameResolveRequestView(
    const name_request_detail::RequestView& view) {
  namespace d = name_request_detail;
  const auto& f = view.fields; const auto& q = view.identifier;
  PsNameResolveRequestV1 staged;
  staged.session_uuid = d::IdentityValue(f[0]); staged.identifier_profile_uuid = d::IdentityValue(f[1]);
  staged.current_schema_uuid = d::IdentityValue(f[3]); staged.session_language_uuid = d::IdentityValue(f[5]);
  staged.source_identifier.qualification_kind = q[0][0];
  staged.source_identifier.source_span_present = q[2][0];
  staged.source_identifier.source_start_utf8_byte = d::Read(q[3]);
  staged.source_identifier.source_length_utf8_bytes = d::Read(q[4]);
  staged.source_identifier.components.reserve(view.component_count);
  for (std::size_t i = 0; i < view.component_count; ++i) {
    const auto& c = view.components[i];
    staged.source_identifier.components.push_back({std::string(reinterpret_cast<const char*>(c[0].data()), c[0].size()), c[1][0] != 0, c[2][0] != 0});
  }
  staged.search_path.reserve(d::Read(f[4].first(2)));
  for (std::size_t offset = 2; offset < f[4].size(); offset += 16) staged.search_path.push_back(d::IdentityValue(f[4].subspan(offset, 16)));
  staged.object_kind_filter.assign(f[6].begin() + 2, f[6].end());
  staged.required_privileges.assign(f[7].begin() + 2, f[7].end()); staged.include_grant_summary = f[8][0];
  staged.catalog_generation = d::Read(f[9]); staged.security_epoch = d::Read(f[10]); staged.policy_generation = d::Read(f[11]);
  static_assert(std::is_nothrow_move_assignable_v<PsNameResolveRequestV1>);
  return staged;
}

inline bool DecodePsNameResolveRequestV1(std::span<const std::uint8_t> bytes, std::size_t maximum_payload_bytes,
                                         PsNameResolveRequestV1* output) {
  name_request_detail::RequestView view;
  if (!output || !name_request_detail::Inspect(bytes, maximum_payload_bytes, view)) return false;
  auto staged = ProjectPsNameResolveRequestView(view);
  *output = std::move(staged); return true;
}

inline bool EncodePsNameResolveRequestV2(const PsNameResolveRequestV2& value,
    std::size_t maximum_payload_bytes, std::vector<std::uint8_t>* output) {
  namespace d = name_request_detail;
  constexpr std::size_t added_bytes = 37; // selector TLV(24) + flags TLV(1)
  if (!output || maximum_payload_bytes < added_bytes || value.local_transaction_id == 0 ||
      !core::uuid::IsEngineIdentityUuid(value.transaction_uuid) || (value.projection_flags & ~1u) != 0)
    return false;
  d::Bytes staged;
  if (!EncodePsNameResolveRequestV1(value.name, maximum_payload_bytes - added_bytes, &staged)) return false;
  staged.reserve(staged.size() + added_bytes);
  staged[0] = 2;
  d::Integer(staged, 13, 2); d::Integer(staged, 24, 4);
  d::Integer(staged, value.local_transaction_id, 8);
  staged.insert(staged.end(), value.transaction_uuid.bytes.begin(), value.transaction_uuid.bytes.end());
  d::Scalar(staged, 14, value.projection_flags, 1);
  *output = std::move(staged); return true;
}

inline bool DecodePsNameResolveRequestV2(std::span<const std::uint8_t> bytes,
    std::size_t maximum_payload_bytes, PsNameResolveRequestV2* output) {
  namespace d = name_request_detail;
  std::array<d::View, 14> fields;
  if (!output || maximum_payload_bytes == 0 || bytes.size() > maximum_payload_bytes ||
      !d::Fields(bytes, fields, 2) || fields[12].size() != 24 || fields[13].size() != 1 ||
      d::Read(fields[12].first(8)) == 0 || !d::Identity(fields[12].subspan(8)) ||
      (fields[13][0] & ~1u) != 0) return false;
  const auto base_size = static_cast<std::size_t>(fields[11].data() + fields[11].size() - bytes.data());
  d::RequestView view;
  if (!d::Inspect(bytes.first(base_size), maximum_payload_bytes, view, 2)) return false;
  PsNameResolveRequestV2 staged;
  staged.name = ProjectPsNameResolveRequestView(view);
  staged.local_transaction_id = d::Read(fields[12].first(8));
  staged.transaction_uuid = d::IdentityValue(fields[12].subspan(8));
  staged.projection_flags = fields[13][0];
  static_assert(std::is_nothrow_move_assignable_v<PsNameResolveRequestV2>);
  *output = std::move(staged); return true;
}

inline bool MatchesPsNameResolveContextV1(const PsNameResolveRequestV1& request, const PsNameResolveContextV1& context) noexcept {
  for (const auto& id : {context.session_uuid, context.identifier_profile_uuid, context.current_schema_uuid, context.session_language_uuid})
    if (!core::uuid::IsEngineIdentityUuid(id)) return false;
  for (const auto& id : context.search_path) if (!core::uuid::IsEngineIdentityUuid(id)) return false;
  return request.session_uuid == context.session_uuid && request.identifier_profile_uuid == context.identifier_profile_uuid &&
      request.current_schema_uuid == context.current_schema_uuid && request.session_language_uuid == context.session_language_uuid &&
      request.search_path == context.search_path && request.catalog_generation == context.catalog_generation &&
      request.security_epoch == context.security_epoch && request.policy_generation == context.policy_generation;
}

inline bool MatchesPsNameResolveContextV2(const PsNameResolveRequestV2& request,
                                         const PsNameResolveContextV2& context) noexcept {
  return MatchesPsNameResolveContextV1(request.name, context.name) &&
      context.local_transaction_id != 0 && core::uuid::IsEngineIdentityUuid(context.transaction_uuid) &&
      request.local_transaction_id == context.local_transaction_id && request.transaction_uuid == context.transaction_uuid &&
      (request.projection_flags & ~1u) == 0 &&
      (request.projection_flags == 0 || context.relation_projection_allowed);
}
} // namespace scratchbird::parser::ipc
