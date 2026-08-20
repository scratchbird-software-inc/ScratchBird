// Copyright (c) 2026 ScratchBird Software Inc.
// SPDX-License-Identifier: MPL-2.0

#include "server/sblr_local_gateway.hpp"
#include "engine/sblr/sblr_opcode_stream.hpp"

#include <algorithm>
#include <array>
#include <iostream>
#include <string_view>
#include <utility>
#include <vector>

namespace server = scratchbird::server;
namespace sblr = scratchbird::engine::sblr;

namespace {
constexpr std::string_view kPackageUuid =
    "018f1234-5678-7abc-8def-0123456789ab";
constexpr std::string_view kRegistryUuid =
    "018f4321-8765-7cba-8fed-ba9876543210";
constexpr std::string_view kParserUuid =
    "018faaaa-bbbb-7ccc-8ddd-eeeeeeeeeeee";
const std::array<std::uint8_t, 16> kPackageBytes{
    0x01, 0x8f, 0x12, 0x34, 0x56, 0x78, 0x7a, 0xbc,
    0x8d, 0xef, 0x01, 0x23, 0x45, 0x67, 0x89, 0xab};

sblr::SblrOperationEnvelope Frame(bool begin) {
  auto operation = sblr::MakeSblrEnvelope(
      begin ? "engine.op.package_begin" : "engine.op.package_end",
      begin ? "SBLR_PACKAGE_BEGIN" : "SBLR_PACKAGE_END", "gateway.frame");
  operation.opcode_code = begin ? 1 : 2;
  operation.result_shape = "void";
  operation.diagnostic_shape = "diagnostic_vector";
  operation.parser_package_uuid = kParserUuid;
  operation.registry_snapshot_uuid = kRegistryUuid;
  sblr::SblrOperand operand;
  operand.ordinal = 1;
  operand.type = begin ? "package.header" : "package.footer";
  operand.name = "package_descriptor";
  operand.value_kind = sblr::SblrValueKind::descriptor_ref;
  operand.value_body.assign(kPackageBytes.begin(), kPackageBytes.end());
  operation.operands.push_back(std::move(operand));
  return operation;
}

std::vector<std::uint8_t> CanonicalPackage() {
  auto root = sblr::MakeSblrEnvelope(
      "query.execute", "SBLR_QUERY_EXECUTE", "gateway.root");
  root.opcode_code = 0x1207;
  root.result_shape = "query_execute_result";
  root.diagnostic_shape = "diagnostic_vector";
  root.parser_package_uuid = kParserUuid;
  root.registry_snapshot_uuid = kRegistryUuid;
  sblr::SblrOpcodeStream stream;
  stream.package_descriptor_uuid = kPackageUuid;
  stream.registry_snapshot_uuid = kRegistryUuid;
  stream.operations = {Frame(true), std::move(root), Frame(false)};
  return sblr::EncodeSblrOpcodeStream(stream);
}
}  // namespace

int main() {
  int failures = 0;
  server::LocalSblrGatewayRequest request;
  request.canonical_sbos = CanonicalPackage();
  if (request.canonical_sbos.empty()) return 2;
  request.root_opcode_code = 0x1207;
  request.root_opcode = "SBLR_QUERY_EXECUTE";
  request.root_operation_id = "query.execute";
  request.route_snapshot_uuid = "018f1111-2222-7333-8444-555555555555";
  request.route_epoch = 7;
  request.route_generation = 9;
  request.security_snapshot_uuid = "018faaaa-bbbb-7ccc-8ddd-eeeeeeeeeeee";
  request.security_epoch = 11;
  request.security_observation_generation = 13;
  request.route_snapshot_engine_owned = true;
  request.security_snapshot_engine_owned = true;

  const auto admitted = server::AdmitLocalNoClusterSblrGateway(request);
  if (!admitted.ok || admitted.disposition !=
                          server::LocalSblrGatewayDisposition::kPassThrough ||
      admitted.gateway_observation_generation != 1 ||
      admitted.route_epoch != request.route_epoch ||
      admitted.route_generation != request.route_generation ||
      admitted.security_epoch != request.security_epoch ||
      admitted.security_observation_generation !=
          request.security_observation_generation ||
      std::all_of(admitted.canonical_payload_sha256.begin(),
                  admitted.canonical_payload_sha256.end(),
                  [](std::uint8_t byte) { return byte == 0; })) {
    std::cerr << "local query.execute PASS_THROUGH evidence mismatch\n";
    ++failures;
  }

  for (unsigned predicate = 0; predicate != 3; ++predicate) {
    auto clustered = request;
    clustered.cluster_context_active = predicate == 0;
    clustered.cluster_transaction_active = predicate == 1;
    clustered.route_fence_present = predicate == 2;
    const auto refused = server::AdmitLocalNoClusterSblrGateway(clustered);
    if (refused.ok || refused.disposition !=
                          server::LocalSblrGatewayDisposition::kRefused ||
        refused.diagnostic_id !=
            "CLUSTER.GATEWAY_CLUSTER_FALLTHROUGH_FORBIDDEN") {
      std::cerr << "cluster activation did not fail closed: " << predicate << '\n';
      ++failures;
    }
  }

  auto stale = request;
  stale.route_generation = 0;
  const auto stale_result = server::AdmitLocalNoClusterSblrGateway(stale);
  if (stale_result.ok ||
      stale_result.diagnostic_id != "SBLR.INGRESS_REVALIDATION_FAILED") {
    std::cerr << "stale route snapshot did not fail closed\n";
    ++failures;
  }
  auto wrong_root = request;
  wrong_root.root_operation_id = "query.plan_operation";
  if (server::AdmitLocalNoClusterSblrGateway(wrong_root).ok) {
    std::cerr << "wrong contained root received local pass-through\n";
    ++failures;
  }
  auto payload_changed = request;
  payload_changed.canonical_sbos.push_back(0);
  const auto changed = server::AdmitLocalNoClusterSblrGateway(payload_changed);
  if (changed.ok ||
      changed.diagnostic_id != "SBLR.INGRESS_REVALIDATION_FAILED") {
    std::cerr << "noncanonical changed SBOS received gateway admission\n";
    ++failures;
  }
  return failures == 0 ? 0 : 1;
}
