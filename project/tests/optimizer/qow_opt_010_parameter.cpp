// Copyright (c) 2026 ScratchBird Software Inc.
// SPDX-License-Identifier: MPL-2.0

#define main qow_opt_009_parameter_fixture_main
#include "qow_opt_009.cpp"
#undef main

namespace {

bool RequireParameter010(const bool condition, const std::string_view detail) {
  if (!condition) {
    std::cerr << "QOW-TEST-OPT-010-PARAMETER-V1: " << detail << '\n';
  }
  return condition;
}

bool ExactParameterIssue010(
    const cache::CanonicalExecutablePlanParameterBindResult& result,
    const std::string_view field) {
  return !result.accepted && result.issues.size() == 1 &&
         result.issues.front().diagnostic_id ==
             "QOW-DIAG-OPT-010-PARAMETER-REFUSAL-V1" &&
         result.issues.front().field_id == field &&
         result.issues.front().stable_code ==
             "QOW-DIAG-OPT-010-PARAMETER-REFUSAL-V1" &&
         result.issues.front().phase == "execute" &&
         result.issues.front().transaction_effect ==
             "statement_failed_transaction_usable";
}

bool ValidateParameterDecisionMatrix010() {
  Fixture009 fixture;
  if (!RequireParameter010(fixture.ready, "fixture admission failed")) {
    return false;
  }
  const auto valid = cache::CanonicalExecutablePlanParameterBinding{
      fixture.prepared_plan->parameters.front(), &fixture.parameter_value};
  bool passed = true;

  passed &= RequireParameter010(
      ExactParameterIssue010(
          cache::ValidateCanonicalExecutablePlanParameterBindings(
              {}, *fixture.prepared_plan),
          "missing"),
      "missing binding did not win refusal precedence");

  auto extra = valid;
  extra.descriptor.ordinal = 99;
  passed &= RequireParameter010(
      ExactParameterIssue010(
          cache::ValidateCanonicalExecutablePlanParameterBindings(
              {valid, extra}, *fixture.prepared_plan),
          "extra"),
      "extra binding was not refused exactly");
  passed &= RequireParameter010(
      ExactParameterIssue010(
          cache::ValidateCanonicalExecutablePlanParameterBindings(
              {extra}, *fixture.prepared_plan),
          "missing"),
      "missing did not dominate extra");

  passed &= RequireParameter010(
      ExactParameterIssue010(
          cache::ValidateCanonicalExecutablePlanParameterBindings(
              {valid, valid}, *fixture.prepared_plan),
          "conflicting_repeated"),
      "duplicate binding was not a conflicting repeated refusal");

  auto null_value = fixture.parameter_value;
  null_value.setState(api::EngineValueState::sql_null);
  null_value.encoded_value.clear();
  null_value.binary_value.clear();
  const auto null_binding = cache::CanonicalExecutablePlanParameterBinding{
      fixture.prepared_plan->parameters.front(), &null_value};
  passed &= RequireParameter010(
      ExactParameterIssue010(
          cache::ValidateCanonicalExecutablePlanParameterBindings(
              {null_binding}, *fixture.prepared_plan),
          "non_nullable_null"),
      "non-nullable NULL was not refused exactly");

  auto wrong = valid;
  auto wrong_value = fixture.parameter_value;
  wrong_value.descriptor.encoded_descriptor += ";wrong=true";
  wrong.typed_value = &wrong_value;
  passed &= RequireParameter010(
      ExactParameterIssue010(
          cache::ValidateCanonicalExecutablePlanParameterBindings(
              {wrong}, *fixture.prepared_plan),
          "wrong_type"),
      "wrong type/descriptor was not refused exactly");

  auto nullable_plan = *fixture.prepared_plan;
  nullable_plan.parameters.front().nullable = true;
  nullable_plan.parameters.front().encoded_descriptor =
      "type_uuid=" + nullable_plan.parameters.front().type_uuid +
      ";nullability=nullable";
  null_value.descriptor.encoded_descriptor =
      nullable_plan.parameters.front().encoded_descriptor;
  const auto nullable_binding = cache::CanonicalExecutablePlanParameterBinding{
      nullable_plan.parameters.front(), &null_value};
  const auto nullable = cache::ValidateCanonicalExecutablePlanParameterBindings(
      {nullable_binding}, nullable_plan);
  passed &= RequireParameter010(
      nullable.accepted && nullable.transient_value_count == 1 &&
          !nullable.parameter_values_retained && nullable.issues.empty(),
      "nullable NULL did not bind as one transient value");

  const auto shared_reference =
      cache::ValidateCanonicalExecutablePlanParameterBindings(
          {valid}, *fixture.prepared_plan);
  passed &= RequireParameter010(
      shared_reference.accepted &&
          shared_reference.transient_value_count == 1 &&
          !shared_reference.parameter_values_retained,
      "one shared binding for repeated references was not accepted");
  return passed;
}

bool ValidateParameterRefusalDoesNotInvalidate010() {
  Fixture009 fixture;
  if (!fixture.ready) return false;
  auto lookup = cache::CanonicalExecutablePlanCacheLookupRequest{};
  lookup.current_key = fixture.key;
  lookup.mga_authority = Authority009(FreshStatement009(1510));
  lookup.engine_lookup_authorized = true;
  lookup.engine_security_revalidated = true;
  lookup.engine_policy_revalidated = true;
  lookup.engine_authorization_revalidated = true;
  lookup.authorization_revalidation_receipt_uuid = Uuid(1514);
  const auto refused = fixture.executable_cache.LookupAndBind(lookup);
  const auto valid_binding = cache::CanonicalExecutablePlanParameterBinding{
      fixture.prepared_plan->parameters.front(), &fixture.parameter_value};
  lookup.parameter_bindings = {valid_binding};
  lookup.mga_authority = Authority009(FreshStatement009(1520));
  lookup.authorization_revalidation_receipt_uuid = Uuid(1524);
  const auto accepted = fixture.executable_cache.LookupAndBind(lookup);
  return RequireParameter010(
      !refused.accepted && !refused.reprepare_required &&
          refused.issues.size() == 1 &&
          refused.issues.front().diagnostic_id ==
              "QOW-DIAG-OPT-010-PARAMETER-REFUSAL-V1" &&
          fixture.executable_cache.Size() == 1 && accepted.accepted &&
          accepted.hit && !accepted.entry->parameter_values_retained,
      "parameter refusal invalidated the plan or retained a value");
}

}  // namespace

// QOW-TEST-OPT-010-PARAMETER-V1
int main() {
  return ValidateParameterDecisionMatrix010() &&
                 ValidateParameterRefusalDoesNotInvalidate010()
             ? EXIT_SUCCESS
             : EXIT_FAILURE;
}
