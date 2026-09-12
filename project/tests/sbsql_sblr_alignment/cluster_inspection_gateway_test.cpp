// Copyright (c) 2026 ScratchBird Software Inc.
// SPDX-License-Identifier: MPL-2.0

#include "canonical_sblr_admission_test_helper.hpp"
#include "cluster_provider/cluster_provider.hpp"
#include "engine/sblr/sblr_dispatch.hpp"
#include "engine/sblr/sblr_opcode_stream.hpp"
#include "server/sblr_local_gateway.hpp"
#include "uuid.hpp"

#include <algorithm>
#include <cstdlib>
#include <iostream>

namespace sblr = scratchbird::engine::sblr;
namespace server = scratchbird::server;
namespace provider = scratchbird::engine::cluster_provider;

namespace {
std::size_t checks = 0, failures = 0, dispatches = 0;
void Check(bool condition, const char* message) {
  ++checks;
  if (!condition && ++failures <= 16) std::cerr << message << '\n';
}

sblr::SblrOperationEnvelope Frame(bool begin, const std::string& descriptor) {
  auto frame = scratchbird::test::sbsql::BuildCanonicalEngineSblrEnvelopeForTest(
      begin ? "engine.op.package_begin" : "engine.op.package_end",
      begin ? "SBLR_PACKAGE_BEGIN" : "SBLR_PACKAGE_END", "inspection.gateway");
  frame.result_shape = "void";
  const auto parsed = scratchbird::core::uuid::ParseUuid(descriptor);
  Check(parsed.ok(), "fixture package identity was not a UUID");
  sblr::SblrOperand operand;
  operand.ordinal = 1;
  operand.type = begin ? "package.header" : "package.footer";
  operand.name = "package_descriptor";
  operand.value_kind = sblr::SblrValueKind::descriptor_ref;
  operand.value_body.assign(parsed.value.bytes.begin(), parsed.value.bytes.end());
  frame.operands.push_back(std::move(operand));
  return frame;
}

sblr::SblrOpcodeStream Package() {
  auto root = scratchbird::test::sbsql::BuildCanonicalEngineSblrEnvelopeForTest(
      "cluster.inspect_provider", "SBLR_CLUSTER_INSPECT_PROVIDER", "inspection.gateway");
  root.result_shape = "cluster_provider_inspection_result";
  root.requires_security_context = true;
  root.requires_transaction_context = true;
  root.requires_cluster_authority = true;
  sblr::SblrOpcodeStream stream;
  stream.package_descriptor_uuid = root.parser_package_uuid;
  stream.registry_snapshot_uuid = root.registry_snapshot_uuid;
  stream.operations = {Frame(true, stream.package_descriptor_uuid), root,
                       Frame(false, stream.package_descriptor_uuid)};
  return stream;
}

server::LocalSblrGatewayRequest Request(const sblr::SblrOpcodeStream& stream,
                                       unsigned flags) {
  server::LocalSblrGatewayRequest request;
  request.canonical_sbos = sblr::EncodeSblrOpcodeStream(stream);
  request.root_operation_id = "cluster.inspect_provider";
  request.root_opcode = "SBLR_CLUSTER_INSPECT_PROVIDER";
  request.root_opcode_code = 0x0B3D;
  request.route_snapshot_uuid = stream.registry_snapshot_uuid;
  request.security_snapshot_uuid = stream.package_descriptor_uuid;
  request.route_epoch = 7;
  request.route_generation = 9;
  request.security_epoch = 11;
  request.security_observation_generation = 13;
  request.route_snapshot_engine_owned = true;
  request.security_snapshot_engine_owned = true;
  request.cluster_context_active = (flags & 1U) != 0;
  request.cluster_transaction_active = (flags & 2U) != 0;
  request.route_fence_present = (flags & 4U) != 0;
  return request;
}

void Refused(const server::LocalSblrGatewayRequest& request) {
  const auto result = server::AdmitLocalNoClusterSblrGateway(request);
  Check(!result.ok && result.disposition == server::LocalSblrGatewayDisposition::kRefused,
        "invalid inspection package passed server gateway");
  Check(!result.diagnostic_id.empty() &&
            result.diagnostic_id != "PROCESS.CLUSTER_PATH_ABSENT",
        "invalid package was disguised as an absent-provider response");
}
}  // namespace

