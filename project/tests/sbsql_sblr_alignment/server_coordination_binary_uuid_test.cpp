// Copyright (c) 2026 ScratchBird Software Inc.
// SPDX-License-Identifier: MPL-2.0
#include "../../src/server/session_registry.hpp"
#include "../../src/server/statement_coordination_uuid.hpp"

#include "../../src/engine/internal_api/sblr_accel_gpu_compile_coordinator.hpp"
#include "../../src/engine/internal_api/sblr_advisory_lock_coordinator.hpp"
#include "../../src/engine/internal_api/sblr_bridge_begin_transaction_coordinator.hpp"
#include "../../src/engine/internal_api/sblr_ddl_alter_aggregate_coordinator.hpp"
#include "../../src/engine/internal_api/sblr_ddl_alter_rewrite_rule_coordinator.hpp"
#include "../../src/engine/internal_api/sblr_admin_register_external_relation_resolver_coordinator.hpp"
#include "../../src/engine/internal_api/sblr_ddl_create_domain_coordinator.hpp"
#include "../../src/engine/internal_api/sblr_ddl_create_operator_family_coordinator.hpp"
#include "../../src/engine/internal_api/sblr_bitemporal_for_versions_between_coordinator.hpp"
#include "../../src/engine/internal_api/sblr_ddl_create_temporary_table_coordinator.hpp"
#include "../../src/engine/internal_api/sblr_database_create_template_clone_coordinator.hpp"
#include "../../src/engine/internal_api/sblr_ddl_drop_publication_coordinator.hpp"
#include "../../src/engine/internal_api/sblr_ddl_purge_system_history_coordinator.hpp"
#include "../../src/engine/internal_api/sblr_ddl_create_collation_coordinator.hpp"
#include "../../src/engine/internal_api/sblr_ddl_create_named_collection_coordinator.hpp"
#include "../../src/engine/internal_api/sblr_kv_structured_stream_read_coordinator.hpp"
#include "../../src/engine/internal_api/sblr_result_set_pass_coordinator.hpp"
#include "../../src/engine/internal_api/sblr_ddl_drop_dictionary_coordinator.hpp"
#include "../../src/engine/internal_api/sblr_sec_drop_policy_coordinator.hpp"
#include "../../src/engine/internal_api/sblr_ddl_drop_operator_family_coordinator.hpp"
#include "../../src/engine/internal_api/sblr_admin_unregister_external_relation_resolver_coordinator.hpp"
#include "../../src/engine/internal_api/sblr_bitemporal_period_overlap_coordinator.hpp"
#include "../../src/engine/internal_api/sblr_dml_async_insert_cancel_coordinator.hpp"
#include "../../src/engine/internal_api/sblr_window_coordinator.hpp"
#include "../../src/engine/internal_api/sblr_cast_coordinator.hpp"
#include "../../src/engine/internal_api/sblr_kv_structured_read_coordinator.hpp"
#include "../../src/engine/internal_api/sblr_database_deserialize_logical_snapshot_coordinator.hpp"
#include "../../src/engine/internal_api/sblr_access_cursor_open_coordinator.hpp"
#include "../../src/engine/internal_api/sblr_read_range_coordinator.hpp"
#include "../../src/engine/internal_api/sblr_accel_llvm_policy_set_coordinator.hpp"
#include "../../src/engine/internal_api/sblr_bitemporal_as_of_valid_time_coordinator.hpp"
#include "../../src/engine/internal_api/sblr_ddl_create_operator_class_coordinator.hpp"
#include "../../src/engine/internal_api/sblr_bitemporal_as_of_coordinator.hpp"
#include "../../src/engine/internal_api/sblr_ddl_drop_view_coordinator.hpp"
#include "../../src/engine/internal_api/sblr_bulk_export_stream_coordinator.hpp"
#include "../../src/engine/internal_api/sblr_sec_create_role_coordinator.hpp"
#include "../../src/engine/internal_api/sblr_ddl_create_table_as_query_coordinator.hpp"
#include "../../src/engine/internal_api/sblr_bridge_rollback_transaction_coordinator.hpp"
#include "../../src/engine/internal_api/sblr_ddl_drop_event_trigger_coordinator.hpp"
#include "../../src/engine/internal_api/sblr_ddl_alter_operator_family_coordinator.hpp"
#include "../../src/engine/internal_api/sblr_dml_async_insert_submit_coordinator.hpp"
#include "../../src/engine/internal_api/sblr_sequence_currval_coordinator.hpp"
#include "../../src/engine/internal_api/sblr_ddl_create_cast_coordinator.hpp"
#include "../../src/engine/internal_api/sblr_ddl_create_aggregate_coordinator.hpp"
#include "../../src/engine/internal_api/sblr_ddl_create_macro_coordinator.hpp"
#include "../../src/engine/internal_api/sblr_ddl_drop_type_coordinator.hpp"
#include "../../src/engine/internal_api/sblr_kv_structured_stream_append_coordinator.hpp"
#include "../../src/engine/internal_api/sblr_system_config_set_coordinator.hpp"
#include "../../src/engine/internal_api/sblr_dml_async_insert_status_coordinator.hpp"
#include "../../src/engine/internal_api/sblr_ddl_create_subscription_coordinator.hpp"
#include "../../src/engine/internal_api/sblr_ddl_drop_collation_coordinator.hpp"
#include "../../src/engine/internal_api/sblr_kv_structured_scan_coordinator.hpp"
#include "../../src/engine/internal_api/sblr_ddl_drop_operator_class_coordinator.hpp"
#include "../../src/engine/internal_api/sblr_versioned_revert_coordinator.hpp"
#include "../../src/engine/internal_api/sblr_ddl_drop_continuous_view_coordinator.hpp"
#include "../../src/engine/internal_api/sblr_ddl_drop_temporary_table_coordinator.hpp"
#include "../../src/engine/internal_api/sblr_sec_drop_group_mapping_coordinator.hpp"
#include "../../src/engine/internal_api/sblr_ddl_validate_constraint_coordinator.hpp"
#include "../../src/engine/internal_api/sblr_read_stream_coordinator.hpp"
#include "../../src/engine/internal_api/sblr_ddl_drop_operator_coordinator.hpp"
#include "../../src/engine/internal_api/sblr_insert_coordinator.hpp"
#include "../../src/engine/internal_api/sblr_sec_deauthenticate_coordinator.hpp"
#include "../../src/engine/internal_api/sblr_ddl_drop_timeseries_value_cache_coordinator.hpp"
#include "../../src/engine/internal_api/sblr_sequence_setval_coordinator.hpp"
#include "../../src/engine/internal_api/sblr_query_numeric_coordinator.hpp"
#include "../../src/engine/internal_api/sblr_sequence_nextval_coordinator.hpp"
#include "../../src/engine/internal_api/sblr_delete_coordinator.hpp"
#include "../../src/engine/internal_api/sblr_table_analyze_coordinator.hpp"
#include "../../src/engine/internal_api/sblr_sec_create_group_mapping_coordinator.hpp"
#include "../../src/engine/internal_api/sblr_sort_coordinator.hpp"
#include "../../src/engine/internal_api/sblr_versioned_status_read_coordinator.hpp"
#include "../../src/engine/internal_api/sblr_versioned_diff_coordinator.hpp"
#include "../../src/engine/internal_api/sblr_kv_structured_mutate_coordinator.hpp"
#include "../../src/engine/internal_api/sblr_ddl_drop_named_collection_coordinator.hpp"
#include "../../src/engine/internal_api/sblr_table_truncate_coordinator.hpp"
#include "../../src/engine/internal_api/sblr_security_drop_privilege_template_coordinator.hpp"
#include "../../src/engine/internal_api/sblr_accel_llvm_compile_coordinator.hpp"
#include "../../src/engine/internal_api/sblr_ddl_timeseries_series_cardinality_policy_coordinator.hpp"
#include "../../src/engine/internal_api/sblr_group_coordinator.hpp"
#include "../../src/engine/internal_api/sblr_project_coordinator.hpp"
#include "../../src/engine/internal_api/sblr_read_by_key_coordinator.hpp"
#include "../../src/engine/internal_api/sblr_versioned_reset_coordinator.hpp"
#include "../../src/engine/internal_api/sblr_statement_batch_coordinator.hpp"
#include "../../src/engine/internal_api/sblr_sec_authenticate_coordinator.hpp"
#include "../../src/engine/internal_api/sblr_atomic_cas_coordinator.hpp"
#include "../../src/engine/internal_api/sblr_ddl_alter_view_coordinator.hpp"
#include "../../src/engine/internal_api/sblr_sec_create_policy_coordinator.hpp"
#include "../../src/engine/internal_api/sblr_versioned_tag_coordinator.hpp"
#include "../../src/engine/internal_api/sblr_ddl_create_index_coordinator.hpp"
#include "../../src/engine/internal_api/sblr_compare_coordinator.hpp"
#include "../../src/engine/internal_api/sblr_versioned_branch_delete_coordinator.hpp"
#include "../../src/engine/internal_api/sblr_ddl_create_dictionary_coordinator.hpp"
#include "../../src/engine/internal_api/sblr_bridge_health_coordinator.hpp"
#include "../../src/engine/internal_api/sblr_ddl_alter_extension_coordinator.hpp"
#include "../../src/engine/internal_api/sblr_ddl_drop_cast_coordinator.hpp"
#include "../../src/engine/internal_api/sblr_bridge_open_session_coordinator.hpp"
#include "../../src/engine/internal_api/sblr_ddl_create_operator_coordinator.hpp"
#include "../../src/engine/internal_api/sblr_ddl_drop_materialized_view_coordinator.hpp"
#include "../../src/engine/internal_api/sblr_versioned_branch_create_coordinator.hpp"
#include "../../src/engine/internal_api/sblr_ddl_create_table_coordinator.hpp"
#include "../../src/engine/internal_api/sblr_ddl_set_table_type_enforcement_coordinator.hpp"
#include "../../src/engine/internal_api/sblr_sec_alter_user_coordinator.hpp"
#include "../../src/engine/internal_api/sblr_function_invoke_coordinator.hpp"
#include "../../src/engine/internal_api/sblr_ddl_alter_type_coordinator.hpp"
#include "../../src/engine/internal_api/sblr_ddl_create_rewrite_rule_coordinator.hpp"
#include "../../src/engine/internal_api/sblr_ddl_drop_extension_coordinator.hpp"
#include "../../src/engine/internal_api/sblr_ddl_alter_event_trigger_coordinator.hpp"
#include "../../src/engine/internal_api/sblr_bridge_open_channel_coordinator.hpp"
#include "../../src/engine/internal_api/sblr_ddl_drop_aggregate_coordinator.hpp"
#include "../../src/engine/internal_api/sblr_accel_gpu_policy_set_coordinator.hpp"
#include "../../src/engine/internal_api/sblr_ddl_set_index_optimizer_eligibility_coordinator.hpp"
#include "../../src/engine/internal_api/sblr_database_serialize_logical_snapshot_coordinator.hpp"
#include "../../src/engine/internal_api/sblr_function_call_coordinator.hpp"
#include "../../src/engine/internal_api/sblr_operator_call_coordinator.hpp"
#include "../../src/engine/internal_api/sblr_sec_revoke_coordinator.hpp"
#include "../../src/engine/internal_api/sblr_ddl_alter_publication_coordinator.hpp"
#include "../../src/engine/internal_api/sblr_sec_alter_role_coordinator.hpp"
#include "../../src/engine/internal_api/sblr_aggregate_invoke_coordinator.hpp"
#include "../../src/engine/internal_api/sblr_ddl_create_view_coordinator.hpp"
#include "../../src/engine/internal_api/sblr_ddl_drop_macro_coordinator.hpp"
#include "../../src/engine/internal_api/sblr_bridge_authenticate_coordinator.hpp"
#include "../../src/engine/internal_api/sblr_verifiable_history_prove_coordinator.hpp"
#include "../../src/engine/internal_api/sblr_ddl_drop_subscription_coordinator.hpp"
#include "../../src/engine/internal_api/sblr_bridge_describe_capabilities_coordinator.hpp"
#include "../../src/engine/internal_api/sblr_ddl_alter_domain_coordinator.hpp"
#include "../../src/engine/internal_api/sblr_ddl_drop_srs_coordinator.hpp"
#include "../../src/engine/internal_api/sblr_session_setting_set_coordinator.hpp"
#include "../../src/engine/internal_api/sblr_ddl_rename_object_vector_coordinator.hpp"
#include "../../src/engine/internal_api/sblr_domain_operation_coordinator.hpp"
#include "../../src/engine/internal_api/sblr_merge_coordinator.hpp"
#include "../../src/engine/internal_api/sblr_sec_grant_coordinator.hpp"
#include "../../src/engine/internal_api/sblr_ddl_create_type_coordinator.hpp"
#include "../../src/engine/internal_api/sblr_ddl_drop_index_coordinator.hpp"
#include "../../src/engine/internal_api/sblr_ddl_alter_timeseries_value_cache_coordinator.hpp"
#include "../../src/engine/internal_api/sblr_dml_timeseries_schema_write_coordinator.hpp"
#include "../../src/engine/internal_api/sblr_session_role_switch_coordinator.hpp"
#include "../../src/engine/internal_api/sblr_limit_coordinator.hpp"
#include "../../src/engine/internal_api/sblr_sec_drop_user_coordinator.hpp"
#include "../../src/engine/internal_api/sblr_security_alter_privilege_template_coordinator.hpp"
#include "../../src/engine/internal_api/sblr_ddl_alter_dictionary_coordinator.hpp"
#include "../../src/engine/internal_api/sblr_update_coordinator.hpp"
#include "../../src/engine/internal_api/sblr_accel_llvm_inspect_coordinator.hpp"
#include "../../src/engine/internal_api/sblr_ddl_create_package_coordinator.hpp"
#include "../../src/engine/internal_api/sblr_bridge_commit_transaction_coordinator.hpp"
#include "../../src/engine/internal_api/sblr_ddl_alter_subscription_coordinator.hpp"
#include "../../src/engine/internal_api/sblr_aggregate_coordinator.hpp"
#include "../../src/engine/internal_api/sblr_ddl_drop_function_coordinator.hpp"
#include "../../src/engine/internal_api/sblr_ddl_create_extension_coordinator.hpp"
#include "../../src/engine/internal_api/sblr_return_result_set_coordinator.hpp"
#include "../../src/engine/internal_api/sblr_dml_counter_add_coordinator.hpp"
#include "../../src/engine/internal_api/sblr_ddl_drop_rewrite_rule_coordinator.hpp"
#include "../../src/engine/internal_api/sblr_kv_structured_timeseries_coordinator.hpp"
#include "../../src/engine/internal_api/sblr_sec_drop_role_coordinator.hpp"
#include "../../src/engine/internal_api/sblr_ddl_alter_collation_coordinator.hpp"
#include "../../src/engine/internal_api/sblr_bridge_close_session_coordinator.hpp"
#include "../../src/engine/internal_api/sblr_advisory_lock_release_coordinator.hpp"
#include "../../src/engine/internal_api/sblr_ddl_refresh_materialized_view_coordinator.hpp"
#include "../../src/engine/internal_api/sblr_ddl_create_event_trigger_coordinator.hpp"
#include "../../src/engine/internal_api/sblr_udr_invoke_coordinator.hpp"
#include "../../src/engine/internal_api/sblr_accel_llvm_invalidate_coordinator.hpp"
#include "../../src/engine/internal_api/sblr_accel_gpu_invalidate_coordinator.hpp"
#include "../../src/engine/internal_api/sblr_ddl_create_or_replace_srs_coordinator.hpp"
#include "../../src/engine/internal_api/sblr_accel_gpu_inspect_coordinator.hpp"
#include "../../src/engine/internal_api/sblr_ddl_create_timeseries_value_cache_coordinator.hpp"
#include "../../src/engine/internal_api/sblr_advanced_datatype_family_coordinator.hpp"
#include "../../src/engine/internal_api/sblr_cluster_alter_placement_policy_coordinator.hpp"
#include "../../src/engine/internal_api/sblr_ddl_drop_fdw_coordinator.hpp"
#include "../../src/engine/internal_api/sblr_ddl_alter_package_coordinator.hpp"
#include "../../src/engine/internal_api/sblr_ddl_create_synonym_coordinator.hpp"
#include "../../src/engine/internal_api/sblr_session_default_qualifier_coordinator.hpp"
#include "../../src/engine/internal_api/sblr_ddl_alter_procedure_coordinator.hpp"
#include "../../src/engine/internal_api/sblr_ddl_drop_procedure_coordinator.hpp"
#include "../../src/engine/internal_api/sblr_ddl_create_continuous_view_coordinator.hpp"
#include "../../src/engine/internal_api/sblr_context_set_coordinator.hpp"
#include "../../src/engine/internal_api/sblr_ddl_alter_function_coordinator.hpp"
#include "../../src/engine/internal_api/sblr_ddl_drop_package_coordinator.hpp"
#include "../../src/engine/internal_api/sblr_ddl_create_materialized_view_coordinator.hpp"
#include "../../src/engine/internal_api/sblr_ddl_create_sequence_coordinator.hpp"
#include "../../src/engine/internal_api/sblr_session_snapshot_handle_coordinator.hpp"
#include "../../src/engine/internal_api/sblr_ddl_drop_synonym_coordinator.hpp"
#include "../../src/engine/internal_api/sblr_cluster_declare_region_coordinator.hpp"
#include "../../src/engine/internal_api/sblr_ddl_create_function_coordinator.hpp"
#include "../../src/engine/internal_api/sblr_cluster_drop_placement_policy_coordinator.hpp"
#include "../../src/engine/internal_api/sblr_session_setting_reset_coordinator.hpp"
#include "../../src/engine/internal_api/sblr_ddl_create_foreign_table_coordinator.hpp"
#include "../../src/engine/internal_api/sblr_cluster_declare_data_placement_coordinator.hpp"
#include "../../src/engine/internal_api/sblr_ddl_create_fdw_coordinator.hpp"
#include "../../src/engine/internal_api/sblr_ddl_rename_object_coordinator.hpp"
#include "../../src/engine/internal_api/sblr_session_setting_get_coordinator.hpp"
#include "../../src/engine/internal_api/sblr_cluster_declare_availability_zone_coordinator.hpp"
#include "../../src/engine/internal_api/sblr_ddl_drop_sequence_coordinator.hpp"
#include "../../src/engine/internal_api/sblr_ddl_drop_foreign_table_coordinator.hpp"
#include "../../src/engine/internal_api/sblr_ddl_alter_continuous_view_coordinator.hpp"
#include "../../src/engine/internal_api/sblr_ddl_alter_sequence_coordinator.hpp"
#include "../../src/engine/internal_api/sblr_session_discard_coordinator.hpp"
#include "../../src/engine/internal_api/sblr_cluster_create_placement_policy_coordinator.hpp"

