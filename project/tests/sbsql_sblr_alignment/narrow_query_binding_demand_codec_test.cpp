// Copyright (c) 2026 ScratchBird Software Inc.
//
// This Source Code Form is subject to the terms of the Mozilla Public
// License, v. 2.0. If a copy of the MPL was not distributed with this
// file, You can obtain one at https://mozilla.org/MPL/2.0/.
//
// SPDX-License-Identifier: MPL-2.0

#include "wire/narrow_query_binding_demand_codec.hpp"

#include <algorithm>
#include <cstdlib>
#include <iostream>
#include <limits>
#include <stdexcept>
#include <string>
#include <utility>
#include <vector>

namespace {

using scratchbird::core::platform::LoadLittle16;
using scratchbird::core::platform::LoadLittle32;
using scratchbird::core::platform::LoadLittle64;
using scratchbird::core::platform::StoreLittle16;
using scratchbird::core::platform::StoreLittle32;
using scratchbird::core::platform::StoreLittle64;
using scratchbird::wire::NarrowQueryBindingDemand;
using scratchbird::wire::NarrowQueryBindingDemandError;
using scratchbird::wire::NarrowQueryBindingDemandErrorCode;
using scratchbird::wire::NarrowQueryBindingDemandValidationContext;
using scratchbird::wire::NarrowQueryDirection;
using scratchbird::wire::NarrowQueryNullPlacement;
using scratchbird::wire::NarrowQueryOrderingDemand;
using scratchbird::wire::NarrowQueryOutputDemand;
using scratchbird::wire::NarrowQueryProfile;
using scratchbird::wire::NarrowQuerySourceDemand;
using scratchbird::wire::NarrowQueryUuid;
using scratchbird::wire::byte;
using scratchbird::wire::u16;
using scratchbird::wire::u32;
using scratchbird::wire::u64;
using scratchbird::wire::kNarrowQueryBindingDemandHeaderBytes;
using scratchbird::wire::kNarrowQueryBindingDemandMaximumCarrierBytes;
using scratchbird::wire::kNarrowQueryBindingDemandOutputPrefixBytes;
using scratchbird::wire::kNarrowQueryBindingDemandOrderingPrefixBytes;
using scratchbird::wire::kNarrowQueryBindingDemandResultBoundBytes;
using scratchbird::wire::kNarrowQueryBindingDemandSourcePrefixBytes;

constexpr u64 kScanBytes = 64ull * 1024ull * 1024ull;

void Require(bool condition, const std::string& message) {
  if (!condition) {
    throw std::runtime_error(message);
  }
}

NarrowQueryUuid Uuid(unsigned seed) {
  NarrowQueryUuid value{};
  for (std::size_t index = 0; index < value.size(); ++index) {
    value[index] = static_cast<byte>((seed + index * 17u) & 0xffu);
  }
  value[0] = static_cast<byte>((seed & 0x7fu) + 1u);
  return value;
}

NarrowQuerySourceDemand Source(u32 ordinal,
                               bool explicit_alias = false,
                               std::string alias = {}) {
  NarrowQuerySourceDemand source;
  source.source_ordinal = ordinal;
  source.relation_object_hint_present = true;
  source.relation_object_uuid_hint = Uuid(100u);
  source.explicit_alias = explicit_alias;
  source.alias_spelling = std::move(alias);
  return source;
}

NarrowQueryOutputDemand Output(u32 ordinal,
                               u32 source_ordinal,
                               std::string column,
                               std::string output_name = {}) {
  NarrowQueryOutputDemand output;
  output.output_ordinal = ordinal;
  output.source_ordinal = source_ordinal;
  output.source_column_spelling = std::move(column);
  output.output_name_present = !output_name.empty();
  output.output_name_spelling = std::move(output_name);
  return output;
}

NarrowQueryOrderingDemand Ordering(
    u32 ordinal,
    u32 source_ordinal,
    std::string column,
    NarrowQueryDirection direction = NarrowQueryDirection::ascending,
    NarrowQueryNullPlacement null_placement =
        NarrowQueryNullPlacement::last) {
  NarrowQueryOrderingDemand term;
  term.term_ordinal = ordinal;
  term.source_ordinal = source_ordinal;
  term.source_column_spelling = std::move(column);
  term.direction = direction;
  term.null_placement = null_placement;
  return term;
}

NarrowQueryBindingDemand OrderedDemand() {
  NarrowQueryBindingDemand demand;
  demand.statement_receipt_uuid = Uuid(1u);
  demand.requested_profile = NarrowQueryProfile::ordered_projection;
  demand.maximum_mga_relation_decoded_bytes_per_pass = kScanBytes;
  demand.row_limit_present = true;
  demand.row_limit = 25;
  demand.row_offset_present = true;
  demand.row_offset = 7;
  demand.sources.push_back(Source(0));
  demand.outputs.push_back(Output(0, 0, "id", "selected_id"));
  demand.outputs.push_back(Output(1, 0, "caf\xc3\xa9"));
  demand.ordering_terms.push_back(Ordering(0, 0, "id"));
  demand.ordering_terms.push_back(Ordering(
      1, 0, "caf\xc3\xa9", NarrowQueryDirection::descending,
      NarrowQueryNullPlacement::first));
  return demand;
}

NarrowQueryBindingDemand ProjectionOccurrenceDemand() {
  NarrowQueryBindingDemand demand;
  demand.statement_receipt_uuid = Uuid(2u);
  demand.requested_profile = NarrowQueryProfile::projection_occurrence;
  demand.maximum_mga_relation_decoded_bytes_per_pass = kScanBytes;
  demand.sources.push_back(Source(0));
  demand.outputs.push_back(Output(0, 0, "payload", "left_payload"));
  demand.outputs.push_back(Output(1, 0, "payload", "right_payload"));
  return demand;
}

NarrowQueryBindingDemand SelfJoinDemand() {
  NarrowQueryBindingDemand demand;
  demand.statement_receipt_uuid = Uuid(3u);
  demand.requested_profile = NarrowQueryProfile::alias_distinct_self_join;
  demand.maximum_mga_relation_decoded_bytes_per_pass = kScanBytes;
  demand.sources.push_back(Source(0, true, "parent_row"));
  demand.sources.push_back(Source(1, true, "child_row"));
  demand.outputs.push_back(Output(0, 0, "id", "parent_id"));
  demand.outputs.push_back(Output(1, 1, "id", "child_id"));
  return demand;
}

NarrowQueryBindingDemandValidationContext Context(
    const NarrowQueryBindingDemand& demand) {
  NarrowQueryBindingDemandValidationContext context;
  context.authenticated_statement_receipt_uuid =
      demand.statement_receipt_uuid;
  context.maximum_mga_relation_decoded_bytes_per_pass =
      demand.maximum_mga_relation_decoded_bytes_per_pass;
  return context;
}

std::vector<byte> Encode(const NarrowQueryBindingDemand& demand) {
  std::vector<byte> encoded;
  NarrowQueryBindingDemandError error;
  Require(scratchbird::wire::EncodeNarrowQueryBindingDemand(
              demand, &encoded, &error),
          std::string("encode failed: ") +
              scratchbird::wire::NarrowQueryBindingDemandErrorCodeName(
                  error.code) +
              " field=" + error.field + " detail=" + error.detail);
  return encoded;
}

NarrowQueryBindingDemand Decode(
    const std::vector<byte>& encoded,
    const NarrowQueryBindingDemandValidationContext& context) {
  NarrowQueryBindingDemand decoded;
  NarrowQueryBindingDemandError error;
  Require(scratchbird::wire::DecodeAndValidateNarrowQueryBindingDemand(
              encoded, context, &decoded, &error),
          std::string("decode failed: ") +
              scratchbird::wire::NarrowQueryBindingDemandErrorCodeName(
                  error.code) +
              " field=" + error.field + " detail=" + error.detail);
  return decoded;
}

void ExpectEncodeError(const NarrowQueryBindingDemand& demand,
                       NarrowQueryBindingDemandErrorCode expected,
                       const std::string& field) {
  std::vector<byte> output{0xacu};
  NarrowQueryBindingDemandError error;
  Require(!scratchbird::wire::EncodeNarrowQueryBindingDemand(
              demand, &output, &error),
          "invalid demand unexpectedly encoded");
  Require(error.code == expected,
          std::string("expected encode error ") +
              scratchbird::wire::NarrowQueryBindingDemandErrorCodeName(
                  expected) +
              " got " +
              scratchbird::wire::NarrowQueryBindingDemandErrorCodeName(
                  error.code));
  Require(error.field == field,
          "encode diagnostic did not identify the exact field");
  Require(!error.diagnostic_code.empty() && !error.detail.empty(),
          "encode diagnostic metadata is incomplete");
  Require(output == std::vector<byte>{0xacu},
          "encode refusal changed the caller output");
}

void ExpectDecodeError(
    const std::vector<byte>& encoded,
    const NarrowQueryBindingDemandValidationContext& context,
    NarrowQueryBindingDemandErrorCode expected,
    const std::string& field) {
  NarrowQueryBindingDemand sentinel;
  sentinel.statement_receipt_uuid = Uuid(240u);
  sentinel.exact_bytes = {0xbdu};
  NarrowQueryBindingDemandError error;
  Require(!scratchbird::wire::DecodeAndValidateNarrowQueryBindingDemand(
              encoded, context, &sentinel, &error),
          "invalid carrier unexpectedly decoded");
  Require(error.code == expected,
          std::string("expected decode error ") +
              scratchbird::wire::NarrowQueryBindingDemandErrorCodeName(
                  expected) +
              " got " +
              scratchbird::wire::NarrowQueryBindingDemandErrorCodeName(
                  error.code));
  Require(error.field == field,
          "decode diagnostic did not identify the exact field");
  Require(!error.diagnostic_code.empty() && !error.detail.empty(),
          "decode diagnostic metadata is incomplete");
  Require(sentinel.statement_receipt_uuid == Uuid(240u) &&
              sentinel.exact_bytes == std::vector<byte>{0xbdu},
          "decode refusal changed the caller destination");
}

std::size_t SourceBegin(const std::vector<byte>& encoded) {
  return kNarrowQueryBindingDemandHeaderBytes + sizeof(u64) +
         LoadLittle32(encoded.data() + 64u);
}

std::size_t OutputBegin(const std::vector<byte>& encoded) {
  return SourceBegin(encoded) + LoadLittle32(encoded.data() + 36u);
}

std::size_t OrderingBegin(const std::vector<byte>& encoded) {
  return OutputBegin(encoded) + LoadLittle32(encoded.data() + 40u);
}

void TestRoundTripAndLayout() {
  for (auto demand : {OrderedDemand(), ProjectionOccurrenceDemand(),
                      SelfJoinDemand()}) {
    const auto encoded = Encode(demand);
    Require(encoded.size() <= kNarrowQueryBindingDemandMaximumCarrierBytes,
            "valid demand exceeded carrier ceiling");
    Require(std::string(encoded.begin(), encoded.begin() + 8u) ==
                "SBQNDR01",
            "carrier magic is not exact");
    Require(LoadLittle16(encoded.data() + 8u) == 1 &&
                LoadLittle16(encoded.data() + 10u) ==
                    kNarrowQueryBindingDemandHeaderBytes &&
                LoadLittle64(encoded.data() + 16u) == encoded.size(),
            "fixed header version or extents are not exact");
    Require(LoadLittle16(encoded.data() + 24u) ==
                    static_cast<u16>(demand.requested_profile) &&
                LoadLittle16(encoded.data() + 26u) == demand.sources.size() &&
                LoadLittle32(encoded.data() + 28u) == demand.outputs.size() &&
                LoadLittle32(encoded.data() + 32u) ==
                    demand.ordering_terms.size(),
            "header profile or vector counts are not exact");
    Require(std::equal(demand.statement_receipt_uuid.begin(),
                       demand.statement_receipt_uuid.end(),
                       encoded.begin() + 48u),
            "header did not carry the exact statement receipt");
    Require(LoadLittle32(encoded.data() + 68u) == 0,
            "header reserved bytes are nonzero");
    Require(LoadLittle64(encoded.data() +
                         kNarrowQueryBindingDemandHeaderBytes) ==
                demand.maximum_mga_relation_decoded_bytes_per_pass,
            "mandatory scan-byte statement policy was not copied at offset 72");

    auto decoded = Decode(encoded, Context(demand));
    Require(decoded.statement_receipt_uuid == demand.statement_receipt_uuid &&
                decoded.requested_profile == demand.requested_profile &&
                decoded.maximum_mga_relation_decoded_bytes_per_pass ==
                    demand.maximum_mga_relation_decoded_bytes_per_pass &&
                decoded.row_limit_present == demand.row_limit_present &&
                decoded.row_limit == demand.row_limit &&
                decoded.row_offset_present == demand.row_offset_present &&
                decoded.row_offset == demand.row_offset &&
                decoded.sources.size() == demand.sources.size() &&
                decoded.outputs.size() == demand.outputs.size() &&
                decoded.ordering_terms.size() ==
                    demand.ordering_terms.size(),
            "decoded demand did not preserve header semantics");
    Require(decoded.exact_bytes == encoded,
            "decode did not retain exact canonical carrier bytes");
    const auto reencoded = Encode(decoded);
    Require(reencoded == encoded,
            "decoded demand did not re-encode byte exactly");
  }
}

void TestIndependentResultBoundPresence() {
  for (unsigned mask = 0; mask < 4; ++mask) {
    auto demand = OrderedDemand();
    demand.row_limit_present = (mask & 1u) != 0;
    demand.row_limit = demand.row_limit_present ? 0u : 0u;
    demand.row_offset_present = (mask & 2u) != 0;
    demand.row_offset = demand.row_offset_present ? 91u : 0u;
    const auto encoded = Encode(demand);
    Require(LoadLittle32(encoded.data() + 12u) == mask,
            "independent bound flags were not encoded exactly");
    Require(LoadLittle32(encoded.data() + 64u) ==
                (mask == 0 ? 0u
                           : kNarrowQueryBindingDemandResultBoundBytes),
            "conditional bound extent did not match presence");
    const auto decoded = Decode(encoded, Context(demand));
    Require(decoded.row_limit_present == demand.row_limit_present &&
                decoded.row_limit == demand.row_limit &&
                decoded.row_offset_present == demand.row_offset_present &&
                decoded.row_offset == demand.row_offset,
            "independent result bounds did not round trip");
  }

  auto absent_nonzero = OrderedDemand();
  absent_nonzero.row_limit_present = false;
  absent_nonzero.row_limit = 1;
  ExpectEncodeError(absent_nonzero,
                    NarrowQueryBindingDemandErrorCode::result_bound_invalid,
                    "post_projection_result_bound");

  auto overflow = OrderedDemand();
  overflow.row_limit = std::numeric_limits<u64>::max();
  overflow.row_offset = 1;
  ExpectEncodeError(overflow,
                    NarrowQueryBindingDemandErrorCode::result_bound_invalid,
                    "post_projection_result_bound");

  auto demand = OrderedDemand();
  demand.row_limit_present = false;
  demand.row_limit = 0;
  const auto context = Context(demand);
  auto encoded = Encode(demand);
  StoreLittle64(encoded.data() + kNarrowQueryBindingDemandHeaderBytes +
                    sizeof(u64),
                1);
  ExpectDecodeError(encoded, context,
                    NarrowQueryBindingDemandErrorCode::result_bound_invalid,
                    "post_projection_result_bound");
}

void TestMandatoryScanByteReceiptPolicy() {
  auto demand = OrderedDemand();
  const auto canonical = Encode(demand);
  const auto context = Context(demand);

  Require(LoadLittle64(canonical.data() + 72u) == kScanBytes,
          "SBQNDR01 mandatory scan-byte value is not at offset 72");
  Require(SourceBegin(canonical) == 96u,
          "SBQNDR01 vectors do not begin at 96 with LIMIT/OFFSET present");

  auto unbounded = ProjectionOccurrenceDemand();
  const auto unbounded_bytes = Encode(unbounded);
  Require(SourceBegin(unbounded_bytes) == 80u,
          "SBQNDR01 vectors do not begin at 80 without LIMIT/OFFSET");

  auto zero = demand;
  zero.maximum_mga_relation_decoded_bytes_per_pass = 0;
  ExpectEncodeError(zero,
                    NarrowQueryBindingDemandErrorCode::resource_limit_exceeded,
                    "maximum_mga_relation_decoded_bytes_per_pass");

  auto below_minimum = demand;
  below_minimum.maximum_mga_relation_decoded_bytes_per_pass =
      scratchbird::wire::kNarrowQueryMinimumMgaRelationDecodedBytesPerPass - 1;
  ExpectEncodeError(below_minimum,
                    NarrowQueryBindingDemandErrorCode::resource_limit_exceeded,
                    "maximum_mga_relation_decoded_bytes_per_pass");

  auto above_maximum = demand;
  above_maximum.maximum_mga_relation_decoded_bytes_per_pass =
      scratchbird::wire::kNarrowQueryMaximumMgaRelationDecodedBytesPerPass + 1;
  ExpectEncodeError(above_maximum,
                    NarrowQueryBindingDemandErrorCode::resource_limit_exceeded,
                    "maximum_mga_relation_decoded_bytes_per_pass");

  auto wrong_context = context;
  wrong_context.maximum_mga_relation_decoded_bytes_per_pass = kScanBytes * 2u;
  ExpectDecodeError(canonical, wrong_context,
                    NarrowQueryBindingDemandErrorCode::resource_limit_exceeded,
                    "maximum_mga_relation_decoded_bytes_per_pass");

  auto missing_context_value = context;
  missing_context_value.maximum_mga_relation_decoded_bytes_per_pass = 0;
  ExpectDecodeError(canonical, missing_context_value,
                    NarrowQueryBindingDemandErrorCode::invalid_argument,
                    "context.maximum_mga_relation_decoded_bytes_per_pass");

  auto truncated = canonical;
  truncated.resize(79u);
  StoreLittle64(truncated.data() + 16u, truncated.size());
  ExpectDecodeError(truncated, context,
                    NarrowQueryBindingDemandErrorCode::extent_invalid,
                    "carrier_extent");

  // The scan-byte ceiling is not a LIMIT, result-row, or carrier-size alias.
  auto independent = demand;
  independent.maximum_mga_relation_decoded_bytes_per_pass =
      scratchbird::wire::kNarrowQueryMinimumMgaRelationDecodedBytesPerPass;
  independent.row_limit = 1;
  const auto independent_bytes = Encode(independent);
  const auto independent_decoded = Decode(independent_bytes,
                                           Context(independent));
  Require(independent_decoded.maximum_mga_relation_decoded_bytes_per_pass ==
              64ull * 1024ull &&
              independent_decoded.row_limit == 1,
          "scan-byte statement policy aliased LIMIT or a carrier bound");
}

void TestReceiptAndHeaderRefusals() {
  const auto demand = OrderedDemand();
  const auto context = Context(demand);
  const auto canonical = Encode(demand);

  auto wrong_context = context;
  wrong_context.authenticated_statement_receipt_uuid = Uuid(222u);
  auto malformed_after_receipt = canonical;
  StoreLittle32(malformed_after_receipt.data() + OutputBegin(canonical),
                std::numeric_limits<u32>::max());
  ExpectDecodeError(malformed_after_receipt, wrong_context,
                    NarrowQueryBindingDemandErrorCode::receipt_mismatch,
                    "statement_receipt_uuid");

  auto invalid_context = context;
  invalid_context.authenticated_statement_receipt_uuid = {};
  ExpectDecodeError(canonical, invalid_context,
                    NarrowQueryBindingDemandErrorCode::receipt_invalid,
                    "context.authenticated_statement_receipt_uuid");
  invalid_context = context;
  invalid_context.maximum_total_bytes = canonical.size() - 1u;
  ExpectDecodeError(canonical, invalid_context,
                    NarrowQueryBindingDemandErrorCode::resource_limit_exceeded,
                    "carrier_extent");
  invalid_context.maximum_total_bytes =
      kNarrowQueryBindingDemandMaximumCarrierBytes + 1u;
  ExpectDecodeError(canonical, invalid_context,
                    NarrowQueryBindingDemandErrorCode::invalid_argument,
                    "context.maximum_total_bytes");

  auto bytes = canonical;
  bytes[0] = 'X';
  ExpectDecodeError(bytes, context,
                    NarrowQueryBindingDemandErrorCode::magic_invalid,
                    "magic");
  bytes = canonical;
  StoreLittle16(bytes.data() + 8u, 2);
  ExpectDecodeError(bytes, context,
                    NarrowQueryBindingDemandErrorCode::version_invalid,
                    "layout_version");
  bytes = canonical;
  StoreLittle16(bytes.data() + 10u, 71);
  ExpectDecodeError(bytes, context,
                    NarrowQueryBindingDemandErrorCode::extent_invalid,
                    "header_or_total_bytes");
  bytes = canonical;
  StoreLittle64(bytes.data() + 16u, bytes.size() + 1u);
  ExpectDecodeError(bytes, context,
                    NarrowQueryBindingDemandErrorCode::extent_invalid,
                    "header_or_total_bytes");
  bytes = canonical;
  StoreLittle32(bytes.data() + 12u,
                LoadLittle32(bytes.data() + 12u) | (1u << 31u));
  ExpectDecodeError(bytes, context,
                    NarrowQueryBindingDemandErrorCode::reserved_invalid,
                    "header_reserved");
  bytes = canonical;
  StoreLittle32(bytes.data() + 68u, 1);
  ExpectDecodeError(bytes, context,
                    NarrowQueryBindingDemandErrorCode::reserved_invalid,
                    "header_reserved");
  bytes = canonical;
  StoreLittle16(bytes.data() + 24u, 99);
  ExpectDecodeError(bytes, context,
                    NarrowQueryBindingDemandErrorCode::profile_invalid,
                    "requested_profile");
  bytes = canonical;
  StoreLittle16(bytes.data() + 26u, 0);
  ExpectDecodeError(bytes, context,
                    NarrowQueryBindingDemandErrorCode::count_invalid,
                    "vector_counts");
  bytes = canonical;
  StoreLittle32(bytes.data() + 36u,
                std::numeric_limits<u32>::max());
  ExpectDecodeError(bytes, context,
                    NarrowQueryBindingDemandErrorCode::extent_invalid,
                    "vector_extents");
  bytes = canonical;
  StoreLittle32(bytes.data() + 64u, 0);
  ExpectDecodeError(bytes, context,
                    NarrowQueryBindingDemandErrorCode::result_bound_invalid,
                    "post_projection_result_bound.extent");
}

void TestSourceRecordRefusals() {
  auto demand = OrderedDemand();
  auto context = Context(demand);
  const auto canonical = Encode(demand);
  const auto source = SourceBegin(canonical);

  auto bytes = canonical;
  StoreLittle32(bytes.data() + source, std::numeric_limits<u32>::max());
  ExpectDecodeError(bytes, context,
                    NarrowQueryBindingDemandErrorCode::source_extent_invalid,
                    "sources.record_bytes");
  bytes = canonical;
  StoreLittle32(bytes.data() + source + 4u, 1);
  ExpectDecodeError(bytes, context,
                    NarrowQueryBindingDemandErrorCode::source_ordinal_invalid,
                    "sources.source_ordinal");
  bytes = canonical;
  StoreLittle32(bytes.data() + source + 8u,
                LoadLittle32(bytes.data() + source + 8u) | (1u << 30u));
  ExpectDecodeError(bytes, context,
                    NarrowQueryBindingDemandErrorCode::flag_invalid,
                    "sources.flags");
  bytes = canonical;
  StoreLittle32(bytes.data() + source + 8u,
                LoadLittle32(bytes.data() + source + 8u) & ~1u);
  ExpectDecodeError(
      bytes, context,
      NarrowQueryBindingDemandErrorCode::source_relation_hint_invalid,
      "sources.relation_object_uuid_hint");

  demand.sources[0].relation_object_hint_present = false;
  ExpectEncodeError(
      demand,
      NarrowQueryBindingDemandErrorCode::source_relation_hint_invalid,
      "sources.relation_object_uuid_hint");
  demand = OrderedDemand();
  demand.sources[0].explicit_alias = false;
  demand.sources[0].alias_spelling = "present_without_flag";
  ExpectEncodeError(demand,
                    NarrowQueryBindingDemandErrorCode::source_alias_invalid,
                    "sources.alias_spelling");
  demand = OrderedDemand();
  demand.sources[0].explicit_alias = true;
  demand.sources[0].alias_spelling = std::string("\xc0\x80", 2);
  ExpectEncodeError(demand,
                    NarrowQueryBindingDemandErrorCode::source_alias_invalid,
                    "sources.alias_spelling");
}

void TestOutputAndOrderingRecordRefusals() {
  auto demand = OrderedDemand();
  auto context = Context(demand);
  const auto canonical = Encode(demand);
  const auto output = OutputBegin(canonical);
  const auto ordering = OrderingBegin(canonical);

  auto bytes = canonical;
  StoreLittle32(bytes.data() + output, std::numeric_limits<u32>::max());
  ExpectDecodeError(bytes, context,
                    NarrowQueryBindingDemandErrorCode::output_extent_invalid,
                    "outputs.record_bytes");
  bytes = canonical;
  StoreLittle32(bytes.data() + output + 4u, 1);
  ExpectDecodeError(bytes, context,
                    NarrowQueryBindingDemandErrorCode::output_ordinal_invalid,
                    "outputs.output_ordinal");
  bytes = canonical;
  StoreLittle32(bytes.data() + output + 8u, 9);
  ExpectDecodeError(bytes, context,
                    NarrowQueryBindingDemandErrorCode::output_source_invalid,
                    "outputs.source_ordinal");
  bytes = canonical;
  StoreLittle32(bytes.data() + output + 12u, 0x80000000u);
  ExpectDecodeError(bytes, context,
                    NarrowQueryBindingDemandErrorCode::flag_invalid,
                    "outputs.flags");
  bytes = canonical;
  const auto output_column = output + kNarrowQueryBindingDemandOutputPrefixBytes;
  bytes[output_column] = 0;
  ExpectDecodeError(
      bytes, context,
      NarrowQueryBindingDemandErrorCode::output_column_spelling_invalid,
      "outputs.source_column_spelling");
  bytes = canonical;
  StoreLittle32(bytes.data() + output + 12u, 0);
  ExpectDecodeError(bytes, context,
                    NarrowQueryBindingDemandErrorCode::output_name_invalid,
                    "outputs.output_name_spelling");

  bytes = canonical;
  StoreLittle32(bytes.data() + ordering,
                std::numeric_limits<u32>::max());
  ExpectDecodeError(
      bytes, context,
      NarrowQueryBindingDemandErrorCode::ordering_extent_invalid,
      "ordering_terms.record_bytes");
  bytes = canonical;
  StoreLittle32(bytes.data() + ordering + 4u, 1);
  ExpectDecodeError(
      bytes, context,
      NarrowQueryBindingDemandErrorCode::ordering_ordinal_invalid,
      "ordering_terms.term_ordinal");
  bytes = canonical;
  StoreLittle32(bytes.data() + ordering + 8u, 9);
  ExpectDecodeError(bytes, context,
                    NarrowQueryBindingDemandErrorCode::ordering_source_invalid,
                    "ordering_terms.source_ordinal");
  bytes = canonical;
  bytes[ordering + 16u] = 9;
  ExpectDecodeError(
      bytes, context,
      NarrowQueryBindingDemandErrorCode::ordering_direction_invalid,
      "ordering_terms.direction");
  bytes = canonical;
  bytes[ordering + 17u] = 9;
  ExpectDecodeError(
      bytes, context,
      NarrowQueryBindingDemandErrorCode::ordering_null_placement_invalid,
      "ordering_terms.null_placement");
  bytes = canonical;
  StoreLittle16(bytes.data() + ordering + 18u, 1);
  ExpectDecodeError(bytes, context,
                    NarrowQueryBindingDemandErrorCode::reserved_invalid,
                    "ordering_terms.reserved");
  bytes = canonical;
  bytes[ordering + kNarrowQueryBindingDemandOrderingPrefixBytes] = 0;
  ExpectDecodeError(
      bytes, context,
      NarrowQueryBindingDemandErrorCode::ordering_column_spelling_invalid,
      "ordering_terms.source_column_spelling");
}

void TestProfileShapeAndPresenceRefusals() {
  auto demand = OrderedDemand();
  demand.outputs[1].source_column_spelling =
      demand.outputs[0].source_column_spelling;
  ExpectEncodeError(demand,
                    NarrowQueryBindingDemandErrorCode::profile_shape_invalid,
                    "ordered_projection_shape");
  demand = OrderedDemand();
  demand.ordering_terms.pop_back();
  ExpectEncodeError(demand,
                    NarrowQueryBindingDemandErrorCode::profile_shape_invalid,
                    "ordered_projection_shape");
  demand = OrderedDemand();
  demand.outputs[0].output_name_present = false;
  ExpectEncodeError(demand,
                    NarrowQueryBindingDemandErrorCode::output_name_invalid,
                    "outputs.output_name_spelling");
  demand = OrderedDemand();
  demand.outputs[0].source_column_spelling.clear();
  ExpectEncodeError(
      demand,
      NarrowQueryBindingDemandErrorCode::output_column_spelling_invalid,
      "outputs.source_column_spelling");
  demand = OrderedDemand();
  demand.ordering_terms[0].source_ordinal = 1;
  ExpectEncodeError(demand,
                    NarrowQueryBindingDemandErrorCode::ordering_source_invalid,
                    "ordering_terms.source_ordinal");

  demand = ProjectionOccurrenceDemand();
  demand.outputs[1].source_column_spelling = "different";
  ExpectEncodeError(demand,
                    NarrowQueryBindingDemandErrorCode::profile_shape_invalid,
                    "projection_occurrence_shape");
  demand = ProjectionOccurrenceDemand();
  demand.ordering_terms.push_back(Ordering(0, 0, "payload"));
  ExpectEncodeError(demand,
                    NarrowQueryBindingDemandErrorCode::profile_shape_invalid,
                    "projection_occurrence_shape");

  demand = SelfJoinDemand();
  demand.sources[1].alias_spelling = demand.sources[0].alias_spelling;
  ExpectEncodeError(demand,
                    NarrowQueryBindingDemandErrorCode::profile_shape_invalid,
                    "alias_distinct_self_join_aliases");
  demand = SelfJoinDemand();
  demand.sources[1].explicit_alias = false;
  demand.sources[1].alias_spelling.clear();
  ExpectEncodeError(demand,
                    NarrowQueryBindingDemandErrorCode::profile_shape_invalid,
                    "alias_distinct_self_join_aliases");
  demand = SelfJoinDemand();
  demand.outputs.erase(demand.outputs.begin() + 1);
  ExpectEncodeError(
      demand, NarrowQueryBindingDemandErrorCode::profile_shape_invalid,
      "alias_distinct_self_join_output_coverage");
  demand = SelfJoinDemand();
  demand.ordering_terms.push_back(Ordering(0, 0, "id"));
  ExpectEncodeError(demand,
                    NarrowQueryBindingDemandErrorCode::profile_shape_invalid,
                    "alias_distinct_self_join_shape");
}

void TestExactBytesAndAllocationPreflight() {
  auto demand = OrderedDemand();
  const auto canonical = Encode(demand);
  auto decoded = Decode(canonical, Context(demand));
  decoded.outputs[0].output_name_spelling = "changed_name";
  ExpectEncodeError(decoded,
                    NarrowQueryBindingDemandErrorCode::exact_bytes_mismatch,
                    "exact_bytes");
  decoded.exact_bytes.clear();
  const auto changed = Encode(decoded);
  Require(changed != canonical,
          "clearing exact evidence did not permit a new canonical demand");

  auto malformed = canonical;
  const auto source = SourceBegin(malformed);
  StoreLittle32(malformed.data() + source,
                std::numeric_limits<u32>::max());
  StoreLittle32(malformed.data() + source + 12u,
                std::numeric_limits<u32>::max());
  ExpectDecodeError(
      malformed, Context(demand),
      NarrowQueryBindingDemandErrorCode::source_extent_invalid,
      "sources.record_bytes");

  malformed = canonical;
  const auto output = OutputBegin(malformed);
  StoreLittle32(malformed.data() + output + 16u,
                std::numeric_limits<u32>::max());
  StoreLittle32(malformed.data() + output + 20u,
                std::numeric_limits<u32>::max());
  ExpectDecodeError(
      malformed, Context(demand),
      NarrowQueryBindingDemandErrorCode::output_extent_invalid,
      "outputs.record_bytes");

  Require(std::string(
              scratchbird::wire::NarrowQueryBindingDemandErrorCodeName(
                  NarrowQueryBindingDemandErrorCode::exact_bytes_mismatch)) ==
              "exact_bytes_mismatch",
          "stable error-code name mapping is missing");
}

}  // namespace

int main() {
  try {
    TestRoundTripAndLayout();
    TestMandatoryScanByteReceiptPolicy();
    TestIndependentResultBoundPresence();
    TestReceiptAndHeaderRefusals();
    TestSourceRecordRefusals();
    TestOutputAndOrderingRecordRefusals();
    TestProfileShapeAndPresenceRefusals();
    TestExactBytesAndAllocationPreflight();
    std::cout << "narrow query binding demand codec: PASS\n";
    return EXIT_SUCCESS;
  } catch (const std::exception& exception) {
    std::cerr << "narrow query binding demand codec: FAIL: "
              << exception.what() << '\n';
    return EXIT_FAILURE;
  }
}
