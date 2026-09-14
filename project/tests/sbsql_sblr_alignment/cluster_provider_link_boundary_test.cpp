// Copyright (c) 2026 ScratchBird Software Inc.
// SPDX-License-Identifier: MPL-2.0
#include "cluster_provider/cluster_provider.hpp"
#include "uuid.hpp"
#include <iostream>
#include <set>
#include <stdexcept>

namespace cp = scratchbird::engine::cluster_provider;
namespace api = scratchbird::engine::internal_api;
namespace uuid = scratchbird::core::uuid;
namespace platform = scratchbird::core::platform;
namespace {
unsigned checks = 0;
void Check(bool condition, const char* message) {
  ++checks;
  if (!condition) throw std::runtime_error(message);
}
platform::Uuid Id(unsigned n) {
  platform::Uuid id;
  id.bytes[0] = 1; id.bytes[6] = 0x70; id.bytes[8] = 0x80; id.bytes[15] = n;
  return id;
}
}
int main() {try {
  static_assert(sizeof(api::EngineUuid) == 16);
  const auto info = cp::DescribeClusterProvider();
#if defined(SCRATCHBIRD_CLUSTER_PROVIDER_STUB)
  Check(info.provider_type == "compile_link_stub" && info.compile_link_only, "actual stub provider selected");
#elif defined(SCRATCHBIRD_CLUSTER_PROVIDER_NO_CLUSTER)
  Check(info.provider_type == "no_cluster" && !info.compile_link_only, "actual absent provider selected");
#else
#error This test qualifies the actual in-tree provider target, not an external provider or a mock.
#endif
  Check(!info.external_provider && !info.supports_execution && !cp::ClusterProviderSupportsExecution() &&
      !info.supports_route_admission && !info.local_runtime_execution_enabled && !info.mutable_by_local_core,
      "in-tree provider advertised executable or mutable authority");
  const auto handshake = cp::ValidateClusterProviderHandshake(info);
  Check(!handshake.ok && !handshake.route_admission_allowed && handshake.failed_closed,
      "in-tree provider handshake admitted cluster execution");
  std::set<std::array<std::uint8_t,16>> occurrences;
  std::set<std::string> operations{std::string(cp::kClusterProviderInfoOperationId)};
  for (const auto& boundary : cp::RequiredClusterProviderCommandBoundarySet())
    if (boundary.provider_routed) operations.emplace(boundary.provider_operation_id);
  Check(operations.contains("cluster.recover_limbo_participant"), "recovery provider operation absent from boundary");
  for (const auto& operation : operations) for (bool claimed_authority : {false, true}) {
    cp::ClusterProviderRequest request;
    request.context.database_uuid = Id(1); request.context.cluster_uuid = Id(2);
    request.context.principal_uuid = Id(3); request.context.transaction_uuid = Id(4);
    request.context.local_transaction_id = 7;
    request.context.security_context_present = claimed_authority;
    request.context.cluster_authority_available = claimed_authority;
    request.envelope.operation_id = operation;
    request.envelope.requires_cluster_authority = true;
    request.envelope.requires_security_context = true;
    request.envelope.contains_sql_text = false;
    request.api_request.context = request.context; request.api_request.operation_id = operation;
    const auto result = operation == cp::kClusterProviderInfoOperationId ?
        cp::InspectClusterProvider(request) : cp::ExecuteClusterOperation(request);
    Check(!result.ok && result.cluster_authority_required && result.operation_id == operation &&
        result.catalog_row_uuid.is_nil() && result.transaction_uuid.is_nil() && !result.local_transaction_id,
        "provider refusal invented success, transaction or catalog identity");
    bool absent = false;
    for (const auto& diagnostic : result.diagnostics) {
      Check(diagnostic.error, "provider returned a successful diagnostic");
      platform::Uuid occurrence; occurrence.bytes = diagnostic.occurrence_uuid;
      Check(uuid::IsEngineIdentityUuid(occurrence) && occurrences.insert(occurrence.bytes).second,
          "source diagnostic occurrence missing or reused");
      const auto copy = diagnostic;
      Check(copy.occurrence_uuid == diagnostic.occurrence_uuid, "diagnostic copy reissued source identity");
      if (diagnostic.code == cp::kClusterPathAbsentCode) {
        absent = true;
        Check(diagnostic.canonical_metadata && diagnostic.canonical_metadata->code == diagnostic.code &&
            diagnostic.canonical_metadata->is_failure, "provider lost canonical absence metadata");
      }
    }
    Check(absent && !result.unsupported_features.empty(), "actual provider refusal omitted absence diagnostic");
    Check(request.context.database_uuid == Id(1) && request.context.transaction_uuid == Id(4) &&
        request.context.local_transaction_id == 7, "provider changed binary request binding");
  }
  std::cout << "provider=" << info.provider_type << " operations=" << operations.size()
            << " checks=" << checks << '\n';
  return 0;
} catch (const std::exception& error) {
  std::cerr << "provider boundary failure: " << error.what() << " checks=" << checks << '\n';
  return 1;
}}
