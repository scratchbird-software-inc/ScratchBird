// Copyright (c) 2026 ScratchBird Software Inc.
//
// This Source Code Form is subject to the terms of the Mozilla Public
// License, v. 2.0. If a copy of the MPL was not distributed with this
// file, You can obtain one at https://mozilla.org/MPL/2.0/.
//
// SPDX-License-Identifier: MPL-2.0

#include "sblr_dispatch.hpp"
#include "sblr_engine_envelope.hpp"

#include "datatype_operations.hpp"

#include <iostream>
#include <string>
#include <utility>
#include <vector>

using namespace scratchbird::engine::internal_api;
using namespace scratchbird::engine::sblr;

namespace {

EngineDescriptor Descriptor(std::string type_name, std::string kind = "scalar") {
  EngineDescriptor descriptor;
  descriptor.descriptor_kind = std::move(kind);
  descriptor.canonical_type_name = std::move(type_name);
  return descriptor;
}

EngineDescriptor DomainDescriptor() {
  EngineDescriptor descriptor;
  descriptor.descriptor_kind = "domain";
  descriptor.canonical_type_name = "positive_int_domain";
  descriptor.encoded_descriptor = "domain_uuid=00000000-0000-7000-8000-00000000d011;base_type=int32";
  return descriptor;
}

EngineRequestContext Context(bool security_context_present = true) {
  EngineRequestContext context;
  context.trust_mode = EngineTrustMode::embedded_in_process;
  context.security_context_present = security_context_present;
  context.request_id = "sblr-datatype-full-surface-probe";
  return context;
}

SblrDispatchResult Dispatch(std::string operation_id,
                            std::string opcode,
                            EngineApiRequest api_request,
                            bool security_context_present = true,
                            bool envelope_requires_security = true) {
  SblrDispatchRequest request;
  request.context = Context(security_context_present);
  request.envelope = MakeSblrEnvelope(std::move(operation_id), std::move(opcode), "datatype-full-surface");
  request.envelope.requires_security_context = envelope_requires_security;
  request.api_request = std::move(api_request);
  return DispatchSblrOperation(request);
}

EngineApiRequest CastRequest() {
  EngineApiRequest request;
  request.descriptors.push_back(Descriptor("character"));
  request.descriptors.push_back(Descriptor("int32"));
  EngineRowValue row;
  row.fields.push_back({"value", {Descriptor("character"), "42", false}});
  request.rows.push_back(row);
  return request;
}

EngineApiRequest ExtractRequest() {
  EngineApiRequest request;
  request.descriptors.push_back(Descriptor("timestamp"));
  EngineRowValue row;
  row.fields.push_back({"value", {Descriptor("timestamp"), "2026-05-01T12:34:56", false}});
  request.rows.push_back(row);
  request.option_envelopes.push_back("field:year");
  return request;
}

EngineApiRequest SetRequest() {
  namespace dt = scratchbird::core::datatypes;
  dt::DatatypeSetDescriptor descriptor;
  descriptor.element_type_id = dt::CanonicalTypeId::character;
  const auto encoded = dt::EncodeSetValue(descriptor,
                                          {{dt::CanonicalTypeId::character, "alpha", false},
                                           {dt::CanonicalTypeId::character, "beta", false}});
  EngineApiRequest request;
  request.descriptors.push_back(Descriptor("character"));
  EngineRowValue row;
  row.fields.push_back({"value", {Descriptor("set_value"), encoded.encoded_set, false}});
  request.rows.push_back(row);
  request.predicate.bound_values.push_back({Descriptor("set_value"), encoded.encoded_set, false});
  request.predicate.bound_values.push_back({Descriptor("character"), "beta", false});
  request.option_envelopes.push_back("set_operation:membership");
  return request;
}

EngineApiRequest NumericRequest() {
  EngineApiRequest request;
  request.descriptors.push_back(Descriptor("decimal"));
  EngineTypedValue left{Descriptor("decimal"), "1.50", false};
  EngineTypedValue right{Descriptor("decimal"), "2.25", false};
  EngineRowValue row;
  row.fields.push_back({"left", left});
  row.fields.push_back({"right", right});
  request.rows.push_back(row);
  request.predicate.bound_values.push_back(left);
  request.predicate.bound_values.push_back(right);
  request.option_envelopes.push_back("numeric_operation:add");
  request.option_envelopes.push_back("precision:38");
  request.option_envelopes.push_back("scale:2");
  request.option_envelopes.push_back("rounding:half_even");
  return request;
}

EngineApiRequest DocumentRequest() {
  EngineApiRequest request;
  request.descriptors.push_back(Descriptor("document"));
  EngineRowValue row;
  row.fields.push_back({"value", {Descriptor("document"), "{\"b\":2,\"a\":1}", false}});
  request.rows.push_back(row);
  request.option_envelopes.push_back("document_donor_profile:native_json");
  return request;
}

EngineApiRequest AdvancedFamilyRequest() {
  EngineApiRequest request;
  request.descriptors.push_back(Descriptor("dense_vector"));
  request.option_envelopes.push_back("advanced_operation:nearest_neighbor");
  request.option_envelopes.push_back("advanced_index:hnsw");
  request.option_envelopes.push_back("vector_dimension:3");
  return request;
}

EngineApiRequest DomainValidateRequest() {
  EngineApiRequest request;
  request.descriptors.push_back(DomainDescriptor());
  EngineRowValue row;
  row.fields.push_back({"value", {Descriptor("int32"), "7", false}});
  request.rows.push_back(row);
  return request;
}

EngineApiRequest DomainMethodRequest() {
  EngineApiRequest request;
  request.descriptors.push_back(DomainDescriptor());
  EngineRowValue row;
  row.fields.push_back({"value", {Descriptor("character"), "Alpha", false}});
  request.rows.push_back(row);
  request.option_envelopes.push_back("method:upper");
  return request;
}

bool HasApiDiagnosticDetail(const EngineApiResult& result, const std::string& text) {
  for (const auto& diagnostic : result.diagnostics) {
    if (diagnostic.detail.find(text) != std::string::npos ||
        diagnostic.code.find(text) != std::string::npos) {
      return true;
    }
  }
  return false;
}

bool HasDispatchDiagnostic(const SblrDispatchResult& result, const std::string& code) {
  for (const auto& diagnostic : result.diagnostics) {
    if (diagnostic.code == code) { return true; }
  }
  return false;
}

bool RuntimeClosureOk() {
  namespace dt = scratchbird::core::datatypes;
  const dt::DatatypeOperationValue lower{dt::CanonicalTypeId::character, "alpha", false};
  const dt::DatatypeOperationValue upper{dt::CanonicalTypeId::character, "ALPHA", false};
  dt::DatatypeTextSeedAuthority text_seed;
  text_seed.active = true;
  text_seed.seed_pack_name = "initial-resource-pack";
  text_seed.seed_pack_version = "probe-runtime-authority";
  text_seed.charset_name = "UTF8";
  text_seed.collation_name = "unicode_ci";
  text_seed.collation_case_insensitive = true;
  dt::DatatypeComparisonRequest compare_request{lower, upper, dt::DatatypeNullOrdering::nulls_first, true};
  compare_request.text_seed = text_seed;
  const auto compare = dt::CompareDatatypeValues(compare_request);
  const auto missing_seed_compare = dt::CompareDatatypeValues(
      {lower, upper, dt::DatatypeNullOrdering::nulls_first, true});
  const auto null_compare = dt::CompareDatatypeValues({{dt::CanonicalTypeId::int32, "", true},
                                                       {dt::CanonicalTypeId::int32, "1", false},
                                                       dt::DatatypeNullOrdering::nulls_last,
                                                       false});
  const auto int128_compare = dt::CompareDatatypeValues(
      {{dt::CanonicalTypeId::int128, "170141183460469231731687303715884105727", false},
       {dt::CanonicalTypeId::int128, "42", false},
       dt::DatatypeNullOrdering::nulls_first,
       false});
  dt::DatatypeSortKeyRequest sort_key_request{lower, dt::DatatypeNullOrdering::nulls_first, true};
  sort_key_request.text_seed = text_seed;
  const auto sort_key = dt::MakeDatatypeSortKey(sort_key_request);
  const auto hash_a = dt::HashDatatypeValue({lower});
  const auto hash_b = dt::HashDatatypeValue({lower});
  const auto serialized = dt::SerializeDatatypeValue({{dt::CanonicalTypeId::uuid,
                                                       "018f7f8f-7c00-7000-8000-000000000001",
                                                       false}});
  const auto deserialized = dt::DeserializeDatatypeValue({dt::CanonicalTypeId::uuid, serialized.serialized_value});
  const auto opaque_compare = dt::CompareDatatypeValues({{dt::CanonicalTypeId::opaque_extension, "opaque", false},
                                                         {dt::CanonicalTypeId::opaque_extension, "opaque", false},
                                                         dt::DatatypeNullOrdering::nulls_first,
                                                         false});
  return compare.ok() && compare.comparison == 0 &&
         !missing_seed_compare.ok() &&
         null_compare.ok() && null_compare.comparison == 1 &&
         int128_compare.ok() && int128_compare.comparison == 1 &&
         sort_key.ok() && !sort_key.sort_key.empty() &&
         hash_a.ok() && hash_b.ok() && hash_a.stable_hash_hex == hash_b.stable_hash_hex &&
         serialized.ok() && deserialized.ok() &&
         deserialized.value.encoded_value == "018f7f8f-7c00-7000-8000-000000000001" &&
         !opaque_compare.ok();
}

void PrintBool(const std::string& name, bool value, bool comma = true) {
  std::cout << "  \"" << name << "\": " << (value ? "true" : "false")
            << (comma ? "," : "") << "\n";
}

}  // namespace

