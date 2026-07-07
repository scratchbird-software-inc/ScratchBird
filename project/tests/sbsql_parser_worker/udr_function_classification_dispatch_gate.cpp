// Copyright (c) 2026 ScratchBird Software Inc.
//
// This Source Code Form is subject to the terms of the Mozilla Public
// License, v. 2.0. If a copy of the MPL was not distributed with this
// file, You can obtain one at https://mozilla.org/MPL/2.0/.
//
// SPDX-License-Identifier: MPL-2.0

#include "dispatch/function_dispatch.hpp"
#include "registry/function_seed_registry.hpp"

#include <algorithm>
#include <cstdlib>
#include <iostream>
#include <string>
#include <string_view>

namespace {

namespace functions = scratchbird::engine::functions;
namespace sblr = scratchbird::engine::sblr;

[[noreturn]] void Fail(const std::string& message) {
  std::cerr << message << '\n';
  std::exit(EXIT_FAILURE);
}

void Require(bool condition, const std::string& message) {
  if (!condition) Fail(message);
}

bool Contains(std::string_view text, std::string_view needle) {
  return text.find(needle) != std::string_view::npos;
}

bool AnyResourceLimit(const functions::FunctionResourceLimits& limits) {
  return limits.max_input_bytes != 0 || limits.max_output_bytes != 0 ||
         limits.max_steps != 0 || limits.max_memory_bytes != 0 ||
         limits.max_recursion_depth != 0;
}

bool IsMissingHandlerDiagnostic(const sblr::SblrRuntimeDiagnostic& diagnostic) {
  return diagnostic.diagnostic_id == "SB_DIAG_FUNCTION_FAMILY_HANDLER_MISSING" ||
         Contains(diagnostic.diagnostic_id, "_FUNCTION_UNHANDLED") ||
         Contains(diagnostic.detail, "function family does not have a dispatch handler") ||
         Contains(diagnostic.detail, "not implemented");
}

functions::FunctionCallRequest RequestFor(const functions::FunctionRegistryEntry& entry) {
  functions::FunctionCallRequest request;
  request.context.function_id = entry.function_id;
  request.context.security_allowed = true;
  request.context.policy_allowed = true;
  request.context.dependency_available = true;
  request.context.sblr_context.cluster_uuid = "UDR-function-classification-cluster";
  request.context.sblr_context.node_uuid = "UDR-function-classification-node";
  request.context.sblr_context.database_uuid = "UDR-function-classification-db";
  request.context.sblr_context.transaction_uuid = "UDR-function-classification-tx";
  request.context.sblr_context.statement_uuid = "UDR-function-classification-stmt";
  request.context.sblr_context.user_uuid = "UDR-function-classification-user";
  request.context.sblr_context.current_role_uuid = "UDR-function-classification-role";
  request.context.sblr_context.current_schema_uuid = "UDR-function-classification-schema";
  request.context.sblr_context.transaction_context_present = true;
  request.context.sblr_context.security_context_present = true;
  request.context.sblr_context.current_timestamp = "2026-07-06T12:00:00Z";
  request.context.sblr_context.statement_timestamp = "2026-07-06T12:00:00Z";
  request.context.sblr_context.transaction_timestamp = "2026-07-06T12:00:00Z";
  request.context.sblr_context.deterministic_random_u64 = 42;
  request.context.sblr_context.deterministic_random_u64_present = true;
  request.context.sblr_context.deterministic_random_bytes_hex =
      "00112233445566778899aabbccddeeff";
  request.context.sblr_context.deterministic_uuid_text =
      "019f9000-0000-7000-8000-000000000001";
  return request;
}

}  // namespace

