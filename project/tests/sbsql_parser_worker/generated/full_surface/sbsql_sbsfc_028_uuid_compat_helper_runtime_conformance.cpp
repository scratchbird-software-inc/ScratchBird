// Copyright (c) 2026 ScratchBird Software Inc.
//
// This Source Code Form is subject to the terms of the Mozilla Public
// License, v. 2.0. If a copy of the MPL was not distributed with this
// file, You can obtain one at https://mozilla.org/MPL/2.0/.
//
// SPDX-License-Identifier: MPL-2.0

#include "dispatch/function_dispatch.hpp"
#include "registry/function_seed_registry.hpp"

#include <cstdint>
#include <iostream>
#include <string>
#include <string_view>
#include <utility>
#include <vector>

namespace {

using scratchbird::engine::functions::BuildStandardFunctionSeedPackage;
using scratchbird::engine::functions::DispatchFunctionCall;
using scratchbird::engine::functions::FunctionArgument;
using scratchbird::engine::functions::FunctionCallRequest;
using scratchbird::engine::functions::FunctionRegistry;
using scratchbird::engine::sblr::SblrStatusCode;
using scratchbird::engine::sblr::SblrValue;
using scratchbird::engine::sblr::SblrValuePayloadKind;

SblrValue NullValue(std::string descriptor = {}) {
  SblrValue value;
  value.descriptor_id = std::move(descriptor);
  value.is_null = true;
  return value;
}

SblrValue TextValue(std::string input, std::string descriptor = "character") {
  SblrValue value;
  value.descriptor_id = std::move(descriptor);
  value.payload_kind = SblrValuePayloadKind::text;
  value.is_null = false;
  value.encoded_value = std::move(input);
  value.text_value = value.encoded_value;
  return value;
}

bool HasDiagnostic(const scratchbird::engine::sblr::SblrResult& result, std::string_view id) {
  for (const auto& diagnostic : result.diagnostics) {
    if (diagnostic.diagnostic_id == id) return true;
  }
  return false;
}

scratchbird::engine::sblr::SblrResult Run(const FunctionRegistry& registry,
                                          std::string function_id,
                                          std::vector<SblrValue> values,
                                          std::string deterministic_uuid = {}) {
  FunctionCallRequest request;
  request.context.function_id = std::move(function_id);
  request.context.security_allowed = true;
  request.context.policy_allowed = true;
  request.context.dependency_available = true;
  request.context.sblr_context.database_uuid = "SBSFC-028-uuid-runtime-db";
  request.context.sblr_context.transaction_uuid = "SBSFC-028-uuid-runtime-tx";
  request.context.sblr_context.transaction_context_present = true;
  request.context.sblr_context.deterministic_uuid_text = std::move(deterministic_uuid);
  for (std::size_t i = 0; i < values.size(); ++i) {
    request.arguments.push_back(FunctionArgument{"arg" + std::to_string(i), std::move(values[i])});
  }
  return DispatchFunctionCall(registry, std::move(request)).result;
}

bool ExpectOkScalar(const scratchbird::engine::sblr::SblrResult& result, std::string_view case_id) {
  if (result.status != SblrStatusCode::ok || !result.diagnostics.empty() ||
      result.scalar_values.size() != 1 || !result.rows.empty()) {
    std::cerr << case_id << ": expected one successful scalar result\n";
    return false;
  }
  return true;
}

bool IsCanonicalUuid(std::string_view value) {
  if (value.size() != 36 || value[8] != '-' || value[13] != '-' ||
      value[18] != '-' || value[23] != '-') {
    return false;
  }
  for (std::size_t i = 0; i < value.size(); ++i) {
    if (i == 8 || i == 13 || i == 18 || i == 23) continue;
    const char ch = value[i];
    if (!((ch >= '0' && ch <= '9') || (ch >= 'a' && ch <= 'f'))) return false;
  }
  return true;
}

bool ExpectUuidVersion(std::string_view case_id,
                       const scratchbird::engine::sblr::SblrResult& result,
                       char version) {
  if (!ExpectOkScalar(result, case_id)) return false;
  const auto& value = result.scalar_values.front();
  if (value.is_null || value.descriptor_id != "uuid" ||
      value.payload_kind != SblrValuePayloadKind::uuid_text ||
      value.text_value != value.encoded_value ||
      !IsCanonicalUuid(value.text_value) ||
      value.text_value[14] != version) {
    std::cerr << case_id << ": expected canonical UUID version " << version << "\n";
    return false;
  }
  return true;
}

bool ExpectText(std::string_view case_id,
                const scratchbird::engine::sblr::SblrResult& result,
                std::string_view descriptor,
                std::string_view expected) {
  if (!ExpectOkScalar(result, case_id)) return false;
  const auto& value = result.scalar_values.front();
  const auto payload_kind = descriptor == "uuid" ? SblrValuePayloadKind::uuid_text
                            : (descriptor == "timestamp" ||
                               descriptor == "timestamp_tz" ||
                               descriptor == "timestamp_epoch_ms" ||
                               descriptor == "date" ||
                               descriptor == "time")
                                  ? SblrValuePayloadKind::temporal_text
                                  : SblrValuePayloadKind::text;
  if (value.is_null || value.descriptor_id != descriptor || value.text_value != expected ||
      value.encoded_value != expected || value.payload_kind != payload_kind) {
    std::cerr << case_id << ": expected " << descriptor << " text value\n";
    return false;
  }
  return true;
}

bool ExpectInt64(std::string_view case_id,
                 const scratchbird::engine::sblr::SblrResult& result,
                 std::int64_t expected) {
  if (!ExpectOkScalar(result, case_id)) return false;
  const auto& value = result.scalar_values.front();
  if (value.is_null || value.descriptor_id != "int64" ||
      value.payload_kind != SblrValuePayloadKind::signed_integer ||
      !value.has_int64_value || value.int64_value != expected ||
      value.encoded_value != std::to_string(expected)) {
    std::cerr << case_id << ": expected int64 value " << expected << "\n";
    return false;
  }
  return true;
}

bool ExpectNull(std::string_view case_id,
                const scratchbird::engine::sblr::SblrResult& result,
                std::string_view descriptor) {
  if (!ExpectOkScalar(result, case_id)) return false;
  const auto& value = result.scalar_values.front();
  if (!value.is_null || value.descriptor_id != descriptor ||
      value.payload_kind != SblrValuePayloadKind::none) {
    std::cerr << case_id << ": expected NULL descriptor " << descriptor << "\n";
    return false;
  }
  return true;
}

bool ExpectFailure(std::string_view case_id,
                   const scratchbird::engine::sblr::SblrResult& result,
                   SblrStatusCode status,
                   std::string_view diagnostic_id) {
  if (result.status != status || result.diagnostics.size() != 1 ||
      !HasDiagnostic(result, diagnostic_id)) {
    std::cerr << case_id << ": expected diagnostic " << diagnostic_id << "\n";
    return false;
  }
  if (!result.scalar_values.empty() || !result.rows.empty()) {
    std::cerr << case_id << ": refusal unexpectedly returned values\n";
    return false;
  }
  return true;
}

}  // namespace

