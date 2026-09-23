// Copyright (c) 2026 ScratchBird Software Inc.
// SPDX-License-Identifier: MPL-2.0
#include "../../src/engine/sblr/canonical_query_window_preparation.cpp"
#include "../support/binary_uuid_fixture.hpp"
#include <cstdlib>
#include <iostream>
#include <source_location>
namespace s = scratchbird::engine::sblr;
namespace a = scratchbird::engine::internal_api;
void Check(bool ok, std::source_location at = std::source_location::current()) {
  if (!ok) { std::cerr << "failure at " << at.line() << '\n'; std::abort(); }
}
int main() {
  a::TypedRelationalDag dag;
  auto type = scratchbird::tests::FixtureUuid(1081, 1);
  type.bytes[14] = 0; type.bytes[15] = 0xff;
  a::RelationalTypeDescriptor descriptor;
  descriptor.descriptor_id = 3;
  descriptor.type_uuid = type;
  dag.descriptors.push_back(descriptor);
  a::RelationalExpressionRecord expression;
  expression.expression_id = 2;
  expression.result_descriptor_id = 3;
  expression.expression_kind = a::RelationalExpressionKind::kIdentifier;
  dag.expressions.push_back(expression);
  a::RelationalWindowInvocationRecord invocation;
  invocation.relation_node_id = 1;
  invocation.builtin_id = "sb.window.lag";
  invocation.argument_expression_ids = {2};
  dag.window_invocations.push_back(invocation);
  Check(s::DirectValueWindowUsesExactTypeV1(dag, 1, "sb.window.lag", type));
  auto crossed = type; crossed.bytes[15] = 0xfe;
  Check(!s::DirectValueWindowUsesExactTypeV1(dag, 1, "sb.window.lag", crossed));
  Check(!s::DirectValueWindowUsesExactTypeV1(dag, 1, "sb.window.lag", {}));
  Check(!s::DirectValueWindowUsesExactTypeV1(dag, 1, "sb.window.lead", type));
  dag.window_invocations.push_back(invocation);
  Check(!s::DirectValueWindowUsesExactTypeV1(dag, 1, "sb.window.lag", type));
  Check(s::kGlobalRowNumberProfile.function_uuid ==
      scratchbird::tests::FixtureUuidLiteral("019de5fc-2400-7539-bcce-00eef3ae7220"));
  Check(s::kGlobalLagProfile.function_uuid != s::kGlobalLeadProfile.function_uuid);
}
