// Copyright (c) 2026 ScratchBird Software Inc.
//
// This Source Code Form is subject to the terms of the Mozilla Public
// License, v. 2.0. If a copy of the MPL was not distributed with this
// file, You can obtain one at https://mozilla.org/MPL/2.0/.
//
// SPDX-License-Identifier: MPL-2.0

#include "sblr_admission.hpp"
#include "sblr_dispatch.hpp"
#include "sblr_engine_envelope.hpp"
#include "sblr_opcode_registry.hpp"
#include "sblr_to_sbsql.hpp"
#include "uuid.hpp"

#include "scratchbird/engine/engine.h"
#include "scratchbird/engine/sblr/lowering.hpp"

#include <array>
#include <chrono>
#include <cstdlib>
#include <cstring>
#include <filesystem>
#include <fstream>
#include <iostream>
#include <sstream>
#include <string>
#include <string_view>
#include <utility>
#include <vector>

namespace {

namespace api = scratchbird::engine::internal_api;
namespace engine_sblr = scratchbird::engine::sblr;
namespace platform = scratchbird::core::platform;
namespace server = scratchbird::server;
namespace uuid = scratchbird::core::uuid;

[[noreturn]] void Fail(std::string_view message) {
  std::cerr << message << '\n';
  std::exit(EXIT_FAILURE);
}

void Require(bool condition, std::string_view message) {
  if (!condition) Fail(message);
}

bool Contains(std::string_view haystack, std::string_view needle) {
  return haystack.find(needle) != std::string_view::npos;
}

std::string ReadText(const std::filesystem::path& path) {
  std::ifstream in(path);
  Require(in.good(), "DPC-074 evidence source file could not be opened");
  std::ostringstream out;
  out << in.rdbuf();
  return out.str();
}

void RequireContains(const std::filesystem::path& path,
                     std::string_view needle,
                     std::string_view message) {
  const auto text = ReadText(path);
  Require(Contains(text, needle), message);
}

platform::u64 NowMillis() {
  return static_cast<platform::u64>(
      std::chrono::duration_cast<std::chrono::milliseconds>(
          std::chrono::system_clock::now().time_since_epoch())
          .count());
}

struct UuidFactory {
  platform::u64 base_millis = NowMillis();

  platform::TypedUuid Typed(platform::UuidKind kind, platform::u64 salt) const {
    if (!uuid::UuidKindAllowsDurableIdentity(kind)) {
      const auto raw = uuid::GenerateCompatibilityUnixTimeV7(base_millis + salt);
      Require(raw.ok(), "DPC-074 generated UUID creation failed");
      const auto typed = uuid::MakeTypedUuid(kind, raw.value);
      Require(typed.ok(), "DPC-074 generated UUID typing failed");
      return typed.value;
    }
    const auto generated =
        uuid::GenerateDurableEngineIdentityV7(kind, base_millis + salt);
    Require(generated.ok(), "DPC-074 durable UUID generation failed");
    return generated.value;
  }

  std::string Text(platform::UuidKind kind, platform::u64 salt) const {
    return uuid::UuidToString(Typed(kind, salt).value);
  }
};

sb_engine_uuid_t EngineUuid(const platform::TypedUuid& typed) {
  sb_engine_uuid_t out{};
  std::memcpy(out.bytes, typed.value.bytes.data(), typed.value.bytes.size());
  return out;
}

struct AbiHarness {
  sb_engine_handle_t engine = nullptr;
  sb_engine_session_t session = nullptr;
  platform::TypedUuid principal;
  platform::TypedUuid session_id;

