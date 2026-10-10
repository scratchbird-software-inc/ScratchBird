// Copyright (c) 2026 ScratchBird Software Inc.
// SPDX-License-Identifier: MPL-2.0
#pragma once
#include "sblr_projection_uuid_literals.hpp"

namespace scratchbird::engine::sblr {
inline bool IsProjectionReal64Literal(const SblrOperand& operand) noexcept {
  return operand.type == "real64" && operand.name.starts_with("projection_") &&
      operand.name.ends_with("_value");
}

inline bool ProjectSblrReal64Literals(const SblrOperationEnvelope& envelope,
    internal_api::EngineApiRequest* request,
    SblrIdentityProjectionFailure* failure = nullptr) noexcept try {
  if (failure) *failure = SblrIdentityProjectionFailure::invalid_operand;
  if (!request) return false;
  const auto& bound = request->projection.real64_literals;
  for (std::size_t i = 0; i < bound.size(); ++i) {
    if (!IsProjectionFunctionIdentityPath(bound[i].first)) return false;
    for (std::size_t j = 0; j < i; ++j)
      if (bound[i].first == bound[j].first) return false;
  }
  decltype(internal_api::EngineProjectionEnvelope::real64_literals) values;
  std::array<std::uint8_t, 16> descriptor_bytes{};
  bool descriptor_loaded = false;
  for (std::size_t i = 0; i < envelope.operands.size(); ++i) {
    const auto& operand = envelope.operands[i];
    if (!IsProjectionReal64Literal(operand)) continue;
    const auto path = operand.name.substr(0, operand.name.size() - 5);
    if (!IsProjectionFunctionIdentityPath(path) || operand.ordinal != i + 1 ||
        !operand.value.empty() || operand.value_kind != SblrValueKind::literal_typed ||
        operand.value_flags != 0 || operand.value_body.size() != 32) return false;
    namespace dt = core::datatypes;
    // Resolve once per reconciliation, not once per literal or in a stale
    // process-global cache. This remains component descriptor verification.
    if (!descriptor_loaded) {
      const auto catalog = dt::LoadCurrentCoreDatatypeCatalogManifest();
      if (!catalog.ok()) return false;
      const auto row = dt::LookupDatatypeCatalogRow(catalog.manifest, dt::CanonicalTypeId::real64);
      if (!row.ok() || row.manifest.descriptor_rows.size() != 1) return false;
      descriptor_bytes = row.manifest.descriptor_rows.front().descriptor_uuid.value.bytes;
      descriptor_loaded = true;
    }
    if (!std::equal(descriptor_bytes.begin(), descriptor_bytes.end(), operand.value_body.begin())) return false;
    if (operand.value_body[16] != 8 ||
        std::any_of(operand.value_body.begin() + 17, operand.value_body.begin() + 24,
                    [](auto byte) { return byte != 0; })) return false;
    if (std::any_of(values.begin(), values.end(), [&](const auto& item) { return item.first == path; }))
      return false;
    std::array<std::uint8_t, 8> bytes;
    std::copy_n(operand.value_body.begin() + 24, 8, bytes.begin());
    values.emplace_back(path, bytes);
  }
  if (!values.empty()) {
    if (!bound.empty()) {
      if (bound.size() != values.size()) return false;
      for (const auto& value : values)
        if (std::find(bound.begin(), bound.end(), value) == bound.end()) return false;
    }
    request->projection.real64_literals.swap(values);
  }
  if (failure) *failure = SblrIdentityProjectionFailure::none;
  return true;
} catch (const std::bad_alloc&) {
  if (failure) *failure = SblrIdentityProjectionFailure::allocation_failed;
  return false;
}

inline bool IsProjectionBinaryLiteral(const SblrOperand& operand) noexcept {
  return operand.type == "binary" && operand.name.starts_with("projection_") &&
      operand.name.ends_with("_value");
}

// Character/SQL literal decoding belongs to the parser. This boundary accepts
// only a canonical datatype descriptor, exact byte length and native payload.
inline bool ProjectSblrBinaryLiterals(const SblrOperationEnvelope& envelope,
    internal_api::EngineApiRequest* request,
    SblrIdentityProjectionFailure* failure = nullptr) noexcept try {
  if (failure) *failure = SblrIdentityProjectionFailure::invalid_operand;
  if (!request) return false;
  const auto& bound = request->projection.binary_literals;
  for (std::size_t i = 0; i < bound.size(); ++i) {
    if (!IsProjectionFunctionIdentityPath(bound[i].first)) return false;
    for (std::size_t j = 0; j < i; ++j)
      if (bound[i].first == bound[j].first) return false;
  }
  std::vector<std::pair<std::string, std::vector<std::uint8_t>>> values;
  for (std::size_t i = 0; i < envelope.operands.size(); ++i) {
    const auto& operand = envelope.operands[i];
    if (!IsProjectionBinaryLiteral(operand)) continue;
    const auto path = operand.name.substr(0, operand.name.size() - 5);
    if (!IsProjectionFunctionIdentityPath(path) || operand.ordinal != i + 1 ||
        !operand.value.empty() || operand.value_kind != SblrValueKind::literal_typed ||
        operand.value_flags != 0 || operand.value_body.size() < 24) return false;
    namespace dt = core::datatypes;
    const auto catalog = dt::LoadCurrentCoreDatatypeCatalogManifest();
    if (!catalog.ok()) return false;
    const auto row = dt::LookupDatatypeCatalogRow(catalog.manifest, dt::CanonicalTypeId::binary);
    if (!row.ok() || row.manifest.descriptor_rows.size() != 1) return false;
    const auto& descriptor = row.manifest.descriptor_rows.front().descriptor_uuid.value;
    if (!std::equal(descriptor.bytes.begin(), descriptor.bytes.end(), operand.value_body.begin())) return false;
    std::uint64_t size = 0;
    for (unsigned n = 0; n < 8; ++n)
      size |= std::uint64_t{operand.value_body[16 + n]} << (8 * n);
    if (size != operand.value_body.size() - 24) return false;
    if (std::any_of(values.begin(), values.end(), [&](const auto& item) {
          return item.first == path;
        })) return false;
    values.emplace_back(path, std::vector<std::uint8_t>(operand.value_body.begin() + 24,
                                                       operand.value_body.end()));
  }
  if (!values.empty()) {
    if (!bound.empty()) {
      if (bound.size() != values.size()) return false;
      for (const auto& value : values)
        if (std::find(bound.begin(), bound.end(), value) == bound.end()) return false;
    }
    request->projection.binary_literals.swap(values);
  }
  if (failure) *failure = SblrIdentityProjectionFailure::none;
  return true;
} catch (const std::bad_alloc&) {
  if (failure) *failure = SblrIdentityProjectionFailure::allocation_failed;
  return false;
}
}  // namespace scratchbird::engine::sblr
