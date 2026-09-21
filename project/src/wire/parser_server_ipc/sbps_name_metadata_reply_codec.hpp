// Copyright (c) 2026 ScratchBird Software Inc.
// SPDX-License-Identifier: MPL-2.0
#pragma once

#include "sbps_name_resolution_request_codec.hpp"
#include "public_relation_projection_codec.hpp"
#include "../message_vector_set_codec.hpp"

#include <optional>

namespace scratchbird::parser::ipc {
inline constexpr std::uint32_t kPsNameResolveResponseSchemaV1 = 1033;
inline constexpr std::uint32_t kPsNameResolveResponseSchemaV2 = 1033;
inline constexpr std::uint32_t kPsUuidRenderRequestSchemaV1 = 1034;
inline constexpr std::uint32_t kPsUuidRenderResponseSchemaV1 = 1035;
struct PsGrantSummaryV1 {
  std::uint64_t generation{0};
  std::vector<std::uint8_t> effective_privileges;
  std::uint8_t visibility{0};
  bool operator==(const PsGrantSummaryV1&) const = default;
};
struct PsNameResolveResponseV1 {
  std::uint8_t outcome{0};
  core::platform::Uuid resolved_object_uuid;
  std::optional<std::uint8_t> resolved_object_kind;
  std::optional<std::string> visible_display_name;
  std::optional<PsGrantSummaryV1> grant_summary;
  std::uint64_t catalog_generation{0}, security_epoch{0}, name_resolution_epoch{0};
  std::vector<std::uint8_t> canonical_messages;
  bool operator==(const PsNameResolveResponseV1&) const = default;
};
struct PsNameResolveResponseV2 {
  PsNameResolveResponseV1 name;
  std::uint64_t local_transaction_id{0};
  core::platform::Uuid transaction_uuid;
  std::optional<PublicRelationProjectionV4> relation_projection;
  bool operator==(const PsNameResolveResponseV2&) const = default;
};
// Separately held expected metadata view, never reconstructed from a reply.
struct PsNameResolveResponseContextV2 {
  PsNameResolveContextV2 request;
  std::uint64_t name_resolution_epoch{0}, resource_epoch{0};
  core::platform::Uuid datatype_catalog_snapshot_uuid;
  std::uint64_t datatype_catalog_generation{0}, datatype_registry_generation{0};
};
struct PsUuidRenderRequestV1 {
  core::platform::Uuid session_uuid, object_uuid;
  std::uint8_t requested_name_class{0};
  core::platform::Uuid session_language_uuid, identifier_profile_uuid;
  std::uint8_t disclosure_context{0};
  bool operator==(const PsUuidRenderRequestV1&) const = default;
};
struct PsUuidRenderResponseV1 {
  std::uint8_t outcome{0};
  std::optional<std::vector<std::string>> display_path;
  std::optional<std::vector<bool>> quoted_flags;
  core::platform::Uuid language_uuid;
  std::uint64_t name_resolution_epoch{0};
  std::vector<std::uint8_t> canonical_messages;
  bool operator==(const PsUuidRenderResponseV1&) const = default;
};
namespace name_reply_detail {
namespace tlv = name_request_detail;
using View = tlv::View;
using Bytes = tlv::Bytes;
template<std::size_t N> inline bool OptionalFields(View bytes, std::array<std::optional<View>, N>& fields,
                                                std::uint16_t revision = 1) noexcept {
  if (bytes.size() < 2 || tlv::Read(bytes.first(2)) != revision) return false;
  std::uint64_t prior = 0;
  for (std::size_t offset = 2; offset < bytes.size();) {
    if (bytes.size() - offset < 6) return false;
    const auto id = tlv::Read(bytes.subspan(offset, 2)), size = tlv::Read(bytes.subspan(offset + 2, 4));
    offset += 6;
    if (id <= prior || id > N || size > bytes.size() - offset) return false;
    fields[id - 1] = bytes.subspan(offset, size); prior = id; offset += size;
  }
  return true;
}
inline bool Utf8(View bytes) noexcept {
  return std::find(bytes.begin(), bytes.end(), 0) == bytes.end() && core::datatypes::ValidateCanonicalUtf8(bytes.data(), bytes.size());
}
inline bool Nil(View bytes) noexcept { return bytes.size() == 16 && std::ranges::all_of(bytes, [](auto b) { return b == 0; }); }
inline bool Grant(const PsGrantSummaryV1& grant, std::uint64_t security_epoch) noexcept {
  return grant.generation == security_epoch && grant.visibility >= 1 && grant.visibility <= 2 &&
      (grant.effective_privileges.empty() || tlv::Codes(grant.effective_privileges, 12));
}
inline bool MessageSetValid(View canonical, bool required_nonempty) {
  wire::message_vector::MessageSet parsed;
  return wire::message_vector::DecodeMessageSet(canonical, &parsed) == wire::message_vector::ValueCodecError::none &&
      (!required_nonempty || !parsed.records.empty());
}
inline bool NeedsMessages(unsigned outcome, bool permit_empty_hidden) noexcept {
  return outcome != 1 && !(outcome == 3 && permit_empty_hidden);
}
inline bool Measure(const PsNameResolveResponseV1& value, std::size_t limit, std::size_t& size) noexcept {
  if (!limit || value.outcome < 1 || value.outcome > 6 || value.canonical_messages.size() > 1024u * 1024u) return false;
  if (value.outcome == 1) {
    if (!core::uuid::IsEngineIdentityUuid(value.resolved_object_uuid) || !value.resolved_object_kind ||
        *value.resolved_object_kind < 1 || *value.resolved_object_kind > 19 || !value.visible_display_name) return false;
  } else if (!value.resolved_object_uuid.is_nil() || value.resolved_object_kind || value.visible_display_name || value.grant_summary) return false;
  size = 83 + value.canonical_messages.size();
  if (size > limit) return false;
  const auto add = [&](std::size_t bytes) { if (bytes > limit - size) return false; size += bytes; return true; };
  if (value.resolved_object_kind && !add(7)) return false;
  if (value.visible_display_name) {
    const auto& name = *value.visible_display_name;
    if (name.size() > std::numeric_limits<std::uint32_t>::max() || !Utf8(tlv::StringBytes(name)) || !add(6) || !add(name.size())) return false;
  }
  return !value.grant_summary || (Grant(*value.grant_summary, value.security_epoch) && add(37 + value.grant_summary->effective_privileges.size()));
}
struct ReplyView {
  std::array<std::optional<View>, 9> fields;
  std::array<View, 3> grant;
  View messages;
  std::uint8_t outcome{0};
};
inline bool Inspect(View bytes, std::size_t limit, ReplyView& out, std::uint16_t revision = 1) noexcept {
  if (!limit || bytes.size() > limit || !OptionalFields(bytes, out.fields, revision)) return false;
  const auto& f = out.fields;
  for (auto field : {0u, 1u, 5u, 6u, 7u, 8u}) if (!f[field]) return false;
  if (f[0]->size() != 1 || (*f[0])[0] < 1 || (*f[0])[0] > 6 || f[5]->size() != 8 || f[6]->size() != 8 || f[7]->size() != 8) return false;
  out.outcome = (*f[0])[0];
  if (out.outcome == 1) {
    if (!tlv::Identity(*f[1]) || !f[2] || f[2]->size() != 1 || (*f[2])[0] < 1 || (*f[2])[0] > 19 || !f[3] || !Utf8(*f[3])) return false;
    if (f[4]) {
      if (!tlv::Fields(*f[4], out.grant)) return false;
      const auto& g = out.grant;
      if (g[0].size() != 8 || tlv::Read(g[0]) != tlv::Read(*f[6]) || g[2].size() != 1 || g[2][0] < 1 || g[2][0] > 2 ||
          g[1].size() < 2 || tlv::Read(g[1].first(2)) != g[1].size() - 2 ||
          (g[1].size() > 2 && !tlv::Codes(g[1].subspan(2), 12))) return false;
    }
  } else if (!Nil(*f[1]) || f[2] || f[3] || f[4]) return false;
  if (f[8]->size() < 4 || tlv::Read(f[8]->first(4)) != f[8]->size() - 4 || f[8]->size() - 4 > 1024u * 1024u) return false;
  out.messages = f[8]->subspan(4); return true;
}
} // namespace name_reply_detail

// permit_empty_hidden is supplied by an admitted disclosure policy, never by
// a wire field. All other non-resolved replies require a nonempty MessageSet.
inline bool EncodePsNameResolveResponseV1(const PsNameResolveResponseV1& value, std::size_t limit,
    bool permit_empty_hidden, std::vector<std::uint8_t>* output) {
  namespace d = name_reply_detail;
  namespace tlv = name_request_detail;
  std::size_t size;
  if (!output || !d::Measure(value, limit, size) || !d::MessageSetValid(value.canonical_messages, d::NeedsMessages(value.outcome, permit_empty_hidden))) return false;
  tlv::Bytes staged{1, 0}; staged.reserve(size);
  tlv::Scalar(staged, 1, value.outcome, 1); tlv::Field(staged, 2, value.resolved_object_uuid.bytes);
  if (value.resolved_object_kind) tlv::Scalar(staged, 3, *value.resolved_object_kind, 1);
  if (value.visible_display_name) tlv::Field(staged, 4, tlv::StringBytes(*value.visible_display_name));
  if (value.grant_summary) {
    const auto& summary = *value.grant_summary;
    tlv::Bytes grant{1, 0}; tlv::Scalar(grant, 1, summary.generation, 8);
    tlv::Integer(grant, 2, 2); tlv::Integer(grant, 2 + summary.effective_privileges.size(), 4);
    tlv::Integer(grant, summary.effective_privileges.size(), 2);
    grant.insert(grant.end(), summary.effective_privileges.begin(), summary.effective_privileges.end());
    tlv::Scalar(grant, 3, summary.visibility, 1); tlv::Field(staged, 5, grant);
  }
  tlv::Scalar(staged, 6, value.catalog_generation, 8); tlv::Scalar(staged, 7, value.security_epoch, 8);
  tlv::Scalar(staged, 8, value.name_resolution_epoch, 8);
  tlv::Integer(staged, 9, 2); tlv::Integer(staged, 4 + value.canonical_messages.size(), 4);
  tlv::Integer(staged, value.canonical_messages.size(), 4);
  staged.insert(staged.end(), value.canonical_messages.begin(), value.canonical_messages.end());
  if (staged.size() != size) return false;
  *output = std::move(staged); return true;
}
namespace name_reply_detail {
inline PsNameResolveResponseV1 ProjectReply(const ReplyView& view) {
  namespace tlv = name_request_detail;
  const auto& f = view.fields;
  PsNameResolveResponseV1 staged;
  staged.outcome = view.outcome; staged.resolved_object_uuid = tlv::IdentityValue(*f[1]);
  if (f[2]) staged.resolved_object_kind = (*f[2])[0];
  if (f[3]) staged.visible_display_name.emplace(reinterpret_cast<const char*>(f[3]->data()), f[3]->size());
  if (f[4]) {
    staged.grant_summary.emplace(); auto& grant = *staged.grant_summary;
    grant.generation = tlv::Read(view.grant[0]); grant.visibility = view.grant[2][0];
    grant.effective_privileges.assign(view.grant[1].begin() + 2, view.grant[1].end());
  }
  staged.catalog_generation = tlv::Read(*f[5]); staged.security_epoch = tlv::Read(*f[6]); staged.name_resolution_epoch = tlv::Read(*f[7]);
  staged.canonical_messages.assign(view.messages.begin(), view.messages.end());
  return staged;
}
} // namespace name_reply_detail
inline bool DecodePsNameResolveResponseV1(std::span<const std::uint8_t> bytes, std::size_t limit,
    bool permit_empty_hidden, PsNameResolveResponseV1* output) {
  namespace d = name_reply_detail;
  d::ReplyView view;
  if (!output || !d::Inspect(bytes, limit, view) || !d::MessageSetValid(view.messages, d::NeedsMessages(view.outcome, permit_empty_hidden))) return false;
  auto staged = d::ProjectReply(view);
  static_assert(std::is_nothrow_move_assignable_v<PsNameResolveResponseV1>);
  *output = std::move(staged); return true;
}

inline bool EncodePsNameResolveResponseV2(const PsNameResolveResponseV2& value, std::size_t limit,
    bool permit_empty_hidden, std::vector<std::uint8_t>* output) {
  namespace d = name_reply_detail;
  namespace tlv = name_request_detail;
  if (!output || limit < 30 || !value.local_transaction_id ||
      !core::uuid::IsEngineIdentityUuid(value.transaction_uuid)) return false;
  std::size_t extra = 30, projection_size = 0, base_size = 0;
  if (value.relation_projection) {
    if (value.name.outcome != 1 || value.relation_projection->relation_uuid != value.name.resolved_object_uuid ||
        limit - extra < 8 || !relation_projection_detail::Measure(*value.relation_projection,
            limit - extra - 8, projection_size)) return false;
    extra += 8 + projection_size;
  }
  if (!d::Measure(value.name, limit - extra, base_size)) return false;
  tlv::Bytes staged;
  if (!EncodePsNameResolveResponseV1(value.name, limit - extra, permit_empty_hidden, &staged)) return false;
  staged.reserve(base_size + extra);
  staged[0] = 2;
  tlv::Integer(staged, 10, 2); tlv::Integer(staged, 24, 4);
  tlv::Integer(staged, value.local_transaction_id, 8);
  staged.insert(staged.end(), value.transaction_uuid.bytes.begin(), value.transaction_uuid.bytes.end());
  if (value.relation_projection) {
    tlv::Bytes projection;
    if (!EncodePublicRelationProjectionV4(*value.relation_projection, projection_size, &projection)) return false;
    tlv::Integer(staged, 11, 2); tlv::Integer(staged, 2 + projection.size(), 4);
    staged.push_back(kPublicRelationProjectionKindV4); staged.push_back(kPublicRelationProjectionVersionV4);
    staged.insert(staged.end(), projection.begin(), projection.end());
  }
  *output = std::move(staged); return true;
}

inline bool DecodePsNameResolveResponseV2(std::span<const std::uint8_t> bytes, std::size_t limit,
    bool permit_empty_hidden, PsNameResolveResponseV2* output) {
  namespace d = name_reply_detail;
  namespace tlv = name_request_detail;
  std::array<std::optional<tlv::View>, 11> fields;
  if (!output || bytes.size() > limit || !d::OptionalFields(bytes, fields, 2) ||
      !fields[9] || fields[9]->size() != 24 || !tlv::Read(fields[9]->first(8)) ||
      !tlv::Identity(fields[9]->subspan(8))) return false;
  const auto base_size = static_cast<std::size_t>(fields[9]->data() - bytes.data()) - 6;
  d::ReplyView view;
  if (!d::Inspect(bytes.first(base_size), limit, view, 2)) return false;
  if (fields[10]) {
    const auto projection = *fields[10];
    PublicRelationProjectionV4 header;
    std::uint32_t count;
    if (view.outcome != 1 || projection.size() < 2 ||
        projection[0] != kPublicRelationProjectionKindV4 || projection[1] != kPublicRelationProjectionVersionV4 ||
        !relation_projection_detail::Inspect(projection.subspan(2), limit, header, count) ||
        header.relation_uuid != tlv::IdentityValue(*view.fields[1])) return false;
  }
  if (!d::MessageSetValid(view.messages, d::NeedsMessages(view.outcome, permit_empty_hidden))) return false;
  PsNameResolveResponseV2 staged;
  staged.name = d::ProjectReply(view);
  staged.local_transaction_id = tlv::Read(fields[9]->first(8));
  staged.transaction_uuid = tlv::IdentityValue(fields[9]->subspan(8));
  if (fields[10]) {
    staged.relation_projection.emplace();
    if (!DecodePublicRelationProjectionV4(fields[10]->subspan(2), limit, &*staged.relation_projection)) return false;
  }
  static_assert(std::is_nothrow_move_assignable_v<PsNameResolveResponseV2>);
  *output = std::move(staged); return true;
}

// Success-only correlation; structural decoding must precede this check.
// This neither issues a descriptor nor looks up a live engine transaction.
inline bool MatchesPsResolvedNameResponseV2(const PsNameResolveResponseV2& response,
    const PsNameResolveRequestV2& request, const PsNameResolveResponseContextV2& expected) noexcept {
  const auto& name = response.name;
  if (!MatchesPsNameResolveContextV2(request, expected.request) || name.outcome != 1 ||
      response.local_transaction_id != request.local_transaction_id || response.transaction_uuid != request.transaction_uuid ||
      name.catalog_generation != expected.request.name.catalog_generation ||
      name.security_epoch != expected.request.name.security_epoch || name.name_resolution_epoch != expected.name_resolution_epoch ||
      !name.resolved_object_kind || std::ranges::find(request.name.object_kind_filter, *name.resolved_object_kind) == request.name.object_kind_filter.end() ||
      (name.grant_summary && !request.name.include_grant_summary) ||
      response.relation_projection.has_value() != (request.projection_flags == 1)) return false;
  if (!response.relation_projection) return true;
  const auto& projection = *response.relation_projection;
  return projection.relation_uuid == name.resolved_object_uuid &&
      projection.validated_resource_epoch == expected.resource_epoch &&
      projection.datatype_catalog_snapshot_uuid == expected.datatype_catalog_snapshot_uuid &&
      projection.datatype_catalog_generation == expected.datatype_catalog_generation &&
      projection.datatype_registry_generation == expected.datatype_registry_generation;
}

inline bool EncodePsUuidRenderRequestV1(const PsUuidRenderRequestV1& value, std::size_t limit, std::vector<std::uint8_t>* output) {
  namespace tlv = name_request_detail;
  if (!output || limit < 104 || value.requested_name_class < 1 || value.requested_name_class > 4 ||
      value.disclosure_context < 1 || value.disclosure_context > 5) return false;
  for (const auto& id : {value.session_uuid, value.object_uuid, value.session_language_uuid, value.identifier_profile_uuid})
    if (!core::uuid::IsEngineIdentityUuid(id)) return false;
  tlv::Bytes staged{1, 0}; staged.reserve(104);
  tlv::Field(staged, 1, value.session_uuid.bytes); tlv::Field(staged, 2, value.object_uuid.bytes);
  tlv::Scalar(staged, 3, value.requested_name_class, 1); tlv::Field(staged, 4, value.session_language_uuid.bytes);
  tlv::Field(staged, 5, value.identifier_profile_uuid.bytes); tlv::Scalar(staged, 6, value.disclosure_context, 1);
  *output = std::move(staged); return true;
}
inline bool DecodePsUuidRenderRequestV1(std::span<const std::uint8_t> bytes, std::size_t limit, PsUuidRenderRequestV1* output) {
  namespace tlv = name_request_detail;
  std::array<tlv::View, 6> f;
  if (!output || bytes.size() > limit || !tlv::Fields(bytes, f) || f[2].size() != 1 || f[2][0] < 1 || f[2][0] > 4 ||
      f[5].size() != 1 || f[5][0] < 1 || f[5][0] > 5) return false;
  for (auto field : {0u, 1u, 3u, 4u}) if (!tlv::Identity(f[field])) return false;
  *output = {tlv::IdentityValue(f[0]), tlv::IdentityValue(f[1]), f[2][0], tlv::IdentityValue(f[3]), tlv::IdentityValue(f[4]), f[5][0]};
  return true;
}

namespace name_reply_detail {
inline bool RenderMeasure(const PsUuidRenderResponseV1& value, std::size_t limit, std::size_t& size) noexcept {
  if (!limit || value.outcome < 1 || value.outcome > 4 || value.canonical_messages.size() > 1024u * 1024u) return false;
  size = 55 + value.canonical_messages.size();
  if (size > limit) return false;
  if (value.outcome != 1) return !value.display_path && !value.quoted_flags && value.language_uuid.is_nil();
  if (!value.display_path || !value.quoted_flags || value.display_path->empty() || value.display_path->size() > 4 ||
      value.display_path->size() != value.quoted_flags->size() || !core::uuid::IsEngineIdentityUuid(value.language_uuid) || limit - size < 16) return false;
  size += 16;
  std::uint64_t path_size = 2;
  for (const auto& item : *value.display_path) {
    constexpr auto field_max = std::numeric_limits<std::uint32_t>::max();
    if (path_size > field_max - 4 || item.size() > field_max - path_size - 4 ||
        !Utf8(tlv::StringBytes(item)) || limit - size < 5) return false;
    path_size += 4 + item.size();
    size += 5;
    if (item.size() > limit - size) return false;
    size += item.size();
  }
  return true;
}
struct RenderView {
  std::array<std::optional<View>, 6> fields;
  std::array<View, 4> path;
  std::size_t path_count{0};
  std::uint8_t outcome{0};
  View messages;
};
inline bool InspectRender(View bytes, std::size_t limit, RenderView& out) noexcept {
  if (!limit || bytes.size() > limit || !OptionalFields(bytes, out.fields)) return false;
  const auto& f = out.fields;
  for (auto i : {0u, 3u, 4u, 5u}) if (!f[i]) return false;
  if (f[0]->size() != 1 || (*f[0])[0] < 1 || (*f[0])[0] > 4 || f[4]->size() != 8) return false;
  out.outcome = (*f[0])[0];
  if (out.outcome == 1) {
    if (!f[1] || !f[2] || !tlv::Identity(*f[3]) || f[1]->size() < 2 || f[2]->size() < 2) return false;
    out.path_count = tlv::Read(f[1]->first(2));
    if (!out.path_count || out.path_count > 4 || tlv::Read(f[2]->first(2)) != out.path_count || f[2]->size() != 2 + out.path_count) return false;
    std::size_t offset = 2;
    for (std::size_t i = 0; i < out.path_count; ++i) {
      if ((*f[2])[i + 2] > 1 || f[1]->size() - offset < 4) return false;
      const auto size = tlv::Read(f[1]->subspan(offset, 4)); offset += 4;
      if (size > f[1]->size() - offset || !Utf8(f[1]->subspan(offset, size))) return false;
      out.path[i] = f[1]->subspan(offset, size); offset += size;
    }
    if (offset != f[1]->size()) return false;
  } else if (f[1] || f[2] || !Nil(*f[3])) return false;
  if (f[5]->size() < 4 || tlv::Read(f[5]->first(4)) != f[5]->size() - 4 || f[5]->size() - 4 > 1024u * 1024u) return false;
  out.messages = f[5]->subspan(4); return true;
}
} // namespace name_reply_detail

inline bool EncodePsUuidRenderResponseV1(const PsUuidRenderResponseV1& value, std::size_t limit, std::vector<std::uint8_t>* output) {
  namespace d = name_reply_detail;
  namespace tlv = name_request_detail;
  std::size_t size;
  if (!output || !d::RenderMeasure(value, limit, size) || !d::MessageSetValid(value.canonical_messages, value.outcome != 1)) return false;
  tlv::Bytes staged{1, 0}; staged.reserve(size); tlv::Scalar(staged, 1, value.outcome, 1);
  if (value.display_path) {
    tlv::Bytes path; tlv::Integer(path, value.display_path->size(), 2);
    for (const auto& item : *value.display_path) { tlv::Integer(path, item.size(), 4); path.insert(path.end(), item.begin(), item.end()); }
    tlv::Field(staged, 2, path);
    tlv::Integer(staged, 3, 2); tlv::Integer(staged, 2 + value.quoted_flags->size(), 4); tlv::Integer(staged, value.quoted_flags->size(), 2);
    for (bool quoted : *value.quoted_flags) staged.push_back(quoted ? 1 : 0);
  }
  tlv::Field(staged, 4, value.language_uuid.bytes); tlv::Scalar(staged, 5, value.name_resolution_epoch, 8);
  tlv::Integer(staged, 6, 2); tlv::Integer(staged, 4 + value.canonical_messages.size(), 4); tlv::Integer(staged, value.canonical_messages.size(), 4);
  staged.insert(staged.end(), value.canonical_messages.begin(), value.canonical_messages.end());
  if (staged.size() != size) return false;
  *output = std::move(staged); return true;
}
inline bool DecodePsUuidRenderResponseV1(std::span<const std::uint8_t> bytes, std::size_t limit, PsUuidRenderResponseV1* output) {
  namespace d = name_reply_detail;
  namespace tlv = name_request_detail;
  d::RenderView view;
  if (!output || !d::InspectRender(bytes, limit, view) || !d::MessageSetValid(view.messages, view.outcome != 1)) return false;
  PsUuidRenderResponseV1 staged; staged.outcome = view.outcome;
  if (view.outcome == 1) {
    staged.display_path.emplace(); staged.quoted_flags.emplace();
    staged.display_path->reserve(view.path_count); staged.quoted_flags->reserve(view.path_count);
    for (std::size_t i = 0; i < view.path_count; ++i) {
      staged.display_path->emplace_back(reinterpret_cast<const char*>(view.path[i].data()), view.path[i].size());
      staged.quoted_flags->push_back((*view.fields[2])[i + 2] != 0);
    }
  }
  staged.language_uuid = tlv::IdentityValue(*view.fields[3]); staged.name_resolution_epoch = tlv::Read(*view.fields[4]);
  staged.canonical_messages.assign(view.messages.begin(), view.messages.end());
  static_assert(std::is_nothrow_move_assignable_v<PsUuidRenderResponseV1>);
  *output = std::move(staged); return true;
}
} // namespace scratchbird::parser::ipc
