// Copyright (c) 2026 ScratchBird Software Inc.
// SPDX-License-Identifier: MPL-2.0
#pragma once
#include "binder/binder.hpp"
#include "functions/registry/function_seed_registry.hpp"
#include "sblr_engine_envelope.hpp"
#include <algorithm>
#include <stdexcept>

namespace scratchbird::tests {
inline const engine::functions::FunctionRegistry& FixtureCallableRegistry() {
  static const auto package = engine::functions::BuildStandardFunctionSeedPackage();
  return package.registry;
}
inline core::platform::Uuid FixtureCallableIdentity(std::string_view name) {
  const auto* function = FixtureCallableRegistry().Lookup(name);
  if (!function || !function->catalog_visible)
    throw std::logic_error("fixture callable absent from actual engine registry");
  return function->function_uuid;
}
inline std::vector<wire::BuiltinFunctionIdentity> FixtureCallableBindings() {
  std::vector<wire::BuiltinFunctionIdentity> result;
  for (const auto& entry : FixtureCallableRegistry().Entries())
    if (entry.catalog_visible) result.push_back({entry.function_id, entry.function_uuid});
  return result;
}
inline void AppendFixtureCallable(engine::sblr::SblrOperationEnvelope& envelope,
                                   std::string_view name, std::string path = "projection_0") {
  const auto id = FixtureCallableIdentity(name);
  engine::sblr::SblrOperand operand;
  operand.name = std::move(path) + "_function_uuid";
  operand.type = "uuid";
  operand.ordinal = envelope.operands.size() + 1;
  operand.value_kind = engine::sblr::SblrValueKind::uuid_ref;
  operand.value_body.assign(id.bytes.begin(), id.bytes.end());
  envelope.operands.push_back(std::move(operand));
}
template<class Envelope>
bool FixtureCallableBound(const Envelope& envelope, std::string_view name) {
  const auto id = FixtureCallableIdentity(name);
  const auto at = std::find_if(envelope.operands.begin(), envelope.operands.end(),
      [](const auto& operand) { return operand.name == "projection_0_function_uuid"; });
  return at != envelope.operands.end() && at->type == "uuid" && at->value.empty() &&
      at->canonical_value_body.size() == 16 &&
      std::equal(at->canonical_value_body.begin(), at->canonical_value_body.end(), id.bytes.begin());
}
template<class Result>
bool FixtureCallableEvidence(const Result& result, std::string_view name) {
  const auto id = FixtureCallableIdentity(name);
  return std::any_of(result.evidence.begin(), result.evidence.end(), [&](const auto& evidence) {
    const auto* value = std::get_if<core::platform::Uuid>(&evidence.evidence_id);
    return evidence.evidence_kind == "function_runtime" && value && *value == id;
  });
}
}  // namespace scratchbird::tests
