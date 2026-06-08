// Copyright (c) 2026 ScratchBird Software Inc.
//
// This Source Code Form is subject to the terms of the Mozilla Public
// License, v. 2.0. If a copy of the MPL was not distributed with this
// file, You can obtain one at https://mozilla.org/MPL/2.0/.
//
// SPDX-License-Identifier: MPL-2.0

#include "agents/job_control_manager.hpp"

// CanonicalAgentRegistry/CanonicalAgentManifest owns production exposure;
// this file provides the local job-control workflow handler.

#include <utility>

namespace scratchbird::core::agents::implemented_agents {
namespace {
using scratchbird::core::platform::Severity; using scratchbird::core::platform::StatusCode; using scratchbird::core::platform::Subsystem;
Status OkStatus(){return {StatusCode::ok,Severity::info,Subsystem::engine};}
Status ErrorStatus(){return {StatusCode::memory_invalid_request,Severity::error,Subsystem::engine};}
JobControlManagerResult Finish(JobControlManagerDecisionKind d, Status s, std::string c, std::string k, std::string detail, bool f){JobControlManagerResult r; r.status=s; r.decision=d; r.fail_closed=f; r.diagnostic=MakeJobControlManagerDiagnostic(r.status,std::move(c),std::move(k),std::move(detail)); r.evidence.push_back({"decision",JobControlManagerDecisionKindName(r.decision)}); r.evidence.push_back({"client_authority","false"}); return r;}
bool Clean(const JobControlManagerRequest& r){return !r.job_uuid.empty()&&r.job_visible&&r.job_metrics_authoritative&&!r.cluster_route_requested&&!r.client_authority;}
AgentLocalWorkflowRequest WorkflowRequest(const JobControlManagerRequest& r,JobControlManagerDecisionKind decision){AgentLocalWorkflowRequest request;request.domain=AgentLocalWorkflowDomain::job_control;request.operation_id=JobControlManagerDecisionKindName(decision);request.idempotency_key=r.idempotency_key.empty()?r.job_uuid+":"+request.operation_id:r.idempotency_key;request.authority.database_uuid=r.database_uuid;request.authority.principal_uuid=r.principal_uuid;request.authority.subject_uuid=r.job_uuid;request.authority.mga_transaction_uuid=r.mga_transaction_uuid;request.authority.evidence_uuid=r.evidence_uuid;request.authority.local_transaction_id=r.local_transaction_id;request.authority.catalog_generation=r.catalog_generation;request.authority.durable_catalog_bound=r.durable_catalog_bound;request.authority.transaction_inventory_bound=r.transaction_inventory_bound;request.authority.job_queue_authoritative=r.job_visible&&r.job_metrics_authoritative;request.authority.client_authority=r.client_authority;request.authority.cluster_route_requested=r.cluster_route_requested;request.subsystem_precondition_satisfied=true;request.intended_state_observed=r.intended_state_observed;return request;}
JobControlManagerResult ApplyWorkflow(AgentLocalWorkflowLedger* ledger,JobControlManagerResult result,const JobControlManagerRequest& request){if(ledger==nullptr||result.fail_closed||result.decision==JobControlManagerDecisionKind::refused)return result;const auto workflow=ledger->Apply(WorkflowRequest(request,result.decision));result.workflow_record=workflow.record;result.workflow_record_written=workflow.ok&&!workflow.idempotent;result.outcome_verified=workflow.record.outcome_verified;result.evidence.push_back({"workflow_uuid",workflow.record.workflow_uuid});result.evidence.push_back({"workflow_state",AgentLocalWorkflowStateName(workflow.record.state)});if(!workflow.ok)return Finish(JobControlManagerDecisionKind::refused,ErrorStatus(),workflow.status.diagnostic_code,"agents.job_control.workflow_refused",workflow.status.detail,true);return result;}
}
const char* JobControlManagerDecisionKindName(JobControlManagerDecisionKind d){switch(d){case JobControlManagerDecisionKind::cancel_job:return "cancel_job";case JobControlManagerDecisionKind::retry_job:return "retry_job";case JobControlManagerDecisionKind::suppress_job:return "suppress_job";case JobControlManagerDecisionKind::refused:return "refused";}return "refused";}
DiagnosticRecord MakeJobControlManagerDiagnostic(Status s,std::string c,std::string k,std::string d){return scratchbird::core::platform::MakeDiagnostic(s.code,s.severity,s.subsystem,std::move(c),std::move(k),{{"detail",std::move(d)}},{},"job_control_manager",{});}
JobControlManagerResult EvaluateJobControlManagerRequest(const JobControlManagerRequest& r){return EvaluateJobControlManagerRequest(nullptr,r);}
JobControlManagerResult EvaluateJobControlManagerRequest(AgentLocalWorkflowLedger* ledger,const JobControlManagerRequest& r){JobControlManagerResult result;if(!Clean(r))result=Finish(JobControlManagerDecisionKind::refused,ErrorStatus(),"SB_AGENT_JOB_CONTROL_AUTHORITY_UNTRUSTED","agents.job_control.untrusted_authority","job action requires visible job and trusted job runtime metrics",true);else if(r.cancel_requested&&r.job_cancellable)result=Finish(JobControlManagerDecisionKind::cancel_job,OkStatus(),"SB_AGENT_JOB_CONTROL_CANCEL_READY","agents.job_control.cancel_ready","job is cancellable",false);else if(r.retry_requested&&r.retry_policy_valid)result=Finish(JobControlManagerDecisionKind::retry_job,OkStatus(),"SB_AGENT_JOB_CONTROL_RETRY_READY","agents.job_control.retry_ready","retry policy and failure state are valid",false);else if(r.suppress_requested&&r.suppression_scope_valid)result=Finish(JobControlManagerDecisionKind::suppress_job,OkStatus(),"SB_AGENT_JOB_CONTROL_SUPPRESS_READY","agents.job_control.suppress_ready","job suppression scope is valid",false);else result=Finish(JobControlManagerDecisionKind::refused,ErrorStatus(),"SB_AGENT_JOB_CONTROL_ACTION_REFUSED","agents.job_control.action_refused","requested job action is not allowed",true);return ApplyWorkflow(ledger,std::move(result),r);}
const char* job_control_manager_implementation_anchor(){return "job_control_manager";}
}  // namespace scratchbird::core::agents::implemented_agents
