// Copyright (c) 2026 ScratchBird Software Inc.
// SPDX-License-Identifier: MPL-2.0

#include "sblr_local_gateway.hpp"

#include "core/hash/hash_digest.hpp"
#include "engine/sblr/sblr_opcode_stream.hpp"

#include <algorithm>

namespace scratchbird::server {
namespace {

bool CanonicalNonzeroUuid(const std::string& value) {
  if (value.size() != 36 || value[8] != '-' || value[13] != '-' ||
      value[18] != '-' || value[23] != '-') return false;
  bool nonzero = false;
  for (std::size_t index = 0; index < value.size(); ++index) {
    if (value[index] == '-') continue;
    const char ch = value[index];
    if (!((ch >= '0' && ch <= '9') || (ch >= 'a' && ch <= 'f'))) return false;
    nonzero = nonzero || ch != '0';
  }
  return nonzero;
}

LocalSblrGatewayDecision Refuse(const LocalSblrGatewayRequest& request,
                                std::string diagnostic_id) {
  LocalSblrGatewayDecision decision;
  decision.diagnostic_id = std::move(diagnostic_id);
  decision.route_snapshot_uuid = request.route_snapshot_uuid;
  decision.route_epoch = request.route_epoch;
  decision.route_generation = request.route_generation;
  decision.security_snapshot_uuid = request.security_snapshot_uuid;
  decision.security_epoch = request.security_epoch;
  decision.security_observation_generation =
      request.security_observation_generation;
  decision.cluster_context_active = request.cluster_context_active;
  decision.cluster_transaction_active = request.cluster_transaction_active;
  decision.route_fence_present = request.route_fence_present;
  if (!request.canonical_sbos.empty()) {
    const auto digest = scratchbird::core::hash::ComputeSha256Digest(
        request.canonical_sbos);
    if (digest.ok()) decision.canonical_payload_sha256 = digest.digest;
  }
  return decision;
}

}  // namespace

LocalSblrGatewayDecision AdmitLocalNoClusterSblrGateway(
    const LocalSblrGatewayRequest& request) {
  const std::string_view encoded(
      reinterpret_cast<const char*>(request.canonical_sbos.data()),
      request.canonical_sbos.size());
  const auto stream = scratchbird::engine::sblr::DecodeSblrOpcodeStream(encoded);
  if (!stream.ok || stream.stream.operations.size() != 3 ||
      request.root_opcode_code != 0x1207u ||
      request.root_opcode != "SBLR_QUERY_EXECUTE" ||
      request.root_operation_id != "query.execute" ||
      stream.stream.operations[1].opcode_code != request.root_opcode_code ||
      stream.stream.operations[1].opcode != request.root_opcode ||
      stream.stream.operations[1].operation_id != request.root_operation_id ||
      !request.route_snapshot_engine_owned ||
      !request.security_snapshot_engine_owned ||
      !CanonicalNonzeroUuid(request.route_snapshot_uuid) ||
      !CanonicalNonzeroUuid(request.security_snapshot_uuid) ||
      request.route_epoch == 0 || request.route_generation == 0 ||
      request.security_epoch == 0 ||
      request.security_observation_generation == 0) {
    return Refuse(request, "SBLR.INGRESS_REVALIDATION_FAILED");
  }
  if (request.cluster_context_active || request.cluster_transaction_active ||
      request.route_fence_present) {
    return Refuse(request,
                  "CLUSTER.GATEWAY_CLUSTER_FALLTHROUGH_FORBIDDEN");
  }
  auto decision = Refuse(request, {});
  if (std::all_of(decision.canonical_payload_sha256.begin(),
                  decision.canonical_payload_sha256.end(),
                  [](std::uint8_t byte) { return byte == 0; })) {
    decision.diagnostic_id = "SBLR.INGRESS_REVALIDATION_FAILED";
    return decision;
  }
  decision.ok = true;
  decision.disposition = LocalSblrGatewayDisposition::kPassThrough;
  decision.gateway_observation_generation = 1;
  return decision;
}

}  // namespace scratchbird::server