int main() {
  const auto package = BuildStandardFunctionSeedPackage();
  const auto& registry = package.registry;
  bool ok = true;

  ok = ExpectUuidVersion("uuid_generate_v1_random",
                         Run(registry, "sb.uuid.generate_v1", {}),
                         '1') && ok;
  ok = ExpectUuidVersion("uuid_generate_v4_random",
                         Run(registry, "sb.uuid.generate_v4", {}),
                         '4') && ok;
  ok = ExpectText("uuid_generate_v1_deterministic",
                  Run(registry, "sb.uuid.generate_v1", {},
                      "6ba7b810-9dad-11d1-80b4-00c04fd430c8"),
                  "uuid",
                  "6ba7b810-9dad-11d1-80b4-00c04fd430c8") && ok;
  ok = ExpectText("uuid_generate_v4_deterministic",
                  Run(registry, "sb.uuid.generate_v4", {},
                      "550e8400-e29b-41d4-a716-446655440000"),
                  "uuid",
                  "550e8400-e29b-41d4-a716-446655440000") && ok;
  ok = ExpectFailure("uuid_generate_v1_arity",
                     Run(registry, "sb.uuid.generate_v1", {TextValue("extra")}),
                     SblrStatusCode::execution_failed,
                     "SB_DIAG_FUNCTION_INVALID_INPUT") && ok;
  ok = ExpectText("uuid_generate_v3_dns_example",
                  Run(registry, "sb.uuid.generate_v3",
                      {TextValue("6ba7b810-9dad-11d1-80b4-00c04fd430c8", "uuid"),
                       TextValue("www.example.com")}),
                  "uuid",
                  "5df41881-3aed-3515-88a7-2f4a814cf09e") && ok;
  ok = ExpectNull("uuid_generate_v3_null",
                  Run(registry, "sb.uuid.generate_v3",
                      {NullValue("uuid"), TextValue("www.example.com")}),
                  "uuid") && ok;
  ok = ExpectFailure("uuid_generate_v3_invalid_namespace",
                     Run(registry, "sb.uuid.generate_v3",
                         {TextValue("not-a-uuid", "uuid"), TextValue("www.example.com")}),
                     SblrStatusCode::execution_failed,
                     "SB_DIAG_FUNCTION_INVALID_INPUT") && ok;
  ok = ExpectFailure("uuid_generate_v3_arity",
                     Run(registry, "sb.uuid.generate_v3", {}),
                     SblrStatusCode::execution_failed,
                     "SB_DIAG_FUNCTION_INVALID_INPUT") && ok;
  ok = ExpectFailure("uuid_generate_v4_arity",
                     Run(registry, "sb.uuid.generate_v4", {TextValue("extra")}),
                     SblrStatusCode::execution_failed,
                     "SB_DIAG_FUNCTION_INVALID_INPUT") && ok;
  ok = ExpectText("uuid_generate_v5_dns_example",
                  Run(registry, "sb.uuid.generate_v5",
                      {TextValue("6ba7b810-9dad-11d1-80b4-00c04fd430c8", "uuid"),
                       TextValue("www.example.com")}),
                  "uuid",
                  "2ed6657d-e927-568b-95e1-2665a8aea6a2") && ok;
  ok = ExpectNull("uuid_generate_v5_null",
                  Run(registry, "sb.uuid.generate_v5",
                      {NullValue("uuid"), TextValue("www.example.com")}),
                  "uuid") && ok;
  ok = ExpectFailure("uuid_generate_v5_invalid_namespace",
                     Run(registry, "sb.uuid.generate_v5",
                         {TextValue("not-a-uuid", "uuid"), TextValue("www.example.com")}),
                     SblrStatusCode::execution_failed,
                     "SB_DIAG_FUNCTION_INVALID_INPUT") && ok;
  ok = ExpectFailure("uuid_generate_v5_arity",
                     Run(registry, "sb.uuid.generate_v5", {}),
                     SblrStatusCode::execution_failed,
                     "SB_DIAG_FUNCTION_INVALID_INPUT") && ok;

  ok = ExpectText("uuid_nil",
                  Run(registry, "sb.uuid.nil", {}),
                  "uuid",
                  "00000000-0000-0000-0000-000000000000") && ok;
  ok = ExpectFailure("uuid_nil_arity",
                     Run(registry, "sb.uuid.nil", {TextValue("extra")}),
                     SblrStatusCode::execution_failed,
                     "SB_DIAG_FUNCTION_INVALID_INPUT") && ok;

  ok = ExpectInt64("uuid_version_v1",
                   Run(registry, "sb.uuid.version",
                       {TextValue("6ba7b810-9dad-11d1-80b4-00c04fd430c8", "uuid")}),
                   1) && ok;
  ok = ExpectInt64("uuid_version_v4",
                   Run(registry, "sb.uuid.version",
                       {TextValue("550e8400-e29b-41d4-a716-446655440000", "uuid")}),
                   4) && ok;
  ok = ExpectInt64("uuid_version_nil",
                   Run(registry, "sb.uuid.version",
                       {TextValue("00000000-0000-0000-0000-000000000000", "uuid")}),
                   0) && ok;
  ok = ExpectNull("uuid_version_null",
                  Run(registry, "sb.uuid.version", {NullValue("uuid")}),
                  "int64") && ok;
  ok = ExpectFailure("uuid_version_invalid",
                     Run(registry, "sb.uuid.version", {TextValue("not-a-uuid")}),
                     SblrStatusCode::execution_failed,
                     "SB_DIAG_FUNCTION_INVALID_INPUT") && ok;
  ok = ExpectFailure("uuid_version_arity",
                     Run(registry, "sb.uuid.version", {}),
                     SblrStatusCode::execution_failed,
                     "SB_DIAG_FUNCTION_INVALID_INPUT") && ok;

  ok = ExpectText("uuid_timestamp_v1",
                  Run(registry, "sb.uuid.timestamp",
                      {TextValue("968b8080-a91b-11ee-8abc-0123456789ab", "uuid")}),
                  "timestamp_tz",
                  "2024-01-02T03:04:05Z") && ok;
  ok = ExpectText("uuid_timestamp_v7",
                  Run(registry, "sb.uuid.timestamp",
                      {TextValue("019e176c-2968-7abc-8def-0123456789ab", "uuid")}),
                  "timestamp_tz",
                  "2026-05-11T14:23:45Z") && ok;
  ok = ExpectNull("uuid_timestamp_null",
                  Run(registry, "sb.uuid.timestamp", {NullValue("uuid")}),
                  "timestamp_tz") && ok;
  ok = ExpectFailure("uuid_timestamp_invalid",
                     Run(registry, "sb.uuid.timestamp", {TextValue("not-a-uuid")}),
                     SblrStatusCode::execution_failed,
                     "SB_DIAG_FUNCTION_INVALID_INPUT") && ok;
  ok = ExpectFailure("uuid_timestamp_unsupported_version",
                     Run(registry, "sb.uuid.timestamp",
                         {TextValue("550e8400-e29b-41d4-a716-446655440000", "uuid")}),
                     SblrStatusCode::execution_failed,
                     "SB_DIAG_FUNCTION_INVALID_INPUT") && ok;
  ok = ExpectFailure("uuid_timestamp_arity",
                     Run(registry, "sb.uuid.timestamp", {}),
                     SblrStatusCode::execution_failed,
                     "SB_DIAG_FUNCTION_INVALID_INPUT") && ok;

  if (!ok) return 1;
  std::cout << "sbsql_sbsfc_028_uuid_compat_helper_runtime_conformance=passed\n";
  return 0;
}
