// Copyright (c) 2026 ScratchBird Software Inc.
// SPDX-License-Identifier: MPL-2.0
#include "catalog_language.hpp"

namespace scratchbird::core::catalog {
bool IsKnownCatalogLanguage(const Uuid& uuid) noexcept {
  for (const auto& language : kCatalogLanguages)
    if (language.uuid == uuid) return true;
  return false;
}
std::optional<Uuid> CatalogLanguageForTag(std::string_view tag) noexcept {
  const auto lower = [](char c) { return c >= 'A' && c <= 'Z' ? static_cast<char>(c - 'A' + 'a') : c; };
  for (const auto& language : kCatalogLanguages) {
    if (language.tag.size() != tag.size()) continue;
    bool same = true;
    for (std::size_t i = 0; i < tag.size() && same; ++i) same = lower(tag[i]) == lower(language.tag[i]);
    if (same) return language.uuid;
  }
  return std::nullopt;
}
}  // namespace scratchbird::core::catalog
