// Copyright (c) 2026 ScratchBird Software Inc.
//
// This Source Code Form is subject to the terms of the Mozilla Public
// License, v. 2.0. If a copy of the MPL was not distributed with this
// file, You can obtain one at https://mozilla.org/MPL/2.0/.
//
// SPDX-License-Identifier: MPL-2.0

#include "expression_index_extractor.hpp"
#include "uuid.hpp"

#include <cstdlib>
#include <iostream>
#include <string>
#include <string_view>
#include <vector>

namespace idx = scratchbird::core::index;
namespace platform = scratchbird::core::platform;

namespace {

[[noreturn]] void Fail(std::string_view message) {
  std::cerr << "expression_index_extractor_gate: " << message << '\n';
  std::exit(EXIT_FAILURE);
}

void Require(bool condition, std::string_view message) {
  if (!condition) {
    Fail(message);
  }
}

platform::TypedUuid TestUuid(platform::UuidKind kind, platform::byte seed) {
  platform::TypedUuid uuid;
  uuid.kind = kind;
  for (std::size_t i = 0; i < uuid.value.bytes.size(); ++i) {
    uuid.value.bytes[i] =
        static_cast<platform::byte>(seed + static_cast<platform::byte>(i + 1));
  }
  uuid.value.bytes[6] =
      static_cast<platform::byte>((uuid.value.bytes[6] & 0x0f) | 0x70);
  uuid.value.bytes[8] =
      static_cast<platform::byte>((uuid.value.bytes[8] & 0x3f) | 0x80);
  return uuid;
}

idx::ExpressionIndexValueDescriptor Descriptor(std::string type_name,
                                               platform::byte seed,
                                               bool case_folded = false,
                                               bool collation = false) {
  idx::ExpressionIndexValueDescriptor descriptor;
  descriptor.canonical_type_name = std::move(type_name);
  descriptor.encoded_descriptor = "type=" + descriptor.canonical_type_name;
  descriptor.type_descriptor_uuid =
      TestUuid(platform::UuidKind::object, seed);
  if (collation) {
    descriptor.collation_uuid =
        TestUuid(platform::UuidKind::object, static_cast<platform::byte>(seed + 0x40));
  }
  descriptor.case_folded = case_folded;
  return descriptor;
}

idx::ExpressionIndexTypedValue Value(std::string type_name,
                                     std::string encoded,
                                     bool is_null = false) {
  idx::ExpressionIndexTypedValue value;
  value.descriptor.canonical_type_name = std::move(type_name);
  value.descriptor.encoded_descriptor = "type=" + value.descriptor.canonical_type_name;
  value.encoded_value = std::move(encoded);
  value.is_null = is_null;
  return value;
}

idx::ExpressionIndexExpressionNode Literal(std::string type_name,
                                           std::string encoded,
                                           bool is_null = false) {
  idx::ExpressionIndexExpressionNode node;
  node.kind = idx::ExpressionIndexNodeKind::literal;
  node.literal = Value(std::move(type_name), std::move(encoded), is_null);
  return node;
}

idx::ExpressionIndexExpressionNode Column(std::string name) {
  idx::ExpressionIndexExpressionNode node;
  node.kind = idx::ExpressionIndexNodeKind::column_ref;
  node.identifier = std::move(name);
  return node;
}

idx::ExpressionIndexExpressionNode Operator(
    std::string id,
    std::vector<idx::ExpressionIndexExpressionNode> arguments) {
  idx::ExpressionIndexExpressionNode node;
  node.kind = idx::ExpressionIndexNodeKind::operator_call;
  node.identifier = std::move(id);
  node.arguments = std::move(arguments);
  return node;
}

idx::ExpressionIndexExpressionNode Function(
    std::string id,
    std::vector<idx::ExpressionIndexExpressionNode> arguments) {
  idx::ExpressionIndexExpressionNode node;
  node.kind = idx::ExpressionIndexNodeKind::function_call;
  node.identifier = std::move(id);
  node.arguments = std::move(arguments);
  return node;
}

idx::ExpressionIndexOperationProof Proof(idx::ExpressionIndexOperationKind kind,
                                         std::string operation_id,
                                         bool deterministic = true) {
  idx::ExpressionIndexOperationProof proof;
  proof.kind = kind;
  proof.operation_id = std::move(operation_id);
  proof.proof_present = true;
  proof.deterministic = deterministic;
  proof.resource_epoch_bound = true;
  proof.proof_digest = "proof:" + proof.operation_id;
  return proof;
}

idx::ExpressionIndexDefinition Definition(
    std::string text,
    idx::ExpressionIndexExpressionNode root) {
  idx::ExpressionIndexDefinition definition;
  definition.expression_text = std::move(text);
  definition.root = std::move(root);
  definition.root_present = true;
  return definition;
}

idx::ExpressionIndexBindRequest BindRequest(
    std::string text,
    idx::ExpressionIndexExpressionNode root,
    idx::ExpressionIndexValueDescriptor result_descriptor,
    std::vector<idx::ExpressionIndexOperationProof> proofs = {}) {
  idx::ExpressionIndexBindRequest request;
  request.definition = Definition(std::move(text), std::move(root));
  request.result_descriptor = std::move(result_descriptor);
  request.epochs.resource_epoch = 11;
  request.epochs.collation_epoch = 22;
  request.epochs.function_resource_epoch = 33;
  request.semantic_profile.profile_id = "expression_index_extractor_gate";
  request.operation_proofs = std::move(proofs);
  return request;
}

idx::ExpressionIndexRowInput Row(std::string row_id,
                                 std::vector<idx::ExpressionIndexColumnValue> columns) {
  idx::ExpressionIndexRowInput row;
  row.row_id = std::move(row_id);
  row.columns = std::move(columns);
  return row;
}

idx::ExpressionIndexColumnValue Field(std::string name,
                                      platform::u32 ordinal,
                                      idx::ExpressionIndexTypedValue value) {
  idx::ExpressionIndexColumnValue field;
  field.name = std::move(name);
  field.ordinal = ordinal;
  field.ordinal_valid = true;
  field.value = std::move(value);
  return field;
}

std::string BytesView(const std::vector<platform::byte>& bytes) {
  return std::string(reinterpret_cast<const char*>(bytes.data()), bytes.size());
}

bool HasEvidence(const std::vector<std::string>& evidence, std::string_view value) {
  for (const auto& candidate : evidence) {
    if (candidate == value) {
      return true;
    }
  }
  return false;
}

void RequireNoDocRuntimeEvidence(const std::vector<std::string>& evidence) {
  for (const auto& value : evidence) {
    Require(value.find("docs/") == std::string::npos,
            "runtime evidence leaked docs path");
    Require(value.find("execution-plans") == std::string::npos,
            "runtime evidence leaked execution_plan path");
    Require(value.find("contracts") == std::string::npos,
            "runtime evidence leaked contract path");
    Require(value.find("findings") == std::string::npos,
            "runtime evidence leaked findings path");
  }
}

void RequireCanonicalDigest() {
  const auto root = Function("fn.upper_ascii", {Column("Name")});
  const auto a = idx::CanonicalizeExpressionIndexDefinition(
      Definition("UPPER ( Name )", root));
  const auto b = idx::CanonicalizeExpressionIndexDefinition(
      Definition(" upper(name)", root));
  Require(a.ok() && b.ok(), "canonicalization refused deterministic expression");
  Require(a.canonical_text == "upper(name)",
          "canonical expression text did not normalize case/whitespace");
  Require(a.expression_digest == b.expression_digest,
          "digest changed under harmless case/whitespace normalization");

  const auto distinct = idx::CanonicalizeExpressionIndexDefinition(
      Definition("upper(other_name)",
                 Function("fn.upper_ascii", {Column("Other_Name")})));
  Require(distinct.ok(), "distinct canonicalization failed");
  Require(a.expression_digest != distinct.expression_digest,
          "distinct expression did not change digest");
}

void RequireDeterminismAndCache() {
  idx::ExpressionIndexDescriptorCache cache;
  const auto text_desc = Descriptor("text", 0x21, true, true);
  auto request = BindRequest(
      "UPPER ( Name )",
      Function("fn.upper_ascii", {Column("Name")}),
      text_desc,
      {Proof(idx::ExpressionIndexOperationKind::function_call, "fn.upper_ascii")});

  auto bound = idx::BindExpressionIndexExtractorDescriptor(&cache, request);
  Require(bound.ok() && bound.cache_miss && !bound.cache_hit,
          "deterministic function proof was not accepted as cache miss");
  Require(HasEvidence(bound.descriptor.evidence,
                      "expression_index_visibility_authority=false"),
          "descriptor missed non-authority evidence");
  Require(!bound.descriptor.non_authority.visibility_authority &&
              !bound.descriptor.non_authority.authorization_authority &&
              !bound.descriptor.non_authority.transaction_finality_authority &&
              !bound.descriptor.non_authority.cleanup_authority &&
              !bound.descriptor.non_authority.recovery_authority,
          "descriptor authority flags were not false");

  auto hit = idx::BindExpressionIndexExtractorDescriptor(&cache, request);
  Require(hit.ok() && hit.cache_hit && !hit.cache_miss,
          "descriptor cache did not hit for same digest/epochs/result");

  request.epochs.collation_epoch = 23;
  auto invalidated = idx::BindExpressionIndexExtractorDescriptor(&cache, request);
  Require(invalidated.ok() && invalidated.cache_miss &&
              invalidated.invalidated_entries == 1,
          "collation epoch change did not invalidate cached descriptor");
  Require(cache.hits == 1 && cache.misses == 2 && cache.invalidations == 1,
          "descriptor cache hit/miss/invalidation counters drifted");

  auto unproven = BindRequest(
      "upper(name)",
      Function("fn.upper_ascii", {Column("Name")}),
      text_desc,
      {});
  auto refused = idx::BindExpressionIndexExtractorDescriptor(&cache, unproven);
  Require(!refused.ok() &&
              refused.diagnostic.diagnostic_code ==
                  "SB-INDEX-EXPR-DETERMINISM-PROOF-MISSING",
          "unproven function was not exactly refused");

  auto nondeterministic = BindRequest(
      "upper(name)",
      Function("fn.upper_ascii", {Column("Name")}),
      text_desc,
      {Proof(idx::ExpressionIndexOperationKind::function_call,
             "fn.upper_ascii",
             false)});
  refused = idx::BindExpressionIndexExtractorDescriptor(&cache, nondeterministic);
  Require(!refused.ok() &&
              refused.diagnostic.diagnostic_code ==
                  "SB-INDEX-EXPR-NONDETERMINISTIC-OPERATION",
          "nondeterministic function proof was not exactly refused");

  auto unsupported = BindRequest(
      "sqrt(amount)",
      Function("fn.sqrt", {Column("amount")}),
      text_desc,
      {Proof(idx::ExpressionIndexOperationKind::function_call, "fn.sqrt")});
  refused = idx::BindExpressionIndexExtractorDescriptor(&cache, unsupported);
  Require(!refused.ok() &&
              refused.diagnostic.diagnostic_code ==
                  "SB-INDEX-EXPR-FUNCTION-UNSUPPORTED",
          "unsupported deterministic function was not refused at bind time");

  auto external = Proof(idx::ExpressionIndexOperationKind::function_call,
                        "fn.upper_ascii");
  external.reads_external_resource = true;
  external.resource_epoch_bound = false;
  auto unbound_resource = BindRequest(
      "upper(name)",
      Function("fn.upper_ascii", {Column("Name")}),
      text_desc,
      {external});
  refused = idx::BindExpressionIndexExtractorDescriptor(&cache, unbound_resource);
  Require(!refused.ok() &&
              refused.diagnostic.diagnostic_code ==
                  "SB-INDEX-EXPR-RESOURCE-PROOF-MISSING",
          "external-resource function without epoch proof was not exactly refused");
}

idx::ExpressionIndexBindResult Bound(
    idx::ExpressionIndexBindRequest request,
    std::string_view label) {
  idx::ExpressionIndexDescriptorCache cache;
  auto result = idx::BindExpressionIndexExtractorDescriptor(&cache, request);
  Require(result.ok(), label);
  return result;
}

void RequireKeyIsSbko(const idx::ExpressionIndexExtractedRow& row,
                      std::string_view label) {
  Require(row.extracted && row.key.ok(), label);
  const std::string key = BytesView(row.key.encoded);
  Require(idx::IsOrderPreservingIndexKeyEncoding(key),
          "expression key did not use SBKO envelope");
  const auto validation = idx::ValidateExpressionIndexExtractorKeyEnvelope(key);
  Require(validation.ok(), "extractor rejected its own SBKO key envelope");
}

void RequireBatchExtraction() {
  const auto int_desc = Descriptor("bigint", 0x31);
  const auto text_desc = Descriptor("text", 0x41, true, true);

  auto literal = Bound(BindRequest("5", Literal("bigint", "5"), int_desc),
                       "literal descriptor bind failed");
  idx::ExpressionIndexBatchExtractionRequest extract_literal;
  extract_literal.descriptor = literal.descriptor;
  extract_literal.rows = {Row("literal-row", {})};
  auto batch = idx::ExtractExpressionIndexKeyBatch(extract_literal);
  Require(batch.ok() && batch.key_count == 1 && batch.error_count == 0,
          "literal expression key extraction failed");
  RequireKeyIsSbko(batch.rows.front(), "literal row key missing");

  auto column = Bound(BindRequest("name", Column("Name"), text_desc),
                      "column descriptor bind failed");
  idx::ExpressionIndexBatchExtractionRequest extract_column;
  extract_column.descriptor = column.descriptor;
  extract_column.rows = {
      Row("column-row", {Field("Name", 0, Value("text", "alpha"))})};
  batch = idx::ExtractExpressionIndexKeyBatch(extract_column);
  Require(batch.ok() && batch.key_count == 1 &&
              batch.rows.front().value.encoded_value == "alpha",
          "column reference key extraction failed");
  RequireKeyIsSbko(batch.rows.front(), "column row key missing");

  auto add = Bound(
      BindRequest("amount + 3",
                  Operator("op_add", {Column("amount"), Literal("bigint", "3")}),
                  int_desc,
                  {Proof(idx::ExpressionIndexOperationKind::operator_call,
                         "op_add")}),
      "operator descriptor bind failed");
  idx::ExpressionIndexBatchExtractionRequest extract_add;
  extract_add.descriptor = add.descriptor;
  extract_add.rows = {
      Row("op-row", {Field("amount", 0, Value("bigint", "4"))}),
      Row("op-null-row", {Field("amount", 0, Value("bigint", "", true))})};
  batch = idx::ExtractExpressionIndexKeyBatch(extract_add);
  Require(batch.ok() && batch.key_count == 2 && batch.null_count == 1 &&
              batch.rows[0].value.encoded_value == "7" &&
              batch.rows[1].expression_null,
          "operator/null expression extraction failed");
  RequireKeyIsSbko(batch.rows[0], "operator row key missing");
  RequireKeyIsSbko(batch.rows[1], "null row key missing");

  auto upper = Bound(
      BindRequest("upper(name)",
                  Function("fn.upper_ascii", {Column("Name")}),
                  text_desc,
                  {Proof(idx::ExpressionIndexOperationKind::function_call,
                         "fn.upper_ascii")}),
      "function descriptor bind failed");
  idx::ExpressionIndexBatchExtractionRequest extract_upper;
  extract_upper.descriptor = upper.descriptor;
  extract_upper.rows = {
      Row("function-row", {Field("Name", 0, Value("text", "alpha"))})};
  batch = idx::ExtractExpressionIndexKeyBatch(extract_upper);
  Require(batch.ok() && batch.key_count == 1 &&
              batch.rows.front().value.encoded_value == "ALPHA",
          "function key extraction failed");
  RequireKeyIsSbko(batch.rows.front(), "function row key missing");

  auto div = Bound(
      BindRequest("amount / 0",
                  Operator("op_div", {Column("amount"), Literal("bigint", "0")}),
                  int_desc,
                  {Proof(idx::ExpressionIndexOperationKind::operator_call,
                         "op_div")}),
      "division descriptor bind failed");
  idx::ExpressionIndexBatchExtractionRequest extract_div;
  extract_div.descriptor = div.descriptor;
  extract_div.rows = {
      Row("error-row", {Field("amount", 0, Value("bigint", "4"))})};
  batch = idx::ExtractExpressionIndexKeyBatch(extract_div);
  Require(batch.ok() && batch.error_count == 1 && batch.key_count == 0,
          "expression error row did not remain row-scoped");
  Require(batch.rows.front().expression_error &&
              batch.rows.front().diagnostic.diagnostic_code ==
                  "SB-INDEX-EXPR-DIVIDE-BY-ZERO",
          "divide-by-zero diagnostic was not exact");

  const std::vector<platform::byte> legacy = {'S', 'B', 'K', '1', 1, 0, 0, 0};
  const std::string legacy_key = BytesView(legacy);
  const auto legacy_validation =
      idx::ValidateExpressionIndexExtractorKeyEnvelope(legacy_key);
  Require(!legacy_validation.ok() &&
              legacy_validation.diagnostic.diagnostic_code ==
                  "SB-INDEX-EXPR-LEGACY-KEY-REFUSED",
          "unsafe legacy expression key was not exactly refused");

  RequireNoDocRuntimeEvidence(literal.evidence);
  RequireNoDocRuntimeEvidence(literal.descriptor.evidence);
  RequireNoDocRuntimeEvidence(batch.evidence);
  for (const auto& row : batch.rows) {
    RequireNoDocRuntimeEvidence(row.evidence);
  }
}

}  // namespace

int main() {
  RequireCanonicalDigest();
  RequireDeterminismAndCache();
  RequireBatchExtraction();
  std::cout << "expression_index_extractor_gate=passed\n";
  return 0;
}
