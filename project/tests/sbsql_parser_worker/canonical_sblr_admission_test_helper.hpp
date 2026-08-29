// Copyright (c) 2026 ScratchBird Software Inc.
//
// This Source Code Form is subject to the terms of the Mozilla Public
// License, v. 2.0. If a copy of the MPL was not distributed with this
// file, You can obtain one at https://mozilla.org/MPL/2.0/.
//
// SPDX-License-Identifier: MPL-2.0

#pragma once

#include "sblr_admission.hpp"
#include "sblr_engine_envelope.hpp"
#include "sblr_opcode_registry.hpp"
#include "scratchbird/engine/sblr_envelope.hpp"

#include <algorithm>
#include <array>
#include <cstdint>
#include <sstream>
#include <stdexcept>
#include <string>
#include <string_view>
#include <utility>
#include <vector>

namespace scratchbird::test::sbsql {

namespace detail {

using CanonicalBytes = std::vector<std::uint8_t>;

inline std::array<std::uint8_t, 16> AdmissionUuid(std::uint8_t suffix) {
  std::array<std::uint8_t, 16> value{};
  value[0] = 0x12;
  value[1] = 0x34;
  value[6] = 0x70;
  value[8] = 0x80;
  value[15] = suffix;
  return value;
}

inline std::string AdmissionUuidText(
    const std::array<std::uint8_t, 16>& value) {
  constexpr char kHex[] = "0123456789abcdef";
  std::string text;
  text.reserve(36);
  for (std::size_t i = 0; i < value.size(); ++i) {
    if (i == 4 || i == 6 || i == 8 || i == 10) text.push_back('-');
    text.push_back(kHex[value[i] >> 4]);
    text.push_back(kHex[value[i] & 0x0f]);
  }
  return text;
}

inline CanonicalBytes AdmissionUuidField(
    const std::array<std::uint8_t, 16>& value) {
  return {value.begin(), value.end()};
}

inline CanonicalBytes AdmissionU16(std::uint16_t value) {
  CanonicalBytes out;
  scratchbird::engine::SblrAppendU16(out, value);
  return out;
}

inline CanonicalBytes AdmissionU32(std::uint32_t value) {
  CanonicalBytes out;
  scratchbird::engine::SblrAppendU32(out, value);
  return out;
}

inline CanonicalBytes AdmissionU64(std::uint64_t value) {
  CanonicalBytes out;
  scratchbird::engine::SblrAppendU64(out, value);
  return out;
}

inline CanonicalBytes AdmissionStruct(std::uint32_t tag,
                                      std::uint8_t value) {
  CanonicalBytes out;
  scratchbird::engine::SblrAppendU32(out, tag);
  scratchbird::engine::SblrAppendU16(out, 1);
  scratchbird::engine::SblrAppendU16(out, 0);
  scratchbird::engine::SblrAppendU64(out, 1);
  out.push_back(value);
  return out;
}

inline CanonicalBytes AdmissionInline(const CanonicalBytes& payload) {
  CanonicalBytes out{1};
  scratchbird::engine::SblrAppendU64(out, payload.size());
  out.insert(out.end(), payload.begin(), payload.end());
  return out;
}

inline CanonicalBytes AdmissionOptionalUuid(
    const std::array<std::uint8_t, 16>& value) {
  CanonicalBytes out{1};
  out.insert(out.end(), value.begin(), value.end());
  return out;
}

}  // namespace detail

// Completes the engine-owned identity fields that parser-worker unit tests do
// not receive from a live parser-package reservation. Unknown operations and
// mnemonic aliases remain zero/unchanged so production validation still fails
// closed; this helper never synthesizes an opcode identity.
inline scratchbird::engine::sblr::SblrOperationEnvelope
CanonicalizeEngineSblrEnvelopeForTest(
    scratchbird::engine::sblr::SblrOperationEnvelope operation) {
  namespace engine_sblr = scratchbird::engine::sblr;
  const auto* registry = engine_sblr::LookupSblrOperation(operation.operation_id);
  if (registry == nullptr) {
    throw std::runtime_error("canonical engine operation is not registered: " +
                             operation.operation_id);
  }
  if (registry->opcode != operation.opcode || registry->code == 0) {
    throw std::runtime_error("canonical engine opcode identity is unavailable for " +
                             operation.operation_id);
  }
  operation.opcode_code = registry->code;
  operation.parser_package_uuid =
      detail::AdmissionUuidText(detail::AdmissionUuid(0x31));
  operation.registry_snapshot_uuid =
      detail::AdmissionUuidText(detail::AdmissionUuid(0x32));
  operation.parser_resolved_names_to_uuids = true;
  for (std::size_t index = 0; index < operation.operands.size(); ++index) {
    auto& operand = operation.operands[index];
    const bool legacy_operand = operand.ordinal == 0;
    if (legacy_operand) {
      operand.ordinal = static_cast<std::uint32_t>(index + 1);
    }
    if (legacy_operand &&
        operand.value_kind == engine_sblr::SblrValueKind::null_value &&
        operand.value_body.empty()) {
      const auto type_uuid = detail::AdmissionUuid(0x51);
      operand.value_kind = engine_sblr::SblrValueKind::literal_typed;
      operand.value_body.assign(type_uuid.begin(), type_uuid.end());
      const auto value_size = detail::AdmissionU64(operand.value.size());
      operand.value_body.insert(operand.value_body.end(), value_size.begin(),
                                value_size.end());
      operand.value_body.insert(operand.value_body.end(), operand.value.begin(),
                                operand.value.end());
      operand.value.clear();
    }
  }
  return operation;
}

inline scratchbird::engine::sblr::SblrOperationEnvelope
BuildCanonicalEngineSblrEnvelopeForTest(std::string_view operation_id,
                                        std::string_view opcode,
                                        std::string_view trace_key) {
  return CanonicalizeEngineSblrEnvelopeForTest(
      scratchbird::engine::sblr::MakeSblrEnvelope(
          std::string(operation_id), std::string(opcode), std::string(trace_key)));
}

// Builds the same canonical SBOP -> SBLR container + SBEE ingress chain used
// by the server.  Parser-worker tests use this instead of the retired textual
// encoded_sblr_envelope compatibility field, whose only valid behavior is
// deterministic SBLR.OPERATION.NONCANONICAL refusal.
inline scratchbird::server::ServerSblrAdmissionRequest
BuildCanonicalSblrAdmissionRequest(
    scratchbird::engine::sblr::SblrOperationEnvelope operation,
    bool cluster_authority_active = false) {
  namespace engine_sblr = scratchbird::engine::sblr;
  namespace public_sblr = scratchbird::engine;
  using detail::CanonicalBytes;

  const std::string operation_id = operation.operation_id;
  const std::string opcode = operation.opcode;
  const auto* registry = engine_sblr::LookupSblrOperation(operation_id);
  if (registry == nullptr) {
    throw std::runtime_error("canonical admission operation is not registered: " +
                             std::string(operation_id));
  }
  if (registry->opcode != opcode) {
    throw std::runtime_error("canonical admission opcode mismatch for " +
                             std::string(operation_id) + ": expected " +
                             registry->opcode + ", received " +
                             std::string(opcode));
  }

  operation = CanonicalizeEngineSblrEnvelopeForTest(std::move(operation));
  const auto parser_uuid = detail::AdmissionUuid(0x31);
  const auto dialect_uuid = detail::AdmissionUuid(0x22);
  const auto registry_uuid = detail::AdmissionUuid(0x32);
  const auto user_uuid = detail::AdmissionUuid(0x42);
  operation.requires_security_context = registry->requires_security_context;
  operation.requires_transaction_context =
      registry->requires_transaction_context;
  operation.requires_cluster_authority = registry->requires_cluster_authority;
  operation.parser_resolved_names_to_uuids = true;
  const auto validation = engine_sblr::ValidateSblrEnvelope(operation);
  if (!validation.ok) {
    std::ostringstream message;
    message << "canonical admission SBOP validation failed for " << operation_id;
    for (const auto& diagnostic : validation.diagnostics) {
      message << "; " << diagnostic.code << '=' << diagnostic.message;
    }
    throw std::runtime_error(message.str());
  }
  const auto sbop_text = engine_sblr::EncodeSblrEnvelope(operation);
  const CanonicalBytes sbop(sbop_text.begin(), sbop_text.end());
  if (sbop.empty()) {
    throw std::runtime_error("canonical admission SBOP encoding failed for " +
                             std::string(operation_id));
  }

  public_sblr::SblrCanonicalContainer container;
  const auto engine_uuid = detail::AdmissionUuid(0x21);
  const auto bundle_uuid = detail::AdmissionUuid(0x24);
  const auto request_uuid = detail::AdmissionUuid(0x25);
  std::copy(engine_uuid.begin(), engine_uuid.end(),
            container.canonical_anchor.begin());
  std::copy(dialect_uuid.begin(), dialect_uuid.end(),
            container.canonical_anchor.begin() + 16);
  std::copy(parser_uuid.begin(), parser_uuid.end(),
            container.canonical_anchor.begin() + 32);
  container.canonical_anchor[48] = 1;
  container.canonical_anchor[52] = 1;
  container.canonical_anchor[60] = 1;
  container.canonical_anchor[68] = 1;
  std::copy(bundle_uuid.begin(), bundle_uuid.end(),
            container.canonical_anchor.begin() + 76);
  container.canonical_anchor[92] = 1;
  container.canonical_anchor[100] = 2;
  std::copy(request_uuid.begin(), request_uuid.end(),
            container.canonical_anchor.begin() + 116);
  container.operation_payload = sbop;
  const auto container_bytes = public_sblr::EncodeSblrContainer(container);

  public_sblr::SblrExecutionEnvelopeV1 ingress;
  auto& fields = ingress.fields;
  fields[0] = detail::AdmissionUuidField(detail::AdmissionUuid(0x41));
  fields[1] = detail::AdmissionU16(1);
  fields[2] = detail::AdmissionU16(0);
  fields[3] = detail::AdmissionU32(0x00010001);
  fields[4] = detail::AdmissionU16(2);
  fields[5] = {0};
  fields[6] = detail::AdmissionInline(sbop);
  fields[7] = {1};
  const auto crc =
      detail::AdmissionU32(public_sblr::SblrCrc32c(sbop.data(), sbop.size()));
  fields[7].insert(fields[7].end(), crc.begin(), crc.end());
  fields[8] = detail::AdmissionU64(sbop.size());
  fields[9] = detail::AdmissionU16(1);
  fields[10] = detail::AdmissionOptionalUuid(dialect_uuid);
  fields[11] = detail::AdmissionOptionalUuid(user_uuid);
  fields[12] = detail::AdmissionStruct(0x1001, 1);
  fields[13] = detail::AdmissionStruct(0x1002, 2);
  fields[14] = {0};
  fields[15] = detail::AdmissionU64(1);
  fields[16] = detail::AdmissionU32(0);
  fields[17] = detail::AdmissionU32(0);
  fields[18] = detail::AdmissionU32(0);
  fields[19] = {0};
  fields[20] = detail::AdmissionU32(0);
  fields[21] = detail::AdmissionStruct(0x1005, 5);
  fields[22] = {0};
  fields[23] = {0};
  fields[24] = {0};
  fields[25] = detail::AdmissionU16(0);
  fields[26] = {0};
  fields[27] = {0};
  const auto ingress_bytes =
      public_sblr::EncodeSblrExecutionEnvelopeV1(ingress);

  scratchbird::server::ServerSblrAdmissionRequest request;
  request.cluster_authority_active = cluster_authority_active;
  request.encoded_sblr_container.assign(container_bytes.begin(),
                                        container_bytes.end());
  request.encoded_execution_envelope.assign(ingress_bytes.begin(),
                                            ingress_bytes.end());
  request.admitted_parser_package_uuid =
      detail::AdmissionUuidText(parser_uuid);
  request.admitted_parser_package_version_major = 1;
  request.admitted_registry_snapshot_uuid =
      detail::AdmissionUuidText(registry_uuid);
  request.authenticated_principal_uuid = detail::AdmissionUuidText(user_uuid);
  request.catalog_snapshot_uuid =
      detail::AdmissionUuidText(detail::AdmissionUuid(0x43));
  request.engine_mga_statement_uuid =
      detail::AdmissionUuidText(detail::AdmissionUuid(0x44));
  request.engine_mga_snapshot_uuid =
      detail::AdmissionUuidText(detail::AdmissionUuid(0x45));
  request.catalog_epoch = 7;
  request.security_epoch = 8;
  request.resource_epoch = 9;
  return request;
}

inline scratchbird::server::ServerSblrAdmissionRequest
BuildCanonicalSblrAdmissionRequest(std::string_view operation_id,
                                   std::string_view opcode,
                                   bool cluster_authority_active = false) {
  auto operation = scratchbird::engine::sblr::MakeSblrEnvelope(
      std::string(operation_id), std::string(opcode),
      "parser-worker-canonical-admission");
  return BuildCanonicalSblrAdmissionRequest(std::move(operation),
                                            cluster_authority_active);
}

template <typename ParserEnvelope>
  requires requires(const ParserEnvelope& envelope) {
    envelope.operation_id;
    envelope.sblr_opcode;
  }
inline scratchbird::server::ServerSblrAdmissionRequest
BuildCanonicalSblrAdmissionRequest(const ParserEnvelope& envelope,
                                   bool cluster_authority_active = false) {
  return BuildCanonicalSblrAdmissionRequest(envelope.operation_id,
                                            envelope.sblr_opcode,
                                            cluster_authority_active);
}

}  // namespace scratchbird::test::sbsql
