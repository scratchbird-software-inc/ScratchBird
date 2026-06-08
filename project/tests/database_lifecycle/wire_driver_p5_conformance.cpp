// Copyright (c) 2026 ScratchBird Software Inc.
//
// This Source Code Form is subject to the terms of the Mozilla Public
// License, v. 2.0. If a copy of the MPL was not distributed with this
// file, You can obtain one at https://mozilla.org/MPL/2.0/.
//
// SPDX-License-Identifier: MPL-2.0

#include "config.hpp"
#include "control_plane.hpp"
#include "datatype_wire_metadata.hpp"
#include "lifecycle.hpp"
#include "local_ipc_contract.hpp"
#include "parser_pool.hpp"
#include "session_registry.hpp"

#include <cstdint>
#include <cstdlib>
#include <filesystem>
#include <fstream>
#include <iostream>
#include <string>
#include <utility>
#include <vector>

namespace {

namespace dt = scratchbird::core::datatypes;

using scratchbird::ipc::DecodeLocalIpcFrame;
using scratchbird::ipc::EncodeLocalIpcFrame;
using scratchbird::ipc::LocalIpcFrame;
using scratchbird::ipc::LocalIpcMessageType;
using scratchbird::ipc::MakeLocalIpcHelloFrame;
using scratchbird::listener::DecodeHelloPayload;
using scratchbird::listener::DecodeRecyclePayload;
using scratchbird::listener::DecodeSessionBindingReportPayload;
using scratchbird::listener::EncodeHelloPayload;
using scratchbird::listener::EncodeRecyclePayload;
using scratchbird::listener::EncodeSessionBindingReportPayload;
using scratchbird::listener::ParserHelloPayload;
using scratchbird::listener::ParserPool;
using scratchbird::listener::ParserWorkerState;
using scratchbird::listener::SessionBindingReportPayload;
using scratchbird::server::ClassifyServerConfigFormat;
using scratchbird::server::ServerBootstrapConfig;
using scratchbird::server::ServerConfigCompatibilityClass;
using scratchbird::server::ServerConfigCompatibilityResult;
using scratchbird::server::ServerRuntimeCleanupOperation;
using scratchbird::server::ValidateServerRuntimeArtifacts;
using scratchbird::server::WriteStartupLifecycleArtifacts;
using scratchbird::server::WriteStoppedLifecycleArtifacts;

void Require(bool condition, const std::string& message) {
  if (!condition) {
    std::cerr << "FAIL: " << message << '\n';
    std::exit(EXIT_FAILURE);
  }
}

dt::WireUuidBytes MetadataUuid(std::uint8_t seed) {
  dt::WireUuidBytes value{};
  for (std::size_t index = 0; index < value.size(); ++index) {
    value[index] = static_cast<std::uint8_t>(seed + index);
  }
  return value;
}

dt::NullableText MetadataText(std::string text) {
  dt::NullableText value;
  value.state = dt::WireNullableTextState::present;
  value.text = std::move(text);
  return value;
}

dt::CanonicalTypeRef MetadataTypeRef(dt::CanonicalTypeId type_id, std::uint8_t seed) {
  dt::CanonicalTypeRef type_ref;
  type_ref.canonical_type_id = dt::WireTypeIdForCanonicalTypeId(type_id);
  type_ref.descriptor_uuid = MetadataUuid(seed);
  type_ref.descriptor_version = 1;
  type_ref.modifier_bitmap =
      dt::CanonicalTypeRefModifierBit(dt::CanonicalTypeRefModifier::precision) |
      dt::CanonicalTypeRefModifierBit(dt::CanonicalTypeRefModifier::scale);
  if (type_id == dt::CanonicalTypeId::int128 || type_id == dt::CanonicalTypeId::uint128) {
    type_ref.precision = 128;
    type_ref.modifier_bitmap |=
        dt::CanonicalTypeRefModifierBit(dt::CanonicalTypeRefModifier::backend_profile_required);
  }
  return type_ref;
}

std::filesystem::path MakeTempDir() {
  std::filesystem::path root =
      std::filesystem::temp_directory_path() / "sb_wire_driver_p5_XXXXXX";
  std::string text = root.string();
  std::vector<char> writable(text.begin(), text.end());
  writable.push_back('\0');
  char* made = ::mkdtemp(writable.data());
  return made == nullptr ? std::filesystem::path{} : std::filesystem::path(made);
}

ServerBootstrapConfig ConfigFor(const std::filesystem::path& root) {
  ServerBootstrapConfig config;
  config.control_dir = root / "control";
  config.data_dir = root / "data";
  config.pid_file = config.control_dir / "sb_server.pid";
  config.lifecycle_state_file = config.control_dir / "sb_server.lifecycle.state";
  config.lifecycle_journal_file = config.control_dir / "sb_server.lifecycle.journal";
  config.database_default_path = config.data_dir / "p5.sbdb";
  config.database_runtime_scope_id = "db-019e12a0-p5";
  config.database_daemon_scope = "dedicated";
  config.sbps_endpoint = config.control_dir / "sb_server.sbps.sock";
  return config;
}

std::string Slurp(const std::filesystem::path& path) {
  std::ifstream input(path);
  return std::string(std::istreambuf_iterator<char>(input), std::istreambuf_iterator<char>());
}

void PutU64At(std::vector<std::uint8_t>* bytes, std::size_t offset, std::uint64_t value) {
  Require(bytes != nullptr && offset + 8 <= bytes->size(), "U64 mutation offset invalid");
  for (int shift = 0; shift < 64; shift += 8) {
    (*bytes)[offset + static_cast<std::size_t>(shift / 8)] =
        static_cast<std::uint8_t>((value >> shift) & 0xffu);
  }
}

void CheckServerLifecycle() {
  const auto root = MakeTempDir();
  Require(!root.empty(), "temporary directory was not created");
  auto config = ConfigFor(root);
  auto startup = WriteStartupLifecycleArtifacts(config, "serving");
  Require(startup.ok(), "startup lifecycle artifacts failed");
  Require(startup.artifacts.database_runtime_scope_id == config.database_runtime_scope_id,
          "startup scope id did not persist");
  Require(startup.artifacts.daemon_scope == "dedicated", "daemon scope did not persist");
  Require(startup.artifacts.sbps_endpoint == config.sbps_endpoint.string(),
          "SBPS endpoint did not persist");
  auto validation = ValidateServerRuntimeArtifacts(config, startup.artifacts, true);
  Require(validation.ok(), "runtime artifacts did not validate");
  Require(validation.database_association_valid, "database association was not valid");
  Require(validation.endpoint_descriptor_valid, "endpoint descriptor was not valid");

  auto stopped = WriteStoppedLifecycleArtifacts(config, startup.artifacts.generation + 1);
  Require(stopped.ok(), "stopped lifecycle artifacts failed");
  const auto state = Slurp(config.lifecycle_state_file);
  Require(state.find("state=stopped") != std::string::npos, "stopped state missing");
  Require(state.find("database_runtime_scope_id=db-019e12a0-p5") != std::string::npos,
          "database scope missing from stopped state");

  const ServerConfigCompatibilityResult old_format =
      ClassifyServerConfigFormat("SBCD0", {}, false, true);
  Require(!old_format.accepted &&
              old_format.compatibility_class ==
                  ServerConfigCompatibilityClass::kMissingMigrationPlanRefused,
          "missing migration plan was not refused");
  const ServerConfigCompatibilityResult current = ClassifyServerConfigFormat("SBCD1");
  Require(current.accepted, "current config format was not accepted");
  (void)ServerRuntimeCleanupOperation::kStop;
}

void CheckListenerSbctAndPool() {
  ParserHelloPayload hello;
  hello.protocol = "sbsql";
  hello.worker_id = 7;
  hello.dialect_protocol_version = 1;
  hello.parser_api_major = scratchbird::listener::kCurrentParserApiMajor;
  hello.profile_id = "SBsql";
  hello.bundle_contract_id = "bundle.default@1";
  auto decoded = DecodeHelloPayload(EncodeHelloPayload(hello), nullptr);
  Require(decoded.has_value(), "HELLO payload did not decode");
  Require(decoded->profile_id == hello.profile_id, "HELLO profile did not round-trip");
  Require(decoded->bundle_contract_id == hello.bundle_contract_id,
          "HELLO bundle contract did not round-trip");

  SessionBindingReportPayload binding;
  binding.current_txn_id = 42;
  binding.effective_group_ids.resize(2);
  auto binding_decoded =
      DecodeSessionBindingReportPayload(EncodeSessionBindingReportPayload(binding), nullptr);
  Require(binding_decoded.has_value(), "SESSION_BINDING_REPORT did not decode");
  Require(binding_decoded->current_txn_id == 42, "SESSION_BINDING_REPORT txn id mismatch");
  Require(binding_decoded->effective_group_ids.size() == 2,
          "SESSION_BINDING_REPORT group list mismatch");

  auto recycle = DecodeRecyclePayload(EncodeRecyclePayload(1), nullptr);
  Require(recycle.has_value() && *recycle == 1, "RECYCLE reason did not round-trip");
  auto bad_recycle = DecodeRecyclePayload(EncodeRecyclePayload(5), nullptr);
  Require(!bad_recycle.has_value(), "reserved RECYCLE reason was not refused");
  Require(scratchbird::listener::WorkerStateName(ParserWorkerState::kQuarantined) == "quarantined",
          "parser worker quarantine state name missing");

  scratchbird::listener::ListenerConfig config;
  config.parser_executable = "/bin/false";
  config.warm_pool_min = 1;
  config.warm_pool_max = 2;
  scratchbird::listener::ListenerMetrics metrics;
  ParserPool pool(config, &metrics);
  auto status = pool.Status();
  Require(!status.running && status.target_min == 1 && status.target_max == 2,
          "parser pool status did not expose configured warm bounds");
}

void CheckLocalIpc() {
  std::array<std::uint8_t, 16> correlation{};
  correlation[0] = 0x12;
  auto hello = MakeLocalIpcHelloFrame(correlation, "local_no_ip", "frame_message;route_message");
  const auto encoded = EncodeLocalIpcFrame(hello);
  Require(!encoded.empty(), "local IPC frame did not encode");
  auto decoded = DecodeLocalIpcFrame(encoded);
  Require(decoded.ok, "local IPC frame did not decode: " + decoded.diagnostic_code);
  Require(decoded.frame.header.message_type == LocalIpcMessageType::kHello,
          "local IPC message type mismatch");
  Require(decoded.frame.header.correlation_uuid[0] == 0x12, "local IPC correlation UUID mismatch");
  Require(std::string(decoded.frame.payload.begin(), decoded.frame.payload.end())
              .find("frame_message") != std::string::npos,
          "local IPC capability payload missing");

  auto bad_magic = encoded;
  bad_magic[0] = 0;
  Require(!DecodeLocalIpcFrame(bad_magic).ok, "local IPC bad magic was not refused");
  auto bad_payload_crc = encoded;
  bad_payload_crc.back() ^= 0x20;
  const auto crc_result = DecodeLocalIpcFrame(bad_payload_crc);
  Require(!crc_result.ok && crc_result.diagnostic_code == "IPC.FRAME.PAYLOAD_CRC",
          "local IPC payload CRC mismatch was not refused");

  auto oversized_payload_length = encoded;
  PutU64At(&oversized_payload_length,
           16,
           static_cast<std::uint64_t>(scratchbird::ipc::kLocalIpcMaxPayloadBytes) + 1);
  const auto oversized = DecodeLocalIpcFrame(oversized_payload_length);
  Require(!oversized.ok && oversized.diagnostic_code == "IPC.FRAME.PAYLOAD_TOO_LARGE",
          "local IPC oversized payload length was not refused before route handling");

  auto truncated_payload = encoded;
  PutU64At(&truncated_payload, 16, 1);
  const auto truncated = DecodeLocalIpcFrame(truncated_payload);
  Require(!truncated.ok && truncated.diagnostic_code == "IPC.FRAME.LENGTH",
          "local IPC mismatched stream/LOB payload length was not refused");

  LocalIpcFrame route;
  route.header.message_type = LocalIpcMessageType::kRouteMessage;
  route.header.session_uuid[0] = 0x45;
  route.header.transaction_uuid[0] = 0x67;
  route.payload = {'S', 'B', 'L', 'R'};
  auto route_decoded = DecodeLocalIpcFrame(EncodeLocalIpcFrame(route));
  Require(route_decoded.ok, "local IPC route frame did not decode");
  Require(route_decoded.frame.header.session_uuid[0] == 0x45, "local IPC session UUID mismatch");
  Require(route_decoded.frame.header.transaction_uuid[0] == 0x67,
          "local IPC transaction UUID mismatch");
}

void CheckFinalityFaultPolicy() {
  scratchbird::server::ServerSessionRecord active;
  active.local_transaction_id = 9;

  scratchbird::server::ServerDriverTransactionDecisionInput unknown;
  unknown.event = scratchbird::server::ServerDriverTransactionEvent::kRetryAfterUnknownFinality;
  unknown.engine_finality_known = false;
  auto retry_unknown = scratchbird::server::ClassifyDriverTransactionEvent(
      active, unknown);
  Require(!retry_unknown.accepted && retry_unknown.must_query_engine_finality &&
              !retry_unknown.driver_may_retry,
          "retry after unknown finality did not fail closed");

  scratchbird::server::ServerDriverTransactionDecisionInput prepared_reconnect;
  prepared_reconnect.event =
      scratchbird::server::ServerDriverTransactionEvent::kReconnectAfterDisconnect;
  prepared_reconnect.prepared_transaction_present = true;
  prepared_reconnect.engine_finality_known = false;
  const auto reconnect = scratchbird::server::ClassifyDriverTransactionEvent(
      scratchbird::server::ServerSessionRecord{}, prepared_reconnect);
  Require(reconnect.accepted && reconnect.must_query_engine_finality &&
              reconnect.requires_explicit_engine_recovery &&
              !reconnect.driver_may_retry,
          "reconnect after prepared transaction implied hidden finality");

  scratchbird::server::ServerDriverTransactionDecisionInput reset;
  reset.event = scratchbird::server::ServerDriverTransactionEvent::kResetSession;
  reset.active_cursor = true;
  const auto reset_decision =
      scratchbird::server::ClassifyDriverTransactionEvent(active, reset);
  Require(!reset_decision.accepted &&
              reset_decision.diagnostic_code ==
                  "SERVER.DRIVER_TX.RESET_REQUIRES_CLEAN_BOUNDARY",
          "reset/session race did not fail closed");

  scratchbird::server::ServerDriverTransactionDecisionInput dormant;
  dormant.event = scratchbird::server::ServerDriverTransactionEvent::kDormantReattach;
  const auto dormant_decision = scratchbird::server::ClassifyDriverTransactionEvent(
      scratchbird::server::ServerSessionRecord{}, dormant);
  Require(!dormant_decision.accepted &&
              dormant_decision.diagnostic_code ==
                  "SERVER.DRIVER_TX.DORMANT_REATTACH_TOKEN_REQUIRED",
          "implicit dormant reattach was not refused");
}

void CheckWireMetadataDiscriminators() {
  dt::RowDescriptionPacket generated;
  generated.result_set_kind = dt::WireResultSetKind::generated_keys;
  dt::ResultColumnDescriptor generated_column;
  generated_column.ordinal = 1;
  generated_column.column_class = dt::WireResultColumnClass::generated_key;
  generated_column.nullability = dt::WireNullability::not_nullable;
  generated_column.metadata_bitmap =
      dt::WireMetadataBit(dt::WireMetadataBitmapBit::display_label_present) |
      dt::WireMetadataBit(dt::WireMetadataBitmapBit::type_name_present) |
      dt::WireMetadataBit(dt::WireMetadataBitmapBit::precision_valid) |
      dt::WireMetadataBit(dt::WireMetadataBitmapBit::generated_key_discriminator_active);
  generated_column.type_ref = MetadataTypeRef(dt::CanonicalTypeId::int128, 0xa0);
  generated_column.generated_value_kind = dt::WireGeneratedValueKind::identity;
  generated_column.display_label = MetadataText("generated_id");
  generated_column.type_name = MetadataText("int128");
  generated.columns.push_back(generated_column);
  const auto generated_encoded = dt::EncodeRowDescriptionPacket(generated);
  Require(generated_encoded.ok, "generated-key metadata did not encode");
  const auto generated_decoded = dt::DecodeRowDescriptionPacket(generated_encoded.bytes);
  Require(generated_decoded.ok, "generated-key metadata did not decode");
  Require(generated_decoded.packet.result_set_kind == dt::WireResultSetKind::generated_keys,
          "generated-key metadata discriminator did not round-trip");

  dt::RowDescriptionPacket out_row;
  out_row.result_set_kind = dt::WireResultSetKind::out_parameter_row;
  dt::ResultColumnDescriptor out_column;
  out_column.ordinal = 1;
  out_column.column_class = dt::WireResultColumnClass::out_parameter;
  out_column.metadata_bitmap =
      dt::WireMetadataBit(dt::WireMetadataBitmapBit::display_label_present) |
      dt::WireMetadataBit(dt::WireMetadataBitmapBit::out_inout_return_discriminator_active);
  out_column.type_ref = MetadataTypeRef(dt::CanonicalTypeId::uint128, 0xb0);
  out_column.source_parameter_ordinal = 2;
  out_column.parameter_direction = dt::WireParameterDirection::inout;
  out_column.display_label = MetadataText("out_value");
  out_row.columns.push_back(out_column);
  const auto out_encoded = dt::EncodeRowDescriptionPacket(out_row);
  Require(out_encoded.ok, "OUT/INOUT metadata did not encode");

  out_row.columns.front().source_parameter_ordinal = 0;
  const auto bad_out = dt::EncodeRowDescriptionPacket(out_row);
  Require(!bad_out.ok &&
              bad_out.diagnostic_code == "NATIVE_WIRE.RESULT.OUT_RETURN_PATH_INVALID",
          "OUT/INOUT metadata without parameter ordinal was not refused");
}

}  // namespace

int main() {
  CheckServerLifecycle();
  CheckListenerSbctAndPool();
  CheckLocalIpc();
  CheckFinalityFaultPolicy();
  CheckWireMetadataDiscriminators();
  std::cout << "PUBLIC_SINGLE_NODE_P5_WIRE_DRIVER_CONFORMANCE ok\n";
  return EXIT_SUCCESS;
}
