// Copyright (c) 2026 ScratchBird Software Inc.
//
// This Source Code Form is subject to the terms of the Mozilla Public
// License, v. 2.0. If a copy of the MPL was not distributed with this
// file, You can obtain one at https://mozilla.org/MPL/2.0/.
//
// SPDX-License-Identifier: MPL-2.0

#include "agents/archive_manager.hpp"

// CanonicalAgentRegistry/CanonicalAgentManifest owns production exposure;
// this file provides the local archive workflow handler.

#include <utility>

namespace scratchbird::core::agents::implemented_agents {
namespace {
using scratchbird::core::platform::Severity;
using scratchbird::core::platform::StatusCode;
using scratchbird::core::platform::Subsystem;
Status OkStatus(){return {StatusCode::ok,Severity::info,Subsystem::engine};}
Status ErrorStatus(){return {StatusCode::memory_invalid_request,Severity::error,Subsystem::engine};}
ArchiveManagerResult Finish(ArchiveManagerDecisionKind d, Status s, std::string c, std::string k, std::string detail, bool f){ArchiveManagerResult r; r.status=s; r.decision=d; r.fail_closed=f; r.diagnostic=MakeArchiveManagerDiagnostic(r.status,std::move(c),std::move(k),std::move(detail)); r.evidence.push_back({"decision",ArchiveManagerDecisionKindName(r.decision)}); r.evidence.push_back({"recovery_authority","false"}); return r;}
AgentLocalWorkflowRequest WorkflowRequest(const ArchiveManagerRequest& r,
                                          ArchiveManagerDecisionKind decision) {
  AgentLocalWorkflowRequest request;
  request.domain = AgentLocalWorkflowDomain::archive;
  request.operation_id = ArchiveManagerDecisionKindName(decision);
  request.idempotency_key = r.idempotency_key.empty() ? r.slice_uuid + ":" + request.operation_id : r.idempotency_key;
  request.authority.database_uuid = r.database_uuid;
  request.authority.principal_uuid = r.principal_uuid;
  request.authority.subject_uuid = r.slice_uuid;
  request.authority.mga_transaction_uuid = r.mga_transaction_uuid;
  request.authority.evidence_uuid = r.evidence_uuid;
  request.authority.local_transaction_id = r.local_transaction_id;
  request.authority.catalog_generation = r.catalog_generation;
  request.authority.durable_catalog_bound = r.durable_catalog_bound;
  request.authority.transaction_inventory_bound = r.transaction_inventory_bound;
  request.authority.metadata_authoritative = r.metadata_authoritative;
  request.authority.recovery_authority = r.recovery_authority;
  request.authority.cluster_route_requested = r.cluster_route_requested;
  request.subsystem_precondition_satisfied = true;
  request.intended_state_observed = r.intended_state_observed;
  return request;
}
ArchiveManagerResult ApplyWorkflow(AgentLocalWorkflowLedger* ledger,
                                   ArchiveManagerResult result,
                                   const ArchiveManagerRequest& request) {
  if (ledger == nullptr || result.fail_closed ||
      result.decision == ArchiveManagerDecisionKind::no_action ||
      result.decision == ArchiveManagerDecisionKind::refused) {
    return result;
  }
  const auto workflow = ledger->Apply(WorkflowRequest(request, result.decision));
  result.workflow_record = workflow.record;
  result.workflow_record_written = workflow.ok && !workflow.idempotent;
  result.outcome_verified = workflow.record.outcome_verified;
  result.evidence.push_back({"workflow_uuid", workflow.record.workflow_uuid});
  result.evidence.push_back({"workflow_state", AgentLocalWorkflowStateName(workflow.record.state)});
  if (!workflow.ok) {
    return Finish(ArchiveManagerDecisionKind::refused, ErrorStatus(),
                  workflow.status.diagnostic_code, "agents.archive.workflow_refused",
                  workflow.status.detail, true);
  }
  return result;
}
}
const char* ArchiveManagerDecisionKindName(ArchiveManagerDecisionKind d){switch(d){case ArchiveManagerDecisionKind::no_action:return "no_action";case ArchiveManagerDecisionKind::seal_archive_slice:return "seal_archive_slice";case ArchiveManagerDecisionKind::request_verify_slice:return "request_verify_slice";case ArchiveManagerDecisionKind::refused:return "refused";}return "refused";}
DiagnosticRecord MakeArchiveManagerDiagnostic(Status s,std::string c,std::string k,std::string d){return scratchbird::core::platform::MakeDiagnostic(s.code,s.severity,s.subsystem,std::move(c),std::move(k),{{"detail",std::move(d)}},{},"archive_manager",{});}
ArchiveManagerResult EvaluateArchiveManagerRequest(const ArchiveManagerRequest& r){return EvaluateArchiveManagerRequest(nullptr, r);}
ArchiveManagerResult EvaluateArchiveManagerRequest(AgentLocalWorkflowLedger* ledger,const ArchiveManagerRequest& r){ArchiveManagerResult result;if(r.slice_uuid.empty())result=Finish(ArchiveManagerDecisionKind::refused,ErrorStatus(),"SB_AGENT_ARCHIVE_ID_REQUIRED","agents.archive.id_required","slice identity required",true);else if(r.cluster_route_requested||r.recovery_authority||!r.metadata_authoritative)result=Finish(ArchiveManagerDecisionKind::refused,ErrorStatus(),"SB_AGENT_ARCHIVE_AUTHORITY_UNTRUSTED","agents.archive.untrusted_authority","archive requires local metadata evidence and external cluster routing",true);else if(r.legal_hold_active)result=Finish(ArchiveManagerDecisionKind::no_action,OkStatus(),"SB_AGENT_ARCHIVE_LEGAL_HOLD","agents.archive.legal_hold","legal hold blocks archive mutation",false);else if(r.seal_requested&&r.slice_complete)result=Finish(ArchiveManagerDecisionKind::seal_archive_slice,OkStatus(),"SB_AGENT_ARCHIVE_SEAL_READY","agents.archive.seal_ready","slice complete and sealable",false);else if(r.verify_requested&&r.slice_available)result=Finish(ArchiveManagerDecisionKind::request_verify_slice,OkStatus(),"SB_AGENT_ARCHIVE_VERIFY_READY","agents.archive.verify_ready","slice available for verification",false);else result=Finish(ArchiveManagerDecisionKind::no_action,OkStatus(),"SB_AGENT_ARCHIVE_NO_ACTION","agents.archive.no_action","archive request not actionable",false);return ApplyWorkflow(ledger,std::move(result),r);}
const char* archive_manager_implementation_anchor(){return "archive_manager";}
}  // namespace scratchbird::core::agents::implemented_agents
