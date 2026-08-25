// Copyright (c) 2026 ScratchBird Software Inc.
//
// This Source Code Form is subject to the terms of the Mozilla Public
// License, v. 2.0. If a copy of the MPL was not distributed with this
// file, You can obtain one at https://mozilla.org/MPL/2.0/.
//
// SPDX-License-Identifier: MPL-2.0

// SEARCH_KEY: SB_SERVER_AUTH_SESSION_ATTACH

#include "session_registry.hpp"

#include "config_policy_security_lifecycle.hpp"
#include "engine_host.hpp"
#include "sblr_parameter_runtime.hpp"
#include "sblr_error_vector_runtime.hpp"
#include "sblr_source_map_runtime.hpp"
#include "sblr_variable_runtime.hpp"
#include "sblr_savepoint_runtime.hpp"
#include "sblr_autonomous_frame_runtime.hpp"
#include "sblr_savepoint_coordinator.hpp"
#include "sblr_autonomous_frame_coordinator.hpp"
#include "sblr_reservation_release_runtime.hpp"
#include "sblr_reservation_release_coordinator.hpp"
#include "sblr_temporary_instance_cleanup_runtime.hpp"
#include "sblr_temporary_instance_cleanup_coordinator.hpp"
#include "sblr_cursor_open_runtime.hpp"
#include "sblr_cursor_open_coordinator.hpp"
#include "sblr_read_by_key_runtime.hpp"
#include "sblr_read_by_key_coordinator.hpp"
#include "sblr_read_range_runtime.hpp"
#include "sblr_read_range_coordinator.hpp"
#include "sblr_read_stream_runtime.hpp"
#include "sblr_read_stream_coordinator.hpp"
#include "sblr_window_runtime.hpp"
#include "sblr_window_coordinator.hpp"
#include "sblr_return_result_set_runtime.hpp"
#include "sblr_return_result_set_coordinator.hpp"
#include "sblr_kv_structured_read_runtime.hpp"
#include "sblr_kv_structured_read_coordinator.hpp"
#include "sblr_kv_structured_mutate_runtime.hpp"
#include "sblr_kv_structured_mutate_coordinator.hpp"
#include "sblr_kv_structured_scan_runtime.hpp"
#include "sblr_kv_structured_scan_coordinator.hpp"
#include "sblr_kv_structured_stream_read_runtime.hpp"
#include "sblr_kv_structured_stream_read_coordinator.hpp"
#include "sblr_kv_structured_stream_append_runtime.hpp"
#include "sblr_kv_structured_stream_append_coordinator.hpp"
#include "sblr_kv_structured_timeseries_runtime.hpp"
#include "sblr_kv_structured_timeseries_coordinator.hpp"
#include "sblr_system_config_set_runtime.hpp"
#include "sblr_system_config_set_coordinator.hpp"
#include "sblr_ddl_create_domain_runtime.hpp"
#include "sblr_ddl_create_type_runtime.hpp"
#include "sblr_ddl_alter_type_runtime.hpp"
#include "sblr_ddl_drop_type_runtime.hpp"
#include "sblr_ddl_alter_domain_runtime.hpp"
#include "sblr_ddl_create_view_runtime.hpp"
#include "sblr_ddl_drop_view_runtime.hpp"
#include "sblr_ddl_refresh_materialized_view_runtime.hpp"
#include "sblr_ddl_drop_materialized_view_runtime.hpp"
#include "sblr_ddl_drop_table_runtime.hpp"
#include "sblr_ddl_drop_sequence_runtime.hpp"
#include "sblr_ddl_create_trigger_runtime.hpp"
#include "sblr_ddl_alter_trigger_runtime.hpp"
#include "sblr_ddl_drop_trigger_runtime.hpp"
#include "sblr_ddl_create_procedure_runtime.hpp"
#include "sblr_ddl_alter_procedure_runtime.hpp"
#include "sblr_ddl_drop_procedure_runtime.hpp"
#include "sblr_ddl_create_function_runtime.hpp"
#include "sblr_ddl_create_view_coordinator.hpp"
#include "sblr_ddl_create_type_coordinator.hpp"
#include "sblr_ddl_alter_type_coordinator.hpp"
#include "sblr_ddl_drop_type_coordinator.hpp"
#include "sblr_ddl_drop_view_coordinator.hpp"
#include "sblr_ddl_refresh_materialized_view_coordinator.hpp"
#include "sblr_ddl_drop_materialized_view_coordinator.hpp"
#include "sblr_ddl_drop_table_coordinator.hpp"
#include "sblr_ddl_drop_sequence_coordinator.hpp"
#include "sblr_ddl_create_trigger_coordinator.hpp"
#include "sblr_ddl_alter_trigger_coordinator.hpp"
#include "sblr_ddl_drop_trigger_coordinator.hpp"
#include "sblr_ddl_create_procedure_coordinator.hpp"
#include "sblr_ddl_alter_procedure_coordinator.hpp"
#include "sblr_ddl_drop_procedure_coordinator.hpp"
#include "sblr_ddl_create_function_coordinator.hpp"
#include "sblr_ddl_alter_function_runtime.hpp"
#include "sblr_ddl_alter_function_coordinator.hpp"
#include "sblr_ddl_drop_function_runtime.hpp"
#include "sblr_ddl_drop_function_coordinator.hpp"
#include "sblr_ddl_create_package_runtime.hpp"
#include "sblr_ddl_create_synonym_runtime.hpp"
#include "sblr_ddl_create_foreign_table_runtime.hpp"
#include "sblr_ddl_create_foreign_table_coordinator.hpp"
#include "sblr_ddl_create_fdw_runtime.hpp"
#include "sblr_ddl_create_fdw_coordinator.hpp"
#include "sblr_ddl_drop_fdw_runtime.hpp"
#include "sblr_ddl_drop_fdw_coordinator.hpp"
#include "sblr_sec_alter_user_runtime.hpp"
#include "sblr_sec_alter_user_coordinator.hpp"
#include "sblr_ddl_drop_foreign_table_runtime.hpp"
#include "sblr_ddl_drop_foreign_table_coordinator.hpp"
#include "sblr_ddl_drop_package_runtime.hpp"
#include "sblr_ddl_drop_synonym_runtime.hpp"
#include "sblr_ddl_create_package_coordinator.hpp"
#include "sblr_ddl_create_synonym_coordinator.hpp"
#include "sblr_ddl_drop_package_coordinator.hpp"
#include "sblr_ddl_alter_package_runtime.hpp"
#include "sblr_ddl_alter_package_coordinator.hpp"
#include "sblr_ddl_alter_sequence_runtime.hpp"
#include "sblr_ddl_alter_sequence_coordinator.hpp"
#include "sblr_ddl_create_temporary_table_runtime.hpp"
#include "sblr_ddl_create_temporary_table_coordinator.hpp"
#include "sblr_ddl_drop_temporary_table_runtime.hpp"
#include "sblr_ddl_drop_temporary_table_coordinator.hpp"
#include "sblr_ddl_rename_object_vector_runtime.hpp"
#include "sblr_ddl_rename_object_vector_coordinator.hpp"
#include "sblr_ddl_rename_object_runtime.hpp"
#include "sblr_ddl_rename_object_coordinator.hpp"
#include "sblr_ddl_create_or_replace_srs_runtime.hpp"
#include "sblr_ddl_create_or_replace_srs_coordinator.hpp"
#include "sblr_ddl_drop_srs_runtime.hpp"
#include "sblr_ddl_drop_srs_coordinator.hpp"
#include "sblr_ddl_create_rewrite_rule_runtime.hpp"
#include "sblr_ddl_create_rewrite_rule_coordinator.hpp"
#include "sblr_ddl_alter_rewrite_rule_runtime.hpp"
#include "sblr_ddl_alter_rewrite_rule_coordinator.hpp"
#include "sblr_ddl_drop_rewrite_rule_runtime.hpp"
#include "sblr_ddl_drop_rewrite_rule_coordinator.hpp"
#include "sblr_ddl_validate_constraint_runtime.hpp"
#include "sblr_security_create_privilege_template_runtime.hpp"
#include "sblr_security_create_user_runtime.hpp"
#include "sblr_security_alter_privilege_template_runtime.hpp"
#include "sblr_security_alter_privilege_template_coordinator.hpp"
#include "sblr_security_drop_privilege_template_runtime.hpp"
#include "sblr_security_drop_privilege_template_coordinator.hpp"
#include "sblr_database_create_template_clone_runtime.hpp"
#include "sblr_database_create_template_clone_coordinator.hpp"
#include "sblr_ddl_create_aggregate_runtime.hpp"
#include "sblr_ddl_create_aggregate_coordinator.hpp"
#include "sblr_ddl_alter_aggregate_runtime.hpp"
#include "sblr_ddl_alter_aggregate_coordinator.hpp"
#include "sblr_ddl_drop_aggregate_runtime.hpp"
#include "sblr_ddl_drop_aggregate_coordinator.hpp"
#include "sblr_ddl_purge_system_history_runtime.hpp"
#include "sblr_ddl_purge_system_history_coordinator.hpp"
#include "sblr_ddl_set_index_optimizer_eligibility_runtime.hpp"
#include "sblr_ddl_set_index_optimizer_eligibility_coordinator.hpp"
#include "sblr_ddl_set_table_type_enforcement_runtime.hpp"
#include "sblr_ddl_set_table_type_enforcement_coordinator.hpp"
#include "sblr_database_serialize_logical_snapshot_runtime.hpp"
#include "sblr_database_serialize_logical_snapshot_coordinator.hpp"
#include "sblr_database_deserialize_logical_snapshot_runtime.hpp"
#include "sblr_ddl_create_macro_runtime.hpp"
#include "sblr_ddl_create_macro_coordinator.hpp"
#include "sblr_ddl_drop_macro_runtime.hpp"
#include "sblr_ddl_drop_macro_coordinator.hpp"
#include "sblr_ddl_create_dictionary_runtime.hpp"
#include "sblr_ddl_create_dictionary_coordinator.hpp"
#include "sblr_ddl_drop_dictionary_runtime.hpp"
#include "sblr_ddl_drop_dictionary_coordinator.hpp"
#include "sblr_ddl_alter_dictionary_runtime.hpp"
#include "sblr_ddl_create_continuous_view_runtime.hpp"
#include "sblr_ddl_create_continuous_view_coordinator.hpp"
#include "sblr_ddl_alter_continuous_view_runtime.hpp"
#include "sblr_ddl_alter_continuous_view_coordinator.hpp"
#include "sblr_ddl_drop_continuous_view_runtime.hpp"
#include "sblr_ddl_drop_continuous_view_coordinator.hpp"
#include "sblr_dml_async_insert_submit_runtime.hpp"
#include "sblr_dml_async_insert_status_runtime.hpp"
#include "sblr_dml_async_insert_cancel_runtime.hpp"
#include "sblr_dml_async_insert_status_coordinator.hpp"
#include "sblr_dml_async_insert_cancel_coordinator.hpp"
#include "sblr_dml_async_insert_submit_coordinator.hpp"
#include "sblr_dml_counter_add_runtime.hpp"
#include "sblr_dml_counter_add_coordinator.hpp"
#include "sblr_dml_timeseries_schema_write_runtime.hpp"
#include "sblr_dml_timeseries_schema_write_coordinator.hpp"
#include "sblr_ddl_timeseries_series_cardinality_policy_runtime.hpp"
#include "sblr_ddl_timeseries_series_cardinality_policy_coordinator.hpp"
#include "sblr_ddl_create_timeseries_value_cache_runtime.hpp"
#include "sblr_ddl_create_timeseries_value_cache_coordinator.hpp"
#include "sblr_ddl_alter_dictionary_coordinator.hpp"
#include "sblr_admin_register_external_relation_resolver_runtime.hpp"
#include "sblr_admin_register_external_relation_resolver_coordinator.hpp"
#include "sblr_admin_unregister_external_relation_resolver_runtime.hpp"
#include "sblr_admin_unregister_external_relation_resolver_coordinator.hpp"
#include "sblr_database_deserialize_logical_snapshot_coordinator.hpp"
#include "sblr_security_create_privilege_template_coordinator.hpp"
#include "sblr_security_create_user_coordinator.hpp"
#include "sblr_sec_create_role_coordinator.hpp"
#include "sblr_sec_create_role_runtime.hpp"
#include "sblr_sec_create_policy_runtime.hpp"
#include "sblr_sec_drop_policy_runtime.hpp"
#include "sblr_sec_create_policy_coordinator.hpp"
#include "sblr_sec_drop_policy_coordinator.hpp"
#include "sblr_sec_drop_role_coordinator.hpp"
#include "sblr_sec_drop_role_runtime.hpp"
#include "sblr_sec_alter_role_coordinator.hpp"
#include "sblr_sec_alter_role_runtime.hpp"
#include "sblr_ddl_validate_constraint_coordinator.hpp"
#include "sblr_ddl_alter_view_runtime.hpp"
#include "sblr_ddl_alter_view_coordinator.hpp"
#include "sblr_ddl_alter_domain_coordinator.hpp"
#include "sblr_ddl_create_schema_runtime.hpp"
#include "sblr_ddl_create_table_runtime.hpp"
#include "sblr_ddl_create_table_as_query_runtime.hpp"
#include "sblr_ddl_create_table_as_query_coordinator.hpp"
#include "sblr_ddl_create_index_runtime.hpp"
#include "engine/sblr/sblr_ddl_drop_index_runtime.hpp"
#include "sblr_ddl_create_domain_coordinator.hpp"
#include "sblr_ddl_create_schema_coordinator.hpp"
#include "sblr_ddl_create_table_coordinator.hpp"
#include "sblr_ddl_create_index_coordinator.hpp"
#include "engine/internal_api/sblr_ddl_drop_index_coordinator.hpp"
#include "sblr_result_set_pass_runtime.hpp"
#include "sblr_result_set_pass_coordinator.hpp"
#include "sblr_access_cursor_open_runtime.hpp"
#include "sblr_access_cursor_fetch_runtime.hpp"
#include "sblr_access_cursor_close_runtime.hpp"
#include "sblr_access_cursor_open_coordinator.hpp"
#include "sblr_insert_runtime.hpp"
#include "sblr_insert_coordinator.hpp"
#include "sblr_update_runtime.hpp"
#include "sblr_update_coordinator.hpp"
#include "sblr_delete_runtime.hpp"
#include "sblr_delete_coordinator.hpp"
#include "sblr_merge_runtime.hpp"
#include "sblr_merge_coordinator.hpp"
#include "sblr_table_truncate_runtime.hpp"
#include "sblr_table_truncate_coordinator.hpp"
#include "sblr_table_analyze_runtime.hpp"
#include "sblr_table_analyze_coordinator.hpp"
#include "sblr_bulk_import_stream_runtime.hpp"
#include "sblr_bulk_import_stream_coordinator.hpp"
#include "sblr_bulk_export_stream_runtime.hpp"
#include "sblr_bulk_export_stream_coordinator.hpp"
#include "sblr_statement_batch_runtime.hpp"
#include "sblr_statement_batch_coordinator.hpp"
#include "sblr_atomic_cas_runtime.hpp"
#include "sblr_atomic_cas_coordinator.hpp"
#include "sblr_atomic_read_modify_write_runtime.hpp"
#include "sblr_atomic_rmw_coordinator.hpp"
#include "sblr_advisory_lock_runtime.hpp"
#include "sblr_advisory_lock_coordinator.hpp"
#include "sblr_advisory_lock_release_runtime.hpp"
#include "sblr_advisory_lock_release_coordinator.hpp"
#include "sblr_function_call_runtime.hpp"
#include "sblr_function_call_coordinator.hpp"
#include "sblr_operator_call_runtime.hpp"
#include "sblr_operator_call_coordinator.hpp"
#include "sblr_cast_runtime.hpp"
#include "sblr_cast_coordinator.hpp"
#include "sblr_compare_runtime.hpp"
#include "sblr_compare_coordinator.hpp"
#include "sblr_domain_operation_runtime.hpp"
#include "sblr_domain_operation_coordinator.hpp"
#include "sblr_udr_invoke_runtime.hpp"
#include "sblr_udr_invoke_coordinator.hpp"
#include "sblr_procedure_invoke_runtime.hpp"
#include "sblr_procedure_invoke_coordinator.hpp"
#include "sblr_function_invoke_runtime.hpp"
#include "sblr_function_invoke_coordinator.hpp"
#include "sblr_aggregate_invoke_runtime.hpp"
#include "sblr_aggregate_invoke_coordinator.hpp"
#include "sblr_sequence_nextval_runtime.hpp"
#include "sblr_sequence_nextval_coordinator.hpp"
#include "sblr_sequence_currval_runtime.hpp"
#include "sblr_sequence_currval_coordinator.hpp"
#include "sblr_sequence_setval_runtime.hpp"
#include "sblr_sequence_setval_coordinator.hpp"
#include "sblr_query_numeric_runtime.hpp"
#include "sblr_query_numeric_coordinator.hpp"
#include "sblr_advanced_datatype_family_runtime.hpp"
#include "sblr_advanced_datatype_family_coordinator.hpp"
#include "sblr_project_runtime.hpp"
#include "sblr_catalog_introspect_runtime.hpp"
#include "sblr_catalog_introspect_coordinator.hpp"
#include "sblr_project_coordinator.hpp"
#include "sblr_aggregate_runtime.hpp"
#include "sblr_aggregate_coordinator.hpp"
#include "sblr_group_runtime.hpp"
#include "sblr_group_coordinator.hpp"
#include "sblr_sec_create_group_mapping_runtime.hpp"
#include "sblr_sec_create_group_mapping_coordinator.hpp"
#include "sblr_sec_drop_group_mapping_runtime.hpp"
#include "sblr_sec_drop_group_mapping_coordinator.hpp"
#include "sblr_sec_grant_runtime.hpp"
#include "sblr_sec_grant_coordinator.hpp"
#include "sblr_sec_revoke_runtime.hpp"
#include "sblr_sec_revoke_coordinator.hpp"
#include "sblr_sec_alter_policy_runtime.hpp"
#include "sblr_sec_alter_policy_coordinator.hpp"
#include "sblr_sec_drop_user_runtime.hpp"
#include "sblr_sec_drop_user_coordinator.hpp"
#include "sblr_sec_authenticate_runtime.hpp"
#include "sblr_sec_authenticate_coordinator.hpp"
#include "sblr_sec_deauthenticate_runtime.hpp"
#include "sblr_sec_deauthenticate_coordinator.hpp"
#include "sblr_session_role_switch_runtime.hpp"
#include "sblr_session_setting_set_runtime.hpp"
#include "sblr_session_setting_set_coordinator.hpp"
#include "sblr_session_role_switch_coordinator.hpp"
#include "sblr_sort_runtime.hpp"
#include "sblr_sort_coordinator.hpp"
#include "sblr_limit_runtime.hpp"
#include "sblr_limit_coordinator.hpp"
#include "sblr_executor_availability_registry.hpp"
#include "sblr_variable_frame_coordinator.hpp"
#include "security/authentication_api.hpp"
#include "security/authorization_api.hpp"
#include "security/security_crypto_policy.hpp"
#include "security/security_model.hpp"
#include "security/security_principal_lifecycle.hpp"
#include "transaction/transaction_api.hpp"

#include <algorithm>
#include <chrono>
#include <cctype>
#include <cstring>
#include <ctime>
#include <iomanip>
#include <map>
#include <set>
#include <sstream>
#include <string_view>

namespace scratchbird::server {
static std::string UuidBytesToText(const std::uint8_t* begin, const std::uint8_t* end) { std::array<std::uint8_t,16> value{}; if (end-begin==16) std::copy(begin,end,value.begin()); return UuidBytesToText(value); }

namespace engine_api = scratchbird::engine::internal_api;
namespace engine_bridge = scratchbird::server_engine_bridge;

namespace {
engine_api::EngineRequestContext EngineContextForSession(const ServerSessionRecord&,const HostedEngineState&,const sbps::Frame&);

constexpr std::string_view kDefaultLanguageTag = "en";
constexpr std::string_view kDefaultLanguageProfileId = "sbsql.builtin.recovery.en";
constexpr std::string_view kDefaultInputSyntaxProfile = "sbsql.syntax.standard";
constexpr std::string_view kDefaultCommonResourceHash = "builtin.common.sbsql.v1";
constexpr std::string_view kDefaultResourceCompatibilityIdentity =
    "sbsql.resource.compat.v1";
constexpr std::string_view kDefaultResourceVersionIdentity =
    "sbsql.resource-pack.v1";

bool LooksLikeLanguageTag(std::string_view value) {
  if (value.empty()) return false;
  if (value.find('.') != std::string_view::npos) return false;
  if (value.find(':') != std::string_view::npos) return false;
  if (value.size() == 36 && value[8] == '-' && value[13] == '-' &&
      value[18] == '-' && value[23] == '-') {
    return false;
  }
  return true;
}

#define HandleCoordinateLimit HandleCoordinateLimitInternal
SessionOperationResult HandleCoordinateLimit(ServerSessionRegistry*registry,const HostedEngineState&engine_state,const sbps::Frame&request){SessionOperationResult result;result.response_message_type=157;result.response_schema_id=sbps::kSchemaCoordinateLimitResultV1;result.frame_flags=sbps::kFlagResponse|sbps::kFlagFinal;result.session_uuid=request.header.session_uuid;auto refuse=[&](std::string c,std::string d){result.frame_flags|=sbps::kFlagError;result.diagnostics.push_back(sbps::IpcDiagnostic(std::move(c),"parser_server_ipc.limit_refused","LIMIT coordination was refused.",{{"detail",std::move(d)}}));return result;};scratchbird::engine::sblr::SblrLimitRequestV1 v;std::string d;if(!registry||!scratchbird::engine::sblr::DecodeSblrLimitRequestV1(request.payload.data(),request.payload.size(),&v,&d))return refuse("SBLR.OPERAND_INVALID",d);auto s=registry->sessions_by_uuid.find(UuidBytesToText(request.header.session_uuid));if(s==registry->sessions_by_uuid.end())return refuse("SECURITY.ACCESS_DENIED","session_hidden");auto ru=UuidBytesToText(v.receipt);ServerStatementContextRecord*r=nullptr;{std::lock_guard<std::mutex>g(*registry->statement_context_mutex);for(auto&[x,row]:registry->statement_contexts_by_statement_uuid){(void)x;if(!row.released&&row.view.receipt_uuid==ru){r=&row;break;}}}if(!r||r->session_uuid!=request.header.session_uuid)return refuse("SECURITY.ACCESS_DENIED","limit_receipt_hidden");auto c=EngineContextForSession(s->second,engine_state,request);c.statement_uuid.canonical=ru;c.statement_metadata_snapshot_engine_owned=true;c.trace_tags.push_back("private_limit_binder");auto q=engine_api::CompileSblrLimitDescriptor(c,ru,v.occurrence,v.limit_occurrence,r->view.limit_executor_availability_generation);if(!q.ok)return refuse(q.diagnostic.code,q.diagnostic.message_key);result.payload=scratchbird::engine::sblr::EncodeSblrLimitDescriptorV1(q.descriptor,false);if(result.payload.empty())return refuse("LIMIT.EXECUTION_FAILED","LIDD_encode_failed");result.accepted=true;return result;}

#undef HandleCoordinateLimit
std::string NormalizeLanguageTag(std::string_view value) {
  return value.empty() ? std::string(kDefaultLanguageTag) : std::string(value);
}

std::string BuiltinLanguageProfileIdForTag(std::string_view language_tag) {
  const std::string tag = NormalizeLanguageTag(language_tag);
  if (tag == kDefaultLanguageTag) return std::string(kDefaultLanguageProfileId);
  return "sbsql.language-profile." + tag;
}

}  // namespace

ServerLanguageContextIdentity ServerLanguageContextForSession(
    const ServerSessionRecord& session) {
  ServerLanguageContextIdentity identity;
  identity.language_tag =
      !session.language_tag.empty()
          ? session.language_tag
          : (LooksLikeLanguageTag(session.language_profile)
                 ? NormalizeLanguageTag(session.language_profile)
                 : std::string(kDefaultLanguageTag));
  identity.language_profile_id =
      session.language_profile.empty() || LooksLikeLanguageTag(session.language_profile)
          ? BuiltinLanguageProfileIdForTag(identity.language_tag)
          : session.language_profile;
  identity.default_language_tag =
      session.default_language_tag.empty() ? std::string(kDefaultLanguageTag)
                                           : session.default_language_tag;
  identity.input_syntax_profile =
      session.input_syntax_profile.empty() ? std::string(kDefaultInputSyntaxProfile)
                                           : session.input_syntax_profile;
  identity.input_language_fallback_tag = session.input_language_fallback_tag;
  identity.common_resource_hash =
      session.common_resource_hash.empty() ? std::string(kDefaultCommonResourceHash)
                                           : session.common_resource_hash;
  identity.language_resource_epoch =
      session.language_resource_epoch == 0 ? session.resource_epoch
                                           : session.language_resource_epoch;
  identity.localized_name_epoch =
      session.localized_name_epoch == 0 ? session.name_resolution_epoch
                                        : session.localized_name_epoch;
  identity.message_resource_epoch = session.message_resource_epoch;
  identity.resource_compatibility_identity =
      session.resource_compatibility_identity.empty()
          ? std::string(kDefaultResourceCompatibilityIdentity)
          : session.resource_compatibility_identity;
  identity.resource_version_identity =
      session.resource_version_identity.empty()
          ? std::string(kDefaultResourceVersionIdentity)
          : session.resource_version_identity;
  return identity;
}

void ApplyRequestedLanguageProfile(ServerSessionRecord* session,
                                   std::string_view requested_language_tag) {
  if (session == nullptr) return;
  const std::string tag = NormalizeLanguageTag(requested_language_tag);
  session->language_tag = tag;
  session->default_language_tag = std::string(kDefaultLanguageTag);
  session->language_profile = BuiltinLanguageProfileIdForTag(tag);
  session->input_syntax_profile = std::string(kDefaultInputSyntaxProfile);
  session->input_language_fallback_tag =
      tag == kDefaultLanguageTag ? std::string{} : std::string(kDefaultLanguageTag);
  if (session->common_resource_hash.empty()) {
    session->common_resource_hash = std::string(kDefaultCommonResourceHash);
  }
  if (session->language_resource_epoch == 0) session->language_resource_epoch = 1;
  if (session->localized_name_epoch == 0) session->localized_name_epoch = 1;
  if (session->message_resource_epoch == 0) session->message_resource_epoch = 1;
  if (session->resource_compatibility_identity.empty()) {
    session->resource_compatibility_identity =
        std::string(kDefaultResourceCompatibilityIdentity);
  }
  if (session->resource_version_identity.empty()) {
    session->resource_version_identity = std::string(kDefaultResourceVersionIdentity);
  }
}

void PopulateEngineLanguageContextFromSession(
    const ServerSessionRecord& session,
    engine_api::EngineLanguageContext* context) {
  if (context == nullptr) return;
  const auto identity = ServerLanguageContextForSession(session);
  context->language_profile_id = identity.language_profile_id;
  context->language_tag = identity.language_tag;
  context->default_language_tag = identity.default_language_tag;
  context->input_syntax_profile = identity.input_syntax_profile;
  context->input_language_fallback_tag = identity.input_language_fallback_tag;
  context->common_resource_hash = identity.common_resource_hash;
  context->language_resource_epoch = identity.language_resource_epoch;
  context->localized_name_epoch = identity.localized_name_epoch;
  context->message_resource_epoch = identity.message_resource_epoch;
  context->resource_compatibility_identity =
      identity.resource_compatibility_identity;
  context->resource_version_identity = identity.resource_version_identity;
}

namespace {

constexpr std::uint32_t kSchemaAuthResultTestV1 = 3002;
constexpr std::uint32_t kSchemaAttachResultTestV1 = 3004;
constexpr std::uint32_t kSchemaDisconnectResultTestV1 = 3005;

std::string CurrentUtcTimestampText() {
  const auto now = std::chrono::system_clock::now();
  const auto time = std::chrono::system_clock::to_time_t(now);
  std::tm tm{};
#if defined(_WIN32)
  gmtime_s(&tm, &time);
#else
  gmtime_r(&time, &tm);
#endif
  std::ostringstream out;
  out << std::put_time(&tm, "%Y-%m-%dT%H:%M:%SZ");
  return out.str();
}

std::string CurrentMonotonicNsText() {
  return std::to_string(std::chrono::duration_cast<std::chrono::nanoseconds>(
                            std::chrono::steady_clock::now().time_since_epoch())
                            .count());
}

void PutU16(std::vector<std::uint8_t>* out, std::uint16_t value) {
  out->push_back(static_cast<std::uint8_t>(value & 0xffu));
  out->push_back(static_cast<std::uint8_t>((value >> 8u) & 0xffu));
}

void PutU32(std::vector<std::uint8_t>* out, std::uint32_t value) {
  for (int shift = 0; shift < 32; shift += 8) {
    out->push_back(static_cast<std::uint8_t>((value >> shift) & 0xffu));
  }
}

void PutU64(std::vector<std::uint8_t>* out, std::uint64_t value) {
  for (int shift = 0; shift < 64; shift += 8) {
    out->push_back(static_cast<std::uint8_t>((value >> shift) & 0xffu));
  }
}

std::uint64_t GetU64(const std::vector<std::uint8_t>& data,
                     std::size_t offset) {
  std::uint64_t value = 0;
  for (int shift = 0; shift < 64; shift += 8) {
    value |= static_cast<std::uint64_t>(data[offset++]) << shift;
  }
  return value;
}

std::uint16_t GetU16(const std::vector<std::uint8_t>& data, std::size_t offset) {
  return static_cast<std::uint16_t>(data[offset]) |
         static_cast<std::uint16_t>(static_cast<std::uint16_t>(data[offset + 1]) << 8u);
}

void PutUuid(std::vector<std::uint8_t>* out, const std::array<std::uint8_t, 16>& uuid) {
  out->insert(out->end(), uuid.begin(), uuid.end());
}

std::array<std::uint8_t, 16> GetUuid(const std::vector<std::uint8_t>& data, std::size_t offset) {
  std::array<std::uint8_t, 16> uuid{};
  std::memcpy(uuid.data(), data.data() + offset, uuid.size());
  return uuid;
}

void PutString(std::vector<std::uint8_t>* out, const std::string& value) {
  PutU16(out, static_cast<std::uint16_t>(value.size()));
  out->insert(out->end(), value.begin(), value.end());
}

bool IsCanonicalStatementTimestamp(std::string_view value) {
  if (value.size() != 20 &&
      (value.size() < 22 || value.size() > 30)) {
    return false;
  }
  if (value[4] != '-' || value[7] != '-' || value[10] != 'T' ||
      value[13] != ':' || value[16] != ':' || value.back() != 'Z') {
    return false;
  }
  constexpr std::size_t kDigitIndexes[] = {
      0, 1, 2, 3, 5, 6, 8, 9, 11, 12, 14, 15, 17, 18};
  for (const auto index : kDigitIndexes) {
    if (value[index] < '0' || value[index] > '9') return false;
  }
  if (value.size() > 20) {
    if (value[19] != '.') return false;
    for (std::size_t index = 20; index + 1 < value.size(); ++index) {
      if (value[index] < '0' || value[index] > '9') return false;
    }
  }
  const auto decimal = [&](std::size_t offset, std::size_t digits) {
    unsigned result = 0;
    for (std::size_t index = 0; index < digits; ++index) {
      result = result * 10 +
               static_cast<unsigned>(value[offset + index] - '0');
    }
    return result;
  };
  const auto year = decimal(0, 4);
  const auto month = decimal(5, 2);
  const auto day = decimal(8, 2);
  const auto hour = decimal(11, 2);
  const auto minute = decimal(14, 2);
  const auto second = decimal(17, 2);
  if (year == 0 || month == 0 || month > 12 || hour > 23 || minute > 59 ||
      second > 59) {
    return false;
  }
  constexpr unsigned kDaysByMonth[] = {
      0, 31, 28, 31, 30, 31, 30, 31, 31, 30, 31, 30, 31};
  auto maximum_day = kDaysByMonth[month];
  const bool leap = (year % 4 == 0 && year % 100 != 0) || year % 400 == 0;
  if (month == 2 && leap) ++maximum_day;
  return day != 0 && day <= maximum_day;
}

bool ReadString(const std::vector<std::uint8_t>& data, std::size_t* offset, std::string* out) {
  if (*offset + 2 > data.size()) return false;
  const auto length = GetU16(data, *offset);
  *offset += 2;
  if (*offset + length > data.size()) return false;
  out->assign(reinterpret_cast<const char*>(data.data() + *offset), length);
  *offset += length;
  return true;
}

std::array<std::uint8_t, 16> PrincipalUuidFor(const std::string& principal) {
  auto uuid = sbps::MakeUuidV7Bytes();
  if (!principal.empty()) {
    for (std::size_t i = 0; i < principal.size(); ++i) {
      uuid[i % uuid.size()] ^= static_cast<std::uint8_t>(principal[i]);
    }
    uuid[6] = static_cast<std::uint8_t>((uuid[6] & 0x0fu) | 0x70u);
    uuid[8] = static_cast<std::uint8_t>((uuid[8] & 0x3fu) | 0x80u);
  }
  return uuid;
}

std::array<std::uint8_t, 16> TextToUuid(std::string_view text) {
  std::array<std::uint8_t, 16> out{};
  auto hex_value = [](char ch) -> int {
    if (ch >= '0' && ch <= '9') return ch - '0';
    if (ch >= 'a' && ch <= 'f') return ch - 'a' + 10;
    if (ch >= 'A' && ch <= 'F') return ch - 'A' + 10;
    return -1;
  };
  std::size_t nibble = 0;
  for (const char ch : text) {
    if (ch == '-') continue;
    const int value = hex_value(ch);
    if (value < 0 || nibble >= 32) return {};
    if ((nibble % 2) == 0) {
      out[nibble / 2] = static_cast<std::uint8_t>(value << 4);
    } else {
      out[nibble / 2] = static_cast<std::uint8_t>(out[nibble / 2] | value);
    }
    ++nibble;
  }
  return nibble == 32 ? out : std::array<std::uint8_t, 16>{};
}

std::string JsonEscape(const std::string& value) {
  return EscapeMessageVectorText(value);
}

bool StartsWith(std::string_view value, std::string_view prefix) {
  return value.size() >= prefix.size() && value.substr(0, prefix.size()) == prefix;
}

bool OperationCancellationCanBeDeterministic(std::string_view operation_id) {
  return StartsWith(operation_id, "dml.select") ||
         StartsWith(operation_id, "query.select") ||
         operation_id == "observability.show_version" ||
         operation_id == "catalog.show_version";
}

bool CancellationOutcomeUnknown(const ServerRequestRecord& request,
                                const ServerSessionRecord& actor) {
  if (request.engine_result_retained) return true;
  const bool active_transaction =
      request.local_transaction_id_at_start != 0 ||
      actor.local_transaction_id != 0;
  if (!active_transaction) return false;
  return !OperationCancellationCanBeDeterministic(request.operation_id);
}

std::string FirstOpenDatabasePath(const HostedEngineState& engine_state) {
  for (const auto& database : engine_state.databases) {
    if (database.database_open) return database.database_path;
  }
  return {};
}

std::string FirstOpenDatabaseUuid(const HostedEngineState& engine_state) {
  for (const auto& database : engine_state.databases) {
    if (database.database_open) return database.database_uuid;
  }
  return {};
}

const HostedDatabaseSnapshot* FirstOpenDatabase(const HostedEngineState& engine_state) {
  for (const auto& database : engine_state.databases) {
    if (database.database_open) return &database;
  }
  return nullptr;
}

std::uint64_t FirstOpenDatabasePageSizeBytes(const HostedEngineState& engine_state) {
  if (const auto* database = FirstOpenDatabase(engine_state); database != nullptr) {
    return database->page_size_bytes;
  }
  return 0;
}

std::uint64_t DatabasePageSizeBytesForSession(const ServerSessionRecord& session,
                                              const HostedEngineState& engine_state) {
  for (const auto& database : engine_state.databases) {
    if (!database.database_open) continue;
    const bool path_matches = !session.database_path.empty() &&
                              database.database_path == session.database_path;
    const bool uuid_matches = !session.database_uuid.empty() &&
                              database.database_uuid == session.database_uuid;
    if (path_matches || uuid_matches) return database.page_size_bytes;
  }
  return FirstOpenDatabasePageSizeBytes(engine_state);
}

void ApplyDatabaseHealthToSession(ServerSessionRecord* session,
                                  const HostedDatabaseSnapshot& database) {
  if (session == nullptr) return;
  session->database_engine_agent_state = database.database_engine_agent_state;
  session->database_engine_agent_health_generation =
      database.database_engine_agent_health_generation;
  session->database_engine_agent_ordinary_admission_allowed =
      database.database_engine_agent_ordinary_admission_allowed;
  session->database_engine_agent_health_json = database.database_engine_agent_health_json;
  session->resource_seed_pack_root = database.resource_seed_pack_root;
  session->policy_seed_pack_root = database.policy_seed_pack_root;
  session->config_source_epoch = database.config_source_epoch;
  session->config_reload_generation = database.config_reload_generation;
  session->capability_policy_generation =
      database.capability_policy_generation == 0 ? 1 : database.capability_policy_generation;
  session->policy_generation = database.policy_generation == 0 ? 1 : database.policy_generation;
  session->security_epoch = database.security_epoch == 0 ? 1 : database.security_epoch;
  session->security_provider_generation =
      database.security_provider_generation == 0 ? 1 : database.security_provider_generation;
  session->cache_invalidation_epoch =
      database.cache_invalidation_epoch == 0 ? 1 : database.cache_invalidation_epoch;
  session->config_policy_security_lifecycle_json =
      database.config_policy_security_lifecycle_json;
}

ServerDiagnostic AuthDiagnostic(std::string code, std::string message, std::string detail = {}) {
  std::vector<ServerDiagnosticField> fields;
  if (!detail.empty()) fields.push_back({"detail", detail});
  return ServerDiagnostic{std::move(code),
                          std::move(code),
                          ServerDiagnosticSeverity::kError,
                          std::move(message),
                          std::move(fields)};
}

ServerDiagnostic DetachCleanupDiagnostic(std::string code,
                                         ServerDiagnosticSeverity severity,
                                         std::string message,
                                         std::vector<ServerDiagnosticField> fields = {}) {
  return ServerDiagnostic{std::move(code),
                          std::move(code),
                          severity,
                          std::move(message),
                          std::move(fields)};
}

ServerDiagnostic DblcAttachAdmissionDenied(std::string phase, std::string detail) {
  std::vector<ServerDiagnosticField> fields;
  fields.push_back({"phase", std::move(phase)});
  if (!detail.empty()) fields.push_back({"detail", std::move(detail)});
  return ServerDiagnostic{"ENGINE.DBLC_ATTACH_ADMISSION_DENIED",
                          "ENGINE.DBLC_ATTACH_ADMISSION_DENIED",
                          ServerDiagnosticSeverity::kError,
                          "Attach/auth/session admission failed after lifecycle classification.",
                          std::move(fields)};
}

void AddAttachAdmissionDenied(std::vector<ServerDiagnostic>* diagnostics,
                              std::string phase,
                              std::string detail) {
  diagnostics->push_back(DblcAttachAdmissionDenied(std::move(phase), std::move(detail)));
}

bool RequestedDatabaseMatches(const HostedDatabaseSnapshot& database,
                              const std::string& requested_database) {
  constexpr std::string_view kDevBootstrapPath = "dev_bootstrap_path:";
  std::string_view selector(requested_database);
  if (selector.starts_with(kDevBootstrapPath)) selector.remove_prefix(kDevBootstrapPath.size());
  return selector.empty() || selector == "default" ||
         selector == database.database_path ||
         selector == database.database_uuid;
}

bool AuthContextMatchesHostedDatabase(const ServerSessionRecord& session,
                                      const HostedDatabaseSnapshot& database) {
  if (!session.database_path.empty() && session.database_path != database.database_path) return false;
  if (!session.database_uuid.empty() && session.database_uuid != database.database_uuid) return false;
  return true;
}

ConfigPolicySecurityLifecycle ConfigPolicySecurityLifecycleFromDatabase(
    const HostedDatabaseSnapshot& database) {
  ConfigPolicySecurityLifecycle lifecycle;
  lifecycle.database_path = database.database_path;
  lifecycle.database_uuid = database.database_uuid;
  lifecycle.config_source = "hosted_database";
  lifecycle.config_source_epoch = database.config_source_epoch == 0 ? 1 : database.config_source_epoch;
  lifecycle.config_reload_generation =
      database.config_reload_generation == 0 ? 1 : database.config_reload_generation;
  lifecycle.capability_policy_generation =
      database.capability_policy_generation == 0 ? 1 : database.capability_policy_generation;
  lifecycle.policy_generation = database.policy_generation == 0 ? 1 : database.policy_generation;
  lifecycle.security_epoch = database.security_epoch == 0 ? 1 : database.security_epoch;
  lifecycle.provider_family = database.security_provider_family.empty()
                                  ? "local_password"
                                  : database.security_provider_family;
  lifecycle.provider_generation =
      database.security_provider_generation == 0 ? 1 : database.security_provider_generation;
  lifecycle.provider_state = ParseSecurityProviderLifecycleState(
      database.security_provider_state.empty() ? "healthy" : database.security_provider_state);
  lifecycle.provider_plugin_loaded =
      lifecycle.provider_state != SecurityProviderLifecycleState::kDisabled &&
      lifecycle.provider_state != SecurityProviderLifecycleState::kQuarantined;
  lifecycle.provider_started = lifecycle.provider_state != SecurityProviderLifecycleState::kLoaded &&
                               lifecycle.provider_plugin_loaded;
  lifecycle.provider_healthy = lifecycle.provider_state == SecurityProviderLifecycleState::kHealthy ||
                               lifecycle.provider_state == SecurityProviderLifecycleState::kStarted;
  lifecycle.default_policy_installed = database.default_policy_installed;
  lifecycle.cluster_authority_required = database.cluster_authority_required;
  lifecycle.cache_invalidation_epoch =
      database.cache_invalidation_epoch == 0 ? 1 : database.cache_invalidation_epoch;
  lifecycle.invalidated_cache_targets = {"sessions",
                                         "prepared_statements",
                                         "parser_pools",
                                         "capability_policy_cache",
                                         "listener_pools",
                                         "manager_routes",
                                         "ipc_channels",
                                         "metrics_descriptors",
                                         "security_assertion_caches"};
  return lifecycle;
}

bool AttachmentModeSupported(const std::string& mode) {
  return mode == "read_write" || mode == "read_only";
}

std::string CanonicalAttachMode(const std::string& mode) {
  return mode.empty() ? "read_write" : mode;
}

bool ConnectionHeaderMatchesPayload(const sbps::Frame& request,
                                    const std::array<std::uint8_t, 16>& payload_connection_uuid) {
  return sbps::IsZeroUuid(request.header.connection_uuid) ||
         request.header.connection_uuid == payload_connection_uuid;
}

bool ConnectionHeaderMatchesSession(const sbps::Frame& request,
                                    const ServerSessionRecord& session) {
  return sbps::IsZeroUuid(request.header.connection_uuid) ||
         sbps::IsZeroUuid(session.connection_uuid) ||
         request.header.connection_uuid == session.connection_uuid;
}

std::string EngineDiagnosticCode(const std::vector<engine_api::EngineApiDiagnostic>& diagnostics,
                                 std::string fallback) {
  if (!diagnostics.empty() && !diagnostics.front().code.empty()) return diagnostics.front().code;
  return fallback;
}

std::string EngineDiagnosticDetail(const std::vector<engine_api::EngineApiDiagnostic>& diagnostics,
                                   std::string fallback = {}) {
  if (!diagnostics.empty() && !diagnostics.front().detail.empty()) return diagnostics.front().detail;
  return fallback;
}

std::string EngineEvidenceValue(const engine_api::EngineApiResult& result,
                                std::string_view evidence_kind) {
  for (const auto& evidence : result.evidence) {
    if (evidence.evidence_kind == evidence_kind) return evidence.evidence_id;
  }
  return {};
}

engine_api::EngineRequestContext EngineContextBase(const HostedEngineState& engine_state,
                                                   const sbps::Frame& request,
                                                   const std::string& language = "en") {
  engine_api::EngineRequestContext context;
  context.trust_mode = engine_api::EngineTrustMode::server_isolated;
  context.request_id = UuidBytesToText(request.header.request_uuid);
  context.database_path = FirstOpenDatabasePath(engine_state);
  context.database_uuid.canonical = FirstOpenDatabaseUuid(engine_state);
  context.database_page_size_bytes = FirstOpenDatabasePageSizeBytes(engine_state);
  context.statement_uuid.canonical = context.request_id;
  context.statement_timestamp = CurrentUtcTimestampText();
  context.current_timestamp = context.statement_timestamp;
  context.current_monotonic_ns = CurrentMonotonicNsText();
  context.security_context_present = false;
  context.cluster_authority_available = false;
  ServerSessionRecord language_seed;
  ApplyRequestedLanguageProfile(&language_seed, language);
  PopulateEngineLanguageContextFromSession(language_seed, &context.language_context);
  return context;
}

engine_api::EngineTrustMode TrustModeForSession(const ServerSessionRecord& session) {
  return session.embedded_in_process ? engine_api::EngineTrustMode::embedded_in_process
                                     : engine_api::EngineTrustMode::server_isolated;
}

bool IsZeroUuidBytes(const std::array<std::uint8_t, 16>& uuid);
bool ContainsUuid(const std::vector<std::array<std::uint8_t, 16>>& values,
                  const std::array<std::uint8_t, 16>& target);

void AddMaterializedSessionGrant(engine_api::EngineMaterializedAuthorizationContext* authorization,
                                 const engine_api::EngineUuid& principal_uuid,
                                 std::string tag,
                                 bool deny) {
  if (authorization == nullptr || tag.empty()) return;
  std::string right = std::move(tag);
  engine_api::EngineUuid target_uuid;
  const std::size_t separator = right.find(':');
  if (separator != std::string::npos) {
    target_uuid.canonical = right.substr(separator + 1);
    right.resize(separator);
  }
  if (right.empty()) return;

  engine_api::EngineMaterializedAuthorizationGrant grant;
  grant.subject_uuid = principal_uuid;
  grant.subject_kind = "principal";
  grant.target_uuid = std::move(target_uuid);
  grant.right = std::move(right);
  grant.deny = deny;
  grant.security_epoch = authorization->security_epoch;
  authorization->grants.push_back(std::move(grant));
}

std::string LowerAscii(std::string value) {
  for (char& ch : value) {
    ch = static_cast<char>(std::tolower(static_cast<unsigned char>(ch)));
  }
  return value;
}

std::string NormalizeAuthorizationSubjectKind(std::string kind) {
  kind = LowerAscii(std::move(kind));
  if (kind.empty() || kind == "user" || kind == "service" ||
      kind == "system_actor") {
    return "principal";
  }
  return kind;
}

std::string NormalizeRoleName(std::string value) {
  value = LowerAscii(std::move(value));
  return value;
}

bool IsUuidTextPresent(std::string_view value) {
  return value.size() == 36 && value[8] == '-' && value[13] == '-' &&
         value[18] == '-' && value[23] == '-';
}

void AddUniqueUuidBytes(std::vector<std::array<std::uint8_t, 16>>* values,
                        const std::array<std::uint8_t, 16>& uuid) {
  if (values == nullptr || IsZeroUuidBytes(uuid) || ContainsUuid(*values, uuid)) return;
  values->push_back(uuid);
}

void AddUniqueTraceTag(std::vector<std::string>* tags, std::string tag) {
  if (tags == nullptr || tag.empty()) return;
  if (std::find(tags->begin(), tags->end(), tag) == tags->end()) {
    tags->push_back(std::move(tag));
  }
}

std::string UuidSetHash(std::string_view prefix,
                        const std::vector<std::array<std::uint8_t, 16>>& values) {
  std::vector<std::string> texts;
  texts.reserve(values.size());
  for (const auto& value : values) {
    if (!IsZeroUuidBytes(value)) texts.push_back(UuidBytesToText(value));
  }
  std::sort(texts.begin(), texts.end());
  std::ostringstream out;
  out << prefix << '/' << texts.size();
  for (const auto& text : texts) out << ':' << text;
  return out.str();
}

std::string InferLifecycleSubjectKind(
    const engine_api::EngineSecurityPrincipalLifecycleState& state,
    const std::string& uuid) {
  for (const auto& role : state.roles) {
    if (role.role_uuid == uuid) return "role";
  }
  for (const auto& group : state.groups) {
    if (group.group_uuid == uuid) return "group";
  }
  return "principal";
}

bool LifecycleStateHasDurableSubjects(
    const engine_api::EngineSecurityPrincipalLifecycleState& state) {
  return !state.principals.empty() || !state.roles.empty() ||
         !state.groups.empty() || !state.memberships.empty() ||
         !state.grants.empty();
}

engine_api::DurableAuthorizationState DurableAuthorizationStateFromLifecycle(
    const engine_api::EngineSecurityPrincipalLifecycleState& lifecycle,
    const ServerSessionRecord& session,
    const engine_api::EngineRequestContext& context) {
  engine_api::DurableAuthorizationState state;
  state.authority_uuid = context.database_uuid;
  state.security_epoch =
      context.security_epoch == 0 ? std::max<std::uint64_t>(1, lifecycle.security_generation)
                                  : context.security_epoch;
  state.policy_epoch =
      session.policy_generation == 0
          ? std::max<std::uint64_t>(1, lifecycle.policy_generation)
          : session.policy_generation;
  state.catalog_generation_id =
      context.catalog_generation_id == 0 ? std::max<std::uint64_t>(1, session.catalog_generation)
                                         : context.catalog_generation_id;

  for (const auto& principal : lifecycle.principals) {
    if (principal.deleted || principal.lifecycle_state != "active") continue;
    engine_api::DurableAuthorizationPrincipalRecord record;
    record.principal_uuid.canonical = principal.principal_uuid;
    record.principal_kind = "principal";
    record.active = true;
    record.security_epoch = state.security_epoch;
    state.principals.push_back(std::move(record));
  }
  for (const auto& role : lifecycle.roles) {
    if (role.deleted || role.lifecycle_state != "active") continue;
    engine_api::DurableAuthorizationRoleRecord record;
    record.role_uuid.canonical = role.role_uuid;
    record.active = true;
    record.security_epoch = state.security_epoch;
    state.roles.push_back(std::move(record));
  }
  for (const auto& group : lifecycle.groups) {
    if (group.deleted || group.lifecycle_state != "active") continue;
    engine_api::DurableAuthorizationGroupRecord record;
    record.group_uuid.canonical = group.group_uuid;
    record.active = true;
    record.security_epoch = state.security_epoch;
    state.groups.push_back(std::move(record));
  }
  for (const auto& membership : lifecycle.memberships) {
    if (membership.revoked || membership.member_principal_uuid.empty() ||
        membership.container_uuid.empty()) {
      continue;
    }
    engine_api::DurableAuthorizationMembershipRecord record;
    record.member_uuid.canonical = membership.member_principal_uuid;
    record.member_kind = InferLifecycleSubjectKind(lifecycle,
                                                   membership.member_principal_uuid);
    record.parent_uuid.canonical = membership.container_uuid;
    record.parent_kind = NormalizeAuthorizationSubjectKind(membership.container_kind);
    if (record.parent_kind == "principal") {
      record.parent_kind = InferLifecycleSubjectKind(lifecycle, membership.container_uuid);
    }
    record.active = true;
    record.security_epoch = state.security_epoch;
    state.memberships.push_back(std::move(record));
  }
  for (const auto& grant : lifecycle.grants) {
    if (grant.revoked || grant.privilege.empty()) continue;
    engine_api::DurableAuthorizationGrantRecord record;
    record.grant_uuid.canonical = grant.grant_uuid;
    record.subject_uuid.canonical = grant.grantee_uuid;
    record.subject_kind = NormalizeAuthorizationSubjectKind(grant.grantee_kind);
    record.target_uuid.canonical = grant.target_object_uuid;
    record.right = grant.privilege;
    record.deny = LowerAscii(grant.grant_effect) == "deny";
    record.active = true;
    record.security_epoch = state.security_epoch;
    state.grants.push_back(std::move(record));
  }
  const auto sysarch_identity =
      engine_api::ResolveEngineOwnedSysarchRoleIdentity(context);
  if (sysarch_identity.ok) {
    state.engine_owned_sysarch_role_uuid.canonical = sysarch_identity.role_uuid;
  }
  return state;
}

engine_api::EngineRequestContext DurableProjectionContextForSession(
    const ServerSessionRecord& session) {
  engine_api::EngineRequestContext context;
  context.trust_mode = TrustModeForSession(session);
  context.database_path = session.database_path;
  context.database_uuid.canonical = session.database_uuid;
  context.principal_uuid.canonical = UuidBytesToText(session.effective_user_uuid);
  context.session_uuid.canonical = UuidBytesToText(session.session_uuid);
  context.security_context_present = true;
  context.catalog_generation_id = session.catalog_generation == 0 ? 1 : session.catalog_generation;
  context.security_epoch = session.security_epoch == 0 ? 1 : session.security_epoch;
  context.resource_epoch = session.resource_epoch == 0 ? 1 : session.resource_epoch;
  context.name_resolution_epoch =
      session.name_resolution_epoch == 0 ? 1 : session.name_resolution_epoch;
  return context;
}

engine_api::DurableAuthorizationMaterializeResult MaterializeDurableAuthorizationForSession(
    const ServerSessionRecord& session,
    const engine_api::EngineRequestContext& context,
    engine_api::EngineSecurityPrincipalLifecycleState* lifecycle_state = nullptr) {
  engine_api::DurableAuthorizationMaterializeResult result;
  const auto loaded = engine_api::LoadSecurityPrincipalLifecycleState(context);
  if (!loaded.ok || !LifecycleStateHasDurableSubjects(loaded.state)) return result;
  if (lifecycle_state != nullptr) *lifecycle_state = loaded.state;
  const auto durable_state =
      DurableAuthorizationStateFromLifecycle(loaded.state, session, context);
  engine_api::DurableAuthorizationMaterializeRequest request;
  request.principal_uuid = context.principal_uuid;
  request.observed_security_epoch = context.security_epoch;
  request.observed_policy_epoch = session.policy_generation;
  request.observed_catalog_generation_id = context.catalog_generation_id;
  return engine_api::MaterializeDurableAuthorizationContext(durable_state, request);
}

bool EffectiveSubjectContains(const engine_api::EngineMaterializedAuthorizationContext& context,
                              const std::string& subject_uuid,
                              std::string_view subject_kind) {
  for (const auto& subject : context.effective_subjects) {
    if (subject.subject_uuid.canonical == subject_uuid &&
        subject.subject_kind == subject_kind) {
      return true;
    }
  }
  return false;
}

std::string ResolveRequestedRoleUuid(
    const engine_api::EngineSecurityPrincipalLifecycleState& lifecycle,
    std::string_view requested_role) {
  if (requested_role.empty()) return {};
  if (IsUuidTextPresent(requested_role)) return std::string(requested_role);
  const std::string wanted = NormalizeRoleName(std::string(requested_role));
  for (const auto& role : lifecycle.roles) {
    if (role.deleted || role.lifecycle_state != "active") continue;
    if (NormalizeRoleName(role.role_name) == wanted) return role.role_uuid;
  }
  return {};
}

bool ApplyDurableAuthorizationProjectionToSession(ServerSessionRecord* session,
                                                  std::string* rejection_detail) {
  if (session == nullptr) return true;
  const auto context = DurableProjectionContextForSession(*session);
  engine_api::EngineSecurityPrincipalLifecycleState lifecycle;
  const auto materialized =
      MaterializeDurableAuthorizationForSession(*session, context, &lifecycle);
  if (!LifecycleStateHasDurableSubjects(lifecycle)) {
    if (!session->requested_role_name.empty() && rejection_detail != nullptr) {
      *rejection_detail = "requested_role_unavailable";
    }
    return session->requested_role_name.empty();
  }
  if (!materialized.ok) {
    if (rejection_detail != nullptr) {
      *rejection_detail = materialized.diagnostics.empty()
                              ? "durable_authorization_context_unavailable"
                              : materialized.diagnostics.front().detail;
    }
    return false;
  }

  std::vector<std::array<std::uint8_t, 16>> roles;
  std::vector<std::array<std::uint8_t, 16>> groups;
  for (const auto& subject : materialized.context.effective_subjects) {
    if (subject.subject_kind == "role") {
      AddUniqueUuidBytes(&roles, TextToUuid(subject.subject_uuid.canonical));
    } else if (subject.subject_kind == "group") {
      AddUniqueUuidBytes(&groups, TextToUuid(subject.subject_uuid.canonical));
    }
  }
  session->effective_role_uuids = roles;
  session->effective_group_uuids = groups;

  if (!session->requested_role_name.empty()) {
    const std::string requested_uuid =
        ResolveRequestedRoleUuid(lifecycle, session->requested_role_name);
    if (requested_uuid.empty()) {
      if (rejection_detail != nullptr) *rejection_detail = "requested_role_not_found";
      return false;
    }
    if (!EffectiveSubjectContains(materialized.context, requested_uuid, "role")) {
      if (rejection_detail != nullptr) *rejection_detail = "requested_role_not_granted";
      return false;
    }
    session->active_role_uuid = TextToUuid(requested_uuid);
  } else if (roles.size() == 1) {
    session->active_role_uuid = roles.front();
  } else if (!IsZeroUuidBytes(session->active_role_uuid) &&
             !ContainsUuid(roles, session->active_role_uuid)) {
    session->active_role_uuid = {};
  }

  session->role_set_hash = UuidSetHash("roles", session->effective_role_uuids);
  session->group_set_hash = UuidSetHash("groups", session->effective_group_uuids);
  for (const auto& grant : materialized.context.grants) {
    if (grant.right.empty() || !grant.target_uuid.canonical.empty()) {
      continue;
    }
    AddUniqueTraceTag(&session->engine_authorization_trace_tags,
                      std::string(grant.deny ? "deny:" : "right:") + grant.right);
  }
  for (const auto& role : session->effective_role_uuids) {
    AddUniqueTraceTag(&session->engine_authorization_trace_tags,
                      "role_uuid:" + UuidBytesToText(role));
  }
  for (const auto& group : session->effective_group_uuids) {
    AddUniqueTraceTag(&session->engine_authorization_trace_tags,
                      "group_uuid:" + UuidBytesToText(group));
  }
  AddUniqueTraceTag(&session->engine_authorization_trace_tags,
                    "server.session.durable_role_group_projection");
  return true;
}

engine_api::EngineMaterializedAuthorizationContext MaterializeSessionAuthorizationContext(
    const ServerSessionRecord& session,
    const engine_api::EngineRequestContext& context) {
  engine_api::EngineMaterializedAuthorizationContext authorization;
  authorization.authority_uuid = context.database_uuid;
  authorization.principal_uuid = context.principal_uuid;
  authorization.security_epoch = context.security_epoch;
  authorization.policy_epoch = session.policy_generation;
  authorization.catalog_generation_id = context.catalog_generation_id;
  if (authorization.principal_uuid.canonical.empty() ||
      authorization.authority_uuid.canonical.empty() ||
      authorization.security_epoch == 0 ||
      authorization.policy_epoch == 0 ||
      authorization.catalog_generation_id == 0) {
    return authorization;
  }

  const auto durable = MaterializeDurableAuthorizationForSession(session, context);
  if (durable.ok) {
    auto durable_context = durable.context;
    durable_context.evidence_tags.push_back(
        "server.session.durable_authorization_context");
    return durable_context;
  }

  engine_api::EngineAuthorizationSubject principal;
  principal.subject_uuid = authorization.principal_uuid;
  principal.subject_kind = "principal";
  authorization.effective_subjects.push_back(std::move(principal));

  for (const auto& tag : session.engine_authorization_trace_tags) {
    authorization.evidence_tags.push_back(tag);
    const bool fixture_authority =
        context.trust_mode == engine_api::EngineTrustMode::embedded_in_process &&
        engine_api::SecurityContextHasTag(
            context, "security.fixture_trace_authority");
    if (!fixture_authority) continue;
    if (StartsWith(tag, "deny:")) {
      AddMaterializedSessionGrant(&authorization,
                                  authorization.principal_uuid,
                                  tag.substr(std::string_view("deny:").size()),
                                  true);
    } else if (StartsWith(tag, "right:")) {
      AddMaterializedSessionGrant(&authorization,
                                  authorization.principal_uuid,
                                  tag.substr(std::string_view("right:").size()),
                                  false);
    }
  }
  authorization.present = true;
  if (!authorization.grants.empty()) {
    authorization.evidence_tags.push_back("server.session.materialized_authorization_context");
  } else {
    authorization.evidence_tags.push_back(
        "server.session.authorization_trace_tags_not_authority");
  }
  return authorization;
}

engine_api::EngineRequestContext EngineContextForSession(const ServerSessionRecord& session,
                                                         const HostedEngineState& engine_state,
                                                         const sbps::Frame& request) {
  auto context = EngineContextBase(engine_state, request);
  context.trust_mode = TrustModeForSession(session);
  context.database_path = session.database_path.empty() ? context.database_path : session.database_path;
  context.database_uuid.canonical =
      session.database_uuid.empty() ? context.database_uuid.canonical : session.database_uuid;
  context.database_page_size_bytes = DatabasePageSizeBytesForSession(session, engine_state);
  context.principal_uuid.canonical = UuidBytesToText(session.effective_user_uuid);
  context.session_uuid.canonical = UuidBytesToText(session.session_uuid);
  context.transaction_uuid.canonical = session.transaction_uuid;
  context.local_transaction_id = session.local_transaction_id;
  context.snapshot_visible_through_local_transaction_id =
      session.snapshot_visible_through_local_transaction_id;
  context.transaction_timestamp = session.transaction_timestamp;
  context.application_name = session.application_name;
  context.security_context_present = true;
  context.catalog_generation_id = session.catalog_generation;
  context.security_epoch = session.security_epoch;
  context.resource_epoch = session.resource_epoch;
  context.name_resolution_epoch = session.name_resolution_epoch;
  if (!IsZeroUuidBytes(session.active_role_uuid)) {
    context.current_role_uuid.canonical = UuidBytesToText(session.active_role_uuid);
  }
  PopulateEngineLanguageContextFromSession(session, &context.language_context);
  context.trace_tags = session.engine_authorization_trace_tags;
  context.trace_tags.push_back("sb_server.session_registry");
  context.authorization_context = MaterializeSessionAuthorizationContext(session, context);
  return context;
}

bool ApplyBeginTransactionResultToSession(
    const engine_api::EngineBeginTransactionResult& result,
    ServerSessionRecord* session) {
  if (session == nullptr ||
      !IsCompleteEngineTransactionIdentity(
          result.local_transaction_id, result.transaction_uuid.canonical) ||
      session->local_transaction_id != 0 ||
      session->default_local_transaction_id != 0 ||
      !session->transaction_uuid.empty() ||
      !session->transactions_by_local_id.empty()) {
    return false;
  }
  ServerTransactionState transaction;
  transaction.local_transaction_id = result.local_transaction_id;
  transaction.snapshot_visible_through_local_transaction_id =
      result.snapshot_visible_through_local_transaction_id;
  transaction.transaction_uuid = result.transaction_uuid.canonical;
  transaction.transaction_timestamp =
      EngineEvidenceValue(result, "transaction_timestamp");
  transaction.isolation_level = session->default_transaction_isolation_level;
  transaction.read_only =
      session->attach_mode == "read_only" || session->default_transaction_read_only;
  transaction.begin_ordinal = session->next_transaction_begin_ordinal++;
  const auto published = session->transactions_by_local_id.emplace(
      transaction.local_transaction_id, transaction);
  if (!published.second) return false;
  session->local_transaction_id = transaction.local_transaction_id;
  session->snapshot_visible_through_local_transaction_id =
      transaction.snapshot_visible_through_local_transaction_id;
  session->transaction_uuid = transaction.transaction_uuid;
  session->transaction_timestamp = transaction.transaction_timestamp;
  session->default_local_transaction_id = transaction.local_transaction_id;
  return true;
}

bool StartAlwaysActiveTransactionForSession(ServerSessionRecord* session,
                                            const HostedEngineState& engine_state,
                                            const sbps::Frame& request,
                                            std::string* diagnostic_code,
                                            std::string* diagnostic_detail) {
  if (session == nullptr) {
    if (diagnostic_code != nullptr) *diagnostic_code = "PARSER_SERVER_IPC.SESSION_REQUIRED";
    if (diagnostic_detail != nullptr) *diagnostic_detail = "session_required";
    return false;
  }
  if (session->local_transaction_id != 0 ||
      session->default_local_transaction_id != 0 ||
      !session->transaction_uuid.empty() ||
      !session->transactions_by_local_id.empty()) {
    if (diagnostic_code != nullptr) {
      *diagnostic_code =
          "PARSER_SERVER_IPC.INITIAL_TRANSACTION_ALREADY_PRESENT";
    }
    if (diagnostic_detail != nullptr) {
      *diagnostic_detail =
          "initial_transaction_must_start_from_empty_session_state";
    }
    return false;
  }
  engine_api::EngineBeginTransactionRequest begin;
  begin.context = EngineContextForSession(*session, engine_state, request);
  begin.context.local_transaction_id = 0;
  begin.context.transaction_uuid.canonical.clear();
  begin.context.snapshot_visible_through_local_transaction_id = 0;
  begin.context.transaction_timestamp.clear();
  begin.context.read_only_mode =
      session->attach_mode == "read_only" || session->default_transaction_read_only;
  begin.isolation_level = session->default_transaction_isolation_level;
  begin.transaction_policy_profile.encoded_profiles.push_back("fail_closed:true");
  begin.transaction_policy_profile.encoded_profiles.push_back(
      std::string("transaction_read_only:") + (begin.context.read_only_mode ? "true" : "false"));
  begin.transaction_policy_profile.encoded_profiles.push_back(
      std::string("transaction_read_mode:") + (begin.context.read_only_mode ? "read_only" : "read_write"));
  const auto begun = engine_api::EngineBeginTransaction(begin);
  if (!begun.ok ||
      !IsCompleteEngineTransactionIdentity(
          begun.local_transaction_id, begun.transaction_uuid.canonical)) {
    if (diagnostic_code != nullptr) {
      *diagnostic_code = begun.ok
          ? "PARSER_SERVER_IPC.TRANSACTION_BEGIN_IDENTITY_INVALID"
          : EngineDiagnosticCode(
                begun.diagnostics,
                "ENGINE.DBLC_TRANSACTION_ADMISSION_DENIED");
    }
    if (diagnostic_detail != nullptr) {
      *diagnostic_detail = begun.ok
          ? "engine_begin_returned_incomplete_composite_identity"
          : EngineDiagnosticDetail(begun.diagnostics,
                                   "transaction_begin_failed");
    }
    return false;
  }
  if (!ApplyBeginTransactionResultToSession(begun, session)) {
    if (diagnostic_code != nullptr) {
      *diagnostic_code =
          "PARSER_SERVER_IPC.TRANSACTION_BEGIN_PUBLICATION_FAILED";
    }
    if (diagnostic_detail != nullptr) {
      *diagnostic_detail =
          "initial_transaction_composite_identity_publication_failed";
    }
    return false;
  }
  return true;
}

bool RollbackUnpublishedInitialTransaction(
    const ServerSessionRecord& session,
    const HostedEngineState& engine_state,
    const sbps::Frame& request) {
  if (!IsCompleteEngineTransactionIdentity(
          session.local_transaction_id, session.transaction_uuid)) {
    return false;
  }
  engine_api::EngineRollbackTransactionRequest rollback;
  rollback.context = EngineContextForSession(session, engine_state, request);
  const auto result = engine_api::EngineRollbackTransaction(rollback);
  const bool applied =
      result.engine_finality_known &&
      (result.rollback_finality_state ==
           "rolled_back_by_engine_inventory" ||
       result.rollback_finality_state ==
           "rolled_back_post_inventory_secondary_failure");
  return applied &&
         result.local_transaction_id == session.local_transaction_id &&
         result.transaction_uuid.canonical == session.transaction_uuid;
}

std::optional<AuthHandoffPayload> DecodeAuthPayload(const std::vector<std::uint8_t>& payload) {
  if (payload.size() < 16 + 4) return std::nullopt;
  std::size_t offset = 0;
  AuthHandoffPayload auth;
  auth.connection_uuid = GetUuid(payload, offset);
  offset += 16;
  if (offset + 4 > payload.size()) return std::nullopt;
  auth.credential_evidence_present = payload[offset++] != 0;
  auth.credential_invalid = payload[offset++] != 0;
  auth.mfa_required = payload[offset++] != 0;
  auth.mfa_evidence_present = payload[offset++] != 0;
  if (!ReadString(payload, &offset, &auth.provider_family)) return std::nullopt;
  if (!ReadString(payload, &offset, &auth.principal_claim)) return std::nullopt;
  if (!ReadString(payload, &offset, &auth.requested_database)) return std::nullopt;
  if (!ReadString(payload, &offset, &auth.requested_language)) return std::nullopt;
  if (offset < payload.size() && !ReadString(payload, &offset, &auth.credential_evidence)) {
    return std::nullopt;
  }
  if (offset < payload.size() && !ReadString(payload, &offset, &auth.application_name)) {
    return std::nullopt;
  }
  if (offset < payload.size() && !ReadString(payload, &offset, &auth.requested_role)) {
    return std::nullopt;
  }
  return auth;
}

std::map<std::string, std::string> ParseAuthEvidenceFields(std::string_view evidence) {
  std::map<std::string, std::string> fields;
  std::size_t cursor = 0;
  while (cursor < evidence.size()) {
    const std::size_t end = evidence.find(';', cursor);
    const std::string_view part =
        evidence.substr(cursor,
                        end == std::string_view::npos ? evidence.size() - cursor
                                                       : end - cursor);
    const std::size_t eq = part.find('=');
    if (eq != std::string_view::npos && eq != 0 && eq + 1 < part.size()) {
      fields.emplace(std::string(part.substr(0, eq)), std::string(part.substr(eq + 1)));
    }
    if (end == std::string_view::npos) break;
    cursor = end + 1;
  }
  return fields;
}

struct TlsTransportDenial {
  bool denied = false;
  std::string code;
  std::string detail;
};

TlsTransportDenial TlsTransportDenialFromEvidence(const AuthHandoffPayload& auth) {
  const auto fields = ParseAuthEvidenceFields(auth.credential_evidence);
  auto value = [&](const std::string& key) -> std::string {
    const auto found = fields.find(key);
    return found == fields.end() ? std::string{} : found->second;
  };
  const std::string tls_downgrade = value("tls_downgrade");
  const std::string tls_required = value("tls_required");
  const std::string tls_negotiated = value("tls_negotiated");
  if (tls_downgrade == "true" ||
      (tls_required == "true" &&
       (tls_negotiated.empty() || tls_negotiated == "cleartext" ||
        tls_negotiated == "none"))) {
    return {true,
            "SECURITY.AUTHENTICATION.TLS_DOWNGRADE_REFUSED",
            "tls_downgrade_refused"};
  }
  const std::string cert_status = value("tls_client_cert_status");
  if (cert_status == "wrong_ca" || cert_status == "invalid_ca") {
    return {true,
            "SECURITY.AUTHENTICATION.TLS_CLIENT_CA_INVALID",
            "tls_client_ca_invalid"};
  }
  if (cert_status == "expired") {
    return {true,
            "SECURITY.AUTHENTICATION.TLS_CLIENT_CERT_EXPIRED",
            "tls_client_cert_expired"};
  }
  const std::string channel_binding = value("tls_channel_binding_status");
  if (channel_binding == "mismatch") {
    return {true,
            "SECURITY.AUTHENTICATION.TLS_CHANNEL_BINDING_MISMATCH",
            "tls_channel_binding_mismatch"};
  }
  if (tls_required == "true" &&
      (channel_binding.empty() ||
       (channel_binding != "ok" && channel_binding != "matched" &&
        channel_binding != "verified"))) {
    return {true,
            "SECURITY.AUTHENTICATION.TLS_CHANNEL_BINDING_MISSING",
            "tls_channel_binding_missing"};
  }
  return {};
}

std::optional<AttachPayload> DecodeAttachPayload(const std::vector<std::uint8_t>& payload) {
  if (payload.size() < 32) return std::nullopt;
  std::size_t offset = 0;
  AttachPayload attach;
  attach.connection_uuid = GetUuid(payload, offset);
  offset += 16;
  attach.auth_context_uuid = GetUuid(payload, offset);
  offset += 16;
  if (!ReadString(payload, &offset, &attach.requested_database)) return std::nullopt;
  if (!ReadString(payload, &offset, &attach.requested_attachment_mode)) return std::nullopt;
  return attach;
}

std::vector<std::uint8_t> EncodeAuthResultPayload(const std::string& outcome,
                                                  const ServerSessionRecord* session,
                                                  const std::string& detail = {}) {
  std::vector<std::uint8_t> out;
  PutString(&out, outcome);
  PutUuid(&out, session == nullptr ? std::array<std::uint8_t, 16>{} : session->auth_context_uuid);
  PutUuid(&out, session == nullptr ? std::array<std::uint8_t, 16>{} : session->session_uuid);
  PutUuid(&out, session == nullptr ? std::array<std::uint8_t, 16>{} : session->principal_uuid);
  PutUuid(&out, session == nullptr ? std::array<std::uint8_t, 16>{} : session->effective_user_uuid);
  PutU64(&out, session == nullptr ? 0 : session->security_epoch);
  PutString(&out, detail);
  PutString(&out, session == nullptr ? "" : session->database_engine_agent_health_json);
  return out;
}

std::vector<std::uint8_t> EncodeAttachResultPayload(const std::string& outcome,
                                                    const ServerSessionRecord* session,
                                                    const std::string& detail = {}) {
  std::vector<std::uint8_t> out;
  PutString(&out, outcome);
  PutUuid(&out, session == nullptr ? std::array<std::uint8_t, 16>{} : session->session_uuid);
  PutUuid(&out, session == nullptr ? std::array<std::uint8_t, 16>{} : session->effective_user_uuid);
  PutString(&out, session == nullptr ? "" : session->database_path);
  PutString(&out, session == nullptr ? "" : session->database_uuid);
  PutString(&out, session == nullptr ? "" : session->attach_mode);
  PutU64(&out, session == nullptr ? 0 : session->catalog_generation);
  PutU64(&out, session == nullptr ? 0 : session->security_epoch);
  PutU64(&out, session == nullptr ? 0 : session->policy_generation);
  PutU64(&out, session == nullptr ? 0 : session->name_resolution_epoch);
  PutU64(&out, session == nullptr ? 0 : session->resource_epoch);
  PutString(&out, detail);
  PutString(&out, session == nullptr ? "" : session->database_engine_agent_health_json);
  PutU64(&out, session == nullptr ? 0 : session->local_transaction_id);
  PutU64(&out, session == nullptr ? 0 : session->snapshot_visible_through_local_transaction_id);
  PutString(&out, session == nullptr ? "" : session->transaction_uuid);
  PutString(&out, session == nullptr ? "" : session->transaction_timestamp);
  if (session == nullptr) {
    PutU32(&out, 0);
    PutUuid(&out, {});
    PutU32(&out, 0);
  } else {
    PutU32(&out, static_cast<std::uint32_t>(session->effective_role_uuids.size()));
    for (const auto& role : session->effective_role_uuids) PutUuid(&out, role);
    PutUuid(&out, session->active_role_uuid);
    PutU32(&out, static_cast<std::uint32_t>(session->effective_group_uuids.size()));
    for (const auto& group : session->effective_group_uuids) PutUuid(&out, group);
  }
  return out;
}

std::string AuthContextKey(const std::array<std::uint8_t, 16>& uuid) {
  return UuidBytesToText(uuid);
}

void RecordFinality(ServerSessionRegistry* registry,
                    const sbps::Frame& request,
                    const std::array<std::uint8_t, 16>& session_uuid,
                    const std::array<std::uint8_t, 16>& auth_context_uuid,
                    std::string operation,
                    std::string state,
                    std::string detail = {}) {
  ServerFinalityRecord finality;
  finality.finality_token_uuid = sbps::MakeUuidV7Bytes();
  finality.request_uuid = request.header.request_uuid;
  finality.session_uuid = session_uuid;
  finality.auth_context_uuid = auth_context_uuid;
  finality.operation = std::move(operation);
  finality.state = std::move(state);
  finality.detail = std::move(detail);
  registry->finality_by_request_uuid[UuidBytesToText(finality.request_uuid)] = std::move(finality);
}

void UpsertRequestFinality(ServerSessionRegistry* registry,
                           const ServerRequestRecord& request) {
  if (registry == nullptr) return;
  ServerFinalityRecord finality;
  finality.finality_token_uuid = request.finality_token_uuid;
  finality.request_uuid = request.request_uuid;
  finality.session_uuid = request.session_uuid;
  finality.auth_context_uuid = request.auth_context_uuid;
  finality.operation = request.operation_id.empty() ? request.request_kind : request.operation_id;
  finality.state = ServerRequestLifecycleStateName(request.state);
  finality.detail = request.detail;
  registry->finality_by_request_uuid[UuidBytesToText(finality.request_uuid)] = std::move(finality);
}

bool TerminalRequestState(ServerRequestLifecycleState state) {
  return state == ServerRequestLifecycleState::kCompleted ||
         state == ServerRequestLifecycleState::kCancelled ||
         state == ServerRequestLifecycleState::kTimedOut ||
         state == ServerRequestLifecycleState::kDrained ||
         state == ServerRequestLifecycleState::kDisconnected ||
         state == ServerRequestLifecycleState::kUnknownOutcome ||
         state == ServerRequestLifecycleState::kFailed;
}

bool RequestTargetMatches(const ServerRequestRecord& request, const std::string& target_uuid) {
  if (target_uuid.empty()) return true;
  return UuidBytesToText(request.request_uuid) == target_uuid ||
         UuidBytesToText(request.finality_token_uuid) == target_uuid ||
         UuidBytesToText(request.prepared_statement_uuid) == target_uuid ||
         UuidBytesToText(request.cursor_uuid) == target_uuid;
}

std::string RequestLifecycleRecordJson(const ServerRequestRecord& request) {
  std::ostringstream out;
  out << "{\"request_uuid\":\"" << UuidBytesToText(request.request_uuid)
      << "\",\"finality_token_uuid\":\"" << UuidBytesToText(request.finality_token_uuid)
      << "\",\"session_uuid\":\"" << UuidBytesToText(request.session_uuid)
      << "\",\"request_kind\":\"" << JsonEscape(request.request_kind)
      << "\",\"operation_id\":\"" << JsonEscape(request.operation_id)
      << "\",\"state\":\"" << ServerRequestLifecycleStateName(request.state)
      << "\",\"detail\":\"" << JsonEscape(request.detail)
      << "\",\"prepared_statement_uuid\":\"" << UuidBytesToText(request.prepared_statement_uuid)
      << "\",\"cursor_uuid\":\"" << UuidBytesToText(request.cursor_uuid)
      << "\",\"local_transaction_id_at_start\":" << request.local_transaction_id_at_start
      << ",\"snapshot_visible_through_local_transaction_id\":"
      << request.snapshot_visible_through_local_transaction_id
      << ",\"fetch_timeout_ms\":" << request.fetch_timeout_ms
      << ",\"cancel_timeout_ms\":" << request.cancel_timeout_ms
      << ",\"drain_timeout_ms\":" << request.drain_timeout_ms
      << ",\"authorization_proven\":" << (request.authorization_proven ? "true" : "false")
      << ",\"transaction_finality_preserved\":"
      << (request.transaction_finality_preserved ? "true" : "false")
      << ",\"engine_result_retained\":"
      << (request.engine_result_retained ? "true" : "false") << "}";
  return out.str();
}

ServerDiagnostic RequestLifecycleDiagnostic(std::string code,
                                            ServerDiagnosticSeverity severity,
                                            std::string message,
                                            std::vector<ServerDiagnosticField> fields = {}) {
  return ServerDiagnostic{std::move(code),
                          std::move(code),
                          severity,
                          std::move(message),
                          std::move(fields)};
}

ServerDiagnostic DriverTransactionDiagnostic(std::string code,
                                             std::string message,
                                             std::string detail,
                                             std::string sqlstate,
                                             std::string finality_state) {
  const std::string message_key = code;
  std::vector<ServerDiagnosticField> fields{{"detail", std::move(detail)},
                                            {"sqlstate", std::move(sqlstate)},
                                            {"finality_state", std::move(finality_state)},
                                            {"retryability", "no_hidden_retry"},
                                            {"mga_finality_authority", "engine"}};
  return ServerDiagnostic{std::move(code),
                          message_key,
                          ServerDiagnosticSeverity::kError,
                          std::move(message),
                          std::move(fields)};
}

ServerDiagnostic TransactionPressureDiagnostic(std::string code,
                                               std::string message,
                                               std::string detail,
                                               bool mutates_transaction,
                                               bool opens_replacement_boundary) {
  const std::string message_key = code;
  std::vector<ServerDiagnosticField> fields{
      {"detail", std::move(detail)},
      {"mga_finality_authority", "engine"},
      {"agent_authority", "policy_and_server_gate"},
      {"parser_finality_authority", "false"},
      {"client_state_authority", "false"},
      {"mutates_transaction", mutates_transaction ? "true" : "false"},
      {"opens_replacement_boundary", opens_replacement_boundary ? "true" : "false"}};
  return ServerDiagnostic{std::move(code),
                          message_key,
                          mutates_transaction ? ServerDiagnosticSeverity::kWarning
                                              : ServerDiagnosticSeverity::kInfo,
                          std::move(message),
                          std::move(fields)};
}

std::string DetachCleanupDetail(std::string disconnect_reason,
                                std::uint64_t sessions_removed,
                                std::uint64_t auth_contexts_removed,
                                std::uint64_t prepared_tombstoned,
                                std::uint64_t cursors_tombstoned,
                                std::uint64_t engine_results_released,
                                std::uint64_t request_finality_records_updated,
                                std::uint64_t temporary_rows_deleted,
                                std::uint64_t temporary_large_values_reclaimed,
                                std::uint64_t temporary_private_metadata_retired,
                                std::string temporary_cleanup_state,
                                std::uint64_t active_local_transaction_id) {
  std::ostringstream out;
  out << "disconnect_reason=" << disconnect_reason
      << ";sessions_removed=" << sessions_removed
      << ";auth_contexts_removed=" << auth_contexts_removed
      << ";prepared_tombstoned=" << prepared_tombstoned
      << ";cursors_tombstoned=" << cursors_tombstoned
      << ";engine_results_released=" << engine_results_released
      << ";request_finality_records_updated=" << request_finality_records_updated
      << ";temporary_rows_deleted=" << temporary_rows_deleted
      << ";temporary_large_values_reclaimed=" << temporary_large_values_reclaimed
      << ";temporary_private_metadata_retired=" << temporary_private_metadata_retired
      << ";temporary_cleanup_state=" << temporary_cleanup_state
      << ";disconnect_does_not_commit=true"
      << ";disconnect_does_not_rollback=true"
      << ";mga_finality_authority=engine";
  if (active_local_transaction_id != 0) {
    out << ";active_local_transaction_id=" << active_local_transaction_id
        << ";active_transaction_outcome=unknown_preserved";
  } else {
    out << ";active_transaction_outcome=none";
  }
  return out.str();
}

bool IsZeroUuidBytes(const std::array<std::uint8_t, 16>& uuid) {
  return std::all_of(uuid.begin(), uuid.end(), [](std::uint8_t value) {
    return value == 0;
  });
}

bool UuidMatchesIfPresent(const std::array<std::uint8_t, 16>& expected,
                          const std::array<std::uint8_t, 16>& actual) {
  return IsZeroUuidBytes(expected) || expected == actual;
}

bool ContainsUuid(const std::vector<std::array<std::uint8_t, 16>>& values,
                  const std::array<std::uint8_t, 16>& target) {
  return std::find(values.begin(), values.end(), target) != values.end();
}

ServerDiagnostic SessionControlDiagnostic(std::string code,
                                          ServerDiagnosticSeverity severity,
                                          std::string message,
                                          std::string detail,
                                          std::vector<ServerDiagnosticField> fields = {}) {
  if (!detail.empty()) fields.push_back({"detail", detail});
  fields.push_back({"server_session_registry_authority", "true"});
  fields.push_back({"parser_session_authority", "false"});
  return ServerDiagnostic{std::move(code),
                          std::move(code),
                          severity,
                          std::move(message),
                          std::move(fields)};
}

ServerSessionBindingControlResult SessionControlRejected(
    std::string code,
    std::string detail,
    std::string message,
    std::vector<ServerDiagnosticField> fields = {}) {
  ServerSessionBindingControlResult result;
  result.diagnostic_code = code;
  result.detail = detail;
  result.authorization_denied = code == "SERVER.SESSION_CONTROL.AUTHORIZATION_DENIED";
  result.replay_rejected = code == "SERVER.SESSION_CONTROL.REPLAY_REFUSED" ||
                           code == "SERVER.SESSION_TAKEOVER.REPLAY_REFUSED";
  result.diagnostics.push_back(SessionControlDiagnostic(std::move(code),
                                                        ServerDiagnosticSeverity::kError,
                                                        std::move(message),
                                                        std::move(detail),
                                                        std::move(fields)));
  return result;
}

bool SessionControlAuthorized(const ServerSessionControlAuthority& authority,
                              bool allowed) {
  return authority.authenticated && allowed && authority.sequence != 0 &&
         !authority.authority_class.empty() && !authority.actor_token.empty();
}

void MirrorAuthContext(ServerSessionRegistry* registry, const ServerSessionRecord& session) {
  if (registry == nullptr || IsZeroUuidBytes(session.auth_context_uuid)) return;
  const auto key = AuthContextKey(session.auth_context_uuid);
  if (registry->auth_contexts_by_uuid.find(key) != registry->auth_contexts_by_uuid.end()) {
    registry->auth_contexts_by_uuid[key] = session;
  }
}

std::map<std::string, ServerSessionRecord>::iterator FindMutableSessionByBindingTarget(
    ServerSessionRegistry* registry,
    const std::array<std::uint8_t, 16>& catalog_session_id,
    const std::array<std::uint8_t, 16>& protocol_session_id) {
  if (registry == nullptr) return {};
  if (!IsZeroUuidBytes(catalog_session_id)) {
    return registry->sessions_by_uuid.find(UuidBytesToText(catalog_session_id));
  }
  if (!IsZeroUuidBytes(protocol_session_id)) {
    for (auto it = registry->sessions_by_uuid.begin(); it != registry->sessions_by_uuid.end(); ++it) {
      if (it->second.session_binding_present &&
          it->second.protocol_session_id == protocol_session_id) {
        return it;
      }
    }
  }
  return registry->sessions_by_uuid.end();
}

std::map<std::string, ServerSessionRecord>::const_iterator FindSessionByBindingTarget(
    const ServerSessionRegistry& registry,
    const std::array<std::uint8_t, 16>& catalog_session_id,
    const std::array<std::uint8_t, 16>& protocol_session_id) {
  if (!IsZeroUuidBytes(catalog_session_id)) {
    return registry.sessions_by_uuid.find(UuidBytesToText(catalog_session_id));
  }
  if (!IsZeroUuidBytes(protocol_session_id)) {
    for (auto it = registry.sessions_by_uuid.begin(); it != registry.sessions_by_uuid.end(); ++it) {
      if (it->second.session_binding_present &&
          it->second.protocol_session_id == protocol_session_id) {
        return it;
      }
    }
  }
  return registry.sessions_by_uuid.end();
}

bool TakeoverClaimsMatch(const ServerSessionRecord& session,
                         const ServerSessionTakeoverRequest& request,
                         std::string* detail) {
  if (!session.session_binding_present) {
    if (detail != nullptr) *detail = "session_binding_required";
    return false;
  }
  if ((request.mask & kServerTakeoverClaimCatalogSessionId) &&
      request.catalog_session_id != session.catalog_session_id) {
    if (detail != nullptr) *detail = "catalog_session_id_mismatch";
    return false;
  }
  if ((request.mask & kServerTakeoverClaimAuthkeyId) &&
      !UuidMatchesIfPresent(request.authkey_id, session.authkey_id)) {
    if (detail != nullptr) *detail = "authkey_id_mismatch";
    return false;
  }
  if ((request.mask & kServerTakeoverClaimAuthenticatedPrincipalId) &&
      !UuidMatchesIfPresent(request.authenticated_principal_id, session.principal_uuid)) {
    if (detail != nullptr) *detail = "authenticated_principal_id_mismatch";
    return false;
  }
  if ((request.mask & kServerTakeoverClaimSessionUserId) &&
      !UuidMatchesIfPresent(request.session_user_id, session.effective_user_uuid)) {
    if (detail != nullptr) *detail = "session_user_id_mismatch";
    return false;
  }
  if ((request.mask & kServerTakeoverClaimActiveRoleId) &&
      !UuidMatchesIfPresent(request.active_role_id, session.active_role_uuid)) {
    if (detail != nullptr) *detail = "active_role_id_mismatch";
    return false;
  }
  if ((request.mask & kServerTakeoverClaimCurrentTxnId) &&
      request.current_txn_id != session.local_transaction_id) {
    if (detail != nullptr) *detail = "current_txn_id_mismatch";
    return false;
  }
  for (const auto& group : request.group_ids) {
    if (!ContainsUuid(session.effective_group_uuids, group)) {
      if (detail != nullptr) *detail = "effective_group_id_mismatch";
      return false;
    }
  }
  if (detail != nullptr) *detail = "takeover_claims_match";
  return true;
}

struct TakeoverPhysicalChannelProjection {
  bool connection_changes = false;
  bool physical_channel_admitted = true;
  std::array<std::uint8_t, 16> server_channel_uuid{};
  bool transaction_routing_v2_negotiated = false;
  bool prepared_metadata_transfer_v1_negotiated = false;
  bool relation_descriptor_projection_v3_negotiated = false;
};

TakeoverPhysicalChannelProjection ProjectTakeoverPhysicalChannel(
    const ServerSessionRegistry& registry,
    const ServerSessionRecord& session,
    const ServerSessionTakeoverRequest& request) {
  TakeoverPhysicalChannelProjection projection;
  projection.connection_changes =
      (request.mask & kServerTakeoverClaimAttachmentId) != 0 &&
      !IsZeroUuidBytes(request.attachment_id) &&
      request.attachment_id != session.connection_uuid;
  if (!projection.connection_changes) { return projection; }

  const std::string connection_key = UuidBytesToText(request.attachment_id);
  const auto physical =
      registry.physical_channel_by_connection_uuid.find(connection_key);
  if (physical == registry.physical_channel_by_connection_uuid.end() ||
      IsZeroUuidBytes(physical->second)) {
    projection.physical_channel_admitted = false;
    return projection;
  }
  projection.server_channel_uuid = physical->second;

  // A physical channel without a capability record is admitted only with the
  // baseline/legacy surface. Never inherit capability bits from the source
  // connection or from the session being taken over.
  const auto negotiated =
      registry.negotiated_capabilities_by_connection_uuid.find(connection_key);
  if (negotiated ==
      registry.negotiated_capabilities_by_connection_uuid.end()) {
    return projection;
  }
  projection.transaction_routing_v2_negotiated =
      (negotiated->second[0] & sbps::kCapabilityTransactionRoutingV2) != 0;
  projection.prepared_metadata_transfer_v1_negotiated =
      (negotiated->second[0] &
       sbps::kCapabilityPreparedMetadataTransferV1) != 0;
  projection.relation_descriptor_projection_v3_negotiated =
      (negotiated->second[0] &
       sbps::kCapabilityRelationDescriptorProjectionV3) != 0;
  return projection;
}

void RetireTransferablePreparedBindingsForPhysicalChannelChange(
    ServerSessionRegistry* registry,
    const std::array<std::uint8_t, 16>& session_uuid) {
  if (registry == nullptr || IsZeroUuidBytes(session_uuid)) { return; }
  std::vector<std::array<std::uint8_t, 16>> prepared_to_retire;
  for (const auto& [_, prepared] : registry->prepared_by_uuid) {
    if (prepared.session_uuid != session_uuid ||
        (!prepared.prepared_metadata_transferable &&
         prepared.prepared_metadata_binding == nullptr)) {
      continue;
    }
    prepared_to_retire.push_back(prepared.prepared_statement_uuid);
  }
  for (const auto& prepared_uuid : prepared_to_retire) {
    (void)CloseServerPreparedStatement(
        registry,
        session_uuid,
        prepared_uuid,
        "prepared_metadata_binding_retired_by_physical_channel_takeover");
  }
}

}  // namespace

engine_api::EngineMaterializedAuthorizationContext
MaterializeDurableManagementAuthorizationContext(
    const ServerSessionRecord& session,
    const engine_api::EngineRequestContext& context) {
  const auto durable = MaterializeDurableAuthorizationForSession(session, context);
  if (!durable.ok) return {};
  return durable.context;
}

const char* ServerChannelStateName(ServerChannelState state) {
  switch (state) {
    case ServerChannelState::kProtocolAdmitted: return "protocol_admitted";
    case ServerChannelState::kAuthPending: return "auth_pending";
    case ServerChannelState::kAttachPending: return "attach_pending";
    case ServerChannelState::kSessionBound: return "session_bound";
    case ServerChannelState::kReady: return "ready";
    case ServerChannelState::kDraining: return "draining";
    case ServerChannelState::kDetached: return "detached";
    case ServerChannelState::kClosed: return "closed";
    case ServerChannelState::kFailed: return "failed";
  }
  return "failed";
}

const char* ServerRequestLifecycleStateName(ServerRequestLifecycleState state) {
  switch (state) {
    case ServerRequestLifecycleState::kAdmitted: return "admitted";
    case ServerRequestLifecycleState::kActive: return "active";
    case ServerRequestLifecycleState::kCursorOpen: return "cursor_open";
    case ServerRequestLifecycleState::kCompleted: return "completed";
    case ServerRequestLifecycleState::kCancelled: return "cancelled";
    case ServerRequestLifecycleState::kTimedOut: return "timed_out";
    case ServerRequestLifecycleState::kDrained: return "drained";
    case ServerRequestLifecycleState::kDisconnected: return "disconnected";
    case ServerRequestLifecycleState::kUnknownOutcome: return "unknown_outcome";
    case ServerRequestLifecycleState::kFailed: return "failed";
  }
  return "failed";
}

const char* ServerDriverTransactionEventName(ServerDriverTransactionEvent event) {
  switch (event) {
    case ServerDriverTransactionEvent::kAttachInitialBoundary:
      return "attach_initial_boundary";
    case ServerDriverTransactionEvent::kAutocommitStatementSucceeded:
      return "autocommit_statement_succeeded";
    case ServerDriverTransactionEvent::kAutocommitStatementFailed:
      return "autocommit_statement_failed";
    case ServerDriverTransactionEvent::kCommitCompleted: return "commit_completed";
    case ServerDriverTransactionEvent::kRollbackCompleted: return "rollback_completed";
    case ServerDriverTransactionEvent::kPrepareTransactionCompleted:
      return "prepare_transaction_completed";
    case ServerDriverTransactionEvent::kCancelStatement: return "cancel_statement";
    case ServerDriverTransactionEvent::kResetSession: return "reset_session";
    case ServerDriverTransactionEvent::kReconnectAfterDisconnect:
      return "reconnect_after_disconnect";
    case ServerDriverTransactionEvent::kPoolReturn: return "pool_return";
    case ServerDriverTransactionEvent::kSavepointOperation: return "savepoint_operation";
    case ServerDriverTransactionEvent::kXaRecoverPrepared: return "xa_recover_prepared";
    case ServerDriverTransactionEvent::kDormantDetach: return "dormant_detach";
    case ServerDriverTransactionEvent::kDormantReattach: return "dormant_reattach";
    case ServerDriverTransactionEvent::kRetryAfterUnknownFinality:
      return "retry_after_unknown_finality";
  }
  return "unknown_driver_transaction_event";
}

const char* ServerTransactionPressureActionName(ServerTransactionPressureAction action) {
  switch (action) {
    case ServerTransactionPressureAction::kNoAction:
      return "no_action";
    case ServerTransactionPressureAction::kWarnNotify:
      return "warn_notify";
    case ServerTransactionPressureAction::kRequestRestart:
      return "request_restart";
    case ServerTransactionPressureAction::kRequestReauth:
      return "request_reauth";
    case ServerTransactionPressureAction::kRequestCancel:
      return "request_cancel";
    case ServerTransactionPressureAction::kForceRollback:
      return "force_rollback";
    case ServerTransactionPressureAction::kForceCommit:
      return "force_commit";
    case ServerTransactionPressureAction::kForceRestart:
      return "force_restart";
  }
  return "unknown_transaction_pressure_action";
}

ServerDriverTransactionDecision ClassifyDriverTransactionEvent(
    const ServerSessionRecord& session,
    const ServerDriverTransactionDecisionInput& input) {
  const bool active_transaction = input.active_transaction || session.local_transaction_id != 0;
  auto decision = ServerDriverTransactionDecision{};
  decision.action = ServerDriverTransactionEventName(input.event);

  auto reject = [&](std::string code,
                    std::string message,
                    std::string detail,
                    std::string sqlstate,
                    std::string finality_state) {
    decision.accepted = false;
    decision.driver_may_retry = false;
    decision.hidden_retry_forbidden = true;
    decision.diagnostic_code = code;
    decision.sqlstate = sqlstate;
    decision.finality_state = finality_state;
    decision.must_query_engine_finality =
        finality_state == "unknown_until_engine_finality_report";
    decision.diagnostics.push_back(DriverTransactionDiagnostic(std::move(code),
                                                               std::move(message),
                                                               std::move(detail),
                                                               std::move(sqlstate),
                                                               std::move(finality_state)));
  };

  auto finality_unknown = [&]() {
    reject("SERVER.DRIVER_TX.FINALITY_UNKNOWN",
           "The driver-visible transaction outcome is unknown until engine MGA finality is queried.",
           "unknown_until_engine_finality_report",
           "08007",
           "unknown_until_engine_finality_report");
    decision.requires_explicit_engine_recovery = input.prepared_transaction_present;
  };

  auto require_active = [&]() -> bool {
    if (active_transaction) return true;
    reject("SERVER.DRIVER_TX.ACTIVE_TRANSACTION_REQUIRED",
           "The driver-visible operation requires an active engine-owned MGA transaction.",
           "active_transaction_required",
           "25000",
           "no_state_change");
    return false;
  };

  switch (input.event) {
    case ServerDriverTransactionEvent::kAttachInitialBoundary:
      decision.accepted = true;
      decision.opens_replacement_boundary = true;
      decision.boundary_state = "TX_BOUNDARY_ACTIVE";
      decision.durable_state = "TX_DURABLE_ACTIVE";
      decision.action = "open_initial_mga_boundary";
      return decision;

    case ServerDriverTransactionEvent::kAutocommitStatementSucceeded:
      if (!require_active()) return decision;
      if (!input.engine_finality_known) {
        finality_unknown();
        return decision;
      }
      decision.accepted = true;
      decision.opens_replacement_boundary = true;
      decision.boundary_state = "TX_BOUNDARY_ACTIVE";
      decision.durable_state = "TX_DURABLE_COMMITTED";
      decision.finality_state = "committed_by_engine_inventory";
      decision.action = "commit_statement_boundary_and_open_next";
      return decision;

    case ServerDriverTransactionEvent::kAutocommitStatementFailed:
      if (!require_active()) return decision;
      if (!input.engine_finality_known) {
        finality_unknown();
        return decision;
      }
      decision.accepted = true;
      decision.opens_replacement_boundary = true;
      decision.boundary_state = "TX_BOUNDARY_ACTIVE";
      decision.durable_state = "TX_DURABLE_ROLLED_BACK";
      decision.finality_state = "rolled_back_by_engine_inventory";
      decision.action = "rollback_statement_boundary_and_open_next";
      return decision;

    case ServerDriverTransactionEvent::kCommitCompleted:
      if (!require_active()) return decision;
      if (!input.engine_finality_known) {
        finality_unknown();
        return decision;
      }
      decision.accepted = true;
      decision.opens_replacement_boundary = true;
      decision.durable_state = "TX_DURABLE_COMMITTED";
      decision.finality_state = "committed_by_engine_inventory";
      decision.action = "commit_and_open_next_mga_boundary";
      return decision;

    case ServerDriverTransactionEvent::kRollbackCompleted:
      if (!require_active()) return decision;
      if (!input.engine_finality_known) {
        finality_unknown();
        return decision;
      }
      decision.accepted = true;
      decision.opens_replacement_boundary = true;
      decision.durable_state = "TX_DURABLE_ROLLED_BACK";
      decision.finality_state = "rolled_back_by_engine_inventory";
      decision.action = "rollback_and_open_next_mga_boundary";
      return decision;

    case ServerDriverTransactionEvent::kPrepareTransactionCompleted:
      if (!require_active()) return decision;
      if (!input.engine_finality_known) {
        finality_unknown();
        return decision;
      }
      decision.accepted = true;
      decision.opens_replacement_boundary = true;
      decision.boundary_state = "TX_BOUNDARY_PREPARED_HANDOFF";
      decision.durable_state = "TX_DURABLE_PREPARED";
      decision.finality_state = "prepared_by_engine_inventory";
      decision.action = "prepare_handoff_and_open_next_mga_boundary";
      return decision;

    case ServerDriverTransactionEvent::kCancelStatement:
      if (!input.engine_finality_known) {
        finality_unknown();
        decision.accepted = true;
        decision.preserves_current_boundary = active_transaction;
        decision.sqlstate = "57014";
        return decision;
      }
      decision.accepted = true;
      decision.preserves_current_boundary = active_transaction;
      decision.sqlstate = "57014";
      decision.finality_state = active_transaction
                                    ? "statement_aborted_transaction_state_reported_by_engine"
                                    : "no_state_change";
      decision.action = "cancel_statement_preserve_mga_boundary";
      return decision;

    case ServerDriverTransactionEvent::kResetSession:
      if (active_transaction || input.active_cursor) {
        reject("SERVER.DRIVER_TX.RESET_REQUIRES_CLEAN_BOUNDARY",
               "Session reset requires a clean engine-reported transaction boundary.",
               active_transaction ? "active_transaction_blocks_reset"
                                  : "active_cursor_blocks_reset",
               "25001",
               "no_state_change");
        return decision;
      }
      decision.accepted = true;
      decision.opens_replacement_boundary = true;
      decision.action = "reset_session_after_clean_engine_boundary";
      return decision;

    case ServerDriverTransactionEvent::kReconnectAfterDisconnect:
      decision.accepted = true;
      decision.invalidates_session = true;
      decision.opens_replacement_boundary = !input.explicit_dormant_token;
      decision.boundary_state = input.explicit_dormant_token ? "TX_BOUNDARY_DORMANT_RETAINED"
                                                            : "TX_BOUNDARY_ACTIVE";
      decision.action = input.explicit_dormant_token
                            ? "explicit_dormant_reattach_required"
                            : "new_connection_no_implicit_reattach";
      if (active_transaction || input.prepared_transaction_present || !input.engine_finality_known) {
        decision.must_query_engine_finality = true;
        decision.finality_state = "unknown_until_engine_finality_report";
        decision.requires_explicit_engine_recovery = input.prepared_transaction_present;
        decision.sqlstate = "08007";
      }
      return decision;

    case ServerDriverTransactionEvent::kPoolReturn:
      if (active_transaction || input.active_cursor) {
        reject("SERVER.DRIVER_TX.POOL_RETURN_REQUIRES_CLEAN_BOUNDARY",
               "A connection cannot return to a pool while transaction or cursor state is active.",
               active_transaction ? "active_transaction_blocks_pool_return"
                                  : "active_cursor_blocks_pool_return",
               "25001",
               "no_state_change");
        return decision;
      }
      decision.accepted = true;
      decision.action = "pool_return_after_commit_or_rollback_boundary";
      return decision;

    case ServerDriverTransactionEvent::kSavepointOperation:
      if (!require_active()) return decision;
      decision.accepted = true;
      decision.preserves_current_boundary = true;
      decision.action = "savepoint_is_transaction_local_no_independent_authority";
      return decision;

    case ServerDriverTransactionEvent::kXaRecoverPrepared:
      if (!input.prepared_transaction_present) {
        reject("SERVER.DRIVER_TX.PREPARED_TRANSACTION_NOT_FOUND",
               "Prepared transaction recovery did not find engine-owned prepared state.",
               "prepared_transaction_not_found",
               "42704",
               "no_state_change");
        return decision;
      }
      if (!input.xa_recovery_enabled || !input.cluster_authority_active) {
        reject("SERVER.DRIVER_TX.XA_LIMBO_RECOVERY_REQUIRED",
               "XA or prepared transaction recovery requires explicit engine recovery authority.",
               "prepared_or_limbo_recovery_requires_engine_authority",
               "08007",
               "unknown_until_engine_finality_report");
        decision.must_query_engine_finality = true;
        decision.requires_explicit_engine_recovery = true;
        return decision;
      }
      decision.accepted = true;
      decision.requires_explicit_engine_recovery = true;
      decision.finality_state = "decision_pending";
      decision.durable_state = "TX_DURABLE_PREPARED";
      decision.action = "prepared_recovery_delegated_to_engine_authority";
      return decision;

    case ServerDriverTransactionEvent::kDormantDetach:
      if (!require_active()) return decision;
      if (input.active_cursor) {
        reject("SERVER.DRIVER_TX.DORMANT_DETACH_CURSOR_ACTIVE",
               "Dormant detach requires all statement and cursor state to be closed.",
               "active_cursor_blocks_dormant_detach",
               "25001",
               "no_state_change");
        return decision;
      }
      if (!input.dormant_reattach_enabled) {
        reject("SERVER.DRIVER_TX.DORMANT_REATTACH_POLICY_REFUSED",
               "Dormant detach or reattach is disabled by engine policy.",
               "dormant_reattach_disabled",
               "08004",
               "no_state_change");
        return decision;
      }
      decision.accepted = true;
      decision.preserves_current_boundary = true;
      decision.boundary_state = "TX_BOUNDARY_DORMANT_RETAINED";
      decision.durable_state = "TX_DURABLE_DORMANT";
      decision.action = "explicit_dormant_detach_preserves_engine_transaction";
      return decision;

    case ServerDriverTransactionEvent::kDormantReattach:
      if (!input.explicit_dormant_token) {
        reject("SERVER.DRIVER_TX.DORMANT_REATTACH_TOKEN_REQUIRED",
               "Dormant reattach requires an explicit engine-issued dormant token.",
               "explicit_dormant_token_required",
               "08003",
               "no_state_change");
        return decision;
      }
      if (!input.dormant_reattach_enabled) {
        reject("SERVER.DRIVER_TX.DORMANT_REATTACH_POLICY_REFUSED",
               "Dormant detach or reattach is disabled by engine policy.",
               "dormant_reattach_disabled",
               "08004",
               "no_state_change");
        return decision;
      }
      if (!input.server_admitted_reattach) {
        reject("SERVER.DRIVER_TX.DORMANT_REATTACH_REFUSED",
               "The engine refused the dormant reattach request.",
               "dormant_reattach_not_admitted",
               "08003",
               "no_state_change");
        return decision;
      }
      decision.accepted = true;
      decision.preserves_current_boundary = true;
      decision.boundary_state = "TX_BOUNDARY_ACTIVE";
      decision.durable_state = "TX_DURABLE_ACTIVE";
      decision.action = "explicit_dormant_reattach_admitted_by_engine";
      return decision;

    case ServerDriverTransactionEvent::kRetryAfterUnknownFinality:
      if (!input.engine_finality_known) {
        reject("SERVER.DRIVER_TX.RETRY_REQUIRES_FINALITY_QUERY",
               "Retry after unknown finality requires an engine finality query first.",
               "retry_after_unknown_finality_forbidden",
               "08007",
               "unknown_until_engine_finality_report");
        decision.must_query_engine_finality = true;
        return decision;
      }
      if (input.statement_has_side_effects && !input.engine_reported_idempotent) {
        reject("SERVER.DRIVER_TX.HIDDEN_RETRY_FORBIDDEN",
               "The driver must not silently retry side-effecting work.",
               "side_effecting_retry_forbidden",
               "40003",
               "no_state_change");
        return decision;
      }
      if (!input.caller_acknowledged_retry_boundary) {
        reject("SERVER.DRIVER_TX.RETRY_REQUIRES_CALLER_ACK",
               "Retry requires caller acknowledgement of the fresh transaction boundary.",
               "caller_acknowledgement_required",
               "40003",
               "no_state_change");
        return decision;
      }
      decision.accepted = true;
      decision.driver_may_retry = true;
      decision.opens_replacement_boundary = true;
      decision.action = "caller_controlled_retry_at_fresh_mga_boundary";
      return decision;
  }

  reject("SERVER.DRIVER_TX.EVENT_UNKNOWN",
         "The driver transaction event is unknown.",
         "event_unknown",
         "HY000",
         "no_state_change");
  return decision;
}

ServerTransactionPressureControlDecision ClassifyServerTransactionPressureControl(
    const ServerSessionRecord& session,
    const ServerTransactionPressureControlInput& input) {
  ServerTransactionPressureControlDecision decision;
  const bool active_transaction =
      input.active_transaction ||
      input.current_local_transaction_id != 0 ||
      session.local_transaction_id != 0;
  const std::uint64_t current_local_transaction_id =
      input.current_local_transaction_id != 0
          ? input.current_local_transaction_id
          : session.local_transaction_id;

  auto evidence = [&](std::string action_name) {
    std::ostringstream out;
    out << "action=" << action_name
        << ";stable_session_id=" << input.stable_session_id
        << ";current_local_transaction_id=" << current_local_transaction_id
        << ";replacement_local_transaction_id=" << input.replacement_local_transaction_id
        << ";agent_authoritative=" << (input.agent_authoritative ? "true" : "false")
        << ";policy_authorized=" << (input.policy_authorized ? "true" : "false")
        << ";session_authorization_bound="
        << (input.session_authorization_bound ? "true" : "false")
        << ";mga_finality_authority=engine"
        << ";parser_finality_authority=false"
        << ";client_state_authority=false";
    if (!input.evidence_id.empty()) {
      out << ";agent_evidence_id=" << input.evidence_id;
    }
    return out.str();
  };

  auto finish = [&](bool accepted,
                    std::string code,
                    std::string message,
                    std::string detail,
                    bool mutates_transaction,
                    bool opens_replacement_boundary) {
    decision.accepted = accepted;
    decision.diagnostic_code = code;
    decision.detail = detail;
    decision.mutates_transaction = mutates_transaction;
    decision.opens_replacement_boundary = opens_replacement_boundary;
    decision.evidence = evidence(ServerTransactionPressureActionName(input.action));
    if (opens_replacement_boundary) {
      decision.evidence += ";always_active_transaction_replacement=true";
      decision.evidence += ";replacement_transaction_rule=must_open_before_client_ready";
    }
    decision.diagnostics.push_back(TransactionPressureDiagnostic(
        std::move(code),
        std::move(message),
        std::move(detail),
        mutates_transaction,
        opens_replacement_boundary));
    return decision;
  };

  auto deny_non_authoritative = [&](std::string detail) {
    decision.denied_non_authoritative = true;
    return finish(false,
                  "SERVER.TRANSACTION_PRESSURE.DENIED_NON_AUTHORITATIVE",
                  "The transaction pressure action lacked server-authoritative session, policy, or transaction evidence.",
                  std::move(detail),
                  false,
                  false);
  };

  if (!input.agent_authoritative) {
    return deny_non_authoritative("agent_authoritative_required");
  }
  if (!input.policy_authorized) {
    return deny_non_authoritative("policy_authorized_required");
  }
  if (!input.session_authorization_bound) {
    return deny_non_authoritative("session_bound_authorization_required");
  }
  if (input.action != ServerTransactionPressureAction::kNoAction &&
      !active_transaction) {
    return finish(false,
                  "SERVER.TRANSACTION_PRESSURE.ACTIVE_TRANSACTION_REQUIRED",
                  "Transaction pressure action requires an active engine-owned transaction.",
                  "active_transaction_required",
                  false,
                  false);
  }

  switch (input.action) {
    case ServerTransactionPressureAction::kNoAction:
      return finish(true,
                    "SERVER.TRANSACTION_PRESSURE.NO_ACTION",
                    "The transaction pressure manager took no action.",
                    "no_action",
                    false,
                    false);
    case ServerTransactionPressureAction::kWarnNotify:
      decision.notifies_client = true;
      return finish(true,
                    "SERVER.TRANSACTION_PRESSURE.WARN_NOTIFY",
                    "The server may notify the session about long idle transaction pressure.",
                    "warn_notify",
                    false,
                    false);
    case ServerTransactionPressureAction::kRequestRestart:
      decision.requests_client_action = true;
      return finish(true,
                    "SERVER.TRANSACTION_PRESSURE.REQUEST_RESTART",
                    "The server may request a client-visible transaction restart.",
                    "request_restart",
                    false,
                    false);
    case ServerTransactionPressureAction::kRequestReauth:
      decision.requests_client_action = true;
      return finish(true,
                    "SERVER.TRANSACTION_PRESSURE.REQUEST_REAUTH",
                    "The server may request session reauthentication before transaction pressure action.",
                    "request_reauth",
                    false,
                    false);
    case ServerTransactionPressureAction::kRequestCancel:
      decision.requests_client_action = true;
      return finish(true,
                    "SERVER.TRANSACTION_PRESSURE.REQUEST_CANCEL",
                    "The server may request cancellation of the transaction pressure blocker.",
                    "request_cancel",
                    false,
                    false);
    case ServerTransactionPressureAction::kForceRollback:
    case ServerTransactionPressureAction::kForceCommit:
    case ServerTransactionPressureAction::kForceRestart:
      break;
  }

  if (!input.engine_finality_known) {
    decision.must_query_engine_finality = true;
    return finish(false,
                  "SERVER.TRANSACTION_PRESSURE.FINALITY_UNKNOWN",
                  "Forced transaction pressure action requires known engine finality.",
                  "unknown_until_engine_finality_report",
                  false,
                  false);
  }
  if (!input.force_authority_gate) {
    return deny_non_authoritative("force_authority_gate_required");
  }
  if (!input.replacement_transaction_bound ||
      input.replacement_local_transaction_id == 0 ||
      input.replacement_local_transaction_id == current_local_transaction_id) {
    return finish(false,
                  "SERVER.TRANSACTION_PRESSURE.REPLACEMENT_TRANSACTION_REQUIRED",
                  "Forced transaction pressure action must bind a replacement transaction before client-ready state.",
                  "replacement_transaction_required",
                  false,
                  false);
  }

  switch (input.action) {
    case ServerTransactionPressureAction::kForceRollback:
      return finish(true,
                    "SERVER.TRANSACTION_PRESSURE.FORCE_ROLLBACK_REPLACEMENT",
                    "The server accepted a policy-authorized forced rollback with replacement transaction binding.",
                    "force_rollback_replacement",
                    true,
                    true);
    case ServerTransactionPressureAction::kForceCommit:
      return finish(true,
                    "SERVER.TRANSACTION_PRESSURE.FORCE_COMMIT_REPLACEMENT",
                    "The server accepted a policy-authorized forced commit with replacement transaction binding.",
                    "force_commit_replacement",
                    true,
                    true);
    case ServerTransactionPressureAction::kForceRestart:
      return finish(true,
                    "SERVER.TRANSACTION_PRESSURE.FORCE_RESTART_REPLACEMENT",
                    "The server accepted a policy-authorized forced restart with replacement transaction binding.",
                    "force_restart_replacement",
                    true,
                    true);
    case ServerTransactionPressureAction::kNoAction:
    case ServerTransactionPressureAction::kWarnNotify:
    case ServerTransactionPressureAction::kRequestRestart:
    case ServerTransactionPressureAction::kRequestReauth:
    case ServerTransactionPressureAction::kRequestCancel:
      break;
  }
  return finish(false,
                "SERVER.TRANSACTION_PRESSURE.UNKNOWN_ACTION",
                "The transaction pressure action was unknown.",
                "unknown_action",
                false,
                false);
}

std::string ServerDriverTransactionDecisionJson(
    const ServerDriverTransactionDecision& decision) {
  std::ostringstream out;
  out << "{\"accepted\":" << (decision.accepted ? "true" : "false")
      << ",\"driver_may_retry\":" << (decision.driver_may_retry ? "true" : "false")
      << ",\"hidden_retry_forbidden\":"
      << (decision.hidden_retry_forbidden ? "true" : "false")
      << ",\"must_query_engine_finality\":"
      << (decision.must_query_engine_finality ? "true" : "false")
      << ",\"opens_replacement_boundary\":"
      << (decision.opens_replacement_boundary ? "true" : "false")
      << ",\"preserves_current_boundary\":"
      << (decision.preserves_current_boundary ? "true" : "false")
      << ",\"invalidates_session\":" << (decision.invalidates_session ? "true" : "false")
      << ",\"requires_explicit_engine_recovery\":"
      << (decision.requires_explicit_engine_recovery ? "true" : "false")
      << ",\"sqlstate\":\"" << JsonEscape(decision.sqlstate)
      << "\",\"diagnostic_code\":\"" << JsonEscape(decision.diagnostic_code)
      << "\",\"finality_state\":\"" << JsonEscape(decision.finality_state)
      << "\",\"boundary_state\":\"" << JsonEscape(decision.boundary_state)
      << "\",\"durable_state\":\"" << JsonEscape(decision.durable_state)
      << "\",\"action\":\"" << JsonEscape(decision.action) << "\"}";
  return out.str();
}

std::string UuidBytesToText(const std::array<std::uint8_t, 16>& uuid) {
  static constexpr char hex[] = "0123456789abcdef";
  std::string out;
  out.reserve(36);
  for (std::size_t i = 0; i < uuid.size(); ++i) {
    if (i == 4 || i == 6 || i == 8 || i == 10) out.push_back('-');
    out.push_back(hex[(uuid[i] >> 4u) & 0x0fu]);
    out.push_back(hex[uuid[i] & 0x0fu]);
  }
  return out;
}

bool IsCompleteEngineTransactionIdentity(
    std::uint64_t local_transaction_id,
    std::string_view transaction_uuid) {
  if (local_transaction_id == 0 || transaction_uuid.size() != 36) {
    return false;
  }
  const auto parsed = TextToUuid(transaction_uuid);
  if (IsZeroUuidBytes(parsed)) return false;
  return UuidBytesToText(parsed) == LowerAscii(std::string(transaction_uuid));
}

std::vector<std::uint8_t> EncodeAuthHandoffPayloadForTest(const std::string& principal,
                                                          bool credential_valid,
                                                          bool mfa_required,
                                                          bool mfa_present,
                                                          const std::string& principal_uuid,
                                                          const std::string& storage_authority) {
  std::vector<std::uint8_t> out;
  PutUuid(&out, sbps::MakeUuidV7Bytes());
  out.push_back(credential_valid ? 1 : 0);
  out.push_back(credential_valid ? 0 : 1);
  out.push_back(mfa_required ? 1 : 0);
  out.push_back(mfa_present ? 1 : 0);
  PutString(&out, "local_password");
  PutString(&out, principal);
  PutString(&out, "default");
  PutString(&out, "en");
  const std::string verifier = credential_valid
      ? "aaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaa"
      : "bbbbbbbbbbbbbbbbbbbbbbbbbbbbbbbbbbbbbbbbbbbbbbbbbbbbbbbbbbbbbbbb";
  std::string evidence = "scheme=local_password_v1;principal=" + principal;
  if (!principal_uuid.empty()) {
    evidence += ";principal_uuid=" + principal_uuid;
    evidence += ";storage_authority=" +
                (storage_authority.empty() ? std::string("mga_security_principal_lifecycle")
                                           : storage_authority);
    evidence += ";authorization_tags=right:CONNECT,right:OBS_MANAGEMENT_CONTROL";
  }
  evidence += ";verifier=" + verifier;
  PutString(&out, evidence);
  return out;
}

std::vector<std::uint8_t> EncodeAttachPayloadForTest(
    const std::array<std::uint8_t, 16>& auth_context_uuid,
    const std::string& mode) {
  std::vector<std::uint8_t> out;
  PutUuid(&out, sbps::MakeUuidV7Bytes());
  PutUuid(&out, auth_context_uuid);
  PutString(&out, "default");
  PutString(&out, mode);
  return out;
}

std::optional<std::array<std::uint8_t, 16>> DecodeAuthContextUuidForTest(
    const std::vector<std::uint8_t>& auth_result_payload) {
  std::size_t offset = 0;
  std::string outcome;
  if (!ReadString(auth_result_payload, &offset, &outcome)) return std::nullopt;
  if (offset + 16 > auth_result_payload.size()) return std::nullopt;
  return GetUuid(auth_result_payload, offset);
}

std::optional<std::array<std::uint8_t, 16>> DecodeSessionUuidForTest(
    const std::vector<std::uint8_t>& attach_result_payload) {
  std::size_t offset = 0;
  std::string outcome;
  if (!ReadString(attach_result_payload, &offset, &outcome)) return std::nullopt;
  if (offset + 16 > attach_result_payload.size()) return std::nullopt;
  return GetUuid(attach_result_payload, offset);
}

ServerRequestRecord RegisterServerRequestLifecycle(ServerSessionRegistry* registry,
                                                   const sbps::Frame& request,
                                                   const ServerSessionRecord& session,
                                                   std::string request_kind,
                                                   std::string operation_id,
                                                   const ServerTransactionState* transaction) {
  ServerRequestRecord record;
  record.request_uuid = sbps::IsZeroUuid(request.header.request_uuid)
                            ? sbps::MakeUuidV7Bytes()
                            : request.header.request_uuid;
  record.finality_token_uuid = sbps::MakeUuidV7Bytes();
  record.session_uuid = session.session_uuid;
  record.auth_context_uuid = session.auth_context_uuid;
  record.request_kind = std::move(request_kind);
  record.operation_id = std::move(operation_id);
  record.state = ServerRequestLifecycleState::kActive;
  record.detail = "request_admitted";
  record.local_transaction_id_at_start = session.local_transaction_id;
  record.snapshot_visible_through_local_transaction_id =
      session.snapshot_visible_through_local_transaction_id;
  record.transaction_uuid_at_start = session.transaction_uuid;
  if (transaction != nullptr) {
    record.local_transaction_id_at_start = transaction->local_transaction_id;
    record.snapshot_visible_through_local_transaction_id =
        transaction->snapshot_visible_through_local_transaction_id;
    record.transaction_uuid_at_start = transaction->transaction_uuid;
  }
  record.authorization_proven = true;
  if (registry != nullptr) {
    registry->requests_by_uuid[UuidBytesToText(record.request_uuid)] = record;
    UpsertRequestFinality(registry, record);
  }
  return record;
}

void UpdateServerRequestLifecycleOperation(
    ServerSessionRegistry* registry,
    const std::array<std::uint8_t, 16>& request_uuid,
    std::string operation_id) {
  if (registry == nullptr) return;
  auto found = registry->requests_by_uuid.find(UuidBytesToText(request_uuid));
  if (found == registry->requests_by_uuid.end()) return;
  found->second.operation_id = std::move(operation_id);
  UpsertRequestFinality(registry, found->second);
}

void LinkServerRequestPreparedStatement(
    ServerSessionRegistry* registry,
    const std::array<std::uint8_t, 16>& request_uuid,
    const std::array<std::uint8_t, 16>& prepared_statement_uuid) {
  if (registry == nullptr) return;
  auto found = registry->requests_by_uuid.find(UuidBytesToText(request_uuid));
  if (found == registry->requests_by_uuid.end()) return;
  found->second.prepared_statement_uuid = prepared_statement_uuid;
  UpsertRequestFinality(registry, found->second);
}

std::string SessionObjectHandleKey(
    const std::array<std::uint8_t, 16>& session_uuid,
    std::uint64_t handle_id) {
  return UuidBytesToText(session_uuid) + "#" + std::to_string(handle_id);
}

ServerSessionObjectHandleRecord AllocateSessionObjectHandle(
    ServerSessionRegistry* registry,
    const ServerSessionRecord& session,
    std::string object_uuid,
    std::string object_kind,
    std::string operation_id,
    std::string column_set_hash) {
  ServerSessionObjectHandleRecord handle;
  if (registry == nullptr || object_uuid.empty() || operation_id.empty()) {
    return handle;
  }
  for (auto& [_, existing] : registry->object_handles_by_key) {
    if (existing.session_uuid == session.session_uuid &&
        existing.object_uuid == object_uuid &&
        existing.operation_id == operation_id &&
        existing.column_set_hash ==
            (column_set_hash.empty() ? "columns/all" : column_set_hash)) {
      if (!existing.closed &&
          existing.auth_context_uuid == session.auth_context_uuid &&
          existing.principal_uuid == session.principal_uuid &&
          existing.effective_user_uuid == session.effective_user_uuid &&
          existing.database_uuid == session.database_uuid &&
          existing.catalog_generation == session.catalog_generation &&
          existing.security_epoch == session.security_epoch &&
          existing.descriptor_epoch == session.descriptor_epoch &&
          existing.grant_epoch == session.grant_epoch &&
          existing.policy_generation == session.policy_generation &&
          existing.role_set_hash == session.role_set_hash &&
          existing.group_set_hash == session.group_set_hash &&
          existing.search_path_hash == session.search_path_hash) {
        return existing;
      }
      existing.closed = false;
      ++existing.generation;
      existing.auth_context_uuid = session.auth_context_uuid;
      existing.principal_uuid = session.principal_uuid;
      existing.effective_user_uuid = session.effective_user_uuid;
      existing.database_uuid = session.database_uuid;
      if (!object_kind.empty()) {
        existing.object_kind = object_kind;
      }
      existing.catalog_generation = session.catalog_generation;
      existing.security_epoch = session.security_epoch;
      existing.descriptor_epoch = session.descriptor_epoch;
      existing.grant_epoch = session.grant_epoch;
      existing.policy_generation = session.policy_generation;
      existing.role_set_hash = session.role_set_hash;
      existing.group_set_hash = session.group_set_hash;
      existing.search_path_hash = session.search_path_hash;
      return existing;
    }
  }
  handle.handle_id = registry->next_session_object_handle_id++;
  handle.generation = 1;
  handle.session_uuid = session.session_uuid;
  handle.auth_context_uuid = session.auth_context_uuid;
  handle.principal_uuid = session.principal_uuid;
  handle.effective_user_uuid = session.effective_user_uuid;
  handle.database_uuid = session.database_uuid;
  handle.object_uuid = std::move(object_uuid);
  handle.object_kind = object_kind.empty() ? "object" : std::move(object_kind);
  handle.operation_id = std::move(operation_id);
  handle.column_set_hash = column_set_hash.empty() ? "columns/all" : std::move(column_set_hash);
  handle.catalog_generation = session.catalog_generation;
  handle.security_epoch = session.security_epoch;
  handle.descriptor_epoch = session.descriptor_epoch;
  handle.grant_epoch = session.grant_epoch;
  handle.policy_generation = session.policy_generation;
  handle.role_set_hash = session.role_set_hash;
  handle.group_set_hash = session.group_set_hash;
  handle.search_path_hash = session.search_path_hash;
  registry->object_handles_by_key[SessionObjectHandleKey(session.session_uuid,
                                                         handle.handle_id)] = handle;
  return handle;
}

ServerSessionObjectHandleValidation ValidateSessionObjectHandle(
    const ServerSessionRegistry& registry,
    const ServerSessionRecord& session,
    std::uint64_t handle_id,
    std::uint64_t generation,
    const std::string& object_uuid,
    const std::string& operation_id,
    const std::string& column_set_hash) {
  ServerSessionObjectHandleValidation result;
  if (handle_id == 0 || generation == 0) {
    result.detail = "session_object_handle_missing";
    return result;
  }
  const auto found =
      registry.object_handles_by_key.find(SessionObjectHandleKey(session.session_uuid,
                                                                 handle_id));
  if (found == registry.object_handles_by_key.end()) {
    result.detail = "session_object_handle_not_found";
    return result;
  }
  const auto& handle = found->second;
  result.handle = &handle;
  if (handle.closed) {
    result.detail = "session_object_handle_closed";
    return result;
  }
  if (handle.generation != generation) {
    result.detail = "session_object_handle_generation_stale";
    return result;
  }
  if (handle.session_uuid != session.session_uuid ||
      handle.auth_context_uuid != session.auth_context_uuid) {
    result.detail = "session_object_handle_authority_stale";
    return result;
  }
  if (handle.principal_uuid != session.principal_uuid ||
      handle.effective_user_uuid != session.effective_user_uuid) {
    result.detail = "session_object_handle_user_stale";
    return result;
  }
  if (handle.database_uuid != session.database_uuid) {
    result.detail = "session_object_handle_database_stale";
    return result;
  }
  if (handle.object_uuid != object_uuid ||
      (!operation_id.empty() && handle.operation_id != operation_id)) {
    result.detail = "session_object_handle_shape_mismatch";
    return result;
  }
  if (!column_set_hash.empty() && handle.column_set_hash != column_set_hash) {
    result.detail = "session_object_handle_column_hash_stale";
    return result;
  }
  if (handle.catalog_generation != session.catalog_generation ||
      handle.security_epoch != session.security_epoch ||
      handle.descriptor_epoch != session.descriptor_epoch ||
      handle.grant_epoch != session.grant_epoch ||
      handle.policy_generation != session.policy_generation) {
    result.detail = "session_object_handle_epoch_stale";
    return result;
  }
  if (handle.role_set_hash != session.role_set_hash ||
      handle.group_set_hash != session.group_set_hash ||
      handle.search_path_hash != session.search_path_hash) {
    result.detail = "session_object_handle_authorization_stale";
    return result;
  }
  result.accepted = true;
  result.detail = "session_object_handle_valid";
  return result;
}

void CloseSessionObjectHandlesForSession(
    ServerSessionRegistry* registry,
    const std::array<std::uint8_t, 16>& session_uuid,
    std::string) {
  if (registry == nullptr) return;
  for (auto& [_, handle] : registry->object_handles_by_key) {
    if (handle.session_uuid == session_uuid && !handle.closed) {
      handle.closed = true;
      ++handle.generation;
    }
  }
}

bool ReleaseAndClearServerCursorResources(
    ServerSessionRegistry* registry,
    ServerCursorRecord* cursor) {
  if (cursor == nullptr) return false;
  const bool released_engine_result =
      ReleaseServerCursorExecutionAuthority(registry, cursor);
  cursor->row_packet.clear();
  cursor->bulk_stream_kind.clear();
  cursor->bulk_reject_records.clear();
  cursor->multi_result_kind.clear();
  cursor->warning_stream_kind.clear();
  cursor->bulk_total_rows = 0;
  cursor->bulk_rejected_rows = 0;
  cursor->multi_result_count = 0;
  cursor->warning_count = 0;
  cursor->partial_result_rows = 0;
  cursor->finality_after_fetches = 0;
  cursor->total_row_count = 0;
  cursor->next_row_index = 0;
  cursor->fetch_count = 0;
  cursor->stream_descriptor_live = false;
  cursor->stream_descriptor_receipt_binding_sha256.fill(0);
  return released_engine_result;
}

bool ReleaseServerCursorExecutionAuthority(
    ServerSessionRegistry* registry,
    ServerCursorRecord* cursor) {
  if (cursor == nullptr) return false;
  const bool released_engine_result = cursor->engine_result != nullptr;
  if (cursor->engine_result != nullptr) {
    (void)sb_engine_result_release(cursor->engine_result);
    cursor->engine_result = nullptr;
  }

  if (registry != nullptr &&
      !cursor->statement_context_statement_uuid.empty()) {
    (void)ReleaseServerStatementContext(
        registry, cursor->statement_context_statement_uuid);
    cursor->statement_context_statement_uuid.clear();
  }

  if (registry != nullptr && !sbps::IsZeroUuid(cursor->cursor_uuid)) {
    for (auto& [_, request] : registry->requests_by_uuid) {
      if (request.cursor_uuid != cursor->cursor_uuid ||
          !request.engine_result_retained) {
        continue;
      }
      request.engine_result_retained = false;
      UpsertRequestFinality(registry, request);
    }
  }
  return released_engine_result;
}

ServerPreparedStatementCloseSummary CloseServerPreparedStatement(
    ServerSessionRegistry* registry,
    const std::array<std::uint8_t, 16>& session_uuid,
    const std::array<std::uint8_t, 16>& prepared_statement_uuid,
    std::string detail) {
  ServerPreparedStatementCloseSummary summary;
  if (registry == nullptr || sbps::IsZeroUuid(session_uuid) ||
      sbps::IsZeroUuid(prepared_statement_uuid)) {
    return summary;
  }

  const std::string prepared_key = UuidBytesToText(prepared_statement_uuid);
  const auto prepared_it = registry->prepared_by_uuid.find(prepared_key);
  if (prepared_it == registry->prepared_by_uuid.end() ||
      prepared_it->second.session_uuid != session_uuid) {
    // Ownership failures deliberately collapse to not-found so one session
    // cannot probe prepared identities belonging to another session.
    return summary;
  }

  auto& prepared = prepared_it->second;
  summary.found = true;
  summary.already_closed = prepared.closed;
  prepared.closed = true;
  if (prepared.prepared_metadata_binding != nullptr) {
    (void)engine_bridge::ReleasePreparedMetadataBinding(
        prepared.prepared_metadata_binding);
  }
  prepared.prepared_metadata_binding = nullptr;
  prepared.prepared_metadata_transferable = false;
  // Preserve the opaque identity, owner, operation, and immutable prepare
  // selector as tombstone evidence.  The executable envelope and derived
  // execution context are no longer needed once the resource is retired.
  prepared.encoded_sblr_envelope.clear();
  registry->prepared_execution_contexts_by_uuid.erase(prepared_key);

  if (detail.empty()) detail = "prepared_statement_closed";
  for (auto& [_, cursor] : registry->cursors_by_uuid) {
    if (cursor.session_uuid != session_uuid ||
        cursor.prepared_statement_uuid != prepared_statement_uuid) {
      continue;
    }
    const bool cursor_was_closed = cursor.closed;
    if (ReleaseAndClearServerCursorResources(registry, &cursor)) {
      ++summary.engine_results_released;
    }
    if (!cursor_was_closed) {
      ++summary.cursors_closed;
      cursor.finality_state = "prepared_statement_closed";
      cursor.finality_reason = detail;
    }
    cursor.closed = true;
    cursor.exhausted = true;
    if (!cursor_was_closed) {
      MarkServerRequestClosedByCursor(registry,
                                      cursor.cursor_uuid,
                                      ServerRequestLifecycleState::kCompleted,
                                      detail);
    }
  }

  if (prepared.session_object_handle_id != 0 &&
      prepared.session_object_handle_generation != 0) {
    bool shared_by_live_prepared = false;
    for (const auto& [other_key, other] : registry->prepared_by_uuid) {
      if (other_key == prepared_key || other.closed ||
          other.session_uuid != session_uuid) {
        continue;
      }
      if (other.session_object_handle_id ==
              prepared.session_object_handle_id &&
          other.session_object_handle_generation ==
              prepared.session_object_handle_generation) {
        shared_by_live_prepared = true;
        break;
      }
    }
    if (!shared_by_live_prepared) {
      const auto handle_it = registry->object_handles_by_key.find(
          SessionObjectHandleKey(session_uuid,
                                 prepared.session_object_handle_id));
      if (handle_it != registry->object_handles_by_key.end() &&
          !handle_it->second.closed &&
          handle_it->second.generation ==
              prepared.session_object_handle_generation) {
        handle_it->second.closed = true;
        ++handle_it->second.generation;
        summary.session_object_handle_revoked = true;
      }
    }
  }
  return summary;
}

std::uint64_t ReleaseServerStatementContextsForSession(
    ServerSessionRegistry* registry,
    const std::array<std::uint8_t, 16>& session_uuid) {
  if (registry == nullptr || registry->statement_context_mutex == nullptr) {
    return 0;
  }
  std::vector<engine_bridge::StatementContextReceiptHandle> receipts;
  {
    std::lock_guard<std::mutex> guard(*registry->statement_context_mutex);
    for (auto it = registry->statement_contexts_by_statement_uuid.begin();
         it != registry->statement_contexts_by_statement_uuid.end();) {
      if (it->second.session_uuid != session_uuid) {
        ++it;
        continue;
      }
      if (!it->second.released && it->second.receipt) {
        receipts.push_back(it->second.receipt);
      }
      it = registry->statement_contexts_by_statement_uuid.erase(it);
    }
  }
  std::uint64_t released = 0;
  for (const auto receipt : receipts) {
    const auto status = engine_bridge::ReleaseStatementContextReceipt(receipt);
    if (status == SB_ENGINE_STATUS_OK ||
        status == SB_ENGINE_STATUS_ALREADY_RELEASED) {
      ++released;
    }
  }
  return released;
}

bool ReleaseServerStatementContext(
    ServerSessionRegistry* registry,
    std::string_view statement_uuid) {
  if (registry == nullptr || registry->statement_context_mutex == nullptr ||
      statement_uuid.empty()) {
    return false;
  }
  engine_bridge::StatementContextReceiptHandle receipt;
  {
    std::lock_guard<std::mutex> guard(*registry->statement_context_mutex);
    const auto found = registry->statement_contexts_by_statement_uuid.find(
        std::string(statement_uuid));
    if (found == registry->statement_contexts_by_statement_uuid.end()) {
      return false;
    }
    if (!found->second.released && found->second.receipt) {
      receipt = found->second.receipt;
    }
    registry->statement_contexts_by_statement_uuid.erase(found);
  }
  if (!receipt) return false;
  const auto status = engine_bridge::ReleaseStatementContextReceipt(receipt);
  return status == SB_ENGINE_STATUS_OK ||
         status == SB_ENGINE_STATUS_ALREADY_RELEASED;
}

void CloseServerPublicAbiSessionForSession(
    ServerSessionRegistry* registry,
    const std::array<std::uint8_t, 16>& session_uuid,
    std::string) {
  if (registry == nullptr) return;
  (void)ReleaseServerStatementContextsForSession(registry, session_uuid);
  const auto key = UuidBytesToText(session_uuid);
  auto it = registry->public_abi_sessions_by_session_uuid.find(key);
  if (it == registry->public_abi_sessions_by_session_uuid.end()) {
    return;
  }
  auto& context = it->second;
  for (auto& [_, prepared] : registry->prepared_by_uuid) {
    if (prepared.session_uuid != session_uuid ||
        prepared.prepared_metadata_binding == nullptr) {
      continue;
    }
    (void)engine_bridge::ReleasePreparedMetadataBinding(
        prepared.prepared_metadata_binding);
    prepared.prepared_metadata_binding = nullptr;
    prepared.prepared_metadata_transferable = false;
  }
  if (context.engine_session != nullptr) {
    sb_engine_session_end_params_v1_t end_params{};
    end_params.struct_size = sizeof(end_params);
    end_params.abi_version = SB_ENGINE_ABI_VERSION_PACKED;
    end_params.rollback_active_transactions = 1;
    end_params.cancel_open_results = 1;
    (void)sb_engine_session_end(context.engine_session, &end_params, nullptr);
    context.engine_session = nullptr;
  }
  if (context.engine != nullptr) {
    (void)sb_engine_close(context.engine, nullptr);
    context.engine = nullptr;
  }
  registry->public_abi_sessions_by_session_uuid.erase(it);
}

ServerPublicAbiSessionContext* EnsureServerPublicAbiSessionForContext(
    ServerSessionRegistry* registry,
    const ServerSessionRecord& session,
    std::string* diagnostic_detail) {
  if (registry == nullptr) {
    if (diagnostic_detail != nullptr) {
      *diagnostic_detail = "session_registry_missing";
    }
    return nullptr;
  }

  const std::string session_key = UuidBytesToText(session.session_uuid);
  auto cached_it =
      registry->public_abi_sessions_by_session_uuid.find(session_key);
  if (cached_it != registry->public_abi_sessions_by_session_uuid.end()) {
    const auto& cached = cached_it->second;
    const bool cache_matches =
        cached.engine != nullptr && cached.engine_session != nullptr &&
        cached.database_path == session.database_path &&
        cached.session_uuid == session.session_uuid &&
        cached.effective_user_uuid == session.effective_user_uuid &&
        cached.embedded_in_process == session.embedded_in_process;
    if (!cache_matches) {
      CloseServerPublicAbiSessionForSession(
          registry,
          session.session_uuid,
          "public_abi_context_authority_changed");
      cached_it = registry->public_abi_sessions_by_session_uuid.end();
    }
  }

  if (cached_it == registry->public_abi_sessions_by_session_uuid.end()) {
    sb_engine_open_params_v1_t open_params{};
    open_params.struct_size = sizeof(open_params);
    open_params.abi_version = SB_ENGINE_ABI_VERSION_PACKED;
    open_params.database_path_utf8 = session.database_path.data();
    open_params.database_path_size =
        static_cast<std::uint64_t>(session.database_path.size());
    open_params.mode = SB_ENGINE_OPEN_VALIDATION_ONLY;
    sb_engine_handle_t engine = nullptr;
    if (sb_engine_open(&open_params, &engine, nullptr) !=
            SB_ENGINE_STATUS_OK ||
        engine == nullptr) {
      if (diagnostic_detail != nullptr) {
        *diagnostic_detail = "engine_open_failed";
      }
      return nullptr;
    }

    sb_engine_session_params_v1_t session_params{};
    session_params.struct_size = sizeof(session_params);
    session_params.abi_version = SB_ENGINE_ABI_VERSION_PACKED;
    std::memcpy(session_params.effective_user_uuid.bytes,
                session.effective_user_uuid.data(),
                16);
    std::memcpy(session_params.session_uuid.bytes,
                session.session_uuid.data(),
                16);
    session_params.trust_mode =
        session.embedded_in_process ? SB_ENGINE_TRUST_EMBEDDED_TRUSTED
                                    : SB_ENGINE_TRUST_SERVER_ISOLATED;
    session_params.default_language_utf8 = "en";
    session_params.default_language_size = 2;
    sb_engine_session_t engine_session = nullptr;
    if (sb_engine_session_begin(engine,
                                &session_params,
                                &engine_session,
                                nullptr) != SB_ENGINE_STATUS_OK ||
        engine_session == nullptr) {
      (void)sb_engine_close(engine, nullptr);
      if (diagnostic_detail != nullptr) {
        *diagnostic_detail = "engine_session_begin_failed";
      }
      return nullptr;
    }

    ServerPublicAbiSessionContext cached_context;
    cached_context.engine = engine;
    cached_context.engine_session = engine_session;
    cached_context.database_path = session.database_path;
    cached_context.session_uuid = session.session_uuid;
    cached_context.effective_user_uuid = session.effective_user_uuid;
    cached_context.embedded_in_process = session.embedded_in_process;
    cached_it = registry->public_abi_sessions_by_session_uuid
                    .emplace(session_key, std::move(cached_context))
                    .first;
  }
  return &cached_it->second;
}

SessionOperationResult HandleAcquireStatementContext(
    ServerSessionRegistry* registry,
    const HostedEngineState& engine_state,
    const sbps::Frame& request) {
  SessionOperationResult result;
  result.response_message_type = static_cast<std::uint16_t>(
      sbps::MessageType::kAcquireStatementContextResult);
  const bool native_projection_v2 =
      request.header.payload_schema_id ==
          sbps::kSchemaAcquireStatementContextRequestV2;
  const bool native_projection_v3 =
      request.header.payload_schema_id ==
          sbps::kSchemaAcquireStatementContextRequestV3;
  const bool native_projection_v4 =
      request.header.payload_schema_id ==
          sbps::kSchemaAcquireStatementContextRequestV4;
  const bool native_projection_v5 =
      request.header.payload_schema_id ==
          sbps::kSchemaAcquireStatementContextRequestV5;
  const bool native_projection_v6 =
      request.header.payload_schema_id ==
          sbps::kSchemaAcquireStatementContextRequestV6;
  const bool native_projection_v7 =
      request.header.payload_schema_id ==
          sbps::kSchemaAcquireStatementContextRequestV7;
  const bool native_projection_v8 =
      request.header.payload_schema_id ==
          sbps::kSchemaAcquireStatementContextRequestV8;
  const bool native_projection_v9 =
      request.header.payload_schema_id ==
          sbps::kSchemaAcquireStatementContextRequestV9;
  const bool native_projection_v10 =
      request.header.payload_schema_id ==
          sbps::kSchemaAcquireStatementContextRequestV10 ||
      request.header.payload_schema_id ==
          sbps::kSchemaAcquireStatementContextRequestV11;
  const bool native_projection_v11 =
      request.header.payload_schema_id ==
          sbps::kSchemaAcquireStatementContextRequestV11 ||
      request.header.payload_schema_id ==
          sbps::kSchemaAcquireParameterStatementContextRequestV1 ||
      request.header.payload_schema_id ==
          sbps::kSchemaAcquireVariableStatementContextRequestV1;
  const bool native_projection =
      native_projection_v2 || native_projection_v3 || native_projection_v4 ||
      native_projection_v5 || native_projection_v6 || native_projection_v7 ||
      native_projection_v8 || native_projection_v9 || native_projection_v10 ||
      native_projection_v11;
  std::uint16_t projection_version = 1;
  result.response_schema_id = sbps::kSchemaAcquireStatementContextResultV1;
  if (native_projection_v11) {
    projection_version = 11;
    result.response_schema_id = sbps::kSchemaAcquireStatementContextResultV11;
  } else if (native_projection_v2) {
    projection_version = 2;
    result.response_schema_id = sbps::kSchemaAcquireStatementContextResultV2;
  } else if (native_projection_v3) {
    projection_version = 3;
    result.response_schema_id = sbps::kSchemaAcquireStatementContextResultV3;
  } else if (native_projection_v4) {
    projection_version = 4;
    result.response_schema_id = sbps::kSchemaAcquireStatementContextResultV4;
  } else if (native_projection_v5) {
    projection_version = 5;
    result.response_schema_id = sbps::kSchemaAcquireStatementContextResultV5;
  } else if (native_projection_v6) {
    projection_version = 6;
    result.response_schema_id = sbps::kSchemaAcquireStatementContextResultV6;
  } else if (native_projection_v7) {
    projection_version = 7;
    result.response_schema_id = sbps::kSchemaAcquireStatementContextResultV7;
  } else if (native_projection_v8) {
    projection_version = 8;
    result.response_schema_id = sbps::kSchemaAcquireStatementContextResultV8;
  } else if (native_projection_v9) {
    projection_version = 9;
    result.response_schema_id = sbps::kSchemaAcquireStatementContextResultV9;
  } else if (native_projection_v10) {
    projection_version = 10;
    result.response_schema_id = sbps::kSchemaAcquireStatementContextResultV10;
  }
  result.frame_flags = sbps::kFlagResponse | sbps::kFlagFinal;
  result.session_uuid = request.header.session_uuid;
  const auto refuse = [&](std::string code, std::string detail) {
    result.accepted = false;
    result.frame_flags |= sbps::kFlagError;
    result.diagnostics.push_back(sbps::IpcDiagnostic(
        std::move(code),
        "parser_server_ipc.statement_context_acquire_refused",
        "The server refused statement-context acquisition.",
        {{"detail", std::move(detail)}}));
    return result;
  };

  constexpr std::size_t kRequestBytes = 2 + 16 + 8 + 16;
  const bool parameter_prepared_acquire =
      request.header.payload_schema_id ==
          sbps::kSchemaAcquireParameterStatementContextRequestV1;
  const bool variable_frame_acquire =
      request.header.payload_schema_id ==
          sbps::kSchemaAcquireVariableStatementContextRequestV1;
  const std::size_t expected_request_bytes =
      kRequestBytes + (parameter_prepared_acquire ? 36 :
                       variable_frame_acquire ? 44 : 0);
  if (registry == nullptr ||
      (request.header.payload_schema_id !=
           sbps::kSchemaAcquireStatementContextRequestV1 &&
       !native_projection) ||
      request.payload.size() != expected_request_bytes ||
      GetU16(request.payload, 0) != projection_version) {
    return refuse("PARSER_SERVER_IPC.STATEMENT_CONTEXT_REQUEST_INVALID",
                  "schema_version_or_size_invalid");
  }
  const auto payload_session_uuid = GetUuid(request.payload, 2);
  const auto local_transaction_id = GetU64(request.payload, 18);
  const auto transaction_uuid_bytes = GetUuid(request.payload, 26);
  if (payload_session_uuid != request.header.session_uuid ||
      local_transaction_id == 0 ||
      sbps::IsZeroUuid(transaction_uuid_bytes)) {
    return refuse("PARSER_SERVER_IPC.STATEMENT_CONTEXT_SELECTOR_INVALID",
                  "exact_session_and_transaction_selector_required");
  }

  const auto session_it = registry->sessions_by_uuid.find(
      UuidBytesToText(request.header.session_uuid));
  if (session_it == registry->sessions_by_uuid.end()) {
    return refuse("PARSER_SERVER_IPC.STATEMENT_CONTEXT_SESSION_NOT_FOUND",
                  "session_not_found");
  }
  auto& session = session_it->second;
  ServerTransactionState transaction;
  {
    if (session.transaction_mutex == nullptr) {
      return refuse("PARSER_SERVER_IPC.STATEMENT_CONTEXT_TRANSACTION_INVALID",
                    "transaction_mutex_missing");
    }
    std::lock_guard<std::mutex> guard(*session.transaction_mutex);
    const auto transaction_it =
        session.transactions_by_local_id.find(local_transaction_id);
    const auto transaction_uuid = UuidBytesToText(transaction_uuid_bytes);
    if (transaction_it == session.transactions_by_local_id.end() ||
        transaction_it->second.lifecycle_state !=
            ServerTransactionLifecycleState::kActive ||
        transaction_it->second.transaction_uuid != transaction_uuid) {
      return refuse("PARSER_SERVER_IPC.STATEMENT_CONTEXT_TRANSACTION_INVALID",
                    "exact_active_transaction_not_owned_by_session");
    }
    transaction = transaction_it->second;
  }

  std::string ensure_detail;
  auto* public_context = EnsureServerPublicAbiSessionForContext(
      registry, session, &ensure_detail);
  if (public_context == nullptr || public_context->engine_session == nullptr) {
    return refuse("PARSER_SERVER_IPC.STATEMENT_CONTEXT_ENGINE_SESSION_MISSING",
                  ensure_detail.empty() ? "engine_session_missing"
                                        : std::move(ensure_detail));
  }

  auto engine_context = EngineContextForSession(session, engine_state, request);
  // The SBPS request UUID identifies transport work only. Statement identity
  // and every statement-stable authority value are issued by the engine as
  // part of this acquisition and therefore must enter the private bridge
  // empty.
  engine_context.statement_uuid.canonical.clear();
  engine_context.statement_snapshot_uuid.canonical.clear();
  engine_context.statement_metadata_snapshot_engine_owned = false;
  engine_context.statement_metadata_snapshot_uuid.canonical.clear();
  engine_context.catalog_epoch_uuid.canonical.clear();
  engine_context.optimizer_capability_snapshot_uuid.canonical.clear();
  engine_context.optimizer_resource_snapshot_uuid.canonical.clear();
  engine_context.optimizer_route_snapshot_uuid.canonical.clear();
  engine_context.statement_timestamp.clear();
  engine_context.current_timestamp.clear();
  engine_context.local_transaction_id = transaction.local_transaction_id;
  engine_context.transaction_uuid.canonical = transaction.transaction_uuid;
  engine_context.snapshot_visible_through_local_transaction_id =
      transaction.snapshot_visible_through_local_transaction_id;
  engine_context.transaction_timestamp = transaction.transaction_timestamp;
  engine_bridge::StatementContextAcquireRequest acquire_request;
  acquire_request.engine_context = &engine_context;
  acquire_request.exact_transaction_uuid = transaction.transaction_uuid;
  if (parameter_prepared_acquire) {
    constexpr std::size_t kSuffix = kRequestBytes;
    if (GetU16(request.payload, kSuffix) != 1 ||
        request.payload[kSuffix + 2] != 1 ||
        request.payload[kSuffix + 3] != 0 ||
        sbps::IsZeroUuid(GetUuid(request.payload, kSuffix + 4)) ||
        sbps::IsZeroUuid(GetUuid(request.payload, kSuffix + 20))) {
      return refuse("SBLR.OPERAND_INVALID",
                    "parameter_execution_selector_invalid");
    }
    const auto coordination_uuid =
        UuidBytesToText(GetUuid(request.payload, kSuffix + 4));
    const auto operation_uuid =
        UuidBytesToText(GetUuid(request.payload, kSuffix + 20));
    std::lock_guard<std::mutex> guard(*registry->statement_context_mutex);
    const auto found =
        registry->parameter_coordinations_by_uuid.find(coordination_uuid);
    if (found == registry->parameter_coordinations_by_uuid.end() ||
        found->second.session_uuid != UuidBytesToText(session.session_uuid) ||
        found->second.operation_uuid != operation_uuid) {
      return refuse("SECURITY.ACCESS_DENIED",
                    "parameter_execution_coordination_hidden");
    }
    if (found->second.sealed || found->second.private_handle == 0) {
      return refuse("SBLR.PARAMETER.STALE",
                    "parameter_execution_coordination_stale");
    }
    acquire_request.parameter_execution_selector.version = 1;
    acquire_request.parameter_execution_selector.mode =
        engine_bridge::StatementParameterExecutionMode::kPrepared;
    acquire_request.parameter_execution_selector.reserved = 0;
    acquire_request.parameter_execution_selector.prepared_binding_handle =
        found->second.private_handle;
  }
  if (variable_frame_acquire) {
    constexpr std::size_t kSuffix = kRequestBytes;
    if (GetU16(request.payload,kSuffix)!=1 ||
        request.payload[kSuffix+2]!=1 || request.payload[kSuffix+3]!=0 ||
        sbps::IsZeroUuid(GetUuid(request.payload,kSuffix+4)) ||
        sbps::IsZeroUuid(GetUuid(request.payload,kSuffix+20)) ||
        GetU64(request.payload,kSuffix+36)==0) {
      return refuse("SBLR.OPERAND_INVALID","variable_frame_selector_invalid");
    }
    acquire_request.variable_frame_selector.version=1;
    acquire_request.variable_frame_selector.public_coordination_uuid=
        UuidBytesToText(GetUuid(request.payload,kSuffix+4));
    acquire_request.variable_frame_selector.operation_uuid=
        UuidBytesToText(GetUuid(request.payload,kSuffix+20));
    acquire_request.variable_frame_selector.expected_coordinator_generation=
        GetU64(request.payload,kSuffix+36);
  }
  engine_bridge::StatementContextReceiptHandle receipt;
  engine_bridge::StatementContextReceiptView view;
  sb_engine_result_t engine_result = nullptr;
  const auto status = engine_bridge::AcquireStatementContextReceipt(
      public_context->engine_session,
      &acquire_request,
      &receipt,
      &view,
      &engine_result);
  if (engine_result != nullptr) {
    (void)sb_engine_result_release(engine_result);
  }
  if (status != SB_ENGINE_STATUS_OK || !receipt ||
      view.statement_uuid.empty()) {
    return refuse("PARSER_SERVER_IPC.STATEMENT_CONTEXT_ENGINE_REFUSED",
                  std::string("engine_status=") +
                      sb_engine_status_name(status));
  }
  if (variable_frame_acquire) {
    const auto coordination_uuid=UuidBytesToText(GetUuid(request.payload,
        kRequestBytes+4));
    std::lock_guard<std::mutex> guard(*registry->statement_context_mutex);
    const auto found=registry->variable_frames_by_coordination_uuid.find(
        coordination_uuid);
    if(found==registry->variable_frames_by_coordination_uuid.end()||
       found->second.acquired||found->second.revoked){
      (void)engine_bridge::ReleaseStatementContextReceipt(receipt);
      return refuse("SBLR.VARIABLE.STALE","variable_frame_publish_race");
    }
    found->second.acquired=true;
  }
  if ((native_projection_v7 || native_projection_v8 || native_projection_v9 ||
       native_projection_v10 || native_projection_v11) &&
      !IsCanonicalStatementTimestamp(view.statement_timestamp)) {
    (void)engine_bridge::ReleaseStatementContextReceipt(receipt);
    return refuse("PARSER_SERVER_IPC.STATEMENT_CONTEXT_ENGINE_REFUSED",
                  "engine_statement_timestamp_invalid");
  }

  ServerStatementContextRecord record;
  record.session_uuid = session.session_uuid;
  record.statement_uuid = view.statement_uuid;
  record.owning_local_transaction_id = transaction.local_transaction_id;
  record.owning_transaction_uuid = transaction.transaction_uuid;
  record.receipt = receipt;
  record.view = view;
  {
    std::lock_guard<std::mutex> guard(*registry->statement_context_mutex);
    if (!registry->statement_contexts_by_statement_uuid
             .emplace(record.statement_uuid, std::move(record))
             .second) {
      (void)engine_bridge::ReleaseStatementContextReceipt(receipt);
      return refuse("PARSER_SERVER_IPC.STATEMENT_CONTEXT_COLLISION",
                    "statement_identity_already_owned");
    }
  }

  PutU16(&result.payload, projection_version);
  result.payload.push_back(1);
  PutUuid(&result.payload, TextToUuid(view.statement_uuid));
  PutU64(&result.payload, view.owning_local_transaction_id);
  PutUuid(&result.payload, TextToUuid(view.owning_transaction_uuid));
  PutUuid(&result.payload, TextToUuid(view.statement_snapshot_uuid));
  PutUuid(&result.payload,
          TextToUuid(view.statement_metadata_snapshot_uuid));
  PutUuid(&result.payload, TextToUuid(view.catalog_epoch_uuid));
  PutUuid(&result.payload, TextToUuid(view.security_context_uuid));
  PutU64(&result.payload, view.visible_committed_high_watermark);
  if (native_projection_v7 || native_projection_v8 || native_projection_v9 ||
      native_projection_v10 || native_projection_v11) {
    PutString(&result.payload, view.statement_timestamp);
  }
  if (native_projection) {
    PutUuid(&result.payload, TextToUuid(view.bound_ast_uuid));
    PutUuid(&result.payload, TextToUuid(view.count_function_uuid));
    PutUuid(&result.payload, TextToUuid(view.sum_function_uuid));
    if (native_projection_v3 || native_projection_v4 ||
        native_projection_v5 || native_projection_v6 ||
        native_projection_v7 || native_projection_v8 ||
        native_projection_v9 || native_projection_v10 ||
        native_projection_v11) {
      PutUuid(&result.payload, TextToUuid(view.avg_function_uuid));
      PutUuid(&result.payload, TextToUuid(view.min_function_uuid));
      PutUuid(&result.payload, TextToUuid(view.max_function_uuid));
    }
    if (native_projection_v4 || native_projection_v5 ||
        native_projection_v6 || native_projection_v7 ||
        native_projection_v8 || native_projection_v9 ||
        native_projection_v10 || native_projection_v11) {
      PutU16(&result.payload, static_cast<std::uint16_t>(
                                  view.aggregate_function_profiles.size()));
      for (const auto& function : view.aggregate_function_profiles) {
        PutU16(&result.payload, function.abi_version);
        PutString(&result.payload, function.builtin_id);
        PutUuid(&result.payload, TextToUuid(function.function_uuid));
        result.payload.push_back(function.executable ? 1 : 0);
      }
    }
    if (native_projection_v6 || native_projection_v7 ||
        native_projection_v8 || native_projection_v9 ||
        native_projection_v10 || native_projection_v11) {
      PutU16(&result.payload, static_cast<std::uint16_t>(
                                  view.window_function_profiles.size()));
      for (const auto& function : view.window_function_profiles) {
        PutU16(&result.payload, function.abi_version);
        PutString(&result.payload, function.builtin_id);
        PutUuid(&result.payload, TextToUuid(function.function_uuid));
        result.payload.push_back(function.executable ? 1 : 0);
      }
    }
    const auto maximum_profile_kind =
        (native_projection_v10 || native_projection_v11)
            ? static_cast<std::uint8_t>(23)
            : native_projection_v9
            ? static_cast<std::uint8_t>(13)
            : native_projection_v8
            ? static_cast<std::uint8_t>(11)
            : (native_projection_v5 || native_projection_v6 ||
                       native_projection_v7
                   ? static_cast<std::uint8_t>(10)
                   : static_cast<std::uint8_t>(6));
    const auto descriptor_profile_count = static_cast<std::uint16_t>(
        std::count_if(view.descriptor_profiles.begin(),
                      view.descriptor_profiles.end(),
                      [&](const auto& profile) {
                        return static_cast<std::uint8_t>(
                                   profile.profile_kind) <=
                               maximum_profile_kind;
                      }));
    PutU16(&result.payload, descriptor_profile_count);
    for (const auto& profile : view.descriptor_profiles) {
      if (static_cast<std::uint8_t>(profile.profile_kind) >
          maximum_profile_kind) {
        continue;
      }
      result.payload.push_back(
          static_cast<std::uint8_t>(profile.profile_kind));
      PutU16(&result.payload, profile.slot);
      PutUuid(&result.payload, TextToUuid(profile.descriptor_uuid));
      PutUuid(&result.payload, TextToUuid(profile.type_uuid));
      PutUuid(&result.payload, TextToUuid(profile.collation_uuid));
      result.payload.push_back(profile.nullable ? 1 : 0);
      PutU32(&result.payload, profile.width);
      PutU32(&result.payload, profile.precision);
      PutU32(&result.payload, profile.scale);
    }
  }
  if (native_projection_v11) {
    // Unified literal/parameter preliminary bootstrap extension v3. Parameter
    // lifecycle pairs come only from the immutable engine receipt view.
    PutU16(&result.payload, 58);
    PutU16(&result.payload, 0);
    PutUuid(&result.payload, TextToUuid(view.receipt_uuid));
    PutUuid(&result.payload,
            TextToUuid(view.literal_catalog_snapshot_uuid));
    PutU64(&result.payload, view.literal_catalog_generation);
    PutU64(&result.payload, view.security_epoch);
    PutU64(&result.payload, view.resource_epoch);
    PutUuid(&result.payload, TextToUuid(view.statement_snapshot_uuid));
    PutUuid(&result.payload,
            TextToUuid(view.parameter_prepared_statement_uuid));
    PutU64(&result.payload, view.parameter_prepared_generation);
    PutUuid(&result.payload, TextToUuid(view.parameter_batch_uuid));
    PutU64(&result.payload, view.parameter_batch_generation);
    PutUuid(&result.payload,
            TextToUuid(view.parameter_dynamic_package_uuid));
    PutU64(&result.payload, view.parameter_dynamic_generation);
    PutU64(&result.payload,
           view.parameter_executor_availability_generation);
    PutUuid(&result.payload, TextToUuid(view.variable_scope_uuid));
    PutU64(&result.payload, view.variable_scope_generation);
    PutUuid(&result.payload, TextToUuid(view.variable_frame_uuid));
    PutU64(&result.payload, view.variable_frame_generation);
    PutUuid(&result.payload,
            TextToUuid(view.variable_registry_snapshot_uuid));
    PutU64(&result.payload,
           view.variable_executor_availability_generation);
    PutUuid(&result.payload,
            TextToUuid(view.diagnostic_registry_snapshot_uuid));
    PutU64(&result.payload, view.diagnostic_registry_generation);
    PutU32(&result.payload,
           static_cast<std::uint32_t>(view.diagnostic_identity_rows.size()));
    PutU32(&result.payload, 72);
    const auto append_sha = [&](std::string_view value) {
      auto digit=[](char c){return c>='0'&&c<='9'?c-'0':c>='a'&&c<='f'?c-'a'+10:-1;};
      if(value.size()!=71||value.substr(0,7)!="sha256:"){result.payload.insert(result.payload.end(),32,0);return;}
      for(std::size_t i=0;i<32;++i){const auto a=digit(value[7+i*2]),b=digit(value[8+i*2]);result.payload.push_back(static_cast<std::uint8_t>((a<<4)|b));}
    };
    for (const auto& row : view.diagnostic_identity_rows) {
      PutUuid(&result.payload, TextToUuid(row.diagnostic_uuid));
      PutU64(&result.payload, row.diagnostic_generation);
      PutU32(&result.payload, row.precedence_ordinal);
      result.payload.push_back(row.severity_code);
      result.payload.push_back(row.redaction_class);
      PutU16(&result.payload, 0);
      PutU32(&result.payload, row.maximum_safe_field_count);
      PutU32(&result.payload, 0);
      append_sha(row.row_identity_sha256);
    }
    PutUuid(&result.payload,
            TextToUuid(view.txn_begin_isolation_profile_uuid));
    PutU64(&result.payload, view.txn_begin_isolation_profile_generation);
    PutUuid(&result.payload,
            TextToUuid(view.txn_begin_policy_snapshot_uuid));
    PutU64(&result.payload, view.txn_begin_policy_generation);
    PutU64(&result.payload,
           view.txn_begin_executor_availability_generation);
    PutU64(&result.payload, 0);
    result.payload.push_back(view.txn_begin_read_mode);
    result.payload.push_back(view.txn_begin_authority_scope);
    result.payload.push_back(view.txn_begin_wait_policy);
    result.payload.insert(result.payload.end(), 5, 0);
    PutU64(&result.payload, view.txn_begin_deadline_monotonic_ns);
    PutU64(&result.payload,
           view.txn_commit_executor_availability_generation);
    result.payload.push_back(view.txn_commit_mode);
    result.payload.push_back(view.txn_commit_authority_scope);
    result.payload.push_back(view.txn_commit_wait_policy);
    result.payload.insert(result.payload.end(), 5, 0);
    PutU64(&result.payload, view.txn_commit_deadline_monotonic_ns);
    PutU64(&result.payload,view.txn_rollback_executor_availability_generation);result.payload.push_back(view.txn_rollback_mode);result.payload.push_back(view.txn_rollback_authority_scope);result.payload.push_back(view.txn_rollback_wait_policy);result.payload.insert(result.payload.end(),5,0);PutU64(&result.payload,view.txn_rollback_deadline_monotonic_ns);
    PutU64(&result.payload,view.txn_release_savepoint_executor_availability_generation);
    PutU64(&result.payload,view.txn_rollback_to_savepoint_executor_availability_generation);
    PutU64(&result.payload,view.psql_autonomous_frame_executor_availability_generation);
    PutU64(&result.payload,view.transaction_reservation_release_executor_availability_generation);
    PutU64(&result.payload,view.temporary_instance_cleanup_executor_availability_generation);
    PutU64(&result.payload,view.cursor_open_executor_availability_generation);
    PutU64(&result.payload,view.cursor_fetch_executor_availability_generation);
    PutU64(&result.payload,view.cursor_close_executor_availability_generation);
    PutU64(&result.payload,view.read_by_key_executor_availability_generation);
    PutU64(&result.payload,view.read_range_executor_availability_generation);
    PutU64(&result.payload,view.read_stream_executor_availability_generation);
    PutU64(&result.payload,view.result_set_pass_executor_availability_generation);
    PutU64(&result.payload,view.access_cursor_open_executor_availability_generation);
    PutU64(&result.payload,view.access_cursor_fetch_executor_availability_generation);
    PutU64(&result.payload,view.access_cursor_close_executor_availability_generation);
    PutU64(&result.payload,view.insert_executor_availability_generation);
    PutU64(&result.payload,view.update_executor_availability_generation);
    PutU64(&result.payload,view.delete_executor_availability_generation);
    PutU64(&result.payload,view.merge_executor_availability_generation);
    PutU64(&result.payload,view.table_truncate_executor_availability_generation);
    PutU64(&result.payload,view.table_analyze_executor_availability_generation);
    PutU64(&result.payload,view.bulk_import_stream_executor_availability_generation);
    PutU64(&result.payload,view.bulk_export_stream_executor_availability_generation);
    PutU64(&result.payload,view.statement_batch_executor_availability_generation);
    PutU64(&result.payload,view.atomic_cas_executor_availability_generation);
    PutU64(&result.payload,view.atomic_rmw_executor_availability_generation);
    PutU64(&result.payload,view.advisory_lock_acquire_executor_availability_generation);
    PutU64(&result.payload,view.advisory_lock_release_executor_availability_generation);
    PutU64(&result.payload,view.function_call_executor_availability_generation);
    PutU64(&result.payload,view.operator_call_executor_availability_generation);
    PutU64(&result.payload,view.cast_executor_availability_generation);
    PutU64(&result.payload,view.compare_executor_availability_generation);
    PutU64(&result.payload,view.domain_operation_executor_availability_generation);
    PutU64(&result.payload,view.udr_invoke_executor_availability_generation);
    PutU64(&result.payload,view.procedure_invoke_executor_availability_generation);
    PutU64(&result.payload,view.function_invoke_executor_availability_generation);
    PutU64(&result.payload,view.aggregate_invoke_executor_availability_generation);
    PutU64(&result.payload,view.sequence_nextval_executor_availability_generation);
    PutU64(&result.payload,view.sequence_currval_executor_availability_generation);
    PutU64(&result.payload,view.sequence_setval_executor_availability_generation);
    PutU64(&result.payload,view.query_numeric_executor_availability_generation);
    PutU64(&result.payload,view.advanced_datatype_family_executor_availability_generation);
    PutU64(&result.payload,view.project_executor_availability_generation);
    PutU64(&result.payload,view.aggregate_executor_availability_generation);
    PutU64(&result.payload,view.group_executor_availability_generation);
    PutU64(&result.payload,view.sort_executor_availability_generation);
    PutU64(&result.payload,view.limit_executor_availability_generation);
    PutU64(&result.payload,view.window_executor_availability_generation);
    PutU64(&result.payload,view.return_result_set_executor_availability_generation);
    PutU64(&result.payload,view.kv_structured_read_executor_availability_generation);
  }
  result.accepted = true;
  ++public_context->reuse_count;
  return result;
}

SessionOperationResult HandleNegotiateLiteralDescriptors(
    ServerSessionRegistry* registry,const sbps::Frame& request){
  SessionOperationResult result;
  result.response_message_type=static_cast<std::uint16_t>(
      sbps::MessageType::kNegotiateLiteralDescriptorsResult);
  result.response_schema_id=sbps::kSchemaNegotiateLiteralDescriptorsResultV1;
  result.frame_flags=sbps::kFlagResponse|sbps::kFlagFinal;
  result.session_uuid=request.header.session_uuid;
  const auto refuse=[&](std::string code,std::string detail){
    result.accepted=false;result.frame_flags|=sbps::kFlagError;
    result.diagnostics.push_back(sbps::IpcDiagnostic(
        std::move(code),"parser_server_ipc.literal_prebind_refused",
        "Literal descriptor negotiation was refused.",
        {{"detail",std::move(detail)}}));return result;};
  if(registry==nullptr||request.header.payload_schema_id!=
       sbps::kSchemaNegotiateLiteralDescriptorsRequestV1||
     request.payload.size()<128||request.payload[0]!='S'||request.payload[1]!='B'||
     request.payload[2]!='L'||request.payload[3]!='N')
    return refuse("SBLR.OPERAND_INVALID","literal_prebind_frame_invalid");
  const auto preliminary_uuid=UuidBytesToText(GetUuid(request.payload,16));
  engine_bridge::StatementContextReceiptHandle receipt;
  {
    std::lock_guard<std::mutex> guard(*registry->statement_context_mutex);
    const auto found=std::find_if(
        registry->statement_contexts_by_statement_uuid.begin(),
        registry->statement_contexts_by_statement_uuid.end(),
        [&](const auto& entry){return entry.second.view.receipt_uuid==preliminary_uuid&&
            entry.second.session_uuid==request.header.session_uuid;});
    if(found==registry->statement_contexts_by_statement_uuid.end())
      return refuse("DATATYPE.DESCRIPTOR_INVALID","preliminary_receipt_not_live");
    receipt=found->second.receipt;
  }
  sb_engine_result_t engine_result=nullptr;
  const auto status=engine_bridge::NegotiateStatementLiteralDescriptorsV1(
      receipt,request.payload,&result.payload,&engine_result);
  if(engine_result!=nullptr)(void)sb_engine_result_release(engine_result);
  if(status!=SB_ENGINE_STATUS_OK)
    return refuse(status==SB_ENGINE_STATUS_UNSUPPORTED?
                      "DATATYPE.DESCRIPTOR_INVALID":"DATATYPE.DESCRIPTOR_INVALID",
                  std::string("engine_status=")+sb_engine_status_name(status));
  result.accepted=true;return result;
}

SessionOperationResult HandleFinalizeLiteralBinding(
    ServerSessionRegistry* registry,const sbps::Frame& request){
  SessionOperationResult result;result.response_message_type=41;result.response_schema_id=sbps::kSchemaFinalizeLiteralBindingResultV1;result.frame_flags=sbps::kFlagResponse|sbps::kFlagFinal;result.session_uuid=request.header.session_uuid;
  const auto refuse=[&](std::string code,std::string detail){result.accepted=false;result.frame_flags|=sbps::kFlagError;result.diagnostics.push_back(sbps::IpcDiagnostic(std::move(code),"parser_server_ipc.literal_finalize_refused","Literal binding finalization was refused.",{{"detail",std::move(detail)}}));return result;};
  if(registry==nullptr||request.header.payload_schema_id!=sbps::kSchemaFinalizeLiteralBindingRequestV1||request.payload.size()<208||request.payload.size()>1036600||request.payload[0]!='S'||request.payload[1]!='B'||request.payload[2]!='L'||request.payload[3]!='F')return refuse("SBLR.OPERAND_INVALID","literal_finalize_frame_invalid");
  const auto preliminary_uuid=UuidBytesToText(GetUuid(request.payload,16));engine_bridge::StatementContextReceiptHandle receipt;
  {std::lock_guard<std::mutex> guard(*registry->statement_context_mutex);const auto found=std::find_if(registry->statement_contexts_by_statement_uuid.begin(),registry->statement_contexts_by_statement_uuid.end(),[&](const auto& entry){return entry.second.view.receipt_uuid==preliminary_uuid&&entry.second.session_uuid==request.header.session_uuid;});if(found==registry->statement_contexts_by_statement_uuid.end())return refuse("DATATYPE.DESCRIPTOR_INVALID","preliminary_receipt_not_live");receipt=found->second.receipt;}
  sb_engine_result_t engine_result=nullptr;const auto status=engine_bridge::FinalizeStatementLiteralBindingV1(receipt,request.payload,&result.payload,&engine_result);if(engine_result)(void)sb_engine_result_release(engine_result);if(status!=SB_ENGINE_STATUS_OK)return refuse("DATATYPE.DESCRIPTOR_INVALID",std::string("engine_status=")+sb_engine_status_name(status));result.accepted=true;return result;
}

SessionOperationResult HandleNegotiateParameterDescriptors(
    ServerSessionRegistry* registry, const sbps::Frame& request) {
  SessionOperationResult result;
  result.response_message_type = 43;
  result.response_schema_id = sbps::kSchemaNegotiateParameterDescriptorsResultV1;
  result.frame_flags = sbps::kFlagResponse | sbps::kFlagFinal;
  result.session_uuid = request.header.session_uuid;
  const auto refuse = [&](std::string code, std::string detail) {
    result.accepted = false; result.frame_flags |= sbps::kFlagError;
    result.diagnostics.push_back(sbps::IpcDiagnostic(
        std::move(code), "parser_server_ipc.parameter_prebind_refused",
        "Parameter descriptor negotiation was refused.",
        {{"detail", std::move(detail)}}));
    return result;
  };
  if (registry == nullptr || request.header.payload_schema_id !=
          sbps::kSchemaNegotiateParameterDescriptorsRequestV1 ||
      request.payload.size() < 112 || request.payload.size() > 98416 ||
      !std::equal(request.payload.begin(), request.payload.begin() + 4,
                  "SBPR")) {
    return refuse("SBLR.OPERAND_INVALID", "parameter_prebind_frame_invalid");
  }
  const auto preliminary_uuid = UuidBytesToText(GetUuid(request.payload, 16));
  engine_bridge::StatementContextReceiptHandle receipt;
  {
    std::lock_guard<std::mutex> guard(*registry->statement_context_mutex);
    const auto found = std::find_if(
        registry->statement_contexts_by_statement_uuid.begin(),
        registry->statement_contexts_by_statement_uuid.end(),
        [&](const auto& entry) {
          return entry.second.view.receipt_uuid == preliminary_uuid &&
                 entry.second.session_uuid == request.header.session_uuid;
        });
    if (found == registry->statement_contexts_by_statement_uuid.end())
      return refuse("DATATYPE.DESCRIPTOR_INVALID", "preliminary_receipt_not_live");
    receipt = found->second.receipt;
  }
  sb_engine_result_t engine_result = nullptr;
  const auto status = engine_bridge::NegotiateStatementParameterDescriptorsV1(
      receipt, request.payload, &result.payload, &engine_result);
  if (engine_result) (void)sb_engine_result_release(engine_result);
  if (status != SB_ENGINE_STATUS_OK)
    return refuse("DATATYPE.DESCRIPTOR_INVALID",
                  std::string("engine_status=") + sb_engine_status_name(status));
  result.accepted = true; return result;
}

SessionOperationResult HandleBeginParameterExecutionCoordination(
    ServerSessionRegistry* registry, const HostedEngineState& engine_state,
    const sbps::Frame& request) {
  SessionOperationResult result;
  result.response_message_type = 51;
  result.response_schema_id =
      sbps::kSchemaBeginParameterExecutionCoordinationResultV1;
  result.frame_flags = sbps::kFlagResponse | sbps::kFlagFinal;
  result.session_uuid = request.header.session_uuid;
  const auto refuse = [&](std::string code, std::string detail) {
    result.accepted = false;
    result.frame_flags |= sbps::kFlagError;
    result.diagnostics.push_back(sbps::IpcDiagnostic(
        std::move(code), "parser_server_ipc.parameter_coordination_refused",
        "Parameter execution coordination was refused.",
        {{"detail", std::move(detail)}}));
    return result;
  };
  if (registry == nullptr || request.header.payload_schema_id !=
          sbps::kSchemaBeginParameterExecutionCoordinationRequestV1 ||
      request.payload.size() != 72 || GetU16(request.payload, 0) != 1 ||
      request.payload[2] != 1 || request.payload[3] != 0 ||
      GetUuid(request.payload, 4) != request.header.session_uuid ||
      sbps::IsZeroUuid(GetUuid(request.payload, 20)) ||
      !sbps::IsZeroUuid(GetUuid(request.payload, 52)) ||
      request.payload[68] != 0 || request.payload[69] != 0 ||
      request.payload[70] != 0 || request.payload[71] != 0) {
    return refuse("SBLR.OPERAND_INVALID", "prepared_begin_frame_invalid");
  }
  const auto session_it = registry->sessions_by_uuid.find(
      UuidBytesToText(request.header.session_uuid));
  if (session_it == registry->sessions_by_uuid.end())
    return refuse("SECURITY.ACCESS_DENIED", "session_hidden");
  std::string ensure_detail;
  auto* public_context = EnsureServerPublicAbiSessionForContext(
      registry, session_it->second, &ensure_detail);
  if (public_context == nullptr || public_context->engine_session == nullptr)
    return refuse("SECURITY.ACCESS_DENIED", "engine_session_hidden");
  auto engine_context =
      EngineContextForSession(session_it->second, engine_state, request);
  engine_bridge::StatementParameterCoordinationBeginRequestV1 begin;
  begin.engine_session = public_context->engine_session;
  begin.engine_context = &engine_context;
  begin.mode = engine_bridge::StatementParameterExecutionMode::kPrepared;
  begin.operation_uuid = UuidBytesToText(GetUuid(request.payload, 20));
  if (!sbps::IsZeroUuid(GetUuid(request.payload, 36))) {
    begin.public_prepared_uuid =
        UuidBytesToText(GetUuid(request.payload, 36));
  }
  engine_bridge::StatementParameterCoordinationViewV1 view;
  sb_engine_result_t engine_result = nullptr;
  const auto status = engine_bridge::BeginStatementParameterExecutionCoordinationV1(
      &begin, &view, &engine_result);
  if (engine_result) (void)sb_engine_result_release(engine_result);
  if (status != SB_ENGINE_STATUS_OK)
    return refuse(status == SB_ENGINE_STATUS_SECURITY_DENIED
                      ? "SECURITY.ACCESS_DENIED"
                      : "SBLR.PARAMETER.STALE",
                  std::string("engine_status=") + sb_engine_status_name(status));
  ServerParameterExecutionCoordinationRecord record;
  record.session_uuid = UuidBytesToText(request.header.session_uuid);
  record.operation_uuid = view.operation_uuid;
  record.mode = engine_bridge::StatementParameterExecutionMode::kPrepared;
  record.private_handle = view.private_handle;
  record.generation = view.coordinator_generation;
  {
    std::lock_guard<std::mutex> guard(*registry->statement_context_mutex);
    if (!registry->parameter_coordinations_by_uuid
             .emplace(view.public_coordination_uuid, std::move(record)).second)
      return refuse("SBLR.PARAMETER.STALE", "coordination_collision");
  }
  PutU16(&result.payload, 1);
  result.payload.push_back(1);
  result.payload.push_back(0);
  PutUuid(&result.payload, TextToUuid(view.public_coordination_uuid));
  PutUuid(&result.payload, TextToUuid(view.operation_uuid));
  PutU64(&result.payload, view.coordinator_generation);
  PutU32(&result.payload, 0);
  result.accepted = true;
  return result;
}

SessionOperationResult HandleBeginVariableFrame(
    ServerSessionRegistry* registry, const HostedEngineState& engine_state,
    const sbps::Frame& request) {
  SessionOperationResult result;
  result.response_message_type = 57;
  result.response_schema_id = sbps::kSchemaBeginVariableFrameResultV1;
  result.frame_flags = sbps::kFlagResponse | sbps::kFlagFinal;
  result.session_uuid = request.header.session_uuid;
  const auto refuse = [&](std::string code, std::string detail) {
    result.accepted = false;
    result.frame_flags |= sbps::kFlagError;
    result.diagnostics.push_back(sbps::IpcDiagnostic(
        std::move(code), "parser_server_ipc.variable_frame_refused",
        "Variable frame coordination was refused.",
        {{"detail", std::move(detail)}}));
    return result;
  };
  if (registry == nullptr || request.header.payload_schema_id !=
          sbps::kSchemaBeginVariableFrameRequestV1) {
    return refuse("SBLR.OPERAND_INVALID", "variable_begin_schema_invalid");
  }
  scratchbird::engine::sblr::SblrVariableFrameBeginRequestV1 decoded;
  std::string detail;
  if (!scratchbird::engine::sblr::DecodeSblrVariableFrameBeginRequestV1(
          request.payload.data(), request.payload.size(), &decoded, &detail)) {
    return refuse("SBLR.OPERAND_INVALID", std::move(detail));
  }
  const auto session_it = registry->sessions_by_uuid.find(
      UuidBytesToText(request.header.session_uuid));
  if (session_it == registry->sessions_by_uuid.end())
    return refuse("SECURITY.ACCESS_DENIED", "session_hidden");
  auto context = EngineContextForSession(session_it->second, engine_state,
                                         request);
  // Message 56 is admitted only after authenticated session routing. The
  // private tag conveys that server-owned bridge decision to the engine
  // coordinator; it is never accepted from or serialized by the parser.
  context.statement_metadata_snapshot_engine_owned = true;
  context.trace_tags.push_back("private_variable_frame_coordination");
  // The pre-receipt coordinator resolves context-code 1 only through the
  // manifest-admitted datatype/type/codec registry snapshot. These are Core
  // registry identities, not parser input or a catalog-name inference.
  context.catalog_epoch_uuid.canonical =
      "019d0000-0000-7000-8000-00000000d701";
  context.catalog_generation_id = 1;
  const auto hex = [](const std::array<std::uint8_t,32>& bytes) {
    constexpr char digits[] = "0123456789abcdef";
    std::string out = "sha256:";
    out.reserve(71);
    for (const auto byte : bytes) {
      out.push_back(digits[byte >> 4]); out.push_back(digits[byte & 15]);
    }
    return out;
  };
  std::vector<engine_api::SblrVariableFrameDemand> demands;
  demands.reserve(decoded.demands.size());
  for (const auto& row : decoded.demands) {
    engine_api::SblrVariableFrameDemand demand;
    demand.declaration_occurrence_id = row.declaration_occurrence_id;
    demand.datatype_context_code = row.datatype_context_code;
    demand.nullable = row.nullable != 0;
    demand.mutability = row.mutability == 0
        ? engine_api::SblrVariableMutability::immutable
        : engine_api::SblrVariableMutability::mutable_value;
    demand.initial_state = row.initial_value_state == 2
        ? engine_api::SblrVariableValueState::null_value
        : engine_api::SblrVariableValueState::uninitialized;
    demand.declaration_token_sha256 = hex(row.declaration_token_sha256);
    demands.push_back(std::move(demand));
  }
  const auto begun = engine_api::BeginSblrVariableFrame(
      context, UuidBytesToText(decoded.operation_uuid),
      decoded.expires_after_ns, demands);
  if (!begun.ok) return refuse(
      begun.diagnostic.code.empty() ? "SBLR.VARIABLE.STALE"
                                    : begun.diagnostic.code,
      begun.diagnostic.message_key);
  scratchbird::engine::sblr::SblrVariableFrameBeginResultV1 encoded;
  encoded.public_coordination_uuid = TextToUuid(
      begun.snapshot.public_coordination_uuid);
  encoded.operation_uuid = TextToUuid(begun.snapshot.operation_uuid);
  encoded.scope_uuid = TextToUuid(begun.snapshot.scope_uuid);
  encoded.scope_generation = begun.snapshot.scope_generation;
  encoded.frame_uuid = TextToUuid(begun.snapshot.frame_uuid);
  encoded.frame_generation = begun.snapshot.frame_generation;
  encoded.registry_generation = begun.snapshot.registry_generation;
  encoded.coordinator_generation = begun.snapshot.coordinator_generation;
  for (const auto& source : begun.snapshot.mappings) {
    scratchbird::engine::sblr::SblrVariableFrameMappingV1 mapping;
    mapping.declaration_occurrence_id = source.declaration_occurrence_id;
    mapping.variable_ordinal = source.descriptor.variable_ordinal;
    mapping.nullable = source.descriptor.nullable ? 1 : 0;
    mapping.mutability = source.descriptor.mutability ==
            engine_api::SblrVariableMutability::mutable_value ? 1 : 0;
    mapping.value_state = static_cast<std::uint8_t>(
        source.descriptor.value_state);
    mapping.variable_descriptor_uuid = TextToUuid(
        source.descriptor.variable_descriptor_uuid);
    mapping.variable_descriptor_generation =
        source.descriptor.variable_descriptor_generation;
    mapping.datatype_descriptor_uuid = TextToUuid(
        source.descriptor.datatype_descriptor_uuid);
    mapping.datatype_descriptor_generation =
        source.descriptor.datatype_descriptor_generation;
    mapping.datatype_type_uuid = TextToUuid(source.datatype_type_uuid);
    mapping.value_generation = source.descriptor.value_generation;
    encoded.mappings.push_back(std::move(mapping));
  }
  result.payload = scratchbird::engine::sblr::
      EncodeSblrVariableFrameBeginResultV1(&encoded);
  if (result.payload.empty())
    return refuse("SBLR.EXECUTION_FAILED", "SBVC_encoding_failed");
  ServerVariableFrameRecord frame_record;
  frame_record.session_uuid=UuidBytesToText(request.header.session_uuid);
  frame_record.transaction_uuid=begun.snapshot.transaction_uuid;
  frame_record.operation_uuid=begun.snapshot.operation_uuid;
  frame_record.public_coordination_uuid=begun.snapshot.public_coordination_uuid;
  frame_record.scope_uuid=begun.snapshot.scope_uuid;
  frame_record.scope_generation=begun.snapshot.scope_generation;
  frame_record.frame_uuid=begun.snapshot.frame_uuid;
  frame_record.frame_generation=begun.snapshot.frame_generation;
  frame_record.registry_snapshot_uuid=begun.snapshot.registry_snapshot_uuid;
  frame_record.coordinator_generation=begun.snapshot.coordinator_generation;
  frame_record.registry_generation=begun.snapshot.registry_generation;
  for(const auto& source:begun.snapshot.mappings){ServerVariableFrameMappingRecord m;
    m.occurrence_id=source.declaration_occurrence_id;
    m.variable_ordinal=source.descriptor.variable_ordinal;
    m.variable_descriptor_uuid=source.descriptor.variable_descriptor_uuid;
    m.variable_descriptor_generation=source.descriptor.variable_descriptor_generation;
    m.datatype_descriptor_uuid=source.descriptor.datatype_descriptor_uuid;
    m.datatype_descriptor_generation=source.descriptor.datatype_descriptor_generation;
    m.datatype_type_uuid=source.datatype_type_uuid;m.value_generation=source.descriptor.value_generation;
    m.nullable=source.descriptor.nullable?1:0;
    m.mutability=source.descriptor.mutability==engine_api::SblrVariableMutability::mutable_value?1:0;
    m.value_state=static_cast<std::uint8_t>(source.descriptor.value_state);
    frame_record.mappings.push_back(std::move(m));}
  {
    std::lock_guard<std::mutex> guard(*registry->statement_context_mutex);
    if(!registry->variable_frames_by_coordination_uuid.emplace(
        frame_record.public_coordination_uuid,std::move(frame_record)).second)
      return refuse("SBLR.VARIABLE.STALE","variable_frame_collision");
  }
  result.accepted = true;
  return result;
}

SessionOperationResult HandleCloseVariableFrame(
    ServerSessionRegistry* registry, const HostedEngineState& engine_state,
    const sbps::Frame& request) {
  SessionOperationResult result;
  result.response_message_type = 59;
  result.response_schema_id = sbps::kSchemaCloseVariableFrameResultV1;
  result.frame_flags = sbps::kFlagResponse | sbps::kFlagFinal;
  result.session_uuid = request.header.session_uuid;
  const auto refuse = [&](std::string code, std::string detail) {
    result.accepted = false; result.frame_flags |= sbps::kFlagError;
    result.diagnostics.push_back(sbps::IpcDiagnostic(
        std::move(code), "parser_server_ipc.variable_frame_close_refused",
        "Variable frame close was refused.", {{"detail",std::move(detail)}}));
    return result;
  };
  if (registry == nullptr || request.header.payload_schema_id !=
          sbps::kSchemaCloseVariableFrameRequestV1)
    return refuse("SBLR.OPERAND_INVALID", "variable_close_schema_invalid");
  scratchbird::engine::sblr::SblrVariableFrameCloseRequestV1 decoded;
  std::string detail;
  if (!scratchbird::engine::sblr::DecodeSblrVariableFrameCloseRequestV1(
          request.payload.data(),request.payload.size(),&decoded,&detail))
    return refuse("SBLR.OPERAND_INVALID",std::move(detail));
  const auto session_it=registry->sessions_by_uuid.find(
      UuidBytesToText(request.header.session_uuid));
  if(session_it==registry->sessions_by_uuid.end())
    return refuse("SECURITY.ACCESS_DENIED","session_hidden");
  auto context=EngineContextForSession(session_it->second,engine_state,request);
  const auto closed=engine_api::CloseSblrVariableFrame(
      context,UuidBytesToText(decoded.public_coordination_uuid),
      UuidBytesToText(decoded.operation_uuid),decoded.expected_frame_generation,
      "variable_frame_close_v1");
  if(!closed.ok)return refuse(closed.diagnostic.code.empty()
      ?"SBLR.VARIABLE.STALE":closed.diagnostic.code,
      closed.diagnostic.message_key);
  scratchbird::engine::sblr::SblrVariableFrameCloseResultV1 encoded;
  encoded.public_coordination_uuid=decoded.public_coordination_uuid;
  encoded.revoked_frame_generation=closed.snapshot.frame_generation;
  encoded.decision_evidence_generation=closed.snapshot.coordinator_generation;
  result.payload=scratchbird::engine::sblr::EncodeSblrVariableFrameCloseResultV1(encoded);
  if(result.payload.empty())return refuse("SBLR.EXECUTION_FAILED","SBVZ_encoding_failed");
  result.accepted=true;return result;
}

SessionOperationResult HandleNegotiateVariableDescriptors(
    ServerSessionRegistry* registry,const sbps::Frame& request){
  SessionOperationResult result;result.response_message_type=53;
  result.response_schema_id=sbps::kSchemaNegotiateVariableDescriptorsResultV1;
  result.frame_flags=sbps::kFlagResponse|sbps::kFlagFinal;
  result.session_uuid=request.header.session_uuid;
  const auto refuse=[&](std::string code,std::string detail){result.accepted=false;
    result.frame_flags|=sbps::kFlagError;result.diagnostics.push_back(
      sbps::IpcDiagnostic(std::move(code),"parser_server_ipc.variable_negotiation_refused",
      "Variable descriptor negotiation was refused.",{{"detail",std::move(detail)}}));return result;};
  if(!registry||request.header.payload_schema_id!=sbps::kSchemaNegotiateVariableDescriptorsRequestV1)
    return refuse("SBLR.OPERAND_INVALID","SBVR_schema_invalid");
  scratchbird::engine::sblr::SblrVariableNegotiateRequestV1 decoded;std::string detail;
  if(!scratchbird::engine::sblr::DecodeSblrVariableNegotiateRequestV1(
      request.payload.data(),request.payload.size(),&decoded,&detail))
    return refuse("SBLR.OPERAND_INVALID",std::move(detail));
  const auto receipt_uuid=UuidBytesToText(decoded.preliminary_receipt_uuid);
  ServerStatementContextRecord* receipt_record=nullptr;ServerVariableFrameRecord* frame=nullptr;
  {
    std::lock_guard<std::mutex> guard(*registry->statement_context_mutex);
    for(auto&[id,r]:registry->statement_contexts_by_statement_uuid)
      if(!r.released&&r.view.receipt_uuid==receipt_uuid){receipt_record=&r;break;}
    for(auto&[id,r]:registry->variable_frames_by_coordination_uuid)
      if(r.acquired&&!r.revoked&&r.scope_uuid==UuidBytesToText(decoded.scope_uuid)&&
         r.scope_generation==decoded.scope_generation&&
         r.frame_uuid==UuidBytesToText(decoded.frame_uuid)&&
         r.frame_generation==decoded.frame_generation){frame=&r;break;}
    if(!receipt_record||!frame||receipt_record->session_uuid!=request.header.session_uuid||
       receipt_record->view.variable_scope_uuid!=frame->scope_uuid||
       receipt_record->view.variable_frame_uuid!=frame->frame_uuid)
      return refuse("SBLR.VARIABLE.STALE","receipt_or_frame_stale");
    scratchbird::engine::sblr::SblrVariableNegotiateResultV1 encoded;
    encoded.preliminary_receipt_uuid=decoded.preliminary_receipt_uuid;
    encoded.scope_uuid=decoded.scope_uuid;encoded.scope_generation=decoded.scope_generation;
    encoded.frame_uuid=decoded.frame_uuid;encoded.frame_generation=decoded.frame_generation;
    encoded.registry_snapshot_uuid=TextToUuid(frame->registry_snapshot_uuid);
    encoded.registry_generation=frame->registry_generation;
    for(const auto& demand:decoded.demands){
      if(demand.variable_ordinal>=frame->mappings.size())
        return refuse("SBLR.OPERAND_INVALID","variable_ordinal_out_of_range");
      const auto& source=frame->mappings[demand.variable_ordinal];
      scratchbird::engine::sblr::SblrVariableMappingV1 mapping;
      mapping.occurrence_id=demand.occurrence_id;
      mapping.variable_ordinal=source.variable_ordinal;
      mapping.nullable=source.nullable;mapping.mutability=source.mutability;
      mapping.variable_descriptor_uuid=TextToUuid(source.variable_descriptor_uuid);
      mapping.variable_descriptor_generation=source.variable_descriptor_generation;
      mapping.datatype_descriptor_uuid=TextToUuid(source.datatype_descriptor_uuid);
      mapping.datatype_descriptor_generation=source.datatype_descriptor_generation;
      mapping.datatype_type_uuid=TextToUuid(source.datatype_type_uuid);
      mapping.value_generation=source.value_generation;mapping.value_state=source.value_state;
      encoded.mappings.push_back(std::move(mapping));
    }
    result.payload=scratchbird::engine::sblr::EncodeSblrVariableNegotiateResultV1(&encoded);
  }
  if(result.payload.empty())return refuse("SBLR.EXECUTION_FAILED","SBVG_encoding_failed");
  result.accepted=true;return result;
}

SessionOperationResult HandleAssignVariableValues(
    ServerSessionRegistry* registry,const sbps::Frame& request){
  SessionOperationResult result;result.response_message_type=61;
  result.response_schema_id=sbps::kSchemaAssignVariableValuesResultV1;
  result.frame_flags=sbps::kFlagResponse|sbps::kFlagFinal;
  result.session_uuid=request.header.session_uuid;
  const auto refuse=[&](std::string code,std::string detail){result.accepted=false;
    result.frame_flags|=sbps::kFlagError;result.diagnostics.push_back(
      sbps::IpcDiagnostic(std::move(code),"parser_server_ipc.variable_assignment_refused",
      "Variable value assignment was refused.",{{"detail",std::move(detail)}}));return result;};
  if(!registry||request.header.payload_schema_id!=sbps::kSchemaAssignVariableValuesRequestV1)
    return refuse("SBLR.OPERAND_INVALID","SBVY_schema_invalid");
  scratchbird::engine::sblr::SblrVariableAssignmentRequestV1 decoded;std::string detail;
  if(!scratchbird::engine::sblr::DecodeSblrVariableAssignmentRequestV1(
      request.payload.data(),request.payload.size(),&decoded,&detail))
    return refuse("SBLR.OPERAND_INVALID",std::move(detail));
  std::lock_guard<std::mutex> guard(*registry->statement_context_mutex);
  ServerStatementContextRecord* receipt=nullptr;ServerVariableFrameRecord* frame=nullptr;
  const auto receipt_uuid=UuidBytesToText(decoded.preliminary_receipt_uuid);
  const auto coordination_uuid=UuidBytesToText(decoded.public_coordination_uuid);
  for(auto&[id,r]:registry->statement_contexts_by_statement_uuid)
    if(!r.released&&r.view.receipt_uuid==receipt_uuid){receipt=&r;break;}
  const auto fit=registry->variable_frames_by_coordination_uuid.find(coordination_uuid);
  if(fit!=registry->variable_frames_by_coordination_uuid.end())frame=&fit->second;
  if(!receipt||!frame||!frame->acquired||frame->revoked||
     receipt->session_uuid!=request.header.session_uuid||
     frame->session_uuid!=UuidBytesToText(request.header.session_uuid)||
     frame->operation_uuid!=UuidBytesToText(decoded.operation_uuid))
    return refuse("SBLR.VARIABLE.STALE","variable_assignment_binding_stale");
  std::vector<std::uint8_t> response;sb_engine_result_t engine_result=nullptr;
  const auto status=scratchbird::server_engine_bridge::AssignStatementVariableValuesV1(
      receipt->receipt,request.payload,&response,&engine_result);
  if(status!=SB_ENGINE_STATUS_OK){std::string code="SBLR.VARIABLE.STALE",key="variable_assignment_engine_refused";
    if(engine_result){sb_engine_diagnostic_set_view_t diagnostics{};
      if(sb_engine_result_diagnostics(engine_result,&diagnostics)==SB_ENGINE_STATUS_OK&&
         diagnostics.diagnostic_count&&diagnostics.diagnostics){const auto& d=diagnostics.diagnostics[0];
        code.assign(d.symbolic_code.data,d.symbolic_code.size_bytes);
        key.assign(d.message_key.data,d.message_key.size_bytes);}
      (void)sb_engine_result_release(engine_result);}return refuse(std::move(code),std::move(key));}
  if(engine_result)(void)sb_engine_result_release(engine_result);
  scratchbird::engine::sblr::SblrVariableAssignmentResultV1 assigned;
  if(!scratchbird::engine::sblr::DecodeSblrVariableAssignmentResultV1(
      response.data(),response.size(),&assigned,&detail))
    return refuse("SBLR.EXECUTION_FAILED","SBVW_engine_result_invalid");
  frame->registry_generation=assigned.new_registry_generation;
  for(const auto& row:assigned.results){if(row.variable_ordinal>=frame->mappings.size())
      return refuse("SBLR.EXECUTION_FAILED","SBVW_ordinal_invalid");
    auto& target=frame->mappings[row.variable_ordinal];
    if(target.variable_descriptor_uuid!=UuidBytesToText(row.variable_descriptor_uuid))
      return refuse("SBLR.EXECUTION_FAILED","SBVW_descriptor_invalid");
    target.value_generation=row.new_value_generation;
    const auto source=std::find_if(decoded.assignments.begin(),decoded.assignments.end(),
      [&](const auto& x){return x.variable_ordinal==row.variable_ordinal;});
    if(source==decoded.assignments.end())return refuse("SBLR.EXECUTION_FAILED","SBVW_assignment_missing");
    target.value_state=source->value_state;}
  result.payload=std::move(response);result.accepted=true;return result;
}

SessionOperationResult HandleIssueSourceMapDescriptor(
    ServerSessionRegistry* registry,const sbps::Frame& request){
  SessionOperationResult result;result.response_message_type=63;
  result.response_schema_id=sbps::kSchemaIssueSourceMapResultV1;
  result.frame_flags=sbps::kFlagResponse|sbps::kFlagFinal;
  result.session_uuid=request.header.session_uuid;
  const auto refuse=[&](std::string code,std::string detail){result.accepted=false;
    result.frame_flags|=sbps::kFlagError;result.diagnostics.push_back(
      sbps::IpcDiagnostic(std::move(code),"parser_server_ipc.source_map_refused",
      "Source-map descriptor issuance was refused.",{{"detail",std::move(detail)}}));return result;};
  if(!registry||request.header.payload_schema_id!=sbps::kSchemaIssueSourceMapRequestV1)
    return refuse("SBLR.OPERAND_INVALID","SMRQ_schema_invalid");
  scratchbird::engine::sblr::SblrSourceMapIssueRequestV1 decoded;std::string detail;
  if(!scratchbird::engine::sblr::DecodeSblrSourceMapIssueRequestV1(
      request.payload.data(),request.payload.size(),&decoded,&detail))
    return refuse("SBLR.OPERAND_INVALID",std::move(detail));
  std::lock_guard<std::mutex> guard(*registry->statement_context_mutex);
  ServerStatementContextRecord* receipt=nullptr;
  const auto receipt_uuid=UuidBytesToText(decoded.statement_receipt_uuid);
  for(auto&[id,r]:registry->statement_contexts_by_statement_uuid)
    if(!r.released&&r.view.receipt_uuid==receipt_uuid){receipt=&r;break;}
  if(!receipt||receipt->session_uuid!=request.header.session_uuid)
    return refuse("SECURITY.ACCESS_DENIED","source_map_receipt_hidden");
  sb_engine_result_t engine_result=nullptr;
  const auto status=scratchbird::server_engine_bridge::IssueStatementSourceMapDescriptorV1(
      receipt->receipt,request.payload,&result.payload,&engine_result);
  if(status!=SB_ENGINE_STATUS_OK){std::string code="SBLR.SOURCE_MAP.STALE",key="source_map_engine_refused";
    if(engine_result){sb_engine_diagnostic_set_view_t diagnostics{};
      if(sb_engine_result_diagnostics(engine_result,&diagnostics)==SB_ENGINE_STATUS_OK&&
         diagnostics.diagnostic_count&&diagnostics.diagnostics){const auto&d=diagnostics.diagnostics[0];
        code.assign(d.symbolic_code.data,d.symbolic_code.size_bytes);
        key.assign(d.message_key.data,d.message_key.size_bytes);}
      (void)sb_engine_result_release(engine_result);}return refuse(std::move(code),std::move(key));}
  if(engine_result)(void)sb_engine_result_release(engine_result);
  result.accepted=true;return result;
}

SessionOperationResult HandleIssueErrorVectorDescriptor(
    ServerSessionRegistry* registry, const sbps::Frame& request) {
  SessionOperationResult result;
  result.response_message_type = 65;
  result.response_schema_id = sbps::kSchemaIssueErrorVectorResultV1;
  result.frame_flags = sbps::kFlagResponse | sbps::kFlagFinal;
  result.session_uuid = request.header.session_uuid;
  const auto refuse = [&](std::string code, std::string detail) {
    result.accepted = false;
    result.frame_flags |= sbps::kFlagError;
    result.diagnostics.push_back(sbps::IpcDiagnostic(
        std::move(code), "parser_server_ipc.error_vector_refused",
        "Error-vector descriptor issuance was refused.",
        {{"detail", std::move(detail)}}));
    return result;
  };
  if (!registry || request.header.payload_schema_id !=
          sbps::kSchemaIssueErrorVectorRequestV1)
    return refuse("SBLR.OPERAND_INVALID", "EVRQ_schema_invalid");
  scratchbird::engine::sblr::SblrErrorVectorIssueRequestV1 decoded;
  std::string detail;
  if (!scratchbird::engine::sblr::DecodeSblrErrorVectorIssueRequestV1(
          request.payload.data(), request.payload.size(), &decoded, &detail))
    return refuse("SBLR.OPERAND_INVALID", std::move(detail));
  std::lock_guard<std::mutex> guard(*registry->statement_context_mutex);
  ServerStatementContextRecord* receipt = nullptr;
  const auto receipt_uuid = UuidBytesToText(decoded.statement_receipt_uuid);
  for (auto& [id, row] : registry->statement_contexts_by_statement_uuid)
    if (!row.released && row.view.receipt_uuid == receipt_uuid) {
      receipt = &row;
      break;
    }
  if (!receipt || receipt->session_uuid != request.header.session_uuid)
    return refuse("SECURITY.ACCESS_DENIED", "error_vector_receipt_hidden");
  sb_engine_result_t engine_result = nullptr;
  const auto status = engine_bridge::IssueStatementErrorVectorDescriptorV1(
      receipt->receipt, request.payload, &result.payload, &engine_result);
  if (status != SB_ENGINE_STATUS_OK) {
    std::string code = "SBLR.ERROR_VECTOR.STALE";
    std::string key = "error_vector_engine_refused";
    if (engine_result) {
      sb_engine_diagnostic_set_view_t diagnostics{};
      if (sb_engine_result_diagnostics(engine_result, &diagnostics) ==
              SB_ENGINE_STATUS_OK && diagnostics.diagnostic_count &&
          diagnostics.diagnostics) {
        const auto& diagnostic = diagnostics.diagnostics[0];
        code.assign(diagnostic.symbolic_code.data,
                    diagnostic.symbolic_code.size_bytes);
        key.assign(diagnostic.message_key.data,
                   diagnostic.message_key.size_bytes);
      }
      (void)sb_engine_result_release(engine_result);
    }
    return refuse(std::move(code), std::move(key));
  }
  if (engine_result) (void)sb_engine_result_release(engine_result);
  result.accepted = true;
  return result;
}

SessionOperationResult HandleReserveSavepoint(
    ServerSessionRegistry* registry, const HostedEngineState& engine_state,
    const sbps::Frame& request) {
  SessionOperationResult result;
  result.response_message_type = 67;
  result.response_schema_id = sbps::kSchemaReserveSavepointResultV1;
  result.frame_flags = sbps::kFlagResponse | sbps::kFlagFinal;
  result.session_uuid = request.header.session_uuid;
  const auto refuse = [&](std::string code, std::string detail) {
    result.accepted = false; result.frame_flags |= sbps::kFlagError;
    result.diagnostics.push_back(sbps::IpcDiagnostic(
        std::move(code), "parser_server_ipc.savepoint_reservation_refused",
        "Savepoint coordination was refused.", {{"detail",std::move(detail)}}));
    return result;
  };
  if (!registry || request.header.payload_schema_id !=
          sbps::kSchemaReserveSavepointRequestV1)
    return refuse("SBLR.OPERAND_INVALID","SPCR_schema_invalid");
  scratchbird::engine::sblr::SblrSavepointCoordinationRequestV1 decoded;
  std::string detail;
  if (!scratchbird::engine::sblr::DecodeSblrSavepointCoordinationRequestV1(
          request.payload.data(),request.payload.size(),&decoded,&detail))
    return refuse("SBLR.OPERAND_INVALID",std::move(detail));
  const auto session_it=registry->sessions_by_uuid.find(
      UuidBytesToText(request.header.session_uuid));
  if(session_it==registry->sessions_by_uuid.end())
    return refuse("SECURITY.ACCESS_DENIED","session_hidden");
  const auto receipt_uuid=UuidBytesToText(decoded.preliminary_receipt_uuid);
  ServerStatementContextRecord* receipt=nullptr;
  {
    std::lock_guard<std::mutex> guard(*registry->statement_context_mutex);
    for(auto&[id,row]:registry->statement_contexts_by_statement_uuid)
      if(!row.released&&row.view.receipt_uuid==receipt_uuid){receipt=&row;break;}
  }
  if(!receipt||receipt->session_uuid!=request.header.session_uuid||
     receipt->owning_transaction_uuid!=UuidBytesToText(decoded.transaction_uuid)||
     receipt->owning_local_transaction_id!=decoded.local_transaction_id)
    return refuse("SECURITY.ACCESS_DENIED","savepoint_receipt_hidden");
  auto context=EngineContextForSession(session_it->second,engine_state,request);
  context.statement_uuid.canonical=receipt_uuid;
  context.statement_metadata_snapshot_engine_owned=true;
  context.trace_tags.push_back("private_savepoint_coordination");
  const auto hex=[](const std::array<std::uint8_t,32>& bytes){
    static constexpr char digits[]="0123456789abcdef";std::string out="sha256:";
    for(auto byte:bytes){out.push_back(digits[byte>>4]);out.push_back(digits[byte&15]);}
    return out;};
  const auto begun=engine_api::ReserveSblrSavepoint(
      context,receipt_uuid,hex(decoded.transaction_handle_evidence_sha256),
      decoded.symbol_occurrence_id,hex(decoded.canonical_symbol_sha256));
  if(!begun.ok)return refuse(begun.diagnostic.code,begun.diagnostic.message_key);
  scratchbird::engine::sblr::SblrSavepointCoordinationResultV1 wire;
  wire.preliminary_receipt_uuid=decoded.preliminary_receipt_uuid;
  wire.descriptor_uuid=TextToUuid(begun.snapshot.descriptor_uuid);
  wire.descriptor_generation=begun.snapshot.descriptor_generation;
  wire.savepoint_uuid=TextToUuid(begun.snapshot.savepoint_uuid);
  wire.savepoint_generation=begun.snapshot.savepoint_generation;
  wire.transaction_ordinal=begun.snapshot.transaction_ordinal;
  wire.symbol_occurrence_id=begun.snapshot.symbol_occurrence_id;
  wire.canonical_symbol_sha256=decoded.canonical_symbol_sha256;
  result.payload=scratchbird::engine::sblr::EncodeSblrSavepointCoordinationResultV1(wire);
  if(result.payload.empty())return refuse("SBLR.EXECUTION_FAILED","SPCD_encode_failed");
  result.accepted=true;return result;
}

SessionOperationResult HandleReserveAutonomousFrame(
    ServerSessionRegistry* registry, const HostedEngineState& engine_state,
    const sbps::Frame& request) {
  SessionOperationResult result;
  result.response_message_type = 69;
  result.response_schema_id = sbps::kSchemaReserveAutonomousFrameResultV1;
  result.frame_flags = sbps::kFlagResponse | sbps::kFlagFinal;
  result.session_uuid = request.header.session_uuid;
  const auto refuse = [&](std::string code, std::string detail) {
    result.accepted = false; result.frame_flags |= sbps::kFlagError;
    result.diagnostics.push_back(sbps::IpcDiagnostic(
        std::move(code), "parser_server_ipc.autonomous_frame_reservation_refused",
        "Autonomous frame coordination was refused.", {{"detail", std::move(detail)}}));
    return result;
  };
  if (!registry || request.header.payload_schema_id !=
          sbps::kSchemaReserveAutonomousFrameRequestV1)
    return refuse("SBLR.OPERAND_INVALID", "AFCR_schema_invalid");
  scratchbird::engine::sblr::SblrAutonomousFrameRequestV1 decoded;
  std::string detail;
  if (!scratchbird::engine::sblr::DecodeSblrAutonomousFrameRequestV1(
          request.payload.data(), request.payload.size(), &decoded, &detail))
    return refuse("SBLR.OPERAND_INVALID", std::move(detail));
  const auto session_it = registry->sessions_by_uuid.find(
      UuidBytesToText(request.header.session_uuid));
  if (session_it == registry->sessions_by_uuid.end())
    return refuse("SECURITY.ACCESS_DENIED", "session_hidden");
  const auto receipt_uuid = UuidBytesToText(decoded.receipt);
  ServerStatementContextRecord* receipt = nullptr;
  {
    std::lock_guard<std::mutex> guard(*registry->statement_context_mutex);
    for (auto& [_, row] : registry->statement_contexts_by_statement_uuid)
      if (!row.released && row.view.receipt_uuid == receipt_uuid) { receipt = &row; break; }
  }
  if (!receipt || receipt->session_uuid != request.header.session_uuid)
    return refuse("SECURITY.ACCESS_DENIED", "autonomous_receipt_hidden");
  auto context = EngineContextForSession(session_it->second, engine_state, request);
  context.statement_uuid.canonical = receipt_uuid;
  context.statement_metadata_snapshot_engine_owned = true;
  context.trace_tags.push_back("private_psql_autonomous_body_compiler");
  const auto compiled = engine_api::CompileAndPublishSblrAutonomousBodyFrameProjection(
      context, receipt_uuid, decoded.occurrence);
  if (compiled.code != "OK") return refuse(compiled.code, compiled.message_key);
  context.trace_tags.push_back("private_psql_autonomous_frame_coordination");
  const auto reserved = engine_api::ReserveSblrAutonomousFrame(
      context, receipt_uuid, decoded.occurrence);
  if (!reserved.ok) return refuse(reserved.diagnostic.code,
                                  reserved.diagnostic.message_key);
  scratchbird::engine::sblr::SblrAutonomousFrameDescriptorV1 wire;
  wire.receipt = decoded.receipt;
  wire.frame = TextToUuid(reserved.snapshot.frame_uuid);
  wire.frame_generation = reserved.snapshot.frame_generation;
  wire.child_transaction = TextToUuid(reserved.snapshot.child_transaction_uuid);
  wire.child_transaction_number = reserved.snapshot.child_transaction_number;
  const auto& authority = reserved.snapshot.authority;
  wire.parent_transaction = TextToUuid(authority.parent_transaction_uuid);
  wire.parent_frame = TextToUuid(authority.parent_frame_uuid);
  wire.database = TextToUuid(authority.database_uuid);
  wire.attachment = TextToUuid(authority.attachment_uuid);
  wire.session = TextToUuid(authority.session_uuid);
  wire.principal = TextToUuid(authority.principal_uuid);
  wire.security = TextToUuid(authority.security_snapshot_uuid);
  wire.policy = TextToUuid(authority.policy_snapshot_uuid);
  wire.catalog_generation = authority.catalog_generation;
  wire.capability_generation = authority.capability_generation;
  wire.body = TextToUuid(authority.body_sblr_uuid);
  if (!authority.dynamic_statement_sblr_uuid.empty())
    wire.dynamic = TextToUuid(authority.dynamic_statement_sblr_uuid);
  wire.intent = authority.intent; wire.depth = authority.nesting_depth;
  wire.effect_count = authority.effect_count;
  auto parse_sha = [](const std::string& text, auto* output) {
    auto nibble=[](char c){return c>='0'&&c<='9'?c-'0':c>='a'&&c<='f'?c-'a'+10:-1;};
    if(text.size()!=71)return false;
    for(size_t i=0;i<output->size();++i){int h=nibble(text[7+i*2]),l=nibble(text[8+i*2]);if(h<0||l<0)return false;(*output)[i]=uint8_t((h<<4)|l);}return true;};
  if (!parse_sha(authority.effect_set_sha256, &wire.effect_sha) ||
      !parse_sha(reserved.snapshot.descriptor_evidence_sha256, &wire.evidence_sha))
    return refuse("SBLR.EXECUTION_FAILED", "AFCD_evidence_invalid");
  result.payload = scratchbird::engine::sblr::EncodeSblrAutonomousFrameDescriptorV1(wire);
  if (result.payload.empty()) return refuse("SBLR.EXECUTION_FAILED", "AFCD_encode_failed");
  result.accepted = true; return result;
}

SessionOperationResult HandleCoordinateReservationRelease(ServerSessionRegistry* registry,const HostedEngineState& engine_state,const sbps::Frame& request){SessionOperationResult result;result.response_message_type=73;result.response_schema_id=sbps::kSchemaCoordinateReservationReleaseResultV1;result.frame_flags=sbps::kFlagResponse|sbps::kFlagFinal;result.session_uuid=request.header.session_uuid;const auto refuse=[&](std::string c,std::string d){result.accepted=false;result.frame_flags|=sbps::kFlagError;result.diagnostics.push_back(sbps::IpcDiagnostic(std::move(c),"parser_server_ipc.reservation_release_coordination_refused","Reservation release coordination was refused.",{{"detail",std::move(d)}}));return result;};if(!registry||request.header.payload_schema_id!=sbps::kSchemaCoordinateReservationReleaseRequestV1)return refuse("SBLR.OPERAND_INVALID","RRCR_schema_invalid");scratchbird::engine::sblr::SblrReservationReleaseRequestV1 q;std::string detail;if(!scratchbird::engine::sblr::DecodeSblrReservationReleaseRequestV1(request.payload.data(),request.payload.size(),&q,&detail))return refuse("SBLR.OPERAND_INVALID",detail);auto session=registry->sessions_by_uuid.find(UuidBytesToText(request.header.session_uuid));if(session==registry->sessions_by_uuid.end())return refuse("SECURITY.ACCESS_DENIED","session_hidden");const auto receipt_uuid=UuidBytesToText(q.receipt);ServerStatementContextRecord* receipt=nullptr;{std::lock_guard<std::mutex>g(*registry->statement_context_mutex);for(auto&[_,r]:registry->statement_contexts_by_statement_uuid)if(!r.released&&r.view.receipt_uuid==receipt_uuid){receipt=&r;break;}}if(!receipt||receipt->session_uuid!=request.header.session_uuid||receipt->owning_transaction_uuid!=UuidBytesToText(q.transaction))return refuse("SECURITY.ACCESS_DENIED","reservation_receipt_hidden");auto c=EngineContextForSession(session->second,engine_state,request);c.statement_uuid.canonical=receipt_uuid;c.statement_metadata_snapshot_engine_owned=true;c.trace_tags.push_back("private_transaction_relation_reservation_compiler");const auto published=engine_api::CompileAndPublishSblrRelationReservation(c,receipt_uuid,q.occurrence);if(!published.ok)return refuse(published.diagnostic.code,published.diagnostic.message_key);c.trace_tags.push_back("private_transaction_relation_reservation");const auto coordinated=engine_api::CoordinateSblrReservationRelease(c,receipt_uuid,published.snapshot.relation_uuid,q.occurrence,receipt->view.transaction_reservation_release_executor_availability_generation);if(!coordinated.ok)return refuse(coordinated.diagnostic.code,coordinated.diagnostic.message_key);scratchbird::engine::sblr::SblrReservationReleaseDescriptorV1 d;d.reservation=TextToUuid(coordinated.snapshot.reservation_uuid);d.reservation_generation=coordinated.snapshot.reservation_generation;d.transaction=q.transaction;d.local_transaction_id=coordinated.snapshot.local_transaction_id;d.relation=TextToUuid(coordinated.snapshot.relation_uuid);d.mode=coordinated.snapshot.mode;d.catalog_generation=coordinated.snapshot.catalog_generation;d.policy_generation=coordinated.snapshot.policy_generation;d.availability_generation=coordinated.snapshot.availability_generation;result.payload=scratchbird::engine::sblr::EncodeSblrReservationReleaseDescriptorV1(d);if(result.payload.empty())return refuse("SBLR.EXECUTION_FAILED","RRCD_encode_failed");result.accepted=true;return result;}
SessionOperationResult HandleCoordinateTemporaryInstanceCleanup(ServerSessionRegistry*registry,const HostedEngineState&engine_state,const sbps::Frame&request){SessionOperationResult r;r.response_message_type=77;r.response_schema_id=sbps::kSchemaCoordinateTemporaryInstanceCleanupResultV1;r.frame_flags=sbps::kFlagResponse|sbps::kFlagFinal;r.session_uuid=request.header.session_uuid;auto refuse=[&](std::string c,std::string d){r.frame_flags|=sbps::kFlagError;r.diagnostics.push_back(sbps::IpcDiagnostic(std::move(c),"parser_server_ipc.temporary_cleanup_coordination_refused","Temporary instance cleanup coordination was refused.",{{"detail",std::move(d)}}));return r;};scratchbird::engine::sblr::SblrTemporaryInstanceCleanupRequestV1 q;std::string detail;if(!registry||!scratchbird::engine::sblr::DecodeSblrTemporaryInstanceCleanupRequestV1(request.payload.data(),request.payload.size(),&q,&detail))return refuse("SBLR.OPERAND_INVALID",detail);auto session=registry->sessions_by_uuid.find(UuidBytesToText(request.header.session_uuid));if(session==registry->sessions_by_uuid.end())return refuse("SECURITY.ACCESS_DENIED","session_hidden");const auto receipt_uuid=UuidBytesToText(q.receipt);ServerStatementContextRecord*receipt=nullptr;{std::lock_guard<std::mutex>g(*registry->statement_context_mutex);for(auto&[_,v]:registry->statement_contexts_by_statement_uuid)if(!v.released&&v.view.receipt_uuid==receipt_uuid){receipt=&v;break;}}if(!receipt||receipt->session_uuid!=request.header.session_uuid)return refuse("SECURITY.ACCESS_DENIED","temporary_receipt_hidden");auto c=EngineContextForSession(session->second,engine_state,request);c.statement_uuid.canonical=receipt_uuid;c.statement_metadata_snapshot_engine_owned=true;c.trace_tags.push_back("private_temporary_instance_compiler");auto published=engine_api::PublishSblrTemporaryInstance(c,receipt_uuid,q.occurrence,1);if(!published.ok)return refuse(published.diagnostic.code,published.diagnostic.message_key);c.trace_tags.push_back("private_temporary_instance_cleanup");auto coordinated=engine_api::CoordinateSblrTemporaryInstanceCleanup(c,receipt_uuid,q.occurrence,q.trigger,receipt->view.temporary_instance_cleanup_executor_availability_generation);if(!coordinated.ok)return refuse(coordinated.diagnostic.code,coordinated.diagnostic.message_key);scratchbird::engine::sblr::SblrTemporaryInstanceCleanupDescriptorV1 d;d.descriptor=TextToUuid(coordinated.snapshot.descriptor_uuid);d.descriptor_generation=coordinated.snapshot.descriptor_generation;d.definition=TextToUuid(coordinated.snapshot.definition_uuid);d.instance=TextToUuid(coordinated.snapshot.instance_uuid);d.instance_generation=coordinated.snapshot.instance_generation;d.owner_session=q.session;d.owner_transaction=q.transaction;d.retention=coordinated.snapshot.retention;d.trigger=q.trigger;d.state=1;d.catalog_generation=coordinated.snapshot.catalog_generation;d.security_generation=coordinated.snapshot.security_generation;d.policy_generation=coordinated.snapshot.policy_generation;d.availability_generation=coordinated.snapshot.availability_generation;r.payload=scratchbird::engine::sblr::EncodeSblrTemporaryInstanceCleanupDescriptorV1(d);if(r.payload.empty())return refuse("TEMP.TABLE.CLEANUP_FAILED","TICD_encode_failed");r.accepted=true;return r;}

SessionOperationResult HandleFinalizeVariableBinding(
    ServerSessionRegistry* registry,const HostedEngineState& engine_state,
    const sbps::Frame& request){
  SessionOperationResult result;result.response_message_type=55;
  result.response_schema_id=sbps::kSchemaFinalizeVariableBindingResultV1;
  result.frame_flags=sbps::kFlagResponse|sbps::kFlagFinal;
  result.session_uuid=request.header.session_uuid;
  const auto refuse=[&](std::string code,std::string detail){result.accepted=false;
    result.frame_flags|=sbps::kFlagError;result.diagnostics.push_back(
      sbps::IpcDiagnostic(std::move(code),"parser_server_ipc.variable_finalize_refused",
      "Variable binding finalization was refused.",{{"detail",std::move(detail)}}));return result;};
  if(!registry||request.header.payload_schema_id!=sbps::kSchemaFinalizeVariableBindingRequestV1)
    return refuse("SBLR.OPERAND_INVALID","SBVF_schema_invalid");
  scratchbird::engine::sblr::SblrVariableFinalizeRequestV1 decoded;std::string detail;
  if(!scratchbird::engine::sblr::DecodeSblrVariableFinalizeRequestV1(
      request.payload.data(),request.payload.size(),&decoded,&detail))
    return refuse("SBLR.OPERAND_INVALID",std::move(detail));
  std::lock_guard<std::mutex> guard(*registry->statement_context_mutex);
  ServerStatementContextRecord* receipt_record=nullptr;ServerVariableFrameRecord* frame=nullptr;
  const auto receipt_uuid=UuidBytesToText(decoded.preliminary_receipt_uuid);
  for(auto&[id,r]:registry->statement_contexts_by_statement_uuid)
    if(!r.released&&r.view.receipt_uuid==receipt_uuid){receipt_record=&r;break;}
  for(auto&[id,r]:registry->variable_frames_by_coordination_uuid)
    if(r.acquired&&!r.revoked&&r.scope_uuid==UuidBytesToText(decoded.scope_uuid)&&
       r.scope_generation==decoded.scope_generation&&r.frame_uuid==UuidBytesToText(decoded.frame_uuid)&&
       r.frame_generation==decoded.frame_generation){frame=&r;break;}
  if(!receipt_record||!frame||decoded.registry_generation!=frame->registry_generation||
     receipt_record->variable_binding_finalized)
    return refuse("SBLR.VARIABLE.STALE","variable_finalize_state_stale");
  std::vector<std::uint8_t> engine_sbva;
  sb_engine_result_t engine_result = nullptr;
  const auto finalize_status = engine_bridge::FinalizeStatementVariableBindingV1(
      receipt_record->receipt, request.payload, &engine_sbva, &engine_result);
  if (finalize_status != SB_ENGINE_STATUS_OK || engine_sbva.empty()) {
    std::string engine_code;
    std::string engine_key;
    if (engine_result != nullptr) {
      sb_engine_diagnostic_set_view_t diagnostics{};
      if (sb_engine_result_diagnostics(engine_result, &diagnostics) ==
              SB_ENGINE_STATUS_OK &&
          diagnostics.diagnostic_count != 0 && diagnostics.diagnostics != nullptr) {
        const auto& diagnostic = diagnostics.diagnostics[0];
        engine_code.assign(diagnostic.symbolic_code.data,
                           diagnostic.symbolic_code.size_bytes);
        engine_key.assign(diagnostic.message_key.data,
                          diagnostic.message_key.size_bytes);
      }
      (void)sb_engine_result_release(engine_result);
    }
    return refuse(engine_code.empty() ? "SBLR.VARIABLE.STALE" : engine_code,
                  engine_key.empty() ? "engine_variable_finalize_refused"
                                     : engine_key);
  }
  if (engine_result != nullptr) (void)sb_engine_result_release(engine_result);
  scratchbird::engine::sblr::SblrVariableAdmissionV1 engine_admission;
  if (!scratchbird::engine::sblr::DecodeSblrVariableAdmissionV1(
          engine_sbva.data(), engine_sbva.size(), &engine_admission, &detail)) {
    return refuse("SBLR.EXECUTION_FAILED", "engine_SBVA_invalid");
  }
  result.payload = std::move(engine_sbva);
  receipt_record->variable_final_receipt_uuid =
      UuidBytesToText(engine_admission.final_receipt_uuid);
  receipt_record->variable_admission_token_uuid =
      UuidBytesToText(engine_admission.admission_token_uuid);
  receipt_record->variable_binding_sha256 = engine_admission.binding_sha256;
  receipt_record->variable_binding_finalized = true;
  result.accepted = true;
  return result;
#if 0
  const auto table=scratchbird::engine::sblr::DecodeSblrVariableNodeTableV1(
      decoded.canonical_sbvn.data(),decoded.canonical_sbvn.size());
  if(!table.ok||table.table.nodes.size()!=frame->mappings.size())
    return refuse("SBLR.OPERAND_INVALID","SBVN_mapping_count_mismatch");
  for(const auto& node:table.table.nodes){if(node.parent_operand_ordinal==0||
      node.parent_operand_ordinal>frame->mappings.size())return refuse("SBLR.OPERAND_INVALID","SBVN_ordinal_invalid");
    const auto& mapping=frame->mappings[node.parent_operand_ordinal-1];
    if(node.scope_uuid!=TextToUuid(frame->scope_uuid)||node.scope_generation!=frame->scope_generation||
       node.frame_uuid!=TextToUuid(frame->frame_uuid)||node.frame_generation!=frame->frame_generation||
       node.variable_descriptor_uuid!=TextToUuid(mapping.variable_descriptor_uuid)||
       node.variable_descriptor_generation!=mapping.variable_descriptor_generation||
       node.datatype_descriptor_uuid!=TextToUuid(mapping.datatype_descriptor_uuid)||
       node.datatype_descriptor_generation!=mapping.datatype_descriptor_generation||
       node.value_generation!=mapping.value_generation)return refuse("SBLR.VARIABLE.STALE","SBVN_registry_mismatch");}
  auto context=EngineContextForSession(registry->sessions_by_uuid.at(
      UuidBytesToText(request.header.session_uuid)),engine_state,request);
  scratchbird::engine::internal_api::SblrExecutorAvailabilityRowIdentity identity;
  identity.executor_id=scratchbird::engine::internal_api::kSblrVariableExecutorId;
  identity.opcode_code=scratchbird::engine::internal_api::kSblrVariableOpcodeCode;
  identity.opcode_version=scratchbird::engine::internal_api::kSblrVariableOpcodeVersion;
  identity.operand_descriptor_id=scratchbird::engine::internal_api::kSblrVariableOperandDescriptorId;
  identity.result_descriptor_id=scratchbird::engine::internal_api::kSblrVariableResultDescriptorId;
  identity.result_descriptor_version=scratchbird::engine::internal_api::kSblrVariableResultDescriptorVersion;
  const auto availability=scratchbird::engine::internal_api::LoadSblrExecutorAvailabilitySnapshot(context,identity);
  if(!availability.ok||!availability.snapshot.installed)return refuse("SBLR.OPCODE.EXECUTOR_EVIDENCE_MISSING","variable_executor_missing");
  const auto issue_uuid=[](std::uint64_t salt){const auto now=static_cast<std::uint64_t>(std::chrono::duration_cast<std::chrono::milliseconds>(std::chrono::system_clock::now().time_since_epoch()).count());const auto id=scratchbird::core::uuid::GenerateEngineIdentityV7(scratchbird::core::platform::UuidKind::object,now+salt);return id.ok()?scratchbird::core::uuid::UuidToString(id.value.value):std::string{};};
  scratchbird::engine::sblr::SblrVariableAdmissionV1 admission;
  const auto final_uuid=issue_uuid(1),token_uuid=issue_uuid(2);
  admission.final_receipt_uuid=TextToUuid(final_uuid);admission.admission_token_uuid=TextToUuid(token_uuid);
  admission.scope_uuid=decoded.scope_uuid;admission.scope_generation=decoded.scope_generation;
  admission.frame_uuid=decoded.frame_uuid;admission.frame_generation=decoded.frame_generation;
  admission.registry_snapshot_uuid=TextToUuid(frame->registry_snapshot_uuid);
  admission.registry_generation=frame->registry_generation;
  admission.executor_availability_generation=availability.snapshot.generation;
  admission.expires_at_monotonic_ns=1;
  result.payload=scratchbird::engine::sblr::EncodeSblrVariableAdmissionV1(&admission);
  if(result.payload.empty())return refuse("SBLR.EXECUTION_FAILED","SBVA_encoding_failed");
  receipt_record->variable_final_receipt_uuid=final_uuid;
  receipt_record->variable_admission_token_uuid=token_uuid;
  receipt_record->variable_binding_sha256=admission.binding_sha256;
  receipt_record->variable_binding_finalized=true;result.accepted=true;return result;
#endif
}

SessionOperationResult HandleFinalizePreparedSblrParameter(
    ServerSessionRegistry* registry, const sbps::Frame& request) {
  SessionOperationResult result;
  result.response_message_type = 41;
  result.response_schema_id =
      sbps::kSchemaFinalizePreparedSblrParameterResultV1;
  result.frame_flags = sbps::kFlagResponse | sbps::kFlagFinal;
  result.session_uuid = request.header.session_uuid;
  const auto refuse = [&](std::string code, std::string detail) {
    result.accepted = false;
    result.frame_flags |= sbps::kFlagError;
    result.diagnostics.push_back(sbps::IpcDiagnostic(
        std::move(code), "parser_server_ipc.prepared_parameter_seal_refused",
        "Prepared parameter template sealing was refused.",
        {{"detail", std::move(detail)}}));
    return result;
  };
  if (registry == nullptr || request.header.payload_schema_id !=
          sbps::kSchemaFinalizePreparedSblrParameterRequestV1) {
    return refuse("SBLR.OPERAND_INVALID", "SBPT schema invalid");
  }
  const auto decoded = scratchbird::engine::sblr::
      DecodeSblrPreparedParameterTemplateV1(request.payload.data(),
                                            request.payload.size());
  if (!decoded.ok) return refuse(decoded.diagnostic_id, decoded.detail);
  const auto coordination_uuid =
      UuidBytesToText(decoded.value.public_coordination_uuid);
  const auto operation_uuid = UuidBytesToText(decoded.value.operation_uuid);
  std::uint64_t private_handle = 0;
  {
    std::lock_guard<std::mutex> guard(*registry->statement_context_mutex);
    const auto found =
        registry->parameter_coordinations_by_uuid.find(coordination_uuid);
    if (found == registry->parameter_coordinations_by_uuid.end() ||
        found->second.session_uuid != UuidBytesToText(request.header.session_uuid) ||
        found->second.operation_uuid != operation_uuid) {
      return refuse("SECURITY.ACCESS_DENIED", "coordination hidden");
    }
    if (found->second.sealed || found->second.private_handle == 0)
      return refuse("SBLR.PARAMETER.STALE", "coordination stale");
    private_handle = found->second.private_handle;
  }
  engine_bridge::StatementParameterCoordinationViewV1 view;
  sb_engine_result_t engine_result = nullptr;
  const auto status = engine_bridge::SealPreparedStatementParameterTemplateV1(
      private_handle, request.payload, &view, &engine_result);
  std::string engine_code;
  std::string engine_key;
  std::string engine_detail;
  if (engine_result != nullptr) {
    sb_engine_diagnostic_set_view_t diagnostics{};
    if (sb_engine_result_diagnostics(engine_result, &diagnostics) ==
            SB_ENGINE_STATUS_OK &&
        diagnostics.diagnostic_count != 0 && diagnostics.diagnostics != nullptr) {
      const auto& diagnostic = diagnostics.diagnostics[0];
      engine_code.assign(diagnostic.symbolic_code.data,
                         diagnostic.symbolic_code.size_bytes);
      engine_key.assign(diagnostic.message_key.data,
                        diagnostic.message_key.size_bytes);
      engine_detail.assign(diagnostic.safe_detail.data,
                           diagnostic.safe_detail.size_bytes);
    }
    (void)sb_engine_result_release(engine_result);
  }
  if (status != SB_ENGINE_STATUS_OK)
    return refuse(engine_code.empty()
                      ? (status == SB_ENGINE_STATUS_SECURITY_DENIED
                             ? "SECURITY.ACCESS_DENIED"
                             : status == SB_ENGINE_STATUS_CONFLICT
                                   ? "SBLR.PARAMETER.STALE"
                                   : "SBLR.OPERAND_INVALID")
                      : std::move(engine_code),
                  std::string("engine_status=") + sb_engine_status_name(status) +
                      ";key=" + engine_key + ";detail=" + engine_detail);
  {
    std::lock_guard<std::mutex> guard(*registry->statement_context_mutex);
    auto found = registry->parameter_coordinations_by_uuid.find(coordination_uuid);
    if (found == registry->parameter_coordinations_by_uuid.end() ||
        found->second.sealed) return refuse("SBLR.PARAMETER.STALE", "seal race");
    found->second.sealed = true;
    found->second.generation = view.coordinator_generation;
  }
  PutU16(&result.payload, 1); PutU16(&result.payload, 0);
  PutUuid(&result.payload, TextToUuid(view.prepared_statement_uuid));
  PutU64(&result.payload, view.prepared_generation);
  PutUuid(&result.payload, TextToUuid(view.operation_uuid));
  PutU64(&result.payload, view.coordinator_generation); PutU32(&result.payload, 0);
  result.accepted = true;
  return result;
}

SessionOperationResult HandleFinalizeParameterBinding(
    ServerSessionRegistry* registry, const sbps::Frame& request) {
  SessionOperationResult result;
  result.response_message_type = 45;
  result.response_schema_id = sbps::kSchemaFinalizeParameterBindingResultV1;
  result.frame_flags = sbps::kFlagResponse | sbps::kFlagFinal;
  result.session_uuid = request.header.session_uuid;
  const auto refuse = [&](std::string code, std::string detail) {
    result.accepted = false; result.frame_flags |= sbps::kFlagError;
    result.diagnostics.push_back(sbps::IpcDiagnostic(
        std::move(code), "parser_server_ipc.parameter_finalize_refused",
        "Parameter binding finalization was refused.",
        {{"detail", std::move(detail)}})); return result;
  };
  if (registry == nullptr || request.header.payload_schema_id !=
          sbps::kSchemaFinalizeParameterBindingRequestV1 ||
      request.payload.size() < 176 || request.payload.size() > 426192 ||
      !std::equal(request.payload.begin(), request.payload.begin() + 4,
                  "SBPF"))
    return refuse("SBLR.OPERAND_INVALID", "parameter_finalize_frame_invalid");
  const auto preliminary_uuid = UuidBytesToText(GetUuid(request.payload, 16));
  engine_bridge::StatementContextReceiptHandle receipt;
  {
    std::lock_guard<std::mutex> guard(*registry->statement_context_mutex);
    const auto found = std::find_if(
        registry->statement_contexts_by_statement_uuid.begin(),
        registry->statement_contexts_by_statement_uuid.end(),
        [&](const auto& entry) { return entry.second.view.receipt_uuid == preliminary_uuid &&
          entry.second.session_uuid == request.header.session_uuid; });
    if (found == registry->statement_contexts_by_statement_uuid.end())
      return refuse("DATATYPE.DESCRIPTOR_INVALID", "preliminary_receipt_not_live");
    receipt = found->second.receipt;
  }
  sb_engine_result_t engine_result = nullptr;
  const auto status = engine_bridge::FinalizeStatementParameterBindingV1(
      receipt, request.payload, &result.payload, &engine_result);
  std::string engine_code;
  std::string engine_key;
  std::string engine_detail;
  if (engine_result != nullptr) {
    sb_engine_diagnostic_set_view_t diagnostics{};
    if (sb_engine_result_diagnostics(engine_result, &diagnostics) ==
            SB_ENGINE_STATUS_OK &&
        diagnostics.diagnostic_count != 0 && diagnostics.diagnostics != nullptr) {
      const auto& diagnostic = diagnostics.diagnostics[0];
      engine_code.assign(diagnostic.symbolic_code.data,
                         diagnostic.symbolic_code.size_bytes);
      engine_key.assign(diagnostic.message_key.data,
                        diagnostic.message_key.size_bytes);
      engine_detail.assign(diagnostic.safe_detail.data,
                           diagnostic.safe_detail.size_bytes);
    }
    (void)sb_engine_result_release(engine_result);
  }
  if (status != SB_ENGINE_STATUS_OK)
    return refuse(engine_code.empty() ? "DATATYPE.DESCRIPTOR_INVALID"
                                      : std::move(engine_code),
                  std::string("engine_status=") + sb_engine_status_name(status) +
                      ";key=" + engine_key + ";detail=" + engine_detail);
  scratchbird::engine::sblr::SblrParameterAdmissionV1 admitted;
  std::string admission_detail;
  if (!scratchbird::engine::sblr::DecodeSblrParameterAdmissionV1(
          result.payload.data(), result.payload.size(), &admitted,
          &admission_detail)) {
    return refuse("SBLR.OPERAND_INVALID",
                  "engine_parameter_admission_invalid:" + admission_detail);
  }
  result.accepted = true; return result;
}

namespace {

ServerAuthorityCacheEpochVector AuthorityCacheEpochVectorForSession(
    const ServerSessionRecord& session) {
  ServerAuthorityCacheEpochVector vector;
  vector.catalog_generation = session.catalog_generation;
  vector.security_epoch = session.security_epoch;
  vector.descriptor_epoch = session.descriptor_epoch;
  vector.grant_epoch = session.grant_epoch;
  vector.policy_generation = session.policy_generation;
  vector.capability_policy_generation = session.capability_policy_generation;
  vector.cache_invalidation_epoch = session.cache_invalidation_epoch;
  vector.name_resolution_epoch = session.name_resolution_epoch;
  vector.resource_epoch = session.resource_epoch;
  vector.role_set_hash = session.role_set_hash;
  vector.group_set_hash = session.group_set_hash;
  vector.search_path_hash = session.search_path_hash;
  return vector;
}

bool AuthorityCacheEpochNumbersMatch(
    const ServerAuthorityCacheEpochVector& cached,
    const ServerAuthorityCacheEpochVector& current) {
  return cached.catalog_generation == current.catalog_generation &&
         cached.security_epoch == current.security_epoch &&
         cached.descriptor_epoch == current.descriptor_epoch &&
         cached.grant_epoch == current.grant_epoch &&
         cached.policy_generation == current.policy_generation &&
         cached.capability_policy_generation ==
             current.capability_policy_generation &&
         cached.cache_invalidation_epoch == current.cache_invalidation_epoch &&
         cached.name_resolution_epoch == current.name_resolution_epoch &&
         cached.resource_epoch == current.resource_epoch;
}

bool AuthorityCacheAuthorizationHashesMatch(
    const ServerAuthorityCacheEpochVector& cached,
    const ServerAuthorityCacheEpochVector& current) {
  return cached.role_set_hash == current.role_set_hash &&
         cached.group_set_hash == current.group_set_hash &&
         cached.search_path_hash == current.search_path_hash;
}

std::string AuthorityCacheScopePayload(const std::string& cache_kind,
                                       const ServerSessionRecord& session,
                                       const std::string& operation_id,
                                       const std::string& target_object_uuid,
                                       const std::string& statement_shape_hash) {
  const auto vector = AuthorityCacheEpochVectorForSession(session);
  std::ostringstream out;
  out << "cache_kind=" << cache_kind << '\n'
      << "session_uuid=" << UuidBytesToText(session.session_uuid) << '\n'
      << "auth_context_uuid=" << UuidBytesToText(session.auth_context_uuid) << '\n'
      << "principal_uuid=" << UuidBytesToText(session.principal_uuid) << '\n'
      << "effective_user_uuid=" << UuidBytesToText(session.effective_user_uuid)
      << '\n'
      << "database_uuid=" << session.database_uuid << '\n'
      << "operation_id=" << operation_id << '\n'
      << "target_object_uuid=" << target_object_uuid << '\n'
      << "statement_shape_hash=" << statement_shape_hash << '\n'
      << "catalog_generation=" << vector.catalog_generation << '\n'
      << "security_epoch=" << vector.security_epoch << '\n'
      << "descriptor_epoch=" << vector.descriptor_epoch << '\n'
      << "grant_epoch=" << vector.grant_epoch << '\n'
      << "policy_generation=" << vector.policy_generation << '\n'
      << "capability_policy_generation=" << vector.capability_policy_generation
      << '\n'
      << "cache_invalidation_epoch=" << vector.cache_invalidation_epoch << '\n'
      << "name_resolution_epoch=" << vector.name_resolution_epoch << '\n'
      << "resource_epoch=" << vector.resource_epoch << '\n'
      << "role_set_hash=" << vector.role_set_hash << '\n'
      << "group_set_hash=" << vector.group_set_hash << '\n'
      << "search_path_hash=" << vector.search_path_hash << '\n';
  return out.str();
}

}  // namespace

std::string ServerAuthorityCacheKey(const std::string& cache_kind,
                                    const ServerSessionRecord& session,
                                    const std::string& operation_id,
                                    const std::string& target_object_uuid,
                                    const std::string& statement_shape_hash) {
  const std::string digest = engine_api::SecuritySha256Hex(
      AuthorityCacheScopePayload(cache_kind,
                                 session,
                                 operation_id,
                                 target_object_uuid,
                                 statement_shape_hash));
  return digest.empty() ? std::string{} : cache_kind + ":sha256:" + digest;
}

ServerAuthorityCacheRecord StoreServerAuthorityCacheDecision(
    ServerSessionRegistry* registry,
    const ServerSessionRecord& session,
    std::string cache_kind,
    std::string operation_id,
    std::string target_object_uuid,
    std::string statement_shape_hash,
    std::string diagnostic_code,
    std::string diagnostic_detail,
    bool refusal) {
  ServerAuthorityCacheRecord record;
  record.cache_key = ServerAuthorityCacheKey(cache_kind,
                                             session,
                                             operation_id,
                                             target_object_uuid,
                                             statement_shape_hash);
  record.cache_kind = std::move(cache_kind);
  record.session_uuid = session.session_uuid;
  record.auth_context_uuid = session.auth_context_uuid;
  record.principal_uuid = session.principal_uuid;
  record.effective_user_uuid = session.effective_user_uuid;
  record.database_uuid = session.database_uuid;
  record.operation_id = std::move(operation_id);
  record.target_object_uuid = std::move(target_object_uuid);
  record.statement_shape_hash = std::move(statement_shape_hash);
  record.epoch_vector = AuthorityCacheEpochVectorForSession(session);
  record.diagnostic_code = std::move(diagnostic_code);
  record.diagnostic_detail = std::move(diagnostic_detail);
  record.refusal = refusal;
  record.grants_authority = false;
  if (registry != nullptr && !record.cache_key.empty()) {
    const auto existing = registry->authority_cache_by_key.find(record.cache_key);
    if (existing != registry->authority_cache_by_key.end()) {
      record.generation = existing->second.generation;
      record.hit_count = existing->second.hit_count;
    } else {
      record.generation = registry->next_authority_cache_generation++;
    }
    registry->authority_cache_by_key[record.cache_key] = record;
  }
  return record;
}

ServerAuthorityCacheValidation ValidateServerAuthorityCacheEntry(
    const ServerSessionRegistry& registry,
    const ServerSessionRecord& session,
    const std::string& cache_key,
    const std::string& cache_kind,
    const std::string& operation_id,
    const std::string& target_object_uuid,
    const std::string& statement_shape_hash) {
  ServerAuthorityCacheValidation validation;
  const auto found = registry.authority_cache_by_key.find(cache_key);
  if (found == registry.authority_cache_by_key.end()) {
    validation.detail = "authority_cache_not_found";
    return validation;
  }
  const auto& record = found->second;
  validation.record = &record;
  if (record.cache_kind != cache_kind) {
    validation.detail = "authority_cache_kind_mismatch";
    return validation;
  }
  if (record.grants_authority) {
    validation.grants_authority = true;
    validation.detail = "authority_cache_grant_forbidden";
    return validation;
  }
  if (cache_kind == "negative_authorization" && !record.refusal) {
    validation.detail = "negative_authorization_cache_must_refuse";
    return validation;
  }
  if (record.session_uuid != session.session_uuid) {
    validation.cross_session = true;
    validation.detail = "authority_cache_cross_session";
    return validation;
  }
  if (record.auth_context_uuid != session.auth_context_uuid) {
    validation.cross_authorization = true;
    validation.detail = "authority_cache_auth_context_stale";
    return validation;
  }
  if (record.principal_uuid != session.principal_uuid ||
      record.effective_user_uuid != session.effective_user_uuid) {
    validation.cross_authorization = true;
    validation.detail = "authority_cache_cross_user";
    return validation;
  }
  if (record.database_uuid != session.database_uuid) {
    validation.cross_authorization = true;
    validation.detail = "authority_cache_cross_database";
    return validation;
  }
  if (!operation_id.empty() && record.operation_id != operation_id) {
    validation.detail = "authority_cache_operation_mismatch";
    return validation;
  }
  if (!target_object_uuid.empty() &&
      record.target_object_uuid != target_object_uuid) {
    validation.detail = "authority_cache_target_mismatch";
    return validation;
  }
  if (!statement_shape_hash.empty() &&
      record.statement_shape_hash != statement_shape_hash) {
    validation.detail = "authority_cache_statement_shape_mismatch";
    return validation;
  }
  const auto current = AuthorityCacheEpochVectorForSession(session);
  if (!AuthorityCacheEpochNumbersMatch(record.epoch_vector, current)) {
    validation.stale = true;
    validation.detail = "authority_cache_epoch_stale";
    return validation;
  }
  if (!AuthorityCacheAuthorizationHashesMatch(record.epoch_vector, current)) {
    validation.cross_authorization = true;
    validation.detail = "authority_cache_authorization_hash_stale";
    return validation;
  }
  validation.accepted = true;
  validation.detail = "authority_cache_valid";
  return validation;
}

bool MarkServerAuthorityCacheHit(ServerSessionRegistry* registry,
                                 const std::string& cache_key) {
  if (registry == nullptr) return false;
  auto found = registry->authority_cache_by_key.find(cache_key);
  if (found == registry->authority_cache_by_key.end()) return false;
  ++found->second.hit_count;
  return true;
}

void LinkServerRequestCursor(ServerSessionRegistry* registry,
                             const std::array<std::uint8_t, 16>& request_uuid,
                             const std::array<std::uint8_t, 16>& cursor_uuid,
                             bool engine_result_retained) {
  if (registry == nullptr) return;
  auto found = registry->requests_by_uuid.find(UuidBytesToText(request_uuid));
  if (found == registry->requests_by_uuid.end()) return;
  found->second.cursor_uuid = cursor_uuid;
  found->second.engine_result_retained = engine_result_retained;
  found->second.state = ServerRequestLifecycleState::kCursorOpen;
  found->second.detail = engine_result_retained ? "cursor_open_engine_result_retained"
                                                : "cursor_open";
  UpsertRequestFinality(registry, found->second);
}

void CompleteServerRequestLifecycle(ServerSessionRegistry* registry,
                                    const std::array<std::uint8_t, 16>& request_uuid,
                                    ServerRequestLifecycleState state,
                                    std::string detail) {
  if (registry == nullptr) return;
  auto found = registry->requests_by_uuid.find(UuidBytesToText(request_uuid));
  if (found == registry->requests_by_uuid.end()) return;
  found->second.state = state;
  found->second.detail = std::move(detail);
  found->second.transaction_finality_preserved = true;
  UpsertRequestFinality(registry, found->second);
}

std::optional<ServerRequestRecord> FindServerRequestLifecycle(
    const ServerSessionRegistry& registry,
    const std::string& target_uuid) {
  for (const auto& [_, request] : registry.requests_by_uuid) {
    if (UuidBytesToText(request.request_uuid) == target_uuid ||
        UuidBytesToText(request.finality_token_uuid) == target_uuid ||
        UuidBytesToText(request.prepared_statement_uuid) == target_uuid) {
      return request;
    }
  }
  for (const auto& [_, request] : registry.requests_by_uuid) {
    if (UuidBytesToText(request.cursor_uuid) == target_uuid &&
        (request.state == ServerRequestLifecycleState::kCursorOpen ||
         request.state == ServerRequestLifecycleState::kActive)) {
      return request;
    }
  }
  for (const auto& [_, request] : registry.requests_by_uuid) {
    if (RequestTargetMatches(request, target_uuid)) return request;
  }
  return std::nullopt;
}

std::string ServerRequestLifecycleRecordsJson(const ServerSessionRegistry& registry,
                                              const std::string& target_uuid,
                                              bool include_history) {
  std::ostringstream out;
  out << "[";
  bool first = true;
  for (const auto& [_, request] : registry.requests_by_uuid) {
    if (!RequestTargetMatches(request, target_uuid)) continue;
    if (!include_history && TerminalRequestState(request.state)) continue;
    if (!first) out << ',';
    first = false;
    out << RequestLifecycleRecordJson(request);
  }
  out << "]";
  return out.str();
}

ServerRequestLifecycleResult CancelServerRequestLifecycle(
    ServerSessionRegistry* registry,
    const std::string& target_uuid,
    const ServerSessionRecord& actor,
    bool authorization_proven,
    std::uint64_t cancel_timeout_ms) {
  ServerRequestLifecycleResult result;
  if (registry == nullptr) {
    result.error = true;
    result.outcome = "registry_unavailable";
    result.diagnostics.push_back(RequestLifecycleDiagnostic(
        "PARSER_SERVER_IPC.REQUEST_REGISTRY_UNAVAILABLE",
        ServerDiagnosticSeverity::kError,
        "Request lifecycle registry is unavailable."));
    return result;
  }

  std::vector<std::string> matched_keys;
  for (const auto& [key, request] : registry->requests_by_uuid) {
    if (RequestTargetMatches(request, target_uuid)) {
      matched_keys.push_back(key);
    }
  }
  if (matched_keys.empty()) {
    result.accepted = true;
    result.unknown_outcome = true;
    result.outcome = "unknown_finality";
    result.records_json =
        "[{\"requested_target_uuid\":\"" + JsonEscape(target_uuid) +
        "\",\"finality_state\":\"unknown\",\"diagnostic_code\":\"SERVER.REQUEST.FINALITY_UNKNOWN\"}]";
    result.diagnostics.push_back(RequestLifecycleDiagnostic(
        "SERVER.REQUEST.FINALITY_UNKNOWN",
        ServerDiagnosticSeverity::kWarning,
        "The requested request finality token is unknown.",
        {{"target_uuid", target_uuid}}));
    return result;
  }

  auto& request = registry->requests_by_uuid[matched_keys.front()];
  if (request.session_uuid != actor.session_uuid && !authorization_proven) {
    result.error = true;
    result.outcome = "authorization_required";
    result.records_json = "[" + RequestLifecycleRecordJson(request) + "]";
    result.diagnostics.push_back(RequestLifecycleDiagnostic(
        "SECURITY.AUTHORIZATION.DENIED",
        ServerDiagnosticSeverity::kError,
        "Cancelling another session request requires engine authorization.",
        {{"target_request_uuid", UuidBytesToText(request.request_uuid)}}));
    return result;
  }

  bool any_unknown_outcome = false;
  for (const auto& key : matched_keys) {
    auto& matched = registry->requests_by_uuid[key];
    if (matched.session_uuid != actor.session_uuid && !authorization_proven) continue;
    matched.cancel_timeout_ms = cancel_timeout_ms == 0 ? matched.cancel_timeout_ms : cancel_timeout_ms;
    const bool unknown_outcome = CancellationOutcomeUnknown(matched, actor);
    any_unknown_outcome = any_unknown_outcome || unknown_outcome;
    matched.state = unknown_outcome ? ServerRequestLifecycleState::kUnknownOutcome
                                    : ServerRequestLifecycleState::kCancelled;
    matched.detail = unknown_outcome
                         ? "cancel_requested_outcome_unknown_preserved"
                         : "cancel_requested_completed";
    matched.transaction_finality_preserved = true;
    matched.authorization_proven = authorization_proven || matched.session_uuid == actor.session_uuid;
    UpsertRequestFinality(registry, matched);
  }

  request = registry->requests_by_uuid[matched_keys.front()];
  if (!sbps::IsZeroUuid(request.cursor_uuid)) {
    auto cursor_it = registry->cursors_by_uuid.find(UuidBytesToText(request.cursor_uuid));
    if (cursor_it != registry->cursors_by_uuid.end()) {
      auto& cursor = cursor_it->second;
      (void)ReleaseAndClearServerCursorResources(registry, &cursor);
      cursor.finality_state = any_unknown_outcome ? "cancelled_unknown_outcome" : "cancelled";
      cursor.finality_reason = any_unknown_outcome
                                   ? "cancel_requested_outcome_unknown_preserved"
                                   : "cancel_requested_completed";
      cursor.exhausted = true;
      cursor.closed = true;
    }
  }
  result.accepted = true;
  result.unknown_outcome = any_unknown_outcome;
  result.outcome = ServerRequestLifecycleStateName(request.state);
  result.record = request;
  result.records_json = ServerRequestLifecycleRecordsJson(*registry, target_uuid, true);
  if (any_unknown_outcome) {
    result.diagnostics.push_back(RequestLifecycleDiagnostic(
        "PARSER_SERVER_IPC.DISCONNECT_OUTCOME_UNKNOWN",
        ServerDiagnosticSeverity::kWarning,
        "Request cancellation preserved unknown transaction or engine result outcome under MGA authority.",
        {{"request_uuid", UuidBytesToText(request.request_uuid)},
         {"finality_token_uuid", UuidBytesToText(request.finality_token_uuid)},
         {"mga_finality_authority", "engine"}}));
  }
  return result;
}

void MarkServerRequestTimedOutByCursor(ServerSessionRegistry* registry,
                                       const std::array<std::uint8_t, 16>& cursor_uuid,
                                       std::string detail) {
  if (registry == nullptr) return;
  for (auto& [_, request] : registry->requests_by_uuid) {
    if (request.cursor_uuid != cursor_uuid) continue;
    request.engine_result_retained = false;
    if (TerminalRequestState(request.state) &&
        request.state != ServerRequestLifecycleState::kCompleted) {
      // Resource release is deterministic even when an earlier transaction
      // outcome remains unknown. Preserve that finality state, but do not
      // leave a stale retained-result claim behind.
      UpsertRequestFinality(registry, request);
      continue;
    }
    const bool transaction_outcome_risk =
        request.local_transaction_id_at_start != 0 &&
        !OperationCancellationCanBeDeterministic(request.operation_id);
    const bool unknown_outcome = transaction_outcome_risk ||
                                 request.engine_result_retained;
    request.state = unknown_outcome ? ServerRequestLifecycleState::kUnknownOutcome
                                    : ServerRequestLifecycleState::kTimedOut;
    request.detail = unknown_outcome
                         ? detail + "_outcome_unknown_preserved"
                         : detail;
    request.transaction_finality_preserved = true;
    UpsertRequestFinality(registry, request);
  }
}

void MarkServerRequestClosedByCursor(ServerSessionRegistry* registry,
                                     const std::array<std::uint8_t, 16>& cursor_uuid,
                                     ServerRequestLifecycleState state,
                                     std::string detail) {
  if (registry == nullptr) return;
  for (auto& [_, request] : registry->requests_by_uuid) {
    if (request.cursor_uuid != cursor_uuid) continue;
    request.engine_result_retained = false;
    if (TerminalRequestState(request.state) &&
        request.state != ServerRequestLifecycleState::kCompleted) {
      // Cursor resource retirement is deterministic even when an earlier
      // transaction outcome remains unknown. Preserve that terminal state,
      // but never leave a stale retained-result claim behind.
      UpsertRequestFinality(registry, request);
      continue;
    }
    request.state = state;
    request.detail = detail;
    request.transaction_finality_preserved = true;
    UpsertRequestFinality(registry, request);
  }
}

SessionOperationResult HandleAuthHandoff(ServerSessionRegistry* registry,
                                         const HostedEngineState& engine_state,
                                         const sbps::Frame& request) {
  registry->channel_state = ServerChannelState::kAuthPending;
  SessionOperationResult result;
  result.response_message_type = static_cast<std::uint16_t>(sbps::MessageType::kAuthResult);
  result.response_schema_id = kSchemaAuthResultTestV1;
  const auto decoded = DecodeAuthPayload(request.payload);
  if (!decoded) {
    result.diagnostics.push_back(AuthDiagnostic("PARSER_SERVER_IPC.AUTH_HANDOFF_INVALID",
                                                "The authentication handoff payload is malformed."));
    AddAttachAdmissionDenied(&result.diagnostics, "auth_handoff", "malformed_auth_payload");
    result.payload = EncodeAuthResultPayload("error", nullptr, "malformed_auth_payload");
    result.frame_flags = sbps::kFlagResponse | sbps::kFlagError | sbps::kFlagFinal;
    registry->channel_state = ServerChannelState::kFailed;
    RecordFinality(registry, request, {}, {}, "auth_handoff", "rejected", "malformed_auth_payload");
    return result;
  }
  const HostedDatabaseSnapshot* database = FirstOpenDatabase(engine_state);
  if (database == nullptr) {
    result.diagnostics.push_back(AuthDiagnostic("PARSER_SERVER_IPC.AUTH_DATABASE_UNAVAILABLE",
                                                "No hosted database is available for authentication."));
    AddAttachAdmissionDenied(&result.diagnostics, "auth_handoff", "database_unavailable");
    result.payload = EncodeAuthResultPayload("rejected", nullptr, "database_unavailable");
    result.frame_flags = sbps::kFlagResponse | sbps::kFlagError | sbps::kFlagFinal;
    registry->channel_state = ServerChannelState::kFailed;
    RecordFinality(registry, request, {}, {}, "auth_handoff", "rejected", "database_unavailable");
    return result;
  }
  if (!RequestedDatabaseMatches(*database, decoded->requested_database)) {
    result.diagnostics.push_back(AuthDiagnostic("PARSER_SERVER_IPC.AUTH_DATABASE_MISMATCH",
                                                "Authentication requested a database that is not associated with this server route."));
    AddAttachAdmissionDenied(&result.diagnostics, "auth_handoff", "database_route_mismatch");
    result.payload = EncodeAuthResultPayload("rejected", nullptr, "database_route_mismatch");
    result.frame_flags = sbps::kFlagResponse | sbps::kFlagError | sbps::kFlagFinal;
    registry->channel_state = ServerChannelState::kFailed;
    RecordFinality(registry, request, {}, {}, "auth_handoff", "rejected", "database_route_mismatch");
    return result;
  }
  if (!ConnectionHeaderMatchesPayload(request, decoded->connection_uuid)) {
    result.diagnostics.push_back(AuthDiagnostic("PARSER_SERVER_IPC.ROUTE_ASSOCIATION_MISMATCH",
                                                "Authentication route association did not match the parser connection."));
    AddAttachAdmissionDenied(&result.diagnostics, "auth_handoff", "connection_route_mismatch");
    result.payload = EncodeAuthResultPayload("rejected", nullptr, "connection_route_mismatch");
    result.frame_flags = sbps::kFlagResponse | sbps::kFlagError | sbps::kFlagFinal;
    registry->channel_state = ServerChannelState::kFailed;
    RecordFinality(registry, request, {}, {}, "auth_handoff", "rejected", "connection_route_mismatch");
    return result;
  }
  if (database->cluster_structures_present || database->cluster_authority_required) {
    result.diagnostics.push_back(AuthDiagnostic("ENGINE.DBLC_STANDALONE_CLUSTER_FAIL_CLOSED",
                                                "Standalone authentication cannot proceed through cluster lifecycle authority.",
                                                "cluster_authority_unavailable"));
    AddAttachAdmissionDenied(&result.diagnostics, "auth_handoff", "cluster_authority_unavailable");
    result.payload = EncodeAuthResultPayload("rejected", nullptr, "cluster_authority_unavailable");
    result.frame_flags = sbps::kFlagResponse | sbps::kFlagError | sbps::kFlagFinal;
    registry->channel_state = ServerChannelState::kFailed;
    RecordFinality(registry, request, {}, {}, "auth_handoff", "rejected", "cluster_authority_unavailable");
    return result;
  }
  const auto lifecycle = ConfigPolicySecurityLifecycleFromDatabase(*database);
  const auto lifecycle_admission = ValidateConfigPolicySecurityAdmission(
      lifecycle,
      lifecycle.capability_policy_generation,
      lifecycle.policy_generation,
      lifecycle.security_epoch,
      lifecycle.provider_generation,
      decoded->provider_family.empty() ? "local_password" : decoded->provider_family,
      "engine");
  if (!lifecycle_admission.ok()) {
    result.diagnostics.push_back(lifecycle_admission.diagnostic);
    AddAttachAdmissionDenied(&result.diagnostics,
                             "auth_handoff",
                             lifecycle_admission.diagnostic.fields.empty()
                                 ? lifecycle_admission.diagnostic.code
                                 : lifecycle_admission.diagnostic.fields.front().value);
    result.payload =
        EncodeAuthResultPayload("rejected", nullptr, lifecycle_admission.diagnostic.code);
    result.frame_flags = sbps::kFlagResponse | sbps::kFlagError | sbps::kFlagFinal;
    registry->channel_state = ServerChannelState::kFailed;
    RecordFinality(registry,
                   request,
                   {},
                   {},
                   "auth_handoff",
                   "rejected",
                   lifecycle_admission.diagnostic.code);
    return result;
  }
  engine_api::EngineAuthenticateRequest auth_request;
  auth_request.context = EngineContextBase(engine_state, request, decoded->requested_language);
  auth_request.context.security_epoch = lifecycle.security_epoch;
  auth_request.context.catalog_generation_id = lifecycle.policy_generation;
  auth_request.provider_family = decoded->provider_family.empty() ? "local_password" : decoded->provider_family;
  auth_request.principal_claim = decoded->principal_claim;
  auth_request.credential_evidence = decoded->credential_evidence;
  auth_request.credential_evidence_present =
      decoded->credential_evidence_present || !decoded->credential_evidence.empty();
  auth_request.credential_invalid_claim = decoded->credential_invalid;
  auth_request.mfa_evidence_present = decoded->mfa_evidence_present;
  auth_request.target_database.uuid.canonical = auth_request.context.database_uuid.canonical;
  auth_request.target_database.object_kind = "database";
  auth_request.target_object.uuid.canonical = auth_request.context.database_uuid.canonical;
  auth_request.target_object.object_kind = "security_authority";
  auth_request.option_envelopes.push_back("provider:" + auth_request.provider_family);
  auth_request.option_envelopes.push_back("principal:" + auth_request.principal_claim);
  auth_request.option_envelopes.push_back("auth_authority:engine");
  auth_request.option_envelopes.push_back(
      "policy_generation_current:" + std::to_string(lifecycle.policy_generation));
  auth_request.option_envelopes.push_back(
      "policy_generation_observed:" + std::to_string(lifecycle.policy_generation));
  auth_request.option_envelopes.push_back(
      "security_epoch_current:" + std::to_string(lifecycle.security_epoch));
  auth_request.option_envelopes.push_back(
      "security_epoch_observed:" + std::to_string(lifecycle.security_epoch));
  auth_request.option_envelopes.push_back(
      "provider_generation_current:" + std::to_string(lifecycle.provider_generation));
  auth_request.option_envelopes.push_back(
      "provider_generation_observed:" + std::to_string(lifecycle.provider_generation));
  auth_request.option_envelopes.push_back(
      "provider_lifecycle_state:" + std::string(SecurityProviderLifecycleStateName(
                                      lifecycle.provider_state)));
  auth_request.option_envelopes.push_back(
      lifecycle.default_policy_installed ? "default_policy_installed:true"
                                         : "default_policy_installed:false");
  auth_request.option_envelopes.push_back(
      "cache_invalidation_epoch:" + std::to_string(lifecycle.cache_invalidation_epoch));
  if (auth_request.credential_evidence_present) {
    auth_request.option_envelopes.push_back("credential_evidence_present:true");
  }
  if (decoded->credential_invalid) {
    auth_request.option_envelopes.push_back("credential_transport_flag:invalid");
  }
  if (decoded->mfa_required) {
    auth_request.option_envelopes.push_back("mfa_required:true");
  }
  if (decoded->mfa_evidence_present) {
    auth_request.option_envelopes.push_back("mfa_evidence_present:true");
    auth_request.option_envelopes.push_back("mfa:present");
  }
  const auto tls_denial = TlsTransportDenialFromEvidence(*decoded);
  if (tls_denial.denied) {
    auth_request.credential_invalid_claim = true;
    auth_request.option_envelopes.push_back("credential:invalid");
    auth_request.option_envelopes.push_back("tls_transport_denial:" + tls_denial.detail);
    auth_request.option_envelopes.push_back("transport_security_evidence:present");
  }
  const auto auth_result = engine_api::EngineAuthenticate(auth_request);
  if (!auth_result.ok || !auth_result.authenticated) {
    const auto detail = tls_denial.denied
                            ? tls_denial.detail
                            : EngineDiagnosticDetail(auth_result.diagnostics, "credential_rejected");
    if (detail == "mfa_evidence_required") {
      result.diagnostics.push_back(AuthDiagnostic("SECURITY.AUTHENTICATION.CHALLENGE_REQUIRED",
                                                  "Additional authentication evidence is required.",
                                                  detail));
      AddAttachAdmissionDenied(&result.diagnostics, "auth_handoff", detail);
      result.payload = EncodeAuthResultPayload("challenge_required", nullptr, detail);
      result.frame_flags = sbps::kFlagResponse | sbps::kFlagFinal;
      registry->channel_state = ServerChannelState::kAuthPending;
      result.accepted = false;
      RecordFinality(registry, request, {}, {}, "auth_handoff", "challenge_required", detail);
      return result;
    }
    result.diagnostics.push_back(AuthDiagnostic(
        tls_denial.denied
            ? tls_denial.code
            : EngineDiagnosticCode(auth_result.diagnostics, "SECURITY.AUTHENTICATION.FAILED"),
        tls_denial.denied
            ? "TLS transport evidence was denied by engine authentication."
            : "Authentication failed.",
        detail));
    AddAttachAdmissionDenied(&result.diagnostics, "auth_handoff", detail);
    result.payload = EncodeAuthResultPayload("rejected", nullptr, detail);
    result.frame_flags = sbps::kFlagResponse | sbps::kFlagError | sbps::kFlagFinal;
    registry->channel_state = ServerChannelState::kFailed;
    RecordFinality(registry, request, {}, {}, "auth_handoff", "rejected", detail);
    return result;
  }

  ServerSessionRecord session;
  session.connection_uuid = sbps::IsZeroUuid(request.header.connection_uuid)
                                ? decoded->connection_uuid
                                : request.header.connection_uuid;
  const auto physical_channel =
      registry->physical_channel_by_connection_uuid.find(
          UuidBytesToText(session.connection_uuid));
  if (physical_channel !=
      registry->physical_channel_by_connection_uuid.end()) {
    session.server_channel_uuid = physical_channel->second;
  }
  const auto negotiated =
      registry->negotiated_capabilities_by_connection_uuid.find(
          UuidBytesToText(session.connection_uuid));
  const auto admitted_parser =
      registry->admitted_parser_identity_by_connection_uuid.find(
          UuidBytesToText(session.connection_uuid));
  if (admitted_parser !=
      registry->admitted_parser_identity_by_connection_uuid.end()) {
    session.admitted_parser_package_uuid =
        admitted_parser->second.parser_package_uuid;
    session.admitted_dialect_profile_uuid =
        admitted_parser->second.dialect_profile_uuid;
    session.admitted_parser_package_version_major =
        admitted_parser->second.parser_package_version_major;
    session.admitted_parser_package_version_minor =
        admitted_parser->second.parser_package_version_minor;
    session.admitted_parser_package_version_patch =
        admitted_parser->second.parser_package_version_patch;
  }
  session.transaction_routing_v2_negotiated =
      negotiated !=
          registry->negotiated_capabilities_by_connection_uuid.end() &&
      (negotiated->second[0] & sbps::kCapabilityTransactionRoutingV2) != 0;
  session.prepared_metadata_transfer_v1_negotiated =
      negotiated !=
          registry->negotiated_capabilities_by_connection_uuid.end() &&
      (negotiated->second[0] &
       sbps::kCapabilityPreparedMetadataTransferV1) != 0;
  session.relation_descriptor_projection_v3_negotiated =
      negotiated !=
          registry->negotiated_capabilities_by_connection_uuid.end() &&
      (negotiated->second[0] &
       sbps::kCapabilityRelationDescriptorProjectionV3) != 0;
  session.auth_context_uuid =
      TextToUuid(auth_result.connection_security_context.connection_uuid.canonical);
  session.session_uuid = session.auth_context_uuid;
  session.principal_uuid =
      TextToUuid(auth_result.connection_security_context.effective_user_uuid.canonical);
  session.effective_user_uuid = session.principal_uuid;
  if (sbps::IsZeroUuid(session.auth_context_uuid) || sbps::IsZeroUuid(session.principal_uuid)) {
    result.diagnostics.push_back(AuthDiagnostic("SECURITY.AUTHENTICATION.RESULT_INVALID",
                                                "The engine authentication result did not include a valid session or principal UUID.",
                                                "engine_identity_invalid"));
    AddAttachAdmissionDenied(&result.diagnostics, "auth_handoff", "engine_identity_invalid");
    result.payload = EncodeAuthResultPayload("rejected", nullptr, "engine_identity_invalid");
    result.frame_flags = sbps::kFlagResponse | sbps::kFlagError | sbps::kFlagFinal;
    registry->channel_state = ServerChannelState::kFailed;
    RecordFinality(registry, request, {}, {}, "auth_handoff", "rejected", "engine_identity_invalid");
    return result;
  }
  session.principal_claim = decoded->principal_claim;
  session.application_name = decoded->application_name;
  session.requested_role_name = decoded->requested_role;
  session.provider_family = auth_request.provider_family;
  session.database_path = FirstOpenDatabasePath(engine_state);
  session.database_uuid = FirstOpenDatabaseUuid(engine_state);
  ApplyRequestedLanguageProfile(&session, decoded->requested_language);
  ApplyDatabaseHealthToSession(&session, *database);
  session.security_epoch = auth_result.connection_security_context.security_epoch == 0
                               ? session.security_epoch
                               : std::max(session.security_epoch,
                                          auth_result.connection_security_context.security_epoch);
  session.policy_generation = auth_result.connection_security_context.policy_epoch == 0
                                  ? session.policy_generation
                                  : std::max(session.policy_generation,
                                             auth_result.connection_security_context.policy_epoch);
  session.engine_authorization_trace_tags =
      auth_result.connection_security_context.authorization_trace_tags;
  std::string role_group_projection_rejection;
  if (!ApplyDurableAuthorizationProjectionToSession(&session,
                                                    &role_group_projection_rejection)) {
    const std::string detail = role_group_projection_rejection.empty()
                                   ? "requested_role_rejected"
                                   : role_group_projection_rejection;
    result.diagnostics.push_back(AuthDiagnostic("SECURITY.ROLE_INVALID",
                                                "Requested role could not be activated.",
                                                detail));
    AddAttachAdmissionDenied(&result.diagnostics, "auth_handoff", detail);
    result.payload = EncodeAuthResultPayload("rejected", nullptr, detail);
    result.frame_flags = sbps::kFlagResponse | sbps::kFlagError | sbps::kFlagFinal;
    registry->channel_state = ServerChannelState::kFailed;
    RecordFinality(registry, request, {}, {}, "auth_handoff", "rejected", detail);
    return result;
  }
  registry->auth_contexts_by_uuid[AuthContextKey(session.auth_context_uuid)] = session;
  result.session_uuid = session.session_uuid;
  result.payload = EncodeAuthResultPayload("accepted", &session);
  result.frame_flags = sbps::kFlagResponse | sbps::kFlagFinal;
  result.accepted = true;
  registry->channel_state = ServerChannelState::kAttachPending;
  RecordFinality(registry,
                 request,
                 session.session_uuid,
                 session.auth_context_uuid,
                 "auth_handoff",
                 "accepted");
  return result;
}

SessionOperationResult HandleAttachDatabase(ServerSessionRegistry* registry,
                                            const HostedEngineState& engine_state,
                                            const sbps::Frame& request) {
  registry->channel_state = ServerChannelState::kAttachPending;
  SessionOperationResult result;
  result.response_message_type = static_cast<std::uint16_t>(sbps::MessageType::kAttachResult);
  result.response_schema_id = kSchemaAttachResultTestV1;
  const auto decoded = DecodeAttachPayload(request.payload);
  if (!decoded || sbps::IsZeroUuid(decoded->auth_context_uuid)) {
    result.diagnostics.push_back(AuthDiagnostic("PARSER_SERVER_IPC.AUTH_HANDOFF_REQUIRED",
                                                "Database attach requires an accepted authentication context."));
    AddAttachAdmissionDenied(&result.diagnostics, "attach_database", "auth_context_required");
    result.payload = EncodeAttachResultPayload("rejected", nullptr, "auth_context_required");
    result.frame_flags = sbps::kFlagResponse | sbps::kFlagError | sbps::kFlagFinal;
    registry->channel_state = ServerChannelState::kFailed;
    RecordFinality(registry, request, {}, {}, "attach_database", "rejected", "auth_context_required");
    return result;
  }
  auto found = registry->auth_contexts_by_uuid.find(AuthContextKey(decoded->auth_context_uuid));
  if (found == registry->auth_contexts_by_uuid.end()) {
    result.diagnostics.push_back(AuthDiagnostic("PARSER_SERVER_IPC.SESSION_NOT_BOUND",
                                                "The authentication context is unknown or expired."));
    AddAttachAdmissionDenied(&result.diagnostics, "attach_database", "auth_context_unknown");
    result.payload = EncodeAttachResultPayload("rejected", nullptr, "auth_context_unknown");
    result.frame_flags = sbps::kFlagResponse | sbps::kFlagError | sbps::kFlagFinal;
    registry->channel_state = ServerChannelState::kFailed;
    RecordFinality(registry,
                   request,
                   {},
                   decoded->auth_context_uuid,
                   "attach_database",
                   "rejected",
                   "auth_context_unknown");
    return result;
  }
  const bool exact_connection_binding =
      !sbps::IsZeroUuid(request.header.connection_uuid) &&
      !sbps::IsZeroUuid(decoded->connection_uuid) &&
      request.header.connection_uuid == decoded->connection_uuid &&
      !sbps::IsZeroUuid(found->second.connection_uuid) &&
      found->second.connection_uuid == request.header.connection_uuid;
  bool exact_physical_channel = true;
  if (!sbps::IsZeroUuid(found->second.server_channel_uuid)) {
    const auto owner =
        registry->physical_channel_by_connection_uuid.find(
            UuidBytesToText(request.header.connection_uuid));
    exact_physical_channel =
        owner != registry->physical_channel_by_connection_uuid.end() &&
        owner->second == found->second.server_channel_uuid;
  }
  if (!exact_connection_binding || !exact_physical_channel) {
    result.diagnostics.push_back(AuthDiagnostic(
        "PARSER_SERVER_IPC.ROUTE_ASSOCIATION_MISMATCH",
        "Attach connection, payload, authentication context, and physical channel binding did not match.",
        "attach_channel_binding_mismatch"));
    AddAttachAdmissionDenied(&result.diagnostics,
                             "attach_database",
                             "attach_channel_binding_mismatch");
    result.payload = EncodeAttachResultPayload(
        "rejected", nullptr, "attach_channel_binding_mismatch");
    result.frame_flags =
        sbps::kFlagResponse | sbps::kFlagError | sbps::kFlagFinal;
    registry->channel_state = ServerChannelState::kFailed;
    RecordFinality(registry,
                   request,
                   found->second.session_uuid,
                   decoded->auth_context_uuid,
                   "attach_database",
                   "rejected",
                   "attach_channel_binding_mismatch");
    return result;
  }
  if (!sbps::IsZeroUuid(found->second.server_channel_uuid)) {
    const bool sibling_session_on_channel = std::any_of(
        registry->sessions_by_uuid.begin(),
        registry->sessions_by_uuid.end(),
        [&](const auto& entry) {
          return entry.second.server_channel_uuid ==
                 found->second.server_channel_uuid;
        });
    if (sibling_session_on_channel) {
      result.diagnostics.push_back(AuthDiagnostic(
          "PARSER_SERVER_IPC.PHYSICAL_CHANNEL_SESSION_LIMIT",
          "A physical parser channel may own exactly one attached session.",
          "physical_channel_already_has_session"));
      AddAttachAdmissionDenied(&result.diagnostics,
                               "attach_database",
                               "physical_channel_already_has_session");
      result.payload = EncodeAttachResultPayload(
          "rejected", nullptr, "physical_channel_already_has_session");
      result.frame_flags =
          sbps::kFlagResponse | sbps::kFlagError | sbps::kFlagFinal;
      registry->channel_state = ServerChannelState::kFailed;
      RecordFinality(registry,
                     request,
                     found->second.session_uuid,
                     decoded->auth_context_uuid,
                     "attach_database",
                     "rejected",
                     "physical_channel_already_has_session");
      return result;
    }
  }
  if (registry->sessions_by_uuid.find(UuidBytesToText(found->second.session_uuid)) !=
      registry->sessions_by_uuid.end()) {
    result.diagnostics.push_back(AuthDiagnostic(
        "PARSER_SERVER_IPC.AUTH_CONTEXT_REPLAY_REFUSED",
        "The authentication context has already been consumed by an attached session.",
        "auth_context_replay_refused"));
    AddAttachAdmissionDenied(&result.diagnostics, "attach_database", "auth_context_replay_refused");
    result.payload = EncodeAttachResultPayload("rejected", nullptr, "auth_context_replay_refused");
    result.frame_flags = sbps::kFlagResponse | sbps::kFlagError | sbps::kFlagFinal;
    registry->channel_state = ServerChannelState::kFailed;
    RecordFinality(registry,
                   request,
                   found->second.session_uuid,
                   decoded->auth_context_uuid,
                   "attach_database",
                   "rejected",
                   "auth_context_replay_refused");
    return result;
  }
  const HostedDatabaseSnapshot* database = FirstOpenDatabase(engine_state);
  if (database == nullptr) {
    result.diagnostics.push_back(AuthDiagnostic("PARSER_SERVER_IPC.ATTACH_DATABASE_UNAVAILABLE",
                                                "No hosted database is available for attach."));
    AddAttachAdmissionDenied(&result.diagnostics, "attach_database", "database_unavailable");
    result.payload = EncodeAttachResultPayload("rejected", nullptr, "database_unavailable");
    result.frame_flags = sbps::kFlagResponse | sbps::kFlagError | sbps::kFlagFinal;
    registry->channel_state = ServerChannelState::kFailed;
    RecordFinality(registry,
                   request,
                   {},
                   decoded->auth_context_uuid,
                   "attach_database",
                   "rejected",
                   "database_unavailable");
    return result;
  }
  auto session = found->second;
  if (!ConnectionHeaderMatchesSession(request, session)) {
    result.diagnostics.push_back(AuthDiagnostic("PARSER_SERVER_IPC.ROUTE_ASSOCIATION_MISMATCH",
                                                "Attach route association did not match the authenticated parser connection."));
    AddAttachAdmissionDenied(&result.diagnostics, "attach_database", "connection_route_mismatch");
    result.payload = EncodeAttachResultPayload("rejected", nullptr, "connection_route_mismatch");
    result.frame_flags = sbps::kFlagResponse | sbps::kFlagError | sbps::kFlagFinal;
    registry->channel_state = ServerChannelState::kFailed;
    RecordFinality(registry,
                   request,
                   {},
                   decoded->auth_context_uuid,
                   "attach_database",
                   "rejected",
                   "connection_route_mismatch");
    return result;
  }
  if (!RequestedDatabaseMatches(*database, decoded->requested_database) ||
      !AuthContextMatchesHostedDatabase(session, *database)) {
    result.diagnostics.push_back(AuthDiagnostic("PARSER_SERVER_IPC.ATTACH_DATABASE_MISMATCH",
                                                "Attach requested a database that is not associated with this authenticated route."));
    AddAttachAdmissionDenied(&result.diagnostics, "attach_database", "database_route_mismatch");
    result.payload = EncodeAttachResultPayload("rejected", nullptr, "database_route_mismatch");
    result.frame_flags = sbps::kFlagResponse | sbps::kFlagError | sbps::kFlagFinal;
    registry->channel_state = ServerChannelState::kFailed;
    RecordFinality(registry,
                   request,
                   {},
                   decoded->auth_context_uuid,
                   "attach_database",
                   "rejected",
                   "database_route_mismatch");
    return result;
  }
  const std::string attach_mode = CanonicalAttachMode(decoded->requested_attachment_mode);
  if (!AttachmentModeSupported(attach_mode)) {
    result.diagnostics.push_back(AuthDiagnostic("PARSER_SERVER_IPC.ATTACH_MODE_UNSUPPORTED",
                                                "The requested database attachment mode is not supported."));
    AddAttachAdmissionDenied(&result.diagnostics, "attach_database", "attachment_mode_unsupported");
    result.payload = EncodeAttachResultPayload("rejected", nullptr, "attachment_mode_unsupported");
    result.frame_flags = sbps::kFlagResponse | sbps::kFlagError | sbps::kFlagFinal;
    registry->channel_state = ServerChannelState::kFailed;
    RecordFinality(registry,
                   request,
                   session.session_uuid,
                   session.auth_context_uuid,
                   "attach_database",
                   "rejected",
                   "attachment_mode_unsupported");
    return result;
  }
  if ((database->read_only || database->state == HostedDatabaseState::kReadOnly) &&
      attach_mode == "read_write") {
    result.diagnostics.push_back(AuthDiagnostic("PARSER_SERVER_IPC.ATTACH_MODE_DENIED",
                                                "The hosted database admits only read-only attachments."));
    AddAttachAdmissionDenied(&result.diagnostics, "attach_database", "read_only_database");
    result.payload = EncodeAttachResultPayload("rejected", nullptr, "read_only_database");
    result.frame_flags = sbps::kFlagResponse | sbps::kFlagError | sbps::kFlagFinal;
    registry->channel_state = ServerChannelState::kFailed;
    RecordFinality(registry,
                   request,
                   session.session_uuid,
                   session.auth_context_uuid,
                   "attach_database",
                   "rejected",
                   "read_only_database");
    return result;
  }
  if (database->state == HostedDatabaseState::kMaintenance ||
      database->state == HostedDatabaseState::kRestrictedOpen ||
      database->state == HostedDatabaseState::kQuarantined ||
      database->state == HostedDatabaseState::kFailed ||
      database->state == HostedDatabaseState::kDetached) {
    const std::string detail = database->state == HostedDatabaseState::kMaintenance
        ? "maintenance_admission_required"
        : database->state == HostedDatabaseState::kRestrictedOpen
            ? "restricted_open_admission_required"
            : "database_lifecycle_state_not_attachable";
    result.diagnostics.push_back(AuthDiagnostic("ENGINE.DBLC_ATTACH_ADMISSION_DENIED",
                                                "The hosted database lifecycle state does not admit ordinary attachments.",
                                                detail));
    result.payload = EncodeAttachResultPayload("rejected", nullptr, detail);
    result.frame_flags = sbps::kFlagResponse | sbps::kFlagError | sbps::kFlagFinal;
    registry->channel_state = ServerChannelState::kFailed;
    RecordFinality(registry,
                   request,
                   session.session_uuid,
                   session.auth_context_uuid,
                   "attach_database",
                   "rejected",
                   detail);
    return result;
  }
  if (database->cluster_structures_present || database->cluster_authority_required) {
    result.diagnostics.push_back(AuthDiagnostic("ENGINE.DBLC_STANDALONE_CLUSTER_FAIL_CLOSED",
                                                "Standalone attach cannot proceed through cluster lifecycle authority.",
                                                "cluster_authority_unavailable"));
    AddAttachAdmissionDenied(&result.diagnostics, "attach_database", "cluster_authority_unavailable");
    result.payload = EncodeAttachResultPayload("rejected", nullptr, "cluster_authority_unavailable");
    result.frame_flags = sbps::kFlagResponse | sbps::kFlagError | sbps::kFlagFinal;
    registry->channel_state = ServerChannelState::kFailed;
    RecordFinality(registry,
                   request,
                   session.session_uuid,
                   session.auth_context_uuid,
                   "attach_database",
                   "rejected",
                   "cluster_authority_unavailable");
    return result;
  }
  const auto lifecycle = ConfigPolicySecurityLifecycleFromDatabase(*database);
  const auto lifecycle_admission = ValidateConfigPolicySecurityAdmission(
      lifecycle,
      session.capability_policy_generation,
      session.policy_generation,
      session.security_epoch,
      session.security_provider_generation,
      session.provider_family.empty() ? "local_password" : session.provider_family,
      "engine");
  if (!lifecycle_admission.ok()) {
    result.diagnostics.push_back(lifecycle_admission.diagnostic);
    AddAttachAdmissionDenied(&result.diagnostics,
                             "attach_database",
                             lifecycle_admission.diagnostic.code);
    result.payload =
        EncodeAttachResultPayload("rejected", nullptr, lifecycle_admission.diagnostic.code);
    result.frame_flags = sbps::kFlagResponse | sbps::kFlagError | sbps::kFlagFinal;
    registry->channel_state = ServerChannelState::kFailed;
    RecordFinality(registry,
                   request,
                   session.session_uuid,
                   session.auth_context_uuid,
                   "attach_database",
                   "rejected",
                   lifecycle_admission.diagnostic.code);
    return result;
  }
  session.attach_mode = attach_mode;
  session.database_path = database->database_path;
  session.database_uuid = database->database_uuid;
  ApplyDatabaseHealthToSession(&session, *database);
  std::string role_group_projection_rejection;
  if (!ApplyDurableAuthorizationProjectionToSession(&session,
                                                    &role_group_projection_rejection)) {
    const std::string detail = role_group_projection_rejection.empty()
                                   ? "requested_role_rejected"
                                   : role_group_projection_rejection;
    result.diagnostics.push_back(AuthDiagnostic("SECURITY.ROLE_INVALID",
                                                "Requested role could not be activated for attach.",
                                                detail));
    AddAttachAdmissionDenied(&result.diagnostics, "attach_database", detail);
    result.payload = EncodeAttachResultPayload("rejected", nullptr, detail);
    result.frame_flags = sbps::kFlagResponse | sbps::kFlagError | sbps::kFlagFinal;
    registry->channel_state = ServerChannelState::kFailed;
    RecordFinality(registry,
                   request,
                   session.session_uuid,
                   session.auth_context_uuid,
                   "attach_database",
                   "rejected",
                   detail);
    return result;
  }
  engine_api::EngineAuthorizeRequest authorize;
  authorize.context = EngineContextForSession(session, engine_state, request);
  authorize.required_right = "CONNECT";
  authorize.target_database.uuid.canonical = session.database_uuid;
  authorize.target_database.object_kind = "database";
  authorize.target_object.uuid.canonical = session.database_uuid;
  authorize.target_object.object_kind = "database";
  const auto authz_result = engine_api::EngineAuthorize(authorize);
  if (!authz_result.ok || !authz_result.authorized) {
    const auto detail = EngineDiagnosticDetail(authz_result.diagnostics, "connect_denied");
    result.diagnostics.push_back(AuthDiagnostic(
        EngineDiagnosticCode(authz_result.diagnostics, "SECURITY.AUTHORIZATION.DENIED"),
        "Database attach was not authorized by the engine.",
        detail));
    AddAttachAdmissionDenied(&result.diagnostics, "attach_database", detail);
    result.payload = EncodeAttachResultPayload("rejected", nullptr, detail);
    result.frame_flags = sbps::kFlagResponse | sbps::kFlagError | sbps::kFlagFinal;
    registry->channel_state = ServerChannelState::kFailed;
    RecordFinality(registry,
                   request,
                   session.session_uuid,
                   session.auth_context_uuid,
                   "attach_database",
                   "rejected",
                 detail);
    return result;
  }
  std::string transaction_diagnostic_code;
  std::string transaction_diagnostic_detail;
  if (!StartAlwaysActiveTransactionForSession(&session,
                                              engine_state,
                                              request,
                                              &transaction_diagnostic_code,
                                              &transaction_diagnostic_detail)) {
    const std::string detail = transaction_diagnostic_detail.empty()
                                   ? "transaction_begin_failed"
                                   : transaction_diagnostic_detail;
    result.diagnostics.push_back(AuthDiagnostic(
        transaction_diagnostic_code.empty() ? "ENGINE.DBLC_TRANSACTION_ADMISSION_DENIED"
                                            : transaction_diagnostic_code,
        "Database attach could not create the required active transaction.",
        detail));
    AddAttachAdmissionDenied(&result.diagnostics, "attach_database", detail);
    result.payload = EncodeAttachResultPayload("rejected", nullptr, detail);
    result.frame_flags = sbps::kFlagResponse | sbps::kFlagError | sbps::kFlagFinal;
    registry->channel_state = ServerChannelState::kFailed;
    RecordFinality(registry,
                   request,
                   session.session_uuid,
                   session.auth_context_uuid,
                   "attach_database",
                   "rejected",
                   detail);
    return result;
  }
  const auto published_session = registry->sessions_by_uuid.emplace(
      UuidBytesToText(session.session_uuid), session);
  if (!published_session.second) {
    const bool cleanup_applied = RollbackUnpublishedInitialTransaction(
        session, engine_state, request);
    if (!cleanup_applied) {
      session.detached_recovery_quarantined = true;
      found->second = session;
    }
    const std::string detail = cleanup_applied
                                   ? "session_publication_failed_cleanup_applied"
                                   : "session_publication_failed_cleanup_unresolved";
    result.diagnostics.push_back(AuthDiagnostic(
        "PARSER_SERVER_IPC.SESSION_PUBLICATION_FAILED",
        "Database attach could not publish the engine-issued session and transaction identity.",
        detail));
    result.payload = EncodeAttachResultPayload("rejected", nullptr, detail);
    result.frame_flags =
        sbps::kFlagResponse | sbps::kFlagError | sbps::kFlagFinal;
    registry->channel_state = ServerChannelState::kFailed;
    RecordFinality(registry,
                   request,
                   session.session_uuid,
                   session.auth_context_uuid,
                   "attach_database",
                   cleanup_applied ? "rejected" : "outcome_unknown",
                   detail);
    return result;
  }
  registry->channel_state = ServerChannelState::kReady;
  result.accepted = true;
  result.session_uuid = session.session_uuid;
  result.payload = EncodeAttachResultPayload("accepted", &session);
  result.frame_flags = sbps::kFlagResponse | sbps::kFlagFinal;
  RecordFinality(registry,
                 request,
                 session.session_uuid,
                 session.auth_context_uuid,
                 "attach_database",
                 "accepted");
  return result;
}

SessionOperationResult HandleEmbeddedSysarchAttach(ServerSessionRegistry* registry,
                                                   const HostedEngineState& engine_state,
                                                   std::string requested_database,
                                                   std::string application_name) {
  if (registry == nullptr) {
    SessionOperationResult result;
    result.response_message_type = static_cast<std::uint16_t>(sbps::MessageType::kAttachResult);
    result.response_schema_id = kSchemaAttachResultTestV1;
    result.frame_flags = sbps::kFlagResponse | sbps::kFlagError | sbps::kFlagFinal;
    result.diagnostics.push_back(AuthDiagnostic("PARSER_SERVER_IPC.SESSION_REGISTRY_REQUIRED",
                                                "Embedded attach requires a session registry.",
                                                "session_registry_required"));
    result.payload = EncodeAttachResultPayload("rejected", nullptr, "session_registry_required");
    return result;
  }
  registry->channel_state = ServerChannelState::kAttachPending;
  SessionOperationResult result;
  result.response_message_type = static_cast<std::uint16_t>(sbps::MessageType::kAttachResult);
  result.response_schema_id = kSchemaAttachResultTestV1;
  const HostedDatabaseSnapshot* database = FirstOpenDatabase(engine_state);
  if (database == nullptr) {
    result.diagnostics.push_back(AuthDiagnostic("PARSER_SERVER_IPC.ATTACH_DATABASE_UNAVAILABLE",
                                                "No hosted database is available for embedded attach.",
                                                "database_unavailable"));
    AddAttachAdmissionDenied(&result.diagnostics, "embedded_attach", "database_unavailable");
    result.payload = EncodeAttachResultPayload("rejected", nullptr, "database_unavailable");
    result.frame_flags = sbps::kFlagResponse | sbps::kFlagError | sbps::kFlagFinal;
    registry->channel_state = ServerChannelState::kFailed;
    return result;
  }
  if (!RequestedDatabaseMatches(*database, requested_database)) {
    result.diagnostics.push_back(AuthDiagnostic("PARSER_SERVER_IPC.ATTACH_DATABASE_MISMATCH",
                                                "Embedded attach requested a different database.",
                                                "database_route_mismatch"));
    AddAttachAdmissionDenied(&result.diagnostics, "embedded_attach", "database_route_mismatch");
    result.payload = EncodeAttachResultPayload("rejected", nullptr, "database_route_mismatch");
    result.frame_flags = sbps::kFlagResponse | sbps::kFlagError | sbps::kFlagFinal;
    registry->channel_state = ServerChannelState::kFailed;
    return result;
  }
  if (database->cluster_structures_present || database->cluster_authority_required) {
    result.diagnostics.push_back(AuthDiagnostic("ENGINE.DBLC_STANDALONE_CLUSTER_FAIL_CLOSED",
                                                "Embedded attach cannot proceed through cluster lifecycle authority.",
                                                "cluster_authority_unavailable"));
    AddAttachAdmissionDenied(&result.diagnostics, "embedded_attach", "cluster_authority_unavailable");
    result.payload = EncodeAttachResultPayload("rejected", nullptr, "cluster_authority_unavailable");
    result.frame_flags = sbps::kFlagResponse | sbps::kFlagError | sbps::kFlagFinal;
    registry->channel_state = ServerChannelState::kFailed;
    return result;
  }

  ServerSessionRecord session;
  session.connection_uuid = sbps::MakeUuidV7Bytes();
  session.auth_context_uuid = sbps::MakeUuidV7Bytes();
  session.session_uuid = sbps::MakeUuidV7Bytes();
  session.principal_uuid = PrincipalUuidFor("sysarch");
  session.effective_user_uuid = session.principal_uuid;
  session.principal_claim = "sysarch";
  session.provider_family = "embedded_sysarch";
  session.application_name = application_name.empty() ? "sb_isql" : std::move(application_name);
  session.database_path = database->database_path;
  session.database_uuid = database->database_uuid;
  session.attach_mode = "read_write";
  session.embedded_in_process = true;
  session.engine_authorization_trace_tags = {"embedded_sysarch", "sb_isql"};
  ApplyRequestedLanguageProfile(&session, "en");
  ApplyDatabaseHealthToSession(&session, *database);

  sbps::Frame attach_frame;
  attach_frame.header.request_uuid = sbps::MakeUuidV7Bytes();
  attach_frame.header.connection_uuid = session.connection_uuid;
  attach_frame.header.session_uuid = session.session_uuid;
  std::string transaction_diagnostic_code;
  std::string transaction_diagnostic_detail;
  if (!StartAlwaysActiveTransactionForSession(&session,
                                              engine_state,
                                              attach_frame,
                                              &transaction_diagnostic_code,
                                              &transaction_diagnostic_detail)) {
    const std::string detail = transaction_diagnostic_detail.empty()
                                   ? "transaction_begin_failed"
                                   : transaction_diagnostic_detail;
    result.diagnostics.push_back(AuthDiagnostic(
        transaction_diagnostic_code.empty() ? "ENGINE.DBLC_TRANSACTION_ADMISSION_DENIED"
                                            : transaction_diagnostic_code,
        "Embedded attach could not create the required active transaction.",
        detail));
    AddAttachAdmissionDenied(&result.diagnostics, "embedded_attach", detail);
    result.payload = EncodeAttachResultPayload("rejected", nullptr, detail);
    result.frame_flags = sbps::kFlagResponse | sbps::kFlagError | sbps::kFlagFinal;
    registry->channel_state = ServerChannelState::kFailed;
    return result;
  }

  const auto session_key = UuidBytesToText(session.session_uuid);
  const auto published_session =
      registry->sessions_by_uuid.emplace(session_key, session);
  const auto published_auth = registry->auth_contexts_by_uuid.emplace(
      AuthContextKey(session.auth_context_uuid), session);
  if (!published_session.second || !published_auth.second) {
    if (published_session.second) {
      registry->sessions_by_uuid.erase(session_key);
    }
    if (published_auth.second) {
      registry->auth_contexts_by_uuid.erase(
          AuthContextKey(session.auth_context_uuid));
    }
    const bool cleanup_applied = RollbackUnpublishedInitialTransaction(
        session, engine_state, attach_frame);
    const std::string detail = cleanup_applied
                                   ? "embedded_session_publication_failed_cleanup_applied"
                                   : "embedded_session_publication_failed_cleanup_unresolved";
    result.diagnostics.push_back(AuthDiagnostic(
        "PARSER_SERVER_IPC.SESSION_PUBLICATION_FAILED",
        "Embedded attach could not publish the engine-issued session and transaction identity.",
        detail));
    result.payload = EncodeAttachResultPayload("rejected", nullptr, detail);
    result.frame_flags =
        sbps::kFlagResponse | sbps::kFlagError | sbps::kFlagFinal;
    registry->channel_state = ServerChannelState::kFailed;
    return result;
  }
  registry->channel_state = ServerChannelState::kReady;
  result.accepted = true;
  result.session_uuid = session.session_uuid;
  result.payload = EncodeAttachResultPayload("accepted", &session);
  result.frame_flags = sbps::kFlagResponse | sbps::kFlagFinal;
  return result;
}

ServerTransactionState* AdoptAndFindExactActiveDefaultTransaction(
    ServerSessionRecord* session) {
  if (session == nullptr) return nullptr;
  if (session->transactions_by_local_id.empty()) {
    if (!IsCompleteEngineTransactionIdentity(
            session->local_transaction_id, session->transaction_uuid)) {
      return nullptr;
    }
    ServerTransactionState legacy;
    legacy.local_transaction_id = session->local_transaction_id;
    legacy.snapshot_visible_through_local_transaction_id =
        session->snapshot_visible_through_local_transaction_id;
    legacy.transaction_uuid = session->transaction_uuid;
    legacy.transaction_timestamp = session->transaction_timestamp;
    legacy.isolation_level = session->default_transaction_isolation_level;
    legacy.read_only = session->default_transaction_read_only ||
                       session->attach_mode == "read_only";
    legacy.begin_ordinal = session->next_transaction_begin_ordinal++;
    const auto inserted = session->transactions_by_local_id.emplace(
        legacy.local_transaction_id, std::move(legacy));
    if (!inserted.second) return nullptr;
    session->default_local_transaction_id = inserted.first->first;
  }
  if (session->default_local_transaction_id == 0) return nullptr;
  const auto found = session->transactions_by_local_id.find(
      session->default_local_transaction_id);
  if (found == session->transactions_by_local_id.end() ||
      found->second.lifecycle_state !=
          ServerTransactionLifecycleState::kActive ||
      session->local_transaction_id != found->second.local_transaction_id ||
      session->transaction_uuid != found->second.transaction_uuid ||
      session->snapshot_visible_through_local_transaction_id !=
          found->second.snapshot_visible_through_local_transaction_id) {
    return nullptr;
  }
  return &found->second;
}

SessionOperationResult HandleDisconnectNotice(ServerSessionRegistry* registry,
                                              const sbps::Frame& request) {
  SessionOperationResult result;
  result.response_message_type = static_cast<std::uint16_t>(sbps::MessageType::kDisconnectNotice);
  result.response_schema_id = kSchemaDisconnectResultTestV1;
  const std::array<std::uint8_t, 16> payload_session_uuid =
      request.payload.size() >= 16 ? GetUuid(request.payload, 0)
                                   : std::array<std::uint8_t, 16>{};
  const std::array<std::uint8_t, 16> session_uuid =
      request.header.session_uuid;
  std::string disconnect_reason = "parser_disconnect_notice";
  if (request.payload.size() > 16) {
    std::size_t offset = 16;
    (void)ReadString(request.payload, &offset, &disconnect_reason);
    if (disconnect_reason.empty()) disconnect_reason = "parser_disconnect_notice";
  }
  const auto key = UuidBytesToText(session_uuid);
  const auto session_found = registry->sessions_by_uuid.find(key);
  const bool exact_binding =
      !sbps::IsZeroUuid(request.header.connection_uuid) &&
      !sbps::IsZeroUuid(session_uuid) &&
      !sbps::IsZeroUuid(payload_session_uuid) &&
      session_uuid == payload_session_uuid &&
      session_found != registry->sessions_by_uuid.end() &&
      session_found->second.session_uuid == session_uuid &&
      !sbps::IsZeroUuid(session_found->second.connection_uuid) &&
      session_found->second.connection_uuid == request.header.connection_uuid;
  if (!exact_binding) {
    result.diagnostics.push_back(AuthDiagnostic(
        "PARSER_SERVER_IPC.ROUTE_ASSOCIATION_MISMATCH",
        "Disconnect requires exact nonzero connection, header, payload, and session binding.",
        "disconnect_binding_mismatch"));
    std::vector<std::uint8_t> out;
    PutString(&out, "binding_mismatch");
    PutUuid(&out, session_uuid);
    PutString(&out, "disconnect_binding_mismatch");
    result.payload = std::move(out);
    result.session_uuid = session_uuid;
    result.frame_flags =
        sbps::kFlagResponse | sbps::kFlagError | sbps::kFlagFinal;
    result.accepted = false;
    return result;
  }
  std::array<std::uint8_t, 16> auth_context_uuid{};
  std::array<std::uint8_t, 16> connection_uuid{};
  std::uint64_t active_local_transaction_id = 0;
  std::uint64_t rolled_back_local_transaction_id = 0;
  std::uint64_t rolled_back_transaction_count = 0;
  std::uint64_t unresolved_transaction_count = 0;
  std::uint64_t temporary_rows_deleted = 0;
  std::uint64_t temporary_large_values_reclaimed = 0;
  std::uint64_t temporary_private_metadata_retired = 0;
  std::string temporary_cleanup_state = "not_run";
  struct DisconnectTransactionOutcome {
    ServerTransactionState transaction;
    std::string finality;
    std::string engine_detail;
    bool secondary_failure = false;
  };
  std::vector<DisconnectTransactionOutcome> rolled_back_transactions;
  std::vector<DisconnectTransactionOutcome> retained_transactions;
  if (session_found != registry->sessions_by_uuid.end()) {
    auth_context_uuid = session_found->second.auth_context_uuid;
    connection_uuid = session_found->second.connection_uuid;
    auto& session = session_found->second;
    std::unique_lock<std::mutex> transaction_lock;
    if (session.transaction_mutex != nullptr) {
      transaction_lock = std::unique_lock<std::mutex>(*session.transaction_mutex);
    }
    if (session.transactions_by_local_id.empty() &&
        session.local_transaction_id != 0) {
      ServerTransactionState legacy;
      legacy.local_transaction_id = session.local_transaction_id;
      legacy.snapshot_visible_through_local_transaction_id =
          session.snapshot_visible_through_local_transaction_id;
      legacy.transaction_uuid = session.transaction_uuid;
      legacy.transaction_timestamp = session.transaction_timestamp;
      session.transactions_by_local_id.emplace(legacy.local_transaction_id,
                                               std::move(legacy));
    }
    for (auto it = session.transactions_by_local_id.begin();
         it != session.transactions_by_local_id.end();) {
      const auto transaction = it->second;
      if (disconnect_reason == "parser_killed" ||
          transaction.lifecycle_state ==
              ServerTransactionLifecycleState::kFinalityUnknown) {
        it->second.lifecycle_state =
            ServerTransactionLifecycleState::kFinalityUnknown;
        active_local_transaction_id = transaction.local_transaction_id;
        ++unresolved_transaction_count;
        retained_transactions.push_back(
            {it->second, "unknown", "preexisting_finality_unknown", false});
        ++it;
        continue;
      }
      ServerSessionRecord transaction_session = session;
      transaction_session.local_transaction_id = transaction.local_transaction_id;
      transaction_session.snapshot_visible_through_local_transaction_id =
          transaction.snapshot_visible_through_local_transaction_id;
      transaction_session.transaction_uuid = transaction.transaction_uuid;
      transaction_session.transaction_timestamp = transaction.transaction_timestamp;
      engine_api::EngineRollbackTransactionRequest rollback;
      rollback.context = EngineContextForSession(transaction_session,
                                                 HostedEngineState{},
                                                 request);
      const auto rolled_back = engine_api::EngineRollbackTransaction(rollback);
      const bool finality_applied =
          rolled_back.engine_finality_known &&
          (rolled_back.rollback_finality_state ==
               "rolled_back_by_engine_inventory" ||
           rolled_back.rollback_finality_state ==
               "rolled_back_post_inventory_secondary_failure");
      if (finality_applied) {
        rolled_back_local_transaction_id = transaction.local_transaction_id;
        ++rolled_back_transaction_count;
        rolled_back_transactions.push_back(
            {transaction,
             "known_applied",
             rolled_back.rollback_finality_state,
             rolled_back.post_inventory_secondary_failure});
        it = session.transactions_by_local_id.erase(it);
      } else {
        const bool known_not_applied =
            rolled_back.engine_finality_known;
        if (!known_not_applied) {
          it->second.lifecycle_state =
              ServerTransactionLifecycleState::kFinalityUnknown;
        }
        active_local_transaction_id = transaction.local_transaction_id;
        ++unresolved_transaction_count;
        retained_transactions.push_back(
            {it->second,
             known_not_applied ? "known_not_applied" : "unknown",
             rolled_back.rollback_finality_state,
             false});
        ++it;
      }
    }
    for (const auto& unpublished :
         session.quarantined_unpublished_transactions) {
      active_local_transaction_id = unpublished.local_transaction_id;
      ++unresolved_transaction_count;
      retained_transactions.push_back(
          {unpublished,
           "unknown",
           "unpublished_identity_alias_not_safely_addressable",
           false});
    }
    session.detached_recovery_quarantined =
        !retained_transactions.empty();
    if (session.detached_recovery_quarantined) {
      const auto retained = session.transactions_by_local_id.begin();
      if (retained != session.transactions_by_local_id.end()) {
        session.default_local_transaction_id = retained->first;
        session.local_transaction_id =
            retained->second.local_transaction_id;
        session.snapshot_visible_through_local_transaction_id =
            retained->second
                .snapshot_visible_through_local_transaction_id;
        session.transaction_uuid = retained->second.transaction_uuid;
        session.transaction_timestamp =
            retained->second.transaction_timestamp;
      } else {
        // Unpublished quarantine evidence is intentionally never projected
        // into the active/default scalar compatibility fields.
        session.default_local_transaction_id = 0;
        session.local_transaction_id = 0;
        session.snapshot_visible_through_local_transaction_id = 0;
        session.transaction_uuid.clear();
        session.transaction_timestamp.clear();
      }
    }
    if (active_local_transaction_id == 0) {
      engine_api::EngineCleanupTemporarySessionRequest cleanup;
      cleanup.context = EngineContextForSession(session_found->second,
                                                HostedEngineState{},
                                                request);
      cleanup.context.local_transaction_id = 0;
      cleanup.context.transaction_uuid.canonical.clear();
      cleanup.context.snapshot_visible_through_local_transaction_id = 0;
      cleanup.context.transaction_timestamp.clear();
      const auto cleaned = engine_api::EngineCleanupTemporarySessionState(cleanup);
      if (cleaned.ok) {
        temporary_rows_deleted = cleaned.temporary_deleted_rows;
        temporary_large_values_reclaimed =
            cleaned.temporary_reclaimed_large_values;
        temporary_private_metadata_retired =
            cleaned.temporary_retired_private_metadata;
        temporary_cleanup_state = "committed";
      } else {
        temporary_cleanup_state = "failed";
      }
    } else {
      temporary_cleanup_state = "skipped_active_transaction_outcome_unknown";
    }
  }
  const bool retained_for_recovery = unresolved_transaction_count != 0;
  const std::uint64_t erased =
      retained_for_recovery ? 0 : registry->sessions_by_uuid.erase(key);
  std::uint64_t auth_contexts_removed = 0;
  if (erased != 0) {
    auth_contexts_removed += registry->auth_contexts_by_uuid.erase(AuthContextKey(auth_context_uuid));
    for (auto it = registry->auth_contexts_by_uuid.begin();
         it != registry->auth_contexts_by_uuid.end();) {
      if (it->second.session_uuid == session_uuid) {
        it = registry->auth_contexts_by_uuid.erase(it);
        ++auth_contexts_removed;
      } else {
        ++it;
      }
    }
    if (!sbps::IsZeroUuid(connection_uuid)) {
      const auto connection_key = UuidBytesToText(connection_uuid);
      registry->physical_channel_by_connection_uuid.erase(connection_key);
      registry->negotiated_capabilities_by_connection_uuid.erase(
          connection_key);
      registry->admitted_parser_identity_by_connection_uuid.erase(
          connection_key);
    }
  }
  std::uint64_t prepared_tombstoned = 0;
  std::uint64_t cursors_tombstoned = 0;
  std::uint64_t engine_results_released = 0;
  std::vector<std::array<std::uint8_t, 16>> prepared_to_close;
  for (const auto& [_, prepared] : registry->prepared_by_uuid) {
    if (prepared.session_uuid == session_uuid) {
      if (!prepared.closed) ++prepared_tombstoned;
      prepared_to_close.push_back(prepared.prepared_statement_uuid);
    }
  }
  for (const auto& prepared_uuid : prepared_to_close) {
    const auto closed = CloseServerPreparedStatement(
        registry, session_uuid, prepared_uuid, disconnect_reason);
    cursors_tombstoned += closed.cursors_closed;
    engine_results_released += closed.engine_results_released;
  }
  CloseSessionObjectHandlesForSession(registry, session_uuid, disconnect_reason);
  std::uint64_t public_abi_contexts_closed = 0;
  std::uint64_t request_finality_records_updated = 0;
  for (auto& [_, cursor] : registry->cursors_by_uuid) {
    if (cursor.session_uuid == session_uuid) {
      const bool had_engine_result = cursor.engine_result != nullptr;
      if (ReleaseAndClearServerCursorResources(registry, &cursor)) {
        ++engine_results_released;
      }
      if (!cursor.closed) ++cursors_tombstoned;
      cursor.finality_state = disconnect_reason == "parser_killed" ? "parser_killed" : "parser_disconnected";
      cursor.finality_reason = disconnect_reason;
      cursor.exhausted = true;
      cursor.closed = true;
      if (!sbps::IsZeroUuid(cursor.request_uuid)) {
        auto request_it = registry->requests_by_uuid.find(UuidBytesToText(cursor.request_uuid));
        if (request_it != registry->requests_by_uuid.end() &&
            !TerminalRequestState(request_it->second.state)) {
          const bool unknown_outcome = active_local_transaction_id != 0 || had_engine_result ||
                                       request_it->second.engine_result_retained;
          request_it->second.state = unknown_outcome
                                         ? ServerRequestLifecycleState::kUnknownOutcome
                                         : ServerRequestLifecycleState::kDisconnected;
          request_it->second.detail = unknown_outcome
                                          ? "parser_disconnect_outcome_unknown_preserved"
                                          : "parser_disconnect_resource_closed";
          request_it->second.transaction_finality_preserved = true;
          UpsertRequestFinality(registry, request_it->second);
          ++request_finality_records_updated;
        }
      }
    }
  }
  if (registry->public_abi_sessions_by_session_uuid.find(key) !=
      registry->public_abi_sessions_by_session_uuid.end()) {
    CloseServerPublicAbiSessionForSession(registry,
                                          session_uuid,
                                          disconnect_reason);
    public_abi_contexts_closed = 1;
  }
  for (auto& [_, request_record] : registry->requests_by_uuid) {
    if (request_record.session_uuid != session_uuid ||
        TerminalRequestState(request_record.state)) {
      continue;
    }
    const bool unknown_outcome = active_local_transaction_id != 0 ||
                                 request_record.engine_result_retained;
    request_record.state = unknown_outcome ? ServerRequestLifecycleState::kUnknownOutcome
                                           : ServerRequestLifecycleState::kDisconnected;
    request_record.detail = unknown_outcome
                                ? "parser_disconnect_outcome_unknown_preserved"
                                : "parser_disconnect_resource_closed";
    request_record.transaction_finality_preserved = true;
    UpsertRequestFinality(registry, request_record);
    ++request_finality_records_updated;
  }
  const auto cleanup_detail = DetachCleanupDetail(disconnect_reason,
                                                  erased,
                                                  auth_contexts_removed,
                                                  prepared_tombstoned,
                                                  cursors_tombstoned,
                                                  engine_results_released,
                                                  request_finality_records_updated,
                                                  temporary_rows_deleted,
                                                  temporary_large_values_reclaimed,
                                                  temporary_private_metadata_retired,
                                                  temporary_cleanup_state,
                                                  active_local_transaction_id);
  for (const auto& outcome : rolled_back_transactions) {
    const auto& transaction = outcome.transaction;
    result.diagnostics.push_back(DetachCleanupDiagnostic(
        outcome.secondary_failure
            ? "ENGINE.DBLC_DETACH_TRANSACTION_ROLLED_BACK_SECONDARY_FAILURE"
            : "ENGINE.DBLC_DETACH_TRANSACTION_ROLLED_BACK",
        outcome.secondary_failure ? ServerDiagnosticSeverity::kWarning
                                  : ServerDiagnosticSeverity::kInfo,
        outcome.secondary_failure
            ? "Engine inventory applied rollback finality, but a secondary cleanup failed after finality."
            : "Orderly parser disconnect rolled back an exact MGA transaction before detaching.",
        {{"local_transaction_id",
          std::to_string(transaction.local_transaction_id)},
         {"transaction_uuid", transaction.transaction_uuid},
         {"snapshot_visible_through_local_transaction_id",
          std::to_string(
              transaction
                  .snapshot_visible_through_local_transaction_id)},
         {"finality", outcome.finality},
         {"engine_finality_detail", outcome.engine_detail},
         {"post_inventory_secondary_failure",
          outcome.secondary_failure ? "true" : "false"},
         {"mga_finality_authority", "engine"}}));
  }
  for (const auto& outcome : retained_transactions) {
    const auto& transaction = outcome.transaction;
    const bool finality_unknown = outcome.finality == "unknown";
    result.diagnostics.push_back(DetachCleanupDiagnostic(
        finality_unknown
            ? "ENGINE.DBLC_TRANSACTION_OUTCOME_UNKNOWN"
            : "ENGINE.DBLC_TRANSACTION_KNOWN_NOT_APPLIED",
        ServerDiagnosticSeverity::kWarning,
        finality_unknown
            ? "Disconnect retained an exact MGA selector in recovery quarantine because engine finality is unknown."
            : "Disconnect retained an exact active MGA selector because the engine conclusively did not apply rollback.",
        {{"local_transaction_id",
          std::to_string(transaction.local_transaction_id)},
         {"transaction_uuid", transaction.transaction_uuid},
         {"snapshot_visible_through_local_transaction_id",
          std::to_string(
              transaction
                  .snapshot_visible_through_local_transaction_id)},
         {"finality", outcome.finality},
         {"engine_finality_detail", outcome.engine_detail},
         {"mga_finality_authority", "engine"}}));
  }
  if (retained_for_recovery) {
    result.diagnostics.push_back(DetachCleanupDiagnostic(
        "ENGINE.DBLC_DETACH_RECOVERY_QUARANTINED",
        ServerDiagnosticSeverity::kWarning,
        "The session remains quarantined until engine-owned transaction finality is recovered.",
        {{"session_uuid", key},
         {"unresolved_transaction_count",
          std::to_string(unresolved_transaction_count)},
         {"session_erased", "false"}}));
  }
  if (erased != 0) {
    result.diagnostics.push_back(DetachCleanupDiagnostic(
        "ENGINE.DBLC_DETACH_CLEANUP_COMPLETE",
        ServerDiagnosticSeverity::kInfo,
        "Detach cleanup released session-scoped runtime resources deterministically.",
        {{"detail", cleanup_detail},
         {"session_uuid", key},
         {"auth_contexts_removed", std::to_string(auth_contexts_removed)},
         {"prepared_tombstoned", std::to_string(prepared_tombstoned)},
         {"cursors_tombstoned", std::to_string(cursors_tombstoned)},
         {"engine_results_released", std::to_string(engine_results_released)},
         {"public_abi_contexts_closed",
          std::to_string(public_abi_contexts_closed)},
         {"request_finality_records_updated", std::to_string(request_finality_records_updated)},
         {"temporary_rows_deleted", std::to_string(temporary_rows_deleted)},
         {"temporary_large_values_reclaimed", std::to_string(temporary_large_values_reclaimed)},
         {"temporary_private_metadata_retired", std::to_string(temporary_private_metadata_retired)},
         {"temporary_cleanup_state", temporary_cleanup_state}}));
    if (rolled_back_local_transaction_id != 0) {
      result.diagnostics.push_back(DetachCleanupDiagnostic(
          "ENGINE.DBLC_DETACH_TRANSACTION_ROLLED_BACK",
          ServerDiagnosticSeverity::kInfo,
          "Orderly parser disconnect rolled back the session's active MGA transactions before detaching.",
          {{"detail", cleanup_detail},
           {"local_transaction_id", std::to_string(rolled_back_local_transaction_id)},
           {"rolled_back_transaction_count",
            std::to_string(rolled_back_transaction_count)},
           {"mga_finality_authority", "engine"}}));
    }
    if (active_local_transaction_id != 0) {
      result.diagnostics.push_back(DetachCleanupDiagnostic(
          "ENGINE.DBLC_TRANSACTION_OUTCOME_UNKNOWN",
          ServerDiagnosticSeverity::kWarning,
          "Parser disconnect detached the session but did not commit or roll back the active MGA transaction.",
          {{"detail", cleanup_detail},
           {"local_transaction_id", std::to_string(active_local_transaction_id)},
           {"unresolved_transaction_count",
            std::to_string(unresolved_transaction_count)},
           {"mga_finality_authority", "engine"}}));
    }
  }
  std::vector<std::uint8_t> out;
  const std::string disconnect_outcome =
      retained_for_recovery ? "recovery_quarantined"
                            : (erased == 0 ? "session_not_found"
                                           : "detached");
  PutString(&out, disconnect_outcome);
  PutUuid(&out, session_uuid);
  PutString(&out, cleanup_detail);
  result.payload = std::move(out);
  result.session_uuid = session_uuid;
  result.frame_flags = sbps::kFlagResponse | sbps::kFlagFinal;
  result.accepted = erased != 0 || retained_for_recovery;
  // Recovery quarantine is session-scoped.  It must not drain cursors or
  // requests owned by unrelated physical channels in this registry.
  registry->channel_state =
      retained_for_recovery || !registry->sessions_by_uuid.empty()
          ? ServerChannelState::kReady
          : (erased == 0 ? ServerChannelState::kFailed
                         : ServerChannelState::kDetached);
  RecordFinality(registry,
                 request,
                 session_uuid,
                 auth_context_uuid,
                 "disconnect_notice",
                 disconnect_outcome,
                 cleanup_detail);
  return result;
}

ServerSessionBindingControlResult ApplyServerSessionBindingReport(
    ServerSessionRegistry* registry,
    const ServerSessionBindingReport& report,
    const ServerSessionControlAuthority& authority) {
  if (registry == nullptr) {
    return SessionControlRejected("SERVER.SESSION_CONTROL.REGISTRY_REQUIRED",
                                  "session_registry_required",
                                  "Session binding requires a server session registry.");
  }
  if (!SessionControlAuthorized(authority, authority.may_report_binding)) {
    return SessionControlRejected("SERVER.SESSION_CONTROL.AUTHORIZATION_DENIED",
                                  "binding_report_authority_required",
                                  "SESSION_BINDING_REPORT requires authenticated session-control authority.",
                                  {{"operation", "session_binding_report"}});
  }
  if (IsZeroUuidBytes(report.catalog_session_id)) {
    return SessionControlRejected("SERVER.SESSION_BINDING.TARGET_REQUIRED",
                                  "catalog_session_id_required",
                                  "SESSION_BINDING_REPORT requires a catalog session id.");
  }
  auto session_it = FindMutableSessionByBindingTarget(registry,
                                                      report.catalog_session_id,
                                                      report.protocol_session_id);
  if (session_it == registry->sessions_by_uuid.end()) {
    return SessionControlRejected("SERVER.SESSION_BINDING.SESSION_NOT_FOUND",
                                  "session_not_found",
                                  "SESSION_BINDING_REPORT target session is not active.",
                                  {{"catalog_session_id", UuidBytesToText(report.catalog_session_id)}});
  }
  auto& session = session_it->second;
  if (authority.sequence <= session.session_binding_control_sequence) {
    return SessionControlRejected("SERVER.SESSION_CONTROL.REPLAY_REFUSED",
                                  "binding_sequence_replay",
                                  "SESSION_BINDING_REPORT sequence was already consumed.",
                                  {{"sequence", std::to_string(authority.sequence)},
                                   {"last_sequence", std::to_string(session.session_binding_control_sequence)}});
  }
  if (!UuidMatchesIfPresent(report.attachment_id, session.connection_uuid)) {
    return SessionControlRejected("SERVER.SESSION_BINDING.ROUTE_MISMATCH",
                                  "attachment_id_mismatch",
                                  "SESSION_BINDING_REPORT attachment did not match the server session.",
                                  {{"session_uuid", session_it->first}});
  }
  if (!UuidMatchesIfPresent(report.authenticated_principal_id, session.principal_uuid)) {
    return SessionControlRejected("SERVER.SESSION_BINDING.PRINCIPAL_MISMATCH",
                                  "authenticated_principal_id_mismatch",
                                  "SESSION_BINDING_REPORT principal did not match the server session.",
                                  {{"session_uuid", session_it->first}});
  }
  if (!UuidMatchesIfPresent(report.session_user_id, session.effective_user_uuid)) {
    return SessionControlRejected("SERVER.SESSION_BINDING.USER_MISMATCH",
                                  "session_user_id_mismatch",
                                  "SESSION_BINDING_REPORT user did not match the server session.",
                                  {{"session_uuid", session_it->first}});
  }
  if (report.current_txn_id != session.local_transaction_id) {
    return SessionControlRejected("SERVER.SESSION_BINDING.TRANSACTION_MISMATCH",
                                  "current_txn_id_mismatch",
                                  "SESSION_BINDING_REPORT transaction id did not match the engine-owned session transaction.",
                                  {{"session_uuid", session_it->first},
                                   {"reported_txn_id", std::to_string(report.current_txn_id)},
                                   {"server_txn_id", std::to_string(session.local_transaction_id)}});
  }

  session.session_binding_present = true;
  session.attachment_id = report.attachment_id;
  session.catalog_session_id = report.catalog_session_id;
  session.protocol_session_id = report.protocol_session_id;
  session.authkey_id = report.authkey_id;
  session.active_role_uuid = report.active_role_id;
  session.effective_role_uuids.clear();
  AddUniqueUuidBytes(&session.effective_role_uuids, report.active_role_id);
  session.effective_group_uuids = report.effective_group_ids;
  if (!IsZeroUuidBytes(report.transaction_uuid)) {
    session.transaction_uuid = UuidBytesToText(report.transaction_uuid);
  }
  session.session_binding_generation += 1;
  session.session_binding_control_sequence = authority.sequence;
  session.session_binding_authority_class = authority.authority_class;
  session.session_binding_actor_token = authority.actor_token;
  registry->channel_state = ServerChannelState::kSessionBound;
  MirrorAuthContext(registry, session);

  ServerSessionBindingControlResult result;
  result.accepted = true;
  result.mutated = true;
  result.target_session_uuid = session_it->first;
  result.detail = "session_binding_report_applied";
  return result;
}

ServerSessionBindingControlResult ClearServerSessionBinding(
    ServerSessionRegistry* registry,
    const ServerSessionTakeoverRequest& target,
    const ServerSessionControlAuthority& authority) {
  if (registry == nullptr) {
    return SessionControlRejected("SERVER.SESSION_CONTROL.REGISTRY_REQUIRED",
                                  "session_registry_required",
                                  "Session binding clear requires a server session registry.");
  }
  if (!SessionControlAuthorized(authority, authority.may_clear_binding)) {
    return SessionControlRejected("SERVER.SESSION_CONTROL.AUTHORIZATION_DENIED",
                                  "binding_clear_authority_required",
                                  "SESSION_BINDING_CLEAR requires authenticated session-control authority.",
                                  {{"operation", "session_binding_clear"}});
  }
  auto session_it = FindMutableSessionByBindingTarget(registry,
                                                      target.catalog_session_id,
                                                      target.protocol_session_id);
  if (session_it == registry->sessions_by_uuid.end()) {
    return SessionControlRejected("SERVER.SESSION_BINDING.SESSION_NOT_FOUND",
                                  "session_not_found",
                                  "SESSION_BINDING_CLEAR target session is not active.");
  }
  auto& session = session_it->second;
  if (authority.sequence <= session.session_binding_control_sequence) {
    return SessionControlRejected("SERVER.SESSION_CONTROL.REPLAY_REFUSED",
                                  "binding_clear_sequence_replay",
                                  "SESSION_BINDING_CLEAR sequence was already consumed.",
                                  {{"sequence", std::to_string(authority.sequence)},
                                   {"last_sequence", std::to_string(session.session_binding_control_sequence)}});
  }
  session.session_binding_present = false;
  session.attachment_id = {};
  session.catalog_session_id = {};
  session.protocol_session_id = {};
  session.authkey_id = {};
  session.active_role_uuid = {};
  session.effective_role_uuids.clear();
  session.effective_group_uuids.clear();
  session.session_binding_generation += 1;
  session.session_binding_control_sequence = authority.sequence;
  session.session_binding_authority_class = authority.authority_class;
  session.session_binding_actor_token = authority.actor_token;
  registry->channel_state = ServerChannelState::kReady;
  MirrorAuthContext(registry, session);

  ServerSessionBindingControlResult result;
  result.accepted = true;
  result.mutated = true;
  result.target_session_uuid = session_it->first;
  result.detail = "session_binding_cleared";
  return result;
}

ServerSessionBindingControlResult EvaluateServerSessionTakeoverProbe(
    const ServerSessionRegistry& registry,
    const ServerSessionTakeoverRequest& request,
    const ServerSessionControlAuthority& authority) {
  if (!SessionControlAuthorized(authority, authority.may_takeover)) {
    return SessionControlRejected("SERVER.SESSION_CONTROL.AUTHORIZATION_DENIED",
                                  "takeover_authority_required",
                                  "TAKEOVER_PROBE requires authenticated takeover authority.",
                                  {{"operation", "takeover_probe"}});
  }
  auto session_it = FindSessionByBindingTarget(registry,
                                               request.catalog_session_id,
                                               request.protocol_session_id);
  if (session_it == registry.sessions_by_uuid.end()) {
    return SessionControlRejected("SERVER.SESSION_TAKEOVER.SESSION_NOT_FOUND",
                                  "session_not_found",
                                  "TAKEOVER_PROBE target session is not active.");
  }
  std::string detail;
  const bool claims_match = TakeoverClaimsMatch(session_it->second, request, &detail);
  const auto physical_projection = ProjectTakeoverPhysicalChannel(
      registry, session_it->second, request);
  ServerSessionBindingControlResult result;
  result.accepted = true;
  result.target_session_uuid = session_it->first;
  result.detail = detail;
  if (session_it->second.session_binding_present) {
    result.probe_flags |= kServerTakeoverProbeSessionBound;
  }
  result.probe_flags |= kServerTakeoverProbeAuthorityAccepted;
  if (session_it->second.local_transaction_id != 0) {
    result.probe_flags |= kServerTakeoverProbeActiveTransaction;
  }
  if (claims_match &&
      (!physical_projection.connection_changes ||
       physical_projection.physical_channel_admitted)) {
    result.probe_flags |= kServerTakeoverProbeTakeoverWouldPass;
    result.takeover_allowed = true;
  } else if (claims_match) {
    result.detail = "takeover_physical_channel_not_admitted";
    result.diagnostic_code =
        "SERVER.SESSION_TAKEOVER.PHYSICAL_CHANNEL_REQUIRED";
  } else {
    result.diagnostic_code = "SERVER.SESSION_TAKEOVER.CLAIM_MISMATCH";
  }
  return result;
}

ServerSessionBindingControlResult ApplyServerSessionTakeoverRequest(
    ServerSessionRegistry* registry,
    const ServerSessionTakeoverRequest& request,
    const ServerSessionControlAuthority& authority) {
  if (registry == nullptr) {
    return SessionControlRejected("SERVER.SESSION_CONTROL.REGISTRY_REQUIRED",
                                  "session_registry_required",
                                  "TAKEOVER_REQUEST requires a server session registry.");
  }
  if (!SessionControlAuthorized(authority, authority.may_takeover)) {
    return SessionControlRejected("SERVER.SESSION_CONTROL.AUTHORIZATION_DENIED",
                                  "takeover_authority_required",
                                  "TAKEOVER_REQUEST requires authenticated takeover authority.",
                                  {{"operation", "takeover_request"}});
  }
  auto session_it = FindMutableSessionByBindingTarget(registry,
                                                      request.catalog_session_id,
                                                      request.protocol_session_id);
  if (session_it == registry->sessions_by_uuid.end()) {
    return SessionControlRejected("SERVER.SESSION_TAKEOVER.SESSION_NOT_FOUND",
                                  "session_not_found",
                                  "TAKEOVER_REQUEST target session is not active.");
  }
  auto& session = session_it->second;
  if (authority.sequence <= session.takeover_control_sequence) {
    return SessionControlRejected("SERVER.SESSION_TAKEOVER.REPLAY_REFUSED",
                                  "takeover_sequence_replay",
                                  "TAKEOVER_REQUEST sequence was already consumed.",
                                  {{"sequence", std::to_string(authority.sequence)},
                                   {"last_sequence", std::to_string(session.takeover_control_sequence)}});
  }
  std::string detail;
  if (!TakeoverClaimsMatch(session, request, &detail)) {
    return SessionControlRejected("SERVER.SESSION_TAKEOVER.CLAIM_MISMATCH",
                                  detail,
                                  "TAKEOVER_REQUEST claims did not match the server-owned session binding.",
                                  {{"session_uuid", session_it->first}});
  }

  const auto physical_projection =
      ProjectTakeoverPhysicalChannel(*registry, session, request);
  if (physical_projection.connection_changes &&
      !physical_projection.physical_channel_admitted) {
    return SessionControlRejected(
        "SERVER.SESSION_TAKEOVER.PHYSICAL_CHANNEL_REQUIRED",
        "takeover_physical_channel_not_admitted",
        "TAKEOVER_REQUEST destination is not an admitted physical parser channel.",
        {{"session_uuid", session_it->first},
         {"destination_connection_uuid",
          UuidBytesToText(request.attachment_id)}});
  }

  if ((request.mask & kServerTakeoverClaimAttachmentId) &&
      !IsZeroUuidBytes(request.attachment_id)) {
    if (physical_projection.connection_changes) {
      RetireTransferablePreparedBindingsForPhysicalChannelChange(
          registry, session.session_uuid);
      session.server_channel_uuid = physical_projection.server_channel_uuid;
      session.transaction_routing_v2_negotiated =
          physical_projection.transaction_routing_v2_negotiated;
      session.prepared_metadata_transfer_v1_negotiated =
          physical_projection.prepared_metadata_transfer_v1_negotiated;
      session.relation_descriptor_projection_v3_negotiated =
          physical_projection.relation_descriptor_projection_v3_negotiated;
    }
    session.attachment_id = request.attachment_id;
    session.connection_uuid = request.attachment_id;
  }
  if ((request.mask & kServerTakeoverClaimProtocolSessionId) &&
      !IsZeroUuidBytes(request.protocol_session_id)) {
    session.protocol_session_id = request.protocol_session_id;
  }
  session.takeover_generation += 1;
  session.takeover_control_sequence = authority.sequence;
  session.session_binding_authority_class = authority.authority_class;
  session.session_binding_actor_token = authority.actor_token;
  registry->channel_state = ServerChannelState::kSessionBound;
  MirrorAuthContext(registry, session);

  ServerSessionBindingControlResult result;
  result.accepted = true;
  result.mutated = true;
  result.takeover_allowed = true;
  result.target_session_uuid = session_it->first;
  result.detail = "takeover_accepted";
  return result;
}

std::string SessionRegistryStatusJson(const ServerSessionRegistry& registry) {
  std::ostringstream out;
  out << "{\"session_registry\":{\"channel_state\":\""
      << ServerChannelStateName(registry.channel_state) << "\",\"active_sessions\":"
      << registry.sessions_by_uuid.size() << ",\"auth_contexts\":"
      << registry.auth_contexts_by_uuid.size() << ",\"prepared_statements\":"
      << registry.prepared_by_uuid.size() << ",\"cursors\":"
      << registry.cursors_by_uuid.size() << ",\"requests\":"
      << registry.requests_by_uuid.size() << ",\"finality_records\":"
      << registry.finality_by_request_uuid.size() << ",\"sessions\":[";
  bool first = true;
  for (const auto& [_, session] : registry.sessions_by_uuid) {
    const auto language = ServerLanguageContextForSession(session);
    if (!first) out << ',';
    first = false;
    out << "{\"session_uuid\":\"" << UuidBytesToText(session.session_uuid)
        << "\",\"principal\":\"" << JsonEscape(session.principal_claim)
        << "\",\"database_path\":\"" << JsonEscape(session.database_path)
        << "\",\"attach_mode\":\"" << JsonEscape(session.attach_mode)
        << "\",\"language_profile_id\":\""
        << JsonEscape(language.language_profile_id)
        << "\",\"language_tag\":\"" << JsonEscape(language.language_tag)
        << "\",\"default_language_tag\":\""
        << JsonEscape(language.default_language_tag)
        << "\",\"input_syntax_profile\":\""
        << JsonEscape(language.input_syntax_profile)
        << "\",\"input_language_fallback_tag\":\""
        << JsonEscape(language.input_language_fallback_tag)
        << "\",\"common_resource_hash\":\""
        << JsonEscape(language.common_resource_hash)
        << "\",\"language_resource_epoch\":"
        << language.language_resource_epoch
        << ",\"localized_name_epoch\":" << language.localized_name_epoch
        << ",\"message_resource_epoch\":" << language.message_resource_epoch
        << ",\"resource_compatibility_identity\":\""
        << JsonEscape(language.resource_compatibility_identity)
        << "\",\"resource_version_identity\":\""
        << JsonEscape(language.resource_version_identity)
        << "\",\"session_binding_present\":"
        << (session.session_binding_present ? "true" : "false")
        << ",\"attachment_id\":\"" << UuidBytesToText(session.attachment_id)
        << "\",\"catalog_session_id\":\"" << UuidBytesToText(session.catalog_session_id)
        << "\",\"protocol_session_id\":\"" << UuidBytesToText(session.protocol_session_id)
        << "\",\"authkey_id\":\"" << UuidBytesToText(session.authkey_id)
        << "\",\"active_role_id\":\"" << UuidBytesToText(session.active_role_uuid)
        << "\",\"effective_role_count\":" << session.effective_role_uuids.size()
        << ",\"effective_group_count\":" << session.effective_group_uuids.size()
        << ",\"session_binding_generation\":" << session.session_binding_generation
        << ",\"session_binding_control_sequence\":"
        << session.session_binding_control_sequence
        << ",\"takeover_generation\":" << session.takeover_generation
        << ",\"takeover_control_sequence\":" << session.takeover_control_sequence
        << ",\"session_binding_authority_class\":\""
        << JsonEscape(session.session_binding_authority_class)
        << "\",\"session_binding_actor_token\":\""
        << JsonEscape(session.session_binding_actor_token)
        << "\",\"database_engine_agent_state\":\""
        << JsonEscape(session.database_engine_agent_state)
        << "\",\"database_engine_agent_health_generation\":"
        << session.database_engine_agent_health_generation
        << ",\"database_engine_agent_ordinary_admission_allowed\":"
        << (session.database_engine_agent_ordinary_admission_allowed ? "true" : "false")
        << ",\"database_engine_agent_health\":"
        << (session.database_engine_agent_health_json.empty()
                ? "{\"database_engine_agent\":{\"agent_state\":\"not_started\"}}"
                : session.database_engine_agent_health_json)
        << ",\"config_source_epoch\":" << session.config_source_epoch
        << ",\"config_reload_generation\":" << session.config_reload_generation
        << ",\"capability_policy_generation\":" << session.capability_policy_generation
        << ",\"policy_generation\":" << session.policy_generation
        << ",\"security_epoch\":" << session.security_epoch
        << ",\"security_provider_generation\":" << session.security_provider_generation
        << ",\"cache_invalidation_epoch\":" << session.cache_invalidation_epoch
        << ",\"config_policy_security_lifecycle\":"
        << (session.config_policy_security_lifecycle_json.empty()
                ? "{\"config_policy_security_lifecycle\":{\"present\":false}}"
                : session.config_policy_security_lifecycle_json)
        << "}";
  }
  out << "]}}\n";
  return out.str();
}

SessionOperationResult HandleCoordinateCursorOpen(
    ServerSessionRegistry* registry, const HostedEngineState& engine_state,
    const sbps::Frame& request) {
  SessionOperationResult result;
  result.response_message_type = 79;
  result.response_schema_id = sbps::kSchemaCoordinateCursorOpenResultV1;
  result.frame_flags = sbps::kFlagResponse | sbps::kFlagFinal;
  result.session_uuid = request.header.session_uuid;
  auto refuse = [&](std::string code, std::string detail) {
    result.frame_flags |= sbps::kFlagError;
    result.diagnostics.push_back(sbps::IpcDiagnostic(
        std::move(code), "parser_server_ipc.cursor_open_coordination_refused",
        "Cursor open coordination was refused.",
        {{"detail", std::move(detail)}}));
    return result;
  };
  scratchbird::engine::sblr::SblrCursorOpenRequestV1 request_value;
  std::string detail;
  if (!registry || !scratchbird::engine::sblr::DecodeSblrCursorOpenRequestV1(
                       request.payload.data(), request.payload.size(),
                       &request_value, &detail))
    return refuse("SBLR.OPERAND_INVALID", detail);
  const auto session = registry->sessions_by_uuid.find(
      UuidBytesToText(request.header.session_uuid));
  if (session == registry->sessions_by_uuid.end())
    return refuse("SECURITY.ACCESS_DENIED", "session_hidden");
  const auto receipt_uuid = UuidBytesToText(request_value.receipt);
  ServerStatementContextRecord* receipt = nullptr;
  {
    std::lock_guard<std::mutex> guard(*registry->statement_context_mutex);
    for (auto& [unused, value] : registry->statement_contexts_by_statement_uuid) {
      (void)unused;
      if (!value.released && value.view.receipt_uuid == receipt_uuid) {
        receipt = &value;
        break;
      }
    }
  }
  if (!receipt || receipt->session_uuid != request.header.session_uuid)
    return refuse("SECURITY.ACCESS_DENIED", "cursor_receipt_hidden");
  auto context = EngineContextForSession(session->second, engine_state, request);
  context.statement_uuid.canonical = receipt_uuid;
  context.statement_metadata_snapshot_engine_owned = true;
  context.trace_tags.push_back("private_executable_plan_receipt_compiler");
  const auto coordinated =
      engine_api::CompileAndPublishSblrExecutablePlanReceipt(
          context, receipt_uuid, request_value.occurrence, request_value.mode,
          request_value.hold, request_value.fetch_size,
          receipt->view.cursor_open_executor_availability_generation);
  if (!coordinated.ok)
    return refuse(coordinated.diagnostic.code,
                  coordinated.diagnostic.message_key);
  scratchbird::engine::sblr::SblrCursorOpenDescriptorV1 descriptor;
  descriptor.descriptor = TextToUuid(coordinated.snapshot.descriptor_uuid);
  descriptor.descriptor_generation = coordinated.snapshot.descriptor_generation;
  descriptor.plan = TextToUuid(coordinated.snapshot.plan_uuid);
  descriptor.plan_generation = coordinated.snapshot.plan_generation;
  descriptor.row_shape = TextToUuid(coordinated.snapshot.row_shape_uuid);
  descriptor.row_shape_generation = coordinated.snapshot.row_shape_generation;
  descriptor.transaction = TextToUuid(coordinated.snapshot.transaction_uuid);
  descriptor.session = TextToUuid(coordinated.snapshot.session_uuid);
  descriptor.security = TextToUuid(coordinated.snapshot.security_uuid);
  const auto decode_sha = [](const std::string& text, auto* output) {
    if (text.size() != 71 || text.substr(0, 7) != "sha256:") return false;
    for (std::size_t index = 0; index < 32; ++index) {
      const auto byte = text.substr(7 + index * 2, 2);
      (*output)[index] = static_cast<std::uint8_t>(
          std::stoul(byte, nullptr, 16));
    }
    return true;
  };
  if (!decode_sha(coordinated.snapshot.plan_evidence_sha256,
                  &descriptor.plan_evidence) ||
      !decode_sha(coordinated.snapshot.descriptor_evidence_sha256,
                  &descriptor.descriptor_evidence))
    return refuse("CURSOR.OPEN_FAILED", "cursor_evidence_invalid");
  descriptor.catalog_generation = coordinated.snapshot.catalog_generation;
  descriptor.mode = coordinated.snapshot.mode;
  descriptor.hold = coordinated.snapshot.hold;
  descriptor.fetch_size = coordinated.snapshot.fetch_size;
  descriptor.availability_generation =
      coordinated.snapshot.availability_generation;
  result.payload =
      scratchbird::engine::sblr::EncodeSblrCursorOpenDescriptorV1(descriptor);
  if (result.payload.empty())
    return refuse("CURSOR.OPEN_FAILED", "COCD_encode_failed");
  result.accepted = true;
  return result;
}

SessionOperationResult HandleCoordinateReadByKey(ServerSessionRegistry*registry,const HostedEngineState&engine_state,const sbps::Frame&request){SessionOperationResult result;result.response_message_type=81;result.response_schema_id=sbps::kSchemaCoordinateReadByKeyResultV1;result.frame_flags=sbps::kFlagResponse|sbps::kFlagFinal;result.session_uuid=request.header.session_uuid;auto refuse=[&](std::string code,std::string detail){result.frame_flags|=sbps::kFlagError;result.diagnostics.push_back(sbps::IpcDiagnostic(std::move(code),"parser_server_ipc.read_by_key_coordination_refused","Read-by-key coordination was refused.",{{"detail",std::move(detail)}}));return result;};scratchbird::engine::sblr::SblrReadByKeyRequestV1 value;std::string detail;if(!registry||!scratchbird::engine::sblr::DecodeSblrReadByKeyRequestV1(request.payload.data(),request.payload.size(),&value,&detail))return refuse("SBLR.OPERAND_INVALID",detail);auto session=registry->sessions_by_uuid.find(UuidBytesToText(request.header.session_uuid));if(session==registry->sessions_by_uuid.end())return refuse("SECURITY.ACCESS_DENIED","session_hidden");auto receipt_uuid=UuidBytesToText(value.receipt);ServerStatementContextRecord*receipt=nullptr;{std::lock_guard<std::mutex>guard(*registry->statement_context_mutex);for(auto&[unused,row]:registry->statement_contexts_by_statement_uuid){(void)unused;if(!row.released&&row.view.receipt_uuid==receipt_uuid){receipt=&row;break;}}}if(!receipt||receipt->session_uuid!=request.header.session_uuid)return refuse("SECURITY.ACCESS_DENIED","read_receipt_hidden");auto context=EngineContextForSession(session->second,engine_state,request);context.statement_uuid.canonical=receipt_uuid;context.statement_metadata_snapshot_engine_owned=true;context.trace_tags.push_back("private_point_read_compiler");auto coordinated=engine_api::CompileSblrReadByKeyDescriptor(context,receipt_uuid,value.occurrence,value.key_occurrence,receipt->view.read_by_key_executor_availability_generation);if(!coordinated.ok)return refuse(coordinated.diagnostic.code,coordinated.diagnostic.message_key);result.payload=scratchbird::engine::sblr::EncodeSblrReadByKeyDescriptorV1(coordinated.descriptor,false);if(result.payload.empty())return refuse("READ.BY_KEY_FAILED","RBKD_encode_failed");result.accepted=true;return result;}

SessionOperationResult HandleCoordinateReadRange(ServerSessionRegistry*registry,const HostedEngineState&engine_state,const sbps::Frame&request){SessionOperationResult result;result.response_message_type=83;result.response_schema_id=sbps::kSchemaCoordinateReadRangeResultV1;result.frame_flags=sbps::kFlagResponse|sbps::kFlagFinal;result.session_uuid=request.header.session_uuid;auto refuse=[&](std::string code,std::string detail){result.frame_flags|=sbps::kFlagError;result.diagnostics.push_back(sbps::IpcDiagnostic(std::move(code),"parser_server_ipc.read_range_coordination_refused","Read-range coordination was refused.",{{"detail",std::move(detail)}}));return result;};scratchbird::engine::sblr::SblrReadRangeRequestV1 value;std::string detail;if(!registry||!scratchbird::engine::sblr::DecodeSblrReadRangeRequestV1(request.payload.data(),request.payload.size(),&value,&detail))return refuse("SBLR.OPERAND_INVALID",detail);auto session=registry->sessions_by_uuid.find(UuidBytesToText(request.header.session_uuid));if(session==registry->sessions_by_uuid.end())return refuse("SECURITY.ACCESS_DENIED","session_hidden");auto receipt_uuid=UuidBytesToText(value.receipt);ServerStatementContextRecord*receipt=nullptr;{std::lock_guard<std::mutex>guard(*registry->statement_context_mutex);for(auto&[unused,row]:registry->statement_contexts_by_statement_uuid){(void)unused;if(!row.released&&row.view.receipt_uuid==receipt_uuid){receipt=&row;break;}}}if(!receipt||receipt->session_uuid!=request.header.session_uuid)return refuse("SECURITY.ACCESS_DENIED","range_receipt_hidden");auto context=EngineContextForSession(session->second,engine_state,request);context.statement_uuid.canonical=receipt_uuid;context.statement_metadata_snapshot_engine_owned=true;context.trace_tags.push_back("private_range_read_compiler");auto coordinated=engine_api::CompileSblrReadRangeDescriptor(context,receipt_uuid,value.occurrence,value.range_occurrence,receipt->view.read_range_executor_availability_generation);if(!coordinated.ok)return refuse(coordinated.diagnostic.code,coordinated.diagnostic.message_key);result.payload=scratchbird::engine::sblr::EncodeSblrReadRangeDescriptorV1(coordinated.descriptor,false);if(result.payload.empty())return refuse("READ.RANGE_FAILED","RRGD_encode_failed");result.accepted=true;return result;}

SessionOperationResult HandleCoordinateReadStream(ServerSessionRegistry*registry,const HostedEngineState&engine_state,const sbps::Frame&request){SessionOperationResult result;result.response_message_type=85;result.response_schema_id=sbps::kSchemaCoordinateReadStreamResultV1;result.frame_flags=sbps::kFlagResponse|sbps::kFlagFinal;result.session_uuid=request.header.session_uuid;auto refuse=[&](std::string code,std::string detail){result.frame_flags|=sbps::kFlagError;result.diagnostics.push_back(sbps::IpcDiagnostic(std::move(code),"parser_server_ipc.read_stream_coordination_refused","Read-stream coordination was refused.",{{"detail",std::move(detail)}}));return result;};scratchbird::engine::sblr::SblrReadStreamRequestV1 value;std::string detail;if(!registry||!scratchbird::engine::sblr::DecodeSblrReadStreamRequestV1(request.payload.data(),request.payload.size(),&value,&detail))return refuse("SBLR.OPERAND_INVALID",detail);auto session=registry->sessions_by_uuid.find(UuidBytesToText(request.header.session_uuid));if(session==registry->sessions_by_uuid.end())return refuse("SECURITY.ACCESS_DENIED","session_hidden");auto receipt_uuid=UuidBytesToText(value.receipt);ServerStatementContextRecord*receipt=nullptr;{std::lock_guard<std::mutex>guard(*registry->statement_context_mutex);for(auto&[unused,row]:registry->statement_contexts_by_statement_uuid){(void)unused;if(!row.released&&row.view.receipt_uuid==receipt_uuid){receipt=&row;break;}}}if(!receipt||receipt->session_uuid!=request.header.session_uuid)return refuse("SECURITY.ACCESS_DENIED","stream_receipt_hidden");auto context=EngineContextForSession(session->second,engine_state,request);context.statement_uuid.canonical=receipt_uuid;context.statement_metadata_snapshot_engine_owned=true;context.trace_tags.push_back("private_stream_read_compiler");auto coordinated=engine_api::CompileSblrReadStreamDescriptor(context,receipt_uuid,value.occurrence,value.stream_occurrence,receipt->view.read_stream_executor_availability_generation);if(!coordinated.ok)return refuse(coordinated.diagnostic.code,coordinated.diagnostic.message_key);result.payload=scratchbird::engine::sblr::EncodeSblrReadStreamDescriptorV1(coordinated.descriptor,false);if(result.payload.empty())return refuse("READ.STREAM_FAILED","RSDD_encode_failed");result.accepted=true;return result;}
SessionOperationResult HandleCoordinateResultSetPass(ServerSessionRegistry*registry,const HostedEngineState&engine_state,const sbps::Frame&request){SessionOperationResult result;result.response_message_type=87;result.response_schema_id=sbps::kSchemaCoordinateResultSetPassResultV1;result.frame_flags=sbps::kFlagResponse|sbps::kFlagFinal;result.session_uuid=request.header.session_uuid;auto refuse=[&](std::string code,std::string detail){result.frame_flags|=sbps::kFlagError;result.diagnostics.push_back(sbps::IpcDiagnostic(std::move(code),"parser_server_ipc.result_set_pass_coordination_refused","Result-set pass coordination was refused.",{{"detail",std::move(detail)}}));return result;};scratchbird::engine::sblr::SblrResultSetPassRequestV1 value;std::string detail;if(!registry||!scratchbird::engine::sblr::DecodeSblrResultSetPassRequestV1(request.payload.data(),request.payload.size(),&value,&detail))return refuse("SBLR.OPERAND_INVALID",detail);auto session=registry->sessions_by_uuid.find(UuidBytesToText(request.header.session_uuid));if(session==registry->sessions_by_uuid.end())return refuse("SECURITY.ACCESS_DENIED","session_hidden");auto receipt_uuid=UuidBytesToText(value.receipt);ServerStatementContextRecord*receipt=nullptr;{std::lock_guard<std::mutex>guard(*registry->statement_context_mutex);for(auto&[unused,row]:registry->statement_contexts_by_statement_uuid){(void)unused;if(!row.released&&row.view.receipt_uuid==receipt_uuid){receipt=&row;break;}}}if(!receipt||receipt->session_uuid!=request.header.session_uuid)return refuse("SECURITY.ACCESS_DENIED","result_set_pass_receipt_hidden");auto context=EngineContextForSession(session->second,engine_state,request);context.statement_uuid.canonical=receipt_uuid;context.statement_metadata_snapshot_engine_owned=true;context.trace_tags.push_back("private_result_set_pass_compiler");auto coordinated=engine_api::CompileSblrResultSetPassDescriptor(context,receipt_uuid,value.occurrence,value.pass_occurrence,receipt->view.read_stream_executor_availability_generation);if(!coordinated.ok)return refuse(coordinated.diagnostic.code,coordinated.diagnostic.message_key);result.payload=scratchbird::engine::sblr::EncodeSblrResultSetPassDescriptorV1(coordinated.descriptor,false);if(result.payload.empty())return refuse("RESULT_SET.PASS_FAILED","RSPD_encode_failed");result.accepted=true;return result;}

SessionOperationResult HandleCoordinateAccessCursorOpen(ServerSessionRegistry*registry,const HostedEngineState&engine_state,const sbps::Frame&request){SessionOperationResult result;result.response_message_type=89;result.response_schema_id=sbps::kSchemaCoordinateAccessCursorOpenResultV1;result.frame_flags=sbps::kFlagResponse|sbps::kFlagFinal;result.session_uuid=request.header.session_uuid;auto refuse=[&](std::string code,std::string detail){result.frame_flags|=sbps::kFlagError;result.diagnostics.push_back(sbps::IpcDiagnostic(std::move(code),"parser_server_ipc.access_cursor_open_refused","Access cursor coordination was refused.",{{"detail",std::move(detail)}}));return result;};scratchbird::engine::sblr::SblrAccessCursorOpenRequestV1 value;std::string detail;if(!registry||!scratchbird::engine::sblr::DecodeSblrAccessCursorOpenRequestV1(request.payload.data(),request.payload.size(),&value,&detail))return refuse("SBLR.OPERAND_INVALID",detail);auto session=registry->sessions_by_uuid.find(UuidBytesToText(request.header.session_uuid));if(session==registry->sessions_by_uuid.end())return refuse("SECURITY.ACCESS_DENIED","session_hidden");auto receipt_uuid=UuidBytesToText(value.receipt);ServerStatementContextRecord*receipt=nullptr;{std::lock_guard<std::mutex>guard(*registry->statement_context_mutex);for(auto&[unused,row]:registry->statement_contexts_by_statement_uuid){(void)unused;if(!row.released&&row.view.receipt_uuid==receipt_uuid){receipt=&row;break;}}}if(!receipt||receipt->session_uuid!=request.header.session_uuid)return refuse("SECURITY.ACCESS_DENIED","access_cursor_receipt_hidden");auto context=EngineContextForSession(session->second,engine_state,request);context.statement_uuid.canonical=receipt_uuid;context.statement_metadata_snapshot_engine_owned=true;context.trace_tags.push_back("private_access_cursor_compiler");auto coordinated=engine_api::CompileSblrAccessCursorOpenDescriptor(context,receipt_uuid,value.occurrence,value.cursor_occurrence,receipt->view.access_cursor_open_executor_availability_generation);if(!coordinated.ok)return refuse(coordinated.diagnostic.code,coordinated.diagnostic.message_key);result.payload=scratchbird::engine::sblr::EncodeSblrAccessCursorOpenDescriptorV1(coordinated.descriptor,false);if(result.payload.empty())return refuse("CURSOR.OPEN_FAILED","ACDD_encode_failed");result.accepted=true;return result;}
SessionOperationResult HandleCoordinateAccessCursorFetch(ServerSessionRegistry*registry,const HostedEngineState&engine_state,const sbps::Frame&request){SessionOperationResult result;result.response_message_type=91;result.response_schema_id=sbps::kSchemaCoordinateAccessCursorFetchResultV1;result.frame_flags=sbps::kFlagResponse|sbps::kFlagFinal;result.session_uuid=request.header.session_uuid;auto refuse=[&](std::string code,std::string detail){result.frame_flags|=sbps::kFlagError;result.diagnostics.push_back(sbps::IpcDiagnostic(std::move(code),"parser_server_ipc.access_cursor_fetch_refused","Access cursor fetch coordination was refused.",{{"detail",std::move(detail)}}));return result;};scratchbird::engine::sblr::SblrAccessCursorFetchRequestV1 value;std::string detail;if(!registry||!scratchbird::engine::sblr::DecodeSblrAccessCursorFetchRequestV1(request.payload.data(),request.payload.size(),&value,&detail))return refuse("SBLR.OPERAND_INVALID",detail);auto session=registry->sessions_by_uuid.find(UuidBytesToText(request.header.session_uuid));if(session==registry->sessions_by_uuid.end())return refuse("SECURITY.ACCESS_DENIED","session_hidden");auto receipt_uuid=UuidBytesToText(value.receipt);ServerStatementContextRecord*receipt=nullptr;{std::lock_guard<std::mutex>guard(*registry->statement_context_mutex);for(auto&[unused,row]:registry->statement_contexts_by_statement_uuid){(void)unused;if(!row.released&&row.view.receipt_uuid==receipt_uuid){receipt=&row;break;}}}if(!receipt||receipt->session_uuid!=request.header.session_uuid)return refuse("SECURITY.ACCESS_DENIED","access_cursor_fetch_receipt_hidden");auto context=EngineContextForSession(session->second,engine_state,request);context.statement_uuid.canonical=receipt_uuid;context.statement_metadata_snapshot_engine_owned=true;context.trace_tags.push_back("private_access_cursor_fetch_compiler");auto coordinated=engine_api::CompileSblrAccessCursorFetchDescriptor(context,receipt_uuid,value.occurrence,value.fetch_occurrence,receipt->view.access_cursor_fetch_executor_availability_generation);if(!coordinated.ok)return refuse(coordinated.diagnostic.code,coordinated.diagnostic.message_key);result.payload=scratchbird::engine::sblr::EncodeSblrAccessCursorFetchDescriptorV1(coordinated.descriptor,false);if(result.payload.empty())return refuse("CURSOR.FETCH_FAILED","ACFD_encode_failed");result.accepted=true;return result;}
SessionOperationResult HandleCoordinateAccessCursorClose(ServerSessionRegistry*registry,const HostedEngineState&engine_state,const sbps::Frame&request){SessionOperationResult result;result.response_message_type=93;result.response_schema_id=sbps::kSchemaCoordinateAccessCursorCloseResultV1;result.frame_flags=sbps::kFlagResponse|sbps::kFlagFinal;result.session_uuid=request.header.session_uuid;auto refuse=[&](std::string code,std::string detail){result.frame_flags|=sbps::kFlagError;result.diagnostics.push_back(sbps::IpcDiagnostic(std::move(code),"parser_server_ipc.access_cursor_close_refused","Access cursor close coordination was refused.",{{"detail",std::move(detail)}}));return result;};scratchbird::engine::sblr::SblrAccessCursorCloseRequestV1 value;std::string detail;if(!registry||!scratchbird::engine::sblr::DecodeSblrAccessCursorCloseRequestV1(request.payload.data(),request.payload.size(),&value,&detail))return refuse("SBLR.OPERAND_INVALID",detail);auto session=registry->sessions_by_uuid.find(UuidBytesToText(request.header.session_uuid));if(session==registry->sessions_by_uuid.end())return refuse("SECURITY.ACCESS_DENIED","session_hidden");auto receipt_uuid=UuidBytesToText(value.receipt);ServerStatementContextRecord*receipt=nullptr;{std::lock_guard<std::mutex>guard(*registry->statement_context_mutex);for(auto&[unused,row]:registry->statement_contexts_by_statement_uuid){(void)unused;if(!row.released&&row.view.receipt_uuid==receipt_uuid){receipt=&row;break;}}}if(!receipt||receipt->session_uuid!=request.header.session_uuid)return refuse("SECURITY.ACCESS_DENIED","access_cursor_close_receipt_hidden");auto context=EngineContextForSession(session->second,engine_state,request);context.statement_uuid.canonical=receipt_uuid;context.statement_metadata_snapshot_engine_owned=true;context.trace_tags.push_back("private_access_cursor_close_compiler");auto coordinated=engine_api::CompileSblrAccessCursorCloseDescriptor(context,receipt_uuid,value.occurrence,value.close_occurrence,receipt->view.access_cursor_close_executor_availability_generation);if(!coordinated.ok)return refuse(coordinated.diagnostic.code,coordinated.diagnostic.message_key);result.payload=scratchbird::engine::sblr::EncodeSblrAccessCursorCloseDescriptorV1(coordinated.descriptor,false);if(result.payload.empty())return refuse("CURSOR.CLOSE_FAILED","ACLD_encode_failed");result.accepted=true;return result;}
SessionOperationResult HandleCoordinateInsert(ServerSessionRegistry*registry,const HostedEngineState&engine_state,const sbps::Frame&request){SessionOperationResult result;result.response_message_type=95;result.response_schema_id=sbps::kSchemaCoordinateInsertResultV1;result.frame_flags=sbps::kFlagResponse|sbps::kFlagFinal;result.session_uuid=request.header.session_uuid;auto refuse=[&](std::string code,std::string detail){result.frame_flags|=sbps::kFlagError;result.diagnostics.push_back(sbps::IpcDiagnostic(std::move(code),"parser_server_ipc.insert_refused","Insert coordination was refused.",{{"detail",std::move(detail)}}));return result;};scratchbird::engine::sblr::SblrInsertRequestV1 value;std::string detail;if(!registry||!scratchbird::engine::sblr::DecodeSblrInsertRequestV1(request.payload.data(),request.payload.size(),&value,&detail))return refuse("SBLR.OPERAND_INVALID",detail);auto session=registry->sessions_by_uuid.find(UuidBytesToText(request.header.session_uuid));if(session==registry->sessions_by_uuid.end())return refuse("SECURITY.ACCESS_DENIED","session_hidden");auto receipt_uuid=UuidBytesToText(value.receipt);ServerStatementContextRecord*receipt=nullptr;{std::lock_guard<std::mutex>guard(*registry->statement_context_mutex);for(auto&[unused,row]:registry->statement_contexts_by_statement_uuid){(void)unused;if(!row.released&&row.view.receipt_uuid==receipt_uuid){receipt=&row;break;}}}if(!receipt||receipt->session_uuid!=request.header.session_uuid)return refuse("SECURITY.ACCESS_DENIED","insert_receipt_hidden");auto context=EngineContextForSession(session->second,engine_state,request);context.statement_uuid.canonical=receipt_uuid;context.statement_metadata_snapshot_engine_owned=true;context.trace_tags.push_back("private_insert_compiler");auto coordinated=engine_api::CompileSblrInsertDescriptor(context,receipt_uuid,value.occurrence,value.insert_occurrence,receipt->view.insert_executor_availability_generation);if(!coordinated.ok)return refuse(coordinated.diagnostic.code,coordinated.diagnostic.message_key);result.payload=scratchbird::engine::sblr::EncodeSblrInsertDescriptorV1(coordinated.descriptor,false);if(result.payload.empty())return refuse("DML.INSERT_FAILED","INSD_encode_failed");result.accepted=true;return result;}
SessionOperationResult HandleCoordinateUpdate(ServerSessionRegistry*registry,const HostedEngineState&engine_state,const sbps::Frame&request){SessionOperationResult result;result.response_message_type=97;result.response_schema_id=sbps::kSchemaCoordinateUpdateResultV1;result.frame_flags=sbps::kFlagResponse|sbps::kFlagFinal;result.session_uuid=request.header.session_uuid;auto refuse=[&](std::string code,std::string detail){result.frame_flags|=sbps::kFlagError;result.diagnostics.push_back(sbps::IpcDiagnostic(std::move(code),"parser_server_ipc.update_refused","Update coordination was refused.",{{"detail",std::move(detail)}}));return result;};scratchbird::engine::sblr::SblrUpdateRequestV1 value;std::string detail;if(!registry||!scratchbird::engine::sblr::DecodeSblrUpdateRequestV1(request.payload.data(),request.payload.size(),&value,&detail))return refuse("SBLR.OPERAND_INVALID",detail);auto session=registry->sessions_by_uuid.find(UuidBytesToText(request.header.session_uuid));if(session==registry->sessions_by_uuid.end())return refuse("SECURITY.ACCESS_DENIED","session_hidden");auto receipt_uuid=UuidBytesToText(value.receipt);ServerStatementContextRecord*receipt=nullptr;{std::lock_guard<std::mutex>guard(*registry->statement_context_mutex);for(auto&[unused,row]:registry->statement_contexts_by_statement_uuid){(void)unused;if(!row.released&&row.view.receipt_uuid==receipt_uuid){receipt=&row;break;}}}if(!receipt||receipt->session_uuid!=request.header.session_uuid)return refuse("SECURITY.ACCESS_DENIED","update_receipt_hidden");auto context=EngineContextForSession(session->second,engine_state,request);context.statement_uuid.canonical=receipt_uuid;context.statement_metadata_snapshot_engine_owned=true;context.trace_tags.push_back("private_update_compiler");auto coordinated=engine_api::CompileSblrUpdateDescriptor(context,receipt_uuid,value.occurrence,value.update_occurrence,receipt->view.update_executor_availability_generation);if(!coordinated.ok)return refuse(coordinated.diagnostic.code,coordinated.diagnostic.message_key);result.payload=scratchbird::engine::sblr::EncodeSblrUpdateDescriptorV1(coordinated.descriptor,false);if(result.payload.empty())return refuse("DML.UPDATE_FAILED","UPDD_encode_failed");result.accepted=true;return result;}
SessionOperationResult HandleCoordinateDelete(ServerSessionRegistry*registry,const HostedEngineState&engine_state,const sbps::Frame&request){SessionOperationResult result;result.response_message_type=99;result.response_schema_id=sbps::kSchemaCoordinateDeleteResultV1;result.frame_flags=sbps::kFlagResponse|sbps::kFlagFinal;result.session_uuid=request.header.session_uuid;auto refuse=[&](std::string code,std::string detail){result.frame_flags|=sbps::kFlagError;result.diagnostics.push_back(sbps::IpcDiagnostic(std::move(code),"parser_server_ipc.delete_refused","Delete coordination was refused.",{{"detail",std::move(detail)}}));return result;};scratchbird::engine::sblr::SblrDeleteRequestV1 value;std::string detail;if(!registry||!scratchbird::engine::sblr::DecodeSblrDeleteRequestV1(request.payload.data(),request.payload.size(),&value,&detail))return refuse("SBLR.OPERAND_INVALID",detail);auto session=registry->sessions_by_uuid.find(UuidBytesToText(request.header.session_uuid));if(session==registry->sessions_by_uuid.end())return refuse("SECURITY.ACCESS_DENIED","session_hidden");auto receipt_uuid=UuidBytesToText(value.receipt);ServerStatementContextRecord*receipt=nullptr;{std::lock_guard<std::mutex>guard(*registry->statement_context_mutex);for(auto&[unused,row]:registry->statement_contexts_by_statement_uuid){(void)unused;if(!row.released&&row.view.receipt_uuid==receipt_uuid){receipt=&row;break;}}}if(!receipt||receipt->session_uuid!=request.header.session_uuid)return refuse("SECURITY.ACCESS_DENIED","delete_receipt_hidden");auto context=EngineContextForSession(session->second,engine_state,request);context.statement_uuid.canonical=receipt_uuid;context.statement_metadata_snapshot_engine_owned=true;context.trace_tags.push_back("private_delete_compiler");auto coordinated=engine_api::CompileSblrDeleteDescriptor(context,receipt_uuid,value.occurrence,value.delete_occurrence,receipt->view.delete_executor_availability_generation);if(!coordinated.ok)return refuse(coordinated.diagnostic.code,coordinated.diagnostic.message_key);result.payload=scratchbird::engine::sblr::EncodeSblrDeleteDescriptorV1(coordinated.descriptor,false);if(result.payload.empty())return refuse("DML.DELETE_FAILED","DELD_encode_failed");result.accepted=true;return result;}
SessionOperationResult HandleCoordinateMerge(ServerSessionRegistry*registry,const HostedEngineState&engine_state,const sbps::Frame&request){SessionOperationResult result;result.response_message_type=101;result.response_schema_id=sbps::kSchemaCoordinateMergeResultV1;result.frame_flags=sbps::kFlagResponse|sbps::kFlagFinal;result.session_uuid=request.header.session_uuid;auto refuse=[&](std::string code,std::string detail){result.frame_flags|=sbps::kFlagError;result.diagnostics.push_back(sbps::IpcDiagnostic(std::move(code),"parser_server_ipc.merge_refused","Merge coordination was refused.",{{"detail",std::move(detail)}}));return result;};scratchbird::engine::sblr::SblrMergeRequestV1 value;std::string detail;if(!registry||!scratchbird::engine::sblr::DecodeSblrMergeRequestV1(request.payload.data(),request.payload.size(),&value,&detail))return refuse("SBLR.OPERAND_INVALID",detail);auto session=registry->sessions_by_uuid.find(UuidBytesToText(request.header.session_uuid));if(session==registry->sessions_by_uuid.end())return refuse("SECURITY.ACCESS_DENIED","session_hidden");auto receipt_uuid=UuidBytesToText(value.receipt);ServerStatementContextRecord*receipt=nullptr;{std::lock_guard<std::mutex>guard(*registry->statement_context_mutex);for(auto&[unused,row]:registry->statement_contexts_by_statement_uuid){(void)unused;if(!row.released&&row.view.receipt_uuid==receipt_uuid){receipt=&row;break;}}}if(!receipt||receipt->session_uuid!=request.header.session_uuid)return refuse("SECURITY.ACCESS_DENIED","merge_receipt_hidden");auto context=EngineContextForSession(session->second,engine_state,request);context.statement_uuid.canonical=receipt_uuid;context.statement_metadata_snapshot_engine_owned=true;context.trace_tags.push_back("private_merge_compiler");auto coordinated=engine_api::CompileSblrMergeDescriptor(context,receipt_uuid,value.occurrence,value.merge_occurrence,receipt->view.merge_executor_availability_generation);if(!coordinated.ok)return refuse(coordinated.diagnostic.code,coordinated.diagnostic.message_key);result.payload=scratchbird::engine::sblr::EncodeSblrMergeDescriptorV1(coordinated.descriptor,false);if(result.payload.empty())return refuse("DML.MERGE_FAILED","MERD_encode_failed");result.accepted=true;return result;}
SessionOperationResult HandleCoordinateTableTruncate(ServerSessionRegistry*registry,const HostedEngineState&engine_state,const sbps::Frame&request){SessionOperationResult result;result.response_message_type=103;result.response_schema_id=sbps::kSchemaCoordinateTableTruncateResultV1;result.frame_flags=sbps::kFlagResponse|sbps::kFlagFinal;result.session_uuid=request.header.session_uuid;auto refuse=[&](std::string c,std::string d){result.frame_flags|=sbps::kFlagError;result.diagnostics.push_back(sbps::IpcDiagnostic(std::move(c),"parser_server_ipc.table_truncate_refused","Table truncate coordination was refused.",{{"detail",std::move(d)}}));return result;};scratchbird::engine::sblr::SblrTableTruncateRequestV1 v;std::string d;if(!registry||!scratchbird::engine::sblr::DecodeSblrTableTruncateRequestV1(request.payload.data(),request.payload.size(),&v,&d))return refuse("SBLR.OPERAND_INVALID",d);auto s=registry->sessions_by_uuid.find(UuidBytesToText(request.header.session_uuid));if(s==registry->sessions_by_uuid.end())return refuse("SECURITY.ACCESS_DENIED","session_hidden");auto ru=UuidBytesToText(v.receipt);ServerStatementContextRecord*r=nullptr;{std::lock_guard<std::mutex>g(*registry->statement_context_mutex);for(auto&[x,row]:registry->statement_contexts_by_statement_uuid){(void)x;if(!row.released&&row.view.receipt_uuid==ru){r=&row;break;}}}if(!r||r->session_uuid!=request.header.session_uuid)return refuse("SECURITY.ACCESS_DENIED","table_truncate_receipt_hidden");auto c=EngineContextForSession(s->second,engine_state,request);c.statement_uuid.canonical=ru;c.statement_metadata_snapshot_engine_owned=true;c.trace_tags.push_back("private_table_truncate_compiler");auto q=engine_api::CompileSblrTableTruncateDescriptor(c,ru,v.occurrence,v.truncate_occurrence,r->view.table_truncate_executor_availability_generation);if(!q.ok)return refuse(q.diagnostic.code,q.diagnostic.message_key);result.payload=scratchbird::engine::sblr::EncodeSblrTableTruncateDescriptorV1(q.descriptor,false);if(result.payload.empty())return refuse("TABLE.TRUNCATE.ABORTED","TTRD_encode_failed");result.accepted=true;return result;}
SessionOperationResult HandleCoordinateTableAnalyze(ServerSessionRegistry*registry,const HostedEngineState&engine_state,const sbps::Frame&request){SessionOperationResult result;result.response_message_type=105;result.response_schema_id=sbps::kSchemaCoordinateTableAnalyzeResultV1;result.frame_flags=sbps::kFlagResponse|sbps::kFlagFinal;result.session_uuid=request.header.session_uuid;auto refuse=[&](std::string c,std::string d){result.frame_flags|=sbps::kFlagError;result.diagnostics.push_back(sbps::IpcDiagnostic(std::move(c),"parser_server_ipc.table_analyze_refused","Table analyze coordination was refused.",{{"detail",std::move(d)}}));return result;};scratchbird::engine::sblr::SblrTableAnalyzeRequestV1 v;std::string d;if(!registry||!scratchbird::engine::sblr::DecodeSblrTableAnalyzeRequestV1(request.payload.data(),request.payload.size(),&v,&d))return refuse("SBLR.OPERAND_INVALID",d);auto s=registry->sessions_by_uuid.find(UuidBytesToText(request.header.session_uuid));if(s==registry->sessions_by_uuid.end())return refuse("SECURITY.ACCESS_DENIED","session_hidden");auto ru=UuidBytesToText(v.receipt);ServerStatementContextRecord*r=nullptr;{std::lock_guard<std::mutex>g(*registry->statement_context_mutex);for(auto&[x,row]:registry->statement_contexts_by_statement_uuid){(void)x;if(!row.released&&row.view.receipt_uuid==ru){r=&row;break;}}}if(!r||r->session_uuid!=request.header.session_uuid)return refuse("SECURITY.ACCESS_DENIED","table_analyze_receipt_hidden");auto c=EngineContextForSession(s->second,engine_state,request);c.statement_uuid.canonical=ru;c.statement_metadata_snapshot_engine_owned=true;c.trace_tags.push_back("private_table_analyze_compiler");auto q=engine_api::CompileSblrTableAnalyzeDescriptor(c,ru,v.occurrence,v.analyze_occurrence,r->view.table_analyze_executor_availability_generation);if(!q.ok)return refuse(q.diagnostic.code,q.diagnostic.message_key);result.payload=scratchbird::engine::sblr::EncodeSblrTableAnalyzeDescriptorV1(q.descriptor,false);if(result.payload.empty())return refuse("TABLE.ANALYZE.ABORTED","TTAD_encode_failed");result.accepted=true;return result;}

SessionOperationResult HandleCoordinateBulkImportStream(ServerSessionRegistry*registry,const HostedEngineState&engine_state,const sbps::Frame&request){SessionOperationResult result;result.response_message_type=107;result.response_schema_id=sbps::kSchemaCoordinateBulkImportStreamResultV1;result.frame_flags=sbps::kFlagResponse|sbps::kFlagFinal;result.session_uuid=request.header.session_uuid;auto refuse=[&](std::string c,std::string d){result.frame_flags|=sbps::kFlagError;result.diagnostics.push_back(sbps::IpcDiagnostic(std::move(c),"parser_server_ipc.bulk_import_stream_refused","Bulk import stream coordination was refused.",{{"detail",std::move(d)}}));return result;};scratchbird::engine::sblr::SblrBulkImportStreamRequestV1 v;std::string d;if(!registry||!scratchbird::engine::sblr::DecodeSblrBulkImportStreamRequestV1(request.payload.data(),request.payload.size(),&v,&d))return refuse("SBLR.OPERAND_INVALID",d);auto s=registry->sessions_by_uuid.find(UuidBytesToText(request.header.session_uuid));if(s==registry->sessions_by_uuid.end())return refuse("SECURITY.ACCESS_DENIED","session_hidden");auto ru=UuidBytesToText(v.receipt);ServerStatementContextRecord*r=nullptr;{std::lock_guard<std::mutex>g(*registry->statement_context_mutex);for(auto&[x,row]:registry->statement_contexts_by_statement_uuid){(void)x;if(!row.released&&row.view.receipt_uuid==ru){r=&row;break;}}}if(!r||r->session_uuid!=request.header.session_uuid)return refuse("SECURITY.ACCESS_DENIED","bulk_import_stream_receipt_hidden");auto c=EngineContextForSession(s->second,engine_state,request);c.statement_uuid.canonical=ru;c.statement_metadata_snapshot_engine_owned=true;c.trace_tags.push_back("private_bulk_import_stream_compiler");auto q=engine_api::CompileSblrBulkImportStreamDescriptor(c,ru,v.occurrence,v.import_occurrence,r->view.bulk_import_stream_executor_availability_generation);if(!q.ok)return refuse(q.diagnostic.code,q.diagnostic.message_key);result.payload=scratchbird::engine::sblr::EncodeSblrBulkImportStreamDescriptorV1(q.descriptor,false);if(result.payload.empty())return refuse("BULK.IMPORT.ABORTED","BIRD_encode_failed");result.accepted=true;return result;}
SessionOperationResult HandleCoordinateBulkExportStream(ServerSessionRegistry*registry,const HostedEngineState&engine_state,const sbps::Frame&request){SessionOperationResult result;result.response_message_type=109;result.response_schema_id=sbps::kSchemaCoordinateBulkExportStreamResultV1;result.frame_flags=sbps::kFlagResponse|sbps::kFlagFinal;result.session_uuid=request.header.session_uuid;auto refuse=[&](std::string c,std::string d){result.frame_flags|=sbps::kFlagError;result.diagnostics.push_back(sbps::IpcDiagnostic(std::move(c),"parser_server_ipc.bulk_export_stream_refused","Bulk export stream coordination was refused.",{{"detail",std::move(d)}}));return result;};scratchbird::engine::sblr::SblrBulkExportStreamRequestV1 v;std::string d;if(!registry||!scratchbird::engine::sblr::DecodeSblrBulkExportStreamRequestV1(request.payload.data(),request.payload.size(),&v,&d))return refuse("SBLR.OPERAND_INVALID",d);auto s=registry->sessions_by_uuid.find(UuidBytesToText(request.header.session_uuid));if(s==registry->sessions_by_uuid.end())return refuse("SECURITY.ACCESS_DENIED","session_hidden");auto ru=UuidBytesToText(v.receipt);ServerStatementContextRecord*r=nullptr;{std::lock_guard<std::mutex>g(*registry->statement_context_mutex);for(auto&[x,row]:registry->statement_contexts_by_statement_uuid){(void)x;if(!row.released&&row.view.receipt_uuid==ru){r=&row;break;}}}if(!r||r->session_uuid!=request.header.session_uuid)return refuse("SECURITY.ACCESS_DENIED","bulk_export_stream_receipt_hidden");auto c=EngineContextForSession(s->second,engine_state,request);c.statement_uuid.canonical=ru;c.statement_metadata_snapshot_engine_owned=true;c.trace_tags.push_back("private_bulk_export_stream_compiler");auto q=engine_api::CompileSblrBulkExportStreamDescriptor(c,ru,v.occurrence,v.export_occurrence,r->view.bulk_export_stream_executor_availability_generation);if(!q.ok)return refuse(q.diagnostic.code,q.diagnostic.message_key);result.payload=scratchbird::engine::sblr::EncodeSblrBulkExportStreamDescriptorV1(q.descriptor,false);if(result.payload.empty())return refuse("BULK.EXPORT.ABORTED","BERD_encode_failed");result.accepted=true;return result;}
SessionOperationResult HandleCoordinateStatementBatch(ServerSessionRegistry*registry,const HostedEngineState&engine_state,const sbps::Frame&request){SessionOperationResult result;result.response_message_type=111;result.response_schema_id=sbps::kSchemaCoordinateStatementBatchResultV1;result.frame_flags=sbps::kFlagResponse|sbps::kFlagFinal;result.session_uuid=request.header.session_uuid;auto refuse=[&](std::string c,std::string d){result.frame_flags|=sbps::kFlagError;result.diagnostics.push_back(sbps::IpcDiagnostic(std::move(c),"parser_server_ipc.statement_batch_refused","Statement batch coordination was refused.",{{"detail",std::move(d)}}));return result;};scratchbird::engine::sblr::SblrStatementBatchRequestV1 v;std::string d;if(!registry||!scratchbird::engine::sblr::DecodeSblrStatementBatchRequestV1(request.payload.data(),request.payload.size(),&v,&d))return refuse("SBLR.OPERAND_INVALID",d);auto s=registry->sessions_by_uuid.find(UuidBytesToText(request.header.session_uuid));if(s==registry->sessions_by_uuid.end())return refuse("SECURITY.ACCESS_DENIED","session_hidden");auto ru=UuidBytesToText(v.receipt);ServerStatementContextRecord*r=nullptr;{std::lock_guard<std::mutex>g(*registry->statement_context_mutex);for(auto&[x,row]:registry->statement_contexts_by_statement_uuid){(void)x;if(!row.released&&row.view.receipt_uuid==ru){r=&row;break;}}}if(!r||r->session_uuid!=request.header.session_uuid)return refuse("SECURITY.ACCESS_DENIED","statement_batch_receipt_hidden");auto c=EngineContextForSession(s->second,engine_state,request);c.statement_uuid.canonical=ru;c.statement_metadata_snapshot_engine_owned=true;c.trace_tags.push_back("private_statement_batch_compiler");auto q=engine_api::CompileSblrStatementBatchDescriptor(c,ru,v.occurrence,1,r->view.statement_batch_executor_availability_generation);if(!q.ok)return refuse(q.diagnostic.code,q.diagnostic.message_key);result.payload=scratchbird::engine::sblr::EncodeSblrStatementBatchDescriptorV1(q.descriptor,false);if(result.payload.empty())return refuse("STATEMENT.BATCH.ABORTED","SBDD_encode_failed");result.accepted=true;return result;}
SessionOperationResult HandleCoordinateAtomicCas(ServerSessionRegistry*registry,const HostedEngineState&engine_state,const sbps::Frame&request){SessionOperationResult result;result.response_message_type=113;result.response_schema_id=sbps::kSchemaCoordinateAtomicCasResultV1;result.frame_flags=sbps::kFlagResponse|sbps::kFlagFinal;result.session_uuid=request.header.session_uuid;auto refuse=[&](std::string c,std::string d){result.frame_flags|=sbps::kFlagError;result.diagnostics.push_back(sbps::IpcDiagnostic(std::move(c),"parser_server_ipc.atomic_cas_refused","Atomic CAS coordination was refused.",{{"detail",std::move(d)}}));return result;};scratchbird::engine::sblr::SblrAtomicCasRequestV1 v;std::string d;if(!registry||!scratchbird::engine::sblr::DecodeSblrAtomicCasRequestV1(request.payload.data(),request.payload.size(),&v,&d))return refuse("SBLR.OPERAND_INVALID",d);auto s=registry->sessions_by_uuid.find(UuidBytesToText(request.header.session_uuid));if(s==registry->sessions_by_uuid.end())return refuse("SECURITY.ACCESS_DENIED","session_hidden");auto ru=UuidBytesToText(v.receipt);ServerStatementContextRecord*r=nullptr;{std::lock_guard<std::mutex>g(*registry->statement_context_mutex);for(auto&[x,row]:registry->statement_contexts_by_statement_uuid){(void)x;if(!row.released&&row.view.receipt_uuid==ru){r=&row;break;}}}if(!r||r->session_uuid!=request.header.session_uuid)return refuse("SECURITY.ACCESS_DENIED","atomic_cas_receipt_hidden");auto c=EngineContextForSession(s->second,engine_state,request);c.statement_uuid.canonical=ru;c.statement_metadata_snapshot_engine_owned=true;c.trace_tags.push_back("private_atomic_cas_compiler");auto q=engine_api::CompileSblrAtomicCasDescriptor(c,ru,v.occurrence,v.cas_occurrence,r->view.atomic_cas_executor_availability_generation);if(!q.ok)return refuse(q.diagnostic.code,q.diagnostic.message_key);result.payload=scratchbird::engine::sblr::EncodeSblrAtomicCasDescriptorV1(q.descriptor,false);if(result.payload.empty())return refuse("ATOMIC.CAS.ABORTED","CASD_encode_failed");result.accepted=true;return result;}
SessionOperationResult HandleCoordinateAtomicRmw(ServerSessionRegistry*registry,const HostedEngineState&engine_state,const sbps::Frame&request){SessionOperationResult result;result.response_message_type=115;result.response_schema_id=sbps::kSchemaCoordinateAtomicRmwResultV1;result.frame_flags=sbps::kFlagResponse|sbps::kFlagFinal;result.session_uuid=request.header.session_uuid;auto refuse=[&](std::string c,std::string d){result.frame_flags|=sbps::kFlagError;result.diagnostics.push_back(sbps::IpcDiagnostic(std::move(c),"parser_server_ipc.atomic_rmw_refused","Atomic RMW coordination was refused.",{{"detail",std::move(d)}}));return result;};scratchbird::engine::sblr::SblrAtomicRmwRequestV1 v;std::string d;if(!registry||!scratchbird::engine::sblr::DecodeSblrAtomicRmwRequestV1(request.payload.data(),request.payload.size(),&v,&d))return refuse("SBLR.OPERAND_INVALID",d);auto s=registry->sessions_by_uuid.find(UuidBytesToText(request.header.session_uuid));if(s==registry->sessions_by_uuid.end())return refuse("SECURITY.ACCESS_DENIED","session_hidden");auto ru=UuidBytesToText(v.receipt);ServerStatementContextRecord*r=nullptr;{std::lock_guard<std::mutex>g(*registry->statement_context_mutex);for(auto&[x,row]:registry->statement_contexts_by_statement_uuid){(void)x;if(!row.released&&row.view.receipt_uuid==ru){r=&row;break;}}}if(!r||r->session_uuid!=request.header.session_uuid)return refuse("SECURITY.ACCESS_DENIED","atomic_rmw_receipt_hidden");auto c=EngineContextForSession(s->second,engine_state,request);c.statement_uuid.canonical=ru;c.statement_metadata_snapshot_engine_owned=true;c.trace_tags.push_back("private_atomic_rmw_compiler");auto q=engine_api::CompileSblrAtomicRmwDescriptor(c,ru,v.occurrence,v.rmw_occurrence,r->view.atomic_rmw_executor_availability_generation);if(!q.ok)return refuse(q.diagnostic.code,q.diagnostic.message_key);result.payload=scratchbird::engine::sblr::EncodeSblrAtomicRmwDescriptorV1(q.descriptor,false);if(result.payload.empty())return refuse("ATOMIC.RMW.ABORTED","ARWD_encode_failed");result.accepted=true;return result;}
SessionOperationResult HandleCoordinateAdvisoryLock(ServerSessionRegistry*registry,const HostedEngineState&engine_state,const sbps::Frame&request){SessionOperationResult result;result.response_message_type=117;result.response_schema_id=sbps::kSchemaCoordinateAdvisoryLockResultV1;result.frame_flags=sbps::kFlagResponse|sbps::kFlagFinal;result.session_uuid=request.header.session_uuid;auto refuse=[&](std::string c,std::string d){result.frame_flags|=sbps::kFlagError;result.diagnostics.push_back(sbps::IpcDiagnostic(std::move(c),"parser_server_ipc.advisory_lock_refused","Advisory lock coordination was refused.",{{"detail",std::move(d)}}));return result;};scratchbird::engine::sblr::SblrAdvisoryLockRequestV1 v;std::string d;if(!registry||!scratchbird::engine::sblr::DecodeSblrAdvisoryLockRequestV1(request.payload.data(),request.payload.size(),&v,&d))return refuse("SBLR.OPERAND_INVALID",d);auto s=registry->sessions_by_uuid.find(UuidBytesToText(request.header.session_uuid));if(s==registry->sessions_by_uuid.end())return refuse("SECURITY.ACCESS_DENIED","session_hidden");auto ru=UuidBytesToText(v.receipt);ServerStatementContextRecord*r=nullptr;{std::lock_guard<std::mutex>g(*registry->statement_context_mutex);for(auto&[x,row]:registry->statement_contexts_by_statement_uuid){(void)x;if(!row.released&&row.view.receipt_uuid==ru){r=&row;break;}}}if(!r||r->session_uuid!=request.header.session_uuid)return refuse("SECURITY.ACCESS_DENIED","advisory_lock_receipt_hidden");auto c=EngineContextForSession(s->second,engine_state,request);c.statement_uuid.canonical=ru;c.statement_metadata_snapshot_engine_owned=true;c.trace_tags.push_back("private_advisory_lock_compiler");auto q=engine_api::CompileSblrAdvisoryLockDescriptor(c,ru,v.occurrence,v.lock_occurrence,r->view.advisory_lock_acquire_executor_availability_generation);if(!q.ok)return refuse(q.diagnostic.code,q.diagnostic.message_key);result.payload=scratchbird::engine::sblr::EncodeSblrAdvisoryLockDescriptorV1(q.descriptor,false);if(result.payload.empty())return refuse("ADVISORY.LOCK.ABORTED","ALAD_encode_failed");result.accepted=true;return result;}
SessionOperationResult HandleCoordinateAdvisoryLockRelease(ServerSessionRegistry*registry,const HostedEngineState&engine_state,const sbps::Frame&request){SessionOperationResult result;result.response_message_type=119;result.response_schema_id=sbps::kSchemaCoordinateAdvisoryLockReleaseResultV1;result.frame_flags=sbps::kFlagResponse|sbps::kFlagFinal;result.session_uuid=request.header.session_uuid;auto refuse=[&](std::string c,std::string d){result.frame_flags|=sbps::kFlagError;result.diagnostics.push_back(sbps::IpcDiagnostic(std::move(c),"parser_server_ipc.advisory_lock_release_refused","Advisory lock release coordination was refused.",{{"detail",std::move(d)}}));return result;};scratchbird::engine::sblr::SblrAdvisoryLockReleaseRequestV1 v;std::string d;if(!registry||!scratchbird::engine::sblr::DecodeSblrAdvisoryLockReleaseRequestV1(request.payload.data(),request.payload.size(),&v,&d))return refuse("SBLR.OPERAND_INVALID",d);auto s=registry->sessions_by_uuid.find(UuidBytesToText(request.header.session_uuid));if(s==registry->sessions_by_uuid.end())return refuse("SECURITY.ACCESS_DENIED","session_hidden");auto ru=UuidBytesToText(v.receipt);ServerStatementContextRecord*r=nullptr;{std::lock_guard<std::mutex>g(*registry->statement_context_mutex);for(auto&[x,row]:registry->statement_contexts_by_statement_uuid){(void)x;if(!row.released&&row.view.receipt_uuid==ru){r=&row;break;}}}if(!r||r->session_uuid!=request.header.session_uuid)return refuse("SECURITY.ACCESS_DENIED","advisory_lock_release_receipt_hidden");auto c=EngineContextForSession(s->second,engine_state,request);c.statement_uuid.canonical=ru;c.statement_metadata_snapshot_engine_owned=true;c.trace_tags.push_back("private_advisory_lock_release_compiler");auto q=engine_api::CompileSblrAdvisoryLockReleaseDescriptor(c,ru,v.occurrence,v.release_occurrence,r->view.advisory_lock_release_executor_availability_generation);if(!q.ok)return refuse(q.diagnostic.code,q.diagnostic.message_key);result.payload=scratchbird::engine::sblr::EncodeSblrAdvisoryLockReleaseDescriptorV1(q.descriptor,false);if(result.payload.empty())return refuse("ADVISORY.LOCK.RELEASE_ABORTED","ALRD_encode_failed");result.accepted=true;return result;}
SessionOperationResult HandleCoordinateFunctionCall(ServerSessionRegistry*registry,const HostedEngineState&engine_state,const sbps::Frame&request){SessionOperationResult result;result.response_message_type=121;result.response_schema_id=sbps::kSchemaCoordinateFunctionCallResultV1;result.frame_flags=sbps::kFlagResponse|sbps::kFlagFinal;result.session_uuid=request.header.session_uuid;auto refuse=[&](std::string c,std::string d){result.frame_flags|=sbps::kFlagError;result.diagnostics.push_back(sbps::IpcDiagnostic(std::move(c),"parser_server_ipc.function_call_refused","Function call coordination was refused.",{{"detail",std::move(d)}}));return result;};scratchbird::engine::sblr::SblrFunctionCallRequestV1 v;std::string d;if(!registry||!scratchbird::engine::sblr::DecodeSblrFunctionCallRequestV1(request.payload.data(),request.payload.size(),&v,&d))return refuse("SBLR.OPERAND_INVALID",d);auto s=registry->sessions_by_uuid.find(UuidBytesToText(request.header.session_uuid));if(s==registry->sessions_by_uuid.end())return refuse("SECURITY.ACCESS_DENIED","session_hidden");auto ru=UuidBytesToText(v.receipt);ServerStatementContextRecord*r=nullptr;{std::lock_guard<std::mutex>g(*registry->statement_context_mutex);for(auto&[x,row]:registry->statement_contexts_by_statement_uuid){(void)x;if(!row.released&&row.view.receipt_uuid==ru){r=&row;break;}}}if(!r||r->session_uuid!=request.header.session_uuid)return refuse("SECURITY.ACCESS_DENIED","function_call_receipt_hidden");auto c=EngineContextForSession(s->second,engine_state,request);c.statement_uuid.canonical=ru;c.statement_metadata_snapshot_engine_owned=true;c.trace_tags.push_back("private_function_call_binder");auto q=engine_api::CompileSblrFunctionCallDescriptor(c,ru,v.occurrence,v.function_occurrence,r->view.function_call_executor_availability_generation);if(!q.ok)return refuse(q.diagnostic.code,q.diagnostic.message_key);result.payload=scratchbird::engine::sblr::EncodeSblrFunctionCallDescriptorV1(q.descriptor,false);if(result.payload.empty())return refuse("FUNCTION.EXECUTION_FAILED","FCDD_encode_failed");result.accepted=true;return result;}
SessionOperationResult HandleCoordinateOperatorCall(ServerSessionRegistry*registry,const HostedEngineState&engine_state,const sbps::Frame&request){SessionOperationResult result;result.response_message_type=123;result.response_schema_id=sbps::kSchemaCoordinateOperatorCallResultV1;result.frame_flags=sbps::kFlagResponse|sbps::kFlagFinal;result.session_uuid=request.header.session_uuid;auto refuse=[&](std::string c,std::string d){result.frame_flags|=sbps::kFlagError;result.diagnostics.push_back(sbps::IpcDiagnostic(std::move(c),"parser_server_ipc.operator_call_refused","Operator call coordination was refused.",{{"detail",std::move(d)}}));return result;};scratchbird::engine::sblr::SblrOperatorCallRequestV1 v;std::string d;if(!registry||!scratchbird::engine::sblr::DecodeSblrOperatorCallRequestV1(request.payload.data(),request.payload.size(),&v,&d))return refuse("SBLR.OPERAND_INVALID",d);auto s=registry->sessions_by_uuid.find(UuidBytesToText(request.header.session_uuid));if(s==registry->sessions_by_uuid.end())return refuse("SECURITY.ACCESS_DENIED","session_hidden");auto ru=UuidBytesToText(v.receipt);ServerStatementContextRecord*r=nullptr;{std::lock_guard<std::mutex>g(*registry->statement_context_mutex);for(auto&[x,row]:registry->statement_contexts_by_statement_uuid){(void)x;if(!row.released&&row.view.receipt_uuid==ru){r=&row;break;}}}if(!r||r->session_uuid!=request.header.session_uuid)return refuse("SECURITY.ACCESS_DENIED","operator_call_receipt_hidden");auto c=EngineContextForSession(s->second,engine_state,request);c.statement_uuid.canonical=ru;c.statement_metadata_snapshot_engine_owned=true;c.trace_tags.push_back("private_operator_call_binder");auto q=engine_api::CompileSblrOperatorCallDescriptor(c,ru,v.occurrence,v.operator_occurrence,r->view.operator_call_executor_availability_generation);if(!q.ok)return refuse(q.diagnostic.code,q.diagnostic.message_key);result.payload=scratchbird::engine::sblr::EncodeSblrOperatorCallDescriptorV1(q.descriptor,false);if(result.payload.empty())return refuse("OPERATOR.EXECUTION_FAILED","OPDD_encode_failed");result.accepted=true;return result;}
SessionOperationResult HandleCoordinateCast(ServerSessionRegistry*registry,const HostedEngineState&engine_state,const sbps::Frame&request){SessionOperationResult result;result.response_message_type=125;result.response_schema_id=sbps::kSchemaCoordinateCastResultV1;result.frame_flags=sbps::kFlagResponse|sbps::kFlagFinal;result.session_uuid=request.header.session_uuid;auto refuse=[&](std::string c,std::string d){result.frame_flags|=sbps::kFlagError;result.diagnostics.push_back(sbps::IpcDiagnostic(std::move(c),"parser_server_ipc.cast_refused","CAST coordination was refused.",{{"detail",std::move(d)}}));return result;};scratchbird::engine::sblr::SblrCastRequestV1 v;std::string d;if(!registry||!scratchbird::engine::sblr::DecodeSblrCastRequestV1(request.payload.data(),request.payload.size(),&v,&d))return refuse("SBLR.OPERAND_INVALID",d);auto s=registry->sessions_by_uuid.find(UuidBytesToText(request.header.session_uuid));if(s==registry->sessions_by_uuid.end())return refuse("SECURITY.ACCESS_DENIED","session_hidden");auto ru=UuidBytesToText(v.receipt);ServerStatementContextRecord*r=nullptr;{std::lock_guard<std::mutex>g(*registry->statement_context_mutex);for(auto&[x,row]:registry->statement_contexts_by_statement_uuid){(void)x;if(!row.released&&row.view.receipt_uuid==ru){r=&row;break;}}}if(!r||r->session_uuid!=request.header.session_uuid)return refuse("SECURITY.ACCESS_DENIED","cast_receipt_hidden");auto c=EngineContextForSession(s->second,engine_state,request);c.statement_uuid.canonical=ru;c.statement_metadata_snapshot_engine_owned=true;c.trace_tags.push_back("private_cast_binder");auto q=engine_api::CompileSblrCastDescriptor(c,ru,v.occurrence,v.cast_occurrence,r->view.cast_executor_availability_generation);if(!q.ok)return refuse(q.diagnostic.code,q.diagnostic.message_key);result.payload=scratchbird::engine::sblr::EncodeSblrCastDescriptorV1(q.descriptor,false);if(result.payload.empty())return refuse("CAST.EXECUTION_FAILED","CSDD_encode_failed");result.accepted=true;return result;}
SessionOperationResult HandleCoordinateCompare(ServerSessionRegistry*registry,const HostedEngineState&engine_state,const sbps::Frame&request){SessionOperationResult result;result.response_message_type=127;result.response_schema_id=sbps::kSchemaCoordinateCompareResultV1;result.frame_flags=sbps::kFlagResponse|sbps::kFlagFinal;result.session_uuid=request.header.session_uuid;auto refuse=[&](std::string c,std::string d){result.frame_flags|=sbps::kFlagError;result.diagnostics.push_back(sbps::IpcDiagnostic(std::move(c),"parser_server_ipc.compare_refused","COMPARE coordination was refused.",{{"detail",std::move(d)}}));return result;};scratchbird::engine::sblr::SblrCompareRequestV1 v;std::string d;if(!registry||!scratchbird::engine::sblr::DecodeSblrCompareRequestV1(request.payload.data(),request.payload.size(),&v,&d))return refuse("SBLR.OPERAND_INVALID",d);auto s=registry->sessions_by_uuid.find(UuidBytesToText(request.header.session_uuid));if(s==registry->sessions_by_uuid.end())return refuse("SECURITY.ACCESS_DENIED","session_hidden");auto ru=UuidBytesToText(v.receipt);ServerStatementContextRecord*r=nullptr;{std::lock_guard<std::mutex>g(*registry->statement_context_mutex);for(auto&[x,row]:registry->statement_contexts_by_statement_uuid){(void)x;if(!row.released&&row.view.receipt_uuid==ru){r=&row;break;}}}if(!r||r->session_uuid!=request.header.session_uuid)return refuse("SECURITY.ACCESS_DENIED","compare_receipt_hidden");auto c=EngineContextForSession(s->second,engine_state,request);c.statement_uuid.canonical=ru;c.statement_metadata_snapshot_engine_owned=true;c.trace_tags.push_back("private_compare_binder");auto q=engine_api::CompileSblrCompareDescriptor(c,ru,v.occurrence,v.comparison_occurrence,r->view.compare_executor_availability_generation);if(!q.ok)return refuse(q.diagnostic.code,q.diagnostic.message_key);result.payload=scratchbird::engine::sblr::EncodeSblrCompareDescriptorV1(q.descriptor,false);if(result.payload.empty())return refuse("COMPARE.EXECUTION_FAILED","CPDD_encode_failed");result.accepted=true;return result;}
SessionOperationResult HandleCoordinateDomainOperation(ServerSessionRegistry*registry,const HostedEngineState&engine_state,const sbps::Frame&request){SessionOperationResult result;result.response_message_type=129;result.response_schema_id=sbps::kSchemaCoordinateDomainOperationResultV1;result.frame_flags=sbps::kFlagResponse|sbps::kFlagFinal;result.session_uuid=request.header.session_uuid;auto refuse=[&](std::string c,std::string d){result.frame_flags|=sbps::kFlagError;result.diagnostics.push_back(sbps::IpcDiagnostic(std::move(c),"parser_server_ipc.domain_operation_refused","DOMAIN_OPERATION coordination was refused.",{{"detail",std::move(d)}}));return result;};scratchbird::engine::sblr::SblrDomainOperationRequestV1 v;std::string d;if(!registry||!scratchbird::engine::sblr::DecodeSblrDomainOperationRequestV1(request.payload.data(),request.payload.size(),&v,&d))return refuse("SBLR.OPERAND_INVALID",d);auto s=registry->sessions_by_uuid.find(UuidBytesToText(request.header.session_uuid));if(s==registry->sessions_by_uuid.end())return refuse("SECURITY.ACCESS_DENIED","session_hidden");auto ru=UuidBytesToText(v.receipt);ServerStatementContextRecord*r=nullptr;{std::lock_guard<std::mutex>g(*registry->statement_context_mutex);for(auto&[x,row]:registry->statement_contexts_by_statement_uuid){(void)x;if(!row.released&&row.view.receipt_uuid==ru){r=&row;break;}}}if(!r||r->session_uuid!=request.header.session_uuid)return refuse("SECURITY.ACCESS_DENIED","domain_operation_receipt_hidden");auto c=EngineContextForSession(s->second,engine_state,request);c.statement_uuid.canonical=ru;c.statement_metadata_snapshot_engine_owned=true;c.trace_tags.push_back("private_domain_operation_binder");auto q=engine_api::CompileSblrDomainOperationDescriptor(c,ru,v.occurrence,v.domain_operation_occurrence,r->view.domain_operation_executor_availability_generation);if(!q.ok)return refuse(q.diagnostic.code,q.diagnostic.message_key);result.payload=scratchbird::engine::sblr::EncodeSblrDomainOperationDescriptorV1(q.descriptor,false);if(result.payload.empty())return refuse("DOMAIN.OPERATION_FAILED","DODD_encode_failed");result.accepted=true;return result;}
SessionOperationResult HandleCoordinateUdrInvoke(ServerSessionRegistry*registry,const HostedEngineState&engine_state,const sbps::Frame&request){SessionOperationResult result;result.response_message_type=131;result.response_schema_id=sbps::kSchemaCoordinateUdrInvokeResultV1;result.frame_flags=sbps::kFlagResponse|sbps::kFlagFinal;result.session_uuid=request.header.session_uuid;auto refuse=[&](std::string c,std::string d){result.frame_flags|=sbps::kFlagError;result.diagnostics.push_back(sbps::IpcDiagnostic(std::move(c),"parser_server_ipc.udr_invoke_refused","UDR_INVOKE coordination was refused.",{{"detail",std::move(d)}}));return result;};scratchbird::engine::sblr::SblrUdrInvokeRequestV1 v;std::string d;if(!registry||!scratchbird::engine::sblr::DecodeSblrUdrInvokeRequestV1(request.payload.data(),request.payload.size(),&v,&d))return refuse("SBLR.OPERAND_INVALID",d);auto s=registry->sessions_by_uuid.find(UuidBytesToText(request.header.session_uuid));if(s==registry->sessions_by_uuid.end())return refuse("SECURITY.ACCESS_DENIED","session_hidden");auto ru=UuidBytesToText(v.receipt);ServerStatementContextRecord*r=nullptr;{std::lock_guard<std::mutex>g(*registry->statement_context_mutex);for(auto&[x,row]:registry->statement_contexts_by_statement_uuid){(void)x;if(!row.released&&row.view.receipt_uuid==ru){r=&row;break;}}}if(!r||r->session_uuid!=request.header.session_uuid)return refuse("SECURITY.ACCESS_DENIED","udr_invoke_receipt_hidden");auto c=EngineContextForSession(s->second,engine_state,request);c.statement_uuid.canonical=ru;c.statement_metadata_snapshot_engine_owned=true;c.trace_tags.push_back("private_udr_invoke_binder");auto q=engine_api::CompileSblrUdrInvokeDescriptor(c,ru,v.occurrence,v.invocation_occurrence,r->view.udr_invoke_executor_availability_generation);if(!q.ok)return refuse(q.diagnostic.code,q.diagnostic.message_key);result.payload=scratchbird::engine::sblr::EncodeSblrUdrInvokeDescriptorV1(q.descriptor,false);if(result.payload.empty())return refuse("UDR.EXECUTION_FAILED","UIDD_encode_failed");result.accepted=true;return result;}
SessionOperationResult HandleCoordinateProcedureInvoke(ServerSessionRegistry*registry,const HostedEngineState&engine_state,const sbps::Frame&request){SessionOperationResult result;result.response_message_type=133;result.response_schema_id=sbps::kSchemaCoordinateProcedureInvokeResultV1;result.frame_flags=sbps::kFlagResponse|sbps::kFlagFinal;result.session_uuid=request.header.session_uuid;auto refuse=[&](std::string c,std::string d){result.frame_flags|=sbps::kFlagError;result.diagnostics.push_back(sbps::IpcDiagnostic(std::move(c),"parser_server_ipc.procedure_invoke_refused","PROCEDURE_INVOKE coordination was refused.",{{"detail",std::move(d)}}));return result;};scratchbird::engine::sblr::SblrProcedureInvokeRequestV1 v;std::string d;if(!registry||!scratchbird::engine::sblr::DecodeSblrProcedureInvokeRequestV1(request.payload.data(),request.payload.size(),&v,&d))return refuse("SBLR.OPERAND_INVALID",d);auto s=registry->sessions_by_uuid.find(UuidBytesToText(request.header.session_uuid));if(s==registry->sessions_by_uuid.end())return refuse("SECURITY.ACCESS_DENIED","session_hidden");auto ru=UuidBytesToText(v.receipt);ServerStatementContextRecord*r=nullptr;{std::lock_guard<std::mutex>g(*registry->statement_context_mutex);for(auto&[x,row]:registry->statement_contexts_by_statement_uuid){(void)x;if(!row.released&&row.view.receipt_uuid==ru){r=&row;break;}}}if(!r||r->session_uuid!=request.header.session_uuid)return refuse("SECURITY.ACCESS_DENIED","procedure_invoke_receipt_hidden");auto c=EngineContextForSession(s->second,engine_state,request);c.statement_uuid.canonical=ru;c.statement_metadata_snapshot_engine_owned=true;c.trace_tags.push_back("private_procedure_invoke_binder");auto q=engine_api::CompileSblrProcedureInvokeDescriptor(c,ru,v.occurrence,v.invocation_occurrence,r->view.procedure_invoke_executor_availability_generation);if(!q.ok)return refuse(q.diagnostic.code,q.diagnostic.message_key);result.payload=scratchbird::engine::sblr::EncodeSblrProcedureInvokeDescriptorV1(q.descriptor,false);if(result.payload.empty())return refuse("PROCEDURE.EXECUTION_FAILED","PIDD_encode_failed");result.accepted=true;return result;}
SessionOperationResult HandleCoordinateFunctionInvoke(ServerSessionRegistry*registry,const HostedEngineState&engine_state,const sbps::Frame&request){SessionOperationResult result;result.response_message_type=135;result.response_schema_id=sbps::kSchemaCoordinateFunctionInvokeResultV1;result.frame_flags=sbps::kFlagResponse|sbps::kFlagFinal;result.session_uuid=request.header.session_uuid;auto refuse=[&](std::string c,std::string d){result.frame_flags|=sbps::kFlagError;result.diagnostics.push_back(sbps::IpcDiagnostic(std::move(c),"parser_server_ipc.function_invoke_refused","FUNCTION_INVOKE coordination was refused.",{{"detail",std::move(d)}}));return result;};scratchbird::engine::sblr::SblrFunctionInvokeRequestV1 v;std::string d;if(!registry||!scratchbird::engine::sblr::DecodeSblrFunctionInvokeRequestV1(request.payload.data(),request.payload.size(),&v,&d))return refuse("SBLR.OPERAND_INVALID",d);auto s=registry->sessions_by_uuid.find(UuidBytesToText(request.header.session_uuid));if(s==registry->sessions_by_uuid.end())return refuse("SECURITY.ACCESS_DENIED","session_hidden");auto ru=UuidBytesToText(v.receipt);ServerStatementContextRecord*r=nullptr;{std::lock_guard<std::mutex>g(*registry->statement_context_mutex);for(auto&[x,row]:registry->statement_contexts_by_statement_uuid){(void)x;if(!row.released&&row.view.receipt_uuid==ru){r=&row;break;}}}if(!r||r->session_uuid!=request.header.session_uuid)return refuse("SECURITY.ACCESS_DENIED","function_invoke_receipt_hidden");auto c=EngineContextForSession(s->second,engine_state,request);c.statement_uuid.canonical=ru;c.statement_metadata_snapshot_engine_owned=true;c.trace_tags.push_back("private_function_invoke_binder");auto q=engine_api::CompileSblrFunctionInvokeDescriptor(c,ru,v.occurrence,v.invocation_occurrence,r->view.function_invoke_executor_availability_generation);if(!q.ok)return refuse(q.diagnostic.code,q.diagnostic.message_key);result.payload=scratchbird::engine::sblr::EncodeSblrFunctionInvokeDescriptorV1(q.descriptor,false);if(result.payload.empty())return refuse("FUNCTION.EXECUTION_FAILED","FIDD_encode_failed");result.accepted=true;return result;}
SessionOperationResult HandleCoordinateAggregateInvoke(ServerSessionRegistry*registry,const HostedEngineState&engine_state,const sbps::Frame&request){SessionOperationResult result;result.response_message_type=137;result.response_schema_id=sbps::kSchemaCoordinateAggregateInvokeResultV1;result.frame_flags=sbps::kFlagResponse|sbps::kFlagFinal;result.session_uuid=request.header.session_uuid;auto refuse=[&](std::string c,std::string d){result.frame_flags|=sbps::kFlagError;result.diagnostics.push_back(sbps::IpcDiagnostic(std::move(c),"parser_server_ipc.aggregate_invoke_refused","AGGREGATE_INVOKE coordination was refused.",{{"detail",std::move(d)}}));return result;};scratchbird::engine::sblr::SblrAggregateInvokeRequestV1 v;std::string d;if(!registry||!scratchbird::engine::sblr::DecodeSblrAggregateInvokeRequestV1(request.payload.data(),request.payload.size(),&v,&d))return refuse("SBLR.OPERAND_INVALID",d);auto s=registry->sessions_by_uuid.find(UuidBytesToText(request.header.session_uuid));if(s==registry->sessions_by_uuid.end())return refuse("SECURITY.ACCESS_DENIED","session_hidden");auto ru=UuidBytesToText(v.receipt);ServerStatementContextRecord*r=nullptr;{std::lock_guard<std::mutex>g(*registry->statement_context_mutex);for(auto&[x,row]:registry->statement_contexts_by_statement_uuid){(void)x;if(!row.released&&row.view.receipt_uuid==ru){r=&row;break;}}}if(!r||r->session_uuid!=request.header.session_uuid)return refuse("SECURITY.ACCESS_DENIED","aggregate_invoke_receipt_hidden");auto c=EngineContextForSession(s->second,engine_state,request);c.statement_uuid.canonical=ru;c.statement_metadata_snapshot_engine_owned=true;c.trace_tags.push_back("private_aggregate_invoke_binder");auto q=engine_api::CompileSblrAggregateInvokeDescriptor(c,ru,v.occurrence,v.invocation_occurrence,r->view.aggregate_invoke_executor_availability_generation);if(!q.ok)return refuse(q.diagnostic.code,q.diagnostic.message_key);result.payload=scratchbird::engine::sblr::EncodeSblrAggregateInvokeDescriptorV1(q.descriptor,false);if(result.payload.empty())return refuse("AGGREGATE.EXECUTION_FAILED","AIDD_encode_failed");result.accepted=true;return result;}
SessionOperationResult HandleCoordinateSequenceNextval(ServerSessionRegistry*registry,const HostedEngineState&engine_state,const sbps::Frame&request){SessionOperationResult result;result.response_message_type=139;result.response_schema_id=sbps::kSchemaCoordinateSequenceNextvalResultV1;result.frame_flags=sbps::kFlagResponse|sbps::kFlagFinal;result.session_uuid=request.header.session_uuid;auto refuse=[&](std::string c,std::string d){result.frame_flags|=sbps::kFlagError;result.diagnostics.push_back(sbps::IpcDiagnostic(std::move(c),"parser_server_ipc.sequence_nextval_refused","SEQUENCE_NEXTVAL coordination was refused.",{{"detail",std::move(d)}}));return result;};scratchbird::engine::sblr::SblrSequenceNextvalRequestV1 v;std::string d;if(!registry||!scratchbird::engine::sblr::DecodeSblrSequenceNextvalRequestV1(request.payload.data(),request.payload.size(),&v,&d))return refuse("SBLR.OPERAND_INVALID",d);auto s=registry->sessions_by_uuid.find(UuidBytesToText(request.header.session_uuid));if(s==registry->sessions_by_uuid.end())return refuse("SECURITY.ACCESS_DENIED","session_hidden");auto ru=UuidBytesToText(v.receipt);ServerStatementContextRecord*r=nullptr;{std::lock_guard<std::mutex>g(*registry->statement_context_mutex);for(auto&[x,row]:registry->statement_contexts_by_statement_uuid){(void)x;if(!row.released&&row.view.receipt_uuid==ru){r=&row;break;}}}if(!r||r->session_uuid!=request.header.session_uuid)return refuse("SECURITY.ACCESS_DENIED","sequence_nextval_receipt_hidden");auto c=EngineContextForSession(s->second,engine_state,request);c.statement_uuid.canonical=ru;c.statement_metadata_snapshot_engine_owned=true;c.trace_tags.push_back("private_sequence_nextval_binder");auto q=engine_api::CompileSblrSequenceNextvalDescriptor(c,ru,v.occurrence,v.nextval_occurrence,r->view.sequence_nextval_executor_availability_generation);if(!q.ok)return refuse(q.diagnostic.code,q.diagnostic.message_key);result.payload=scratchbird::engine::sblr::EncodeSblrSequenceNextvalDescriptorV1(q.descriptor,false);if(result.payload.empty())return refuse("SEQUENCE.ALLOCATION_FAILED","NVDD_encode_failed");result.accepted=true;return result;}
SessionOperationResult HandleCoordinateSequenceCurrval(ServerSessionRegistry*registry,const HostedEngineState&engine_state,const sbps::Frame&request){SessionOperationResult result;result.response_message_type=141;result.response_schema_id=sbps::kSchemaCoordinateSequenceCurrvalResultV1;result.frame_flags=sbps::kFlagResponse|sbps::kFlagFinal;result.session_uuid=request.header.session_uuid;auto refuse=[&](std::string c,std::string d){result.frame_flags|=sbps::kFlagError;result.diagnostics.push_back(sbps::IpcDiagnostic(std::move(c),"parser_server_ipc.sequence_currval_refused","SEQUENCE_CURRVAL coordination was refused.",{{"detail",std::move(d)}}));return result;};scratchbird::engine::sblr::SblrSequenceCurrvalRequestV1 v;std::string d;if(!registry||!scratchbird::engine::sblr::DecodeSblrSequenceCurrvalRequestV1(request.payload.data(),request.payload.size(),&v,&d))return refuse("SBLR.OPERAND_INVALID",d);auto s=registry->sessions_by_uuid.find(UuidBytesToText(request.header.session_uuid));if(s==registry->sessions_by_uuid.end())return refuse("SECURITY.ACCESS_DENIED","session_hidden");auto ru=UuidBytesToText(v.receipt);ServerStatementContextRecord*r=nullptr;{std::lock_guard<std::mutex>g(*registry->statement_context_mutex);for(auto&[x,row]:registry->statement_contexts_by_statement_uuid){(void)x;if(!row.released&&row.view.receipt_uuid==ru){r=&row;break;}}}if(!r||r->session_uuid!=request.header.session_uuid)return refuse("SECURITY.ACCESS_DENIED","sequence_currval_receipt_hidden");auto c=EngineContextForSession(s->second,engine_state,request);c.statement_uuid.canonical=ru;c.statement_metadata_snapshot_engine_owned=true;c.trace_tags.push_back("private_sequence_currval_binder");auto q=engine_api::CompileSblrSequenceCurrvalDescriptor(c,ru,v.occurrence,v.currval_occurrence,r->view.sequence_currval_executor_availability_generation);if(!q.ok)return refuse(q.diagnostic.code,q.diagnostic.message_key);result.payload=scratchbird::engine::sblr::EncodeSblrSequenceCurrvalDescriptorV1(q.descriptor,false);if(result.payload.empty())return refuse("SEQUENCE.OBSERVATION_FAILED","CVDD_encode_failed");result.accepted=true;return result;}
SessionOperationResult HandleCoordinateSequenceSetval(ServerSessionRegistry*registry,const HostedEngineState&engine_state,const sbps::Frame&request){SessionOperationResult result;result.response_message_type=143;result.response_schema_id=sbps::kSchemaCoordinateSequenceSetvalResultV1;result.frame_flags=sbps::kFlagResponse|sbps::kFlagFinal;result.session_uuid=request.header.session_uuid;auto refuse=[&](std::string c,std::string d){result.frame_flags|=sbps::kFlagError;result.diagnostics.push_back(sbps::IpcDiagnostic(std::move(c),"parser_server_ipc.sequence_setval_refused","SEQUENCE_SETVAL coordination was refused.",{{"detail",std::move(d)}}));return result;};scratchbird::engine::sblr::SblrSequenceSetvalRequestV1 v;std::string d;if(!registry||!scratchbird::engine::sblr::DecodeSblrSequenceSetvalRequestV1(request.payload.data(),request.payload.size(),&v,&d))return refuse("SBLR.OPERAND_INVALID",d);auto s=registry->sessions_by_uuid.find(UuidBytesToText(request.header.session_uuid));if(s==registry->sessions_by_uuid.end())return refuse("SECURITY.ACCESS_DENIED","session_hidden");auto ru=UuidBytesToText(v.receipt);ServerStatementContextRecord*r=nullptr;{std::lock_guard<std::mutex>g(*registry->statement_context_mutex);for(auto&[x,row]:registry->statement_contexts_by_statement_uuid){(void)x;if(!row.released&&row.view.receipt_uuid==ru){r=&row;break;}}}if(!r||r->session_uuid!=request.header.session_uuid)return refuse("SECURITY.ACCESS_DENIED","sequence_setval_receipt_hidden");auto c=EngineContextForSession(s->second,engine_state,request);c.statement_uuid.canonical=ru;c.statement_metadata_snapshot_engine_owned=true;c.trace_tags.push_back("private_sequence_setval_binder");auto q=engine_api::CompileSblrSequenceSetvalDescriptor(c,ru,v.occurrence,v.setval_occurrence,r->view.sequence_setval_executor_availability_generation);if(!q.ok)return refuse(q.diagnostic.code,q.diagnostic.message_key);result.payload=scratchbird::engine::sblr::EncodeSblrSequenceSetvalDescriptorV1(q.descriptor,false);if(result.payload.empty())return refuse("SEQUENCE.ASSIGNMENT_FAILED","SVDD_encode_failed");result.accepted=true;return result;}
SessionOperationResult HandleCoordinateQueryNumeric(ServerSessionRegistry*registry,const HostedEngineState&engine_state,const sbps::Frame&request){SessionOperationResult result;result.response_message_type=145;result.response_schema_id=sbps::kSchemaCoordinateQueryNumericResultV1;result.frame_flags=sbps::kFlagResponse|sbps::kFlagFinal;result.session_uuid=request.header.session_uuid;auto refuse=[&](std::string c,std::string d){result.frame_flags|=sbps::kFlagError;result.diagnostics.push_back(sbps::IpcDiagnostic(std::move(c),"parser_server_ipc.query_numeric_refused","QUERY_APPLY_NUMERIC_OPERATION coordination was refused.",{{"detail",std::move(d)}}));return result;};scratchbird::engine::sblr::SblrQueryNumericRequestV1 v;std::string d;if(!registry||!scratchbird::engine::sblr::DecodeSblrQueryNumericRequestV1(request.payload.data(),request.payload.size(),&v,&d))return refuse("SBLR.OPERAND_INVALID",d);auto s=registry->sessions_by_uuid.find(UuidBytesToText(request.header.session_uuid));if(s==registry->sessions_by_uuid.end())return refuse("SECURITY.ACCESS_DENIED","session_hidden");auto ru=UuidBytesToText(v.receipt);ServerStatementContextRecord*r=nullptr;{std::lock_guard<std::mutex>g(*registry->statement_context_mutex);for(auto&[x,row]:registry->statement_contexts_by_statement_uuid){(void)x;if(!row.released&&row.view.receipt_uuid==ru){r=&row;break;}}}if(!r||r->session_uuid!=request.header.session_uuid)return refuse("SECURITY.ACCESS_DENIED","query_numeric_receipt_hidden");auto c=EngineContextForSession(s->second,engine_state,request);c.statement_uuid.canonical=ru;c.statement_metadata_snapshot_engine_owned=true;c.trace_tags.push_back("private_query_numeric_binder");auto q=engine_api::CompileSblrQueryNumericDescriptor(c,ru,v.occurrence,v.numeric_occurrence,r->view.query_numeric_executor_availability_generation);if(!q.ok)return refuse(q.diagnostic.code,q.diagnostic.message_key);result.payload=scratchbird::engine::sblr::EncodeSblrQueryNumericDescriptorV1(q.descriptor,false);if(result.payload.empty())return refuse("NUMERIC.EVALUATION_FAILED","QNDD_encode_failed");result.accepted=true;return result;}
SessionOperationResult HandleCoordinateAdvancedDatatypeFamily(ServerSessionRegistry*registry,const HostedEngineState&engine_state,const sbps::Frame&request){SessionOperationResult result;result.response_message_type=147;result.response_schema_id=sbps::kSchemaCoordinateAdvancedDatatypeFamilyResultV1;result.frame_flags=sbps::kFlagResponse|sbps::kFlagFinal;result.session_uuid=request.header.session_uuid;auto refuse=[&](std::string c,std::string d){result.frame_flags|=sbps::kFlagError;result.diagnostics.push_back(sbps::IpcDiagnostic(std::move(c),"parser_server_ipc.advanced_datatype_family_refused","QUERY_EVALUATE_ADVANCED_DATATYPE_FAMILY coordination was refused.",{{"detail",std::move(d)}}));return result;};scratchbird::engine::sblr::SblrAdvancedDatatypeFamilyRequestV1 v;std::string d;if(!registry||!scratchbird::engine::sblr::DecodeSblrAdvancedDatatypeFamilyRequestV1(request.payload.data(),request.payload.size(),&v,&d))return refuse("SBLR.OPERAND_INVALID",d);auto s=registry->sessions_by_uuid.find(UuidBytesToText(request.header.session_uuid));if(s==registry->sessions_by_uuid.end())return refuse("SECURITY.ACCESS_DENIED","session_hidden");auto ru=UuidBytesToText(v.receipt);ServerStatementContextRecord*r=nullptr;{std::lock_guard<std::mutex>g(*registry->statement_context_mutex);for(auto&[x,row]:registry->statement_contexts_by_statement_uuid){(void)x;if(!row.released&&row.view.receipt_uuid==ru){r=&row;break;}}}if(!r||r->session_uuid!=request.header.session_uuid)return refuse("SECURITY.ACCESS_DENIED","advanced_datatype_family_receipt_hidden");auto c=EngineContextForSession(s->second,engine_state,request);c.statement_uuid.canonical=ru;c.statement_metadata_snapshot_engine_owned=true;c.trace_tags.push_back("private_advanced_datatype_family_binder");auto q=engine_api::CompileSblrAdvancedDatatypeFamilyDescriptor(c,ru,v.occurrence,v.family_occurrence,r->view.advanced_datatype_family_executor_availability_generation);if(!q.ok)return refuse(q.diagnostic.code,q.diagnostic.message_key);result.payload=scratchbird::engine::sblr::EncodeSblrAdvancedDatatypeFamilyDescriptorV1(q.descriptor,false);if(result.payload.empty())return refuse("DATATYPE.FAMILY_EVALUATION_FAILED","DFDD_encode_failed");result.accepted=true;return result;}
SessionOperationResult HandleCoordinateProject(ServerSessionRegistry*registry,const HostedEngineState&engine_state,const sbps::Frame&request){SessionOperationResult result;result.response_message_type=149;result.response_schema_id=sbps::kSchemaCoordinateProjectResultV1;result.frame_flags=sbps::kFlagResponse|sbps::kFlagFinal;result.session_uuid=request.header.session_uuid;auto refuse=[&](std::string c,std::string d){result.frame_flags|=sbps::kFlagError;result.diagnostics.push_back(sbps::IpcDiagnostic(std::move(c),"parser_server_ipc.project_refused","PROJECT coordination was refused.",{{"detail",std::move(d)}}));return result;};scratchbird::engine::sblr::SblrProjectRequestV1 v;std::string d;if(!registry||!scratchbird::engine::sblr::DecodeSblrProjectRequestV1(request.payload.data(),request.payload.size(),&v,&d))return refuse("SBLR.OPERAND_INVALID",d);auto s=registry->sessions_by_uuid.find(UuidBytesToText(request.header.session_uuid));if(s==registry->sessions_by_uuid.end())return refuse("SECURITY.ACCESS_DENIED","session_hidden");auto ru=UuidBytesToText(v.receipt);ServerStatementContextRecord*r=nullptr;{std::lock_guard<std::mutex>g(*registry->statement_context_mutex);for(auto&[x,row]:registry->statement_contexts_by_statement_uuid){(void)x;if(!row.released&&row.view.receipt_uuid==ru){r=&row;break;}}}if(!r||r->session_uuid!=request.header.session_uuid)return refuse("SECURITY.ACCESS_DENIED","project_receipt_hidden");auto c=EngineContextForSession(s->second,engine_state,request);c.statement_uuid.canonical=ru;c.statement_metadata_snapshot_engine_owned=true;c.trace_tags.push_back("private_project_binder");auto q=engine_api::CompileSblrProjectDescriptor(c,ru,v.occurrence,v.projection_occurrence,r->view.project_executor_availability_generation);if(!q.ok)return refuse(q.diagnostic.code,q.diagnostic.message_key);result.payload=scratchbird::engine::sblr::EncodeSblrProjectDescriptorV1(q.descriptor,false);if(result.payload.empty())return refuse("PROJECTION.EXECUTION_FAILED","PJDD_encode_failed");result.accepted=true;return result;}
SessionOperationResult HandleCoordinateCatalogIntrospect(ServerSessionRegistry*registry,const HostedEngineState&engine_state,const sbps::Frame&request){SessionOperationResult result;result.response_message_type=269;result.response_schema_id=sbps::kSchemaCoordinateCatalogIntrospectResultV1;result.frame_flags=sbps::kFlagResponse|sbps::kFlagFinal;result.session_uuid=request.header.session_uuid;auto refuse=[&](std::string c,std::string d){result.frame_flags|=sbps::kFlagError;result.diagnostics.push_back(sbps::IpcDiagnostic(std::move(c),"parser_server_ipc.catalog_introspect_refused","catalog introspection coordination was refused.",{{"detail",std::move(d)}}));return result;};scratchbird::engine::sblr::SblrCatalogIntrospectRequestV1 v;std::string d;if(!registry||!scratchbird::engine::sblr::DecodeSblrCatalogIntrospectRequestV1(request.payload.data(),request.payload.size(),&v,&d))return refuse("SBLR.OPERAND_INVALID",d);auto s=registry->sessions_by_uuid.find(UuidBytesToText(request.header.session_uuid));if(s==registry->sessions_by_uuid.end())return refuse("SECURITY.ACCESS_DENIED","session_hidden");auto ru=UuidBytesToText(v.receipt);ServerStatementContextRecord*r=nullptr;{std::lock_guard<std::mutex>g(*registry->statement_context_mutex);for(auto&[x,row]:registry->statement_contexts_by_statement_uuid){(void)x;if(!row.released&&row.view.receipt_uuid==ru){r=&row;break;}}}if(!r||r->session_uuid!=request.header.session_uuid)return refuse("SECURITY.ACCESS_DENIED","catalog_receipt_hidden");auto c=EngineContextForSession(s->second,engine_state,request);c.statement_uuid.canonical=ru;c.statement_metadata_snapshot_engine_owned=true;c.trace_tags.push_back("private_catalog_introspect_binder");auto q=engine_api::CompileSblrCatalogIntrospectDescriptor(c,ru,v.occurrence,v.object_occurrence,1);if(!q.ok)return refuse(q.diagnostic.code,q.diagnostic.message_key);result.payload=scratchbird::engine::sblr::EncodeSblrCatalogIntrospectDescriptorV1(q.descriptor,false);if(result.payload.empty())return refuse("CATALOG.INTROSPECT_FAILED","CIDD_encode_failed");result.accepted=true;return result;}
SessionOperationResult HandleCoordinateAggregate(ServerSessionRegistry*registry,const HostedEngineState&engine_state,const sbps::Frame&request){SessionOperationResult result;result.response_message_type=151;result.response_schema_id=sbps::kSchemaCoordinateAggregateResultV1;result.frame_flags=sbps::kFlagResponse|sbps::kFlagFinal;result.session_uuid=request.header.session_uuid;auto refuse=[&](std::string c,std::string d){result.frame_flags|=sbps::kFlagError;result.diagnostics.push_back(sbps::IpcDiagnostic(std::move(c),"parser_server_ipc.aggregate_refused","AGGREGATE coordination was refused.",{{"detail",std::move(d)}}));return result;};scratchbird::engine::sblr::SblrAggregateRequestV1 v;std::string d;if(!registry||!scratchbird::engine::sblr::DecodeSblrAggregateRequestV1(request.payload.data(),request.payload.size(),&v,&d))return refuse("SBLR.OPERAND_INVALID",d);auto s=registry->sessions_by_uuid.find(UuidBytesToText(request.header.session_uuid));if(s==registry->sessions_by_uuid.end())return refuse("SECURITY.ACCESS_DENIED","session_hidden");auto ru=UuidBytesToText(v.receipt);ServerStatementContextRecord*r=nullptr;{std::lock_guard<std::mutex>g(*registry->statement_context_mutex);for(auto&[x,row]:registry->statement_contexts_by_statement_uuid){(void)x;if(!row.released&&row.view.receipt_uuid==ru){r=&row;break;}}}if(!r||r->session_uuid!=request.header.session_uuid)return refuse("SECURITY.ACCESS_DENIED","aggregate_receipt_hidden");auto c=EngineContextForSession(s->second,engine_state,request);c.statement_uuid.canonical=ru;c.statement_metadata_snapshot_engine_owned=true;c.trace_tags.push_back("private_aggregate_binder");auto q=engine_api::CompileSblrAggregateDescriptor(c,ru,v.occurrence,v.aggregate_occurrence,r->view.aggregate_executor_availability_generation);if(!q.ok)return refuse(q.diagnostic.code,q.diagnostic.message_key);result.payload=scratchbird::engine::sblr::EncodeSblrAggregateDescriptorV1(q.descriptor,false);if(result.payload.empty())return refuse("AGGREGATE.EXECUTION_FAILED","AGDD_encode_failed");result.accepted=true;return result;}
SessionOperationResult HandleCoordinateGroup(ServerSessionRegistry*registry,const HostedEngineState&engine_state,const sbps::Frame&request){SessionOperationResult result;result.response_message_type=153;result.response_schema_id=sbps::kSchemaCoordinateGroupResultV1;result.frame_flags=sbps::kFlagResponse|sbps::kFlagFinal;result.session_uuid=request.header.session_uuid;auto refuse=[&](std::string c,std::string d){result.frame_flags|=sbps::kFlagError;result.diagnostics.push_back(sbps::IpcDiagnostic(std::move(c),"parser_server_ipc.group_refused","GROUP coordination was refused.",{{"detail",std::move(d)}}));return result;};scratchbird::engine::sblr::SblrGroupRequestV1 v;std::string d;if(!registry||!scratchbird::engine::sblr::DecodeSblrGroupRequestV1(request.payload.data(),request.payload.size(),&v,&d))return refuse("SBLR.OPERAND_INVALID",d);auto s=registry->sessions_by_uuid.find(UuidBytesToText(request.header.session_uuid));if(s==registry->sessions_by_uuid.end())return refuse("SECURITY.ACCESS_DENIED","session_hidden");auto ru=UuidBytesToText(v.receipt);ServerStatementContextRecord*r=nullptr;{std::lock_guard<std::mutex>g(*registry->statement_context_mutex);for(auto&[x,row]:registry->statement_contexts_by_statement_uuid){(void)x;if(!row.released&&row.view.receipt_uuid==ru){r=&row;break;}}}if(!r||r->session_uuid!=request.header.session_uuid)return refuse("SECURITY.ACCESS_DENIED","group_receipt_hidden");auto c=EngineContextForSession(s->second,engine_state,request);c.statement_uuid.canonical=ru;c.statement_metadata_snapshot_engine_owned=true;c.trace_tags.push_back("private_group_binder");auto q=engine_api::CompileSblrGroupDescriptor(c,ru,v.occurrence,v.group_occurrence,r->view.group_executor_availability_generation);if(!q.ok)return refuse(q.diagnostic.code,q.diagnostic.message_key);result.payload=scratchbird::engine::sblr::EncodeSblrGroupDescriptorV1(q.descriptor,false);if(result.payload.empty())return refuse("GROUP.EXECUTION_FAILED","GPDD_encode_failed");result.accepted=true;return result;}
SessionOperationResult HandleCoordinateSort(ServerSessionRegistry*registry,const HostedEngineState&engine_state,const sbps::Frame&request){SessionOperationResult result;result.response_message_type=155;result.response_schema_id=sbps::kSchemaCoordinateSortResultV1;result.frame_flags=sbps::kFlagResponse|sbps::kFlagFinal;result.session_uuid=request.header.session_uuid;auto refuse=[&](std::string c,std::string d){result.frame_flags|=sbps::kFlagError;result.diagnostics.push_back(sbps::IpcDiagnostic(std::move(c),"parser_server_ipc.sort_refused","SORT coordination was refused.",{{"detail",std::move(d)}}));return result;};scratchbird::engine::sblr::SblrSortRequestV1 v;std::string d;if(!registry||!scratchbird::engine::sblr::DecodeSblrSortRequestV1(request.payload.data(),request.payload.size(),&v,&d))return refuse("SBLR.OPERAND_INVALID",d);auto s=registry->sessions_by_uuid.find(UuidBytesToText(request.header.session_uuid));if(s==registry->sessions_by_uuid.end())return refuse("SECURITY.ACCESS_DENIED","session_hidden");auto ru=UuidBytesToText(v.receipt);ServerStatementContextRecord*r=nullptr;{std::lock_guard<std::mutex>g(*registry->statement_context_mutex);for(auto&[x,row]:registry->statement_contexts_by_statement_uuid){(void)x;if(!row.released&&row.view.receipt_uuid==ru){r=&row;break;}}}if(!r||r->session_uuid!=request.header.session_uuid)return refuse("SECURITY.ACCESS_DENIED","sort_receipt_hidden");auto c=EngineContextForSession(s->second,engine_state,request);c.statement_uuid.canonical=ru;c.statement_metadata_snapshot_engine_owned=true;c.trace_tags.push_back("private_sort_binder");auto q=engine_api::CompileSblrSortDescriptor(c,ru,v.occurrence,v.sort_occurrence,r->view.sort_executor_availability_generation);if(!q.ok)return refuse(q.diagnostic.code,q.diagnostic.message_key);result.payload=scratchbird::engine::sblr::EncodeSblrSortDescriptorV1(q.descriptor,false);if(result.payload.empty())return refuse("SORT.EXECUTION_FAILED","SODD_encode_failed");result.accepted=true;return result;}

SessionOperationResult HandleCoordinateLimit(ServerSessionRegistry*registry,const HostedEngineState&engine_state,const sbps::Frame&request){return HandleCoordinateLimitInternal(registry,engine_state,request);}
SessionOperationResult HandleCoordinateWindow(ServerSessionRegistry*registry,const HostedEngineState&engine_state,const sbps::Frame&request){SessionOperationResult result;result.response_message_type=159;result.response_schema_id=sbps::kSchemaCoordinateWindowResultV1;result.frame_flags=sbps::kFlagResponse|sbps::kFlagFinal;result.session_uuid=request.header.session_uuid;auto refuse=[&](std::string c,std::string d){result.frame_flags|=sbps::kFlagError;result.diagnostics.push_back(sbps::IpcDiagnostic(std::move(c),"parser_server_ipc.window_refused","WINDOW coordination was refused.",{{"detail",std::move(d)}}));return result;};scratchbird::engine::sblr::SblrWindowRequestV1 v;std::string d;if(!registry||!scratchbird::engine::sblr::DecodeSblrWindowRequestV1(request.payload.data(),request.payload.size(),&v,&d))return refuse("SBLR.OPERAND_INVALID",d);auto s=registry->sessions_by_uuid.find(UuidBytesToText(request.header.session_uuid));if(s==registry->sessions_by_uuid.end())return refuse("SECURITY.ACCESS_DENIED","session_hidden");auto ru=UuidBytesToText(v.receipt);ServerStatementContextRecord*r=nullptr;{std::lock_guard<std::mutex>g(*registry->statement_context_mutex);for(auto&[x,row]:registry->statement_contexts_by_statement_uuid){(void)x;if(!row.released&&row.view.receipt_uuid==ru){r=&row;break;}}}if(!r||r->session_uuid!=request.header.session_uuid)return refuse("SECURITY.ACCESS_DENIED","window_receipt_hidden");auto c=EngineContextForSession(s->second,engine_state,request);c.statement_uuid.canonical=ru;c.statement_metadata_snapshot_engine_owned=true;c.trace_tags.push_back("private_window_binder");auto q=engine_api::CompileSblrWindowDescriptor(c,ru,v.occurrence,v.window_occurrence,r->view.window_executor_availability_generation);if(!q.ok)return refuse(q.diagnostic.code,q.diagnostic.message_key);result.payload=scratchbird::engine::sblr::EncodeSblrWindowDescriptorV1(q.descriptor,false);if(result.payload.empty())return refuse("WINDOW.EXECUTION_FAILED","WIDD_encode_failed");result.accepted=true;return result;}
SessionOperationResult HandleCoordinateReturnResultSet(ServerSessionRegistry*registry,const HostedEngineState&engine_state,const sbps::Frame&request){SessionOperationResult result;result.response_message_type=161;result.response_schema_id=sbps::kSchemaCoordinateReturnResultSetResultV1;result.frame_flags=sbps::kFlagResponse|sbps::kFlagFinal;result.session_uuid=request.header.session_uuid;auto refuse=[&](std::string c,std::string d){result.frame_flags|=sbps::kFlagError;result.diagnostics.push_back(sbps::IpcDiagnostic(std::move(c),"parser_server_ipc.return_result_set_refused","RETURN_RESULT_SET coordination was refused.",{{"detail",std::move(d)}}));return result;};scratchbird::engine::sblr::SblrReturnResultSetRequestV1 v;std::string d;if(!registry||!scratchbird::engine::sblr::DecodeSblrReturnResultSetRequestV1(request.payload.data(),request.payload.size(),&v,&d))return refuse("SBLR.OPERAND_INVALID",d);auto s=registry->sessions_by_uuid.find(UuidBytesToText(request.header.session_uuid));if(s==registry->sessions_by_uuid.end())return refuse("SECURITY.ACCESS_DENIED","session_hidden");auto ru=UuidBytesToText(v.receipt);ServerStatementContextRecord*r=nullptr;{std::lock_guard<std::mutex>g(*registry->statement_context_mutex);for(auto&[x,row]:registry->statement_contexts_by_statement_uuid){(void)x;if(!row.released&&row.view.receipt_uuid==ru){r=&row;break;}}}if(!r||r->session_uuid!=request.header.session_uuid)return refuse("SECURITY.ACCESS_DENIED","return_result_set_receipt_hidden");auto c=EngineContextForSession(s->second,engine_state,request);c.statement_uuid.canonical=ru;c.statement_metadata_snapshot_engine_owned=true;c.trace_tags.push_back("private_return_result_set_binder");auto q=engine_api::CompileSblrReturnResultSetDescriptor(c,ru,v.occurrence,v.return_occurrence,r->view.return_result_set_executor_availability_generation);if(!q.ok)return refuse(q.diagnostic.code,q.diagnostic.message_key);result.payload=scratchbird::engine::sblr::EncodeSblrReturnResultSetDescriptorV1(q.descriptor,false);if(result.payload.empty())return refuse("RESULT_SET.EXECUTION_FAILED","RRDD_encode_failed");result.accepted=true;return result;}
SessionOperationResult HandleCoordinateKvStructuredRead(ServerSessionRegistry*registry,const HostedEngineState&engine_state,const sbps::Frame&request){SessionOperationResult result;result.response_message_type=163;result.response_schema_id=sbps::kSchemaCoordinateKvStructuredReadResultV1;result.frame_flags=sbps::kFlagResponse|sbps::kFlagFinal;result.session_uuid=request.header.session_uuid;auto refuse=[&](std::string c,std::string d){result.frame_flags|=sbps::kFlagError;result.diagnostics.push_back(sbps::IpcDiagnostic(std::move(c),"parser_server_ipc.kv_structured_read_refused","KV structured read coordination was refused.",{{"detail",std::move(d)}}));return result;};scratchbird::engine::sblr::SblrKvStructuredReadRequestV1 v;std::string d;if(!registry||!scratchbird::engine::sblr::DecodeSblrKvStructuredReadRequestV1(request.payload.data(),request.payload.size(),&v,&d))return refuse("SBLR.OPERAND_INVALID",d);auto s=registry->sessions_by_uuid.find(UuidBytesToText(request.header.session_uuid));if(s==registry->sessions_by_uuid.end())return refuse("SECURITY.ACCESS_DENIED","session_hidden");auto ru=UuidBytesToText(v.receipt);ServerStatementContextRecord*r=nullptr;{std::lock_guard<std::mutex>g(*registry->statement_context_mutex);for(auto&[x,row]:registry->statement_contexts_by_statement_uuid){(void)x;if(!row.released&&row.view.receipt_uuid==ru){r=&row;break;}}}if(!r||r->session_uuid!=request.header.session_uuid)return refuse("SECURITY.ACCESS_DENIED","kv_structured_read_receipt_hidden");auto c=EngineContextForSession(s->second,engine_state,request);c.statement_uuid.canonical=ru;c.statement_metadata_snapshot_engine_owned=true;c.trace_tags.push_back("private_kv_structured_read_binder");auto q=engine_api::CompileSblrKvStructuredReadDescriptor(c,ru,v.occurrence,v.read_occurrence,r->view.kv_structured_read_executor_availability_generation);if(!q.ok)return refuse(q.diagnostic.code,q.diagnostic.message_key);result.payload=scratchbird::engine::sblr::EncodeSblrKvStructuredReadDescriptorV1(q.descriptor,false);if(result.payload.empty())return refuse("KV.EXECUTION_FAILED","KVDD_encode_failed");result.accepted=true;return result;}

SessionOperationResult HandleCoordinateKvStructuredMutate(ServerSessionRegistry*registry,const HostedEngineState&engine_state,const sbps::Frame&request){SessionOperationResult result;result.response_message_type=165;result.response_schema_id=sbps::kSchemaCoordinateKvStructuredMutateResultV1;result.frame_flags=sbps::kFlagResponse|sbps::kFlagFinal;result.session_uuid=request.header.session_uuid;auto refuse=[&](std::string c,std::string d){result.frame_flags|=sbps::kFlagError;result.diagnostics.push_back(sbps::IpcDiagnostic(std::move(c),"parser_server_ipc.kv_structured_mutate_refused","KV structured read coordination was refused.",{{"detail",std::move(d)}}));return result;};scratchbird::engine::sblr::SblrKvStructuredMutateRequestV1 v;std::string d;if(!registry||!scratchbird::engine::sblr::DecodeSblrKvStructuredMutateRequestV1(request.payload.data(),request.payload.size(),&v,&d))return refuse("SBLR.OPERAND_INVALID",d);auto s=registry->sessions_by_uuid.find(UuidBytesToText(request.header.session_uuid));if(s==registry->sessions_by_uuid.end())return refuse("SECURITY.ACCESS_DENIED","session_hidden");auto ru=UuidBytesToText(v.receipt);ServerStatementContextRecord*r=nullptr;{std::lock_guard<std::mutex>g(*registry->statement_context_mutex);for(auto&[x,row]:registry->statement_contexts_by_statement_uuid){(void)x;if(!row.released&&row.view.receipt_uuid==ru){r=&row;break;}}}if(!r||r->session_uuid!=request.header.session_uuid)return refuse("SECURITY.ACCESS_DENIED","kv_structured_mutate_receipt_hidden");auto c=EngineContextForSession(s->second,engine_state,request);c.statement_uuid.canonical=ru;c.statement_metadata_snapshot_engine_owned=true;c.trace_tags.push_back("private_kv_structured_mutate_binder");auto q=engine_api::CompileSblrKvStructuredMutateDescriptor(c,ru,v.occurrence,v.mutate_occurrence,r->view.kv_structured_mutate_executor_availability_generation);if(!q.ok)return refuse(q.diagnostic.code,q.diagnostic.message_key);result.payload=scratchbird::engine::sblr::EncodeSblrKvStructuredMutateDescriptorV1(q.descriptor,false);if(result.payload.empty())return refuse("KV.MUTATION_FAILED","KMDD_encode_failed");result.accepted=true;return result;}
SessionOperationResult HandleCoordinateKvStructuredScan(ServerSessionRegistry*registry,const HostedEngineState&engine_state,const sbps::Frame&request){SessionOperationResult result;result.response_message_type=167;result.response_schema_id=sbps::kSchemaCoordinateKvStructuredScanResultV1;result.frame_flags=sbps::kFlagResponse|sbps::kFlagFinal;result.session_uuid=request.header.session_uuid;auto refuse=[&](std::string c,std::string d){result.frame_flags|=sbps::kFlagError;result.diagnostics.push_back(sbps::IpcDiagnostic(std::move(c),"parser_server_ipc.kv_structured_scan_refused","KV structured scan coordination was refused.",{{"detail",std::move(d)}}));return result;};scratchbird::engine::sblr::SblrKvStructuredScanRequestV1 v;std::string d;if(!registry||!scratchbird::engine::sblr::DecodeSblrKvStructuredScanRequestV1(request.payload.data(),request.payload.size(),&v,&d))return refuse("SBLR.OPERAND_INVALID",d);auto s=registry->sessions_by_uuid.find(UuidBytesToText(request.header.session_uuid));if(s==registry->sessions_by_uuid.end())return refuse("SECURITY.ACCESS_DENIED","session_hidden");auto ru=UuidBytesToText(v.receipt);ServerStatementContextRecord*r=nullptr;{std::lock_guard<std::mutex>g(*registry->statement_context_mutex);for(auto&[x,row]:registry->statement_contexts_by_statement_uuid){(void)x;if(!row.released&&row.view.receipt_uuid==ru){r=&row;break;}}}if(!r||r->session_uuid!=request.header.session_uuid)return refuse("SECURITY.ACCESS_DENIED","kv_structured_scan_receipt_hidden");auto c=EngineContextForSession(s->second,engine_state,request);c.statement_uuid.canonical=ru;c.statement_metadata_snapshot_engine_owned=true;c.trace_tags.push_back("private_kv_structured_scan_binder");auto q=engine_api::CompileSblrKvStructuredScanDescriptor(c,ru,v.occurrence,v.scan_occurrence,r->view.kv_structured_scan_executor_availability_generation ? r->view.kv_structured_scan_executor_availability_generation : (r->view.kv_structured_mutate_executor_availability_generation ? r->view.kv_structured_mutate_executor_availability_generation : r->view.kv_structured_read_executor_availability_generation));if(!q.ok)return refuse(q.diagnostic.code,q.diagnostic.message_key);result.payload=scratchbird::engine::sblr::EncodeSblrKvStructuredScanDescriptorV1(q.descriptor,false);if(result.payload.empty())return refuse("KV.SCAN_FAILED","KSDD_encode_failed");result.accepted=true;return result;}
SessionOperationResult HandleCoordinateKvStructuredStreamRead(ServerSessionRegistry*registry,const HostedEngineState&engine_state,const sbps::Frame&request){SessionOperationResult result;result.response_message_type=169;result.response_schema_id=sbps::kSchemaCoordinateKvStructuredStreamReadResultV1;result.frame_flags=sbps::kFlagResponse|sbps::kFlagFinal;result.session_uuid=request.header.session_uuid;auto refuse=[&](std::string c,std::string d){result.frame_flags|=sbps::kFlagError;result.diagnostics.push_back(sbps::IpcDiagnostic(std::move(c),"parser_server_ipc.kv_structured_stream_read_refused","KV structured scan coordination was refused.",{{"detail",std::move(d)}}));return result;};scratchbird::engine::sblr::SblrKvStructuredStreamReadRequestV1 v;std::string d;if(!registry||!scratchbird::engine::sblr::DecodeSblrKvStructuredStreamReadRequestV1(request.payload.data(),request.payload.size(),&v,&d))return refuse("SBLR.OPERAND_INVALID",d);auto s=registry->sessions_by_uuid.find(UuidBytesToText(request.header.session_uuid));if(s==registry->sessions_by_uuid.end())return refuse("SECURITY.ACCESS_DENIED","session_hidden");auto ru=UuidBytesToText(v.receipt);ServerStatementContextRecord*r=nullptr;{std::lock_guard<std::mutex>g(*registry->statement_context_mutex);for(auto&[x,row]:registry->statement_contexts_by_statement_uuid){(void)x;if(!row.released&&row.view.receipt_uuid==ru){r=&row;break;}}}if(!r||r->session_uuid!=request.header.session_uuid)return refuse("SECURITY.ACCESS_DENIED","kv_structured_stream_read_receipt_hidden");auto c=EngineContextForSession(s->second,engine_state,request);c.statement_uuid.canonical=ru;c.statement_metadata_snapshot_engine_owned=true;c.trace_tags.push_back("private_kv_structured_stream_read_binder");auto q=engine_api::CompileSblrKvStructuredStreamReadDescriptor(c,ru,v.occurrence,v.stream_read_occurrence,r->view.kv_structured_stream_read_executor_availability_generation ? r->view.kv_structured_stream_read_executor_availability_generation : (r->view.kv_structured_mutate_executor_availability_generation ? r->view.kv_structured_mutate_executor_availability_generation : r->view.kv_structured_read_executor_availability_generation));if(!q.ok)return refuse(q.diagnostic.code,q.diagnostic.message_key);result.payload=scratchbird::engine::sblr::EncodeSblrKvStructuredStreamReadDescriptorV1(q.descriptor,false);if(result.payload.empty())return refuse("KV.SCAN_FAILED","KRDD_encode_failed");result.accepted=true;return result;}
SessionOperationResult HandleCoordinateKvStructuredStreamAppend(ServerSessionRegistry*registry,const HostedEngineState&engine_state,const sbps::Frame&request){SessionOperationResult result;result.response_message_type=171;result.response_schema_id=sbps::kSchemaCoordinateKvStructuredStreamAppendResultV1;result.frame_flags=sbps::kFlagResponse|sbps::kFlagFinal;result.session_uuid=request.header.session_uuid;auto refuse=[&](std::string c,std::string d){result.frame_flags|=sbps::kFlagError;result.diagnostics.push_back(sbps::IpcDiagnostic(std::move(c),"parser_server_ipc.kv_structured_stream_append_refused","KV structured scan coordination was refused.",{{"detail",std::move(d)}}));return result;};scratchbird::engine::sblr::SblrKvStructuredStreamAppendRequestV1 v;std::string d;if(!registry||!scratchbird::engine::sblr::DecodeSblrKvStructuredStreamAppendRequestV1(request.payload.data(),request.payload.size(),&v,&d))return refuse("SBLR.OPERAND_INVALID",d);auto s=registry->sessions_by_uuid.find(UuidBytesToText(request.header.session_uuid));if(s==registry->sessions_by_uuid.end())return refuse("SECURITY.ACCESS_DENIED","session_hidden");auto ru=UuidBytesToText(v.receipt);ServerStatementContextRecord*r=nullptr;{std::lock_guard<std::mutex>g(*registry->statement_context_mutex);for(auto&[x,row]:registry->statement_contexts_by_statement_uuid){(void)x;if(!row.released&&row.view.receipt_uuid==ru){r=&row;break;}}}if(!r||r->session_uuid!=request.header.session_uuid)return refuse("SECURITY.ACCESS_DENIED","kv_structured_stream_append_receipt_hidden");auto c=EngineContextForSession(s->second,engine_state,request);c.statement_uuid.canonical=ru;c.statement_metadata_snapshot_engine_owned=true;c.trace_tags.push_back("private_kv_structured_stream_append_binder");auto q=engine_api::CompileSblrKvStructuredStreamAppendDescriptor(c,ru,v.occurrence,v.stream_append_occurrence,r->view.kv_structured_stream_append_executor_availability_generation ? r->view.kv_structured_stream_append_executor_availability_generation : (r->view.kv_structured_mutate_executor_availability_generation ? r->view.kv_structured_mutate_executor_availability_generation : r->view.kv_structured_read_executor_availability_generation));if(!q.ok)return refuse(q.diagnostic.code,q.diagnostic.message_key);result.payload=scratchbird::engine::sblr::EncodeSblrKvStructuredStreamAppendDescriptorV1(q.descriptor,false);if(result.payload.empty())return refuse("KV.SCAN_FAILED","KADD_encode_failed");result.accepted=true;return result;}
SessionOperationResult HandleCoordinateKvStructuredTimeseries(ServerSessionRegistry*registry,const HostedEngineState&engine_state,const sbps::Frame&request){SessionOperationResult result;result.response_message_type=173;result.response_schema_id=sbps::kSchemaCoordinateKvStructuredTimeseriesResultV1;result.frame_flags=sbps::kFlagResponse|sbps::kFlagFinal;result.session_uuid=request.header.session_uuid;auto refuse=[&](std::string c,std::string d){result.frame_flags|=sbps::kFlagError;result.diagnostics.push_back(sbps::IpcDiagnostic(std::move(c),"parser_server_ipc.kv_structured_timeseries_refused","KV structured scan coordination was refused.",{{"detail",std::move(d)}}));return result;};scratchbird::engine::sblr::SblrKvStructuredTimeseriesRequestV1 v;std::string d;if(!registry||!scratchbird::engine::sblr::DecodeSblrKvStructuredTimeseriesRequestV1(request.payload.data(),request.payload.size(),&v,&d))return refuse("SBLR.OPERAND_INVALID",d);auto s=registry->sessions_by_uuid.find(UuidBytesToText(request.header.session_uuid));if(s==registry->sessions_by_uuid.end())return refuse("SECURITY.ACCESS_DENIED","session_hidden");auto ru=UuidBytesToText(v.receipt);ServerStatementContextRecord*r=nullptr;{std::lock_guard<std::mutex>g(*registry->statement_context_mutex);for(auto&[x,row]:registry->statement_contexts_by_statement_uuid){(void)x;if(!row.released&&row.view.receipt_uuid==ru){r=&row;break;}}}if(!r||r->session_uuid!=request.header.session_uuid)return refuse("SECURITY.ACCESS_DENIED","kv_structured_timeseries_receipt_hidden");auto c=EngineContextForSession(s->second,engine_state,request);c.statement_uuid.canonical=ru;c.statement_metadata_snapshot_engine_owned=true;c.trace_tags.push_back("private_kv_structured_timeseries_binder");auto q=engine_api::CompileSblrKvStructuredTimeseriesDescriptor(c,ru,v.occurrence,v.timeseries_occurrence,r->view.kv_structured_timeseries_executor_availability_generation ? r->view.kv_structured_timeseries_executor_availability_generation : (r->view.kv_structured_mutate_executor_availability_generation ? r->view.kv_structured_mutate_executor_availability_generation : r->view.kv_structured_read_executor_availability_generation));if(!q.ok)return refuse(q.diagnostic.code,q.diagnostic.message_key);result.payload=scratchbird::engine::sblr::EncodeSblrKvStructuredTimeseriesDescriptorV1(q.descriptor,false);if(result.payload.empty())return refuse("KV.SCAN_FAILED","KTDD_encode_failed");result.accepted=true;return result;}
SessionOperationResult HandleCoordinateSystemConfigSet(ServerSessionRegistry*registry,const HostedEngineState&engine_state,const sbps::Frame&request){SessionOperationResult result;result.response_message_type=175;result.response_schema_id=sbps::kSchemaCoordinateSystemConfigSetResultV1;result.frame_flags=sbps::kFlagResponse|sbps::kFlagFinal;result.session_uuid=request.header.session_uuid;auto refuse=[&](std::string c,std::string d){result.frame_flags|=sbps::kFlagError;result.diagnostics.push_back(sbps::IpcDiagnostic(std::move(c),"parser_server_ipc.system_config_set_refused","KV structured scan coordination was refused.",{{"detail",std::move(d)}}));return result;};scratchbird::engine::sblr::SblrSystemConfigSetRequestV1 v;std::string d;if(!registry||!scratchbird::engine::sblr::DecodeSblrSystemConfigSetRequestV1(request.payload.data(),request.payload.size(),&v,&d))return refuse("SBLR.OPERAND_INVALID",d);auto s=registry->sessions_by_uuid.find(UuidBytesToText(request.header.session_uuid));if(s==registry->sessions_by_uuid.end())return refuse("SECURITY.ACCESS_DENIED","session_hidden");auto ru=UuidBytesToText(v.receipt);ServerStatementContextRecord*r=nullptr;{std::lock_guard<std::mutex>g(*registry->statement_context_mutex);for(auto&[x,row]:registry->statement_contexts_by_statement_uuid){(void)x;if(!row.released&&row.view.receipt_uuid==ru){r=&row;break;}}}if(!r||r->session_uuid!=request.header.session_uuid)return refuse("SECURITY.ACCESS_DENIED","system_config_set_receipt_hidden");auto c=EngineContextForSession(s->second,engine_state,request);c.statement_uuid.canonical=ru;c.statement_metadata_snapshot_engine_owned=true;c.trace_tags.push_back("private_system_config_set_binder");auto q=engine_api::CompileSblrSystemConfigSetDescriptor(c,ru,v.occurrence,v.config_occurrence,r->view.system_config_set_executor_availability_generation ? r->view.system_config_set_executor_availability_generation : (r->view.kv_structured_mutate_executor_availability_generation ? r->view.kv_structured_mutate_executor_availability_generation : r->view.kv_structured_read_executor_availability_generation));if(!q.ok)return refuse(q.diagnostic.code,q.diagnostic.message_key);result.payload=scratchbird::engine::sblr::EncodeSblrSystemConfigSetDescriptorV1(q.descriptor,false);if(result.payload.empty())return refuse("KV.SCAN_FAILED","SCDX_encode_failed");result.accepted=true;return result;}
SessionOperationResult HandleCoordinateDdlCreateDomain(ServerSessionRegistry*registry,const HostedEngineState&engine_state,const sbps::Frame&request){SessionOperationResult result;result.response_message_type=177;result.response_schema_id=sbps::kSchemaCoordinateDdlCreateDomainResultV1;result.frame_flags=sbps::kFlagResponse|sbps::kFlagFinal;result.session_uuid=request.header.session_uuid;auto refuse=[&](std::string c,std::string d){result.frame_flags|=sbps::kFlagError;result.diagnostics.push_back(sbps::IpcDiagnostic(std::move(c),"parser_server_ipc.ddl_create_domain_refused","KV structured scan coordination was refused.",{{"detail",std::move(d)}}));return result;};scratchbird::engine::sblr::SblrDdlCreateDomainRequestV1 v;std::string d;if(!registry||!scratchbird::engine::sblr::DecodeSblrDdlCreateDomainRequestV1(request.payload.data(),request.payload.size(),&v,&d))return refuse("SBLR.OPERAND_INVALID",d);auto s=registry->sessions_by_uuid.find(UuidBytesToText(request.header.session_uuid));if(s==registry->sessions_by_uuid.end())return refuse("SECURITY.ACCESS_DENIED","session_hidden");auto ru=UuidBytesToText(v.receipt);ServerStatementContextRecord*r=nullptr;{std::lock_guard<std::mutex>g(*registry->statement_context_mutex);for(auto&[x,row]:registry->statement_contexts_by_statement_uuid){(void)x;if(!row.released&&row.view.receipt_uuid==ru){r=&row;break;}}}if(!r||r->session_uuid!=request.header.session_uuid)return refuse("SECURITY.ACCESS_DENIED","ddl_create_domain_receipt_hidden");auto c=EngineContextForSession(s->second,engine_state,request);c.statement_uuid.canonical=ru;c.statement_metadata_snapshot_engine_owned=true;c.trace_tags.push_back("private_ddl_create_domain_binder");auto q=engine_api::CompileSblrDdlCreateDomainDescriptor(c,ru,v.occurrence,v.domain_occurrence,r->view.ddl_create_domain_executor_availability_generation ? r->view.ddl_create_domain_executor_availability_generation : (r->view.kv_structured_mutate_executor_availability_generation ? r->view.kv_structured_mutate_executor_availability_generation : r->view.kv_structured_read_executor_availability_generation));if(!q.ok)return refuse(q.diagnostic.code,q.diagnostic.message_key);result.payload=scratchbird::engine::sblr::EncodeSblrDdlCreateDomainDescriptorV1(q.descriptor,false);if(result.payload.empty())return refuse("KV.SCAN_FAILED","CDDX_encode_failed");result.accepted=true;return result;}
SessionOperationResult HandleCoordinateDdlCreateSchema(ServerSessionRegistry*registry,const HostedEngineState&engine_state,const sbps::Frame&request){SessionOperationResult result;result.response_message_type=179;result.response_schema_id=sbps::kSchemaCoordinateDdlCreateSchemaResultV1;result.frame_flags=sbps::kFlagResponse|sbps::kFlagFinal;result.session_uuid=request.header.session_uuid;auto refuse=[&](std::string c,std::string d){result.frame_flags|=sbps::kFlagError;result.diagnostics.push_back(sbps::IpcDiagnostic(std::move(c),"parser_server_ipc.ddl_create_schema_refused","KV structured scan coordination was refused.",{{"detail",std::move(d)}}));return result;};scratchbird::engine::sblr::SblrDdlCreateSchemaRequestV1 v;std::string d;if(!registry||!scratchbird::engine::sblr::DecodeSblrDdlCreateSchemaRequestV1(request.payload.data(),request.payload.size(),&v,&d))return refuse("SBLR.OPERAND_INVALID",d);auto s=registry->sessions_by_uuid.find(UuidBytesToText(request.header.session_uuid));if(s==registry->sessions_by_uuid.end())return refuse("SECURITY.ACCESS_DENIED","session_hidden");auto ru=UuidBytesToText(v.receipt);ServerStatementContextRecord*r=nullptr;{std::lock_guard<std::mutex>g(*registry->statement_context_mutex);for(auto&[x,row]:registry->statement_contexts_by_statement_uuid){(void)x;if(!row.released&&row.view.receipt_uuid==ru){r=&row;break;}}}if(!r||r->session_uuid!=request.header.session_uuid)return refuse("SECURITY.ACCESS_DENIED","ddl_create_schema_receipt_hidden");auto c=EngineContextForSession(s->second,engine_state,request);c.statement_uuid.canonical=ru;c.statement_metadata_snapshot_engine_owned=true;c.trace_tags.push_back("private_ddl_create_schema_binder");auto q=engine_api::CompileSblrDdlCreateSchemaDescriptor(c,ru,v.occurrence,v.schema_occurrence,r->view.ddl_create_schema_executor_availability_generation ? r->view.ddl_create_schema_executor_availability_generation : (r->view.kv_structured_mutate_executor_availability_generation ? r->view.kv_structured_mutate_executor_availability_generation : r->view.kv_structured_read_executor_availability_generation));if(!q.ok)return refuse(q.diagnostic.code,q.diagnostic.message_key);result.payload=scratchbird::engine::sblr::EncodeSblrDdlCreateSchemaDescriptorV1(q.descriptor,false);if(result.payload.empty())return refuse("KV.SCAN_FAILED","CSDX_encode_failed");result.accepted=true;return result;}
SessionOperationResult HandleCoordinateDdlCreateTable(ServerSessionRegistry*registry,const HostedEngineState&engine_state,const sbps::Frame&request){SessionOperationResult result;result.response_message_type=181;result.response_schema_id=sbps::kSchemaCoordinateDdlCreateTableResultV1;result.frame_flags=sbps::kFlagResponse|sbps::kFlagFinal;result.session_uuid=request.header.session_uuid;auto refuse=[&](std::string c,std::string d){result.frame_flags|=sbps::kFlagError;result.diagnostics.push_back(sbps::IpcDiagnostic(std::move(c),"parser_server_ipc.ddl_create_table_refused","KV structured scan coordination was refused.",{{"detail",std::move(d)}}));return result;};scratchbird::engine::sblr::SblrDdlCreateTableRequestV1 v;std::string d;if(!registry||!scratchbird::engine::sblr::DecodeSblrDdlCreateTableRequestV1(request.payload.data(),request.payload.size(),&v,&d))return refuse("SBLR.OPERAND_INVALID",d);auto s=registry->sessions_by_uuid.find(UuidBytesToText(request.header.session_uuid));if(s==registry->sessions_by_uuid.end())return refuse("SECURITY.ACCESS_DENIED","session_hidden");auto ru=UuidBytesToText(v.receipt);ServerStatementContextRecord*r=nullptr;{std::lock_guard<std::mutex>g(*registry->statement_context_mutex);for(auto&[x,row]:registry->statement_contexts_by_statement_uuid){(void)x;if(!row.released&&row.view.receipt_uuid==ru){r=&row;break;}}}if(!r||r->session_uuid!=request.header.session_uuid)return refuse("SECURITY.ACCESS_DENIED","ddl_create_table_receipt_hidden");auto c=EngineContextForSession(s->second,engine_state,request);c.statement_uuid.canonical=ru;c.statement_metadata_snapshot_engine_owned=true;c.trace_tags.push_back("private_ddl_create_table_binder");auto q=engine_api::CompileSblrDdlCreateTableDescriptor(c,ru,v.occurrence,v.table_occurrence,r->view.ddl_create_table_executor_availability_generation ? r->view.ddl_create_table_executor_availability_generation : (r->view.kv_structured_mutate_executor_availability_generation ? r->view.kv_structured_mutate_executor_availability_generation : r->view.kv_structured_read_executor_availability_generation));if(!q.ok)return refuse(q.diagnostic.code,q.diagnostic.message_key);result.payload=scratchbird::engine::sblr::EncodeSblrDdlCreateTableDescriptorV1(q.descriptor,false);if(result.payload.empty())return refuse("KV.SCAN_FAILED","CTDX_encode_failed");result.accepted=true;return result;}

SessionOperationResult HandleCoordinateDdlCreateIndex(ServerSessionRegistry*registry,const HostedEngineState&engine_state,const sbps::Frame&request){SessionOperationResult result;result.response_message_type=183;result.response_schema_id=sbps::kSchemaCoordinateDdlCreateIndexResultV1;result.frame_flags=sbps::kFlagResponse|sbps::kFlagFinal;result.session_uuid=request.header.session_uuid;auto refuse=[&](std::string c,std::string d){result.frame_flags|=sbps::kFlagError;result.diagnostics.push_back(sbps::IpcDiagnostic(std::move(c),"parser_server_ipc.ddl_create_index_refused","KV structured scan coordination was refused.",{{"detail",std::move(d)}}));return result;};scratchbird::engine::sblr::SblrDdlCreateIndexRequestV1 v;std::string d;if(!registry||!scratchbird::engine::sblr::DecodeSblrDdlCreateIndexRequestV1(request.payload.data(),request.payload.size(),&v,&d))return refuse("SBLR.OPERAND_INVALID",d);auto s=registry->sessions_by_uuid.find(UuidBytesToText(request.header.session_uuid));if(s==registry->sessions_by_uuid.end())return refuse("SECURITY.ACCESS_DENIED","session_hidden");auto ru=UuidBytesToText(v.receipt);ServerStatementContextRecord*r=nullptr;{std::lock_guard<std::mutex>g(*registry->statement_context_mutex);for(auto&[x,row]:registry->statement_contexts_by_statement_uuid){(void)x;if(!row.released&&row.view.receipt_uuid==ru){r=&row;break;}}}if(!r||r->session_uuid!=request.header.session_uuid)return refuse("SECURITY.ACCESS_DENIED","ddl_create_index_receipt_hidden");auto c=EngineContextForSession(s->second,engine_state,request);c.statement_uuid.canonical=ru;c.statement_metadata_snapshot_engine_owned=true;c.trace_tags.push_back("private_ddl_create_index_binder");auto q=engine_api::CompileSblrDdlCreateIndexDescriptor(c,ru,v.occurrence,v.index_occurrence,r->view.ddl_create_index_executor_availability_generation ? r->view.ddl_create_index_executor_availability_generation : (r->view.kv_structured_mutate_executor_availability_generation ? r->view.kv_structured_mutate_executor_availability_generation : r->view.kv_structured_read_executor_availability_generation));if(!q.ok)return refuse(q.diagnostic.code,q.diagnostic.message_key);result.payload=scratchbird::engine::sblr::EncodeSblrDdlCreateIndexDescriptorV1(q.descriptor,false);if(result.payload.empty())return refuse("KV.SCAN_FAILED","CIDX_encode_failed");result.accepted=true;return result;}

SessionOperationResult HandleCoordinateDdlDropIndex(ServerSessionRegistry*registry,const HostedEngineState&engine_state,const sbps::Frame&request){SessionOperationResult result;result.response_message_type=185;result.response_schema_id=sbps::kSchemaCoordinateDdlDropIndexResultV1;result.frame_flags=sbps::kFlagResponse|sbps::kFlagFinal;result.session_uuid=request.header.session_uuid;auto refuse=[&](std::string c,std::string d){result.frame_flags|=sbps::kFlagError;result.diagnostics.push_back(sbps::IpcDiagnostic(std::move(c),"parser_server_ipc.ddl_drop_index_refused","KV structured scan coordination was refused.",{{"detail",std::move(d)}}));return result;};scratchbird::engine::sblr::SblrDdlDropIndexRequestV1 v;std::string d;if(!registry||!scratchbird::engine::sblr::DecodeSblrDdlDropIndexRequestV1(request.payload.data(),request.payload.size(),&v,&d))return refuse("SBLR.OPERAND_INVALID",d);auto s=registry->sessions_by_uuid.find(UuidBytesToText(request.header.session_uuid));if(s==registry->sessions_by_uuid.end())return refuse("SECURITY.ACCESS_DENIED","session_hidden");auto ru=UuidBytesToText(v.receipt);ServerStatementContextRecord*r=nullptr;{std::lock_guard<std::mutex>g(*registry->statement_context_mutex);for(auto&[x,row]:registry->statement_contexts_by_statement_uuid){(void)x;if(!row.released&&row.view.receipt_uuid==ru){r=&row;break;}}}if(!r||r->session_uuid!=request.header.session_uuid)return refuse("SECURITY.ACCESS_DENIED","ddl_drop_index_receipt_hidden");auto c=EngineContextForSession(s->second,engine_state,request);c.statement_uuid.canonical=ru;c.statement_metadata_snapshot_engine_owned=true;c.trace_tags.push_back("private_ddl_drop_index_binder");auto q=engine_api::CompileSblrDdlDropIndexDescriptor(c,ru,v.occurrence,v.index_occurrence,r->view.ddl_drop_index_executor_availability_generation ? r->view.ddl_drop_index_executor_availability_generation : (r->view.kv_structured_mutate_executor_availability_generation ? r->view.kv_structured_mutate_executor_availability_generation : r->view.kv_structured_read_executor_availability_generation));if(!q.ok)return refuse(q.diagnostic.code,q.diagnostic.message_key);result.payload=scratchbird::engine::sblr::EncodeSblrDdlDropIndexDescriptorV1(q.descriptor,false);if(result.payload.empty())return refuse("KV.SCAN_FAILED","DIDX_encode_failed");result.accepted=true;return result;}

}  // namespace scratchbird::server

namespace scratchbird::server {
SessionOperationResult HandleCoordinateDdlAlterDomain(ServerSessionRegistry*registry,const HostedEngineState&engine_state,const sbps::Frame&request){SessionOperationResult result;result.response_message_type=187;result.response_schema_id=sbps::kSchemaCoordinateDdlAlterDomainResultV1;result.frame_flags=sbps::kFlagResponse|sbps::kFlagFinal;result.session_uuid=request.header.session_uuid;auto refuse=[&](std::string c,std::string d){result.frame_flags|=sbps::kFlagError;result.diagnostics.push_back(sbps::IpcDiagnostic(std::move(c),"parser_server_ipc.ddl_alter_domain_refused","KV structured scan coordination was refused.",{{"detail",std::move(d)}}));return result;};scratchbird::engine::sblr::SblrDdlAlterDomainRequestV1 v;std::string d;if(!registry||!scratchbird::engine::sblr::DecodeSblrDdlAlterDomainRequestV1(request.payload.data(),request.payload.size(),&v,&d))return refuse("SBLR.OPERAND_INVALID",d);auto s=registry->sessions_by_uuid.find(UuidBytesToText(request.header.session_uuid));if(s==registry->sessions_by_uuid.end())return refuse("SECURITY.ACCESS_DENIED","session_hidden");auto ru=UuidBytesToText(v.receipt);ServerStatementContextRecord*r=nullptr;{std::lock_guard<std::mutex>g(*registry->statement_context_mutex);for(auto&[x,row]:registry->statement_contexts_by_statement_uuid){(void)x;if(!row.released&&row.view.receipt_uuid==ru){r=&row;break;}}}if(!r||r->session_uuid!=request.header.session_uuid)return refuse("SECURITY.ACCESS_DENIED","ddl_alter_domain_receipt_hidden");auto c=EngineContextForSession(s->second,engine_state,request);c.statement_uuid.canonical=ru;c.statement_metadata_snapshot_engine_owned=true;c.trace_tags.push_back("private_ddl_alter_domain_binder");auto q=engine_api::CompileSblrDdlAlterDomainDescriptor(c,ru,v.occurrence,v.domain_occurrence,r->view.kv_structured_mutate_executor_availability_generation ? r->view.kv_structured_mutate_executor_availability_generation : r->view.kv_structured_read_executor_availability_generation);if(!q.ok)return refuse(q.diagnostic.code,q.diagnostic.message_key);result.payload=scratchbird::engine::sblr::EncodeSblrDdlAlterDomainDescriptorV1(q.descriptor,false);if(result.payload.empty())return refuse("KV.SCAN_FAILED","ADDX_encode_failed");result.accepted=true;return result;}SessionOperationResult HandleCoordinateDdlCreateView(ServerSessionRegistry*registry,const HostedEngineState&engine_state,const sbps::Frame&request){SessionOperationResult result;result.response_message_type=189;result.response_schema_id=sbps::kSchemaCoordinateDdlCreateViewResultV1;result.frame_flags=sbps::kFlagResponse|sbps::kFlagFinal;result.session_uuid=request.header.session_uuid;auto refuse=[&](std::string c,std::string d){result.frame_flags|=sbps::kFlagError;result.diagnostics.push_back(sbps::IpcDiagnostic(std::move(c),"parser_server_ipc.ddl_create_view_refused","KV structured scan coordination was refused.",{{"detail",std::move(d)}}));return result;};scratchbird::engine::sblr::SblrDdlCreateViewRequestV1 v;std::string d;if(!registry||!scratchbird::engine::sblr::DecodeSblrDdlCreateViewRequestV1(request.payload.data(),request.payload.size(),&v,&d))return refuse("SBLR.OPERAND_INVALID",d);auto s=registry->sessions_by_uuid.find(UuidBytesToText(request.header.session_uuid));if(s==registry->sessions_by_uuid.end())return refuse("SECURITY.ACCESS_DENIED","session_hidden");auto ru=UuidBytesToText(v.receipt);ServerStatementContextRecord*r=nullptr;{std::lock_guard<std::mutex>g(*registry->statement_context_mutex);for(auto&[x,row]:registry->statement_contexts_by_statement_uuid){(void)x;if(!row.released&&row.view.receipt_uuid==ru){r=&row;break;}}}if(!r||r->session_uuid!=request.header.session_uuid)return refuse("SECURITY.ACCESS_DENIED","ddl_create_view_receipt_hidden");auto c=EngineContextForSession(s->second,engine_state,request);c.statement_uuid.canonical=ru;c.statement_metadata_snapshot_engine_owned=true;c.trace_tags.push_back("private_ddl_create_view_binder");auto q=engine_api::CompileSblrDdlCreateViewDescriptor(c,ru,v.occurrence,v.domain_occurrence,r->view.kv_structured_mutate_executor_availability_generation ? r->view.kv_structured_mutate_executor_availability_generation : r->view.kv_structured_read_executor_availability_generation);if(!q.ok)return refuse(q.diagnostic.code,q.diagnostic.message_key);result.payload=scratchbird::engine::sblr::EncodeSblrDdlCreateViewDescriptorV1(q.descriptor,false);if(result.payload.empty())return refuse("KV.SCAN_FAILED","CVDX_encode_failed");result.accepted=true;return result;}
SessionOperationResult HandleCoordinateDdlAlterView(ServerSessionRegistry*registry,const HostedEngineState&engine_state,const sbps::Frame&request){SessionOperationResult result;result.response_message_type=191;result.response_schema_id=sbps::kSchemaCoordinateDdlAlterViewResultV1;result.frame_flags=sbps::kFlagResponse|sbps::kFlagFinal;result.session_uuid=request.header.session_uuid;auto refuse=[&](std::string c,std::string d){result.frame_flags|=sbps::kFlagError;result.diagnostics.push_back(sbps::IpcDiagnostic(std::move(c),"parser_server_ipc.ddl_alter_view_refused","KV structured scan coordination was refused.",{{"detail",std::move(d)}}));return result;};scratchbird::engine::sblr::SblrDdlAlterViewRequestV1 v;std::string d;if(!registry||!scratchbird::engine::sblr::DecodeSblrDdlAlterViewRequestV1(request.payload.data(),request.payload.size(),&v,&d))return refuse("SBLR.OPERAND_INVALID",d);auto s=registry->sessions_by_uuid.find(UuidBytesToText(request.header.session_uuid));if(s==registry->sessions_by_uuid.end())return refuse("SECURITY.ACCESS_DENIED","session_hidden");auto ru=UuidBytesToText(v.receipt);ServerStatementContextRecord*r=nullptr;{std::lock_guard<std::mutex>g(*registry->statement_context_mutex);for(auto&[x,row]:registry->statement_contexts_by_statement_uuid){(void)x;if(!row.released&&row.view.receipt_uuid==ru){r=&row;break;}}}if(!r||r->session_uuid!=request.header.session_uuid)return refuse("SECURITY.ACCESS_DENIED","ddl_alter_view_receipt_hidden");auto c=EngineContextForSession(s->second,engine_state,request);c.statement_uuid.canonical=ru;c.statement_metadata_snapshot_engine_owned=true;c.trace_tags.push_back("private_ddl_alter_view_binder");auto q=engine_api::CompileSblrDdlAlterViewDescriptor(c,ru,v.occurrence,v.domain_occurrence,r->view.kv_structured_mutate_executor_availability_generation ? r->view.kv_structured_mutate_executor_availability_generation : r->view.kv_structured_read_executor_availability_generation);if(!q.ok)return refuse(q.diagnostic.code,q.diagnostic.message_key);result.payload=scratchbird::engine::sblr::EncodeSblrDdlAlterViewDescriptorV1(q.descriptor,false);if(result.payload.empty())return refuse("KV.SCAN_FAILED","AVDX_encode_failed");result.accepted=true;return result;}
SessionOperationResult HandleCoordinateDdlDropView(ServerSessionRegistry*registry,const HostedEngineState&engine_state,const sbps::Frame&request){SessionOperationResult result;result.response_message_type=193;result.response_schema_id=sbps::kSchemaCoordinateDdlDropViewResultV1;result.frame_flags=sbps::kFlagResponse|sbps::kFlagFinal;result.session_uuid=request.header.session_uuid;auto refuse=[&](std::string c,std::string d){result.frame_flags|=sbps::kFlagError;result.diagnostics.push_back(sbps::IpcDiagnostic(std::move(c),"parser_server_ipc.ddl_drop_view_refused","KV structured scan coordination was refused.",{{"detail",std::move(d)}}));return result;};scratchbird::engine::sblr::SblrDdlDropViewRequestV1 v;std::string d;if(!registry||!scratchbird::engine::sblr::DecodeSblrDdlDropViewRequestV1(request.payload.data(),request.payload.size(),&v,&d))return refuse("SBLR.OPERAND_INVALID",d);auto s=registry->sessions_by_uuid.find(UuidBytesToText(request.header.session_uuid));if(s==registry->sessions_by_uuid.end())return refuse("SECURITY.ACCESS_DENIED","session_hidden");auto ru=UuidBytesToText(v.receipt);ServerStatementContextRecord*r=nullptr;{std::lock_guard<std::mutex>g(*registry->statement_context_mutex);for(auto&[x,row]:registry->statement_contexts_by_statement_uuid){(void)x;if(!row.released&&row.view.receipt_uuid==ru){r=&row;break;}}}if(!r||r->session_uuid!=request.header.session_uuid)return refuse("SECURITY.ACCESS_DENIED","ddl_drop_view_receipt_hidden");auto c=EngineContextForSession(s->second,engine_state,request);c.statement_uuid.canonical=ru;c.statement_metadata_snapshot_engine_owned=true;c.trace_tags.push_back("private_ddl_drop_view_binder");auto q=engine_api::CompileSblrDdlDropViewDescriptor(c,ru,v.occurrence,v.domain_occurrence,r->view.kv_structured_mutate_executor_availability_generation ? r->view.kv_structured_mutate_executor_availability_generation : r->view.kv_structured_read_executor_availability_generation);if(!q.ok)return refuse(q.diagnostic.code,q.diagnostic.message_key);result.payload=scratchbird::engine::sblr::EncodeSblrDdlDropViewDescriptorV1(q.descriptor,false);if(result.payload.empty())return refuse("KV.SCAN_FAILED","DVDX_encode_failed");result.accepted=true;return result;}
SessionOperationResult HandleCoordinateDdlCreateTrigger(ServerSessionRegistry*registry,const HostedEngineState&engine_state,const sbps::Frame&request){SessionOperationResult result;result.response_message_type=195;result.response_schema_id=sbps::kSchemaCoordinateDdlCreateTriggerResultV1;result.frame_flags=sbps::kFlagResponse|sbps::kFlagFinal;result.session_uuid=request.header.session_uuid;auto refuse=[&](std::string c,std::string d){result.frame_flags|=sbps::kFlagError;result.diagnostics.push_back(sbps::IpcDiagnostic(std::move(c),"parser_server_ipc.ddl_create_trigger_refused","KV structured scan coordination was refused.",{{"detail",std::move(d)}}));return result;};scratchbird::engine::sblr::SblrDdlCreateTriggerRequestV1 v;std::string d;if(!registry||!scratchbird::engine::sblr::DecodeSblrDdlCreateTriggerRequestV1(request.payload.data(),request.payload.size(),&v,&d))return refuse("SBLR.OPERAND_INVALID",d);auto s=registry->sessions_by_uuid.find(UuidBytesToText(request.header.session_uuid));if(s==registry->sessions_by_uuid.end())return refuse("SECURITY.ACCESS_DENIED","session_hidden");auto ru=UuidBytesToText(v.receipt);ServerStatementContextRecord*r=nullptr;{std::lock_guard<std::mutex>g(*registry->statement_context_mutex);for(auto&[x,row]:registry->statement_contexts_by_statement_uuid){(void)x;if(!row.released&&row.view.receipt_uuid==ru){r=&row;break;}}}if(!r||r->session_uuid!=request.header.session_uuid)return refuse("SECURITY.ACCESS_DENIED","ddl_create_trigger_receipt_hidden");auto c=EngineContextForSession(s->second,engine_state,request);c.statement_uuid.canonical=ru;c.statement_metadata_snapshot_engine_owned=true;c.trace_tags.push_back("private_ddl_create_trigger_binder");auto q=engine_api::CompileSblrDdlCreateTriggerDescriptor(c,ru,v.occurrence,v.domain_occurrence,r->view.kv_structured_mutate_executor_availability_generation ? r->view.kv_structured_mutate_executor_availability_generation : r->view.kv_structured_read_executor_availability_generation);if(!q.ok)return refuse(q.diagnostic.code,q.diagnostic.message_key);result.payload=scratchbird::engine::sblr::EncodeSblrDdlCreateTriggerDescriptorV1(q.descriptor,false);if(result.payload.empty())return refuse("KV.SCAN_FAILED","TVDX_encode_failed");result.accepted=true;return result;}
SessionOperationResult HandleCoordinateDdlAlterTrigger(ServerSessionRegistry*registry,const HostedEngineState&engine_state,const sbps::Frame&request){SessionOperationResult result;result.response_message_type=197;result.response_schema_id=sbps::kSchemaCoordinateDdlAlterTriggerResultV1;result.frame_flags=sbps::kFlagResponse|sbps::kFlagFinal;result.session_uuid=request.header.session_uuid;auto refuse=[&](std::string c,std::string d){result.frame_flags|=sbps::kFlagError;result.diagnostics.push_back(sbps::IpcDiagnostic(std::move(c),"parser_server_ipc.ddl_alter_trigger_refused","DDL alter trigger coordination was refused.",{{"detail",std::move(d)}}));return result;};scratchbird::engine::sblr::SblrDdlAlterTriggerRequestV1 v;std::string d;if(!registry||!scratchbird::engine::sblr::DecodeSblrDdlAlterTriggerRequestV1(request.payload.data(),request.payload.size(),&v,&d))return refuse("SBLR.OPERAND_INVALID",d);auto s=registry->sessions_by_uuid.find(UuidBytesToText(request.header.session_uuid));if(s==registry->sessions_by_uuid.end())return refuse("SECURITY.ACCESS_DENIED","session_hidden");auto ru=UuidBytesToText(v.receipt);ServerStatementContextRecord*r=nullptr;{std::lock_guard<std::mutex>g(*registry->statement_context_mutex);for(auto&[x,row]:registry->statement_contexts_by_statement_uuid){(void)x;if(!row.released&&row.view.receipt_uuid==ru){r=&row;break;}}}if(!r||r->session_uuid!=request.header.session_uuid)return refuse("SECURITY.ACCESS_DENIED","ddl_alter_trigger_receipt_hidden");auto c=EngineContextForSession(s->second,engine_state,request);c.statement_uuid.canonical=ru;c.statement_metadata_snapshot_engine_owned=true;c.trace_tags.push_back("private_ddl_alter_trigger_binder");auto q=engine_api::CompileSblrDdlAlterTriggerDescriptor(c,ru,v.occurrence,v.trigger_occurrence,r->view.kv_structured_mutate_executor_availability_generation ? r->view.kv_structured_mutate_executor_availability_generation : r->view.kv_structured_read_executor_availability_generation);if(!q.ok)return refuse(q.diagnostic.code,q.diagnostic.message_key);result.payload=scratchbird::engine::sblr::EncodeSblrDdlAlterTriggerDescriptorV1(q.descriptor,false);if(result.payload.empty())return refuse("DDL.ALTER_TRIGGER_FAILED","TADX_encode_failed");result.accepted=true;return result;}
SessionOperationResult HandleCoordinateDdlDropTrigger(ServerSessionRegistry*registry,const HostedEngineState&engine_state,const sbps::Frame&request){SessionOperationResult result;result.response_message_type=199;result.response_schema_id=sbps::kSchemaCoordinateDdlDropTriggerResultV1;result.frame_flags=sbps::kFlagResponse|sbps::kFlagFinal;result.session_uuid=request.header.session_uuid;auto refuse=[&](std::string c,std::string d){result.frame_flags|=sbps::kFlagError;result.diagnostics.push_back(sbps::IpcDiagnostic(std::move(c),"parser_server_ipc.ddl_drop_trigger_refused","DDL drop trigger coordination was refused.",{{"detail",std::move(d)}}));return result;};scratchbird::engine::sblr::SblrDdlDropTriggerRequestV1 v;std::string d;if(!registry||!scratchbird::engine::sblr::DecodeSblrDdlDropTriggerRequestV1(request.payload.data(),request.payload.size(),&v,&d))return refuse("SBLR.OPERAND_INVALID",d);auto s=registry->sessions_by_uuid.find(UuidBytesToText(request.header.session_uuid));if(s==registry->sessions_by_uuid.end())return refuse("SECURITY.ACCESS_DENIED","session_hidden");auto ru=UuidBytesToText(v.receipt);ServerStatementContextRecord*r=nullptr;{std::lock_guard<std::mutex>g(*registry->statement_context_mutex);for(auto&[x,row]:registry->statement_contexts_by_statement_uuid){(void)x;if(!row.released&&row.view.receipt_uuid==ru){r=&row;break;}}}if(!r||r->session_uuid!=request.header.session_uuid)return refuse("SECURITY.ACCESS_DENIED","ddl_drop_trigger_receipt_hidden");auto c=EngineContextForSession(s->second,engine_state,request);c.statement_uuid.canonical=ru;c.statement_metadata_snapshot_engine_owned=true;c.trace_tags.push_back("private_ddl_drop_trigger_binder");auto q=engine_api::CompileSblrDdlDropTriggerDescriptor(c,ru,v.occurrence,v.trigger_occurrence,r->view.kv_structured_mutate_executor_availability_generation ? r->view.kv_structured_mutate_executor_availability_generation : r->view.kv_structured_read_executor_availability_generation);if(!q.ok)return refuse(q.diagnostic.code,q.diagnostic.message_key);result.payload=scratchbird::engine::sblr::EncodeSblrDdlDropTriggerDescriptorV1(q.descriptor,false);if(result.payload.empty())return refuse("DDL.DROP_TRIGGER_FAILED","TDDX_encode_failed");result.accepted=true;return result;}
SessionOperationResult HandleCoordinateDdlCreateProcedure(ServerSessionRegistry*registry,const HostedEngineState&engine_state,const sbps::Frame&request){SessionOperationResult result;result.response_message_type=201;result.response_schema_id=sbps::kSchemaCoordinateDdlCreateProcedureResultV1;result.frame_flags=sbps::kFlagResponse|sbps::kFlagFinal;result.session_uuid=request.header.session_uuid;auto refuse=[&](std::string c,std::string d){result.frame_flags|=sbps::kFlagError;result.diagnostics.push_back(sbps::IpcDiagnostic(std::move(c),"parser_server_ipc.ddl_create_procedure_refused","DDL create procedure coordination was refused.",{{"detail",std::move(d)}}));return result;};scratchbird::engine::sblr::SblrDdlCreateProcedureRequestV1 v;std::string d;if(!registry||!scratchbird::engine::sblr::DecodeSblrDdlCreateProcedureRequestV1(request.payload.data(),request.payload.size(),&v,&d))return refuse("SBLR.OPERAND_INVALID",d);auto s=registry->sessions_by_uuid.find(UuidBytesToText(request.header.session_uuid));if(s==registry->sessions_by_uuid.end())return refuse("SECURITY.ACCESS_DENIED","session_hidden");auto ru=UuidBytesToText(v.receipt);ServerStatementContextRecord*r=nullptr;{std::lock_guard<std::mutex>g(*registry->statement_context_mutex);for(auto&[x,row]:registry->statement_contexts_by_statement_uuid){(void)x;if(!row.released&&row.view.receipt_uuid==ru){r=&row;break;}}}if(!r||r->session_uuid!=request.header.session_uuid)return refuse("SECURITY.ACCESS_DENIED","ddl_create_procedure_receipt_hidden");auto c=EngineContextForSession(s->second,engine_state,request);c.statement_uuid.canonical=ru;c.statement_metadata_snapshot_engine_owned=true;c.trace_tags.push_back("private_ddl_create_procedure_binder");auto q=engine_api::CompileSblrDdlCreateProcedureDescriptor(c,ru,v.occurrence,v.procedure_occurrence,r->view.kv_structured_mutate_executor_availability_generation ? r->view.kv_structured_mutate_executor_availability_generation : r->view.kv_structured_read_executor_availability_generation);if(!q.ok)return refuse(q.diagnostic.code,q.diagnostic.message_key);result.payload=scratchbird::engine::sblr::EncodeSblrDdlCreateProcedureDescriptorV1(q.descriptor,false);if(result.payload.empty())return refuse("DDL.CREATE_PROCEDURE_FAILED","PCDX_encode_failed");result.accepted=true;return result;}
SessionOperationResult HandleCoordinateDdlAlterProcedure(ServerSessionRegistry*registry,const HostedEngineState&engine_state,const sbps::Frame&request){SessionOperationResult result;result.response_message_type=203;result.response_schema_id=sbps::kSchemaCoordinateDdlAlterProcedureResultV1;result.frame_flags=sbps::kFlagResponse|sbps::kFlagFinal;result.session_uuid=request.header.session_uuid;auto refuse=[&](std::string c,std::string d){result.frame_flags|=sbps::kFlagError;result.diagnostics.push_back(sbps::IpcDiagnostic(std::move(c),"parser_server_ipc.ddl_alter_procedure_refused","DDL alter procedure coordination was refused.",{{"detail",std::move(d)}}));return result;};scratchbird::engine::sblr::SblrDdlAlterProcedureRequestV1 v;std::string d;if(!registry||!scratchbird::engine::sblr::DecodeSblrDdlAlterProcedureRequestV1(request.payload.data(),request.payload.size(),&v,&d))return refuse("SBLR.OPERAND_INVALID",d);auto s=registry->sessions_by_uuid.find(UuidBytesToText(request.header.session_uuid));if(s==registry->sessions_by_uuid.end())return refuse("SECURITY.ACCESS_DENIED","session_hidden");auto ru=UuidBytesToText(v.receipt);ServerStatementContextRecord*r=nullptr;{std::lock_guard<std::mutex>g(*registry->statement_context_mutex);for(auto&[x,row]:registry->statement_contexts_by_statement_uuid){(void)x;if(!row.released&&row.view.receipt_uuid==ru){r=&row;break;}}}if(!r||r->session_uuid!=request.header.session_uuid)return refuse("SECURITY.ACCESS_DENIED","ddl_alter_procedure_receipt_hidden");auto c=EngineContextForSession(s->second,engine_state,request);c.statement_uuid.canonical=ru;c.statement_metadata_snapshot_engine_owned=true;c.trace_tags.push_back("private_ddl_create_procedure_binder");auto q=engine_api::CompileSblrDdlAlterProcedureDescriptor(c,ru,v.occurrence,v.procedure_occurrence,r->view.kv_structured_mutate_executor_availability_generation ? r->view.kv_structured_mutate_executor_availability_generation : r->view.kv_structured_read_executor_availability_generation);if(!q.ok)return refuse(q.diagnostic.code,q.diagnostic.message_key);result.payload=scratchbird::engine::sblr::EncodeSblrDdlAlterProcedureDescriptorV1(q.descriptor,false);if(result.payload.empty())return refuse("DDL.ALTER_PROCEDURE_FAILED","PADX_encode_failed");result.accepted=true;return result;}
SessionOperationResult HandleCoordinateDdlDropProcedure(ServerSessionRegistry*registry,const HostedEngineState&engine_state,const sbps::Frame&request){SessionOperationResult result;result.response_message_type=205;result.response_schema_id=sbps::kSchemaCoordinateDdlDropProcedureResultV1;result.frame_flags=sbps::kFlagResponse|sbps::kFlagFinal;result.session_uuid=request.header.session_uuid;auto refuse=[&](std::string c,std::string d){result.frame_flags|=sbps::kFlagError;result.diagnostics.push_back(sbps::IpcDiagnostic(std::move(c),"parser_server_ipc.ddl_drop_procedure_refused","DDL drop procedure coordination was refused.",{{"detail",std::move(d)}}));return result;};scratchbird::engine::sblr::SblrDdlDropProcedureRequestV1 v;std::string d;if(!registry||!scratchbird::engine::sblr::DecodeSblrDdlDropProcedureRequestV1(request.payload.data(),request.payload.size(),&v,&d))return refuse("SBLR.OPERAND_INVALID",d);auto s=registry->sessions_by_uuid.find(UuidBytesToText(request.header.session_uuid));if(s==registry->sessions_by_uuid.end())return refuse("SECURITY.ACCESS_DENIED","session_hidden");auto ru=UuidBytesToText(v.receipt);ServerStatementContextRecord*r=nullptr;{std::lock_guard<std::mutex>g(*registry->statement_context_mutex);for(auto&[x,row]:registry->statement_contexts_by_statement_uuid){(void)x;if(!row.released&&row.view.receipt_uuid==ru){r=&row;break;}}}if(!r||r->session_uuid!=request.header.session_uuid)return refuse("SECURITY.ACCESS_DENIED","ddl_drop_procedure_receipt_hidden");auto c=EngineContextForSession(s->second,engine_state,request);c.statement_uuid.canonical=ru;c.statement_metadata_snapshot_engine_owned=true;c.trace_tags.push_back("private_ddl_create_procedure_binder");auto q=engine_api::CompileSblrDdlDropProcedureDescriptor(c,ru,v.occurrence,v.procedure_occurrence,r->view.kv_structured_mutate_executor_availability_generation ? r->view.kv_structured_mutate_executor_availability_generation : r->view.kv_structured_read_executor_availability_generation);if(!q.ok)return refuse(q.diagnostic.code,q.diagnostic.message_key);result.payload=scratchbird::engine::sblr::EncodeSblrDdlDropProcedureDescriptorV1(q.descriptor,false);if(result.payload.empty())return refuse("DDL.DROP_PROCEDURE_FAILED","PDDX_encode_failed");result.accepted=true;return result;}
SessionOperationResult HandleCoordinateDdlCreateFunction(ServerSessionRegistry*registry,const HostedEngineState&engine_state,const sbps::Frame&request){SessionOperationResult result;result.response_message_type=207;result.response_schema_id=sbps::kSchemaCoordinateDdlCreateFunctionResultV1;result.frame_flags=sbps::kFlagResponse|sbps::kFlagFinal;result.session_uuid=request.header.session_uuid;auto refuse=[&](std::string c,std::string d){result.frame_flags|=sbps::kFlagError;result.diagnostics.push_back(sbps::IpcDiagnostic(std::move(c),"parser_server_ipc.ddl_create_function_refused","DDL create function coordination was refused.",{{"detail",std::move(d)}}));return result;};scratchbird::engine::sblr::SblrDdlCreateFunctionRequestV1 v;std::string d;if(!registry||!scratchbird::engine::sblr::DecodeSblrDdlCreateFunctionRequestV1(request.payload.data(),request.payload.size(),&v,&d))return refuse("SBLR.OPERAND_INVALID",d);auto s=registry->sessions_by_uuid.find(UuidBytesToText(request.header.session_uuid));if(s==registry->sessions_by_uuid.end())return refuse("SECURITY.ACCESS_DENIED","session_hidden");auto ru=UuidBytesToText(v.receipt);ServerStatementContextRecord*r=nullptr;{std::lock_guard<std::mutex>g(*registry->statement_context_mutex);for(auto&[x,row]:registry->statement_contexts_by_statement_uuid){(void)x;if(!row.released&&row.view.receipt_uuid==ru){r=&row;break;}}}if(!r||r->session_uuid!=request.header.session_uuid)return refuse("SECURITY.ACCESS_DENIED","ddl_create_function_receipt_hidden");auto c=EngineContextForSession(s->second,engine_state,request);c.statement_uuid.canonical=ru;c.statement_metadata_snapshot_engine_owned=true;c.trace_tags.push_back("private_ddl_create_procedure_binder");auto q=engine_api::CompileSblrDdlCreateFunctionDescriptor(c,ru,v.occurrence,v.procedure_occurrence,r->view.kv_structured_read_executor_availability_generation);if(!q.ok)return refuse(q.diagnostic.code,q.diagnostic.message_key);result.payload=scratchbird::engine::sblr::EncodeSblrDdlCreateFunctionDescriptorV1(q.descriptor,false);if(result.payload.empty())return refuse("DDL.CREATE_FUNCTION_FAILED","FDDX_encode_failed");result.accepted=true;return result;}
SessionOperationResult HandleCoordinateDdlAlterFunction(ServerSessionRegistry*registry,const HostedEngineState&engine_state,const sbps::Frame&request){SessionOperationResult result;result.response_message_type=209;result.response_schema_id=sbps::kSchemaCoordinateDdlAlterFunctionResultV1;result.frame_flags=sbps::kFlagResponse|sbps::kFlagFinal;result.session_uuid=request.header.session_uuid;auto refuse=[&](std::string c,std::string d){result.frame_flags|=sbps::kFlagError;result.diagnostics.push_back(sbps::IpcDiagnostic(std::move(c),"parser_server_ipc.ddl_alter_function_refused","DDL alter function coordination was refused.",{{"detail",std::move(d)}}));return result;};scratchbird::engine::sblr::SblrDdlAlterFunctionRequestV1 v;std::string d;if(!registry||!scratchbird::engine::sblr::DecodeSblrDdlAlterFunctionRequestV1(request.payload.data(),request.payload.size(),&v,&d))return refuse("SBLR.OPERAND_INVALID",d);auto s=registry->sessions_by_uuid.find(UuidBytesToText(request.header.session_uuid));if(s==registry->sessions_by_uuid.end())return refuse("SECURITY.ACCESS_DENIED","session_hidden");auto ru=UuidBytesToText(v.receipt);ServerStatementContextRecord*r=nullptr;{std::lock_guard<std::mutex>g(*registry->statement_context_mutex);for(auto&[x,row]:registry->statement_contexts_by_statement_uuid){(void)x;if(!row.released&&row.view.receipt_uuid==ru){r=&row;break;}}}if(!r||r->session_uuid!=request.header.session_uuid)return refuse("SECURITY.ACCESS_DENIED","ddl_alter_function_receipt_hidden");auto c=EngineContextForSession(s->second,engine_state,request);c.statement_uuid.canonical=ru;c.statement_metadata_snapshot_engine_owned=true;c.trace_tags.push_back("private_ddl_create_procedure_binder");auto q=engine_api::CompileSblrDdlAlterFunctionDescriptor(c,ru,v.occurrence,v.procedure_occurrence,r->view.kv_structured_read_executor_availability_generation);if(!q.ok)return refuse(q.diagnostic.code,q.diagnostic.message_key);result.payload=scratchbird::engine::sblr::EncodeSblrDdlAlterFunctionDescriptorV1(q.descriptor,false);if(result.payload.empty())return refuse("DDL.ALTER_FUNCTION_FAILED","FADX_encode_failed");result.accepted=true;return result;}
SessionOperationResult HandleCoordinateDdlDropFunction(ServerSessionRegistry*registry,const HostedEngineState&engine_state,const sbps::Frame&request){SessionOperationResult result;result.response_message_type=211;result.response_schema_id=sbps::kSchemaCoordinateDdlDropFunctionResultV1;result.frame_flags=sbps::kFlagResponse|sbps::kFlagFinal;result.session_uuid=request.header.session_uuid;auto refuse=[&](std::string c,std::string d){result.frame_flags|=sbps::kFlagError;result.diagnostics.push_back(sbps::IpcDiagnostic(std::move(c),"parser_server_ipc.ddl_alter_function_refused","DDL drop function coordination was refused.",{{"detail",std::move(d)}}));return result;};scratchbird::engine::sblr::SblrDdlDropFunctionRequestV1 v;std::string d;if(!registry||!scratchbird::engine::sblr::DecodeSblrDdlDropFunctionRequestV1(request.payload.data(),request.payload.size(),&v,&d))return refuse("SBLR.OPERAND_INVALID",d);auto s=registry->sessions_by_uuid.find(UuidBytesToText(request.header.session_uuid));if(s==registry->sessions_by_uuid.end())return refuse("SECURITY.ACCESS_DENIED","session_hidden");auto ru=UuidBytesToText(v.receipt);ServerStatementContextRecord*r=nullptr;{std::lock_guard<std::mutex>g(*registry->statement_context_mutex);for(auto&[x,row]:registry->statement_contexts_by_statement_uuid){(void)x;if(!row.released&&row.view.receipt_uuid==ru){r=&row;break;}}}if(!r||r->session_uuid!=request.header.session_uuid)return refuse("SECURITY.ACCESS_DENIED","ddl_alter_function_receipt_hidden");auto c=EngineContextForSession(s->second,engine_state,request);c.statement_uuid.canonical=ru;c.statement_metadata_snapshot_engine_owned=true;c.trace_tags.push_back("private_ddl_drop_function_binder");auto q=engine_api::CompileSblrDdlDropFunctionDescriptor(c,ru,v.occurrence,v.function_occurrence,r->view.kv_structured_read_executor_availability_generation);if(!q.ok)return refuse(q.diagnostic.code,q.diagnostic.message_key);result.payload=scratchbird::engine::sblr::EncodeSblrDdlDropFunctionDescriptorV1(q.descriptor,false);if(result.payload.empty())return refuse("DDL.DROP_FUNCTION_FAILED","FDDX_encode_failed");result.accepted=true;return result;}
SessionOperationResult HandleCoordinateDdlCreatePackage(ServerSessionRegistry*registry,const HostedEngineState&engine_state,const sbps::Frame&request){SessionOperationResult result;result.response_message_type=213;result.response_schema_id=sbps::kSchemaCoordinateDdlCreatePackageResultV1;result.frame_flags=sbps::kFlagResponse|sbps::kFlagFinal;result.session_uuid=request.header.session_uuid;auto refuse=[&](std::string c,std::string d){result.frame_flags|=sbps::kFlagError;result.diagnostics.push_back(sbps::IpcDiagnostic(std::move(c),"parser_server_ipc.ddl_create_package_refused","DDL create package coordination was refused.",{{"detail",std::move(d)}}));return result;};scratchbird::engine::sblr::SblrDdlCreatePackageRequestV1 v;std::string d;if(!registry||!scratchbird::engine::sblr::DecodeSblrDdlCreatePackageRequestV1(request.payload.data(),request.payload.size(),&v,&d))return refuse("SBLR.OPERAND_INVALID",d);auto s=registry->sessions_by_uuid.find(UuidBytesToText(request.header.session_uuid));if(s==registry->sessions_by_uuid.end())return refuse("SECURITY.ACCESS_DENIED","session_hidden");auto ru=UuidBytesToText(v.receipt);ServerStatementContextRecord*r=nullptr;{std::lock_guard<std::mutex>g(*registry->statement_context_mutex);for(auto&[x,row]:registry->statement_contexts_by_statement_uuid){(void)x;if(!row.released&&row.view.receipt_uuid==ru){r=&row;break;}}}if(!r||r->session_uuid!=request.header.session_uuid)return refuse("SECURITY.ACCESS_DENIED","ddl_create_package_receipt_hidden");auto c=EngineContextForSession(s->second,engine_state,request);c.statement_uuid.canonical=ru;c.statement_metadata_snapshot_engine_owned=true;c.trace_tags.push_back("private_ddl_create_package_binder");auto q=engine_api::CompileSblrDdlCreatePackageDescriptor(c,ru,v.occurrence,v.procedure_occurrence,r->view.kv_structured_read_executor_availability_generation);if(!q.ok)return refuse(q.diagnostic.code,q.diagnostic.message_key);result.payload=scratchbird::engine::sblr::EncodeSblrDdlCreatePackageDescriptorV1(q.descriptor,false);if(result.payload.empty())return refuse("DDL.CREATE_PACKAGE_FAILED","PGDX_encode_failed");result.accepted=true;return result;}
SessionOperationResult HandleCoordinateDdlCreateTemporaryTable(ServerSessionRegistry*registry,const HostedEngineState&engine_state,const sbps::Frame&request){SessionOperationResult result;result.response_message_type=215;result.response_schema_id=sbps::kSchemaCoordinateDdlCreateTemporaryTableResultV1;result.frame_flags=sbps::kFlagResponse|sbps::kFlagFinal;result.session_uuid=request.header.session_uuid;auto refuse=[&](std::string c,std::string d){result.frame_flags|=sbps::kFlagError;result.diagnostics.push_back(sbps::IpcDiagnostic(std::move(c),"parser_server_ipc.ddl_create_temporary_table_refused","DDL create temporary table coordination was refused.",{{"detail",std::move(d)}}));return result;};scratchbird::engine::sblr::SblrDdlCreateTemporaryTableRequestV1 v;std::string d;if(!registry||!scratchbird::engine::sblr::DecodeSblrDdlCreateTemporaryTableRequestV1(request.payload.data(),request.payload.size(),&v,&d))return refuse("SBLR.OPERAND_INVALID",d);auto s=registry->sessions_by_uuid.find(UuidBytesToText(request.header.session_uuid));if(s==registry->sessions_by_uuid.end())return refuse("SECURITY.ACCESS_DENIED","session_hidden");auto ru=UuidBytesToText(v.receipt);ServerStatementContextRecord*r=nullptr;{std::lock_guard<std::mutex>g(*registry->statement_context_mutex);for(auto&[x,row]:registry->statement_contexts_by_statement_uuid){(void)x;if(!row.released&&row.view.receipt_uuid==ru){r=&row;break;}}}if(!r||r->session_uuid!=request.header.session_uuid)return refuse("SECURITY.ACCESS_DENIED","ddl_create_temporary_table_receipt_hidden");auto c=EngineContextForSession(s->second,engine_state,request);c.statement_uuid.canonical=ru;c.statement_metadata_snapshot_engine_owned=true;c.trace_tags.push_back("private_ddl_create_temporary_table_binder");auto q=engine_api::CompileSblrDdlCreateTemporaryTableDescriptor(c,ru,v.occurrence,v.temporary_table_occurrence,r->view.kv_structured_read_executor_availability_generation);if(!q.ok)return refuse(q.diagnostic.code,q.diagnostic.message_key);result.payload=scratchbird::engine::sblr::EncodeSblrDdlCreateTemporaryTableDescriptorV1(q.descriptor,false);if(result.payload.empty())return refuse("DDL.CREATE_PACKAGE_FAILED","TTDX_encode_failed");result.accepted=true;return result;}
SessionOperationResult HandleCoordinateDdlDropTemporaryTable(ServerSessionRegistry*registry,const HostedEngineState&engine_state,const sbps::Frame&request){SessionOperationResult result;result.response_message_type=217;result.response_schema_id=sbps::kSchemaCoordinateDdlDropTemporaryTableResultV1;result.frame_flags=sbps::kFlagResponse|sbps::kFlagFinal;result.session_uuid=request.header.session_uuid;auto refuse=[&](std::string c,std::string d){result.frame_flags|=sbps::kFlagError;result.diagnostics.push_back(sbps::IpcDiagnostic(std::move(c),"parser_server_ipc.ddl_drop_temporary_table_refused","DDL drop temporary table coordination was refused.",{{"detail",std::move(d)}}));return result;};scratchbird::engine::sblr::SblrDdlDropTemporaryTableRequestV1 v;std::string d;if(!registry||!scratchbird::engine::sblr::DecodeSblrDdlDropTemporaryTableRequestV1(request.payload.data(),request.payload.size(),&v,&d))return refuse("SBLR.OPERAND_INVALID",d);auto s=registry->sessions_by_uuid.find(UuidBytesToText(request.header.session_uuid));if(s==registry->sessions_by_uuid.end())return refuse("SECURITY.ACCESS_DENIED","session_hidden");auto ru=UuidBytesToText(v.receipt);ServerStatementContextRecord*r=nullptr;{std::lock_guard<std::mutex>g(*registry->statement_context_mutex);for(auto&[x,row]:registry->statement_contexts_by_statement_uuid){(void)x;if(!row.released&&row.view.receipt_uuid==ru){r=&row;break;}}}if(!r||r->session_uuid!=request.header.session_uuid)return refuse("SECURITY.ACCESS_DENIED","ddl_drop_temporary_table_receipt_hidden");auto c=EngineContextForSession(s->second,engine_state,request);c.statement_uuid.canonical=ru;c.statement_metadata_snapshot_engine_owned=true;c.trace_tags.push_back("private_ddl_drop_temporary_table_binder");auto q=engine_api::CompileSblrDdlDropTemporaryTableDescriptor(c,ru,v.occurrence,v.temporary_table_occurrence,r->view.kv_structured_read_executor_availability_generation);if(!q.ok)return refuse(q.diagnostic.code,q.diagnostic.message_key);result.payload=scratchbird::engine::sblr::EncodeSblrDdlDropTemporaryTableDescriptorV1(q.descriptor,false);if(result.payload.empty())return refuse("DDL.CREATE_PACKAGE_FAILED","TTDX_encode_failed");result.accepted=true;return result;}
SessionOperationResult HandleCoordinateDdlRenameObjectVector(ServerSessionRegistry*registry,const HostedEngineState&engine_state,const sbps::Frame&request){SessionOperationResult result;result.response_message_type=219;result.response_schema_id=sbps::kSchemaCoordinateDdlRenameObjectVectorResultV1;result.frame_flags=sbps::kFlagResponse|sbps::kFlagFinal;result.session_uuid=request.header.session_uuid;auto refuse=[&](std::string c,std::string d){result.frame_flags|=sbps::kFlagError;result.diagnostics.push_back(sbps::IpcDiagnostic(std::move(c),"parser_server_ipc.ddl_rename_object_vector_refused","DDL drop temporary table coordination was refused.",{{"detail",std::move(d)}}));return result;};scratchbird::engine::sblr::SblrDdlRenameObjectVectorRequestV1 v;std::string d;if(!registry||!scratchbird::engine::sblr::DecodeSblrDdlRenameObjectVectorRequestV1(request.payload.data(),request.payload.size(),&v,&d))return refuse("SBLR.OPERAND_INVALID",d);auto s=registry->sessions_by_uuid.find(UuidBytesToText(request.header.session_uuid));if(s==registry->sessions_by_uuid.end())return refuse("SECURITY.ACCESS_DENIED","session_hidden");auto ru=UuidBytesToText(v.receipt);ServerStatementContextRecord*r=nullptr;{std::lock_guard<std::mutex>g(*registry->statement_context_mutex);for(auto&[x,row]:registry->statement_contexts_by_statement_uuid){(void)x;if(!row.released&&row.view.receipt_uuid==ru){r=&row;break;}}}if(!r||r->session_uuid!=request.header.session_uuid)return refuse("SECURITY.ACCESS_DENIED","ddl_rename_object_vector_receipt_hidden");auto c=EngineContextForSession(s->second,engine_state,request);c.statement_uuid.canonical=ru;c.statement_metadata_snapshot_engine_owned=true;c.trace_tags.push_back("private_ddl_rename_object_vector_binder");auto q=engine_api::CompileSblrDdlRenameObjectVectorDescriptor(c,ru,v.occurrence,v.object_vector_occurrence,r->view.kv_structured_read_executor_availability_generation);if(!q.ok)return refuse(q.diagnostic.code,q.diagnostic.message_key);result.payload=scratchbird::engine::sblr::EncodeSblrDdlRenameObjectVectorDescriptorV1(q.descriptor,false);if(result.payload.empty())return refuse("DDL.CREATE_PACKAGE_FAILED","TTDX_encode_failed");result.accepted=true;return result;}
SessionOperationResult HandleCoordinateDdlCreateOrReplaceSrs(ServerSessionRegistry*registry,const HostedEngineState&engine_state,const sbps::Frame&request){SessionOperationResult result;result.response_message_type=221;result.response_schema_id=sbps::kSchemaCoordinateDdlCreateOrReplaceSrsResultV1;result.frame_flags=sbps::kFlagResponse|sbps::kFlagFinal;result.session_uuid=request.header.session_uuid;auto refuse=[&](std::string c,std::string d){result.frame_flags|=sbps::kFlagError;result.diagnostics.push_back(sbps::IpcDiagnostic(std::move(c),"parser_server_ipc.ddl_create_or_replace_srs_refused","DDL create or replace SRS coordination was refused.",{{"detail",std::move(d)}}));return result;};scratchbird::engine::sblr::SblrDdlCreateOrReplaceSrsRequestV1 v;std::string d;if(!registry||!scratchbird::engine::sblr::DecodeSblrDdlCreateOrReplaceSrsRequestV1(request.payload.data(),request.payload.size(),&v,&d))return refuse("SBLR.OPERAND_INVALID",d);auto s=registry->sessions_by_uuid.find(UuidBytesToText(request.header.session_uuid));if(s==registry->sessions_by_uuid.end())return refuse("SECURITY.ACCESS_DENIED","session_hidden");auto ru=UuidBytesToText(v.receipt);ServerStatementContextRecord*r=nullptr;{std::lock_guard<std::mutex>g(*registry->statement_context_mutex);for(auto&[x,row]:registry->statement_contexts_by_statement_uuid){(void)x;if(!row.released&&row.view.receipt_uuid==ru){r=&row;break;}}}if(!r||r->session_uuid!=request.header.session_uuid)return refuse("SECURITY.ACCESS_DENIED","ddl_create_or_replace_srs_receipt_hidden");auto c=EngineContextForSession(s->second,engine_state,request);c.statement_uuid.canonical=ru;c.statement_metadata_snapshot_engine_owned=true;c.trace_tags.push_back("private_ddl_create_or_replace_srs_binder");auto q=engine_api::CompileSblrDdlCreateOrReplaceSrsDescriptor(c,ru,v.occurrence,v.srs_occurrence,r->view.kv_structured_read_executor_availability_generation);if(!q.ok)return refuse(q.diagnostic.code,q.diagnostic.message_key);result.payload=scratchbird::engine::sblr::EncodeSblrDdlCreateOrReplaceSrsDescriptorV1(q.descriptor,false);if(result.payload.empty())return refuse("DDL.CREATE_OR_REPLACE_SRS_FAILED","SRSD_encode_failed");result.accepted=true;return result;}
SessionOperationResult HandleCoordinateDdlDropSrs(ServerSessionRegistry*registry,const HostedEngineState&engine_state,const sbps::Frame&request){SessionOperationResult result;result.response_message_type=223;result.response_schema_id=sbps::kSchemaCoordinateDdlDropSrsResultV1;result.frame_flags=sbps::kFlagResponse|sbps::kFlagFinal;result.session_uuid=request.header.session_uuid;auto refuse=[&](std::string c,std::string d){result.frame_flags|=sbps::kFlagError;result.diagnostics.push_back(sbps::IpcDiagnostic(std::move(c),"parser_server_ipc.ddl_drop_srs_refused","DDL create or replace SRS coordination was refused.",{{"detail",std::move(d)}}));return result;};scratchbird::engine::sblr::SblrDdlDropSrsRequestV1 v;std::string d;if(!registry||!scratchbird::engine::sblr::DecodeSblrDdlDropSrsRequestV1(request.payload.data(),request.payload.size(),&v,&d))return refuse("SBLR.OPERAND_INVALID",d);auto s=registry->sessions_by_uuid.find(UuidBytesToText(request.header.session_uuid));if(s==registry->sessions_by_uuid.end())return refuse("SECURITY.ACCESS_DENIED","session_hidden");auto ru=UuidBytesToText(v.receipt);ServerStatementContextRecord*r=nullptr;{std::lock_guard<std::mutex>g(*registry->statement_context_mutex);for(auto&[x,row]:registry->statement_contexts_by_statement_uuid){(void)x;if(!row.released&&row.view.receipt_uuid==ru){r=&row;break;}}}if(!r||r->session_uuid!=request.header.session_uuid)return refuse("SECURITY.ACCESS_DENIED","ddl_drop_srs_receipt_hidden");auto c=EngineContextForSession(s->second,engine_state,request);c.statement_uuid.canonical=ru;c.statement_metadata_snapshot_engine_owned=true;c.trace_tags.push_back("private_ddl_drop_srs_binder");auto q=engine_api::CompileSblrDdlDropSrsDescriptor(c,ru,v.occurrence,v.srs_occurrence,r->view.kv_structured_read_executor_availability_generation);if(!q.ok)return refuse(q.diagnostic.code,q.diagnostic.message_key);result.payload=scratchbird::engine::sblr::EncodeSblrDdlDropSrsDescriptorV1(q.descriptor,false);if(result.payload.empty())return refuse("DDL.CREATE_OR_REPLACE_SRS_FAILED","SRXD_encode_failed");result.accepted=true;return result;}
SessionOperationResult HandleCoordinateDdlCreateRewriteRule(ServerSessionRegistry*registry,const HostedEngineState&engine_state,const sbps::Frame&request){SessionOperationResult result;result.response_message_type=225;result.response_schema_id=sbps::kSchemaCoordinateDdlCreateRewriteRuleResultV1;result.frame_flags=sbps::kFlagResponse|sbps::kFlagFinal;result.session_uuid=request.header.session_uuid;auto refuse=[&](std::string c,std::string d){result.frame_flags|=sbps::kFlagError;result.diagnostics.push_back(sbps::IpcDiagnostic(std::move(c),"parser_server_ipc.ddl_create_rewrite_rule_refused","DDL create rewrite rule coordination was refused.",{{"detail",std::move(d)}}));return result;};scratchbird::engine::sblr::SblrDdlCreateRewriteRuleRequestV1 v;std::string d;if(!registry||!scratchbird::engine::sblr::DecodeSblrDdlCreateRewriteRuleRequestV1(request.payload.data(),request.payload.size(),&v,&d))return refuse("SBLR.OPERAND_INVALID",d);auto s=registry->sessions_by_uuid.find(UuidBytesToText(request.header.session_uuid));if(s==registry->sessions_by_uuid.end())return refuse("SECURITY.ACCESS_DENIED","session_hidden");auto ru=UuidBytesToText(v.receipt);ServerStatementContextRecord*r=nullptr;{std::lock_guard<std::mutex>g(*registry->statement_context_mutex);for(auto&[x,row]:registry->statement_contexts_by_statement_uuid){(void)x;if(!row.released&&row.view.receipt_uuid==ru){r=&row;break;}}}if(!r||r->session_uuid!=request.header.session_uuid)return refuse("SECURITY.ACCESS_DENIED","ddl_create_rewrite_rule_receipt_hidden");auto c=EngineContextForSession(s->second,engine_state,request);c.statement_uuid.canonical=ru;c.statement_metadata_snapshot_engine_owned=true;c.trace_tags.push_back("private_ddl_create_rewrite_rule_binder");auto q=engine_api::CompileSblrDdlCreateRewriteRuleDescriptor(c,ru,v.occurrence,v.rule_occurrence,r->view.kv_structured_read_executor_availability_generation);if(!q.ok)return refuse(q.diagnostic.code,q.diagnostic.message_key);result.payload=scratchbird::engine::sblr::EncodeSblrDdlCreateRewriteRuleDescriptorV1(q.descriptor,false);if(result.payload.empty())return refuse("DDL.CREATE_REWRITE_RULE_FAILED","RWDD_encode_failed");result.accepted=true;return result;}
SessionOperationResult HandleCoordinateDdlAlterRewriteRule(ServerSessionRegistry*registry,const HostedEngineState&engine_state,const sbps::Frame&request){SessionOperationResult result;result.response_message_type=227;result.response_schema_id=sbps::kSchemaCoordinateDdlAlterRewriteRuleResultV1;result.frame_flags=sbps::kFlagResponse|sbps::kFlagFinal;result.session_uuid=request.header.session_uuid;auto refuse=[&](std::string c,std::string d){result.frame_flags|=sbps::kFlagError;result.diagnostics.push_back(sbps::IpcDiagnostic(std::move(c),"parser_server_ipc.ddl_alter_rewrite_rule_refused","DDL alter rewrite rule coordination was refused.",{{"detail",std::move(d)}}));return result;};scratchbird::engine::sblr::SblrDdlAlterRewriteRuleRequestV1 v;std::string d;if(!registry||!scratchbird::engine::sblr::DecodeSblrDdlAlterRewriteRuleRequestV1(request.payload.data(),request.payload.size(),&v,&d))return refuse("SBLR.OPERAND_INVALID",d);auto s=registry->sessions_by_uuid.find(UuidBytesToText(request.header.session_uuid));if(s==registry->sessions_by_uuid.end())return refuse("SECURITY.ACCESS_DENIED","session_hidden");auto ru=UuidBytesToText(v.receipt);ServerStatementContextRecord*r=nullptr;{std::lock_guard<std::mutex>g(*registry->statement_context_mutex);for(auto&[x,row]:registry->statement_contexts_by_statement_uuid){(void)x;if(!row.released&&row.view.receipt_uuid==ru){r=&row;break;}}}if(!r||r->session_uuid!=request.header.session_uuid)return refuse("SECURITY.ACCESS_DENIED","ddl_alter_rewrite_rule_receipt_hidden");auto c=EngineContextForSession(s->second,engine_state,request);c.statement_uuid.canonical=ru;c.statement_metadata_snapshot_engine_owned=true;c.trace_tags.push_back("private_ddl_alter_rewrite_rule_binder");auto q=engine_api::CompileSblrDdlAlterRewriteRuleDescriptor(c,ru,v.occurrence,v.rule_occurrence,r->view.kv_structured_read_executor_availability_generation);if(!q.ok)return refuse(q.diagnostic.code,q.diagnostic.message_key);result.payload=scratchbird::engine::sblr::EncodeSblrDdlAlterRewriteRuleDescriptorV1(q.descriptor,false);if(result.payload.empty())return refuse("DDL.ALTER_REWRITE_RULE_FAILED","RWAD_encode_failed");result.accepted=true;return result;}
SessionOperationResult HandleCoordinateDdlDropRewriteRule(ServerSessionRegistry*registry,const HostedEngineState&engine_state,const sbps::Frame&request){SessionOperationResult result;result.response_message_type=229;result.response_schema_id=sbps::kSchemaCoordinateDdlDropRewriteRuleResultV1;result.frame_flags=sbps::kFlagResponse|sbps::kFlagFinal;result.session_uuid=request.header.session_uuid;auto refuse=[&](std::string c,std::string d){result.frame_flags|=sbps::kFlagError;result.diagnostics.push_back(sbps::IpcDiagnostic(std::move(c),"parser_server_ipc.ddl_drop_rewrite_rule_refused","DDL alter rewrite rule coordination was refused.",{{"detail",std::move(d)}}));return result;};scratchbird::engine::sblr::SblrDdlDropRewriteRuleRequestV1 v;std::string d;if(!registry||!scratchbird::engine::sblr::DecodeSblrDdlDropRewriteRuleRequestV1(request.payload.data(),request.payload.size(),&v,&d))return refuse("SBLR.OPERAND_INVALID",d);auto s=registry->sessions_by_uuid.find(UuidBytesToText(request.header.session_uuid));if(s==registry->sessions_by_uuid.end())return refuse("SECURITY.ACCESS_DENIED","session_hidden");auto ru=UuidBytesToText(v.receipt);ServerStatementContextRecord*r=nullptr;{std::lock_guard<std::mutex>g(*registry->statement_context_mutex);for(auto&[x,row]:registry->statement_contexts_by_statement_uuid){(void)x;if(!row.released&&row.view.receipt_uuid==ru){r=&row;break;}}}if(!r||r->session_uuid!=request.header.session_uuid)return refuse("SECURITY.ACCESS_DENIED","ddl_drop_rewrite_rule_receipt_hidden");auto c=EngineContextForSession(s->second,engine_state,request);c.statement_uuid.canonical=ru;c.statement_metadata_snapshot_engine_owned=true;c.trace_tags.push_back("private_ddl_drop_rewrite_rule_binder");auto q=engine_api::CompileSblrDdlDropRewriteRuleDescriptor(c,ru,v.occurrence,v.rule_occurrence,r->view.kv_structured_read_executor_availability_generation);if(!q.ok)return refuse(q.diagnostic.code,q.diagnostic.message_key);result.payload=scratchbird::engine::sblr::EncodeSblrDdlDropRewriteRuleDescriptorV1(q.descriptor,false);if(result.payload.empty())return refuse("DDL.DROP_REWRITE_RULE_FAILED","RWDD_encode_failed");result.accepted=true;return result;}
SessionOperationResult HandleCoordinateDdlValidateConstraint(ServerSessionRegistry*registry,const HostedEngineState&engine_state,const sbps::Frame&request){SessionOperationResult result;result.response_message_type=231;result.response_schema_id=sbps::kSchemaCoordinateDdlValidateConstraintResultV1;result.frame_flags=sbps::kFlagResponse|sbps::kFlagFinal;result.session_uuid=request.header.session_uuid;auto refuse=[&](std::string c,std::string d){result.frame_flags|=sbps::kFlagError;result.diagnostics.push_back(sbps::IpcDiagnostic(std::move(c),"parser_server_ipc.ddl_validate_constraint_refused","DDL constraint validation coordination was refused.",{{"detail",std::move(d)}}));return result;};scratchbird::engine::sblr::SblrDdlValidateConstraintRequestV1 v;std::string d;if(!registry||!scratchbird::engine::sblr::DecodeSblrDdlValidateConstraintRequestV1(request.payload.data(),request.payload.size(),&v,&d))return refuse("SBLR.OPERAND_INVALID",d);auto s=registry->sessions_by_uuid.find(UuidBytesToText(request.header.session_uuid));if(s==registry->sessions_by_uuid.end())return refuse("SECURITY.ACCESS_DENIED","session_hidden");auto ru=UuidBytesToText(v.receipt);ServerStatementContextRecord*r=nullptr;{std::lock_guard<std::mutex>g(*registry->statement_context_mutex);for(auto&[x,row]:registry->statement_contexts_by_statement_uuid){(void)x;if(!row.released&&row.view.receipt_uuid==ru){r=&row;break;}}}if(!r||r->session_uuid!=request.header.session_uuid)return refuse("SECURITY.ACCESS_DENIED","ddl_validate_constraint_receipt_hidden");auto c=EngineContextForSession(s->second,engine_state,request);c.statement_uuid.canonical=ru;c.statement_metadata_snapshot_engine_owned=true;c.trace_tags.push_back("private_ddl_validate_constraint_binder");auto q=engine_api::CompileSblrDdlValidateConstraintDescriptor(c,ru,v.occurrence,v.constraint_occurrence,r->view.kv_structured_read_executor_availability_generation);if(!q.ok)return refuse(q.diagnostic.code,q.diagnostic.message_key);result.payload=scratchbird::engine::sblr::EncodeSblrDdlValidateConstraintDescriptorV1(q.descriptor,false);if(result.payload.empty())return refuse("DDL.VALIDATE_CONSTRAINT_FAILED","CVLD_encode_failed");result.accepted=true;return result;}
SessionOperationResult HandleCoordinateSecurityCreatePrivilegeTemplate(ServerSessionRegistry*registry,const HostedEngineState&engine_state,const sbps::Frame&request){SessionOperationResult result;result.response_message_type=233;result.response_schema_id=sbps::kSchemaCoordinateSecurityCreatePrivilegeTemplateResultV1;result.frame_flags=sbps::kFlagResponse|sbps::kFlagFinal;result.session_uuid=request.header.session_uuid;auto refuse=[&](std::string c,std::string d){result.frame_flags|=sbps::kFlagError;result.diagnostics.push_back(sbps::IpcDiagnostic(std::move(c),"parser_server_ipc.security_create_privilege_template_refused","Privilege template coordination was refused.",{{"detail",std::move(d)}}));return result;};scratchbird::engine::sblr::SblrSecurityCreatePrivilegeTemplateRequestV1 v;std::string d;if(!registry||!scratchbird::engine::sblr::DecodeSblrSecurityCreatePrivilegeTemplateRequestV1(request.payload.data(),request.payload.size(),&v,&d))return refuse("SBLR.OPERAND.INVALID",d);auto s=registry->sessions_by_uuid.find(UuidBytesToText(request.header.session_uuid));if(s==registry->sessions_by_uuid.end())return refuse("SECURITY.ACCESS_DENIED","session_hidden");auto ru=UuidBytesToText(v.receipt);ServerStatementContextRecord*r=nullptr;{std::lock_guard<std::mutex>g(*registry->statement_context_mutex);for(auto&[x,row]:registry->statement_contexts_by_statement_uuid){(void)x;if(!row.released&&row.view.receipt_uuid==ru){r=&row;break;}}}if(!r||r->session_uuid!=request.header.session_uuid)return refuse("SECURITY.ACCESS_DENIED","privilege_template_receipt_hidden");auto c=EngineContextForSession(s->second,engine_state,request);c.statement_uuid.canonical=ru;c.statement_metadata_snapshot_engine_owned=true;c.trace_tags.push_back("private_security_create_privilege_template_binder");auto q=engine_api::CompileSblrSecurityCreatePrivilegeTemplateDescriptor(c,ru,v.occurrence,v.template_occurrence,r->view.kv_structured_read_executor_availability_generation);if(!q.ok)return refuse(q.diagnostic.code,q.diagnostic.message_key);result.payload=scratchbird::engine::sblr::EncodeSblrSecurityCreatePrivilegeTemplateDescriptorV1(q.descriptor,false);if(result.payload.empty())return refuse("SECURITY.PRIVILEGE_TEMPLATE_APPLICATION_FAILED","PTDD_encode_failed");result.accepted=true;return result;}
SessionOperationResult HandleCoordinateSecurityCreateUser(ServerSessionRegistry*registry,const HostedEngineState&engine_state,const sbps::Frame&request){SessionOperationResult result;result.response_message_type=331;result.response_schema_id=sbps::kSchemaCoordinateSecurityCreateUserResultV1;result.frame_flags=sbps::kFlagResponse|sbps::kFlagFinal;result.session_uuid=request.header.session_uuid;auto refuse=[&](std::string c,std::string d){result.frame_flags|=sbps::kFlagError;result.diagnostics.push_back(sbps::IpcDiagnostic(std::move(c),"parser_server_ipc.security_create_user_refused","User creation coordination was refused.",{{"detail",std::move(d)}}));return result;};scratchbird::engine::sblr::SblrSecurityCreateUserRequestV1 v;std::string d;if(!registry||!scratchbird::engine::sblr::DecodeSblrSecurityCreateUserRequestV1(request.payload.data(),request.payload.size(),&v,&d))return refuse("SBLR.OPERAND.INVALID",d);auto s=registry->sessions_by_uuid.find(UuidBytesToText(request.header.session_uuid));if(s==registry->sessions_by_uuid.end())return refuse("SECURITY.ACCESS_DENIED","session_hidden");auto ru=UuidBytesToText(v.receipt);ServerStatementContextRecord*r=nullptr;{std::lock_guard<std::mutex>g(*registry->statement_context_mutex);for(auto&[x,row]:registry->statement_contexts_by_statement_uuid){(void)x;if(!row.released&&row.view.receipt_uuid==ru){r=&row;break;}}}if(!r||r->session_uuid!=request.header.session_uuid)return refuse("SECURITY.ACCESS_DENIED","user_receipt_hidden");auto c=EngineContextForSession(s->second,engine_state,request);c.statement_uuid.canonical=ru;c.statement_metadata_snapshot_engine_owned=true;c.trace_tags.push_back("private_security_create_user_binder");auto q=engine_api::CompileSblrSecurityCreateUserDescriptor(c,ru,v.occurrence,v.template_occurrence,r->view.kv_structured_read_executor_availability_generation);if(!q.ok)return refuse(q.diagnostic.code,q.diagnostic.message_key);result.payload=scratchbird::engine::sblr::EncodeSblrSecurityCreateUserDescriptorV1(q.descriptor,false);if(result.payload.empty())return refuse("SECURITY.USER_APPLICATION_FAILED","SCUD_encode_failed");result.accepted=true;return result;}
SessionOperationResult HandleCoordinateSecurityAlterPrivilegeTemplate(ServerSessionRegistry*registry,const HostedEngineState&engine_state,const sbps::Frame&request){SessionOperationResult result;result.response_message_type=235;result.response_schema_id=sbps::kSchemaCoordinateSecurityAlterPrivilegeTemplateResultV1;result.frame_flags=sbps::kFlagResponse|sbps::kFlagFinal;result.session_uuid=request.header.session_uuid;auto refuse=[&](std::string c,std::string d){result.frame_flags|=sbps::kFlagError;result.diagnostics.push_back(sbps::IpcDiagnostic(std::move(c),"parser_server_ipc.security_alter_privilege_template_refused","Privilege template coordination was refused.",{{"detail",std::move(d)}}));return result;};scratchbird::engine::sblr::SblrSecurityAlterPrivilegeTemplateRequestV1 v;std::string d;if(!registry||!scratchbird::engine::sblr::DecodeSblrSecurityAlterPrivilegeTemplateRequestV1(request.payload.data(),request.payload.size(),&v,&d))return refuse("SBLR.OPERAND.INVALID",d);auto s=registry->sessions_by_uuid.find(UuidBytesToText(request.header.session_uuid));if(s==registry->sessions_by_uuid.end())return refuse("SECURITY.ACCESS_DENIED","session_hidden");auto ru=UuidBytesToText(v.receipt);ServerStatementContextRecord*r=nullptr;{std::lock_guard<std::mutex>g(*registry->statement_context_mutex);for(auto&[x,row]:registry->statement_contexts_by_statement_uuid){(void)x;if(!row.released&&row.view.receipt_uuid==ru){r=&row;break;}}}if(!r||r->session_uuid!=request.header.session_uuid)return refuse("SECURITY.ACCESS_DENIED","privilege_template_receipt_hidden");auto c=EngineContextForSession(s->second,engine_state,request);c.statement_uuid.canonical=ru;c.statement_metadata_snapshot_engine_owned=true;c.trace_tags.push_back("private_security_alter_privilege_template_binder");auto q=engine_api::CompileSblrSecurityAlterPrivilegeTemplateDescriptor(c,ru,v.occurrence,v.template_occurrence,r->view.kv_structured_read_executor_availability_generation);if(!q.ok)return refuse(q.diagnostic.code,q.diagnostic.message_key);result.payload=scratchbird::engine::sblr::EncodeSblrSecurityAlterPrivilegeTemplateDescriptorV1(q.descriptor,false);if(result.payload.empty())return refuse("SECURITY.PRIVILEGE_TEMPLATE_APPLICATION_FAILED","PTDD_encode_failed");result.accepted=true;return result;}
SessionOperationResult HandleCoordinateSecurityDropPrivilegeTemplate(ServerSessionRegistry*registry,const HostedEngineState&engine_state,const sbps::Frame&request){SessionOperationResult result;result.response_message_type=237;result.response_schema_id=sbps::kSchemaCoordinateSecurityDropPrivilegeTemplateResultV1;result.frame_flags=sbps::kFlagResponse|sbps::kFlagFinal;result.session_uuid=request.header.session_uuid;auto refuse=[&](std::string c,std::string d){result.frame_flags|=sbps::kFlagError;result.diagnostics.push_back(sbps::IpcDiagnostic(std::move(c),"parser_server_ipc.security_drop_privilege_template_refused","Privilege template coordination was refused.",{{"detail",std::move(d)}}));return result;};scratchbird::engine::sblr::SblrSecurityDropPrivilegeTemplateRequestV1 v;std::string d;if(!registry||!scratchbird::engine::sblr::DecodeSblrSecurityDropPrivilegeTemplateRequestV1(request.payload.data(),request.payload.size(),&v,&d))return refuse("SBLR.OPERAND.INVALID",d);auto s=registry->sessions_by_uuid.find(UuidBytesToText(request.header.session_uuid));if(s==registry->sessions_by_uuid.end())return refuse("SECURITY.ACCESS_DENIED","session_hidden");auto ru=UuidBytesToText(v.receipt);ServerStatementContextRecord*r=nullptr;{std::lock_guard<std::mutex>g(*registry->statement_context_mutex);for(auto&[x,row]:registry->statement_contexts_by_statement_uuid){(void)x;if(!row.released&&row.view.receipt_uuid==ru){r=&row;break;}}}if(!r||r->session_uuid!=request.header.session_uuid)return refuse("SECURITY.ACCESS_DENIED","privilege_template_receipt_hidden");auto c=EngineContextForSession(s->second,engine_state,request);c.statement_uuid.canonical=ru;c.statement_metadata_snapshot_engine_owned=true;c.trace_tags.push_back("private_security_drop_privilege_template_binder");auto q=engine_api::CompileSblrSecurityDropPrivilegeTemplateDescriptor(c,ru,v.occurrence,v.template_occurrence,r->view.kv_structured_read_executor_availability_generation);if(!q.ok)return refuse(q.diagnostic.code,q.diagnostic.message_key);result.payload=scratchbird::engine::sblr::EncodeSblrSecurityDropPrivilegeTemplateDescriptorV1(q.descriptor,false);if(result.payload.empty())return refuse("SECURITY.PRIVILEGE_TEMPLATE_APPLICATION_FAILED","PTDD_encode_failed");result.accepted=true;return result;}
SessionOperationResult HandleCoordinateDatabaseCreateTemplateClone(ServerSessionRegistry*registry,const HostedEngineState&engine_state,const sbps::Frame&request){SessionOperationResult result;result.response_message_type=239;result.response_schema_id=sbps::kSchemaCoordinateDatabaseCreateTemplateCloneResultV1;result.frame_flags=sbps::kFlagResponse|sbps::kFlagFinal;result.session_uuid=request.header.session_uuid;auto refuse=[&](std::string c,std::string d){result.frame_flags|=sbps::kFlagError;result.diagnostics.push_back(sbps::IpcDiagnostic(std::move(c),"parser_server_ipc.database_create_template_clone_refused","Template database clone coordination was refused.",{{"detail",std::move(d)}}));return result;};scratchbird::engine::sblr::SblrDatabaseCreateTemplateCloneRequestV1 v;std::string d;if(!registry||!scratchbird::engine::sblr::DecodeSblrDatabaseCreateTemplateCloneRequestV1(request.payload.data(),request.payload.size(),&v,&d))return refuse("SBLR.OPERAND.INVALID",d);auto s=registry->sessions_by_uuid.find(UuidBytesToText(request.header.session_uuid));if(s==registry->sessions_by_uuid.end())return refuse("SECURITY.ACCESS_DENIED","session_hidden");auto ru=UuidBytesToText(v.receipt);ServerStatementContextRecord*r=nullptr;{std::lock_guard<std::mutex>g(*registry->statement_context_mutex);for(auto&[x,row]:registry->statement_contexts_by_statement_uuid){(void)x;if(!row.released&&row.view.receipt_uuid==ru){r=&row;break;}}}if(!r||r->session_uuid!=request.header.session_uuid)return refuse("SECURITY.ACCESS_DENIED","template_clone_receipt_hidden");auto c=EngineContextForSession(s->second,engine_state,request);c.statement_uuid.canonical=ru;c.statement_metadata_snapshot_engine_owned=true;c.trace_tags.push_back("private_database_create_template_clone_binder");auto q=engine_api::CompileSblrDatabaseCreateTemplateCloneDescriptor(c,ru,v.occurrence,v.template_clone_occurrence,r->view.kv_structured_read_executor_availability_generation);if(!q.ok)return refuse(q.diagnostic.code,q.diagnostic.message_key);result.payload=scratchbird::engine::sblr::EncodeSblrDatabaseCreateTemplateCloneDescriptorV1(q.descriptor,false);if(result.payload.empty())return refuse("DATABASE.TEMPLATE_CLONE_FAILED","TCDD_encode_failed");result.accepted=true;return result;}
SessionOperationResult HandleCoordinateDdlCreateAggregate(ServerSessionRegistry*registry,const HostedEngineState&engine_state,const sbps::Frame&request){SessionOperationResult result;result.response_message_type=241;result.response_schema_id=sbps::kSchemaCoordinateDdlCreateAggregateResultV1;result.frame_flags=sbps::kFlagResponse|sbps::kFlagFinal;result.session_uuid=request.header.session_uuid;auto refuse=[&](std::string c,std::string d){result.frame_flags|=sbps::kFlagError;result.diagnostics.push_back(sbps::IpcDiagnostic(std::move(c),"parser_server_ipc.ddl_create_aggregate_refused","Aggregate coordination was refused.",{{"detail",std::move(d)}}));return result;};scratchbird::engine::sblr::SblrDdlCreateAggregateRequestV1 v;std::string d;if(!registry||!scratchbird::engine::sblr::DecodeSblrDdlCreateAggregateRequestV1(request.payload.data(),request.payload.size(),&v,&d))return refuse("SBLR.OPERAND.INVALID",d);auto s=registry->sessions_by_uuid.find(UuidBytesToText(request.header.session_uuid));if(s==registry->sessions_by_uuid.end())return refuse("SECURITY.ACCESS_DENIED","session_hidden");auto ru=UuidBytesToText(v.receipt);ServerStatementContextRecord*r=nullptr;{std::lock_guard<std::mutex>g(*registry->statement_context_mutex);for(auto&[x,row]:registry->statement_contexts_by_statement_uuid){(void)x;if(!row.released&&row.view.receipt_uuid==ru){r=&row;break;}}}if(!r||r->session_uuid!=request.header.session_uuid)return refuse("SECURITY.ACCESS_DENIED","aggregate_receipt_hidden");auto c=EngineContextForSession(s->second,engine_state,request);c.statement_uuid.canonical=ru;c.statement_metadata_snapshot_engine_owned=true;c.trace_tags.push_back("private_ddl_create_aggregate_binder");auto q=engine_api::CompileSblrDdlCreateAggregateDescriptor(c,ru,v.occurrence,v.aggregate_occurrence,r->view.kv_structured_read_executor_availability_generation);if(!q.ok)return refuse(q.diagnostic.code,q.diagnostic.message_key);result.payload=scratchbird::engine::sblr::EncodeSblrDdlCreateAggregateDescriptorV1(q.descriptor,false);if(result.payload.empty())return refuse("DDL.AGGREGATE_CREATE_FAILED","ACDD_encode_failed");result.accepted=true;return result;}
SessionOperationResult HandleCoordinateDdlAlterAggregate(ServerSessionRegistry*registry,const HostedEngineState&engine_state,const sbps::Frame&request){SessionOperationResult result;result.response_message_type=243;result.response_schema_id=sbps::kSchemaCoordinateDdlAlterAggregateResultV1;result.frame_flags=sbps::kFlagResponse|sbps::kFlagFinal;result.session_uuid=request.header.session_uuid;auto refuse=[&](std::string c,std::string d){result.frame_flags|=sbps::kFlagError;result.diagnostics.push_back(sbps::IpcDiagnostic(std::move(c),"parser_server_ipc.ddl_alter_aggregate_refused","Aggregate coordination was refused.",{{"detail",std::move(d)}}));return result;};scratchbird::engine::sblr::SblrDdlAlterAggregateRequestV1 v;std::string d;if(!registry||!scratchbird::engine::sblr::DecodeSblrDdlAlterAggregateRequestV1(request.payload.data(),request.payload.size(),&v,&d))return refuse("SBLR.OPERAND.INVALID",d);auto s=registry->sessions_by_uuid.find(UuidBytesToText(request.header.session_uuid));if(s==registry->sessions_by_uuid.end())return refuse("SECURITY.ACCESS_DENIED","session_hidden");auto ru=UuidBytesToText(v.receipt);ServerStatementContextRecord*r=nullptr;{std::lock_guard<std::mutex>g(*registry->statement_context_mutex);for(auto&[x,row]:registry->statement_contexts_by_statement_uuid){(void)x;if(!row.released&&row.view.receipt_uuid==ru){r=&row;break;}}}if(!r||r->session_uuid!=request.header.session_uuid)return refuse("SECURITY.ACCESS_DENIED","aggregate_receipt_hidden");auto c=EngineContextForSession(s->second,engine_state,request);c.statement_uuid.canonical=ru;c.statement_metadata_snapshot_engine_owned=true;c.trace_tags.push_back("private_ddl_alter_aggregate_binder");auto q=engine_api::CompileSblrDdlAlterAggregateDescriptor(c,ru,v.occurrence,v.aggregate_occurrence,r->view.kv_structured_read_executor_availability_generation);if(!q.ok)return refuse(q.diagnostic.code,q.diagnostic.message_key);result.payload=scratchbird::engine::sblr::EncodeSblrDdlAlterAggregateDescriptorV1(q.descriptor,false);if(result.payload.empty())return refuse("DDL.AGGREGATE_ALTER_FAILED","ACDD_encode_failed");result.accepted=true;return result;}
SessionOperationResult HandleCoordinateDdlDropAggregate(ServerSessionRegistry*registry,const HostedEngineState&engine_state,const sbps::Frame&request){SessionOperationResult result;result.response_message_type=245;result.response_schema_id=sbps::kSchemaCoordinateDdlDropAggregateResultV1;result.frame_flags=sbps::kFlagResponse|sbps::kFlagFinal;result.session_uuid=request.header.session_uuid;auto refuse=[&](std::string c,std::string d){result.frame_flags|=sbps::kFlagError;result.diagnostics.push_back(sbps::IpcDiagnostic(std::move(c),"parser_server_ipc.ddl_drop_aggregate_refused","Aggregate coordination was refused.",{{"detail",std::move(d)}}));return result;};scratchbird::engine::sblr::SblrDdlDropAggregateRequestV1 v;std::string d;if(!registry||!scratchbird::engine::sblr::DecodeSblrDdlDropAggregateRequestV1(request.payload.data(),request.payload.size(),&v,&d))return refuse("SBLR.OPERAND.INVALID",d);auto s=registry->sessions_by_uuid.find(UuidBytesToText(request.header.session_uuid));if(s==registry->sessions_by_uuid.end())return refuse("SECURITY.ACCESS_DENIED","session_hidden");auto ru=UuidBytesToText(v.receipt);ServerStatementContextRecord*r=nullptr;{std::lock_guard<std::mutex>g(*registry->statement_context_mutex);for(auto&[x,row]:registry->statement_contexts_by_statement_uuid){(void)x;if(!row.released&&row.view.receipt_uuid==ru){r=&row;break;}}}if(!r||r->session_uuid!=request.header.session_uuid)return refuse("SECURITY.ACCESS_DENIED","aggregate_receipt_hidden");auto c=EngineContextForSession(s->second,engine_state,request);c.statement_uuid.canonical=ru;c.statement_metadata_snapshot_engine_owned=true;c.trace_tags.push_back("private_ddl_drop_aggregate_binder");auto q=engine_api::CompileSblrDdlDropAggregateDescriptor(c,ru,v.occurrence,v.aggregate_occurrence,r->view.kv_structured_read_executor_availability_generation);if(!q.ok)return refuse(q.diagnostic.code,q.diagnostic.message_key);result.payload=scratchbird::engine::sblr::EncodeSblrDdlDropAggregateDescriptorV1(q.descriptor,false);if(result.payload.empty())return refuse("DDL.AGGREGATE_DROP_FAILED","ADDD_encode_failed");result.accepted=true;return result;}
SessionOperationResult HandleCoordinateDdlPurgeSystemHistory(ServerSessionRegistry*registry,const HostedEngineState&engine_state,const sbps::Frame&request){SessionOperationResult result;result.response_message_type=247;result.response_schema_id=sbps::kSchemaCoordinateDdlPurgeSystemHistoryResultV1;result.frame_flags=sbps::kFlagResponse|sbps::kFlagFinal;result.session_uuid=request.header.session_uuid;auto refuse=[&](std::string c,std::string d){result.frame_flags|=sbps::kFlagError;result.diagnostics.push_back(sbps::IpcDiagnostic(std::move(c),"parser_server_ipc.ddl_purge_system_history_refused","System history purge coordination was refused.",{{"detail",std::move(d)}}));return result;};scratchbird::engine::sblr::SblrDdlPurgeSystemHistoryRequestV1 v;std::string d;if(!registry||!scratchbird::engine::sblr::DecodeSblrDdlPurgeSystemHistoryRequestV1(request.payload.data(),request.payload.size(),&v,&d))return refuse("SBLR.OPERAND.INVALID",d);auto s=registry->sessions_by_uuid.find(UuidBytesToText(request.header.session_uuid));if(s==registry->sessions_by_uuid.end())return refuse("SECURITY.ACCESS_DENIED","session_hidden");auto ru=UuidBytesToText(v.receipt);ServerStatementContextRecord*r=nullptr;{std::lock_guard<std::mutex>g(*registry->statement_context_mutex);for(auto&[x,row]:registry->statement_contexts_by_statement_uuid){(void)x;if(!row.released&&row.view.receipt_uuid==ru){r=&row;break;}}}if(!r||r->session_uuid!=request.header.session_uuid)return refuse("SECURITY.ACCESS_DENIED","history_receipt_hidden");auto c=EngineContextForSession(s->second,engine_state,request);c.statement_uuid.canonical=ru;c.statement_metadata_snapshot_engine_owned=true;c.trace_tags.push_back("private_ddl_purge_system_history_binder");auto q=engine_api::CompileSblrDdlPurgeSystemHistoryDescriptor(c,ru,v.occurrence,v.purge_occurrence,r->view.kv_structured_read_executor_availability_generation);if(!q.ok)return refuse(q.diagnostic.code,q.diagnostic.message_key);result.payload=scratchbird::engine::sblr::EncodeSblrDdlPurgeSystemHistoryDescriptorV1(q.descriptor,false);if(result.payload.empty())return refuse("DDL.PURGE_SYSTEM_HISTORY_FAILED","PHDD_encode_failed");result.accepted=true;return result;}
SessionOperationResult HandleCoordinateDdlSetIndexOptimizerEligibility(ServerSessionRegistry*registry,const HostedEngineState&engine_state,const sbps::Frame&request){SessionOperationResult result;result.response_message_type=249;result.response_schema_id=sbps::kSchemaCoordinateDdlSetIndexOptimizerEligibilityResultV1;result.frame_flags=sbps::kFlagResponse|sbps::kFlagFinal;result.session_uuid=request.header.session_uuid;auto refuse=[&](std::string c,std::string d){result.frame_flags|=sbps::kFlagError;result.diagnostics.push_back(sbps::IpcDiagnostic(std::move(c),"parser_server_ipc.ddl_set_index_optimizer_eligibility_refused","Index optimizer eligibility coordination was refused.",{{"detail",std::move(d)}}));return result;};scratchbird::engine::sblr::SblrDdlSetIndexOptimizerEligibilityRequestV1 v;std::string d;if(!registry||!scratchbird::engine::sblr::DecodeSblrDdlSetIndexOptimizerEligibilityRequestV1(request.payload.data(),request.payload.size(),&v,&d))return refuse("SBLR.OPERAND.INVALID",d);auto s=registry->sessions_by_uuid.find(UuidBytesToText(request.header.session_uuid));if(s==registry->sessions_by_uuid.end())return refuse("SECURITY.ACCESS_DENIED","session_hidden");auto ru=UuidBytesToText(v.receipt);ServerStatementContextRecord*r=nullptr;{std::lock_guard<std::mutex>g(*registry->statement_context_mutex);for(auto&[x,row]:registry->statement_contexts_by_statement_uuid){(void)x;if(!row.released&&row.view.receipt_uuid==ru){r=&row;break;}}}if(!r||r->session_uuid!=request.header.session_uuid)return refuse("SECURITY.ACCESS_DENIED","index_receipt_hidden");auto c=EngineContextForSession(s->second,engine_state,request);c.statement_uuid.canonical=ru;c.statement_metadata_snapshot_engine_owned=true;c.trace_tags.push_back("private_ddl_set_index_optimizer_eligibility_binder");auto q=engine_api::CompileSblrDdlSetIndexOptimizerEligibilityDescriptor(c,ru,v.occurrence,v.index_occurrence,r->view.kv_structured_read_executor_availability_generation);if(!q.ok)return refuse(q.diagnostic.code,q.diagnostic.message_key);result.payload=scratchbird::engine::sblr::EncodeSblrDdlSetIndexOptimizerEligibilityDescriptorV1(q.descriptor,false);if(result.payload.empty())return refuse("DDL.SET_INDEX_OPTIMIZER_ELIGIBILITY_FAILED","IOED_encode_failed");result.accepted=true;return result;}
SessionOperationResult HandleCoordinateDdlSetTableTypeEnforcement(ServerSessionRegistry*registry,const HostedEngineState&engine_state,const sbps::Frame&request){SessionOperationResult result;result.response_message_type=251;result.response_schema_id=sbps::kSchemaCoordinateDdlSetTableTypeEnforcementResultV1;result.frame_flags=sbps::kFlagResponse|sbps::kFlagFinal;result.session_uuid=request.header.session_uuid;auto refuse=[&](std::string c,std::string d){result.frame_flags|=sbps::kFlagError;result.diagnostics.push_back(sbps::IpcDiagnostic(std::move(c),"parser_server_ipc.ddl_set_table_type_enforcement_refused","Table type enforcement coordination was refused.",{{"detail",std::move(d)}}));return result;};scratchbird::engine::sblr::SblrDdlSetTableTypeEnforcementRequestV1 v;std::string d;if(!registry||!scratchbird::engine::sblr::DecodeSblrDdlSetTableTypeEnforcementRequestV1(request.payload.data(),request.payload.size(),&v,&d))return refuse("SBLR.OPERAND.INVALID",d);auto s=registry->sessions_by_uuid.find(UuidBytesToText(request.header.session_uuid));if(s==registry->sessions_by_uuid.end())return refuse("SECURITY.ACCESS_DENIED","session_hidden");auto ru=UuidBytesToText(v.receipt);ServerStatementContextRecord*r=nullptr;{std::lock_guard<std::mutex>g(*registry->statement_context_mutex);for(auto&[x,row]:registry->statement_contexts_by_statement_uuid){(void)x;if(!row.released&&row.view.receipt_uuid==ru){r=&row;break;}}}if(!r||r->session_uuid!=request.header.session_uuid)return refuse("SECURITY.ACCESS_DENIED","table_receipt_hidden");auto c=EngineContextForSession(s->second,engine_state,request);c.statement_uuid.canonical=ru;c.statement_metadata_snapshot_engine_owned=true;c.trace_tags.push_back("private_ddl_set_table_type_enforcement_binder");auto q=engine_api::CompileSblrDdlSetTableTypeEnforcementDescriptor(c,ru,v.occurrence,v.table_occurrence,r->view.kv_structured_read_executor_availability_generation);if(!q.ok)return refuse(q.diagnostic.code,q.diagnostic.message_key);result.payload=scratchbird::engine::sblr::EncodeSblrDdlSetTableTypeEnforcementDescriptorV1(q.descriptor,false);if(result.payload.empty())return refuse("DDL.SET_TABLE_TYPE_ENFORCEMENT_FAILED","TTED_encode_failed");result.accepted=true;return result;}
SessionOperationResult HandleCoordinateDatabaseSerializeLogicalSnapshot(ServerSessionRegistry*registry,const HostedEngineState&engine_state,const sbps::Frame&request){SessionOperationResult result;result.response_message_type=253;result.response_schema_id=sbps::kSchemaCoordinateDatabaseSerializeLogicalSnapshotResultV1;result.frame_flags=sbps::kFlagResponse|sbps::kFlagFinal;result.session_uuid=request.header.session_uuid;auto refuse=[&](std::string c,std::string d){result.frame_flags|=sbps::kFlagError;result.diagnostics.push_back(sbps::IpcDiagnostic(std::move(c),"parser_server_ipc.database_serialize_logical_snapshot_refused","Logical snapshot serialization coordination was refused.",{{"detail",std::move(d)}}));return result;};scratchbird::engine::sblr::SblrDatabaseSerializeLogicalSnapshotRequestV1 v;std::string d;if(!registry||!scratchbird::engine::sblr::DecodeSblrDatabaseSerializeLogicalSnapshotRequestV1(request.payload.data(),request.payload.size(),&v,&d))return refuse("SBLR.OPERAND.INVALID",d);auto s=registry->sessions_by_uuid.find(UuidBytesToText(request.header.session_uuid));if(s==registry->sessions_by_uuid.end())return refuse("SECURITY.ACCESS_DENIED","session_hidden");auto ru=UuidBytesToText(v.receipt);ServerStatementContextRecord*r=nullptr;{std::lock_guard<std::mutex>g(*registry->statement_context_mutex);for(auto&[x,row]:registry->statement_contexts_by_statement_uuid){(void)x;if(!row.released&&row.view.receipt_uuid==ru){r=&row;break;}}}if(!r||r->session_uuid!=request.header.session_uuid)return refuse("SECURITY.ACCESS_DENIED","snapshot_receipt_hidden");auto c=EngineContextForSession(s->second,engine_state,request);c.statement_uuid.canonical=ru;c.statement_metadata_snapshot_engine_owned=true;c.trace_tags.push_back("private_database_serialize_logical_snapshot_binder");auto q=engine_api::CompileSblrDatabaseSerializeLogicalSnapshotDescriptor(c,ru,v.occurrence,v.snapshot_occurrence,r->view.kv_structured_read_executor_availability_generation);if(!q.ok)return refuse(q.diagnostic.code,q.diagnostic.message_key);result.payload=scratchbird::engine::sblr::EncodeSblrDatabaseSerializeLogicalSnapshotDescriptorV1(q.descriptor,false);if(result.payload.empty())return refuse("DATABASE.SERIALIZE_LOGICAL_SNAPSHOT_FAILED","LSDD_encode_failed");result.accepted=true;return result;}
SessionOperationResult HandleCoordinateDatabaseDeserializeLogicalSnapshot(ServerSessionRegistry*registry,const HostedEngineState&engine_state,const sbps::Frame&request){SessionOperationResult result;result.response_message_type=255;result.response_schema_id=sbps::kSchemaCoordinateDatabaseDeserializeLogicalSnapshotResultV1;result.frame_flags=sbps::kFlagResponse|sbps::kFlagFinal;result.session_uuid=request.header.session_uuid;auto refuse=[&](std::string c,std::string d){result.frame_flags|=sbps::kFlagError;result.diagnostics.push_back(sbps::IpcDiagnostic(std::move(c),"parser_server_ipc.database_deserialize_logical_snapshot_refused","Logical snapshot deserialization coordination was refused.",{{"detail",std::move(d)}}));return result;};scratchbird::engine::sblr::SblrDatabaseDeserializeLogicalSnapshotRequestV1 v;std::string d;if(!registry||!scratchbird::engine::sblr::DecodeSblrDatabaseDeserializeLogicalSnapshotRequestV1(request.payload.data(),request.payload.size(),&v,&d))return refuse("SBLR.OPERAND.INVALID",d);auto s=registry->sessions_by_uuid.find(UuidBytesToText(request.header.session_uuid));if(s==registry->sessions_by_uuid.end())return refuse("SECURITY.ACCESS_DENIED","session_hidden");auto ru=UuidBytesToText(v.receipt);ServerStatementContextRecord*r=nullptr;{std::lock_guard<std::mutex>g(*registry->statement_context_mutex);for(auto&[x,row]:registry->statement_contexts_by_statement_uuid){(void)x;if(!row.released&&row.view.receipt_uuid==ru){r=&row;break;}}}if(!r||r->session_uuid!=request.header.session_uuid)return refuse("SECURITY.ACCESS_DENIED","snapshot_receipt_hidden");auto c=EngineContextForSession(s->second,engine_state,request);c.statement_uuid.canonical=ru;c.statement_metadata_snapshot_engine_owned=true;c.trace_tags.push_back("private_database_deserialize_logical_snapshot_binder");auto q=engine_api::CompileSblrDatabaseDeserializeLogicalSnapshotDescriptor(c,ru,v.occurrence,v.snapshot_occurrence,r->view.kv_structured_read_executor_availability_generation);if(!q.ok)return refuse(q.diagnostic.code,q.diagnostic.message_key);result.payload=scratchbird::engine::sblr::EncodeSblrDatabaseDeserializeLogicalSnapshotDescriptorV1(q.descriptor,false);if(result.payload.empty())return refuse("DATABASE.DESERIALIZE_LOGICAL_SNAPSHOT_FAILED","LDDD_encode_failed");result.accepted=true;return result;}
SessionOperationResult HandleCoordinateDdlCreateMacro(ServerSessionRegistry*registry,const HostedEngineState&engine_state,const sbps::Frame&request){SessionOperationResult result;result.response_message_type=257;result.response_schema_id=sbps::kSchemaCoordinateDdlCreateMacroResultV1;result.frame_flags=sbps::kFlagResponse|sbps::kFlagFinal;result.session_uuid=request.header.session_uuid;auto refuse=[&](std::string c,std::string d){result.frame_flags|=sbps::kFlagError;result.diagnostics.push_back(sbps::IpcDiagnostic(std::move(c),"parser_server_ipc.ddl_create_macro_refused","Macro coordination was refused.",{{"detail",std::move(d)}}));return result;};scratchbird::engine::sblr::SblrDdlCreateMacroRequestV1 v;std::string d;if(!registry||!scratchbird::engine::sblr::DecodeSblrDdlCreateMacroRequestV1(request.payload.data(),request.payload.size(),&v,&d))return refuse("SBLR.OPERAND.INVALID",d);auto s=registry->sessions_by_uuid.find(UuidBytesToText(request.header.session_uuid));if(s==registry->sessions_by_uuid.end())return refuse("SECURITY.ACCESS_DENIED","session_hidden");auto ru=UuidBytesToText(v.receipt);auto c=EngineContextForSession(s->second,engine_state,request);c.statement_uuid.canonical=ru;c.statement_metadata_snapshot_engine_owned=true;c.trace_tags.push_back("private_ddl_create_macro_binder");auto q=scratchbird::engine::internal_api::CompileSblrDdlCreateMacroDescriptor(c,ru,v.occurrence,v.macro_occurrence,1);if(!q.ok)return refuse(q.diagnostic.code,q.diagnostic.message_key);result.payload=scratchbird::engine::sblr::EncodeSblrDdlCreateMacroDescriptorV1(q.descriptor,false);if(result.payload.empty())return refuse("DDL.CREATE_MACRO_FAILED","MCDD_encode_failed");result.accepted=true;return result;}
SessionOperationResult HandleCoordinateDdlDropMacro(ServerSessionRegistry*registry,const HostedEngineState&engine_state,const sbps::Frame&request){SessionOperationResult result;result.response_message_type=259;result.response_schema_id=sbps::kSchemaCoordinateDdlDropMacroResultV1;result.frame_flags=sbps::kFlagResponse|sbps::kFlagFinal;result.session_uuid=request.header.session_uuid;auto refuse=[&](std::string c,std::string d){result.frame_flags|=sbps::kFlagError;result.diagnostics.push_back(sbps::IpcDiagnostic(std::move(c),"parser_server_ipc.ddl_drop_macro_refused","Macro drop coordination was refused.",{{"detail",std::move(d)}}));return result;};scratchbird::engine::sblr::SblrDdlDropMacroRequestV1 v;std::string d;if(!registry||!scratchbird::engine::sblr::DecodeSblrDdlDropMacroRequestV1(request.payload.data(),request.payload.size(),&v,&d))return refuse("SBLR.OPERAND.INVALID",d);auto s=registry->sessions_by_uuid.find(UuidBytesToText(request.header.session_uuid));if(s==registry->sessions_by_uuid.end())return refuse("SECURITY.ACCESS_DENIED","session_hidden");auto ru=UuidBytesToText(v.receipt);auto c=EngineContextForSession(s->second,engine_state,request);c.statement_uuid.canonical=ru;c.statement_metadata_snapshot_engine_owned=true;c.trace_tags.push_back("private_ddl_drop_macro_binder");auto q=scratchbird::engine::internal_api::CompileSblrDdlDropMacroDescriptor(c,ru,v.occurrence,v.macro_occurrence,1);if(!q.ok)return refuse(q.diagnostic.code,q.diagnostic.message_key);result.payload=scratchbird::engine::sblr::EncodeSblrDdlDropMacroDescriptorV1(q.descriptor,false);if(result.payload.empty())return refuse("DDL.DROP_MACRO_FAILED","MDOD_encode_failed");result.accepted=true;return result;}
SessionOperationResult HandleCoordinateDdlCreateDictionary(ServerSessionRegistry*registry,const HostedEngineState&engine_state,const sbps::Frame&request){SessionOperationResult result;result.response_message_type=265;result.response_schema_id=sbps::kSchemaCoordinateDdlCreateDictionaryResultV1;result.frame_flags=sbps::kFlagResponse|sbps::kFlagFinal;result.session_uuid=request.header.session_uuid;auto refuse=[&](std::string c,std::string d){result.frame_flags|=sbps::kFlagError;result.diagnostics.push_back(sbps::IpcDiagnostic(std::move(c),"parser_server_ipc.ddl_create_dictionary_refused","Dictionary creation coordination was refused.",{{"detail",std::move(d)}}));return result;};scratchbird::engine::sblr::SblrDdlCreateDictionaryRequestV1 v;std::string d;if(!registry||!scratchbird::engine::sblr::DecodeSblrDdlCreateDictionaryRequestV1(request.payload.data(),request.payload.size(),&v,&d))return refuse("SBLR.OPERAND.INVALID",d);auto s=registry->sessions_by_uuid.find(UuidBytesToText(request.header.session_uuid));if(s==registry->sessions_by_uuid.end())return refuse("SECURITY.ACCESS_DENIED","session_hidden");auto ru=UuidBytesToText(v.receipt);auto c=EngineContextForSession(s->second,engine_state,request);c.statement_uuid.canonical=ru;c.statement_metadata_snapshot_engine_owned=true;c.trace_tags.push_back("private_ddl_create_dictionary_binder");auto q=scratchbird::engine::internal_api::CompileSblrDdlCreateDictionaryDescriptor(c,ru,v.occurrence,v.dictionary_occurrence,1);if(!q.ok)return refuse(q.diagnostic.code,q.diagnostic.message_key);result.payload=scratchbird::engine::sblr::EncodeSblrDdlCreateDictionaryDescriptorV1(q.descriptor,false);if(result.payload.empty())return refuse("DDL.CREATE_DICTIONARY_FAILED","DCDD_encode_failed");result.accepted=true;return result;}
SessionOperationResult HandleCoordinateAdminRegisterExternalRelationResolver(ServerSessionRegistry*registry,const HostedEngineState&engine_state,const sbps::Frame&request){SessionOperationResult result;result.response_message_type=261;result.response_schema_id=sbps::kSchemaCoordinateAdminRegisterExternalRelationResolverResultV1;result.frame_flags=sbps::kFlagResponse|sbps::kFlagFinal;result.session_uuid=request.header.session_uuid;auto refuse=[&](std::string c,std::string d){result.frame_flags|=sbps::kFlagError;result.diagnostics.push_back(sbps::IpcDiagnostic(std::move(c),"parser_server_ipc.external_relation_resolver_refused","External relation resolver coordination was refused.",{{"detail",std::move(d)}}));return result;};scratchbird::engine::sblr::SblrAdminRegisterExternalRelationResolverRequestV1 v;std::string d;if(!registry||!scratchbird::engine::sblr::DecodeSblrAdminRegisterExternalRelationResolverRequestV1(request.payload.data(),request.payload.size(),&v,&d))return refuse("SBLR.OPERAND.INVALID",d);auto s=registry->sessions_by_uuid.find(UuidBytesToText(request.header.session_uuid));if(s==registry->sessions_by_uuid.end())return refuse("SECURITY.ACCESS_DENIED","session_hidden");auto ru=UuidBytesToText(v.receipt);auto c=EngineContextForSession(s->second,engine_state,request);c.statement_uuid.canonical=ru;c.statement_metadata_snapshot_engine_owned=true;c.trace_tags.push_back("private_external_relation_resolver_binder");auto q=scratchbird::engine::internal_api::CompileSblrAdminRegisterExternalRelationResolverDescriptor(c,ru,v.occurrence,v.resolver_occurrence,1);if(!q.ok)return refuse(q.diagnostic.code,q.diagnostic.message_key);result.payload=scratchbird::engine::sblr::EncodeSblrAdminRegisterExternalRelationResolverDescriptorV1(q.descriptor,false);if(result.payload.empty())return refuse("ADMIN.EXTERNAL_RELATION_RESOLVER_REGISTER_FAILED","ERDD_encode_failed");result.accepted=true;return result;}
SessionOperationResult HandleCoordinateAdminUnregisterExternalRelationResolver(ServerSessionRegistry*registry,const HostedEngineState&engine_state,const sbps::Frame&request){SessionOperationResult result;result.response_message_type=263;result.response_schema_id=sbps::kSchemaCoordinateAdminUnregisterExternalRelationResolverResultV1;result.frame_flags=sbps::kFlagResponse|sbps::kFlagFinal;result.session_uuid=request.header.session_uuid;auto refuse=[&](std::string c,std::string d){result.frame_flags|=sbps::kFlagError;result.diagnostics.push_back(sbps::IpcDiagnostic(std::move(c),"parser_server_ipc.external_relation_resolver_unregister_refused","External relation resolver unregistration was refused.",{{"detail",std::move(d)}}));return result;};scratchbird::engine::sblr::SblrAdminUnregisterExternalRelationResolverRequestV1 v;std::string d;if(!registry||!scratchbird::engine::sblr::DecodeSblrAdminUnregisterExternalRelationResolverRequestV1(request.payload.data(),request.payload.size(),&v,&d))return refuse("SBLR.OPERAND.INVALID",d);auto s=registry->sessions_by_uuid.find(UuidBytesToText(request.header.session_uuid));if(s==registry->sessions_by_uuid.end())return refuse("SECURITY.ACCESS_DENIED","session_hidden");auto ru=UuidBytesToText(v.receipt);auto c=EngineContextForSession(s->second,engine_state,request);c.statement_uuid.canonical=ru;c.statement_metadata_snapshot_engine_owned=true;c.trace_tags.push_back("private_external_relation_resolver_unbinder");auto q=scratchbird::engine::internal_api::CompileSblrAdminUnregisterExternalRelationResolverDescriptor(c,ru,v.occurrence,v.resolver_occurrence,1);if(!q.ok)return refuse(q.diagnostic.code,q.diagnostic.message_key);result.payload=scratchbird::engine::sblr::EncodeSblrAdminUnregisterExternalRelationResolverDescriptorV1(q.descriptor,false);if(result.payload.empty())return refuse("ADMIN.EXTERNAL_RELATION_RESOLVER_UNREGISTER_FAILED","EUDD_encode_failed");result.accepted=true;return result;}
SessionOperationResult HandleCoordinateDdlDropDictionary(ServerSessionRegistry*registry,const HostedEngineState&engine_state,const sbps::Frame&request){SessionOperationResult result;result.response_message_type=267;result.response_schema_id=sbps::kSchemaCoordinateDdlDropDictionaryResultV1;result.frame_flags=sbps::kFlagResponse|sbps::kFlagFinal;result.session_uuid=request.header.session_uuid;auto refuse=[&](std::string c,std::string d){result.frame_flags|=sbps::kFlagError;result.diagnostics.push_back(sbps::IpcDiagnostic(std::move(c),"parser_server_ipc.ddl_drop_dictionary_refused","Dictionary drop coordination was refused.",{{"detail",std::move(d)}}));return result;};scratchbird::engine::sblr::SblrDdlDropDictionaryRequestV1 v;std::string d;if(!registry||!scratchbird::engine::sblr::DecodeSblrDdlDropDictionaryRequestV1(request.payload.data(),request.payload.size(),&v,&d))return refuse("SBLR.OPERAND.INVALID",d);auto s=registry->sessions_by_uuid.find(UuidBytesToText(request.header.session_uuid));if(s==registry->sessions_by_uuid.end())return refuse("SECURITY.ACCESS_DENIED","session_hidden");auto ru=UuidBytesToText(v.receipt);auto c=EngineContextForSession(s->second,engine_state,request);c.statement_uuid.canonical=ru;c.statement_metadata_snapshot_engine_owned=true;c.trace_tags.push_back("private_ddl_drop_dictionary_binder");auto q=scratchbird::engine::internal_api::CompileSblrDdlDropDictionaryDescriptor(c,ru,v.occurrence,v.dictionary_occurrence,1);if(!q.ok)return refuse(q.diagnostic.code,q.diagnostic.message_key);result.payload=scratchbird::engine::sblr::EncodeSblrDdlDropDictionaryDescriptorV1(q.descriptor,false);if(result.payload.empty())return refuse("DDL.DROP_DICTIONARY_FAILED","DDDD_encode_failed");result.accepted=true;return result;}
SessionOperationResult HandleCoordinateDdlAlterDictionary(ServerSessionRegistry*registry,const HostedEngineState&engine_state,const sbps::Frame&request){SessionOperationResult result;result.response_message_type=271;result.response_schema_id=sbps::kSchemaCoordinateDdlAlterDictionaryResultV1;result.frame_flags=sbps::kFlagResponse|sbps::kFlagFinal;result.session_uuid=request.header.session_uuid;auto refuse=[&](std::string c,std::string d){result.frame_flags|=sbps::kFlagError;result.diagnostics.push_back(sbps::IpcDiagnostic(std::move(c),"parser_server_ipc.ddl_alter_dictionary_refused","Dictionary alter coordination was refused.",{{"detail",std::move(d)}}));return result;};scratchbird::engine::sblr::SblrDdlAlterDictionaryRequestV1 v;std::string d;if(!registry||!scratchbird::engine::sblr::DecodeSblrDdlAlterDictionaryRequestV1(request.payload.data(),request.payload.size(),&v,&d))return refuse("SBLR.OPERAND.INVALID",d);auto s=registry->sessions_by_uuid.find(UuidBytesToText(request.header.session_uuid));if(s==registry->sessions_by_uuid.end())return refuse("SECURITY.ACCESS_DENIED","session_hidden");auto ru=UuidBytesToText(v.receipt);auto c=EngineContextForSession(s->second,engine_state,request);c.statement_uuid.canonical=ru;c.statement_metadata_snapshot_engine_owned=true;c.trace_tags.push_back("private_ddl_alter_dictionary_binder");auto q=scratchbird::engine::internal_api::CompileSblrDdlAlterDictionaryDescriptor(c,ru,v.occurrence,v.dictionary_occurrence,1);if(!q.ok)return refuse(q.diagnostic.code,q.diagnostic.message_key);result.payload=scratchbird::engine::sblr::EncodeSblrDdlAlterDictionaryDescriptorV1(q.descriptor,false);if(result.payload.empty())return refuse("DDL.ALTER_DICTIONARY_FAILED","ADDD_encode_failed");result.accepted=true;return result;}
SessionOperationResult HandleCoordinateDdlCreateContinuousView(ServerSessionRegistry*registry,const HostedEngineState&engine_state,const sbps::Frame&request){SessionOperationResult result;result.response_message_type=273;result.response_schema_id=sbps::kSchemaCoordinateDdlCreateContinuousViewResultV1;result.frame_flags=sbps::kFlagResponse|sbps::kFlagFinal;result.session_uuid=request.header.session_uuid;auto refuse=[&](std::string c,std::string d){result.frame_flags|=sbps::kFlagError;result.diagnostics.push_back(sbps::IpcDiagnostic(std::move(c),"parser_server_ipc.ddl_create_continuous_view_refused","Continuous view coordination was refused.",{{"detail",std::move(d)}}));return result;};scratchbird::engine::sblr::SblrDdlCreateContinuousViewRequestV1 v;std::string d;if(!registry||!scratchbird::engine::sblr::DecodeSblrDdlCreateContinuousViewRequestV1(request.payload.data(),request.payload.size(),&v,&d))return refuse("SBLR.OPERAND.INVALID",d);auto s=registry->sessions_by_uuid.find(UuidBytesToText(request.header.session_uuid));if(s==registry->sessions_by_uuid.end())return refuse("SECURITY.ACCESS_DENIED","session_hidden");auto ru=UuidBytesToText(v.receipt);auto c=EngineContextForSession(s->second,engine_state,request);c.statement_uuid.canonical=ru;c.statement_metadata_snapshot_engine_owned=true;c.trace_tags.push_back("private_ddl_create_continuous_view_binder");auto q=scratchbird::engine::internal_api::CompileSblrDdlCreateContinuousViewDescriptor(c,ru,v.occurrence,static_cast<std::uint32_t>(v.view_occurrence),1);if(!q.ok)return refuse(q.diagnostic.code,q.diagnostic.message_key);result.payload=scratchbird::engine::sblr::EncodeSblrDdlCreateContinuousViewDescriptorV1(q.descriptor,false);if(result.payload.empty())return refuse("DDL.CREATE_CONTINUOUS_VIEW_FAILED","CVDD_encode_failed");result.accepted=true;return result;}
SessionOperationResult HandleCoordinateDdlAlterContinuousView(ServerSessionRegistry*registry,const HostedEngineState&engine_state,const sbps::Frame&request){SessionOperationResult result;result.response_message_type=275;result.response_schema_id=sbps::kSchemaCoordinateDdlAlterContinuousViewResultV1;result.frame_flags=sbps::kFlagResponse|sbps::kFlagFinal;result.session_uuid=request.header.session_uuid;auto refuse=[&](std::string c,std::string d){result.frame_flags|=sbps::kFlagError;result.diagnostics.push_back(sbps::IpcDiagnostic(std::move(c),"parser_server_ipc.ddl_alter_continuous_view_refused","Continuous view alteration coordination was refused.",{{"detail",std::move(d)}}));return result;};scratchbird::engine::sblr::SblrDdlAlterContinuousViewRequestV1 v;std::string d;if(!registry||!scratchbird::engine::sblr::DecodeSblrDdlAlterContinuousViewRequestV1(request.payload.data(),request.payload.size(),&v,&d))return refuse("SBLR.OPERAND.INVALID",d);auto s=registry->sessions_by_uuid.find(UuidBytesToText(request.header.session_uuid));if(s==registry->sessions_by_uuid.end())return refuse("SECURITY.ACCESS_DENIED","session_hidden");auto ru=UuidBytesToText(v.receipt);auto c=EngineContextForSession(s->second,engine_state,request);c.statement_uuid.canonical=ru;c.statement_metadata_snapshot_engine_owned=true;c.trace_tags.push_back("private_ddl_alter_continuous_view_binder");auto q=scratchbird::engine::internal_api::CompileSblrDdlAlterContinuousViewDescriptor(c,ru,v.occurrence,static_cast<std::uint32_t>(v.view_occurrence),1);if(!q.ok)return refuse(q.diagnostic.code,q.diagnostic.message_key);result.payload=scratchbird::engine::sblr::EncodeSblrDdlAlterContinuousViewDescriptorV1(q.descriptor,false);if(result.payload.empty())return refuse("DDL.ALTER_CONTINUOUS_VIEW_FAILED","AVDD_encode_failed");result.accepted=true;return result;}
SessionOperationResult HandleCoordinateDdlDropContinuousView(ServerSessionRegistry*registry,const HostedEngineState&engine_state,const sbps::Frame&request){SessionOperationResult result;result.response_message_type=277;result.response_schema_id=sbps::kSchemaCoordinateDdlDropContinuousViewResultV1;result.frame_flags=sbps::kFlagResponse|sbps::kFlagFinal;result.session_uuid=request.header.session_uuid;auto refuse=[&](std::string c,std::string d){result.frame_flags|=sbps::kFlagError;result.diagnostics.push_back(sbps::IpcDiagnostic(std::move(c),"parser_server_ipc.ddl_drop_continuous_view_refused","Continuous view drop coordination was refused.",{{"detail",std::move(d)}}));return result;};scratchbird::engine::sblr::SblrDdlDropContinuousViewRequestV1 v;std::string d;if(!registry||!scratchbird::engine::sblr::DecodeSblrDdlDropContinuousViewRequestV1(request.payload.data(),request.payload.size(),&v,&d))return refuse("SBLR.OPERAND.INVALID",d);auto s=registry->sessions_by_uuid.find(UuidBytesToText(request.header.session_uuid));if(s==registry->sessions_by_uuid.end())return refuse("SECURITY.ACCESS_DENIED","session_hidden");auto ru=UuidBytesToText(v.receipt);auto c=EngineContextForSession(s->second,engine_state,request);c.statement_uuid.canonical=ru;c.statement_metadata_snapshot_engine_owned=true;c.trace_tags.push_back("private_ddl_drop_continuous_view_binder");auto q=scratchbird::engine::internal_api::CompileSblrDdlDropContinuousViewDescriptor(c,ru,v.occurrence,static_cast<std::uint32_t>(v.view_occurrence),1);if(!q.ok)return refuse(q.diagnostic.code,q.diagnostic.message_key);result.payload=scratchbird::engine::sblr::EncodeSblrDdlDropContinuousViewDescriptorV1(q.descriptor,false);if(result.payload.empty())return refuse("DDL.DROP_CONTINUOUS_VIEW_FAILED","DVDD_encode_failed");result.accepted=true;return result;}
SessionOperationResult HandleCoordinateDmlAsyncInsertSubmit(ServerSessionRegistry*registry,const HostedEngineState&engine_state,const sbps::Frame&request){SessionOperationResult result;result.response_message_type=279;result.response_schema_id=sbps::kSchemaCoordinateDmlAsyncInsertSubmitResultV1;result.frame_flags=sbps::kFlagResponse|sbps::kFlagFinal;result.session_uuid=request.header.session_uuid;auto refuse=[&](std::string c,std::string d){result.frame_flags|=sbps::kFlagError;result.diagnostics.push_back(sbps::IpcDiagnostic(std::move(c),"parser_server_ipc.dml_async_insert_submit_refused","Async insert submit coordination was refused.",{{"detail",std::move(d)}}));return result;};scratchbird::engine::sblr::SblrDmlAsyncInsertSubmitRequestV1 v;std::string d;if(!registry||!scratchbird::engine::sblr::DecodeSblrDmlAsyncInsertSubmitRequestV1(request.payload.data(),request.payload.size(),&v,&d))return refuse("SBLR.OPERAND.INVALID",d);auto s=registry->sessions_by_uuid.find(UuidBytesToText(request.header.session_uuid));if(s==registry->sessions_by_uuid.end())return refuse("SECURITY.ACCESS_DENIED","session_hidden");auto ru=UuidBytesToText(v.receipt);auto c=EngineContextForSession(s->second,engine_state,request);c.statement_uuid.canonical=ru;c.statement_metadata_snapshot_engine_owned=true;c.trace_tags.push_back("private_dml_async_insert_submit_binder");auto q=scratchbird::engine::internal_api::CompileSblrDmlAsyncInsertSubmitDescriptor(c,ru,v.occurrence,v.submission_occurrence,1);if(!q.ok)return refuse(q.diagnostic.code,q.diagnostic.message_key);result.payload=scratchbird::engine::sblr::EncodeSblrDmlAsyncInsertSubmitDescriptorV1(q.descriptor,false);if(result.payload.empty())return refuse("DML.ASYNC_INSERT.SUBMIT_FAILED","AIDD_encode_failed");result.accepted=true;return result;}
SessionOperationResult HandleCoordinateDmlAsyncInsertStatus(ServerSessionRegistry*registry,const HostedEngineState&engine_state,const sbps::Frame&request){SessionOperationResult result;result.response_message_type=281;result.response_schema_id=sbps::kSchemaCoordinateDmlAsyncInsertStatusResultV1;result.frame_flags=sbps::kFlagResponse|sbps::kFlagFinal;result.session_uuid=request.header.session_uuid;auto refuse=[&](std::string c,std::string d){result.frame_flags|=sbps::kFlagError;result.diagnostics.push_back(sbps::IpcDiagnostic(std::move(c),"parser_server_ipc.dml_async_insert_status_refused","Async insert submit coordination was refused.",{{"detail",std::move(d)}}));return result;};scratchbird::engine::sblr::SblrDmlAsyncInsertStatusRequestV1 v;std::string d;if(!registry||!scratchbird::engine::sblr::DecodeSblrDmlAsyncInsertStatusRequestV1(request.payload.data(),request.payload.size(),&v,&d))return refuse("SBLR.OPERAND.INVALID",d);auto s=registry->sessions_by_uuid.find(UuidBytesToText(request.header.session_uuid));if(s==registry->sessions_by_uuid.end())return refuse("SECURITY.ACCESS_DENIED","session_hidden");auto ru=UuidBytesToText(v.receipt);auto c=EngineContextForSession(s->second,engine_state,request);c.statement_uuid.canonical=ru;c.statement_metadata_snapshot_engine_owned=true;c.trace_tags.push_back("private_dml_async_insert_status_binder");auto q=scratchbird::engine::internal_api::CompileSblrDmlAsyncInsertStatusDescriptor(c,ru,v.occurrence,v.status_occurrence,1);if(!q.ok)return refuse(q.diagnostic.code,q.diagnostic.message_key);result.payload=scratchbird::engine::sblr::EncodeSblrDmlAsyncInsertStatusDescriptorV1(q.descriptor,false);if(result.payload.empty())return refuse("DML.ASYNC_INSERT.SUBMIT_FAILED","AITD_encode_failed");result.accepted=true;return result;}

SessionOperationResult HandleCoordinateDmlCounterAdd(ServerSessionRegistry*registry,const HostedEngineState&engine_state,const sbps::Frame&request){SessionOperationResult result;result.response_message_type=287;result.response_schema_id=sbps::kSchemaCoordinateDmlCounterAddResultV1;result.frame_flags=sbps::kFlagResponse|sbps::kFlagFinal;result.session_uuid=request.header.session_uuid;auto refuse=[&](std::string c,std::string d){result.frame_flags|=sbps::kFlagError;result.diagnostics.push_back(sbps::IpcDiagnostic(std::move(c),"parser_server_ipc.dml_counter_add_refused","Counter add coordination was refused.",{{"detail",std::move(d)}}));return result;};scratchbird::engine::sblr::SblrDmlCounterAddRequestV1 v;std::string d;if(!registry||!scratchbird::engine::sblr::DecodeSblrDmlCounterAddRequestV1(request.payload.data(),request.payload.size(),&v,&d))return refuse("SBLR.OPERAND.INVALID",d);auto s=registry->sessions_by_uuid.find(UuidBytesToText(request.header.session_uuid));if(s==registry->sessions_by_uuid.end())return refuse("SECURITY.ACCESS_DENIED","session_hidden");auto ru=UuidBytesToText(v.receipt);auto c=EngineContextForSession(s->second,engine_state,request);c.statement_uuid.canonical=ru;c.statement_metadata_snapshot_engine_owned=true;c.trace_tags.push_back("private_dml_counter_add_binder");auto q=scratchbird::engine::internal_api::CompileSblrDmlCounterAddDescriptor(c,ru,v.occurrence,v.counter_occurrence,1);if(!q.ok)return refuse(q.diagnostic.code,q.diagnostic.message_key);result.payload=scratchbird::engine::sblr::EncodeSblrDmlCounterAddDescriptorV1(q.descriptor,false);if(result.payload.empty())return refuse("DML.COUNTER_ADD.SUBMIT_FAILED","CARD_encode_failed");result.accepted=true;return result;}
SessionOperationResult HandleCoordinateDmlTimeseriesSchemaWrite(ServerSessionRegistry*registry,const HostedEngineState&engine_state,const sbps::Frame&request){SessionOperationResult result;result.response_message_type=289;result.response_schema_id=sbps::kSchemaCoordinateDmlTimeseriesSchemaWriteResultV1;result.frame_flags=sbps::kFlagResponse|sbps::kFlagFinal;result.session_uuid=request.header.session_uuid;auto refuse=[&](std::string c,std::string d){result.frame_flags|=sbps::kFlagError;result.diagnostics.push_back(sbps::IpcDiagnostic(std::move(c),"parser_server_ipc.dml_timeseries_schema_write_refused","Timeseries schema write coordination was refused.",{{"detail",std::move(d)}}));return result;};scratchbird::engine::sblr::SblrDmlTimeseriesSchemaWriteRequestV1 v;std::string d;if(!registry||!scratchbird::engine::sblr::DecodeSblrDmlTimeseriesSchemaWriteRequestV1(request.payload.data(),request.payload.size(),&v,&d))return refuse("SBLR.OPERAND.INVALID",d);auto s=registry->sessions_by_uuid.find(UuidBytesToText(request.header.session_uuid));if(s==registry->sessions_by_uuid.end())return refuse("SECURITY.ACCESS_DENIED","session_hidden");auto ru=UuidBytesToText(v.receipt);auto c=EngineContextForSession(s->second,engine_state,request);c.statement_uuid.canonical=ru;c.statement_metadata_snapshot_engine_owned=true;c.trace_tags.push_back("private_dml_timeseries_schema_write_binder");auto q=scratchbird::engine::internal_api::CompileSblrDmlTimeseriesSchemaWriteDescriptor(c,ru,v.occurrence,v.write_occurrence,1);if(!q.ok)return refuse(q.diagnostic.code,q.diagnostic.message_key);result.payload=scratchbird::engine::sblr::EncodeSblrDmlTimeseriesSchemaWriteDescriptorV1(q.descriptor,false);if(result.payload.empty())return refuse("DML.TIMESERIES.SCHEMA_WRITE.SUBMIT_FAILED","TSDD_encode_failed");result.accepted=true;return result;}

SessionOperationResult HandleCoordinateDdlTimeseriesSeriesCardinalityPolicy(ServerSessionRegistry*registry,const HostedEngineState&engine_state,const sbps::Frame&request){SessionOperationResult result;result.response_message_type=291;result.response_schema_id=sbps::kSchemaCoordinateDdlTimeseriesSeriesCardinalityPolicyResultV1;result.frame_flags=sbps::kFlagResponse|sbps::kFlagFinal;result.session_uuid=request.header.session_uuid;auto refuse=[&](std::string c,std::string d){result.frame_flags|=sbps::kFlagError;result.diagnostics.push_back(sbps::IpcDiagnostic(std::move(c),"parser_server_ipc.ddl_timeseries_series_cardinality_policy_refused","Timeseries series-cardinality policy coordination was refused.",{{"detail",std::move(d)}}));return result;};scratchbird::engine::sblr::SblrDdlTimeseriesSeriesCardinalityPolicyRequestV1 v;std::string d;if(!registry||!scratchbird::engine::sblr::DecodeSblrDdlTimeseriesSeriesCardinalityPolicyRequestV1(request.payload.data(),request.payload.size(),&v,&d))return refuse("SBLR.OPERAND.INVALID",d);auto s=registry->sessions_by_uuid.find(UuidBytesToText(request.header.session_uuid));if(s==registry->sessions_by_uuid.end())return refuse("SECURITY.ACCESS_DENIED","session_hidden");auto ru=UuidBytesToText(v.receipt);auto c=EngineContextForSession(s->second,engine_state,request);c.statement_uuid.canonical=ru;c.statement_metadata_snapshot_engine_owned=true;c.trace_tags.push_back("private_ddl_timeseries_series_cardinality_policy_binder");auto q=scratchbird::engine::internal_api::CompileSblrDdlTimeseriesSeriesCardinalityPolicyDescriptor(c,ru,v.occurrence,v.policy_occurrence,1);if(!q.ok)return refuse(q.diagnostic.code,q.diagnostic.message_key);result.payload=scratchbird::engine::sblr::EncodeSblrDdlTimeseriesSeriesCardinalityPolicyDescriptorV1(q.descriptor,false);if(result.payload.empty())return refuse("DDL.TIMESERIES.SERIES_CARDINALITY.SUBMIT_FAILED","SCPD_encode_failed");result.accepted=true;return result;}
SessionOperationResult HandleCoordinateDmlAsyncInsertCancel(ServerSessionRegistry*registry,const HostedEngineState&engine_state,const sbps::Frame&request){SessionOperationResult result;result.response_message_type=283;result.response_schema_id=sbps::kSchemaCoordinateDmlAsyncInsertCancelResultV1;result.frame_flags=sbps::kFlagResponse|sbps::kFlagFinal;result.session_uuid=request.header.session_uuid;auto refuse=[&](std::string c,std::string d){result.frame_flags|=sbps::kFlagError;result.diagnostics.push_back(sbps::IpcDiagnostic(std::move(c),"parser_server_ipc.dml_async_insert_cancel_refused","Async insert submit coordination was refused.",{{"detail",std::move(d)}}));return result;};scratchbird::engine::sblr::SblrDmlAsyncInsertCancelRequestV1 v;std::string d;if(!registry||!scratchbird::engine::sblr::DecodeSblrDmlAsyncInsertCancelRequestV1(request.payload.data(),request.payload.size(),&v,&d))return refuse("SBLR.OPERAND.INVALID",d);auto s=registry->sessions_by_uuid.find(UuidBytesToText(request.header.session_uuid));if(s==registry->sessions_by_uuid.end())return refuse("SECURITY.ACCESS_DENIED","session_hidden");auto ru=UuidBytesToText(v.receipt);auto c=EngineContextForSession(s->second,engine_state,request);c.statement_uuid.canonical=ru;c.statement_metadata_snapshot_engine_owned=true;c.trace_tags.push_back("private_dml_async_insert_cancel_binder");auto q=scratchbird::engine::internal_api::CompileSblrDmlAsyncInsertCancelDescriptor(c,ru,v.occurrence,v.cancel_occurrence,1);if(!q.ok)return refuse(q.diagnostic.code,q.diagnostic.message_key);result.payload=scratchbird::engine::sblr::EncodeSblrDmlAsyncInsertCancelDescriptorV1(q.descriptor,false);if(result.payload.empty())return refuse("DML.ASYNC_INSERT.SUBMIT_FAILED","AICD_encode_failed");result.accepted=true;return result;}
SessionOperationResult HandleCoordinateDdlCreateTimeseriesValueCache(ServerSessionRegistry*registry,const HostedEngineState&engine_state,const sbps::Frame&request){SessionOperationResult result;result.response_message_type=293;result.response_schema_id=sbps::kSchemaCoordinateDdlCreateTimeseriesValueCacheResultV1;result.frame_flags=sbps::kFlagResponse|sbps::kFlagFinal;result.session_uuid=request.header.session_uuid;auto refuse=[&](std::string c,std::string d){result.frame_flags|=sbps::kFlagError;result.diagnostics.push_back(sbps::IpcDiagnostic(std::move(c),"parser_server_ipc.ddl_create_timeseries_value_cache_refused","Timeseries value-cache creation coordination was refused.",{{"detail",std::move(d)}}));return result;};scratchbird::engine::sblr::SblrDdlCreateTimeseriesValueCacheRequestV1 v;std::string d;if(!registry||!scratchbird::engine::sblr::DecodeSblrDdlCreateTimeseriesValueCacheRequestV1(request.payload.data(),request.payload.size(),&v,&d))return refuse("SBLR.OPERAND.INVALID",d);auto s=registry->sessions_by_uuid.find(UuidBytesToText(request.header.session_uuid));if(s==registry->sessions_by_uuid.end())return refuse("SECURITY.ACCESS_DENIED","session_hidden");auto ru=UuidBytesToText(v.receipt);auto c=EngineContextForSession(s->second,engine_state,request);c.statement_uuid.canonical=ru;c.statement_metadata_snapshot_engine_owned=true;c.trace_tags.push_back("private_ddl_create_timeseries_value_cache_binder");auto q=scratchbird::engine::internal_api::CompileSblrDdlCreateTimeseriesValueCacheDescriptor(c,ru,v.occurrence,v.cache_occurrence,1);if(!q.ok)return refuse(q.diagnostic.code,q.diagnostic.message_key);result.payload=scratchbird::engine::sblr::EncodeSblrDdlCreateTimeseriesValueCacheDescriptorV1(q.descriptor,false);if(result.payload.empty())return refuse("TIMESERIES.VALUE_CACHE.CREATE_FAILED","VCDD_encode_failed");result.accepted=true;return result;}
SessionOperationResult HandleCoordinateDdlRefreshMaterializedView(ServerSessionRegistry*registry,const HostedEngineState&engine_state,const sbps::Frame&request){SessionOperationResult result;result.response_message_type=static_cast<std::uint16_t>(sbps::MessageType::kCoordinateDdlRefreshMaterializedViewResult);result.response_schema_id=sbps::kSchemaCoordinateDdlRefreshMaterializedViewResultV1;result.frame_flags=sbps::kFlagResponse|sbps::kFlagFinal;result.session_uuid=request.header.session_uuid;auto refuse=[&](std::string c,std::string d){result.frame_flags|=sbps::kFlagError;result.diagnostics.push_back(sbps::IpcDiagnostic(std::move(c),"parser_server_ipc.ddl_refresh_materialized_view_refused","Materialized view refresh coordination was refused.",{{"detail",std::move(d)}}));return result;};scratchbird::engine::sblr::SblrDdlRefreshMaterializedViewDescriptorV1 v;std::string d;if(!registry||request.payload.size()!=64)return refuse("SBLR.OPERAND.INVALID","RVDX request invalid");auto s=registry->sessions_by_uuid.find(UuidBytesToText(request.header.session_uuid));if(s==registry->sessions_by_uuid.end())return refuse("SECURITY.ACCESS_DENIED","session_hidden");auto ru=UuidBytesToText(request.payload.data(),request.payload.data()+16);auto c=EngineContextForSession(s->second,engine_state,request);c.statement_uuid.canonical=ru;c.statement_metadata_snapshot_engine_owned=true;c.trace_tags.push_back("private_ddl_refresh_materialized_view_binder");auto q=scratchbird::engine::internal_api::CompileSblrDdlRefreshMaterializedViewDescriptor(c,ru,1,1,1);if(!q.ok)return refuse(q.diagnostic.code,q.diagnostic.message_key);result.payload=scratchbird::engine::sblr::EncodeSblrDdlRefreshMaterializedViewDescriptorV1(q.descriptor,false);if(result.payload.empty())return refuse("DDL.MATERIALIZED_VIEW.REFRESH_FAILED","RVDX_encode_failed");result.accepted=true;return result;}
SessionOperationResult HandleCoordinateDdlDropMaterializedView(ServerSessionRegistry*registry,const HostedEngineState&engine_state,const sbps::Frame&request){SessionOperationResult result;result.response_message_type=static_cast<std::uint16_t>(sbps::MessageType::kCoordinateDdlDropMaterializedViewResult);result.response_schema_id=sbps::kSchemaCoordinateDdlDropMaterializedViewResultV1;result.frame_flags=sbps::kFlagResponse|sbps::kFlagFinal;result.session_uuid=request.header.session_uuid;auto refuse=[&](std::string c,std::string d){result.frame_flags|=sbps::kFlagError;result.diagnostics.push_back(sbps::IpcDiagnostic(std::move(c),"parser_server_ipc.ddl_drop_materialized_view_refused","Materialized view drop coordination was refused.",{{"detail",std::move(d)}}));return result;};if(!registry||request.payload.size()!=64)return refuse("SBLR.OPERAND.INVALID","MVDX request invalid");auto s=registry->sessions_by_uuid.find(UuidBytesToText(request.header.session_uuid));if(s==registry->sessions_by_uuid.end())return refuse("SECURITY.ACCESS_DENIED","session_hidden");auto ru=UuidBytesToText(request.payload.data(),request.payload.data()+16);auto c=EngineContextForSession(s->second,engine_state,request);c.statement_uuid.canonical=ru;c.statement_metadata_snapshot_engine_owned=true;c.trace_tags.push_back("private_ddl_drop_materialized_view_binder");auto q=scratchbird::engine::internal_api::CompileSblrDdlDropMaterializedViewDescriptor(c,ru,1,1,1);if(!q.ok)return refuse(q.diagnostic.code,q.diagnostic.message_key);result.payload=scratchbird::engine::sblr::EncodeSblrDdlDropMaterializedViewDescriptorV1(q.descriptor,false);if(result.payload.empty())return refuse("DDL.MATERIALIZED_VIEW.DROP_FAILED","MVDX_encode_failed");result.accepted=true;return result;}
SessionOperationResult HandleCoordinateDdlCreateType(ServerSessionRegistry* r,const HostedEngineState& e,const sbps::Frame& q){SessionOperationResult o;o.response_message_type=301;o.response_schema_id=sbps::kSchemaCoordinateDdlCreateTypeResultV1;o.frame_flags=sbps::kFlagResponse|sbps::kFlagFinal;o.session_uuid=q.header.session_uuid;auto bad=[&](std::string c,std::string d){o.frame_flags|=sbps::kFlagError;o.diagnostics.push_back(sbps::IpcDiagnostic(std::move(c),"parser_server_ipc.ddl_create_type_refused","CREATE TYPE coordination refused.",{{"detail",std::move(d)}}));return o;};scratchbird::engine::sblr::SblrDdlCreateTypeRequestV1 v;std::string d;if(!r||!scratchbird::engine::sblr::DecodeSblrDdlCreateViewRequestV1(q.payload.data(),q.payload.size(),&v,&d))return bad("SBLR.OPERAND.INVALID",d);auto s=r->sessions_by_uuid.find(UuidBytesToText(q.header.session_uuid));if(s==r->sessions_by_uuid.end())return bad("SECURITY.ACCESS_DENIED","session_hidden");auto u=UuidBytesToText(v.receipt);auto c=EngineContextForSession(s->second,e,q);c.statement_uuid.canonical=u;c.statement_metadata_snapshot_engine_owned=true;c.trace_tags.push_back("private_ddl_create_type_binder");auto x=scratchbird::engine::internal_api::CompileSblrDdlCreateTypeDescriptor(c,u,v.occurrence,v.domain_occurrence,1);if(!x.ok)return bad(x.diagnostic.code,x.diagnostic.message_key);o.payload=scratchbird::engine::sblr::EncodeSblrDdlCreateTypeDescriptorV1(x.descriptor,false);o.accepted=!o.payload.empty();return o;}
SessionOperationResult HandleCoordinateDdlAlterType(ServerSessionRegistry*r,const HostedEngineState&e,const sbps::Frame&q){SessionOperationResult o;o.response_message_type=303;o.response_schema_id=sbps::kSchemaCoordinateDdlAlterTypeResultV1;o.frame_flags=sbps::kFlagResponse|sbps::kFlagFinal;o.session_uuid=q.header.session_uuid;auto bad=[&](std::string c,std::string d){o.frame_flags|=sbps::kFlagError;o.diagnostics.push_back(sbps::IpcDiagnostic(std::move(c),"parser_server_ipc.ddl_alter_type_refused","ALTER TYPE coordination refused.",{{"detail",std::move(d)}}));return o;};scratchbird::engine::sblr::SblrDdlAlterTypeRequestV1 v;std::string d;if(!r||!scratchbird::engine::sblr::DecodeSblrDdlAlterTypeRequestV1(q.payload.data(),q.payload.size(),&v,&d))return bad("SBLR.OPERAND.INVALID",d);auto s=r->sessions_by_uuid.find(UuidBytesToText(q.header.session_uuid));if(s==r->sessions_by_uuid.end())return bad("SECURITY.ACCESS_DENIED","session_hidden");auto u=UuidBytesToText(v.receipt);auto c=EngineContextForSession(s->second,e,q);c.statement_uuid.canonical=u;c.statement_metadata_snapshot_engine_owned=true;c.trace_tags.push_back("private_ddl_alter_type_binder");auto x=scratchbird::engine::internal_api::CompileSblrDdlAlterTypeDescriptor(c,u,v.occurrence,v.domain_occurrence,1);if(!x.ok)return bad(x.diagnostic.code,x.diagnostic.message_key);o.payload=scratchbird::engine::sblr::EncodeSblrDdlAlterTypeDescriptorV1(x.descriptor,false);if(o.payload.empty())return bad("SBLR.OPERAND.INVALID","alter_type_descriptor_encode_failed");o.accepted=true;return o;}
SessionOperationResult HandleCoordinateDdlDropType(ServerSessionRegistry*r,const HostedEngineState&e,const sbps::Frame&q){SessionOperationResult o;o.response_message_type=305;o.response_schema_id=sbps::kSchemaCoordinateDdlDropTypeResultV1;o.frame_flags=sbps::kFlagResponse|sbps::kFlagFinal;o.session_uuid=q.header.session_uuid;auto bad=[&](std::string c,std::string d){o.frame_flags|=sbps::kFlagError;o.diagnostics.push_back(sbps::IpcDiagnostic(std::move(c),"parser_server_ipc.ddl_drop_type_refused","DROP TYPE coordination refused.",{{"detail",std::move(d)}}));return o;};scratchbird::engine::sblr::SblrDdlDropTypeRequestV1 v;std::string d;if(!r||!scratchbird::engine::sblr::DecodeSblrDdlDropViewRequestV1(q.payload.data(),q.payload.size(),&v,&d))return bad("SBLR.OPERAND.INVALID",d);auto s=r->sessions_by_uuid.find(UuidBytesToText(q.header.session_uuid));if(s==r->sessions_by_uuid.end())return bad("SECURITY.ACCESS_DENIED","session_hidden");auto u=UuidBytesToText(v.receipt);auto c=EngineContextForSession(s->second,e,q);c.statement_uuid.canonical=u;c.statement_metadata_snapshot_engine_owned=true;c.trace_tags.push_back("private_ddl_drop_type_binder");auto x=scratchbird::engine::internal_api::CompileSblrDdlDropTypeDescriptor(c,u,v.occurrence,v.domain_occurrence,1);if(!x.ok)return bad(x.diagnostic.code,x.diagnostic.message_key);o.payload=scratchbird::engine::sblr::EncodeSblrDdlDropTypeDescriptorV1(x.descriptor,false);if(o.payload.empty())return bad("SBLR.OPERAND.INVALID","drop_type_descriptor_encode_failed");o.accepted=true;return o;}
SessionOperationResult HandleCoordinateDdlDropPackage(ServerSessionRegistry*registry,const HostedEngineState&engine_state,const sbps::Frame&request){SessionOperationResult result;result.response_message_type=static_cast<std::uint16_t>(sbps::MessageType::kCoordinateDdlDropPackageResult);result.response_schema_id=sbps::kSchemaCoordinateDdlDropPackageResultV1;result.frame_flags=sbps::kFlagResponse|sbps::kFlagFinal;result.session_uuid=request.header.session_uuid;auto refuse=[&](std::string c,std::string d){result.frame_flags|=sbps::kFlagError;result.diagnostics.push_back(sbps::IpcDiagnostic(std::move(c),"parser_server_ipc.ddl_drop_package_refused","DDL drop package coordination was refused.",{{"detail",std::move(d)}}));return result;};scratchbird::engine::sblr::SblrDdlDropPackageRequestV1 v;std::string d;if(!registry||!scratchbird::engine::sblr::DecodeSblrDdlDropPackageRequestV1(request.payload.data(),request.payload.size(),&v,&d))return refuse("SBLR.OPERAND.INVALID",d);auto s=registry->sessions_by_uuid.find(UuidBytesToText(request.header.session_uuid));if(s==registry->sessions_by_uuid.end())return refuse("SECURITY.ACCESS_DENIED","session_hidden");auto ru=UuidBytesToText(v.receipt);auto c=EngineContextForSession(s->second,engine_state,request);c.statement_uuid.canonical=ru;c.statement_metadata_snapshot_engine_owned=true;c.trace_tags.push_back("private_ddl_drop_package_binder");auto q=scratchbird::engine::internal_api::CompileSblrDdlDropPackageDescriptor(c,ru,v.occurrence,v.procedure_occurrence,1);if(!q.ok)return refuse(q.diagnostic.code,q.diagnostic.message_key);result.payload=scratchbird::engine::sblr::EncodeSblrDdlDropPackageDescriptorV1(q.descriptor,false);if(result.payload.empty())return refuse("DDL.DROP_PACKAGE_FAILED","PKDX_encode_failed");result.accepted=true;return result;}

SessionOperationResult HandleCoordinateDdlDropSynonym(ServerSessionRegistry*registry,const HostedEngineState&engine_state,const sbps::Frame&request){auto translated=request;if(translated.payload.size()>=4){translated.payload[0]='P';translated.payload[1]='C';translated.payload[2]='Q';translated.payload[3]='X';}auto result=HandleCoordinateDdlDropPackage(registry,engine_state,translated);result.response_message_type=static_cast<std::uint16_t>(sbps::MessageType::kCoordinateDdlDropSynonymResult);result.response_schema_id=sbps::kSchemaCoordinateDdlDropSynonymResultV1;if(result.payload.size()>=4&&!result.payload.empty()){result.payload[0]='D';result.payload[1]='S';result.payload[2]='Y';result.payload[3]='D';}return result;}
SessionOperationResult HandleCoordinateDdlDropForeignTable(ServerSessionRegistry*registry,const HostedEngineState&engine_state,const sbps::Frame&request){auto translated=request;if(translated.payload.size()>=4){translated.payload[0]='P';translated.payload[1]='C';translated.payload[2]='Q';translated.payload[3]='X';}auto result=HandleCoordinateDdlDropPackage(registry,engine_state,translated);result.response_message_type=static_cast<std::uint16_t>(sbps::MessageType::kCoordinateDdlDropForeignTableResult);result.response_schema_id=sbps::kSchemaCoordinateDdlDropForeignTableResultV1;if(result.payload.size()>=4){result.payload[0]='D';result.payload[1]='F';result.payload[2]='T';result.payload[3]='D';}return result;}
SessionOperationResult HandleCoordinateDdlDropFdw(ServerSessionRegistry*registry,const HostedEngineState&engine_state,const sbps::Frame&request){auto translated=request;if(translated.payload.size()>=4){translated.payload[0]='P';translated.payload[1]='C';translated.payload[2]='Q';translated.payload[3]='X';}auto result=HandleCoordinateDdlDropPackage(registry,engine_state,translated);result.response_message_type=static_cast<std::uint16_t>(sbps::MessageType::kCoordinateDdlDropFdwResult);result.response_schema_id=sbps::kSchemaCoordinateDdlDropFdwResultV1;if(result.payload.size()>=4){result.payload[0]='F';result.payload[1]='D';result.payload[2]='X';result.payload[3]='D';}return result;}

SessionOperationResult HandleCoordinateDdlCreateTableAsQueryWithData(ServerSessionRegistry*registry,const HostedEngineState&engine_state,const sbps::Frame&request){
 SessionOperationResult o;o.response_message_type=static_cast<std::uint16_t>(sbps::MessageType::kCoordinateDdlCreateTableAsQueryWithDataResult);o.response_schema_id=sbps::kSchemaCoordinateDdlCreateTableAsQueryWithDataResultV1;o.frame_flags=sbps::kFlagResponse|sbps::kFlagFinal;o.session_uuid=request.header.session_uuid;
 scratchbird::engine::sblr::SblrCreateTableAsQueryRequestV1 q;std::string d;if(!registry||!scratchbird::engine::sblr::DecodeSblrCreateTableAsQueryRequestV1(request.payload.data(),request.payload.size(),&q,&d)){o.frame_flags|=sbps::kFlagError;o.diagnostics.push_back(sbps::IpcDiagnostic("SBLR.OPERAND.INVALID","parser_server_ipc.ctas_refused","CTAS coordination refused.",{{"detail",d}}));return o;}scratchbird::engine::sblr::SblrCreateTableAsQueryDescriptorV1 x;x.opcode=1669;x.table_uuid=q.receipt;x.query_plan_uuid=q.occurrence;x.catalog_epoch=q.catalog_epoch;x.query_plan={1};o.payload=scratchbird::engine::sblr::EncodeSblrCreateTableAsQueryDescriptorV1(x);o.accepted=!o.payload.empty();return o;}
SessionOperationResult HandleCoordinateDdlCreateTableAsQueryWithNoData(ServerSessionRegistry*registry,const HostedEngineState&engine_state,const sbps::Frame&request){auto o=HandleCoordinateDdlCreateTableAsQueryWithData(registry,engine_state,request);o.response_message_type=static_cast<std::uint16_t>(sbps::MessageType::kCoordinateDdlCreateTableAsQueryWithNoDataResult);o.response_schema_id=sbps::kSchemaCoordinateDdlCreateTableAsQueryWithNoDataResultV1;if(o.accepted){scratchbird::engine::sblr::SblrCreateTableAsQueryDescriptorV1 x;std::string d;if(scratchbird::engine::sblr::DecodeSblrCreateTableAsQueryDescriptorV1(o.payload.data(),o.payload.size(),&x,&d)){x.opcode=1670;o.payload=scratchbird::engine::sblr::EncodeSblrCreateTableAsQueryDescriptorV1(x);}}return o;}
SessionOperationResult HandleCoordinateDdlAlterPackage(ServerSessionRegistry*registry,const HostedEngineState&engine_state,const sbps::Frame&request){SessionOperationResult result;result.response_message_type=static_cast<std::uint16_t>(sbps::MessageType::kCoordinateDdlAlterPackageResult);result.response_schema_id=sbps::kSchemaCoordinateDdlAlterPackageResultV1;result.frame_flags=sbps::kFlagResponse|sbps::kFlagFinal;result.session_uuid=request.header.session_uuid;auto refuse=[&](std::string c,std::string d){result.frame_flags|=sbps::kFlagError;result.diagnostics.push_back(sbps::IpcDiagnostic(std::move(c),"parser_server_ipc.ddl_alter_package_refused","DDL alter package coordination was refused.",{{"detail",std::move(d)}}));return result;};scratchbird::engine::sblr::SblrDdlAlterPackageRequestV1 v;std::string d;if(!registry||!scratchbird::engine::sblr::DecodeSblrDdlAlterPackageRequestV1(request.payload.data(),request.payload.size(),&v,&d))return refuse("SBLR.OPERAND.INVALID",d);auto s=registry->sessions_by_uuid.find(UuidBytesToText(request.header.session_uuid));if(s==registry->sessions_by_uuid.end())return refuse("SECURITY.ACCESS_DENIED","session_hidden");auto ru=UuidBytesToText(v.receipt);auto c=EngineContextForSession(s->second,engine_state,request);c.statement_uuid.canonical=ru;c.statement_metadata_snapshot_engine_owned=true;c.trace_tags.push_back("private_ddl_create_package_binder");auto q=scratchbird::engine::internal_api::CompileSblrDdlAlterPackageDescriptor(c,ru,v.occurrence,v.procedure_occurrence,1);if(!q.ok)return refuse(q.diagnostic.code,q.diagnostic.message_key);result.payload=scratchbird::engine::sblr::EncodeSblrDdlAlterPackageDescriptorV1(q.descriptor,false);if(result.payload.empty())return refuse("DDL.ALTER_PACKAGE_FAILED","PADX_encode_failed");result.accepted=true;return result;}
SessionOperationResult HandleCoordinateDdlAlterSequence(ServerSessionRegistry*registry,const HostedEngineState&engine_state,const sbps::Frame&request){SessionOperationResult result;result.response_message_type=static_cast<std::uint16_t>(sbps::MessageType::kCoordinateDdlAlterSequenceResult);result.response_schema_id=sbps::kSchemaCoordinateDdlAlterSequenceResultV1;result.frame_flags=sbps::kFlagResponse|sbps::kFlagFinal;result.session_uuid=request.header.session_uuid;auto refuse=[&](std::string c,std::string d){result.frame_flags|=sbps::kFlagError;result.diagnostics.push_back(sbps::IpcDiagnostic(std::move(c),"parser_server_ipc.ddl_alter_sequence_refused","DDL alter sequence coordination was refused.",{{"detail",std::move(d)}}));return result;};scratchbird::engine::sblr::SblrDdlAlterSequenceRequestV1 v;std::string d;if(!registry||!scratchbird::engine::sblr::DecodeSblrDdlAlterSequenceRequestV1(request.payload.data(),request.payload.size(),&v,&d))return refuse("SBLR.OPERAND.INVALID",d);auto s=registry->sessions_by_uuid.find(UuidBytesToText(request.header.session_uuid));if(s==registry->sessions_by_uuid.end())return refuse("SECURITY.ACCESS_DENIED","session_hidden");auto ru=UuidBytesToText(v.receipt);auto c=EngineContextForSession(s->second,engine_state,request);c.statement_uuid.canonical=ru;c.statement_metadata_snapshot_engine_owned=true;c.trace_tags.push_back("private_ddl_create_package_binder");auto q=scratchbird::engine::internal_api::CompileSblrDdlAlterSequenceDescriptor(c,ru,v.occurrence,v.procedure_occurrence,1);if(!q.ok)return refuse(q.diagnostic.code,q.diagnostic.message_key);result.payload=scratchbird::engine::sblr::EncodeSblrDdlAlterSequenceDescriptorV1(q.descriptor,false);if(result.payload.empty())return refuse("DDL.ALTER_SEQUENCE_FAILED","SADX_encode_failed");result.accepted=true;return result;}
SessionOperationResult HandleCoordinateDdlCreateMaterializedView(ServerSessionRegistry*r,const HostedEngineState&e,const sbps::Frame&f){auto o=HandleCoordinateDdlCreateView(r,e,f);o.response_message_type=static_cast<std::uint16_t>(sbps::MessageType::kCoordinateDdlCreateMaterializedViewResult);o.response_schema_id=sbps::kSchemaCoordinateDdlCreateMaterializedViewResultV1;if(o.payload.size()>=4&&o.payload[0]=='C'&&o.payload[1]=='V'&&o.payload[2]=='D'&&o.payload[3]=='X'){o.payload[0]='M';o.payload[1]='V';o.payload[2]='D';o.payload[3]='X';}return o;}
SessionOperationResult HandleCoordinateDdlDropTable(ServerSessionRegistry* registry,
    const HostedEngineState& engine_state, const sbps::Frame& request) {
  SessionOperationResult result;
  result.response_message_type = static_cast<std::uint16_t>(sbps::MessageType::kCoordinateDdlDropTableResult);
  result.response_schema_id = sbps::kSchemaCoordinateDdlDropTableResultV1;
  result.frame_flags = sbps::kFlagResponse | sbps::kFlagFinal;
  result.session_uuid = request.header.session_uuid;
  auto refuse = [&](std::string code, std::string detail) {
    result.frame_flags |= sbps::kFlagError;
    result.diagnostics.push_back(sbps::IpcDiagnostic(std::move(code),
        "parser_server_ipc.ddl_drop_table_refused", "DROP TABLE coordination was refused.",
        {{"detail", std::move(detail)}}));
    return result;
  };
  scratchbird::engine::sblr::SblrDdlDropTableRequestV1 value;
  std::string detail;
  if (!registry || !scratchbird::engine::sblr::DecodeSblrDdlDropTableRequestV1(
          request.payload.data(), request.payload.size(), &value, &detail))
    return refuse("SBLR.OPERAND.INVALID", detail);
  auto session = registry->sessions_by_uuid.find(UuidBytesToText(request.header.session_uuid));
  if (session == registry->sessions_by_uuid.end())
    return refuse("SECURITY.ACCESS_DENIED", "session_hidden");
  auto receipt = UuidBytesToText(value.receipt);
  auto context = EngineContextForSession(session->second, engine_state, request);
  context.statement_uuid.canonical = receipt;
  context.statement_metadata_snapshot_engine_owned = true;
  context.trace_tags.push_back("private_ddl_drop_table_binder");
  auto compiled = scratchbird::engine::internal_api::CompileSblrDdlDropTableDescriptor(
      context, receipt, value.occurrence, value.table_occurrence, 1);
  if (!compiled.ok) return refuse(compiled.diagnostic.code, compiled.diagnostic.message_key);
  result.payload = scratchbird::engine::sblr::EncodeSblrDdlDropTableDescriptorV1(
      compiled.descriptor, false);
  if (result.payload.empty()) return refuse("SBLR.OPERAND.INVALID", "drop_table_descriptor_encode_failed");
  result.accepted = true;
  return result;
}

SessionOperationResult HandleCoordinateDdlDropSequence(ServerSessionRegistry*registry,const HostedEngineState&engine_state,const sbps::Frame&request){SessionOperationResult result;result.response_message_type=static_cast<std::uint16_t>(sbps::MessageType::kCoordinateDdlDropSequenceResult);result.response_schema_id=sbps::kSchemaCoordinateDdlDropSequenceResultV1;result.frame_flags=sbps::kFlagResponse|sbps::kFlagFinal;result.session_uuid=request.header.session_uuid;auto refuse=[&](std::string c,std::string d){result.frame_flags|=sbps::kFlagError;result.diagnostics.push_back(sbps::IpcDiagnostic(std::move(c),"parser_server_ipc.ddl_drop_sequence_refused","DDL drop sequence coordination was refused.",{{"detail",std::move(d)}}));return result;};scratchbird::engine::sblr::SblrDdlDropSequenceRequestV1 v;std::string d;if(!registry||!scratchbird::engine::sblr::DecodeSblrDdlDropSequenceRequestV1(request.payload.data(),request.payload.size(),&v,&d))return refuse("SBLR.OPERAND.INVALID",d);auto s=registry->sessions_by_uuid.find(UuidBytesToText(request.header.session_uuid));if(s==registry->sessions_by_uuid.end())return refuse("SECURITY.ACCESS_DENIED","session_hidden");auto ru=UuidBytesToText(v.receipt);auto c=EngineContextForSession(s->second,engine_state,request);c.statement_uuid.canonical=ru;c.statement_metadata_snapshot_engine_owned=true;c.trace_tags.push_back("private_ddl_drop_sequence_binder");auto q=scratchbird::engine::internal_api::CompileSblrDdlDropSequenceDescriptor(c,ru,v.occurrence,v.procedure_occurrence,1);if(!q.ok)return refuse(q.diagnostic.code,q.diagnostic.message_key);result.payload=scratchbird::engine::sblr::EncodeSblrDdlDropSequenceDescriptorV1(q.descriptor,false);if(result.payload.empty())return refuse("DDL.DROP_SEQUENCE_FAILED","SDDX_encode_failed");result.accepted=true;return result;}

SessionOperationResult HandleCoordinateDdlRenameObject(ServerSessionRegistry*registry,const HostedEngineState&engine_state,const sbps::Frame&request){SessionOperationResult result;result.response_message_type=231;result.response_schema_id=sbps::kSchemaCoordinateDdlRenameObjectResultV1;result.frame_flags=sbps::kFlagResponse|sbps::kFlagFinal;result.session_uuid=request.header.session_uuid;auto refuse=[&](std::string c,std::string d){result.frame_flags|=sbps::kFlagError;result.diagnostics.push_back(sbps::IpcDiagnostic(std::move(c),"parser_server_ipc.ddl_rename_object_refused","DDL rename object coordination was refused.",{{"detail",std::move(d)}}));return result;};scratchbird::engine::sblr::SblrDdlRenameObjectRequestV1 v;std::string d;if(!registry||!scratchbird::engine::sblr::DecodeSblrDdlRenameObjectRequestV1(request.payload.data(),request.payload.size(),&v,&d))return refuse("SBLR.OPERAND_INVALID",d);auto s=registry->sessions_by_uuid.find(UuidBytesToText(request.header.session_uuid));if(s==registry->sessions_by_uuid.end())return refuse("SECURITY.ACCESS_DENIED","session_hidden");auto ru=UuidBytesToText(v.receipt);auto c=EngineContextForSession(s->second,engine_state,request);c.statement_uuid.canonical=ru;c.statement_metadata_snapshot_engine_owned=true;c.trace_tags.push_back("private_ddl_rename_object_binder");auto q=scratchbird::engine::internal_api::CompileSblrDdlRenameObjectDescriptor(c,ru,v.occurrence,v.object_vector_occurrence,1);if(!q.ok)return refuse(q.diagnostic.code,q.diagnostic.message_key);result.payload=scratchbird::engine::sblr::EncodeSblrDdlRenameObjectDescriptorV1(q.descriptor,false);if(result.payload.empty())return refuse("DDL.RENAME_OBJECT_FAILED","RNOD_encode_failed");result.accepted=true;return result;}

SessionOperationResult HandleCoordinateDdlCreateSynonym(ServerSessionRegistry*registry,const HostedEngineState&engine_state,const sbps::Frame&request){SessionOperationResult result;result.response_message_type=215;result.response_schema_id=sbps::kSchemaCoordinateDdlCreateSynonymResultV1;result.frame_flags=sbps::kFlagResponse|sbps::kFlagFinal;result.session_uuid=request.header.session_uuid;auto refuse=[&](std::string c,std::string d){result.frame_flags|=sbps::kFlagError;result.diagnostics.push_back(sbps::IpcDiagnostic(std::move(c),"parser_server_ipc.ddl_create_synonym_refused","DDL create synonym coordination was refused.",{{"detail",std::move(d)}}));return result;};scratchbird::engine::sblr::SblrDdlCreateSynonymRequestV1 v;std::string d;if(!registry||!scratchbird::engine::sblr::DecodeSblrDdlCreateSynonymRequestV1(request.payload.data(),request.payload.size(),&v,&d))return refuse("SBLR.OPERAND_INVALID",d);auto s=registry->sessions_by_uuid.find(UuidBytesToText(request.header.session_uuid));if(s==registry->sessions_by_uuid.end())return refuse("SECURITY.ACCESS_DENIED","session_hidden");auto ru=UuidBytesToText(v.receipt);auto c=EngineContextForSession(s->second,engine_state,request);c.statement_uuid.canonical=ru;c.statement_metadata_snapshot_engine_owned=true;c.trace_tags.push_back("private_ddl_create_synonym_binder");auto q=scratchbird::engine::internal_api::CompileSblrDdlCreateSynonymDescriptor(c,ru,v.occurrence,v.procedure_occurrence,1);if(!q.ok)return refuse(q.diagnostic.code,q.diagnostic.message_key);result.payload=scratchbird::engine::sblr::EncodeSblrDdlCreateSynonymDescriptorV1(q.descriptor,false);if(result.payload.empty())return refuse("DDL.CREATE_SYNONYM_FAILED","CSYD_encode_failed");result.accepted=true;return result;}

SessionOperationResult HandleCoordinateDdlCreateForeignTable(ServerSessionRegistry*registry,const HostedEngineState&engine_state,const sbps::Frame&request){SessionOperationResult result;result.response_message_type=217;result.response_schema_id=sbps::kSchemaCoordinateDdlCreateForeignTableResultV1;result.frame_flags=sbps::kFlagResponse|sbps::kFlagFinal;result.session_uuid=request.header.session_uuid;auto refuse=[&](std::string c,std::string d){result.frame_flags|=sbps::kFlagError;result.diagnostics.push_back(sbps::IpcDiagnostic(std::move(c),"parser_server_ipc.ddl_create_foreign_table_refused","DDL create foreign table coordination was refused.",{{"detail",std::move(d)}}));return result;};scratchbird::engine::sblr::SblrDdlCreateForeignTableRequestV1 v;std::string d;if(!registry||!scratchbird::engine::sblr::DecodeSblrDdlCreateForeignTableRequestV1(request.payload.data(),request.payload.size(),&v,&d))return refuse("SBLR.OPERAND_INVALID",d);auto s=registry->sessions_by_uuid.find(UuidBytesToText(request.header.session_uuid));if(s==registry->sessions_by_uuid.end())return refuse("SECURITY.ACCESS_DENIED","session_hidden");auto ru=UuidBytesToText(v.receipt);auto c=EngineContextForSession(s->second,engine_state,request);c.statement_uuid.canonical=ru;c.statement_metadata_snapshot_engine_owned=true;c.trace_tags.push_back("private_ddl_create_foreign_table_binder");auto q=scratchbird::engine::internal_api::CompileSblrDdlCreateForeignTableDescriptor(c,ru,v.occurrence,v.table_occurrence,1);if(!q.ok)return refuse(q.diagnostic.code,q.diagnostic.message_key);result.payload=scratchbird::engine::sblr::EncodeSblrDdlCreateForeignTableDescriptorV1(q.descriptor,false);if(result.payload.empty())return refuse("DDL.CREATE_FOREIGN_TABLE_FAILED","CFTD_encode_failed");result.accepted=true;return result;}
SessionOperationResult HandleCoordinateDdlCreateFdw(ServerSessionRegistry*registry,const HostedEngineState&engine_state,const sbps::Frame&request){SessionOperationResult result;result.response_message_type=static_cast<std::uint16_t>(sbps::MessageType::kCoordinateDdlCreateFdwResult);result.response_schema_id=sbps::kSchemaCoordinateDdlCreateFdwResultV1;result.frame_flags=sbps::kFlagResponse|sbps::kFlagFinal;result.session_uuid=request.header.session_uuid;auto refuse=[&](std::string c,std::string d){result.frame_flags|=sbps::kFlagError;result.diagnostics.push_back(sbps::IpcDiagnostic(std::move(c),"parser_server_ipc.ddl_create_fdw_refused","DDL create FDW coordination was refused.",{{"detail",std::move(d)}}));return result;};scratchbird::engine::sblr::SblrDdlCreateFdwRequestV1 v;std::string d;if(!registry||!scratchbird::engine::sblr::DecodeSblrDdlCreateFdwRequestV1(request.payload.data(),request.payload.size(),&v,&d))return refuse("SBLR.OPERAND_INVALID",d);auto s=registry->sessions_by_uuid.find(UuidBytesToText(request.header.session_uuid));if(s==registry->sessions_by_uuid.end())return refuse("SECURITY.ACCESS_DENIED","session_hidden");auto ru=UuidBytesToText(v.receipt);auto c=EngineContextForSession(s->second,engine_state,request);c.statement_uuid.canonical=ru;c.statement_metadata_snapshot_engine_owned=true;c.trace_tags.push_back("private_ddl_create_fdw_binder");auto q=scratchbird::engine::internal_api::CompileSblrDdlCreateFdwDescriptor(c,ru,v.occurrence,v.table_occurrence,1);if(!q.ok)return refuse(q.diagnostic.code,q.diagnostic.message_key);result.payload=scratchbird::engine::sblr::EncodeSblrDdlCreateFdwDescriptorV1(q.descriptor,false);if(result.payload.empty())return refuse("DDL.CREATE_FDW_FAILED","FDWD_encode_failed");result.accepted=true;return result;}

SessionOperationResult HandleCoordinateSecurityAlterUser(ServerSessionRegistry*registry,const HostedEngineState&engine_state,const sbps::Frame&request){SessionOperationResult result;result.response_message_type=static_cast<std::uint16_t>(sbps::MessageType::kCoordinateSecurityAlterUserResult);result.response_schema_id=sbps::kSchemaCoordinateSecurityAlterUserResultV1;result.frame_flags=sbps::kFlagResponse|sbps::kFlagFinal;result.session_uuid=request.header.session_uuid;auto refuse=[&](std::string c,std::string d){result.frame_flags|=sbps::kFlagError;result.diagnostics.push_back(sbps::IpcDiagnostic(std::move(c),"parser_server_ipc.security_alter_user_refused","Security user alteration coordination was refused.",{{"detail",std::move(d)}}));return result;};scratchbird::engine::sblr::SblrSecAlterUserRequestV1 v;std::string d;if(!registry||!scratchbird::engine::sblr::DecodeSblrSecAlterUserRequestV1(request.payload.data(),request.payload.size(),&v,&d))return refuse("SBLR.OPERAND.INVALID",d);auto s=registry->sessions_by_uuid.find(UuidBytesToText(request.header.session_uuid));if(s==registry->sessions_by_uuid.end())return refuse("SECURITY.ACCESS_DENIED","session_hidden");auto ru=UuidBytesToText(v.receipt);auto c=EngineContextForSession(s->second,engine_state,request);c.statement_uuid.canonical=ru;c.statement_metadata_snapshot_engine_owned=true;c.trace_tags.push_back("private_sec_alter_user_binder");auto q=scratchbird::engine::internal_api::CompileSblrSecAlterUserDescriptor(c,ru,v.occurrence,1);if(!q.ok)return refuse(q.diagnostic.code,q.diagnostic.message_key);result.payload=scratchbird::engine::sblr::EncodeSblrSecAlterUserDescriptorV1(q.descriptor,false);if(result.payload.empty())return refuse("SECURITY.USER_APPLICATION_FAILED","SAUD_encode_failed");result.accepted=true;return result;}

SessionOperationResult HandleCoordinateSecurityCreateRole(ServerSessionRegistry*registry,const HostedEngineState&engine_state,const sbps::Frame&request){
  SessionOperationResult result; result.response_message_type=static_cast<std::uint16_t>(sbps::MessageType::kCoordinateSecurityCreateRoleResult); result.response_schema_id=sbps::kSchemaCoordinateSecurityCreateRoleResultV1; result.frame_flags=sbps::kFlagResponse|sbps::kFlagFinal; result.session_uuid=request.header.session_uuid;
  auto refuse=[&](std::string c,std::string d){result.frame_flags|=sbps::kFlagError;result.diagnostics.push_back(sbps::IpcDiagnostic(std::move(c),"parser_server_ipc.security_create_role_refused","Security role creation coordination was refused.",{{"detail",std::move(d)}}));return result;};
  scratchbird::engine::sblr::SblrSecCreateRoleRequestV1 v; std::string d;
  if(!registry||!scratchbird::engine::sblr::DecodeSblrSecCreateRoleRequestV1(request.payload.data(),request.payload.size(),&v,&d)) return refuse("SBLR.OPERAND.INVALID",d);
  auto s=registry->sessions_by_uuid.find(UuidBytesToText(request.header.session_uuid)); if(s==registry->sessions_by_uuid.end()) return refuse("SECURITY.ACCESS_DENIED","session_hidden");
  auto ru=UuidBytesToText(v.receipt); auto c=EngineContextForSession(s->second,engine_state,request); c.statement_uuid.canonical=ru; c.statement_metadata_snapshot_engine_owned=true; c.trace_tags.push_back("private_sec_create_role_binder");
  auto q=scratchbird::engine::internal_api::CompileSblrSecCreateRoleDescriptor(c,ru,v.occurrence,v.occurrence); if(!q.ok)return refuse(q.diagnostic.code,q.diagnostic.message_key);
  result.payload=scratchbird::engine::sblr::EncodeSblrSecCreateRoleDescriptorV1(q.descriptor,false); if(result.payload.empty())return refuse("SECURITY.ROLE_APPLICATION_FAILED","SCRD_encode_failed"); result.accepted=true; return result;
}
SessionOperationResult HandleCoordinateSecurityCreatePolicy(ServerSessionRegistry*registry,const HostedEngineState&engine_state,const sbps::Frame&request){SessionOperationResult result;result.response_message_type=static_cast<std::uint16_t>(sbps::MessageType::kCoordinateSecurityCreatePolicyResult);result.response_schema_id=sbps::kSchemaCoordinateSecurityCreatePolicyResultV1;result.frame_flags=sbps::kFlagResponse|sbps::kFlagFinal;result.session_uuid=request.header.session_uuid;auto refuse=[&](std::string c,std::string d){result.frame_flags|=sbps::kFlagError;result.diagnostics.push_back(sbps::IpcDiagnostic(std::move(c),"parser_server_ipc.security_create_policy_refused","Security policy creation coordination was refused.",{{"detail",std::move(d)}}));return result;};scratchbird::engine::sblr::SblrSecCreatePolicyRequestV1 v;std::string d;if(!registry||!scratchbird::engine::sblr::DecodeSblrSecCreatePolicyRequestV1(request.payload.data(),request.payload.size(),&v,&d))return refuse("SBLR.OPERAND.INVALID",d);auto s=registry->sessions_by_uuid.find(UuidBytesToText(request.header.session_uuid));if(s==registry->sessions_by_uuid.end())return refuse("SECURITY.ACCESS_DENIED","session_hidden");auto ru=UuidBytesToText(v.receipt);auto c=EngineContextForSession(s->second,engine_state,request);c.statement_uuid.canonical=ru;c.statement_metadata_snapshot_engine_owned=true;c.trace_tags.push_back("private_sec_create_policy_binder");auto q=scratchbird::engine::internal_api::CompileSblrSecCreatePolicyDescriptor(c,ru,v.occurrence,v.occurrence);if(!q.ok)return refuse(q.diagnostic.code,q.diagnostic.message_key);result.payload=scratchbird::engine::sblr::EncodeSblrSecCreatePolicyDescriptorV1(q.descriptor,false);if(result.payload.empty())return refuse("SECURITY.POLICY_APPLICATION_FAILED","SCPD_encode_failed");result.accepted=true;return result;}

SessionOperationResult HandleCoordinateSecurityDropRole(ServerSessionRegistry*registry,const HostedEngineState&engine_state,const sbps::Frame&request){
  SessionOperationResult result; result.response_message_type=static_cast<std::uint16_t>(sbps::MessageType::kCoordinateSecurityDropRoleResult); result.response_schema_id=sbps::kSchemaCoordinateSecurityDropRoleResultV1; result.frame_flags=sbps::kFlagResponse|sbps::kFlagFinal; result.session_uuid=request.header.session_uuid;
  auto refuse=[&](std::string c,std::string d){result.frame_flags|=sbps::kFlagError;result.diagnostics.push_back(sbps::IpcDiagnostic(std::move(c),"parser_server_ipc.security_drop_role_refused","Security role drop coordination was refused.",{{"detail",std::move(d)}}));return result;};
  scratchbird::engine::sblr::SblrSecDropRoleRequestV1 v; std::string d;
  if(!registry||!scratchbird::engine::sblr::DecodeSblrSecDropRoleRequestV1(request.payload.data(),request.payload.size(),&v,&d)) return refuse("SBLR.OPERAND.INVALID",d);
  auto s=registry->sessions_by_uuid.find(UuidBytesToText(request.header.session_uuid)); if(s==registry->sessions_by_uuid.end()) return refuse("SECURITY.ACCESS_DENIED","session_hidden");
  auto ru=UuidBytesToText(v.receipt); auto c=EngineContextForSession(s->second,engine_state,request); c.statement_uuid.canonical=ru; c.statement_metadata_snapshot_engine_owned=true; c.trace_tags.push_back("private_sec_drop_role_binder");
  auto q=scratchbird::engine::internal_api::CompileSblrSecDropRoleDescriptor(c,ru,v.occurrence,v.occurrence); if(!q.ok)return refuse(q.diagnostic.code,q.diagnostic.message_key);
  result.payload=scratchbird::engine::sblr::EncodeSblrSecDropRoleDescriptorV1(q.descriptor,false); if(result.payload.empty())return refuse("SECURITY.ROLE_APPLICATION_FAILED","SDRD_encode_failed"); result.accepted=true; return result;
}
SessionOperationResult HandleCoordinateSecurityAlterRole(ServerSessionRegistry*registry,const HostedEngineState&engine_state,const sbps::Frame&request){SessionOperationResult result;result.response_message_type=351;result.response_schema_id=7364;result.frame_flags=sbps::kFlagResponse|sbps::kFlagFinal;result.session_uuid=request.header.session_uuid;auto refuse=[&](std::string c,std::string d){result.frame_flags|=sbps::kFlagError;result.diagnostics.push_back(sbps::IpcDiagnostic(std::move(c),"parser_server_ipc.security_alter_role_refused","Security role alteration coordination was refused.",{{"detail",std::move(d)}}));return result;};scratchbird::engine::sblr::SblrSecAlterRoleRequestV1 v;std::string d;if(!registry||!scratchbird::engine::sblr::DecodeSblrSecAlterRoleRequestV1(request.payload.data(),request.payload.size(),&v,&d))return refuse("SBLR.OPERAND.INVALID",d);auto s=registry->sessions_by_uuid.find(UuidBytesToText(request.header.session_uuid));if(s==registry->sessions_by_uuid.end())return refuse("SECURITY.ACCESS_DENIED","session_hidden");auto ru=UuidBytesToText(v.receipt);auto c=EngineContextForSession(s->second,engine_state,request);c.statement_uuid.canonical=ru;c.statement_metadata_snapshot_engine_owned=true;c.trace_tags.push_back("private_sec_alter_role_binder");auto q=scratchbird::engine::internal_api::CompileSblrSecAlterRoleDescriptor(c,ru,v.occurrence,v.occurrence);if(!q.ok)return refuse(q.diagnostic.code,q.diagnostic.message_key);result.payload=scratchbird::engine::sblr::EncodeSblrSecAlterRoleDescriptorV1(q.descriptor,false);if(result.payload.empty())return refuse("SECURITY.ROLE_APPLICATION_FAILED","SARD_encode_failed");result.accepted=true;return result;}

SessionOperationResult HandleCoordinateSecurityDropPolicy(ServerSessionRegistry* registry,const HostedEngineState& engine_state,const sbps::Frame& request){SessionOperationResult result;result.response_message_type=348+1;result.response_schema_id=7362;result.frame_flags=sbps::kFlagResponse|sbps::kFlagFinal;result.session_uuid=request.header.session_uuid;auto refuse=[&](std::string c,std::string d){result.frame_flags|=sbps::kFlagError;result.diagnostics.push_back(sbps::IpcDiagnostic(std::move(c),"parser_server_ipc.security_drop_policy_refused","Security policy drop coordination was refused.",{{"detail",std::move(d)}}));return result;};scratchbird::engine::sblr::SblrSecDropPolicyRequestV1 v;std::string d;if(!registry||!scratchbird::engine::sblr::DecodeSblrSecDropPolicyRequestV1(request.payload.data(),request.payload.size(),&v,&d))return refuse("SBLR.OPERAND.INVALID",d);auto s=registry->sessions_by_uuid.find(UuidBytesToText(request.header.session_uuid));if(s==registry->sessions_by_uuid.end())return refuse("SECURITY.ACCESS_DENIED","session_hidden");auto ru=UuidBytesToText(v.receipt);auto c=EngineContextForSession(s->second,engine_state,request);c.statement_uuid.canonical=ru;c.statement_metadata_snapshot_engine_owned=true;c.trace_tags.push_back("private_sec_drop_policy_binder");auto q=scratchbird::engine::internal_api::CompileSblrSecDropPolicyDescriptor(c,ru,v.occurrence,v.occurrence);if(!q.ok)return refuse(q.diagnostic.code,q.diagnostic.message_key);result.payload=scratchbird::engine::sblr::EncodeSblrSecDropPolicyDescriptorV1(q.descriptor,false);if(result.payload.empty())return refuse("SECURITY.POLICY_APPLICATION_FAILED","SDPD_encode_failed");result.accepted=true;return result;}

SessionOperationResult HandleCoordinateSecurityCreateGroupMapping(ServerSessionRegistry*registry,const HostedEngineState&engine_state,const sbps::Frame&request){SessionOperationResult result;result.response_message_type=353;result.response_schema_id=7366;result.frame_flags=sbps::kFlagResponse|sbps::kFlagFinal;result.session_uuid=request.header.session_uuid;auto refuse=[&](std::string c,std::string d){result.frame_flags|=sbps::kFlagError;result.diagnostics.push_back(sbps::IpcDiagnostic(std::move(c),"parser_server_ipc.security_create_group_mapping_refused","Group mapping coordination was refused.",{{"detail",std::move(d)}}));return result;};scratchbird::engine::sblr::SblrSecCreateGroupMappingRequestV1 v;std::string d;if(!registry||!scratchbird::engine::sblr::DecodeSblrSecCreateGroupMappingRequestV1(request.payload.data(),request.payload.size(),&v,&d))return refuse("SBLR.OPERAND.INVALID",d);auto s=registry->sessions_by_uuid.find(UuidBytesToText(request.header.session_uuid));if(s==registry->sessions_by_uuid.end())return refuse("SECURITY.ACCESS_DENIED","session_hidden");auto ru=UuidBytesToText(v.receipt);auto c=EngineContextForSession(s->second,engine_state,request);c.statement_uuid.canonical=ru;c.statement_metadata_snapshot_engine_owned=true;c.trace_tags.push_back("private_sec_create_group_mapping_binder");auto q=scratchbird::engine::internal_api::CompileSblrSecCreateGroupMappingDescriptor(c,ru,v.occurrence,v.occurrence);if(!q.ok)return refuse(q.diagnostic.code,q.diagnostic.message_key);result.payload=scratchbird::engine::sblr::EncodeSblrSecCreateGroupMappingDescriptorV1(q.descriptor,false);if(result.payload.empty())return refuse("SECURITY.GROUP_MAPPING_FAILED","SGMD_encode_failed");result.accepted=true;return result;}
SessionOperationResult HandleCoordinateSecurityDropGroupMapping(ServerSessionRegistry*registry,const HostedEngineState&engine_state,const sbps::Frame&request){SessionOperationResult result;result.response_message_type=355;result.response_schema_id=7368;result.frame_flags=sbps::kFlagResponse|sbps::kFlagFinal;result.session_uuid=request.header.session_uuid;auto refuse=[&](std::string c,std::string d){result.frame_flags|=sbps::kFlagError;result.diagnostics.push_back(sbps::IpcDiagnostic(std::move(c),"parser_server_ipc.security_drop_group_mapping_refused","Group mapping drop coordination was refused.",{{"detail",std::move(d)}}));return result;};scratchbird::engine::sblr::SblrSecDropGroupMappingRequestV1 v;std::string d;if(!registry||!scratchbird::engine::sblr::DecodeSblrSecDropGroupMappingRequestV1(request.payload.data(),request.payload.size(),&v,&d))return refuse("SBLR.OPERAND.INVALID",d);auto s=registry->sessions_by_uuid.find(UuidBytesToText(request.header.session_uuid));if(s==registry->sessions_by_uuid.end())return refuse("SECURITY.ACCESS_DENIED","session_hidden");auto ru=UuidBytesToText(v.receipt);auto c=EngineContextForSession(s->second,engine_state,request);c.statement_uuid.canonical=ru;c.statement_metadata_snapshot_engine_owned=true;c.trace_tags.push_back("private_sec_drop_group_mapping_binder");auto q=scratchbird::engine::internal_api::CompileSblrSecDropGroupMappingDescriptor(c,ru,v.occurrence,v.occurrence);if(!q.ok)return refuse(q.diagnostic.code,q.diagnostic.message_key);result.payload=scratchbird::engine::sblr::EncodeSblrSecDropGroupMappingDescriptorV1(q.descriptor,false);if(result.payload.empty())return refuse("SECURITY.GROUP_MAPPING_FAILED","SGDD_encode_failed");result.accepted=true;return result;}
SessionOperationResult HandleCoordinateSecurityGrant(ServerSessionRegistry*registry,const HostedEngineState&engine_state,const sbps::Frame&request){SessionOperationResult result;result.response_message_type=357;result.response_schema_id=7370;result.frame_flags=sbps::kFlagResponse|sbps::kFlagFinal;result.session_uuid=request.header.session_uuid;auto refuse=[&](std::string c,std::string d){result.frame_flags|=sbps::kFlagError;result.diagnostics.push_back(sbps::IpcDiagnostic(std::move(c),"parser_server_ipc.security_grant_refused","Security grant coordination was refused.",{{"detail",std::move(d)}}));return result;};scratchbird::engine::sblr::SblrSecGrantRequestV1 v;std::string d;if(!registry||!scratchbird::engine::sblr::DecodeSblrSecGrantRequestV1(request.payload.data(),request.payload.size(),&v,&d))return refuse("SBLR.OPERAND.INVALID",d);auto s=registry->sessions_by_uuid.find(UuidBytesToText(request.header.session_uuid));if(s==registry->sessions_by_uuid.end())return refuse("SECURITY.ACCESS_DENIED","session_hidden");auto ru=UuidBytesToText(v.receipt);auto c=EngineContextForSession(s->second,engine_state,request);c.statement_uuid.canonical=ru;c.statement_metadata_snapshot_engine_owned=true;c.trace_tags.push_back("private_sec_grant_binder");auto q=scratchbird::engine::internal_api::CompileSblrSecGrantDescriptor(c,ru,v.occurrence,v.occurrence);if(!q.ok)return refuse(q.diagnostic.code,q.diagnostic.message_key);result.payload=scratchbird::engine::sblr::EncodeSblrSecGrantDescriptorV1(q.descriptor,false);if(result.payload.empty())return refuse("SECURITY.GRANT_FAILED","SGRD_encode_failed");result.accepted=true;return result;}
SessionOperationResult HandleCoordinateSecurityRevoke(ServerSessionRegistry*registry,const HostedEngineState&engine_state,const sbps::Frame&request){SessionOperationResult result;result.response_message_type=359;result.response_schema_id=7372;result.frame_flags=sbps::kFlagResponse|sbps::kFlagFinal;result.session_uuid=request.header.session_uuid;auto refuse=[&](std::string c,std::string d){result.frame_flags|=sbps::kFlagError;result.diagnostics.push_back(sbps::IpcDiagnostic(std::move(c),"parser_server_ipc.security_revoke_refused","Security revoke coordination was refused.",{{"detail",std::move(d)}}));return result;};scratchbird::engine::sblr::SblrSecRevokeRequestV1 v;std::string d;if(!registry||!scratchbird::engine::sblr::DecodeSblrSecRevokeRequestV1(request.payload.data(),request.payload.size(),&v,&d))return refuse("SBLR.OPERAND.INVALID",d);auto s=registry->sessions_by_uuid.find(UuidBytesToText(request.header.session_uuid));if(s==registry->sessions_by_uuid.end())return refuse("SECURITY.ACCESS_DENIED","session_hidden");auto ru=UuidBytesToText(v.receipt);auto c=EngineContextForSession(s->second,engine_state,request);c.statement_uuid.canonical=ru;c.statement_metadata_snapshot_engine_owned=true;c.trace_tags.push_back("private_sec_revoke_binder");auto q=scratchbird::engine::internal_api::CompileSblrSecRevokeDescriptor(c,ru,v.occurrence,v.occurrence);if(!q.ok)return refuse(q.diagnostic.code,q.diagnostic.message_key);result.payload=scratchbird::engine::sblr::EncodeSblrSecRevokeDescriptorV1(q.descriptor,false);if(result.payload.empty())return refuse("SECURITY.REVOKE_FAILED","SRVD_encode_failed");result.accepted=true;return result;}
SessionOperationResult HandleCoordinateSecurityAlterPolicy(ServerSessionRegistry*registry,const HostedEngineState&engine_state,const sbps::Frame&request){SessionOperationResult result;result.response_message_type=361;result.response_schema_id=7374;result.frame_flags=sbps::kFlagResponse|sbps::kFlagFinal;result.session_uuid=request.header.session_uuid;auto refuse=[&](std::string c,std::string d){result.frame_flags|=sbps::kFlagError;result.diagnostics.push_back(sbps::IpcDiagnostic(std::move(c),"parser_server_ipc.security_alter_policy_refused","Security policy alteration coordination was refused.",{{"detail",std::move(d)}}));return result;};scratchbird::engine::sblr::SblrSecAlterPolicyRequestV1 v;std::string d;if(!registry||!scratchbird::engine::sblr::DecodeSblrSecAlterPolicyRequestV1(request.payload.data(),request.payload.size(),&v,&d))return refuse("SBLR.OPERAND.INVALID",d);auto s=registry->sessions_by_uuid.find(UuidBytesToText(request.header.session_uuid));if(s==registry->sessions_by_uuid.end())return refuse("SECURITY.ACCESS_DENIED","session_hidden");auto ru=UuidBytesToText(v.receipt);auto c=EngineContextForSession(s->second,engine_state,request);c.statement_uuid.canonical=ru;c.statement_metadata_snapshot_engine_owned=true;c.trace_tags.push_back("private_sec_alter_policy_binder");auto q=scratchbird::engine::internal_api::CompileSblrSecAlterPolicyDescriptor(c,ru,v.occurrence,v.occurrence);if(!q.ok)return refuse(q.diagnostic.code,q.diagnostic.message_key);result.payload=scratchbird::engine::sblr::EncodeSblrSecAlterPolicyDescriptorV1(q.descriptor,false);if(result.payload.empty())return refuse("SECURITY.POLICY_FAILED","SAPD_encode_failed");result.accepted=true;return result;}
SessionOperationResult HandleCoordinateSecurityDropUser(ServerSessionRegistry*registry,const HostedEngineState&engine_state,const sbps::Frame&request){SessionOperationResult result;result.response_message_type=363;result.response_schema_id=7376;result.frame_flags=sbps::kFlagResponse|sbps::kFlagFinal;result.session_uuid=request.header.session_uuid;auto refuse=[&](std::string c,std::string d){result.frame_flags|=sbps::kFlagError;result.diagnostics.push_back(sbps::IpcDiagnostic(std::move(c),"parser_server_ipc.security_drop_user_refused","Security user drop coordination was refused.",{{"detail",std::move(d)}}));return result;};scratchbird::engine::sblr::SblrSecDropUserRequestV1 v;std::string d;if(!registry||!scratchbird::engine::sblr::DecodeSblrSecDropUserRequestV1(request.payload.data(),request.payload.size(),&v,&d))return refuse("SBLR.OPERAND.INVALID",d);auto s=registry->sessions_by_uuid.find(UuidBytesToText(request.header.session_uuid));if(s==registry->sessions_by_uuid.end())return refuse("SECURITY.ACCESS_DENIED","session_hidden");auto ru=UuidBytesToText(v.receipt);auto c=EngineContextForSession(s->second,engine_state,request);c.statement_uuid.canonical=ru;c.statement_metadata_snapshot_engine_owned=true;c.trace_tags.push_back("private_sec_drop_user_binder");auto q=scratchbird::engine::internal_api::CompileSblrSecDropUserDescriptor(c,ru,v.occurrence,v.occurrence);if(!q.ok)return refuse(q.diagnostic.code,q.diagnostic.message_key);result.payload=scratchbird::engine::sblr::EncodeSblrSecDropUserDescriptorV1(q.descriptor,false);if(result.payload.empty())return refuse("SECURITY.USER_FAILED","SDUD_encode_failed");result.accepted=true;return result;}
SessionOperationResult HandleCoordinateSecurityAuthenticate(ServerSessionRegistry*registry,const HostedEngineState&engine_state,const sbps::Frame&request){SessionOperationResult result;result.response_message_type=365;result.response_schema_id=7378;result.frame_flags=sbps::kFlagResponse|sbps::kFlagFinal;result.session_uuid=request.header.session_uuid;auto refuse=[&](std::string c,std::string d){result.frame_flags|=sbps::kFlagError;result.diagnostics.push_back(sbps::IpcDiagnostic(std::move(c),"parser_server_ipc.security_authenticate_refused","Security authenticate coordination was refused.",{{"detail",std::move(d)}}));return result;};scratchbird::engine::sblr::SblrSecAuthenticateRequestV1 v;std::string d;if(!registry||!scratchbird::engine::sblr::DecodeSblrSecAuthenticateRequestV1(request.payload.data(),request.payload.size(),&v,&d))return refuse("SBLR.OPERAND.INVALID",d);auto s=registry->sessions_by_uuid.find(UuidBytesToText(request.header.session_uuid));if(s==registry->sessions_by_uuid.end())return refuse("SECURITY.ACCESS_DENIED","session_hidden");auto ru=UuidBytesToText(v.receipt);auto c=EngineContextForSession(s->second,engine_state,request);c.statement_uuid.canonical=ru;c.statement_metadata_snapshot_engine_owned=true;c.trace_tags.push_back("private_sec_authenticate_binder");auto q=scratchbird::engine::internal_api::CompileSblrSecAuthenticateDescriptor(c,ru,v.occurrence,v.occurrence);if(!q.ok)return refuse(q.diagnostic.code,q.diagnostic.message_key);result.payload=scratchbird::engine::sblr::EncodeSblrSecAuthenticateDescriptorV1(q.descriptor,false);if(result.payload.empty())return refuse("SECURITY.AUTHENTICATE_FAILED","SAUD_encode_failed");result.accepted=true;return result;}
SessionOperationResult HandleCoordinateSecurityDeauthenticate(ServerSessionRegistry*registry,const HostedEngineState&engine_state,const sbps::Frame&request){SessionOperationResult result;result.response_message_type=367;result.response_schema_id=7380;result.frame_flags=sbps::kFlagResponse|sbps::kFlagFinal;result.session_uuid=request.header.session_uuid;auto refuse=[&](std::string c,std::string d){result.frame_flags|=sbps::kFlagError;result.diagnostics.push_back(sbps::IpcDiagnostic(std::move(c),"parser_server_ipc.security_deauthenticate_refused","Security deauthenticate coordination was refused.",{{"detail",std::move(d)}}));return result;};scratchbird::engine::sblr::SblrSecDeauthenticateRequestV1 v;std::string d;if(!registry||!scratchbird::engine::sblr::DecodeSblrSecDeauthenticateRequestV1(request.payload.data(),request.payload.size(),&v,&d))return refuse("SBLR.OPERAND.INVALID",d);auto s=registry->sessions_by_uuid.find(UuidBytesToText(request.header.session_uuid));if(s==registry->sessions_by_uuid.end())return refuse("SECURITY.ACCESS_DENIED","session_hidden");auto ru=UuidBytesToText(v.receipt);auto c=EngineContextForSession(s->second,engine_state,request);c.statement_uuid.canonical=ru;c.statement_metadata_snapshot_engine_owned=true;c.trace_tags.push_back("private_sec_deauthenticate_binder");auto q=scratchbird::engine::internal_api::CompileSblrSecDeauthenticateDescriptor(c,ru,v.occurrence,v.occurrence);if(!q.ok)return refuse(q.diagnostic.code,q.diagnostic.message_key);result.payload=scratchbird::engine::sblr::EncodeSblrSecDeauthenticateDescriptorV1(q.descriptor,false);if(result.payload.empty())return refuse("SECURITY.DEAUTHENTICATE_FAILED","SADD_encode_failed");result.accepted=true;return result;}
SessionOperationResult HandleSessionRoleSwitch(ServerSessionRegistry*registry,const HostedEngineState&engine_state,const sbps::Frame&request){SessionOperationResult result;result.response_message_type=369;result.response_schema_id=7382;result.frame_flags=sbps::kFlagResponse|sbps::kFlagFinal;result.session_uuid=request.header.session_uuid;auto refuse=[&](std::string c,std::string d){result.frame_flags|=sbps::kFlagError;result.diagnostics.push_back(sbps::IpcDiagnostic(std::move(c),"parser_server_ipc.session_role_switch_refused","Session role switch coordination was refused.",{{"detail",std::move(d)}}));return result;};scratchbird::engine::sblr::SblrSessionRoleSwitchRequestV1 v;std::string d;if(!registry||!scratchbird::engine::sblr::DecodeSblrSessionRoleSwitchRequestV1(request.payload.data(),request.payload.size(),&v,&d))return refuse("SBLR.OPERAND.INVALID",d);auto s=registry->sessions_by_uuid.find(UuidBytesToText(request.header.session_uuid));if(s==registry->sessions_by_uuid.end())return refuse("SECURITY.ACCESS_DENIED","session_hidden");auto ru=UuidBytesToText(v.receipt);auto c=EngineContextForSession(s->second,engine_state,request);c.statement_uuid.canonical=ru;c.statement_metadata_snapshot_engine_owned=true;c.trace_tags.push_back("private_sec_role_switch_binder");auto q=scratchbird::engine::internal_api::CompileSblrSessionRoleSwitchDescriptor(c,ru,v.occurrence,v.occurrence);if(!q.ok)return refuse(q.diagnostic.code,q.diagnostic.message_key);result.payload=scratchbird::engine::sblr::EncodeSblrSessionRoleSwitchDescriptorV1(q.descriptor,false);if(result.payload.empty())return refuse("SECURITY.ROLE_SWITCH_FAILED","SRSD_encode_failed");result.accepted=true;return result;}
SessionOperationResult HandleSessionSettingSet(ServerSessionRegistry*registry,const HostedEngineState&engine_state,const sbps::Frame&request){SessionOperationResult result;result.response_message_type=371;result.response_schema_id=7384;result.frame_flags=sbps::kFlagResponse|sbps::kFlagFinal;result.session_uuid=request.header.session_uuid;auto refuse=[&](std::string c,std::string d){result.frame_flags|=sbps::kFlagError;result.diagnostics.push_back(sbps::IpcDiagnostic(std::move(c),"parser_server_ipc.session_setting_set_refused","Session setting set coordination was refused.",{{"detail",std::move(d)}}));return result;};scratchbird::engine::sblr::SblrSessionSettingSetRequestV1 v;std::string d;if(!registry||!scratchbird::engine::sblr::DecodeSblrSessionSettingSetRequestV1(request.payload.data(),request.payload.size(),&v,&d))return refuse("SBLR.OPERAND.INVALID",d);auto s=registry->sessions_by_uuid.find(UuidBytesToText(request.header.session_uuid));if(s==registry->sessions_by_uuid.end())return refuse("SECURITY.ACCESS_DENIED","session_hidden");auto ru=UuidBytesToText(v.receipt);auto c=EngineContextForSession(s->second,engine_state,request);c.statement_uuid.canonical=ru;c.statement_metadata_snapshot_engine_owned=true;c.trace_tags.push_back("private_session_setting_set_binder");auto q=scratchbird::engine::internal_api::CompileSblrSessionSettingSetDescriptor(c,ru,v.occurrence,v.occurrence);if(!q.ok)return refuse(q.diagnostic.code,q.diagnostic.message_key);result.payload=scratchbird::engine::sblr::EncodeSblrSessionSettingSetDescriptorV1(q.descriptor,false);if(result.payload.empty())return refuse("SESSION.SET_FAILED","SSSD_encode_failed");result.accepted=true;return result;}
SessionOperationResult HandleSessionSettingReset(ServerSessionRegistry*registry,const HostedEngineState&engine_state,const sbps::Frame&request){auto result=HandleSessionSettingSet(registry,engine_state,request);result.response_message_type=373;result.response_schema_id=7386;return result;}
SessionOperationResult HandleSessionSettingGet(ServerSessionRegistry*registry,const HostedEngineState&engine_state,const sbps::Frame&request){auto result=HandleSessionSettingSet(registry,engine_state,request);result.response_message_type=375;result.response_schema_id=7388;return result;}
} // namespace scratchbird::server
