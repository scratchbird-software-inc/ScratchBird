// Copyright (c) 2026 ScratchBird Software Inc.
//
// This Source Code Form is subject to the terms of the Mozilla Public
// License, v. 2.0. If a copy of the MPL was not distributed with this
// file, You can obtain one at https://mozilla.org/MPL/2.0/.
//
// SPDX-License-Identifier: MPL-2.0

#include "engine/sblr/canonical_query_aggregate_registration.hpp"
#include "engine/sblr/canonical_query_node_composition.hpp"
#include "engine/sblr/canonical_query_object_free_composition_support.hpp"
#include "engine/sblr/canonical_query_window_registration.hpp"

#include <array>
#include <cstdio>
#include <cstdlib>
#include <iostream>
#include <string>
#include <string_view>

namespace api = scratchbird::engine::internal_api;
namespace exec = scratchbird::engine::executor;
namespace plan = scratchbird::engine::planner;
namespace sblr = scratchbird::engine::sblr;

namespace {

struct Results {
  unsigned cases = 0;
  unsigned failures = 0;
  void Check(bool ok, std::string_view name) {
    ++cases;
    if (!ok) {
      ++failures;
      std::cerr << "window preparation failed: " << name << '\n';
    }
  }
};

std::string Id(unsigned ordinal) {
  char text[37];
  std::snprintf(text, sizeof(text), "019f0000-0000-7200-8000-%012x", ordinal);
  return text;
}

std::string Type(std::string_view name) {
  return sblr::ExactCanonicalCoreDatatypeTypeUuidV1(name);
}

std::array<std::string, 4> SignedTypes() {
  return {Type("int8"), Type("int16"), Type("int32"), Type("int64")};
}

api::RelationalTypeDescriptor Descriptor(unsigned id, std::string_view type,
                                         bool nullable = false) {
  api::RelationalTypeDescriptor result;
  result.descriptor_id = id;
  result.descriptor_uuid = Id(id);
  result.type_uuid = Type(type);
  result.nullability = nullable ? api::RelationalNullability::kNullable
                               : api::RelationalNullability::kNonNull;
  return result;
}

api::EngineDescriptor Runtime(const api::RelationalTypeDescriptor& descriptor,
                              std::string_view name) {
  api::EngineDescriptor result;
  result.descriptor_uuid.canonical = descriptor.descriptor_uuid;
  result.descriptor_kind = "scalar";
  result.canonical_type_name = name;
  result.encoded_descriptor = "type_uuid=" + descriptor.type_uuid +
      ";nullability=" +
      (descriptor.nullability == api::RelationalNullability::kNullable
           ? "nullable" : "non_null");
  return result;
}

// Typed preparation input only: no parser, storage, or synthesized MGA context.
struct Fixture {
  api::TypedRelationalDag dag;
  plan::CanonicalLogicalPropertyCatalog properties;
  plan::CanonicalLogicalRelationalNode consumer;
  plan::CanonicalLogicalRelationalNode previous;
  sblr::PreparedSortRoot sort;
  sblr::GlobalRankingWindowProfile profile;
  std::string result_type;

