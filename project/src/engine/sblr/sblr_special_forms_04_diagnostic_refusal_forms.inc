// Copyright (c) 2026 ScratchBird Software Inc.
//
// This Source Code Form is subject to the terms of the Mozilla Public
// License, v. 2.0. If a copy of the MPL was not distributed with this
// file, You can obtain one at https://mozilla.org/MPL/2.0/.
//
// SPDX-License-Identifier: MPL-2.0

// SBSQL source-layout fragment. Included only by sblr_special_forms.cpp.
// This fragment preserves pre-existing behavior; do not add unrelated future work here.
                                     const SblrValue& start,
                                     const SblrValue& length,
                                     const SblrExecutionContext& context) {
  if (value.is_null || start.is_null || length.is_null) {
    return ScalarResult(operation_id, NullValue("text"));
  }
  if (!start.has_int64_value || !length.has_int64_value) {
    return SpecialFormFailure(operation_id, context, "SBLR.DESCRIPTOR_MISMATCH", "substring start and length must be integer values");
  }
  if (start.int64_value < 1 || length.int64_value < 0) {
    return SpecialFormFailure(operation_id, context, "SBLR.SUBSTRING_RANGE", "substring start is one-based and length must not be negative");
  }
  const auto zero_based_start = static_cast<std::size_t>(start.int64_value - 1);
  if (zero_based_start >= value.text_value.size()) {
    SblrValue out;
    out.descriptor_id = "text";
    out.is_null = false;
    out.payload_kind = SblrValuePayloadKind::text;
    return ScalarResult(operation_id, std::move(out));
  }
  SblrValue out;
  out.descriptor_id = value.descriptor_id.empty() ? "text" : value.descriptor_id;
  out.is_null = false;
  out.payload_kind = value.payload_kind == SblrValuePayloadKind::binary ? SblrValuePayloadKind::binary : SblrValuePayloadKind::text;
  out.text_value = value.text_value.substr(zero_based_start, static_cast<std::size_t>(length.int64_value));
  out.encoded_value = out.text_value;
  return ScalarResult(operation_id, std::move(out));
}

SblrResult EvaluateSblrCastForm(std::string_view operation_id,
                                const SblrValue& value,
                                std::string_view target_descriptor_id,
                                const SblrExecutionContext& context,
                                bool explicit_cast,
                                bool donor_compatibility_profile) {
  const auto target_type = CanonicalTypeFromDescriptor(target_descriptor_id);
  if (target_type == datatypes::CanonicalTypeId::unknown) {
    return SpecialFormFailure(operation_id,
                              context,
                              "SBLR.CAST_TARGET_UNKNOWN",
                              "cast target descriptor is not a known canonical datatype descriptor");
  }

  datatypes::DatatypeCastRequest request;
  request.value.type_id = InferSblrSourceType(value);
  request.value.encoded_value = EncodedCastInput(value);
  request.value.is_null = value.is_null;
  request.target_type_id = target_type;
  request.explicit_cast = explicit_cast;
  request.donor_compatibility_profile = donor_compatibility_profile;

  if (!value.is_null && request.value.type_id == datatypes::CanonicalTypeId::unknown) {
    return SpecialFormFailure(operation_id,
                              context,
                              "SBLR.CAST_SOURCE_UNKNOWN",
                              "cast source value does not carry a descriptor or inferable payload kind");
  }

  const auto result = datatypes::CastDatatypeValue(request);
  if (!result.ok()) {
    std::string diagnostic_id = result.diagnostic.diagnostic_code.empty() ? "SBLR.CAST_FAILED" : result.diagnostic.diagnostic_code;
    std::string detail = result.diagnostic.message_key.empty() ? "canonical datatype cast refused the conversion" : result.diagnostic.message_key;
    if (!result.diagnostic.remediation_hint.empty()) {
      detail += ": ";
      detail += result.diagnostic.remediation_hint;
    }
    return SpecialFormFailure(operation_id, context, std::move(diagnostic_id), std::move(detail));
  }

  return ScalarResult(operation_id, SblrValueFromDatatypeValue(result.value));
}

SblrResult EvaluateSblrExtractForm(std::string_view operation_id,
                                   std::string_view field_name,
                                   const SblrValue& value,
                                   const SblrExecutionContext& context) {
  datatypes::DatatypeExtractRequest request;
  request.value.type_id = InferSblrSourceType(value);
  request.value.encoded_value = EncodedCastInput(value);
  request.value.is_null = value.is_null;
  request.field = Lower(field_name);

  if (!value.is_null && request.value.type_id == datatypes::CanonicalTypeId::unknown) {
    return SpecialFormFailure(operation_id,
                              context,
                              "SBLR.EXTRACT_SOURCE_UNKNOWN",
                              "extract source value does not carry a descriptor or inferable payload kind");
  }

  const auto result = datatypes::ExtractDatatypeField(request);
  if (!result.ok()) {
    std::string diagnostic_id = result.diagnostic.diagnostic_code.empty() ? "SBLR.EXTRACT_FAILED" : result.diagnostic.diagnostic_code;
    std::string detail = result.diagnostic.message_key.empty() ? "canonical datatype extract refused the field" : result.diagnostic.message_key;
    if (!result.diagnostic.remediation_hint.empty()) {
      detail += ": ";
      detail += result.diagnostic.remediation_hint;
    }
    return SpecialFormFailure(operation_id, context, std::move(diagnostic_id), std::move(detail));
  }

  SblrValue out = SblrValueFromDatatypeValue(result.value);
  if (out.is_null && out.descriptor_id == "null_type") {
    out.descriptor_id = "int64";
  }
  return ScalarResult(operation_id, std::move(out));
}

SblrResult RefuseSblrSpecialForm(std::string_view operation_id, const SblrExecutionContext& context, std::string detail) {
  auto diagnostic = MakeSblrRefusalDiagnostic("SB_DIAG_SPECIAL_FORM_NOT_ACTIVE", context, std::move(detail));
  diagnostic.fields.push_back({"operation_id", std::string(operation_id)});
  return MakeSblrFailure(SblrStatusCode::unsupported_feature, std::string(operation_id), std::move(diagnostic));
}

}  // namespace scratchbird::engine::sblr
