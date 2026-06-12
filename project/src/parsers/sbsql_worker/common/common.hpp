// Copyright (c) 2026 ScratchBird Software Inc.
//
// This Source Code Form is subject to the terms of the Mozilla Public
// License, v. 2.0. If a copy of the MPL was not distributed with this
// file, You can obtain one at https://mozilla.org/MPL/2.0/.
//
// SPDX-License-Identifier: MPL-2.0

#pragma once

#include <cstdint>
#include <cstddef>
#include <string>
#include <string_view>
#include <utility>
#include <vector>

#include "parser_ipc_common.hpp"

namespace scratchbird::parser::sbsql {

inline constexpr std::uint32_t kSbsqlWorkerParserApiCurrentMajor = 1;
inline constexpr std::uint32_t kSbsqlWorkerProtocolCurrentVersion = 1;
inline constexpr std::uint32_t kSbsqlWorkerProtocolMinSupported = 1;
inline constexpr std::uint32_t kSbsqlWorkerProtocolMaxSupported = 1;
inline constexpr std::uint32_t kSbsqlWorkerRegistryCurrentVersion = 1;

enum class ParserState {
  kSpawned,
  kPackageAdmitted,
  kInitializing,
  kWireReady,
  kIdlePreAuth,
  kClientConnected,
  kAuthenticating,
  kAuthenticated,
  kActive,
  kDraining,
  kRecycled,
  kDisconnected,
  kTerminating,
  kFailed,
  kQuarantined,
};

struct ParserResourceBudget {
  std::uint64_t max_statement_bytes{1024 * 1024};
  std::uint64_t max_identifier_bytes{256};
  std::uint64_t max_token_count{131072};
  std::uint64_t max_literal_bytes{1024 * 1024};
  std::uint64_t max_ast_depth{256};
  std::uint64_t max_parameter_count{65535};
  std::uint64_t max_sblr_envelope_bytes{16 * 1024 * 1024};
  std::uint64_t max_diagnostic_payload_bytes{64 * 1024};
  std::uint64_t max_message_vector_count{1024};
  std::uint64_t max_result_metadata_columns{4096};
  std::uint64_t max_render_output_bytes{1024 * 1024};
  std::uint64_t max_parser_cache_entries{10000};
};

struct ParserConfig {
  bool listener_worker{false};
  bool probe_mode{false};
  bool allow_probe_auth{false};
  std::intptr_t listener_control_fd{-1};
  std::string listener_control_socket;
  std::uint64_t worker_numeric_id{1};
  std::string parser_uuid;
  std::string listener_uuid;
  std::string database_token;
  std::string server_endpoint;
  bool embedded_engine_direct{false};
  bool embedded_auth_bypass_sysarch{false};
  bool embedded_database_ownership_prelocked{false};
  std::string embedded_database_path;
  std::string dialect{"sbsql"};
  std::string profile_id{"default"};
  std::string bundle_contract_id{"sbp_sbsql@1"};
  std::string build_id{"dev"};
  bool tls_required{false};
  std::string tls_cert_file;
  std::string tls_key_file;
  std::string tls_ca_file;
  std::string manager_auth_provider_family;
  std::string manager_auth_principal;
  std::string manager_auth_token;
  std::uint32_t parser_api_major{kSbsqlWorkerParserApiCurrentMajor};
  std::uint32_t protocol_version{kSbsqlWorkerProtocolCurrentVersion};
  std::uint32_t registry_version{kSbsqlWorkerRegistryCurrentVersion};
  std::uint32_t metrics_schema_version{1};
  ParserResourceBudget resource_budget;
};

struct SessionContext {
  bool authenticated{false};
  std::string session_uuid;
  std::string connection_uuid;
  std::string database_uuid;
  std::string authenticated_user_uuid;
  std::string principal_claim;
  std::string auth_provider_family;
  std::vector<std::string> effective_role_uuids;
  std::vector<std::string> effective_group_uuids;
  std::string default_language{"en"};
  std::string language_profile{"sbsql.builtin.recovery.en"};
  std::string language_tag{"en"};
  std::string common_resource_hash{"builtin.common.sbsql.v1"};
  std::string dialect_profile_uuid;
  std::string policy_profile_uuid{"default"};
  std::uint64_t language_resource_epoch{0};
  std::uint64_t localized_name_epoch{0};
  std::uint64_t message_resource_epoch{0};
  std::uint64_t udr_epoch{0};
  std::vector<std::string> search_path;
  std::string transaction_context;
  std::uint64_t local_transaction_id{0};
  std::uint64_t snapshot_visible_through_local_transaction_id{0};
  std::string transaction_uuid;
  std::string transaction_timestamp;
  std::uint64_t security_policy_epoch{0};
  std::uint64_t grant_epoch{0};
  std::uint64_t catalog_epoch{0};
  std::uint64_t descriptor_epoch{0};
  std::string result_rendering_policy{"default"};
  std::string metric_redaction_policy{"default"};
};

struct PipelineResult {
  bool accepted{false};
  bool frontdoor_cache_hit{false};
  bool parser_executes_sql{false};
  bool cached_storage_authority{false};
  bool cached_authorization_authority{false};
  bool cached_finality_authority{false};
  std::string statement_family;
  std::string operation_family;
  std::uint64_t statement_hash{0};
  std::string sblr_payload;
  std::string server_operation_id;
  std::string server_cursor_uuid;
  std::uint64_t server_row_count{0};
  std::string server_result_payload;
  MessageVectorSet messages;
};

std::string StateName(ParserState state);
std::string TrimAscii(std::string_view text);
std::uint64_t Fnv1a64(std::string_view text);

} // namespace scratchbird::parser::sbsql
