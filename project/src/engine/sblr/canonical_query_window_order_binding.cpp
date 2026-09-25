// Copyright (c) 2026 ScratchBird Software Inc.
// SPDX-License-Identifier: MPL-2.0
#include "canonical_query_window_order_binding.hpp"
namespace scratchbird::engine::sblr {
// SEARCH_KEY: SB_ENGINE_CANONICAL_QUERY_WINDOW_ORDER_BINDING_AUTHORITY
// Revalidate the published order through the existing executor receipt issuer.
// This adapter owns neither optimizer publication nor transaction finality.
std::shared_ptr<const executor::CanonicalWindowOrderBindingReceipt>
BindCanonicalWindowOrder(
    const executor::TypedPhysicalNodeDag& dag,
    const executor::PhysicalNodeRecord& node,
    const executor::DescriptorBatch& input,
    const executor::CanonicalDescriptorOrderTerm& term,
    const executor::CanonicalExecutionMgaAuthority& authority,
    executor::DescriptorRuntimeDiagnostic* diagnostic) {
  const auto refuse = [&](const char* detail) {
    diagnostic->ok = false;
    diagnostic->diagnostic_code = "QOW-DIAG-WINDOW-PROPERTY-BINDING";
    diagnostic->detail = detail;
  };
  if (term.column >= input.columns.size() ||
      node.required_property_uuids.size() != 1) {
    refuse("window order-term binding is unresolved");
    return {};
  }
  auto receipt = executor::CanonicalWindowOrderBindingReceipt::Issue(
      dag, node.physical_node_id, input.columns[term.column], term,
      node.required_property_uuids.front(), authority, node.memory_bytes_required);
  if (!receipt) refuse("window order-term receipt admission failed");
  return receipt;
}
} // namespace scratchbird::engine::sblr
