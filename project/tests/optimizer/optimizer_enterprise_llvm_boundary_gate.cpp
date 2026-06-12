// Copyright (c) 2026 ScratchBird Software Inc.
//
// This Source Code Form is subject to the terms of the Mozilla Public
// License, v. 2.0. If a copy of the MPL was not distributed with this
// file, You can obtain one at https://mozilla.org/MPL/2.0/.
//
// SPDX-License-Identifier: MPL-2.0

#include "native_compile.hpp"

#include <algorithm>
#include <cstdlib>
#include <iostream>
#include <string>

namespace native = scratchbird::engine::native_compile;

namespace {

void Require(bool condition, const std::string& message) {
  if (!condition) {
    std::cerr << "optimizer_enterprise_llvm_boundary_gate: " << message << '\n';
    std::exit(1);
  }
}

native::NativeCompileRequest BaseRequest() {
  native::NativeCompileRequest request;
  request.module_payload = "sblr:predicate:col_i32_gt_const";
  request.target_object_uuid = "018f0000-0000-7000-8000-00000000ae04";
  request.principal_uuid = "018f0000-0000-7000-8000-00000000ae05";
  request.database_path = "/tmp/sb_oeic_llvm_boundary";
  request.catalog_generation_id = 100;
  request.security_epoch = 101;
  request.policy_epoch = 102;
  request.resource_epoch = 103;
  request.security_context_present = true;
  request.policy_profiles.push_back("native_compile.jit_optional");
  request.memory_accounting.explicit_test_fixture = true;
  request.memory_accounting.production_like = false;
  request.memory_accounting.evidence.push_back(
      "optimizer_enterprise_llvm_boundary_gate.fixture=true");
  request.descriptors.push_back({"018f0000-0000-7000-8000-00000000d001",
                                 "table_descriptor",
                                 "sys.test",
                                 "columns:i32"});
  return request;
}

bool IsSha256Hex(const std::string& value) {
  return value.size() == 64 &&
         std::all_of(value.begin(), value.end(), [](unsigned char c) {
           return (c >= '0' && c <= '9') || (c >= 'a' && c <= 'f');
         });
}

void TestUnavailableFallbackIsExact() {
  // SEARCH_KEY: OEIC_LLVM_DYNAMIC_STATIC_INTEGRATION
  auto request = BaseRequest();
  request.simulate_backend_unavailable = true;
  request.allow_interpreter_fallback = true;
  const auto result = native::CompileNativeUnit(request);
  Require(result.ok, "optional unavailable LLVM path did not use interpreter fallback");
  Require(result.fallback_used, "optional unavailable LLVM path did not record fallback");
  Require(result.effective_mode == native::NativeCompileEffectiveMode::interpreter,
          "optional unavailable LLVM path did not remain interpreter semantics");
  Require(result.diagnostic_code == "NATIVE.COMPILE_FAILED_FALLBACK",
          "optional unavailable LLVM diagnostic changed");
  Require(!result.llvm_load_mode.empty(), "LLVM load mode evidence missing");
  Require(IsSha256Hex(result.llvm_library_path_digest),
          "LLVM library path digest is not SHA-256 evidence");
  Require(IsSha256Hex(result.llvm_source_root_digest),
          "LLVM source-root digest is not SHA-256 evidence");
  Require(IsSha256Hex(result.llvm_tools_root_digest),
          "LLVM tools-root digest is not SHA-256 evidence");
  Require(!result.target_triple.empty(), "LLVM target triple evidence missing");
  Require(result.target_feature_set.find("scalar_exact") != std::string::npos,
          "LLVM target feature evidence missing scalar exact fallback");
  Require(!result.llvm_memory_reserved,
          "test-fixture unavailable LLVM path unexpectedly reserved live memory");
  Require(result.llvm_memory_test_fixture,
          "LLVM unavailable simulation did not record explicit fixture mode");
  Require(IsSha256Hex(result.sblr_or_ir_digest),
          "LLVM SBLR input digest is not SHA-256 evidence");
  Require(IsSha256Hex(result.descriptor_set_digest),
          "LLVM descriptor set digest is not SHA-256 evidence");
}

void TestRequiredUnavailableRefuses() {
  auto request = BaseRequest();
  request.simulate_backend_unavailable = true;
  request.allow_interpreter_fallback = true;
  request.policy_profiles.clear();
  request.policy_profiles.push_back("native_compile.jit_required_for_declared_units");
  const auto result = native::CompileNativeUnit(request);
  Require(!result.ok, "required unavailable LLVM path did not refuse");
  Require(result.effective_mode == native::NativeCompileEffectiveMode::refused,
          "required unavailable LLVM path did not record refused mode");
  Require(result.diagnostic_code == "NATIVE.LLVM_BACKEND_UNAVAILABLE",
          "required unavailable LLVM diagnostic changed");
}

void TestUnavailableSimulationRequiresFixture() {
  auto request = BaseRequest();
  request.simulate_backend_unavailable = true;
  request.allow_interpreter_fallback = true;
  request.memory_accounting.explicit_test_fixture = false;
  request.memory_accounting.production_like = true;
  const auto result = native::CompileNativeUnit(request);
  Require(!result.ok && result.diagnostic_code == "NATIVE.LLVM_TEST_FIXTURE_REQUIRED",
          "non-fixture LLVM unavailable simulation was accepted");
}

void TestForbiddenAuthorityInputsRefuse() {
  auto sql = BaseRequest();
  sql.module_payload = "sql:SELECT * FROM sys.tables";
  const auto sql_result = native::CompileNativeUnit(sql);
  Require(!sql_result.ok && sql_result.diagnostic_code == "NATIVE.SQL_COMPILE_FORBIDDEN",
          "SQL text was accepted as LLVM compile authority");

  auto cluster = BaseRequest();
  cluster.module_payload = "sblr:cluster:remote_fragment";
  const auto cluster_result = native::CompileNativeUnit(cluster);
  Require(!cluster_result.ok &&
              cluster_result.diagnostic_detail == "cluster_operation_forbidden_noncluster",
          "cluster operation was accepted by noncluster LLVM compile boundary");

  auto mutation = BaseRequest();
  mutation.module_payload = "sblr:dml_mutation:commit";
  const auto mutation_result = native::CompileNativeUnit(mutation);
  Require(!mutation_result.ok &&
              mutation_result.diagnostic_detail == "mutation_side_effect_forbidden",
          "mutation/finality operation was accepted by LLVM compile boundary");

  auto parser = BaseRequest();
  parser.module_payload = "sblr:parser_ast:expr";
  const auto parser_result = native::CompileNativeUnit(parser);
  Require(!parser_result.ok &&
              parser_result.diagnostic_detail == "parser_authority_forbidden",
          "parser AST authority was accepted by LLVM compile boundary");

  auto reference = BaseRequest();
  reference.module_payload = "sblr:predicate:reference_plan";
  const auto reference_result = native::CompileNativeUnit(reference);
  Require(!reference_result.ok &&
              reference_result.diagnostic_detail == "reference_authority_forbidden",
          "reference authority was accepted by LLVM compile boundary");

  auto protocol = BaseRequest();
  protocol.module_payload = "sblr:protocol_frame:predicate";
  const auto protocol_result = native::CompileNativeUnit(protocol);
  Require(!protocol_result.ok &&
              protocol_result.diagnostic_detail == "protocol_or_client_authority_forbidden",
          "protocol/client authority was accepted by LLVM compile boundary");
}

void TestEngineIrRequiresValidation() {
  auto unvalidated = BaseRequest();
  unvalidated.module_payload = "engine_ir:predicate:col_i32_gt_const";
  const auto unvalidated_result = native::CompileNativeUnit(unvalidated);
  Require(!unvalidated_result.ok &&
              unvalidated_result.diagnostic_detail == "engine_ir_validation_required",
          "unvalidated engine IR was accepted by LLVM compile boundary");

  auto validated = BaseRequest();
  validated.module_payload = "engine_ir:predicate:col_i32_gt_const";
  validated.option_envelopes.push_back("engine_ir:validated");
  validated.simulate_backend_unavailable = true;
  validated.allow_interpreter_fallback = true;
  const auto validated_result = native::CompileNativeUnit(validated);
  Require(validated_result.ok && validated_result.fallback_used,
          "validated engine IR did not preserve interpreter fallback semantics");
  Require(validated_result.lowerability == "llvm_safe",
          "validated engine IR did not pass lowerability classification");
}

}  // namespace

int main() {
  TestUnavailableFallbackIsExact();
  TestRequiredUnavailableRefuses();
  TestUnavailableSimulationRequiresFixture();
  TestForbiddenAuthorityInputsRefuse();
  TestEngineIrRequiresValidation();
  std::cout << "optimizer enterprise LLVM boundary gate passed\n";
  return 0;
}
