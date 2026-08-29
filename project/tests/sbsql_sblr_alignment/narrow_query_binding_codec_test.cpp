// Copyright (c) 2026 ScratchBird Software Inc.
// SPDX-License-Identifier: MPL-2.0

#include "wire/narrow_query_binding_codec.hpp"

#include "runtime_platform.hpp"

#include <algorithm>
#include <cstdlib>
#include <iostream>
#include <limits>
#include <string>
#include <vector>

namespace {

using namespace scratchbird::wire;
using scratchbird::core::platform::LoadLittle16;
using scratchbird::core::platform::LoadLittle32;
using scratchbird::core::platform::LoadLittle64;
using scratchbird::core::platform::StoreLittle16;
using scratchbird::core::platform::StoreLittle32;
using scratchbird::core::platform::StoreLittle64;

constexpr u64 kScanBytes = 64ull * 1024ull * 1024ull;

[[noreturn]] void Die(const std::string& message) {
  std::cerr << "FAIL: " << message << '\n';
  std::exit(1);
}

void Require(bool condition, const std::string& message) {
  if (!condition) {
    Die(message);
  }
}

NarrowQueryUuid Uuid(unsigned seed) {
  NarrowQueryUuid value{};
  for (std::size_t index = 0; index < value.size(); ++index) {
    value[index] = static_cast<byte>((seed * 29u + index * 17u) & 0xffu);
  }
  value[0] |= 1u;
  return value;
}

NarrowQueryHash Hash(unsigned seed) {
  NarrowQueryHash value{};
  for (std::size_t index = 0; index < value.size(); ++index) {
    value[index] = static_cast<byte>((seed * 13u + index * 7u) & 0xffu);
  }
  value[0] |= 1u;
  return value;
}

NarrowQuerySourceOccurrence Source(unsigned ordinal,
                                   const NarrowQueryUuid& object_uuid,
                                   std::string alias) {
  NarrowQuerySourceOccurrence source;
  source.source_ordinal = ordinal;
  source.source_occurrence_uuid = Uuid(100 + ordinal);
  source.source_occurrence_generation = 10 + ordinal;
  source.relation_descriptor_uuid = Uuid(200);
  source.relation_descriptor_generation = 21;
  source.relation_object_uuid = object_uuid;
  source.schema_uuid = Uuid(201);
  source.validated_resource_epoch = 22;
  source.relation_projection_sha256 = Hash(202);
  source.alias = std::move(alias);
  return source;
}

NarrowQueryOutputOccurrence Output(
    unsigned ordinal,
    const NarrowQuerySourceOccurrence& source,
    const NarrowQueryUuid& column_uuid,
    unsigned column_ordinal,
    std::string name,
    unsigned name_occurrence) {
  NarrowQueryOutputOccurrence output;
  output.output_ordinal = ordinal;
  output.name_occurrence = name_occurrence;
  output.output_occurrence_uuid = Uuid(300 + ordinal);
  output.output_occurrence_generation = 30 + ordinal;
  output.source_occurrence_uuid = source.source_occurrence_uuid;
  output.source_occurrence_generation = source.source_occurrence_generation;
  output.source_column_uuid = column_uuid;
  output.source_column_ordinal = column_ordinal;
  output.output_descriptor_uuid = Uuid(400 + ordinal);
  output.output_descriptor_generation = 40 + ordinal;
  output.datatype_descriptor_uuid = Uuid(500);
  output.datatype_descriptor_generation = 51;
  output.datatype_type_uuid = Uuid(501);
  output.datatype_type_generation = 52;
  output.datatype_binary_type_code = 7;
  output.codec_version = 1;
  output.nullability = 1;
  output.null_encoding = 1;
  output.codec_generation = 53;
  output.canonical_value_bytes = 8;
  output.name = std::move(name);
  output.codec_id = "datatype.int64.le.v1";
  return output;
}

NarrowQueryOrderingTerm Term(unsigned ordinal,
                             const NarrowQuerySourceOccurrence& source,
                             const NarrowQueryUuid& column_uuid,
                             unsigned column_ordinal) {
  NarrowQueryOrderingTerm term;
  term.term_ordinal = ordinal;
  term.ordering_term_uuid = Uuid(600 + ordinal);
  term.ordering_term_generation = 60 + ordinal;
  term.source_occurrence_uuid = source.source_occurrence_uuid;
  term.source_occurrence_generation = source.source_occurrence_generation;
  term.source_column_uuid = column_uuid;
  term.source_column_ordinal = column_ordinal;
  term.direction = ordinal == 0 ? NarrowQueryDirection::ascending
                                : NarrowQueryDirection::descending;
  term.null_placement = ordinal == 0 ? NarrowQueryNullPlacement::last
                                     : NarrowQueryNullPlacement::first;
  return term;
}

NarrowQueryBinding BaseBinding(NarrowQueryProfile profile) {
  NarrowQueryBinding binding;
  binding.profile = profile;
  binding.statement_receipt_uuid = Uuid(1);
  binding.owning_transaction_uuid = Uuid(2);
  binding.owning_local_transaction_id = 3;
  binding.statement_snapshot_uuid = Uuid(4);
  binding.datatype_catalog_snapshot_uuid = Uuid(5);
  binding.datatype_catalog_generation = 6;
  binding.datatype_registry_generation = 7;
  binding.security_context_uuid = Uuid(8);
  binding.policy_snapshot_uuid = Uuid(9);
  binding.policy_generation = 10;
  binding.resource_grant_receipt_uuid = Uuid(11);
  binding.resource_grant_generation = 12;
  binding.cancellation_receipt_uuid = Uuid(13);
  binding.cancellation_generation = 14;
  binding.execution_uuid = Uuid(15);
  binding.result_set_uuid = Uuid(16);
  binding.row_descriptor_uuid = Uuid(17);
  binding.row_descriptor_generation = 18;
  binding.source_vector_uuid = Uuid(19);
  binding.source_vector_generation = 20;
  binding.output_vector_uuid = Uuid(21);
  binding.output_vector_generation = 22;
  binding.maximum_mga_relation_decoded_bytes_per_pass = kScanBytes;
  if (profile == NarrowQueryProfile::ordered_projection) {
    binding.ordering_vector_uuid = Uuid(23);
    binding.ordering_vector_generation = 24;
  }
  return binding;
}

NarrowQueryBinding OrderedBinding() {
  auto binding = BaseBinding(NarrowQueryProfile::ordered_projection);
  binding.sources.push_back(Source(0, Uuid(700), "t"));
  const auto first_column = Uuid(701);
  const auto second_column = Uuid(702);
  binding.outputs.push_back(
      Output(0, binding.sources[0], first_column, 0, "id", 0));
  binding.outputs.push_back(
      Output(1, binding.sources[0], second_column, 1, "payload;=", 0));
  binding.ordering_terms.push_back(
      Term(0, binding.sources[0], first_column, 0));
  binding.ordering_terms.push_back(
      Term(1, binding.sources[0], second_column, 1));
  return binding;
}

NarrowQueryBinding DuplicateProjectionBinding() {
  auto binding = BaseBinding(NarrowQueryProfile::projection_occurrence);
  binding.sources.push_back(Source(0, Uuid(710), "d"));
  const auto column = Uuid(711);
  binding.outputs.push_back(
      Output(0, binding.sources[0], column, 2, "payload;=", 0));
  binding.outputs.push_back(
      Output(1, binding.sources[0], column, 2, "payload;=", 1));
  return binding;
}

NarrowQueryBinding SelfJoinBinding() {
  auto binding = BaseBinding(NarrowQueryProfile::alias_distinct_self_join);
  const auto object = Uuid(720);
  binding.sources.push_back(Source(0, object, "a"));
  binding.sources.push_back(Source(1, object, "b"));
  binding.sources.push_back(Source(2, object, "c"));
  const auto column = Uuid(721);
  for (unsigned index = 0; index < binding.sources.size(); ++index) {
    binding.outputs.push_back(Output(index, binding.sources[index], column, 0,
                                     "id", index));
  }
  return binding;
}

NarrowQueryBindingValidationContext Context(const NarrowQueryBinding& binding) {
  NarrowQueryBindingValidationContext context;
  context.statement_receipt_uuid = binding.statement_receipt_uuid;
  context.owning_transaction_uuid = binding.owning_transaction_uuid;
  context.owning_local_transaction_id = binding.owning_local_transaction_id;
  context.statement_snapshot_uuid = binding.statement_snapshot_uuid;
  context.datatype_catalog_snapshot_uuid =
      binding.datatype_catalog_snapshot_uuid;
  context.datatype_catalog_generation = binding.datatype_catalog_generation;
  context.datatype_registry_generation = binding.datatype_registry_generation;
  context.security_context_uuid = binding.security_context_uuid;
  context.policy_snapshot_uuid = binding.policy_snapshot_uuid;
  context.policy_generation = binding.policy_generation;
  context.resource_grant_receipt_uuid = binding.resource_grant_receipt_uuid;
  context.resource_grant_generation = binding.resource_grant_generation;
  context.cancellation_receipt_uuid = binding.cancellation_receipt_uuid;
  context.cancellation_generation = binding.cancellation_generation;
  context.execution_uuid = binding.execution_uuid;
  context.result_set_uuid = binding.result_set_uuid;
  context.row_descriptor_uuid = binding.row_descriptor_uuid;
  context.row_descriptor_generation = binding.row_descriptor_generation;
  context.maximum_mga_relation_decoded_bytes_per_pass =
      binding.maximum_mga_relation_decoded_bytes_per_pass;
  context.maximum_result_rows = kNarrowQueryMaximumExplicitResultRows;
  context.validate_canonical_alias = [](std::string_view alias) {
    return !alias.empty();
  };
  context.validate_source = [](const NarrowQuerySourceOccurrence&) {
    return NarrowQueryAuthorityDecision::accepted;
  };
  context.validate_output_datatype =
      [](const NarrowQueryOutputOccurrence&) {
        return NarrowQueryAuthorityDecision::accepted;
      };
  context.validate_collation = [](const NarrowQueryOrderingTerm&) {
    return NarrowQueryAuthorityDecision::accepted;
  };
  return context;
}

std::vector<byte> Encode(const NarrowQueryBinding& binding) {
  std::vector<byte> encoded;
  NarrowQueryBindingError error;
  Require(EncodeNarrowQueryBinding(binding, &encoded, &error),
          "encode failed: " + error.field + ":" + error.detail);
  Require(error.ok(), "successful encode retained an error");
  return encoded;
}

NarrowQueryBinding Decode(const std::vector<byte>& encoded,
                          const NarrowQueryBindingValidationContext& context) {
  NarrowQueryBinding decoded;
  NarrowQueryBindingError error;
  Require(DecodeAndValidateNarrowQueryBinding(encoded, context, &decoded,
                                               &error),
          "decode failed: " + error.field + ":" + error.detail);
  Require(error.ok(), "successful decode retained an error");
  return decoded;
}

void ExpectDecodeError(const std::vector<byte>& encoded,
                       const NarrowQueryBindingValidationContext& context,
                       NarrowQueryBindingErrorCode expected,
                       std::string_view diagnostic) {
  auto sentinel = SelfJoinBinding();
  const auto original_profile = sentinel.profile;
  const auto original_source_count = sentinel.sources.size();
  NarrowQueryBindingError error;
  Require(!DecodeAndValidateNarrowQueryBinding(encoded, context, &sentinel,
                                                &error),
          "malformed carrier unexpectedly decoded");
  Require(error.code == expected,
          std::string("wrong error: expected ") +
              NarrowQueryBindingErrorCodeName(expected) + " got " +
              NarrowQueryBindingErrorCodeName(error.code));
  Require(error.diagnostic_code == diagnostic,
          "wrong canonical diagnostic for " + error.field);
  Require(sentinel.profile == original_profile &&
              sentinel.sources.size() == original_source_count,
          "decode refusal modified the caller output");
}

void ExpectEncodeError(const NarrowQueryBinding& binding,
                       NarrowQueryBindingErrorCode expected) {
  std::vector<byte> output{0xaa, 0xbb};
  NarrowQueryBindingError error;
  Require(!EncodeNarrowQueryBinding(binding, &output, &error),
          "invalid binding unexpectedly encoded");
  Require(error.code == expected,
          std::string("wrong encode error: expected ") +
              NarrowQueryBindingErrorCodeName(expected) + " got " +
              NarrowQueryBindingErrorCodeName(error.code));
  Require(output == std::vector<byte>({0xaa, 0xbb}),
          "encode refusal modified the caller output");
}

void RoundTripProfiles() {
  for (const auto binding : {OrderedBinding(), DuplicateProjectionBinding(),
                             SelfJoinBinding()}) {
    const auto encoded = Encode(binding);
    const auto encoded_again = Encode(binding);
    Require(encoded == encoded_again, "encoding is not deterministic");
    Require(encoded.size() >= kNarrowQueryBindingHeaderBytes,
            "encoded carrier is shorter than its header");
    Require(std::string(encoded.begin(), encoded.begin() + 8) == "SBQNPB01",
            "wrong carrier magic");
    Require(LoadLittle16(encoded.data() + 8) == 1 &&
                LoadLittle16(encoded.data() + 10) == 472,
            "wrong carrier version/header bytes");
    Require(LoadLittle64(encoded.data() + 16) == encoded.size(),
            "wrong total carrier bytes");
    Require(LoadLittle16(encoded.data() + 24) ==
                static_cast<u16>(binding.profile),
            "wrong profile code");
    Require(LoadLittle16(encoded.data() + 26) == binding.sources.size() &&
                LoadLittle32(encoded.data() + 28) == binding.outputs.size() &&
                LoadLittle32(encoded.data() + 32) ==
                    binding.ordering_terms.size(),
            "wrong vector counts");
    Require(std::any_of(encoded.begin() + 296, encoded.begin() + 328,
                        [](byte value) { return value != 0; }) &&
                std::any_of(encoded.begin() + 352, encoded.begin() + 384,
                            [](byte value) { return value != 0; }) &&
                std::any_of(encoded.begin() + 408, encoded.begin() + 440,
                            [](byte value) { return value != 0; }) &&
                std::any_of(encoded.begin() + 440, encoded.begin() + 472,
                            [](byte value) { return value != 0; }),
            "one or more exact evidence hashes are zero");
    Require(LoadLittle64(encoded.data() + 472u) ==
                binding.maximum_mga_relation_decoded_bytes_per_pass,
            "mandatory scan-byte resource policy is not at offset 472");
    const auto decoded = Decode(encoded, Context(binding));
    Require(decoded.profile == binding.profile &&
                decoded.maximum_mga_relation_decoded_bytes_per_pass ==
                    binding.maximum_mga_relation_decoded_bytes_per_pass &&
                decoded.sources.size() == binding.sources.size() &&
                decoded.outputs.size() == binding.outputs.size() &&
                decoded.ordering_terms.size() == binding.ordering_terms.size(),
            "round-trip shape mismatch");
  }

  const auto duplicate = DuplicateProjectionBinding();
  const auto decoded = Decode(Encode(duplicate), Context(duplicate));
  Require(decoded.outputs[0].name == "payload;=" &&
              decoded.outputs[1].name == "payload;=" &&
              decoded.outputs[0].name_occurrence == 0 &&
              decoded.outputs[1].name_occurrence == 1 &&
              decoded.outputs[0].output_occurrence_uuid !=
                  decoded.outputs[1].output_occurrence_uuid &&
              decoded.outputs[0].output_descriptor_uuid !=
                  decoded.outputs[1].output_descriptor_uuid,
          "duplicate-name occurrence identity was collapsed");

  const auto duplicate_bytes = Encode(duplicate);
  const auto join_bytes = Encode(SelfJoinBinding());
  Require(std::equal(duplicate_bytes.begin() + 408,
                     duplicate_bytes.begin() + 440,
                     join_bytes.begin() + 408),
          "canonical empty ordering vector hash differs across profiles");
}

void ResultBoundSemantics() {
  auto unbounded = DuplicateProjectionBinding();
  const auto unbounded_bytes = Encode(unbounded);
  Require(LoadLittle32(unbounded_bytes.data() + 12) == 0,
          "absent result bound set a presence flag");
  Require(LoadLittle32(unbounded_bytes.data() + 480) ==
              kNarrowQuerySourcePrefixBytes + unbounded.sources[0].alias.size(),
          "absent result bound moved the first source record");

  auto limit_zero = DuplicateProjectionBinding();
  limit_zero.row_limit_present = true;
  limit_zero.row_limit = 0;
  const auto limit_zero_bytes = Encode(limit_zero);
  Require(LoadLittle32(limit_zero_bytes.data() + 12) == 1 &&
              LoadLittle64(limit_zero_bytes.data() + 472) == kScanBytes &&
              LoadLittle64(limit_zero_bytes.data() + 480) == 0 &&
              LoadLittle64(limit_zero_bytes.data() + 488) == 0 &&
              LoadLittle32(limit_zero_bytes.data() + 496) ==
                  kNarrowQuerySourcePrefixBytes +
                      limit_zero.sources[0].alias.size(),
          "LIMIT 0 did not retain its explicit conditional extent");
  const auto decoded_zero = Decode(limit_zero_bytes, Context(limit_zero));
  Require(decoded_zero.row_limit_present && decoded_zero.row_limit == 0 &&
              !decoded_zero.row_offset_present &&
              decoded_zero.row_offset == 0,
          "LIMIT 0 was collapsed into absent/unbounded");

  auto offset_only = SelfJoinBinding();
  offset_only.row_offset_present = true;
  offset_only.row_offset = 7;
  const auto offset_only_bytes = Encode(offset_only);
  Require(LoadLittle32(offset_only_bytes.data() + 12) == 2 &&
              LoadLittle64(offset_only_bytes.data() + 480) == 0 &&
              LoadLittle64(offset_only_bytes.data() + 488) == 7,
          "independent OFFSET was not encoded exactly");
  const auto decoded_offset = Decode(offset_only_bytes, Context(offset_only));
  Require(!decoded_offset.row_limit_present &&
              decoded_offset.row_limit == 0 &&
              decoded_offset.row_offset_present &&
              decoded_offset.row_offset == 7,
          "independent OFFSET did not round-trip");

  auto window = OrderedBinding();
  window.row_limit_present = true;
  window.row_limit = 9;
  window.row_offset_present = true;
  window.row_offset = 3;
  const auto window_bytes = Encode(window);
  Require(LoadLittle32(window_bytes.data() + 12) == 3 &&
              LoadLittle64(window_bytes.data() + 480) == 9 &&
              LoadLittle64(window_bytes.data() + 488) == 3,
          "LIMIT/OFFSET window encoding mismatch");
  const auto decoded_window = Decode(window_bytes, Context(window));
  Require(decoded_window.row_limit_present && decoded_window.row_limit == 9 &&
              decoded_window.row_offset_present &&
              decoded_window.row_offset == 3,
          "LIMIT/OFFSET window did not round-trip");
  Require(std::equal(unbounded_bytes.begin() + 296,
                     unbounded_bytes.begin() + 328,
                     limit_zero_bytes.begin() + 296) &&
              std::equal(unbounded_bytes.begin() + 352,
                         unbounded_bytes.begin() + 384,
                         limit_zero_bytes.begin() + 352) &&
              std::equal(unbounded_bytes.begin() + 408,
                         unbounded_bytes.begin() + 440,
                         limit_zero_bytes.begin() + 408),
          "post-projection result bound changed a source/output/ordering vector hash");

  auto absent_nonzero = DuplicateProjectionBinding();
  absent_nonzero.row_limit = 1;
  ExpectEncodeError(absent_nonzero,
                    NarrowQueryBindingErrorCode::result_bound_invalid);

  auto overflow = DuplicateProjectionBinding();
  overflow.row_limit_present = true;
  overflow.row_limit = std::numeric_limits<u64>::max();
  overflow.row_offset_present = true;
  overflow.row_offset = 1;
  ExpectEncodeError(overflow,
                    NarrowQueryBindingErrorCode::result_bound_invalid);

  auto profile_exceeded = DuplicateProjectionBinding();
  profile_exceeded.row_limit_present = true;
  profile_exceeded.row_limit = kNarrowQueryMaximumExplicitResultRows + 1;
  ExpectEncodeError(profile_exceeded,
                    NarrowQueryBindingErrorCode::resource_limit_exceeded);

  auto combined_exceeded = DuplicateProjectionBinding();
  combined_exceeded.row_limit_present = true;
  combined_exceeded.row_limit = 600000;
  combined_exceeded.row_offset_present = true;
  combined_exceeded.row_offset = 600000;
  ExpectEncodeError(combined_exceeded,
                    NarrowQueryBindingErrorCode::resource_limit_exceeded);

  auto truncated_bound = limit_zero_bytes;
  truncated_bound.resize(kNarrowQueryBindingHeaderBytes + sizeof(u64) + 8u);
  StoreLittle64(truncated_bound.data() + 16, truncated_bound.size());
  ExpectDecodeError(truncated_bound, Context(limit_zero),
                    NarrowQueryBindingErrorCode::extent_invalid,
                    "SBLR.OPERAND.INVALID");

  auto malformed_absent_value = limit_zero_bytes;
  StoreLittle64(malformed_absent_value.data() + 488, 1);
  ExpectDecodeError(malformed_absent_value, Context(limit_zero),
                    NarrowQueryBindingErrorCode::result_bound_invalid,
                    "SBLR.OPERAND.INVALID");

  auto unknown_flag = limit_zero_bytes;
  StoreLittle32(unknown_flag.data() + 12, 4);
  ExpectDecodeError(unknown_flag, Context(limit_zero),
                    NarrowQueryBindingErrorCode::reserved_invalid,
                    "SBLR.OPERAND.INVALID");

  auto evidence_mutation = window_bytes;
  StoreLittle64(evidence_mutation.data() + 480, 8);
  ExpectDecodeError(evidence_mutation, Context(window),
                    NarrowQueryBindingErrorCode::carrier_evidence_mismatch,
                    "SBLR.OPERAND.INVALID");

  auto resource_context = Context(window);
  resource_context.maximum_result_rows = 10;
  ExpectDecodeError(window_bytes, resource_context,
                    NarrowQueryBindingErrorCode::resource_limit_exceeded,
                    "RESOURCE.BUDGET_EXCEEDED");
}

void ScanByteResourcePolicy() {
  auto binding = DuplicateProjectionBinding();
  const auto canonical = Encode(binding);
  const auto context = Context(binding);
  Require(LoadLittle64(canonical.data() + 472u) == kScanBytes &&
              LoadLittle32(canonical.data() + 480u) ==
                  kNarrowQuerySourcePrefixBytes + binding.sources[0].alias.size(),
          "SBQNPB01 mandatory scan-byte extent or vector offset is wrong");

  auto zero = binding;
  zero.maximum_mga_relation_decoded_bytes_per_pass = 0;
  ExpectEncodeError(zero,
                    NarrowQueryBindingErrorCode::resource_limit_exceeded);

  auto below_minimum = binding;
  below_minimum.maximum_mga_relation_decoded_bytes_per_pass =
      kNarrowQueryMinimumMgaRelationDecodedBytesPerPass - 1u;
  ExpectEncodeError(below_minimum,
                    NarrowQueryBindingErrorCode::resource_limit_exceeded);

  auto above_maximum = binding;
  above_maximum.maximum_mga_relation_decoded_bytes_per_pass =
      kNarrowQueryMaximumMgaRelationDecodedBytesPerPass + 1u;
  ExpectEncodeError(above_maximum,
                    NarrowQueryBindingErrorCode::resource_limit_exceeded);

  auto wrong_context = context;
  wrong_context.maximum_mga_relation_decoded_bytes_per_pass = kScanBytes * 2u;
  ExpectDecodeError(canonical, wrong_context,
                    NarrowQueryBindingErrorCode::resource_grant_mismatch,
                    "RESOURCE.BUDGET_EXCEEDED");

  auto truncated = canonical;
  truncated.resize(479u);
  StoreLittle64(truncated.data() + 16u, truncated.size());
  ExpectDecodeError(truncated, context,
                    NarrowQueryBindingErrorCode::extent_invalid,
                    "SBLR.OPERAND.INVALID");

  auto hash_mutation = canonical;
  StoreLittle64(hash_mutation.data() + 472u, kScanBytes * 2u);
  ExpectDecodeError(hash_mutation, context,
                    NarrowQueryBindingErrorCode::carrier_evidence_mismatch,
                    "SBLR.OPERAND.INVALID");

  auto independent = binding;
  independent.maximum_mga_relation_decoded_bytes_per_pass =
      kNarrowQueryMinimumMgaRelationDecodedBytesPerPass;
  const auto independent_bytes = Encode(independent);
  auto independent_context = Context(independent);
  independent_context.maximum_result_rows =
      kNarrowQueryMaximumExplicitResultRows;
  const auto decoded = Decode(independent_bytes, independent_context);
  Require(decoded.maximum_mga_relation_decoded_bytes_per_pass == 64ull * 1024ull,
          "scan-byte ceiling aliased the result-row or carrier limit");
  Require(std::equal(canonical.begin() + 296u, canonical.begin() + 328u,
                     independent_bytes.begin() + 296u) &&
              std::equal(canonical.begin() + 352u, canonical.begin() + 384u,
                         independent_bytes.begin() + 352u) &&
              std::equal(canonical.begin() + 408u, canonical.begin() + 440u,
                         independent_bytes.begin() + 408u) &&
              !std::equal(canonical.begin() + 440u, canonical.begin() + 472u,
                          independent_bytes.begin() + 440u),
          "scan-byte field changed vector hashes or failed to bind the carrier hash");
}

void MalformedBytePrecedence() {
  const auto binding = OrderedBinding();
  const auto context = Context(binding);
  const auto canonical = Encode(binding);

  auto mutated = canonical;
  mutated[0] ^= 1u;
  ExpectDecodeError(mutated, context,
                    NarrowQueryBindingErrorCode::magic_invalid,
                    "SBLR.OPERAND.INVALID");

  mutated = canonical;
  StoreLittle16(mutated.data() + 8, 2);
  ExpectDecodeError(mutated, context,
                    NarrowQueryBindingErrorCode::version_invalid,
                    "SBLR.OPERAND.INVALID");

  mutated = canonical;
  StoreLittle64(mutated.data() + 16, mutated.size() + 1);
  ExpectDecodeError(mutated, context,
                    NarrowQueryBindingErrorCode::extent_invalid,
                    "SBLR.OPERAND.INVALID");

  mutated = canonical;
  StoreLittle32(mutated.data() + 36, 1);
  ExpectDecodeError(mutated, context,
                    NarrowQueryBindingErrorCode::reserved_invalid,
                    "SBLR.OPERAND.INVALID");

  mutated = canonical;
  StoreLittle16(mutated.data() + 24, 99);
  ExpectDecodeError(mutated, context,
                    NarrowQueryBindingErrorCode::profile_invalid,
                    "SBLR.OPERAND.INVALID");

  mutated = canonical;
  StoreLittle16(mutated.data() + 26, 0);
  ExpectDecodeError(mutated, context,
                    NarrowQueryBindingErrorCode::count_invalid,
                    "SBLR.OPERAND.INVALID");

  mutated = canonical;
  StoreLittle32(mutated.data() + 480, 135);
  ExpectDecodeError(mutated, context,
                    NarrowQueryBindingErrorCode::extent_invalid,
                    "SBLR.OPERAND.INVALID");

  mutated = canonical;
  mutated[296] ^= 1u;
  ExpectDecodeError(mutated, context,
                    NarrowQueryBindingErrorCode::source_vector_evidence_mismatch,
                    "SBLR.OPERAND.INVALID");

  mutated = canonical;
  mutated[352] ^= 1u;
  ExpectDecodeError(mutated, context,
                    NarrowQueryBindingErrorCode::output_vector_evidence_mismatch,
                    "SBLR.OPERAND.INVALID");

  mutated = canonical;
  mutated[408] ^= 1u;
  ExpectDecodeError(
      mutated, context,
      NarrowQueryBindingErrorCode::ordering_vector_evidence_mismatch,
      "SBLR.OPERAND.INVALID");

  mutated = canonical;
  mutated[440] ^= 1u;
  ExpectDecodeError(mutated, context,
                    NarrowQueryBindingErrorCode::carrier_evidence_mismatch,
                    "SBLR.OPERAND.INVALID");

  auto limited_context = context;
  limited_context.maximum_total_bytes = canonical.size() - 1;
  ExpectDecodeError(mutated = canonical, limited_context,
                    NarrowQueryBindingErrorCode::resource_limit_exceeded,
                    "RESOURCE.BUDGET_EXCEEDED");
}

void ContextAndAuthorityPrecedence() {
  const auto binding = OrderedBinding();
  const auto encoded = Encode(binding);
  auto context = Context(binding);

  context.statement_receipt_uuid = Uuid(900);
  ExpectDecodeError(encoded, context,
                    NarrowQueryBindingErrorCode::statement_receipt_mismatch,
                    "MGA.TRANSACTION.STALE");

  context = Context(binding);
  context.security_context_uuid = Uuid(901);
  ExpectDecodeError(encoded, context,
                    NarrowQueryBindingErrorCode::security_mismatch,
                    "SECURITY.ACCESS_DENIED");

  context = Context(binding);
  context.validate_source = [](const NarrowQuerySourceOccurrence&) {
    return NarrowQueryAuthorityDecision::hidden_or_unauthorized;
  };
  ExpectDecodeError(encoded, context,
                    NarrowQueryBindingErrorCode::source_unauthorized,
                    "SECURITY.ACCESS_DENIED");

  context = Context(binding);
  context.validate_output_datatype =
      [](const NarrowQueryOutputOccurrence&) {
        return NarrowQueryAuthorityDecision::stale_or_mismatched;
      };
  ExpectDecodeError(encoded, context,
                    NarrowQueryBindingErrorCode::output_datatype_invalid,
                    "DATATYPE.DESCRIPTOR_INVALID");

  context = Context(binding);
  context.validate_collation = [](const NarrowQueryOrderingTerm&) {
    return NarrowQueryAuthorityDecision::stale_or_mismatched;
  };
  ExpectDecodeError(encoded, context,
                    NarrowQueryBindingErrorCode::ordering_collation_invalid,
                    "SORT.COLLATION_PROFILE_INVALID");

  context = Context(binding);
  context.cancelled = true;
  ExpectDecodeError(encoded, context,
                    NarrowQueryBindingErrorCode::cancelled,
                    "PROCESS.CANCELLED");

  context = Context(binding);
  context.validate_source = {};
  ExpectDecodeError(encoded, context,
                    NarrowQueryBindingErrorCode::validation_context_invalid,
                    "SBLR.OPERAND.INVALID");
}

void StructuralProfileRefusals() {
  auto binding = DuplicateProjectionBinding();
  binding.outputs[1].output_occurrence_uuid =
      binding.outputs[0].output_occurrence_uuid;
  binding.outputs[1].output_occurrence_generation =
      binding.outputs[0].output_occurrence_generation;
  ExpectEncodeError(binding,
                    NarrowQueryBindingErrorCode::output_identity_invalid);

  binding = DuplicateProjectionBinding();
  binding.outputs[1].name_occurrence = 0;
  ExpectEncodeError(binding,
                    NarrowQueryBindingErrorCode::output_record_invalid);

  binding = DuplicateProjectionBinding();
  binding.outputs[1].source_column_uuid = Uuid(999);
  ExpectEncodeError(binding,
                    NarrowQueryBindingErrorCode::profile_shape_invalid);

  binding = OrderedBinding();
  binding.ordering_terms.pop_back();
  ExpectEncodeError(binding,
                    NarrowQueryBindingErrorCode::profile_shape_invalid);

  binding = OrderedBinding();
  binding.ordering_terms[1].source_column_uuid =
      binding.ordering_terms[0].source_column_uuid;
  binding.ordering_terms[1].source_column_ordinal =
      binding.ordering_terms[0].source_column_ordinal;
  ExpectEncodeError(binding,
                    NarrowQueryBindingErrorCode::profile_shape_invalid);

  binding = SelfJoinBinding();
  binding.sources[1].relation_descriptor_generation++;
  ExpectEncodeError(binding,
                    NarrowQueryBindingErrorCode::source_authority_stale);

  binding = SelfJoinBinding();
  binding.sources[1].relation_object_uuid = Uuid(930);
  binding.sources[2].relation_object_uuid = Uuid(931);
  ExpectEncodeError(binding,
                    NarrowQueryBindingErrorCode::profile_shape_invalid);

  binding = SelfJoinBinding();
  binding.outputs.pop_back();
  ExpectEncodeError(binding,
                    NarrowQueryBindingErrorCode::profile_shape_invalid);

  binding = SelfJoinBinding();
  binding.sources[1].alias = binding.sources[0].alias;
  ExpectEncodeError(binding,
                    NarrowQueryBindingErrorCode::source_alias_invalid);

  binding = DuplicateProjectionBinding();
  binding.outputs[0].codec_id = "bad codec";
  ExpectEncodeError(binding,
                    NarrowQueryBindingErrorCode::output_record_invalid);

  binding = DuplicateProjectionBinding();
  binding.outputs[0].name.assign("bad\0name", 8);
  ExpectEncodeError(binding,
                    NarrowQueryBindingErrorCode::output_record_invalid);

  binding = OrderedBinding();
  binding.ordering_terms[0].collation_generation = 1;
  ExpectEncodeError(binding,
                    NarrowQueryBindingErrorCode::ordering_record_invalid);
}

}  // namespace

int main() {
  RoundTripProfiles();
  ScanByteResourcePolicy();
  ResultBoundSemantics();
  MalformedBytePrecedence();
  ContextAndAuthorityPrecedence();
  StructuralProfileRefusals();
  std::cout << "PASS narrow_query_binding_codec exact SBQNPB01 profiles=3"
            << " hashes=4 result_bound=1 malformed_precedence=1"
            << " occurrence_identity=1\n";
  return 0;
}
