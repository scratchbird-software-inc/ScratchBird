// Copyright (c) 2026 ScratchBird Software Inc.
// SPDX-License-Identifier: MPL-2.0

#include "canonical_sblr_admission_test_helper.hpp"
#include "cluster_provider/cluster_provider.hpp"
#include "engine/sblr/sblr_dispatch.hpp"
#include "engine/sblr/sblr_opcode_registry.hpp"

#include <algorithm>
#include <cstdlib>
#include <iostream>
#include <string_view>

namespace api = scratchbird::engine::internal_api;
namespace provider = scratchbird::engine::cluster_provider;
namespace sblr = scratchbird::engine::sblr;

namespace {
std::size_t checks = 0;
std::size_t failures = 0;
std::string_view expected_provider = "no_cluster";
// Independent oracle: Core gateway ABI/API contract section5; never derive
// the expected diagnostic from the provider implementation's constants.
constexpr std::string_view kAbsent = "PROCESS.CLUSTER_PATH_ABSENT";

void Check(bool condition, const char* detail) {
  ++checks;
  if (!condition && ++failures <= 12) std::cerr << detail << '\n';
}

bool HasAbsentDiagnostic(const api::EngineApiResult& result) {
  return std::any_of(result.diagnostics.begin(), result.diagnostics.end(),
                     [](const auto& d) { return d.code == kAbsent; });
}

void CheckRefusal(const api::EngineApiResult& result,
                  std::string_view operation) {
  Check(!result.ok && result.cluster_authority_required,
        "standalone cluster request returned success or lost ownership");
  Check(result.operation_id == operation, "refusal changed operation identity");
  Check(result.diagnostics.size() == 1 && HasAbsentDiagnostic(result),
        "standalone request lacks the exact absent-path diagnostic");
  Check(result.result_shape.rows.empty() && result.result_shape.columns.empty() &&
            result.result_shape.result_kind.empty(),
        "standalone refusal fabricated a typed result or UUID-bearing column");
  Check(result.catalog_row_uuid.canonical.empty() &&
            result.transaction_uuid.canonical.empty() &&
            result.local_transaction_id == 0,
        "standalone refusal fabricated catalog or transaction authority");
  Check(std::any_of(result.evidence.begin(), result.evidence.end(),
                    [](const auto& e) {
                      return e.evidence_kind == "cluster_provider_type" &&
                             e.evidence_id == expected_provider;
                    }), "refusal lost host provider evidence");
}
}  // namespace

