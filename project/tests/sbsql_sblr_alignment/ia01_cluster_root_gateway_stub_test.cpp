// Copyright (c) 2026 ScratchBird Software Inc.
// SPDX-License-Identifier: MPL-2.0

#include "cluster_provider/cluster_provider.hpp"
#include "engine/sblr/sblr_dispatch.hpp"
#include "engine/sblr/sblr_opcode_stream.hpp"
#include "server/sblr_local_gateway.hpp"

#include <array>
#include <algorithm>
#include <cstdlib>
#include <iostream>
#include <string>

namespace sblr = scratchbird::engine::sblr;

namespace {
constexpr std::string_view kParserUuid =
    "018faaaa-bbbb-7ccc-8ddd-eeeeeeeeeeee";
constexpr std::string_view kRegistryUuid =
    "018f4321-8765-7cba-8fed-ba9876543210";

sblr::SblrOperationEnvelope Frame(bool begin) {
  auto operation = sblr::MakeSblrEnvelope(
      begin ? "engine.op.package_begin" : "engine.op.package_end",
      begin ? "SBLR_PACKAGE_BEGIN" : "SBLR_PACKAGE_END", "cluster.gateway");
  operation.opcode_code = begin ? 1 : 2;
  operation.result_shape = "void";
  operation.diagnostic_shape = "diagnostic_vector";
  operation.parser_package_uuid = std::string(kParserUuid);
  operation.registry_snapshot_uuid = std::string(kRegistryUuid);
  sblr::SblrOperand operand;
  operand.ordinal = 1;
  operand.type = begin ? "package.header" : "package.footer";
  operand.name = "package_descriptor";
  operand.value_kind = sblr::SblrValueKind::descriptor_ref;
  operand.value_body = {0x01, 0x8f, 0x12, 0x34, 0x56, 0x78, 0x7a, 0xbc,
                        0x8d, 0xef, 0x01, 0x23, 0x45, 0x67, 0x89, 0xab};
  operation.operands.push_back(std::move(operand));
  return operation;
}

std::vector<std::uint8_t> ClusterPackage() {
  auto root = sblr::MakeSblrEnvelope(
      "cluster.join", "SBLR_CLUSTER_JOIN", "cluster.gateway");
  root.opcode_code = 0x0B00;
  root.result_shape = "void";
  root.diagnostic_shape = "diagnostic_vector";
  root.requires_security_context = true;
  root.requires_transaction_context = true;
  root.requires_cluster_authority = true;
  root.parser_package_uuid = std::string(kParserUuid);
  root.registry_snapshot_uuid = std::string(kRegistryUuid);

  sblr::SblrOpcodeStream stream;
  stream.package_descriptor_uuid = "018f1234-5678-7abc-8def-0123456789ab";
  stream.registry_snapshot_uuid = std::string(kRegistryUuid);
  stream.operations = {Frame(true), std::move(root), Frame(false)};
  return sblr::EncodeSblrOpcodeStream(stream);
}
}  // namespace

int main() {
  const auto payload = ClusterPackage();
  scratchbird::server::LocalSblrGatewayRequest gateway_request;
  gateway_request.canonical_sbos = payload;
  gateway_request.root_opcode_code = 0x0B00;
  gateway_request.root_opcode = "SBLR_CLUSTER_JOIN";
  gateway_request.root_operation_id = "cluster.join";
  gateway_request.route_snapshot_uuid = "018f1111-2222-7333-8444-555555555555";
  gateway_request.route_epoch = 7;
  gateway_request.route_generation = 9;
  gateway_request.security_snapshot_uuid = "018faaaa-bbbb-7ccc-8ddd-eeeeeeeeeeee";
  gateway_request.security_epoch = 11;
  gateway_request.security_observation_generation = 13;
  gateway_request.route_snapshot_engine_owned = true;
  gateway_request.security_snapshot_engine_owned = true;

  const auto admitted = scratchbird::server::AdmitLocalNoClusterSblrGateway(
      gateway_request);
  if (!admitted.ok || admitted.disposition !=
                          scratchbird::server::LocalSblrGatewayDisposition::kPassThrough) {
    const auto decoded = sblr::DecodeSblrOpcodeStream(
        std::string_view(reinterpret_cast<const char*>(payload.data()), payload.size()));
    std::cerr << "stream_ok=" << decoded.ok << " detail=" << decoded.detail
              << " diagnostic=" << decoded.diagnostic_id << '\n';
    std::cerr << "cluster root was rejected before SBLR provider dispatch: "
              << admitted.diagnostic_id << '\n';
    return EXIT_FAILURE;
  }

  auto envelope = sblr::MakeSblrEnvelope(
      "cluster.join", "SBLR_CLUSTER_JOIN", "cluster.gateway");
  envelope.opcode_code = 0x0B00;
  envelope.requires_security_context = true;
  envelope.requires_transaction_context = true;
  envelope.requires_cluster_authority = true;
  envelope.parser_package_uuid = std::string(kParserUuid);
  envelope.registry_snapshot_uuid = std::string(kRegistryUuid);
  sblr::SblrDispatchRequest dispatch_request;
  dispatch_request.context.security_context_present = true;
  dispatch_request.context.local_transaction_id = 7;
  dispatch_request.envelope = std::move(envelope);
  const auto dispatched = sblr::DispatchSblrOperation(dispatch_request);
  if (!dispatched.accepted || !dispatched.dispatched_to_api ||
      dispatched.api_result.ok || dispatched.api_result.result_shape.rows.size() != 0) {
    for (const auto& diagnostic : dispatched.diagnostics) {
      std::cerr << "dispatch diagnostic=" << diagnostic.code << '\n';
    }
    for (const auto& diagnostic : dispatched.api_result.diagnostics) {
      std::cerr << "api diagnostic=" << diagnostic.code << '\n';
    }
    std::cerr << "cluster root did not fail closed at the configured provider\n";
    return EXIT_FAILURE;
  }
  const auto& info = scratchbird::engine::cluster_provider::DescribeClusterProvider();
  const std::string expected = info.provider_type == "compile_link_stub"
      ? std::string(scratchbird::engine::cluster_provider::kClusterHandshakeStubCompileLinkOnlyCode)
      : std::string(scratchbird::engine::cluster_provider::kClusterSupportNotEnabledCode);
  const bool diagnostic_present = std::any_of(
      dispatched.api_result.diagnostics.begin(), dispatched.api_result.diagnostics.end(),
      [&](const auto& diagnostic) { return diagnostic.code == expected; });
  if (!diagnostic_present) {
    std::cerr << "cluster provider refusal diagnostic missing: " << expected << '\n';
    return EXIT_FAILURE;
  }
  return EXIT_SUCCESS;
}
