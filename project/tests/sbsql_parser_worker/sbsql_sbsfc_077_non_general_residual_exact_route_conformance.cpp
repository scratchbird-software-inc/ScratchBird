// Copyright (c) 2026 ScratchBird Software Inc.
//
// This Source Code Form is subject to the terms of the Mozilla Public
// License, v. 2.0. If a copy of the MPL was not distributed with this
// file, You can obtain one at https://mozilla.org/MPL/2.0/.
//
// SPDX-License-Identifier: MPL-2.0

#include "ast/ast.hpp"
#include "binder/binder.hpp"
#include "cst/cst.hpp"
#include "database_lifecycle.hpp"
#include "filespace_header.hpp"
#include "lowering/lowering.hpp"
#include "memory.hpp"
#include "registry/generated/sbsql_generated_registry.hpp"
#include "rendering/rendering.hpp"
#include "sblr_admission.hpp"
#include "sblr_dispatch.hpp"
#include "sblr_engine_envelope.hpp"
#include "sblr_opcode_registry.hpp"
#include "uuid.hpp"

#include <algorithm>
#include <chrono>
#include <cstdlib>
#include <filesystem>
#include <fstream>
#include <iostream>
#include <string>
#include <string_view>
#include <vector>

namespace {

using namespace scratchbird::parser::sbsql;
namespace api = scratchbird::engine::internal_api;
namespace db = scratchbird::storage::database;
namespace filespace = scratchbird::storage::filespace;
namespace memory = scratchbird::core::memory;
namespace sblr = scratchbird::engine::sblr;
namespace uuid = scratchbird::core::uuid;
using scratchbird::core::platform::UuidKind;

constexpr std::string_view kTargetUuid = "019f0000-0000-7000-8000-000000077001";

memory::AllocationPolicy MemoryPolicy() {
  memory::AllocationPolicy policy;
  policy.policy_name = "sbsql_sbsfc_077_non_general_residual";
  policy.hard_limit_bytes = 64ull * 1024ull * 1024ull;
  policy.soft_limit_bytes = 48ull * 1024ull * 1024ull;
  policy.per_context_limit_bytes = 32ull * 1024ull * 1024ull;
  policy.page_buffer_pool_limit_bytes = 16ull * 1024ull * 1024ull;
  policy.track_allocations = true;
  policy.zero_memory_on_release = true;
  return policy;
}

struct CaseRow {
  std::string_view surface_id;
  std::string_view canonical_name;
  std::string_view family;
  std::string_view sql;
  std::string_view operation_id;
  std::string_view opcode;
  std::string_view route_kind;
  std::string_view engine_api_function;
  std::string_view runtime_evidence_kind;
  std::string_view runtime_evidence_id;
  bool requires_transaction_context;
};

const CaseRow kCases[] = {
    {"SBSQL-1DFEDF33C807", "llvm_stmt", "sblr.acceleration.llvm.v3", "LLVM MODULE sblr_projection;", "extensibility.compile_llvm_module", "SBLR_EXTENSIBILITY_COMPILE_LLVM_MODULE", "llvm_sblr_module_compile", "EngineCompileLlvmModule", "llvm_compile_runtime", "interpreter", false},
    {"SBSQL-235C1E504518", "ch_join_strictness", "sblr.query.relational.v3", "QUERY CH_JOIN_STRICTNESS STRICT;", "query.plan_operation", "SBLR_QUERY_PLAN_OPERATION", "join_strictness_descriptor_plan", "EnginePlanOperation", "query_plan", "table_scan", false},
    {"SBSQL-23939D03CA74", "fulltext_search_query", "sblr.query.relational.v3", "QUERY FULLTEXT SEARCH docs FOR 'alpha';", "nosql.search_query", "SBLR_NOSQL_SEARCH_QUERY", "fulltext_search_query", "EngineSearchQuery", "search_query", "full_text_descriptor_query", false},
    {"SBSQL-2F646BF9C145", "show_transaction_runtime", "sblr.transaction.control.v3", "SHOW TRANSACTION RUNTIME;", "transaction.execute_block", "SBLR_TRANSACTION_EXECUTE_BLOCK", "show_transaction_runtime", "EngineExecuteTransactionBlock", "transaction_internal_procedure_block", "show_transaction_runtime", true},
    {"SBSQL-2F6F77257FD1", "psql_statement", "sblr.observability.inspect.v3", "PSQL STATEMENT current;", "observability.show_statements", "SBLR_OBSERVABILITY_SHOW_STATEMENTS", "psql_statement_inspect", "EngineShowStatements", "observability", "observability.show_statements", false},
    {"SBSQL-35979EDB4632", "commit_options", "sblr.transaction.control.v3", "COMMIT RETAIN;", "transaction.commit", "SBLR_TRANSACTION_COMMIT", "commit_options", "EngineCommitTransaction", "transaction_state", "committed", true},
    {"SBSQL-35DF04DE66C3", "stream_consumer_group_stmt", "sblr.query.relational.v3", "STREAM CONSUMER GROUP group_a;", "query.plan_operation", "SBLR_QUERY_PLAN_OPERATION", "stream_consumer_group_plan", "EnginePlanOperation", "query_plan", "table_scan", false},
    {"SBSQL-3B633A20C1D6", "statement", "sblr.observability.inspect.v3", "STATEMENT current;", "observability.show_statements", "SBLR_OBSERVABILITY_SHOW_STATEMENTS", "statement_inspect", "EngineShowStatements", "observability", "observability.show_statements", false},
    {"SBSQL-4015EEDB32B8", "gpu_stmt", "sblr.acceleration.operation.v3", "GPU CAPABILITY;", "extensibility.inspect_gpu_capability", "SBLR_EXTENSIBILITY_INSPECT_GPU_CAPABILITY", "gpu_statement_inspect", "EngineInspectGpuCapability", "gpu_capability", "inspected", false},
    {"SBSQL-407DF23BC3A4", "filespace_name", "sblr.storage.management_operation.v3", "STORAGE FILESPACE main;", "storage.manage_operation", "SBLR_STORAGE_MANAGEMENT_OPERATION", "filespace_profile", "EngineStorageManagementOperation", "storage_management_operation", "filespace_name", false},
    {"SBSQL-FADDF1E52001", "grow_filespace_stmt", "sblr.storage.management_operation.v3", "GROW FILESPACE fs_runtime BY 16 PAGES;", "filespace.preallocate", "SBLR_FILESPACE_PREALLOCATE", "grow_filespace", "EngineFilespacePreallocate", "storage_executor", "PreallocateFilespace", true},
    {"SBSQL-FADDF1E52002", "resize_filespace_stmt", "sblr.storage.management_operation.v3", "RESIZE FILESPACE fs_runtime TO 128 PAGES;", "filespace.preallocate", "SBLR_FILESPACE_PREALLOCATE", "resize_filespace", "EngineFilespacePreallocate", "storage_executor", "PreallocateFilespace", true},
    {"SBSQL-FADDF1E51001", "attach_filespace_stmt", "sblr.storage.management_operation.v3", "ATTACH FILESPACE fs_runtime;", "filespace.attach", "SBLR_FILESPACE_ATTACH", "attach_filespace", "EngineFilespaceLifecycleOperation", "filespace_lifecycle_operation", "filespace.attach", true},
    {"SBSQL-FADDF1E51004", "move_filespace_stmt", "sblr.storage.management_operation.v3", "MOVE FILESPACE fs_move TO '/tmp/sbsfc077-move-target.filespace';", "filespace.move", "SBLR_FILESPACE_MOVE", "move_filespace", "EngineFilespaceLifecycleOperation", "filespace_lifecycle_operation", "filespace.move", true},
    {"SBSQL-FADDF1E53010", "merge_filespace_stmt", "sblr.storage.management_operation.v3", "MERGE FILESPACE fs_merge INTO fs_merge_target;", "filespace.merge", "SBLR_FILESPACE_MERGE", "merge_filespace", "EngineFilespaceLifecycleOperation", "filespace_lifecycle_operation", "filespace.merge", true},
    {"SBSQL-FADDF1E51003", "promote_filespace_stmt", "sblr.storage.management_operation.v3", "PROMOTE FILESPACE fs_runtime;", "filespace.promote", "SBLR_FILESPACE_PROMOTE", "promote_filespace", "EngineFilespaceLifecycleOperation", "filespace_lifecycle_operation", "filespace.promote", true},
    {"SBSQL-FADDF1E53001", "verify_filespace_stmt", "sblr.storage.management_operation.v3", "VERIFY FILESPACE fs_runtime;", "filespace.verify", "SBLR_FILESPACE_VERIFY", "verify_filespace", "EngineFilespaceLifecycleOperation", "filespace_lifecycle_operation", "filespace.verify", true},
    {"SBSQL-FADDF1E53002", "compact_filespace_stmt", "sblr.storage.management_operation.v3", "COMPACT FILESPACE fs_runtime;", "filespace.compact", "SBLR_FILESPACE_COMPACT", "compact_filespace", "EngineFilespaceLifecycleOperation", "filespace_lifecycle_operation", "filespace.compact", true},
    {"SBSQL-FADDF1E52003", "shrink_filespace_stmt", "sblr.storage.management_operation.v3", "SHRINK FILESPACE fs_runtime TO SAFE TRUNCATE BOUNDARY;", "filespace.truncate", "SBLR_FILESPACE_TRUNCATE", "shrink_filespace_safe_truncate", "EngineFilespaceLifecycleOperation", "filespace_lifecycle_operation", "filespace.truncate", true},
    {"SBSQL-FADDF1E53003", "fence_filespace_stmt", "sblr.storage.management_operation.v3", "FENCE FILESPACE fs_runtime;", "filespace.fence", "SBLR_FILESPACE_FENCE", "fence_filespace", "EngineFilespaceLifecycleOperation", "filespace_lifecycle_operation", "filespace.fence", true},
    {"SBSQL-FADDF1E53004", "release_filespace_stmt", "sblr.storage.management_operation.v3", "RELEASE FILESPACE fs_runtime;", "filespace.release", "SBLR_FILESPACE_RELEASE", "release_filespace", "EngineFilespaceLifecycleOperation", "filespace_lifecycle_operation", "filespace.release", true},
    {"SBSQL-FADDF1E53005", "archive_filespace_stmt", "sblr.storage.management_operation.v3", "ARCHIVE FILESPACE fs_archive;", "filespace.archive", "SBLR_FILESPACE_ARCHIVE", "archive_filespace", "EngineFilespaceLifecycleOperation", "filespace_lifecycle_operation", "filespace.archive", true},
    {"SBSQL-FADDF1E53008", "quarantine_filespace_stmt", "sblr.storage.management_operation.v3", "QUARANTINE FILESPACE fs_quarantine;", "filespace.quarantine", "SBLR_FILESPACE_QUARANTINE", "quarantine_filespace", "EngineFilespaceLifecycleOperation", "filespace_lifecycle_operation", "filespace.quarantine", true},
    {"SBSQL-FADDF1E53006", "disconnect_filespace_stmt", "sblr.storage.management_operation.v3", "DISCONNECT FILESPACE fs_runtime;", "filespace.disconnect", "SBLR_FILESPACE_DISCONNECT", "disconnect_filespace", "EngineFilespaceLifecycleOperation", "filespace_lifecycle_operation", "filespace.disconnect", true},
    {"SBSQL-FADDF1E51002", "detach_filespace_stmt", "sblr.storage.management_operation.v3", "DETACH FILESPACE fs_runtime;", "filespace.detach", "SBLR_FILESPACE_DETACH", "detach_filespace", "EngineFilespaceLifecycleOperation", "filespace_lifecycle_operation", "filespace.detach", true},
    {"SBSQL-FADDF1E54001", "create_snapshot_filespace_stmt", "sblr.storage.management_operation.v3", "CREATE SNAPSHOT FILESPACE fs_snapshot;", "filespace.snapshot.create", "SBLR_FILESPACE_SNAPSHOT_CREATE", "create_snapshot_filespace", "EngineFilespaceLifecycleOperation", "filespace_lifecycle_operation", "filespace.snapshot.create", true},
    {"SBSQL-FADDF1E54002", "refresh_snapshot_filespace_stmt", "sblr.storage.management_operation.v3", "REFRESH SNAPSHOT FILESPACE fs_snapshot;", "filespace.snapshot.refresh", "SBLR_FILESPACE_SNAPSHOT_REFRESH", "refresh_snapshot_filespace", "EngineFilespaceLifecycleOperation", "filespace_lifecycle_operation", "filespace.snapshot.refresh", true},
    {"SBSQL-FADDF1E54003", "validate_snapshot_filespace_stmt", "sblr.storage.management_operation.v3", "VALIDATE SNAPSHOT FILESPACE fs_snapshot;", "filespace.snapshot.validate", "SBLR_FILESPACE_SNAPSHOT_VALIDATE", "validate_snapshot_filespace", "EngineFilespaceLifecycleOperation", "filespace_lifecycle_operation", "filespace.snapshot.validate", true},
    {"SBSQL-FADDF1E54004", "retire_snapshot_filespace_stmt", "sblr.storage.management_operation.v3", "RETIRE SNAPSHOT FILESPACE fs_snapshot;", "filespace.snapshot.retire", "SBLR_FILESPACE_SNAPSHOT_RETIRE", "retire_snapshot_filespace", "EngineFilespaceLifecycleOperation", "filespace_lifecycle_operation", "filespace.snapshot.retire", true},
    {"SBSQL-FADDF1E54005", "create_shadow_filespace_stmt", "sblr.storage.management_operation.v3", "CREATE SHADOW FILESPACE fs_shadow;", "filespace.shadow.create", "SBLR_FILESPACE_SHADOW_CREATE", "create_shadow_filespace", "EngineFilespaceLifecycleOperation", "filespace_lifecycle_operation", "filespace.shadow.create", true},
    {"SBSQL-FADDF1E54006", "refresh_shadow_filespace_stmt", "sblr.storage.management_operation.v3", "REFRESH SHADOW FILESPACE fs_shadow;", "filespace.shadow.refresh", "SBLR_FILESPACE_SHADOW_REFRESH", "refresh_shadow_filespace", "EngineFilespaceLifecycleOperation", "filespace_lifecycle_operation", "filespace.shadow.refresh", true},
    {"SBSQL-FADDF1E54007", "validate_shadow_filespace_stmt", "sblr.storage.management_operation.v3", "VALIDATE SHADOW FILESPACE fs_shadow;", "filespace.shadow.validate", "SBLR_FILESPACE_SHADOW_VALIDATE", "validate_shadow_filespace", "EngineFilespaceLifecycleOperation", "filespace_lifecycle_operation", "filespace.shadow.validate", true},
    {"SBSQL-FADDF1E54008", "promote_shadow_filespace_stmt", "sblr.storage.management_operation.v3", "ALTER SHADOW fs_shadow PROMOTE TO PRIMARY AFTER VERIFY;", "filespace.shadow.promote", "SBLR_FILESPACE_SHADOW_PROMOTE", "promote_shadow_filespace", "EngineFilespaceLifecycleOperation", "filespace_lifecycle_operation", "filespace.shadow.promote", true},
    {"SBSQL-FADDF1E53007", "drop_storage_filespace_stmt", "sblr.storage.management_operation.v3", "DROP STORAGE FILESPACE fs_runtime;", "filespace.drop", "SBLR_FILESPACE_DROP", "drop_storage_filespace", "EngineFilespaceLifecycleOperation", "filespace_lifecycle_operation", "filespace.drop", true},
    {"SBSQL-FADDF1E53009", "delete_storage_filespace_stmt", "sblr.storage.management_operation.v3", "DELETE STORAGE FILESPACE fs_delete;", "filespace.delete_physical", "SBLR_FILESPACE_DELETE_PHYSICAL", "delete_storage_filespace", "EngineFilespaceLifecycleOperation", "filespace_lifecycle_operation", "filespace.delete_physical", true},
    {"SBSQL-FADDF1E53011", "repair_filespace_stmt", "sblr.storage.management_operation.v3", "REPAIR FILESPACE fs_repair WITH PLAN verified_header_repair;", "filespace.repair", "SBLR_FILESPACE_REPAIR", "repair_filespace", "EngineFilespaceLifecycleOperation", "filespace_lifecycle_operation", "filespace.repair", true},
    {"SBSQL-FADDF1E53012", "rebuild_filespace_stmt", "sblr.storage.management_operation.v3", "REBUILD FILESPACE fs_rebuild WITH PLAN verified_header_rebuild;", "filespace.rebuild", "SBLR_FILESPACE_REBUILD", "rebuild_filespace", "EngineFilespaceLifecycleOperation", "filespace_lifecycle_operation", "filespace.rebuild", true},
    {"SBSQL-FADDF1E53013", "salvage_filespace_stmt", "sblr.storage.management_operation.v3", "SALVAGE FILESPACE fs_salvage WITH PLAN quarantine_salvage;", "filespace.salvage", "SBLR_FILESPACE_SALVAGE", "salvage_filespace", "EngineFilespaceLifecycleOperation", "filespace_lifecycle_operation", "filespace.salvage", true},
    {"SBSQL-44EC6B9E653D", "quota_limit", "sblr.query.relational.v3", "RESOURCE QUOTA limit_cpu 10;", "query.plan_operation", "SBLR_QUERY_PLAN_OPERATION", "resource_quota_plan", "EnginePlanOperation", "query_plan", "table_scan", false},
    {"SBSQL-46A38D5C9D18", "graph_subquery_stmt", "sblr.query.relational.v3", "GRAPH SUBQUERY social;", "nosql.graph_query", "SBLR_NOSQL_GRAPH_QUERY", "graph_subquery_descriptor_scan", "EngineGraphQuery", "graph_query", "local_descriptor_scan", false},
    {"SBSQL-4B3998EA6BCE", "placement_clause", "sblr.cluster.private_operation.v3", "PLACEMENT PROFILE primary;", "cluster.profile_operation", "SBLR_CLUSTER_PROFILE_OPERATION", "placement_profile", "EngineClusterProfileOperation", "cluster_profile_route", "open_core_profile_metadata", false},
    {"SBSQL-4D4C7A74054C", "cypher_with_clause", "sblr.query.relational.v3", "CYPHER WITH n;", "nosql.graph_query", "SBLR_NOSQL_GRAPH_QUERY", "cypher_with_descriptor_scan", "EngineGraphQuery", "graph_query", "local_descriptor_scan", false},
    {"SBSQL-5D191798949E", "statistics_kind", "sblr.observability.inspect.v3", "STATISTICS KIND histogram;", "observability.show_metrics", "SBLR_OBSERVABILITY_SHOW_METRICS", "statistics_kind_metrics", "EngineShowMetrics", "metrics_registry", "local_node", false},
    {"SBSQL-61FABBFAE0A2", "psql_select_into", "sblr.query.relational.v3", "PSQL SELECT INTO target;", "query.plan_operation", "SBLR_QUERY_PLAN_OPERATION", "psql_select_into_plan", "EnginePlanOperation", "query_plan", "table_scan", false},
    {"SBSQL-65DE8F82E1EB", "psql_execute_statement", "sblr.observability.inspect.v3", "PSQL EXECUTE STATEMENT stmt;", "observability.show_statements", "SBLR_OBSERVABILITY_SHOW_STATEMENTS", "psql_execute_statement_inspect", "EngineShowStatements", "observability", "observability.show_statements", false},
    {"SBSQL-683EC052F3B8", "prewhere_clause", "sblr.query.relational.v3", "QUERY PREWHERE active;", "query.plan_operation", "SBLR_QUERY_PLAN_OPERATION", "prewhere_descriptor_plan", "EnginePlanOperation", "query_plan", "table_scan", false},
    {"SBSQL-6FCF0A0801AB", "psql_autonomous_block", "sblr.transaction.control.v3", "AUTONOMOUS BLOCK;", "transaction.execute_block", "SBLR_TRANSACTION_EXECUTE_BLOCK", "psql_autonomous_block", "EngineExecuteTransactionBlock", "transaction_internal_procedure_block", "psql_autonomous_block", true},
    {"SBSQL-703A59D593A1", "aof_mode", "sblr.storage.management_operation.v3", "STORAGE AOF ON;", "storage.manage_operation", "SBLR_STORAGE_MANAGEMENT_OPERATION", "aof_mode", "EngineStorageManagementOperation", "storage_management_operation", "aof_mode", false},
    {"SBSQL-89723101A513", "region_split_stmt", "sblr.cluster.private_operation.v3", "REGION SPLIT region_a;", "cluster.profile_operation", "SBLR_CLUSTER_PROFILE_OPERATION", "region_split_profile", "EngineClusterProfileOperation", "cluster_profile_route", "open_core_profile_metadata", false},
    {"SBSQL-8DBF202B71F5", "lock_mode", "sblr.transaction.control.v3", "LOCK MODE EXCLUSIVE;", "transaction.set_characteristics", "SBLR_TRANSACTION_SET_CHARACTERISTICS", "lock_mode", "EngineSetTransactionCharacteristics", "transaction_characteristics", "session_defaults_applied", false},
    {"SBSQL-934576EDD0E2", "psql_execute_block", "sblr.transaction.control.v3", "PSQL EXECUTE BLOCK;", "transaction.execute_block", "SBLR_TRANSACTION_EXECUTE_BLOCK", "psql_execute_block", "EngineExecuteTransactionBlock", "transaction_internal_procedure_block", "psql_execute_block", true},
    {"SBSQL-93ED47FFF17E", "statement_extension", "sblr.observability.inspect.v3", "STATEMENT EXTENSION pg_stat;", "observability.show_statements", "SBLR_OBSERVABILITY_SHOW_STATEMENTS", "statement_extension_inspect", "EngineShowStatements", "observability", "observability.show_statements", false},
    {"SBSQL-941FB8EEC93C", "checkpoint_stmt", "sblr.storage.management_operation.v3", "CHECKPOINT;", "storage.manage_operation", "SBLR_STORAGE_MANAGEMENT_OPERATION", "checkpoint_stmt", "EngineStorageManagementOperation", "storage_management_operation", "checkpoint_stmt", false},
    {"SBSQL-98750976AE9F", "grouping_set", "sblr.query.relational.v3", "QUERY GROUPING SET basic;", "query.plan_operation", "SBLR_QUERY_PLAN_OPERATION", "grouping_set_plan", "EnginePlanOperation", "query_plan", "table_scan", false},
    {"SBSQL-98D487EA96E0", "reference_log_mode", "sblr.storage.management_operation.v3", "STORAGE REFERENCE LOG logical;", "storage.manage_operation", "SBLR_STORAGE_MANAGEMENT_OPERATION", "reference_log_mode", "EngineStorageManagementOperation", "storage_management_operation", "reference_log_mode", false},
    {"SBSQL-9B34B7BF03F1", "cypher_where_clause", "sblr.query.relational.v3", "CYPHER WHERE n.active;", "nosql.graph_query", "SBLR_NOSQL_GRAPH_QUERY", "cypher_where_descriptor_scan", "EngineGraphQuery", "graph_query", "local_descriptor_scan", false},
    {"SBSQL-9F9AE11CDE1E", "cypher_call_subquery", "sblr.query.relational.v3", "CYPHER CALL SUBQUERY;", "nosql.graph_query", "SBLR_NOSQL_GRAPH_QUERY", "cypher_call_subquery_descriptor_scan", "EngineGraphQuery", "graph_query", "local_descriptor_scan", false},
    {"SBSQL-A17C46CF3CC9", "truncate_statement", "sblr.observability.inspect.v3", "TRUNCATE STATEMENT current;", "observability.show_statements", "SBLR_OBSERVABILITY_SHOW_STATEMENTS", "truncate_statement_inspect", "EngineShowStatements", "observability", "observability.show_statements", false},
    {"SBSQL-AA0C49D7929D", "tablegroup_name", "sblr.query.relational.v3", "TABLEGROUP tg_main;", "query.plan_operation", "SBLR_QUERY_PLAN_OPERATION", "tablegroup_descriptor_plan", "EnginePlanOperation", "query_plan", "table_scan", false},
    {"SBSQL-B310A7981FC1", "gpu_capability_name", "sblr.acceleration.operation.v3", "GPU CAPABILITY NAME cuda;", "extensibility.inspect_gpu_capability", "SBLR_EXTENSIBILITY_INSPECT_GPU_CAPABILITY", "gpu_capability_name", "EngineInspectGpuCapability", "gpu_capability", "inspected", false},
    {"SBSQL-B4FB30E2A8B7", "cypher_subquery_expr", "sblr.query.relational.v3", "CYPHER SUBQUERY expr;", "nosql.graph_query", "SBLR_NOSQL_GRAPH_QUERY", "cypher_subquery_expr_descriptor_scan", "EngineGraphQuery", "graph_query", "local_descriptor_scan", false},
    {"SBSQL-BA05CF34CED8", "shard_clause", "sblr.cluster.private_operation.v3", "SHARD BY HASH;", "cluster.profile_operation", "SBLR_CLUSTER_PROFILE_OPERATION", "shard_clause_profile", "EngineClusterProfileOperation", "cluster_profile_route", "open_core_profile_metadata", false},
    {"SBSQL-BA60D929B008", "execute_block", "sblr.transaction.control.v3", "EXECUTE BLOCK;", "transaction.execute_block", "SBLR_TRANSACTION_EXECUTE_BLOCK", "execute_block", "EngineExecuteTransactionBlock", "transaction_internal_procedure_block", "execute_block", true},
    {"SBSQL-BC5903A8ED25", "transaction_ref", "sblr.transaction.control.v3", "TRANSACTION REF current;", "transaction.execute_block", "SBLR_TRANSACTION_EXECUTE_BLOCK", "transaction_ref", "EngineExecuteTransactionBlock", "transaction_internal_procedure_block", "transaction_ref", true},
    {"SBSQL-C0853CB531DA", "shard_method", "sblr.cluster.private_operation.v3", "SHARD METHOD hash;", "cluster.profile_operation", "SBLR_CLUSTER_PROFILE_OPERATION", "shard_method_profile", "EngineClusterProfileOperation", "cluster_profile_route", "open_core_profile_metadata", false},
    {"SBSQL-D8AF2C8CE395", "checkpoint_action", "sblr.storage.management_operation.v3", "CHECKPOINT ACTION FLUSH;", "storage.manage_operation", "SBLR_STORAGE_MANAGEMENT_OPERATION", "checkpoint_action", "EngineStorageManagementOperation", "storage_management_operation", "checkpoint_action", false},
    {"SBSQL-DD06A2C3A7AB", "quantified_subquery", "sblr.query.relational.v3", "QUERY QUANTIFIED EXISTS;", "query.plan_operation", "SBLR_QUERY_PLAN_OPERATION", "quantified_subquery_plan", "EnginePlanOperation", "query_plan", "table_scan", false},
    {"SBSQL-E083C027A577", "select_with_timeline", "sblr.query.relational.v3", "SELECT WITH TIMELINE events;", "query.plan_operation", "SBLR_QUERY_PLAN_OPERATION", "timeline_query_plan", "EnginePlanOperation", "query_plan", "table_scan", false},
    {"SBSQL-E2BCC530037E", "storage_tier", "sblr.storage.management_operation.v3", "STORAGE TIER hot;", "storage.manage_operation", "SBLR_STORAGE_MANAGEMENT_OPERATION", "storage_tier", "EngineStorageManagementOperation", "storage_management_operation", "storage_tier", false},
    {"SBSQL-E2C4F8296EAA", "create_resource_group_stmt", "sblr.query.relational.v3", "CREATE RESOURCE GROUP rg;", "query.plan_operation", "SBLR_QUERY_PLAN_OPERATION", "resource_group_management_plan", "EnginePlanOperation", "query_plan", "table_scan", false},
    {"SBSQL-E68A4F7B0843", "gpu_capability_options", "sblr.acceleration.operation.v3", "GPU CAPABILITY OPTIONS JSON;", "extensibility.inspect_gpu_capability", "SBLR_EXTENSIBILITY_INSPECT_GPU_CAPABILITY", "gpu_capability_options", "EngineInspectGpuCapability", "gpu_capability", "inspected", false},
    {"SBSQL-E8872FCED3B3", "region_name", "sblr.cluster.private_operation.v3", "REGION NAME east;", "cluster.profile_operation", "SBLR_CLUSTER_PROFILE_OPERATION", "region_profile", "EngineClusterProfileOperation", "cluster_profile_route", "open_core_profile_metadata", false},
    {"SBSQL-F1273F00C35D", "secret_storage", "sblr.storage.management_operation.v3", "STORAGE SECRET local;", "storage.manage_operation", "SBLR_STORAGE_MANAGEMENT_OPERATION", "secret_storage", "EngineStorageManagementOperation", "storage_management_operation", "secret_storage", false},
    {"SBSQL-F603ACB8C1D1", "checkpoint_options", "sblr.storage.management_operation.v3", "CHECKPOINT OPTIONS FULL;", "storage.manage_operation", "SBLR_STORAGE_MANAGEMENT_OPERATION", "checkpoint_options", "EngineStorageManagementOperation", "storage_management_operation", "checkpoint_options", false},
    {"SBSQL-F67516D19442", "package_init_block", "sblr.transaction.control.v3", "PACKAGE INIT body;", "transaction.execute_block", "SBLR_TRANSACTION_EXECUTE_BLOCK", "package_init_block", "EngineExecuteTransactionBlock", "transaction_internal_procedure_block", "package_init_block", true},
    {"SBSQL-F86E82852A27", "for_select_form", "sblr.query.relational.v3", "FOR SELECT cursor_name;", "query.plan_operation", "SBLR_QUERY_PLAN_OPERATION", "for_select_descriptor_plan", "EnginePlanOperation", "query_plan", "table_scan", false},
    {"SBSQL-F950D94C902C", "grouping_form", "sblr.query.relational.v3", "QUERY GROUPING FORM rollup;", "query.plan_operation", "SBLR_QUERY_PLAN_OPERATION", "grouping_form_plan", "EnginePlanOperation", "query_plan", "table_scan", false},
    {"SBSQL-FBCC40F5D52C", "shadow_name", "sblr.storage.management_operation.v3", "STORAGE SHADOW shadow_a;", "storage.manage_operation", "SBLR_STORAGE_MANAGEMENT_OPERATION", "shadow_name", "EngineStorageManagementOperation", "storage_management_operation", "shadow_name", false},
};

void Require(bool condition, std::string_view message) {
  if (!condition) {
    std::cerr << message << '\n';
    std::exit(EXIT_FAILURE);
  }
}

void ConfigureMemoryFixture() {
  const auto configured = memory::ConfigureDefaultMemoryManagerForFixture(
      MemoryPolicy(), "sbsql_sbsfc_077_non_general_residual_exact_route_conformance");
  Require(configured.ok(), "SBSFC-077 memory fixture configuration failed");
  Require(configured.fixture_mode, "SBSFC-077 memory fixture mode was not active");
}

bool Contains(std::string_view haystack, std::string_view needle) {
  return haystack.find(needle) != std::string_view::npos;
}

bool StartsWith(std::string_view value, std::string_view prefix) {
  return value.size() >= prefix.size() && value.substr(0, prefix.size()) == prefix;
}

bool HasValue(const std::vector<std::string>& values, std::string_view expected) {
  return std::find(values.begin(), values.end(), expected) != values.end();
}

bool HasEvidence(const api::EngineApiResult& result,
                 std::string_view kind,
                 std::string_view id) {
  for (const auto& evidence : result.evidence) {
    if (evidence.evidence_kind == kind && evidence.evidence_id == id) return true;
  }
  return false;
}

std::string LifecycleFilespaceUuid() {
  static const std::string uuid_text = [] {
    const auto generated =
        uuid::GenerateEngineIdentityV7(UuidKind::filespace, 1779810770003);
    Require(generated.ok(), "SBSFC-077 lifecycle filespace UUID generation failed");
    return uuid::UuidToString(generated.value.value);
  }();
  return uuid_text;
}

std::string ArchiveFilespaceUuid() {
  static const std::string uuid_text = [] {
    const auto generated =
        uuid::GenerateEngineIdentityV7(UuidKind::filespace, 1779810770004);
    Require(generated.ok(), "SBSFC-077 archive filespace UUID generation failed");
    return uuid::UuidToString(generated.value.value);
  }();
  return uuid_text;
}

std::string QuarantineFilespaceUuid() {
  static const std::string uuid_text = [] {
    const auto generated =
        uuid::GenerateEngineIdentityV7(UuidKind::filespace, 1779810770005);
    Require(generated.ok(), "SBSFC-077 quarantine filespace UUID generation failed");
    return uuid::UuidToString(generated.value.value);
  }();
  return uuid_text;
}

std::string MoveFilespaceUuid() {
  static const std::string uuid_text = [] {
    const auto generated =
        uuid::GenerateEngineIdentityV7(UuidKind::filespace, 1779810770006);
    Require(generated.ok(), "SBSFC-077 move filespace UUID generation failed");
    return uuid::UuidToString(generated.value.value);
  }();
  return uuid_text;
}

std::string MergeFilespaceUuid() {
  static const std::string uuid_text = [] {
    const auto generated =
        uuid::GenerateEngineIdentityV7(UuidKind::filespace, 1779810770008);
    Require(generated.ok(), "SBSFC-077 merge filespace UUID generation failed");
    return uuid::UuidToString(generated.value.value);
  }();
  return uuid_text;
}

std::string MergeTargetFilespaceUuid() {
  static const std::string uuid_text = [] {
    const auto generated =
        uuid::GenerateEngineIdentityV7(UuidKind::filespace, 1779810770009);
    Require(generated.ok(), "SBSFC-077 merge target filespace UUID generation failed");
    return uuid::UuidToString(generated.value.value);
  }();
  return uuid_text;
}

std::string DeletePhysicalFilespaceUuid() {
  static const std::string uuid_text = [] {
    const auto generated =
        uuid::GenerateEngineIdentityV7(UuidKind::filespace, 1779810770007);
    Require(generated.ok(), "SBSFC-077 physical delete filespace UUID generation failed");
    return uuid::UuidToString(generated.value.value);
  }();
  return uuid_text;
}

std::string RepairFilespaceUuid() {
  static const std::string uuid_text = [] {
    const auto generated =
        uuid::GenerateEngineIdentityV7(UuidKind::filespace, 1779810770011);
    Require(generated.ok(), "SBSFC-077 repair filespace UUID generation failed");
    return uuid::UuidToString(generated.value.value);
  }();
  return uuid_text;
}

std::string RebuildFilespaceUuid() {
  static const std::string uuid_text = [] {
    const auto generated =
        uuid::GenerateEngineIdentityV7(UuidKind::filespace, 1779810770012);
    Require(generated.ok(), "SBSFC-077 rebuild filespace UUID generation failed");
    return uuid::UuidToString(generated.value.value);
  }();
  return uuid_text;
}

std::string SalvageFilespaceUuid() {
  static const std::string uuid_text = [] {
    const auto generated =
        uuid::GenerateEngineIdentityV7(UuidKind::filespace, 1779810770013);
    Require(generated.ok(), "SBSFC-077 salvage filespace UUID generation failed");
    return uuid::UuidToString(generated.value.value);
  }();
  return uuid_text;
}

std::string TargetUuidFor(const CaseRow& row) {
  if (row.operation_id == "filespace.archive") {
    return ArchiveFilespaceUuid();
  }
  if (row.operation_id == "filespace.quarantine") {
    return QuarantineFilespaceUuid();
  }
  if (row.operation_id == "filespace.move") {
    return MoveFilespaceUuid();
  }
  if (row.operation_id == "filespace.merge") {
    return MergeFilespaceUuid();
  }
  if (row.operation_id == "filespace.delete_physical") {
    return DeletePhysicalFilespaceUuid();
  }
  if (row.operation_id == "filespace.repair") {
    return RepairFilespaceUuid();
  }
  if (row.operation_id == "filespace.rebuild") {
    return RebuildFilespaceUuid();
  }
  if (row.operation_id == "filespace.salvage") {
    return SalvageFilespaceUuid();
  }
  if (StartsWith(row.operation_id, "filespace.")) {
    return LifecycleFilespaceUuid();
  }
  return std::string(kTargetUuid);
}

std::string_view ExpectedAdmissionFamily(const CaseRow& row) {
  if (row.family == "sblr.acceleration.operation.v3" ||
      row.family == "sblr.acceleration.llvm.v3" ||
      row.family == "sblr.acceleration.gpu.v3") {
    if (StartsWith(row.operation_id, "extensibility.compile_llvm") ||
        StartsWith(row.operation_id, "llvm.")) {
      return "sblr.acceleration.llvm.v3";
    }
    if (StartsWith(row.operation_id, "extensibility.inspect_gpu") ||
        StartsWith(row.operation_id, "gpu.")) {
      return "sblr.acceleration.gpu.v3";
    }
  }
  if (row.family == "sblr.observability.inspect.v3") {
    if (row.operation_id == "observability.show_metrics") return "sblr.metrics.read.v3";
    if (row.operation_id == "observability.show_transactions") return "sblr.mga.report.v3";
    return "sblr.management.report.v3";
  }
  if (row.operation_id == "query.plan_operation") {
    return "sblr.optimizer.plan.v3";
  }
  if (row.family == "sblr.storage.management_operation.v3") {
    return "sblr.filespace.management.v3";
  }
  if (row.family == "sblr.cluster.private_operation.v3") {
    return "sblr.cluster.control.v3";
  }
  return row.family;
}

const char* LifecycleRoleFor(const CaseRow& row) {
  if (row.operation_id == "filespace.archive") return "secondary_history";
  if (row.operation_id == "filespace.quarantine") return "secondary_data";
  if (row.operation_id == "filespace.move") return "secondary_data";
  if (row.operation_id == "filespace.merge") return "secondary_data";
  if (row.operation_id == "filespace.delete_physical") return "secondary_data";
  if (row.operation_id == "filespace.repair") return "secondary_data";
  if (row.operation_id == "filespace.rebuild") return "secondary_data";
  if (row.operation_id == "filespace.salvage") return "secondary_data";
  return "primary_candidate";
}

SessionContext ParserSession() {
  SessionContext session;
  session.authenticated = true;
  session.session_uuid = "019f0000-0000-7000-8000-000000077101";
  session.connection_uuid = "019f0000-0000-7000-8000-000000077102";
  session.database_uuid = "019f0000-0000-7000-8000-000000077103";
  session.dialect_profile_uuid = "sbsql_v3";
  session.catalog_epoch = 77;
  session.security_policy_epoch = 78;
  session.descriptor_epoch = 79;
  return session;
}

ParserConfig ParserConfigForTest() {
  ParserConfig config;
  config.probe_mode = true;
  config.server_endpoint = "sb_server_sbsfc_077_non_general_residual";
  config.parser_uuid = "019f0000-0000-7000-8000-000000077104";
  config.bundle_contract_id = "sbp_sbsql@sbsfc-077-non-general-residual";
  config.build_id = "sbsql-sbsfc-077-non-general-residual";
  return config;
}

struct PipelineArtifacts {
  CstDocument cst;
  AstDocument ast;
  BoundStatement bound;
  SblrEnvelope envelope;
  SblrVerifierResult verifier;
};

PipelineArtifacts RunPipeline(const CaseRow& row) {
  PipelineArtifacts artifacts;
  const auto session = ParserSession();
  artifacts.cst = BuildCst(std::string(row.sql));
  artifacts.ast = BuildAst(artifacts.cst);
  artifacts.bound = BindAst(artifacts.ast,
                            artifacts.cst,
                            ParserConfigForTest(),
                            session,
                            {std::string(kTargetUuid)});
  artifacts.envelope = LowerToSblr(artifacts.bound, artifacts.cst, session);
  artifacts.verifier = VerifySblrEnvelope(artifacts.envelope);
  return artifacts;
}

void RequireRegistryEvidence(const CaseRow& row) {
  const auto* registry_row = FindGeneratedSurfaceRegistryRowById(row.surface_id);
  Require(registry_row != nullptr, "SBSFC-077 generated registry row missing");
  Require(registry_row->canonical_name == row.canonical_name,
          "SBSFC-077 generated registry canonical name drifted");
  Require(registry_row->source_status == "native_now",
          "SBSFC-077 generated registry status drifted");
  Require(registry_row->cluster_scope == "noncluster_or_profile_scoped",
          "SBSFC-077 generated registry cluster scope drifted");
  Require(registry_row->sblr_operation_family == row.family,
          "SBSFC-077 generated registry SBLR family drifted");
}

void RequireExactLowering(const CaseRow& row, const PipelineArtifacts& artifacts) {
  if (artifacts.cst.messages.has_errors()) std::cerr << RenderMessageVectorSet(artifacts.cst.messages);
  if (artifacts.ast.messages.has_errors()) std::cerr << RenderMessageVectorSet(artifacts.ast.messages);
  if (!artifacts.bound.bound) std::cerr << RenderMessageVectorSet(artifacts.bound.messages);
  if (!artifacts.verifier.admitted) std::cerr << RenderMessageVectorSet(artifacts.verifier.messages);
  Require(!artifacts.cst.messages.has_errors(), "SBSFC-077 CST failed");
  Require(!artifacts.ast.messages.has_errors(), "SBSFC-077 AST failed");
  Require(artifacts.bound.bound, "SBSFC-077 bind failed");
  Require(artifacts.verifier.admitted, "SBSFC-077 verifier rejected exact route");
  if (artifacts.envelope.operation_family != row.family) {
    std::cerr << "SBSFC-077 operation family mismatch for " << row.surface_id
              << ": expected " << row.family << ", got "
              << artifacts.envelope.operation_family << '\n';
  }
  Require(artifacts.envelope.operation_family == row.family, "SBSFC-077 operation family mismatch");
  if (artifacts.envelope.sblr_operation_key != row.family) {
    std::cerr << "SBSFC-077 operation key mismatch for " << row.surface_id
              << ": expected " << row.family << ", got "
              << artifacts.envelope.sblr_operation_key << '\n';
  }
  Require(artifacts.envelope.sblr_operation_key == row.family, "SBSFC-077 operation key mismatch");
  Require(artifacts.envelope.operation_id == row.operation_id, "SBSFC-077 operation id mismatch");
  Require(artifacts.envelope.engine_api_operation_id == row.operation_id,
          "SBSFC-077 engine API operation id mismatch");
  Require(artifacts.envelope.sblr_opcode == row.opcode, "SBSFC-077 opcode mismatch");
  Require(artifacts.envelope.engine_api_function == row.engine_api_function,
          "SBSFC-077 engine API function mismatch");
  Require(HasValue(artifacts.envelope.required_authority_steps,
                   "authority.parser.no_sql_text_execution"),
          "SBSFC-077 parser no-SQL-execution authority missing");
  Require(HasValue(artifacts.envelope.required_authority_steps,
                   "authority.parser.no_storage_or_finality"),
          "SBSFC-077 parser no-finality authority missing");
  Require(Contains(artifacts.envelope.payload, row.surface_id),
          "SBSFC-077 payload missing row surface id");
  Require(Contains(artifacts.envelope.payload, row.route_kind),
          "SBSFC-077 payload missing route kind");
  Require(Contains(artifacts.envelope.payload, row.runtime_evidence_kind),
          "SBSFC-077 payload missing runtime evidence kind");
  Require(Contains(artifacts.envelope.payload, row.runtime_evidence_id),
          "SBSFC-077 payload missing runtime evidence id");
  Require(!artifacts.envelope.parser_executes_sql,
          "SBSFC-077 lowering allowed parser SQL execution");
  Require(!Contains(artifacts.envelope.payload, row.sql),
          "SBSFC-077 payload embedded source SQL text");
  Require(!Contains(artifacts.envelope.payload, "SBSQL_SURFACE_REPLAY") &&
              !Contains(artifacts.envelope.payload, "exact_refusal") &&
              !Contains(artifacts.envelope.payload, "cluster_support_not_enabled"),
          "SBSFC-077 payload used replay or refusal evidence");
  Require(!Contains(artifacts.envelope.payload, "WAL") &&
              !Contains(artifacts.envelope.payload, "wal") &&
              !Contains(artifacts.envelope.payload, "recovery"),
          "SBSFC-077 payload carried WAL/recovery authority");

  const auto admission = scratchbird::server::AdmitServerSblrEnvelope(
      scratchbird::server::ServerSblrAdmissionRequest{artifacts.envelope.payload, false});
  Require(admission.admitted, "SBSFC-077 server admission rejected exact route");
  Require(admission.requires_public_abi_dispatch,
          "SBSFC-077 server admission did not require public ABI dispatch");
  Require(admission.operation_id == row.operation_id,
          "SBSFC-077 server admission operation id mismatch");
  if (admission.operation_family != ExpectedAdmissionFamily(row)) {
    std::cerr << "SBSFC-077 admission family mismatch for " << row.surface_id
              << " operation " << row.operation_id << ": expected "
              << ExpectedAdmissionFamily(row) << ", got "
              << admission.operation_family << '\n';
  }
  Require(admission.operation_family == ExpectedAdmissionFamily(row),
          "SBSFC-077 server admission operation family mismatch");

  const auto* opcode = sblr::LookupSblrOperation(std::string(row.operation_id));
  Require(opcode != nullptr, "SBSFC-077 opcode registry row missing");
  Require(opcode->opcode == row.opcode, "SBSFC-077 opcode registry drifted");
}

std::uint64_t CurrentUnixMillis() {
  return static_cast<std::uint64_t>(
      std::chrono::duration_cast<std::chrono::milliseconds>(
          std::chrono::system_clock::now().time_since_epoch()).count());
}

std::filesystem::path TestDatabasePath() {
  return std::filesystem::temp_directory_path() /
         ("sbsql_sbsfc_077_non_general_residual_" + std::to_string(CurrentUnixMillis()) + ".sbdb");
}

void RemoveDatabaseArtifacts(const std::filesystem::path& path) {
  std::error_code ignored;
  std::filesystem::remove(path, ignored);
  for (const auto suffix : {".sb.api_events", ".sb.crud_events", ".sb.name_events",
                            ".sb.transaction_inventory", ".dirty.manifest",
                            ".sb.owner.lock", ".sb.txn_publish", ".sb.txn_publish.tmp"}) {
    std::filesystem::remove(path.string() + suffix, ignored);
  }
}

std::string CreateMinimalDatabase(const std::filesystem::path& path) {
  db::DatabaseCreateConfig create;
  create.path = path.string();
  const auto database_uuid = uuid::GenerateEngineIdentityV7(UuidKind::database, 1779810770000);
  const auto filespace_uuid = uuid::GenerateEngineIdentityV7(UuidKind::filespace, 1779810770001);
  Require(database_uuid.ok(), "SBSFC-077 database UUID generation failed");
  Require(filespace_uuid.ok(), "SBSFC-077 filespace UUID generation failed");
  create.database_uuid = database_uuid.value;
  create.filespace_uuid = filespace_uuid.value;
  create.page_size = 16384;
  create.creation_unix_epoch_millis = 1779810770002;
  create.allow_minimal_resource_bootstrap = true;
  create.require_resource_seed_pack = false;
  create.allow_overwrite = true;
  const auto created = db::CreateDatabaseFile(create);
  if (!created.ok()) {
    std::cerr << created.diagnostic.diagnostic_code << ':'
              << created.diagnostic.message_key << '\n';
    for (const auto& argument : created.diagnostic.arguments) {
      std::cerr << "  " << argument.key << '=' << argument.value << '\n';
    }
  }
  Require(created.ok(), "SBSFC-077 database create failed");
  return uuid::UuidToString(create.database_uuid.value);
}

api::EngineRequestContext EngineContext(const std::filesystem::path& path,
                                        const std::string& database_uuid) {
  api::EngineRequestContext context;
  context.request_id = "sbsql-sbsfc-077-non-general-residual";
  context.database_path = path.string();
  context.database_uuid.canonical = database_uuid;
  context.session_uuid.canonical = "019f0000-0000-7000-8000-000000077201";
  context.principal_uuid.canonical = "019f0000-0000-7000-8000-000000077202";
  context.current_schema_uuid.canonical = "019f0000-0000-7000-8000-000000077203";
  context.security_context_present = true;
  context.catalog_generation_id = 1;
  context.security_epoch = 1;
  context.resource_epoch = 1;
  context.name_resolution_epoch = 1;
  context.trace_tags.push_back("security.bootstrap");
  context.trace_tags.push_back("right:OBS_METRICS_READ_ALL");
  context.trace_tags.push_back("right:OBS_AGENT_CONTROL");
  context.trace_tags.push_back("right:FILESPACE_LIFECYCLE_CONTROL");
  return context;
}

api::EngineRequestContext BeginEngineTransaction(const std::filesystem::path& path,
                                                 const std::string& database_uuid) {
  auto context = EngineContext(path, database_uuid);
  auto envelope = sblr::MakeSblrEnvelope("transaction.begin",
                                         "SBLR_TRANSACTION_BEGIN",
                                         "trace.sbsfc077.transaction.begin");
  envelope.requires_security_context = true;
  envelope.requires_transaction_context = false;
  envelope.contains_sql_text = false;
  const auto result = sblr::DispatchSblrOperation({context, envelope, api::EngineApiRequest{}});
  Require(result.envelope_validated, "SBSFC-077 transaction begin envelope rejected");
  Require(result.accepted, "SBSFC-077 transaction begin not accepted");
  Require(result.api_result.ok, "SBSFC-077 transaction begin failed");
  context.local_transaction_id = result.api_result.local_transaction_id;
  context.transaction_uuid = result.api_result.transaction_uuid;
  context.snapshot_visible_through_local_transaction_id = context.local_transaction_id;
  return context;
}

void PrintDispatchDiagnostics(const sblr::SblrDispatchResult& result) {
  for (const auto& diagnostic : result.diagnostics) {
    std::cerr << diagnostic.code << ':' << diagnostic.message << '\n';
  }
  for (const auto& diagnostic : result.api_result.diagnostics) {
    std::cerr << diagnostic.code << ':' << diagnostic.detail << '\n';
  }
}

sblr::SblrOperationEnvelope EngineEnvelope(const CaseRow& row) {
  auto envelope = sblr::MakeSblrEnvelope(std::string(row.operation_id),
                                         std::string(row.opcode),
                                         "trace.sbsfc077.non_general_residual");
  const bool filespace_lifecycle = StartsWith(row.operation_id, "filespace.");
  const std::string target_uuid = TargetUuidFor(row);
  envelope.requires_security_context = true;
  envelope.requires_transaction_context = row.requires_transaction_context;
  envelope.requires_cluster_authority = false;
  envelope.contains_sql_text = false;
  envelope.parser_resolved_names_to_uuids = true;
  envelope.operands.push_back({"text", "target_object_uuid", target_uuid});
  envelope.operands.push_back({"text",
                               "target_object_kind",
                               filespace_lifecycle ? "filespace" : "sbsfc077_surface"});
  envelope.operands.push_back({"text", "sbsfc077_surface_id", std::string(row.surface_id)});
  envelope.operands.push_back({"text", "surface_variant", std::string(row.route_kind)});
  if (filespace_lifecycle) {
    envelope.operands.push_back({"text",
                                 "filespace.role",
                                 LifecycleRoleFor(row)});
    envelope.operands.push_back({"text", "filespace.allow_primary_replacement", "true"});
    if (row.operation_id == "filespace.archive") {
      envelope.operands.push_back({"text", "filespace.allow_archive_owner_assignment", "true"});
    }
    if (row.operation_id == "filespace.move") {
      envelope.operands.push_back({"text", "filespace.path", "/tmp/sbsfc077-move-target.filespace"});
      envelope.operands.push_back({"text", "filespace.allow_filespace_move", "true"});
      envelope.operands.push_back({"text", "filespace.page_agent_relocation_complete_for_move", "true"});
      envelope.operands.push_back({"text", "filespace.startup_open_safe_for_move", "true"});
    }
    if (row.operation_id == "filespace.merge") {
      envelope.operands.push_back({"text", "filespace.merge_target_uuid", MergeTargetFilespaceUuid()});
      envelope.operands.push_back({"text", "filespace.allow_filespace_merge", "true"});
      envelope.operands.push_back({"text", "filespace.page_agent_merge_complete_for_merge", "true"});
      envelope.operands.push_back({"text", "filespace.startup_open_safe_for_merge", "true"});
    }
    if (row.operation_id == "filespace.delete_physical") {
      envelope.operands.push_back({"text", "filespace.allow_physical_filespace_delete", "true"});
      envelope.operands.push_back({"text", "filespace.physical_delete_legal_hold_clear", "true"});
      envelope.operands.push_back({"text", "filespace.physical_delete_retention_satisfied", "true"});
      envelope.operands.push_back({"text", "filespace.physical_delete_cleanup_horizon_authoritative", "true"});
    }
    if (row.operation_id == "filespace.repair") {
      envelope.operands.push_back({"text", "filespace.allow_filespace_repair", "true"});
      envelope.operands.push_back({"text", "filespace.repair_plan_authorized", "true"});
      envelope.operands.push_back({"text", "filespace.repair_evidence_preserved", "true"});
    }
    if (row.operation_id == "filespace.rebuild") {
      envelope.operands.push_back({"text", "filespace.allow_filespace_rebuild", "true"});
      envelope.operands.push_back({"text", "filespace.repair_plan_authorized", "true"});
      envelope.operands.push_back({"text", "filespace.repair_evidence_preserved", "true"});
      envelope.operands.push_back({"text", "filespace.rebuild_source_verified", "true"});
      envelope.operands.push_back({"text", "filespace.page_agent_rebuild_complete", "true"});
      envelope.operands.push_back({"text", "filespace.startup_open_safe_for_rebuild", "true"});
    }
    if (row.operation_id == "filespace.salvage") {
      envelope.operands.push_back({"text", "filespace.allow_filespace_salvage", "true"});
      envelope.operands.push_back({"text", "filespace.repair_plan_authorized", "true"});
      envelope.operands.push_back({"text", "filespace.repair_evidence_preserved", "true"});
      envelope.operands.push_back({"text", "filespace.salvage_review_authorized", "true"});
      envelope.operands.push_back({"text", "filespace.salvage_output_quarantined", "true"});
    }
    if (row.operation_id == "filespace.preallocate") {
      envelope.operands.push_back({"u64", "requested_pages", "16"});
      envelope.operands.push_back({"u64", "filespace.current_pages", "64"});
      envelope.operands.push_back({"u64", "filespace.preallocated_pages", "0"});
      envelope.operands.push_back({"u64", "filespace.maximum_pages", "256"});
    }
    if (row.operation_id == "filespace.detach" ||
        row.operation_id == "filespace.disconnect") {
      envelope.operands.push_back({"text", "filespace.allow_primary_detach", "true"});
    }
  }
  if (row.operation_id == "storage.manage_operation") {
    envelope.operands.push_back({"text", "storage_action", std::string(row.canonical_name)});
  } else if (row.operation_id == "cluster.profile_operation") {
    envelope.operands.push_back({"text", "cluster_profile_action", std::string(row.canonical_name)});
  } else if (row.operation_id == "transaction.execute_block") {
    envelope.operands.push_back({"text", "transaction_block_kind", std::string(row.canonical_name)});
  }
  return envelope;
}

api::EngineApiRequest ApiRequestFor(const CaseRow& row) {
  api::EngineApiRequest request;
  const bool filespace_lifecycle = StartsWith(row.operation_id, "filespace.");
  request.target_object.uuid.canonical = TargetUuidFor(row);
  request.target_object.object_kind = filespace_lifecycle ? "filespace" : "sbsfc077_surface";
  request.option_envelopes.push_back(std::string("sbsfc077_surface_id:") + std::string(row.surface_id));
  request.option_envelopes.push_back(std::string("surface_kind:") + std::string(row.canonical_name));
  if (row.operation_id == "query.plan_operation") {
    request.option_envelopes.push_back("operation:table_scan");
    request.option_envelopes.push_back(std::string("query_surface:") + std::string(row.canonical_name));
  } else if (row.operation_id == "nosql.graph_query") {
    request.option_envelopes.push_back("surface:sbsfc077_graph");
    request.option_envelopes.push_back(std::string("graph_surface:") + std::string(row.canonical_name));
  } else if (row.operation_id == "nosql.search_query") {
    request.option_envelopes.push_back("surface:sbsfc077_search");
    request.option_envelopes.push_back("search_kind:full_text_descriptor_query");
  } else if (row.operation_id == "storage.manage_operation") {
    request.option_envelopes.push_back(std::string("storage_action:") + std::string(row.canonical_name));
  } else if (filespace_lifecycle) {
    request.option_envelopes.push_back(std::string("filespace.role:") +
                                       LifecycleRoleFor(row));
    request.option_envelopes.push_back("filespace.allow_primary_replacement:true");
    if (row.operation_id == "filespace.archive") {
      request.option_envelopes.push_back("filespace.allow_archive_owner_assignment:true");
    }
    if (row.operation_id == "filespace.move") {
      request.option_envelopes.push_back("filespace.path:/tmp/sbsfc077-move-target.filespace");
      request.option_envelopes.push_back("filespace.allow_filespace_move:true");
      request.option_envelopes.push_back("filespace.page_agent_relocation_complete_for_move:true");
      request.option_envelopes.push_back("filespace.startup_open_safe_for_move:true");
    }
    if (row.operation_id == "filespace.merge") {
      request.option_envelopes.push_back(std::string("filespace.merge_target_uuid:") +
                                         MergeTargetFilespaceUuid());
      request.option_envelopes.push_back("filespace.allow_filespace_merge:true");
      request.option_envelopes.push_back("filespace.page_agent_merge_complete_for_merge:true");
      request.option_envelopes.push_back("filespace.startup_open_safe_for_merge:true");
    }
    if (row.operation_id == "filespace.delete_physical") {
      request.option_envelopes.push_back("filespace.allow_physical_filespace_delete:true");
      request.option_envelopes.push_back("filespace.physical_delete_legal_hold_clear:true");
      request.option_envelopes.push_back("filespace.physical_delete_retention_satisfied:true");
      request.option_envelopes.push_back("filespace.physical_delete_cleanup_horizon_authoritative:true");
    }
    if (row.operation_id == "filespace.repair") {
      request.option_envelopes.push_back("filespace.allow_filespace_repair:true");
      request.option_envelopes.push_back("filespace.repair_plan_authorized:true");
      request.option_envelopes.push_back("filespace.repair_evidence_preserved:true");
    }
    if (row.operation_id == "filespace.rebuild") {
      request.option_envelopes.push_back("filespace.allow_filespace_rebuild:true");
      request.option_envelopes.push_back("filespace.repair_plan_authorized:true");
      request.option_envelopes.push_back("filespace.repair_evidence_preserved:true");
      request.option_envelopes.push_back("filespace.rebuild_source_verified:true");
      request.option_envelopes.push_back("filespace.page_agent_rebuild_complete:true");
      request.option_envelopes.push_back("filespace.startup_open_safe_for_rebuild:true");
    }
    if (row.operation_id == "filespace.salvage") {
      request.option_envelopes.push_back("filespace.allow_filespace_salvage:true");
      request.option_envelopes.push_back("filespace.repair_plan_authorized:true");
      request.option_envelopes.push_back("filespace.repair_evidence_preserved:true");
      request.option_envelopes.push_back("filespace.salvage_review_authorized:true");
      request.option_envelopes.push_back("filespace.salvage_output_quarantined:true");
    }
    if (row.operation_id == "filespace.preallocate") {
      request.option_envelopes.push_back("requested_pages:16");
      request.option_envelopes.push_back("filespace.current_pages:64");
      request.option_envelopes.push_back("filespace.preallocated_pages:0");
      request.option_envelopes.push_back("filespace.maximum_pages:256");
    }
    if (row.operation_id == "filespace.detach" ||
        row.operation_id == "filespace.disconnect") {
      request.option_envelopes.push_back("filespace.allow_primary_detach:true");
    }
  } else if (row.operation_id == "cluster.profile_operation") {
    request.option_envelopes.push_back(std::string("cluster_profile_action:") + std::string(row.canonical_name));
  } else if (row.operation_id == "transaction.execute_block") {
    request.option_envelopes.push_back(std::string("transaction_block_kind:") + std::string(row.canonical_name));
  } else if (row.operation_id == "transaction.set_characteristics") {
    request.option_envelopes.push_back("transaction_read_mode:read_write");
    request.option_envelopes.push_back("transaction_isolation_level:read_committed");
  } else if (row.operation_id == "extensibility.compile_llvm_module") {
    request.option_envelopes.push_back("compile:jit");
    request.option_envelopes.push_back("module:sblr_projection_unit");
    request.option_envelopes.push_back("sblr_fragment");
    request.option_envelopes.push_back("cache_key:sbsfc077-llvm");
    request.option_envelopes.push_back("allow_interpreter_fallback");
    request.option_envelopes.push_back("simulate_llvm_unavailable");
    request.option_envelopes.push_back("llvm_test_fixture");
  } else if (row.operation_id == "extensibility.inspect_gpu_capability") {
    request.option_envelopes.push_back("simulate_gpu_provider:gpu.simulated");
    request.option_envelopes.push_back("workload:inspect");
    request.option_envelopes.push_back("gpu_profile:inspect_only");
  } else if (row.operation_id == "observability.show_metrics") {
    request.option_envelopes.push_back("family:engine");
  }
  return request;
}

void AttachArchiveFilespaceForMaintenance(const api::EngineRequestContext& context) {
  auto envelope = sblr::MakeSblrEnvelope("filespace.attach",
                                         "SBLR_FILESPACE_ATTACH",
                                         "trace.sbsfc077.archive.filespace.attach");
  envelope.requires_security_context = true;
  envelope.requires_transaction_context = true;
  envelope.requires_cluster_authority = false;
  envelope.contains_sql_text = false;
  envelope.parser_resolved_names_to_uuids = true;
  envelope.operands.push_back({"text", "target_object_uuid", ArchiveFilespaceUuid()});
  envelope.operands.push_back({"text", "target_object_kind", "filespace"});
  envelope.operands.push_back({"text", "filespace.role", "secondary_history"});
  envelope.operands.push_back({"text", "filespace.allow_primary_replacement", "true"});

  api::EngineApiRequest request;
  request.target_object.uuid.canonical = ArchiveFilespaceUuid();
  request.target_object.object_kind = "filespace";
  request.option_envelopes.push_back("filespace.role:secondary_history");
  request.option_envelopes.push_back("filespace.allow_primary_replacement:true");

  const auto result = sblr::DispatchSblrOperation({context, envelope, request});
  PrintDispatchDiagnostics(result);
  Require(result.envelope_validated, "SBSFC-077 archive filespace attach envelope rejected");
  Require(result.accepted, "SBSFC-077 archive filespace attach did not accept route");
  Require(result.dispatched_to_api, "SBSFC-077 archive filespace attach did not dispatch to API");
  Require(result.api_result.ok, "SBSFC-077 archive filespace attach failed");
}

void AttachQuarantineFilespaceForMaintenance(const api::EngineRequestContext& context) {
  auto envelope = sblr::MakeSblrEnvelope("filespace.attach",
                                         "SBLR_FILESPACE_ATTACH",
                                         "trace.sbsfc077.quarantine.filespace.attach");
  envelope.requires_security_context = true;
  envelope.requires_transaction_context = true;
  envelope.requires_cluster_authority = false;
  envelope.contains_sql_text = false;
  envelope.parser_resolved_names_to_uuids = true;
  envelope.operands.push_back({"text", "target_object_uuid", QuarantineFilespaceUuid()});
  envelope.operands.push_back({"text", "target_object_kind", "filespace"});
  envelope.operands.push_back({"text", "filespace.role", "secondary_data"});

  api::EngineApiRequest request;
  request.target_object.uuid.canonical = QuarantineFilespaceUuid();
  request.target_object.object_kind = "filespace";
  request.option_envelopes.push_back("filespace.role:secondary_data");

  const auto result = sblr::DispatchSblrOperation({context, envelope, request});
  PrintDispatchDiagnostics(result);
  Require(result.envelope_validated, "SBSFC-077 quarantine filespace attach envelope rejected");
  Require(result.accepted, "SBSFC-077 quarantine filespace attach did not accept route");
  Require(result.dispatched_to_api, "SBSFC-077 quarantine filespace attach did not dispatch to API");
  Require(result.api_result.ok, "SBSFC-077 quarantine filespace attach failed");
}

void AttachMoveFilespaceForMaintenance(const api::EngineRequestContext& context) {
  auto envelope = sblr::MakeSblrEnvelope("filespace.attach",
                                         "SBLR_FILESPACE_ATTACH",
                                         "trace.sbsfc077.move.filespace.attach");
  envelope.requires_security_context = true;
  envelope.requires_transaction_context = true;
  envelope.requires_cluster_authority = false;
  envelope.contains_sql_text = false;
  envelope.parser_resolved_names_to_uuids = true;
  envelope.operands.push_back({"text", "target_object_uuid", MoveFilespaceUuid()});
  envelope.operands.push_back({"text", "target_object_kind", "filespace"});
  envelope.operands.push_back({"text", "filespace.role", "secondary_data"});

  api::EngineApiRequest request;
  request.target_object.uuid.canonical = MoveFilespaceUuid();
  request.target_object.object_kind = "filespace";
  request.option_envelopes.push_back("filespace.role:secondary_data");

  const auto result = sblr::DispatchSblrOperation({context, envelope, request});
  PrintDispatchDiagnostics(result);
  Require(result.envelope_validated, "SBSFC-077 move filespace attach envelope rejected");
  Require(result.accepted, "SBSFC-077 move filespace attach did not accept route");
  Require(result.dispatched_to_api, "SBSFC-077 move filespace attach did not dispatch to API");
  Require(result.api_result.ok, "SBSFC-077 move filespace attach failed");
}

void AttachMergeFilespacesForMaintenance(const api::EngineRequestContext& context) {
  const struct MergeAttachFixture {
    std::string uuid;
    const char* trace;
    const char* label;
  } fixtures[] = {
      {MergeTargetFilespaceUuid(),
       "trace.sbsfc077.merge.target.filespace.attach",
       "merge target"},
      {MergeFilespaceUuid(),
       "trace.sbsfc077.merge.source.filespace.attach",
       "merge source"},
  };

  for (const auto& fixture : fixtures) {
    auto envelope = sblr::MakeSblrEnvelope("filespace.attach",
                                           "SBLR_FILESPACE_ATTACH",
                                           fixture.trace);
    envelope.requires_security_context = true;
    envelope.requires_transaction_context = true;
    envelope.requires_cluster_authority = false;
    envelope.contains_sql_text = false;
    envelope.parser_resolved_names_to_uuids = true;
    envelope.operands.push_back({"text", "target_object_uuid", fixture.uuid});
    envelope.operands.push_back({"text", "target_object_kind", "filespace"});
    envelope.operands.push_back({"text", "filespace.role", "secondary_data"});

    api::EngineApiRequest request;
    request.target_object.uuid.canonical = fixture.uuid;
    request.target_object.object_kind = "filespace";
    request.option_envelopes.push_back("filespace.role:secondary_data");

    const auto result = sblr::DispatchSblrOperation({context, envelope, request});
    PrintDispatchDiagnostics(result);
    Require(result.envelope_validated,
            std::string("SBSFC-077 ") + fixture.label + " filespace attach envelope rejected");
    Require(result.accepted,
            std::string("SBSFC-077 ") + fixture.label + " filespace attach did not accept route");
    Require(result.dispatched_to_api,
            std::string("SBSFC-077 ") + fixture.label + " filespace attach did not dispatch to API");
    Require(result.api_result.ok,
            std::string("SBSFC-077 ") + fixture.label + " filespace attach failed");
  }
}

std::filesystem::path DeletePhysicalFilespacePath(const std::filesystem::path& database_path) {
  return std::filesystem::path(database_path.string() + "." +
                               DeletePhysicalFilespaceUuid() + ".filespace");
}

std::filesystem::path LifecycleFilespacePath(const std::filesystem::path& database_path,
                                             const std::string& filespace_uuid) {
  return std::filesystem::path(database_path.string() + "." + filespace_uuid + ".filespace");
}

void AttachSecondaryFilespaceForRepair(const api::EngineRequestContext& context,
                                       const std::string& filespace_uuid,
                                       const char* trace,
                                       const char* label) {
  auto envelope = sblr::MakeSblrEnvelope("filespace.attach",
                                         "SBLR_FILESPACE_ATTACH",
                                         trace);
  envelope.requires_security_context = true;
  envelope.requires_transaction_context = true;
  envelope.requires_cluster_authority = false;
  envelope.contains_sql_text = false;
  envelope.parser_resolved_names_to_uuids = true;
  envelope.operands.push_back({"text", "target_object_uuid", filespace_uuid});
  envelope.operands.push_back({"text", "target_object_kind", "filespace"});
  envelope.operands.push_back({"text", "filespace.role", "secondary_data"});

  api::EngineApiRequest request;
  request.target_object.uuid.canonical = filespace_uuid;
  request.target_object.object_kind = "filespace";
  request.option_envelopes.push_back("filespace.role:secondary_data");

  const auto result = sblr::DispatchSblrOperation({context, envelope, request});
  PrintDispatchDiagnostics(result);
  Require(result.envelope_validated,
          std::string("SBSFC-077 ") + label + " attach envelope rejected");
  Require(result.accepted,
          std::string("SBSFC-077 ") + label + " attach did not accept route");
  Require(result.dispatched_to_api,
          std::string("SBSFC-077 ") + label + " attach did not dispatch to API");
  Require(result.api_result.ok,
          std::string("SBSFC-077 ") + label + " attach failed");
}

void WriteRepairFixtureHeader(const std::filesystem::path& database_path,
                              const std::string& database_uuid,
                              const std::string& filespace_uuid) {
  const auto parsed_database = uuid::ParseTypedUuid(UuidKind::database, database_uuid);
  const auto parsed_filespace = uuid::ParseTypedUuid(UuidKind::filespace, filespace_uuid);
  Require(parsed_database.ok(), "SBSFC-077 repair fixture database UUID parse failed");
  Require(parsed_filespace.ok(), "SBSFC-077 repair fixture filespace UUID parse failed");

  filespace::PhysicalFilespaceHeader header;
  header.database_uuid = parsed_database.value;
  header.filespace_uuid = parsed_filespace.value;
  header.role = filespace::FilespaceRole::secondary_data;
  header.state = filespace::FilespaceState::attached;
  header.page_size = 16384;
  header.physical_filespace_id = 17;
  header.total_pages = 64;
  header.free_pages = 32;
  header.preallocated_pages = 8;
  header.allocation_root_page = 4;
  header.header_generation = 1;
  const auto writer_uuid =
      uuid::GenerateEngineIdentityV7(UuidKind::object, CurrentUnixMillis() + 177);
  Require(writer_uuid.ok(), "SBSFC-077 repair fixture writer UUID generation failed");
  header.writer_identity_uuid = writer_uuid.value;
  header.creation_operation_uuid = "sbsfc077.filespace.repair.fixture";

  const auto path = LifecycleFilespacePath(database_path, filespace_uuid);
  const auto written = filespace::CreatePhysicalFilespaceFile(path.string(), header, true);
  Require(written.ok(), "SBSFC-077 repair fixture physical header write failed");
}

void PreparePhysicalRepairFilespace(const api::EngineRequestContext& context,
                                    const std::filesystem::path& database_path,
                                    const std::string& database_uuid,
                                    const CaseRow& row) {
  if (row.operation_id == "filespace.repair") {
    AttachSecondaryFilespaceForRepair(context,
                                      RepairFilespaceUuid(),
                                      "trace.sbsfc077.repair.filespace.attach",
                                      "repair filespace");
    WriteRepairFixtureHeader(database_path, database_uuid, RepairFilespaceUuid());
    return;
  }
  if (row.operation_id == "filespace.rebuild") {
    AttachSecondaryFilespaceForRepair(context,
                                      RebuildFilespaceUuid(),
                                      "trace.sbsfc077.rebuild.filespace.attach",
                                      "rebuild filespace");
    const auto path = LifecycleFilespacePath(database_path, RebuildFilespaceUuid());
    std::ofstream damaged(path);
    damaged << "sbsfc077 damaged filespace header";
    damaged.close();
    Require(std::filesystem::exists(path), "SBSFC-077 rebuild fixture file missing");
    return;
  }
  if (row.operation_id == "filespace.salvage") {
    AttachSecondaryFilespaceForRepair(context,
                                      SalvageFilespaceUuid(),
                                      "trace.sbsfc077.salvage.filespace.attach",
                                      "salvage filespace");
    WriteRepairFixtureHeader(database_path, database_uuid, SalvageFilespaceUuid());
    return;
  }
}

void RequirePhysicalRepairHeader(const std::filesystem::path& database_path,
                                 const CaseRow& row) {
  if (row.operation_id != "filespace.repair" &&
      row.operation_id != "filespace.rebuild" &&
      row.operation_id != "filespace.salvage") {
    return;
  }
  const std::string filespace_uuid = TargetUuidFor(row);
  const auto path = LifecycleFilespacePath(database_path, filespace_uuid);
  const auto physical = filespace::ReadPhysicalFilespaceHeader(path.string());
  Require(physical.ok(), "SBSFC-077 physical repair header read failed after mutation");
  if (row.operation_id == "filespace.salvage") {
    Require(physical.header.role == filespace::FilespaceRole::archive_detached,
            "SBSFC-077 salvage did not isolate header as archive_detached");
    Require(physical.header.state == filespace::FilespaceState::quarantine,
            "SBSFC-077 salvage did not quarantine physical header");
  } else {
    Require(physical.header.role == filespace::FilespaceRole::secondary_data,
            "SBSFC-077 repair/rebuild changed filespace role unexpectedly");
    Require(physical.header.state == filespace::FilespaceState::read_only,
            "SBSFC-077 repair/rebuild did not fence physical header read-only");
  }
}

void RemoveLifecycleFilespaceArtifacts(const std::filesystem::path& database_path) {
  std::error_code ignored;
  for (const std::string& filespace_uuid : {DeletePhysicalFilespaceUuid(),
                                            RepairFilespaceUuid(),
                                            RebuildFilespaceUuid(),
                                            SalvageFilespaceUuid()}) {
    std::filesystem::remove(LifecycleFilespacePath(database_path, filespace_uuid), ignored);
  }
}

void AttachDropDeletePhysicalFilespaceForMaintenance(
    const api::EngineRequestContext& context,
    const std::filesystem::path& database_path) {
  auto attach_envelope = sblr::MakeSblrEnvelope(
      "filespace.attach",
      "SBLR_FILESPACE_ATTACH",
      "trace.sbsfc077.physical_delete.filespace.attach");
  attach_envelope.requires_security_context = true;
  attach_envelope.requires_transaction_context = true;
  attach_envelope.requires_cluster_authority = false;
  attach_envelope.contains_sql_text = false;
  attach_envelope.parser_resolved_names_to_uuids = true;
  attach_envelope.operands.push_back({"text", "target_object_uuid", DeletePhysicalFilespaceUuid()});
  attach_envelope.operands.push_back({"text", "target_object_kind", "filespace"});
  attach_envelope.operands.push_back({"text", "filespace.role", "secondary_data"});

  api::EngineApiRequest attach_request;
  attach_request.target_object.uuid.canonical = DeletePhysicalFilespaceUuid();
  attach_request.target_object.object_kind = "filespace";
  attach_request.option_envelopes.push_back("filespace.role:secondary_data");

  const auto attached = sblr::DispatchSblrOperation({context, attach_envelope, attach_request});
  PrintDispatchDiagnostics(attached);
  Require(attached.envelope_validated, "SBSFC-077 physical delete attach envelope rejected");
  Require(attached.accepted, "SBSFC-077 physical delete attach did not accept route");
  Require(attached.dispatched_to_api, "SBSFC-077 physical delete attach did not dispatch to API");
  Require(attached.api_result.ok, "SBSFC-077 physical delete attach failed");

  const auto physical_path = DeletePhysicalFilespacePath(database_path);
  {
    std::ofstream physical(physical_path);
    physical << "sbsfc077 physical delete fixture";
  }
  Require(std::filesystem::exists(physical_path),
          "SBSFC-077 physical delete fixture file missing");

  auto drop_envelope = sblr::MakeSblrEnvelope(
      "filespace.drop",
      "SBLR_FILESPACE_DROP",
      "trace.sbsfc077.physical_delete.filespace.drop");
  drop_envelope.requires_security_context = true;
  drop_envelope.requires_transaction_context = true;
  drop_envelope.requires_cluster_authority = false;
  drop_envelope.contains_sql_text = false;
  drop_envelope.parser_resolved_names_to_uuids = true;
  drop_envelope.operands.push_back({"text", "target_object_uuid", DeletePhysicalFilespaceUuid()});
  drop_envelope.operands.push_back({"text", "target_object_kind", "filespace"});

  api::EngineApiRequest drop_request;
  drop_request.target_object.uuid.canonical = DeletePhysicalFilespaceUuid();
  drop_request.target_object.object_kind = "filespace";

  const auto dropped = sblr::DispatchSblrOperation({context, drop_envelope, drop_request});
  PrintDispatchDiagnostics(dropped);
  Require(dropped.envelope_validated, "SBSFC-077 physical delete drop envelope rejected");
  Require(dropped.accepted, "SBSFC-077 physical delete drop did not accept route");
  Require(dropped.dispatched_to_api, "SBSFC-077 physical delete drop did not dispatch to API");
  Require(dropped.api_result.ok, "SBSFC-077 physical delete logical drop failed");
  Require(std::filesystem::exists(physical_path),
          "SBSFC-077 logical drop removed physical file before delete");
}

void RequireEngineDispatch(const api::EngineRequestContext& base_context,
                           const std::filesystem::path& path,
                           const std::string& database_uuid,
                           const CaseRow& row) {
  api::EngineRequestContext context = base_context;
  if (row.requires_transaction_context ||
      row.operation_id == "transaction.commit" ||
      row.operation_id == "transaction.execute_block") {
    context = BeginEngineTransaction(path, database_uuid);
  }
  if (row.operation_id == "filespace.archive") {
    AttachArchiveFilespaceForMaintenance(context);
  }
  if (row.operation_id == "filespace.quarantine") {
    AttachQuarantineFilespaceForMaintenance(context);
  }
  if (row.operation_id == "filespace.move") {
    AttachMoveFilespaceForMaintenance(context);
  }
  if (row.operation_id == "filespace.merge") {
    AttachMergeFilespacesForMaintenance(context);
  }
  if (row.operation_id == "filespace.delete_physical") {
    AttachDropDeletePhysicalFilespaceForMaintenance(context, path);
  }
  if (row.operation_id == "filespace.repair" ||
      row.operation_id == "filespace.rebuild" ||
      row.operation_id == "filespace.salvage") {
    PreparePhysicalRepairFilespace(context, path, database_uuid, row);
  }
  const auto result = sblr::DispatchSblrOperation({context, EngineEnvelope(row), ApiRequestFor(row)});
  PrintDispatchDiagnostics(result);
  Require(result.envelope_validated, "SBSFC-077 engine envelope rejected");
  Require(result.accepted, "SBSFC-077 engine dispatch did not accept route");
  Require(result.dispatched_to_api, "SBSFC-077 engine did not dispatch to API");
  Require(result.api_result.operation_id == row.operation_id,
          "SBSFC-077 runtime operation id drifted");
  Require(result.api_result.ok, "SBSFC-077 runtime API did not complete");
  Require(HasEvidence(result.api_result, row.runtime_evidence_kind, row.runtime_evidence_id),
          "SBSFC-077 runtime evidence missing");
  if (row.operation_id == "filespace.delete_physical") {
    Require(HasEvidence(result.api_result, "physical_file_removed", "true"),
            "SBSFC-077 physical delete removal evidence missing");
    Require(!std::filesystem::exists(DeletePhysicalFilespacePath(path)),
            "SBSFC-077 physical delete did not remove staged file");
  }
  if (row.operation_id == "filespace.merge") {
    Require(HasEvidence(result.api_result,
                        "merge_target_filespace",
                        MergeTargetFilespaceUuid()),
            "SBSFC-077 filespace merge target evidence missing");
  }
  RequirePhysicalRepairHeader(path, row);
  Require(HasEvidence(result.api_result, "sbsfc077_surface", row.surface_id) ||
              row.operation_id == "query.plan_operation" ||
              row.operation_id == "observability.show_statements" ||
              row.operation_id == "observability.show_metrics" ||
              row.operation_id == "extensibility.inspect_gpu_capability" ||
              row.operation_id == "extensibility.compile_llvm_module" ||
              row.operation_id == "nosql.graph_query" ||
              row.operation_id == "nosql.search_query" ||
              row.operation_id == "transaction.commit" ||
              row.operation_id == "transaction.set_characteristics",
          "SBSFC-077 runtime did not carry row surface evidence where expected");
}

}  // namespace

int main() {
  ConfigureMemoryFixture();
  static_assert(sizeof(kCases) / sizeof(kCases[0]) == 78);
  for (const auto& row : kCases) {
    RequireRegistryEvidence(row);
    RequireExactLowering(row, RunPipeline(row));
  }

  const auto path = TestDatabasePath();
  RemoveDatabaseArtifacts(path);
  RemoveLifecycleFilespaceArtifacts(path);
  const auto database_uuid = CreateMinimalDatabase(path);
  const auto base_context = EngineContext(path, database_uuid);
  for (const auto& row : kCases) {
    RequireEngineDispatch(base_context, path, database_uuid, row);
  }
  RemoveDatabaseArtifacts(path);
  RemoveLifecycleFilespaceArtifacts(path);

  std::cout << "sbsql_sbsfc_077_non_general_residual_exact_route_conformance=passed\n";
  return EXIT_SUCCESS;
}