int main() {
  Check(provider::DescribeClusterProvider().provider_type == "no_cluster",
        "inspection regression requires the actual standalone provider");
  const auto package = Package();
  for (unsigned flags = 0; flags < 8; ++flags) {
    const auto request = Request(package, flags);
    Check(!request.canonical_sbos.empty(), "inspection package failed encoding");
    const auto original = request.canonical_sbos;
    const auto admitted = server::AdmitLocalNoClusterSblrGateway(request);
    Check(admitted.ok, "valid inspection was excluded before provider dispatch");
    if (!admitted.ok) continue;
    // This is the existing server shim's handoff to engine dispatch, NOT the
    // final external gateway ABI disposition or full parser/process/IPC E2E.
    Check(admitted.disposition == server::LocalSblrGatewayDisposition::kPassThrough,
          "server shim fabricated a handled inspection result");
    Check(request.canonical_sbos == original,
          "server shim changed canonical SBLR bytes");
    Check(admitted.route_snapshot_uuid == request.route_snapshot_uuid &&
              admitted.security_snapshot_uuid == request.security_snapshot_uuid &&
              admitted.route_epoch == 7 && admitted.route_generation == 9 &&
              admitted.security_epoch == 11 &&
              admitted.security_observation_generation == 13,
          "inspection admission changed engine route/security observations");
    Check(admitted.cluster_context_active == request.cluster_context_active &&
              admitted.cluster_transaction_active == request.cluster_transaction_active &&
              admitted.route_fence_present == request.route_fence_present,
          "inspection admission erased cluster ownership observations");

    const auto decoded = sblr::DecodeSblrOpcodeStream(std::string_view(
        reinterpret_cast<const char*>(original.data()), original.size()));
    Check(decoded.ok && decoded.stream.operations.size() == 3,
          "gateway-admitted package failed actual canonical decode");
    if (!decoded.ok || decoded.stream.operations.size() != 3) continue;
    for (bool available : {false, true}) {
      sblr::SblrDispatchRequest dispatch;
      dispatch.envelope = decoded.stream.operations[1];
      dispatch.context.security_context_present = true;
      dispatch.context.local_transaction_id = 7;
      dispatch.context.cluster_authority_available = available;
      dispatch.context.cluster_transaction_active = request.cluster_transaction_active;
      dispatch.context.route_fence_present = request.route_fence_present;
      const auto result = sblr::DispatchSblrOperation(std::move(dispatch));
      ++dispatches;
      Check(result.envelope_validated && result.accepted && result.dispatched_to_api,
            "admitted inspection did not reach engine provider dispatch");
      Check(!result.api_result.ok && result.api_result.cluster_authority_required,
            "admitted inspection fabricated provider success");
      Check(result.api_result.diagnostics.size() == 1 &&
                result.api_result.diagnostics.front().code == "PROCESS.CLUSTER_PATH_ABSENT" &&
                result.diagnostics.size() == 1 &&
                result.diagnostics.front().code == "PROCESS.CLUSTER_PATH_ABSENT",
            "inspection lost the normative absent-path response");
      Check(result.api_result.result_shape.rows.empty() &&
                result.api_result.result_shape.columns.empty() &&
                !result.optimizer_admitted && !result.physical_dag_executed &&
                !result.canonical_result_published && result.canonical_result_bytes.empty(),
            "inspection dependency refusal fabricated rows or local execution");
    }
  }

  for (unsigned flags : {0U, 1U, 7U}) {
    auto invalid = Request(package, flags);
    invalid.security_snapshot_engine_owned = false; Refused(invalid);
    invalid = Request(package, flags);
    invalid.route_snapshot_engine_owned = false; Refused(invalid);
    invalid = Request(package, flags);
    invalid.security_epoch = 0; Refused(invalid);
    invalid = Request(package, flags);
    invalid.route_generation = 0; Refused(invalid);
    invalid = Request(package, flags);
    invalid.root_operation_id = "engine.op.cluster_join"; Refused(invalid);
    invalid = Request(package, flags);
    invalid.canonical_sbos.front() ^= 1; Refused(invalid);
    auto with_operand = package;
    with_operand.operations[1].operands = package.operations.front().operands;
    invalid = Request(with_operand, flags);
    Check(!invalid.canonical_sbos.empty(), "unexpected operand fixture did not encode");
    const auto extra = server::AdmitLocalNoClusterSblrGateway(invalid);
    Check(!extra.ok && extra.diagnostic_id == "SBLR.OPERAND_INVALID",
          "inspection operand error was mislabeled as a cluster-context exclusion");
  }
  Check(dispatches == 16, "not all cluster-context combinations reached the provider");
  std::cout << "inspection_gateway_checks=" << checks << " failures=" << failures
            << " provider_dispatches=" << dispatches << " parser_ipc_e2e=false\n";
  return failures == 0 ? EXIT_SUCCESS : EXIT_FAILURE;
}