  explicit AbiHarness(const UuidFactory& uuids)
      : principal(uuids.Typed(platform::UuidKind::principal, 501)),
        session_id(uuids.Typed(platform::UuidKind::session, 502)) {
    sb_engine_open_params_v1_t open{};
    open.struct_size = sizeof(open);
    open.abi_version = SB_ENGINE_ABI_VERSION_PACKED;
    open.mode = SB_ENGINE_OPEN_VALIDATION_ONLY;
    Require(sb_engine_open(&open, &engine, nullptr) == SB_ENGINE_STATUS_OK &&
                engine != nullptr,
            "DPC-074 public ABI engine open failed");

    sb_engine_session_params_v1_t session_params{};
    session_params.struct_size = sizeof(session_params);
    session_params.abi_version = SB_ENGINE_ABI_VERSION_PACKED;
    session_params.effective_user_uuid = EngineUuid(principal);
    session_params.session_uuid = EngineUuid(session_id);
    session_params.default_language_utf8 = "en";
    session_params.default_language_size = 2;
    session_params.trust_mode = SB_ENGINE_TRUST_EMBEDDED_TRUSTED;
    Require(sb_engine_session_begin(engine, &session_params, &session, nullptr) ==
                SB_ENGINE_STATUS_OK &&
                session != nullptr,
            "DPC-074 public ABI session begin failed");
  }