int main() {
  const auto package = functions::BuildStandardFunctionSeedPackage();
  const auto closure_errors =
      functions::ValidateFunctionRegistryForClosure(package.registry);
  if (!closure_errors.empty()) {
    std::cerr << "UDR function classification closure failures:\n";
    for (const auto& error : closure_errors) std::cerr << "  " << error << '\n';
    return EXIT_FAILURE;
  }

  auto entries = package.registry.Entries();
  std::sort(entries.begin(), entries.end(), [](const auto& lhs, const auto& rhs) {
    return lhs.function_id < rhs.function_id;
  });

  std::size_t core_rows = 0;
  std::size_t optional_rows = 0;
  std::size_t refusal_rows = 0;
  std::size_t final_rows = 0;
  std::size_t success_rows = 0;
  std::size_t exact_refusal_rows = 0;
  std::size_t invalid_input_rows = 0;

  for (const auto& entry : entries) {
    Require(!entry.function_id.empty(), "function_id must be present");
    Require(!entry.function_uuid.empty(), entry.function_id + ": function_uuid must be present");
    Require(!entry.family.empty(), entry.function_id + ": family must be present");
    Require(!entry.short_name.empty(), entry.function_id + ": short_name must be present");
    Require(!entry.owner_source.empty(), entry.function_id + ": owner_source must be present");
    Require(!entry.owner_test.empty(), entry.function_id + ": owner_test must be present");
    Require(!entry.execute_right.empty(), entry.function_id + ": execute_right must be present");
    Require(!entry.metadata_visibility_right.empty(),
            entry.function_id + ": metadata visibility right must be present");
    Require(!entry.optimizer_metadata.descriptor_rule.empty(),
            entry.function_id + ": descriptor rule metadata must be present");
    Require(AnyResourceLimit(entry.resource_limits),
            entry.function_id + ": resource limits must be present");
    Require(!functions::IsForbiddenClosureState(entry.implementation_state),
            entry.function_id + ": forbidden classifier state reached closure");
    Require(functions::IsFinalFunctionImplementationState(entry.implementation_state),
            entry.function_id + ": non-final classifier state reached closure");

    switch (entry.package_state) {
      case functions::FunctionPackageState::core:
        ++core_rows;
        break;
      case functions::FunctionPackageState::optional:
        ++optional_rows;
        break;
      case functions::FunctionPackageState::future_or_refusal:
        ++refusal_rows;
        break;
    }

    ++final_rows;
    const auto result =
        functions::DispatchFunctionCall(package.registry, RequestFor(entry)).result;
    if (result.ok()) {
      ++success_rows;
      continue;
    }
    Require(!result.diagnostics.empty(),
            entry.function_id + ": failed dispatch must carry diagnostics");
    bool invalid_input = false;
    for (const auto& diagnostic : result.diagnostics) {
      Require(!IsMissingHandlerDiagnostic(diagnostic),
              entry.function_id + ": final classifier row reached missing handler");
      invalid_input = invalid_input ||
                      diagnostic.diagnostic_id == "SB_DIAG_FUNCTION_INVALID_INPUT";
    }
    if (invalid_input) {
      ++invalid_input_rows;
    } else {
      ++exact_refusal_rows;
    }
  }

  Require(entries.size() >= 300, "function registry row count unexpectedly small");
  Require(final_rows == entries.size(), "all function registry rows must be final");
  Require(core_rows >= 300, "native core function classification count unexpectedly small");
  Require(optional_rows == 0,
          "optional UDR function packages must not be silently active in the beta core gate");
  Require(refusal_rows == 0,
          "future/refusal package rows must have been converted to exact final states");
  Require(success_rows > 0, "function registry dispatch produced no successful rows");
  Require(exact_refusal_rows > 0,
          "function registry dispatch produced no exact refusal rows");

  std::cout << "udr_function_classification_dispatch_gate=passed\n"
            << "registry_rows=" << entries.size() << '\n'
            << "final_rows=" << final_rows << '\n'
            << "core_rows=" << core_rows << '\n'
            << "success_rows=" << success_rows << '\n'
            << "invalid_input_rows=" << invalid_input_rows << '\n'
            << "exact_refusal_rows=" << exact_refusal_rows << '\n';
  return EXIT_SUCCESS;
}
