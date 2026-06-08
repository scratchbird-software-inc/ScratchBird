// Copyright (c) 2026 ScratchBird Software Inc.
//
// This Source Code Form is subject to the terms of the Mozilla Public
// License, v. 2.0. If a copy of the MPL was not distributed with this
// file, You can obtain one at https://mozilla.org/MPL/2.0/.
//
// SPDX-License-Identifier: MPL-2.0

#include "agents/identity_manager.hpp"

// CanonicalAgentRegistry/CanonicalAgentManifest owns production exposure;
// this file provides the local identity workflow handler.

#include <utility>

namespace scratchbird::core::agents::implemented_agents {
namespace {
using scratchbird::core::platform::Severity; using scratchbird::core::platform::StatusCode; using scratchbird::core::platform::Subsystem;
Status OkStatus(){return {StatusCode::ok,Severity::info,Subsystem::engine};}
Status ErrorStatus(){return {StatusCode::memory_invalid_request,Severity::error,Subsystem::engine};}
IdentityManagerResult Finish(IdentityManagerDecisionKind d, Status s, std::string c, std::string k, std::string detail, bool f){IdentityManagerResult r; r.status=s; r.decision=d; r.fail_closed=f; r.diagnostic=MakeIdentityManagerDiagnostic(r.status,std::move(c),std::move(k),std::move(detail)); r.evidence.push_back({"decision",IdentityManagerDecisionKindName(r.decision)}); r.evidence.push_back({"parser_authority","false"}); return r;}
bool Clean(const IdentityManagerRequest& r){return !r.principal_uuid.empty()&&r.identity_metrics_authoritative&&r.redaction_policy_valid&&!r.cluster_route_requested&&!r.parser_authority;}
AgentLocalWorkflowRequest WorkflowRequest(const IdentityManagerRequest& r,IdentityManagerDecisionKind decision){AgentLocalWorkflowRequest request;request.domain=AgentLocalWorkflowDomain::identity;request.operation_id=IdentityManagerDecisionKindName(decision);request.idempotency_key=r.idempotency_key.empty()?r.principal_uuid+":"+request.operation_id:r.idempotency_key;request.authority.database_uuid=r.database_uuid;request.authority.principal_uuid=r.operator_principal_uuid.empty()?r.principal_uuid:r.operator_principal_uuid;request.authority.subject_uuid=r.principal_uuid;request.authority.mga_transaction_uuid=r.mga_transaction_uuid;request.authority.evidence_uuid=r.evidence_uuid;request.authority.local_transaction_id=r.local_transaction_id;request.authority.catalog_generation=r.catalog_generation;request.authority.durable_catalog_bound=r.durable_catalog_bound;request.authority.transaction_inventory_bound=r.transaction_inventory_bound;request.authority.security_catalog_authoritative=r.identity_metrics_authoritative;request.authority.redaction_policy_valid=r.redaction_policy_valid;request.authority.parser_authority=r.parser_authority;request.authority.cluster_route_requested=r.cluster_route_requested;request.subsystem_precondition_satisfied=true;request.intended_state_observed=r.intended_state_observed;return request;}
IdentityManagerResult ApplyWorkflow(AgentLocalWorkflowLedger* ledger,IdentityManagerResult result,const IdentityManagerRequest& request){if(ledger==nullptr||result.fail_closed||result.decision==IdentityManagerDecisionKind::refused)return result;const auto workflow=ledger->Apply(WorkflowRequest(request,result.decision));result.workflow_record=workflow.record;result.workflow_record_written=workflow.ok&&!workflow.idempotent;result.outcome_verified=workflow.record.outcome_verified;result.evidence.push_back({"workflow_uuid",workflow.record.workflow_uuid});result.evidence.push_back({"workflow_state",AgentLocalWorkflowStateName(workflow.record.state)});if(!workflow.ok)return Finish(IdentityManagerDecisionKind::refused,ErrorStatus(),workflow.status.diagnostic_code,"agents.identity.workflow_refused",workflow.status.detail,true);return result;}
}
const char* IdentityManagerDecisionKindName(IdentityManagerDecisionKind d){switch(d){case IdentityManagerDecisionKind::lock_user:return "lock_user";case IdentityManagerDecisionKind::require_reauth:return "require_reauth";case IdentityManagerDecisionKind::emit_identity_evidence:return "emit_identity_evidence";case IdentityManagerDecisionKind::refused:return "refused";}return "refused";}
DiagnosticRecord MakeIdentityManagerDiagnostic(Status s,std::string c,std::string k,std::string d){return scratchbird::core::platform::MakeDiagnostic(s.code,s.severity,s.subsystem,std::move(c),std::move(k),{{"detail",std::move(d)}},{},"identity_manager",{});}
IdentityManagerResult EvaluateIdentityManagerRequest(const IdentityManagerRequest& r){return EvaluateIdentityManagerRequest(nullptr,r);}
IdentityManagerResult EvaluateIdentityManagerRequest(AgentLocalWorkflowLedger* ledger,const IdentityManagerRequest& r){IdentityManagerResult result;if(!Clean(r))result=Finish(IdentityManagerDecisionKind::refused,ErrorStatus(),"SB_AGENT_IDENTITY_AUTHORITY_UNTRUSTED","agents.identity.untrusted_authority","identity actions require local trusted metrics and redaction proof",true);else if(r.lock_requested&&(r.explicit_admin_request||r.anomaly_detected))result=Finish(IdentityManagerDecisionKind::lock_user,OkStatus(),"SB_AGENT_IDENTITY_LOCK_READY","agents.identity.lock_ready","lock request has admin/anomaly evidence",false);else if(r.reauth_requested)result=Finish(IdentityManagerDecisionKind::require_reauth,OkStatus(),"SB_AGENT_IDENTITY_REAUTH_READY","agents.identity.reauth_ready","reauth request has session/auth evidence",false);else if(r.emit_evidence_requested)result=Finish(IdentityManagerDecisionKind::emit_identity_evidence,OkStatus(),"SB_AGENT_IDENTITY_EVIDENCE_READY","agents.identity.evidence_ready","identity evidence can be emitted with redaction",false);else result=Finish(IdentityManagerDecisionKind::refused,ErrorStatus(),"SB_AGENT_IDENTITY_ACTION_REQUIRED","agents.identity.action_required","no identity action requested",true);return ApplyWorkflow(ledger,std::move(result),r);}
const char* identity_manager_implementation_anchor(){return "identity_manager";}
}  // namespace scratchbird::core::agents::implemented_agents
