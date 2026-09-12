// Copyright (c) 2026 ScratchBird Software Inc.
// SPDX-License-Identifier: MPL-2.0

#include "canonical_sblr_admission_test_helper.hpp"
#include "cluster_provider/cluster_provider.hpp"
#include "engine/sblr/sblr_dispatch.hpp"

#include <algorithm>
#include <cstdlib>
#include <iostream>

namespace sblr = scratchbird::engine::sblr;
namespace provider = scratchbird::engine::cluster_provider;

namespace {
std::size_t checks = 0;
std::size_t failures = 0;

void Check(bool condition, const char* message) {
  ++checks;
  if (!condition) {
    ++failures;
    std::cerr << message << '\n';
  }
}

bool HasProviderRefusal(const sblr::SblrDispatchResult& result) {
  return std::any_of(result.api_result.diagnostics.begin(),
                     result.api_result.diagnostics.end(), [](const auto& d) {
    return d.code == provider::kClusterSupportNotEnabledCode;
  });
}

sblr::SblrDispatchRequest QueryBoundary(bool available, unsigned flags,
                                       bool explicit_requirement = false) {
  sblr::SblrDispatchRequest request;
  request.envelope = scratchbird::test::sbsql::
      BuildCanonicalEngineSblrEnvelopeForTest(
          "query.execute", "SBLR_QUERY_EXECUTE", "cluster.route.availability");
  request.envelope.result_shape = "query_execute_result";
  request.envelope.requires_security_context = true;
  request.envelope.requires_transaction_context = true;
  request.envelope.requires_cluster_authority = explicit_requirement;
  request.context.security_context_present = true;
  request.context.local_transaction_id = 7;
  request.context.cluster_authority_available = available;
  request.context.cluster_transaction_active = (flags & 1U) != 0;
  request.context.route_fence_present = (flags & 2U) != 0;
  // Deliberately no executable relational DAG: this boundary test isolates
  // provider selection from query implementation. Local controls must reach
  // real graph validation/refusal, not fabricate a successful query result.
  return request;
}

void CheckNoQueryExecution(const sblr::SblrDispatchResult& result) {
  Check(!result.api_result.ok, "route refusal reported API success");
  Check(!result.physical_dag_executed, "cluster-required query executed locally");
  Check(!result.canonical_result_published, "route refusal published query rows");
  Check(result.canonical_result_row_count == 0 &&
            result.canonical_result_bytes.empty() &&
            result.api_result.result_shape.rows.empty(),
        "route refusal fabricated result data");
}
}  // namespace

int main() {
  Check(provider::DescribeClusterProvider().provider_type == "no_cluster" &&
            !provider::ClusterProviderSupportsExecution(),
        "this regression requires the actual no_cluster provider");
  for (bool available : {false, true}) {
    for (bool explicit_requirement : {false, true}) {
      for (unsigned flags = 0; flags < 4; ++flags) {
        const auto result = sblr::DispatchSblrOperation(
            QueryBoundary(available, flags, explicit_requirement));
        Check(result.envelope_validated,
              "routing fixture did not pass canonical envelope validation");
        CheckNoQueryExecution(result);
        if (flags != 0 || explicit_requirement) {
          if (!HasProviderRefusal(result)) {
            std::cerr << "available=" << available << " flags=" << flags
                      << " explicit=" << explicit_requirement << '\n';
          }
          Check(HasProviderRefusal(result),
                "unavailable authority erased a required provider route");
          Check(result.accepted && result.dispatched_to_api &&
                    result.api_result.cluster_authority_required,
                "cluster requirement did not reach the actual provider");
          Check(!result.optimizer_admitted && !result.logical_graph_populated,
                "cluster requirement entered local query planning");
        } else {
          Check(!HasProviderRefusal(result),
                "provider availability alone rerouted a local query");
          Check(!result.accepted && !result.dispatched_to_api,
                "empty local query bypassed real graph validation");
        }
      }
    }
  }

  // Structural package records are local-observed, not conditional queries.
  // Exercise the same context matrix without claiming a complete package run.
  for (bool available : {false, true}) {
    for (unsigned flags = 0; flags < 4; ++flags) {
      auto structural = QueryBoundary(available, flags);
      structural.envelope = scratchbird::test::sbsql::
          BuildCanonicalEngineSblrEnvelopeForTest(
              "engine.op.package_begin", "SBLR_PACKAGE_BEGIN",
              "cluster.route.structural");
      structural.envelope.requires_security_context = true;
      const auto observed = sblr::DispatchSblrOperation(std::move(structural));
      Check(observed.envelope_validated,
            "structural routing fixture failed canonical validation");
      Check(!HasProviderRefusal(observed),
            "structural envelope record was routed to the provider");
      Check(!observed.physical_dag_executed &&
                !observed.canonical_result_published,
            "structural boundary fabricated query execution");
    }
  }

  auto malformed = QueryBoundary(false, 3);
  malformed.envelope.opcode_code = 0xFFFF;
  const auto invalid = sblr::DispatchSblrOperation(std::move(malformed));
  Check(!invalid.envelope_validated && !HasProviderRefusal(invalid),
        "invalid opcode was disguised as a provider dependency failure");
  CheckNoQueryExecution(invalid);

  auto unauthorized = QueryBoundary(false, 3);
  unauthorized.context.security_context_present = false;
  const auto denied = sblr::DispatchSblrOperation(std::move(unauthorized));
  Check(!denied.accepted && !HasProviderRefusal(denied),
        "cluster routing bypassed security-context validation");
  CheckNoQueryExecution(denied);

  auto no_transaction = QueryBoundary(false, 3);
  no_transaction.context.local_transaction_id = 0;
  const auto unbound = sblr::DispatchSblrOperation(std::move(no_transaction));
  Check(!unbound.accepted && !HasProviderRefusal(unbound),
        "cluster routing bypassed transaction-context validation");
  CheckNoQueryExecution(unbound);

  std::cout << "cluster_route_boundary_checks=" << checks
            << " failures=" << failures << " query_e2e=false\n";
  return failures == 0 ? EXIT_SUCCESS : EXIT_FAILURE;
}
