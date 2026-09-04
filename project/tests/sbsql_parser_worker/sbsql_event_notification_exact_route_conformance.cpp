// Copyright (c) 2026 ScratchBird Software Inc.
//
// This Source Code Form is subject to the terms of the Mozilla Public
// License, v. 2.0. If a copy of the MPL was not distributed with this
// file, You can obtain one at https://mozilla.org/MPL/2.0/.
//
// SPDX-License-Identifier: MPL-2.0

#include "ast/ast.hpp"
#include "canonical_sblr_admission_test_helper.hpp"
#include "binder/binder.hpp"
#include "cst/cst.hpp"
#include "database_lifecycle.hpp"
#include "lowering/lowering.hpp"
#include "registry/generated/sbsql_generated_registry.hpp"
#include "sblr_admission.hpp"
#include "sblr_dispatch.hpp"
#include "sblr_engine_envelope.hpp"
#include "sblr_event_notification.hpp"
#include "sblr_opcode_registry.hpp"
#include "transaction/transaction_api.hpp"
#include "uuid.hpp"

#include <algorithm>
#include <array>
#include <cstdlib>
#include <filesystem>
#include <iostream>
#include <string>
#include <string_view>
#include <unistd.h>
#include <vector>

namespace {

using namespace scratchbird::parser::sbsql;
namespace api = scratchbird::engine::internal_api;
namespace db = scratchbird::storage::database;
namespace sblr = scratchbird::engine::sblr;
namespace uuid = scratchbird::core::uuid;
using scratchbird::core::platform::UuidKind;

constexpr std::string_view kChannelUuid = "019f0000-0000-7000-8000-000000024901";
constexpr std::string_view kSessionUuid = "019f0000-0000-7000-8000-000000024902";
constexpr std::string_view kPrincipalUuid = "019f0000-0000-7000-8000-000000024903";

struct EventRowEvidence {
  std::string_view surface_id;
  std::string_view canonical_name;
  std::string_view validation_fixture_id;
  std::string_view sql;
  std::string_view operation_id;
  std::string_view opcode;
  std::string_view expected_family;
  std::string_view registry_family;
  std::string_view parser_handler_key;
  std::string_view lowering_handler_key;
  std::string_view server_admission_key;
  std::string_view engine_rule_key;
};

struct EngineFixture {
  std::filesystem::path temp_dir;
  std::filesystem::path database_path;
  std::string database_uuid;
};

constexpr std::array<EventRowEvidence, 10> kEventRows{{
    {"SBSQL-7693C369D578",
     "create_event_stmt",
     "SBSQL-SURFACE-BD4A7B1CDE19",
     "CREATE EVENT CHANNEL audit_channel",
     "engine.op.event_channel_create",
     "SBLR_EVENT_CHANNEL_CREATE",
     "sblr.catalog.mutation.v3",
     "ddl_catalog",
     "parser.statement_family.ddl_catalog",
     "lowering.sblr_family.sblr_catalog_mutation_v3",
     "server.admission.sblr_catalog_mutation_v3",
     "engine.rule.sblr_catalog_mutation_v3"},
    {"SBSQL-E52360D3932B",
     "channel_def",
     "SBSQL-SURFACE-33D7484A73BE",
     "CREATE EVENT CHANNEL audit_channel",
     "engine.op.event_channel_create",
     "SBLR_EVENT_CHANNEL_CREATE",
     "sblr.catalog.mutation.v3",
     "general",
     "parser.grammar_ast",
     "lowering.sblr_family.sblr_general_operation_v3",
     "server.admission.sblr_general_operation_v3",
     "engine.rule.sblr_general_operation_v3"},
    {"SBSQL-8AD3BDAA2DEF",
     "channel_name",
     "SBSQL-SURFACE-560BAA7C1E0A",
     "LISTEN EVENT CHANNEL audit_channel",
     "engine.op.event_channel_listen",
     "SBLR_EVENT_CHANNEL_LISTEN",
     "sblr.event.channel.v3",
     "general",
     "parser.grammar_ast",
     "lowering.sblr_family.sblr_general_operation_v3",
     "server.admission.sblr_general_operation_v3",
     "engine.rule.sblr_general_operation_v3"},
    {"SBSQL-34936281765E",
     "listen_notify_stmt",
     "SBSQL-SURFACE-3C216CD0A0B7",
     "NOTIFY EVENT CHANNEL audit_channel PAYLOAD 'payload-001'",
     "engine.op.event_channel_notify",
     "SBLR_EVENT_CHANNEL_NOTIFY",
     "sblr.event.channel.v3",
     "general",
     "parser.grammar_ast",
     "lowering.sblr_family.sblr_general_operation_v3",
     "server.admission.sblr_general_operation_v3",
     "engine.rule.sblr_general_operation_v3"},
    {"SBSQL-33A1149AB350",
     "post_event_stmt",
     "SBSQL-SURFACE-B5BC5EEFE5D4",
     "POST EVENT CHANNEL audit_channel PAYLOAD 'post-001'",
     "engine.op.event_channel_notify",
     "SBLR_EVENT_CHANNEL_NOTIFY",
     "sblr.event.channel.v3",
     "general",
     "parser.grammar_ast",
     "lowering.sblr_family.sblr_general_operation_v3",
     "server.admission.sblr_general_operation_v3",
     "engine.rule.sblr_general_operation_v3"},
    {"SBSQL-832A8A3D2913",
     "subscription_stmt",
     "SBSQL-SURFACE-84AC75E4F879",
     "SUBSCRIBE EVENT CHANNEL audit_channel",
     "engine.op.event_channel_listen",
     "SBLR_EVENT_CHANNEL_LISTEN",
     "sblr.event.channel.v3",
     "general",
     "parser.grammar_ast",
     "lowering.sblr_family.sblr_general_operation_v3",
     "server.admission.sblr_general_operation_v3",
     "engine.rule.sblr_general_operation_v3"},
    {"SBSQL-9F45E03EC6AF",
     "subscription_action",
     "SBSQL-SURFACE-7BC5BD7146C8",
     "SUBSCRIBE EVENT CHANNEL audit_channel",
     "engine.op.event_channel_listen",
     "SBLR_EVENT_CHANNEL_LISTEN",
     "sblr.event.channel.v3",
     "general",
     "parser.grammar_ast",
     "lowering.sblr_family.sblr_general_operation_v3",
     "server.admission.sblr_general_operation_v3",
     "engine.rule.sblr_general_operation_v3"},
    {"SBSQL-34936281765E",
     "listen_notify_stmt",
     "SBSQL-SURFACE-3C216CD0A0B7",
     "UNLISTEN EVENT CHANNEL audit_channel",
     "engine.op.event_channel_unlisten",
     "SBLR_EVENT_CHANNEL_UNLISTEN",
     "sblr.event.channel.v3",
     "general",
     "parser.grammar_ast",
     "lowering.sblr_family.sblr_general_operation_v3",
     "server.admission.sblr_general_operation_v3",
     "engine.rule.sblr_general_operation_v3"},
    {"SBSQL-832A8A3D2913",
     "subscription_stmt",
     "SBSQL-SURFACE-84AC75E4F879",
     "UNSUBSCRIBE EVENT CHANNEL audit_channel",
     "engine.op.event_channel_unlisten",
     "SBLR_EVENT_CHANNEL_UNLISTEN",
     "sblr.event.channel.v3",
     "general",
     "parser.grammar_ast",
     "lowering.sblr_family.sblr_general_operation_v3",
     "server.admission.sblr_general_operation_v3",
     "engine.rule.sblr_general_operation_v3"},
    {"SBSQL-832A8A3D2913",
     "subscription_stmt",
     "SBSQL-SURFACE-84AC75E4F879",
     "SHOW EVENT SUBSCRIPTIONS",
     "engine.op.event_subscription_list",
     "SBLR_EVENT_SUBSCRIPTION_LIST",
     "sblr.event.subscription.v3",
     "general",
     "parser.grammar_ast",
     "lowering.sblr_family.sblr_general_operation_v3",
     "server.admission.sblr_general_operation_v3",
     "engine.rule.sblr_general_operation_v3"},
}};

void Require(bool condition, std::string_view message) {
  if (!condition) {
    std::cerr << message << '\n';
    std::exit(EXIT_FAILURE);
  }
}

bool Contains(std::string_view haystack, std::string_view needle) {
  return haystack.find(needle) != std::string_view::npos;
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

std::filesystem::path MakeTempDir() {
  std::string tmpl = "/tmp/sb_sbsql_event_route.XXXXXX";
  std::vector<char> writable(tmpl.begin(), tmpl.end());
  writable.push_back('\0');
  char* made = ::mkdtemp(writable.data());
  Require(made != nullptr, "event notification temp directory create failed");
  return std::filesystem::path(made);
}

EngineFixture MakeEngineFixture() {
  EngineFixture fixture;
  fixture.temp_dir = MakeTempDir();
  fixture.database_path = fixture.temp_dir / "event_route.sbdb";

  db::DatabaseCreateConfig create;
  create.path = fixture.database_path.string();
  const auto database_uuid = uuid::GenerateEngineIdentityV7(UuidKind::database, 1779131901000);
  const auto filespace_uuid = uuid::GenerateEngineIdentityV7(UuidKind::filespace, 1779131901001);
  Require(database_uuid.ok(), "event notification database UUID generation failed");
  Require(filespace_uuid.ok(), "event notification filespace UUID generation failed");
  create.database_uuid = database_uuid.value;
  create.filespace_uuid = filespace_uuid.value;
  create.page_size = 16384;
  create.creation_unix_epoch_millis = 1779131901002;
  create.allow_minimal_resource_bootstrap = true;
  create.require_resource_seed_pack = false;
  create.allow_overwrite = true;

  const auto created = db::CreateDatabaseFile(create);
  Require(created.ok(), "event notification test database create failed");
  const auto opened = db::OpenDatabaseFile({fixture.database_path.string(), false, false, false});
  Require(opened.ok(), "event notification test database open failed");
  const auto clean = db::MarkDatabaseCleanShutdown(fixture.database_path.string());
  Require(clean.ok(), "event notification clean shutdown marker failed");
  fixture.database_uuid = uuid::UuidToString(create.database_uuid.value);
  return fixture;
}

SessionContext ParserSession() {
  SessionContext session;
  session.authenticated = true;
  session.session_uuid = std::string(kSessionUuid);
  session.connection_uuid = "019f0000-0000-7000-8000-000000024904";
  session.database_uuid = "019f0000-0000-7000-8000-000000024905";
  session.catalog_epoch = 31;
  session.security_policy_epoch = 32;
  session.descriptor_epoch = 33;
  return session;
}

ParserConfig ParserConfigForTest() {
  ParserConfig config;
  config.probe_mode = true;
  config.server_endpoint = "sb_server_name_resolver";
  config.parser_uuid = "019f0000-0000-7000-8000-000000024906";
  config.bundle_contract_id = "sbp_sbsql@event-notification-route-test";
  config.build_id = "sbsql-event-notification-route-test";
  return config;
}

struct PipelineArtifacts {
  CstDocument cst;
  AstDocument ast;
  BoundStatement bound;
  SblrEnvelope envelope;
  SblrVerifierResult verifier;
};

std::vector<std::string> ResolvedUuidsFor(const EventRowEvidence& row) {
  if (row.operation_id == "engine.op.event_channel_create" ||
      row.operation_id == "engine.op.event_subscription_list") {
    return {};
  }
  return {std::string(kChannelUuid)};
}

std::vector<std::uint8_t> EventUuid(std::uint8_t suffix) {
  std::vector<std::uint8_t> value(16, 0);
  value[0] = 0x12;
  value[6] = 0x70;
  value[8] = 0x80;
  value[15] = suffix;
  return value;
}

std::vector<std::uint8_t> EventText(std::string_view value) {
  return {value.begin(), value.end()};
}

std::vector<std::uint8_t> EventU32(std::uint32_t value) {
  return {static_cast<std::uint8_t>(value),
          static_cast<std::uint8_t>(value >> 8),
          static_cast<std::uint8_t>(value >> 16),
          static_cast<std::uint8_t>(value >> 24)};
}

sblr::SblrEventNotificationOpcode EventOpcode(const EventRowEvidence& row) {
  if (row.operation_id == "engine.op.event_channel_create") {
    return sblr::SblrEventNotificationOpcode::channel_create;
  }
  if (row.operation_id == "engine.op.event_channel_listen") {
    return sblr::SblrEventNotificationOpcode::channel_listen;
  }
  if (row.operation_id == "engine.op.event_channel_unlisten") {
    return sblr::SblrEventNotificationOpcode::channel_unlisten;
  }
  if (row.operation_id == "engine.op.event_channel_notify") {
    return sblr::SblrEventNotificationOpcode::channel_notify;
  }
  return sblr::SblrEventNotificationOpcode::subscription_list;
}

sblr::SblrOperationEnvelope CanonicalEventEnvelope(
    const EventRowEvidence& row) {
  const auto opcode = EventOpcode(row);
  sblr::SblrEventNotificationRecord record;
  record.opcode = opcode;
  record.request_uuid[0] = 1;
  record.security_context_uuid[0] = 2;
  record.policy_epoch = 1;
  if (opcode != sblr::SblrEventNotificationOpcode::subscription_list) {
    record.transaction_id = 1;
  }
  const auto add = [&record](std::uint16_t tag,
                             std::vector<std::uint8_t> value) {
    record.fields.push_back({tag, std::move(value)});
  };
  switch (opcode) {
    case sblr::SblrEventNotificationOpcode::channel_create:
      add(1, EventUuid(1));
      add(2, EventText("audit_channel"));
      add(3, EventUuid(3));
      add(4, EventUuid(4));
      add(5, EventText("normal"));
      add(6, EventText("none"));
      break;
    case sblr::SblrEventNotificationOpcode::channel_listen:
      add(1, EventUuid(1));
      add(2, EventUuid(2));
      add(3, EventUuid(3));
      add(4, EventText("native_message"));
      break;
    case sblr::SblrEventNotificationOpcode::channel_unlisten:
      add(1, EventUuid(1));
      add(2, EventUuid(2));
      add(3, EventUuid(3));
      break;
    case sblr::SblrEventNotificationOpcode::channel_notify:
      add(1, EventUuid(1));
      add(2, EventText("native_message"));
      add(3, EventUuid(3));
      add(4, EventText("payload-001"));
      add(5, std::vector<std::uint8_t>(16, 0));
      add(6, EventUuid(6));
      break;
    case sblr::SblrEventNotificationOpcode::subscription_list:
      add(1, EventUuid(1));
      add(2, EventU32(16));
      add(3, {});
      break;
    default:
      break;
  }
  const auto encoded = sblr::EncodeSblrEventNotificationRecord(record);
  Require(encoded.ok, "event notification exact carrier encode failed");

  sblr::SblrOperationEnvelope envelope;
  envelope.operation_id = std::string(row.operation_id);
  envelope.opcode = std::string(row.opcode);
  envelope.opcode_code = static_cast<std::uint16_t>(opcode);
  const auto* registry = sblr::LookupSblrOperation(row.operation_id);
  Require(registry != nullptr,
          "event notification exact carrier registry row missing");
  envelope.result_shape = registry->result_contract;
  envelope.diagnostic_shape = "diagnostic_vector";
  envelope.trace_key = "trace.event_notification.exact_route";
  envelope.requires_security_context = true;
  envelope.requires_transaction_context = registry->requires_transaction_context;
  envelope.parser_resolved_names_to_uuids = true;
  envelope.operands.push_back(sblr::MakeSblrEventNotificationOperand(encoded));
  return scratchbird::test::sbsql::CanonicalizeEngineSblrEnvelopeForTest(
      std::move(envelope));
}

PipelineArtifacts RunPipeline(std::string_view sql,
                              const std::vector<std::string>& resolved_object_uuids) {
  PipelineArtifacts artifacts;
  const auto session = ParserSession();
  artifacts.cst = BuildCst(sql);
  artifacts.ast = BuildAst(artifacts.cst);
  artifacts.bound =
      BindAst(artifacts.ast, artifacts.cst, ParserConfigForTest(), session, resolved_object_uuids);
  artifacts.envelope = LowerToSblr(artifacts.bound, artifacts.cst, session);
  artifacts.verifier = VerifySblrEnvelope(artifacts.envelope);
  return artifacts;
}

void RequireRegistryEvidence(const EventRowEvidence& row) {
  const auto* registry_row = FindGeneratedSurfaceRegistryRowById(row.surface_id);
  Require(registry_row != nullptr, "event notification generated registry row missing");
  Require(registry_row->canonical_name == row.canonical_name,
          "event notification registry canonical name drifted");
  Require(registry_row->surface_kind == "grammar_production",
          "event notification registry surface kind drifted");
  Require(registry_row->family == row.registry_family,
          "event notification registry family drifted");
  Require(registry_row->source_status == "native_now",
          "event notification registry status drifted");
  Require(registry_row->cluster_scope == "noncluster_or_profile_scoped",
          "event notification registry cluster scope drifted");
  Require(registry_row->parser_handler_key == row.parser_handler_key,
          "event notification registry parser handler drifted");
  Require(registry_row->lowering_handler_key == row.lowering_handler_key,
          "event notification registry lowering handler drifted");
  Require(registry_row->server_admission_key == row.server_admission_key,
          "event notification registry server admission key drifted");
  Require(registry_row->engine_rule_key == row.engine_rule_key,
          "event notification registry engine rule key drifted");
  Require(registry_row->validation_fixture_id == row.validation_fixture_id,
          "event notification registry fixture id drifted");
}

void RequireExactLowering(const EventRowEvidence& row) {
  const auto artifacts = RunPipeline(row.sql, ResolvedUuidsFor(row));
  Require(!artifacts.cst.messages.has_errors(), "event notification CST failed");
  Require(!artifacts.ast.messages.has_errors(), "event notification AST failed");
  Require(artifacts.bound.bound, "event notification bind failed");
  if (!artifacts.verifier.admitted) {
    for (const auto& diagnostic : artifacts.verifier.messages.diagnostics) {
      std::cerr << diagnostic.code << ':' << diagnostic.message << '\n';
      for (const auto& field : diagnostic.fields) {
        std::cerr << "  " << field.name << '=' << field.value << '\n';
      }
    }
  }
  Require(artifacts.verifier.admitted, "event notification verifier rejected exact route");
  Require(artifacts.envelope.operation_family == row.expected_family,
          "event notification operation family mismatch");
  Require(artifacts.envelope.sblr_operation_key == row.expected_family,
          "event notification SBLR operation key mismatch");
  Require(artifacts.envelope.operation_id == row.operation_id,
          "event notification operation id mismatch");
  Require(artifacts.envelope.engine_api_operation_id == row.operation_id,
          "event notification engine API operation id mismatch");
  Require(artifacts.envelope.sblr_opcode == row.opcode,
          "event notification opcode mismatch");
  Require(HasValue(artifacts.envelope.required_authority_steps,
                   "authority.engine.event_notification_api_required"),
          "event notification engine API authority missing");
  Require(HasValue(artifacts.envelope.required_authority_steps,
                   "authority.server.security_policy_context_required"),
          "event notification security context authority missing");
  Require(HasValue(artifacts.envelope.required_authority_steps,
                   "authority.server.transaction_context_required"),
          "event notification transaction context authority missing");
  Require(HasValue(artifacts.envelope.required_authority_steps,
                   "authority.parser.no_storage_or_finality"),
          "event notification parser storage/finality authority missing");
  Require(HasValue(artifacts.envelope.required_authority_steps,
                   "authority.parser.no_sql_text_execution"),
          "event notification parser SQL execution authority missing");
  Require(HasValue(artifacts.envelope.descriptor_refs, "sys.event.channel"),
          "event notification channel descriptor ref missing");
  Require(Contains(artifacts.envelope.payload,
                   "\"event_envelope_kind\":\"event_notification_route\""),
          "event notification payload missing envelope kind");
  Require(Contains(artifacts.envelope.payload, row.surface_id),
          "event notification payload missing row-identifiable surface id");
  Require(Contains(artifacts.envelope.payload, "\"sql_text_included\":false"),
          "event notification payload did not prove no SQL text authority");
  Require(Contains(artifacts.envelope.payload, "\"parser_executes_event_delivery\":false"),
          "event notification payload allowed parser event delivery");
  if (row.operation_id == "engine.op.event_channel_create") {
    Require(Contains(artifacts.envelope.payload, "\"channel\":\"audit_channel\""),
            "CREATE EVENT CHANNEL payload missing structured channel name");
    Require(Contains(artifacts.envelope.payload,
                     "\"channel_name_text_is_user_payload\":true"),
            "CREATE EVENT CHANNEL did not mark channel name as user payload");
  } else if (row.operation_id == "engine.op.event_subscription_list") {
    Require(!Contains(artifacts.envelope.payload, "\"channel_uuid\""),
            "event subscription list payload carried an unneeded channel UUID");
    Require(!Contains(artifacts.envelope.payload, "audit_channel"),
            "event subscription list payload embedded channel name text");
    Require(HasValue(artifacts.envelope.required_rights,
                     "right.event_delivery_read"),
            "event subscription list delivery-read authority missing");
  } else {
    Require(Contains(artifacts.envelope.payload,
                     std::string("\"channel_uuid\":\"") + std::string(kChannelUuid) + "\""),
            "event notification payload missing channel UUID");
    Require(!Contains(artifacts.envelope.payload, "audit_channel"),
            "runtime event notification payload embedded channel name text");
  }
  if (row.operation_id == "engine.op.event_channel_notify") {
    Require(Contains(artifacts.envelope.payload, "\"payload_text_is_user_payload\":true"),
            "event notification payload did not mark payload as user data");
    Require(Contains(artifacts.envelope.payload,
                     "\"payload_descriptor_uuid\":\"event_payload_descriptor:text.v1\""),
            "event notification payload descriptor missing");
  }
  if (row.operation_id == "engine.op.event_channel_unlisten") {
    Require(HasValue(artifacts.envelope.required_rights,
                     "right.event_subscribe"),
            "event unlisten authority missing");
  }
  Require(!Contains(artifacts.envelope.payload, row.sql),
          "event notification envelope embedded source SQL text");
  Require(!Contains(artifacts.envelope.payload, "\"source_text\""),
          "event notification envelope embedded source_text");

  const auto admission = scratchbird::server::AdmitServerSblrEnvelope(
      scratchbird::test::sbsql::BuildCanonicalSblrAdmissionRequest(
          CanonicalEventEnvelope(row)));
  Require(admission.admitted, "server admission rejected event notification exact route");
  Require(admission.requires_public_abi_dispatch,
          "server admission did not require public ABI dispatch for event notification");
  Require(admission.operation_id == row.operation_id,
          "server admission event notification operation id mismatch");
  Require(admission.operation_family == row.expected_family,
          "server admission event notification operation family mismatch");

  const auto* opcode_entry = sblr::LookupSblrOperation(row.operation_id);
  Require(opcode_entry != nullptr, "event notification opcode registry row missing");
  Require(opcode_entry->opcode == row.opcode,
          "event notification opcode registry opcode drifted");
  Require(opcode_entry->requires_security_context,
          "event notification opcode registry security context drifted");
  Require(opcode_entry->requires_transaction_context ==
              (row.operation_id != "engine.op.event_subscription_list"),
          "event notification opcode registry transaction context drifted");
}

api::EngineRequestContext EngineContext(const EngineFixture& fixture) {
  api::EngineRequestContext context;
  context.trust_mode = api::EngineTrustMode::embedded_in_process;
  context.request_id = "sbsql-event-notification-exact-route";
  context.database_path = fixture.database_path.string();
  context.database_uuid.canonical = fixture.database_uuid;
  context.session_uuid.canonical = std::string(kSessionUuid);
  context.principal_uuid.canonical = std::string(kPrincipalUuid);
  context.security_context_present = true;
  context.catalog_generation_id = 1;
  context.security_epoch = 1;
  context.resource_epoch = 1;
  context.name_resolution_epoch = 1;
  context.trace_tags.push_back("security.fixture_trace_authority");
  context.trace_tags.push_back("right:EVENT_CREATE");
  context.trace_tags.push_back("right:EVENT_SUBSCRIBE");
  context.trace_tags.push_back("right:EVENT_PUBLISH");
  context.trace_tags.push_back("right:EVENT_DELIVERY_READ");
  return context;
}

api::EngineRequestContext BeginTx(const EngineFixture& fixture) {
  api::EngineBeginTransactionRequest begin;
  begin.context = EngineContext(fixture);
  begin.isolation_level = "read_committed";
  const auto begun = api::EngineBeginTransaction(begin);
  for (const auto& diagnostic : begun.diagnostics) {
    if (diagnostic.error) {
      std::cerr << diagnostic.code << ':' << diagnostic.detail << '\n';
    }
  }
  Require(begun.ok, "event notification begin transaction failed");
  auto context = EngineContext(fixture);
  context.local_transaction_id = begun.local_transaction_id;
  context.transaction_uuid = begun.transaction_uuid;
  return context;
}

void CommitTx(const api::EngineRequestContext& context) {
  api::EngineCommitTransactionRequest commit;
  commit.context = context;
  const auto committed = api::EngineCommitTransaction(commit);
  for (const auto& diagnostic : committed.diagnostics) {
    if (diagnostic.error) {
      std::cerr << diagnostic.code << ':' << diagnostic.detail << '\n';
    }
  }
  Require(committed.ok, "event notification commit transaction failed");
}

sblr::SblrOperationEnvelope EngineEnvelope(std::string operation_id,
                                           std::string opcode,
                                           std::string channel_uuid = {},
                                           std::string payload = {}) {
  auto envelope = sblr::MakeSblrEnvelope(std::move(operation_id), std::move(opcode),
                                         "trace.event_notification.exact_route");
  envelope.requires_security_context = true;
  envelope.requires_transaction_context = true;
  envelope.requires_cluster_authority = false;
  envelope.contains_sql_text = false;
  envelope.parser_resolved_names_to_uuids = true;
  if (!channel_uuid.empty()) {
    envelope.operands.push_back({"text", "target_object_uuid", channel_uuid});
    envelope.operands.push_back({"text", "target_object_kind", "event_channel"});
    envelope.operands.push_back({"text", "channel_uuid", channel_uuid});
  }
  if (!payload.empty()) {
    envelope.operands.push_back({"text", "payload", payload});
    envelope.operands.push_back({"text", "payload_descriptor_uuid",
                                 "event_payload_descriptor:text.v1"});
  }
  return envelope;
}

void RequireDispatchAccepted(const sblr::SblrDispatchResult& result,
                             std::string_view operation_id) {
  for (const auto& diagnostic : result.diagnostics) {
    std::cerr << diagnostic.code << ':' << diagnostic.message << '\n';
  }
  for (const auto& diagnostic : result.api_result.diagnostics) {
    std::cerr << diagnostic.code << ':' << diagnostic.detail << '\n';
  }
  Require(result.envelope_validated, "event notification SBLR envelope did not validate");
  Require(result.accepted, "event notification SBLR dispatch did not accept operation");
  Require(result.dispatched_to_api,
          "event notification SBLR dispatch did not route to an internal API");
  Require(result.api_result.ok, "event notification engine API did not complete");
  Require(result.api_result.operation_id == operation_id,
          "event notification engine API operation id mismatch");
}

void RequireEngineDispatch() {
  for (const auto& row : kEventRows) {
    api::EngineRequestContext context;
    context.security_context_present = true;
    context.local_transaction_id = 1;
    context.transaction_uuid.canonical =
        "019f0000-0000-7000-8000-000000024907";
    unsigned cancellation_probes = 0;
    context.query_cancellation_requested = [&] {
      ++cancellation_probes;
      return true;
    };
    const auto result = sblr::DispatchSblrOperation(
        {context, CanonicalEventEnvelope(row), api::EngineApiRequest{}});
    Require(result.envelope_validated,
            "event notification exact refusal envelope did not validate");
    Require(!result.accepted && !result.dispatched_to_api,
            "event notification executed without accepted executor evidence");
    const bool exact_evidence_refusal =
        std::any_of(result.diagnostics.begin(), result.diagnostics.end(),
                    [](const auto& diagnostic) {
                      return diagnostic.code ==
                             "SBLR.OPCODE.EXECUTOR_EVIDENCE_MISSING";
                    }) ||
        std::any_of(result.api_result.diagnostics.begin(),
                    result.api_result.diagnostics.end(),
                    [](const auto& diagnostic) {
                      return diagnostic.code ==
                             "SBLR.OPCODE.EXECUTOR_EVIDENCE_MISSING";
                    });
    if (!exact_evidence_refusal) {
      for (const auto& diagnostic : result.diagnostics) {
        std::cerr << diagnostic.code << ':' << diagnostic.message << '\n';
      }
      for (const auto& diagnostic : result.api_result.diagnostics) {
        std::cerr << diagnostic.code << ':' << diagnostic.detail << '\n';
      }
    }
    Require(exact_evidence_refusal,
            "event notification exact executor-evidence diagnostic drifted");
    Require(cancellation_probes == 0,
            "event notification cancellation ran before executor-evidence refusal");
  }
}

}  // namespace

int main() {
  for (const auto& row : kEventRows) {
    RequireRegistryEvidence(row);
    RequireExactLowering(row);
  }
  RequireEngineDispatch();
  std::cout << "sbsql_event_notification_exact_route_conformance=passed\n";
  return EXIT_SUCCESS;
}
