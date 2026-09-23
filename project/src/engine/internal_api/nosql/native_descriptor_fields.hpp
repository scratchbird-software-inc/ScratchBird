// Copyright (c) 2026 ScratchBird Software Inc.
// SPDX-License-Identifier: MPL-2.0
#pragma once
#include "catalog/column_metadata_codec.hpp"
#include <variant>
namespace scratchbird::engine::internal_api {
using NativeDescriptorField = std::variant<std::string_view, EngineUuid>;
inline bool ExactNativeDescriptorFields(
    const EngineDescriptor& descriptor,
    const std::initializer_list<std::pair<std::string_view, NativeDescriptorField>>& expected) {
  CatalogColumnMetadata fields;
  if (descriptor.descriptor_kind == "canonical_type_descriptor") {
    if (!DecodeCatalogColumnMetadata(descriptor.encoded_descriptor, &fields)) return false;
    const auto type = fields.identities.find("type_uuid");
    if (type == fields.identities.end() || type->second != descriptor.type_uuid) return false;
  } else if (descriptor.descriptor_kind == "scalar") {
    if (!core::uuid::IsEngineIdentityUuid(descriptor.type_uuid)) return false;
    fields.identities.emplace("type_uuid", descriptor.type_uuid);
    std::string_view encoded(descriptor.encoded_descriptor);
    while (!encoded.empty()) {
      const auto end = encoded.find(';');
      const auto field = encoded.substr(0, end);
      const auto equal = field.find('=');
      if (equal == std::string_view::npos || equal == 0 || equal + 1 == field.size()) return false;
      const auto key = field.substr(0, equal);
      if (key.ends_with("uuid") || !fields.text.emplace(key, field.substr(equal + 1)).second) return false;
      if (end == std::string_view::npos) break;
      encoded.remove_prefix(end + 1);
      if (encoded.empty()) return false;
    }
  } else return false;
  if (fields.text.size() + fields.identities.size() != expected.size()) return false;
  for (const auto& [key, value] : expected) {
    if (const auto* uuid = std::get_if<EngineUuid>(&value)) {
      const auto found = fields.identities.find(std::string(key));
      if (found == fields.identities.end() || found->second != *uuid) return false;
    } else {
      const auto found = fields.text.find(std::string(key));
      if (found == fields.text.end() || found->second != std::get<std::string_view>(value)) return false;
    }
  }
  return true;
}
} // namespace scratchbird::engine::internal_api