#include "../../src/engine/internal_api/sblr_dml_conditional_mutate_coordinator.hpp"
#include "../../src/engine/internal_api/sblr_bulk_import_stream_coordinator.hpp"
#include "../../src/engine/internal_api/sblr_ddl_alter_trigger_coordinator.hpp"
#include "../../src/engine/internal_api/sblr_atomic_rmw_coordinator.hpp"
#include "../../src/engine/internal_api/sblr_ddl_drop_trigger_coordinator.hpp"
#include "../../src/engine/internal_api/sblr_ddl_create_procedure_coordinator.hpp"
#include "../../src/engine/internal_api/sblr_procedure_invoke_coordinator.hpp"
#include "../../src/engine/internal_api/sblr_ddl_create_publication_coordinator.hpp"
#include "../../src/engine/internal_api/sblr_ddl_drop_table_coordinator.hpp"
#include "../../src/engine/internal_api/sblr_security_create_privilege_template_coordinator.hpp"
#include "../../src/engine/internal_api/sblr_ddl_create_trigger_coordinator.hpp"

#include <algorithm>
#include <cstdlib>
#include <cstdio>
#include <type_traits>

namespace server = scratchbird::server;
using Uuid = scratchbird::core::platform::Uuid;
using CoordinationMap = decltype(server::ServerSessionRegistry::parameter_coordinations_by_uuid);
static_assert(sizeof(Uuid) == 16);
static_assert(std::is_same_v<CoordinationMap::key_type, Uuid>);
static_assert(std::is_same_v<decltype(server::ServerParameterExecutionCoordinationRecord::session_uuid), Uuid>);
static_assert(std::is_same_v<decltype(server::ServerParameterExecutionCoordinationRecord::operation_uuid), Uuid>);
static_assert(!std::is_constructible_v<Uuid, std::string>);

