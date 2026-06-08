// Copyright (c) 2026 ScratchBird Software Inc.
//
// This Source Code Form is subject to the terms of the Mozilla Public
// License, v. 2.0. If a copy of the MPL was not distributed with this
// file, You can obtain one at https://mozilla.org/MPL/2.0/.
//
// SPDX-License-Identifier: MPL-2.0

#include "catalog/wire_driver_metadata_api.hpp"

#include "datatype_operations.hpp"
#include "datatype_wire_metadata.hpp"

#include <cstdint>
#include <sstream>
#include <utility>

namespace scratchbird::engine::internal_api {
namespace {

namespace dt = scratchbird::core::datatypes;

bool StartsWith(const std::string& value, const std::string& prefix) {
  return value.rfind(prefix, 0) == 0;
}

std::string DescriptorField(const std::string& descriptor, const std::string& key) {
  std::istringstream in(descriptor);
  std::string part;
  const std::string prefix = key + "=";
  while (std::getline(in, part, ';')) {
    if (StartsWith(part, prefix)) { return part.substr(prefix.size()); }
  }
  return {};
}

int HexValue(char c) {
  if (c >= '0' && c <= '9') { return c - '0'; }
  if (c >= 'a' && c <= 'f') { return 10 + c - 'a'; }
  if (c >= 'A' && c <= 'F') { return 10 + c - 'A'; }
  return -1;
}

std::string HexDecodeOrOriginal(const std::string& value) {
  if ((value.size() % 2) != 0) { return value; }
  std::string out;
  out.reserve(value.size() / 2);
  for (std::size_t i = 0; i < value.size(); i += 2) {
    const int hi = HexValue(value[i]);
    const int lo = HexValue(value[i + 1]);
    if (hi < 0 || lo < 0) { return value; }
    out.push_back(static_cast<char>((hi << 4) | lo));
  }
  return out;
}

bool IsOpaqueDescriptor(const EngineDescriptor& descriptor) {
  return descriptor.canonical_type_name == "opaque_extension" ||
         DescriptorField(descriptor.encoded_descriptor, "canonical") == "opaque_extension" ||
         DescriptorField(descriptor.encoded_descriptor, "base_type") == "opaque_extension";
}

std::string DisplayTypeFor(const EngineDescriptor& descriptor,
                           const std::string& base_type,
                           const std::string& donor_label) {
  const std::string display = DescriptorField(descriptor.encoded_descriptor, "driver_display_type");
  if (!display.empty()) { return display; }
  if (!donor_label.empty()) { return donor_label; }
  if (descriptor.descriptor_kind == "domain" && !descriptor.canonical_type_name.empty()) {
    return descriptor.canonical_type_name;
  }
  if (!base_type.empty()) { return base_type; }
  return descriptor.canonical_type_name;
}

dt::CanonicalTypeId DescriptorTypeId(const EngineDescriptor& descriptor,
                                     const std::string& base_type) {
  if (!base_type.empty()) {
    const auto base_type_id = dt::CanonicalTypeIdFromStableName(base_type);
    if (base_type_id != dt::CanonicalTypeId::unknown) { return base_type_id; }
  }
  const auto type_id = dt::CanonicalTypeIdFromStableName(descriptor.canonical_type_name);
  if (type_id != dt::CanonicalTypeId::unknown) { return type_id; }
  const std::string canonical = DescriptorField(descriptor.encoded_descriptor, "canonical");
  if (!canonical.empty()) {
    return dt::CanonicalTypeIdFromStableName(canonical);
  }
  return dt::CanonicalTypeId::unknown;
}

std::int64_t ParseI64Or(const std::string& value, std::int64_t fallback) {
  if (value.empty()) { return fallback; }
  try {
    std::size_t consumed = 0;
    const auto parsed = std::stoll(value, &consumed, 10);
    return consumed == value.size() ? parsed : fallback;
  } catch (...) {
    return fallback;
  }
}

std::int64_t DefaultPrecision(const dt::DatatypeDescriptor& descriptor) {
  if (descriptor.default_precision != 0) {
    return descriptor.default_precision;
  }
  if (descriptor.family == dt::TypeFamily::signed_integer ||
      descriptor.family == dt::TypeFamily::unsigned_integer ||
      descriptor.family == dt::TypeFamily::real) {
    return descriptor.bit_width;
  }
  return 0;
}

std::uint32_t NumericRadix(const dt::DatatypeDescriptor& descriptor) {
  if (descriptor.family == dt::TypeFamily::signed_integer ||
      descriptor.family == dt::TypeFamily::unsigned_integer ||
      descriptor.family == dt::TypeFamily::real) {
    return 2;
  }
  if (descriptor.family == dt::TypeFamily::decimal) { return 10; }
  return 0;
}

std::string Signedness(const dt::DatatypeDescriptor& descriptor) {
  if (descriptor.family == dt::TypeFamily::signed_integer ||
      descriptor.family == dt::TypeFamily::real ||
      descriptor.family == dt::TypeFamily::decimal) {
    return "YES";
  }
  if (descriptor.family == dt::TypeFamily::unsigned_integer) { return "NO"; }
  return "UNKNOWN";
}

std::int64_t DisplaySizeFor(const dt::DatatypeDescriptor& descriptor, std::int64_t precision) {
  if (descriptor.family == dt::TypeFamily::signed_integer) {
    return precision == 128 ? 40 : precision;
  }
  if (descriptor.family == dt::TypeFamily::unsigned_integer) {
    return precision == 128 ? 39 : precision;
  }
  if (descriptor.type_id == dt::CanonicalTypeId::real128) { return 48; }
  if (descriptor.family == dt::TypeFamily::decimal) { return precision + 2; }
  if (descriptor.type_id == dt::CanonicalTypeId::uuid) { return 36; }
  return 0;
}

}  // namespace

EngineWireDriverMetadata RenderWireDriverMetadata(const EngineDescriptor& descriptor,
                                                  std::string donor_dialect,
                                                  std::string donor_label) {
  EngineWireDriverMetadata metadata;
  metadata.descriptor_kind = descriptor.descriptor_kind.empty() ? "scalar" : descriptor.descriptor_kind;
  metadata.canonical_type_name = descriptor.canonical_type_name;
  metadata.domain_uuid = DescriptorField(descriptor.encoded_descriptor, "domain_uuid");
  metadata.base_canonical_type_name = DescriptorField(descriptor.encoded_descriptor, "base_type");
  metadata.donor_dialect = std::move(donor_dialect);
  metadata.donor_label = std::move(donor_label);
  metadata.driver_metadata_envelope = HexDecodeOrOriginal(DescriptorField(descriptor.encoded_descriptor, "driver_metadata"));
  metadata.wire_metadata_envelope = HexDecodeOrOriginal(DescriptorField(descriptor.encoded_descriptor, "wire_metadata"));
  metadata.domain_descriptor = metadata.descriptor_kind == "domain" || !metadata.domain_uuid.empty();
  metadata.native_descriptor = !metadata.domain_descriptor && metadata.donor_label.empty();
  metadata.donor_label_alias_only = true;
  metadata.opaque_render_only = IsOpaqueDescriptor(descriptor);
  metadata.comparison_supported = !metadata.opaque_render_only;
  metadata.mutation_supported = !metadata.opaque_render_only;
  if (metadata.domain_descriptor && metadata.base_canonical_type_name.empty()) {
    metadata.base_canonical_type_name = descriptor.canonical_type_name;
  }
  metadata.driver_display_type = DisplayTypeFor(descriptor, metadata.base_canonical_type_name, metadata.donor_label);
  metadata.metadata_projection_source = "sys.information.scratchbird_datatype_descriptors";

  const auto type_id = DescriptorTypeId(descriptor, metadata.base_canonical_type_name);
  const auto wire_type_id = dt::WireTypeIdForCanonicalTypeId(type_id);
  metadata.canonical_type_family_id = wire_type_id.type_family;
  metadata.canonical_type_code_id = wire_type_id.type_code;
  metadata.canonical_type_version = wire_type_id.type_version;
  metadata.canonical_type_flags = wire_type_id.type_flags;
  metadata.canonical_type_family = dt::CanonicalWireTypeFamilyName(
      static_cast<dt::CanonicalWireTypeFamily>(wire_type_id.type_family));
  metadata.canonical_type_code = dt::CanonicalWireTypeCodeName(wire_type_id);

  const auto descriptor_lookup = dt::LookupDatatypeDescriptor(type_id);
  if (descriptor_lookup.ok()) {
    const auto& datatype = descriptor_lookup.descriptor;
    metadata.precision =
        ParseI64Or(DescriptorField(descriptor.encoded_descriptor, "precision"),
                   DefaultPrecision(datatype));
    metadata.scale =
        ParseI64Or(DescriptorField(descriptor.encoded_descriptor, "scale"),
                   datatype.default_scale);
    metadata.display_size =
        ParseI64Or(DescriptorField(descriptor.encoded_descriptor, "display_size"),
                   DisplaySizeFor(datatype, metadata.precision));
    metadata.numeric_precision_radix = NumericRadix(datatype);
    metadata.signedness = Signedness(datatype);
    metadata.nullability = DescriptorField(descriptor.encoded_descriptor, "nullable") == "false"
                               ? "NO"
                               : "UNKNOWN";
    metadata.backend_profile_required = datatype.requires_mandatory_library;
    if (datatype.requires_mandatory_library) {
      metadata.backend_profile = "sbl_numeric:" + datatype.required_capability_key;
    }
    if (metadata.opaque_render_only) {
      metadata.compatibility_class = "render_only";
      metadata.support_state = "unsupported";
      metadata.unsupported_reason = "opaque_render_only";
    }
  } else {
    metadata.compatibility_class = "unsupported_by_policy";
    metadata.support_state = "unsupported";
    metadata.unsupported_reason = "unknown_canonical_type";
  }
  return metadata;
}

}  // namespace scratchbird::engine::internal_api
