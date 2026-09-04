// Copyright (c) 2026 ScratchBird Software Inc.
// SPDX-License-Identifier: MPL-2.0

#include "engine/sblr/sblr_diagnostic_control_runtime.hpp"

#include <algorithm>
#include <cstdlib>
#include <iostream>
#include <string>
#include <string_view>
#include <vector>

namespace sblr = scratchbird::engine::sblr;

namespace {

[[noreturn]] void Fail(std::string_view message) {
  std::cerr << message << '\n';
  std::exit(EXIT_FAILURE);
}

void Require(bool condition, std::string_view message) {
  if (!condition) Fail(message);
}

sblr::DiagnosticControlUuid Uuid(std::uint8_t seed) {
  sblr::DiagnosticControlUuid value{};
  for (std::size_t index = 0; index < value.size(); ++index) {
    value[index] = static_cast<std::uint8_t>(seed + index);
  }
  return value;
}

sblr::SblrDiagnosticControlDescriptorV1 Descriptor() {
  sblr::SblrDiagnosticControlDescriptorV1 value;
  value.operation_uuid = Uuid(1);
  value.authenticated_receipt_uuid = Uuid(17);
  value.target_scope_uuid = Uuid(33);
  value.target_scope_generation = 7;
  value.scope_kind = sblr::DiagnosticControlScopeKind::session;
  value.action = sblr::DiagnosticControlAction::enable;
  value.diagnostic_state_uuid = Uuid(49);
  value.diagnostic_state_generation = 11;
  value.security_context_uuid = Uuid(65);
  value.policy_snapshot_uuid = Uuid(81);
  value.policy_generation = 13;
  value.transaction_uuid = Uuid(97);
  value.route_provider_evidence_uuid = Uuid(113);
  value.confirmation_token_uuid = Uuid(129);
  return value;
}

sblr::SblrDiagnosticControlResultV1 Result() {
  sblr::SblrDiagnosticControlResultV1 value;
  value.status = sblr::DiagnosticControlResultStatus::applied;
  value.operation_uuid = Uuid(1);
  value.target_scope_uuid = Uuid(33);
  value.old_generation = 7;
  value.new_generation = 8;
  value.audit_event_uuid = Uuid(145);
  value.evidence_uuid = Uuid(161);
  value.recovery_replay_evidence_uuid = Uuid(177);
  value.publication_barrier_uuid = Uuid(193);
  return value;
}

void RequireDescriptorRefusal(std::vector<std::uint8_t> bytes,
                              std::string_view scenario) {
  sblr::SblrDiagnosticControlDescriptorV1 decoded;
  std::string detail;
  if (sblr::DecodeSblrDiagnosticControlDescriptorV1(
          bytes.data(), bytes.size(), &decoded, &detail)) {
    std::cerr << scenario << " was accepted\n";
    std::exit(EXIT_FAILURE);
  }
}

void RequireResultRefusal(std::vector<std::uint8_t> bytes,
                          std::string_view scenario) {
  sblr::SblrDiagnosticControlResultV1 decoded;
  std::string detail;
  if (sblr::DecodeSblrDiagnosticControlResultV1(
          bytes.data(), bytes.size(), &decoded, &detail)) {
    std::cerr << scenario << " was accepted\n";
    std::exit(EXIT_FAILURE);
  }
}

}  // namespace