int main(int argc, char** argv) {
  if (argc == 2) expected_provider = argv[1];
  Check(argc <= 2 && (expected_provider == "no_cluster" ||
                     expected_provider == "compile_link_stub"),
        "regression requires an explicit supported in-tree provider mode");
  const auto descriptor = provider::DescribeClusterProvider();
  Check(descriptor.provider_type == expected_provider &&
            !descriptor.supports_execution && !descriptor.supports_route_admission,
        "regression must use the actual standalone provider");

  // Exhaust the current provider boundary operation set, including inspection.
  // These are direct provider boundary calls, not admitted command executions.
  for (const auto operation : provider::RequiredClusterProviderOperationSet()) {
    provider::ClusterProviderRequest request;
    request.envelope.operation_id = operation;
    CheckRefusal(provider::ExecuteClusterOperation(request), operation);
  }

  for (bool available : {false, true}) {
    for (unsigned flags = 0; flags != 4; ++flags) {
      provider::ClusterProviderRequest request;
      request.envelope = scratchbird::test::sbsql::
          BuildCanonicalEngineSblrEnvelopeForTest(
              provider::kClusterProviderInfoOperationId,
              provider::kClusterProviderInfoOpcode, "cluster.absent.inspect");
      request.envelope.requires_security_context = true;
      request.envelope.requires_transaction_context = true;
      request.envelope.requires_cluster_authority = true;
      request.context.security_context_present = true;
      request.context.local_transaction_id = 7;
      request.context.cluster_authority_available = available;
      request.context.cluster_transaction_active = (flags & 1U) != 0;
      request.context.route_fence_present = (flags & 2U) != 0;
      CheckRefusal(provider::InspectClusterProvider(request),
                   provider::kClusterProviderInfoOperationId);

      const auto bytes = sblr::EncodeSblrEnvelope(request.envelope);
      Check(!bytes.empty(), "inspection fixture failed canonical encoding");
      const auto routed = sblr::DecodeAndDispatchSblrOperation(bytes, request.context);
      Check(routed.envelope_validated && routed.accepted && routed.dispatched_to_api,
            "inspection did not reach the real provider through canonical decoding");
      CheckRefusal(routed.api_result, provider::kClusterProviderInfoOperationId);
      Check(routed.diagnostics.size() == 1 &&
                routed.diagnostics.front().code == kAbsent,
            "canonical dispatch dropped the absent-path response diagnostic");
      Check(!routed.optimizer_admitted && !routed.physical_dag_executed &&
                !routed.canonical_result_published &&
                routed.canonical_result_bytes.empty(),
            "inspection refusal produced local query execution or rows");

      auto unauthorized = request.context;
      unauthorized.security_context_present = false;
      const auto denied = sblr::DecodeAndDispatchSblrOperation(bytes, unauthorized);
      Check(!denied.accepted && !HasAbsentDiagnostic(denied.api_result),
            "absence refusal masked missing security authority");
      auto unbound = request.context;
      unbound.local_transaction_id = 0;
      const auto no_transaction = sblr::DecodeAndDispatchSblrOperation(bytes, unbound);
      Check(!no_transaction.accepted && !HasAbsentDiagnostic(no_transaction.api_result),
            "absence refusal masked missing transaction authority");
      auto corrupt = bytes;
      corrupt[0] ^= 1;
      const auto malformed = sblr::DecodeAndDispatchSblrOperation(corrupt, request.context);
      Check(!malformed.envelope_validated && !HasAbsentDiagnostic(malformed.api_result),
            "absence refusal masked malformed canonical transport");
    }
  }
  // These canonical roots deliberately have no cluster context and no caller
  // cluster flag. Ownership belongs to the registered opcode, not a name prefix
  // or a non-transported envelope boolean. Empty operand fixtures prove routing
  // and dependency refusal only, not complete cluster command admission or E2E.
  struct ClusterRoot {
    const char* operation;
    const char* opcode;
  };
  const ClusterRoot roots[] = {
      {"engine.op.cluster_join", "SBLR_CLUSTER_JOIN"},
      {"engine.op.cluster_leave", "SBLR_CLUSTER_LEAVE"},
      {"engine.op.repl_2pc_heartbeat", "SBLR_REPL_2PC_HEARTBEAT"},
      {"engine.op.cluster_create_placement_policy",
       "SBLR_CLUSTER_CREATE_PLACEMENT_POLICY"},
  };
  for (const auto& root : roots) {
    auto envelope = scratchbird::test::sbsql::
        BuildCanonicalEngineSblrEnvelopeForTest(
            root.operation, root.opcode, "cluster.registry.route");
    envelope.requires_cluster_authority = false;
    const auto* entry = sblr::LookupSblrOpcodeCode(envelope.opcode_code);
    Check(entry != nullptr && entry->requires_cluster_authority,
          "cluster root fixture lacks registry-owned cluster precondition");
    const auto bytes = sblr::EncodeSblrEnvelope(envelope);
    const auto decoded = sblr::DecodeSblrEnvelope(bytes);
    Check(decoded.ok && !decoded.envelope.requires_cluster_authority,
          "canonical fixture accidentally retained caller cluster authority");
    for (bool available : {false, true}) {
      api::EngineRequestContext context;
      context.security_context_present = true;
      context.local_transaction_id = 7;
      context.cluster_authority_available = available;
      const auto routed = sblr::DecodeAndDispatchSblrOperation(bytes, context);
      Check(routed.envelope_validated && routed.accepted && routed.dispatched_to_api,
            "registry-owned cluster root missed canonical provider dispatch");
      CheckRefusal(routed.api_result, root.operation);
      Check(routed.diagnostics.size() == 1 &&
                routed.diagnostics.front().code == kAbsent,
            "registry-owned root lost the provider dependency diagnostic");
      Check(!routed.optimizer_admitted && !routed.physical_dag_executed &&
                !routed.canonical_result_published &&
                routed.canonical_result_bytes.empty(),
            "registry-owned root fell through to local execution or rows");

      auto unauthorized = context;
      unauthorized.security_context_present = false;
      const auto denied = sblr::DecodeAndDispatchSblrOperation(bytes, unauthorized);
      Check(!denied.accepted && !HasAbsentDiagnostic(denied.api_result),
            "registry routing masked missing security authority");
      if (entry != nullptr && entry->requires_transaction_context) {
        auto unbound = context;
        unbound.local_transaction_id = 0;
        const auto no_transaction = sblr::DecodeAndDispatchSblrOperation(bytes, unbound);
        Check(!no_transaction.accepted && !HasAbsentDiagnostic(no_transaction.api_result),
              "registry routing masked missing transaction authority");
      }
      auto corrupt = bytes;
      if (!corrupt.empty()) corrupt[0] ^= 1;
      const auto malformed = sblr::DecodeAndDispatchSblrOperation(corrupt, context);
      Check(!malformed.envelope_validated && !HasAbsentDiagnostic(malformed.api_result),
            "registry routing masked malformed canonical transport");
    }
  }
  // SOURCE_MAP is structural/local-observed even in a cluster context. The
  // internal executor must validate its single binary descriptor reference
  // before indexing the operands or publishing evidence. A well-shaped test
  // reference is not proof of receipt issuance or public/process E2E.
  auto source_map = scratchbird::test::sbsql::
      BuildCanonicalEngineSblrEnvelopeForTest(
          "engine.op.source_map", "SBLR_SOURCE_MAP", "source.map.admission");
  sblr::SblrOperand source_reference;
  source_reference.ordinal = 1;
  source_reference.type = "source_map.vector";
  source_reference.name = "source_map";
  source_reference.value_kind = sblr::SblrValueKind::descriptor_ref;
  source_reference.value_body.resize(24);
  source_reference.value_body[0] = 0x01;
  source_reference.value_body[6] = 0x70;
  source_reference.value_body[8] = 0x80;
  source_reference.value_body[15] = 0x3b;
  source_reference.value_body[16] = 1;
  source_map.operands.push_back(source_reference);
  for (bool available : {false, true}) {
    for (unsigned flags = 0; flags != 4; ++flags) {
      api::EngineRequestContext context;
      context.security_context_present = true;
      context.cluster_authority_available = available;
      context.cluster_transaction_active = (flags & 1U) != 0;
      context.route_fence_present = (flags & 2U) != 0;
      for (unsigned mutation = 0; mutation != 10; ++mutation) {
        auto malformed = source_map;
        auto& body = malformed.operands.front().value_body;
        switch (mutation) {
          case 0: malformed.operands.clear(); break;
          case 1:
            malformed.operands.push_back(source_reference);
            malformed.operands.back().ordinal = 2;
            break;
          case 2: body.resize(16); break;
          case 3: body.push_back(0); break;
          case 4: std::fill(body.begin(), body.begin() + 16, 0); break;
          case 5: std::fill(body.begin() + 16, body.end(), 0); break;
          case 6: body[6] = 0x40; break;  // User-data UUIDv4 is not engine authority.
          case 7: body[8] = 0; break;
          case 8: malformed.operands.front().value_kind = sblr::SblrValueKind::uuid_ref; break;
          case 9: body.clear(); break;
        }
        sblr::SblrDispatchRequest request;
        request.envelope = malformed;
        request.context = context;
        const auto preflight = sblr::PreflightSblrQueryOperation(request);
        Check(!preflight.ok, "malformed SOURCE_MAP passed preflight");
        const auto direct = sblr::DispatchSblrOperation(request);
        const auto canonical = sblr::DecodeAndDispatchSblrOperation(
            sblr::EncodeSblrEnvelope(malformed), context);
        for (const auto* rejected : {&direct, &canonical}) {
          Check(!rejected->accepted && !rejected->dispatched_to_api &&
                    !rejected->api_result.ok,
                "malformed SOURCE_MAP entered execution");
          Check(!HasAbsentDiagnostic(rejected->api_result) &&
                    rejected->api_result.evidence.empty(),
                "malformed SOURCE_MAP reached provider or published evidence");
          Check(!rejected->canonical_result_published &&
                    rejected->api_result.result_shape.rows.empty(),
                "malformed SOURCE_MAP fabricated results");
          if (mutation == 0) {
            Check(rejected->api_result.diagnostics.size() == 1 &&
                      rejected->api_result.diagnostics.front().code ==
                          "SBLR.OPERAND_INVALID",
                  "empty SOURCE_MAP lost exact operand diagnostic");
          }
        }
      }
      sblr::SblrDispatchRequest valid;
      valid.envelope = source_map;
      valid.context = context;
      Check(sblr::PreflightSblrQueryOperation(valid).ok,
            "well-shaped SOURCE_MAP lost internal preflight compatibility");
      const auto executed = sblr::DecodeAndDispatchSblrOperation(
          sblr::EncodeSblrEnvelope(source_map), context);
      Check(executed.accepted && executed.dispatched_to_api &&
                executed.api_result.ok && !HasAbsentDiagnostic(executed.api_result),
            "well-shaped SOURCE_MAP stopped being local-observed");
    }
  }
  std::cout << "standalone_cluster_refusal_checks=" << checks
            << " failures=" << failures << " provider=" << expected_provider
            << " parser_ipc_e2e=false\n";
  return failures == 0 ? EXIT_SUCCESS : EXIT_FAILURE;
}
