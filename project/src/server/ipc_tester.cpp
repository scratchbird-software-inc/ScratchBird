// Copyright (c) 2026 ScratchBird Software Inc.
//
// This Source Code Form is subject to the terms of the Mozilla Public
// License, v. 2.0. If a copy of the MPL was not distributed with this
// file, You can obtain one at https://mozilla.org/MPL/2.0/.
//
// SPDX-License-Identifier: MPL-2.0

// SEARCH_KEY: SB_SERVER_IPC_TESTER

#include "sbps.hpp"
#include "manager_control.hpp"
#include "session_registry.hpp"
#include "sblr_dispatch_server.hpp"

#include <array>
#include <filesystem>
#include <cstdlib>
#include <cstring>
#include <fstream>
#include <iostream>
#include <map>
#include <optional>
#include <string>
#include <utility>
#include <vector>

#ifndef _WIN32
#include <sys/socket.h>
#include <sys/un.h>
#include <unistd.h>
#endif

namespace {

using scratchbird::server::sbps::FrameHeader;

struct Options {
  std::string endpoint;
  std::string scenario = "hello";
  std::string expected = "accept";
  std::string expected_code;
  std::string expected_payload_contains;
  std::string fixture;
  std::string principal = "alice";
  std::string principal_uuid;
};

std::string JsonEscape(const std::string& value) {
  return scratchbird::server::EscapeMessageVectorText(value);
}

void Usage() {
  std::cout << "Usage: sb_ipc_tester --endpoint PATH [--scenario hello|malformed_magic|unsupported_message|version_mismatch|payload_crc_mismatch|database_status|session_registry_status|parser_registry_status|notification_router_status|listener_orchestrator_status|server_management_rights|hello_disabled_package|hello_quarantined_package|hello_retired_package|hello_hash_failure|hello_dev_warning|hello_udr_missing|auth_success|auth_failure|auth_challenge|attach_without_auth|auth_then_attach|auth_attach_detach|detach_unknown|sblr_prepare_execute_show_version|sblr_raw_sql_rejected|sblr_fetch_close_show_version|sblr_cluster_refused|sblr_crud_insert|sblr_crud_select|sblr_crud_update|sblr_crud_delete|sblr_catalog_create_table|sblr_catalog_get_descriptor|sblr_index_create|sblr_datatype_cast|sblr_datatype_extract|sblr_datatype_set|sblr_optimizer_explain|sblr_optimizer_plan|sblr_llvm_compile|event_subscribe_unsubscribe|event_notify_delivery|management_show_server_status|management_show_listeners|management_show_metrics|management_export_support_bundle|management_start_listener|management_restart_listener|management_listener_proxy_refused|management_unauthorized_start_listener|management_show_server_lifecycle|management_reload_server_config|management_reload_invalid|management_drain_server|management_set_maintenance|management_clear_maintenance|management_begin_backup_fence|management_end_backup_fence|management_begin_restore_fence|management_end_restore_fence|management_cancel_unknown|management_stop_server|management_restart_server] [--principal PRINCIPAL] [--principal-uuid UUID] [--expect accept|error] [--expect-code CODE] [--expect-payload-contains TEXT]\n"
               "       sb_ipc_tester --fixture PATH\n";
}

void PutU16(std::vector<std::uint8_t>* out, std::uint16_t value) {
  out->push_back(static_cast<std::uint8_t>(value & 0xffu));
  out->push_back(static_cast<std::uint8_t>((value >> 8u) & 0xffu));
}

void PutString(std::vector<std::uint8_t>* out, const std::string& value) {
  PutU16(out, static_cast<std::uint16_t>(value.size()));
  out->insert(out->end(), value.begin(), value.end());
}

std::vector<std::uint8_t> EncodeEventFieldPayloadForTest(
    const std::vector<std::pair<std::string, std::string>>& fields) {
  std::vector<std::uint8_t> out;
  PutU16(&out, static_cast<std::uint16_t>(fields.size()));
  for (const auto& field : fields) {
    PutString(&out, field.first);
    PutString(&out, field.second);
  }
  return out;
}

std::map<std::string, std::string> ReadFixture(const std::string& path) {
  std::ifstream in(path);
  std::map<std::string, std::string> values;
  std::string line;
  while (std::getline(in, line)) {
    if (line.empty() || line[0] == '#') continue;
    const auto eq = line.find('=');
    if (eq == std::string::npos) continue;
    values[line.substr(0, eq)] = line.substr(eq + 1);
  }
  return values;
}

Options Parse(int argc, char** argv) {
  Options options;
  for (int i = 1; i < argc; ++i) {
    const std::string arg = argv[i] == nullptr ? "" : argv[i];
    auto value = [&](const char* name) -> std::string {
      if (i + 1 >= argc || argv[i + 1] == nullptr) {
        std::cerr << "missing value for " << name << "\n";
        return {};
      }
      ++i;
      return argv[i];
    };
    if (arg == "--endpoint") options.endpoint = value("--endpoint");
    else if (arg == "--scenario") options.scenario = value("--scenario");
    else if (arg == "--expect") options.expected = value("--expect");
    else if (arg == "--expect-code") options.expected_code = value("--expect-code");
    else if (arg == "--expect-payload-contains") options.expected_payload_contains = value("--expect-payload-contains");
    else if (arg == "--principal") options.principal = value("--principal");
    else if (arg == "--principal-uuid") options.principal_uuid = value("--principal-uuid");
    else if (arg == "--fixture") options.fixture = value("--fixture");
    else if (arg == "--help") {
      Usage();
      std::exit(0);
    } else {
      std::cerr << "unknown option: " << arg << "\n";
      std::exit(2);
    }
  }
  if (!options.fixture.empty()) {
    const auto values = ReadFixture(options.fixture);
    if (values.contains("endpoint")) options.endpoint = values.at("endpoint");
    if (values.contains("scenario")) options.scenario = values.at("scenario");
    if (values.contains("expect")) options.expected = values.at("expect");
    if (values.contains("expected_code")) options.expected_code = values.at("expected_code");
    if (values.contains("expected_payload_contains")) options.expected_payload_contains = values.at("expected_payload_contains");
    if (values.contains("principal")) options.principal = values.at("principal");
    if (values.contains("principal_uuid")) options.principal_uuid = values.at("principal_uuid");
  }
  return options;
}

std::vector<std::uint8_t> BuildRequest(
    const std::string& scenario,
    const std::optional<std::array<std::uint8_t, 16>>& auth_context_uuid = std::nullopt,
    const std::optional<std::array<std::uint8_t, 16>>& session_uuid = std::nullopt,
    const std::optional<std::array<std::uint8_t, 16>>& prepared_statement_uuid = std::nullopt,
    const std::optional<std::array<std::uint8_t, 16>>& cursor_uuid = std::nullopt,
    const std::string& principal = "alice",
    const std::string& principal_uuid = "") {
  std::vector<std::uint8_t> payload = scratchbird::server::sbps::EncodeHelloRequestForTest();
  FrameHeader header;
  header.message_type = static_cast<std::uint16_t>(scratchbird::server::sbps::MessageType::kHello);
  header.payload_schema_id = scratchbird::server::sbps::kSchemaHelloRequestV1;
  header.sequence_number = 1;
  header.request_uuid = scratchbird::server::sbps::MakeUuidV7Bytes();
  if (scenario == "unsupported_message") {
    header.message_type = 47;
    payload.clear();
    header.payload_schema_id = 0;
  } else if (scenario == "database_status" || scenario == "session_registry_status" ||
             scenario == "parser_registry_status" || scenario == "notification_router_status" ||
             scenario == "listener_orchestrator_status" || scenario == "server_management_rights") {
    const std::string status_request = scenario == "database_status" ? "database_status"
                                       : scenario == "session_registry_status" ? "session_registry_status"
                                       : scenario == "parser_registry_status" ? "parser_registry_status"
                                       : scenario == "notification_router_status" ? "notification_router_status"
                                       : scenario == "listener_orchestrator_status" ? "listener_orchestrator_status"
                                                                                    : "server_management_rights";
    payload.assign(status_request.begin(), status_request.end());
    header.message_type = static_cast<std::uint16_t>(scratchbird::server::sbps::MessageType::kPing);
    header.payload_schema_id = 9001;
  } else if (scenario == "hello_disabled_package" || scenario == "hello_quarantined_package" ||
             scenario == "hello_retired_package" || scenario == "hello_hash_failure" ||
             scenario == "hello_dev_warning" || scenario == "hello_udr_missing") {
    header.message_type = static_cast<std::uint16_t>(scratchbird::server::sbps::MessageType::kHello);
    header.payload_schema_id = scratchbird::server::sbps::kSchemaHelloRequestV1;
  } else if (scenario == "auth_success" || scenario == "auth_then_attach" ||
             scenario == "auth_attach_detach") {
    payload = scratchbird::server::EncodeAuthHandoffPayloadForTest(
        principal, true, false, false, principal_uuid);
    header.message_type =
        static_cast<std::uint16_t>(scratchbird::server::sbps::MessageType::kAuthHandoff);
    header.payload_schema_id = 3001;
  } else if (scenario == "auth_failure") {
    payload = scratchbird::server::EncodeAuthHandoffPayloadForTest(
        principal, false, false, false, principal_uuid);
    header.message_type =
        static_cast<std::uint16_t>(scratchbird::server::sbps::MessageType::kAuthHandoff);
    header.payload_schema_id = 3001;
  } else if (scenario == "auth_challenge") {
    payload = scratchbird::server::EncodeAuthHandoffPayloadForTest(
        principal, true, true, false, principal_uuid);
    header.message_type =
        static_cast<std::uint16_t>(scratchbird::server::sbps::MessageType::kAuthHandoff);
    header.payload_schema_id = 3001;
  } else if (scenario == "attach_without_auth") {
    payload = scratchbird::server::EncodeAttachPayloadForTest(std::array<std::uint8_t, 16>{});
    header.message_type =
        static_cast<std::uint16_t>(scratchbird::server::sbps::MessageType::kAttachDatabase);
    header.payload_schema_id = 3003;
  } else if (scenario == "attach_with_auth") {
    payload = scratchbird::server::EncodeAttachPayloadForTest(auth_context_uuid.value_or(
        std::array<std::uint8_t, 16>{}));
    header.message_type =
        static_cast<std::uint16_t>(scratchbird::server::sbps::MessageType::kAttachDatabase);
    header.payload_schema_id = 3003;
  } else if (scenario == "detach_unknown") {
    payload.assign(16, 0);
    header.message_type =
        static_cast<std::uint16_t>(scratchbird::server::sbps::MessageType::kDisconnectNotice);
    header.payload_schema_id = 3005;
  } else if (scenario == "detach_with_session") {
    const auto uuid = session_uuid.value_or(std::array<std::uint8_t, 16>{});
    header.session_uuid = uuid;
    payload.assign(uuid.begin(), uuid.end());
    header.message_type =
        static_cast<std::uint16_t>(scratchbird::server::sbps::MessageType::kDisconnectNotice);
    header.payload_schema_id = 3005;
  } else if (scenario == "sblr_prepare_show_version") {
    header.session_uuid = session_uuid.value_or(std::array<std::uint8_t, 16>{});
    payload = scratchbird::server::EncodePrepareSblrPayloadForTest(
        header.session_uuid, scratchbird::server::EncodeShowVersionSblrForTest());
    header.message_type =
        static_cast<std::uint16_t>(scratchbird::server::sbps::MessageType::kPrepareSblr);
    header.payload_schema_id = 4001;
  } else if (scenario == "sblr_raw_sql_rejected") {
    header.session_uuid = session_uuid.value_or(std::array<std::uint8_t, 16>{});
    payload = scratchbird::server::EncodePrepareSblrPayloadForTest(
        header.session_uuid, scratchbird::server::EncodeRawSqlSblrBypassForTest());
    header.message_type =
        static_cast<std::uint16_t>(scratchbird::server::sbps::MessageType::kPrepareSblr);
    header.payload_schema_id = 4001;
  } else if (scenario == "sblr_execute_prepared_show_version") {
    header.session_uuid = session_uuid.value_or(std::array<std::uint8_t, 16>{});
    payload = scratchbird::server::EncodeExecuteSblrPayloadForTest(
        header.session_uuid,
        prepared_statement_uuid.value_or(std::array<std::uint8_t, 16>{}),
        "",
        false);
    header.message_type =
        static_cast<std::uint16_t>(scratchbird::server::sbps::MessageType::kExecuteSblr);
    header.payload_schema_id = 4003;
  } else if (scenario == "sblr_execute_cursor_show_version") {
    header.session_uuid = session_uuid.value_or(std::array<std::uint8_t, 16>{});
    payload = scratchbird::server::EncodeExecuteSblrPayloadForTest(
        header.session_uuid,
        {},
        scratchbird::server::EncodeShowVersionSblrForTest(),
        true);
    header.message_type =
        static_cast<std::uint16_t>(scratchbird::server::sbps::MessageType::kExecuteSblr);
    header.payload_schema_id = 4003;
  } else if (scenario == "sblr_fetch") {
    header.session_uuid = session_uuid.value_or(std::array<std::uint8_t, 16>{});
    payload = scratchbird::server::EncodeFetchPayloadForTest(
        header.session_uuid, cursor_uuid.value_or(std::array<std::uint8_t, 16>{}));
    header.message_type =
        static_cast<std::uint16_t>(scratchbird::server::sbps::MessageType::kFetch);
    header.payload_schema_id = 4005;
  } else if (scenario == "management_show_server_status" ||
             scenario == "management_show_listeners" ||
             scenario == "management_show_metrics" ||
             scenario == "management_export_support_bundle" ||
             scenario == "management_start_listener" ||
             scenario == "management_restart_listener" ||
             scenario == "management_listener_proxy_refused" ||
             scenario == "management_unauthorized_start_listener" ||
             scenario == "management_show_server_lifecycle" ||
             scenario == "management_reload_server_config" ||
             scenario == "management_reload_invalid" ||
             scenario == "management_drain_server" ||
             scenario == "management_set_maintenance" ||
             scenario == "management_clear_maintenance" ||
             scenario == "management_set_clear_maintenance" ||
             scenario == "management_begin_backup_fence" ||
             scenario == "management_end_backup_fence" ||
             scenario == "management_begin_restore_fence" ||
             scenario == "management_end_restore_fence" ||
             scenario == "management_cancel_unknown" ||
             scenario == "management_stop_server" ||
             scenario == "management_restart_server") {
    header.session_uuid = session_uuid.value_or(std::array<std::uint8_t, 16>{});
    scratchbird::server::ServerManagementRequest management;
    management.operation_key = scenario == "management_show_server_status" ? "show_server_status"
                             : scenario == "management_show_listeners" ? "show_listeners"
                             : scenario == "management_show_metrics" ? "show_server_metrics"
                             : scenario == "management_export_support_bundle" ? "export_server_support_bundle"
                             : scenario == "management_show_server_lifecycle" ? "show_server_lifecycle"
                             : scenario == "management_reload_server_config" ? "reload_server_config"
                             : scenario == "management_reload_invalid" ? "reload_server_config"
                             : scenario == "management_drain_server" ? "drain_server"
                             : scenario == "management_set_maintenance" ? "set_server_maintenance_mode"
                             : scenario == "management_set_clear_maintenance" ? "set_server_maintenance_mode"
                             : scenario == "management_clear_maintenance" ? "clear_server_maintenance_mode"
                             : scenario == "management_begin_backup_fence" ? "begin_backup_fence"
                             : scenario == "management_end_backup_fence" ? "end_backup_fence"
                             : scenario == "management_begin_restore_fence" ? "begin_restore_fence"
                             : scenario == "management_end_restore_fence" ? "end_restore_fence"
                             : scenario == "management_cancel_unknown" ? "cancel_request"
                             : scenario == "management_stop_server" ? "stop_server"
                             : scenario == "management_restart_server" ? "restart_server"
                             : scenario == "management_restart_listener" ? "restart_listener"
                             : scenario == "management_listener_proxy_refused" ? "listener_proxy_execute"
                                                                               : "start_listener";
    management.target_uuid = scenario == "management_cancel_unknown" ? "00000000-0000-0000-0000-000000000000" : "";
    management.audit_reason = "ipc_tester";
    management.mode = scenario == "management_reload_invalid" ? "invalid_config" : "graceful";
    payload = scratchbird::server::EncodeServerManagementRequestForTest(management);
    header.message_type =
        static_cast<std::uint16_t>(scratchbird::server::sbps::MessageType::kManagementRequest);
    header.payload_schema_id = scratchbird::server::sbps::kSchemaManagementRequestV1;
  } else if (scenario == "sblr_close_cursor") {
    header.session_uuid = session_uuid.value_or(std::array<std::uint8_t, 16>{});
    payload = scratchbird::server::EncodeCloseCursorPayloadForTest(
        header.session_uuid, cursor_uuid.value_or(std::array<std::uint8_t, 16>{}));
    header.message_type =
        static_cast<std::uint16_t>(scratchbird::server::sbps::MessageType::kCloseCursor);
    header.payload_schema_id = 4007;
  } else if (scenario == "sblr_cluster_refused") {
    header.session_uuid = session_uuid.value_or(std::array<std::uint8_t, 16>{});
    payload = scratchbird::server::EncodeExecuteSblrPayloadForTest(
        header.session_uuid,
        {},
        scratchbird::server::EncodeClusterSblrForTest(),
        false);
    header.message_type =
        static_cast<std::uint16_t>(scratchbird::server::sbps::MessageType::kExecuteSblr);
    header.payload_schema_id = 4003;
  } else if (scenario == "sblr_crud_insert" || scenario == "sblr_crud_select" ||
             scenario == "sblr_crud_update" || scenario == "sblr_crud_delete" ||
             scenario == "sblr_catalog_create_table" || scenario == "sblr_catalog_get_descriptor" ||
             scenario == "sblr_index_create" || scenario == "sblr_datatype_cast" ||
             scenario == "sblr_datatype_extract" || scenario == "sblr_datatype_set" ||
             scenario == "sblr_optimizer_explain" || scenario == "sblr_optimizer_plan" ||
             scenario == "sblr_llvm_compile") {
    header.session_uuid = session_uuid.value_or(std::array<std::uint8_t, 16>{});
    std::string encoded = scenario == "sblr_crud_insert" ? scratchbird::server::EncodeCrudInsertSblrForTest()
                        : scenario == "sblr_crud_select" ? scratchbird::server::EncodeCrudSelectSblrForTest()
                        : scenario == "sblr_crud_update" ? scratchbird::server::EncodeCrudUpdateSblrForTest()
                        : scenario == "sblr_crud_delete" ? scratchbird::server::EncodeCrudDeleteSblrForTest()
                        : scenario == "sblr_catalog_create_table" ? scratchbird::server::EncodeCatalogCreateTableSblrForTest()
                        : scenario == "sblr_catalog_get_descriptor" ? scratchbird::server::EncodeCatalogGetDescriptorSblrForTest()
                        : scenario == "sblr_index_create" ? scratchbird::server::EncodeIndexCreateSblrForTest()
                        : scenario == "sblr_datatype_cast" ? scratchbird::server::EncodeDatatypeCastSblrForTest()
                        : scenario == "sblr_datatype_extract" ? scratchbird::server::EncodeDatatypeExtractSblrForTest()
                        : scenario == "sblr_datatype_set" ? scratchbird::server::EncodeDatatypeSetSblrForTest()
                        : scenario == "sblr_optimizer_explain" ? scratchbird::server::EncodeOptimizerExplainSblrForTest()
                        : scenario == "sblr_optimizer_plan" ? scratchbird::server::EncodeOptimizerPlanSblrForTest()
                                                             : scratchbird::server::EncodeLlvmCompileSblrForTest();
    payload = scratchbird::server::EncodeExecuteSblrPayloadForTest(
        header.session_uuid, {}, encoded, false);
    header.message_type =
        static_cast<std::uint16_t>(scratchbird::server::sbps::MessageType::kExecuteSblr);
    header.payload_schema_id = 4003;
  } else if (scenario == "sblr_begin_transaction") {
    header.session_uuid = session_uuid.value_or(std::array<std::uint8_t, 16>{});
    payload = scratchbird::server::EncodeExecuteSblrPayloadForTest(
        header.session_uuid, {}, scratchbird::server::EncodeBeginTransactionSblrForTest(), false);
    header.message_type =
        static_cast<std::uint16_t>(scratchbird::server::sbps::MessageType::kExecuteSblr);
    header.payload_schema_id = 4003;
  } else if (scenario == "sblr_event_channel_create") {
    header.session_uuid = session_uuid.value_or(std::array<std::uint8_t, 16>{});
    payload = scratchbird::server::EncodeExecuteSblrPayloadForTest(
        header.session_uuid, {}, scratchbird::server::EncodeEventChannelCreateSblrForTest("event.test.default"), false);
    header.message_type =
        static_cast<std::uint16_t>(scratchbird::server::sbps::MessageType::kExecuteSblr);
    header.payload_schema_id = 4003;
  } else if (scenario == "sblr_event_channel_notify") {
    header.session_uuid = session_uuid.value_or(std::array<std::uint8_t, 16>{});
    payload = scratchbird::server::EncodeExecuteSblrPayloadForTest(
        header.session_uuid, {}, scratchbird::server::EncodeEventChannelNotifySblrForTest("event.test.default", "event payload from ipc"), false);
    header.message_type =
        static_cast<std::uint16_t>(scratchbird::server::sbps::MessageType::kExecuteSblr);
    header.payload_schema_id = 4003;
  } else if (scenario == "event_subscribe") {
    header.session_uuid = session_uuid.value_or(std::array<std::uint8_t, 16>{});
    payload = EncodeEventFieldPayloadForTest({{"channel_uuid", "event.test.default"},
                                              {"rendering_profile_uuid", "native.sbps.test"},
                                              {"delivery_profile", "ephemeral_session"},
                                              {"policy_generation", "1"}});
    header.message_type =
        static_cast<std::uint16_t>(scratchbird::server::sbps::MessageType::kEventSubscribeRequest);
    header.payload_schema_id = scratchbird::server::sbps::kSchemaEventSubscribeRequestV1;
  } else if (scenario == "event_unsubscribe") {
    header.session_uuid = session_uuid.value_or(std::array<std::uint8_t, 16>{});
    payload = EncodeEventFieldPayloadForTest({{"channel_uuid", "event.test.default"}, {"all_channels", "true"}});
    header.message_type =
        static_cast<std::uint16_t>(scratchbird::server::sbps::MessageType::kEventUnsubscribeRequest);
    header.payload_schema_id = scratchbird::server::sbps::kSchemaEventUnsubscribeRequestV1;
  }
  if (scenario == "version_mismatch") {
    header.protocol_major = 99;
  }
  auto frame = scratchbird::server::sbps::EncodeFrame(header, payload);
  if (scenario == "malformed_magic") {
    frame[0] = 0;
  } else if (scenario == "payload_crc_mismatch" && !payload.empty()) {
    frame.back() ^= 0x55u;
  }
  return frame;
}

#ifndef _WIN32
bool WriteAll(int fd, const std::vector<std::uint8_t>& data) {
  std::size_t sent = 0;
  while (sent < data.size()) {
    const auto rc = ::send(fd, data.data() + sent, data.size() - sent, 0);
    if (rc < 0) return false;
    if (rc == 0) return false;
    sent += static_cast<std::size_t>(rc);
  }
  return true;
}

bool ReadExact(int fd, std::vector<std::uint8_t>* data, std::size_t bytes) {
  data->resize(bytes);
  std::size_t got = 0;
  while (got < bytes) {
    const auto rc = ::recv(fd, data->data() + got, bytes - got, 0);
    if (rc < 0) return false;
    if (rc == 0) return false;
    got += static_cast<std::size_t>(rc);
  }
  return true;
}

std::optional<scratchbird::server::sbps::Frame> Exchange(const std::string& endpoint,
                                                         const std::vector<std::uint8_t>& request,
                                                         std::string* error_message) {
  const int fd = ::socket(AF_UNIX, SOCK_STREAM, 0);
  if (fd < 0) {
    *error_message = "socket create failed";
    return std::nullopt;
  }
  sockaddr_un addr {};
  addr.sun_family = AF_UNIX;
  if (endpoint.size() >= sizeof(addr.sun_path)) {
    *error_message = "endpoint path too long";
    ::close(fd);
    return std::nullopt;
  }
  std::strncpy(addr.sun_path, endpoint.c_str(), sizeof(addr.sun_path) - 1);
  if (::connect(fd, reinterpret_cast<sockaddr*>(&addr), sizeof(addr)) != 0) {
    *error_message = std::string("connect failed: ") + endpoint;
    ::close(fd);
    return std::nullopt;
  }
  if (!WriteAll(fd, request)) {
    *error_message = "write failed";
    ::close(fd);
    return std::nullopt;
  }
  std::vector<std::uint8_t> response_header;
  if (!ReadExact(fd, &response_header, scratchbird::server::sbps::kHeaderBytes)) {
    *error_message = "response header read failed";
    ::close(fd);
    return std::nullopt;
  }
  const auto payload_len =
      scratchbird::server::sbps::PayloadLengthFromHeader(response_header).value_or(0);
  std::vector<std::uint8_t> payload;
  if (payload_len > 0 && !ReadExact(fd, &payload, payload_len)) {
    *error_message = "response payload read failed";
    ::close(fd);
    return std::nullopt;
  }
  ::close(fd);
  std::vector<std::uint8_t> full = response_header;
  full.insert(full.end(), payload.begin(), payload.end());
  const auto decoded = scratchbird::server::sbps::DecodeFrameBytes(full, 1048576);
  if (!decoded.ok()) {
    *error_message = "response decode failed";
    return std::nullopt;
  }
  return decoded.frame;
}

std::vector<scratchbird::server::sbps::Frame> ExchangeAll(const std::string& endpoint,
                                                          const std::vector<std::uint8_t>& request,
                                                          std::string* error_message) {
  std::vector<scratchbird::server::sbps::Frame> frames;
  const int fd = ::socket(AF_UNIX, SOCK_STREAM, 0);
  if (fd < 0) {
    *error_message = "socket create failed";
    return frames;
  }
  sockaddr_un addr {};
  addr.sun_family = AF_UNIX;
  if (endpoint.size() >= sizeof(addr.sun_path)) {
    *error_message = "endpoint path too long";
    ::close(fd);
    return frames;
  }
  std::strncpy(addr.sun_path, endpoint.c_str(), sizeof(addr.sun_path) - 1);
  if (::connect(fd, reinterpret_cast<sockaddr*>(&addr), sizeof(addr)) != 0) {
    *error_message = std::string("connect failed: ") + endpoint;
    ::close(fd);
    return frames;
  }
  if (!WriteAll(fd, request)) {
    *error_message = "write failed";
    ::close(fd);
    return frames;
  }
  while (true) {
    std::vector<std::uint8_t> response_header;
    if (!ReadExact(fd, &response_header, scratchbird::server::sbps::kHeaderBytes)) break;
    const auto payload_len =
        scratchbird::server::sbps::PayloadLengthFromHeader(response_header).value_or(0);
    std::vector<std::uint8_t> payload;
    if (payload_len > 0 && !ReadExact(fd, &payload, payload_len)) {
      *error_message = "response payload read failed";
      break;
    }
    std::vector<std::uint8_t> full = response_header;
    full.insert(full.end(), payload.begin(), payload.end());
    const auto decoded = scratchbird::server::sbps::DecodeFrameBytes(full, 1048576);
    if (!decoded.ok() || !decoded.frame) {
      *error_message = "response decode failed";
      break;
    }
    frames.push_back(*decoded.frame);
  }
  ::close(fd);
  return frames;
}
#endif

}  // namespace