int main() {
  auto descriptor = Descriptor();
  std::string detail;
  const auto encoded =
      sblr::EncodeSblrDiagnosticControlDescriptorV1(descriptor, &detail);
  Require(encoded.size() == sblr::DiagnosticControlWireLayout::descriptor_size,
          "exact SLDC descriptor did not encode");
  Require(std::equal(encoded.begin(), encoded.begin() + 4, "SLDC"),
          "SLDC magic drifted");
  Require(encoded[4] == 1 && encoded[5] == 0 && encoded[6] == 0x40 &&
              encoded[7] == 0x01,
          "SLDC header drifted");
  sblr::SblrDiagnosticControlDescriptorV1 decoded;
  Require(sblr::DecodeSblrDiagnosticControlDescriptorV1(
              encoded.data(), encoded.size(), &decoded, &detail),
          "exact SLDC descriptor did not decode");
  Require(sblr::EncodeSblrDiagnosticControlDescriptorV1(decoded) == encoded,
          "SLDC descriptor did not round trip byte exactly");

  auto malformed = encoded;
  malformed[0] = 'X';
  RequireDescriptorRefusal(std::move(malformed), "wrong SLDC magic");
  malformed = encoded;
  malformed[5] = 1;
  RequireDescriptorRefusal(std::move(malformed), "nonzero SLDC flags");
  malformed = encoded;
  malformed[64] = 0;
  RequireDescriptorRefusal(std::move(malformed), "unknown scope kind");
  malformed = encoded;
  malformed[65] = 7;
  RequireDescriptorRefusal(std::move(malformed), "unknown control action");
  malformed = encoded;
  malformed[66] = 1;
  RequireDescriptorRefusal(std::move(malformed), "nonzero descriptor reserve");
  malformed = encoded;
  malformed[180] = 1;
  RequireDescriptorRefusal(std::move(malformed), "action/parameter mismatch");
  malformed = encoded;
  malformed[212] ^= 1;
  RequireDescriptorRefusal(std::move(malformed), "parameter hash drift");
  malformed = encoded;
  malformed[244] ^= 1;
  RequireDescriptorRefusal(std::move(malformed), "evidence hash drift");
  malformed = encoded;
  malformed[288] ^= 1;
  RequireDescriptorRefusal(std::move(malformed), "descriptor hash drift");

  descriptor = Descriptor();
  descriptor.action = sblr::DiagnosticControlAction::set_level;
  descriptor.parameter.kind = sblr::DiagnosticControlParameterKind::level;
  descriptor.parameter.length = 4;
  descriptor.parameter.value = 1;
  descriptor.parameter.parameter_uuid = Uuid(209);
  detail.clear();
  Require(sblr::EncodeSblrDiagnosticControlDescriptorV1(descriptor, &detail)
              .empty() &&
              detail ==
                  "parameterized diagnostic control range authority is unavailable",
          "unallocated parameter range authority did not fail closed");

  auto result = Result();
  const auto encoded_result =
      sblr::EncodeSblrDiagnosticControlResultV1(result, &detail);
  Require(encoded_result.size() ==
              sblr::DiagnosticControlWireLayout::result_size,
          "exact SLZR result did not encode");
  sblr::SblrDiagnosticControlResultV1 decoded_result;
  Require(sblr::DecodeSblrDiagnosticControlResultV1(
              encoded_result.data(), encoded_result.size(), &decoded_result,
              &detail),
          "exact SLZR result did not decode");
  Require(sblr::EncodeSblrDiagnosticControlResultV1(decoded_result) ==
              encoded_result,
          "SLZR result did not round trip byte exactly");

  auto malformed_result = encoded_result;
  malformed_result[5] = 0;
  RequireResultRefusal(std::move(malformed_result), "unknown result status");
  malformed_result = encoded_result;
  malformed_result[6] = 1;
  RequireResultRefusal(std::move(malformed_result), "nonzero result flags");
  malformed_result = encoded_result;
  malformed_result[88] ^= 1;
  RequireResultRefusal(std::move(malformed_result), "result material hash drift");
  malformed_result = encoded_result;
  malformed_result[160] = 1;
  RequireResultRefusal(std::move(malformed_result), "nonzero result reserve");

  result = Result();
  result.status = sblr::DiagnosticControlResultStatus::refused;
  Require(sblr::EncodeSblrDiagnosticControlResultV1(result, &detail).empty(),
          "refused SLZR without one diagnostic was accepted");
  result.diagnostic_vector_count = 1;
  Require(!sblr::EncodeSblrDiagnosticControlResultV1(result, &detail).empty(),
          "refused SLZR with one diagnostic was rejected");
  return EXIT_SUCCESS;
}
