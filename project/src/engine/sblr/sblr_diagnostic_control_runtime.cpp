// Copyright (c) 2026 ScratchBird Software Inc.
// SPDX-License-Identifier: MPL-2.0

#include "sblr_diagnostic_control_runtime.hpp"

#include "core/hash/hash_digest.hpp"

#include <algorithm>
#include <string_view>

namespace scratchbird::engine::sblr {
namespace {

constexpr std::string_view kParameterDomain =
    "ScratchBird.SblrDiagnosticControl.Parameters.V1";
constexpr std::string_view kDescriptorEvidenceDomain =
    "ScratchBird.SblrDiagnosticControl.DescriptorEvidence.V1";
constexpr std::string_view kDescriptorDomain =
    "ScratchBird.SblrDiagnosticControl.Descriptor.V1";
constexpr std::string_view kResultDomain =
    "ScratchBird.SblrDiagnosticControl.Result.V1";

void SetDetail(std::string* detail, std::string_view value) {
  if (detail != nullptr) *detail = value;
}

void StoreLe(std::uint8_t* output, std::uint64_t value, std::size_t extent) {
  for (std::size_t index = 0; index < extent; ++index) {
    output[index] = static_cast<std::uint8_t>(value >> (index * 8U));
  }
}

std::uint64_t LoadLe(const std::uint8_t* bytes, std::size_t extent) {
  std::uint64_t value = 0;
  for (std::size_t index = 0; index < extent; ++index) {
    value |= static_cast<std::uint64_t>(bytes[index]) << (index * 8U);
  }
  return value;
}

template <std::size_t N>
bool AnyNonzero(const std::array<std::uint8_t, N>& value) {
  return std::any_of(value.begin(), value.end(),
                     [](std::uint8_t byte) { return byte != 0; });
}

template <std::size_t N>
void StoreArray(std::vector<std::uint8_t>* output, std::size_t offset,
                const std::array<std::uint8_t, N>& value) {
  std::copy(value.begin(), value.end(), output->begin() + offset);
}

template <std::size_t N>
void LoadArray(const std::uint8_t* bytes, std::size_t offset,
               std::array<std::uint8_t, N>* value) {
  std::copy_n(bytes + offset, N, value->begin());
}

DiagnosticControlSha256 Hash(std::string_view domain,
                             const std::uint8_t* bytes,
                             std::size_t size) {
  std::vector<std::uint8_t> material(domain.begin(), domain.end());
  material.insert(material.end(), bytes, bytes + size);
  return scratchbird::core::hash::ComputeSha256Digest(material).digest;
}

bool ScopeKnown(DiagnosticControlScopeKind value) {
  const auto raw = static_cast<std::uint8_t>(value);
  return raw >= 1 && raw <= 5;
}

bool ActionKnown(DiagnosticControlAction value) {
  const auto raw = static_cast<std::uint8_t>(value);
  return raw >= 1 && raw <= 6;
}

bool ParameterValid(const SblrDiagnosticControlDescriptorV1& descriptor,
                    std::string* detail) {
  const auto kind = descriptor.parameter.kind;
  if (descriptor.parameter.version != 1) {
    SetDetail(detail, "diagnostic control parameter version is not V1");
    return false;
  }
  const bool parameterless_action =
      descriptor.action == DiagnosticControlAction::enable ||
      descriptor.action == DiagnosticControlAction::disable ||
      descriptor.action == DiagnosticControlAction::quarantine ||
      descriptor.action == DiagnosticControlAction::release;
  if (parameterless_action) {
    if (kind != DiagnosticControlParameterKind::none ||
        descriptor.parameter.length != 0 || descriptor.parameter.value != 0 ||
        AnyNonzero(descriptor.parameter.parameter_uuid)) {
      SetDetail(detail,
                "parameterless diagnostic control action has parameter data");
      return false;
    }
    return true;
  }

  const auto expected_kind =
      descriptor.action == DiagnosticControlAction::set_level
          ? DiagnosticControlParameterKind::level
          : DiagnosticControlParameterKind::sampling;
  if (kind != expected_kind) {
    SetDetail(detail, "diagnostic control action and parameter kind differ");
    return false;
  }

  // The manifest-listed packet requires a registered range but does not name
  // that registry/range or define the canonical length semantics. Refuse to
  // invent either until the Core contradiction is resolved.
  SetDetail(detail,
            "parameterized diagnostic control range authority is unavailable");
  return false;
}

bool DescriptorFieldsValid(
    const SblrDiagnosticControlDescriptorV1& descriptor,
    std::string* detail) {
  if (descriptor.flags != 0 || !ScopeKnown(descriptor.scope_kind) ||
      !ActionKnown(descriptor.action)) {
    SetDetail(detail, "diagnostic control flags or enum value is invalid");
    return false;
  }
  if (!AnyNonzero(descriptor.operation_uuid) ||
      !AnyNonzero(descriptor.authenticated_receipt_uuid) ||
      !AnyNonzero(descriptor.target_scope_uuid) ||
      descriptor.target_scope_generation == 0 ||
      !AnyNonzero(descriptor.diagnostic_state_uuid) ||
      descriptor.diagnostic_state_generation == 0 ||
      !AnyNonzero(descriptor.security_context_uuid) ||
      !AnyNonzero(descriptor.policy_snapshot_uuid) ||
      descriptor.policy_generation == 0 ||
      !AnyNonzero(descriptor.transaction_uuid) ||
      !AnyNonzero(descriptor.route_provider_evidence_uuid) ||
      !AnyNonzero(descriptor.confirmation_token_uuid)) {
    SetDetail(detail,
              "diagnostic control UUID or generation field is incomplete");
    return false;
  }
  return ParameterValid(descriptor, detail);
}

bool ResultFieldsValid(const SblrDiagnosticControlResultV1& result,
                       std::string* detail) {
  const auto status = static_cast<std::uint8_t>(result.status);
  if (status < 1 || status > 3 || !AnyNonzero(result.operation_uuid) ||
      !AnyNonzero(result.target_scope_uuid) || result.old_generation == 0 ||
      result.new_generation == 0 || !AnyNonzero(result.audit_event_uuid) ||
      !AnyNonzero(result.evidence_uuid) ||
      !AnyNonzero(result.recovery_replay_evidence_uuid) ||
      !AnyNonzero(result.publication_barrier_uuid)) {
    SetDetail(detail, "diagnostic control result identity is incomplete");
    return false;
  }
  const std::uint64_t expected_count =
      result.status == DiagnosticControlResultStatus::refused ? 1 : 0;
  if (result.diagnostic_vector_count != expected_count) {
    SetDetail(detail,
              "diagnostic control result diagnostic count is invalid");
    return false;
  }
  return true;
}

std::array<std::uint8_t, 32> ParameterBytes(
    const SblrDiagnosticControlParameterV1& parameter) {
  std::array<std::uint8_t, 32> bytes{};
  StoreLe(bytes.data(), static_cast<std::uint16_t>(parameter.kind), 2);
  StoreLe(bytes.data() + 2, parameter.version, 2);
  StoreLe(bytes.data() + 4, parameter.length, 4);
  StoreLe(bytes.data() + 8, parameter.value, 4);
  std::copy(parameter.parameter_uuid.begin(), parameter.parameter_uuid.end(),
            bytes.begin() + 16);
  return bytes;
}

}  // namespace

std::vector<std::uint8_t> EncodeSblrDiagnosticControlDescriptorV1(
    const SblrDiagnosticControlDescriptorV1& descriptor,
    std::string* detail) {
  if (!DescriptorFieldsValid(descriptor, detail)) return {};

  std::vector<std::uint8_t> output(
      DiagnosticControlWireLayout::descriptor_size, 0);
  std::copy_n("SLDC", 4, output.begin());
  output[4] = 1;
  output[5] = descriptor.flags;
  StoreLe(output.data() + 6, output.size(), 2);
  StoreArray(&output, 8, descriptor.operation_uuid);
  StoreArray(&output, 24, descriptor.authenticated_receipt_uuid);
  StoreArray(&output, 40, descriptor.target_scope_uuid);
  StoreLe(output.data() + 56, descriptor.target_scope_generation, 8);
  output[64] = static_cast<std::uint8_t>(descriptor.scope_kind);
  output[65] = static_cast<std::uint8_t>(descriptor.action);
  StoreArray(&output, 68, descriptor.diagnostic_state_uuid);
  StoreLe(output.data() + 84, descriptor.diagnostic_state_generation, 8);
  StoreArray(&output, 92, descriptor.security_context_uuid);
  StoreArray(&output, 108, descriptor.policy_snapshot_uuid);
  StoreLe(output.data() + 124, descriptor.policy_generation, 8);
  StoreArray(&output, 132, descriptor.transaction_uuid);
  StoreArray(&output, 148, descriptor.route_provider_evidence_uuid);
  StoreArray(&output, 164, descriptor.confirmation_token_uuid);
  const auto parameter = ParameterBytes(descriptor.parameter);
  std::copy(parameter.begin(), parameter.end(), output.begin() + 180);
  const auto parameter_hash =
      Hash(kParameterDomain, parameter.data(), parameter.size());
  if (AnyNonzero(descriptor.parameter_sha256) &&
      descriptor.parameter_sha256 != parameter_hash) {
    SetDetail(detail, "diagnostic control parameter hash differs");
    return {};
  }
  StoreArray(&output, 212, parameter_hash);

  const auto evidence_hash =
      Hash(kDescriptorEvidenceDomain, output.data(), output.size());
  if (AnyNonzero(descriptor.descriptor_evidence_sha256) &&
      descriptor.descriptor_evidence_sha256 != evidence_hash) {
    SetDetail(detail, "diagnostic control descriptor evidence differs");
    return {};
  }
  StoreArray(&output, 244, evidence_hash);
  const auto descriptor_hash =
      Hash(kDescriptorDomain, output.data(), output.size());
  if (AnyNonzero(descriptor.descriptor_sha256) &&
      descriptor.descriptor_sha256 != descriptor_hash) {
    SetDetail(detail, "diagnostic control descriptor hash differs");
    return {};
  }
  StoreArray(&output, 288, descriptor_hash);
  return output;
}

bool DecodeSblrDiagnosticControlDescriptorV1(
    const std::uint8_t* bytes,
    std::size_t size,
    SblrDiagnosticControlDescriptorV1* descriptor,
    std::string* detail) {
  if (bytes == nullptr || descriptor == nullptr ||
      size != DiagnosticControlWireLayout::descriptor_size ||
      !std::equal(bytes, bytes + 4, "SLDC") || bytes[4] != 1 ||
      LoadLe(bytes + 6, 2) != size ||
      !std::all_of(bytes + 66, bytes + 68,
                   [](std::uint8_t byte) { return byte == 0; }) ||
      !std::all_of(bytes + 276, bytes + 288,
                   [](std::uint8_t byte) { return byte == 0; })) {
    SetDetail(detail, "SLDC framing or reserved bytes are invalid");
    return false;
  }

  SblrDiagnosticControlDescriptorV1 value;
  value.flags = bytes[5];
  LoadArray(bytes, 8, &value.operation_uuid);
  LoadArray(bytes, 24, &value.authenticated_receipt_uuid);
  LoadArray(bytes, 40, &value.target_scope_uuid);
  value.target_scope_generation = LoadLe(bytes + 56, 8);
  value.scope_kind = static_cast<DiagnosticControlScopeKind>(bytes[64]);
  value.action = static_cast<DiagnosticControlAction>(bytes[65]);
  LoadArray(bytes, 68, &value.diagnostic_state_uuid);
  value.diagnostic_state_generation = LoadLe(bytes + 84, 8);
  LoadArray(bytes, 92, &value.security_context_uuid);
  LoadArray(bytes, 108, &value.policy_snapshot_uuid);
  value.policy_generation = LoadLe(bytes + 124, 8);
  LoadArray(bytes, 132, &value.transaction_uuid);
  LoadArray(bytes, 148, &value.route_provider_evidence_uuid);
  LoadArray(bytes, 164, &value.confirmation_token_uuid);
  value.parameter.kind =
      static_cast<DiagnosticControlParameterKind>(LoadLe(bytes + 180, 2));
  value.parameter.version = static_cast<std::uint16_t>(LoadLe(bytes + 182, 2));
  value.parameter.length = static_cast<std::uint32_t>(LoadLe(bytes + 184, 4));
  value.parameter.value = static_cast<std::uint32_t>(LoadLe(bytes + 188, 4));
  if (!std::all_of(bytes + 192, bytes + 196,
                   [](std::uint8_t byte) { return byte == 0; })) {
    SetDetail(detail, "SLDC parameter reserved bytes are invalid");
    return false;
  }
  LoadArray(bytes, 196, &value.parameter.parameter_uuid);
  LoadArray(bytes, 212, &value.parameter_sha256);
  LoadArray(bytes, 244, &value.descriptor_evidence_sha256);
  LoadArray(bytes, 288, &value.descriptor_sha256);
  if (!DescriptorFieldsValid(value, detail)) return false;

  const auto parameter = ParameterBytes(value.parameter);
  if (Hash(kParameterDomain, parameter.data(), parameter.size()) !=
      value.parameter_sha256) {
    SetDetail(detail, "SLDC parameter hash is invalid");
    return false;
  }
  std::vector<std::uint8_t> evidence_material(bytes, bytes + size);
  std::fill(evidence_material.begin() + 244, evidence_material.begin() + 276,
            0);
  std::fill(evidence_material.begin() + 288, evidence_material.end(), 0);
  if (Hash(kDescriptorEvidenceDomain, evidence_material.data(),
           evidence_material.size()) != value.descriptor_evidence_sha256) {
    SetDetail(detail, "SLDC descriptor evidence hash is invalid");
    return false;
  }
  std::vector<std::uint8_t> descriptor_material(bytes, bytes + size);
  std::fill(descriptor_material.begin() + 288, descriptor_material.end(), 0);
  if (Hash(kDescriptorDomain, descriptor_material.data(),
           descriptor_material.size()) != value.descriptor_sha256) {
    SetDetail(detail, "SLDC descriptor hash is invalid");
    return false;
  }
  *descriptor = value;
  return true;
}

std::vector<std::uint8_t> EncodeSblrDiagnosticControlResultV1(
    const SblrDiagnosticControlResultV1& result,
    std::string* detail) {
  if (!ResultFieldsValid(result, detail)) return {};
  std::vector<std::uint8_t> output(
      DiagnosticControlWireLayout::result_size, 0);
  std::copy_n("SLZR", 4, output.begin());
  output[4] = 1;
  output[5] = static_cast<std::uint8_t>(result.status);
  StoreArray(&output, 8, result.operation_uuid);
  StoreArray(&output, 24, result.target_scope_uuid);
  StoreLe(output.data() + 40, result.old_generation, 8);
  StoreLe(output.data() + 48, result.new_generation, 8);
  StoreArray(&output, 56, result.audit_event_uuid);
  StoreArray(&output, 72, result.evidence_uuid);
  StoreArray(&output, 120, result.recovery_replay_evidence_uuid);
  StoreArray(&output, 136, result.publication_barrier_uuid);
  StoreLe(output.data() + 152, result.diagnostic_vector_count, 8);
  const auto material_hash = Hash(kResultDomain, output.data(), output.size());
  if (AnyNonzero(result.material_sha256) &&
      result.material_sha256 != material_hash) {
    SetDetail(detail, "diagnostic control result material hash differs");
    return {};
  }
  StoreArray(&output, 88, material_hash);
  return output;
}

bool DecodeSblrDiagnosticControlResultV1(
    const std::uint8_t* bytes,
    std::size_t size,
    SblrDiagnosticControlResultV1* result,
    std::string* detail) {
  if (bytes == nullptr || result == nullptr ||
      size != DiagnosticControlWireLayout::result_size ||
      !std::equal(bytes, bytes + 4, "SLZR") || bytes[4] != 1 ||
      !std::all_of(bytes + 6, bytes + 8,
                   [](std::uint8_t byte) { return byte == 0; }) ||
      !std::all_of(bytes + 160, bytes + 192,
                   [](std::uint8_t byte) { return byte == 0; })) {
    SetDetail(detail, "SLZR framing or reserved bytes are invalid");
    return false;
  }
  SblrDiagnosticControlResultV1 value;
  value.status = static_cast<DiagnosticControlResultStatus>(bytes[5]);
  LoadArray(bytes, 8, &value.operation_uuid);
  LoadArray(bytes, 24, &value.target_scope_uuid);
  value.old_generation = LoadLe(bytes + 40, 8);
  value.new_generation = LoadLe(bytes + 48, 8);
  LoadArray(bytes, 56, &value.audit_event_uuid);
  LoadArray(bytes, 72, &value.evidence_uuid);
  LoadArray(bytes, 88, &value.material_sha256);
  LoadArray(bytes, 120, &value.recovery_replay_evidence_uuid);
  LoadArray(bytes, 136, &value.publication_barrier_uuid);
  value.diagnostic_vector_count = LoadLe(bytes + 152, 8);
  if (!ResultFieldsValid(value, detail)) return false;
  std::vector<std::uint8_t> material(bytes, bytes + size);
  std::fill(material.begin() + 88, material.begin() + 120, 0);
  if (Hash(kResultDomain, material.data(), material.size()) !=
      value.material_sha256) {
    SetDetail(detail, "SLZR material hash is invalid");
    return false;
  }
  *result = value;
  return true;
}

}  // namespace scratchbird::engine::sblr
