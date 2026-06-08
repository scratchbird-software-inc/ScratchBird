// Copyright (c) 2026 ScratchBird Software Inc.
//
// This Source Code Form is subject to the terms of the Mozilla Public
// License, v. 2.0. If a copy of the MPL was not distributed with this
// file, You can obtain one at https://mozilla.org/MPL/2.0/.
//
// SPDX-License-Identifier: MPL-2.0

#include "agents/pitr_manager.hpp"

// CanonicalAgentRegistry/CanonicalAgentManifest owns production exposure;
// this file provides the local PITR workflow handler.

#include <utility>

namespace scratchbird::core::agents::implemented_agents {
namespace {
using scratchbird::core::platform::Severity; using scratchbird::core::platform::StatusCode; using scratchbird::core::platform::Subsystem;
Status OkStatus(){return {StatusCode::ok,Severity::info,Subsystem::engine};}
Status ErrorStatus(){return {StatusCode::memory_invalid_request,Severity::error,Subsystem::engine};}
PitrManagerResult Finish(PitrManagerDecisionKind d, Status s, std::string c, std::string k, std::string detail, bool f){PitrManagerResult r; r.status=s; r.decision=d; r.fail_closed=f; r.diagnostic=MakePitrManagerDiagnostic(r.status,std::move(c),std::move(k),std::move(detail)); r.evidence.push_back({"decision",PitrManagerDecisionKindName(r.decision)}); r.evidence.push_back({"recovery_authority","false"}); return r;}
AgentLocalWorkflowRequest WorkflowRequest(const PitrManagerRequest& r,PitrManagerDecisionKind decision){AgentLocalWorkflowRequest request;request.domain=AgentLocalWorkflowDomain::pitr;request.operation_id=PitrManagerDecisionKindName(decision);request.idempotency_key=r.idempotency_key.empty()?r.target_uuid+":"+request.operation_id:r.idempotency_key;request.authority.database_uuid=r.database_uuid;request.authority.principal_uuid=r.principal_uuid;request.authority.subject_uuid=r.target_uuid;request.authority.mga_transaction_uuid=r.mga_transaction_uuid;request.authority.evidence_uuid=r.evidence_uuid;request.authority.local_transaction_id=r.local_transaction_id;request.authority.catalog_generation=r.catalog_generation;request.authority.durable_catalog_bound=r.durable_catalog_bound;request.authority.transaction_inventory_bound=r.transaction_inventory_bound;request.authority.storage_snapshot_authoritative=r.storage_snapshot_authoritative||r.archive_window_authoritative;request.authority.metadata_authoritative=r.metadata_authoritative||r.archive_window_authoritative;request.authority.recovery_authority=r.recovery_authority;request.authority.cluster_route_requested=r.cluster_route_requested;request.subsystem_precondition_satisfied=true;request.intended_state_observed=r.intended_state_observed;return request;}
PitrManagerResult ApplyWorkflow(AgentLocalWorkflowLedger* ledger,PitrManagerResult result,const PitrManagerRequest& request){if(ledger==nullptr||result.fail_closed||result.decision==PitrManagerDecisionKind::estimate_pitr||result.decision==PitrManagerDecisionKind::refused)return result;const auto workflow=ledger->Apply(WorkflowRequest(request,result.decision));result.workflow_record=workflow.record;result.workflow_record_written=workflow.ok&&!workflow.idempotent;result.outcome_verified=workflow.record.outcome_verified;result.evidence.push_back({"workflow_uuid",workflow.record.workflow_uuid});result.evidence.push_back({"workflow_state",AgentLocalWorkflowStateName(workflow.record.state)});if(!workflow.ok)return Finish(PitrManagerDecisionKind::refused,ErrorStatus(),workflow.status.diagnostic_code,"agents.pitr.workflow_refused",workflow.status.detail,true);return result;}
}
const char* PitrManagerDecisionKindName(PitrManagerDecisionKind d){switch(d){case PitrManagerDecisionKind::estimate_pitr:return "estimate_pitr";case PitrManagerDecisionKind::request_restore_plan:return "request_restore_plan";case PitrManagerDecisionKind::refused:return "refused";}return "refused";}
DiagnosticRecord MakePitrManagerDiagnostic(Status s,std::string c,std::string k,std::string d){return scratchbird::core::platform::MakeDiagnostic(s.code,s.severity,s.subsystem,std::move(c),std::move(k),{{"detail",std::move(d)}},{},"pitr_manager",{});}
PitrManagerResult EvaluatePitrManagerRequest(const PitrManagerRequest& r){return EvaluatePitrManagerRequest(nullptr,r);}
PitrManagerResult EvaluatePitrManagerRequest(AgentLocalWorkflowLedger* ledger,const PitrManagerRequest& r){PitrManagerResult result;if(r.target_uuid.empty())result=Finish(PitrManagerDecisionKind::refused,ErrorStatus(),"SB_AGENT_PITR_TARGET_REQUIRED","agents.pitr.target_required","PITR target identity required",true);else if(r.cluster_route_requested||r.recovery_authority||!r.archive_window_authoritative)result=Finish(PitrManagerDecisionKind::refused,ErrorStatus(),"SB_AGENT_PITR_AUTHORITY_UNTRUSTED","agents.pitr.untrusted_authority","PITR requires authoritative archive window without recovery authority drift",true);else if(r.restore_plan_requested&&r.target_reachable)result=Finish(PitrManagerDecisionKind::request_restore_plan,OkStatus(),"SB_AGENT_PITR_RESTORE_PLAN_READY","agents.pitr.restore_plan_ready","target reachable for restore plan",false);else if(r.estimate_requested&&r.window_available_seconds>0)result=Finish(PitrManagerDecisionKind::estimate_pitr,OkStatus(),"SB_AGENT_PITR_ESTIMATE_READY","agents.pitr.estimate_ready","archive window available for PITR estimate",false);else result=Finish(PitrManagerDecisionKind::refused,ErrorStatus(),"SB_AGENT_PITR_TARGET_UNREACHABLE","agents.pitr.target_unreachable","PITR target or archive window unavailable",true);return ApplyWorkflow(ledger,std::move(result),r);}
const char* pitr_manager_implementation_anchor(){return "pitr_manager";}
}  // namespace scratchbird::core::agents::implemented_agents