  ~AbiHarness() {
    if (session != nullptr) {
      sb_engine_session_end_params_v1_t end{};
      end.struct_size = sizeof(end);
      end.abi_version = SB_ENGINE_ABI_VERSION_PACKED;
      end.rollback_active_transactions = 1;
      end.cancel_open_results = 1;
      (void)sb_engine_session_end(session, &end, nullptr);
    }
    if (engine != nullptr) {
      (void)sb_engine_close(engine, nullptr);
    }
  }
};

std::string PayloadText(sb_engine_result_t result) {
  sb_engine_string_view_t payload{};
  Require(sb_engine_result_payload(result, &payload) == SB_ENGINE_STATUS_OK,
          "DPC-074 public ABI payload unavailable");
  return payload.data == nullptr ? std::string{}
                                 : std::string(payload.data, payload.size_bytes);
}

bool HasDiagnostic(sb_engine_result_t result, std::string_view expected_code) {
  sb_engine_diagnostic_set_view_t diagnostics{};
  if (sb_engine_result_diagnostics(result, &diagnostics) != SB_ENGINE_STATUS_OK) {
    return false;
  }
  for (std::uint64_t i = 0; i < diagnostics.diagnostic_count; ++i) {
    const auto& diagnostic = diagnostics.diagnostics[i];
    if (diagnostic.symbolic_code.data != nullptr &&
        std::string_view(diagnostic.symbolic_code.data,
                         static_cast<std::size_t>(
                             diagnostic.symbolic_code.size_bytes)) ==
            expected_code) {
      return true;
    }
  }
  return false;
}

void AddOperand(engine_sblr::SblrOperationEnvelope* envelope,
                std::string name,
                std::string value) {
  envelope->operands.push_back({"text", std::move(name), std::move(value)});
}

engine_sblr::SblrSourceSymbolArtifact Symbol(std::string symbol_kind,
                                             std::string stable_key,
                                             std::string resolved_uuid,
                                             std::string render_hint,
                                             std::string scope) {
  engine_sblr::SblrSourceSymbolArtifact symbol;
  symbol.symbol_kind = std::move(symbol_kind);
  symbol.stable_key = std::move(stable_key);
  symbol.resolved_uuid = std::move(resolved_uuid);
  symbol.render_hint = std::move(render_hint);
  symbol.scope = std::move(scope);
  symbol.source_hash = "sha256:dpc074-source-symbol";
  symbol.authoritative = false;
  symbol.contains_sql_text = false;
  return symbol;
}

void AttachSourcePolicy(engine_sblr::SblrOperationEnvelope* envelope,
                        const UuidFactory& uuids,
                        platform::u64 salt) {
  envelope->source_artifact_map.policy_status =
      "non_authoritative_render_metadata";
  envelope->source_artifact_map.source_identity =
      "dpc074-source-map:" + uuids.Text(platform::UuidKind::object, salt);
  envelope->source_artifact_map.source_hash = "sha256:dpc074-source-map";
  envelope->source_artifact_map.render_metadata_only = true;
  envelope->source_artifact_map.contains_sql_text = false;
  envelope->source_artifact_map.raw_sql_text_authoritative = false;
}

engine_sblr::SblrOperationEnvelope BuildDmlInsertEnvelope(
    const UuidFactory& uuids) {
  const auto* entry = engine_sblr::LookupSblrOperation("dml.insert");
  Require(entry != nullptr, "DPC-074 dml.insert registry entry missing");

  auto envelope = engine_sblr::MakeSblrEnvelope(
      entry->operation_id, entry->opcode, "DPC-074-SBLR-API-ABI");
  envelope.parser_package_uuid = uuids.Text(platform::UuidKind::object, 100);
  envelope.registry_snapshot_uuid = uuids.Text(platform::UuidKind::object, 101);
  envelope.requires_security_context = entry->requires_security_context;
  envelope.requires_transaction_context = entry->requires_transaction_context;
  envelope.requires_cluster_authority = entry->requires_cluster_authority;

  const std::string table_uuid = uuids.Text(platform::UuidKind::object, 110);
  const std::string descriptor_uuid =
      uuids.Text(platform::UuidKind::object, 111);
  const std::string column_uuid = uuids.Text(platform::UuidKind::object, 112);
  const std::string parameter_uuid =
      uuids.Text(platform::UuidKind::object, 113);

  AddOperand(&envelope, "sbsql_render_family",
             "source_preserving_dml_single_row_v1");
  AddOperand(&envelope, "authority_descriptor_uuid", descriptor_uuid);
  AddOperand(&envelope, "target_object_uuid", table_uuid);
  AddOperand(&envelope, "value_column_symbol_key", "col.note");
  AddOperand(&envelope, "value_column_uuid", column_uuid);
  AddOperand(&envelope, "value_parameter_symbol_key", "param.note");
  AddOperand(&envelope, "value_parameter_uuid", parameter_uuid);

  AttachSourcePolicy(&envelope, uuids, 120);
  envelope.source_artifact_map.symbols.push_back(Symbol(
      "object_display_name", "object.dpc074_table", table_uuid,
      "dpc074_table", "dml.target"));
  envelope.source_artifact_map.symbols.push_back(
      Symbol("column_alias", "col.note", column_uuid, "note", "dml.value"));
  envelope.source_artifact_map.symbols.push_back(Symbol(
      "parameter", "param.note", parameter_uuid, ":p_note", "dml.parameter"));
  return envelope;
}

api::EngineRequestContext MakeContext(const UuidFactory& uuids) {
  api::EngineRequestContext context;
  context.trust_mode = api::EngineTrustMode::embedded_in_process;
  context.request_id = "dpc074-sblr-api-abi";
  context.database_uuid.canonical = uuids.Text(platform::UuidKind::database, 200);
  context.principal_uuid.canonical =
      uuids.Text(platform::UuidKind::principal, 201);
  context.session_uuid.canonical = uuids.Text(platform::UuidKind::session, 202);
  context.transaction_uuid.canonical = uuids.Text(platform::UuidKind::object, 203);
  context.local_transaction_id = 74;
  context.snapshot_visible_through_local_transaction_id = 74;
  context.security_context_present = true;
  context.cluster_authority_available = false;
  context.catalog_generation_id = 1;
  context.security_epoch = 1;
  context.resource_epoch = 1;
  context.name_resolution_epoch = 1;
  context.trace_tags.push_back("group:OPS");
  return context;
}

void AssertRegistryAndTextRoundTrip(const UuidFactory& uuids) {
  constexpr std::array<std::pair<std::string_view, std::string_view>, 8>
      kRequiredEntries{{
          {"dml.insert", "SBLR_INSERT"},
          {"dml.select_rows", "SBLR_DML_SELECT_ROWS"},
          {"transaction.txn_commit", "SBLR_TXN_COMMIT"},
          {"observability.show_version", "SBLR_OBSERVABILITY_SHOW_VERSION"},
          {"op.show.capabilities", "SBLR_OP_SHOW_CAPABILITIES"},
          {"index.validate", "SBLR_INDEX_VALIDATE"},
          {"index.repair", "SBLR_INDEX_REPAIR"},
          {"cluster.write_admission", "SBLR_CLUSTER_WRITE_ADMISSION"},
      }};

  for (const auto& [operation_id, opcode] : kRequiredEntries) {
    const auto* by_operation = engine_sblr::LookupSblrOperation(operation_id);
    const auto* by_opcode = engine_sblr::LookupSblrOpcode(opcode);
    Require(by_operation != nullptr && by_opcode != nullptr,
            "DPC-074 required SBLR registry entry missing");
    Require(by_operation == by_opcode,
            "DPC-074 SBLR operation/opcode registry lookup mismatch");

    auto envelope = engine_sblr::MakeSblrEnvelope(
        by_operation->operation_id, by_operation->opcode, "dpc074.registry");
    envelope.parser_package_uuid = uuids.Text(platform::UuidKind::object, 10);
    envelope.registry_snapshot_uuid =
        uuids.Text(platform::UuidKind::object, 11);
    envelope.requires_security_context = by_operation->requires_security_context;
    envelope.requires_transaction_context =
        by_operation->requires_transaction_context;
    envelope.requires_cluster_authority =
        by_operation->requires_cluster_authority;
    const auto validation = engine_sblr::ValidateSblrOpcodeForEnvelope(envelope);
    Require(validation.ok, "DPC-074 canonical registry envelope refused");

    if (by_operation->requires_cluster_authority) {
      auto missing_cluster = envelope;
      missing_cluster.requires_cluster_authority = false;
      const auto refused =
          engine_sblr::ValidateSblrOpcodeForEnvelope(missing_cluster);
      Require(!refused.ok &&
                  refused.diagnostic_id == "SB_DIAG_CLUSTER_TXN_UNAVAILABLE",
              "DPC-074 cluster-disabled registry diagnostic drifted");
    }
  }

  auto insert = BuildDmlInsertEnvelope(uuids);
  AddOperand(&insert, "escape_probe", "line1\\line2\tline3");
  const auto encoded = engine_sblr::EncodeSblrEnvelope(insert);
  const auto decoded = engine_sblr::DecodeSblrEnvelope(encoded);
  Require(decoded.ok, "DPC-074 text SBLR envelope decode failed");
  Require(decoded.envelope.operation_id == insert.operation_id &&
              decoded.envelope.opcode == insert.opcode,
          "DPC-074 text SBLR envelope operation drifted after round trip");
  Require(decoded.envelope.operands.size() == insert.operands.size(),
          "DPC-074 text SBLR envelope operand count drifted after round trip");
  Require(decoded.envelope.source_artifact_map.policy_status ==
              "non_authoritative_render_metadata",
          "DPC-074 source artifact policy drifted after round trip");
  Require(decoded.envelope.source_artifact_map.symbols.size() == 3,
          "DPC-074 source symbol artifacts drifted after round trip");
}

void AssertBinaryRoundTripAndCodecDiagnostics() {
  const std::uint8_t descriptor[] = {'d', 'p', 'c', '0', '7', '4'};
  const std::string payload = "operation_id=observability.show_version\n"
                              "opcode=SBLR_OBSERVABILITY_SHOW_VERSION\n";
  const auto encoded = scratchbird::engine::sblr::EnvelopeBuilder()
                           .operation(
                               scratchbird::engine::SblrOperationFamily::
                                   management_inspect,
                               1)
                           .descriptor(7, descriptor, sizeof(descriptor))
                           .append_bytes(
                               reinterpret_cast<const std::uint8_t*>(
                                   payload.data()),
                               payload.size())
                           .encode();
  const auto decoded = scratchbird::engine::DecodeSblrEnvelopeBytes(
      encoded.data(), static_cast<std::uint64_t>(encoded.size()));
  Require(decoded.status == scratchbird::engine::SblrCodecStatus::ok,
          "DPC-074 binary SBLR envelope decode failed");
  Require(decoded.envelope.family ==
              scratchbird::engine::SblrOperationFamily::management_inspect,
          "DPC-074 binary SBLR family drifted");
  Require(decoded.envelope.descriptors.size() == 1 &&
              decoded.envelope.descriptors.front().payload.size() ==
                  sizeof(descriptor),
          "DPC-074 binary SBLR descriptor drifted");
  Require(std::string(decoded.envelope.canonical_bytes.begin(),
                      decoded.envelope.canonical_bytes.end()) == payload,
          "DPC-074 binary SBLR payload drifted");

  const auto unsupported_version = scratchbird::engine::sblr::EnvelopeBuilder()
                                       .version(2, 0)
                                       .operation(
                                           scratchbird::engine::
                                               SblrOperationFamily::
                                                   management_inspect,
                                           1)
                                       .encode();
  const auto version_result = scratchbird::engine::DecodeSblrEnvelopeBytes(
      unsupported_version.data(), unsupported_version.size());
  Require(version_result.status ==
              scratchbird::engine::SblrCodecStatus::version_unsupported &&
              version_result.diagnostic_code == "SBLR.VERSION.UNSUPPORTED",
          "DPC-074 binary version-mismatch diagnostic drifted");
}

void AssertSblrToSbsqlConversion(const UuidFactory& uuids) {
  const auto envelope = BuildDmlInsertEnvelope(uuids);
  const auto rendered = engine_sblr::RenderSblrEnvelopeToSbsql(
      envelope, {.source_preserving = true});
  Require(rendered.ok,
          "DPC-074 user-facing SBLR-to-SBsql conversion refused valid route");
  Require(rendered.sbsql_text ==
              "INSERT INTO dpc074_table (note) VALUES (:p_note);",
          "DPC-074 SBLR-to-SBsql render output drifted");

  auto redacted = envelope;
  redacted.source_artifact_map.policy_status = "redacted_render_metadata";
  const auto refused = engine_sblr::RenderSblrEnvelopeToSbsql(
      redacted, {.source_preserving = true});
  Require(!refused.ok && !refused.diagnostics.empty() &&
              refused.diagnostics.front().code ==
                  "SB_SBLR_TO_SBSQL_SOURCE_ARTIFACT_REDACTED",
          "DPC-074 redacted SBLR-to-SBsql diagnostic drifted");
}

void AssertAdmissionAndEmbeddedDispatch(const UuidFactory& uuids) {
  auto envelope = engine_sblr::MakeSblrEnvelope(
      "observability.show_version", "SBLR_OBSERVABILITY_SHOW_VERSION",
      "dpc074.server-admission");
  envelope.parser_package_uuid = uuids.Text(platform::UuidKind::object, 300);
  envelope.registry_snapshot_uuid =
      uuids.Text(platform::UuidKind::object, 301);
  envelope.requires_transaction_context = false;

  server::ServerSblrAdmissionRequest admission_request;
  admission_request.encoded_sblr_envelope =
      engine_sblr::EncodeSblrEnvelope(envelope);
  const auto admission = server::AdmitServerSblrEnvelope(admission_request);
  Require(admission.admitted &&
              admission.operation_family == "sblr.observability.inspect.v3" &&
              admission.operation_id == "observability.show_version" &&
              admission.requires_public_abi_dispatch,
          "DPC-074 server admission did not classify public ABI route");

  server::ServerSblrAdmissionRequest raw_sql;
  raw_sql.encoded_sblr_envelope = "select * from dpc074";
  const auto raw_sql_result = server::AdmitServerSblrEnvelope(raw_sql);
  Require(!raw_sql_result.admitted &&
              !raw_sql_result.diagnostics.empty() &&
              raw_sql_result.diagnostics.front().code ==
                  "SBLR.SQL_TEXT_FORBIDDEN",
          "DPC-074 raw SQL admission diagnostic drifted");

  server::ServerSblrAdmissionRequest old_envelope;
  old_envelope.encoded_sblr_envelope =
      "envelope=SBLRExecutionEnvelope.v2\n"
      "operation_id=observability.show_version\n"
      "sblr_operation_family=sblr.observability.inspect.v3\n";
  const auto old_result = server::AdmitServerSblrEnvelope(old_envelope);
  Require(!old_result.admitted && !old_result.diagnostics.empty() &&
              old_result.diagnostics.front().code ==
                  "PARSER_SERVER_IPC.SBLR_ENVELOPE_VERSION_UNSUPPORTED",
          "DPC-074 server envelope version diagnostic drifted");

  engine_sblr::SblrDispatchRequest dispatch_request;
  dispatch_request.context = MakeContext(uuids);
  dispatch_request.envelope = envelope;
  const auto dispatch = engine_sblr::DispatchSblrOperation(dispatch_request);
  Require(dispatch.envelope_validated && dispatch.accepted &&
              dispatch.dispatched_to_api && dispatch.api_result.ok &&
              dispatch.api_result.operation_id == "observability.show_version",
          "DPC-074 embedded SBLR dispatch did not reach engine API");
}

std::vector<std::uint8_t> PublicOperationEnvelope(
    const engine_sblr::SblrOperationEnvelope& envelope) {
  const std::string text = engine_sblr::EncodeSblrEnvelope(envelope);
  return scratchbird::engine::sblr::EnvelopeBuilder()
      .operation(scratchbird::engine::SblrOperationFamily::management_inspect, 1)
      .append_bytes(reinterpret_cast<const std::uint8_t*>(text.data()),
                    text.size())
      .encode();
}

sb_engine_status_t DispatchPublic(AbiHarness& harness,
                                  const std::vector<std::uint8_t>& envelope,
                                  sb_engine_result_t* result) {
  sb_engine_request_context_v1_t context{};
  context.struct_size = sizeof(context);
  context.abi_version = SB_ENGINE_ABI_VERSION_PACKED;
  context.effective_user_uuid = EngineUuid(harness.principal);
  context.session_uuid = EngineUuid(harness.session_id);
  context.trust_mode = SB_ENGINE_TRUST_EMBEDDED_TRUSTED;
  context.rights_set_ref = 1;
  context.capability_set_ref = 1;
  context.transaction_ref = 74;

  sb_engine_sblr_dispatch_params_v1_t params{};
  params.struct_size = sizeof(params);
  params.abi_version = SB_ENGINE_ABI_VERSION_PACKED;
  params.envelope_bytes = envelope.data();
  params.envelope_size_bytes = static_cast<std::uint64_t>(envelope.size());
  return sb_engine_dispatch_sblr(harness.session, nullptr, &context, &params,
                                 result);
}

void AssertPublicAbiAndCapabilities(const UuidFactory& uuids) {
  Require(sb_engine_abi_version_packed() == SB_ENGINE_ABI_VERSION_PACKED,
          "DPC-074 ABI version reporter drifted");
  const char* build_id = nullptr;
  std::uint64_t build_id_size = 0;
  Require(sb_engine_abi_build_id(&build_id, &build_id_size) ==
              SB_ENGINE_STATUS_OK &&
              std::string_view(build_id, static_cast<std::size_t>(build_id_size))
                      .find("scratchbird-engine-abi-v1") !=
                  std::string_view::npos,
          "DPC-074 ABI build id reporter drifted");

  sb_engine_open_params_v1_t bad_abi{};
  bad_abi.struct_size = sizeof(bad_abi);
  bad_abi.abi_version = SB_ENGINE_ABI_VERSION_PACKED + 1;
  sb_engine_handle_t rejected_engine = nullptr;
  sb_engine_result_t result = nullptr;
  Require(sb_engine_open(&bad_abi, &rejected_engine, &result) ==
              SB_ENGINE_STATUS_INVALID_ARGUMENT &&
              rejected_engine == nullptr && result != nullptr &&
              HasDiagnostic(result, "ENGINE.ABI.VERSION_UNSUPPORTED"),
          "DPC-074 public ABI version-mismatch diagnostic drifted");
  (void)sb_engine_result_release(result);

  AbiHarness harness(uuids);
  sb_engine_capability_request_v1_t request{};
  request.struct_size = sizeof(request);
  request.abi_version = SB_ENGINE_ABI_VERSION_PACKED;
  result = nullptr;
  Require(sb_engine_describe_capabilities(harness.engine, &request, &result) ==
              SB_ENGINE_STATUS_OK &&
              result != nullptr,
          "DPC-074 capability report failed");
  const std::string capability_payload = PayloadText(result);
  Require(Contains(capability_payload, "sblr_dispatch=admission_only") &&
              Contains(capability_payload, "cluster_provider_support=") &&
              Contains(capability_payload,
                       "sblr.acceleration.management.v3=capability_fail_closed"),
          "DPC-074 capability report missing SBLR/cluster/ABI capability data");
  Require(!Contains(capability_payload, "driver_speed_benchmark"),
          "DPC-074 capability report falsely claims driver speed evidence");
  (void)sb_engine_result_release(result);

  auto show_version = engine_sblr::MakeSblrEnvelope(
      "observability.show_version", "SBLR_OBSERVABILITY_SHOW_VERSION",
      "dpc074.public-abi");
  show_version.parser_package_uuid =
      uuids.Text(platform::UuidKind::object, 401);
  show_version.registry_snapshot_uuid =
      uuids.Text(platform::UuidKind::object, 402);
  result = nullptr;
  Require(DispatchPublic(harness, PublicOperationEnvelope(show_version),
                         &result) == SB_ENGINE_STATUS_OK &&
              result != nullptr,
          "DPC-074 public ABI SBLR dispatch failed");
  const std::string payload = PayloadText(result);
  Require(Contains(payload, "operation_id=observability.show_version") &&
              Contains(payload, "product=ScratchBird"),
          "DPC-074 public ABI result payload missing route evidence");
  (void)sb_engine_result_release(result);

  const auto binary_version_2 = scratchbird::engine::sblr::EnvelopeBuilder()
                                   .version(2, 0)
                                   .operation(
                                       scratchbird::engine::
                                           SblrOperationFamily::
                                               management_inspect,
                                       1)
                                   .encode();
  result = nullptr;
  Require(DispatchPublic(harness, binary_version_2, &result) ==
              SB_ENGINE_STATUS_UNSUPPORTED &&
              result != nullptr && HasDiagnostic(result,
                                                 "SBLR.VERSION.UNSUPPORTED"),
          "DPC-074 public ABI binary version diagnostic drifted");
  (void)sb_engine_result_release(result);

  const auto cluster = scratchbird::engine::sblr::EnvelopeBuilder()
                           .operation(scratchbird::engine::SblrOperationFamily::
                                          cluster_placement,
                                      1)
                           .encode();
  result = nullptr;
  Require(DispatchPublic(harness, cluster, &result) ==
              SB_ENGINE_STATUS_CAPABILITY_DISABLED &&
              result != nullptr && HasDiagnostic(result,
                                                 "SBLR.CAPABILITY.FORBIDDEN"),
          "DPC-074 cluster-disabled public ABI diagnostic drifted");
  (void)sb_engine_result_release(result);
}

void AssertStandaloneEvidenceLinks(const std::filesystem::path& repo_root) {
  const auto db_cmake =
      repo_root / "project/tests/database_lifecycle/CMakeLists.txt";
  const auto sbsql_cmake =
      repo_root / "project/tests/sbsql_parser_worker/CMakeLists.txt";
  const auto engine_abi_cmake =
      repo_root / "project/tests/engine_public_abi/CMakeLists.txt";
  const auto listener_cmake = repo_root / "project/tests/listener/CMakeLists.txt";
  const auto ipc_tester = repo_root / "project/src/server/ipc_tester.cpp";
  const auto dml_gate =
      repo_root / "project/tests/sbsql_parser_worker/sbsql_dml_exact_route_conformance.cpp";
  const auto driver_jdbc =
      repo_root /
      "project/drivers/driver/jdbc/src/main/java/com/scratchbird/jdbc/SBProtocolHandler.java";
  const auto driver_cpp =
      repo_root / "project/drivers/driver/cpp/src/protocol/sbwp_protocol.cpp";

  RequireContains(db_cmake, "dpc_sblr_api_abi_compatibility_gate",
                  "DPC-074 standalone CTest registration missing");
  RequireContains(sbsql_cmake, "sbsql_dml_exact_route_conformance",
                  "DPC-074 SBsql lowering CTest evidence missing");
  RequireContains(sbsql_cmake, "sbsql_sblr_to_sbsql_conversion_route_conformance",
                  "DPC-074 SBLR-to-SBsql CTest evidence missing");
  RequireContains(sbsql_cmake, "sbsql_sb_isql_local_ipc_route_smoke",
                  "DPC-074 local IPC route CTest evidence missing");
  RequireContains(sbsql_cmake,
                  "sb_listener_sbp_sbsql_sbwp_tls_engine_auth_route_smoke",
                  "DPC-074 INET/SBWP TLS route CTest evidence missing");
  RequireContains(engine_abi_cmake, "sb_engine_public_sblr_admission_fixture",
                  "DPC-074 public SBLR ABI CTest evidence missing");
  RequireContains(listener_cmake, "sb_listener_native_sbwp_tls_smoke",
                  "DPC-074 native INET listener CTest evidence missing");
  RequireContains(ipc_tester, "sblr_prepare_execute_show_version",
                  "DPC-074 IPC prepare/execute route evidence missing");
  RequireContains(ipc_tester, "sblr_raw_sql_rejected",
                  "DPC-074 IPC raw SQL refusal evidence missing");
  RequireContains(ipc_tester, "sblr_cluster_refused",
                  "DPC-074 IPC cluster refusal evidence missing");
  RequireContains(dml_gate, "lowering.sblr_family.sblr_dml_operation_v3",
                  "DPC-074 SBsql lowering-to-SBLR evidence missing");
  RequireContains(dml_gate, "server.admission.sblr_dml_operation_v3",
                  "DPC-074 SBsql server-admission evidence missing");
  RequireContains(driver_jdbc, "MSG_SBLR_EXECUTE",
                  "DPC-074 JDBC driver-visible SBLR route evidence missing");
  RequireContains(driver_jdbc, "QUERY_FLAG_RETURN_SBLR",
                  "DPC-074 JDBC driver SBLR capability evidence missing");
  RequireContains(driver_cpp, "buildSblrExecutePayload",
                  "DPC-074 C++ driver-visible SBLR route evidence missing");
}

}  // namespace

int main(int argc, char** argv) {
  Require(argc == 2, "usage: dpc_sblr_api_abi_compatibility_gate REPO_ROOT");
  const std::filesystem::path repo_root = argv[1];
  const UuidFactory uuids;

  AssertRegistryAndTextRoundTrip(uuids);
  AssertBinaryRoundTripAndCodecDiagnostics();
  AssertSblrToSbsqlConversion(uuids);
  AssertAdmissionAndEmbeddedDispatch(uuids);
  AssertPublicAbiAndCapabilities(uuids);
  AssertStandaloneEvidenceLinks(repo_root);

  std::cout << "dpc_sblr_api_abi_compatibility_gate=passed\n";
  return EXIT_SUCCESS;
}
