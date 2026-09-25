// Copyright (c) 2026 ScratchBird Software Inc.
// SPDX-License-Identifier: MPL-2.0
#pragma once
#include "engine/executor/descriptor_value_runtime.hpp"
namespace scratchbird::engine::sblr {
std::shared_ptr<const executor::CanonicalWindowOrderBindingReceipt>
BindCanonicalWindowOrder(
    const executor::TypedPhysicalNodeDag& dag,
    const executor::PhysicalNodeRecord& node,
    const executor::DescriptorBatch& input,
    const executor::CanonicalDescriptorOrderTerm& term,
    const executor::CanonicalExecutionMgaAuthority& authority,
    executor::DescriptorRuntimeDiagnostic* diagnostic);
// The four descriptor window requests retain the same immutable binding pair.
template<class Request>
bool BindCanonicalWindowOrderRequest(
    Request& request, const executor::TypedPhysicalNodeDag& dag,
    const executor::PhysicalNodeRecord& node,
    const executor::DescriptorBatch& input,
    executor::DescriptorRuntimeDiagnostic* diagnostic) {
  request.order_term_binding_receipt = BindCanonicalWindowOrder(
      dag, node, input, request.order_term, request.mga_authority, diagnostic);
  if (!request.order_term_binding_receipt) return false;
  request.order_term_binding_evidence_uuid = request.order_term_binding_receipt->identity();
  return true;
}
} // namespace scratchbird::engine::sblr
