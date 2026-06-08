// Copyright (c) 2026 ScratchBird Software Inc.
//
// This Source Code Form is subject to the terms of the Mozilla Public
// License, v. 2.0. If a copy of the MPL was not distributed with this
// file, You can obtain one at https://mozilla.org/MPL/2.0/.
//
// SPDX-License-Identifier: MPL-2.0

#include "agents/agent_local_workflow_store_api.hpp"

// SEARCH_KEY: AEIC_LOCAL_OPERATIONAL_WORKFLOW_STORE

#include <utility>

namespace scratchbird::engine::internal_api {
namespace {

namespace agents = scratchbird::core::agents;

std::string DiagnosticDetail(const EngineApiDiagnostic& diagnostic) {
  if (!diagnostic.detail.empty()) { return diagnostic.detail; }
  if (!diagnostic.message_key.empty()) { return diagnostic.message_key; }
  return diagnostic.code;
}

agents::AgentLocalWorkflowApplyResult StoreFailure(std::string code,
                                                   std::string detail,
                                                   const agents::AgentLocalWorkflowRequest& request) {
  agents::AgentLocalWorkflowApplyResult result;
  result.status = agents::AgentError(std::move(code), std::move(detail));
  result.ok = false;
  result.failed_closed = true;
  result.record.domain = request.domain;
  result.record.state = agents::AgentLocalWorkflowState::refused;
  result.record.operation_id = request.operation_id;
  result.record.subject_uuid = request.authority.subject_uuid;
  result.record.idempotency_key = request.idempotency_key;
  result.record.diagnostic_code = result.status.diagnostic_code;
  result.record.workflow_uuid =
      agents::DeterministicAgentRuntimeObjectUuidFromKey(
          "agent_local_workflow_store_refused|" + request.operation_id +
          "|" + request.idempotency_key + "|" + result.status.diagnostic_code);
  return result;
}

}  // namespace

AgentLocalWorkflowStoreResult ApplyAgentLocalWorkflowWithDurableCatalogStore(
    const AgentLocalWorkflowStoreRequest& request) {
  AgentLocalWorkflowStoreResult result;
  result.loaded_catalog =
      LoadAgentDurableCatalogImage(request.context, request.production_live_path);
  if (!result.loaded_catalog.ok) {
    result.workflow =
        StoreFailure("SB_AGENT_LOCAL_WORKFLOW_STORE.LOAD_FAILED",
                     DiagnosticDetail(result.loaded_catalog.diagnostic),
                     request.workflow);
    return result;
  }
  result.loaded_from_store = true;

  agents::DurableAgentCatalogImage catalog = result.loaded_catalog.image;
  agents::AgentLocalWorkflowLedger ledger(&catalog);
  result.workflow = ledger.Apply(request.workflow);
  if (!result.workflow.ok || result.workflow.idempotent) {
    return result;
  }

  AgentDurableCatalogStoreRequest store_request;
  store_request.context = request.context;
  store_request.image = std::move(catalog);
  store_request.evidence_uuid = result.workflow.record.workflow_uuid;
  store_request.production_live_path = request.production_live_path;
  store_request.fsync_or_checkpoint_evidence =
      request.fsync_or_checkpoint_evidence;
  result.persisted_catalog = PersistAgentDurableCatalogImage(store_request);
  if (!result.persisted_catalog.ok) {
    result.workflow =
        StoreFailure("SB_AGENT_LOCAL_WORKFLOW_STORE.PERSIST_FAILED",
                     DiagnosticDetail(result.persisted_catalog.diagnostic),
                     request.workflow);
    return result;
  }
  result.persisted_to_store = true;
  return result;
}

}  // namespace scratchbird::engine::internal_api