int main(int argc, char** argv) {
  const auto options = Parse(argc, argv);
  if (options.endpoint.empty()) {
    std::cerr << "endpoint is required\n";
    return 2;
  }
#ifdef _WIN32
  std::cerr << "POSIX AF_UNIX tester transport is required in this SIF stage\n";
  return 2;
#else
  std::string exchange_error;
  std::optional<scratchbird::server::sbps::Frame> response;
  auto authenticate_and_attach_as = [&](const std::string& principal,
                                        std::array<std::uint8_t, 16>* session_uuid) -> bool {
    const std::string principal_uuid =
        principal == options.principal ? options.principal_uuid : "";
    auto auth = Exchange(options.endpoint,
                         BuildRequest("auth_then_attach",
                                      std::nullopt,
                                      std::nullopt,
                                      std::nullopt,
                                      std::nullopt,
                                      principal,
                                      principal_uuid),
                         &exchange_error);
    if (!auth) return false;
    const auto auth_context = scratchbird::server::DecodeAuthContextUuidForTest(auth->payload);
    if (!auth_context) {
      exchange_error = "auth context decode failed";
      return false;
    }
    auto attach = Exchange(options.endpoint, BuildRequest("attach_with_auth", auth_context), &exchange_error);
    if (!attach) return false;
    const auto decoded_session = scratchbird::server::DecodeSessionUuidForTest(attach->payload);
    if (!decoded_session) {
      exchange_error = "session uuid decode failed";
      return false;
    }
    *session_uuid = *decoded_session;
    return true;
  };
  auto authenticate_and_attach = [&](std::array<std::uint8_t, 16>* session_uuid) -> bool {
    return authenticate_and_attach_as("alice", session_uuid);
  };
  if (options.scenario == "auth_then_attach") {
    response = Exchange(options.endpoint,
                        BuildRequest("auth_then_attach",
                                     std::nullopt,
                                     std::nullopt,
                                     std::nullopt,
                                     std::nullopt,
                                     options.principal,
                                     options.principal_uuid),
                        &exchange_error);
    if (!response) {
      std::cerr << exchange_error << "\n";
      return 2;
    }
    const auto auth_context =
        scratchbird::server::DecodeAuthContextUuidForTest(response->payload);
    if (!auth_context) {
      std::cerr << "auth context decode failed\n";
      return 2;
    }
    response = Exchange(options.endpoint, BuildRequest("attach_with_auth", auth_context), &exchange_error);
  } else if (options.scenario == "auth_attach_detach") {
    std::array<std::uint8_t, 16> session_uuid{};
    if (!authenticate_and_attach(&session_uuid)) {
      std::cerr << exchange_error << "\n";
      return 2;
    }
    response = Exchange(options.endpoint,
                        BuildRequest("detach_with_session", std::nullopt, session_uuid),
                        &exchange_error);
  } else if (options.scenario == "sblr_prepare_execute_show_version") {
    std::array<std::uint8_t, 16> session_uuid{};
    if (!authenticate_and_attach(&session_uuid)) {
      std::cerr << exchange_error << "\n";
      return 2;
    }
    auto prepare = Exchange(options.endpoint,
                            BuildRequest("sblr_prepare_show_version", std::nullopt, session_uuid),
                            &exchange_error);
    if (!prepare) {
      std::cerr << exchange_error << "\n";
      return 2;
    }
    const auto prepared_uuid = scratchbird::server::DecodePreparedStatementUuidForTest(prepare->payload);
    if (!prepared_uuid) {
      std::cerr << "prepared statement decode failed\n";
      return 2;
    }
    response = Exchange(options.endpoint,
                        BuildRequest("sblr_execute_prepared_show_version",
                                     std::nullopt,
                                     session_uuid,
                                     prepared_uuid),
                        &exchange_error);
  } else if (options.scenario == "sblr_raw_sql_rejected") {
    std::array<std::uint8_t, 16> session_uuid{};
    if (!authenticate_and_attach(&session_uuid)) {
      std::cerr << exchange_error << "\n";
      return 2;
    }
    response = Exchange(options.endpoint,
                        BuildRequest("sblr_raw_sql_rejected", std::nullopt, session_uuid),
                        &exchange_error);
  } else if (options.scenario == "sblr_fetch_close_show_version") {
    std::array<std::uint8_t, 16> session_uuid{};
    if (!authenticate_and_attach(&session_uuid)) {
      std::cerr << exchange_error << "\n";
      return 2;
    }
    auto execute = Exchange(options.endpoint,
                            BuildRequest("sblr_execute_cursor_show_version", std::nullopt, session_uuid),
                            &exchange_error);
    if (!execute) {
      std::cerr << exchange_error << "\n";
      return 2;
    }
    const auto cursor_uuid = scratchbird::server::DecodeCursorUuidForTest(execute->payload);
    if (!cursor_uuid) {
      std::cerr << "cursor uuid decode failed\n";
      return 2;
    }
    auto fetch = Exchange(options.endpoint,
                          BuildRequest("sblr_fetch", std::nullopt, session_uuid, std::nullopt, cursor_uuid),
                          &exchange_error);
    if (!fetch) {
      std::cerr << exchange_error << "\n";
      return 2;
    }
    response = Exchange(options.endpoint,
                        BuildRequest("sblr_close_cursor", std::nullopt, session_uuid, std::nullopt, cursor_uuid),
                        &exchange_error);
  } else if (options.scenario == "sblr_cluster_refused") {
    std::array<std::uint8_t, 16> session_uuid{};
    if (!authenticate_and_attach(&session_uuid)) {
      std::cerr << exchange_error << "\n";
      return 2;
    }
    response = Exchange(options.endpoint,
                        BuildRequest("sblr_cluster_refused", std::nullopt, session_uuid),
                        &exchange_error);
  } else if (options.scenario == "sblr_crud_insert" || options.scenario == "sblr_crud_select" ||
             options.scenario == "sblr_crud_update" || options.scenario == "sblr_crud_delete" ||
             options.scenario == "sblr_catalog_create_table" ||
             options.scenario == "sblr_catalog_get_descriptor" ||
             options.scenario == "sblr_index_create" ||
             options.scenario == "sblr_datatype_cast" ||
             options.scenario == "sblr_datatype_extract" ||
             options.scenario == "sblr_datatype_set" ||
             options.scenario == "sblr_optimizer_explain" ||
             options.scenario == "sblr_optimizer_plan" ||
             options.scenario == "sblr_llvm_compile") {
    std::array<std::uint8_t, 16> session_uuid{};
    if (!authenticate_and_attach(&session_uuid)) {
      std::cerr << exchange_error << "\n";
      return 2;
    }
    if (options.scenario == "sblr_crud_insert" || options.scenario == "sblr_crud_select" ||
        options.scenario == "sblr_crud_update" || options.scenario == "sblr_crud_delete" ||
        options.scenario == "sblr_catalog_create_table" || options.scenario == "sblr_index_create") {
      auto begin = Exchange(options.endpoint,
                            BuildRequest("sblr_begin_transaction", std::nullopt, session_uuid),
                            &exchange_error);
      if (!begin || (begin->header.flags & scratchbird::server::sbps::kFlagError) != 0) {
        std::cerr << (exchange_error.empty() ? "transaction begin failed" : exchange_error) << "\n";
        if (begin) for (const auto& code : scratchbird::server::sbps::DecodeMessageVectorDiagnosticCodes(begin->payload)) std::cerr << code << "\n";
        return 2;
      }
    }
    response = Exchange(options.endpoint,
                        BuildRequest(options.scenario, std::nullopt, session_uuid),
                        &exchange_error);
  } else if (options.scenario == "event_subscribe_unsubscribe" ||
             options.scenario == "event_notify_delivery") {
    std::array<std::uint8_t, 16> session_uuid{};
    if (!authenticate_and_attach(&session_uuid)) {
      std::cerr << exchange_error << "\n";
      return 2;
    }
    auto begin = Exchange(options.endpoint,
                          BuildRequest("sblr_begin_transaction", std::nullopt, session_uuid),
                          &exchange_error);
    if (!begin || (begin->header.flags & scratchbird::server::sbps::kFlagError) != 0) {
      std::cerr << (exchange_error.empty() ? "transaction begin failed" : exchange_error) << "\n";
      if (begin) for (const auto& code : scratchbird::server::sbps::DecodeMessageVectorDiagnosticCodes(begin->payload)) std::cerr << code << "\n";
      return 2;
    }
    auto create = Exchange(options.endpoint,
                           BuildRequest("sblr_event_channel_create", std::nullopt, session_uuid),
                           &exchange_error);
    if (!create || (create->header.flags & scratchbird::server::sbps::kFlagError) != 0) {
      std::cerr << (exchange_error.empty() ? "event channel create failed" : exchange_error) << "\n";
      if (create) for (const auto& code : scratchbird::server::sbps::DecodeMessageVectorDiagnosticCodes(create->payload)) std::cerr << code << "\n";
      return 2;
    }
    auto subscribe = Exchange(options.endpoint,
                              BuildRequest("event_subscribe", std::nullopt, session_uuid),
                              &exchange_error);
    if (!subscribe || (subscribe->header.flags & scratchbird::server::sbps::kFlagError) != 0) {
      std::cerr << (exchange_error.empty() ? "event subscribe failed" : exchange_error) << "\n";
      if (subscribe) for (const auto& code : scratchbird::server::sbps::DecodeMessageVectorDiagnosticCodes(subscribe->payload)) std::cerr << code << "\n";
      return 2;
    }
    if (options.scenario == "event_subscribe_unsubscribe") {
      response = Exchange(options.endpoint,
                          BuildRequest("event_unsubscribe", std::nullopt, session_uuid),
                          &exchange_error);
    } else {
      auto frames = ExchangeAll(options.endpoint,
                                BuildRequest("sblr_event_channel_notify", std::nullopt, session_uuid),
                                &exchange_error);
      if (frames.empty()) {
        std::cerr << (exchange_error.empty() ? "event notify exchange failed" : exchange_error) << "\n";
        return 2;
      }
      for (const auto& frame : frames) {
        if (frame.header.message_type ==
            static_cast<std::uint16_t>(scratchbird::server::sbps::MessageType::kEventNotification)) {
          response = frame;
          break;
        }
      }
      if (!response) response = frames.back();
    }
  } else if (options.scenario == "management_set_clear_maintenance") {
    std::array<std::uint8_t, 16> session_uuid{};
    if (!authenticate_and_attach(&session_uuid)) {
      std::cerr << exchange_error << "\n";
      return 2;
    }
    auto set = Exchange(options.endpoint,
                        BuildRequest("management_set_maintenance", std::nullopt, session_uuid),
                        &exchange_error);
    if (!set || (set->header.flags & scratchbird::server::sbps::kFlagError) != 0) {
      std::cerr << (exchange_error.empty() ? "maintenance set failed" : exchange_error) << "\n";
      return 2;
    }
    auto blocked_auth = Exchange(options.endpoint, BuildRequest("auth_then_attach"), &exchange_error);
    if (!blocked_auth) {
      std::cerr << exchange_error << "\n";
      return 2;
    }
    const auto blocked_auth_context =
        scratchbird::server::DecodeAuthContextUuidForTest(blocked_auth->payload);
    if (!blocked_auth_context) {
      std::cerr << "blocked auth context decode failed\n";
      return 2;
    }
    auto blocked_attach = Exchange(options.endpoint,
                                   BuildRequest("attach_with_auth", blocked_auth_context),
                                   &exchange_error);
    if (!blocked_attach ||
        (blocked_attach->header.flags & scratchbird::server::sbps::kFlagError) == 0) {
      std::cerr << "maintenance did not refuse fresh attach\n";
      return 2;
    }
    auto clear = Exchange(options.endpoint,
                          BuildRequest("management_clear_maintenance", std::nullopt, session_uuid),
                          &exchange_error);
    if (!clear || (clear->header.flags & scratchbird::server::sbps::kFlagError) != 0) {
      std::cerr << (exchange_error.empty() ? "maintenance clear failed" : exchange_error) << "\n";
      return 2;
    }
    response = Exchange(options.endpoint,
                        BuildRequest("detach_with_session", std::nullopt, session_uuid),
                        &exchange_error);
    if (!response || (response->header.flags & scratchbird::server::sbps::kFlagError) != 0) {
      std::cerr << (exchange_error.empty() ? "maintenance fixture detach failed" : exchange_error)
                << "\n";
      return 2;
    }
    std::array<std::uint8_t, 16> post_clear_session_uuid{};
    if (!authenticate_and_attach(&post_clear_session_uuid)) {
      std::cerr << exchange_error << "\n";
      return 2;
    }
    response = Exchange(options.endpoint,
                        BuildRequest("detach_with_session", std::nullopt, post_clear_session_uuid),
                        &exchange_error);
  } else if (options.scenario == "management_show_server_status" ||
             options.scenario == "management_show_listeners" ||
             options.scenario == "management_show_metrics" ||
             options.scenario == "management_export_support_bundle" ||
             options.scenario == "management_start_listener" ||
             options.scenario == "management_restart_listener" ||
             options.scenario == "management_listener_proxy_refused" ||
             options.scenario == "management_unauthorized_start_listener" ||
             options.scenario == "management_show_server_lifecycle" ||
             options.scenario == "management_reload_server_config" ||
             options.scenario == "management_reload_invalid" ||
             options.scenario == "management_drain_server" ||
             options.scenario == "management_set_maintenance" ||
             options.scenario == "management_clear_maintenance" ||
             options.scenario == "management_set_clear_maintenance" ||
             options.scenario == "management_begin_backup_fence" ||
             options.scenario == "management_end_backup_fence" ||
             options.scenario == "management_begin_restore_fence" ||
             options.scenario == "management_end_restore_fence" ||
             options.scenario == "management_cancel_unknown" ||
             options.scenario == "management_stop_server" ||
             options.scenario == "management_restart_server") {
    std::array<std::uint8_t, 16> session_uuid{};
    const std::string principal =
        options.scenario == "management_unauthorized_start_listener" ? "mallory" : options.principal;
    if (!authenticate_and_attach_as(principal, &session_uuid)) {
      std::cerr << exchange_error << "\n";
      return 2;
    }
    response = Exchange(options.endpoint,
                        BuildRequest(options.scenario, std::nullopt, session_uuid),
                        &exchange_error);
    if (response && options.scenario != "management_stop_server" &&
        options.scenario != "management_restart_server") {
      auto detach = Exchange(options.endpoint,
                             BuildRequest("detach_with_session", std::nullopt, session_uuid),
                             &exchange_error);
      if (!detach || (detach->header.flags & scratchbird::server::sbps::kFlagError) != 0) {
        std::cerr << (exchange_error.empty() ? "management fixture detach failed" : exchange_error)
                  << "\n";
        return 2;
      }
    }
  } else {
    response = Exchange(options.endpoint, BuildRequest(options.scenario), &exchange_error);
  }
  if (!response) {
    std::cerr << exchange_error << "\n";
    return 2;
  }
  const bool error = (response->header.flags & scratchbird::server::sbps::kFlagError) != 0;
  const bool accepted =
      response->header.message_type ==
          static_cast<std::uint16_t>(scratchbird::server::sbps::MessageType::kHelloAccept) ||
      (options.scenario == "database_status" &&
       response->header.message_type ==
           static_cast<std::uint16_t>(scratchbird::server::sbps::MessageType::kPong) &&
       !response->payload.empty()) ||
      (options.scenario == "session_registry_status" &&
       response->header.message_type ==
           static_cast<std::uint16_t>(scratchbird::server::sbps::MessageType::kPong) &&
       !response->payload.empty()) ||
      (options.scenario == "parser_registry_status" &&
       response->header.message_type ==
           static_cast<std::uint16_t>(scratchbird::server::sbps::MessageType::kPong) &&
       !response->payload.empty()) ||
      (options.scenario == "notification_router_status" &&
       response->header.message_type ==
           static_cast<std::uint16_t>(scratchbird::server::sbps::MessageType::kPong) &&
       !response->payload.empty()) ||
      ((options.scenario == "listener_orchestrator_status" ||
        options.scenario == "server_management_rights") &&
       response->header.message_type ==
           static_cast<std::uint16_t>(scratchbird::server::sbps::MessageType::kPong) &&
       !response->payload.empty()) ||
      ((options.scenario == "auth_success" || options.scenario == "auth_challenge") &&
       response->header.message_type ==
           static_cast<std::uint16_t>(scratchbird::server::sbps::MessageType::kAuthResult) &&
       !error && !response->payload.empty()) ||
      (options.scenario == "auth_then_attach" &&
       response->header.message_type ==
           static_cast<std::uint16_t>(scratchbird::server::sbps::MessageType::kAttachResult) &&
       !error && !response->payload.empty()) ||
      ((options.scenario == "detach_unknown" || options.scenario == "auth_attach_detach" ||
        options.scenario == "management_set_clear_maintenance") &&
       response->header.message_type ==
           static_cast<std::uint16_t>(scratchbird::server::sbps::MessageType::kDisconnectNotice) &&
       !error && !response->payload.empty()) ||
      ((options.scenario == "sblr_prepare_execute_show_version") &&
       response->header.message_type ==
           static_cast<std::uint16_t>(scratchbird::server::sbps::MessageType::kExecuteResult) &&
       !error && !response->payload.empty()) ||
      ((options.scenario == "sblr_crud_insert" || options.scenario == "sblr_crud_select" ||
        options.scenario == "sblr_crud_update" || options.scenario == "sblr_crud_delete" ||
        options.scenario == "sblr_catalog_create_table" ||
        options.scenario == "sblr_catalog_get_descriptor" ||
        options.scenario == "sblr_index_create" ||
        options.scenario == "sblr_datatype_cast" ||
        options.scenario == "sblr_datatype_extract" ||
        options.scenario == "sblr_datatype_set" ||
        options.scenario == "sblr_optimizer_explain" ||
        options.scenario == "sblr_optimizer_plan" ||
        options.scenario == "sblr_llvm_compile") &&
       response->header.message_type ==
           static_cast<std::uint16_t>(scratchbird::server::sbps::MessageType::kExecuteResult) &&
       !error && !response->payload.empty()) ||
      ((options.scenario == "sblr_fetch_close_show_version") &&
       response->header.message_type ==
           static_cast<std::uint16_t>(scratchbird::server::sbps::MessageType::kCloseCursorResult) &&
       !error && !response->payload.empty()) ||
      ((options.scenario == "event_subscribe_unsubscribe") &&
       response->header.message_type ==
           static_cast<std::uint16_t>(scratchbird::server::sbps::MessageType::kEventUnsubscribeResult) &&
       !error && !response->payload.empty()) ||
      ((options.scenario == "event_notify_delivery") &&
       response->header.message_type ==
           static_cast<std::uint16_t>(scratchbird::server::sbps::MessageType::kEventNotification) &&
       !error && !response->payload.empty()) ||
      ((options.scenario == "management_show_server_status" ||
        options.scenario == "management_show_listeners" ||
        options.scenario == "management_show_metrics" ||
        options.scenario == "management_export_support_bundle" ||
        options.scenario == "management_start_listener" ||
        options.scenario == "management_restart_listener" ||
        options.scenario == "management_show_server_lifecycle" ||
        options.scenario == "management_reload_server_config" ||
        options.scenario == "management_drain_server" ||
        options.scenario == "management_set_maintenance" ||
        options.scenario == "management_clear_maintenance" ||
        options.scenario == "management_set_clear_maintenance" ||
        options.scenario == "management_begin_backup_fence" ||
        options.scenario == "management_end_backup_fence" ||
        options.scenario == "management_begin_restore_fence" ||
        options.scenario == "management_end_restore_fence" ||
        options.scenario == "management_cancel_unknown" ||
        options.scenario == "management_stop_server" ||
        options.scenario == "management_restart_server") &&
       response->header.message_type ==
           static_cast<std::uint16_t>(scratchbird::server::sbps::MessageType::kManagementResult) &&
       !error && !response->payload.empty());
  const auto codes = scratchbird::server::sbps::DecodeMessageVectorDiagnosticCodes(
      response->payload);
  const std::string payload_text(response->payload.begin(), response->payload.end());
  bool code_match = options.expected_code.empty();
  for (const auto& code : codes) {
    if (code == options.expected_code) code_match = true;
  }
  const bool payload_match = options.expected_payload_contains.empty() ||
                             payload_text.find(options.expected_payload_contains) != std::string::npos;
  const bool expectation_match =
      (options.expected == "accept" ? accepted && !error : error) && code_match && payload_match;
  std::cout << "{\"ipc_tester\":{\"scenario\":\"" << JsonEscape(options.scenario)
            << "\",\"accepted\":" << (accepted ? "true" : "false")
            << ",\"error\":" << (error ? "true" : "false")
            << ",\"response_type\":" << response->header.message_type
            << ",\"expectation_match\":" << (expectation_match ? "true" : "false")
            << ",\"diagnostic_codes\":[";
  for (std::size_t i = 0; i < codes.size(); ++i) {
    if (i != 0) std::cout << ',';
    std::cout << '"' << JsonEscape(codes[i]) << '"';
  }
  std::cout << "]";
  if (options.scenario == "database_status" || options.scenario == "session_registry_status" ||
      options.scenario == "parser_registry_status" || options.scenario == "notification_router_status" ||
      options.scenario == "listener_orchestrator_status" || options.scenario == "server_management_rights" ||
      options.scenario == "management_show_server_status" || options.scenario == "management_show_listeners" ||
      options.scenario == "management_show_metrics" || options.scenario == "management_export_support_bundle" ||
      options.scenario == "management_start_listener" ||
      options.scenario == "management_restart_listener" ||
      options.scenario == "management_show_server_lifecycle" ||
      options.scenario == "management_reload_server_config" ||
      options.scenario == "management_reload_invalid" ||
      options.scenario == "management_drain_server" ||
      options.scenario == "management_set_maintenance" ||
      options.scenario == "management_clear_maintenance" ||
      options.scenario == "management_set_clear_maintenance" ||
      options.scenario == "management_begin_backup_fence" ||
      options.scenario == "management_end_backup_fence" ||
      options.scenario == "management_begin_restore_fence" ||
      options.scenario == "management_end_restore_fence" ||
      options.scenario == "management_cancel_unknown" ||
      options.scenario == "management_stop_server" ||
      options.scenario == "management_restart_server" ||
      options.scenario == "sblr_crud_insert" ||
      options.scenario == "sblr_crud_select" ||
      options.scenario == "sblr_crud_update" ||
      options.scenario == "sblr_crud_delete" ||
      options.scenario == "sblr_catalog_create_table" ||
      options.scenario == "sblr_catalog_get_descriptor" ||
      options.scenario == "sblr_index_create" ||
      options.scenario == "sblr_datatype_cast" ||
      options.scenario == "sblr_datatype_extract" ||
      options.scenario == "sblr_datatype_set" ||
      options.scenario == "sblr_optimizer_explain" ||
      options.scenario == "sblr_optimizer_plan" ||
      options.scenario == "sblr_llvm_compile" ||
      options.scenario == "event_subscribe_unsubscribe" || options.scenario == "event_notify_delivery" ||
      !options.expected_payload_contains.empty()) {
    std::cout << ",\"payload\":\"" << JsonEscape(payload_text) << "\"";
  }
  std::cout << "}}\n";
  return expectation_match ? 0 : 1;
#endif
}
