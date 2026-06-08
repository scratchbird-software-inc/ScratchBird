// Copyright (c) 2026 ScratchBird Software Inc.
//
// This Source Code Form is subject to the terms of the Mozilla Public
// License, v. 2.0. If a copy of the MPL was not distributed with this
// file, You can obtain one at https://mozilla.org/MPL/2.0/.
//
// SPDX-License-Identifier: MPL-2.0

#include "descriptor_value_runtime.hpp"

#include <iostream>
#include <string>

namespace {

bool Require(bool condition, const std::string& message) {
  if (!condition) {
    std::cerr << "FAIL: " << message << "\n";
    return false;
  }
  return true;
}

}  // namespace

int main() {
  using namespace scratchbird::engine::executor;

  const auto int64_descriptor = MakeExecutorDescriptor("int64");
  const auto text_descriptor = MakeExecutorDescriptor("text");
  const auto bool_descriptor = MakeExecutorDescriptor("boolean");
  DescriptorBatch batch = MakeDescriptorBatch(
      {{"id", int64_descriptor, false}, {"name", text_descriptor, false}, {"active", bool_descriptor, true}},
      {{{EncodeInt64Value(7), EncodeTextValue("alpha"), EncodeBoolValue(true)}},
       {{EncodeInt64Value(8), EncodeTextValue("beta"), MakeExecutorValue(bool_descriptor, "", true)}}});

  bool ok = true;
  const auto validation = ValidateDescriptorBatch(batch);
  ok &= Require(validation.ok, "descriptor batch validates");
  ok &= Require(!DescriptorFingerprint(batch.columns).empty(), "descriptor fingerprint produced");
  const auto name_column = FindColumnByStableName(batch, "name");
  ok &= Require(name_column.has_value() && *name_column == 1, "stable-name lookup works");
  const auto projected = ProjectDescriptorBatch(batch, {1, 0});
  ok &= Require(projected.columns.size() == 2 && projected.rows.size() == 2, "projection preserves descriptor rows");
  ok &= Require(projected.columns[0].stable_name == "name", "projection column order retained");

  DescriptorRuntimeDiagnostic relation_diagnostic;
  const auto filtered = FilterDescriptorInt64GreaterThan(batch, 0, 7, &relation_diagnostic);
  ok &= Require(relation_diagnostic.ok && filtered.rows.size() == 1, "descriptor filter works");
  ok &= Require(DecodeInt64Value(filtered.rows[0].values[0]).value == 8, "descriptor filter retains expected row");

  const auto sorted_desc = SortDescriptorBatchByColumn(batch, 0, false, &relation_diagnostic);
  ok &= Require(relation_diagnostic.ok && sorted_desc.rows.size() == 2, "descriptor sort works");
  ok &= Require(DecodeInt64Value(sorted_desc.rows[0].values[0]).value == 8, "descriptor sort order works");

  const auto limited = LimitOffsetDescriptorBatch(sorted_desc, 1, 1);
  ok &= Require(limited.rows.size() == 1, "descriptor limit/offset works");
  ok &= Require(DecodeInt64Value(limited.rows[0].values[0]).value == 7, "descriptor limit/offset retains expected row");

  DescriptorBatch set_rhs = MakeDescriptorBatch(
      batch.columns,
      {{{EncodeInt64Value(8), EncodeTextValue("beta"), MakeExecutorValue(bool_descriptor, "", true)}},
       {{EncodeInt64Value(9), EncodeTextValue("gamma"), EncodeBoolValue(false)}}});
  const auto unioned = SetUnionDistinctDescriptorBatch(batch, set_rhs, &relation_diagnostic);
  ok &= Require(relation_diagnostic.ok && unioned.rows.size() == 3, "descriptor union distinct works");
  const auto intersected = SetIntersectDistinctDescriptorBatch(batch, set_rhs, &relation_diagnostic);
  ok &= Require(relation_diagnostic.ok && intersected.rows.size() == 1, "descriptor intersect distinct works");
  ok &= Require(DecodeInt64Value(intersected.rows[0].values[0]).value == 8, "descriptor intersect retains expected row");
  const auto excepted = SetExceptDistinctDescriptorBatch(batch, set_rhs, &relation_diagnostic);
  ok &= Require(relation_diagnostic.ok && excepted.rows.size() == 1, "descriptor except distinct works");
  ok &= Require(DecodeInt64Value(excepted.rows[0].values[0]).value == 7, "descriptor except retains expected row");

  DescriptorBatch join_rhs = MakeDescriptorBatch(
      {{"owner_id", int64_descriptor, false}, {"owner_name", text_descriptor, false}},
      {{{EncodeInt64Value(8), EncodeTextValue("owner-beta")}},
       {{EncodeInt64Value(10), EncodeTextValue("owner-delta")}}});
  const auto joined = JoinDescriptorBatchesOnInt64(batch, join_rhs, 0, 0, &relation_diagnostic);
  ok &= Require(relation_diagnostic.ok && joined.rows.size() == 1, "descriptor int64 join works");
  ok &= Require(joined.columns.size() == 5, "descriptor join output shape works");
  ok &= Require(DecodeInt64Value(joined.rows[0].values[0]).value == 8, "descriptor join retained left row");

  DescriptorBatch aggregate_input = MakeDescriptorBatch(
      {{"bucket", int64_descriptor, false}, {"value", text_descriptor, false}},
      {{{EncodeInt64Value(2), EncodeTextValue("a")}},
       {{EncodeInt64Value(2), EncodeTextValue("b")}},
       {{EncodeInt64Value(3), EncodeTextValue("c")}}});
  const auto grouped = AggregateDescriptorCountByInt64(aggregate_input, 0, "row_count", &relation_diagnostic);
  ok &= Require(relation_diagnostic.ok && grouped.rows.size() == 2, "descriptor count aggregate works");
  ok &= Require(DecodeInt64Value(grouped.rows[0].values[0]).value == 2 &&
                DecodeInt64Value(grouped.rows[0].values[1]).value == 2,
                "descriptor count aggregate retains expected first group");

  const auto row_numbered = WindowDescriptorRowNumberByInt64(batch, 0, "row_number", false, &relation_diagnostic);
  ok &= Require(relation_diagnostic.ok && row_numbered.rows.size() == 2, "descriptor row_number window works");
  ok &= Require(row_numbered.columns.size() == 4, "descriptor row_number output shape works");
  ok &= Require(DecodeInt64Value(row_numbered.rows[0].values[0]).value == 8 &&
                DecodeInt64Value(row_numbered.rows[0].values[3]).value == 1,
                "descriptor row_number order works");

  const auto add_result = EvaluateDescriptorExpression(
      DescriptorExpressionOperator::kInt64Add, EncodeInt64Value(11), EncodeInt64Value(4), &relation_diagnostic);
  ok &= Require(relation_diagnostic.ok && DecodeInt64Value(add_result).value == 15, "descriptor int64 add expression works");
  const auto gt_result = EvaluateDescriptorExpression(
      DescriptorExpressionOperator::kInt64GreaterThan, EncodeInt64Value(11), EncodeInt64Value(4), &relation_diagnostic);
  ok &= Require(relation_diagnostic.ok && DecodeBoolValue(gt_result).value, "descriptor int64 comparison expression works");
  const auto and_result = EvaluateDescriptorExpression(
      DescriptorExpressionOperator::kBoolAnd, EncodeBoolValue(true), EncodeBoolValue(false), &relation_diagnostic);
  ok &= Require(relation_diagnostic.ok && !DecodeBoolValue(and_result).value, "descriptor bool expression works");

  const auto coalesced = EvaluateDescriptorCoalesce(
      {MakeExecutorValue(text_descriptor, "", true), EncodeTextValue("fallback")}, &relation_diagnostic);
  ok &= Require(relation_diagnostic.ok && coalesced.encoded_value == "fallback", "descriptor coalesce special form works");

  const auto text_from_int = CastDescriptorValue(EncodeInt64Value(42), text_descriptor, &relation_diagnostic);
  ok &= Require(relation_diagnostic.ok && text_from_int.encoded_value == "42", "descriptor cast int64 to text works");
  const auto int_from_text = CastDescriptorValue(EncodeTextValue("42"), int64_descriptor, &relation_diagnostic);
  ok &= Require(relation_diagnostic.ok && DecodeInt64Value(int_from_text).value == 42, "descriptor cast text to int64 works");
  const auto uuid_descriptor = MakeExecutorDescriptor("uuidv7");
  const auto uuid_value = CastDescriptorValue(
      EncodeTextValue("018f46d2-9a85-7cc8-b3a1-8f0cf1c20111"), uuid_descriptor, &relation_diagnostic);
  ok &= Require(relation_diagnostic.ok && uuid_value.descriptor.canonical_type_name == "uuidv7",
                "descriptor cast text to uuid works");
  const auto uuid_text = CastDescriptorValue(uuid_value, text_descriptor, &relation_diagnostic);
  ok &= Require(relation_diagnostic.ok && uuid_text.encoded_value == "018f46d2-9a85-7cc8-b3a1-8f0cf1c20111",
                "descriptor cast uuid to text works");
  const auto document_descriptor = MakeExecutorDescriptor("document");
  DescriptorBatch document_batch = MakeDescriptorBatch(
      {{"payload", document_descriptor, false}},
      {{{MakeExecutorValue(document_descriptor, "{\"ok\":true}", false)}}});
  ok &= Require(ValidateDescriptorBatch(document_batch).ok, "descriptor document encoded value validates");
  const auto vector_descriptor = MakeExecutorDescriptor("vector");
  DescriptorBatch vector_batch = MakeDescriptorBatch(
      {{"embedding", vector_descriptor, false}},
      {{{MakeExecutorValue(vector_descriptor, "[0.1,0.2,0.3]", false)}}});
  ok &= Require(ValidateDescriptorBatch(vector_batch).ok, "descriptor vector encoded value validates");
  const auto blob_descriptor = MakeExecutorDescriptor("blob");
  DescriptorBatch blob_batch = MakeDescriptorBatch(
      {{"payload_blob", blob_descriptor, false}},
      {{{MakeExecutorValue(blob_descriptor, "hex:01020304", false)}}});
  ok &= Require(ValidateDescriptorBatch(blob_batch).ok, "descriptor blob encoded value validates");

  const auto timestamp_descriptor = MakeExecutorDescriptor("timestamp");
  const auto extracted_year = ExtractDescriptorField(
      MakeExecutorValue(timestamp_descriptor, "2026-05-06T12:34:56", false), "year", &relation_diagnostic);
  ok &= Require(relation_diagnostic.ok && DecodeInt64Value(extracted_year).value == 2026, "descriptor extract year works");
  const auto extracted_second = ExtractDescriptorField(
      MakeExecutorValue(timestamp_descriptor, "2026-05-06T12:34:56", false), "second", &relation_diagnostic);
  ok &= Require(relation_diagnostic.ok && DecodeInt64Value(extracted_second).value == 56, "descriptor extract second works");

  DescriptorRuntimeSetScope set_scope;
  SetDescriptorRuntimeVariable(&set_scope, "work_mem_bytes", EncodeInt64Value(65536));
  const auto set_value = GetDescriptorRuntimeVariable(set_scope, "work_mem_bytes");
  ok &= Require(set_value.has_value() && DecodeInt64Value(*set_value).value == 65536, "descriptor SET variable support works");

  DescriptorDomainPolicy age_domain;
  age_domain.domain_stable_name = "positive_age";
  age_domain.base_descriptor = int64_descriptor;
  age_domain.nullable = false;
  age_domain.min_int64 = 0;
  age_domain.max_int64 = 130;
  ok &= Require(ValidateDescriptorDomainValue(age_domain, EncodeInt64Value(42)).ok,
                "descriptor domain validates accepted int64");
  ok &= Require(ValidateDescriptorDomainValue(age_domain, EncodeInt64Value(-1)).diagnostic_code ==
                    "SB_EXECUTOR_DOMAIN_MIN_VIOLATION",
                "descriptor domain rejects min violation");
  const auto validate_method = EvaluateDescriptorDomainMethod(
      age_domain, "validate", EncodeInt64Value(42), "", &relation_diagnostic);
  ok &= Require(relation_diagnostic.ok && DecodeBoolValue(validate_method).value,
                "descriptor domain validate method works");

  DescriptorDomainPolicy secret_text_domain;
  secret_text_domain.domain_stable_name = "masked_secret";
  secret_text_domain.base_descriptor = text_descriptor;
  secret_text_domain.nullable = false;
  secret_text_domain.max_text_bytes = 32;
  secret_text_domain.mask_kind = DescriptorDomainMaskKind::kRevealLast4;
  secret_text_domain.required_security_token = "can_view_secret";
  const auto masked_secret = ApplyDescriptorDomainMask(
      secret_text_domain, EncodeTextValue("account-1234"), "", &relation_diagnostic);
  ok &= Require(relation_diagnostic.ok && masked_secret.encoded_value == "********1234",
                "descriptor domain masking works");
  const auto unmasked_secret = ApplyDescriptorDomainMask(
      secret_text_domain, EncodeTextValue("account-1234"), "can_view_secret", &relation_diagnostic);
  ok &= Require(relation_diagnostic.ok && unmasked_secret.encoded_value == "account-1234",
                "descriptor domain security hook permits unmasked value");
  const auto visible_method = EvaluateDescriptorDomainMethod(
      secret_text_domain, "is_visible", EncodeTextValue("account-1234"), "", &relation_diagnostic);
  ok &= Require(relation_diagnostic.ok && !DecodeBoolValue(visible_method).value,
                "descriptor domain visibility method works");

  const auto decoded_int = DecodeInt64Value(batch.rows[0].values[0]);
  ok &= Require(decoded_int.ok() && decoded_int.value == 7, "int64 decode works");
  const auto decoded_bool = DecodeBoolValue(batch.rows[0].values[2]);
  ok &= Require(decoded_bool.ok() && decoded_bool.value, "bool decode works");

  DescriptorBatch bad_width = batch;
  bad_width.rows[0].values.pop_back();
  ok &= Require(!ValidateDescriptorBatch(bad_width).ok, "bad row width rejected");

  DescriptorBatch null_violation = batch;
  null_violation.rows[0].values[0] = MakeExecutorValue(int64_descriptor, "", true);
  ok &= Require(ValidateDescriptorBatch(null_violation).diagnostic_code == "SB_EXECUTOR_NULL_NOT_ALLOWED",
                "non-nullable null rejected");

  DescriptorBatch bad_decode = batch;
  bad_decode.rows[0].values[0] = MakeExecutorValue(int64_descriptor, "not-int", false);
  ok &= Require(ValidateDescriptorBatch(bad_decode).diagnostic_code == "SB_EXECUTOR_INT64_DECODE_FAILED",
                "bad int64 encoding rejected");

  std::cout << "{\n"
            << "  \"ok\": " << (ok ? "true" : "false") << ",\n"
            << "  \"rows\": " << batch.rows.size() << ",\n"
            << "  \"columns\": " << batch.columns.size() << "\n"
            << "}\n";
  return ok ? 0 : 1;
}
