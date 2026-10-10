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
                                bool reference_compatibility_profile,
                                std::shared_ptr<const internal_api::EngineDescriptor> target_binding) {
  if (!SblrNativeCastCarrierValid(value)) {
    return SpecialFormFailure(operation_id, context, "SBLR.DESCRIPTOR_MISMATCH",
                              "cast source has conflicting or non-native value carriers");
  }
  const auto target_type = CanonicalTypeFromDescriptor(target_descriptor_id);
  if (target_type == datatypes::CanonicalTypeId::unknown) {
    return SpecialFormFailure(operation_id,
                              context,
                              "SBLR.CAST_TARGET_UNKNOWN",
                              "cast target descriptor is not a known canonical datatype descriptor");
  }

  datatypes::DatatypeCastRequest request;
  request.value.type_id = InferSblrSourceType(value);
  request.value.is_null = value.is_null;
  request.target_type_id = target_type;
  request.explicit_cast = explicit_cast;
  request.context = explicit_cast ? datatypes::DatatypeCastContext::explicit_cast
                                  : datatypes::DatatypeCastContext::implicit;
  request.reference_compatibility_profile = reference_compatibility_profile;

  const auto native_numeric_type = [](auto type) {
    return type == datatypes::CanonicalTypeId::int64 ||
           type == datatypes::CanonicalTypeId::real64;
  };
  const bool native_numeric_pair = native_numeric_type(request.value.type_id) &&
                                   native_numeric_type(target_type);
  if (native_numeric_pair) {
    // Fixed-width execution values are not decimal display strings. Retain all
    // 64 bits (including signed zero) across the datatype-owner boundary.
    if (!value.is_null &&
        !(request.value.type_id == datatypes::CanonicalTypeId::int64
              ? SblrInt64PayloadValid(value) : SblrReal64PayloadValid(value))) {
      return SpecialFormFailure(operation_id, context, "NUMERIC.ENCODING.NONCANONICAL",
                                "native numeric cast has conflicting value carriers");
    }
    request.value.encoded_value.clear();
    if (!value.is_null) {
      const auto bits = request.value.type_id == datatypes::CanonicalTypeId::int64
          ? std::bit_cast<std::uint64_t>(value.int64_value)
          : std::bit_cast<std::uint64_t>(value.real64_value);
      request.value.encoded_value.resize(8);
      for (unsigned byte = 0; byte < 8; ++byte)
        request.value.encoded_value[byte] = static_cast<char>(bits >> (byte * 8));
    }
  } else {
    request.value.encoded_value = EncodedCastInput(value);
  }

  if (native_numeric_pair || value.projection_descriptor || target_binding) {
    // This native adapter consumes supplied bindings; target spelling is only
    // checked for agreement and cannot supply missing descriptor authority.
    const auto native_type = [&](auto type) {
      return type == datatypes::CanonicalTypeId::uuid ||
             type == datatypes::CanonicalTypeId::binary ||
             (native_numeric_pair && native_numeric_type(type));
    };
    const auto bind = [&](const auto& binding, auto type, auto* descriptor) {
      if (!binding || !native_type(type) ||
          CanonicalTypeFromDescriptor(binding->canonical_type_name) != type ||
          !binding->charset_uuid.is_nil() || !binding->collation_uuid.is_nil()) return false;
      internal_api::CatalogColumnMetadata fields;
      if (!internal_api::AdmitCatalogColumnMetadata(binding->encoded_descriptor, &fields) ||
          !fields.identities.empty()) return false;
      for (const auto& [key, ignored] : fields.text)
        if (key != "nullability" && key != "nullable") return false;
      std::string detail;
      return executor::BuildBoundExecutionTypeDescriptor(*binding, type, descriptor, &detail);
    };
    if (reference_compatibility_profile ||
        !bind(value.projection_descriptor, request.value.type_id, &request.value.descriptor) ||
        !bind(target_binding, target_type, &request.target_descriptor)) {
      return SpecialFormFailure(operation_id, context, "DATATYPE.DESCRIPTOR.INVALID",
                                "native cast requires exact source and target bindings without modifiers");
    }
  }

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
    auto failure = SpecialFormFailure(
        operation_id, context, std::move(diagnostic_id), std::move(detail));
    failure.numeric_facts = result.numeric_facts;
    if (!failure.diagnostics.empty()) {
      for (const auto& argument : result.diagnostic.arguments) {
        if (const auto* text = argument.text()) {
          failure.diagnostics.front().fields.push_back(
              {"datatype_" + argument.key, *text});
        }
      }
    }
    return failure;
  }

  SblrValue published;
  if (native_numeric_pair) {
    published.descriptor_id = datatypes::CanonicalTypeName(target_type);
    published.is_null = result.value.is_null;
    if (!published.is_null) {
      if (result.value.encoded_value.size() != 8)
        return SpecialFormFailure(operation_id, context, "NUMERIC.ENCODING.NONCANONICAL",
                                  "datatype owner returned a non-native numeric cast result");
      std::uint64_t bits = 0;
      for (unsigned byte = 0; byte < 8; ++byte)
        bits |= static_cast<std::uint64_t>(
            static_cast<unsigned char>(result.value.encoded_value[byte])) << (byte * 8);
      if (target_type == datatypes::CanonicalTypeId::int64) {
        published.payload_kind = SblrValuePayloadKind::signed_integer;
        published.int64_value = std::bit_cast<std::int64_t>(bits);
        published.has_int64_value = true;
      } else {
        published.payload_kind = SblrValuePayloadKind::real64;
        published.real64_value = std::bit_cast<double>(bits);
        published.has_real64_value = true;
      }
    }
  } else {
    published = SblrValueFromDatatypeValue(result.value);
  }
  published.projection_descriptor = std::move(target_binding);
  auto output = ScalarResult(operation_id, std::move(published));
  output.numeric_facts = result.numeric_facts;
  return output;
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
