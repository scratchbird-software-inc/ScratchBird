// Copyright (c) 2026 ScratchBird Software Inc.
// SPDX-License-Identifier: MPL-2.0
#pragma once

#include "core/uuid/uuid.hpp"
#include <cstdint>
#include <optional>
#include <string>

namespace scratchbird::engine::internal_api {

enum class RelationalNullability : std::uint8_t {
  kNonNull = 1,
  kNullable,
  kUnknown,
};

struct RelationalTypeDescriptor {
  std::uint32_t descriptor_id{0};
  core::platform::Uuid descriptor_uuid;
  core::platform::Uuid type_uuid;
  RelationalNullability nullability{RelationalNullability::kUnknown};
  std::optional<core::platform::Uuid> collation_uuid;
  std::optional<std::string> timezone_profile_id;
  std::optional<std::uint32_t> width;
  std::optional<std::uint32_t> precision;
  std::optional<std::uint32_t> scale;
  bool datatype_identity_authoritative{false};
  std::uint64_t descriptor_generation{0};
  std::uint64_t type_generation{0};
  std::string codec_id;
  std::uint16_t codec_version{0};
  std::uint64_t codec_generation{0};
  core::platform::Uuid statement_receipt_uuid;
  core::platform::Uuid datatype_catalog_snapshot_uuid;
  std::uint64_t datatype_catalog_generation{0};
  std::uint64_t datatype_registry_generation{0};
};

}  // namespace scratchbird::engine::internal_api