// Compile-time admission contract: receipt identity is always a raw UUID.
// These declarations do not qualify the command bodies as implemented.
template <typename R, typename... Args>
std::true_type BinaryReceiptSignature(R (*)(const scratchbird::engine::internal_api::EngineRequestContext&, const scratchbird::engine::internal_api::EngineUuid&, Args...));
namespace api = scratchbird::engine::internal_api;
static_assert(decltype(BinaryReceiptSignature(&api::CompileSblrAccelGpuCompileDescriptor))::value);
static_assert(decltype(BinaryReceiptSignature(&api::CompileSblrAccelGpuInspectDescriptor))::value);
static_assert(decltype(BinaryReceiptSignature(&api::CompileSblrAccelGpuInvalidateDescriptor))::value);
static_assert(decltype(BinaryReceiptSignature(&api::CompileSblrAccelGpuPolicySetDescriptor))::value);
static_assert(decltype(BinaryReceiptSignature(&api::CompileSblrAccelLlvmCompileDescriptor))::value);
static_assert(decltype(BinaryReceiptSignature(&api::CompileSblrAccelLlvmInspectDescriptor))::value);
static_assert(decltype(BinaryReceiptSignature(&api::CompileSblrAccelLlvmInvalidateDescriptor))::value);
static_assert(decltype(BinaryReceiptSignature(&api::CompileSblrAccelLlvmPolicySetDescriptor))::value);
static_assert(decltype(BinaryReceiptSignature(&api::CompileSblrAccessCursorCloseDescriptor))::value);
static_assert(decltype(BinaryReceiptSignature(&api::CompileSblrAccessCursorFetchDescriptor))::value);
static_assert(decltype(BinaryReceiptSignature(&api::CompileSblrAccessCursorOpenDescriptor))::value);
static_assert(decltype(BinaryReceiptSignature(&api::CompileSblrAdminRegisterExternalRelationResolverDescriptor))::value);
static_assert(decltype(BinaryReceiptSignature(&api::CompileSblrAdminUnregisterExternalRelationResolverDescriptor))::value);
static_assert(decltype(BinaryReceiptSignature(&api::CompileSblrAdvancedDatatypeFamilyDescriptor))::value);
static_assert(decltype(BinaryReceiptSignature(&api::CompileSblrAdvisoryLockDescriptor))::value);
static_assert(decltype(BinaryReceiptSignature(&api::CompileSblrAdvisoryLockReleaseDescriptor))::value);
static_assert(decltype(BinaryReceiptSignature(&api::CompileSblrAggregateDescriptor))::value);
static_assert(decltype(BinaryReceiptSignature(&api::CompileSblrAggregateInvokeDescriptor))::value);
static_assert(decltype(BinaryReceiptSignature(&api::CompileSblrAtomicCasDescriptor))::value);
static_assert(decltype(BinaryReceiptSignature(&api::CompileSblrAtomicRmwDescriptor))::value);
static_assert(decltype(BinaryReceiptSignature(&api::CompileSblrBitemporalAsOfDescriptor))::value);
static_assert(decltype(BinaryReceiptSignature(&api::CompileSblrBitemporalAsOfValidTimeDescriptor))::value);
static_assert(decltype(BinaryReceiptSignature(&api::CompileSblrBitemporalForVersionsBetweenDescriptor))::value);
static_assert(decltype(BinaryReceiptSignature(&api::CompileSblrBitemporalPeriodOverlapDescriptor))::value);
static_assert(decltype(BinaryReceiptSignature(&api::CompileSblrBridgeAuthenticateDescriptor))::value);
static_assert(decltype(BinaryReceiptSignature(&api::CompileSblrBridgeBeginTransactionDescriptor))::value);
static_assert(decltype(BinaryReceiptSignature(&api::CompileSblrBridgeCloseSessionDescriptor))::value);
static_assert(decltype(BinaryReceiptSignature(&api::CompileSblrBridgeCommitTransactionDescriptor))::value);
static_assert(decltype(BinaryReceiptSignature(&api::CompileSblrBridgeDescribeCapabilitiesDescriptor))::value);
static_assert(decltype(BinaryReceiptSignature(&api::CompileSblrBridgeHealthDescriptor))::value);
static_assert(decltype(BinaryReceiptSignature(&api::CompileSblrBridgeOpenChannelDescriptor))::value);
static_assert(decltype(BinaryReceiptSignature(&api::CompileSblrBridgeOpenSessionDescriptor))::value);
static_assert(decltype(BinaryReceiptSignature(&api::CompileSblrBridgeRollbackTransactionDescriptor))::value);
static_assert(decltype(BinaryReceiptSignature(&api::CompileSblrBulkExportStreamDescriptor))::value);
static_assert(decltype(BinaryReceiptSignature(&api::CompileSblrBulkImportStreamDescriptor))::value);
static_assert(decltype(BinaryReceiptSignature(&api::CompileSblrCastDescriptor))::value);
static_assert(decltype(BinaryReceiptSignature(&api::CompileSblrClusterAlterPlacementPolicyDescriptor))::value);
static_assert(decltype(BinaryReceiptSignature(&api::CompileSblrClusterCreatePlacementPolicyDescriptor))::value);
static_assert(decltype(BinaryReceiptSignature(&api::CompileSblrClusterDeclareAvailabilityZoneDescriptor))::value);
static_assert(decltype(BinaryReceiptSignature(&api::CompileSblrClusterDeclareDataPlacementDescriptor))::value);
static_assert(decltype(BinaryReceiptSignature(&api::CompileSblrClusterDeclareRegionDescriptor))::value);
static_assert(decltype(BinaryReceiptSignature(&api::CompileSblrClusterDropPlacementPolicyDescriptor))::value);
static_assert(decltype(BinaryReceiptSignature(&api::CompileSblrCompareDescriptor))::value);
static_assert(decltype(BinaryReceiptSignature(&api::CompileSblrContextSetDescriptor))::value);
static_assert(decltype(BinaryReceiptSignature(&api::CompileSblrDatabaseCreateTemplateCloneDescriptor))::value);
static_assert(decltype(BinaryReceiptSignature(&api::CompileSblrDatabaseDeserializeLogicalSnapshotDescriptor))::value);
static_assert(decltype(BinaryReceiptSignature(&api::CompileSblrDatabaseSerializeLogicalSnapshotDescriptor))::value);
static_assert(decltype(BinaryReceiptSignature(&api::CompileSblrDdlAlterAggregateDescriptor))::value);
static_assert(decltype(BinaryReceiptSignature(&api::CompileSblrDdlAlterCollationDescriptor))::value);
static_assert(decltype(BinaryReceiptSignature(&api::CompileSblrDdlAlterContinuousViewDescriptor))::value);
static_assert(decltype(BinaryReceiptSignature(&api::CompileSblrDdlAlterDictionaryDescriptor))::value);
static_assert(decltype(BinaryReceiptSignature(&api::CompileSblrDdlAlterDomainDescriptor))::value);
static_assert(decltype(BinaryReceiptSignature(&api::CompileSblrDdlAlterEventTriggerDescriptor))::value);
static_assert(decltype(BinaryReceiptSignature(&api::CompileSblrDdlAlterExtensionDescriptor))::value);
static_assert(decltype(BinaryReceiptSignature(&api::CompileSblrDdlAlterFunctionDescriptor))::value);
static_assert(decltype(BinaryReceiptSignature(&api::CompileSblrDdlAlterOperatorFamilyDescriptor))::value);
static_assert(decltype(BinaryReceiptSignature(&api::CompileSblrDdlAlterPackageDescriptor))::value);
static_assert(decltype(BinaryReceiptSignature(&api::CompileSblrDdlAlterProcedureDescriptor))::value);
static_assert(decltype(BinaryReceiptSignature(&api::CompileSblrDdlAlterPublicationDescriptor))::value);
static_assert(decltype(BinaryReceiptSignature(&api::CompileSblrDdlAlterRewriteRuleDescriptor))::value);
static_assert(decltype(BinaryReceiptSignature(&api::CompileSblrDdlAlterSequenceDescriptor))::value);
static_assert(decltype(BinaryReceiptSignature(&api::CompileSblrDdlAlterSubscriptionDescriptor))::value);
static_assert(decltype(BinaryReceiptSignature(&api::CompileSblrDdlAlterTimeseriesValueCacheDescriptor))::value);
static_assert(decltype(BinaryReceiptSignature(&api::CompileSblrDdlAlterTriggerDescriptor))::value);
static_assert(decltype(BinaryReceiptSignature(&api::CompileSblrDdlAlterTypeDescriptor))::value);
static_assert(decltype(BinaryReceiptSignature(&api::CompileSblrDdlAlterViewDescriptor))::value);
static_assert(decltype(BinaryReceiptSignature(&api::CompileSblrDdlCreateAggregateDescriptor))::value);
static_assert(decltype(BinaryReceiptSignature(&api::CompileSblrDdlCreateCastDescriptor))::value);
static_assert(decltype(BinaryReceiptSignature(&api::CompileSblrDdlCreateCollationDescriptor))::value);
static_assert(decltype(BinaryReceiptSignature(&api::CompileSblrDdlCreateContinuousViewDescriptor))::value);
static_assert(decltype(BinaryReceiptSignature(&api::CompileSblrDdlCreateDictionaryDescriptor))::value);
static_assert(decltype(BinaryReceiptSignature(&api::CompileSblrDdlCreateDomainDescriptor))::value);
static_assert(decltype(BinaryReceiptSignature(&api::CompileSblrDdlCreateEventTriggerDescriptor))::value);
static_assert(decltype(BinaryReceiptSignature(&api::CompileSblrDdlCreateExtensionDescriptor))::value);
static_assert(decltype(BinaryReceiptSignature(&api::CompileSblrDdlCreateFdwDescriptor))::value);
static_assert(decltype(BinaryReceiptSignature(&api::CompileSblrDdlCreateForeignTableDescriptor))::value);
static_assert(decltype(BinaryReceiptSignature(&api::CompileSblrDdlCreateFunctionDescriptor))::value);
static_assert(decltype(BinaryReceiptSignature(&api::CompileSblrDdlCreateIndexDescriptor))::value);
static_assert(decltype(BinaryReceiptSignature(&api::CompileSblrDdlCreateMacroDescriptor))::value);
static_assert(decltype(BinaryReceiptSignature(&api::CompileSblrDdlCreateMaterializedViewDescriptor))::value);
static_assert(decltype(BinaryReceiptSignature(&api::CompileSblrDdlCreateNamedCollectionDescriptor))::value);
static_assert(decltype(BinaryReceiptSignature(&api::CompileSblrDdlCreateOperatorClassDescriptor))::value);
static_assert(decltype(BinaryReceiptSignature(&api::CompileSblrDdlCreateOperatorDescriptor))::value);
static_assert(decltype(BinaryReceiptSignature(&api::CompileSblrDdlCreateOperatorFamilyDescriptor))::value);
static_assert(decltype(BinaryReceiptSignature(&api::CompileSblrDdlCreateOrReplaceSrsDescriptor))::value);
static_assert(decltype(BinaryReceiptSignature(&api::CompileSblrDdlCreatePackageDescriptor))::value);
static_assert(decltype(BinaryReceiptSignature(&api::CompileSblrDdlCreateProcedureDescriptor))::value);
static_assert(decltype(BinaryReceiptSignature(&api::CompileSblrDdlCreatePublicationDescriptor))::value);
static_assert(decltype(BinaryReceiptSignature(&api::CompileSblrDdlCreateRewriteRuleDescriptor))::value);
static_assert(decltype(BinaryReceiptSignature(&api::CompileSblrDdlCreateSequenceDescriptor))::value);
static_assert(decltype(BinaryReceiptSignature(&api::CompileSblrDdlCreateSubscriptionDescriptor))::value);
static_assert(decltype(BinaryReceiptSignature(&api::CompileSblrDdlCreateSynonymDescriptor))::value);
static_assert(decltype(BinaryReceiptSignature(&api::CompileSblrDdlCreateTableAsQueryDescriptor))::value);
static_assert(decltype(BinaryReceiptSignature(&api::CompileSblrDdlCreateTableDescriptor))::value);
static_assert(decltype(BinaryReceiptSignature(&api::CompileSblrDdlCreateTemporaryTableDescriptor))::value);
static_assert(decltype(BinaryReceiptSignature(&api::CompileSblrDdlCreateTimeseriesValueCacheDescriptor))::value);
static_assert(decltype(BinaryReceiptSignature(&api::CompileSblrDdlCreateTriggerDescriptor))::value);
static_assert(decltype(BinaryReceiptSignature(&api::CompileSblrDdlCreateTypeDescriptor))::value);
static_assert(decltype(BinaryReceiptSignature(&api::CompileSblrDdlCreateViewDescriptor))::value);
static_assert(decltype(BinaryReceiptSignature(&api::CompileSblrDdlDropAggregateDescriptor))::value);
static_assert(decltype(BinaryReceiptSignature(&api::CompileSblrDdlDropCastDescriptor))::value);
static_assert(decltype(BinaryReceiptSignature(&api::CompileSblrDdlDropCollationDescriptor))::value);
static_assert(decltype(BinaryReceiptSignature(&api::CompileSblrDdlDropContinuousViewDescriptor))::value);
static_assert(decltype(BinaryReceiptSignature(&api::CompileSblrDdlDropDictionaryDescriptor))::value);
static_assert(decltype(BinaryReceiptSignature(&api::CompileSblrDdlDropEventTriggerDescriptor))::value);
static_assert(decltype(BinaryReceiptSignature(&api::CompileSblrDdlDropExtensionDescriptor))::value);
static_assert(decltype(BinaryReceiptSignature(&api::CompileSblrDdlDropFdwDescriptor))::value);
static_assert(decltype(BinaryReceiptSignature(&api::CompileSblrDdlDropForeignTableDescriptor))::value);
static_assert(decltype(BinaryReceiptSignature(&api::CompileSblrDdlDropFunctionDescriptor))::value);
static_assert(decltype(BinaryReceiptSignature(&api::CompileSblrDdlDropIndexDescriptor))::value);
static_assert(decltype(BinaryReceiptSignature(&api::CompileSblrDdlDropMacroDescriptor))::value);
static_assert(decltype(BinaryReceiptSignature(&api::CompileSblrDdlDropMaterializedViewDescriptor))::value);
static_assert(decltype(BinaryReceiptSignature(&api::CompileSblrDdlDropNamedCollectionDescriptor))::value);
static_assert(decltype(BinaryReceiptSignature(&api::CompileSblrDdlDropOperatorClassDescriptor))::value);
static_assert(decltype(BinaryReceiptSignature(&api::CompileSblrDdlDropOperatorDescriptor))::value);
static_assert(decltype(BinaryReceiptSignature(&api::CompileSblrDdlDropOperatorFamilyDescriptor))::value);
static_assert(decltype(BinaryReceiptSignature(&api::CompileSblrDdlDropPackageDescriptor))::value);
static_assert(decltype(BinaryReceiptSignature(&api::CompileSblrDdlDropProcedureDescriptor))::value);
static_assert(decltype(BinaryReceiptSignature(&api::CompileSblrDdlDropPublicationDescriptor))::value);
static_assert(decltype(BinaryReceiptSignature(&api::CompileSblrDdlDropRewriteRuleDescriptor))::value);
static_assert(decltype(BinaryReceiptSignature(&api::CompileSblrDdlDropSequenceDescriptor))::value);
static_assert(decltype(BinaryReceiptSignature(&api::CompileSblrDdlDropSrsDescriptor))::value);
static_assert(decltype(BinaryReceiptSignature(&api::CompileSblrDdlDropSubscriptionDescriptor))::value);
static_assert(decltype(BinaryReceiptSignature(&api::CompileSblrDdlDropSynonymDescriptor))::value);
static_assert(decltype(BinaryReceiptSignature(&api::CompileSblrDdlDropTableDescriptor))::value);
static_assert(decltype(BinaryReceiptSignature(&api::CompileSblrDdlDropTemporaryTableDescriptor))::value);
static_assert(decltype(BinaryReceiptSignature(&api::CompileSblrDdlDropTimeseriesValueCacheDescriptor))::value);
static_assert(decltype(BinaryReceiptSignature(&api::CompileSblrDdlDropTriggerDescriptor))::value);
static_assert(decltype(BinaryReceiptSignature(&api::CompileSblrDdlDropTypeDescriptor))::value);
static_assert(decltype(BinaryReceiptSignature(&api::CompileSblrDdlDropViewDescriptor))::value);
static_assert(decltype(BinaryReceiptSignature(&api::CompileSblrDdlPurgeSystemHistoryDescriptor))::value);
static_assert(decltype(BinaryReceiptSignature(&api::CompileSblrDdlRefreshMaterializedViewDescriptor))::value);
static_assert(decltype(BinaryReceiptSignature(&api::CompileSblrDdlRenameObjectDescriptor))::value);
static_assert(decltype(BinaryReceiptSignature(&api::CompileSblrDdlRenameObjectVectorDescriptor))::value);
static_assert(decltype(BinaryReceiptSignature(&api::CompileSblrDdlSetIndexOptimizerEligibilityDescriptor))::value);
static_assert(decltype(BinaryReceiptSignature(&api::CompileSblrDdlSetTableTypeEnforcementDescriptor))::value);
static_assert(decltype(BinaryReceiptSignature(&api::CompileSblrDdlTimeseriesSeriesCardinalityPolicyDescriptor))::value);
static_assert(decltype(BinaryReceiptSignature(&api::CompileSblrDdlValidateConstraintDescriptor))::value);
static_assert(decltype(BinaryReceiptSignature(&api::CompileSblrDeleteDescriptor))::value);
static_assert(decltype(BinaryReceiptSignature(&api::CompileSblrDmlAsyncInsertCancelDescriptor))::value);
static_assert(decltype(BinaryReceiptSignature(&api::CompileSblrDmlAsyncInsertStatusDescriptor))::value);
static_assert(decltype(BinaryReceiptSignature(&api::CompileSblrDmlAsyncInsertSubmitDescriptor))::value);
static_assert(decltype(BinaryReceiptSignature(&api::CompileSblrDmlConditionalMutateDescriptor))::value);
static_assert(decltype(BinaryReceiptSignature(&api::CompileSblrDmlCounterAddDescriptor))::value);
static_assert(decltype(BinaryReceiptSignature(&api::CompileSblrDmlTimeseriesSchemaWriteDescriptor))::value);
static_assert(decltype(BinaryReceiptSignature(&api::CompileSblrDomainOperationDescriptor))::value);
static_assert(decltype(BinaryReceiptSignature(&api::CompileSblrFunctionCallDescriptor))::value);
static_assert(decltype(BinaryReceiptSignature(&api::CompileSblrFunctionInvokeDescriptor))::value);
static_assert(decltype(BinaryReceiptSignature(&api::CompileSblrGroupDescriptor))::value);
static_assert(decltype(BinaryReceiptSignature(&api::CompileSblrInsertDescriptor))::value);
static_assert(decltype(BinaryReceiptSignature(&api::CompileSblrKvStructuredMutateDescriptor))::value);
static_assert(decltype(BinaryReceiptSignature(&api::CompileSblrKvStructuredReadDescriptor))::value);
static_assert(decltype(BinaryReceiptSignature(&api::CompileSblrKvStructuredScanDescriptor))::value);
static_assert(decltype(BinaryReceiptSignature(&api::CompileSblrKvStructuredStreamAppendDescriptor))::value);
static_assert(decltype(BinaryReceiptSignature(&api::CompileSblrKvStructuredStreamReadDescriptor))::value);
static_assert(decltype(BinaryReceiptSignature(&api::CompileSblrKvStructuredTimeseriesDescriptor))::value);
static_assert(decltype(BinaryReceiptSignature(&api::CompileSblrLimitDescriptor))::value);
static_assert(decltype(BinaryReceiptSignature(&api::CompileSblrMergeDescriptor))::value);
static_assert(decltype(BinaryReceiptSignature(&api::CompileSblrOperatorCallDescriptor))::value);
static_assert(decltype(BinaryReceiptSignature(&api::CompileSblrProcedureInvokeDescriptor))::value);
static_assert(decltype(BinaryReceiptSignature(&api::CompileSblrProjectDescriptor))::value);
static_assert(decltype(BinaryReceiptSignature(&api::CompileSblrQueryNumericDescriptor))::value);
static_assert(decltype(BinaryReceiptSignature(&api::CompileSblrReadByKeyDescriptor))::value);
static_assert(decltype(BinaryReceiptSignature(&api::CompileSblrReadRangeDescriptor))::value);
static_assert(decltype(BinaryReceiptSignature(&api::CompileSblrReadStreamDescriptor))::value);
static_assert(decltype(BinaryReceiptSignature(&api::CompileSblrResultSetPassDescriptor))::value);
static_assert(decltype(BinaryReceiptSignature(&api::CompileSblrReturnResultSetDescriptor))::value);
static_assert(decltype(BinaryReceiptSignature(&api::CompileSblrSecAlterRoleDescriptor))::value);
static_assert(decltype(BinaryReceiptSignature(&api::CompileSblrSecAlterUserDescriptor))::value);
static_assert(decltype(BinaryReceiptSignature(&api::CompileSblrSecAuthenticateDescriptor))::value);
static_assert(decltype(BinaryReceiptSignature(&api::CompileSblrSecCreateGroupMappingDescriptor))::value);
static_assert(decltype(BinaryReceiptSignature(&api::CompileSblrSecCreatePolicyDescriptor))::value);
static_assert(decltype(BinaryReceiptSignature(&api::CompileSblrSecCreateRoleDescriptor))::value);
static_assert(decltype(BinaryReceiptSignature(&api::CompileSblrSecDeauthenticateDescriptor))::value);
static_assert(decltype(BinaryReceiptSignature(&api::CompileSblrSecDropGroupMappingDescriptor))::value);
static_assert(decltype(BinaryReceiptSignature(&api::CompileSblrSecDropPolicyDescriptor))::value);
static_assert(decltype(BinaryReceiptSignature(&api::CompileSblrSecDropRoleDescriptor))::value);
static_assert(decltype(BinaryReceiptSignature(&api::CompileSblrSecDropUserDescriptor))::value);
static_assert(decltype(BinaryReceiptSignature(&api::CompileSblrSecGrantDescriptor))::value);
static_assert(decltype(BinaryReceiptSignature(&api::CompileSblrSecRevokeDescriptor))::value);
static_assert(decltype(BinaryReceiptSignature(&api::CompileSblrSecurityAlterPrivilegeTemplateDescriptor))::value);
static_assert(decltype(BinaryReceiptSignature(&api::CompileSblrSecurityCreatePrivilegeTemplateDescriptor))::value);
static_assert(decltype(BinaryReceiptSignature(&api::CompileSblrSecurityDropPrivilegeTemplateDescriptor))::value);
static_assert(decltype(BinaryReceiptSignature(&api::CompileSblrSequenceCurrvalDescriptor))::value);
static_assert(decltype(BinaryReceiptSignature(&api::CompileSblrSequenceNextvalDescriptor))::value);
static_assert(decltype(BinaryReceiptSignature(&api::CompileSblrSequenceSetvalDescriptor))::value);
static_assert(decltype(BinaryReceiptSignature(&api::CompileSblrSessionDefaultQualifierSetDescriptor))::value);
static_assert(decltype(BinaryReceiptSignature(&api::CompileSblrSessionDiscardDescriptor))::value);
static_assert(decltype(BinaryReceiptSignature(&api::CompileSblrSessionRoleSwitchDescriptor))::value);
static_assert(decltype(BinaryReceiptSignature(&api::CompileSblrSessionSettingGetDescriptor))::value);
static_assert(decltype(BinaryReceiptSignature(&api::CompileSblrSessionSettingResetDescriptor))::value);
static_assert(decltype(BinaryReceiptSignature(&api::CompileSblrSessionSettingSetDescriptor))::value);
static_assert(decltype(BinaryReceiptSignature(&api::CompileSblrSessionSnapshotHandleDescriptor))::value);
static_assert(decltype(BinaryReceiptSignature(&api::CompileSblrSortDescriptor))::value);
static_assert(decltype(BinaryReceiptSignature(&api::CompileSblrStatementBatchDescriptor))::value);
static_assert(decltype(BinaryReceiptSignature(&api::CompileSblrSystemConfigSetDescriptor))::value);
static_assert(decltype(BinaryReceiptSignature(&api::CompileSblrTableAnalyzeDescriptor))::value);
static_assert(decltype(BinaryReceiptSignature(&api::CompileSblrTableTruncateDescriptor))::value);
static_assert(decltype(BinaryReceiptSignature(&api::CompileSblrUdrInvokeDescriptor))::value);
static_assert(decltype(BinaryReceiptSignature(&api::CompileSblrUpdateDescriptor))::value);
static_assert(decltype(BinaryReceiptSignature(&api::CompileSblrVerifiableHistoryProveDescriptor))::value);
static_assert(decltype(BinaryReceiptSignature(&api::CompileSblrVersionedBranchCreateDescriptor))::value);
static_assert(decltype(BinaryReceiptSignature(&api::CompileSblrVersionedBranchDeleteDescriptor))::value);
static_assert(decltype(BinaryReceiptSignature(&api::CompileSblrVersionedDiffDescriptor))::value);
static_assert(decltype(BinaryReceiptSignature(&api::CompileSblrVersionedResetDescriptor))::value);
static_assert(decltype(BinaryReceiptSignature(&api::CompileSblrVersionedRevertDescriptor))::value);
static_assert(decltype(BinaryReceiptSignature(&api::CompileSblrVersionedStatusReadDescriptor))::value);
static_assert(decltype(BinaryReceiptSignature(&api::CompileSblrVersionedTagDescriptor))::value);
static_assert(decltype(BinaryReceiptSignature(&api::CompileSblrWindowDescriptor))::value);

