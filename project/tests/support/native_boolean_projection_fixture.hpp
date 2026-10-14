// Copyright (c) 2026 ScratchBird Software Inc.
// SPDX-License-Identifier: MPL-2.0
#pragma once
#include "engine/sblr/sblr_engine_envelope.hpp"
#include "core/datatypes/datatype_catalog_manifest.hpp"
#include <stdexcept>
#include <string>
#include <utility>

namespace scratchbird::tests {
// Structural component-test payload only: no descriptor occurrence, statement,
// catalog pin, authorization, allocation or live runtime receipt is issued.
inline void AppendNativeBooleanProjectionFixture(
    engine::sblr::SblrOperationEnvelope& envelope, std::string name, bool value) {
  namespace dt = core::datatypes;
  const auto catalog = dt::LoadCurrentCoreDatatypeCatalogManifest();
  if (!catalog.ok()) throw std::runtime_error("BOOLEAN fixture catalog unavailable");
  const auto row = dt::LookupDatatypeCatalogRow(catalog.manifest, dt::CanonicalTypeId::boolean);
  if (!row.ok() || row.manifest.descriptor_rows.size() != 1)
    throw std::runtime_error("BOOLEAN fixture datatype unavailable");
  engine::sblr::SblrOperand operand;
  operand.ordinal = static_cast<std::uint32_t>(envelope.operands.size() + 1);
  operand.type = "boolean"; operand.name = std::move(name);
  operand.value_kind = engine::sblr::SblrValueKind::literal_typed;
  const auto& id = row.manifest.descriptor_rows.front().descriptor_uuid.value;
  operand.value_body.assign(id.bytes.begin(), id.bytes.end());
  operand.value_body.push_back(1); operand.value_body.insert(operand.value_body.end(), 7, 0);
  operand.value_body.push_back(value ? 1 : 0);
  envelope.operands.push_back(std::move(operand));
}
}
