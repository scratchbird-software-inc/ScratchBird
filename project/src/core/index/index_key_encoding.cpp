// Copyright (c) 2026 ScratchBird Software Inc.
//
// This Source Code Form is subject to the terms of the Mozilla Public
// License, v. 2.0. If a copy of the MPL was not distributed with this
// file, You can obtain one at https://mozilla.org/MPL/2.0/.
//
// SPDX-License-Identifier: MPL-2.0

#include "index_key_encoding.hpp"

#include "uuid.hpp"

#include <algorithm>
#include <array>
#include <cctype>
#include <string_view>
#include <utility>

namespace scratchbird::core::index {
namespace {
using scratchbird::core::platform::DiagnosticArgument;
using scratchbird::core::platform::MakeDiagnostic;
using scratchbird::core::platform::Severity;
using scratchbird::core::platform::StatusCode;
using scratchbird::core::platform::Subsystem;
using scratchbird::core::uuid::ExtractUuidV7TimePrefix;

inline constexpr byte kNullRankFirst = 0x00;
inline constexpr byte kNonNullRank = 0x7f;
inline constexpr byte kNullRankLast = 0xff;
inline constexpr std::array<byte, 4> kOrderPreservingMagic = {'S', 'B', 'K', 'O'};
inline constexpr std::array<byte, 4> kUnsafeLegacyMagic = {'S', 'B', 'K', '1'};

Status OkStatus() { return {StatusCode::ok, Severity::info, Subsystem::engine}; }
Status ErrorStatus() { return {StatusCode::platform_required_feature_missing, Severity::error, Subsystem::engine}; }

bool StartsWith(std::string_view bytes, const std::array<byte, 4>& magic) {
  if (bytes.size() < magic.size()) {
    return false;
  }
  for (std::size_t i = 0; i < magic.size(); ++i) {
    if (static_cast<byte>(static_cast<unsigned char>(bytes[i])) != magic[i]) {
      return false;
    }
  }
  return true;
}

std::string_view OrderBytes(std::string_view bytes) {
  return bytes.substr(kOrderPreservingMagic.size());
}

int CompareUnsignedBytes(std::string_view left, std::string_view right) {
  const auto count = std::min(left.size(), right.size());
  for (std::size_t i = 0; i < count; ++i) {
    const auto l = static_cast<unsigned char>(left[i]);
    const auto r = static_cast<unsigned char>(right[i]);
    if (l < r) {
      return -1;
    }
    if (l > r) {
      return 1;
    }
  }
  return left.size() < right.size() ? -1 : (right.size() < left.size() ? 1 : 0);
}

byte NullRankFor(const IndexKeyEncodingComponent& component) {
  if (!component.is_null) {
    return kNonNullRank;
  }
  switch (component.null_placement) {
    case IndexKeyNullPlacement::nulls_first: return kNullRankFirst;
    case IndexKeyNullPlacement::nulls_last: return kNullRankLast;
    case IndexKeyNullPlacement::donor_profile_default: return kNullRankLast;
  }
  return kNullRankLast;
}

std::vector<byte> PayloadForOrder(const IndexKeyEncodingComponent& component) {
  std::vector<byte> payload = component.payload;
  if (component.case_folded) {
    for (byte& value : payload) {
      value = static_cast<byte>(std::tolower(static_cast<unsigned char>(value)));
    }
  }
  return payload;
}

void AppendEscapedPayload(std::vector<byte>* out, const std::vector<byte>& payload) {
  for (byte value : payload) {
    out->push_back(value);
    if (value == 0x00) {
      out->push_back(0xff);
    }
  }
  out->push_back(0x00);
  out->push_back(0x00);
}

void AppendEscapedPayloadPrefix(std::vector<byte>* out,
                                const std::vector<byte>& payload) {
  for (byte value : payload) {
    out->push_back(value);
    if (value == 0x00) {
      out->push_back(0xff);
    }
  }
}

std::vector<byte> ComparablePayloadForSort(const IndexKeyEncodingComponent& component) {
  std::vector<byte> comparable;
  AppendEscapedPayload(&comparable, PayloadForOrder(component));
  if (component.sort_direction == IndexKeySortDirection::descending) {
    for (byte& value : comparable) {
      value = static_cast<byte>(~value);
    }
  }
  return comparable;
}

std::vector<byte> ComparablePayloadPrefixForSort(
    const IndexKeyEncodingComponent& component) {
  std::vector<byte> comparable;
  AppendEscapedPayloadPrefix(&comparable, PayloadForOrder(component));
  if (component.sort_direction == IndexKeySortDirection::descending) {
    for (byte& value : comparable) {
      value = static_cast<byte>(~value);
    }
  }
  return comparable;
}

std::vector<byte> UuidV7Payload(const IndexKeyEncodingComponent& component) {
  return std::vector<byte>(component.uuid_v7_value.value.bytes.begin(),
                           component.uuid_v7_value.value.bytes.end());
}

IndexKeyEncodingResult RefuseEncoding(std::string code,
                                      std::string key,
                                      std::string detail = {}) {
  IndexKeyEncodingResult result;
  result.status = ErrorStatus();
  result.diagnostic = MakeIndexKeyEncodingDiagnostic(
      result.status, std::move(code), std::move(key), std::move(detail));
  return result;
}

IndexKeyPrefixBoundsResult RefusePrefix(std::string code,
                                        std::string key,
                                        std::string detail = {}) {
  IndexKeyPrefixBoundsResult result;
  result.status = ErrorStatus();
  result.diagnostic = MakeIndexKeyEncodingDiagnostic(
      result.status, std::move(code), std::move(key), std::move(detail));
  return result;
}

bool AdvanceLexicographicUpperPrefix(std::vector<byte>* bytes) {
  if (bytes == nullptr || bytes->size() <= kOrderPreservingMagic.size()) {
    return false;
  }
  for (std::size_t i = bytes->size(); i > kOrderPreservingMagic.size(); --i) {
    byte& value = (*bytes)[i - 1];
    if (value != 0xff) {
      value = static_cast<byte>(value + 1u);
      bytes->resize(i);
      return true;
    }
  }
  return false;
}

IndexKeyPrefixBoundsResult BuildEncodedPrefixBoundsInternal(
    const std::vector<IndexKeyEncodingComponent>& components,
    const IndexKeySemanticProfile& profile,
    std::string evidence_label) {
  if (components.empty()) {
    return RefusePrefix("SB-INDEX-KEY-PREFIX-EMPTY",
                        "index.key_prefix.empty");
  }
  if (!profile.bytewise_stable) {
    return RefusePrefix("SB-INDEX-KEY-PREFIX-UNSTABLE-PROFILE",
                        "index.key_prefix.unstable_profile_refused",
                        profile.profile_id);
  }

  IndexKeyPrefixBoundsResult result;
  result.status = OkStatus();
  result.requires_recheck = profile.requires_recheck;
  result.matcher_prefix.assign(kOrderPreservingMagic.begin(),
                               kOrderPreservingMagic.end());

  for (const auto& component : components) {
    if (component.kind == IndexKeyComponentKind::donor_raw) {
      return RefusePrefix("SB-INDEX-KEY-PREFIX-DONOR-RAW-REFUSED",
                          "index.key_prefix.donor_raw_refused",
                          std::to_string(component.ordinal));
    }
    if (component.null_placement == IndexKeyNullPlacement::donor_profile_default) {
      return RefusePrefix("SB-INDEX-KEY-PREFIX-DONOR-NULLS-REFUSED",
                          "index.key_prefix.donor_null_placement_refused",
                          std::to_string(component.ordinal));
    }
    if (!component.type_descriptor_uuid.valid()) {
      return RefusePrefix("SB-INDEX-KEY-PREFIX-TYPE-UUID-MISSING",
                          "index.key_prefix.type_uuid_missing",
                          std::to_string(component.ordinal));
    }

    result.lossy = result.lossy || component.lossy;
    result.requires_recheck = result.requires_recheck || component.lossy;
    result.matcher_prefix.push_back(NullRankFor(component));
    if (component.is_null) {
      continue;
    }

    IndexKeyEncodingComponent payload_component = component;
    if (component.kind == IndexKeyComponentKind::uuid_v7) {
      if (!component.uuid_v7_value.valid()) {
        return RefusePrefix("SB-INDEX-KEY-PREFIX-UUIDV7-MISSING",
                            "index.key_prefix.uuidv7_missing",
                            std::to_string(component.ordinal));
      }
      const auto time = ExtractUuidV7TimePrefix(component.uuid_v7_value.value);
      if (!time.ok) {
        return RefusePrefix("SB-INDEX-KEY-PREFIX-UUIDV7-REFUSED",
                            "index.key_prefix.uuidv7_refused",
                            time.refusal_reason);
      }
      payload_component.payload = UuidV7Payload(component);
      result.evidence.push_back("uuidv7_prefix_component=true");
      result.evidence.push_back("uuidv7_prefix_time_millis=" +
                                std::to_string(time.unix_epoch_millis));
    }
    const auto prefix_payload =
        ComparablePayloadPrefixForSort(payload_component);
    result.matcher_prefix.insert(result.matcher_prefix.end(),
                                 prefix_payload.begin(),
                                 prefix_payload.end());
  }

  result.lower_bound = result.matcher_prefix;
  result.upper_bound = result.matcher_prefix;
  result.upper_bound_unbounded =
      !AdvanceLexicographicUpperPrefix(&result.upper_bound);
  if (result.upper_bound_unbounded) {
    result.upper_bound.clear();
  }
  result.evidence.push_back("index_key_prefix_builder=" + std::move(evidence_label));
  result.evidence.push_back("index_key_prefix_magic=SBKO");
  result.evidence.push_back("index_key_prefix_matcher_bytes=" +
                            std::to_string(result.matcher_prefix.size()));
  result.evidence.push_back("index_key_prefix_lower_bound_generated=true");
  result.evidence.push_back("index_key_prefix_upper_bound_unbounded=" +
                            std::string(result.upper_bound_unbounded ? "true"
                                                                     : "false"));
  result.evidence.push_back("visibility_authority=false");
  result.evidence.push_back("transaction_finality_authority=false");
  return result;
}

}  // namespace

IndexKeyEncodingResult EncodeIndexKey(const std::vector<IndexKeyEncodingComponent>& components,
                                      const IndexKeySemanticProfile& profile) {
  if (components.empty()) {
    return RefuseEncoding("SB-INDEX-KEY-ENCODING-EMPTY",
                          "index.key_encoding.empty");
  }
  if (!profile.bytewise_stable) {
    return RefuseEncoding("SB-INDEX-KEY-ENCODING-UNSTABLE-PROFILE",
                          "index.key_encoding.unstable_profile_refused",
                          profile.profile_id);
  }
  IndexKeyEncodingResult result;
  result.status = OkStatus();
  result.requires_recheck = profile.requires_recheck;
  result.encoded.assign(kOrderPreservingMagic.begin(), kOrderPreservingMagic.end());

  for (const auto& component : components) {
    if (component.kind == IndexKeyComponentKind::donor_raw) {
      return RefuseEncoding("SB-INDEX-KEY-ENCODING-DONOR-RAW-REFUSED",
                            "index.key_encoding.donor_raw_refused",
                            std::to_string(component.ordinal));
    }
    if (component.null_placement == IndexKeyNullPlacement::donor_profile_default) {
      return RefuseEncoding("SB-INDEX-KEY-ENCODING-DONOR-NULLS-REFUSED",
                            "index.key_encoding.donor_null_placement_refused",
                            std::to_string(component.ordinal));
    }
    if (!component.type_descriptor_uuid.valid()) {
      return RefuseEncoding("SB-INDEX-KEY-ENCODING-TYPE-UUID-MISSING",
                            "index.key_encoding.type_uuid_missing",
                            std::to_string(component.ordinal));
    }

    if (component.kind == IndexKeyComponentKind::uuid_v7 && !component.is_null) {
      if (!component.uuid_v7_value.valid()) {
        return RefuseEncoding("SB-INDEX-KEY-ENCODING-UUIDV7-MISSING",
                              "index.key_encoding.uuidv7_missing",
                              std::to_string(component.ordinal));
      }
      const auto time = ExtractUuidV7TimePrefix(component.uuid_v7_value.value);
      if (!time.ok) {
        return RefuseEncoding("SB-INDEX-KEY-ENCODING-UUIDV7-REFUSED",
                              "index.key_encoding.uuidv7_refused",
                              time.refusal_reason);
      }
      result.uuid_v7_specialized = true;
      result.evidence.push_back("uuidv7_index_key_component=true");
      result.evidence.push_back("uuidv7_time_prefix_millis=" +
                                std::to_string(time.unix_epoch_millis));
      result.evidence.push_back("uuid_ordering_finality_authority=false");
      result.evidence.push_back("visibility_authority=false");
    }

    result.lossy = result.lossy || component.lossy;
    result.requires_recheck = result.requires_recheck || component.lossy;
    result.encoded.push_back(NullRankFor(component));
    if (!component.is_null) {
      const IndexKeyEncodingComponent payload_component =
          component.kind == IndexKeyComponentKind::uuid_v7
              ? [&component] {
                  IndexKeyEncodingComponent copy = component;
                  copy.payload = UuidV7Payload(component);
                  return copy;
                }()
              : component;
      const auto payload = ComparablePayloadForSort(payload_component);
      result.encoded.insert(result.encoded.end(), payload.begin(), payload.end());
    }
  }
  return result;
}

IndexKeyPrefixBoundsResult BuildEncodedPrefixMatcher(
    const std::vector<IndexKeyEncodingComponent>& components,
    const IndexKeySemanticProfile& profile) {
  return BuildEncodedPrefixBoundsInternal(components, profile, "matcher");
}

IndexKeyPrefixBoundsResult BuildEncodedPrefixLowerBound(
    const std::vector<IndexKeyEncodingComponent>& components,
    const IndexKeySemanticProfile& profile) {
  return BuildEncodedPrefixBoundsInternal(components, profile, "lower_bound");
}

IndexKeyPrefixBoundsResult BuildEncodedPrefixUpperBound(
    const std::vector<IndexKeyEncodingComponent>& components,
    const IndexKeySemanticProfile& profile) {
  return BuildEncodedPrefixBoundsInternal(components, profile, "upper_bound");
}

IndexKeyCompareResult CompareEncodedIndexKeys(const std::vector<byte>& left,
                                              const std::vector<byte>& right) {
  const std::string_view left_view(reinterpret_cast<const char*>(left.data()), left.size());
  const std::string_view right_view(reinterpret_cast<const char*>(right.data()), right.size());
  return CompareEncodedIndexKeyBytes(left_view, right_view);
}

IndexKeyCompareResult CompareEncodedIndexKeyBytes(std::string_view left,
                                                  std::string_view right) {
  IndexKeyCompareResult result;
  if (IsUnsafeLegacyIndexKeyEncoding(left) || IsUnsafeLegacyIndexKeyEncoding(right)) {
    result.status = ErrorStatus();
    result.diagnostic = MakeIndexKeyEncodingDiagnostic(
        result.status,
        "SB-INDEX-KEY-COMPARE-UNSAFE-LEGACY-ENVELOPE",
        "index.key_compare.unsafe_legacy_envelope");
    return result;
  }
  if (!IsOrderPreservingIndexKeyEncoding(left) ||
      !IsOrderPreservingIndexKeyEncoding(right)) {
    result.status = ErrorStatus();
    result.diagnostic = MakeIndexKeyEncodingDiagnostic(result.status,
                                                       "SB-INDEX-KEY-COMPARE-BAD-ENVELOPE",
                                                       "index.key_compare.bad_envelope");
    return result;
  }

  result.status = OkStatus();
  const auto left_order = OrderBytes(left);
  const auto right_order = OrderBytes(right);
  result.comparison = CompareUnsignedBytes(left_order, right_order);
  return result;
}

bool IsOrderPreservingIndexKeyEncoding(std::string_view bytes) {
  return StartsWith(bytes, kOrderPreservingMagic);
}

bool IsUnsafeLegacyIndexKeyEncoding(std::string_view bytes) {
  return StartsWith(bytes, kUnsafeLegacyMagic);
}

DiagnosticRecord MakeIndexKeyEncodingDiagnostic(Status status,
                                                std::string diagnostic_code,
                                                std::string message_key,
                                                std::string detail) {
  std::vector<DiagnosticArgument> arguments;
  if (!detail.empty()) {
    arguments.push_back({"detail", std::move(detail)});
  }
  return MakeDiagnostic(status.code,
                        status.severity,
                        status.subsystem,
                        std::move(diagnostic_code),
                        std::move(message_key),
                        std::move(arguments),
                        {},
                        "core.index.key_encoding");
}

}  // namespace scratchbird::core::index