  explicit Fixture(sblr::GlobalRankingWindowProfile selected,
                   bool count_star = false) : profile(selected) {
    const bool aggregate = selected.semantic_variant_id ==
                           "window.aggregate-bridge.v1";
    const bool count = aggregate && selected.builtin_id == "sb.aggregate.count";
    const bool navigation = selected.builtin_id == "sb.window.lag" ||
        selected.builtin_id == "sb.window.lead" ||
        selected.builtin_id == "sb.window.first_value" ||
        selected.builtin_id == "sb.window.last_value" ||
        selected.builtin_id == "sb.window.nth_value";
    const bool nth = selected.builtin_id == "sb.window.nth_value";
    const bool ntile = selected.builtin_id == "sb.window.ntile";
    const bool value = navigation || (aggregate && !count_star);
    const auto value_type = aggregate && selected.result_type_name == "boolean"
                                ? "boolean" : "int64";
    dag.root_node_id = 3;
    dag.descriptors = {Descriptor(101, "int64"), Descriptor(102, value_type),
        Descriptor(103, selected.result_type_name, navigation || (aggregate && !count)),
        Descriptor(104, "int64")};
    result_type = dag.descriptors[2].type_uuid;
    for (unsigned id = 1; id <= 2; ++id) {
      api::RelationalExpressionRecord expression;
      expression.expression_id = id;
      expression.expression_kind = api::RelationalExpressionKind::kIdentifier;
      expression.result_descriptor_id = 100 + id;
      expression.bound_name_uuid = Id(200 + id);
      dag.expressions.push_back(expression);
    }
    api::RelationalExpressionRecord literal;
    literal.expression_id = 4;
    literal.expression_kind = api::RelationalExpressionKind::kLiteral;
    literal.result_descriptor_id = 104;
    literal.literal_kind = api::RelationalLiteralKind::kNumeric;
    literal.literal_or_parameter_ref = "2";
    dag.expressions.push_back(literal);
    api::RelationalExpressionRecord function;
    function.expression_id = 3;
    function.expression_kind = api::RelationalExpressionKind::kFunctionCall;
    function.result_descriptor_id = 103;
    function.function_uuid = std::string(selected.function_uuid);
    if (value) function.child_expression_ids.push_back(2);
    if (ntile || nth) function.child_expression_ids.push_back(4);
    dag.expressions.push_back(function);

    api::RelationalDagNode input;
    input.node_id = 1;
    input.output_descriptor_ids = {101, 102};
    api::RelationalDagNode ordered;
    ordered.node_id = 2;
    ordered.node_kind = api::RelationalDagNodeKind::kSort;
    ordered.input_node_ids = {1};
    ordered.output_descriptor_ids = {101, 102};
    ordered.bound_expression_ids = {1};
    api::RelationalDagNode window;
    window.node_id = 3;
    window.node_kind = api::RelationalDagNodeKind::kWindow;
    window.input_node_ids = {2};
    window.output_descriptor_ids = {101, 102, 103};
    window.bound_expression_ids = {1};
    window.bound_expression_ids.insert(window.bound_expression_ids.end(),
        function.child_expression_ids.begin(), function.child_expression_ids.end());
    window.bound_expression_ids.push_back(3);
    window.semantic_variant_id = selected.semantic_variant_id;
    window.required_property_uuids = {Id(301)};
    window.delivered_property_uuids = {Id(301), Id(302)};
    dag.nodes = {input, ordered, window};
    dag.outputs = {{1, 1, 1, "order", 101, true, 0},
                   {2, 1, 2, "value", 102, true, 1},
                   {3, 3, 1, "order", 101, true, 0},
                   {4, 3, 2, "value", 102, true, 1},
                   {5, 3, 3, "result", 103, true, 2}};
    api::RelationalWindowDefinitionRecord definition;
    definition.window_id = 1;
    definition.relation_node_id = 3;
    definition.ordering_terms = {{1, api::RelationalPropertySortDirection::kAscending,
        api::RelationalPropertyNullPlacement::kNullsLast, {}}};
    dag.window_definitions.push_back(definition);
    api::RelationalWindowInvocationRecord invocation;
    invocation.invocation_id = 1;
    invocation.relation_node_id = 3;
    invocation.function_expression_id = 3;
    invocation.window_definition_id = 1;
    invocation.function_abi_version = 1;
    invocation.function_uuid = selected.function_uuid;
    invocation.builtin_id = selected.builtin_id;
    invocation.argument_expression_ids = function.child_expression_ids;
    invocation.result_descriptor_id = 103;
    invocation.output_name_utf8 = "result";
    dag.window_invocations.push_back(invocation);
    consumer.logical_node_id = 3;
    consumer.node_kind = plan::CanonicalLogicalRelationalNodeKind::kWindow;
    consumer.input_logical_node_ids = {2};
    consumer.output_descriptor_ids = {101, 102, 103};
    previous.logical_node_id = 2;
    previous.node_kind = plan::CanonicalLogicalRelationalNodeKind::kSort;
    previous.output_descriptor_ids = {101, 102};
    sort.ok = true;
    sort.ordering_property_uuid = Id(301);
    sort.order_terms.resize(1);
    plan::CanonicalLogicalPropertyRecord property;
    property.property_uuid = Id(302);
    property.property_kind = plan::CanonicalLogicalPropertyKind::kWindow;
    property.origin_logical_node_id = 3;
    property.dependency_property_uuids = {Id(301)};
    property.window_frame_descriptor_uuid = Id(303);
    properties.properties.push_back(property);
  }

