// Copyright (c) 2026 ScratchBird Software Inc.
// SPDX-License-Identifier: MPL-2.0

#include "parsers/sbsql_worker/wire/sbsql_test_wire.hpp"

#include <cstdlib>
#include <iostream>

using scratchbird::parser::sbsql::CanonicalizePreparedParameterWireValue;
using scratchbird::parser::sbsql::PreparedParameterPayloadEncoding;
using scratchbird::parser::sbsql::PreparedParameterWireValue;

namespace {
constexpr std::string_view kDescriptor =
    "019d0000-0000-7000-8000-00000000d711";
constexpr std::string_view kType =
    "019d0000-0000-7000-8000-00000000d712";
void Require(bool value, const char* message) {
  if (!value) { std::cerr << message << '\n'; std::exit(EXIT_FAILURE); }
}
}

int main() {
  PreparedParameterWireValue binary;
  binary.encoding = PreparedParameterPayloadEncoding::binary;
  binary.raw_bytes = {1, 0, 0, 0, 0, 0, 0, 0};
  binary.public_type_metadata = 20;
  auto value = CanonicalizePreparedParameterWireValue(
      binary, kDescriptor, kType, false);
  Require(value.accepted && !value.is_null &&
              value.canonical_bytes == binary.raw_bytes,
          "binary int64 carrier was not preserved exactly");

  PreparedParameterWireValue text;
  text.encoding = PreparedParameterPayloadEncoding::utf8_text;
  text.raw_bytes = {'-', '2'};
  value = CanonicalizePreparedParameterWireValue(text, kDescriptor, kType, false);
  Require(value.accepted && value.canonical_bytes.size() == 8 &&
              value.canonical_bytes[0] == 0xfe &&
              value.canonical_bytes[7] == 0xff,
          "canonical text int64 was not encoded as signed LE");

  PreparedParameterWireValue null_value;
  null_value.is_null = true;
  value = CanonicalizePreparedParameterWireValue(
      null_value, kDescriptor, kType, true);
  Require(value.accepted && value.is_null && value.canonical_bytes.empty(),
          "nullable value state was not preserved");
  value = CanonicalizePreparedParameterWireValue(
      null_value, kDescriptor, kType, false);
  Require(!value.accepted && value.diagnostic_code == "SBLR.PARAMETER.UNBOUND",
          "required null value was not refused");

  binary.raw_bytes.resize(7);
  value = CanonicalizePreparedParameterWireValue(
      binary, kDescriptor, kType, false);
  Require(!value.accepted &&
              value.diagnostic_code == "DATATYPE.CONVERSION_FAILED",
          "malformed binary int64 was not refused");
  text.raw_bytes = {'0', '1'};
  value = CanonicalizePreparedParameterWireValue(text, kDescriptor, kType, false);
  Require(!value.accepted &&
              value.diagnostic_code == "DATATYPE.CONVERSION_FAILED",
          "noncanonical text int64 was not refused");
  value = CanonicalizePreparedParameterWireValue(
      binary, "019d0000-0000-7000-8000-00000000d799", kType, false);
  Require(!value.accepted &&
              value.diagnostic_code == "DATATYPE.DESCRIPTOR_INVALID",
          "unnegotiated datatype identity was not refused");
  return EXIT_SUCCESS;
}
