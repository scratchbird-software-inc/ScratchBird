// Copyright (c) 2026 ScratchBird Software Inc.
//
// This Source Code Form is subject to the terms of the Mozilla Public
// License, v. 2.0. If a copy of the MPL was not distributed with this
// file, You can obtain one at https://mozilla.org/MPL/2.0/.
//
// SPDX-License-Identifier: MPL-2.0

#pragma once

#include "engine/sblr/contextual_text_literal_v2_codec.hpp"

#include <cstdint>
#include <span>
#include <string>
#include <string_view>
#include <vector>

namespace scratchbird::engine::internal_api {

// SEARCH_KEY: MGA_SEALED_CONTEXTUAL_TEXT_SIDECAR_SET_V2
// Pure construction and validation for the manifest-owned ordered descriptor
// field vector. This module does not publish, select, replay, or repair MGA
// state. SBTLTD02 encoding remains owned by contextual_text_literal_v2_codec.

inline constexpr std::string_view kMgaContextualTextSidecarSetAuthorityV2 =
    "MGA-SEALED-CONTEXTUAL-TEXT-SIDECAR-SET-V2";
inline constexpr std::string_view kMgaContextualTextSidecarBlobSuffixV2 =
    ".contextual_text_descriptor_sidecar_v2";
inline constexpr std::string_view kMgaContextualTextSidecarHashSuffixV2 =
    ".contextual_text_descriptor_sidecar_v2.sha256";
inline constexpr std::string_view kMgaContextualTextSidecarSetSealKeyV2 =
    "contextual_text_descriptor_sidecar_set_v2.seal_sha256";
inline constexpr std::string_view kMgaContextualTextSidecarSetSealDomainV2 =
    "ScratchBird.ContextualText.SealedRelationDescriptorSidecarSet.V2";

using MgaContextualTextUuidV2 = sblr::ContextualTextUuidV2;
using MgaContextualTextSha256V2 = sblr::ContextualTextSha256V2;
using MgaContextualTextRawBytesV2 = std::vector<std::uint8_t>;

struct MgaContextualTextDescriptorFieldPairV2 {
  MgaContextualTextRawBytesV2 key_raw_bytes;
  MgaContextualTextRawBytesV2 value_raw_bytes;

  bool operator==(const MgaContextualTextDescriptorFieldPairV2&) const =
      default;
};

struct MgaContextualTextSidecarSetOwnerV2 {
  std::uint64_t creator_transaction_id = 0;
  std::uint64_t event_sequence = 0;
  MgaContextualTextUuidV2 relation_uuid{};
  MgaContextualTextUuidV2 relation_descriptor_uuid{};
  std::uint64_t relation_descriptor_generation = 0;

  bool operator==(const MgaContextualTextSidecarSetOwnerV2&) const = default;
};

// The projection fields are independent inputs from the authenticated visible
// relation projection. expected_text_descriptor carries the complete expected
// live TEXT authority and is encoded/compared through Carver's SBTLTD02 codec;
// it is ignored when comparable_persisted_text is false.
struct MgaContextualTextProjectedColumnV2 {
  std::uint32_t column_ordinal = 0;
  MgaContextualTextUuidV2 column_uuid{};
  bool comparable_persisted_text = false;
  MgaContextualTextUuidV2 projected_datatype_descriptor_uuid{};
  std::uint64_t projected_datatype_descriptor_generation = 0;
  MgaContextualTextUuidV2 projected_datatype_catalog_snapshot_uuid{};
  std::uint64_t projected_datatype_catalog_generation = 0;
  std::uint64_t projected_datatype_registry_generation = 0;
  std::uint64_t projected_resource_epoch = 0;
  sblr::ContextualTextDescriptorV2 expected_text_descriptor;
};

struct MgaContextualTextSidecarSetV2 {
  MgaContextualTextSidecarSetOwnerV2 owner;
  std::uint64_t descriptor_field_count = 0;
  std::uint64_t descriptor_field_bytes = 0;
  std::uint32_t contextual_sidecar_count = 0;
  std::vector<MgaContextualTextDescriptorFieldPairV2> descriptor_fields;

  // Logical alias of the final pair's exact raw value. It is not a second
  // persisted seal field.
  MgaContextualTextSha256V2 seal_sha256{};
};

struct MgaContextualTextSidecarLookupResultV2 {
  sblr::ContextualTextDescriptorV2 descriptor;
  MgaContextualTextRawBytesV2 exact_blob;
  MgaContextualTextSha256V2 descriptor_evidence_sha256{};
};

struct MgaContextualTextSidecarSetDiagnosticV2 {
  std::string code;
  std::string detail;
};

bool SerializeMgaContextualTextDescriptorFieldVectorV2(
    std::span<const MgaContextualTextDescriptorFieldPairV2> fields,
    MgaContextualTextRawBytesV2* canonical_serialization,
    std::uint64_t* canonical_serialized_bytes,
    MgaContextualTextSidecarSetDiagnosticV2* diagnostic);

bool BuildMgaContextualTextSidecarSetV2(
    const MgaContextualTextSidecarSetOwnerV2& owner,
    std::span<const MgaContextualTextDescriptorFieldPairV2>
        base_descriptor_fields,
    std::span<const MgaContextualTextProjectedColumnV2> projected_columns,
    MgaContextualTextSidecarSetV2* out,
    MgaContextualTextSidecarSetDiagnosticV2* diagnostic);

bool ValidateMgaContextualTextSidecarSetV2(
    const MgaContextualTextSidecarSetOwnerV2& expected_owner,
    std::span<const MgaContextualTextDescriptorFieldPairV2>
        expected_base_descriptor_fields,
    std::span<const MgaContextualTextProjectedColumnV2>
        expected_projected_columns,
    const MgaContextualTextSidecarSetV2& candidate,
    MgaContextualTextSidecarSetDiagnosticV2* diagnostic);

// This operation always validates and scans the complete vector before
// returning a match; it never treats the first matching key as authority.
bool LookupMgaContextualTextSidecarV2(
    const MgaContextualTextSidecarSetOwnerV2& expected_owner,
    std::span<const MgaContextualTextDescriptorFieldPairV2>
        expected_base_descriptor_fields,
    std::span<const MgaContextualTextProjectedColumnV2>
        expected_projected_columns,
    const MgaContextualTextSidecarSetV2& candidate,
    std::uint32_t column_ordinal,
    const MgaContextualTextUuidV2& column_uuid,
    MgaContextualTextSidecarLookupResultV2* out,
    MgaContextualTextSidecarSetDiagnosticV2* diagnostic);

}  // namespace scratchbird::engine::internal_api
