// Copyright (c) 2026 ScratchBird Software Inc.
// SPDX-License-Identifier: MPL-2.0
#pragma once
#include "sblr_bound_object_identity.hpp"
#include "core/datatypes/datatype_catalog_manifest.hpp"

namespace scratchbird::engine::sblr {
inline bool IsProjectionUuidLiteral(const SblrOperand& operand) noexcept {
  return operand.type == "uuid" && operand.name.starts_with("projection_") &&
      operand.name.ends_with("_value");
}

// Structural scalar-data admission only. The Core descriptor is resolved from
// its catalog; the literal's 16 bytes are data, never an object reference.
inline bool ProjectSblrUuidLiterals(const SblrOperationEnvelope& envelope,
    internal_api::EngineApiRequest* request,
    SblrIdentityProjectionFailure* failure = nullptr) noexcept try {
  if (failure) *failure = SblrIdentityProjectionFailure::invalid_operand;
  if (!request) return false;
  const auto& bound = request->projection.uuid_literals;
  for (std::size_t i = 0; i < bound.size(); ++i) {
    if (!IsProjectionFunctionIdentityPath(bound[i].first)) return false;
    for (std::size_t j = 0; j < i; ++j)
      if (bound[i].first == bound[j].first) return false;
  }
  std::vector<std::pair<std::string, core::platform::Uuid>> values;
  for (std::size_t i = 0; i < envelope.operands.size(); ++i) {
    const auto& operand = envelope.operands[i];
    if (!IsProjectionUuidLiteral(operand)) continue;
    const auto path = operand.name.substr(0, operand.name.size() - 5);
    if (!IsProjectionFunctionIdentityPath(path) || operand.ordinal != i + 1 ||
        !operand.value.empty() || operand.value_kind != SblrValueKind::literal_typed ||
        operand.value_flags != 0 || operand.value_body.size() != 40) return false;
    namespace dt = core::datatypes;
    const auto catalog = dt::LoadCurrentCoreDatatypeCatalogManifest();
    if (!catalog.ok()) return false;
    const auto row = dt::LookupDatatypeCatalogRow(catalog.manifest, dt::CanonicalTypeId::uuid);
    if (!row.ok() || row.manifest.descriptor_rows.size() != 1) return false;
    const auto& descriptor = row.manifest.descriptor_rows.front().descriptor_uuid.value;
    if (!std::equal(descriptor.bytes.begin(), descriptor.bytes.end(), operand.value_body.begin()) ||
        operand.value_body[16] != 16 ||
        std::any_of(operand.value_body.begin() + 17, operand.value_body.begin() + 24,
                    [](auto byte) { return byte != 0; })) return false;
    if (std::any_of(values.begin(), values.end(), [&](const auto& item) {
          return item.first == path;
        })) return false;
    core::platform::Uuid value;
    std::copy_n(operand.value_body.begin() + 24, 16, value.bytes.begin());
    values.emplace_back(path, value);
  }
  if (!values.empty()) {
    if (!bound.empty()) {
      if (bound.size() != values.size()) return false;
      for (const auto& value : values)
        if (std::find(bound.begin(), bound.end(), value) == bound.end()) return false;
    }
    request->projection.uuid_literals.swap(values);
  }
  if (failure) *failure = SblrIdentityProjectionFailure::none;
  return true;
} catch (const std::bad_alloc&) {
  if (failure) *failure = SblrIdentityProjectionFailure::allocation_failed;
  return false;
}
} // namespace scratchbird::engine::sblr
