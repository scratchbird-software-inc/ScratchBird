// Copyright (c) 2026 ScratchBird Software Inc.
// SPDX-License-Identifier: MPL-2.0
#pragma once
#include "../core/platform/runtime_platform.hpp"

#include <cstdint>
#include <limits>
#include <optional>
#include <stdexcept>
#include <string_view>
#include <vector>

namespace scratchbird::server_engine_bridge {

// Taken from the server reservation at issuance and the consumed engine-owned
// reservation at dispatch. Encoding does not confer reservation authority.
struct AdmissionReservationBinding {
  std::uint64_t handle = 0;
  std::uint8_t payload_kind = 0;
  std::uint64_t payload_size = 0;
  std::uint32_t record_count = 0;
  std::uint64_t resource_policy_generation = 0;
};

inline constexpr std::string_view kAdmissionTokenBindingDomainV2 =
    "ScratchBird.SBLR.AdmissionToken.BinaryUuid.V2";

// Shared by server issuance and engine revalidation. UUID fields must have a
// raw sixteen-byte .bytes carrier: no text overload, formatter or UUID parser.
// Executor identifiers are length-framed TEXT; embedded NUL cannot move a
// boundary. This is an encoding function, not an identity/receipt validator.
template <typename Fields>
std::vector<std::uint8_t> EncodeAdmissionTokenBindingV2(
    const Fields& fields,
    const std::optional<AdmissionReservationBinding>& reservation = std::nullopt) {
  std::vector<std::uint8_t> out;
  const auto append = [&](const auto& bytes) {
    out.insert(out.end(), bytes.begin(), bytes.end());
  };
  const auto uuid = [&](const scratchbird::core::platform::Uuid& value) {
    append(value.bytes);
  };
  const auto number = [&](std::uint64_t value, unsigned count) {
    for (unsigned index = 0; index < count; ++index)
      out.push_back(static_cast<std::uint8_t>(value >> (8 * index)));
  };
  const auto text = [&](std::string_view value) {
    if (value.size() > std::numeric_limits<std::uint32_t>::max())
      throw std::length_error("admission executor identifier exceeds uint32 framing");
    number(value.size(), 4);
    append(value);
  };
  append(kAdmissionTokenBindingDomainV2);
  append(fields.container_sha256);
  append(fields.execution_envelope_sha256);
  append(fields.operation_sha256);
  uuid(fields.authenticated_principal_uuid);
  uuid(fields.catalog_snapshot_uuid);
  uuid(fields.engine_mga_statement_uuid);
  uuid(fields.engine_mga_snapshot_uuid);
  number(fields.catalog_epoch, 8);
  number(fields.security_epoch, 8);
  number(fields.resource_epoch, 8);
  out.push_back(reservation.has_value() ? 1 : 0);
  if (reservation) {
    number(reservation->handle, 8);
    number(reservation->payload_kind, 1);
    number(reservation->payload_size, 8);
    number(reservation->record_count, 4);
    number(reservation->resource_policy_generation, 8);
    const auto& gateway = fields.gateway_evidence;
    number(static_cast<std::uint8_t>(gateway.source), 1);
    number(static_cast<std::uint8_t>(gateway.disposition), 1);
    number(gateway.provider_observation_generation, 8);
    append(gateway.canonical_payload_sha256);
    uuid(gateway.route_snapshot_uuid);
    uuid(gateway.security_snapshot_uuid);
    number(gateway.route_epoch, 8);
    number(gateway.route_generation, 8);
    number(gateway.security_epoch, 8);
    number(gateway.security_observation_generation, 8);
    out.push_back(gateway.cluster_context_active ? 1 : 0);
    out.push_back(gateway.cluster_transaction_active ? 1 : 0);
    out.push_back(gateway.route_fence_present ? 1 : 0);
    const auto& executor = fields.package_executor_evidence;
    text(executor.begin_executor_id);
    text(executor.end_executor_id);
    uuid(executor.registry_snapshot_uuid);
    number(executor.executor_evidence_generation, 8);
    append(executor.canonical_payload_sha256);
  }
  return out;
}

}  // namespace scratchbird::server_engine_bridge
