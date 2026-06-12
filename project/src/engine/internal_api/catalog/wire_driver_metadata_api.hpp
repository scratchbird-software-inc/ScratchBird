// Copyright (c) 2026 ScratchBird Software Inc.
//
// This Source Code Form is subject to the terms of the Mozilla Public
// License, v. 2.0. If a copy of the MPL was not distributed with this
// file, You can obtain one at https://mozilla.org/MPL/2.0/.
//
// SPDX-License-Identifier: MPL-2.0

#pragma once

#include "api_types.hpp"

#include <cstdint>
#include <string>

namespace scratchbird::engine::internal_api {

// SEARCH_KEY: SB_ENGINE_WIRE_DRIVER_METADATA_API
// Engine-owned metadata rendering for parser/driver packages. Names and reference
// labels remain aliases; canonical descriptors and UUID-backed domain
// descriptors remain the authority.

struct EngineWireDriverMetadata {
  std::string descriptor_kind;
  std::string canonical_type_name;
  std::string domain_uuid;
  std::string base_canonical_type_name;
  std::string compatibility_dialect;
  std::string reference_label;
  std::string driver_display_type;
  std::string canonical_type_family;
  std::string canonical_type_code;
  std::uint16_t canonical_type_family_id = 0;
  std::uint16_t canonical_type_code_id = 0;
  std::uint16_t canonical_type_version = 0;
  std::uint16_t canonical_type_flags = 0;
  std::int64_t precision = 0;
  std::int64_t scale = 0;
  std::int64_t display_size = 0;
  std::uint32_t numeric_precision_radix = 0;
  std::string signedness = "UNKNOWN";
  std::string nullability = "UNKNOWN";
  std::string compatibility_class = "native_or_better";
  std::string support_state = "supported";
  std::string backend_profile;
  std::string unsupported_reason;
  std::string metadata_projection_source;
  std::string driver_metadata_envelope;
  std::string wire_metadata_envelope;
  bool native_descriptor = false;
  bool domain_descriptor = false;
  bool reference_label_alias_only = true;
  bool opaque_render_only = false;
  bool comparison_supported = true;
  bool mutation_supported = true;
  bool backend_profile_required = false;
};

EngineWireDriverMetadata RenderWireDriverMetadata(const EngineDescriptor& descriptor,
                                                  std::string compatibility_dialect = {},
                                                  std::string reference_label = {});

}  // namespace scratchbird::engine::internal_api
