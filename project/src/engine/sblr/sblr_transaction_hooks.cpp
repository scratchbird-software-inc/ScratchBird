// Copyright (c) 2026 ScratchBird Software Inc.
//
// This Source Code Form is subject to the terms of the Mozilla Public
// License, v. 2.0. If a copy of the MPL was not distributed with this
// file, You can obtain one at https://mozilla.org/MPL/2.0/.
//
// SPDX-License-Identifier: MPL-2.0

#include "sblr_transaction_hooks.hpp"

#include "sblr_refusal.hpp"

#include <string>

namespace scratchbird::engine::sblr {
namespace {

SblrResult LocalHookSuccess(std::string operation_id) {
  return MakeSblrSuccess(std::move(operation_id));
}

}  // namespace

SblrResult sb_sblr_begin_statement_boundary(const SblrExecutionContext& context) {
  if (context.statement_uuid.empty()) {
    return RefuseSblrOperation(context, "sb_sblr_begin_statement_boundary", "SB_DIAG_TXN_STATEMENT_BOUNDARY_BEGIN_FAILED", "statement_uuid is required");
  }
  return LocalHookSuccess("sb_sblr_begin_statement_boundary");
}

SblrResult sb_sblr_open_internal_savepoint(const SblrExecutionContext& context, std::string_view savepoint_name) {
  if (!context.transaction_context_present) {
    return RefuseSblrOperation(context, "sb_sblr_open_internal_savepoint", "SB_DIAG_TXN_SAVEPOINT_OPEN_FAILED", "transaction context is required");
  }
  return LocalHookSuccess(std::string("sb_sblr_open_internal_savepoint:") + std::string(savepoint_name));
}

SblrResult sb_sblr_commit_internal_region(const SblrExecutionContext& context, std::string_view savepoint_name) {
  if (!context.transaction_context_present) {
    return RefuseSblrOperation(context, "sb_sblr_commit_internal_region", "SB_DIAG_TXN_SAVEPOINT_COMMIT_FAILED", "transaction context is required");
  }
  return LocalHookSuccess(std::string("sb_sblr_commit_internal_region:") + std::string(savepoint_name));
}

SblrResult sb_sblr_rollback_internal_region(const SblrExecutionContext& context, std::string_view savepoint_name) {
  if (!context.transaction_context_present) {
    return RefuseSblrOperation(context, "sb_sblr_rollback_internal_region", "SB_DIAG_TXN_SAVEPOINT_ROLLBACK_FAILED", "transaction context is required");
  }
  return LocalHookSuccess(std::string("sb_sblr_rollback_internal_region:") + std::string(savepoint_name));
}

SblrResult sb_sblr_sequence_next(const SblrExecutionContext& context, std::string_view sequence_uuid) {
  if (sequence_uuid.empty()) {
    return RefuseSblrOperation(context, "sb_sblr_sequence_next", "SB_DIAG_SEQUENCE_NEXT_FAILED", "sequence_uuid is required");
  }
  return LocalHookSuccess("sb_sblr_sequence_next");
}

SblrResult sb_sblr_runtime_log(const SblrExecutionContext& context, std::string_view message) {
  if (message.empty()) {
    return RefuseSblrOperation(context, "sb_sblr_runtime_log", "SB_DIAG_RUNTIME_LOG_FAILED", "message is required");
  }
  return LocalHookSuccess("sb_sblr_runtime_log");
}

SblrResult sb_sblr_end_statement_boundary(const SblrExecutionContext& context) {
  if (context.statement_uuid.empty()) {
    return RefuseSblrOperation(context, "sb_sblr_end_statement_boundary", "SB_DIAG_TXN_STATEMENT_BOUNDARY_END_FAILED", "statement_uuid is required");
  }
  return LocalHookSuccess("sb_sblr_end_statement_boundary");
}

SblrResult sb_sblr_cluster_begin_branch(const SblrExecutionContext& context) {
  return RefuseClusterTransactionHook(context, "sb_sblr_cluster_begin_branch", "SB_WITH_CLUSTER_TRANSACTIONS");
}

SblrResult sb_sblr_cluster_prepare_branch(const SblrExecutionContext& context) {
  return RefuseClusterTransactionHook(context, "sb_sblr_cluster_prepare_branch", "SB_WITH_CLUSTER_TRANSACTIONS");
}

SblrResult sb_sblr_cluster_record_decision(const SblrExecutionContext& context) {
  return RefuseClusterTransactionHook(context, "sb_sblr_cluster_record_decision", "decision-service authority present");
}

SblrResult sb_sblr_cluster_publish_commit(const SblrExecutionContext& context) {
  return RefuseClusterTransactionHook(context, "sb_sblr_cluster_publish_commit", "cluster publication barrier present");
}

SblrResult sb_sblr_cluster_publish_rollback(const SblrExecutionContext& context) {
  return RefuseClusterTransactionHook(context, "sb_sblr_cluster_publish_rollback", "cluster publication barrier present");
}

SblrResult sb_sblr_cluster_recover_limbo(const SblrExecutionContext& context) {
  return RefuseClusterTransactionHook(context, "sb_sblr_cluster_recover_limbo", "limbo recovery authority present");
}

SblrResult sb_sblr_cluster_route_remote_fragment(const SblrExecutionContext& context) {
  return RefuseClusterTransactionHook(context, "sb_sblr_cluster_route_remote_fragment", "route-fence authority present");
}

}  // namespace scratchbird::engine::sblr