  sblr::PreparedGlobalRowNumberWindowBinding Bind(bool project = false) const {
    return sblr::PrepareGlobalRankingWindowBindingForComposition(
        dag, properties, dag.nodes[2], consumer, previous, sort, 2, 2,
        result_type, Type("int64"), Type("boolean"), SignedTypes(),
        "preparation-test", profile, project);
  }
};

void BindingMatrix(Results& results) {
  for (auto profile : {sblr::kGlobalRowNumberProfile, sblr::kGlobalRankProfile,
      sblr::kGlobalDenseRankProfile, sblr::kGlobalPercentRankProfile,
      sblr::kGlobalCumeDistProfile, sblr::kGlobalNtileProfile,
      sblr::kGlobalLagProfile, sblr::kGlobalLeadProfile,
      sblr::kGlobalFirstValueProfile, sblr::kGlobalLastValueProfile,
      sblr::kGlobalNthValueProfile}) {
    Fixture fixture(profile);
    const auto bound = fixture.Bind();
    results.Check(bound.ok && bound.invocation == &fixture.dag.window_invocations[0] &&
        bound.function == &fixture.dag.expressions[3] &&
        bound.result_descriptor == &fixture.dag.descriptors[2] &&
        bound.window_property_uuid == Id(302) &&
        bound.window_frame_descriptor_uuid == Id(303), profile.display_name);
    if (!bound.ok) std::cerr << bound.detail << '\n';
    if (profile.builtin_id == "sb.window.ntile") {
      results.Check(bound.ntile_bucket_count_operand.has_value() &&
          exec::DecodeInt64Value(*bound.ntile_bucket_count_operand).value == 2,
          "NTILE literal binding");
    }
    if (profile.builtin_id == "sb.window.nth_value") {
      results.Check(bound.nth_value_position_operand.has_value() &&
          exec::DecodeInt64Value(*bound.nth_value_position_operand).value == 2 &&
          bound.navigation_value_column == 1, "NTH_VALUE literal/value binding");
    }
    fixture.dag.window_invocations.push_back(fixture.dag.window_invocations[0]);
    results.Check(!fixture.Bind().ok, "duplicate invocation: " + std::string(profile.display_name));
  }

  const Fixture baseline(sblr::kGlobalRowNumberProfile);
  const auto wrapper = sblr::PrepareGlobalRowNumberWindowBindingForComposition(
      baseline.dag, baseline.properties, baseline.dag.nodes[2], baseline.consumer,
      baseline.previous, baseline.sort, 2, 2, Type("int64"), "wrapper-test");
  results.Check(wrapper.ok, "ROW_NUMBER convenience adapter");
  const auto refuse = [&](std::string_view label, auto mutate) {
    auto fixture = baseline;
    mutate(fixture);
    const auto bound = fixture.Bind();
    results.Check(!bound.ok && bound.invocation == nullptr &&
        bound.function == nullptr && bound.result_descriptor == nullptr, label);
  };
  refuse("missing definition", [](auto& f) { f.dag.window_definitions.clear(); });
  refuse("duplicate definition", [](auto& f) { f.dag.window_definitions.push_back(f.dag.window_definitions[0]); });
  refuse("wrong function UUID", [](auto& f) { f.dag.window_invocations[0].function_uuid = Id(999); });
  refuse("wrong ABI", [](auto& f) { f.dag.window_invocations[0].function_abi_version = 2; });
  refuse("unexpected partition", [](auto& f) { f.dag.window_definitions[0].partition_expression_ids = {1}; });
  refuse("wrong property origin", [](auto& f) { f.properties.properties[0].origin_logical_node_id = 2; });
  refuse("missing frame identity", [](auto& f) { f.properties.properties[0].window_frame_descriptor_uuid.clear(); });
  refuse("missing ordering", [](auto& f) { f.dag.nodes[2].required_property_uuids.clear(); });
  refuse("nullable ranking result", [](auto& f) { f.dag.descriptors[2].nullability = api::RelationalNullability::kNullable; });
  refuse("aliased ranking identity", [](auto& f) { f.dag.descriptors[2].descriptor_uuid = f.result_type; });
  refuse("invisible passthrough", [](auto& f) { f.dag.outputs[2].visible = false; });
  refuse("wrong output descriptor", [](auto& f) { f.dag.outputs.back().descriptor_id = 101; });
  refuse("wrong consumer input", [](auto& f) { f.consumer.input_logical_node_ids = {1}; });
  refuse("wrong result arity", [](auto& f) { f.consumer.output_descriptor_ids.pop_back(); });
  auto projected = baseline;
  api::RelationalDagNode project;
  project.node_id = 4;
  project.node_kind = api::RelationalDagNodeKind::kProject;
  project.input_node_ids = {3};
  projected.dag.nodes.push_back(project);
  projected.dag.root_node_id = 4;
  results.Check(projected.Bind(true).ok && !projected.Bind(false).ok,
                "explicit Project-root permission");
  projected.dag.outputs[0].expression_id = 2;
  results.Check(!projected.Bind(true).ok, "Project-root passthrough lineage mismatch");
  for (auto profile : {sblr::kGlobalNtileProfile, sblr::kGlobalNthValueProfile}) {
    for (const auto literal : {"0", "-1", "9223372036854775808"}) {
      Fixture fixture(profile);
      fixture.dag.expressions[2].literal_or_parameter_ref = literal;
      results.Check(!fixture.Bind().ok, std::string(profile.display_name) + " invalid literal " + literal);
    }
  }
}

void AggregateBridgeMatrix(Results& results) {
  for (const auto builtin : {"sb.aggregate.sum", "sb.aggregate.min", "sb.aggregate.max",
      "sb.aggregate.count", "sb.aggregate.bool_and", "sb.aggregate.bool_or", "sb.aggregate.every"}) {
    const auto* entry = exec::LookupCanonicalAggregateByBuiltinIdV1(builtin);
    results.Check(entry != nullptr, builtin);
    if (entry == nullptr) continue;
    api::TypedRelationalDag dag;
    api::RelationalWindowInvocationRecord invocation;
    invocation.relation_node_id = 3;
    invocation.function_uuid = entry->function_uuid;
    invocation.function_abi_version = entry->abi_version;
    invocation.builtin_id = entry->builtin_id;
    dag.window_invocations.push_back(invocation);
    const auto profile = sblr::GlobalAggregateWindowProfileForComposition(dag, 3);
    results.Check(profile.builtin_id == builtin, "aggregate-window profile");
    if (profile.builtin_id.empty()) continue;
    Fixture fixture(profile);
    results.Check(fixture.Bind().ok, builtin);
    if (profile.builtin_id == "sb.aggregate.count") {
      Fixture star(profile, true);
      const auto bound = star.Bind();
      results.Check(bound.ok && bound.aggregate_count_star, "COUNT(*) window binding");
    }
    dag.window_invocations[0].function_abi_version = 65535;
    results.Check(sblr::GlobalAggregateWindowProfileForComposition(dag, 3).builtin_id.empty(),
                  "aggregate-window unknown ABI");
    dag.window_invocations[0].function_abi_version = entry->abi_version;
    dag.window_invocations.push_back(dag.window_invocations[0]);
    results.Check(sblr::GlobalAggregateWindowProfileForComposition(dag, 3).builtin_id.empty(),
                  "aggregate-window duplicate invocation");
  }
}

void DescriptorMatrix(Results& results) {
  for (const auto name : {"int8", "int16", "int32", "int64"}) {
    const auto descriptor = Descriptor(101, name);
    auto runtime = Runtime(descriptor, name);
    const auto source = [&] {
      return sblr::ExactCanonicalBoundedSignedWindowSourceForComposition(
          descriptor, runtime, false, SignedTypes(), Id(201), Id(202), Id(301), Id(302), Id(303));
    };
    results.Check(source(), std::string(name) + " exact signed source");
    results.Check(sblr::ExactCanonicalBoundedSignedWindowOrderForComposition(
        descriptor, runtime, false, SignedTypes(), Id(201), Id(202), Type("int64"),
        Id(301), Id(302), Id(303)), std::string(name) + " exact signed order");
    runtime.encoded_descriptor += ";nullability=non_null";
    results.Check(!source(), "duplicate signed nullability");
  }
  unsigned rank = 0;
  for (const auto& type : SignedTypes()) {
    results.Check(sblr::ExactBoundedSignedIntegerTypeRankForComposition(type) == ++rank,
                  "exact signed type rank");
  }
  results.Check(sblr::ExactBoundedSignedIntegerTypeRankForComposition(Id(999)) == 0,
                "unknown signed type rank");
  const auto boolean = Descriptor(101, "boolean", true);
  auto runtime = Runtime(boolean, "boolean");
  const auto bool_source = [&] {
    return sblr::ExactCanonicalBooleanWindowSourceForComposition(boolean, runtime, true,
        Type("boolean"), Id(201), Id(202), Id(301), Id(302), Id(303));
  };
  results.Check(bool_source(), "nullable boolean source");
  runtime.descriptor_uuid.canonical = Id(999);
  results.Check(!bool_source(), "boolean runtime identity mismatch");

  const auto scalar = Descriptor(101, "int64", true);
  runtime = Runtime(scalar, "int64");
  const auto operand = [&] {
    return sblr::ExactCanonicalScalarWindowOperandForComposition(scalar, runtime, true,
        Id(201), Id(202), Type("int64"), Id(102), Type("int64"), false,
        Id(301), Id(302), Id(303));
  };
  results.Check(operand(), "exact scalar operand");
  runtime.encoded_descriptor = "type_uuid=" + scalar.type_uuid + ";nullable=true";
  results.Check(operand(), "legacy scalar nullability adapter");
  runtime.encoded_descriptor += ";nullability=nullable";
  results.Check(!operand(), "ambiguous scalar nullability");
  results.Check(!sblr::CanonicalDescriptorFieldEqualsForComposition(runtime, "absent", "x"),
                "missing descriptor field");
  results.Check(sblr::CanonicalDescriptorFieldEqualsForComposition(runtime, "absent", std::nullopt),
                "absent optional descriptor field");
  runtime.encoded_descriptor += ";nullable=true";
  results.Check(!sblr::CanonicalDescriptorFieldEqualsForComposition(runtime, "nullable", "true"),
                "duplicate descriptor field");
  for (auto profile : {sblr::kGlobalLagProfile, sblr::kGlobalLeadProfile,
      sblr::kGlobalFirstValueProfile, sblr::kGlobalLastValueProfile,
      sblr::kGlobalNthValueProfile}) {
    Fixture fixture(profile);
    const auto exact = [&] {
      return sblr::DirectValueWindowUsesExactTypeForComposition(fixture.dag, 3,
          profile.builtin_id, Type("int64"));
    };
    results.Check(exact(), std::string(profile.display_name) + " direct value type");
    fixture.dag.expressions[1].expression_kind = api::RelationalExpressionKind::kLiteral;
    results.Check(!exact(), "non-identifier direct value operand");
  }
}

}  // namespace

int main() {
  Results results;
  BindingMatrix(results);
  AggregateBridgeMatrix(results);
  DescriptorMatrix(results);
  std::cout << "window_preparation_cases=" << results.cases
            << " failures=" << results.failures << '\n';
  return results.failures == 0 ? EXIT_SUCCESS : EXIT_FAILURE;
}