int main() {
  std::size_t checks = 0;
  const auto check = [&](bool condition) {
    ++checks;
    if (!condition) {
      std::fprintf(stderr, "server coordination UUID check %zu failed\n", checks);
      std::abort();
    }
  };
  Uuid base{{0x01,0x99,0x65,0xab,0xcd,0xef,0x70,0x00,
             0x80,0x00,0x00,0x00,0x00,0x00,0x00,0x01}};
  std::array<std::uint8_t, 16> sentinel{};
  sentinel.fill(0xa5);
  for (std::size_t position = 0; position != 16; ++position) {
    for (unsigned octet = 0; octet != 256; ++octet) {
      auto identity = base;
      identity.bytes[position] = static_cast<std::uint8_t>(octet);
      // Independent wire-shape oracle, not the production UUID validator.
      const bool valid = (identity.bytes[6] >> 4) == 7 &&
                         (identity.bytes[8] & 0xc0) == 0x80;
      for (const bool optional : {false, true}) {
        auto output = sentinel;
        check(server::CopyCoordinationSystemUuid(identity, &output, optional) == valid);
        check(output == (valid ? identity.bytes : sentinel));
        check(!server::CopyCoordinationSystemUuid(identity, nullptr, optional));
      }
    }
  }
  auto output = sentinel;
  check(!server::CopyCoordinationSystemUuid({}, &output));
  check(output == sentinel);
  check(server::CopyCoordinationSystemUuid({}, &output, true));
  check(output == std::array<std::uint8_t, 16>{});

  // All 128 identity bits participate in lookup; embedded NUL and high bytes
  // must not truncate or collide. This exercises the actual registry key type.
  CoordinationMap map;
  server::ServerParameterExecutionCoordinationRecord record;
  record.session_uuid = base;
  record.operation_uuid = base;
  record.private_handle = 71;
  record.generation = 9;
  check(map.emplace(base, record).second);
  for (std::size_t bit = 0; bit != 128; ++bit) {
    auto other = base;
    other.bytes[bit / 8] ^= static_cast<std::uint8_t>(1u << (bit % 8));
    check(map.find(other) == map.end());
    auto distinct = record;
    distinct.private_handle = 100 + bit;
    check(map.emplace(other, distinct).second);
    check(map.at(other).private_handle == 100 + bit);
    check(map.at(base).private_handle == 71);
    check(map.at(other).session_uuid.bytes == base.bytes);
  }
  check(map.size() == 129);
  check(!map.emplace(base, record).second);
  for (auto previous = map.begin(), current = std::next(previous);
       current != map.end(); ++previous, ++current) {
    check(std::lexicographical_compare(previous->first.bytes.begin(), previous->first.bytes.end(),
                                       current->first.bytes.begin(), current->first.bytes.end()));
  }
  // Actual production receipt projection and binary key isolation only.
  // This coordinator does not prove execution of a SQL session setting.
  api::EngineRequestContext context;
  context.security_context_present = true;
  for (unsigned version = 0; version != 16; ++version) {
    for (unsigned variant = 0; variant != 4; ++variant) {
      if (version == 7 && variant == 2) continue;
      auto invalid = base;
      invalid.bytes[6] = static_cast<std::uint8_t>(version << 4);
      invalid.bytes[8] = static_cast<std::uint8_t>(variant << 6);
      context.statement_uuid = invalid;
      check(!api::CompileSblrSessionSettingSetDescriptor(context, invalid, 1, 1).ok);
    }
  }
  context.statement_uuid = {};
  check(!api::CompileSblrSessionSettingSetDescriptor(context, {}, 1, 1).ok);
  context.statement_uuid = base;
  auto different = base;
  different.bytes[15] ^= 0xff;
  check(!api::CompileSblrSessionSettingSetDescriptor(context, different, 1, 1).ok);
  auto first = api::CompileSblrSessionSettingSetDescriptor(context, base, 1, 1);
  check(first.ok);
  check(first.descriptor.receipt == base.bytes);
  context.statement_uuid = different;
  check(!api::ConsumeSblrSessionSettingSetDescriptor(context, first.descriptor).ok);
  auto second = api::CompileSblrSessionSettingSetDescriptor(context, different, 1, 1);
  check(second.ok);
  check(second.descriptor.receipt == different.bytes);
  check(api::ConsumeSblrSessionSettingSetDescriptor(context, second.descriptor).ok);
  check(!api::ConsumeSblrSessionSettingSetDescriptor(context, second.descriptor).ok);
  context.statement_uuid = base;
  check(api::ConsumeSblrSessionSettingSetDescriptor(context, first.descriptor).ok);
  check(!api::ConsumeSblrSessionSettingSetDescriptor(context, first.descriptor).ok);
  std::printf("server coordination binary UUID component: PASS %zu checks; not live IPC acceptance\n", checks);
}
