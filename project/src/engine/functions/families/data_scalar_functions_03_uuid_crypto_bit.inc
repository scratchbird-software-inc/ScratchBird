// Copyright (c) 2026 ScratchBird Software Inc.
//
// This Source Code Form is subject to the terms of the Mozilla Public
// License, v. 2.0. If a copy of the MPL was not distributed with this
// file, You can obtain one at https://mozilla.org/MPL/2.0/.
//
// SPDX-License-Identifier: MPL-2.0

// SBSQL source-layout fragment. Included only by data_scalar_functions.cpp.
// This fragment preserves pre-existing behavior; do not add unrelated future work here.
  }
  return dots == 3 && digits > 0;
}

bool LooksIpv6(const std::string& value) {
  return value.find(':') != std::string::npos;
}

}  // namespace

bool IsDataScalarFunction(const FunctionCallRequest& request) {
  return request.context.function_id.rfind("data.scalar.", 0) == 0 ||
         request.context.function_id.rfind("sb.fn.data.scalar.", 0) == 0 ||
         // SBSFC-010: canonical sb.scalar.<name> ids from the SBsql surface
         // registry's builtin-expression-registry binding flow through the
         // data.scalar family dispatch.
         request.context.function_id.rfind("sb.scalar.", 0) == 0 ||
         // SBSFC-012 / regex_match / uuid_v1_v7 sub-streams register
         // canonicals from additional sb.<package>.<name> namespaces with
         // family=data.scalar in their SeedDefs (sb.temporal.*, sb.session.*,
         // sb.uuid.*, sb.regex.*). Family routing recognizes those prefixes
         // so dispatch reaches the data.scalar handler where the canonical
         // id-specific IdIs entries route to the correct engine guard.
         request.context.function_id.rfind("sb.temporal.", 0) == 0 ||
         request.context.function_id.rfind("sb.session.", 0) == 0 ||
         request.context.function_id.rfind("sb.uuid.", 0) == 0 ||
         request.context.function_id.rfind("sb.regex.", 0) == 0 ||
         request.context.function_id == "sb.special.current_timestamp_keyword" ||
         request.context.function_id == "sb.special.substring_keyword" ||
         request.context.function_id == "sb.special.trim_keyword" ||
         request.context.function_id == "sb.fn.diagnostic.row_count" ||
         IdIs(request.context.function_id, {"nextval", "currval", "lastval", "gen_id", "setval", "next_value_for",
                                            "sb.scalar.nextval", "sb.scalar.currval", "sb.scalar.lastval",
                                            "sb.scalar.gen_id", "sb.scalar.setval",
                                            "data.sequence.next", "data.sequence.current",
                                            "data.identity.current", "data.identity.next",
                                            "domain.validate", "domain.cast_in", "domain.cast_out",
                                            "domain.mask", "domain.read_policy", "domain.method",
                                            "data.domain.validate", "data.domain.cast_in",
                                            "data.domain.cast_out", "data.domain.method"});
}

