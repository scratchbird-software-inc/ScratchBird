// Copyright (c) 2026 ScratchBird Software Inc.
// SPDX-License-Identifier: MPL-2.0
#include "query/expression_api.hpp"
#include "engine/sblr/relational_descriptor_codec.hpp"
#include "engine/sblr/contextual_text_literal_v2_codec.hpp"
#include "datatype_operations.hpp"
#include "core/uuid/uuid.hpp"
#include <utility>

namespace scratchbird::engine::internal_api {

bool QowDecodeCanonicalTextDescriptorV1(
    const EngineDescriptor& descriptor, RelationalTypeDescriptor* output) {
  namespace dt = scratchbird::core::datatypes;
  if (!output || descriptor.descriptor_kind != "scalar" ||
      dt::CanonicalTypeIdFromStableName(descriptor.canonical_type_name) !=
          dt::CanonicalTypeId::character) return false;
  RelationalTypeDescriptor staged;
  const auto& bytes = descriptor.encoded_descriptor;
  if (!sblr::DecodeRelationalTypeDescriptorV1(
          reinterpret_cast<const std::uint8_t*>(bytes.data()), bytes.size(), &staged) ||
      !staged.datatype_identity_authoritative ||
      staged.descriptor_uuid != descriptor.descriptor_uuid ||
      staged.type_uuid != descriptor.type_uuid ||
      staged.collation_uuid != descriptor.collation_uuid ||
      staged.codec_id != sblr::kContextualTextCodecIdentifierV2 ||
      !staged.collation_uuid.has_value() || staged.timezone_profile_id.has_value() ||
      staged.precision.has_value() || staged.scale.has_value()) return false;
  *output = std::move(staged);
  return true;
}

// QOW-SOURCE-QRY-008-COLLATION-V1
// The caller owns live receipt/catalog/resource authority. This shared path
// validates the binary value descriptors and executes canonical comparison.
bool QowCompareCanonicalCollatedScalarsV1(
    const EngineTypedValue& left_value, const EngineTypedValue& right_value,
    const EngineUuid& collation_uuid, const EngineApiU64 resource_epoch,
    const EngineApiU64 collation_epoch,
    const scratchbird::core::datatypes::DatatypeTextSeedAuthority& text_seed,
    int* comparison, std::string* refusal_detail) {
  namespace dt = scratchbird::core::datatypes;
  if (!comparison || !refusal_detail) return false;
  refusal_detail->clear();
  RelationalTypeDescriptor left, right;
  if (!core::uuid::IsEngineIdentityUuid(collation_uuid) ||
      !QowDecodeCanonicalTextDescriptorV1(left_value.descriptor, &left) ||
      !QowDecodeCanonicalTextDescriptorV1(right_value.descriptor, &right) ||
      left.collation_uuid != collation_uuid || right.collation_uuid != collation_uuid) {
    *refusal_detail = "canonical character descriptors do not share the bound binary collation UUID";
    return false;
  }
  if (resource_epoch == 0 || collation_epoch == 0 || !text_seed.active ||
      text_seed.seed_pack_name.empty() || text_seed.seed_pack_version.empty() ||
      text_seed.charset_name.empty() || text_seed.collation_name.empty()) {
    *refusal_detail = "bound collation resource authority is incomplete";
    return false;
  }
  if (left_value.isSqlNull() || right_value.isSqlNull()) {
    *refusal_detail = "SQL NULL comparison requires the shared three-valued predicate seam";
    return false;
  }
  if (left_value.state != EngineValueState::value ||
      right_value.state != EngineValueState::value || left_value.is_null || right_value.is_null ||
      !left_value.binary_value.empty() || !right_value.binary_value.empty()) {
    *refusal_detail = "collation comparison requires two canonical non-NULL character values";
    return false;
  }
  dt::DatatypeComparisonRequest request;
  request.left.type_id = dt::CanonicalTypeId::character;
  request.left.encoded_value = left_value.encoded_value;
  request.right.type_id = dt::CanonicalTypeId::character;
  request.right.encoded_value = right_value.encoded_value;
  request.case_insensitive_character_compare = text_seed.collation_case_insensitive;
  request.text_seed = text_seed;
  const auto compared = dt::CompareDatatypeValues(request);
  if (!compared.ok()) {
    *refusal_detail = compared.diagnostic.diagnostic_code.empty()
        ? "canonical collation comparison refused" : compared.diagnostic.diagnostic_code;
    return false;
  }
  *comparison = compared.comparison;
  return true;
}

}  // namespace scratchbird::engine::internal_api
