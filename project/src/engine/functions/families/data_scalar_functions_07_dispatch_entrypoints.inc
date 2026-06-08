// Copyright (c) 2026 ScratchBird Software Inc.
//
// This Source Code Form is subject to the terms of the Mozilla Public
// License, v. 2.0. If a copy of the MPL was not distributed with this
// file, You can obtain one at https://mozilla.org/MPL/2.0/.
//
// SPDX-License-Identifier: MPL-2.0

// SBSQL source-layout fragment. Included only by data_scalar_functions.cpp.
// This fragment preserves pre-existing behavior; do not add unrelated future work here.
    if (IsSqlNull(request.arguments[0].value) || IsSqlNull(request.arguments[1].value)) return MakeFunctionSuccess(request, {MakeNullValue("character")});
    if (!request.arguments[1].value.has_int64_value || request.arguments[1].value.int64_value < 0) {
      return RefuseFunctionInvalidInput(request, "array_get index must be a non-negative int64");
    }
    const auto values = SplitCollectionText(ValueAsText(request.arguments[0].value));
    const auto index = static_cast<std::size_t>(request.arguments[1].value.int64_value);
    return index >= values.size() ? MakeFunctionSuccess(request, {MakeNullValue("character")})
                                  : MakeFunctionSuccess(request, {MakeTextValue("character", values[index])});
  }

  if (IdIs(id, {"sb.scalar.soundex", "soundex", "text_soundex"})) {
    if (request.arguments.size() != 1) return RefuseFunctionInvalidInput(request, "soundex expects exactly one text argument");
    if (IsSqlNull(request.arguments[0].value)) return MakeFunctionSuccess(request, {MakeNullValue("character")});
    return MakeFunctionSuccess(request, {MakeTextValue("character", SoundexText(ValueAsText(request.arguments[0].value)))});
  }

  if (IdIs(id, {"sb.scalar.metaphone", "metaphone"})) {
    if (request.arguments.size() != 1 && request.arguments.size() != 2) {
      return RefuseFunctionInvalidInput(request, "metaphone expects text and optional max_length");
    }
    if (IsSqlNull(request.arguments[0].value) ||
        (request.arguments.size() == 2 && IsSqlNull(request.arguments[1].value))) {
      return MakeFunctionSuccess(request, {MakeNullValue("character")});
    }
    std::int64_t max_length = 0;
    if (request.arguments.size() == 2) {
      if (!ValueAsInt64Strict(request.arguments[1].value, &max_length) || max_length <= 0 || max_length > 1024) {
        return RefuseFunctionInvalidInput(request, "metaphone max_length must be a positive int64 in [1, 1024]");
      }
    }
    return MakeFunctionSuccess(request, {MakeTextValue("character",
                                                       MetaphoneText(ValueAsText(request.arguments[0].value),
                                                                     static_cast<std::size_t>(max_length)))});
  }

  if (IdIs(id, {"sb.scalar.dmetaphone", "sb.scalar.dmetaphone_alt", "dmetaphone", "dmetaphone_alt"})) {
    if (request.arguments.size() != 1) return RefuseFunctionInvalidInput(request, "dmetaphone expects exactly one text argument");
    if (IsSqlNull(request.arguments[0].value)) return MakeFunctionSuccess(request, {MakeNullValue("character")});
    const bool alternate = IdIs(id, {"sb.scalar.dmetaphone_alt", "dmetaphone_alt"});
    return MakeFunctionSuccess(request, {MakeTextValue("character", DoubleMetaphoneText(ValueAsText(request.arguments[0].value), alternate))});
  }

  if (IdIs(id, {"sb.scalar.levenshtein", "levenshtein"})) {
    if (request.arguments.size() != 2 && request.arguments.size() != 5) {
      return RefuseFunctionInvalidInput(request, "levenshtein expects text, text, and optional insert/delete/substitute costs");
    }
    for (const auto& argument : request.arguments) {
      if (IsSqlNull(argument.value)) return MakeFunctionSuccess(request, {MakeNullValue("int64")});
    }
    const auto left = ValueAsText(request.arguments[0].value);
    const auto right = ValueAsText(request.arguments[1].value);
    if (!EditDistanceSizeAllowed(left.size(), right.size())) {
      return RefuseFunctionInvalidInput(request, "levenshtein input product exceeds scalar resource budget");
    }
    std::int64_t insert_cost = 1;
    std::int64_t delete_cost = 1;
    std::int64_t substitute_cost = 1;
    if (request.arguments.size() == 5) {
      if (!ValueAsInt64Strict(request.arguments[2].value, &insert_cost) ||
          !ValueAsInt64Strict(request.arguments[3].value, &delete_cost) ||
          !ValueAsInt64Strict(request.arguments[4].value, &substitute_cost) ||
          insert_cost <= 0 || delete_cost <= 0 || substitute_cost <= 0) {
        return RefuseFunctionInvalidInput(request, "levenshtein costs must be positive int64 values");
      }
    }
    return MakeFunctionSuccess(request, {MakeInt64Value("int64", LevenshteinDistance(left, right, insert_cost, delete_cost, substitute_cost))});
  }

  if (IdIs(id, {"sb.scalar.levenshtein_le", "levenshtein_le", "levenshtein_less_equal"})) {
    if (request.arguments.size() != 3) return RefuseFunctionInvalidInput(request, "levenshtein_le expects text, text, and max distance");
    for (const auto& argument : request.arguments) {
      if (IsSqlNull(argument.value)) return MakeFunctionSuccess(request, {MakeNullValue("int64")});
    }
    std::int64_t max_distance = 0;
    if (!ValueAsInt64Strict(request.arguments[2].value, &max_distance) || max_distance < 0) {
      return RefuseFunctionInvalidInput(request, "levenshtein_le max distance must be a non-negative int64");
    }
    const auto left = ValueAsText(request.arguments[0].value);
    const auto right = ValueAsText(request.arguments[1].value);
    if (!EditDistanceSizeAllowed(left.size(), right.size())) {
      return RefuseFunctionInvalidInput(request, "levenshtein_le input product exceeds scalar resource budget");
    }
    return MakeFunctionSuccess(request, {MakeInt64Value("int64", LevenshteinDistance(left, right, 1, 1, 1, max_distance))});
  }

  if (IdIs(id, {"sb.scalar.damerau_levenshtein", "damerau_levenshtein"})) {
    if (request.arguments.size() != 2) return RefuseFunctionInvalidInput(request, "damerau_levenshtein expects two text arguments");
    if (IsSqlNull(request.arguments[0].value) || IsSqlNull(request.arguments[1].value)) {
      return MakeFunctionSuccess(request, {MakeNullValue("int64")});
    }
    const auto left = ValueAsText(request.arguments[0].value);
    const auto right = ValueAsText(request.arguments[1].value);
    if (!EditDistanceSizeAllowed(left.size(), right.size())) {
      return RefuseFunctionInvalidInput(request, "damerau_levenshtein input product exceeds scalar resource budget");
    }
    return MakeFunctionSuccess(request, {MakeInt64Value("int64", DamerauLevenshteinDistance(left, right))});
  }

  if (IdIs(id, {"sb.scalar.jaro_similarity", "jaro_similarity"})) {
    if (request.arguments.size() != 2) return RefuseFunctionInvalidInput(request, "jaro_similarity expects two text arguments");
    if (IsSqlNull(request.arguments[0].value) || IsSqlNull(request.arguments[1].value)) {
      return MakeFunctionSuccess(request, {MakeNullValue("real64")});
    }
    return MakeFunctionSuccess(request, {MakeReal64Value("real64", JaroSimilarity(ValueAsText(request.arguments[0].value),
                                                                                  ValueAsText(request.arguments[1].value)))});
  }

  if (IdIs(id, {"sb.scalar.jaro_winkler_similarity", "jaro_winkler_similarity"})) {
    if (request.arguments.size() != 2) return RefuseFunctionInvalidInput(request, "jaro_winkler_similarity expects two text arguments");
    if (IsSqlNull(request.arguments[0].value) || IsSqlNull(request.arguments[1].value)) {
      return MakeFunctionSuccess(request, {MakeNullValue("real64")});
    }
    return MakeFunctionSuccess(request, {MakeReal64Value("real64", JaroWinklerSimilarity(ValueAsText(request.arguments[0].value),
                                                                                         ValueAsText(request.arguments[1].value)))});
  }

  if (IdIs(id, {"sb.scalar.similarity", "similarity"})) {
    if (request.arguments.size() != 2) return RefuseFunctionInvalidInput(request, "similarity expects two text arguments");
    if (IsSqlNull(request.arguments[0].value) || IsSqlNull(request.arguments[1].value)) {
      return MakeFunctionSuccess(request, {MakeNullValue("real64")});
    }
    if (!EditDistanceSizeAllowed(ValueAsText(request.arguments[0].value).size(), ValueAsText(request.arguments[1].value).size())) {
      return RefuseFunctionInvalidInput(request, "similarity input product exceeds scalar resource budget");
    }
    return MakeFunctionSuccess(request, {MakeReal64Value("real64", TrigramSimilarity(ValueAsText(request.arguments[0].value),
                                                                                     ValueAsText(request.arguments[1].value)))});
  }

  if (IdIs(id, {"sb.scalar.word_similarity", "word_similarity"})) {
    if (request.arguments.size() != 2) return RefuseFunctionInvalidInput(request, "word_similarity expects two text arguments");
    if (IsSqlNull(request.arguments[0].value) || IsSqlNull(request.arguments[1].value)) {
      return MakeFunctionSuccess(request, {MakeNullValue("real64")});
    }
    if (!EditDistanceSizeAllowed(ValueAsText(request.arguments[0].value).size(), ValueAsText(request.arguments[1].value).size())) {
      return RefuseFunctionInvalidInput(request, "word_similarity input product exceeds scalar resource budget");
    }
    return MakeFunctionSuccess(request, {MakeReal64Value("real64", WordTrigramSimilarity(ValueAsText(request.arguments[0].value),
                                                                                         ValueAsText(request.arguments[1].value)))});
  }

  if (IdIs(id, {"sb.scalar.show_trgm", "show_trgm"})) {
    if (request.arguments.size() != 1) return RefuseFunctionInvalidInput(request, "show_trgm expects exactly one text argument");
    if (IsSqlNull(request.arguments[0].value)) return MakeFunctionSuccess(request, {MakeNullValue("array")});
    return MakeFunctionSuccess(
        request,
        {MakeTextValue("array", JsonArrayFromTextValues(TrigramsForText(ValueAsText(request.arguments[0].value))))});
  }

  if (IdIs(id, {"sb.scalar.pg_trgm", "pg_trgm"})) {
    if (!request.arguments.empty()) return RefuseFunctionInvalidInput(request, "pg_trgm expects no arguments");
    return MakeFunctionSuccess(
        request,
        {MakeTextValue(
            "json_document",
            "{\"capability\":\"pg_trgm\","
            "\"provider\":\"scratchbird-native-scalar\","
            "\"available\":true,"
            "\"mutable\":false,"
            "\"functions\":[\"similarity\",\"word_similarity\",\"show_trgm\"]}")});
  }

  if (IdIs(id, {"data.scalar.array_contains", "sb.fn.collection.contains", "array_contains"})) {
    if (request.arguments.size() != 2) return RefuseFunctionInvalidInput(request, "array_contains expects collection and value");
    if (IsSqlNull(request.arguments[0].value) || IsSqlNull(request.arguments[1].value)) return MakeFunctionSuccess(request, {MakeNullValue("boolean")});
    const auto values = SplitCollectionText(ValueAsText(request.arguments[0].value));
    const auto needle = ValueAsText(request.arguments[1].value);
    return MakeFunctionSuccess(request, {MakeInt64Value("boolean", std::find(values.begin(), values.end(), needle) == values.end() ? 0 : 1)});
  }

  if (IdIs(id, {"data.scalar.array_concat", "sb.fn.collection.array_concat", "array_concat"})) {
    if (request.arguments.size() != 2) return RefuseFunctionInvalidInput(request, "array_concat expects two collections");
    if (IsSqlNull(request.arguments[0].value)) return MakeFunctionSuccess(request, {request.arguments[1].value});
    if (IsSqlNull(request.arguments[1].value)) return MakeFunctionSuccess(request, {request.arguments[0].value});
    auto left = SplitCollectionText(ValueAsText(request.arguments[0].value));
    auto right = SplitCollectionText(ValueAsText(request.arguments[1].value));
    left.insert(left.end(), right.begin(), right.end());
    std::string out = "[";
    for (std::size_t i = 0; i < left.size(); ++i) {
      if (i != 0) out += ",";
      out += left[i];
    }
    out += "]";
    return MakeFunctionSuccess(request, {MakeTextValue("array", std::move(out))});
  }

  if (IdIs(id, {"data.scalar.inet_family", "sb.fn.network.inet_family", "inet_family"})) {
    if (request.arguments.size() != 1) return RefuseFunctionInvalidInput(request, "inet_family expects one address argument");
    if (IsSqlNull(request.arguments[0].value)) return MakeFunctionSuccess(request, {MakeNullValue("int64")});
    const auto address = ValueAsText(request.arguments[0].value);
    if (LooksIpv4(address)) return MakeFunctionSuccess(request, {MakeInt64Value("int64", 4)});
    if (LooksIpv6(address)) return MakeFunctionSuccess(request, {MakeInt64Value("int64", 6)});
    return RefuseFunctionInvalidInput(request, "inet_family expects IPv4 or IPv6 text");
  }

  if (IdIs(id, {"data.scalar.is_ipv4", "sb.fn.network.is_ipv4", "is_ipv4"})) {
    if (request.arguments.size() != 1) return RefuseFunctionInvalidInput(request, "is_ipv4 expects one address argument");
    if (IsSqlNull(request.arguments[0].value)) return MakeFunctionSuccess(request, {MakeNullValue("boolean")});
    return MakeFunctionSuccess(request, {MakeInt64Value("boolean", LooksIpv4(ValueAsText(request.arguments[0].value)) ? 1 : 0)});
  }

  if (IdIs(id, {"data.scalar.is_ipv6", "sb.fn.network.is_ipv6", "is_ipv6"})) {
    if (request.arguments.size() != 1) return RefuseFunctionInvalidInput(request, "is_ipv6 expects one address argument");
    if (IsSqlNull(request.arguments[0].value)) return MakeFunctionSuccess(request, {MakeNullValue("boolean")});
    return MakeFunctionSuccess(request, {MakeInt64Value("boolean", LooksIpv6(ValueAsText(request.arguments[0].value)) ? 1 : 0)});
  }

  if (IdIs(id, {"data.scalar.current_database_uuid", "sb.fn.system.current_database_uuid"})) {
    return MakeFunctionSuccess(request, {MakeTextValue("uuid", request.context.sblr_context.database_uuid)});
  }

  if (IdIs(id, {"data.scalar.current_node_uuid", "sb.fn.system.current_node_uuid"})) {
    return MakeFunctionSuccess(request, {MakeTextValue("uuid", request.context.sblr_context.node_uuid)});
  }

  if (IdIs(id, {"data.scalar.encode", "sb.fn.encoding.encode", "sb.scalar.encode", "encode"})) {
    if (request.arguments.size() != 2) return RefuseFunctionInvalidInput(request, "encode expects binary value and format");
    if (IsSqlNull(request.arguments[0].value) || IsSqlNull(request.arguments[1].value)) {
      return MakeFunctionSuccess(request, {MakeNullValue("character")});
    }
    if (request.arguments[0].value.payload_kind != scratchbird::engine::sblr::SblrValuePayloadKind::binary) {
      return RefuseFunctionInvalidInput(request, "encode input must be binary");
    }
    const std::string format = LowerAscii(ValueAsText(request.arguments[1].value));
    const auto bytes = request.arguments[0].value.binary_value;
    if (format == "hex") return MakeFunctionSuccess(request, {MakeTextValue("character", HexEncodeBytes(bytes))});
    if (format == "base64") return MakeFunctionSuccess(request, {MakeTextValue("character", Base64EncodeBytes(bytes))});
    if (format == "escape") return MakeFunctionSuccess(request, {MakeTextValue("character", EscapeEncodeBytes(bytes))});
    return RefuseFunctionInvalidInput(request, "encode format must be one of hex, base64, or escape");
  }

  if (IdIs(id, {"data.scalar.decode", "sb.fn.encoding.decode", "sb.scalar.decode", "decode"})) {
    if (request.arguments.size() != 2) return RefuseFunctionInvalidInput(request, "decode expects text value and format");
    if (IsSqlNull(request.arguments[0].value) || IsSqlNull(request.arguments[1].value)) {
      return MakeFunctionSuccess(request, {MakeNullValue("binary")});
    }
    const std::string input = ValueAsText(request.arguments[0].value);
    const std::string format = LowerAscii(ValueAsText(request.arguments[1].value));
    std::vector<std::uint8_t> bytes;
    if (format == "hex") {
      if (!HexDecodeBytes(input, &bytes)) return RefuseFunctionInvalidInput(request, "decode hex input must contain an even number of hexadecimal digits");
      return MakeFunctionSuccess(request, {MakeBinaryValue("binary", std::move(bytes))});
    }
    if (format == "base64") {
      if (!CanonicalBase64Text(input) || !Base64DecodeBytes(input, &bytes)) {
        return RefuseFunctionInvalidInput(request, "decode base64 input is not canonical base64 text");
      }
      return MakeFunctionSuccess(request, {MakeBinaryValue("binary", std::move(bytes))});
    }
    if (format == "escape") {
      if (!EscapeDecodeBytes(input, &bytes)) return RefuseFunctionInvalidInput(request, "decode escape input is not valid escape text");
      return MakeFunctionSuccess(request, {MakeBinaryValue("binary", std::move(bytes))});
    }
    return RefuseFunctionInvalidInput(request, "decode format must be one of hex, base64, or escape");
  }

  if (IdIs(id, {"sb.scalar.to_numeric", "to_numeric", "sb.scalar.to_number", "to_number",
                "sb.scalar.to_double", "to_double", "sb.scalar.to_real", "to_real"})) {
    if (request.arguments.size() != 1) {
      return RefuseFunctionInvalidInput(request, "numeric conversion expects exactly one argument");
    }
    if (IsSqlNull(request.arguments[0].value)) return MakeFunctionSuccess(request, {MakeNullValue("real64")});
    double converted = 0.0;
    if (!ValueAsReal64(request.arguments[0].value, &converted) || !std::isfinite(converted)) {
      return RefuseFunctionInvalidInput(request, "numeric conversion requires a finite numeric value");
    }
    return MakeFunctionSuccess(request, {MakeReal64Value("real64", converted)});
  }

  if (IdIs(id, {"sb.scalar.to_integer", "to_integer", "sb.scalar.to_bigint", "to_bigint"})) {
    if (request.arguments.size() != 1) {
      return RefuseFunctionInvalidInput(request, "integer conversion expects exactly one argument");
    }
    if (IsSqlNull(request.arguments[0].value)) return MakeFunctionSuccess(request, {MakeNullValue("int64")});
    std::int64_t converted = 0;
    if (!ValueAsInt64Strict(request.arguments[0].value, &converted)) {
      return RefuseFunctionInvalidInput(request, "integer conversion requires an exact int64 value");
    }
    return MakeFunctionSuccess(request, {MakeInt64Value("int64", converted)});
  }

  if (IdIs(id, {"sb.scalar.to_boolean", "to_boolean"})) {
    if (request.arguments.size() != 1) {
      return RefuseFunctionInvalidInput(request, "boolean conversion expects exactly one argument");
    }
    if (IsSqlNull(request.arguments[0].value)) return MakeFunctionSuccess(request, {MakeNullValue("boolean")});
    bool converted = false;
    if (!ParseBooleanText(request.arguments[0].value, &converted)) {
      return RefuseFunctionInvalidInput(request, "boolean conversion requires boolean-compatible text or integer");
    }
    return MakeFunctionSuccess(request, {MakeInt64Value("boolean", converted ? 1 : 0)});
  }

  if (IdIs(id, {"sb.scalar.is_ascii", "is_ascii", "sb.scalar.is_alphanumeric", "is_alphanumeric",
                "sb.scalar.is_space", "is_space", "sb.scalar.is_digit", "is_digit"})) {
    if (request.arguments.size() != 1) {
      return RefuseFunctionInvalidInput(request, "text classification expects exactly one argument");
    }
    if (IsSqlNull(request.arguments[0].value)) return MakeFunctionSuccess(request, {MakeNullValue("boolean")});
    const auto text = ValueAsText(request.arguments[0].value);
    bool matched = false;
    if (IdIs(id, {"sb.scalar.is_ascii", "is_ascii"})) {
      matched = IsAsciiText(text);
    } else if (IdIs(id, {"sb.scalar.is_alphanumeric", "is_alphanumeric"})) {
      matched = !text.empty() && std::all_of(text.begin(), text.end(), [](unsigned char ch) {
        return ch < 0x80 && std::isalnum(ch);
      });
    } else if (IdIs(id, {"sb.scalar.is_space", "is_space"})) {
      matched = !text.empty() && std::all_of(text.begin(), text.end(), [](unsigned char ch) {
        return ch < 0x80 && std::isspace(ch);
      });
    } else {
      matched = !text.empty() && std::all_of(text.begin(), text.end(), [](unsigned char ch) {
        return ch < 0x80 && std::isdigit(ch);
      });
    }
    return MakeFunctionSuccess(request, {MakeInt64Value("boolean", matched ? 1 : 0)});
  }

  if (IdIs(id, {"sb.scalar.quote_ident", "quote_ident"})) {
    if (request.arguments.size() != 1) return RefuseFunctionInvalidInput(request, "quote_ident expects exactly one argument");
    if (IsSqlNull(request.arguments[0].value)) return MakeFunctionSuccess(request, {MakeNullValue("character")});
    return MakeFunctionSuccess(request, {MakeTextValue("character", QuoteIdentifierText(ValueAsText(request.arguments[0].value)))});
  }

  if (IdIs(id, {"sb.scalar.quote_literal", "quote_literal", "sb.scalar.quote_nullable", "quote_nullable"})) {
    if (request.arguments.size() != 1) return RefuseFunctionInvalidInput(request, "quote literal function expects exactly one argument");
    if (IsSqlNull(request.arguments[0].value)) {
      if (IdIs(id, {"sb.scalar.quote_nullable", "quote_nullable"})) {
        return MakeFunctionSuccess(request, {MakeTextValue("character", "NULL")});
      }
      return MakeFunctionSuccess(request, {MakeNullValue("character")});
    }
    return MakeFunctionSuccess(request, {MakeTextValue("character", QuoteLiteralText(ValueAsText(request.arguments[0].value)))});
  }

  if (IdIs(id, {"data.scalar.similar_to_escape", "sb.scalar.similar_to_escape", "similar_to_escape"})) {
    if (request.arguments.size() != 1) {
      return RefuseFunctionInvalidInput(request, "similar_to_escape expects exactly one text argument");
    }
    if (IsSqlNull(request.arguments[0].value)) {
      return MakeFunctionSuccess(request, {MakeNullValue("character")});
    }
    const std::string input = ValueAsText(request.arguments[0].value);
    std::string out;
    out.reserve(input.size() * 2);
    for (const char ch : input) {
      switch (ch) {
        case '%':
        case '_':
        case '|':
        case '*':
        case '+':
        case '?':
        case '{':
        case '}':
        case '(':
        case ')':
        case '[':
        case ']':
        case '.':
        case '^':
        case '$':
        case '\\':
          out.push_back('\\');
          break;
        default:
          break;
      }
      out.push_back(ch);
    }
    return MakeFunctionSuccess(request, {MakeTextValue("character", std::move(out))});
  }

  if (IdIs(id, {"sb.scalar.bit_set", "bit_set", "sb.scalar.bit_clear", "bit_clear",
                "sb.scalar.bit_toggle", "bit_toggle", "sb.scalar.bit_test", "bit_test"})) {
    if (request.arguments.size() != 2) return RefuseFunctionInvalidInput(request, "bit helper expects value and position");
    if (IsSqlNull(request.arguments[0].value) || IsSqlNull(request.arguments[1].value)) {
      return MakeFunctionSuccess(request, {MakeNullValue(IdIs(id, {"sb.scalar.bit_test", "bit_test"}) ? "boolean" : "int64")});
    }
    std::int64_t input = 0;
    std::int64_t position = 0;
    if (!ValueAsInt64Strict(request.arguments[0].value, &input) ||
        !ValueAsInt64Strict(request.arguments[1].value, &position) ||
        position < 0 || position > 63) {
      return RefuseFunctionInvalidInput(request, "bit helper requires int64 value and bit position in [0, 63]");
    }
    const auto mask = static_cast<std::uint64_t>(1) << static_cast<unsigned>(position);
    const auto bits = static_cast<std::uint64_t>(input);
    if (IdIs(id, {"sb.scalar.bit_test", "bit_test"})) {
      return MakeFunctionSuccess(request, {MakeInt64Value("boolean", (bits & mask) != 0 ? 1 : 0)});
    }
    std::uint64_t output = bits;
    if (IdIs(id, {"sb.scalar.bit_set", "bit_set"})) {
      output |= mask;
    } else if (IdIs(id, {"sb.scalar.bit_clear", "bit_clear"})) {
      output &= ~mask;
    } else {
      output ^= mask;
    }
    return MakeFunctionSuccess(request, {MakeInt64Value("int64", static_cast<std::int64_t>(output))});
  }

  if (IdIs(id, {"sb.scalar.crc32", "crc32", "sb.scalar.crc32c", "crc32c"})) {
    if (request.arguments.size() != 1) return RefuseFunctionInvalidInput(request, "CRC helper expects exactly one argument");
    if (IsSqlNull(request.arguments[0].value)) return MakeFunctionSuccess(request, {MakeNullValue("uint64")});
    const auto bytes = RawBytesFromValue(request.arguments[0].value);
    const auto crc = IdIs(id, {"sb.scalar.crc32c", "crc32c"}) ? Crc32cBytes(bytes) : Crc32Bytes(bytes);
    return MakeFunctionSuccess(request, {MakeUint64Value("uint64", static_cast<std::uint64_t>(crc))});
  }

  if (IdIs(id, {"sb.scalar.to_bytes", "to_bytes"})) {
    if (request.arguments.size() != 1 && request.arguments.size() != 2) {
      return RefuseFunctionInvalidInput(request, "to_bytes expects text and optional UTF-8 encoding");
    }
    if (IsSqlNull(request.arguments[0].value) ||
        (request.arguments.size() == 2 && IsSqlNull(request.arguments[1].value))) {
      return MakeFunctionSuccess(request, {MakeNullValue("binary")});
    }
    if (request.arguments.size() == 2 && !IsUtf8EncodingName(ValueAsText(request.arguments[1].value))) {
      return RefuseFunctionInvalidInput(request, "to_bytes currently supports utf8/utf-8 encoding only");
    }
    const auto text = ValueAsText(request.arguments[0].value);
    return MakeFunctionSuccess(request, {MakeBinaryValue("binary", std::vector<std::uint8_t>(text.begin(), text.end()))});
  }

  if (IdIs(id, {"sb.scalar.from_bytes", "from_bytes"})) {
    if (request.arguments.size() != 1 && request.arguments.size() != 2) {
      return RefuseFunctionInvalidInput(request, "from_bytes expects binary and optional UTF-8 encoding");
    }
    if (IsSqlNull(request.arguments[0].value) ||
        (request.arguments.size() == 2 && IsSqlNull(request.arguments[1].value))) {
      return MakeFunctionSuccess(request, {MakeNullValue("character")});
    }
    if (request.arguments[0].value.payload_kind != scratchbird::engine::sblr::SblrValuePayloadKind::binary) {
      return RefuseFunctionInvalidInput(request, "from_bytes input must be binary");
    }
    if (request.arguments.size() == 2 && !IsUtf8EncodingName(ValueAsText(request.arguments[1].value))) {
      return RefuseFunctionInvalidInput(request, "from_bytes currently supports utf8/utf-8 encoding only");
    }
    const auto& bytes = request.arguments[0].value.binary_value;
    return MakeFunctionSuccess(request, {MakeTextValue("character", std::string(bytes.begin(), bytes.end()))});
  }

  if (IdIs(id, {"data.scalar.hash64", "data.scalar.checksum64", "sb.fn.crypto.hash64", "hash64", "checksum64"})) {
    if (request.arguments.size() != 1) return RefuseFunctionInvalidInput(request, "hash64/checksum64 expects one argument");
    if (IsSqlNull(request.arguments[0].value)) return MakeFunctionSuccess(request, {MakeNullValue("uint64")});
    return MakeFunctionSuccess(request, {MakeUint64Value("uint64", Fnva64(RawBytesFromValue(request.arguments[0].value)))});
  }

  if (IdIs(id, {"data.scalar.digest", "sb.fn.crypto.digest", "sb.fn.data.scalar.sb_crypto_digest", "digest",
                "sb.scalar.digest",
                "md5", "sha1", "sha224", "sha256", "sha384", "sha512",
                "sb.scalar.md5", "sb.scalar.sha1", "sb.scalar.sha224", "sb.scalar.sha256",
                "sb.scalar.sha384", "sb.scalar.sha512"})) {
    if (request.arguments.empty()) return RefuseFunctionInvalidInput(request, "digest expects value and optional algorithm");
    std::string algorithm = id;
    if (id == "data.scalar.digest" || id == "sb.fn.crypto.digest" ||
        id == "sb.fn.data.scalar.sb_crypto_digest" || id == "digest" || id == "sb.scalar.digest") {
      if (request.arguments.size() != 2) return RefuseFunctionInvalidInput(request, "digest expects value and algorithm");
      if (IsSqlNull(request.arguments[0].value) || IsSqlNull(request.arguments[1].value)) {
        return MakeFunctionSuccess(request, {MakeNullValue("character")});
      }
      algorithm = LowerAscii(ValueAsText(request.arguments[1].value));
    } else if (id == "sb.scalar.md5") {
      algorithm = "md5";
    } else if (id == "sb.scalar.sha1") {
      algorithm = "sha1";
    } else if (id == "sb.scalar.sha224") {
      algorithm = "sha224";
    } else if (id == "sb.scalar.sha256") {
      algorithm = "sha256";
    } else if (id == "sb.scalar.sha384") {
      algorithm = "sha384";
    } else if (id == "sb.scalar.sha512") {
      algorithm = "sha512";
    }
    if (algorithm == "fnv64" || algorithm == "hash64" || algorithm == "checksum64") {
      if (IsSqlNull(request.arguments[0].value)) return MakeFunctionSuccess(request, {MakeNullValue("uint64")});
      return MakeFunctionSuccess(request, {MakeUint64Value("uint64", Fnva64(RawBytesFromValue(request.arguments[0].value)))});
    }
    return RefuseFunctionWithDiagnostic(request,
                                        scratchbird::engine::sblr::SblrStatusCode::dependency_unavailable,
                                        "SB_DIAG_FUNCTION_DEPENDENCY_UNAVAILABLE",
                                        "requested cryptographic digest algorithm requires the configured crypto provider");
  }

  if (id == "data.scalar.random" || id == "sb.fn.data.scalar.sb_random" || id == "sb.scalar.random") {
    if (!request.arguments.empty()) return RefuseFunctionInvalidInput(request, "random expects no arguments");
    std::uint64_t raw = 0;
    if (request.context.sblr_context.deterministic_random_u64_present) {
      raw = request.context.sblr_context.deterministic_random_u64;
    } else {
      std::random_device random;
      raw = (static_cast<std::uint64_t>(random()) << 32) ^ static_cast<std::uint64_t>(random());
    }
    const double normalized = static_cast<double>(raw >> 11) * (1.0 / 9007199254740992.0);
    return MakeFunctionSuccess(request, {MakeReal64Value("real64", normalized)});
  }

  if (id == "data.scalar.crypto_random_bytes" || id == "sb.fn.data.scalar.sb_crypto_random_bytes") {
    if (request.arguments.size() != 1) return RefuseFunctionInvalidInput(request, "crypto_random_bytes expects byte length");
    const auto& length_value = request.arguments[0].value;
    if (IsSqlNull(length_value)) return MakeFunctionSuccess(request, {MakeNullValue("binary")});
    if (!length_value.has_int64_value || length_value.int64_value < 0 || length_value.int64_value > 1048576) {
      return RefuseFunctionInvalidInput(request, "crypto_random_bytes length must be between 0 and 1048576 bytes");
    }
    const auto byte_count = static_cast<std::size_t>(length_value.int64_value);
    std::vector<std::uint8_t> bytes;
    if (!request.context.sblr_context.deterministic_random_bytes_hex.empty()) {
      bytes = BytesFromHexPrefix(request.context.sblr_context.deterministic_random_bytes_hex, byte_count);
      if (bytes.size() != byte_count) {
        return RefuseFunctionInvalidInput(request, "deterministic random byte provider did not contain enough valid hex bytes");
      }
    } else {
      bytes = RandomBytes(byte_count);
    }
    return MakeFunctionSuccess(request, {MakeBinaryValue("binary", std::move(bytes))});
  }

  if (IdIs(id, {"data.scalar.uuid_from_string", "sb.fn.uuid.from_string", "sb.scalar.uuid_from_string", "uuid_from_string"})) {
    if (request.arguments.size() != 1) return RefuseFunctionInvalidInput(request, "uuid_from_string expects canonical UUID text");
    if (IsSqlNull(request.arguments[0].value)) return MakeFunctionSuccess(request, {MakeNullValue("uuid")});
    std::string canonical;
    if (!CanonicalizeUuidText(ValueAsText(request.arguments[0].value), &canonical)) {
      return RefuseFunctionInvalidInput(request, "uuid_from_string expects canonical UUID text");
    }
    return MakeFunctionSuccess(request, {MakeTextValue("uuid", std::move(canonical))});
  }

  if (IdIs(id, {"data.scalar.uuid_to_string", "sb.fn.uuid.to_string", "sb.scalar.uuid_to_string", "uuid_to_string"})) {
    if (request.arguments.size() != 1) return RefuseFunctionInvalidInput(request, "uuid_to_string expects a UUID value");
    if (IsSqlNull(request.arguments[0].value)) return MakeFunctionSuccess(request, {MakeNullValue("character")});
    std::string canonical;
    if (!CanonicalizeUuidText(ValueAsText(request.arguments[0].value), &canonical)) {
      return RefuseFunctionInvalidInput(request, "uuid_to_string expects canonical UUID text");
    }
    return MakeFunctionSuccess(request, {MakeTextValue("character", std::move(canonical))});
  }

  if (id == "sb.uuid.generate_v4") {
    if (!request.arguments.empty()) return RefuseFunctionInvalidInput(request, "uuid_generate_v4 expects no arguments");
    if (!request.context.sblr_context.deterministic_uuid_text.empty()) {
      return MakeFunctionSuccess(request, {MakeTextValue("uuid", request.context.sblr_context.deterministic_uuid_text)});
    }
    return MakeFunctionSuccess(request, {MakeTextValue("uuid", RandomUuidV4Text())});
  }

  if (id == "data.scalar.uuid_generate" || id == "sb.fn.data.scalar.sb_uuid_generate" ||
      id == "sb.uuid.v4") {
    if (request.arguments.size() > 1) return RefuseFunctionInvalidInput(request, "uuid_generate expects optional version");
    if (!request.context.sblr_context.deterministic_uuid_text.empty()) {
      return MakeFunctionSuccess(request, {MakeTextValue("uuid", request.context.sblr_context.deterministic_uuid_text)});
    }
    std::int64_t version = 4;
    if (request.arguments.size() == 1) {
      if (IsSqlNull(request.arguments[0].value)) return MakeFunctionSuccess(request, {MakeNullValue("uuid")});
      if (!request.arguments[0].value.has_int64_value) {
        return RefuseFunctionInvalidInput(request, "uuid_generate version must be an integer");
      }
      version = request.arguments[0].value.int64_value;
    }
    if (version == 4) return MakeFunctionSuccess(request, {MakeTextValue("uuid", RandomUuidV4Text())});
    if (version == 1) return MakeFunctionSuccess(request, {MakeTextValue("uuid", RandomUuidV1Text())});
    if (version == 7) return MakeFunctionSuccess(request, {MakeTextValue("uuid", RandomUuidV7Text())});
    return RefuseFunctionInvalidInput(request, "uuid_generate supports versions 1, 4, and 7 only");
  }

  if (id == "sb.uuid.generate_v1") {
    if (!request.arguments.empty()) return RefuseFunctionInvalidInput(request, "uuid_generate_v1 expects no arguments");
    if (!request.context.sblr_context.deterministic_uuid_text.empty()) {
      return MakeFunctionSuccess(request, {MakeTextValue("uuid", request.context.sblr_context.deterministic_uuid_text)});
    }
    return MakeFunctionSuccess(request, {MakeTextValue("uuid", RandomUuidV1Text())});
  }

  if (id == "sb.uuid.generate_v3" || id == "sb.uuid.generate_v5") {
    if (request.arguments.size() != 2) {
      return RefuseFunctionInvalidInput(request, "uuid_generate_v3/v5 expects namespace UUID and name");
    }
    if (IsSqlNull(request.arguments[0].value) || IsSqlNull(request.arguments[1].value)) {
      return MakeFunctionSuccess(request, {MakeNullValue("uuid")});
    }
    const auto namespace_uuid = scratchbird::core::uuid::ParseUuid(ValueAsText(request.arguments[0].value));
    if (!namespace_uuid.ok()) {
      return RefuseFunctionInvalidInput(request, "uuid_generate_v3/v5 expects canonical namespace UUID text");
    }
    const auto name = ValueAsText(request.arguments[1].value);
    const auto generated = id == "sb.uuid.generate_v3"
                               ? scratchbird::core::uuid::GenerateCompatibilityNameBasedV3(namespace_uuid.value, name)
                               : scratchbird::core::uuid::GenerateCompatibilityNameBasedV5(namespace_uuid.value, name);
    if (!generated.ok()) {
      return RefuseFunctionInvalidInput(request, "uuid_generate_v3/v5 could not generate a compatibility UUID");
    }
    return MakeFunctionSuccess(
        request,
        {MakeTextValue("uuid", scratchbird::core::uuid::UuidToString(generated.value))});
  }

  if (id == "sb.uuid.v1") {
    if (!request.arguments.empty()) return RefuseFunctionInvalidInput(request, "uuid_v1 expects no arguments");
    if (!request.context.sblr_context.deterministic_uuid_text.empty()) {
      return MakeFunctionSuccess(request, {MakeTextValue("uuid", request.context.sblr_context.deterministic_uuid_text)});
    }
    return MakeFunctionSuccess(request, {MakeTextValue("uuid", RandomUuidV1Text())});
  }

  if (id == "sb.uuid.v7") {
    if (!request.arguments.empty()) return RefuseFunctionInvalidInput(request, "uuid_v7 expects no arguments");
    if (!request.context.sblr_context.deterministic_uuid_text.empty()) {
      return MakeFunctionSuccess(request, {MakeTextValue("uuid", request.context.sblr_context.deterministic_uuid_text)});
    }
    return MakeFunctionSuccess(request, {MakeTextValue("uuid", RandomUuidV7Text())});
  }

  if (id == "sb.uuid.nil") {
    if (!request.arguments.empty()) return RefuseFunctionInvalidInput(request, "uuid_nil expects no arguments");
    return MakeFunctionSuccess(request, {MakeTextValue("uuid", "00000000-0000-0000-0000-000000000000")});
  }

  if (id == "sb.uuid.version") {
    if (request.arguments.size() != 1) return RefuseFunctionInvalidInput(request, "uuid_version expects one UUID argument");
    if (IsSqlNull(request.arguments[0].value)) return MakeFunctionSuccess(request, {MakeNullValue("int64")});
    std::string canonical;
    if (!CanonicalizeUuidText(ValueAsText(request.arguments[0].value), &canonical)) {
      return RefuseFunctionInvalidInput(request, "uuid_version expects canonical UUID text");
    }
    const int version = HexValue(canonical[14]);
    if (version < 0) return RefuseFunctionInvalidInput(request, "uuid_version expects canonical UUID text");
    return MakeFunctionSuccess(request, {MakeInt64Value("int64", version)});
  }

  if (id == "sb.uuid.timestamp") {
    if (request.arguments.size() != 1) return RefuseFunctionInvalidInput(request, "uuid_timestamp expects one UUID argument");
    if (IsSqlNull(request.arguments[0].value)) return MakeFunctionSuccess(request, {MakeNullValue("timestamp_tz")});
    std::array<std::uint8_t, 16> bytes{};
    if (!CanonicalUuidBytes(ValueAsText(request.arguments[0].value), &bytes)) {
      return RefuseFunctionInvalidInput(request, "uuid_timestamp expects canonical UUID text");
    }
    const auto timestamp = UuidEmbeddedTimestampText(bytes);
    if (!timestamp.has_value()) {
      return RefuseFunctionInvalidInput(request, "uuid_timestamp supports UUID versions 1 and 7 only");
    }
    return MakeFunctionSuccess(request, {MakeTextValue("timestamp_tz", *timestamp)});
  }

  constexpr std::string_view kPolicyRefusalPrefix = "sb.scalar.refusal_";
  if (id.size() >= kPolicyRefusalPrefix.size() &&
      std::string_view(id).substr(0, kPolicyRefusalPrefix.size()) == kPolicyRefusalPrefix) {
    return RefuseFunctionWithDiagnostic(
        request,
        scratchbird::engine::sblr::SblrStatusCode::unsupported_feature,
        "SB_DIAG_FUNCTION_RUNTIME_REFUSAL",
        "public SBsql surface is refused by fixed policy before engine side effects");
  }

  return RefuseFunctionWithDiagnostic(request,
                                      scratchbird::engine::sblr::SblrStatusCode::unsupported_feature,
                                      "SB_DIAG_FUNCTION_HANDLER_MISSING",
                                      "data.scalar handler is not registered");
}

}  // namespace scratchbird::engine::functions