FunctionCallResult DispatchDataScalarFunction(const FunctionCallRequest& request) {
  const auto& id = request.context.function_id;

  if (IdIs(id, {"nextval", "sb.scalar.nextval", "next_value_for", "data.sequence.next", "data.identity.next"})) {
    if (request.arguments.empty() || IsSqlNull(request.arguments[0].value)) {
      return RefuseFunctionInvalidInput(request, "sequence next requires a sequence UUID argument");
    }
    scratchbird::engine::sblr::SblrSequenceRequest sequence_request;
    sequence_request.context = request.context.sblr_context;
    sequence_request.sequence_uuid = ValueAsText(request.arguments[0].value);
    sequence_request.result_descriptor_id = "int64";
    if (request.arguments.size() > 1 && !IsSqlNull(request.arguments[1].value)) {
      std::int64_t increment = 0;
      if (!ValueAsInt64Strict(request.arguments[1].value, &increment)) {
        return RefuseFunctionInvalidInput(request, "sequence next increment must be an int64-compatible value");
      }
      sequence_request.increment_override = increment;
      sequence_request.has_increment_override = true;
    }
    return FunctionCallResult{scratchbird::engine::sblr::NextSblrSequenceValue(
        &scratchbird::engine::sblr::ProcessSblrSequenceRegistry(), sequence_request)};
  }

  if (IdIs(id, {"gen_id", "sb.scalar.gen_id"})) {
    if (request.arguments.empty() || IsSqlNull(request.arguments[0].value)) {
      return RefuseFunctionInvalidInput(request, "GEN_ID requires generator UUID/name and optional increment");
    }
    scratchbird::engine::sblr::SblrSequenceRequest sequence_request;
    sequence_request.context = request.context.sblr_context;
    sequence_request.sequence_uuid = ValueAsText(request.arguments[0].value);
    sequence_request.result_descriptor_id = "int64";
    if (request.arguments.size() > 1 && !IsSqlNull(request.arguments[1].value)) {
      std::int64_t increment = 0;
      if (!ValueAsInt64Strict(request.arguments[1].value, &increment)) {
        return RefuseFunctionInvalidInput(request, "GEN_ID increment must be an int64-compatible value");
      }
      sequence_request.increment_override = increment;
      sequence_request.has_increment_override = true;
    }
    return FunctionCallResult{scratchbird::engine::sblr::NextSblrSequenceValue(
        &scratchbird::engine::sblr::ProcessSblrSequenceRegistry(), sequence_request)};
  }

  if (IdIs(id, {"currval", "sb.scalar.currval", "data.sequence.current"})) {
    if (request.arguments.empty() || IsSqlNull(request.arguments[0].value)) {
      return RefuseFunctionInvalidInput(request, "sequence current requires a sequence UUID argument");
    }
    scratchbird::engine::sblr::SblrSequenceRequest sequence_request;
    sequence_request.context = request.context.sblr_context;
    sequence_request.sequence_uuid = ValueAsText(request.arguments[0].value);
    sequence_request.result_descriptor_id = "int64";
    return FunctionCallResult{scratchbird::engine::sblr::CurrentSblrSequenceValue(
        &scratchbird::engine::sblr::ProcessSblrSequenceRegistry(), sequence_request)};
  }

  if (IdIs(id, {"setval", "sb.scalar.setval"})) {
    if (request.arguments.size() < 2 || request.arguments.size() > 3 || IsSqlNull(request.arguments[0].value) ||
        IsSqlNull(request.arguments[1].value)) {
      return RefuseFunctionInvalidInput(request, "setval requires sequence UUID/name, int64 value, and optional is_called");
    }
    std::int64_t set_value = 0;
    if (!ValueAsInt64Strict(request.arguments[1].value, &set_value)) {
      return RefuseFunctionInvalidInput(request, "setval value must be an int64-compatible value");
    }
    bool is_called = true;
    if (request.arguments.size() == 3) {
      if (IsSqlNull(request.arguments[2].value)) return MakeFunctionSuccess(request, {MakeNullValue("int64")});
      if (!ParseBooleanText(request.arguments[2].value, &is_called)) {
        return RefuseFunctionInvalidInput(request, "setval is_called must be boolean-compatible");
      }
    }
    scratchbird::engine::sblr::SblrSequenceRequest sequence_request;
    sequence_request.context = request.context.sblr_context;
    sequence_request.sequence_uuid = ValueAsText(request.arguments[0].value);
    sequence_request.result_descriptor_id = "int64";
    sequence_request.set_value = set_value;
    sequence_request.is_called = is_called;
    return FunctionCallResult{scratchbird::engine::sblr::SetSblrSequenceValue(
        &scratchbird::engine::sblr::ProcessSblrSequenceRegistry(), sequence_request)};
  }

  if (IdIs(id, {"lastval", "sb.scalar.lastval", "data.identity.current"})) {
    return FunctionCallResult{scratchbird::engine::sblr::IdentityCurrentValue(request.context.sblr_context)};
  }

  if (IdIs(id, {"domain.validate", "domain.cast_in", "data.domain.validate", "data.domain.cast_in",
                "domain.cast_out", "data.domain.cast_out", "domain.mask", "domain.read_policy",
                "domain.method", "data.domain.method"})) {
    if (request.arguments.size() < 2 || IsSqlNull(request.arguments[0].value)) {
      return RefuseFunctionInvalidInput(request, "domain helper requires domain UUID and value arguments");
    }
    scratchbird::engine::sblr::SblrDomainRequest domain_request;
    domain_request.context = request.context.sblr_context;
    domain_request.domain_uuid = ValueAsText(request.arguments[0].value);
    domain_request.value = request.arguments[1].value;
    if (IdIs(id, {"domain.method", "data.domain.method"})) {
      if (request.arguments.size() < 3 || IsSqlNull(request.arguments[2].value)) {
        return RefuseFunctionInvalidInput(request, "domain method requires domain UUID, value, and method name");
      }
      domain_request.method_name = ValueAsText(request.arguments[2].value);
      return FunctionCallResult{scratchbird::engine::sblr::InvokeSblrDomainMethod(domain_request)};
    }
    if (IdIs(id, {"domain.cast_out", "data.domain.cast_out"})) {
      return FunctionCallResult{scratchbird::engine::sblr::CastSblrDomainValueToBase(domain_request)};
    }
    if (IdIs(id, {"domain.mask", "domain.read_policy"})) {
      return FunctionCallResult{scratchbird::engine::sblr::ApplySblrDomainReadPolicy(domain_request)};
    }
    return FunctionCallResult{scratchbird::engine::sblr::CastSblrValueToDomain(domain_request)};
  }

  if (IdIs(id, {"data.scalar.lower", "sb.fn.string.lower", "sb.scalar.lower", "lower", "lcase"})) return UnaryText(request, LowerAscii);
  if (IdIs(id, {"data.scalar.upper", "sb.fn.string.upper", "sb.scalar.upper", "upper", "ucase"})) return UnaryText(request, UpperAscii);
  if (IdIs(id, {"data.scalar.initcap", "sb.fn.string.initcap", "sb.scalar.initcap", "initcap"})) return UnaryText(request, InitcapText);
  if (IdIs(id, {"data.scalar.trim", "sb.fn.string.trim", "sb.scalar.trim", "sb.scalar.btrim",
                "sb.special.trim_keyword", "trim", "btrim"})) {
    if (request.arguments.size() == 1) return UnaryText(request, TrimAscii);
    if (request.arguments.size() == 2) {
      if (IsSqlNull(request.arguments[0].value) || IsSqlNull(request.arguments[1].value)) {
        return MakeFunctionSuccess(request, {MakeNullValue(request.arguments[0].value.descriptor_id)});
      }
      return MakeFunctionSuccess(request, {MakeTextValue(request.arguments[0].value.descriptor_id,
                                                         TrimChars(ValueAsText(request.arguments[0].value),
                                                                   ValueAsText(request.arguments[1].value)))});
    }
    return RefuseFunctionInvalidInput(request, "trim/btrim expects 1 or 2 arguments");
  }
  if (IdIs(id, {"data.scalar.ltrim", "sb.fn.string.ltrim", "sb.scalar.ltrim", "ltrim"})) {
    if (request.arguments.size() == 1) return UnaryText(request, LTrimAscii);
    if (request.arguments.size() == 2) {
      if (IsSqlNull(request.arguments[0].value) || IsSqlNull(request.arguments[1].value)) {
        return MakeFunctionSuccess(request, {MakeNullValue(request.arguments[0].value.descriptor_id)});
      }
      return MakeFunctionSuccess(request, {MakeTextValue(request.arguments[0].value.descriptor_id,
                                                         LTrimChars(ValueAsText(request.arguments[0].value),
                                                                    ValueAsText(request.arguments[1].value)))});
    }
    return RefuseFunctionInvalidInput(request, "ltrim expects 1 or 2 arguments");
  }
  if (IdIs(id, {"data.scalar.rtrim", "sb.fn.string.rtrim", "sb.scalar.rtrim", "rtrim"})) {
    if (request.arguments.size() == 1) return UnaryText(request, RTrimAscii);
    if (request.arguments.size() == 2) {
      if (IsSqlNull(request.arguments[0].value) || IsSqlNull(request.arguments[1].value)) {
        return MakeFunctionSuccess(request, {MakeNullValue(request.arguments[0].value.descriptor_id)});
      }
      return MakeFunctionSuccess(request, {MakeTextValue(request.arguments[0].value.descriptor_id,
                                                         RTrimChars(ValueAsText(request.arguments[0].value),
                                                                    ValueAsText(request.arguments[1].value)))});
    }
    return RefuseFunctionInvalidInput(request, "rtrim expects 1 or 2 arguments");
  }
  if (IdIs(id, {"data.scalar.reverse", "sb.fn.string.reverse", "sb.scalar.reverse", "reverse"})) return UnaryText(request, ReverseText);

  if (IdIs(id, {"data.scalar.left", "sb.fn.string.left", "sb.scalar.left", "left"})) {
    if (request.arguments.size() != 2) return RefuseFunctionInvalidInput(request, "left expects text and length");
    if (IsSqlNull(request.arguments[0].value) || IsSqlNull(request.arguments[1].value)) {
      return MakeFunctionSuccess(request, {MakeNullValue("character")});
    }
    std::int64_t count = 0;
    if (!ValueAsInt64Strict(request.arguments[1].value, &count)) {
      return RefuseFunctionInvalidInput(request, "left length must be int64-compatible");
    }
    return MakeFunctionSuccess(request, {MakeTextValue("character", LeftTextSlice(ValueAsText(request.arguments[0].value), count))});
  }

  if (IdIs(id, {"data.scalar.right", "sb.fn.string.right", "sb.scalar.right", "right"})) {
    if (request.arguments.size() != 2) return RefuseFunctionInvalidInput(request, "right expects text and length");
    if (IsSqlNull(request.arguments[0].value) || IsSqlNull(request.arguments[1].value)) {
      return MakeFunctionSuccess(request, {MakeNullValue("character")});
    }
    std::int64_t count = 0;
    if (!ValueAsInt64Strict(request.arguments[1].value, &count)) {
      return RefuseFunctionInvalidInput(request, "right length must be int64-compatible");
    }
    return MakeFunctionSuccess(request, {MakeTextValue("character", RightTextSlice(ValueAsText(request.arguments[0].value), count))});
  }

  if (IdIs(id, {"data.scalar.length", "sb.fn.string.length", "sb.scalar.length", "sb.scalar.char_length", "length", "char_length", "character_length"})) {
    if (request.arguments.size() != 1) return RefuseFunctionInvalidInput(request, "expected exactly one argument");
    if (IsSqlNull(request.arguments[0].value)) return MakeFunctionSuccess(request, {MakeNullValue("int64")});
    return MakeFunctionSuccess(request, {MakeInt64Value("int64", static_cast<std::int64_t>(
                                                           ValueAsText(request.arguments[0].value).size()))});
  }

  if (IdIs(id, {"data.scalar.octet_length", "sb.fn.string.octet_length", "sb.scalar.octet_length", "octet_length"})) {
    if (request.arguments.size() != 1) return RefuseFunctionInvalidInput(request, "expected exactly one argument");
    if (IsSqlNull(request.arguments[0].value)) return MakeFunctionSuccess(request, {MakeNullValue("int64")});
    const auto text = ValueAsText(request.arguments[0].value);
    return MakeFunctionSuccess(request, {MakeInt64Value("int64", static_cast<std::int64_t>(text.size()))});
  }

  if (IdIs(id, {"data.scalar.bit_length", "sb.fn.string.bit_length", "sb.scalar.bit_length", "bit_length"})) {
    if (request.arguments.size() != 1) return RefuseFunctionInvalidInput(request, "bit_length expects exactly one text or binary argument");
    if (IsSqlNull(request.arguments[0].value)) return MakeFunctionSuccess(request, {MakeNullValue("int64")});
    const std::size_t byte_count =
        request.arguments[0].value.payload_kind == scratchbird::engine::sblr::SblrValuePayloadKind::binary
            ? request.arguments[0].value.binary_value.size()
            : ValueAsText(request.arguments[0].value).size();
    if (byte_count > static_cast<std::size_t>(std::numeric_limits<std::int64_t>::max() / 8)) {
      return RefuseFunctionOverflow(request, "bit_length result overflows int64");
    }
    return MakeFunctionSuccess(request, {MakeInt64Value("int64", static_cast<std::int64_t>(byte_count * 8))});
  }

  if (IdIs(id, {"data.scalar.chr", "sb.fn.string.chr", "sb.scalar.chr", "chr"})) {
    if (request.arguments.size() != 1) return RefuseFunctionInvalidInput(request, "chr expects exactly one integer argument");
    if (IsSqlNull(request.arguments[0].value)) return MakeFunctionSuccess(request, {MakeNullValue("character")});
    if (!request.arguments[0].value.has_int64_value) return RefuseFunctionInvalidInput(request, "chr argument must be an int64 scalar value");
    const std::int64_t codepoint = request.arguments[0].value.int64_value;
    if (codepoint < 0 || codepoint > 127) {
      return RefuseFunctionInvalidInput(request, "chr argument must be in ASCII range [0, 127]");
    }
    return MakeFunctionSuccess(request, {MakeTextValue("character", std::string(1, static_cast<char>(codepoint)))});
  }

  if (IdIs(id, {"data.scalar.ascii", "sb.fn.string.ascii", "sb.scalar.ascii", "ascii"})) {
    if (request.arguments.size() != 1) return RefuseFunctionInvalidInput(request, "ascii expects exactly one text argument");
    if (IsSqlNull(request.arguments[0].value)) return MakeFunctionSuccess(request, {MakeNullValue("int64")});
    const auto text = ValueAsText(request.arguments[0].value);
    if (text.empty()) return MakeFunctionSuccess(request, {MakeInt64Value("int64", 0)});
    return MakeFunctionSuccess(request, {MakeInt64Value("int64", static_cast<std::int64_t>(static_cast<unsigned char>(text[0])))});
  }

  if (IdIs(id, {"data.scalar.unicode", "sb.fn.string.unicode", "sb.scalar.unicode", "unicode"})) {
    if (request.arguments.size() != 1) return RefuseFunctionInvalidInput(request, "unicode expects exactly one text argument");
    if (IsSqlNull(request.arguments[0].value)) return MakeFunctionSuccess(request, {MakeNullValue("int64")});
    std::int64_t codepoint = 0;
    if (!Utf8FirstCodepoint(ValueAsText(request.arguments[0].value), &codepoint)) {
      return RefuseFunctionInvalidInput(request, "unicode expects valid UTF-8 text");
    }
    return MakeFunctionSuccess(request, {MakeInt64Value("int64", codepoint)});
  }

  if (IdIs(id, {"data.scalar.normalize",
                "data.scalar.unicode_normalize",
                "sb.scalar.normalize",
                "sb.scalar.normalize_text_form",
                "sb.scalar.unicode_normalize",
                "normalize",
                "normalize(text[,NFC|NFD|NFKC|NFKD])",
                "unicode_normalize"})) {
    if (request.arguments.empty()) {
      return MakeFunctionSuccess(
          request,
          {MakeTextValue("character", "unicode_normalization.nfc.route")});
    }
    if (request.arguments.size() > 2) {
      return RefuseFunctionInvalidInput(
          request,
          "normalize expects zero arguments, one text argument, or text plus NFC/NFD/NFKC/NFKD form");
    }
    if (IsSqlNull(request.arguments[0].value)) {
      return MakeFunctionSuccess(request, {MakeNullValue("character")});
    }
    std::string form = "NFC";
    if (request.arguments.size() == 2) {
      if (IsSqlNull(request.arguments[1].value)) {
        return MakeFunctionSuccess(request, {MakeNullValue("character")});
      }
      form = ValueAsText(request.arguments[1].value);
    }
    std::string normalized;
    std::string canonical_form;
    std::string error;
    if (!NormalizeUnicodeText(ValueAsText(request.arguments[0].value),
                              std::move(form),
                              &normalized,
                              &canonical_form,
                              &error)) {
      return RefuseFunctionInvalidInput(
          request,
          error.empty() ? "normalize expects valid UTF-8 text and a supported form" : error);
    }
    return MakeFunctionSuccess(request, {MakeTextValue("character", std::move(normalized))});
  }

  if (IdIs(id, {"data.scalar.is_alpha", "sb.scalar.is_alpha", "is_alpha"})) {
    if (request.arguments.empty()) {
      return MakeFunctionSuccess(request, {MakeBooleanValue(false)});
    }
    if (request.arguments.size() != 1) {
      return RefuseFunctionInvalidInput(request, "is_alpha expects zero or one text argument");
    }
    if (IsSqlNull(request.arguments[0].value)) {
      return MakeFunctionSuccess(request, {MakeNullValue("boolean")});
    }
    bool alpha = false;
    std::string error;
    if (!IsUnicodeAlphaText(ValueAsText(request.arguments[0].value), &alpha, &error)) {
      return RefuseFunctionInvalidInput(
          request,
          error.empty() ? "is_alpha expects valid UTF-8 text" : error);
    }
    return MakeFunctionSuccess(request, {MakeBooleanValue(alpha)});
  }

  const auto is_bit_string_value = [](const scratchbird::engine::sblr::SblrValue& value) {
    return value.descriptor_id == "bit_string" &&
           value.payload_kind == scratchbird::engine::sblr::SblrValuePayloadKind::binary;
  };
  const auto bit_at = [](const std::vector<std::uint8_t>& bytes, std::size_t bit_index) {
    const auto byte = bytes[bit_index / 8];
    const auto shift = 7u - static_cast<unsigned>(bit_index % 8);
    return ((byte >> shift) & 0x01u) != 0;
  };
  const auto slice_bits = [&](const std::vector<std::uint8_t>& bytes,
                              std::size_t start_bit,
                              std::size_t bit_count) {
    std::vector<std::uint8_t> out((bit_count + 7u) / 8u, 0);
    for (std::size_t i = 0; i < bit_count; ++i) {
      if (!bit_at(bytes, start_bit + i)) continue;
      out[i / 8] |= static_cast<std::uint8_t>(1u << (7u - static_cast<unsigned>(i % 8)));
    }
    return out;
  };

  if (IdIs(id, {"data.scalar.substring", "sb.fn.string.substring", "sb.scalar.substring",
                "sb.scalar.substr", "sb.special.substring_keyword",
                "sb.scalar.bit_string_substring", "bit_string_substring",
                "substring", "substr", "mid"})) {
    if (request.arguments.size() != 2 && request.arguments.size() != 3) {
      return RefuseFunctionInvalidInput(request, "expected value, one-based start, optional length");
    }
    if (IsSqlNull(request.arguments[0].value) || IsSqlNull(request.arguments[1].value) ||
        (request.arguments.size() == 3 && IsSqlNull(request.arguments[2].value))) {
      return MakeFunctionSuccess(request, {MakeNullValue(request.arguments[0].value.descriptor_id)});
    }
    if (!request.arguments[1].value.has_int64_value ||
        (request.arguments.size() == 3 && !request.arguments[2].value.has_int64_value)) {
      return RefuseFunctionInvalidInput(request, "substring start and length must be int64 scalar values");
    }
    if (is_bit_string_value(request.arguments[0].value)) {
      const auto& source = request.arguments[0].value.binary_value;
      const std::int64_t one_based_start = request.arguments[1].value.int64_value;
      if (one_based_start < 1) return RefuseFunctionInvalidInput(request, "substring start is one-based and must be >= 1");
      const std::size_t start = static_cast<std::size_t>(one_based_start - 1);
      const std::size_t total_bits = source.size() * 8u;
      if (start >= total_bits) {
        return MakeFunctionSuccess(request, {MakeBinaryValue(request.arguments[0].value.descriptor_id, {})});
      }
      std::size_t count = total_bits - start;
      if (request.arguments.size() == 3) {
        if (request.arguments[2].value.int64_value < 0) {
          return RefuseFunctionInvalidInput(request, "substring length must be >= 0");
        }
        count = static_cast<std::size_t>(request.arguments[2].value.int64_value);
      }
      if (start + count > total_bits) count = total_bits - start;
      return MakeFunctionSuccess(request,
                                 {MakeBinaryValue(request.arguments[0].value.descriptor_id,
                                                  slice_bits(source, start, count))});
    }
    const auto source = ValueAsText(request.arguments[0].value);
    const std::int64_t one_based_start = request.arguments[1].value.int64_value;
    if (one_based_start < 1) return RefuseFunctionInvalidInput(request, "substring start is one-based and must be >= 1");
    const std::size_t start = static_cast<std::size_t>(one_based_start - 1);