int main() {
  const auto cast = Dispatch("query.cast_value", "SBLR_QUERY_CAST_VALUE", CastRequest());
  const auto extract = Dispatch("query.extract_value", "SBLR_QUERY_EXTRACT_VALUE", ExtractRequest());
  const auto set_operation = Dispatch("query.set_operation", "SBLR_QUERY_SET_OPERATION", SetRequest());
  const auto numeric_operation = Dispatch("query.apply_numeric_operation",
                                         "SBLR_QUERY_APPLY_NUMERIC_OPERATION",
                                         NumericRequest());
  const auto document_canonicalize = Dispatch("query.canonicalize_document_value",
                                              "SBLR_QUERY_CANONICALIZE_DOCUMENT_VALUE",
                                              DocumentRequest());
  const auto advanced_family = Dispatch("query.evaluate_advanced_datatype_family",
                                        "SBLR_QUERY_EVALUATE_ADVANCED_DATATYPE_FAMILY",
                                        AdvancedFamilyRequest());
  const auto domain_validate = Dispatch("query.validate_domain_value",
                                        "SBLR_QUERY_VALIDATE_DOMAIN_VALUE",
                                        DomainValidateRequest());
  const auto domain_method = Dispatch("query.invoke_domain_method",
                                      "SBLR_QUERY_INVOKE_DOMAIN_METHOD",
                                      DomainMethodRequest());
  const auto security_required = Dispatch("query.cast_value",
                                          "SBLR_QUERY_CAST_VALUE",
                                          CastRequest(),
                                          false,
                                          true);
  const auto unknown = Dispatch("query.datatype_unknown",
                                "SBLR_QUERY_DATATYPE_UNKNOWN",
                                EngineApiRequest{});

  const bool cast_ok = cast.accepted && cast.dispatched_to_api && cast.api_result.ok &&
                       !cast.api_result.result_shape.columns.empty();
  const bool extract_ok = extract.accepted && extract.dispatched_to_api && extract.api_result.ok &&
                          extract.api_result.result_shape.result_kind == "typed_value";
  const bool set_ok = set_operation.accepted && set_operation.dispatched_to_api &&
                      set_operation.api_result.ok &&
                      !set_operation.api_result.result_shape.columns.empty();
  const bool numeric_ok = numeric_operation.accepted && numeric_operation.dispatched_to_api &&
                          numeric_operation.api_result.ok &&
                          numeric_operation.api_result.result_shape.result_kind == "typed_value";
  const bool document_ok = document_canonicalize.accepted && document_canonicalize.dispatched_to_api &&
                           document_canonicalize.api_result.ok &&
                           document_canonicalize.api_result.result_shape.result_kind == "typed_value";
  const bool advanced_ok = advanced_family.accepted && advanced_family.dispatched_to_api &&
                           advanced_family.api_result.ok &&
                           advanced_family.api_result.result_shape.result_kind == "datatype_family_evaluation";
  const bool domain_fail_closed = domain_validate.accepted && domain_validate.dispatched_to_api &&
                                  !domain_validate.api_result.ok &&
                                  HasApiDiagnosticDetail(domain_validate.api_result, "domain_not_visible");
  const bool domain_method_fail_closed = domain_method.accepted && domain_method.dispatched_to_api &&
                                         !domain_method.api_result.ok &&
                                         HasApiDiagnosticDetail(domain_method.api_result, "domain_not_visible");
  const bool security_fail_closed = !security_required.accepted &&
                                    !security_required.api_result.ok &&
                                    HasDispatchDiagnostic(security_required,
                                                         "SB_SBLR_DISPATCH_SECURITY_CONTEXT_REQUIRED");
  const bool unknown_fail_closed = !unknown.accepted && !unknown.dispatched_to_api &&
                                   !unknown.api_result.ok &&
                                   HasDispatchDiagnostic(unknown, "SB_SBLR_DISPATCH_UNKNOWN_OPERATION");
  const bool result_shapes_ok = cast.api_result.result_shape.result_kind == "typed_value" &&
                                extract.api_result.result_shape.result_kind == "typed_value" &&
                                set_operation.api_result.result_shape.result_kind == "typed_value" &&
                                numeric_operation.api_result.result_shape.result_kind == "typed_value" &&
                                document_canonicalize.api_result.result_shape.result_kind == "typed_value" &&
                                advanced_family.api_result.result_shape.result_kind == "datatype_family_evaluation";
  const bool runtime_closure_ok = RuntimeClosureOk();
  const bool ok = cast_ok && extract_ok && set_ok && numeric_ok && document_ok && advanced_ok &&
                  domain_fail_closed && domain_method_fail_closed &&
                  security_fail_closed && unknown_fail_closed && result_shapes_ok && runtime_closure_ok;

  std::cout << "{\n";
  PrintBool("ok", ok);
  PrintBool("cast_dispatch_ok", cast_ok);
  PrintBool("extract_dispatch_ok", extract_ok);
  PrintBool("set_operation_dispatch_ok", set_ok);
  PrintBool("numeric_operation_dispatch_ok", numeric_ok);
  PrintBool("document_canonicalize_dispatch_ok", document_ok);
  PrintBool("advanced_family_dispatch_ok", advanced_ok);
  PrintBool("domain_validate_fail_closed", domain_fail_closed);
  PrintBool("domain_method_fail_closed", domain_method_fail_closed);
  PrintBool("security_fail_closed", security_fail_closed);
  PrintBool("unknown_operation_fail_closed", unknown_fail_closed);
  PrintBool("result_shapes_ok", result_shapes_ok);
  PrintBool("runtime_closure_ok", runtime_closure_ok, false);
  std::cout << "}\n";
  return ok ? 0 : 1;
}
