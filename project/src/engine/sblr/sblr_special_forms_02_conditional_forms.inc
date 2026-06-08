// Copyright (c) 2026 ScratchBird Software Inc.
//
// This Source Code Form is subject to the terms of the Mozilla Public
// License, v. 2.0. If a copy of the MPL was not distributed with this
// file, You can obtain one at https://mozilla.org/MPL/2.0/.
//
// SPDX-License-Identifier: MPL-2.0

// SBSQL source-layout fragment. Included only by sblr_special_forms.cpp.
// This fragment preserves pre-existing behavior; do not add unrelated future work here.
    case datatypes::CanonicalTypeId::uint8:
    case datatypes::CanonicalTypeId::uint16:
    case datatypes::CanonicalTypeId::uint32:
    case datatypes::CanonicalTypeId::uint64: {
      out.payload_kind = SblrValuePayloadKind::unsigned_integer;
      std::uint64_t parsed = 0;
      if (ParseUnsignedInt64(value.encoded_value, &parsed)) {
        out.has_uint64_value = true;
        out.uint64_value = parsed;
      }
      out.text_value = value.encoded_value;
      return out;
    }
    case datatypes::CanonicalTypeId::real32:
    case datatypes::CanonicalTypeId::real64: {
      out.payload_kind = SblrValuePayloadKind::real64;
      double parsed = 0.0;
      if (ParseReal64(value.encoded_value, &parsed)) {
        out.has_real64_value = true;
        out.real64_value = parsed;
      }
      out.text_value = value.encoded_value;
      return out;
    }
    case datatypes::CanonicalTypeId::int128:
    case datatypes::CanonicalTypeId::uint128:
    case datatypes::CanonicalTypeId::real128:
    case datatypes::CanonicalTypeId::decimal:
    case datatypes::CanonicalTypeId::decimal_float:
      out.payload_kind = SblrValuePayloadKind::high_precision_numeric_text;
      out.text_value = value.encoded_value;
      return out;
    case datatypes::CanonicalTypeId::character:
      out.payload_kind = SblrValuePayloadKind::text;
      out.text_value = value.encoded_value;
      return out;
    case datatypes::CanonicalTypeId::binary:
    case datatypes::CanonicalTypeId::blob:
      out.payload_kind = SblrValuePayloadKind::binary;
      out.binary_value.assign(value.encoded_value.begin(), value.encoded_value.end());
      return out;
    case datatypes::CanonicalTypeId::uuid:
      out.payload_kind = SblrValuePayloadKind::uuid_text;
      out.text_value = value.encoded_value;
      return out;
    case datatypes::CanonicalTypeId::date:
    case datatypes::CanonicalTypeId::time:
    case datatypes::CanonicalTypeId::timestamp:
    case datatypes::CanonicalTypeId::interval:
      out.payload_kind = SblrValuePayloadKind::temporal_text;
      out.text_value = value.encoded_value;
      return out;
    default:
      out.payload_kind = SblrValuePayloadKind::descriptor_payload;
      out.text_value = value.encoded_value;
      return out;
  }
}

}  // namespace

SblrResult EvaluateSblrCaseSearchedForm(std::string_view operation_id,
                                        const std::vector<SblrCaseBranch>& branches,
                                        const SblrValue& else_value) {
  for (const auto& branch : branches) {
    if (Truthy(branch.condition)) {
      return ScalarResult(operation_id, branch.result);
    }
  }
  return ScalarResult(operation_id, else_value);
}

SblrResult EvaluateSblrCaseSearchedFormLazy(std::string_view operation_id,
                                            const std::vector<SblrCaseThunkBranch>& branches,
                                            SblrExpressionThunk else_value,
                                            const SblrExecutionContext& context) {
  for (const auto& branch : branches) {
    SblrValue condition;
    const auto condition_result = EvaluateThunkScalar(operation_id, context, branch.condition, &condition);
    if (!condition_result.ok()) return condition_result;
    if (Truthy(condition)) {
      SblrValue result;
      const auto branch_result = EvaluateThunkScalar(operation_id, context, branch.result, &result);
      if (!branch_result.ok()) return branch_result;
      return ScalarResult(operation_id, std::move(result));
    }
  }
  if (!else_value) return ScalarResult(operation_id, NullValue());
  SblrValue result;
  const auto else_result = EvaluateThunkScalar(operation_id, context, else_value, &result);
  if (!else_result.ok()) return else_result;
  return ScalarResult(operation_id, std::move(result));
}

SblrResult EvaluateSblrIifForm(std::string_view operation_id,
                               const SblrValue& condition,
                               const SblrValue& true_value,
                               const SblrValue& false_value) {
  return ScalarResult(operation_id, Truthy(condition) ? true_value : false_value);
}

SblrResult EvaluateSblrIifFormLazy(std::string_view operation_id,
                                   SblrExpressionThunk condition,
                                   SblrExpressionThunk true_value,
                                   SblrExpressionThunk false_value,
                                   const SblrExecutionContext& context) {
  SblrValue condition_value;
  const auto condition_result = EvaluateThunkScalar(operation_id, context, condition, &condition_value);
  if (!condition_result.ok()) return condition_result;
  SblrValue result;
  const auto selected_result = EvaluateThunkScalar(operation_id, context, Truthy(condition_value) ? true_value : false_value, &result);
  if (!selected_result.ok()) return selected_result;
  return ScalarResult(operation_id, std::move(result));
}
